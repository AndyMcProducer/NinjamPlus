// ProVideoDecoder.h
#pragma once

#include <memory>

namespace juce { class Image; }

class ProVideoDecoder
{
public:
    ProVideoDecoder();
    ~ProVideoDecoder();

    // Decode encoded H.264 frame data into a JUCE Image (RGB). Returns true on success.
    bool decode(const void* data, int size, juce::Image& outImage);

    // Reset decoder state
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
