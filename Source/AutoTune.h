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
// TuningPreset — frequency-ratio table for any tuning system
// =========================================================================
struct TuningPreset
{
    const char* name = "";
    std::vector<double> ratios;   // frequency ratios from root, in [1, octaveRatio)
    double octaveRatio = 2.0;     // 2.0 = octave, 3.0 = tritave (Bohlen-Pierce)
};

// =========================================================================
// ScaleQuantizer — snap a frequency to the nearest allowed note
// Supports standard 12-TET scales, microtonal EDO systems, just intonation,
// world music tunings, and instrument-specific tunings.
// =========================================================================
class ScaleQuantizer
{
public:
    enum class Scale
    {
        // Standard 12-TET (0-6)
        Chromatic, Major, Minor, Dorian, Mixolydian, PentatonicMajor, PentatonicMinor,
        // Microtonal EDO & Just (7-12)
        Tet24, Edo22, Edo31, BohlenPierce, JustIntonation7, JustIntonation11,
        // World (13-23)
        IndianShruti, Maqam, GamelanSlendro, GamelanPelog, DeltaBlues, Georgian,
        ChinesePentatonic, JapaneseHirajoshi, KoreanPentatonic, African5tone, African7tone,
        // Instruments (24-27)
        SitarShruti, OudMaqam, Bagpipe, HonkyTonkPiano,
        // Hip-Hop (28-30)
        TrapMinor, PhrygianDominant, MelodicPentatonic,
        Count
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

        const auto& preset = getTuningPreset(scale);
        if (preset.ratios.empty())
            return inputHz;

        // Root frequency based on key (key 0 = C, key 9 = A = 440 Hz)
        const double rootHz = 440.0 * std::pow(2.0, (double)(key - 9) / 12.0);

        // Search over several octaves to find nearest pitch
        double bestDist = 1e9;
        double bestHz = (double)inputHz;

        for (int oct = -3; oct <= 5; ++oct)
        {
            const double octaveMult = std::pow(preset.octaveRatio, (double)oct);
            for (double r : preset.ratios)
            {
                const double candidateHz = rootHz * octaveMult * r;
                if (candidateHz <= 0.0)
                    continue;
                const double dist = std::abs(std::log2((double)inputHz / candidateHz));
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestHz = candidateHz;
                }
            }
        }

        return (float)bestHz;
    }

    static const char* getScaleName(Scale s)
    {
        return getTuningPreset(s).name;
    }

    static const char* getKeyName(int k)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return (k >= 0 && k < 12) ? names[k] : "--";
    }

    static int getNumScales() { return (int)Scale::Count; }

    // Category indices for menu organisation
    static int getFirstStandardScale()   { return 0; }
    static int getNumStandardScales()    { return 7; }
    static int getFirstMicrotonalScale() { return 7; }
    static int getNumMicrotonalScales()  { return 6; }
    static int getFirstWorldScale()      { return 13; }
    static int getNumWorldScales()       { return 11; }
    static int getFirstInstrumentScale() { return 24; }
    static int getNumInstrumentScales()  { return 4; }
    static int getFirstHiphopScale()     { return 28; }
    static int getNumHiphopScales()      { return 3; }

private:
    Scale scale = Scale::Chromatic;
    int key = 0; // 0 = C

    static const TuningPreset& getTuningPreset(Scale s)
    {
        // Helper: generate EDO ratios
        static auto makeEdo = [](int divisions) {
            std::vector<double> r;
            r.reserve((size_t)divisions);
            for (int i = 0; i < divisions; ++i)
                r.push_back(std::pow(2.0, (double)i / (double)divisions));
            return r;
        };

        // Helper: generate Bohlen-Pierce ratios (tritave divided into 13)
        static auto makeBP = []() {
            std::vector<double> r;
            r.reserve(13);
            for (int i = 0; i < 13; ++i)
                r.push_back(std::pow(3.0, (double)i / 13.0));
            return r;
        };

        // Helper: cents to ratio
        static auto cents = [](double c) { return std::pow(2.0, c / 1200.0); };

        // Helper: vector of cents to ratios
        static auto fromCents = [](std::initializer_list<double> cs) {
            std::vector<double> r;
            for (double c : cs)
                r.push_back(std::pow(2.0, c / 1200.0));
            return r;
        };

        // Helper: just intonation ratios
        static auto justRatios = [](std::initializer_list<double> rs) {
            return std::vector<double>(rs);
        };

        static const TuningPreset presets[] = {
            // ── Standard 12-TET (0-6) ──
            { "Chromatic",       makeEdo(12), 2.0 },
            { "Major",           justRatios({1, std::pow(2.0,2.0/12), std::pow(2.0,4.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,9.0/12), std::pow(2.0,11.0/12)}), 2.0 },
            { "Minor",           justRatios({1, std::pow(2.0,2.0/12), std::pow(2.0,3.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,8.0/12), std::pow(2.0,10.0/12)}), 2.0 },
            { "Dorian",          justRatios({1, std::pow(2.0,2.0/12), std::pow(2.0,3.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,9.0/12), std::pow(2.0,10.0/12)}), 2.0 },
            { "Mixolydian",      justRatios({1, std::pow(2.0,2.0/12), std::pow(2.0,4.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,9.0/12), std::pow(2.0,10.0/12)}), 2.0 },
            { "Pentatonic Major",justRatios({1, std::pow(2.0,2.0/12), std::pow(2.0,4.0/12), std::pow(2.0,7.0/12), std::pow(2.0,9.0/12)}), 2.0 },
            { "Pentatonic Minor",justRatios({1, std::pow(2.0,3.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,10.0/12)}), 2.0 },

            // ── Microtonal EDO & Just (7-12) ──
            { "24-TET (Quarter-Tones)", makeEdo(24), 2.0 },
            { "22-EDO",                  makeEdo(22), 2.0 },
            { "31-EDO",                  makeEdo(31), 2.0 },
            { "Bohlen-Pierce",           makeBP(),     3.0 }, // tritave
            { "Just Intonation (7-Limit)", justRatios({1.0, 16.0/15, 9.0/8, 6.0/5, 5.0/4, 4.0/3, 7.0/5, 3.0/2, 8.0/5, 5.0/3, 7.0/4, 15.0/8}), 2.0 },
            { "Just Intonation (11-Limit)",justRatios({1.0, 16.0/15, 9.0/8, 6.0/5, 5.0/4, 4.0/3, 11.0/8, 3.0/2, 8.0/5, 5.0/3, 7.0/4, 11.0/6, 15.0/8}), 2.0 },

            // ── World (13-23) ──
            // Indian Classical: 22 Shruti just-intonation ratios
            { "Indian Classical (22 Shruti)", justRatios({
                1.0, 256.0/243, 16.0/15, 10.0/9, 9.0/8, 32.0/27, 6.0/5, 5.0/4,
                81.0/64, 4.0/3, 27.0/20, 45.0/32, 3.0/2, 128.0/81, 8.0/5, 5.0/3,
                27.0/16, 16.0/9, 9.0/5, 15.0/8, 243.0/128
            }), 2.0 },
            // Middle Eastern Maqam: 17-tone with quarter-tone intervals
            { "Middle Eastern Maqam", fromCents({
                0, 90, 150, 204, 294, 350, 408, 498, 588, 650, 702,
                792, 850, 906, 996, 1050, 1110
            }), 2.0 },
            // Indonesian Gamelan Slendro: 5 near-equal divisions
            { "Gamelan Slendro", fromCents({0, 231, 474, 717, 955}), 2.0 },
            // Indonesian Gamelan Pelog: 7 unequal divisions
            { "Gamelan Pelog", fromCents({0, 120, 258, 540, 663, 785, 945}), 2.0 },
            // American Delta Blues: blue notes with quarter-tone bends
            { "Delta Blues", fromCents({
                0, 200, 300, 350, 400, 500, 550, 600, 700, 800, 900, 950, 1000, 1100, 1150
            }), 2.0 },
            // Georgian Adaptive: approximated 10-tone scale
            { "Georgian Adaptive", fromCents({0, 150, 300, 400, 520, 670, 820, 970, 1080}), 2.0 },
            // Chinese Pentatonic (Just Intonation)
            { "Chinese Pentatonic (Just)", justRatios({1.0, 9.0/8, 5.0/4, 3.0/2, 5.0/3}), 2.0 },
            // Japanese Hirajoshi
            { "Japanese Hirajoshi", fromCents({0, 200, 340, 500, 700, 800, 1000, 1140}), 2.0 },
            // Korean Pentatonic
            { "Korean Pentatonic", fromCents({0, 200, 350, 500, 700, 900, 1050}), 2.0 },
            // African Equidistant 5-tone
            { "African 5-Tone (Equidistant)", fromCents({0, 240, 480, 720, 960}), 2.0 },
            // African Equidistant 7-tone
            { "African 7-Tone (Equidistant)", fromCents({0, 171, 343, 514, 686, 857, 1029}), 2.0 },

            // ── Instruments (24-27) ──
            // Sitar: 22 Shruti (same ratios as Indian Classical)
            { "Sitar (22 Shruti)", justRatios({
                1.0, 256.0/243, 16.0/15, 10.0/9, 9.0/8, 32.0/27, 6.0/5, 5.0/4,
                81.0/64, 4.0/3, 27.0/20, 45.0/32, 3.0/2, 128.0/81, 8.0/5, 5.0/3,
                27.0/16, 16.0/9, 9.0/5, 15.0/8, 243.0/128
            }), 2.0 },
            // Fretless Oud: extended maqam with finer quarter-tone resolution
            { "Fretless Oud (Maqam Extended)", fromCents({
                0, 50, 90, 150, 204, 250, 294, 350, 408, 450, 498, 550, 588,
                650, 702, 750, 792, 850, 906, 950, 996, 1050, 1110, 1150
            }), 2.0 },
            // Bagpipe: Just Intonation Mixolydian (Highland bagpipe approximation)
            { "Bagpipe (Just Mixolydian)", justRatios({1.0, 9.0/8, 5.0/4, 4.0/3, 3.0/2, 5.0/3, 16.0/9}), 2.0 },
            // Honky-Tonk Piano: 12-TET with per-note detuning
            { "Honky-Tonk Piano (Detuned)", fromCents({
                3, 98, 204, 299, 402, 497, 601, 696, 802, 897, 1003, 1098
            }), 2.0 },

            // ── Hip-Hop (28-30) ──
            // Trap Minor (Aeolian) — dark trap standard, ~80% of dark trap beats
            { "Trap Minor (Aeolian)", justRatios({1, std::pow(2.0,2.0/12), std::pow(2.0,3.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,8.0/12), std::pow(2.0,10.0/12)}), 2.0 },
            // Phrygian Dominant — minor with flat 2nd, menacing/exotic dark energy
            { "Phrygian Dominant (Dark Trap)", justRatios({1, std::pow(2.0,1.0/12), std::pow(2.0,3.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,8.0/12), std::pow(2.0,10.0/12)}), 2.0 },
            // Melodic Pentatonic — no half-steps, ideal for sung triplet flows
            { "Melodic Pentatonic (Triplet Flow)", justRatios({1, std::pow(2.0,3.0/12), std::pow(2.0,5.0/12), std::pow(2.0,7.0/12), std::pow(2.0,10.0/12)}), 2.0 },
        };

        const int idx = (int)s;
        if (idx >= 0 && idx < (int)Scale::Count)
            return presets[idx];
        return presets[0];
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
