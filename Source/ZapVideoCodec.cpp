#include "ZapVideoCodec.h"

#include <algorithm>

namespace ninjamplus::zap
{
    namespace
    {
        int normaliseQuality(int quality)
        {
            return juce::jlimit(kZapJpegMinQuality, kZapJpegMaxQuality, quality);
        }
    }

    juce::String getCodecName(VideoCodec codec)
    {
        switch (codec)
        {
            case VideoCodec::mjpeg: return "MJPEG";
            case VideoCodec::h264:  return "H.264";
            case VideoCodec::vp8:   return "VP8";
            case VideoCodec::unknown:
            default:                return "Unknown";
        }
    }

    VideoCodecCapability getCodecCapability(VideoCodec codec)
    {
        switch (codec)
        {
            case VideoCodec::mjpeg:
                return { true, true, false, "JUCE JPEG" };

            case VideoCodec::h264:
                return { false, false, false, "native backend not compiled" };

            case VideoCodec::vp8:
               #if NINJAMPLUS_HAS_LIBVPX
                return { false, true, false, "libvpx decode" };
               #else
                return { false, false, false, "libvpx backend not compiled" };
               #endif

            case VideoCodec::unknown:
            default:
                return {};
        }
    }

    juce::String getCodecCapabilitySummary()
    {
        juce::StringArray parts;

        for (auto codec : { VideoCodec::mjpeg, VideoCodec::h264, VideoCodec::vp8 })
        {
            const auto cap = getCodecCapability(codec);
            juce::String text = getCodecName(codec) + ": ";

            if (cap.canEncode && cap.canDecode)
                text << "encode/decode";
            else if (cap.canEncode)
                text << "encode";
            else if (cap.canDecode)
                text << "decode";
            else
                text << "transport-only";

            if (cap.hardwareAccelerated)
                text << " HW";

            if (cap.backend.isNotEmpty())
                text << " (" << cap.backend << ")";

            parts.add(text);
        }

        return parts.joinIntoString("; ");
    }

    juce::Image makeZap720pFrame(const juce::Image& source)
    {
        if (!source.isValid())
            return {};

        if (source.getWidth() == kZapVideoWidth
            && source.getHeight() == kZapVideoHeight
            && source.getFormat() == juce::Image::RGB)
        {
            return source;
        }

        juce::Image frame(juce::Image::RGB, kZapVideoWidth, kZapVideoHeight, true);
        juce::Graphics g(frame);
        g.fillAll(juce::Colours::black);
        g.drawImageWithin(source,
                          0,
                          0,
                          kZapVideoWidth,
                          kZapVideoHeight,
                          juce::RectanglePlacement::centred);
        return frame;
    }

    bool encodeMjpegFrame(const juce::Image& source, int jpegQuality, juce::MemoryBlock& outData)
    {
        const auto frame = makeZap720pFrame(source);
        if (!frame.isValid())
            return false;

        outData.reset();

        juce::JPEGImageFormat jpeg;
        jpeg.setQuality((float) normaliseQuality(jpegQuality) / 100.0f);

        juce::MemoryOutputStream stream(outData, false);
        return jpeg.writeImageToStream(frame, stream) && outData.getSize() > 0;
    }

    bool decodeMjpegFrame(const void* data, size_t dataSize, juce::Image& outImage)
    {
        if (data == nullptr || dataSize == 0)
            return false;

        juce::MemoryInputStream stream(data, dataSize, false);
        juce::JPEGImageFormat jpeg;
        auto decoded = jpeg.decodeImage(stream);

        if (!decoded.isValid())
            return false;

        outImage = std::move(decoded);
        return true;
    }

    int adaptJpegQualityForBandwidth(int currentQuality, size_t encodedBytes, size_t targetBytesPerFrame)
    {
        currentQuality = normaliseQuality(currentQuality);

        if (targetBytesPerFrame == 0 || encodedBytes == 0)
            return currentQuality;

        const auto target = (double) targetBytesPerFrame;
        const auto actual = (double) encodedBytes;

        if (actual > target * 1.20)
            return normaliseQuality(currentQuality - 8);

        if (actual > target * 1.05)
            return normaliseQuality(currentQuality - 3);

        if (actual < target * 0.65)
            return normaliseQuality(currentQuality + 4);

        return currentQuality;
    }

    void appendBigEndian32(juce::MemoryBlock& outData, juce::uint32 value)
    {
        const unsigned char bytes[4]
        {
            static_cast<unsigned char>((value >> 24) & 0xff),
            static_cast<unsigned char>((value >> 16) & 0xff),
            static_cast<unsigned char>((value >> 8) & 0xff),
            static_cast<unsigned char>(value & 0xff)
        };
        outData.append(bytes, sizeof(bytes));
    }

    juce::uint32 readBigEndian32(const void* data)
    {
        if (data == nullptr)
            return 0;

        const auto* bytes = static_cast<const unsigned char*>(data);
        return (static_cast<juce::uint32>(bytes[0]) << 24)
             | (static_cast<juce::uint32>(bytes[1]) << 16)
             | (static_cast<juce::uint32>(bytes[2]) << 8)
             | static_cast<juce::uint32>(bytes[3]);
    }

    bool appendLengthPrefixedChunk(const void* innerData, size_t innerSize, juce::MemoryBlock& outData)
    {
        if (innerSize > kZapMaxChunkPayloadBytes)
            return false;

        appendBigEndian32(outData, static_cast<juce::uint32>(innerSize));

        if (innerData != nullptr && innerSize > 0)
            outData.append(innerData, innerSize);

        return true;
    }

    bool makeSyncMarkerChunk(juce::uint32 intervalCounter, const unsigned char audioGuid[16], juce::MemoryBlock& outData)
    {
        if (audioGuid == nullptr)
            return false;

        juce::MemoryBlock markerPayload;
        appendBigEndian32(markerPayload, intervalCounter);
        markerPayload.append(audioGuid, 16);

        outData.reset();
        return appendLengthPrefixedChunk(markerPayload.getData(), markerPayload.getSize(), outData);
    }

    bool parseSyncMarkerPayload(const void* innerData, size_t innerSize, SyncMarker& outMarker)
    {
        if (innerData == nullptr || innerSize != 20)
            return false;

        const auto* bytes = static_cast<const unsigned char*>(innerData);
        outMarker.intervalCounter = readBigEndian32(bytes);
        std::copy(bytes + 4, bytes + 20, outMarker.audioGuid.begin());
        return true;
    }

    std::vector<juce::MemoryBlock> ChunkReassembler::pushBytes(const void* data, size_t dataSize)
    {
        std::vector<juce::MemoryBlock> chunks;

        if (data != nullptr && dataSize > 0)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            pendingBytes.insert(pendingBytes.end(), bytes, bytes + dataSize);
        }

        for (;;)
        {
            if (expectedPayloadBytes == 0)
            {
                if (pendingBytes.size() < 4)
                    break;

                expectedPayloadBytes = readBigEndian32(pendingBytes.data());
                pendingBytes.erase(pendingBytes.begin(), pendingBytes.begin() + 4);

                if (expectedPayloadBytes > kZapMaxChunkPayloadBytes)
                {
                    reset();
                    break;
                }
            }

            if (pendingBytes.size() < expectedPayloadBytes)
                break;

            juce::MemoryBlock chunk;
            if (expectedPayloadBytes > 0)
                chunk.append(pendingBytes.data(), expectedPayloadBytes);
            chunks.push_back(std::move(chunk));

            pendingBytes.erase(pendingBytes.begin(), pendingBytes.begin() + static_cast<std::ptrdiff_t>(expectedPayloadBytes));
            expectedPayloadBytes = 0;
        }

        return chunks;
    }

    void ChunkReassembler::reset()
    {
        pendingBytes.clear();
        expectedPayloadBytes = 0;
    }
}
