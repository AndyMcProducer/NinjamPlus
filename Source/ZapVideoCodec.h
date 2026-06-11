#pragma once

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <vector>

namespace ninjamplus::zap
{
    enum class VideoCodec
    {
        unknown,
        mjpeg,
        h264,
        vp8,
        vp9
    };

    struct VideoCodecCapability
    {
        bool canEncode = false;
        bool canDecode = false;
        bool hardwareAccelerated = false;
        juce::String backend;
    };

    enum class CameraCodecPreference
    {
        autoCodec,
        h264,
        mjpeg,
        h264Hardware,
        h264Software
    };

    enum class H264EncoderPreference
    {
        autoHardware,
        hardwareOnly,
        softwareOnly
    };

    constexpr int kZapVideoWidth = 1280;
    constexpr int kZapVideoHeight = 720;
    constexpr int kZapVideoFps = 30;
    constexpr int kZapJpegDefaultQuality = 72;
    constexpr int kZapJpegMinQuality = 30;
    constexpr int kZapJpegMaxQuality = 90;
    constexpr size_t kZapMaxChunkPayloadBytes = 16 * 1024 * 1024;

    struct SyncMarker
    {
        juce::uint32 intervalCounter = 0;
        std::array<unsigned char, 16> audioGuid {};
    };

    juce::String getCodecName(VideoCodec codec);
    VideoCodecCapability getCodecCapability(VideoCodec codec);
    juce::String getCodecCapabilitySummary();

    juce::Image makeZap720pFrame(const juce::Image& source);
    bool encodeMjpegFrame(const juce::Image& source, int jpegQuality, juce::MemoryBlock& outData);
    bool decodeMjpegFrame(const void* data, size_t dataSize, juce::Image& outImage);
    int adaptJpegQualityForBandwidth(int currentQuality, size_t encodedBytes, size_t targetBytesPerFrame);

    struct EncodedH264Frame
    {
        juce::MemoryBlock configChunk;
        juce::MemoryBlock frameChunk;
    };

    class H264Encoder
    {
    public:
        H264Encoder();
        ~H264Encoder();

        bool open(int width, int height, int fps, int bitrateBitsPerSecond,
                  H264EncoderPreference preference = H264EncoderPreference::autoHardware);
        void close();
        bool isOpen() const;
        juce::String getBackendName() const;
        bool encodeFrame(const juce::Image& source, EncodedH264Frame& outFrame);

        static bool isAvailable();
        static bool isHardwareAvailable();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    void appendBigEndian32(juce::MemoryBlock& outData, juce::uint32 value);
    juce::uint32 readBigEndian32(const void* data);
    bool appendLengthPrefixedChunk(const void* innerData, size_t innerSize, juce::MemoryBlock& outData);
    bool makeSyncMarkerChunk(juce::uint32 intervalCounter, const unsigned char audioGuid[16], juce::MemoryBlock& outData);
    bool parseSyncMarkerPayload(const void* innerData, size_t innerSize, SyncMarker& outMarker);

    class ChunkReassembler
    {
    public:
        std::vector<juce::MemoryBlock> pushBytes(const void* data, size_t dataSize);
        void reset();
        bool hasBufferedData() const noexcept { return !pendingBytes.empty(); }

    private:
        std::vector<unsigned char> pendingBytes;
        size_t expectedPayloadBytes = 0;
    };
}
