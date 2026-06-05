// ProVideoDecoder_MF.cpp
#include "ProVideoDecoder.h"
#include "JuceHeader.h"

#ifndef NOMINMAX
#define NOMINMAX 1
#endif

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <cstring>

namespace
{
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

    static bool succeeded(HRESULT hr) noexcept { return SUCCEEDED(hr); }

    static int getDefaultStride(IMFMediaType* type, const GUID& subtype, int width)
    {
        LONG stride = 0;
        if (type != nullptr && SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, reinterpret_cast<UINT32*>(&stride))))
            return stride;

        if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32)
            return width * 4;

        return width;
    }

    static int clampToByte(int value) noexcept
    {
        return value < 0 ? 0 : (value > 255 ? 255 : value);
    }

    static bool imageFromRgb32(const BYTE* data, DWORD dataLen, int width, int height, int stride, juce::Image& outImage)
    {
        if (data == nullptr || width <= 0 || height <= 0)
            return false;

        const int absStride = std::abs(stride);
        if (absStride < width * 4 || (DWORD) (absStride * height) > dataLen)
            return false;

        juce::Image image(juce::Image::RGB, width, height, false);
        juce::Image::BitmapData bd(image, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < height; ++y)
        {
            const int sourceY = stride < 0 ? (height - 1 - y) : y;
            const BYTE* src = data + (size_t) sourceY * (size_t) absStride;
            for (int x = 0; x < width; ++x)
            {
                const BYTE b = src[(size_t) x * 4u + 0u];
                const BYTE g = src[(size_t) x * 4u + 1u];
                const BYTE r = src[(size_t) x * 4u + 2u];
                bd.setPixelColour(x, y, juce::Colour(r, g, b));
            }
        }

        outImage = std::move(image);
        return true;
    }

    static bool imageFromNv12(const BYTE* data, DWORD dataLen, int width, int height, int stride, juce::Image& outImage)
    {
        if (data == nullptr || width <= 0 || height <= 0)
            return false;

        const int yStride = std::max(std::abs(stride), width);
        const int uvStride = yStride;
        const size_t yBytes = (size_t) yStride * (size_t) height;
        const size_t uvBytes = (size_t) uvStride * (size_t) ((height + 1) / 2);
        if (yBytes + uvBytes > (size_t) dataLen)
            return false;

        const BYTE* yPlane = data;
        const BYTE* uvPlane = data + yBytes;

        juce::Image image(juce::Image::RGB, width, height, false);
        juce::Image::BitmapData bd(image, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < height; ++y)
        {
            const BYTE* yRow = yPlane + (size_t) y * (size_t) yStride;
            const BYTE* uvRow = uvPlane + (size_t) (y / 2) * (size_t) uvStride;
            for (int x = 0; x < width; ++x)
            {
                const int yy = (int) yRow[x] - 16;
                const int uvIndex = (x / 2) * 2;
                const int u = (int) uvRow[uvIndex] - 128;
                const int v = (int) uvRow[uvIndex + 1] - 128;
                const int c = std::max(0, yy);
                const int r = clampToByte((298 * c + 409 * v + 128) >> 8);
                const int g = clampToByte((298 * c - 100 * u - 208 * v + 128) >> 8);
                const int b = clampToByte((298 * c + 516 * u + 128) >> 8);
                bd.setPixelColour(x, y, juce::Colour((juce::uint8) r, (juce::uint8) g, (juce::uint8) b));
            }
        }

        outImage = std::move(image);
        return true;
    }
}

struct ProVideoDecoder::Impl
{
    ComPtr<IMFTransform> decoder;
    GUID outputSubtype = GUID_NULL;
    bool comInitialised = false;
    bool mfStarted = false;
    bool outputTypeSet = false;
    int width = 0;
    int height = 0;
    int stride = 0;
    LONGLONG sampleTime = 0;

    ~Impl()
    {
        close();
    }

    void close()
    {
        if (decoder)
            decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);

        decoder.reset();
        outputSubtype = GUID_NULL;
        outputTypeSet = false;
        width = 0;
        height = 0;
        stride = 0;
        sampleTime = 0;

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

    bool ensureOpen()
    {
        if (decoder)
            return true;

        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInitialised = coHr == S_OK || coHr == S_FALSE;
        if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE)
            return false;

        if (!succeeded(MFStartup(MF_VERSION)))
            return false;
        mfStarted = true;

        if (!succeeded(CoCreateInstance(CLSID_CMSH264DecoderMFT,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(decoder.put()))))
        {
            return false;
        }

        ComPtr<IMFMediaType> inputType;
        if (!succeeded(MFCreateMediaType(inputType.put())))
            return false;

        inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (!succeeded(decoder->SetInputType(0, inputType.get(), 0)))
            return false;

        decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool configureOutputType()
    {
        if (!decoder)
            return false;

        static const GUID preferredSubtypes[] =
        {
            MFVideoFormat_NV12,
            MFVideoFormat_RGB32,
            MFVideoFormat_ARGB32
        };

        for (const auto& preferred : preferredSubtypes)
        {
            for (DWORD index = 0;; ++index)
            {
                ComPtr<IMFMediaType> type;
                const HRESULT hr = decoder->GetOutputAvailableType(0, index, type.put());
                if (hr == MF_E_NO_MORE_TYPES)
                    break;
                if (FAILED(hr) || !type)
                    break;

                GUID subtype = GUID_NULL;
                if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != preferred)
                    continue;

                UINT32 w = 0;
                UINT32 h = 0;
                if (FAILED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0)
                    continue;

                if (!succeeded(decoder->SetOutputType(0, type.get(), 0)))
                    continue;

                outputSubtype = subtype;
                width = (int) w;
                height = (int) h;
                stride = getDefaultStride(type.get(), subtype, width);
                outputTypeSet = true;
                return true;
            }
        }

        return false;
    }

    bool makeInputSample(const void* data, int size, IMFSample** outSample)
    {
        if (outSample == nullptr || data == nullptr || size <= 0)
            return false;

        *outSample = nullptr;

        ComPtr<IMFMediaBuffer> buffer;
        if (!succeeded(MFCreateMemoryBuffer((DWORD) size, buffer.put())))
            return false;

        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        DWORD currentLen = 0;
        if (!succeeded(buffer->Lock(&dst, &maxLen, &currentLen)))
            return false;

        std::memcpy(dst, data, (size_t) size);
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD) size);

        ComPtr<IMFSample> sample;
        if (!succeeded(MFCreateSample(sample.put())))
            return false;

        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(sampleTime);
        sample->SetSampleDuration(333333);
        sampleTime += 333333;

        *outSample = sample.get();
        (*outSample)->AddRef();
        return true;
    }

    bool convertOutputSample(IMFSample* sample, juce::Image& outImage)
    {
        if (sample == nullptr || !outputTypeSet)
            return false;

        ComPtr<IMFMediaBuffer> contiguous;
        if (!succeeded(sample->ConvertToContiguousBuffer(contiguous.put())))
            return false;

        BYTE* src = nullptr;
        DWORD maxLen = 0;
        DWORD len = 0;
        if (!succeeded(contiguous->Lock(&src, &maxLen, &len)) || src == nullptr || len == 0)
            return false;

        const bool ok = outputSubtype == MFVideoFormat_NV12
            ? imageFromNv12(src, len, width, height, stride, outImage)
            : imageFromRgb32(src, len, width, height, stride, outImage);

        contiguous->Unlock();
        return ok;
    }

    bool drain(juce::Image& outImage)
    {
        if (!decoder)
            return false;

        bool produced = false;

        for (;;)
        {
            if (!outputTypeSet && !configureOutputType())
                return produced;

            MFT_OUTPUT_STREAM_INFO streamInfo {};
            if (!succeeded(decoder->GetOutputStreamInfo(0, &streamInfo)))
                return produced;

            ComPtr<IMFSample> outputSample;
            if ((streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0)
            {
                ComPtr<IMFMediaBuffer> outBuffer;
                const DWORD bufferSize = std::max<DWORD>(streamInfo.cbSize, (DWORD) (std::max(1, width) * std::max(1, height) * 4));
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
            const HRESULT hr = decoder->ProcessOutput(0, 1, &output, &status);

            if (output.pEvents != nullptr)
                output.pEvents->Release();

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return produced;

            if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET)
            {
                outputTypeSet = false;
                configureOutputType();
                continue;
            }

            if (FAILED(hr) || output.pSample == nullptr)
                return produced;

            juce::Image frame;
            if (convertOutputSample(output.pSample, frame) && frame.isValid())
            {
                outImage = std::move(frame);
                produced = true;
            }

            if (streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)
                output.pSample->Release();

            if (produced)
                return true;
        }
    }
};

ProVideoDecoder::ProVideoDecoder()
    : impl(std::make_unique<Impl>())
{
}

ProVideoDecoder::~ProVideoDecoder() = default;

bool ProVideoDecoder::decode(const void* data, int size, juce::Image& outImage)
{
    if (data == nullptr || size <= 0 || !impl->ensureOpen())
        return false;

    ComPtr<IMFSample> sample;
    IMFSample* rawSample = nullptr;
    if (!impl->makeInputSample(data, size, &rawSample))
        return false;
    *sample.put() = rawSample;

    HRESULT hr = impl->decoder->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING)
    {
        juce::Image drained;
        impl->drain(drained);
        hr = impl->decoder->ProcessInput(0, sample.get(), 0);
    }

    if (FAILED(hr))
        return false;

    return impl->drain(outImage);
}

void ProVideoDecoder::reset()
{
    impl->close();
}
