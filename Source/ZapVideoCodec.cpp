#include "ZapVideoCodec.h"

#include <algorithm>
#include <cstring>
#include <memory>

#if !defined(NINJAMPLUS_HAS_LIBJPEG_TURBO)
#define NINJAMPLUS_HAS_LIBJPEG_TURBO 0
#endif

#if !defined(NINJAMPLUS_HAS_H264_DECODE)
#define NINJAMPLUS_HAS_H264_DECODE 0
#endif

#if NINJAMPLUS_HAS_LIBJPEG_TURBO
#include <turbojpeg.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <comdef.h>
#endif

namespace ninjamplus::zap
{
    namespace
    {
        int normaliseQuality(int quality)
        {
            return juce::jlimit(kZapJpegMinQuality, kZapJpegMaxQuality, quality);
        }

        void appendBigEndian16(juce::MemoryBlock& outData, juce::uint16 value)
        {
            const unsigned char bytes[2]
            {
                static_cast<unsigned char>((value >> 8) & 0xff),
                static_cast<unsigned char>(value & 0xff)
            };
            outData.append(bytes, sizeof(bytes));
        }

        struct NalUnit
        {
            const unsigned char* data = nullptr;
            size_t size = 0;
        };

        std::vector<NalUnit> splitAnnexB(const void* data, size_t size)
        {
            std::vector<NalUnit> nals;
            const auto* bytes = static_cast<const unsigned char*>(data);
            if (bytes == nullptr || size < 5)
                return nals;

            auto findStart = [bytes, size](size_t from) -> std::pair<size_t, size_t>
            {
                for (size_t i = from; i + 3 < size; ++i)
                {
                    if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
                        return { i, 3 };
                    if (i + 4 < size && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1)
                        return { i, 4 };
                }
                return { size, 0 };
            };

            auto current = findStart(0);
            while (current.second != 0)
            {
                const size_t nalStart = current.first + current.second;
                const auto next = findStart(nalStart);
                const size_t nalEnd = next.second != 0 ? next.first : size;
                if (nalEnd > nalStart)
                    nals.push_back({ bytes + nalStart, nalEnd - nalStart });
                current = next;
            }

            return nals;
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
               #if NINJAMPLUS_HAS_LIBJPEG_TURBO
                return { true, true, false, "libjpeg-turbo" };
               #else
                return { true, true, false, "JUCE JPEG" };
               #endif

            case VideoCodec::h264:
               #if defined(_WIN32)
                return { true, NINJAMPLUS_HAS_H264_DECODE != 0, true, "Media Foundation H.264" };
               #elif NINJAMPLUS_HAS_H264_DECODE
               #if defined(__APPLE__)
                return { false, true, true, "VideoToolbox H.264 decode" };
               #elif defined(__linux__)
                return { false, true, true, "GStreamer H.264 decode" };
               #else
                return { false, true, false, "native H.264 decode" };
               #endif
               #else
                return { false, false, false, "H.264 backend not compiled" };
               #endif

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

#if NINJAMPLUS_HAS_LIBJPEG_TURBO
        // Encode with libjpeg-turbo for better performance when available.
        const juce::Image rgb = frame;
        juce::Image::BitmapData bd(rgb, juce::Image::BitmapData::readOnly);
        const int w = rgb.getWidth();
        const int h = rgb.getHeight();

        std::vector<unsigned char> srcBuf((size_t)w * h * 3);
        for (int y = 0; y < h; ++y)
            std::memcpy(srcBuf.data() + (size_t)y * w * 3, bd.getLinePointer(y), (size_t)w * 3);

        tjhandle tj = tjInitCompress();
        if (!tj)
            return false;

        unsigned char* jpegBuf = nullptr;
        unsigned long jpegSize = 0;
        const int quality = normaliseQuality(jpegQuality);
        if (tjCompress2(tj,
                        srcBuf.data(),
                        w,
                        0,
                        h,
                        TJPF_RGB,
                        &jpegBuf,
                        &jpegSize,
                        quality,
                        TJFLAG_FASTDCT) < 0)
        {
            tjDestroy(tj);
            return false;
        }

        outData.append(jpegBuf, (size_t)jpegSize);
        tjFree(jpegBuf);
        tjDestroy(tj);
        return outData.getSize() > 0;
#else
        juce::JPEGImageFormat jpeg;
        jpeg.setQuality((float) normaliseQuality(jpegQuality) / 100.0f);

        juce::MemoryOutputStream stream(outData, false);
        return jpeg.writeImageToStream(frame, stream) && outData.getSize() > 0;
#endif
    }

    bool decodeMjpegFrame(const void* data, size_t dataSize, juce::Image& outImage)
    {
        if (data == nullptr || dataSize == 0)
            return false;

    #if NINJAMPLUS_HAS_LIBJPEG_TURBO
        tjhandle tj = tjInitDecompress();
        if (!tj)
            goto juce_fallback;

        int w = 0, h = 0, subsamp = 0, colorspace = 0;
        if (tjDecompressHeader3(tj, static_cast<unsigned char*>(const_cast<void*>(data)), (unsigned long)dataSize, &w, &h, &subsamp, &colorspace) < 0)
        {
            tjDestroy(tj);
            goto juce_fallback;
        }

        std::vector<unsigned char> dst((size_t)w * h * 3);
        if (tjDecompress2(tj, static_cast<unsigned char*>(const_cast<void*>(data)), (unsigned long)dataSize,
                          dst.data(), w, 0, h, TJPF_RGB, 0) < 0)
        {
            tjDestroy(tj);
            goto juce_fallback;
        }

        tjDestroy(tj);

        juce::Image img(juce::Image::RGB, w, h, false);
        juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
            std::memcpy(bd.getLinePointer(y), dst.data() + (size_t)y * w * 3, (size_t)w * 3);

        outImage = std::move(img);
        return true;
    juce_fallback:;
    #endif

        juce::MemoryInputStream stream(data, dataSize, false);
        juce::JPEGImageFormat jpeg;
        auto decoded = jpeg.decodeImage(stream);

        if (!decoded.isValid())
            return false;

        outImage = std::move(decoded);
        return true;
    }

#if defined(_WIN32)
    template <typename T>
    class ComPtr
    {
    public:
        ComPtr() = default;
        ~ComPtr() { reset(); }

        ComPtr(const ComPtr&) = delete;
        ComPtr& operator=(const ComPtr&) = delete;

        T* get() const noexcept { return ptr; }
        T** put()
        {
            reset();
            return &ptr;
        }
        T* operator->() const noexcept { return ptr; }
        explicit operator bool() const noexcept { return ptr != nullptr; }

        void reset()
        {
            if (ptr != nullptr)
            {
                ptr->Release();
                ptr = nullptr;
            }
        }

    private:
        T* ptr = nullptr;
    };

    static bool succeeded(HRESULT hr) { return SUCCEEDED(hr); }

    static void setCodecApiUInt32(IMFTransform* transform, const GUID& key, juce::uint32 value)
    {
        if (transform == nullptr)
            return;

        ComPtr<ICodecAPI> codecApi;
        if (FAILED(transform->QueryInterface(IID_PPV_ARGS(codecApi.put()))))
            return;

        VARIANT var;
        VariantInit(&var);
        var.vt = VT_UI4;
        var.ulVal = value;
        codecApi->SetValue(&key, &var);
        VariantClear(&var);
    }

    static std::vector<unsigned char> imageToNV12(const juce::Image& source)
    {
        const auto frame = makeZap720pFrame(source);
        if (!frame.isValid())
            return {};

        const int width = frame.getWidth();
        const int height = frame.getHeight();
        std::vector<unsigned char> nv12((size_t) width * (size_t) height * 3u / 2u, 0);
        auto* yPlane = nv12.data();
        auto* uvPlane = nv12.data() + (size_t) width * (size_t) height;

        juce::Image::BitmapData bd(frame, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const auto c = bd.getPixelColour(x, y);
                const int r = c.getRed();
                const int g = c.getGreen();
                const int b = c.getBlue();
                yPlane[(size_t) y * width + x] = (unsigned char) juce::jlimit(0, 255, ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
        }

        for (int y = 0; y < height; y += 2)
        {
            for (int x = 0; x < width; x += 2)
            {
                int uSum = 0;
                int vSum = 0;
                for (int yy = 0; yy < 2; ++yy)
                {
                    for (int xx = 0; xx < 2; ++xx)
                    {
                        const auto c = bd.getPixelColour(x + xx, y + yy);
                        const int r = c.getRed();
                        const int g = c.getGreen();
                        const int b = c.getBlue();
                        uSum += juce::jlimit(0, 255, ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                        vSum += juce::jlimit(0, 255, ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                    }
                }

                const size_t uvIndex = (size_t) (y / 2) * width + x;
                uvPlane[uvIndex] = (unsigned char) (uSum / 4);
                uvPlane[uvIndex + 1] = (unsigned char) (vSum / 4);
            }
        }

        return nv12;
    }

    struct H264Encoder::Impl
    {
        ComPtr<IMFTransform> encoder;
        bool comInitialised = false;
        bool mfStarted = false;
        int width = 0;
        int height = 0;
        int fps = 0;
        LONGLONG frameTime = 0;
        LONGLONG frameDuration = 0;
        juce::MemoryBlock lastConfigInner;
        juce::String backendName;

        ~Impl()
        {
            close();
        }

        void close()
        {
            if (encoder)
                encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            encoder.reset();
            lastConfigInner.reset();
            backendName.clear();
            if (mfStarted)
            {
                MFShutdown();
                mfStarted = false;
            }
            if (comInitialised)
            {
                CoUninitialize();
                comInitialised = false;
            }
        }

        static void unlockAsyncTransformIfNeeded(IMFTransform* transform)
        {
            if (transform == nullptr)
                return;

            ComPtr<IMFAttributes> attributes;
            if (FAILED(transform->GetAttributes(attributes.put())) || !attributes)
                return;

            UINT32 isAsync = 0;
            if (SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync)) && isAsync != 0)
                attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }

        bool createHardwareEncoder()
        {
            MFT_REGISTER_TYPE_INFO inputInfo {};
            inputInfo.guidMajorType = MFMediaType_Video;
            inputInfo.guidSubtype = MFVideoFormat_NV12;

            MFT_REGISTER_TYPE_INFO outputInfo {};
            outputInfo.guidMajorType = MFMediaType_Video;
            outputInfo.guidSubtype = MFVideoFormat_H264;

            IMFActivate** activations = nullptr;
            UINT32 count = 0;
            const HRESULT enumHr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                             MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                             &inputInfo,
                                             &outputInfo,
                                             &activations,
                                             &count);

            if (FAILED(enumHr) || activations == nullptr || count == 0)
            {
                if (activations != nullptr)
                    CoTaskMemFree(activations);
                return false;
            }

            bool opened = false;
            for (UINT32 i = 0; i < count; ++i)
            {
                if (activations[i] == nullptr)
                    continue;

                ComPtr<IMFTransform> candidate;
                if (SUCCEEDED(activations[i]->ActivateObject(IID_PPV_ARGS(candidate.put()))) && candidate)
                {
                    unlockAsyncTransformIfNeeded(candidate.get());
                    encoder.reset();
                    *encoder.put() = candidate.get();
                    candidate.get()->AddRef();
                    backendName = "Media Foundation H.264 hardware";
                    opened = true;
                    break;
                }
            }

            for (UINT32 i = 0; i < count; ++i)
            {
                if (activations[i] != nullptr)
                    activations[i]->Release();
            }
            CoTaskMemFree(activations);
            return opened;
        }

        bool createSoftwareEncoder()
        {
            if (!succeeded(CoCreateInstance(CLSID_CMSH264EncoderMFT,
                                            nullptr,
                                            CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(encoder.put()))))
            {
                return false;
            }

            backendName = "Media Foundation H.264 software";
            return true;
        }

        bool createEncoder(H264EncoderPreference preference)
        {
            if (preference == H264EncoderPreference::hardwareOnly)
                return createHardwareEncoder();

            if (preference == H264EncoderPreference::softwareOnly)
                return createSoftwareEncoder();

            return createHardwareEncoder() || createSoftwareEncoder();
        }

        bool open(int w, int h, int frameRate, int bitrate, H264EncoderPreference preference)
        {
            close();

            width = w;
            height = h;
            fps = juce::jmax(1, frameRate);
            frameTime = 0;
            frameDuration = 10000000LL / fps;

            const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInitialised = coHr == S_OK || coHr == S_FALSE;
            if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
                return false;

            if (!succeeded(MFStartup(MF_VERSION)))
                return false;
            mfStarted = true;

            if (!createEncoder(preference))
                return false;

            setCodecApiUInt32(encoder.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            setCodecApiUInt32(encoder.get(), CODECAPI_AVEncCommonMeanBitRate, (juce::uint32) bitrate);
            setCodecApiUInt32(encoder.get(), CODECAPI_AVEncMPVGOPSize, (juce::uint32) fps);
            setCodecApiUInt32(encoder.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
            setCodecApiUInt32(encoder.get(), CODECAPI_AVLowLatencyMode, 1);

            ComPtr<IMFMediaType> outputType;
            if (!succeeded(MFCreateMediaType(outputType.put())))
                return false;
            outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            outputType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32) bitrate);
            outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(outputType.get(), MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
            MFSetAttributeRatio(outputType.get(), MF_MT_FRAME_RATE, (UINT32) fps, 1);
            MFSetAttributeRatio(outputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (!succeeded(encoder->SetOutputType(0, outputType.get(), 0)))
                return false;

            ComPtr<IMFMediaType> inputType;
            if (!succeeded(MFCreateMediaType(inputType.put())))
                return false;
            inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(inputType.get(), MF_MT_FRAME_SIZE, (UINT32) width, (UINT32) height);
            MFSetAttributeRatio(inputType.get(), MF_MT_FRAME_RATE, (UINT32) fps, 1);
            MFSetAttributeRatio(inputType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (!succeeded(encoder->SetInputType(0, inputType.get(), 0)))
                return false;

            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            return true;
        }

        bool makeSample(const std::vector<unsigned char>& nv12, IMFSample** outSample)
        {
            if (outSample == nullptr || nv12.empty())
                return false;

            ComPtr<IMFMediaBuffer> buffer;
            if (!succeeded(MFCreateMemoryBuffer((DWORD) nv12.size(), buffer.put())))
                return false;

            BYTE* dst = nullptr;
            DWORD maxLen = 0;
            DWORD currentLen = 0;
            if (!succeeded(buffer->Lock(&dst, &maxLen, &currentLen)))
                return false;

            std::memcpy(dst, nv12.data(), nv12.size());
            buffer->Unlock();
            buffer->SetCurrentLength((DWORD) nv12.size());

            ComPtr<IMFSample> sample;
            if (!succeeded(MFCreateSample(sample.put())))
                return false;
            sample->AddBuffer(buffer.get());
            sample->SetSampleTime(frameTime);
            sample->SetSampleDuration(frameDuration);
            frameTime += frameDuration;

            *outSample = sample.get();
            (*outSample)->AddRef();
            return true;
        }

        bool drainOutput(EncodedH264Frame& outFrame)
        {
            MFT_OUTPUT_STREAM_INFO streamInfo {};
            if (!succeeded(encoder->GetOutputStreamInfo(0, &streamInfo)))
                return false;

            bool produced = false;
            for (;;)
            {
                ComPtr<IMFSample> outputSample;
                if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
                {
                    ComPtr<IMFMediaBuffer> outBuffer;
                    const DWORD bufferSize = juce::jmax<DWORD>(streamInfo.cbSize, (DWORD) (width * height));
                    if (!succeeded(MFCreateMemoryBuffer(bufferSize, outBuffer.put())))
                        return produced;
                    if (!succeeded(MFCreateSample(outputSample.put())))
                        return produced;
                    outputSample->AddBuffer(outBuffer.get());
                }

                MFT_OUTPUT_DATA_BUFFER output {};
                output.dwStreamID = 0;
                output.pSample = outputSample.get();
                DWORD status = 0;
                const HRESULT hr = encoder->ProcessOutput(0, 1, &output, &status);

                if (output.pEvents != nullptr)
                    output.pEvents->Release();

                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                    return produced;

                if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
                    continue;

                if (FAILED(hr) || output.pSample == nullptr)
                    return produced;

                ComPtr<IMFMediaBuffer> contiguous;
                if (succeeded(output.pSample->ConvertToContiguousBuffer(contiguous.put())))
                {
                    BYTE* src = nullptr;
                    DWORD maxLen = 0;
                    DWORD len = 0;
                    if (succeeded(contiguous->Lock(&src, &maxLen, &len)) && src != nullptr && len > 0)
                    {
                        processOutputBytes(src, len, outFrame);
                        produced = outFrame.configChunk.getSize() > 0 || outFrame.frameChunk.getSize() > 0;
                        contiguous->Unlock();
                    }
                }

                if (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
                    output.pSample->Release();
            }
        }

        void processOutputBytes(const void* data, size_t size, EncodedH264Frame& outFrame)
        {
            const auto nals = splitAnnexB(data, size);
            if (nals.empty())
                return;

            NalUnit sps;
            NalUnit pps;
            juce::MemoryBlock frameInner;

            for (const auto& nal : nals)
            {
                if (nal.size == 0 || nal.data == nullptr)
                    continue;

                const unsigned char nalType = nal.data[0] & 0x1f;
                if (nalType == 7)
                    sps = nal;
                else if (nalType == 8)
                    pps = nal;
                else
                {
                    appendBigEndian32(frameInner, (juce::uint32) nal.size);
                    frameInner.append(nal.data, nal.size);
                }
            }

            if (sps.size > 0 && pps.size > 0
                && sps.size <= 0xffff && pps.size <= 0xffff)
            {
                juce::MemoryBlock configInner;
                appendBigEndian16(configInner, (juce::uint16) sps.size);
                configInner.append(sps.data, sps.size);
                appendBigEndian16(configInner, (juce::uint16) pps.size);
                configInner.append(pps.data, pps.size);

                if (configInner.getSize() > 0
                    && (lastConfigInner.getSize() != configInner.getSize()
                        || std::memcmp(lastConfigInner.getData(), configInner.getData(), configInner.getSize()) != 0))
                {
                    lastConfigInner = configInner;
                    appendLengthPrefixedChunk(configInner.getData(), configInner.getSize(), outFrame.configChunk);
                }
            }

            if (frameInner.getSize() > 0)
                appendLengthPrefixedChunk(frameInner.getData(), frameInner.getSize(), outFrame.frameChunk);
        }
    };
#else
    struct H264Encoder::Impl
    {
        juce::String backendName;
        bool open(int, int, int, int, H264EncoderPreference) { return false; }
        void close() {}
        bool encodeFrame(const juce::Image&, EncodedH264Frame&) { return false; }
    };
#endif

    H264Encoder::H264Encoder()
        : impl(std::make_unique<Impl>())
    {
    }

    H264Encoder::~H264Encoder() = default;

    bool H264Encoder::open(int width, int height, int fps, int bitrateBitsPerSecond, H264EncoderPreference preference)
    {
        return impl->open(width, height, fps, bitrateBitsPerSecond, preference);
    }

    void H264Encoder::close()
    {
        impl->close();
    }

    bool H264Encoder::isOpen() const
    {
       #if defined(_WIN32)
        return impl->encoder.get() != nullptr;
       #else
        return false;
       #endif
    }

    juce::String H264Encoder::getBackendName() const
    {
       #if defined(_WIN32)
        return impl->backendName;
       #else
        return {};
       #endif
    }

    bool H264Encoder::encodeFrame(const juce::Image& source, EncodedH264Frame& outFrame)
    {
       #if defined(_WIN32)
        if (!isOpen())
            return false;

        outFrame = {};
        const auto nv12 = imageToNV12(source);
        if (nv12.empty())
            return false;

        ComPtr<IMFSample> sample;
        IMFSample* rawSample = nullptr;
        if (!impl->makeSample(nv12, &rawSample))
            return false;
        sample.reset();
        *sample.put() = rawSample;

        const HRESULT inputHr = impl->encoder->ProcessInput(0, sample.get(), 0);
        if (FAILED(inputHr))
            return false;

        return impl->drainOutput(outFrame);
       #else
        juce::ignoreUnused(source, outFrame);
        return false;
       #endif
    }

    bool H264Encoder::isAvailable()
    {
       #if defined(_WIN32)
        return true;
       #else
        return false;
       #endif
    }

    bool H264Encoder::isHardwareAvailable()
    {
       #if defined(_WIN32)
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool didInitCom = coHr == S_OK || coHr == S_FALSE;
        if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
            return false;

        if (!succeeded(MFStartup(MF_VERSION)))
        {
            if (didInitCom)
                CoUninitialize();
            return false;
        }

        MFT_REGISTER_TYPE_INFO inputInfo {};
        inputInfo.guidMajorType = MFMediaType_Video;
        inputInfo.guidSubtype = MFVideoFormat_NV12;

        MFT_REGISTER_TYPE_INFO outputInfo {};
        outputInfo.guidMajorType = MFMediaType_Video;
        outputInfo.guidSubtype = MFVideoFormat_H264;

        IMFActivate** activations = nullptr;
        UINT32 count = 0;
        const HRESULT enumHr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                                         &inputInfo,
                                         &outputInfo,
                                         &activations,
                                         &count);

        if (activations != nullptr)
        {
            for (UINT32 i = 0; i < count; ++i)
                if (activations[i] != nullptr)
                    activations[i]->Release();
            CoTaskMemFree(activations);
        }

        MFShutdown();
        if (didInitCom)
            CoUninitialize();

        return SUCCEEDED(enumHr) && count > 0;
       #else
        return false;
       #endif
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
