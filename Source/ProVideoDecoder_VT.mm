// ProVideoDecoder_VT.mm
#include "ProVideoDecoder.h"

#import <VideoToolbox/VideoToolbox.h>

#include "JuceHeader.h"

#include <vector>

namespace
{
    struct NalUnit
    {
        const unsigned char* data = nullptr;
        size_t size = 0;
    };

    static bool isStartCode(const unsigned char* data, size_t size, size_t offset, size_t& codeSize)
    {
        if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1)
        {
            codeSize = 3;
            return true;
        }

        if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1)
        {
            codeSize = 4;
            return true;
        }

        return false;
    }

    static std::vector<NalUnit> splitAnnexB(const void* data, int size)
    {
        std::vector<NalUnit> nals;
        const auto* bytes = static_cast<const unsigned char*>(data);
        if (bytes == nullptr || size <= 0)
            return nals;

        const size_t total = (size_t) size;
        size_t pos = 0;
        while (pos < total)
        {
            size_t codeSize = 0;
            while (pos < total && !isStartCode(bytes, total, pos, codeSize))
                ++pos;

            if (pos >= total)
                break;

            const size_t nalStart = pos + codeSize;
            pos = nalStart;

            size_t nextCodeSize = 0;
            while (pos < total && !isStartCode(bytes, total, pos, nextCodeSize))
                ++pos;

            if (pos > nalStart)
                nals.push_back({ bytes + nalStart, pos - nalStart });
        }

        return nals;
    }

    static void appendBe32(juce::MemoryBlock& block, size_t value)
    {
        const unsigned char bytes[4]
        {
            (unsigned char) ((value >> 24) & 0xff),
            (unsigned char) ((value >> 16) & 0xff),
            (unsigned char) ((value >> 8) & 0xff),
            (unsigned char) (value & 0xff)
        };
        block.append(bytes, sizeof(bytes));
    }
}

struct ProVideoDecoder::Impl
{
    ~Impl()
    {
        reset();
    }

    void reset()
    {
        if (session != nullptr)
        {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }

        if (formatDescription != nullptr)
        {
            CFRelease(formatDescription);
            formatDescription = nullptr;
        }

        sps.clear();
        pps.clear();
        lastImage = {};
    }

    bool decode(const void* data, int size, juce::Image& outImage)
    {
        const auto nals = splitAnnexB(data, size);
        if (nals.empty())
            return false;

        juce::MemoryBlock sampleData;
        bool sawDecodeNal = false;

        for (const auto& nal : nals)
        {
            if (nal.size == 0)
                continue;

            const unsigned char type = nal.data[0] & 0x1f;
            if (type == 7)
            {
                sps.assign(nal.data, nal.data + nal.size);
                rebuildSession();
                continue;
            }

            if (type == 8)
            {
                pps.assign(nal.data, nal.data + nal.size);
                rebuildSession();
                continue;
            }

            appendBe32(sampleData, nal.size);
            sampleData.append(nal.data, nal.size);
            if (type == 1 || type == 5)
                sawDecodeNal = true;
        }

        if (!sawDecodeNal || sampleData.getSize() == 0 || !ensureSession())
            return false;

        CMBlockBufferRef block = nullptr;
        OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,
                                                             const_cast<void*>(sampleData.getData()),
                                                             sampleData.getSize(),
                                                             kCFAllocatorNull,
                                                             nullptr,
                                                             0,
                                                             sampleData.getSize(),
                                                             0,
                                                             &block);
        if (status != noErr || block == nullptr)
            return false;

        CMSampleBufferRef sample = nullptr;
        const size_t sampleSize = sampleData.getSize();
        status = CMSampleBufferCreateReady(kCFAllocatorDefault,
                                           block,
                                           formatDescription,
                                           1,
                                           0,
                                           nullptr,
                                           1,
                                           &sampleSize,
                                           &sample);
        CFRelease(block);

        if (status != noErr || sample == nullptr)
            return false;

        lastImage = {};
        VTDecodeFrameFlags flags = 0;
        VTDecodeInfoFlags infoFlags = 0;
        status = VTDecompressionSessionDecodeFrame(session,
                                                   sample,
                                                   flags,
                                                   this,
                                                   &infoFlags);
        VTDecompressionSessionWaitForAsynchronousFrames(session);
        CFRelease(sample);

        if (status != noErr || !lastImage.isValid())
            return false;

        outImage = lastImage;
        return true;
    }

    bool ensureSession()
    {
        if (session != nullptr)
            return true;

        return rebuildSession();
    }

    bool rebuildSession()
    {
        if (sps.empty() || pps.empty())
            return false;

        if (session != nullptr)
        {
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }

        if (formatDescription != nullptr)
        {
            CFRelease(formatDescription);
            formatDescription = nullptr;
        }

        const uint8_t* parameterSets[] { sps.data(), pps.data() };
        const size_t parameterSetSizes[] { sps.size(), pps.size() };
        OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault,
                                                                              2,
                                                                              parameterSets,
                                                                              parameterSetSizes,
                                                                              4,
                                                                              &formatDescription);
        if (status != noErr || formatDescription == nullptr)
            return false;

        const OSType pixelFormat = kCVPixelFormatType_32BGRA;
        CFNumberRef pixelFormatNumber = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &pixelFormat);
        const void* keys[] { kCVPixelBufferPixelFormatTypeKey };
        const void* values[] { pixelFormatNumber };
        CFDictionaryRef attrs = CFDictionaryCreate(kCFAllocatorDefault,
                                                   keys,
                                                   values,
                                                   1,
                                                   &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);

        VTDecompressionOutputCallbackRecord callback {};
        callback.decompressionOutputCallback = &decodeCallback;
        callback.decompressionOutputRefCon = this;

        status = VTDecompressionSessionCreate(kCFAllocatorDefault,
                                              formatDescription,
                                              nullptr,
                                              attrs,
                                              &callback,
                                              &session);

        if (attrs != nullptr)
            CFRelease(attrs);
        if (pixelFormatNumber != nullptr)
            CFRelease(pixelFormatNumber);

        if (status != noErr || session == nullptr)
            return false;

        return true;
    }

    static void decodeCallback(void* refCon,
                               void*,
                               OSStatus status,
                               VTDecodeInfoFlags,
                               CVImageBufferRef imageBuffer,
                               CMTime,
                               CMTime)
    {
        if (status != noErr || refCon == nullptr || imageBuffer == nullptr)
            return;

        auto* self = static_cast<Impl*>(refCon);
        CVPixelBufferRef pixelBuffer = (CVPixelBufferRef) imageBuffer;
        CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

        const int width = (int) CVPixelBufferGetWidth(pixelBuffer);
        const int height = (int) CVPixelBufferGetHeight(pixelBuffer);
        const auto* base = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddress(pixelBuffer));
        const size_t stride = CVPixelBufferGetBytesPerRow(pixelBuffer);

        if (base != nullptr && width > 0 && height > 0)
        {
            juce::Image img(juce::Image::RGB, width, height, false);
            juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < height; ++y)
            {
                const auto* src = base + (size_t) y * stride;
                for (int x = 0; x < width; ++x)
                {
                    const auto* px = src + (size_t) x * 4u;
                    bd.setPixelColour(x, y, juce::Colour(px[2], px[1], px[0]));
                }
            }
            self->lastImage = img;
        }

        CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    }

    CMVideoFormatDescriptionRef formatDescription = nullptr;
    VTDecompressionSessionRef session = nullptr;
    std::vector<unsigned char> sps;
    std::vector<unsigned char> pps;
    juce::Image lastImage;
};

ProVideoDecoder::ProVideoDecoder()
    : impl(std::make_unique<Impl>())
{
}

ProVideoDecoder::~ProVideoDecoder() = default;

bool ProVideoDecoder::decode(const void* data, int size, juce::Image& outImage)
{
    return impl != nullptr && impl->decode(data, size, outImage);
}

void ProVideoDecoder::reset()
{
    if (impl != nullptr)
        impl->reset();
}
