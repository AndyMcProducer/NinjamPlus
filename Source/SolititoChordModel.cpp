#include "SolititoChordModel.h"
#include "ChordDetector.h"

#ifndef NINJAMPLUS_HAS_ONNX_CHORDS
#define NINJAMPLUS_HAS_ONNX_CHORDS 0
#endif

#if NINJAMPLUS_HAS_ONNX_CHORDS
#include <onnxruntime_cxx_api.h>
#endif

#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr int targetSampleRate = 16000;
constexpr int fftSize = 8192;
constexpr int fftOrder = 13;
constexpr int hopLength = 256;
constexpr int analysisHopStride = 4;
constexpr int analysisHopLength = hopLength * analysisHopStride;
constexpr int inferenceFrameStride = 2;
constexpr int fftBins = fftSize / 2 + 1;
constexpr float minRefLevel = 0.02f;
constexpr float silenceRmsThreshold = 0.001f;

struct WeightHeader
{
    char magic[8];
    juce::uint32 fftSize = 0;
    juce::uint32 sampleRate = 0;
    juce::uint32 cqtCount = 0;
    juce::uint32 chromaCount = 0;
};

bool readLittleEndianUInt32(const uint8_t*& data, const uint8_t* end, juce::uint32& out)
{
    if (end - data < 4)
        return false;

    out = (juce::uint32)data[0]
        | ((juce::uint32)data[1] << 8)
        | ((juce::uint32)data[2] << 16)
        | ((juce::uint32)data[3] << 24);
    data += 4;
    return true;
}

bool readFloatBlock(const uint8_t*& data, const uint8_t* end, std::vector<float>& out, size_t count)
{
    const size_t bytes = count * sizeof(float);
    if ((size_t)(end - data) < bytes)
        return false;

    out.resize(count);
    std::memcpy(out.data(), data, bytes);
    data += bytes;
    return true;
}

int argmaxSoftmax(const float* logits, int count, float& confidence)
{
    int best = 0;
    float bestLogit = logits[0];
    float maxLogit = logits[0];

    for (int i = 1; i < count; ++i)
    {
        if (logits[i] > bestLogit)
        {
            bestLogit = logits[i];
            best = i;
        }
        if (logits[i] > maxLogit)
            maxLogit = logits[i];
    }

    float sum = 0.0f;
    for (int i = 0; i < count; ++i)
        sum += std::exp(logits[i] - maxLogit);

    confidence = sum > 0.0f ? std::exp(bestLogit - maxLogit) / sum : 0.0f;
    return best;
}
}

struct SolititoChordModel::Impl
{
    struct Track
    {
        std::vector<float> inputAccumulator;
        std::vector<float> resampledAudio;
        std::deque<std::array<float, featureSize>> history;
        int inputSampleRate = 0;
        int framesSinceCandidate = 0;
        float runningRef = 0.05f;
    };

    explicit Impl(int numTracksIn)
        : numTracks(juce::jmax(1, numTracksIn)), tracks((size_t)numTracks), fft(fftOrder)
    {
        fftInput.resize((size_t)fftSize);
        fftOutput.resize((size_t)fftSize);
        window.resize((size_t)fftSize);

        for (int i = 0; i < fftSize; ++i)
        {
            const float n = (float)i;
            const float size = (float)(fftSize - 1);
            window[(size_t)i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * n / size));
        }
    }

    bool load(const juce::File& modelFile, const juce::File& weightsFile)
    {
        available = false;
        status = {};

        if (!weightsFile.existsAsFile())
        {
            status = "Missing Solitito DSP weights: " + weightsFile.getFullPathName();
            return false;
        }

        if (!loadWeights(weightsFile))
            return false;

#if NINJAMPLUS_HAS_ONNX_CHORDS
        if (!modelFile.existsAsFile())
        {
            status = "Missing Solitito ONNX model: " + modelFile.getFullPathName();
            return false;
        }

        try
        {
            sessionOptions.SetIntraOpNumThreads(1);
            sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
           #if JUCE_WINDOWS
            session = std::make_unique<Ort::Session>(env, modelFile.getFullPathName().toWideCharPointer(), sessionOptions);
           #else
            session = std::make_unique<Ort::Session>(env, modelFile.getFullPathName().toRawUTF8(), sessionOptions);
           #endif
            available = true;
            status = "Solitito ONNX chord model loaded";
            return true;
        }
        catch (const Ort::Exception& e)
        {
            status = "Solitito ONNX load failed: " + juce::String(e.what());
            session.reset();
            return false;
        }
#else
        juce::ignoreUnused(modelFile);
        status = "ONNX chord support is not compiled in";
        return false;
#endif
    }

    bool loadWeights(const juce::File& weightsFile)
    {
        juce::MemoryBlock block;
        if (!weightsFile.loadFileAsData(block))
        {
            status = "Could not read Solitito DSP weights";
            return false;
        }

        const auto* data = static_cast<const uint8_t*>(block.getData());
        const auto* end = data + block.getSize();
        if (end - data < 24 || std::memcmp(data, "SOLIDSP1", 8) != 0)
        {
            status = "Invalid Solitito DSP weights header";
            return false;
        }
        data += 8;

        juce::uint32 headerFft = 0;
        juce::uint32 headerSr = 0;
        juce::uint32 cqtCount = 0;
        juce::uint32 chromaCount = 0;
        if (!readLittleEndianUInt32(data, end, headerFft)
            || !readLittleEndianUInt32(data, end, headerSr)
            || !readLittleEndianUInt32(data, end, cqtCount)
            || !readLittleEndianUInt32(data, end, chromaCount))
        {
            status = "Truncated Solitito DSP weights header";
            return false;
        }

        if (headerFft != fftSize || headerSr != targetSampleRate
            || cqtCount != (juce::uint32)(fftBins * cqtBins)
            || chromaCount != (juce::uint32)(cqtBins * chromaBins))
        {
            status = "Solitito DSP weights dimensions do not match the model";
            return false;
        }

        if (!readFloatBlock(data, end, cqtRe, cqtCount)
            || !readFloatBlock(data, end, cqtIm, cqtCount)
            || !readFloatBlock(data, end, chromaWeights, chromaCount))
        {
            status = "Truncated Solitito DSP weights data";
            return false;
        }

        return true;
    }

    bool isAvailable() const { return available; }

    int getMemoryKb() const
    {
        const size_t weightBytes = (cqtRe.size() + cqtIm.size() + chromaWeights.size()) * sizeof(float);
        const size_t trackBytes = (size_t)numTracks * (fftSize * 2 + contextFloats * contextFrames) * sizeof(float);
        return (int)((weightBytes + trackBytes + 1023) / 1024);
    }

    void resetTrack(int trackIndex)
    {
        if (!isValidTrack(trackIndex))
            return;

        auto& track = tracks[(size_t)trackIndex];
        track.inputAccumulator.clear();
        track.resampledAudio.clear();
        track.history.clear();
        track.inputSampleRate = 0;
        track.framesSinceCandidate = 0;
        track.runningRef = 0.05f;
    }

    void appendSamples(int trackIndex,
                       const float* samples,
                       int numSamples,
                       int inputSampleRate,
                       std::vector<Candidate>& candidates)
    {
        if (!available || !isValidTrack(trackIndex) || samples == nullptr || numSamples <= 0)
            return;

        auto& track = tracks[(size_t)trackIndex];
        const int safeRate = juce::jlimit(8000, 192000, inputSampleRate > 1000 ? inputSampleRate : targetSampleRate);
        if (track.inputSampleRate != safeRate)
        {
            resetTrack(trackIndex);
            track.inputSampleRate = safeRate;
        }

        track.inputAccumulator.insert(track.inputAccumulator.end(), samples, samples + numSamples);
        resamplePendingInput(track, safeRate);
        processReadyWindows(track, trackIndex, candidates);
    }

    void resamplePendingInput(Track& track, int inputSampleRate)
    {
        const double ratio = (double)inputSampleRate / (double)targetSampleRate;
        if (ratio <= 0.0 || track.inputAccumulator.size() < 2)
            return;

        const int targetSamples = (int)std::floor((double)track.inputAccumulator.size() / ratio);
        if (targetSamples <= 0)
            return;

        const size_t oldSize = track.resampledAudio.size();
        track.resampledAudio.resize(oldSize + (size_t)targetSamples);

        for (int i = 0; i < targetSamples; ++i)
        {
            const double src = (double)i * ratio;
            const int idx = (int)src;
            const float frac = (float)(src - (double)idx);
            float sample = 0.0f;
            if (idx + 1 < (int)track.inputAccumulator.size())
            {
                const float s0 = track.inputAccumulator[(size_t)idx];
                const float s1 = track.inputAccumulator[(size_t)(idx + 1)];
                sample = s0 + frac * (s1 - s0);
            }
            track.resampledAudio[oldSize + (size_t)i] = sample;
        }

        const int used = juce::jlimit(0, (int)track.inputAccumulator.size(), (int)std::floor((double)targetSamples * ratio));
        if (used > 0)
            track.inputAccumulator.erase(track.inputAccumulator.begin(), track.inputAccumulator.begin() + used);
    }

    void processReadyWindows(Track& track, int trackIndex, std::vector<Candidate>& candidates)
    {
        while ((int)track.resampledAudio.size() >= fftSize)
        {
            const float* chunk = track.resampledAudio.data();
            double energy = 0.0;
            for (int i = 0; i < fftSize; ++i)
                energy += (double)chunk[i] * (double)chunk[i];

            const float rms = (float)std::sqrt(energy / (double)fftSize);
            if (rms >= silenceRmsThreshold)
            {
                std::array<float, featureSize> frame {};
                computeFrame(track, chunk, frame);
                pushFeatureFrame(track, trackIndex, frame, candidates);
            }
            else
            {
                track.history.clear();
                track.framesSinceCandidate = 0;
            }

            track.resampledAudio.erase(track.resampledAudio.begin(), track.resampledAudio.begin() + analysisHopLength);
        }
    }

    void pushFeatureFrame(Track& track,
                          int trackIndex,
                          const std::array<float, featureSize>& frame,
                          std::vector<Candidate>& candidates)
    {
        if ((int)track.history.size() >= contextFrames)
            track.history.pop_front();
        track.history.push_back(frame);

        if ((int)track.history.size() < contextFrames)
            return;

        if (++track.framesSinceCandidate < inferenceFrameStride)
            return;
        track.framesSinceCandidate = 0;

        Candidate candidate;
        candidate.trackIndex = trackIndex;
        size_t offset = 0;
        for (const auto& historyFrame : track.history)
        {
            std::copy(historyFrame.begin(), historyFrame.end(), candidate.features.begin() + (ptrdiff_t)offset);
            offset += featureSize;
        }
        candidates.push_back(candidate);
    }

    void computeFrame(Track& track, const float* chunk, std::array<float, featureSize>& frame)
    {
        for (int i = 0; i < fftSize; ++i)
        {
            fftInput[(size_t)i].real(chunk[i] * window[(size_t)i]);
            fftInput[(size_t)i].imag(0.0f);
        }

        fft.perform(fftInput.data(), fftOutput.data(), false);

        float cqt[cqtBins] = {};
        for (int i = 0; i < cqtBins; ++i)
        {
            float sumRe = 0.0f;
            float sumIm = 0.0f;
            for (int k = 0; k < fftBins; ++k)
            {
                const int idx = k * cqtBins + i;
                const float wr = cqtRe[(size_t)idx];
                const float wi = cqtIm[(size_t)idx];
                const auto& fv = fftOutput[(size_t)k];
                sumRe += fv.real() * wr - fv.imag() * wi;
                sumIm += fv.real() * wi + fv.imag() * wr;
            }
            cqt[i] = std::sqrt(sumRe * sumRe + sumIm * sumIm);
        }

        for (int i = 0; i < 28; ++i)
            cqt[i] *= 0.1f;

        float frameMax = 0.0f;
        for (float value : cqt)
            frameMax = juce::jmax(frameMax, value);

        if (frameMax > track.runningRef)
            track.runningRef = track.runningRef * 0.5f + frameMax * 0.5f;
        else
            track.runningRef = track.runningRef * 0.995f + frameMax * 0.005f;

        const float effectiveRef = juce::jmax(track.runningRef, minRefLevel);
        for (int i = 0; i < cqtBins; ++i)
        {
            const float val = juce::jmax(cqt[i], 1.0e-12f);
            const float db = 20.0f * std::log10(val / effectiveRef);
            float norm = juce::jlimit(0.0f, 1.0f, (db + 80.0f) / 80.0f);
            norm = norm < 0.20f ? 0.0f : (norm - 0.20f) / 0.80f;
            cqt[i] = norm;
            frame[(size_t)i] = norm;
        }

        float chroma[chromaBins] = {};
        for (int i = 0; i < chromaBins; ++i)
        {
            float sum = 0.0f;
            for (int k = 0; k < cqtBins; ++k)
                sum += cqt[k] * chromaWeights[(size_t)(k * chromaBins + i)];
            chroma[i] = sum;
        }

        float chromaMax = 1.0e-9f;
        for (float value : chroma)
            chromaMax = juce::jmax(chromaMax, value);

        for (int i = 0; i < chromaBins; ++i)
            frame[(size_t)(cqtBins + i)] = chroma[i] / chromaMax;
    }

    bool runBatch(const std::vector<Candidate>& candidates, std::vector<Prediction>& predictions)
    {
#if NINJAMPLUS_HAS_ONNX_CHORDS
        predictions.clear();
        if (!available || session == nullptr || candidates.empty())
            return false;

        const int batch = (int)candidates.size();
        std::vector<float> inputData((size_t)batch * contextFloats, 0.0f);
        for (int i = 0; i < batch; ++i)
            std::copy(candidates[(size_t)i].features.begin(), candidates[(size_t)i].features.end(), inputData.begin() + (ptrdiff_t)((size_t)i * contextFloats));

        std::array<int64_t, 3> inputShape { batch, contextFrames, featureSize };
        auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);
        auto inputTensor = Ort::Value::CreateTensor<float>(memoryInfo,
                                                           inputData.data(),
                                                           inputData.size(),
                                                           inputShape.data(),
                                                           inputShape.size());

        const char* inputNames[] = { "in" };
        const char* outputNames[] = { "out_root", "out_qual" };

        try
        {
            auto outputs = session->Run(Ort::RunOptions { nullptr }, inputNames, &inputTensor, 1, outputNames, 2);
            const float* rootData = outputs[0].GetTensorData<float>();
            const float* qualData = outputs[1].GetTensorData<float>();

            predictions.reserve(candidates.size());
            for (int i = 0; i < batch; ++i)
            {
                float rootConf = 0.0f;
                float qualConf = 0.0f;
                const int rootIdx = argmaxSoftmax(rootData + i * 13, 13, rootConf);
                const int qualIdx = argmaxSoftmax(qualData + i * 10, 10, qualConf);

                Prediction prediction;
                prediction.trackIndex = candidates[(size_t)i].trackIndex;
                prediction.root = rootIdx;
                prediction.confidence = rootConf * qualConf;

                mapQuality(qualIdx, prediction);
                if (rootIdx == 12)
                    prediction.isNoise = true;

                predictions.push_back(prediction);
            }
            return true;
        }
        catch (const Ort::Exception& e)
        {
            status = "Solitito ONNX run failed: " + juce::String(e.what());
            return false;
        }
#else
        juce::ignoreUnused(candidates, predictions);
        return false;
#endif
    }

    static void mapQuality(int qualIdx, Prediction& prediction)
    {
        prediction.quality = ChordDetector::Major;
        prediction.intervals = 0;
        prediction.isNote = false;

        switch (qualIdx)
        {
            case 0: prediction.quality = ChordDetector::Major; break;
            case 1: prediction.quality = ChordDetector::Minor; break;
            case 2: prediction.quality = ChordDetector::Dominant; prediction.intervals = 7; break;
            case 3: prediction.quality = ChordDetector::Major; prediction.intervals = 7; break;
            case 4: prediction.quality = ChordDetector::Minor; prediction.intervals = 7; break;
            case 5: prediction.quality = ChordDetector::Dimished5th; prediction.intervals = 7; break;
            case 6: prediction.quality = ChordDetector::Dimished5th; prediction.intervals = 5; break;
            case 7: prediction.quality = ChordDetector::Dominant; prediction.intervals = 9; break;
            case 8: prediction.quality = ChordDetector::Dominant; prediction.intervals = 13; break;
            case 9: prediction.isNote = true; break;
            default: break;
        }
    }

    bool isValidTrack(int trackIndex) const
    {
        return trackIndex >= 0 && trackIndex < numTracks;
    }

    int numTracks = 0;
    std::vector<Track> tracks;
    juce::dsp::FFT fft;
    std::vector<juce::dsp::Complex<float>> fftInput;
    std::vector<juce::dsp::Complex<float>> fftOutput;
    std::vector<float> window;
    std::vector<float> cqtRe;
    std::vector<float> cqtIm;
    std::vector<float> chromaWeights;
    bool available = false;
    juce::String status;

#if NINJAMPLUS_HAS_ONNX_CHORDS
    Ort::Env env { ORT_LOGGING_LEVEL_WARNING, "NINJAMplusSolititoChord" };
    Ort::SessionOptions sessionOptions;
    std::unique_ptr<Ort::Session> session;
#endif
};

SolititoChordModel::SolititoChordModel(int numTracks)
    : impl(std::make_unique<Impl>(numTracks))
{
}

SolititoChordModel::~SolititoChordModel() = default;

bool SolititoChordModel::load(const juce::File& modelFile, const juce::File& weightsFile)
{
    return impl->load(modelFile, weightsFile);
}

bool SolititoChordModel::isAvailable() const
{
    return impl->isAvailable();
}

juce::String SolititoChordModel::getStatus() const
{
    return impl->status;
}

int SolititoChordModel::getMemoryKb() const
{
    return impl->getMemoryKb();
}

void SolititoChordModel::resetTrack(int trackIndex)
{
    impl->resetTrack(trackIndex);
}

void SolititoChordModel::appendSamples(int trackIndex,
                                       const float* samples,
                                       int numSamples,
                                       int inputSampleRate,
                                       std::vector<Candidate>& candidates)
{
    impl->appendSamples(trackIndex, samples, numSamples, inputSampleRate, candidates);
}

bool SolititoChordModel::runBatch(const std::vector<Candidate>& candidates, std::vector<Prediction>& predictions)
{
    return impl->runBatch(candidates, predictions);
}