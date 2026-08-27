#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <rubberband/RubberBandLiveShifter.h>
#include <vector>
#include <atomic>
#include <cmath>

namespace ninjamplus
{

// =========================================================================
// PitchDetector — monophonic pitch detection with two quality modes
// =========================================================================
class PitchDetector
{
public:
    enum class Quality
    {
        Low,    // Autocorrelation — fast, lower accuracy
        High    // YIN — slower, better accuracy for voice
    };

    PitchDetector() = default;

    void prepare(double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        const int bufSize = juce::jmax(2048, maxBlockSize * 4);
        inputBuffer.resize((size_t)bufSize, 0.0f);
        writePos = 0;
        bufferedSamples = 0;
    }

    void reset()
    {
        std::fill(inputBuffer.begin(), inputBuffer.end(), 0.0f);
        writePos = 0;
        bufferedSamples = 0;
        lastFreq = 0.0f;
    }

    void setQuality(Quality q) { quality = q; }
    Quality getQuality() const { return quality; }

    // Push a block of mono samples and return the detected frequency in Hz,
    // or 0.0 if no clear pitch was found.
    float processBlock(const float* samples, int numSamples)
    {
        if (samples == nullptr || numSamples <= 0)
            return 0.0f;

        // Append to ring buffer
        for (int i = 0; i < numSamples; ++i)
        {
            inputBuffer[(size_t)writePos] = samples[i];
            writePos = (writePos + 1) % (int)inputBuffer.size();
        }
        bufferedSamples = juce::jmin((int)inputBuffer.size(), bufferedSamples + numSamples);

        // Need at least 1024 samples for reliable detection
        if (bufferedSamples < 1024)
            return lastFreq;

        // Extract a linear buffer from the ring
        const int analysisSize = juce::jmin(2048, bufferedSamples);
        linearBuffer.resize((size_t)analysisSize);
        int readPos = (writePos - analysisSize + (int)inputBuffer.size()) % (int)inputBuffer.size();
        for (int i = 0; i < analysisSize; ++i)
        {
            linearBuffer[(size_t)i] = inputBuffer[(size_t)readPos];
            readPos = (readPos + 1) % (int)inputBuffer.size();
        }

        // Check for silence / noise floor
        float rms = 0.0f;
        for (int i = 0; i < analysisSize; ++i)
            rms += linearBuffer[(size_t)i] * linearBuffer[(size_t)i];
        rms = std::sqrt(rms / analysisSize);
        if (rms < 0.001f) // ~ -60dB
        {
            lastFreq = 0.0f;
            return 0.0f;
        }

        const float freq = (quality == Quality::High)
            ? detectYIN(linearBuffer.data(), analysisSize)
            : detectAutocorrelation(linearBuffer.data(), analysisSize);

        // Smooth: limit max jump between frames to avoid glitches
        if (lastFreq > 0.0f && freq > 0.0f)
        {
            const float ratio = freq / lastFreq;
            if (ratio > 2.0f || ratio < 0.5f)
            {
                // Too big a jump — keep previous
                return lastFreq;
            }
        }

        lastFreq = freq;
        return freq;
    }

    float getLastFrequency() const { return lastFreq; }

private:
    Quality quality = Quality::Low;
    double sr = 44100.0;
    std::vector<float> inputBuffer;
    std::vector<float> linearBuffer;
    int writePos = 0;
    int bufferedSamples = 0;
    float lastFreq = 0.0f;

    float detectAutocorrelation(const float* data, int size) const
    {
        // Normalised autocorrelation, find first peak after zero-crossing
        const int minLag = (int)(sr / 1000.0);   // 1000 Hz max
        const int maxLag = (int)(sr / 70.0);     // 70 Hz min
        if (maxLag >= size) return 0.0f;

        float bestCorr = 0.0f;
        int bestLag = 0;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            float corr = 0.0f;
            float norm = 0.0f;
            for (int i = 0; i < size - lag; ++i)
            {
                corr += data[i] * data[i + lag];
                norm += data[i] * data[i];
            }
            if (norm < 1e-10f) continue;
            corr /= norm;

            if (corr > bestCorr)
            {
                bestCorr = corr;
                bestLag = lag;
            }
        }

        if (bestLag <= 0 || bestCorr < 0.3f)
            return 0.0f;

        return (float)(sr / bestLag);
    }

    float detectYIN(const float* data, int size) const
    {
        const int minLag = (int)(sr / 1000.0);
        const int maxLag = juce::jmin((int)(sr / 70.0), size / 2);
        if (maxLag <= minLag) return 0.0f;

        // Step 1: difference function
        std::vector<float> diff((size_t)(maxLag + 1), 0.0f);
        for (int lag = 0; lag <= maxLag; ++lag)
        {
            float sum = 0.0f;
            for (int i = 0; i < size - lag - maxLag; ++i)
                sum += (data[i] - data[i + lag]) * (data[i] - data[i + lag]);
            diff[(size_t)lag] = sum;
        }

        // Step 2: cumulative mean normalised difference
        std::vector<float> cmnd((size_t)(maxLag + 1), 1.0f);
        float runningSum = 0.0f;
        for (int lag = 1; lag <= maxLag; ++lag)
        {
            runningSum += diff[(size_t)lag];
            cmnd[(size_t)lag] = (runningSum > 1e-10f) ? diff[(size_t)lag] * lag / runningSum : 1.0f;
        }

        // Step 3: absolute threshold
        const float threshold = 0.15f;
        int tau = 0;
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            if (cmnd[(size_t)lag] < threshold)
            {
                // Find local minimum
                while (lag + 1 <= maxLag && cmnd[(size_t)lag + 1] < cmnd[(size_t)lag])
                    ++lag;
                tau = lag;
                break;
            }
        }

        if (tau <= 0)
        {
            // No value below threshold — take global minimum
            float minVal = 1.0f;
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                if (cmnd[(size_t)lag] < minVal)
                {
                    minVal = cmnd[(size_t)lag];
                    tau = lag;
                }
            }
            if (tau <= 0 || minVal > 0.5f)
                return 0.0f;
        }

        // Step 4: parabolic interpolation for sub-sample accuracy
        if (tau > minLag && tau < maxLag)
        {
            float s0 = cmnd[(size_t)tau - 1];
            float s1 = cmnd[(size_t)tau];
            float s2 = cmnd[(size_t)tau + 1];
            float denom = 2.0f * (2.0f * s1 - s2 - s0);
            if (std::abs(denom) > 1e-10f)
            {
                float betterTau = (float)tau + (s2 - s0) / denom;
                return (float)(sr / betterTau);
            }
        }

        return (float)(sr / tau);
    }
};

// =========================================================================
// ScaleQuantizer — snap a frequency to the nearest allowed note
// =========================================================================
class ScaleQuantizer
{
public:
    enum class Scale
    {
        Chromatic,
        Major,
        Minor,
        Dorian,
        Mixolydian,
        PentatonicMajor,
        PentatonicMinor
    };

    ScaleQuantizer() = default;

    void setScale(Scale s) { scale = s; }
    void setKey(int k) { key = juce::jlimit(0, 11, k); }
    Scale getScale() const { return scale; }
    int getKey() const { return key; }

    // Returns the target frequency in Hz for the given input frequency,
    // snapped to the nearest note in the current scale/key.
    float snapToScale(float inputHz) const
    {
        if (inputHz <= 0.0f)
            return inputHz;

        // Convert to MIDI note number (float)
        const float midiNote = 69.0f + 12.0f * std::log2(inputHz / 440.0f);
        const int nearestSemitone = (int)std::round(midiNote);
        const int pitchClass = ((nearestSemitone % 12) + 12) % 12;
        const int relativeClass = (pitchClass - key + 12) % 12;

        // Find nearest allowed pitch class in the scale
        const auto& intervals = getScaleIntervals(scale);
        int bestDist = 12;
        int bestClass = pitchClass;

        for (int interval : intervals)
        {
            int dist = std::abs(relativeClass - interval);
            dist = juce::jmin(dist, 12 - dist);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestClass = (key + interval) % 12;
            }
        }

        // Convert back to frequency
        const int targetNote = nearestSemitone - pitchClass + bestClass;
        const float targetHz = 440.0f * std::pow(2.0f, (targetNote - 69.0f) / 12.0f);
        return targetHz;
    }

    static const char* getScaleName(Scale s)
    {
        switch (s)
        {
            case Scale::Chromatic:       return "Chromatic";
            case Scale::Major:           return "Major";
            case Scale::Minor:           return "Minor";
            case Scale::Dorian:          return "Dorian";
            case Scale::Mixolydian:      return "Mixolydian";
            case Scale::PentatonicMajor: return "Pentatonic Major";
            case Scale::PentatonicMinor: return "Pentatonic Minor";
        }
        return "Unknown";
    }

    static const char* getKeyName(int k)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return (k >= 0 && k < 12) ? names[k] : "--";
    }

private:
    Scale scale = Scale::Chromatic;
    int key = 0; // 0 = C

    static const std::vector<int>& getScaleIntervals(Scale s)
    {
        static const std::vector<int> chromatic       = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        static const std::vector<int> major           = { 0, 2, 4, 5, 7, 9, 11 };
        static const std::vector<int> minor           = { 0, 2, 3, 5, 7, 8, 10 };
        static const std::vector<int> dorian          = { 0, 2, 3, 5, 7, 9, 10 };
        static const std::vector<int> mixolydian      = { 0, 2, 4, 5, 7, 9, 10 };
        static const std::vector<int> pentatonicMajor = { 0, 2, 4, 7, 9 };
        static const std::vector<int> pentatonicMinor = { 0, 3, 5, 7, 10 };

        switch (s)
        {
            case Scale::Chromatic:       return chromatic;
            case Scale::Major:           return major;
            case Scale::Minor:           return minor;
            case Scale::Dorian:          return dorian;
            case Scale::Mixolydian:      return mixolydian;
            case Scale::PentatonicMajor: return pentatonicMajor;
            case Scale::PentatonicMinor: return pentatonicMinor;
        }
        return chromatic;
    }
};

// =========================================================================
// AutoTuneProcessor — wraps RubberBandLiveShifter with pitch detection
// =========================================================================
class AutoTuneProcessor
{
public:
    AutoTuneProcessor() = default;
    ~AutoTuneProcessor() = default;

    void prepare(double sampleRate, int maxBlockSize)
    {
        sr = sampleRate;
        pitchDetector.prepare(sampleRate, maxBlockSize);

        // Create the RubberBandLiveShifter (mono, short window for low latency)
        const size_t channels = 1;
        const int options = RubberBand::RubberBandLiveShifter::OptionWindowShort
                          | RubberBand::RubberBandLiveShifter::OptionFormantPreserved;
        shifter = std::make_unique<RubberBand::RubberBandLiveShifter>(
            (size_t)sampleRate, channels, options);
        shifter->setPitchScale(1.0);
        shifter->setDebugLevel(0);

        blockSize = (int)shifter->getBlockSize();
        startDelay = (int)shifter->getStartDelay();

        // Allocate internal buffers
        shiftInput.resize((size_t)blockSize, 0.0f);
        shiftOutput.resize((size_t)blockSize, 0.0f);
        inputRing.resize((size_t)juce::jmax(blockSize * 4, maxBlockSize * 4), 0.0f);
        outputRing.resize((size_t)juce::jmax(blockSize * 4, maxBlockSize * 4), 0.0f);
        inputWritePos = 0;
        inputReadPos = 0;
        inputAvailable = 0;
        outputWritePos = 0;
        outputReadPos = 0;
        outputAvailable = 0;
        delaySamplesRemaining = startDelay;
        prepared = true;
    }

    void release()
    {
        prepared = false;
        shifter.reset();
        shiftInput.clear();
        shiftOutput.clear();
        inputRing.clear();
        outputRing.clear();
    }

    void reset()
    {
        if (!prepared || !shifter) return;
        std::fill(inputRing.begin(), inputRing.end(), 0.0f);
        std::fill(outputRing.begin(), outputRing.end(), 0.0f);
        std::fill(shiftInput.begin(), shiftInput.end(), 0.0f);
        std::fill(shiftOutput.begin(), shiftOutput.end(), 0.0f);
        inputWritePos = 0;
        inputReadPos = 0;
        inputAvailable = 0;
        outputWritePos = 0;
        outputReadPos = 0;
        outputAvailable = 0;
        delaySamplesRemaining = startDelay;
        pitchDetector.reset();
        shifter->reset();
        currentPitchRatio = 1.0;
    }

    void setEnabled(bool e) { enabled = e; }
    bool isEnabled() const { return enabled; }

    // Correction speed: 0.0 = very slow glide, 1.0 = instant snap
    void setCorrectionSpeed(float speed) { correctionSpeed = juce::jlimit(0.0f, 1.0f, speed); }
    float getCorrectionSpeed() const { return correctionSpeed; }

    void setQuality(PitchDetector::Quality q)
    {
        pitchDetector.setQuality(q);
        // Recreate shifter with appropriate window
        if (prepared)
        {
            const int options = (q == PitchDetector::Quality::High
                ? (int)RubberBand::RubberBandLiveShifter::OptionWindowMedium
                : (int)RubberBand::RubberBandLiveShifter::OptionWindowShort)
                | RubberBand::RubberBandLiveShifter::OptionFormantPreserved;
            const size_t channels = 1;
            shifter = std::make_unique<RubberBand::RubberBandLiveShifter>(
                (size_t)sr, channels, options);
            shifter->setPitchScale(1.0);
            shifter->setDebugLevel(0);
            blockSize = (int)shifter->getBlockSize();
            startDelay = (int)shifter->getStartDelay();
            shiftInput.assign((size_t)blockSize, 0.0f);
            shiftOutput.assign((size_t)blockSize, 0.0f);
            delaySamplesRemaining = startDelay;
        }
    }

    void setScale(ScaleQuantizer::Scale s) { quantizer.setScale(s); }
    void setKey(int k) { quantizer.setKey(k); }
    ScaleQuantizer::Scale getScale() const { return quantizer.getScale(); }
    int getKey() const { return quantizer.getKey(); }

    // Process a mono block in-place. The buffer is modified with pitch-corrected audio.
    void process(float* samples, int numSamples)
    {
        if (!prepared || !shifter || !enabled)
            return;

        // Push input into ring buffer
        for (int i = 0; i < numSamples; ++i)
        {
            inputRing[(size_t)inputWritePos] = samples[i];
            inputWritePos = (inputWritePos + 1) % (int)inputRing.size();
            inputAvailable++;
        }

        // Process as many full blocks as possible
        while (inputAvailable >= blockSize)
        {
            // Extract one block from input ring
            for (int i = 0; i < blockSize; ++i)
            {
                shiftInput[(size_t)i] = inputRing[(size_t)inputReadPos];
                inputReadPos = (inputReadPos + 1) % (int)inputRing.size();
            }
            inputAvailable -= blockSize;

            // Detect pitch and set pitch scale
            const float detectedHz = pitchDetector.processBlock(shiftInput.data(), blockSize);
            double targetRatio = 1.0;
            if (detectedHz > 50.0f)
            {
                const float targetHz = quantizer.snapToScale(detectedHz);
                targetRatio = (double)targetHz / (double)detectedHz;
            }

            // Smooth the ratio based on correction speed
            // speed=1.0: instant snap, speed=0.0: very slow glide
            const double smoothCoeff = juce::jlimit(0.001, 1.0, (double)correctionSpeed);
            currentPitchRatio += (targetRatio - currentPitchRatio) * smoothCoeff;
            shifter->setPitchScale(currentPitchRatio);

            // Shift
            const float* inPtrs[1] = { shiftInput.data() };
            float* outPtrs[1] = { shiftOutput.data() };
            shifter->shift(inPtrs, outPtrs);

            // Push output into output ring
            for (int i = 0; i < blockSize; ++i)
            {
                outputRing[(size_t)outputWritePos] = shiftOutput[(size_t)i];
                outputWritePos = (outputWritePos + 1) % (int)outputRing.size();
                outputAvailable++;
            }
        }

        // Read back corrected samples, handling start delay
        for (int i = 0; i < numSamples; ++i)
        {
            if (outputAvailable > 0 && delaySamplesRemaining <= 0)
            {
                samples[i] = outputRing[(size_t)outputReadPos];
                outputReadPos = (outputReadPos + 1) % (int)outputRing.size();
                outputAvailable--;
            }
            else
            {
                if (delaySamplesRemaining > 0)
                    delaySamplesRemaining--;
                samples[i] = 0.0f; // silence during initial delay
            }
        }
    }

    int getLatencySamples() const { return startDelay; }

private:
    bool prepared = false;
    bool enabled = false;
    float correctionSpeed = 1.0f;   // 0=slow glide, 1=instant snap
    double currentPitchRatio = 1.0; // smoothed ratio
    double sr = 44100.0;
    int blockSize = 0;
    int startDelay = 0;
    int delaySamplesRemaining = 0;

    std::unique_ptr<RubberBand::RubberBandLiveShifter> shifter;
    PitchDetector pitchDetector;
    ScaleQuantizer quantizer;

    std::vector<float> shiftInput;
    std::vector<float> shiftOutput;
    std::vector<float> inputRing;
    std::vector<float> outputRing;
    int inputWritePos = 0, inputReadPos = 0, inputAvailable = 0;
    int outputWritePos = 0, outputReadPos = 0, outputAvailable = 0;
};

} // namespace ninjamplus
