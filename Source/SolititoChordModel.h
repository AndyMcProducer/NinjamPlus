#pragma once

#include <JuceHeader.h>
#include <array>
#include <deque>
#include <memory>
#include <vector>

class SolititoChordModel
{
public:
    static constexpr int cqtBins = 144;
    static constexpr int chromaBins = 12;
    static constexpr int featureSize = cqtBins + chromaBins;
    static constexpr int contextFrames = 32;
    static constexpr int contextFloats = contextFrames * featureSize;

    struct Candidate
    {
        int trackIndex = -1;
        std::array<float, contextFloats> features {};
    };

    struct Prediction
    {
        int trackIndex = -1;
        int root = -1;
        int quality = 0;
        int intervals = 0;
        bool isNote = false;
        bool isNoise = false;
        float confidence = 0.0f;
    };

    explicit SolititoChordModel(int numTracks);
    ~SolititoChordModel();

    bool load(const juce::File& runtimeFile,
              const juce::File& modelFile,
              const juce::File& weightsFile);
    bool isAvailable() const;
    juce::String getStatus() const;
    int getMemoryKb() const;

    void resetTrack(int trackIndex);
    void appendSamples(int trackIndex,
                       const float* samples,
                       int numSamples,
                       int inputSampleRate,
                       std::vector<Candidate>& candidates);
    bool runBatch(const std::vector<Candidate>& candidates, std::vector<Prediction>& predictions);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
