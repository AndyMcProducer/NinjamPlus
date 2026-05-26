#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Chromagram.h"
#include "ChordDetector.h"

#ifdef interface
#undef interface
#endif

#include "jnetlib/httpget.h"
#include <ableton/LinkAudio.hpp>
#include <ableton/link/HostTimeFilter.hpp>
#include <ableton/util/FloatIntConversion.hpp>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>

// ---- Video pipeline debug log ----
static void vlog(const char* msg)
{
    juce::File f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("ninjam_video_debug.txt");
    const juce::String line = juce::Time::getCurrentTime().toString(true, true, true, true)
        + "  " + juce::String::fromUTF8(msg) + "\n";
    f.appendText(line, false, false);
}
static void vlogStr(const juce::String& msg) { vlog(msg.toRawUTF8()); }
// ----------------------------------

class LocalVideoHttpServer final : private juce::Thread
{
public:
    LocalVideoHttpServer(const juce::File& helperRootDir, std::function<juce::String()> intervalPayloadProviderIn)
        : juce::Thread("NINJAMVideoHelperServer"),
          helperRoot(helperRootDir),
          intervalPayloadProvider(std::move(intervalPayloadProviderIn))
    {
        reloadStaticContent();
    }

    ~LocalVideoHttpServer() override
    {
        stop();
    }

    bool start()
    {
        if (isThreadRunning())
            return true;

        reloadStaticContent();
        if (!helperRoot.isDirectory() || helperIndexHtml.isEmpty() || helperAppHtml.isEmpty())
            return false;

        listener = std::make_unique<juce::StreamingSocket>();
        if (!listener->createListener(8100, "127.0.0.1"))
        {
            listener.reset();
            return false;
        }

        startThread();
        return true;
    }

    void stop()
    {
        signalThreadShouldExit();
        if (listener)
            listener->close();
        stopThread(500);
        listener.reset();
    }

private:
    struct HttpResponse
    {
        int statusCode = 200;
        juce::String statusText = "OK";
        juce::String contentType = "text/plain; charset=utf-8";
        juce::MemoryBlock body;
        bool noStore = false;
    };

    juce::File helperRoot;
    std::function<juce::String()> intervalPayloadProvider;
    std::unique_ptr<juce::StreamingSocket> listener;
    juce::String helperIndexHtml;
    juce::String helperAppHtml;
    juce::MemoryBlock helperIconPng;

    void run() override
    {
        while (!threadShouldExit())
        {
            if (!listener)
                break;

            if (listener->waitUntilReady(true, 200) <= 0)
                continue;

            std::unique_ptr<juce::StreamingSocket> client(listener->waitForNextConnection());
            if (client)
                handleClient(*client);
        }
    }

    static bool writeAll(juce::StreamingSocket& socket, const void* data, size_t bytesToWrite)
    {
        const char* bytes = static_cast<const char*>(data);
        size_t remaining = bytesToWrite;
        while (remaining > 0)
        {
            const int chunk = (int) juce::jmin(remaining, (size_t) 32768);
            const int written = socket.write(bytes, chunk);
            if (written <= 0)
                return false;
            bytes += written;
            remaining -= (size_t) written;
        }
        return true;
    }

    static juce::MemoryBlock makeUtf8Body(const juce::String& text)
    {
        const auto* utf8 = text.toRawUTF8();
        return juce::MemoryBlock(utf8, std::strlen(utf8));
    }

    void handleClient(juce::StreamingSocket& client)
    {
        const juce::String request = readRequest(client);
        if (request.isEmpty())
            return;

        const juce::String requestLine = request.upToFirstOccurrenceOf("\r\n", false, false)
                                           .upToFirstOccurrenceOf("\n", false, false)
                                           .trim();
        const juce::String method = requestLine.upToFirstOccurrenceOf(" ", false, false).trim().toUpperCase();
        const juce::String remainder = requestLine.fromFirstOccurrenceOf(" ", false, false).trim();
        const juce::String target = remainder.upToFirstOccurrenceOf(" ", false, false).trim();
        const HttpResponse response = buildResponse(method, target);
        sendResponse(client, method, response);
    }

    juce::String readRequest(juce::StreamingSocket& client) const
    {
        juce::MemoryOutputStream requestData;
        char buffer[2048] = {};
        while (!threadShouldExit() && requestData.getDataSize() < 65536)
        {
            if (client.waitUntilReady(true, 100) <= 0)
                break;

            const int bytesRead = client.read(buffer, (int) std::size(buffer), false);
            if (bytesRead <= 0)
                break;

            requestData.write(buffer, (size_t) bytesRead);
            const juce::String requestText = requestData.toString();
            if (requestText.contains("\r\n\r\n") || requestText.contains("\n\n"))
                return requestText;
        }

        return requestData.toString();
    }

    HttpResponse buildResponse(const juce::String& method, const juce::String& requestTarget)
    {
        const juce::String path = requestTarget.upToFirstOccurrenceOf("?", false, false).trim();
        const bool isHead = (method == "HEAD");

        if (path.isEmpty() || path == "/" || path == "/buffer-room" || path == "/sync-buffer-room" || path == "/index.html")
        {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.body = makeUtf8Body(helperIndexHtml);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/app")
        {
            HttpResponse response;
            response.contentType = "text/html; charset=utf-8";
            response.body = makeUtf8Body(helperAppHtml);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/intervals")
        {
            HttpResponse response;
            response.contentType = "application/json; charset=utf-8";
            response.noStore = true;
            juce::String payload = intervalPayloadProvider ? intervalPayloadProvider().trim() : juce::String();
            if (payload.isEmpty())
                payload = "[]";
            response.body = makeUtf8Body(payload);
            if (isHead)
                response.body.reset();
            return response;
        }

        if (path == "/icon.png" && helperIconPng.getSize() > 0)
        {
            HttpResponse response;
            response.contentType = "image/png";
            response.body = helperIconPng;
            if (isHead)
                response.body.reset();
            return response;
        }

        HttpResponse response;
        response.statusCode = 404;
        response.statusText = "Not Found";
        response.body = makeUtf8Body("Not found");
        if (isHead)
            response.body.reset();
        return response;
    }

    void sendResponse(juce::StreamingSocket& client, const juce::String& method, const HttpResponse& response)
    {
        juce::String header;
        header << "HTTP/1.1 " << response.statusCode << ' ' << response.statusText << "\r\n";
        header << "Content-Type: " << response.contentType << "\r\n";
        header << "Content-Length: " << (juce::int64) response.body.getSize() << "\r\n";
        header << "Connection: close\r\n";
        if (response.noStore)
            header << "Cache-Control: no-store, no-cache, must-revalidate\r\n";
        header << "\r\n";

        const juce::MemoryBlock headerBytes = makeUtf8Body(header);
        if (!writeAll(client, headerBytes.getData(), headerBytes.getSize()))
            return;

        if (method != "HEAD" && response.body.getSize() > 0)
            writeAll(client, response.body.getData(), response.body.getSize());
    }

    void reloadStaticContent()
    {
        helperIndexHtml = helperRoot.getChildFile("index.html").loadFileAsString();
        helperAppHtml = helperRoot.getChildFile("app.html").loadFileAsString();
        if (helperAppHtml.isEmpty())
            helperAppHtml = extractAppHtmlFromServerScript(helperRoot.getChildFile("server.js"));

        helperIconPng.reset();
        helperRoot.getChildFile("icon.png").loadFileAsData(helperIconPng);
    }

    static juce::String extractAppHtmlFromServerScript(const juce::File& scriptFile)
    {
        juce::String scriptText = scriptFile.loadFileAsString().replace("\r\n", "\n");
        if (scriptText.isEmpty())
            return {};

        static const char* startMarker = "const htmlPage = `";
        const int start = scriptText.indexOf(startMarker);
        if (start < 0)
            return {};

        const int contentStart = start + (int) std::strlen(startMarker);
        int end = scriptText.indexOf(contentStart, "`;\n\nconst server = http.createServer");
        if (end < 0)
            end = scriptText.indexOf(contentStart, "`;\nconst server = http.createServer");
        if (end < 0)
            return {};

        return scriptText.substring(contentStart, end);
    }
};

class LocalChordAnalyzer final
    : private juce::Thread
{
public:
    LocalChordAnalyzer()
        : juce::Thread("NINJAMLocalChordAnalyzer")
    {
        memoryKb.store(estimateMemoryKb());
    }

    ~LocalChordAnalyzer() override
    {
        stop();
    }

    void prepare(double newSampleRate)
    {
        ready.store(false, std::memory_order_release);
        stopThread(1000);

        sampleRate = newSampleRate > 1.0 ? newSampleRate : 44100.0;
        frame.assign((size_t)frameSize, 0.0);
        averagedChroma.assign(12, 0.0);
        readBuffer.assign((size_t)frameSize * 4, 0.0f);
        const int newRingSize = juce::jmax(frameSize * 16, (int)std::round(sampleRate));
        ringBuffer.assign((size_t)newRingSize, 0.0f);
        audioFifo = std::make_unique<juce::AbstractFifo>(newRingSize);
        frameFill = 0;
        rmsSmoothed = 0.0;
        samplesSinceCpuUpdate = 0;
        analysisMsSinceCpuUpdate = 0.0;
        chromaHistory = {};
        chromaHistoryWrite = 0;
        chromaHistorySize = 0;
        resetChordDecisionState();
        droppedSamples.store(0, std::memory_order_relaxed);
        cpuPercent.store(0.0, std::memory_order_relaxed);

        const int roundedRate = juce::jlimit(8000, 192000, (int)std::round(sampleRate));
        requestedSampleRate.store(roundedRate, std::memory_order_relaxed);
        configureChromagram(roundedRate);
        memoryKb.store(estimateMemoryKb(newRingSize), std::memory_order_relaxed);

        ready.store(true, std::memory_order_release);
        startThread(juce::Thread::Priority::background);
    }

    void processBlock(const float* input, int numSamples, int inputSampleRate = 0)
    {
        processFrames(input, numSamples, 1, inputSampleRate);
    }

    void processInterleavedBlock(const float* input, int numFrames, int numChannels, int inputSampleRate)
    {
        processFrames(input, numFrames, juce::jmax(1, numChannels), inputSampleRate);
    }

    void processFrames(const float* input, int numFrames, int numChannels, int inputSampleRate)
    {
        if (!ready.load(std::memory_order_acquire))
            return;

        if (input == nullptr || numFrames <= 0 || audioFifo == nullptr || ringBuffer.empty())
        {
            markNoInput();
            return;
        }

        if (inputSampleRate > 1000)
            requestedSampleRate.store(juce::jlimit(8000, 192000, inputSampleRate), std::memory_order_relaxed);

        int writableSamples = juce::jmin(numFrames, audioFifo->getFreeSpace());
        if (writableSamples <= 0)
        {
            droppedSamples.fetch_add(numFrames, std::memory_order_relaxed);
            return;
        }

        int inputStartFrame = 0;
        if (writableSamples < numFrames)
        {
            inputStartFrame = numFrames - writableSamples;
            droppedSamples.fetch_add(numFrames - writableSamples, std::memory_order_relaxed);
        }

        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        audioFifo->prepareToWrite(writableSamples, start1, size1, start2, size2);

        copyMonoFramesToRing(input, inputStartFrame, numChannels, start1, size1);
        copyMonoFramesToRing(input, inputStartFrame + size1, numChannels, start2, size2);

        audioFifo->finishedWrite(size1 + size2);
        samplesAvailable.signal();
    }

    void markNoInput()
    {
        chordValid.store(false, std::memory_order_relaxed);
        noteValid.store(false, std::memory_order_relaxed);
    }

    void stop()
    {
        ready.store(false, std::memory_order_release);
        signalThreadShouldExit();
        samplesAvailable.signal();
        stopThread(1000);
    }

    bool isPrepared() const
    {
        return ready.load(std::memory_order_acquire);
    }

    static const char* getPitchClassName(int pitchClass)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return pitchClass >= 0 && pitchClass < 12 ? names[pitchClass] : "--";
    }

    static juce::String formatChordLabel(int root, int quality, int intervals)
    {
        juce::String suffix;

        switch (quality)
        {
            case ChordDetector::Major:       suffix = intervals == 7 ? "maj7" : ""; break;
            case ChordDetector::Minor:       suffix = intervals == 7 ? "m7" : "m"; break;
            case ChordDetector::Suspended:   suffix = intervals == 2 ? "sus2" : (intervals == 4 ? "sus4" : "sus"); break;
            case ChordDetector::Dominant:    suffix = "7"; break;
            case ChordDetector::Dimished5th: suffix = "dim"; break;
            case ChordDetector::Augmented5th:suffix = "aug"; break;
            default:                         suffix = ""; break;
        }

        return juce::String(getPitchClassName(root)) + suffix;
    }

    juce::String getLabel() const
    {
        if (chordValid.load(std::memory_order_relaxed))
        {
            const int root = chordRoot.load(std::memory_order_relaxed);
            const int quality = chordQuality.load(std::memory_order_relaxed);
            const int intervals = chordIntervals.load(std::memory_order_relaxed);
            if (root >= 0 && root < 12)
                return formatChordLabel(root, quality, intervals);
        }

        if (noteValid.load(std::memory_order_relaxed))
        {
            const int root = noteRoot.load(std::memory_order_relaxed);
            if (root >= 0 && root < 12)
                return getPitchClassName(root);
        }

        return "--";
    }

    double getCpuPercent() const
    {
        return cpuPercent.load(std::memory_order_relaxed);
    }

    int getMemoryKb() const
    {
        return memoryKb.load(std::memory_order_relaxed);
    }

private:
    static int estimateMemoryKb(int fifoSamples = 48000)
    {
        constexpr int bufferSize = 8192;
        constexpr int downsampledFrameSize = 512 / 4;
        const size_t doubleVectors = (size_t)(bufferSize + bufferSize + (bufferSize / 2 + 1)
                                      + 12 + downsampledFrameSize + 512) * sizeof(double);
        const size_t kissFftBuffers = (size_t)bufferSize * 2 * sizeof(float) * 2;
        const size_t fifoBytes = (size_t)juce::jmax(0, fifoSamples) * sizeof(float);
        const size_t bytes = doubleVectors + kissFftBuffers + fifoBytes + sizeof(ChordDetector) + (64 * 1024);
        return (int)((bytes + 1023) / 1024);
    }

    void copyMonoFramesToRing(const float* input, int inputStartFrame, int numChannels, int ringStart, int count)
    {
        if (count <= 0)
            return;

        if (numChannels <= 1)
        {
            std::memcpy(ringBuffer.data() + ringStart,
                        input + inputStartFrame,
                        (size_t)count * sizeof(float));
            return;
        }

        for (int i = 0; i < count; ++i)
        {
            const int source = (inputStartFrame + i) * numChannels;
            ringBuffer[(size_t)(ringStart + i)] = 0.5f * (input[source] + input[source + 1]);
        }
    }

    void configureChromagram(int newSampleRate)
    {
        analyzerSampleRate = juce::jlimit(8000, 192000, newSampleRate);
        sampleRate = (double)analyzerSampleRate;
        chromagram = std::make_unique<Chromagram>(frameSize, analyzerSampleRate);
        chromagram->setChromaCalculationInterval(chromaCalculationIntervalSamples);
        frameFill = 0;
        rmsSmoothed = 0.0;
        chromaHistory = {};
        chromaHistoryWrite = 0;
        chromaHistorySize = 0;
        resetChordDecisionState();
    }

    void run() override
    {
        while (!threadShouldExit())
        {
            if (!readAvailableSamples())
                samplesAvailable.wait(20);
        }
    }

    bool readAvailableSamples()
    {
        if (!ready.load(std::memory_order_acquire))
            return false;

        if (audioFifo == nullptr || ringBuffer.empty() || readBuffer.empty())
            return false;

        const int requestedRate = requestedSampleRate.load(std::memory_order_relaxed);
        if (chromagram == nullptr || requestedRate != analyzerSampleRate)
            configureChromagram(requestedRate);

        const int available = audioFifo->getNumReady();
        if (available <= 0)
            return false;

        const int toRead = juce::jmin(available, (int)readBuffer.size());
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        audioFifo->prepareToRead(toRead, start1, size1, start2, size2);

        if (size1 > 0)
            std::memcpy(readBuffer.data(), ringBuffer.data() + start1, (size_t)size1 * sizeof(float));
        if (size2 > 0)
            std::memcpy(readBuffer.data() + size1, ringBuffer.data() + start2, (size_t)size2 * sizeof(float));

        audioFifo->finishedRead(size1 + size2);

        const double startMs = juce::Time::getMillisecondCounterHiRes();
        const int samplesRead = size1 + size2;
        for (int i = 0; i < samplesRead; ++i)
        {
            frame[(size_t)frameFill++] = (double)readBuffer[(size_t)i];
            if (frameFill >= frameSize)
            {
                processFrame();
                frameFill = 0;
            }
        }

        const double elapsedMs = juce::Time::getMillisecondCounterHiRes() - startMs;
        analysisMsSinceCpuUpdate += juce::jmax(0.0, elapsedMs);
        samplesSinceCpuUpdate += samplesRead;

        const int updateSamples = juce::jmax(1, (int)std::round(sampleRate));
        if (samplesSinceCpuUpdate >= updateSamples)
        {
            const double realTimeMs = ((double)samplesSinceCpuUpdate / sampleRate) * 1000.0;
            const double rawCpu = realTimeMs > 0.0 ? (analysisMsSinceCpuUpdate / realTimeMs) * 100.0 : 0.0;
            const double previous = cpuPercent.load(std::memory_order_relaxed);
            const double smoothed = previous <= 0.0 ? rawCpu : previous * 0.75 + rawCpu * 0.25;
            cpuPercent.store(juce::jlimit(0.0, 200.0, smoothed), std::memory_order_relaxed);
            analysisMsSinceCpuUpdate = 0.0;
            samplesSinceCpuUpdate = 0;
        }

        return true;
    }

    void processFrame()
    {
        double energy = 0.0;
        for (double sample : frame)
            energy += sample * sample;

        const double frameRms = std::sqrt(energy / (double)frame.size());
        rmsSmoothed = frameRms > rmsSmoothed ? frameRms : rmsSmoothed * 0.92 + frameRms * 0.08;

        chromagram->processAudioFrame(frame.data());
        if (!chromagram->isReady())
            return;

        if (rmsSmoothed < silenceRmsThreshold)
        {
            noteInvalidChordFrame();
            return;
        }

        auto chroma = chromagram->getChromagram();
        double chromaTotal = 0.0;
        for (double value : chroma)
            chromaTotal += std::abs(value);

        if (chromaTotal <= 1.0e-9)
        {
            noteInvalidChordFrame();
            return;
        }

        chromaHistory[(size_t)chromaHistoryWrite].fill(0.0);
        for (int i = 0; i < 12 && i < (int)chroma.size(); ++i)
            chromaHistory[(size_t)chromaHistoryWrite][(size_t)i] = chroma[(size_t)i];
        chromaHistoryWrite = (chromaHistoryWrite + 1) % chromaHistoryFrames;
        chromaHistorySize = juce::jmin(chromaHistorySize + 1, chromaHistoryFrames);

        averagedChroma.assign(12, 0.0);
        for (int h = 0; h < chromaHistorySize; ++h)
            for (int i = 0; i < 12; ++i)
                averagedChroma[(size_t)i] += chromaHistory[(size_t)h][(size_t)i];

        const double scale = 1.0 / (double)juce::jmax(1, chromaHistorySize);
        for (double& value : averagedChroma)
            value *= scale;

        int dominantPitchClass = -1;
        if (hasEnoughHarmonicContent(averagedChroma))
        {
            chordDetector.detectChord(averagedChroma);
            if (chordDetector.rootNote < 0 || chordDetector.rootNote >= 12)
            {
                noteInvalidChordFrame();
                return;
            }

            publishChordCandidate(chordDetector.rootNote, chordDetector.quality, chordDetector.intervals);
            return;
        }

        if (!hasDominantPitchClass(averagedChroma, dominantPitchClass))
        {
            noteInvalidChordFrame();
            return;
        }

        publishNoteCandidate(dominantPitchClass);
    }

    static int makeChordKey(int root, int quality, int intervals)
    {
        return root * 100 + quality * 10 + intervals;
    }

    void resetChordDecisionState()
    {
        pendingChordKey = -1;
        pendingChordHits = 0;
        pendingNoteRoot = -1;
        pendingNoteHits = 0;
        displayedChordKey = -1;
        invalidChordFrames = 0;
        chordRoot.store(-1, std::memory_order_relaxed);
        chordQuality.store(ChordDetector::Major, std::memory_order_relaxed);
        chordIntervals.store(0, std::memory_order_relaxed);
        chordValid.store(false, std::memory_order_relaxed);
        noteRoot.store(-1, std::memory_order_relaxed);
        noteValid.store(false, std::memory_order_relaxed);
    }

    void noteInvalidChordFrame()
    {
        pendingChordKey = -1;
        pendingChordHits = 0;
        pendingNoteRoot = -1;
        pendingNoteHits = 0;

        if (++invalidChordFrames >= invalidClearThresholdFrames)
            resetChordDecisionState();
    }

    static bool hasEnoughHarmonicContent(const std::vector<double>& chroma)
    {
        double sum = 0.0;
        double max1 = 0.0;
        double max2 = 0.0;
        double max3 = 0.0;

        for (double value : chroma)
        {
            const double v = std::abs(value);
            sum += v;
            if (v > max1)
            {
                max3 = max2;
                max2 = max1;
                max1 = v;
            }
            else if (v > max2)
            {
                max3 = max2;
                max2 = v;
            }
            else if (v > max3)
            {
                max3 = v;
            }
        }

        if (sum <= 1.0e-9 || max1 <= 1.0e-9)
            return false;

        const double mean = sum / 12.0;
        if (max1 < mean * 1.7)
            return false;

        if (max2 < max1 * 0.24)
            return false;

        const double top3Share = (max1 + max2 + max3) / sum;
        if (top3Share < 0.40)
            return false;

        const double dominantShare = max1 / sum;

        int strongPitchClasses = 0;
        for (double value : chroma)
            if (std::abs(value) >= max1 * 0.25)
                ++strongPitchClasses;

        if (dominantShare > 0.34 && strongPitchClasses <= 2)
            return false;

        return strongPitchClasses >= 2;
    }

    static bool hasDominantPitchClass(const std::vector<double>& chroma, int& dominantPitchClass)
    {
        dominantPitchClass = -1;

        double sum = 0.0;
        double max1 = 0.0;
        int strongPitchClasses = 0;

        for (int i = 0; i < 12 && i < (int)chroma.size(); ++i)
        {
            const double v = std::abs(chroma[(size_t)i]);
            sum += v;

            if (v > max1)
            {
                max1 = v;
                dominantPitchClass = i;
            }
        }

        if (sum <= 1.0e-9 || max1 <= 1.0e-9 || dominantPitchClass < 0)
            return false;

        const double mean = sum / 12.0;
        if (max1 < mean * 2.0)
            return false;

        if ((max1 / sum) < 0.22)
            return false;

        for (double value : chroma)
            if (std::abs(value) >= max1 * 0.40)
                ++strongPitchClasses;

        return strongPitchClasses <= 4;
    }

    void publishNoteCandidate(int root)
    {
        invalidChordFrames = 0;
        pendingChordKey = -1;
        pendingChordHits = 0;

        if (root == pendingNoteRoot)
            pendingNoteHits = juce::jmin(pendingNoteHits + 1, 1000);
        else
        {
            pendingNoteRoot = root;
            pendingNoteHits = 1;
        }

        const int displayedNote = noteRoot.load(std::memory_order_relaxed);
        if (noteValid.load(std::memory_order_relaxed) && root == displayedNote)
            return;

        const int requiredHits = displayedNote >= 0 ? stableNoteChangeHitThreshold : stableNoteHitThreshold;
        if (pendingNoteHits < requiredHits)
            return;

        noteRoot.store(root, std::memory_order_relaxed);
        noteValid.store(true, std::memory_order_relaxed);
        chordValid.store(false, std::memory_order_relaxed);
    }

    void publishChordCandidate(int root, int quality, int intervals)
    {
        const int candidateKey = makeChordKey(root, quality, intervals);
        invalidChordFrames = 0;
        pendingNoteRoot = -1;
        pendingNoteHits = 0;

        if (candidateKey == pendingChordKey)
            pendingChordHits = juce::jmin(pendingChordHits + 1, 1000);
        else
        {
            pendingChordKey = candidateKey;
            pendingChordHits = 1;
        }

        if (candidateKey == displayedChordKey)
        {
            noteRoot.store(-1, std::memory_order_relaxed);
            noteValid.store(false, std::memory_order_relaxed);
            chordValid.store(true, std::memory_order_relaxed);
            return;
        }

        const int requiredHits = displayedChordKey >= 0 ? stableChordChangeHitThreshold : stableCandidateHitThreshold;
        if (pendingChordHits < requiredHits)
            return;

        displayedChordKey = candidateKey;
        chordRoot.store(root, std::memory_order_relaxed);
        chordQuality.store(quality, std::memory_order_relaxed);
        chordIntervals.store(intervals, std::memory_order_relaxed);
        noteRoot.store(-1, std::memory_order_relaxed);
        noteValid.store(false, std::memory_order_relaxed);
        chordValid.store(true, std::memory_order_relaxed);
    }

    static constexpr int frameSize = 512;
    static constexpr int chromaCalculationIntervalSamples = 2048;
    static constexpr int chromaHistoryFrames = 8;
    static constexpr int stableCandidateHitThreshold = 3;
    static constexpr int stableChordChangeHitThreshold = 6;
    static constexpr int stableNoteHitThreshold = 1;
    static constexpr int stableNoteChangeHitThreshold = 4;
    static constexpr int invalidClearThresholdFrames = 8;
    static constexpr double silenceRmsThreshold = 0.001;

    std::unique_ptr<Chromagram> chromagram;
    std::unique_ptr<juce::AbstractFifo> audioFifo;
    ChordDetector chordDetector;
    std::vector<double> frame;
    std::vector<double> averagedChroma;
    std::array<std::array<double, 12>, chromaHistoryFrames> chromaHistory {};
    std::vector<float> ringBuffer;
    std::vector<float> readBuffer;
    juce::WaitableEvent samplesAvailable;
    double sampleRate = 44100.0;
    double rmsSmoothed = 0.0;
    double analysisMsSinceCpuUpdate = 0.0;
    int samplesSinceCpuUpdate = 0;
    int analyzerSampleRate = 0;
    int frameFill = 0;
    int chromaHistoryWrite = 0;
    int chromaHistorySize = 0;
    int pendingChordKey = -1;
    int pendingChordHits = 0;
    int pendingNoteRoot = -1;
    int pendingNoteHits = 0;
    int displayedChordKey = -1;
    int invalidChordFrames = 0;
    std::atomic<bool> ready { false };
    std::atomic<bool> chordValid { false };
    std::atomic<bool> noteValid { false };
    std::atomic<int> chordRoot { -1 };
    std::atomic<int> noteRoot { -1 };
    std::atomic<int> chordQuality { ChordDetector::Major };
    std::atomic<int> chordIntervals { 0 };
    std::atomic<double> cpuPercent { 0.0 };
    std::atomic<int> memoryKb { 0 };
    std::atomic<int> requestedSampleRate { 44100 };
    std::atomic<long long> droppedSamples { 0 };
};

class AsyncChatTranslationWorker final : private juce::Thread
{
public:
    struct Request
    {
        juce::String originalLine;
        juce::String lineSender;
        juce::String linePrefix;
        juce::String lineBody;
        juce::String targetCode;
        juce::uint64 configRevision = 0;
    };

    explicit AsyncChatTranslationWorker(NinjamVst3AudioProcessor& ownerProcessor)
        : juce::Thread("NINJAMChatTranslation"), owner(ownerProcessor)
    {
        startThread();
    }

    ~AsyncChatTranslationWorker() override
    {
        stop();
    }

    void enqueue(Request request)
    {
        {
            const juce::ScopedLock lock(queueLock);
            queue.push_back(std::move(request));
        }
        workAvailable.signal();
    }

    void stop(int timeoutMs = 1500)
    {
        signalThreadShouldExit();
        workAvailable.signal();
        stopThread(juce::jlimit(100, 6000, timeoutMs));

        const juce::ScopedLock lock(queueLock);
        queue.clear();
    }

private:
    void run() override
    {
        while (!threadShouldExit())
        {
            Request request;
            bool haveRequest = false;

            {
                const juce::ScopedLock lock(queueLock);
                if (!queue.empty())
                {
                    request = std::move(queue.front());
                    queue.pop_front();
                    haveRequest = true;
                }
            }

            if (!haveRequest)
            {
                workAvailable.wait(200);
                continue;
            }

            const juce::String translatedBody = owner.translateTextForTarget(request.lineBody, request.targetCode);
            if (threadShouldExit())
                break;

            owner.applyAsyncTranslatedChatLine(request.originalLine,
                                               request.lineSender,
                                               request.linePrefix + translatedBody,
                                               request.configRevision);
        }
    }

    NinjamVst3AudioProcessor& owner;
    juce::CriticalSection queueLock;
    std::deque<Request> queue;
    juce::WaitableEvent workAvailable;
};

static juce::String getSystemTranslationLanguageCode()
{
    juce::String language = juce::SystemStats::getDisplayLanguage();
    if (language.isEmpty())
        language = juce::SystemStats::getUserLanguage();

    language = language.trim().replaceCharacter('_', '-').toLowerCase();

    if (language.startsWith("zh-hant") || language.startsWith("zh-tw") || language.startsWith("zh-hk"))
        return "zh-Hant";

    if (language.startsWith("zh"))
        return "zh-Hans";

    if (language.startsWith("pt-br"))
        return "pt-BR";

    if (language.startsWith("no") || language.startsWith("nb"))
        return "nb";

    if (language.startsWith("iw"))
        return "he";

    if (language.startsWith("in"))
        return "id";

    const int dash = language.indexOfChar('-');
    if (dash > 0)
        language = language.substring(0, dash);

    if (language.isEmpty())
        language = "en";

    return language;
}

static juce::String resolveTranslateTargetLanguageCode(const juce::String& preferredCode)
{
    juce::String normalised = preferredCode.trim().replaceCharacter('_', '-').toLowerCase();
    if (normalised.isEmpty() || normalised == "system")
        return getSystemTranslationLanguageCode();

    if (normalised == "zh-cn" || normalised == "zh-hans")
        return "zh-Hans";

    if (normalised == "zh-tw" || normalised == "zh-hk" || normalised == "zh-hant")
        return "zh-Hant";

    if (normalised == "pt-br")
        return "pt-BR";

    if (normalised == "no" || normalised == "nb")
        return "nb";

    const int dash = normalised.indexOfChar('-');
    if (dash > 0)
        normalised = normalised.substring(0, dash);

    return normalised.isNotEmpty() ? normalised : "en";
}

static bool detectedLanguageMatchesTarget(const juce::var& detected, const juce::String& targetCode)
{
    auto sameAsTarget = [&targetCode](const juce::String& languageCode)
    {
        return resolveTranslateTargetLanguageCode(languageCode) == targetCode;
    };

    if (auto* detectedObject = detected.getDynamicObject())
        return sameAsTarget(detectedObject->getProperty("language").toString());

    if (auto* detectedArray = detected.getArray(); detectedArray != nullptr && !detectedArray->isEmpty())
    {
        if (auto* firstObject = detectedArray->getReference(0).getDynamicObject())
            return sameAsTarget(firstObject->getProperty("language").toString());
    }

    return false;
}

static bool tryTranslateWithFedilab(const juce::String& text,
                                    const juce::String& targetCode,
                                    juce::String& translatedText,
                                    juce::String& error)
{
    juce::URL requestUrl("https://translate.fedilab.app/translate");
    requestUrl = requestUrl.withParameter("q", text)
                           .withParameter("source", "auto")
                           .withParameter("target", targetCode)
                           .withParameter("format", "text");

    int httpStatusCode = 0;
    auto responseStream = requestUrl.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
            .withConnectionTimeoutMs(2000)
            .withNumRedirectsToFollow(2)
            .withStatusCode(&httpStatusCode)
            .withExtraHeaders("User-Agent: NINJAMVST3/1.0\r\nAccept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\n")
            .withHttpRequestCmd("POST"));

    if (responseStream == nullptr)
    {
        error = "primary translator could not be reached";
        return false;
    }

    if (httpStatusCode != 0 && httpStatusCode != 200)
    {
        error = "primary translator returned HTTP " + juce::String(httpStatusCode);
        return false;
    }

    const juce::String responseText = responseStream->readEntireStreamAsString();
    if (responseText.isEmpty())
    {
        error = "primary translator returned an empty response";
        return false;
    }

    const juce::var parsed = juce::JSON::parse(responseText);
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
    {
        error = "primary translator returned invalid JSON";
        return false;
    }

    if (root->hasProperty("error"))
    {
        error = root->getProperty("error").toString().isNotEmpty()
                    ? root->getProperty("error").toString()
                    : juce::String("primary translator reported an error");
        return false;
    }

    if (auto detected = root->getProperty("detectedLanguage"); !detected.isVoid() && detectedLanguageMatchesTarget(detected, targetCode))
    {
        translatedText = text;
        return true;
    }

    translatedText = root->getProperty("translatedText").toString();
    if (translatedText.isEmpty())
    {
        error = "primary translator did not return translated text";
        return false;
    }

    return true;
}

static bool tryTranslateWithGoogleFallback(const juce::String& text,
                                           const juce::String& targetCode,
                                           juce::String& translatedText,
                                           juce::String& error)
{
    juce::URL requestUrl("https://translate.googleapis.com/translate_a/single");
    requestUrl = requestUrl.withParameter("client", "gtx")
                           .withParameter("sl", "auto")
                           .withParameter("tl", targetCode)
                           .withParameter("dt", "t")
                           .withParameter("q", text);

    int httpStatusCode = 0;
    auto responseStream = requestUrl.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(2000)
            .withNumRedirectsToFollow(2)
            .withStatusCode(&httpStatusCode)
            .withExtraHeaders("User-Agent: NINJAMVST3/1.0\r\nAccept: application/json\r\n")
            .withHttpRequestCmd("GET"));

    if (responseStream == nullptr)
    {
        error = "fallback translator could not be reached";
        return false;
    }

    if (httpStatusCode != 0 && httpStatusCode != 200)
    {
        error = "fallback translator returned HTTP " + juce::String(httpStatusCode);
        return false;
    }

    const juce::String responseText = responseStream->readEntireStreamAsString();
    if (responseText.isEmpty())
    {
        error = "fallback translator returned an empty response";
        return false;
    }

    const juce::var parsed = juce::JSON::parse(responseText);
    auto* rootArray = parsed.getArray();
    if (rootArray == nullptr || rootArray->isEmpty())
    {
        error = "fallback translator returned invalid JSON";
        return false;
    }

    if (rootArray->size() > 2 && resolveTranslateTargetLanguageCode(rootArray->getReference(2).toString()) == targetCode)
    {
        translatedText = text;
        return true;
    }

    auto* segments = rootArray->getReference(0).getArray();
    if (segments == nullptr || segments->isEmpty())
    {
        error = "fallback translator did not return translated segments";
        return false;
    }

    juce::String combined;
    for (const auto& segmentVar : *segments)
    {
        if (auto* segment = segmentVar.getArray(); segment != nullptr && !segment->isEmpty())
            combined << segment->getReference(0).toString();
    }

    if (combined.isEmpty())
    {
        error = "fallback translator returned empty translated text";
        return false;
    }

    translatedText = combined;
    return true;
}

static bool copyPlayHeadPositionToCurrentInfo(juce::AudioPlayHead& playHead,
                                              juce::AudioPlayHead::CurrentPositionInfo& info)
{
    const auto position = playHead.getPosition();
    if (!position)
        return false;

    info.resetToDefault();

    if (const auto sig = position->getTimeSignature())
    {
        info.timeSigNumerator = sig->numerator;
        info.timeSigDenominator = sig->denominator;
    }

    if (const auto loop = position->getLoopPoints())
    {
        info.ppqLoopStart = loop->ppqStart;
        info.ppqLoopEnd = loop->ppqEnd;
    }

    if (const auto frame = position->getFrameRate())
        info.frameRate = *frame;
    if (const auto timeInSeconds = position->getTimeInSeconds())
        info.timeInSeconds = *timeInSeconds;
    if (const auto lastBarStartPpq = position->getPpqPositionOfLastBarStart())
        info.ppqPositionOfLastBarStart = *lastBarStartPpq;
    if (const auto ppqPosition = position->getPpqPosition())
        info.ppqPosition = *ppqPosition;
    if (const auto originTime = position->getEditOriginTime())
        info.editOriginTime = *originTime;
    if (const auto bpm = position->getBpm())
        info.bpm = *bpm;
    if (const auto timeInSamples = position->getTimeInSamples())
        info.timeInSamples = *timeInSamples;

    info.isPlaying = position->getIsPlaying();
    info.isRecording = position->getIsRecording();
    info.isLooping = position->getIsLooping();

    return true;
}

static int computeJamTabaHostSyncStartPositionSamples(const juce::AudioPlayHead::CurrentPositionInfo& hostInfo,
                                                      double sampleRate)
{
    if (sampleRate <= 1.0
        || !std::isfinite(hostInfo.bpm)
        || hostInfo.bpm <= 0.0
        || !std::isfinite(hostInfo.ppqPosition))
    {
        return 0;
    }

    const double samplesPerBeat = (60.0 * sampleRate) / hostInfo.bpm;
    if (!std::isfinite(samplesPerBeat) || samplesPerBeat <= 0.0)
        return 0;

    if (hostInfo.ppqPosition > 0.0)
    {
        const int denominator = hostInfo.timeSigDenominator > 0 ? hostInfo.timeSigDenominator : 4;
        const int numerator = hostInfo.timeSigNumerator > 0 ? hostInfo.timeSigNumerator : 4;
        const double barLengthInQuarterNotes = (4.0 * (double) numerator) / (double) denominator;
        if (barLengthInQuarterNotes > 0.0)
        {
            double cursorPosInMeasure = hostInfo.ppqPosition - hostInfo.ppqPositionOfLastBarStart;
            if (!std::isfinite(hostInfo.ppqPositionOfLastBarStart)
                || cursorPosInMeasure < -1.0e-8
                || cursorPosInMeasure > barLengthInQuarterNotes + 1.0e-8)
            {
                cursorPosInMeasure = std::fmod(hostInfo.ppqPosition, barLengthInQuarterNotes);
                if (cursorPosInMeasure < 0.0)
                    cursorPosInMeasure += barLengthInQuarterNotes;
            }

            constexpr double hostSyncStartGraceMs = 80.0;
            const double startGraceBeats = (hostSyncStartGraceMs / 1000.0) * hostInfo.bpm / 60.0;
            if (cursorPosInMeasure <= juce::jmax(1.0e-8, startGraceBeats))
                return 0;

            if (cursorPosInMeasure > 1.0e-8)
            {
                const double samplesUntilNextMeasure = (barLengthInQuarterNotes - cursorPosInMeasure) * samplesPerBeat;
                return -(int) std::llround(samplesUntilNextMeasure);
            }
        }

        return 0;
    }

    return (int) std::llround(hostInfo.ppqPosition * samplesPerBeat);
}

static int computeLinkSyncStartPositionSamples(double phaseBeats,
                                               double quantum,
                                               double tempoBpm,
                                               double sampleRate)
{
    if (sampleRate <= 1.0
        || !std::isfinite(phaseBeats)
        || !std::isfinite(quantum)
        || quantum <= 0.0
        || !std::isfinite(tempoBpm)
        || tempoBpm <= 0.0)
    {
        return 0;
    }

    double wrappedPhase = std::fmod(phaseBeats, quantum);
    if (wrappedPhase < 0.0)
        wrappedPhase += quantum;

    constexpr double phaseEpsilon = 1.0e-6;
    if (wrappedPhase <= phaseEpsilon || (quantum - wrappedPhase) <= phaseEpsilon)
        return 0;

    const double samplesPerBeat = (60.0 * sampleRate) / tempoBpm;
    if (!std::isfinite(samplesPerBeat) || samplesPerBeat <= 0.0)
        return 0;

    const double samplesUntilNextQuantum = (quantum - wrappedPhase) * samplesPerBeat;
    return -(int) std::llround(samplesUntilNextQuantum);
}

static int normaliseSignedIntervalPosition(int positionSamples, int intervalLength)
{
    if (intervalLength <= 0)
        return 0;

    int normalised = positionSamples % intervalLength;
    if (normalised < 0)
        normalised += intervalLength;
    return normalised;
}

static std::chrono::microseconds getNextLinkQuantumTime(
    const ableton::LinkAudio::SessionState& sessionState,
    std::chrono::microseconds fromTime,
    double quantum,
    double tempoBpm)
{
    if (!std::isfinite(quantum) || quantum <= 0.0
        || !std::isfinite(tempoBpm) || tempoBpm <= 0.0)
    {
        return fromTime;
    }

    double phase = std::fmod(sessionState.phaseAtTime(fromTime, quantum), quantum);
    if (phase < 0.0)
        phase += quantum;

    constexpr double phaseEpsilon = 1.0e-5;
    if (phase <= phaseEpsilon || (quantum - phase) <= phaseEpsilon)
        return fromTime;

    const double microsPerBeat = 60000000.0 / tempoBpm;
    const double microsUntilQuantum = (quantum - phase) * microsPerBeat;
    if (!std::isfinite(microsUntilQuantum) || microsUntilQuantum <= 0.0)
        return fromTime;

    return fromTime + std::chrono::microseconds((long long)std::llround(microsUntilQuantum));
}

static bool openUrlExternal(const juce::String& urlText)
{
#ifdef _WIN32
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", urlText.toWideCharPointer(), nullptr, nullptr, SW_SHOWNORMAL);
    if ((INT_PTR)result > 32)
        return true;
#endif
    return juce::URL(urlText).launchInDefaultBrowser();
}

static bool openUrlExternalOnMessageThread(const juce::String& urlText)
{
#if JUCE_WINDOWS
    return openUrlExternal(urlText);
#else
    if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
    {
        if (mm->isThisTheMessageThread())
            return openUrlExternal(urlText);

        struct UrlPayload
        {
            juce::String url;
            bool opened = false;
        } payload { urlText, false };

        mm->callFunctionOnMessageThread(
            [](void* userData) -> void*
            {
                auto* p = static_cast<UrlPayload*>(userData);
                p->opened = openUrlExternal(p->url);
                return nullptr;
            },
            &payload);

        return payload.opened;
    }

    return openUrlExternal(urlText);
#endif
}

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <dlfcn.h>
#endif


namespace
{
    constexpr unsigned int makeNjFourcc(const char a, const char b, const char c, const char d)
    {
        return ((unsigned int)(unsigned char)a) |
               ((unsigned int)(unsigned char)b << 8) |
               ((unsigned int)(unsigned char)c << 16) |
               ((unsigned int)(unsigned char)d << 24);
    }
    constexpr const char* opusSyncAppFamily = "ninjam-vst3";
    constexpr int opusSyncHandshakeVersion = 1;
    constexpr const char* opusSyncChatPrefix = "__NINJAM_VST3_OPUSSYNC__ ";
    // Custom FOURCC for opusSyncSupport broadcast via NINJAM interval channel
    // Any server routes it transparently; other clients ignore unknown FOURCCs
    constexpr unsigned int kOpusSyncFourcc    = makeNjFourcc('N','J','S','3');
    // Custom FOURCC for interval sync signals (intervalSyncTag, transportProbe, latencyReport)
    constexpr unsigned int kSyncSignalFourcc  = makeNjFourcc('N','J','S','4');
    constexpr const char* sideSignalChatPrefix = "__NINJAM_VST3_SIDESIGNAL__ ";
    constexpr int remoteLatencyUpdateCadenceIntervals = 1;
    constexpr int kSyncSignalChannelIndex = 0;
    constexpr double intervalHelperPayloadMinWriteMs = 100.0;
    constexpr long long intervalSyncMarkerKeyBeatStride = 1024;
    constexpr int kLocalInputLinkAudioSentinel = -2000000000;
    constexpr double linkAudioQuantumBeats = 4.0;

    juce::String makeShortUserName(juce::String fullName)
    {
        fullName = fullName.trim();
        const int atPos = fullName.indexOfChar('@');
        if (atPos > 0)
            fullName = fullName.substring(0, atPos);
        return fullName;
    }

    int getIntervalBeatIndexForPosition(int pos, int length, int bpi)
    {
        const int safeBpi = juce::jmax(1, bpi);
        if (length <= 0)
            return 0;

        const int safePos = juce::jlimit(0, length - 1, pos);
        const double progress = (double)safePos / (double)length;
        return juce::jlimit(0, safeBpi - 1, (int)std::floor(progress * (double)safeBpi));
    }

    int getIntervalSyncMarkerBeatForBeat(int, int)
    {
        return 0;
    }

    bool isIntervalSyncMarkerBeat(int beatIndex, int bpi)
    {
        const int safeBpi = juce::jmax(1, bpi);
        if (beatIndex < 0 || beatIndex >= safeBpi)
            return false;

        return beatIndex == 0;
    }

    long long makeIntervalSyncMarkerKey(int interval, int markerBeat)
    {
        return ((long long)juce::jmax(0, interval) * intervalSyncMarkerKeyBeatStride)
             + (long long)juce::jlimit(0, (int)intervalSyncMarkerKeyBeatStride - 1, markerBeat);
    }

    juce::String formatIntervalSyncMarkerBeat(int markerBeat)
    {
        return "BPI" + juce::String(juce::jmax(0, markerBeat) + 1);
    }

    juce::String normaliseOpusPeerId(juce::String userId)
    {
        userId = userId.trim();
        const int atPos = userId.indexOfChar('@');
        if (atPos > 0)
            userId = userId.substring(0, atPos);
        if (userId.startsWithIgnoreCase("anonymous:"))
            userId = userId.substring(10);
        if (userId.startsWithIgnoreCase("guest:"))
            userId = userId.substring(6);
        return userId.toLowerCase();
    }

    juce::String normaliseChatTargetNick(juce::String userId)
    {
        userId = userId.trim();
        const int atPos = userId.indexOfChar('@');
        if (atPos > 0)
            userId = userId.substring(0, atPos);
        const int colonPos = userId.lastIndexOfChar(':');
        if (colonPos >= 0 && colonPos < userId.length() - 1)
            userId = userId.substring(colonPos + 1);
        return userId.trim();
    }

    bool tryParseServerEndpoint(juce::String serverText, juce::String& hostOut, int& portOut)
    {
        serverText = serverText.trim();
        if (serverText.isEmpty())
            return false;

        const int schemePos = serverText.indexOf("://");
        if (schemePos >= 0)
            serverText = serverText.substring(schemePos + 3);

        const int slashPos = serverText.indexOfChar('/');
        if (slashPos >= 0)
            serverText = serverText.substring(0, slashPos);

        const int atPos = serverText.lastIndexOfChar('@');
        if (atPos >= 0 && atPos + 1 < serverText.length())
            serverText = serverText.substring(atPos + 1);

        serverText = serverText.trim();
        if (serverText.isEmpty())
            return false;

        hostOut = serverText;
        portOut = 2049;

        if (serverText.startsWithChar('['))
        {
            const int closingBracket = serverText.indexOfChar(']');
            if (closingBracket <= 1)
                return false;

            hostOut = serverText.substring(1, closingBracket).trim();
            if (closingBracket + 1 < serverText.length() && serverText[closingBracket + 1] == ':')
            {
                const juce::String portText = serverText.substring(closingBracket + 2).trim();
                if (portText.isNotEmpty())
                    portOut = juce::jlimit(1, 65535, portText.getIntValue());
            }

            return hostOut.isNotEmpty();
        }

        const int lastColonPos = serverText.lastIndexOfChar(':');
        if (lastColonPos > 0 && serverText.indexOfChar(':') == lastColonPos)
        {
            const juce::String candidatePort = serverText.substring(lastColonPos + 1).trim();
            bool allDigits = candidatePort.isNotEmpty();
            for (int i = 0; i < candidatePort.length() && allDigits; ++i)
                allDigits = juce::CharacterFunctions::isDigit(candidatePort[i]);

            if (allDigits)
            {
                hostOut = serverText.substring(0, lastColonPos).trim();
                portOut = juce::jlimit(1, 65535, candidatePort.getIntValue());
            }
        }

        return hostOut.isNotEmpty();
    }

    juce::String canonicalDelayUserKey(juce::String userId)
    {
        userId = normaliseOpusPeerId(userId);
        if (userId.startsWith("anonymous:"))
            userId = userId.substring(10);
        userId = userId.trim().toLowerCase();
        return userId;
    }

    juce::String getWrapperTypeName(juce::AudioProcessor::WrapperType wrapperType)
    {
        using WrapperType = juce::AudioProcessor::WrapperType;
        switch (wrapperType)
        {
            case WrapperType::wrapperType_Standalone: return "standalone";
            case WrapperType::wrapperType_VST: return "vst";
            case WrapperType::wrapperType_VST3: return "vst3";
            case WrapperType::wrapperType_AudioUnit: return "au";
            case WrapperType::wrapperType_AudioUnitv3: return "auv3";
            case WrapperType::wrapperType_AAX: return "aax";
            case WrapperType::wrapperType_LV2: return "lv2";
            default: break;
        }
        return "unknown";
    }

    inline float softClipSample(float x)
    {
        const float k = 2.0f;
        const float d = std::tanh(k);
        const float c = d / k;
        const float target = 0.891251f;

        float y = std::tanh(k * c * x);
        if (d != 0.0f)
            y = (y / d) * target;
        return y;
    }

    inline juce::String buildDefaultLocalChannelName(int channelIndex)
    {
        return "Ch" + juce::String(channelIndex + 1);
    }

    inline bool isDefaultLocalChannelName(const juce::String& name)
    {
        auto trimmed = name.trim();
        if (!trimmed.startsWithIgnoreCase("ch"))
            return false;

        auto numberPart = trimmed.substring(2).trim();
        if (numberPart.isEmpty() || !numberPart.containsOnly("0123456789"))
            return false;

        return numberPart.getIntValue() > 0;
    }

}

struct NinjamVst3AudioProcessor::LinkTimingState
{
    ableton::link::HostTimeFilter<ableton::LinkAudio::Clock> hostTimeFilter;
    double nextSampleTime = 0.0;

    void reset()
    {
        hostTimeFilter.reset();
        nextSampleTime = 0.0;
    }
};

NinjamVst3AudioProcessor::NinjamVst3AudioProcessor()
     : AudioProcessor (BusesProperties()
                     .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                     .withInput  ("Input 2", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 3", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 4", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 5", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 6", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 7", juce::AudioChannelSet::stereo(), false)
                     .withInput  ("Input 8", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output Main", juce::AudioChannelSet::stereo(), true)
                     .withOutput ("Output 2", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 3", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 4", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 5", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 6", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 7", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 8", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 9", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 10", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 11", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 12", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 13", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 14", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 15", juce::AudioChannelSet::stereo(), false)
                     .withOutput ("Output 16", juce::AudioChannelSet::stereo(), false)
                       )
{
    for (int i = 0; i < maxLocalChannels; ++i)
    {
        localChannelGains[(size_t)i].store(1.0f);
        localChannelPeaks[(size_t)i].store(0.0f);
        localChannelPeaksL[(size_t)i].store(0.0f);
        localChannelPeaksR[(size_t)i].store(0.0f);
        localChannelInputs[(size_t)i].store(-1);
        localChannelReverbSends[(size_t)i].store(0.0f);
        localChannelDelaySends[(size_t)i].store(0.0f);
        localChannelNames[(size_t)i] = buildDefaultLocalChannelName(i);
    }

    for (int i = 0; i < maxRemoteChordUsers; ++i)
    {
        remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
        remoteChordUserKeys[(size_t)i].clear();
    }

    localChordAnalyzer = std::make_unique<LocalChordAnalyzer>();
    linkTimingState = std::make_unique<LinkTimingState>();
    for (auto& analyzer : remoteChordAnalyzers)
        analyzer = std::make_unique<LocalChordAnalyzer>();

    setLatencySamples(0);
    startTimer(20); // Run NINJAM client loop every 20ms

    // Set callbacks
    ninjamClient.LicenseAgreementCallback = LicenseAgreementCallback;
    ninjamClient.LicenseAgreement_User = this;

    ninjamClient.ChatMessage_Callback = ChatMessage_Callback;
    ninjamClient.ChatMessage_User = this;
    ninjamClient.IntervalMediaItem_Callback = IntervalMediaItem_Callback;
    ninjamClient.IntervalMediaItem_User = this;
    ninjamClient.IntervalChunkCallback = IntervalChunkCallback_cb;
    ninjamClient.IntervalChunkCallbackUser = this;
    ninjamClient.RemoteChannelAudioTap = RemoteChannelAudioTap_Callback;
    ninjamClient.RemoteChannelAudioTap_User = this;
    ninjamClient.NewIntervalCallback = NewIntervalCallback_cb;
    ninjamClient.NewIntervalCallbackUser = this;
    opusSyncInstanceId = juce::Uuid().toString();
    
    // Default Metronome
    ninjamClient.config_metronome = 1.0f; // -12dB or similar? 1.0 is 0dB
    
    // Ensure disconnected state
    ninjamClient.Disconnect();

    // Initialize JNetLib (WSAStartup on Windows)
    JNL::open_socketlib();

    videoHelperRootDir = resolveVideoHelperRootDir();
    asyncChatTranslationWorker = std::make_unique<AsyncChatTranslationWorker>(*this);
    abletonLink = std::make_unique<ableton::LinkAudio>(120.0, getLinkPeerName().toStdString());
    refreshAbletonLinkActivation();
}

void NinjamVst3AudioProcessor::connectToServer(juce::String host, juce::String user, juce::String pass)
{
    host = host.trim();
    user = user.trim();
    pass = pass.trim();

    if (host.isEmpty())
        host = "127.0.0.1";

    if (user.isEmpty())
    {
        user = "anonymous:jammer";
        pass = "anon";
    }

    {
        const juce::ScopedLock lock(opusSyncPeerLock);
        opusSyncPeers.clear();
    }
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        lastAnnouncedRemoteIntervalByUser.clear();
        localIntervalStartMsByInterval.clear();
        pendingRemoteIntervalStartsByUser.clear();
        lastRemoteServerLatencyMsByUser.clear();
        pendingTransportProbeSentMsById.clear();
        remoteLatencyLastAppliedIntervalByUser.clear();
        remoteLatencyAverageByUser.clear();
        remoteLatencyFirmDelayMsByUser.clear();
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
    }
    opusSyncAvailable.store(false);
    opusSyncHasLegacyClients.store(false);
    lastOpusSupportBroadcastMs = 0.0;
    lastServerLatencyProbeAttemptMs = 0.0;
    localServerLatencyMs.store(-1);
    lastServerLatencyProbeInterval.store(-1);
    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();
    serverLatencyProbeInProgress.store(false);

    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs.clear();
        remoteLinkAudioSinks.clear();
    }

    applyCodecPreference();

    ninjamClient.Connect(host.toRawUTF8(), user.toRawUTF8(), pass.toRawUTF8());
    currentServer = host;
    currentUser = user;
    refreshAbletonLinkActivation();

    // Do NOT reset isTransmitting here — the user may have toggled it before
    // connecting. The NJC_STATUS_OK handler calls syncLocalIntervalChannelConfig()
    // which re-applies the current isTransmitting state to NJClient.
}

void NinjamVst3AudioProcessor::disconnectFromServer()
{
    ninjamClient.Disconnect();
    currentServer = {};
    currentUser = {};
    refreshAbletonLinkActivation();
    {
        const juce::ScopedLock lock(opusSyncPeerLock);
        opusSyncPeers.clear();
    }
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        lastAnnouncedRemoteIntervalByUser.clear();
        localIntervalStartMsByInterval.clear();
        pendingRemoteIntervalStartsByUser.clear();
        lastRemoteServerLatencyMsByUser.clear();
        pendingTransportProbeSentMsById.clear();
        remoteLatencyLastAppliedIntervalByUser.clear();
        remoteLatencyAverageByUser.clear();
        remoteLatencyFirmDelayMsByUser.clear();
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
    }
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs.clear();
        remoteLinkAudioSinks.clear();
    }
    opusSyncAvailable.store(false);
    opusSyncHasLegacyClients.store(false);
    lastServerLatencyProbeAttemptMs = 0.0;
    localServerLatencyMs.store(-1);
    lastServerLatencyProbeInterval.store(-1);
    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();
    serverLatencyProbeInProgress.store(false);
    applyCodecPreference();
}

void NinjamVst3AudioProcessor::sendChatMessage(juce::String msg)
{
    msg = msg.trim();
    if (msg.isEmpty())
        return;

    {
        juce::ScopedLock lock(chatLock);
        juce::String localLine = "Me: " + msg;
        chatHistory.add(localLine);
        chatSenders.add("me");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }

    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        ninjamClient.ChatMessage_Send("MSG", msg.toRawUTF8());
    juce::Logger::writeToLog("NINJAM Chat (local): " + msg);
}

void NinjamVst3AudioProcessor::setMetronomeVolume(float vol)
{
    vol = juce::jlimit(0.0f, 1.0f, vol);
    if (vol > 0.0f)
    {
        storedMetronomeVolume.store(vol);
        metronomeMuted.store(false);
    }
    ninjamClient.config_metronome = metronomeMuted.load() ? 0.0f : vol;
}

float NinjamVst3AudioProcessor::getMetronomeVolume() const
{
    return ninjamClient.config_metronome;
}

void NinjamVst3AudioProcessor::setMetronomeMuted(bool shouldMute)
{
    metronomeMuted.store(shouldMute);
    if (shouldMute)
    {
        const float current = ninjamClient.config_metronome;
        if (current > 0.0f)
            storedMetronomeVolume.store(current);
        ninjamClient.config_metronome = 0.0f;
    }
    else
    {
        ninjamClient.config_metronome = juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load());
    }
}

bool NinjamVst3AudioProcessor::isMetronomeMuted() const
{
    return metronomeMuted.load();
}

void NinjamVst3AudioProcessor::setStoredMetronomeVolume(float vol)
{
    storedMetronomeVolume.store(juce::jlimit(0.0f, 1.0f, vol));
}

float NinjamVst3AudioProcessor::getStoredMetronomeVolume() const
{
    return juce::jlimit(0.0f, 1.0f, storedMetronomeVolume.load());
}

bool NinjamVst3AudioProcessor::isOpusSyncAvailable() const
{
    return opusSyncAvailable.load();
}

juce::String NinjamVst3AudioProcessor::getIntervalSyncStatusText() const
{
    const juce::ScopedLock lock(intervalSyncStatusLock);
    return intervalSyncStatusText;
}

void NinjamVst3AudioProcessor::setIntervalSyncStatusText(const juce::String& text)
{
    const juce::ScopedLock lock(intervalSyncStatusLock);
    intervalSyncStatusText = text;
}

void NinjamVst3AudioProcessor::broadcastIntervalSyncTag(const juce::String& target, int markerBeatIndex)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const int displayInterval = getDisplayIntervalIndex();
    const int bpi = juce::jmax(1, getBPI());
    const float intervalProgress = juce::jlimit(0.0f, 1.0f, getIntervalProgress());
    const int currentBeatIndex = juce::jlimit(0, bpi - 1, (int)std::floor(intervalProgress * (float)bpi));
    const int beatIndex = markerBeatIndex >= 0 ? juce::jlimit(0, bpi - 1, markerBeatIndex) : currentBeatIndex;
    const juce::String userId = normaliseOpusPeerId(currentUser);
    const juce::String tag = buildIntervalSyncTag(displayInterval, bpi);

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "intervalSyncTag");
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("tag", tag);
    obj->setProperty("intervalIndex", displayInterval);
    obj->setProperty("intervalAbsolute", intervalIndex.load());
    obj->setProperty("bpi", bpi);
    obj->setProperty("beatIndex", beatIndex);
    obj->setProperty("intervalProgress", intervalProgress);
    if (const int serverLatencyMs = localServerLatencyMs.load(); serverLatencyMs >= 0)
        obj->setProperty("serverLatencyMs", serverLatencyMs);
    obj->setProperty("eventId", "intervalTag:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++sideSignalEventCounter));
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    const juce::String safeTarget = target.isNotEmpty() ? target : "*";
    sendIntervalSignal("intervalSyncTag", payload);
    return;
}

void NinjamVst3AudioProcessor::broadcastTransportProbe(const juce::String& target)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    const juce::String probeId = "probe:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++transportProbeCounter);
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const int displayInterval = getDisplayIntervalIndex();
    const int bpi = juce::jmax(1, getBPI());
    const float intervalProgress = juce::jlimit(0.0f, 1.0f, getIntervalProgress());
    const int beatIndex = juce::jlimit(0, bpi - 1, (int)std::floor(intervalProgress * (float)bpi));
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        pendingTransportProbeSentMsById[probeId] = nowMs;
        while ((int)pendingTransportProbeSentMsById.size() > 256)
            pendingTransportProbeSentMsById.erase(pendingTransportProbeSentMsById.begin());
    }

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "intervalTransportProbe");
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("probeId", probeId);
    obj->setProperty("intervalIndex", displayInterval);
    obj->setProperty("intervalAbsolute", intervalIndex.load());
    obj->setProperty("beatIndex", beatIndex);
    obj->setProperty("eventId", "transportProbe:" + probeId);
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    const juce::String safeTarget = target.isNotEmpty() ? target : "*";
    sendIntervalSignal("intervalTransportProbe", payload);
}

void NinjamVst3AudioProcessor::measureServerLatencyAsync()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    bool expected = false;
    if (!serverLatencyProbeInProgress.compare_exchange_strong(expected, true))
        return;

    const juce::String serverText = currentServer;
    if (serverText.isEmpty())
    {
        serverLatencyProbeInProgress.store(false);
        return;
    }

    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();

    serverLatencyProbeFuture = std::async(std::launch::async, [this, serverText]()
    {
        struct ProbeScope
        {
            explicit ProbeScope(std::atomic<bool>& flagIn) : flag(flagIn) {}
            ~ProbeScope() { flag.store(false); }

            std::atomic<bool>& flag;
        } scope(serverLatencyProbeInProgress);

        juce::String host;
        int port = 2049;
        if (!tryParseServerEndpoint(serverText, host, port))
            return;

        juce::StreamingSocket socket;
        const double startMs = juce::Time::getMillisecondCounterHiRes();
        if (!socket.connect(host.toRawUTF8(), port, 600))
            return;

        const double rttMs = juce::Time::getMillisecondCounterHiRes() - startMs;
        if (!std::isfinite(rttMs) || rttMs <= 0.0 || rttMs > 4000.0)
            return;

        const int measuredOneWayMs = juce::jlimit(0, 2000, (int)std::llround(rttMs * 0.5));
        const int priorOneWayMs = localServerLatencyMs.load();
        const int smoothedOneWayMs = priorOneWayMs >= 0
            ? juce::jlimit(0, 2000, (int)std::llround((double)priorOneWayMs * 0.7 + (double)measuredOneWayMs * 0.3))
            : measuredOneWayMs;

        localServerLatencyMs.store(smoothedOneWayMs);
        vlogStr("[INTSYNC] Server RTT host=" + host + ":" + juce::String(port)
            + " rttMs=" + juce::String((int)std::llround(rttMs))
            + " oneWayMs=" + juce::String(smoothedOneWayMs));
    });
}

void NinjamVst3AudioProcessor::broadcastOpusSyncSupport(const juce::String& target)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const juce::String userId = normaliseOpusPeerId(currentUser);
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("type", "opusSyncSupport");
    obj->setProperty("userId", userId.isNotEmpty() ? userId : currentUser);
    obj->setProperty("clientId", opusSyncInstanceId);
    obj->setProperty("appFamily", opusSyncAppFamily);
    obj->setProperty("handshakeVersion", opusSyncHandshakeVersion);
    obj->setProperty("runtimeFormat", getWrapperTypeName(wrapperType));
    obj->setProperty("pluginName", juce::String(JucePlugin_Name));
    obj->setProperty("pluginVersion", juce::String(NINJAM_DISPLAY_VERSION));
    obj->setProperty("supportsOpus", true);
    obj->setProperty("enabled", numLocalChannels.load() > 1);
    obj->setProperty("numChannels", numLocalChannels.load());
    obj->setProperty("eventId", "opusSupport:" + (userId.isNotEmpty() ? userId : currentUser) + ":" + juce::String(++sideSignalEventCounter));
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    vlogStr("broadcastOpusSyncSupport -> target=" + (target.isNotEmpty() ? target : "*") + " enabled=" + juce::String(numLocalChannels.load() > 1 ? "true" : "false"));
    // Use NINJAM interval channel with custom FOURCC — works on any standard server,
    // routed like audio data, other clients silently ignore unknown FOURCCs.
    // Target is ignored here (interval data goes to all subscribers of our channel 0).
    juce::ignoreUnused(target);
    ninjamClient.SendRawIntervalItem(0, kOpusSyncFourcc, payload.toRawUTF8(), (int)payload.getNumBytesAsUTF8());
}

void NinjamVst3AudioProcessor::refreshOpusSyncAvailabilityFromUsers()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    bool available = false;
    int freshPeerCount = 0;
    {
        const juce::ScopedLock lock(opusSyncPeerLock);
        for (auto it = opusSyncPeers.begin(); it != opusSyncPeers.end();)
        {
            const auto& peer = it->second;
            const bool isFresh = (nowMs - peer.lastSeenMs) <= 6500.0;
            if (peer.supportsOpus && isFresh)
                ++it;
            else
                it = opusSyncPeers.erase(it);
        }
        available = !opusSyncPeers.empty();
        freshPeerCount = (int)opusSyncPeers.size();
    }

    // Rebuild the quick username→multiChan snapshot (separate lock, no njclient calls)
    {
        const juce::ScopedLock lock2(opusSyncPeerLock);
        const juce::ScopedLock mcLock(peerMultiChanLock);
        peerMultiChanByName.clear();
        for (auto& [key, peer] : opusSyncPeers)
        {
            if (peer.supportsOpus && !peer.userId.isEmpty())
            {
                const juce::String snapKey = canonicalDelayUserKey(peer.userId);
                peerMultiChanByName[snapKey] = { peer.multiChanEnabled, peer.numChannels };
            }
        }
    }

    const int remoteUserCount = juce::jmax(0, ninjamClient.GetNumUsers());
    const bool hasLegacyClients = remoteUserCount > freshPeerCount;

    const bool previous = opusSyncAvailable.exchange(available);
    const bool previousLegacy = opusSyncHasLegacyClients.exchange(hasLegacyClients);
    if (!previous && available)
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Multi Channel Client Detected.");
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }
    if (previous != available || previousLegacy != hasLegacyClients)
    {
        applyCodecPreference();
        syncLocalIntervalChannelConfig();
    }
}

void NinjamVst3AudioProcessor::setTransmitLocal(bool shouldTransmit)
{
    isTransmitting = shouldTransmit;
    syncLocalIntervalChannelConfig();
}

void NinjamVst3AudioProcessor::syncLocalIntervalChannelConfig()
{
    const bool shouldTransmit = isTransmitting;
    const int bitrate = shouldTransmit ? localBitrate : 24;
    const int flags = voiceChatMode ? 2 : 0;
    const int numCh = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load() && shouldTransmit;
    const bool singleStereoLocal = numCh == 1 && getLocalChannelInput(0) < 0;

    if (multiChanAuto)
    {
        // NINJAM ch 0: Vorbis mixdown (for all clients including legacy)
        // NINJAM ch 1..N: Opus per-channel (for our VST3 clients only)
        juce::String ch0Name = getLocalChannelName(0);
        if (ch0Name.isEmpty()) ch0Name = "Mix";
        ninjamClient.SetLocalChannelInfo(0, ch0Name.toRawUTF8(),
            true, numCh,          // srcch = mix buffer at inputs[numCh]
            true, bitrate, true, true, false, 0, true, flags);
        // Mute engine local output — monitor block handles local audio routing with proper stereo.
        ninjamClient.SetLocalChannelMonitoring(0, false, 0.f, false, 0.f, true, true, false, false);
        for (int i = 0; i < numCh; ++i)
        {
            juce::String chName = getLocalChannelName(i);
            if (chName.isEmpty()) chName = "Ch " + juce::String(i + 1);
            ninjamClient.SetLocalChannelInfo(i + 1, chName.toRawUTF8(),
                true, i,          // srcch = original buffer slot i
                true, bitrate, true, true, false, 0, true, flags);
            ninjamClient.SetLocalChannelMonitoring(i + 1, false, 0.f, false, 0.f, true, true, false, false);
        }
        for (int i = numCh + 1; i <= maxLocalChannels; ++i)
            ninjamClient.DeleteLocalChannel(i);
    }
    else
    {
        // Vorbis only: single channel
        juce::String ch0Name = getLocalChannelName(0);
        if (ch0Name.isEmpty()) ch0Name = "Input";
        const int sourceChannel = shouldTransmit ? (singleStereoLocal ? 1024 : 0) : 1023;
        ninjamClient.SetLocalChannelInfo(0, ch0Name.toRawUTF8(),
            true, sourceChannel, true, bitrate, true, true, false, 0, true, flags);
        // Mute engine local output — monitor block handles local audio routing with proper stereo.
        ninjamClient.SetLocalChannelMonitoring(0, false, 0.f, false, 0.f, true, true, false, false);
        for (int i = 1; i <= maxLocalChannels; ++i)
            ninjamClient.DeleteLocalChannel(i);
    }

    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        ninjamClient.NotifyServerOfChannelChange();
}

void NinjamVst3AudioProcessor::setLocalBitrate(int bitrate)
{
    localBitrate = bitrate;
    syncLocalIntervalChannelConfig();
}

int NinjamVst3AudioProcessor::getLocalBitrate() const
{
    return localBitrate;
}

void NinjamVst3AudioProcessor::setVoiceChatMode(bool enabled)
{
    voiceChatMode = enabled;
    syncLocalIntervalChannelConfig();
}

bool NinjamVst3AudioProcessor::isVoiceChatMode() const
{
    return voiceChatMode;
}

void NinjamVst3AudioProcessor::applyCodecPreference()
{
    const int numCh = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
    const bool multiChanAuto = numCh > 1 && opusSyncAvailable.load();
    const int decodeCaps = NJClient::NJCLIENT_CAP_DECODE_VORBIS | NJClient::NJCLIENT_CAP_DECODE_OPUS;

    if (multiChanAuto)
    {
        // ch 0: Vorbis only (mixdown for all clients)
        // ch 1..N: Opus only (per-channel for our VST3 clients)
        unsigned int vorbisMask = 0x1u;
        unsigned int opusMask = 0u;
        for (int i = 0; i < numCh; ++i)
            opusMask |= (1u << (i + 1));
        ninjamClient.SetCodecCapabilities(
            NJClient::NJCLIENT_CAP_ENCODE_VORBIS | NJClient::NJCLIENT_CAP_ENCODE_OPUS, decodeCaps);
        ninjamClient.SetCodecConfig(vorbisMask, opusMask);
    }
    else
    {
        // Single channel or no VST3 peers: Vorbis only
        ninjamClient.SetCodecCapabilities(NJClient::NJCLIENT_CAP_ENCODE_VORBIS, decodeCaps);
        ninjamClient.SetCodecConfig(0x1u, 0u);
    }
}

juce::String NinjamVst3AudioProcessor::buildIntervalSyncTag(int interval, int length) const
{
    const juce::String userPart = currentUser.isNotEmpty() ? currentUser : "unknown";
    return userPart + ":" + juce::String(interval) + ":" + juce::String(length);
}

void NinjamVst3AudioProcessor::invalidateIntervalSyncLatencyState(bool keepRemoteServerLatency)
{
    lastBroadcastIntervalTag.store(-1);
    lastProcessedIntervalMarkerKey.store(-1);
    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
    lastAnnouncedRemoteIntervalByUser.clear();
    localIntervalStartMsByInterval.clear();
    pendingRemoteIntervalStartsByUser.clear();
    pendingTransportProbeSentMsById.clear();
    remoteLatencyLastAppliedIntervalByUser.clear();
    remoteLatencyAverageByUser.clear();
    if (!keepRemoteServerLatency)
    {
        remoteLatencyFirmDelayMsByUser.clear();
        remoteVideoBufferRefreshIdByUser.clear();
        videoBufferRefreshCounter = 0;
        lastRemoteServerLatencyMsByUser.clear();
    }
}

void NinjamVst3AudioProcessor::pruneDisconnectedRemoteSyncState()
{
    std::set<juce::String> activeUserKeys;
    const int numUsers = juce::jmax(0, ninjamClient.GetNumUsers());
    for (int userIdx = 0; userIdx < numUsers; ++userIdx)
    {
        const char* userNameChars = ninjamClient.GetUserState(userIdx, nullptr, nullptr, nullptr);
        if (userNameChars == nullptr || userNameChars[0] == '\0')
            continue;

        const juce::String userName = juce::String::fromUTF8(userNameChars);
        const juce::String senderKey = normaliseOpusPeerId(userName);
        const juce::String canonicalUserKey = canonicalDelayUserKey(userName);
        if (senderKey.isNotEmpty())
            activeUserKeys.insert(senderKey);
        if (canonicalUserKey.isNotEmpty())
            activeUserKeys.insert(canonicalUserKey);
    }

    const auto isActiveUserKey = [&activeUserKeys](const juce::String& key) -> bool
    {
        return key.isNotEmpty() && activeUserKeys.find(key) != activeUserKeys.end();
    };

    const juce::ScopedLock lock(intervalSyncAnnouncementLock);

    for (auto it = lastAnnouncedRemoteIntervalByUser.begin(); it != lastAnnouncedRemoteIntervalByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = lastAnnouncedRemoteIntervalByUser.erase(it);
        else
            ++it;
    }

    for (auto it = lastRemoteServerLatencyMsByUser.begin(); it != lastRemoteServerLatencyMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = lastRemoteServerLatencyMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteLatencyLastAppliedIntervalByUser.begin(); it != remoteLatencyLastAppliedIntervalByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteLatencyLastAppliedIntervalByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteLatencyAverageByUser.begin(); it != remoteLatencyAverageByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteLatencyAverageByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteLatencyFirmDelayMsByUser.begin(); it != remoteLatencyFirmDelayMsByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteLatencyFirmDelayMsByUser.erase(it);
        else
            ++it;
    }

    for (auto it = remoteVideoBufferRefreshIdByUser.begin(); it != remoteVideoBufferRefreshIdByUser.end();)
    {
        if (!isActiveUserKey(it->first))
            it = remoteVideoBufferRefreshIdByUser.erase(it);
        else
            ++it;
    }

    for (auto it = pendingRemoteIntervalStartsByUser.begin(); it != pendingRemoteIntervalStartsByUser.end();)
    {
        const juce::String pendingSenderKey = it->second.senderKey.isNotEmpty()
            ? it->second.senderKey
            : it->first.upToFirstOccurrenceOf(":", false, false);

        if (!isActiveUserKey(pendingSenderKey))
            it = pendingRemoteIntervalStartsByUser.erase(it);
        else
            ++it;
    }
}

static juce::File getThisModuleFile()
{
#ifdef _WIN32
    HMODULE hm = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&getThisModuleFile,
                            &hm))
        return {};

    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(hm, path, (DWORD)std::size(path)) == 0)
        return {};
    return juce::File(juce::String(path));
#else
    Dl_info info {};
    if (dladdr((void*)&getThisModuleFile, &info) == 0 || info.dli_fname == nullptr)
        return {};
    return juce::File(juce::String::fromUTF8(info.dli_fname));
#endif
}

juce::File NinjamVst3AudioProcessor::resolveVideoHelperRootDir() const
{
    juce::Array<juce::File> candidates;
    juce::Array<juce::File> roots;

    const juce::File exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    roots.add(exeDir);

    const juce::File moduleFile = getThisModuleFile();
    if (moduleFile.existsAsFile())
        roots.addIfNotAlreadyThere(moduleFile.getParentDirectory());

    for (const auto& root : roots)
    {
        juce::File probe = root;
        for (int i = 0; i < 8; ++i)
        {
            candidates.add(probe.getChildFile("Resources").getChildFile("advanced-vdo-client"));
            candidates.add(probe.getParentDirectory().getChildFile("Resources").getChildFile("advanced-vdo-client"));
            candidates.add(probe.getChildFile("advanced-vdo-client"));
            probe = probe.getParentDirectory();
        }
    }

    for (const auto& dir : candidates)
    {
        if (dir.isDirectory()
            && dir.getChildFile("index.html").existsAsFile()
            && (dir.getChildFile("app.html").existsAsFile() || dir.getChildFile("server.js").existsAsFile()))
            return dir;
    }

    return {};
}

bool NinjamVst3AudioProcessor::isAdvancedVideoClientAvailable() const
{
    // Use a fast localhost TCP probe; if connect succeeds, helper is up.
    juce::StreamingSocket sock;
    if (!sock.connect("127.0.0.1", 8100, 500))
        return false;
    return true;
}

bool NinjamVst3AudioProcessor::ensureAdvancedVideoClientStarted()
{
    if (isAdvancedVideoClientAvailable())
    {
        videoHelperRunning.store(true);
        lastIntervalHelperPayloadWriteMs = 0.0;
        return true;
    }

    const juce::File rootDir = resolveVideoHelperRootDir();
    if (!rootDir.isDirectory())
        return false;

    if (!advancedVideoServer)
    {
        advancedVideoServer = std::make_unique<LocalVideoHttpServer>(
            rootDir,
            [this]()
            {
                const juce::ScopedLock lock(intervalHelperPayloadLock);
                return intervalHelperPayload;
            });
    }

    if (!advancedVideoServer->start())
    {
        advancedVideoServer.reset();
        return false;
    }

    for (int i = 0; i < 10; ++i)
    {
        juce::Thread::sleep(50);
        if (isAdvancedVideoClientAvailable())
        {
            videoHelperRunning.store(true);
            lastIntervalHelperPayloadWriteMs = 0.0;
            return true;
        }
    }

    if (advancedVideoServer)
        advancedVideoServer->stop();
    advancedVideoServer.reset();
    videoHelperRunning.store(false);
    lastIntervalHelperPayloadWriteMs = 0.0;
    return false;
}

void NinjamVst3AudioProcessor::launchVideoSessionAsync()
{
    bool expected = false;
    if (!videoLaunchInProgress.compare_exchange_strong(expected, true))
        return;

    const juce::ScopedLock launchLock(videoLaunchWorkerLock);
    if (videoLaunchFuture.valid())
        videoLaunchFuture.wait();

    videoLaunchFuture = std::async(std::launch::async, [this]()
    {
        struct VideoLaunchScope
        {
            explicit VideoLaunchScope(std::atomic<bool>& inProgress) : flag(inProgress) {}
            ~VideoLaunchScope() { flag.store(false); }

            std::atomic<bool>& flag;
        } scope(videoLaunchInProgress);

        try
        {
            launchVideoSession();
        }
        catch (...)
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add("Video launch failed unexpectedly; opening direct VDO link may be unavailable.");
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
    });
}

void NinjamVst3AudioProcessor::stopAdvancedVideoClient()
{
    videoHelperRunning.store(false);
    lastIntervalHelperPayloadWriteMs = 0.0;
    if (advancedVideoServer)
        advancedVideoServer->stop();
    advancedVideoServer.reset();
}



void NinjamVst3AudioProcessor::launchVideoSession()
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Connect to a server first, then click VDO.");
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
        return;
    }

    juce::String roomSource = currentServer.trim();
    const int schemePos = roomSource.indexOf("://");
    if (schemePos >= 0)
        roomSource = roomSource.substring(schemePos + 3);
    const int slashPos = roomSource.indexOfChar('/');
    if (slashPos >= 0)
        roomSource = roomSource.substring(0, slashPos);
    const int atPos = roomSource.lastIndexOfChar('@');
    if (atPos >= 0 && atPos + 1 < roomSource.length())
        roomSource = roomSource.substring(atPos + 1);

    juce::String hostPart = roomSource.trim();
    juce::String portPart;
    const int lastColonPos = hostPart.lastIndexOfChar(':');
    if (lastColonPos > 0 && lastColonPos + 1 < hostPart.length())
    {
        const juce::String candidatePort = hostPart.substring(lastColonPos + 1).trim();
        bool allDigits = candidatePort.isNotEmpty();
        for (int i = 0; i < candidatePort.length() && allDigits; ++i)
            allDigits = juce::CharacterFunctions::isDigit(candidatePort[i]);
        if (allDigits)
        {
            hostPart = hostPart.substring(0, lastColonPos);
            portPart = candidatePort;
        }
    }

    const int firstDotPos = hostPart.indexOfChar('.');
    if (firstDotPos > 0)
        hostPart = hostPart.substring(0, firstDotPos);

    juce::String roomRaw = hostPart;
    if (portPart.isNotEmpty())
        roomRaw << "_" << portPart;

    juce::String room;
    bool lastWasUnderscore = false;
    for (int i = 0; i < roomRaw.length(); ++i)
    {
        const juce_wchar ch = roomRaw[i];
        if (juce::CharacterFunctions::isLetterOrDigit(ch))
        {
            room << juce::String::charToString((juce_wchar) juce::CharacterFunctions::toLowerCase(ch));
            lastWasUnderscore = false;
        }
        else if (!lastWasUnderscore)
        {
            room << "_";
            lastWasUnderscore = true;
        }
    }
    room = room.trimCharactersAtStart("_").trimCharactersAtEnd("_");
    if (room.isEmpty())
        room = "ninjam_room";
    const juce::String cleanUserLabel = normaliseChatTargetNick(currentUser);
    const juce::String label = cleanUserLabel.isNotEmpty() ? cleanUserLabel : "NINJAM";
    static constexpr int minimumVdoBufferMs = 0;
    int viewDelayMs = 0;
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        for (const auto& entry : remoteLatencyFirmDelayMsByUser)
            viewDelayMs = juce::jmax(viewDelayMs, juce::jmax(0, entry.second));
    }
    const int launchBufferMs = juce::jmax(minimumVdoBufferMs, viewDelayMs);
    const int chunkMs = juce::jlimit(60, 800, (int)std::llround((double)launchBufferMs * 0.25));

    if (ensureAdvancedVideoClientStarted())
    {
        juce::URL helperUrl("http://127.0.0.1:8100/buffer-room");
        helperUrl = helperUrl.withParameter("room", room)
                             .withParameter("label", label)
                             .withParameter("bufferMode", "remote")
                             .withParameter("buffer", juce::String(launchBufferMs))
                             .withParameter("chunked", juce::String(chunkMs));
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add("Tip: If your cam isn't showing, refresh the video page and select your camera before entering the room.");
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
        const auto helperUrlText = helperUrl.toString(true);
        const bool opened = openUrlExternalOnMessageThread(helperUrlText);
        if (!opened)
        {
            juce::ScopedLock lock(chatLock);
            chatHistory.add("Failed to open video helper URL: " + helperUrlText);
            chatSenders.add("");
            chatRevision.fetch_add(1);
            if (chatHistory.size() > 100)
            {
                chatHistory.removeRange(0, chatHistory.size() - 100);
                chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
            }
        }
        return;
    }

    juce::URL url("https://vdo.ninja/");
    url = url.withParameter("room", room)
             .withParameter("label", label)
             .withParameter("chunked", juce::String(chunkMs))
             .withParameter("chunkbufferadaptive", "0")
             .withParameter("chunkbufferceil", "180000")
             .withParameter("noaudio", "1")
             .withParameter("buffer2", "0")
             .withParameter("buffer", juce::String(launchBufferMs));
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Advanced sync helper unavailable on this machine; opening direct VDO view without live auto-buffer updates.");
        chatSenders.add("");
        chatHistory.add("Tip: If your cam isn't showing, refresh the video page and select your camera before entering the room.");
        chatSenders.add("");
        chatRevision.fetch_add(2);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }
    const auto directUrlText = url.toString(true);
    const bool opened = openUrlExternalOnMessageThread(directUrlText);
    if (!opened)
    {
        juce::ScopedLock lock(chatLock);
        chatHistory.add("Failed to open VDO URL: " + directUrlText);
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }
}

void NinjamVst3AudioProcessor::writeIntervalHelperJson(int pos, int length)
{
    if (!videoHelperRunning.load())
        return;

    const int safeLength = juce::jmax(1, length);
    const int displayInterval = getDisplayIntervalIndex();
    const int bpi = juce::jmax(1, getBPI());
    const double bpm = juce::jmax(1.0, (double)getBPM());
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double globalUnit = (double)displayInterval * (double)safeLength + (double)juce::jlimit(0, safeLength, pos);
    const double beatLength = (double)safeLength / (double)bpi;
    const double globalBeat = beatLength > 0.0 ? std::floor(globalUnit / beatLength) : 0.0;
    const juce::String syncTag = buildIntervalSyncTag(displayInterval, safeLength);

    juce::Array<juce::var> entries;
    {
        juce::DynamicObject::Ptr infoObj = new juce::DynamicObject();
        infoObj->setProperty("type", "intervalInfo");
        infoObj->setProperty("interval", displayInterval);
        infoObj->setProperty("pos", pos);
        infoObj->setProperty("length", safeLength);
        infoObj->setProperty("bpm", bpm);
        infoObj->setProperty("bpi", bpi);
        infoObj->setProperty("globalUnit", globalUnit);
        infoObj->setProperty("globalBeat", globalBeat);
        infoObj->setProperty("videoClockMs", nowMs);
        infoObj->setProperty("syncTag", syncTag);
        infoObj->setProperty("bufferMode", "remote");
        infoObj->setProperty("voiceChatMode", false);
        entries.add(juce::var(infoObj.get()));
    }

    const int numUsers = ninjamClient.GetNumUsers();
    for (int userIdx = 0; userIdx < numUsers; ++userIdx)
    {
        const char* userNameChars = ninjamClient.GetUserState(userIdx, nullptr, nullptr, nullptr);
        if (!userNameChars || !userNameChars[0])
            continue;

        const juce::String userName = juce::String::fromUTF8(userNameChars);
        const juce::String senderKey = normaliseOpusPeerId(userName);
        const juce::String canonicalUserKey = canonicalDelayUserKey(userName);
        const int localServerRouteLatencyMs = juce::jmax(0, localServerLatencyMs.load());
        bool remoteSub = false;
        float remoteChVol = 1.0f, remoteChPan = 0.0f;
        bool remoteChMute = false, remoteChSolo = false;
        int remoteOutCh = 0, remoteFlags = 0;
        const char* remoteChannelName = ninjamClient.GetUserChannelState(userIdx, 0,
                                         &remoteSub,
                                         &remoteChVol,
                                         &remoteChPan,
                                         &remoteChMute,
                                         &remoteChSolo,
                                         &remoteOutCh,
                                         &remoteFlags);
        const bool remoteVoiceChatMode = (remoteChannelName != nullptr) && ((remoteFlags & 2) != 0);
        time_t lastUpdate = 0;
        double maxLen = 0.0;
        const double userPos = ninjamClient.GetUserSessionPos(userIdx, &lastUpdate, &maxLen);

        int bufferMs = -1;
        int remoteServerLatencyMs = -1;
        juce::uint64 bufferRefreshId = 0;
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            auto firmIt = remoteLatencyFirmDelayMsByUser.find(senderKey);
            if (firmIt != remoteLatencyFirmDelayMsByUser.end())
                bufferMs = juce::jmax(0, firmIt->second);
            if (bufferMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalFirmIt = remoteLatencyFirmDelayMsByUser.find(canonicalUserKey);
                if (canonicalFirmIt != remoteLatencyFirmDelayMsByUser.end())
                    bufferMs = juce::jmax(0, canonicalFirmIt->second);
            }
            auto serverIt = lastRemoteServerLatencyMsByUser.find(senderKey);
            if (serverIt != lastRemoteServerLatencyMsByUser.end())
                remoteServerLatencyMs = juce::jmax(0, serverIt->second);
            if (remoteServerLatencyMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalServerIt = lastRemoteServerLatencyMsByUser.find(canonicalUserKey);
                if (canonicalServerIt != lastRemoteServerLatencyMsByUser.end())
                    remoteServerLatencyMs = juce::jmax(0, canonicalServerIt->second);
            }
            auto refreshIt = remoteVideoBufferRefreshIdByUser.find(senderKey);
            if (refreshIt != remoteVideoBufferRefreshIdByUser.end())
                bufferRefreshId = refreshIt->second;
            if (bufferRefreshId == 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalRefreshIt = remoteVideoBufferRefreshIdByUser.find(canonicalUserKey);
                if (canonicalRefreshIt != remoteVideoBufferRefreshIdByUser.end())
                    bufferRefreshId = canonicalRefreshIt->second;
            }
            if (bufferMs < 0)
            {
                auto avgIt = remoteLatencyAverageByUser.find(senderKey);
                if (avgIt != remoteLatencyAverageByUser.end())
                {
                    const auto& state = avgIt->second;
                    double fallback = state.firmAverageMs;
                    if (!(fallback > 0.0))
                        fallback = state.averageMs;
                    if (!(fallback > 0.0))
                        fallback = state.lastMeasurementMs;
                    if (fallback > 0.0)
                        bufferMs = juce::jmax(0, (int)std::llround(fallback)) + juce::jmax(0, remoteServerLatencyMs) + localServerRouteLatencyMs;
                }
            }
            if (bufferMs < 0 && canonicalUserKey.isNotEmpty())
            {
                auto canonicalAvgIt = remoteLatencyAverageByUser.find(canonicalUserKey);
                if (canonicalAvgIt != remoteLatencyAverageByUser.end())
                {
                    const auto& state = canonicalAvgIt->second;
                    double fallback = state.firmAverageMs;
                    if (!(fallback > 0.0))
                        fallback = state.averageMs;
                    if (!(fallback > 0.0))
                        fallback = state.lastMeasurementMs;
                    if (fallback > 0.0)
                        bufferMs = juce::jmax(0, (int)std::llround(fallback)) + juce::jmax(0, remoteServerLatencyMs) + localServerRouteLatencyMs;
                }
            }
            // Diagnostic log per-user buffer decision
        }

        juce::DynamicObject::Ptr userObj = new juce::DynamicObject();
        userObj->setProperty("type", "videoTimecode");
        userObj->setProperty("userId", userName);
        userObj->setProperty("userKey", canonicalUserKey);
        userObj->setProperty("interval", displayInterval);
        userObj->setProperty("timecode", userPos);
        userObj->setProperty("globalUnit", (double)displayInterval * (double)safeLength + userPos);
        userObj->setProperty("globalBeat", globalBeat);
        userObj->setProperty("videoClockMs", nowMs);
        userObj->setProperty("syncTag", syncTag);
        userObj->setProperty("bufferMode", remoteVoiceChatMode ? "realtime" : "remote");
        userObj->setProperty("voiceChatMode", remoteVoiceChatMode);
        if (bufferMs >= 0)
        {
            userObj->setProperty("bufferTotalMs", (double)bufferMs);
            userObj->setProperty("senderBufferMs", 0.0);
            userObj->setProperty("receiverBufferMs", (double)bufferMs);
            userObj->setProperty("measuredAudioDelayMs", (double)bufferMs);
            if (remoteServerLatencyMs >= 0)
                userObj->setProperty("senderServerLatencyMs", (double)remoteServerLatencyMs);
            if (localServerRouteLatencyMs >= 0)
                userObj->setProperty("receiverServerLatencyMs", (double)localServerRouteLatencyMs);
            if (bufferRefreshId != 0)
            {
                userObj->setProperty("refreshBuffer", true);
                userObj->setProperty("bufferRefreshEventId", "videoBufferRefresh:" + canonicalUserKey + ":" + juce::String((juce::int64)bufferRefreshId));
            }
        }
        entries.add(juce::var(userObj.get()));
    }

    const juce::String payload = juce::JSON::toString(juce::var(entries), false);
    const juce::ScopedLock lock(intervalHelperPayloadLock);
    intervalHelperPayload = payload;
}

bool NinjamVst3AudioProcessor::isTransmittingLocal() const
{
    return isTransmitting;
}

juce::StringArray NinjamVst3AudioProcessor::getChatMessages()
{
    juce::ScopedLock lock(chatLock);
    return chatHistory;
}

void NinjamVst3AudioProcessor::addSystemChatMessage(const juce::String& message)
{
    const juce::ScopedLock lock(chatLock);
    chatHistory.add(message);
    chatSenders.add("");
    chatRevision.fetch_add(1);
    if (chatHistory.size() > 100)
    {
        chatHistory.removeRange(0, chatHistory.size() - 100);
        chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
    }
}

void NinjamVst3AudioProcessor::noteTranslationFailure(const juce::String& reason)
{
    juce::Logger::writeToLog("Auto Translate failed: " + reason);

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    bool shouldPostNotice = false;
    {
        const juce::ScopedLock lock(chatLock);
        const bool reasonChanged = lastTranslationFailureReason != reason;
        const bool cooldownExpired = (nowMs - lastTranslationFailureNoticeMs) >= 30000.0;

        if (!translationFailureActive || reasonChanged || cooldownExpired)
        {
            translationFailureActive = true;
            lastTranslationFailureNoticeMs = nowMs;
            lastTranslationFailureReason = reason;
            shouldPostNotice = true;
        }
    }

    if (shouldPostNotice)
    {
        addSystemChatMessage("Auto Translate failed; incoming chat will stay in the original language until the translator responds again.");
    }
}

void NinjamVst3AudioProcessor::clearTranslationFailureState()
{
    const juce::ScopedLock lock(chatLock);
    translationFailureActive = false;
    lastTranslationFailureReason.clear();
}

void NinjamVst3AudioProcessor::setAutoTranslateEnabled(bool shouldEnable)
{
    bool changed = false;
    {
        juce::ScopedLock lock(chatLock);
        changed = autoTranslate != shouldEnable;
        autoTranslate = shouldEnable;
    }

    if (changed)
        translationConfigRevision.fetch_add(1, std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isAutoTranslateEnabled() const
{
    return autoTranslate;
}

void NinjamVst3AudioProcessor::setTranslateSourceLang(const juce::String& langCode)
{
    juce::ScopedLock lock(chatLock);
    juce::ignoreUnused(langCode);
    translateSourceLang = "auto";
}

juce::String NinjamVst3AudioProcessor::getTranslateSourceLang() const
{
    return translateSourceLang;
}

void NinjamVst3AudioProcessor::setTranslateTargetLang(const juce::String& langCode)
{
    juce::String normalised = langCode.trim().toLowerCase();
    if (normalised.isEmpty())
        normalised = "system";

    bool changed = false;
    {
        juce::ScopedLock lock(chatLock);
        changed = translateTargetLang != normalised;
        translateTargetLang = normalised;
    }

    if (changed)
        translationConfigRevision.fetch_add(1, std::memory_order_relaxed);
}

juce::String NinjamVst3AudioProcessor::getTranslateTargetLang() const
{
    return translateTargetLang;
}

std::vector<NinjamVst3AudioProcessor::UserInfo> NinjamVst3AudioProcessor::getConnectedUsers()
{
    std::vector<UserInfo> users;
    int numUsers = ninjamClient.GetNumUsers();
    bool spread = spreadOutputsEnabled.load();

    const int maxOutputPairs = 16;
    std::set<int> reservedPairs;
    if (spread)
    {
        for (auto& kv : userOutputAssignment)
        {
            int pair = kv.second;
            if (pair >= 0 && pair < maxOutputPairs)
                reservedPairs.insert(pair);
        }

        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        for (const auto& pair : remoteLinkAudioOutputPairs)
        {
            if (pair.second >= 0 && pair.second < maxOutputPairs)
                reservedPairs.insert(pair.second);
        }
    }

    std::set<int> usedPairsThisCall;
    std::set<int> activeUserIndexes;

    for (int i=0; i<numUsers; ++i)
    {
        const char* name = ninjamClient.GetUserState(i, nullptr, nullptr, nullptr);
        if (name)
        {
            activeUserIndexes.insert(i);
            UserInfo u;
            u.index = i;
            juce::String fullName = juce::String::fromUTF8(name);
            int atPos = fullName.indexOfChar('@');
            if (atPos > 0)
                u.name = fullName.substring(0, atPos);
            else
                u.name = fullName;

            const juce::String userIdentityKey = canonicalDelayUserKey(u.name);
            auto identityIt = remoteUserNameByIndex.find(i);
            if (identityIt == remoteUserNameByIndex.end() || identityIt->second != userIdentityKey)
            {
                resetRemoteUserIndexState(i, u.name);
                remoteUserNameByIndex[i] = userIdentityKey;
            }
            
            bool sub = false;
            float chVol = 1.0f, chPan = 0.0f;
            bool chMute = false, chSolo = false;
            int outCh = 0, flags = 0;
            const char* chName = ninjamClient.GetUserChannelState(i, 0, &sub, &chVol, &chPan, &chMute, &chSolo, &outCh, &flags);
            if (chName)
            {
                float baseVol = chVol;
                bool hasStored = false;
                auto byNameIt = userVolumeByName.find(u.name);
                if (byNameIt != userVolumeByName.end())
                {
                    baseVol = byNameIt->second;
                    hasStored = true;
                }

                auto volIt = userBaseVolume.find(i);
                if (volIt != userBaseVolume.end())
                {
                    baseVol = volIt->second;
                    hasStored = true;
                }

                if (!hasStored)
                    baseVol = 1.0f;

                u.volume = baseVol;

                auto panIt = userPanOverrides.find(i);
                if (panIt != userPanOverrides.end())
                    u.pan = panIt->second;
                else
                    u.pan = chPan;

                u.isMuted = chMute;
                u.isSolo = chSolo;
                u.outputChannel = outCh;

                if (!hasStored || std::abs(baseVol - chVol) > 1.0e-4f)
                    setUserVolume(i, baseVol);
            }
            else
            {
                float baseVol = 1.0f;
                bool hasStored = false;
                auto byNameIt = userVolumeByName.find(u.name);
                if (byNameIt != userVolumeByName.end())
                {
                    baseVol = byNameIt->second;
                    hasStored = true;
                }

                auto volIt = userBaseVolume.find(i);
                if (volIt != userBaseVolume.end())
                {
                    baseVol = volIt->second;
                    hasStored = true;
                }

                u.volume = baseVol;

                u.pan = 0.0f;
                u.isMuted = false;
                u.isSolo = false;
                u.outputChannel = ninjamClient.GetUserChannelOutput(i, 0);

                if (!hasStored)
                    setUserVolume(i, baseVol);
            }

            int linkOutputPair = -1;
            {
                const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
                auto linkIt = remoteLinkAudioOutputPairs.find(u.name);
                if (linkIt != remoteLinkAudioOutputPairs.end())
                    linkOutputPair = linkIt->second;
            }

            if (linkOutputPair >= 0)
            {
                const int desiredChannel = linkOutputPair * 2;
                if (u.outputChannel != desiredChannel)
                {
                    for (int ch = 0; ch < 32; ++ch)
                        ninjamClient.SetUserChannelState(i, ch, false, false, false, 0, false, 0, false, false, false, false, true, desiredChannel);
                }
                u.outputChannel = desiredChannel;
                u.outputUsesLinkAudio = true;
                usedPairsThisCall.insert(linkOutputPair);
            }
            else if (spread)
            {
                juce::String shortName = u.name;
                auto itAssign = userOutputAssignment.find(shortName);
                int desiredPair = -1;

                if (itAssign != userOutputAssignment.end())
                {
                    desiredPair = itAssign->second;
                }
                else
                {
                    if ((int)reservedPairs.size() < maxOutputPairs)
                    {
                        for (int cand = 0; cand < maxOutputPairs; ++cand)
                        {
                            if (!reservedPairs.count(cand))
                            {
                                desiredPair = cand;
                                reservedPairs.insert(cand);
                                break;
                            }
                        }
                    }
                    else
                    {
                        std::set<int> connectedNow = usedPairsThisCall;
                        int fallback = -1;
                        for (int cand = 0; cand < maxOutputPairs; ++cand)
                        {
                            if (!connectedNow.count(cand))
                            {
                                fallback = cand;
                                break;
                            }
                        }
                        if (fallback < 0)
                            fallback = 0;
                        desiredPair = fallback;
                    }

                    userOutputAssignment[shortName] = desiredPair;
                }

                if (desiredPair >= 0)
                {
                    int desiredChannel = desiredPair * 2;
                    if (u.outputChannel != desiredChannel)
                        setUserOutput(i, desiredChannel);
                    u.outputChannel = desiredChannel;
                    usedPairsThisCall.insert(desiredPair);
                }
            }

            userBaseVolume[i] = u.volume;
            userVolumeByName[u.name] = u.volume;

            // Look up multichannel state from the snapshot updated by refreshOpusSyncAvailabilityFromUsers().
            // This map is keyed by normalised username and never holds njclient locks.
            {
                const juce::String normName = canonicalDelayUserKey(u.name);
                const juce::ScopedLock mcLock(peerMultiChanLock);
                auto it = peerMultiChanByName.find(normName);
                if (it != peerMultiChanByName.end())
                {
                    u.isMultiChanPeer = it->second.isMultiChan;
                    if (u.isMultiChanPeer)
                        u.numChannels = juce::jmax(2, it->second.numChannels);
                }
            }

            // Populate channel names from NINJAM state (safe: no locks held here)
            if (u.isMultiChanPeer)
            {
                u.channelNames.clear();
                for (int ch = 0; ch < u.numChannels; ++ch)
                {
                    const char* chName = ninjamClient.GetUserChannelState(i, ch + 1); // ch0=mix, ch1..N=individual
                    if (chName != nullptr && *chName != '\0')
                        u.channelNames.add(juce::String::fromUTF8(chName));
                    else
                        u.channelNames.add("Ch " + juce::String(ch + 1));
                }
            }
            else
            {
                // Count basic NINJAM channel names for non-VST3 peers (display only, no expand button)
                u.channelNames.clear();
                for (int ch = 0; ch < 32; ++ch)
                {
                    const char* chName = ninjamClient.GetUserChannelState(i, ch);
                    if (chName != nullptr)
                    {
                        ++u.numChannels;
                        u.channelNames.add(juce::String::fromUTF8(chName));
                    }
                }
                if (u.numChannels < 1) { u.numChannels = 1; u.channelNames.add(""); }
            }

            if (i >= 0 && i < maxRemoteChordUsers)
            {
                if (remoteChordUserKeys[(size_t)i] != u.name)
                {
                    remoteChordUserKeys[(size_t)i] = u.name;
                    remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
                    auto& analyzer = remoteChordAnalyzers[(size_t)i];
                    if (analyzer && analyzer->isPrepared())
                        analyzer->markNoInput();
                }

                auto& analyzer = remoteChordAnalyzers[(size_t)i];
                if (analyzer && !analyzer->isPrepared())
                    analyzer->prepare(processingSampleRate);
            }

            users.push_back(u);
        }
    }

    for (int i = numUsers; i < maxRemoteChordUsers; ++i)
    {
        remoteChordUserKeys[(size_t)i].clear();
        remoteChordDetectionEnabled[(size_t)i].store(true, std::memory_order_relaxed);
        auto& analyzer = remoteChordAnalyzers[(size_t)i];
        if (analyzer && analyzer->isPrepared())
            analyzer->markNoInput();
    }

    for (auto it = remoteUserNameByIndex.begin(); it != remoteUserNameByIndex.end();)
    {
        const int userIndex = it->first;
        if (activeUserIndexes.count(userIndex) == 0)
        {
            userBaseVolume.erase(userIndex);
            userPanOverrides.erase(userIndex);
            userClipEnabled.erase(userIndex);
            it = remoteUserNameByIndex.erase(it);
        }
        else
        {
            ++it;
        }
    }

    return users;
}

void NinjamVst3AudioProcessor::rememberUserVolume(int userIndex, float volume, const juce::String& name)
{
    userBaseVolume[userIndex] = volume;
    juce::String shortName = name;
    int atPos = shortName.indexOfChar('@');
    if (atPos > 0)
        shortName = shortName.substring(0, atPos);
    userVolumeByName[shortName] = volume;
}

void NinjamVst3AudioProcessor::resetRemoteUserIndexState(int userIndex, const juce::String& userName)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    userBaseVolume.erase(userIndex);
    userPanOverrides.erase(userIndex);
    userClipEnabled.erase(userIndex);

    if (userIndex >= 0 && userIndex < maxRemoteChordUsers)
    {
        remoteChordDetectionEnabled[(size_t)userIndex].store(true, std::memory_order_relaxed);
        remoteChordUserKeys[(size_t)userIndex].clear();
        auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
        if (analyzer && analyzer->isPrepared())
            analyzer->markNoInput();
    }

    float baseVol = 1.0f;
    const juce::String shortName = makeShortUserName(userName);
    auto byNameIt = userVolumeByName.find(shortName);
    if (byNameIt != userVolumeByName.end())
        baseVol = byNameIt->second;

    for (int ch = 0; ch < 32; ++ch)
    {
        ninjamClient.SetUserChannelState(userIndex, ch,
                                         false, false,
                                         true, baseVol,
                                         true, 0.0f,
                                         true, false,
                                         true, false);
    }
}

void NinjamVst3AudioProcessor::setUserOutput(int userIndex, int outputChannelIndex)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    // Update all channels for this user to the new output
    // Iterate through all potential channels (MAX_USER_CHANNELS is 32)
    for (int i = 0; i < 32; ++i)
    {
        // SetUserChannelState arguments: useridx, channelidx, setsub, sub, setvol, vol, setpan, pan, setmute, mute, setsolo, solo, setoutch, outchannel
        ninjamClient.SetUserChannelState(userIndex, i, false, false, false, 0, false, 0, false, false, false, false, true, outputChannelIndex);
    }

    const char* name = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
    if (name)
    {
        const juce::String shortName = makeShortUserName(juce::String::fromUTF8(name));
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            remoteLinkAudioOutputPairs.erase(shortName);
            remoteLinkAudioSinks.erase(shortName);
        }
        int pairIndex = (outputChannelIndex & 1023) / 2;
        userOutputAssignment[shortName] = pairIndex;
    }

    lastLinkAudioEndpointRefreshMs = 0.0;
    rebuildLinkAudioEndpoints();
}

bool NinjamVst3AudioProcessor::setUserOutputToLinkAudio(int userIndex)
{
    if (!isLinkAudioEnabled() || !isLinkAudioSendEnabled())
        return false;

    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return false;

    const char* nameChars = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
    if (nameChars == nullptr || nameChars[0] == 0)
        return false;

    const juce::String shortName = makeShortUserName(juce::String::fromUTF8(nameChars));
    const int maxPairs = getTotalNumOutputChannels() / 2;
    if (maxPairs <= 0)
        return false;

    std::set<int> usedPairs;
    for (int otherUser = 0; otherUser < numUsers; ++otherUser)
    {
        if (otherUser == userIndex)
            continue;

        const char* otherNameChars = ninjamClient.GetUserState(otherUser, nullptr, nullptr, nullptr);
        if (otherNameChars == nullptr || otherNameChars[0] == 0)
            continue;

        const juce::String otherShortName = makeShortUserName(juce::String::fromUTF8(otherNameChars));
        int pairIndex = -1;
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            auto linkIt = remoteLinkAudioOutputPairs.find(otherShortName);
            if (linkIt != remoteLinkAudioOutputPairs.end())
                pairIndex = linkIt->second;
        }

        if (pairIndex < 0)
        {
            const int outChannel = ninjamClient.GetUserChannelOutput(otherUser, 0);
            pairIndex = (outChannel & 1023) / 2;
        }

        if (pairIndex >= 0 && pairIndex < maxPairs)
            usedPairs.insert(pairIndex);
    }

    int stagingPair = -1;
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        auto existing = remoteLinkAudioOutputPairs.find(shortName);
        if (existing != remoteLinkAudioOutputPairs.end())
            stagingPair = existing->second;
    }

    if (stagingPair < 0)
    {
        for (int candidate = maxPairs - 1; candidate >= 0; --candidate)
        {
            if (!usedPairs.count(candidate))
            {
                stagingPair = candidate;
                break;
            }
        }
    }

    if (stagingPair < 0)
        return false;

    for (int channel = 0; channel < 32; ++channel)
        ninjamClient.SetUserChannelState(userIndex, channel, false, false, false, 0, false, 0, false, false, false, false, true, stagingPair * 2);

    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        remoteLinkAudioOutputPairs[shortName] = stagingPair;
    }

    lastLinkAudioEndpointRefreshMs = 0.0;
    rebuildLinkAudioEndpoints();
    return true;
}

void NinjamVst3AudioProcessor::setUserLevel(int userIndex, float volume, float pan, bool isMuted, bool isSolo)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    userBaseVolume[userIndex] = volume;
    if (userIndex >= 0 && userIndex < numUsers)
    {
        const char* name = ninjamClient.GetUserState(userIndex, nullptr, nullptr, nullptr);
        if (name)
        {
            juce::String fullName = juce::String::fromUTF8(name);
            int atPos = fullName.indexOfChar('@');
            juce::String shortName;
            if (atPos > 0)
                shortName = fullName.substring(0, atPos);
            else
                shortName = fullName;
            userVolumeByName[shortName] = volume;
        }
    }
    userPanOverrides[userIndex] = pan;
    for (int i = 0; i < 32; ++i)
    {
        ninjamClient.SetUserChannelState(userIndex, i, false, false, true, volume, true, pan, true, isMuted, true, isSolo);
    }
}

void NinjamVst3AudioProcessor::setUserVolume(int userIndex, float volume)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return;

    for (int i = 0; i < 32; ++i)
    {
        ninjamClient.SetUserChannelState(userIndex, i, false, false, true, volume, false, 0, false, false, false, false, false, 0);
    }
}

float NinjamVst3AudioProcessor::getUserPeak(int userIndex, int channelIndex)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers)
        return 0.0f;

    if (isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load()))
        return 0.0f;

    float maxPeak = 0.0f;
    for (int i = 0; i < 32; ++i)
    {
        float p = ninjamClient.GetUserChannelPeak(userIndex, i, channelIndex);
        if (p > maxPeak) maxPeak = p;
    }
    return maxPeak;
}

float NinjamVst3AudioProcessor::getUserChannelPeak(int userIndex, int njChanIdx, int lrSide)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers || njChanIdx < 0)
        return 0.0f;

    return ninjamClient.GetUserChannelPeak(userIndex, njChanIdx, lrSide);
}

void NinjamVst3AudioProcessor::setUserNjChannelVolume(int userIndex, int njChanIdx, float volume)
{
    const int numUsers = ninjamClient.GetNumUsers();
    if (userIndex < 0 || userIndex >= numUsers || njChanIdx < 0)
        return;

    ninjamClient.SetUserChannelState(userIndex, njChanIdx, false, false, true, volume, false, 0, false, false, false, false);
}

void NinjamVst3AudioProcessor::setMasterOutputGain(float gain)
{
    masterOutputGain.store(gain);
}

float NinjamVst3AudioProcessor::getMasterOutputGain() const
{
    return masterOutputGain.load();
}

float NinjamVst3AudioProcessor::getMasterPeak() const
{
    return masterPeak.load();
}

float NinjamVst3AudioProcessor::getMasterPeakLeft() const
{
    return masterPeakL.load();
}

float NinjamVst3AudioProcessor::getMasterPeakRight() const
{
    return masterPeakR.load();
}

juce::String NinjamVst3AudioProcessor::getVersionString() const
{
    static juce::String cachedVersion;
    static bool cached = false;
    
    if (cached)
        return cachedVersion;
    
    juce::String version = NINJAM_DISPLAY_VERSION;

    auto runGit = [](const juce::String& command) -> juce::String
    {
        juce::ChildProcess cp;
        if (!cp.start(command + " 2>nul"))
            return {};
        return cp.readAllProcessOutput().trim();
    };

    const juce::String repoCheck = runGit("git rev-parse --is-inside-work-tree");
    if (repoCheck == "true")
    {
        const juce::String latestTag = runGit("git describe --tags --abbrev=0");
        if (latestTag.isNotEmpty())
            version = latestTag;

        bool appendPlus = runGit("git status --porcelain").isNotEmpty();

        if (!appendPlus)
        {
            if (latestTag.isNotEmpty())
            {
                const juce::String commitsSinceTag = runGit("git rev-list --count HEAD ^" + latestTag);
                appendPlus = commitsSinceTag.getIntValue() > 0;
            }
            else
            {
                // In a git repo without tags, mark build as post-release state.
                appendPlus = true;
            }
        }

        if (appendPlus)
            version += "+";
    }
    
    cachedVersion = version;
    cached = true;
    return cachedVersion;
}

void NinjamVst3AudioProcessor::setSoftLimiterEnabled(bool shouldEnable)
{
    softLimiterEnabled.store(shouldEnable);
}

bool NinjamVst3AudioProcessor::isSoftLimiterEnabled() const
{
    return softLimiterEnabled.load();
}

void NinjamVst3AudioProcessor::setUserClipEnabled(int userIndex, bool enabled)
{
    userClipEnabled[userIndex] = enabled;
}

bool NinjamVst3AudioProcessor::isUserClipEnabled(int userIndex) const
{
    auto it = userClipEnabled.find(userIndex);
    if (it != userClipEnabled.end())
        return it->second;

    return softLimiterEnabled.load();
}

void NinjamVst3AudioProcessor::setMasterLimiterEnabled(bool shouldEnable)
{
    dspLimiterEnabled.store(shouldEnable);
}

bool NinjamVst3AudioProcessor::isMasterLimiterEnabled() const
{
    return dspLimiterEnabled.load();
}

void NinjamVst3AudioProcessor::setLimiterThreshold(float db)
{
    limiterThresholdDb.store(db);
    masterLimiter.setThreshold(db);
}

void NinjamVst3AudioProcessor::setLimiterRelease(float ms)
{
    limiterReleaseMs.store(ms);
    masterLimiter.setRelease(ms);
}

void NinjamVst3AudioProcessor::setLocalInputGain(float gain)
{
    localInputGain.store(gain);
    setLocalChannelGain(0, gain);
}

float NinjamVst3AudioProcessor::getLocalInputGain() const
{
    return localChannelGains[0].load();
}

void NinjamVst3AudioProcessor::setNumLocalChannels(int num)
{
    const int previous = numLocalChannels.load();
    int clamped = juce::jlimit(1, maxLocalChannels, num);

    {
        juce::ScopedLock lock(localChannelNamesLock);
        for (int i = 0; i < maxLocalChannels; ++i)
        {
            auto& name = localChannelNames[(size_t)i];
            if (name.isEmpty() || isDefaultLocalChannelName(name))
                name = buildDefaultLocalChannelName(i);
        }
    }

    numLocalChannels.store(clamped);
    syncLocalIntervalChannelConfig();
    applyCodecPreference();

    // Post a local status message when transitioning into or out of multichannel
    if (previous != clamped)
    {
        juce::String msg;
        if (clamped > 1 && previous <= 1)
            msg = "MultiChannel mode enabled (" + juce::String(clamped) + " channels). Waiting for peer detection.";
        else if (clamped > 1)
            msg = "Local channels: " + juce::String(clamped) + ".";
        else
            msg = "MultiChannel mode disabled (single channel).";
        juce::ScopedLock lock(chatLock);
        chatHistory.add(msg);
        chatSenders.add("");
        chatRevision.fetch_add(1);
        if (chatHistory.size() > 100)
        {
            chatHistory.removeRange(0, chatHistory.size() - 100);
            chatSenders.removeRange(0, juce::jmax(0, chatSenders.size() - 100));
        }
    }

    // Immediately tell peers about the change so they update their expand buttons
    if (ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK)
        broadcastOpusSyncSupport();
}

int NinjamVst3AudioProcessor::getNumLocalChannels() const
{
    return numLocalChannels.load();
}

void NinjamVst3AudioProcessor::setLocalChannelName(int channel, const juce::String& name)
{
    if (channel < 0 || channel >= maxLocalChannels) return;
    { juce::ScopedLock lock(localChannelNamesLock); localChannelNames[(size_t)channel] = name; }
    syncLocalIntervalChannelConfig();
}

juce::String NinjamVst3AudioProcessor::getLocalChannelName(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels) return {};
    juce::ScopedLock lock(localChannelNamesLock);
    return localChannelNames[(size_t)channel];
}

void NinjamVst3AudioProcessor::setLocalChannelGain(int channel, float gain)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelGains[(size_t)channel].store(gain);
}

float NinjamVst3AudioProcessor::getLocalChannelGain(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 1.0f;
    return localChannelGains[(size_t)channel].load();
}

void NinjamVst3AudioProcessor::setLocalChannelInput(int channel, int inputIndex)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelInputs[(size_t)channel].store(inputIndex);
}

int NinjamVst3AudioProcessor::getLocalChannelInput(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0;
    return localChannelInputs[(size_t)channel].load();
}

void NinjamVst3AudioProcessor::setLocalChannelUsesLinkAudioInput(int channel, bool shouldUse)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;

    if (shouldUse)
        localChannelInputs[(size_t)channel].store(kLocalInputLinkAudioSentinel);
    else if (localChannelInputs[(size_t)channel].load() == kLocalInputLinkAudioSentinel)
        localChannelInputs[(size_t)channel].store(0);
}

bool NinjamVst3AudioProcessor::isLocalChannelUsingLinkAudioInput(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return false;
    return localChannelInputs[(size_t)channel].load() == kLocalInputLinkAudioSentinel;
}

float NinjamVst3AudioProcessor::getLocalChannelPeak(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelPeaks[(size_t)channel].load();
}

float NinjamVst3AudioProcessor::getLocalChannelPeakLeft(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelPeaksL[(size_t)channel].load();
}

float NinjamVst3AudioProcessor::getLocalChannelPeakRight(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelPeaksR[(size_t)channel].load();
}

juce::String NinjamVst3AudioProcessor::getLocalChordLabel() const
{
    if (!isChordDetectionEnabled())
        return "Off";

    return localChordAnalyzer ? localChordAnalyzer->getLabel() : "--";
}

double NinjamVst3AudioProcessor::getLocalChordCpuPercent() const
{
    if (!isChordDetectionEnabled())
        return 0.0;

    return localChordAnalyzer ? localChordAnalyzer->getCpuPercent() : 0.0;
}

int NinjamVst3AudioProcessor::getLocalChordMemoryKb() const
{
    return localChordAnalyzer ? localChordAnalyzer->getMemoryKb() : 0;
}

juce::String NinjamVst3AudioProcessor::getUserChordLabel(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return "--";

    if (!isChordDetectionEnabled() || !isUserChordDetectionEnabled(userIndex))
        return "Off";

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    return analyzer ? analyzer->getLabel() : "--";
}

double NinjamVst3AudioProcessor::getUserChordCpuPercent(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return 0.0;

    if (!isChordDetectionEnabled() || !isUserChordDetectionEnabled(userIndex))
        return 0.0;

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    return analyzer ? analyzer->getCpuPercent() : 0.0;
}

void NinjamVst3AudioProcessor::setChordDetectionEnabled(bool enabled)
{
    chordDetectionEnabled.store(enabled, std::memory_order_relaxed);

    if (!enabled)
    {
        if (localChordAnalyzer)
            localChordAnalyzer->markNoInput();

        for (auto& analyzer : remoteChordAnalyzers)
            if (analyzer && analyzer->isPrepared())
                analyzer->markNoInput();
    }
}

bool NinjamVst3AudioProcessor::isChordDetectionEnabled() const
{
    return chordDetectionEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setUserChordDetectionEnabled(int userIndex, bool enabled)
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return;

    remoteChordDetectionEnabled[(size_t)userIndex].store(enabled, std::memory_order_relaxed);

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    if (analyzer && analyzer->isPrepared())
        analyzer->markNoInput();
}

bool NinjamVst3AudioProcessor::isUserChordDetectionEnabled(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return false;

    return remoteChordDetectionEnabled[(size_t)userIndex].load(std::memory_order_relaxed);
}

int NinjamVst3AudioProcessor::getUserChordMemoryKb(int userIndex) const
{
    if (userIndex < 0 || userIndex >= maxRemoteChordUsers)
        return 0;

    const auto& analyzer = remoteChordAnalyzers[(size_t)userIndex];
    return analyzer ? analyzer->getMemoryKb() : 0;
}

void NinjamVst3AudioProcessor::RemoteChannelAudioTap_Callback(void* userData,
                                                              int useridx,
                                                              const char*,
                                                              int channelidx,
                                                              const float* interleaved,
                                                              int numChannels,
                                                              int numFrames,
                                                              int sampleRate)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    if (self == nullptr || interleaved == nullptr || numFrames <= 0)
        return;

    if (useridx < 0 || useridx >= maxRemoteChordUsers)
        return;

    // Channel 0 is the normal per-user mixdown, including the VST3 multichannel
    // mix slot. Tapping only this channel avoids double-feeding expanded peers.
    if (channelidx != 0)
        return;

    auto& analyzer = self->remoteChordAnalyzers[(size_t)useridx];
    if (analyzer == nullptr)
        return;

    if (!self->isChordDetectionEnabled() || !self->isUserChordDetectionEnabled(useridx))
    {
        analyzer->markNoInput();
        return;
    }

    analyzer->processInterleavedBlock(interleaved, numFrames, numChannels, sampleRate);
}

void NinjamVst3AudioProcessor::setLocalMonitorEnabled(bool enabled)
{
    localMonitorEnabled.store(enabled);
}

bool NinjamVst3AudioProcessor::isLocalMonitorEnabled() const
{
    return localMonitorEnabled.load();
}

void NinjamVst3AudioProcessor::setFxReverbEnabled(bool enabled)
{
    fxReverbEnabled.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxReverbEnabled() const
{
    return fxReverbEnabled.load();
}

void NinjamVst3AudioProcessor::setFxDelayEnabled(bool enabled)
{
    fxDelayEnabled.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxDelayEnabled() const
{
    return fxDelayEnabled.load();
}

void NinjamVst3AudioProcessor::setFxReverbRoomSize(float roomSize)
{
    fxReverbRoomSize.store(juce::jlimit(0.0f, 1.0f, roomSize));
}

float NinjamVst3AudioProcessor::getFxReverbRoomSize() const
{
    return fxReverbRoomSize.load();
}

void NinjamVst3AudioProcessor::setFxReverbDamping(float damping)
{
    fxReverbDamping.store(juce::jlimit(0.0f, 1.0f, damping));
}

float NinjamVst3AudioProcessor::getFxReverbDamping() const
{
    return fxReverbDamping.load();
}

void NinjamVst3AudioProcessor::setFxReverbWetDryMix(float wetDryMix)
{
    fxReverbWetDryMix.store(juce::jlimit(0.0f, 1.0f, wetDryMix));
}

float NinjamVst3AudioProcessor::getFxReverbWetDryMix() const
{
    return fxReverbWetDryMix.load();
}

void NinjamVst3AudioProcessor::setFxReverbEarlyReflections(float earlyReflections)
{
    fxReverbEarlyReflections.store(juce::jlimit(0.0f, 1.0f, earlyReflections));
}

float NinjamVst3AudioProcessor::getFxReverbEarlyReflections() const
{
    return fxReverbEarlyReflections.load();
}

void NinjamVst3AudioProcessor::setFxReverbTail(float tail)
{
    fxReverbTail.store(juce::jlimit(0.0f, 1.0f, tail));
}

float NinjamVst3AudioProcessor::getFxReverbTail() const
{
    return fxReverbTail.load();
}

void NinjamVst3AudioProcessor::setFxDelayTimeMs(float timeMs)
{
    fxDelayTimeMs.store(juce::jlimit(20.0f, 2000.0f, timeMs));
}

float NinjamVst3AudioProcessor::getFxDelayTimeMs() const
{
    return fxDelayTimeMs.load();
}

void NinjamVst3AudioProcessor::setFxDelaySyncToHost(bool enabled)
{
    fxDelaySyncToHost.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxDelaySyncToHost() const
{
    return fxDelaySyncToHost.load();
}

void NinjamVst3AudioProcessor::setFxDelayDivision(int division)
{
    if (division != 1 && division != 8 && division != 16)
        division = 8;
    fxDelayDivision.store(division);
}

int NinjamVst3AudioProcessor::getFxDelayDivision() const
{
    return fxDelayDivision.load();
}

void NinjamVst3AudioProcessor::setFxDelayPingPong(bool enabled)
{
    fxDelayPingPong.store(enabled);
}

bool NinjamVst3AudioProcessor::isFxDelayPingPong() const
{
    return fxDelayPingPong.load();
}

void NinjamVst3AudioProcessor::setFxDelayWetDryMix(float wetDryMix)
{
    fxDelayWetDryMix.store(juce::jlimit(0.0f, 1.0f, wetDryMix));
}

float NinjamVst3AudioProcessor::getFxDelayWetDryMix() const
{
    return fxDelayWetDryMix.load();
}

void NinjamVst3AudioProcessor::setFxDelayFeedback(float feedback)
{
    fxDelayFeedback.store(juce::jlimit(0.0f, 0.95f, feedback));
}

float NinjamVst3AudioProcessor::getFxDelayFeedback() const
{
    return fxDelayFeedback.load();
}

void NinjamVst3AudioProcessor::setLocalChannelReverbSend(int channel, float send)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelReverbSends[(size_t)channel].store(juce::jlimit(0.0f, 1.0f, send));
}

float NinjamVst3AudioProcessor::getLocalChannelReverbSend(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelReverbSends[(size_t)channel].load();
}

void NinjamVst3AudioProcessor::setLocalChannelDelaySend(int channel, float send)
{
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    localChannelDelaySends[(size_t)channel].store(juce::jlimit(0.0f, 1.0f, send));
}

float NinjamVst3AudioProcessor::getLocalChannelDelaySend(int channel) const
{
    if (channel < 0 || channel >= maxLocalChannels)
        return 0.0f;
    return localChannelDelaySends[(size_t)channel].load();
}

int NinjamVst3AudioProcessor::getBPI()
{
    return ninjamClient.GetBPI();
}

float NinjamVst3AudioProcessor::getIntervalProgress()
{
    if (isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load()))
        return 0.0f;

    int pos = 0;
    int length = 0;
    ninjamClient.GetPosition(&pos, &length);
    if (length > 0)
    {
        if (isTransportSyncEnabled() && hostWasPlaying.load())
        {
            int basePos = syncDisplayPositionOffset.load();
            int relativePos = pos - basePos;
            if (relativePos < 0)
                relativePos += length;
            return (float)relativePos / (float)length;
        }
        return (float)pos / (float)length;
    }
    return 0.0f;
}

float NinjamVst3AudioProcessor::getBPM()
{
    return ninjamClient.GetActualBPM();
}

int NinjamVst3AudioProcessor::getIntervalIndex() const
{
    return getDisplayIntervalIndex();
}

float NinjamVst3AudioProcessor::getLocalPeak() const
{
    return localPeak.load();
}

float NinjamVst3AudioProcessor::getLocalPeakLeft() const
{
    return localPeakL.load();
}

float NinjamVst3AudioProcessor::getLocalPeakRight() const
{
    return localPeakR.load();
}

void NinjamVst3AudioProcessor::sendSideSignal(const juce::String& target, const juce::String& type, const juce::String& payload)
{
    const char* tgt = target.isNotEmpty() ? target.toRawUTF8() : "*";
    ninjamClient.ChatMessage_Send("SIDE_SIGNAL", tgt, type.toRawUTF8(), payload.toRawUTF8());
}

void NinjamVst3AudioProcessor::sendIntervalSignal(const juce::String& type, const juce::String& payload)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK) return;

    // Wrap in {"sig":type, "data":payload} so the receiver knows the type
    juce::DynamicObject::Ptr wrapper = new juce::DynamicObject();
    wrapper->setProperty("sig", type);
    wrapper->setProperty("data", payload);
    const juce::String msg = juce::JSON::toString(juce::var(wrapper.get()));
    // Custom FOURCC interval items stay hidden from normal chat clients.
    ninjamClient.SendRawIntervalItem(kSyncSignalChannelIndex, kSyncSignalFourcc, msg.toRawUTF8(), (int)msg.getNumBytesAsUTF8());
}

void NinjamVst3AudioProcessor::setSpreadOutputsEnabled(bool shouldEnable)
{
    bool wasEnabled = spreadOutputsEnabled.load();
    spreadOutputsEnabled.store(shouldEnable);

    if (wasEnabled && !shouldEnable)
    {
        userOutputAssignment.clear();
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            remoteLinkAudioOutputPairs.clear();
            remoteLinkAudioSinks.clear();
        }

        int numUsers = ninjamClient.GetNumUsers();
        for (int userIdx = 0; userIdx < numUsers; ++userIdx)
        {
            for (int ch = 0; ch < 32; ++ch)
            {
                ninjamClient.SetUserChannelState(userIdx, ch,
                                                 false, false,
                                                 false, 0.0f,
                                                 false, 0.0f,
                                                 false, false,
                                                 false, false,
                                                 true, 0);
            }
        }
    }
}

bool NinjamVst3AudioProcessor::isSpreadOutputsEnabled() const
{
    return spreadOutputsEnabled.load();
}

int NinjamVst3AudioProcessor::getCodecMode() const
{
    const bool multiChanAuto = numLocalChannels.load() > 1 && opusSyncAvailable.load();
    if (!multiChanAuto)
        return 0;
    // multiChanAuto: always mixed mode (Vorbis ch0 + Opus ch1..N)
    return 1;
}

unsigned int NinjamVst3AudioProcessor::getVorbisMask() const
{
    return ninjamClient.GetCodecVorbisMask();
}

unsigned int NinjamVst3AudioProcessor::getOpusMask() const
{
    return ninjamClient.GetCodecOpusMask();
}

juce::String NinjamVst3AudioProcessor::translateText(const juce::String& text)
{
    juce::String targetCode;
    {
        juce::ScopedLock lock(chatLock);
        if (!autoTranslate)
            return text;

        targetCode = translateTargetLang.isNotEmpty() ? translateTargetLang : "en";
    }

    targetCode = resolveTranslateTargetLanguageCode(targetCode);
    if (targetCode.isEmpty())
        targetCode = "en";
    return translateTextForTarget(text, targetCode);
}

juce::String NinjamVst3AudioProcessor::translateTextForTarget(const juce::String& text, const juce::String& targetCode)
{
    auto fail = [this, &text](const juce::String& reason)
    {
        noteTranslationFailure(reason);
        return text;
    };

    if (text.trim().isEmpty())
        return text;

    juce::String translatedText;
    juce::String primaryError;
    if (tryTranslateWithFedilab(text, targetCode, translatedText, primaryError))
    {
        clearTranslationFailureState();
        return translatedText;
    }

    juce::String fallbackError;
    if (tryTranslateWithGoogleFallback(text, targetCode, translatedText, fallbackError))
    {
        clearTranslationFailureState();
        return translatedText;
    }

    juce::String combinedError = primaryError;
    if (combinedError.isNotEmpty() && fallbackError.isNotEmpty())
        combinedError << "; fallback: " << fallbackError;
    else if (combinedError.isEmpty())
        combinedError = fallbackError;

    if (combinedError.isEmpty())
        combinedError = "all translation services failed";

    return fail(combinedError);
}

void NinjamVst3AudioProcessor::enqueueAsyncTranslation(const juce::String& originalLine,
                                                       const juce::String& lineSender,
                                                       const juce::String& linePrefix,
                                                       const juce::String& lineBody)
{
    if (lineBody.trim().isEmpty() || asyncChatTranslationWorker == nullptr)
        return;

    juce::String preferredTarget;
    juce::uint64 configRevision = 0;
    {
        const juce::ScopedLock lock(chatLock);
        if (!autoTranslate)
            return;

        preferredTarget = translateTargetLang.isNotEmpty() ? translateTargetLang : "system";
        configRevision = translationConfigRevision.load(std::memory_order_relaxed);
    }

    juce::String targetCode = resolveTranslateTargetLanguageCode(preferredTarget);
    if (targetCode.isEmpty())
        targetCode = "en";

    AsyncChatTranslationWorker::Request request;
    request.originalLine = originalLine;
    request.lineSender = lineSender;
    request.linePrefix = linePrefix;
    request.lineBody = lineBody;
    request.targetCode = targetCode;
    request.configRevision = configRevision;
    asyncChatTranslationWorker->enqueue(std::move(request));
}

void NinjamVst3AudioProcessor::applyAsyncTranslatedChatLine(const juce::String& originalLine,
                                                            const juce::String& lineSender,
                                                            const juce::String& translatedLine,
                                                            juce::uint64 configRevision)
{
    const juce::ScopedLock lock(chatLock);
    if (!autoTranslate || configRevision != translationConfigRevision.load(std::memory_order_relaxed))
        return;

    for (int i = chatHistory.size(); --i >= 0;)
    {
        if (chatSenders[i] == lineSender && chatHistory[i] == originalLine)
        {
            if (chatHistory[i] != translatedLine)
            {
                chatHistory.set(i, translatedLine);
                chatRevision.fetch_add(1);
            }
            return;
        }
    }
}

std::vector<NinjamVst3AudioProcessor::PublicServerInfo> NinjamVst3AudioProcessor::getPublicServers() const
{
    std::vector<PublicServerInfo> copy;
    const juce::ScopedLock lock(serverListLock);
    copy = publicServers;
    return copy;
}

void NinjamVst3AudioProcessor::refreshPublicServers()
{
    std::vector<PublicServerInfo> result;

#if defined(_WIN32)
    const wchar_t* host = L"ninbot.com";
    const wchar_t* path = L"/app/servers.php";

    HINTERNET hSession = WinHttpOpen(L"NINJAMVST3/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession)
        return;

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                                            L"GET",
                                            path,
                                            NULL,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    BOOL ok = WinHttpSendRequest(hRequest,
                                 WINHTTP_NO_ADDITIONAL_HEADERS,
                                 0,
                                 WINHTTP_NO_REQUEST_DATA,
                                 0,
                                 0,
                                 0);
    if (!ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    ok = WinHttpReceiveResponse(hRequest, NULL);
    if (!ok)
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return;
    }

    std::string response;
    DWORD dwSize = 0;
    do
    {
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0)
            break;

        std::string chunk;
        chunk.resize(dwSize);
        DWORD dwDownloaded = 0;
        if (!WinHttpReadData(hRequest, &chunk[0], dwSize, &dwDownloaded) || dwDownloaded == 0)
            break;

        response.append(chunk.data(), dwDownloaded);
    }
    while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (response.empty())
        return;

    juce::String jsonText = juce::String::fromUTF8(response.c_str(), (int)response.size());

    juce::var root;
    juce::Result parseError = juce::JSON::parse(jsonText, root);
    if (parseError.failed() || !root.isObject())
        return;

    auto* rootObj = root.getDynamicObject();
    if (!rootObj)
        return;

    juce::var serversVar = rootObj->getProperty("servers");
    if (!serversVar.isArray())
        return;

    auto* serversArray = serversVar.getArray();
    if (!serversArray)
        return;

    for (auto& serverVar : *serversArray)
    {
        if (!serverVar.isObject())
            continue;
        auto* obj = serverVar.getDynamicObject();
        if (!obj)
            continue;

        PublicServerInfo info;
        juce::String nameText = obj->getProperty("name").toString();
        info.name = nameText;

        int colon = nameText.lastIndexOfChar(':');
        if (colon > 0)
        {
            info.host = nameText.substring(0, colon);
            info.port = nameText.substring(colon + 1).getIntValue();
        }
        else
        {
            info.host = nameText;
            info.port = 2049;
        }

        info.bpi = obj->getProperty("bpi").toString().getIntValue();
        info.bpm = (float)obj->getProperty("bpm").toString().getDoubleValue();

        juce::var usersVar = obj->getProperty("users");
        if (usersVar.isArray() && usersVar.getArray() != nullptr)
            info.userCount = usersVar.getArray()->size();
        else
            info.userCount = obj->getProperty("user_count").toString().getIntValue();

        info.userMax = obj->getProperty("user_max").toString().getIntValue();
        result.push_back(info);
    }
#else
    JNL_HTTPGet request(JNL_CONNECTION_AUTODNS, 16384, nullptr);
    request.addheader("User-Agent: NINJAMplus/1.0");
    request.addheader("Accept: application/json,*/*");
    request.connect("http://ninbot.com/app/servers.php");

    juce::MemoryOutputStream response;
    const double deadlineMs = juce::Time::getMillisecondCounterHiRes() + 8000.0;
    char buffer[4096] = {};

    for (;;)
    {
        const int runResult = request.run();

        for (;;)
        {
            const int available = request.bytes_available();
            if (available <= 0)
                break;

            const int bytesToRead = juce::jmin(available, (int)sizeof(buffer));
            const int bytesRead = request.get_bytes(buffer, bytesToRead);
            if (bytesRead <= 0)
                break;

            response.write(buffer, (size_t)bytesRead);
            if (response.getDataSize() > 4 * 1024 * 1024)
                return;
        }

        if (runResult == -1)
            return;
        if (runResult == 1)
            break;
        if (juce::Time::getMillisecondCounterHiRes() >= deadlineMs)
            return;

        juce::Thread::sleep(10);
    }

    const int replyCode = request.getreplycode();
    if (replyCode != 0 && (replyCode < 200 || replyCode >= 300))
        return;
    if (response.getDataSize() == 0)
        return;

    juce::String jsonText = juce::String::fromUTF8(static_cast<const char*>(response.getData()),
                                                   (int)response.getDataSize());

    juce::var root;
    juce::Result parseError = juce::JSON::parse(jsonText, root);
    if (parseError.failed() || !root.isObject())
        return;

    auto* rootObj = root.getDynamicObject();
    if (rootObj == nullptr)
        return;

    juce::var serversVar = rootObj->getProperty("servers");
    if (!serversVar.isArray())
        return;

    auto* serversArray = serversVar.getArray();
    if (serversArray == nullptr)
        return;

    for (auto& serverVar : *serversArray)
    {
        if (!serverVar.isObject())
            continue;
        auto* obj = serverVar.getDynamicObject();
        if (obj == nullptr)
            continue;

        PublicServerInfo info;
        juce::String nameText = obj->getProperty("name").toString();
        info.name = nameText;

        int colon = nameText.lastIndexOfChar(':');
        if (colon > 0)
        {
            info.host = nameText.substring(0, colon);
            info.port = nameText.substring(colon + 1).getIntValue();
        }
        else
        {
            info.host = nameText;
            info.port = 2049;
        }

        info.bpi = obj->getProperty("bpi").toString().getIntValue();
        info.bpm = (float)obj->getProperty("bpm").toString().getDoubleValue();

        juce::var usersVar = obj->getProperty("users");
        if (usersVar.isArray() && usersVar.getArray() != nullptr)
            info.userCount = usersVar.getArray()->size();
        else
            info.userCount = obj->getProperty("user_count").toString().getIntValue();

        info.userMax = obj->getProperty("user_max").toString().getIntValue();
        result.push_back(info);
    }
#endif

    const juce::ScopedLock lock(serverListLock);
    publicServers.swap(result);
}

NinjamVst3AudioProcessor::~NinjamVst3AudioProcessor()
{
    stopTimer();
    ninjamClient.RemoteChannelAudioTap = nullptr;
    ninjamClient.RemoteChannelAudioTap_User = nullptr;
    localChordAnalyzer.reset();
    for (auto& analyzer : remoteChordAnalyzers)
        analyzer.reset();
    if (asyncChatTranslationWorker)
        asyncChatTranslationWorker->stop();
    asyncChatTranslationWorker.reset();
    {
        const juce::ScopedLock launchLock(videoLaunchWorkerLock);
        if (videoLaunchFuture.valid())
            videoLaunchFuture.wait();
    }
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        abletonLinkSource.reset();
        abletonLinkSink.reset();
    }
    abletonLink.reset();
    if (serverLatencyProbeFuture.valid())
        serverLatencyProbeFuture.wait();
    stopAdvancedVideoClient();
    ninjamClient.Disconnect();
    JNL::close_socketlib();
}

const juce::String NinjamVst3AudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NinjamVst3AudioProcessor::acceptsMidi() const
{
    return true;
}

bool NinjamVst3AudioProcessor::producesMidi() const
{
    return true;
}

bool NinjamVst3AudioProcessor::isMidiEffect() const
{
    return false;
}

double NinjamVst3AudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NinjamVst3AudioProcessor::getNumPrograms()
{
    return 1;
}

int NinjamVst3AudioProcessor::getCurrentProgram()
{
    return 0;
}

void NinjamVst3AudioProcessor::setCurrentProgram (int index)
{
}

const juce::String NinjamVst3AudioProcessor::getProgramName (int index)
{
    return {};
}

void NinjamVst3AudioProcessor::changeProgramName (int index, const juce::String& newName)
{
}

void NinjamVst3AudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    intervalSyncSampleCounter.store(0, std::memory_order_relaxed);
    processingSampleRate = sampleRate > 1.0 ? sampleRate : 44100.0;
    if (linkTimingState != nullptr)
        linkTimingState->reset();
    if (localChordAnalyzer)
        localChordAnalyzer->prepare(processingSampleRate);
    for (auto& analyzer : remoteChordAnalyzers)
        if (analyzer && analyzer->isPrepared())
            analyzer->prepare(processingSampleRate);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (juce::uint32) samplesPerBlock;
    spec.numChannels = (juce::uint32) getTotalNumOutputChannels();
    masterLimiter.prepare(spec);
    masterLimiter.setThreshold(limiterThresholdDb.load());
    masterLimiter.setRelease(limiterReleaseMs.load());
    masterLimiter.reset();

    fxReverb.reset();
    juce::Reverb::Parameters params;
    params.roomSize = fxReverbRoomSize.load();
    params.damping = 0.45f;
    params.width = 1.0f;
    params.wetLevel = 0.35f;
    params.dryLevel = 0.0f;
    params.freezeMode = 0.0f;
    fxReverb.setParameters(params);

    const int maxDelaySamples = juce::jmax(1, (int)std::ceil(processingSampleRate * 2.5));
    fxDelayBuffer.setSize(2, maxDelaySamples, false, true, true);
    fxDelayBuffer.clear();
    fxDelayWritePosition = 0;

    fxReverbInputBuffer.setSize(1, juce::jmax(1, samplesPerBlock), false, true, true);
    fxDelayInputBuffer.setSize(1, juce::jmax(1, samplesPerBlock), false, true, true);
    fxReturnBuffer.setSize(2, juce::jmax(1, samplesPerBlock), false, true, true);

    linkAudioMaxNumSamples = (size_t) juce::jmax(8192, samplesPerBlock * 2);
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        if (abletonLinkSink != nullptr)
            abletonLinkSink->requestMaxNumSamples(linkAudioMaxNumSamples);
    }
}

void NinjamVst3AudioProcessor::releaseResources()
{
    if (linkTimingState != nullptr)
        linkTimingState->reset();
}

bool NinjamVst3AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    auto mainIn = layouts.getMainInputChannelSet();
    if (!mainIn.isDisabled()
        && mainIn != juce::AudioChannelSet::stereo()
        && mainIn != juce::AudioChannelSet::mono())
        return false;

    for (int i = 1; i < layouts.inputBuses.size(); ++i)
    {
        if (!layouts.inputBuses[i].isDisabled() && layouts.inputBuses[i] != juce::AudioChannelSet::stereo())
            return false;
    }

    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        if (!layouts.outputBuses[i].isDisabled() && layouts.outputBuses[i] != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

int NinjamVst3AudioProcessor::LicenseAgreementCallback(void* userData, const char* licensetext)
{
    // Auto-accept license for now (or log it)
    // Ideally, show a dialog to the user
    // Since this is called from Run(), which we call from timerCallback (UI thread),
    // we can show a message box.
    // However, for automation/testing, we might want to auto-accept.
    
    // Simple auto-accept for this proof of concept:
    juce::Logger::writeToLog("License Agreement Requested: " + juce::String(licensetext));
    return 1; 
}

void NinjamVst3AudioProcessor::processSyncSignal(const juce::String& sender, const juce::String& type, const juce::String& payload)
{
    if (type == "intervalLatencyReport")
        return;
    if (type == "intervalTransportProbe")
    {
        juce::String payloadUserId;
        juce::String probeId;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("probeId"))
                probeId = obj->getProperty("probeId").toString();
        }
        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        if (probeId.isEmpty() || sender.isEmpty() || senderKey.isEmpty())
            return;
        return;
    }
    if (type == "intervalTransportProbeAck")
    {
        return;
    }
    if (type == "midiRelay")
    {
        juce::String payloadUserId;
        MidiControllerEvent event;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId")) payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("isController")) event.isController = (bool)obj->getProperty("isController");
            if (obj->hasProperty("midiChannel")) event.midiChannel = (int)obj->getProperty("midiChannel");
            if (obj->hasProperty("number")) event.number = (int)obj->getProperty("number");
            if (obj->hasProperty("value")) event.value = (int)obj->getProperty("value");
            if (obj->hasProperty("normalized")) event.normalized = (float)(double)obj->getProperty("normalized");
            if (obj->hasProperty("isNoteOn")) event.isNoteOn = (bool)obj->getProperty("isNoteOn");
        }

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey)
            return;

        event.midiChannel = juce::jlimit(1, 16, event.midiChannel);
        event.number = juce::jlimit(0, 127, event.number);
        event.value = juce::jlimit(0, 127, event.value);
        event.normalized = juce::jlimit(0.0f, 1.0f, event.normalized);

        bool acceptForLearn = false;
        const juce::String learnSource = getMidiLearnInputDeviceId();
        if (learnSource == "__learn_relay__" || learnSource == "__learn_relay__:*")
        {
            acceptForLearn = true;
        }
        else if (learnSource.startsWith("__learn_relay__:"))
        {
            const juce::String desired = learnSource.fromFirstOccurrenceOf("__learn_relay__:", false, false).trim();
            if (desired.isEmpty() || desired == "*")
                acceptForLearn = true;
            else
                acceptForLearn = normaliseOpusPeerId(desired) == senderKey;
        }

        if (acceptForLearn)
        {
            const juce::SpinLock::ScopedLockType learnLock(midiEventQueueLock);
            pendingMidiControllerEvents.push_back(event);
            if (pendingMidiControllerEvents.size() > 512)
                pendingMidiControllerEvents.erase(pendingMidiControllerEvents.begin(), pendingMidiControllerEvents.begin() + (long long)(pendingMidiControllerEvents.size() - 512));
        }

        const juce::SpinLock::ScopedLockType lock(inboundMidiRelayQueueLock);
        pendingInboundMidiRelayEvents.push_back(event);
        if (pendingInboundMidiRelayEvents.size() > 512)
            pendingInboundMidiRelayEvents.erase(pendingInboundMidiRelayEvents.begin(), pendingInboundMidiRelayEvents.begin() + (long long)(pendingInboundMidiRelayEvents.size() - 512));
        return;
    }
    if (type == "oscRelay")
    {
        juce::String payloadUserId;
        OscRelayEvent event;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("userId")) payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("address")) event.address = obj->getProperty("address").toString();
            if (obj->hasProperty("normalized")) event.normalized = (float)(double)obj->getProperty("normalized");
            if (obj->hasProperty("binaryOn")) event.binaryOn = (bool)obj->getProperty("binaryOn");
        }

        const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
        const juce::String localUserKey = normaliseOpusPeerId(currentUser);
        if (senderKey.isEmpty() || senderKey == localUserKey)
            return;

        event.senderKey = senderKey;
        event.normalized = juce::jlimit(0.0f, 1.0f, event.normalized);
        if (event.address.isEmpty())
            return;

        const juce::SpinLock::ScopedLockType lock(inboundOscRelayQueueLock);
        pendingInboundOscRelayEvents.push_back(event);
        if (pendingInboundOscRelayEvents.size() > 512)
            pendingInboundOscRelayEvents.erase(pendingInboundOscRelayEvents.begin(), pendingInboundOscRelayEvents.begin() + (long long)(pendingInboundOscRelayEvents.size() - 512));
        return;
    }
    if (type == "localInputSelect")
    {
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            const int channel = obj->hasProperty("channel") ? (int)obj->getProperty("channel") : -1;
            const int inputIndex = obj->hasProperty("inputIndex") ? (int)obj->getProperty("inputIndex") : 0;
            if (channel >= 0 && channel < maxLocalChannels)
                setLocalChannelInput(channel, inputIndex);
        }
        return;
    }
    if (type == "intervalSyncTag")
    {
        juce::String tag;
        juce::String payloadUserId;
        int remoteInterval = -1;
        int remoteIntervalAbsolute = -1;
        int remoteServerLatencyMs = -1;
        int remoteBpi = 0;
        int remoteBeat = -1;
        const juce::var parsed = juce::JSON::parse(payload);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (obj->hasProperty("tag"))
                tag = obj->getProperty("tag").toString();
            if (obj->hasProperty("userId"))
                payloadUserId = obj->getProperty("userId").toString();
            if (obj->hasProperty("intervalIndex"))
                remoteInterval = (int)obj->getProperty("intervalIndex");
            if (obj->hasProperty("intervalAbsolute"))
                remoteIntervalAbsolute = (int)obj->getProperty("intervalAbsolute");
            if (obj->hasProperty("serverLatencyMs"))
                remoteServerLatencyMs = (int)obj->getProperty("serverLatencyMs");
            if (obj->hasProperty("bpi"))
                remoteBpi = (int)obj->getProperty("bpi");
            if (obj->hasProperty("beatIndex"))
                remoteBeat = (int)obj->getProperty("beatIndex");
        }
        const int localInterval = getIntervalIndex();
        juce::String status = "Interval Tag " + sender;
        if (remoteInterval >= 0)
        {
            const int delta = remoteInterval - localInterval;
            status << " remoteInt " << juce::String(remoteInterval)
                   << " localInt " << juce::String(localInterval)
                   << " d=" << juce::String(delta);
        }
        if (remoteBeat >= 0 && remoteBpi > 0)
            status << " beat " << juce::String(remoteBeat + 1) << "/" << juce::String(remoteBpi);
        if (tag.isNotEmpty())
            status << " tag " << tag;
        setIntervalSyncStatusText(status);

        const int localBpi = juce::jmax(1, getBPI());
        const int markerBpi = remoteBpi > 0 ? remoteBpi : localBpi;
        if (remoteInterval >= 0 && isIntervalSyncMarkerBeat(remoteBeat, markerBpi))
        {
            const juce::String localUserKey = normaliseOpusPeerId(currentUser);
            const juce::String senderKey = normaliseOpusPeerId(payloadUserId.isNotEmpty() ? payloadUserId : sender);
            if (senderKey.isNotEmpty() && senderKey != localUserKey)
            {
                const bool bpiMatches = (remoteBpi <= 0 || remoteBpi == localBpi);
                if (!bpiMatches)
                    return;
                const int remoteMarkerBeat = getIntervalSyncMarkerBeatForBeat(remoteBeat, localBpi);
                const int remoteSourceInterval = remoteIntervalAbsolute >= 0 ? remoteIntervalAbsolute : remoteInterval;
                const long long remoteMarkerKey = makeIntervalSyncMarkerKey(remoteSourceInterval, remoteMarkerBeat);
                bool shouldStorePending = false;
                const juce::String displaySender = sender.isNotEmpty() ? sender : (payloadUserId.isNotEmpty() ? payloadUserId : senderKey);
                const long long receivedSampleCount = intervalSyncSampleCounter.load(std::memory_order_relaxed);
                const double receivedAtMs = juce::Time::getMillisecondCounterHiRes();
                {
                    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                    const int clampedRemoteServerLatencyMs = remoteServerLatencyMs >= 0 ? juce::jlimit(0, 3000, remoteServerLatencyMs) : -1;
                    if (clampedRemoteServerLatencyMs >= 0)
                    {
                        lastRemoteServerLatencyMsByUser[senderKey] = clampedRemoteServerLatencyMs;
                        const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
                        if (canonicalSenderKey.isNotEmpty())
                            lastRemoteServerLatencyMsByUser[canonicalSenderKey] = clampedRemoteServerLatencyMs;
                    }
                    auto it = lastAnnouncedRemoteIntervalByUser.find(senderKey);
                    if (it != lastAnnouncedRemoteIntervalByUser.end() && remoteMarkerKey + intervalSyncMarkerKeyBeatStride < it->second)
                        remoteLatencyAverageByUser.erase(senderKey);
                    if (it == lastAnnouncedRemoteIntervalByUser.end() || it->second != remoteMarkerKey)
                    {
                        lastAnnouncedRemoteIntervalByUser[senderKey] = remoteMarkerKey;
                        shouldStorePending = true;
                    }
                }

                if (shouldStorePending)
                {
                    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                    const juce::String pendingKey = senderKey + ":" + juce::String((juce::int64)remoteMarkerKey);
                    auto& pending = pendingRemoteIntervalStartsByUser[pendingKey];
                    pending.remoteInterval = remoteInterval;
                    pending.remoteIntervalAbsolute = remoteIntervalAbsolute;
                    pending.remoteBeat = remoteMarkerBeat;
                    pending.remoteBpi = remoteBpi;
                    pending.remoteServerLatencyMs = remoteServerLatencyMs >= 0 ? juce::jlimit(0, 3000, remoteServerLatencyMs) : -1;
                    pending.senderKey = senderKey;
                    pending.displaySender = displaySender;
                    pending.receivedSampleCount = receivedSampleCount;
                    pending.receivedAtMs = receivedAtMs;
                    vlogStr("[INTTAG] Pending stored from=" + sender + " userId=" + (payloadUserId.isNotEmpty() ? payloadUserId : sender) + " senderKey=" + senderKey + " remoteInterval=" + juce::String(remoteInterval) + " remoteAbs=" + juce::String(remoteIntervalAbsolute) + " beat=" + juce::String(remoteMarkerBeat) + " samples=" + juce::String(receivedSampleCount) + " receivedAtMs=" + juce::String(receivedAtMs, 2));
                }
            }
        }
        return;
    }
}

void NinjamVst3AudioProcessor::ChatMessage_Callback(void* userData, NJClient* inst, const char** parms, int nparms)
{
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    auto processOpusSyncSupport = [self](const juce::String& sender, const juce::String& payload, juce::String* outEventId) -> bool
    {
        juce::Logger::writeToLog("processOpusSyncSupport from sender='" + sender + "'");
        vlogStr("processOpusSyncSupport from sender='" + sender + "' payload=" + payload.substring(0, 80));
        juce::var parsed = juce::JSON::parse(payload);
        bool supportsOpus = false;
        bool multiChanEnabled = false;
        int peerNumChannels = 1;
        juce::String userId = normaliseOpusPeerId(sender);
        juce::String clientId;
        juce::String appFamily;
        int handshakeVersion = 0;
        juce::String runtimeFormat;
        juce::String pluginVersion;
        if (auto* obj = parsed.getDynamicObject())
        {
            const juce::String supports = obj->getProperty("supportsOpus").toString();
            supportsOpus = supports == "1" || supports.equalsIgnoreCase("true");
            const juce::String enabledStr = obj->getProperty("enabled").toString();
            multiChanEnabled = enabledStr == "1" || enabledStr.equalsIgnoreCase("true");
            const juce::var numChVar = obj->getProperty("numChannels");
            if (!numChVar.isVoid()) peerNumChannels = juce::jmax(1, (int)numChVar);
            juce::String payloadUserId = obj->getProperty("userId").toString();
            if (payloadUserId.isNotEmpty())
                userId = normaliseOpusPeerId(payloadUserId);
            clientId = obj->getProperty("clientId").toString().trim();
            appFamily = obj->getProperty("appFamily").toString().trim();
            handshakeVersion = (int)obj->getProperty("handshakeVersion");
            runtimeFormat = obj->getProperty("runtimeFormat").toString().trim();
            pluginVersion = obj->getProperty("pluginVersion").toString().trim();
            if (outEventId != nullptr)
                *outEventId = obj->getProperty("eventId").toString();
        }
        else
            return false;

        const bool isLocalClient = clientId.isNotEmpty() ? (clientId == self->opusSyncInstanceId)
                                                          : (userId == normaliseOpusPeerId(self->currentUser));
        const bool sameAppFamily = appFamily.isEmpty() || appFamily == opusSyncAppFamily;
        const bool compatibleHandshake = handshakeVersion <= 0 || handshakeVersion == opusSyncHandshakeVersion;
        const juce::String peerKey = clientId.isNotEmpty() ? clientId : userId;
        if (peerKey.isNotEmpty() && userId.isNotEmpty() && !isLocalClient)
        {
            bool recognizedNow = false;
            juce::String recognizedMessage;
            {
                juce::ScopedLock lock(self->opusSyncPeerLock);
                if (supportsOpus && sameAppFamily && compatibleHandshake)
                {
                    const bool wasKnown = self->opusSyncPeers.find(peerKey) != self->opusSyncPeers.end();
                    auto& peer = self->opusSyncPeers[peerKey];
                    const bool wasMultiChan = peer.multiChanEnabled;
                    peer.userId = userId;
                    peer.supportsOpus = true;
                    peer.multiChanEnabled = multiChanEnabled;
                    peer.numChannels = peerNumChannels;
                    peer.appFamily = appFamily;
                    peer.handshakeVersion = handshakeVersion;
                    peer.runtimeFormat = runtimeFormat;
                    peer.pluginVersion = pluginVersion;
                    peer.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
                    juce::String peerLabel = sender.isNotEmpty() ? sender : userId;
                    if (!wasKnown)
                    {
                        juce::String peerInfo = peer.runtimeFormat;
                        if (peer.pluginVersion.isNotEmpty())
                        {
                            if (peerInfo.isNotEmpty())
                                peerInfo << " ";
                            peerInfo << peer.pluginVersion;
                        }
                        recognizedMessage = "Multi Client Detected: " + peerLabel;
                        if (peerInfo.isNotEmpty())
                            recognizedMessage << " (" << peerInfo << ")";
                        if (multiChanEnabled)
                            recognizedMessage << " [MultiChannel ON]";
                        recognizedNow = true;
                    }
                    else if (multiChanEnabled && !wasMultiChan)
                    {
                        recognizedMessage = "MultiChannel Detected: " + peerLabel;
                        recognizedNow = true;
                    }
                    else if (!multiChanEnabled && wasMultiChan)
                    {
                        recognizedMessage = "MultiChannel Off: " + peerLabel;
                        recognizedNow = true;
                    }
                }
                else
                    self->opusSyncPeers.erase(peerKey);
            }
            if (recognizedNow)
            {
                juce::ScopedLock lock(self->chatLock);
                self->chatHistory.add(recognizedMessage);
                self->chatSenders.add("");
                self->chatRevision.fetch_add(1);
                if (self->chatHistory.size() > 100)
                {
                    self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
                    self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
                }
            }
            vlogStr("[MCRefresh] processOpusSyncSupport updated peer cache. sender='" + sender + "' userId='" + userId + "' multiChanEnabled=" + juce::String(multiChanEnabled ? 1 : 0) + " nCh=" + juce::String(peerNumChannels));
        }
        return true;
    };
    auto processInboundSideSignal = [self, &processOpusSyncSupport](const juce::String& sender, const juce::String& type, const juce::String& payload, juce::String* outEventId) -> bool
    {
        if (type == "opusSyncSupport")
            return processOpusSyncSupport(sender, payload, outEventId);
        juce::ignoreUnused(outEventId);
        self->processSyncSignal(sender, type, payload);
        return true;
    };
    // nparms is the static array size (always 5); count only non-null entries
    {
        int actualNparms = 0;
        while (actualNparms < nparms && parms[actualNparms] != nullptr)
            ++actualNparms;
        nparms = actualNparms;
    }
    if (nparms > 0)
    {
        juce::String cmd = parms[0];
        vlogStr("ChatMsg cmd=" + cmd + " nparms=" + juce::String(nparms));
        juce::Logger::writeToLog("ChatMsg cmd=" + cmd + " nparms=" + juce::String(nparms));
        auto applyServerCaps = [self](const juce::String& capsText)
        {
            juce::Logger::writeToLog("SERVER_CAPS received: " + capsText);
            const juce::String caps = capsText.toLowerCase();
            const bool hasOpusSyncCap = caps.contains("opus_sync_v2")
                                     || caps.contains("hd_audio_v2")
                                     || caps.contains("hd_sync_v2");
            self->opusSyncServerSupported.store(hasOpusSyncCap);
            juce::Logger::writeToLog("opusSyncServerSupported -> " + juce::String(hasOpusSyncCap ? "true" : "false"));
        };
        juce::String line;
        if (cmd == "SERVER_CAPS" && nparms >= 2)
        {
            applyServerCaps(juce::String(parms[1]));
            return;
        }
        bool isSideSignalCmd = (cmd == "SIDE_SIGNAL_FROM" && nparms >= 4)
                               || (cmd == "SIDE_SIGNAL" && nparms >= 4)
                               || (cmd == "VIDEO_SIGNAL_FROM" && nparms >= 4)
                               || (cmd == "VIDEO_SIGNAL" && nparms >= 4);
        if (isSideSignalCmd)
        {
            juce::String sender;
            juce::String type;
            juce::String payload;
            juce::String signalEventId;
            sender = nparms >= 2 ? juce::String(parms[1]) : juce::String();
            type = nparms >= 3 ? juce::String(parms[nparms - 2]) : juce::String();
            payload = nparms >= 2 ? juce::String(parms[nparms - 1]) : juce::String();
            if (type.isEmpty() || payload.isEmpty())
                return;

            processInboundSideSignal(sender, type, payload, &signalEventId);
            juce::String logLine = "NINJAM Side Signal From " + sender + " [" + type + "]";
            if (signalEventId.isNotEmpty())
                logLine += " eid=" + signalEventId;
            juce::Logger::writeToLog(logLine);
            return;
        }
        if ((cmd == "MSG" || cmd == "PRIVMSG") && nparms >= 3)
        {
            const juce::String sender = juce::String(parms[1]);
            const juce::String messageText = juce::String(parms[2]);
            if (messageText.startsWith(opusSyncChatPrefix))
            {
                vlogStr("MSG opusSyncChatPrefix MATCHED from " + sender);
                juce::Logger::writeToLog("MSG opusSyncChatPrefix received from " + sender);
            }
            const juce::String trimmedText = messageText.trim();
            if (sender == "*" && trimmedText.startsWithIgnoreCase("SERVER_CAPS"))
            {
                juce::String capsText = trimmedText.fromFirstOccurrenceOf("SERVER_CAPS", false, true).trim();
                if (capsText.startsWithChar(':'))
                    capsText = capsText.substring(1).trim();
                applyServerCaps(capsText);
                return;
            }
            if (messageText.startsWith(opusSyncChatPrefix))
            {
                juce::String signalEventId;
                const juce::String payload = messageText.fromFirstOccurrenceOf(opusSyncChatPrefix, false, false);
                if (processOpusSyncSupport(sender, payload, &signalEventId))
                {
                    juce::String logLine = "NINJAM Opus Sync Signal From " + sender + " [chat]";
                    if (signalEventId.isNotEmpty())
                        logLine += " eid=" + signalEventId;
                    juce::Logger::writeToLog(logLine);
                    return;
                }
            }
            bool isSideSignalChat = messageText.startsWith(sideSignalChatPrefix);
            if (isSideSignalChat)
            {
                const char* signalPrefix = sideSignalChatPrefix;
                const juce::String wrapperJson = messageText.fromFirstOccurrenceOf(signalPrefix, false, false);
                juce::var wrapped = juce::JSON::parse(wrapperJson);
                if (auto* wrappedObj = wrapped.getDynamicObject())
                {
                    const juce::String type = wrappedObj->getProperty("type").toString();
                    const juce::String payload = wrappedObj->getProperty("payload").toString();
                    if (type.isNotEmpty() && payload.isNotEmpty())
                    {
                        juce::String signalEventId;
                        if (processInboundSideSignal(sender, type, payload, &signalEventId))
                        {
                            juce::String logLine = "NINJAM Side Signal From " + sender + " [" + type + " chat]";
                            if (signalEventId.isNotEmpty())
                                logLine += " eid=" + signalEventId;
                            juce::Logger::writeToLog(logLine);
                            return;
                        }
                    }
                }
            }
        }

        auto cleanName = [](const char* raw) -> juce::String {
            return normaliseChatTargetNick(juce::String(raw));
        };

        juce::String lineSender;
        juce::String linePrefix;
        juce::String lineBody;
        bool shouldTranslateBody = false;
        if (cmd == "MSG" && nparms >= 3)
        {
            // Suppress server echo of our own messages
            if (normaliseChatTargetNick(juce::String(parms[1])) == normaliseChatTargetNick(self->currentUser))
                return;
            juce::String name = cleanName(parms[1]);
            linePrefix = name + ": ";
            lineBody = juce::String(parms[2]);
            line = linePrefix + lineBody;
            lineSender = name;
            shouldTranslateBody = true;
        }
        else if (cmd == "PRIVMSG" && nparms >= 3)
        {
            juce::String name = cleanName(parms[1]);
            linePrefix = "(Private) " + name + ": ";
            lineBody = juce::String(parms[2]);
            line = linePrefix + lineBody;
            lineSender = name;
            shouldTranslateBody = true;
        }
        else if (cmd == "TOPIC" && nparms >= 2)
            line = "Topic: " + juce::String(parms[1]);
        else if (cmd == "JOIN" && nparms >= 2)
        {
            line = cleanName(parms[1]) + " has joined.";
        }
        else if (cmd == "PART" && nparms >= 2)
             line = cleanName(parms[1]) + " has left.";
        else
        {
            line = cmd;
            for (int i=1; i<nparms; ++i) 
                if (parms[i]) line += " " + juce::String(parms[i]);
        }
        {
            const juce::String stored = line;
            juce::ScopedLock lock(self->chatLock);
            self->chatHistory.add(stored);
            self->chatSenders.add(lineSender);
            self->chatRevision.fetch_add(1);
            if (self->chatHistory.size() > 100)
            {
                self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
                self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
            }
        }

        if (shouldTranslateBody)
            self->enqueueAsyncTranslation(line, lineSender, linePrefix, lineBody);
        
        // Also log
        juce::Logger::writeToLog("NINJAM Chat: " + line);
    }
}

void NinjamVst3AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::AudioPlayHead::CurrentPositionInfo hostInfoAtBlock;
    bool gotHostPosition = false;
    if (auto* playHead = getPlayHead())
    {
        juce::AudioPlayHead::CurrentPositionInfo info;
        if (copyPlayHeadPositionToCurrentInfo(*playHead, info))
        {
            gotHostPosition = true;
            hostInfoAtBlock = info;
            const juce::ScopedLock lock(transportLock);
            lastHostPosition = info;
            lastHostPositionValid.store(true, std::memory_order_relaxed);
        }
        else
        {
            lastHostPositionValid.store(false, std::memory_order_relaxed);
        }
    }
    else
    {
        lastHostPositionValid.store(false, std::memory_order_relaxed);
    }

    const int numSamples = buffer.getNumSamples();
    std::chrono::microseconds filteredLinkBufferTime { 0 };
    if (linkTimingState != nullptr)
    {
        filteredLinkBufferTime = linkTimingState->hostTimeFilter.sampleTimeToHostTime(linkTimingState->nextSampleTime);
        linkTimingState->nextSampleTime += (double) numSamples;
    }

    const SyncMode syncModeAtBlock = getSyncMode();
    const bool linkTransportMode = (syncModeAtBlock == SyncMode::abletonLink);
    const bool anyLinkActive = linkTransportMode || isLinkAudioEnabled();
    std::optional<ableton::LinkAudio::SessionState> linkSessionState;
    std::chrono::microseconds linkBufferTime { 0 };
    const double linkQuantum = juce::jmax(1.0, (double) getBPI());
    const double linkStartQuantum = juce::jmax(1.0, juce::jmin(linkQuantum, linkAudioQuantumBeats));
    constexpr double linkAudioQuantum = linkAudioQuantumBeats;
    bool gotLinkState = false;
    bool linkPlayingAtBlock = false;
    double linkTempoAtBlock = 0.0;
    double linkPhaseAtBlock = 0.0;
    int linkPeersAtBlock = 0;
    if (anyLinkActive && abletonLink != nullptr)
    {
        linkSessionState.emplace(abletonLink->captureAudioSessionState());
        linkBufferTime = linkTimingState != nullptr ? filteredLinkBufferTime : abletonLink->clock().micros();
        linkPlayingAtBlock = linkSessionState->isPlaying();
        linkTempoAtBlock = linkSessionState->tempo();
        linkPhaseAtBlock = linkSessionState->phaseAtTime(linkBufferTime, linkQuantum);
        linkPeersAtBlock = (int) abletonLink->numPeers();
        gotLinkState = true;
        {
            const juce::ScopedLock lock(linkTransportStateLock);
            lastLinkTempo = linkTempoAtBlock;
            lastLinkPhaseBeats = linkPhaseAtBlock;
            lastLinkPeerCount = linkPeersAtBlock;
            lastLinkIsPlaying = linkPlayingAtBlock;
        }
    }

    intervalSyncSampleCounter.fetch_add((long long)numSamples, std::memory_order_relaxed);
    const bool useHostMidiForLearn = getMidiLearnInputDeviceId().isEmpty();
    const bool useHostMidiForRelay = getMidiRelayInputDeviceId().isEmpty();
    {
        const juce::SpinLock::ScopedLockType midiQueueLock(midiEventQueueLock);
        const juce::SpinLock::ScopedLockType relayQueueLock(outboundMidiRelayQueueLock);
        for (const auto metadata : midiMessages)
        {
            const auto& msg = metadata.getMessage();
            if (msg.isController())
            {
                MidiControllerEvent event;
                event.isController = true;
                event.midiChannel = msg.getChannel();
                event.number = msg.getControllerNumber();
                event.value = msg.getControllerValue();
                event.normalized = (float)event.value / 127.0f;
                event.isNoteOn = event.value >= 64;
                if (useHostMidiForLearn)
                    pendingMidiControllerEvents.push_back(event);
                if (useHostMidiForRelay)
                    pendingOutboundMidiRelayEvents.push_back(event);
            }
            else if (msg.isNoteOnOrOff())
            {
                MidiControllerEvent event;
                event.isController = false;
                event.midiChannel = msg.getChannel();
                event.number = msg.getNoteNumber();
                event.value = msg.getVelocity();
                event.normalized = msg.isNoteOn() ? ((float)event.value / 127.0f) : 0.0f;
                event.isNoteOn = msg.isNoteOn();
                if (useHostMidiForLearn)
                    pendingMidiControllerEvents.push_back(event);
                if (useHostMidiForRelay)
                    pendingOutboundMidiRelayEvents.push_back(event);
            }
        }
        if (pendingMidiControllerEvents.size() > 512)
            pendingMidiControllerEvents.erase(pendingMidiControllerEvents.begin(), pendingMidiControllerEvents.begin() + (long long)(pendingMidiControllerEvents.size() - 512));
        if (pendingOutboundMidiRelayEvents.size() > 512)
            pendingOutboundMidiRelayEvents.erase(pendingOutboundMidiRelayEvents.begin(), pendingOutboundMidiRelayEvents.begin() + (long long)(pendingOutboundMidiRelayEvents.size() - 512));
    }
    injectInboundMidiRelayEvents(midiMessages);

    int totalInputChannels = 0;
    int numInputBuses = getBusCount(true);
    for (int bus = 0; bus < numInputBuses; ++bus)
    {
        int busChans = getChannelCountOfBus(true, bus);
        if (busChans <= 0)
            continue;
        totalInputChannels += busChans;
    }

    bool anyLocalUsesLinkAudioInput = false;
    for (int channel = 0; channel < maxLocalChannels; ++channel)
    {
        if (localChannelInputs[(size_t) channel].load() == kLocalInputLinkAudioSentinel)
        {
            anyLocalUsesLinkAudioInput = true;
            break;
        }
    }

    std::vector<float> blockLinkAudioSamples;
    int blockLinkAudioNumChannels = 0;
    int blockLinkAudioNumFrames = 0;
    if (isLinkAudioEnabled() && isLinkAudioReceiveEnabled())
    {
        blockLinkAudioSamples.assign((size_t)numSamples * 2u, 0.0f);
        blockLinkAudioNumChannels = 2;
        blockLinkAudioNumFrames = numSamples;

        const size_t maxBufferedFrames = (size_t) (numSamples <= 64
            ? juce::jmax(numSamples * 16, 1024)
            : juce::jmax(numSamples * 4, 512));
        const size_t availableFrames = linkAudioReceiveRing.available();
        if (availableFrames > maxBufferedFrames)
            linkAudioReceiveRing.discard(availableFrames - maxBufferedFrames);

        const size_t framesRead = linkAudioReceiveRing.readInterleaved(blockLinkAudioSamples.data(), (size_t)numSamples);
        if (framesRead == 0)
        {
            blockLinkAudioSamples.clear();
            blockLinkAudioNumChannels = 0;
            blockLinkAudioNumFrames = 0;
        }
    }
    else
    {
        linkAudioReceiveRing.reset();
    }

    const int linkInputChannels = blockLinkAudioNumChannels > 0 ? juce::jlimit(1, 2, blockLinkAudioNumChannels) : 0;
    const int totalAvailableInputChannels = totalInputChannels + linkInputChannels;

    if (tempInputBuffer.getNumChannels() < totalAvailableInputChannels || tempInputBuffer.getNumSamples() < numSamples)
        tempInputBuffer.setSize(totalAvailableInputChannels, numSamples, false, false, true);

    int inputChanIndex = 0;
    for (int bus = 0; bus < numInputBuses; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, true, bus);
        int busChans = busBuffer.getNumChannels();
        if (busChans <= 0)
            continue;
        for (int ch = 0; ch < busChans; ++ch)
        {
            if (inputChanIndex < totalInputChannels)
            {
                tempInputBuffer.copyFrom(inputChanIndex, 0, busBuffer, ch, 0, numSamples);
                ++inputChanIndex;
            }
        }
    }

    if (linkInputChannels > 0)
    {
        for (int channel = 0; channel < linkInputChannels; ++channel)
            tempInputBuffer.clear(totalInputChannels + channel, 0, numSamples);

        const int framesToCopy = juce::jmin(numSamples, blockLinkAudioNumFrames);
        for (int sampleIndex = 0; sampleIndex < framesToCopy; ++sampleIndex)
        {
            const int sourceOffset = sampleIndex * blockLinkAudioNumChannels;
            tempInputBuffer.setSample(totalInputChannels, sampleIndex, blockLinkAudioSamples[(size_t) sourceOffset]);
            if (linkInputChannels > 1)
            {
                const int rightSource = sourceOffset + juce::jmin(1, blockLinkAudioNumChannels - 1);
                tempInputBuffer.setSample(totalInputChannels + 1, sampleIndex, blockLinkAudioSamples[(size_t) rightSource]);
            }
        }
    }

    if (localChannelBuffer.getNumChannels() < maxLocalChannels || localChannelBuffer.getNumSamples() < numSamples)
        localChannelBuffer.setSize(maxLocalChannels, numSamples, false, false, true);

    int requestedLocal = numLocalChannels.load();
    int actualLocal = juce::jlimit(1, maxLocalChannels, requestedLocal);
    actualLocal = totalAvailableInputChannels > 0 ? juce::jmin(actualLocal, totalAvailableInputChannels) : 0;
    std::array<int, maxLocalChannels> monitorSourceLeft{};
    std::array<int, maxLocalChannels> monitorSourceRight{};
    std::array<bool, maxLocalChannels> monitorStereo{};
    monitorSourceLeft.fill(-1);
    monitorSourceRight.fill(-1);
    monitorStereo.fill(false);

    bool fedLocalChordAnalyzer = false;
    float globalLocalMax = 0.0f;
    float globalLocalMaxL = 0.0f;
    float globalLocalMaxR = 0.0f;
    for (int ch = 0; ch < actualLocal; ++ch)
    {
        int srcIndex = localChannelInputs[(size_t)ch].load();
        int leftSource = -1;
        int rightSource = -1;

        if (srcIndex == kLocalInputLinkAudioSentinel)
        {
            const int left = totalInputChannels;
            const int right = linkInputChannels > 1 ? totalInputChannels + 1 : left;

            localChannelBuffer.clear(ch, 0, numSamples);
            if (left < totalAvailableInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, left, 0, numSamples, linkInputChannels > 1 ? 0.5f : 1.0f);
            if (linkInputChannels > 1 && right < totalAvailableInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, right, 0, numSamples, 0.5f);

            leftSource = linkInputChannels > 0 ? left : -1;
            rightSource = linkInputChannels > 1 ? right : leftSource;
            monitorStereo[(size_t)ch] = (linkInputChannels > 1);
        }
        else if (srcIndex >= 0)
        {
            if (srcIndex >= totalInputChannels)
                srcIndex = juce::jlimit(0, totalInputChannels - 1, srcIndex);

            int left = juce::jlimit(0, juce::jmax(totalInputChannels - 1, 0), srcIndex);
            int right = left;

            localChannelBuffer.clear(ch, 0, numSamples);
            if (left < totalInputChannels)
                localChannelBuffer.copyFrom(ch, 0, tempInputBuffer, left, 0, numSamples);

            leftSource = left;
            rightSource = right;
        }
        else
        {
            int pairIndex = -1 - srcIndex;
            int left = pairIndex * 2;
            int right = left + 1;

            if (left < 0 || left >= totalInputChannels)
                left = juce::jlimit(0, juce::jmax(totalInputChannels - 1, 0), left);
            if (right < 0 || right >= totalInputChannels)
                right = left;

            localChannelBuffer.clear(ch, 0, numSamples);
            if (left < totalInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, left, 0, numSamples, 0.5f);
            if (right < totalInputChannels)
                localChannelBuffer.addFrom(ch, 0, tempInputBuffer, right, 0, numSamples, 0.5f);

            leftSource = left;
            rightSource = right;
            monitorStereo[(size_t)ch] = (right != left);
        }

        monitorSourceLeft[(size_t)ch] = leftSource;
        monitorSourceRight[(size_t)ch] = rightSource;

        if (ch == 0 && localChordAnalyzer && isChordDetectionEnabled())
        {
            localChordAnalyzer->processBlock(localChannelBuffer.getReadPointer(ch), numSamples);
            fedLocalChordAnalyzer = true;
        }

        float gain = localChannelGains[(size_t)ch].load();
        if (gain != 1.0f)
            localChannelBuffer.applyGain(ch, 0, numSamples, gain);

        const float* data = localChannelBuffer.getReadPointer(ch);
        float localMax = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            float a = std::abs(data[i]);
            if (a > localMax)
                localMax = a;
        }

        float localMaxL = 0.0f;
        float localMaxR = 0.0f;

        if (leftSource >= 0 && leftSource < totalAvailableInputChannels)
        {
            const float* leftData = tempInputBuffer.getReadPointer(leftSource);
            for (int i = 0; i < numSamples; ++i)
            {
                float a = std::abs(leftData[i] * gain);
                if (a > localMaxL)
                    localMaxL = a;
            }
        }

        if (rightSource >= 0 && rightSource < totalAvailableInputChannels)
        {
            const float* rightData = tempInputBuffer.getReadPointer(rightSource);
            for (int i = 0; i < numSamples; ++i)
            {
                float a = std::abs(rightData[i] * gain);
                if (a > localMaxR)
                    localMaxR = a;
            }
        }

        localChannelPeaks[(size_t)ch].store(localMax);
        localChannelPeaksL[(size_t)ch].store(localMaxL);
        localChannelPeaksR[(size_t)ch].store(localMaxR);
        if (localMax > globalLocalMax)
            globalLocalMax = localMax;
        if (localMaxL > globalLocalMaxL)
            globalLocalMaxL = localMaxL;
        if (localMaxR > globalLocalMaxR)
            globalLocalMaxR = localMaxR;
    }

    for (int ch = actualLocal; ch < maxLocalChannels; ++ch)
    {
        localChannelBuffer.clear(ch, 0, numSamples);
        localChannelPeaks[(size_t)ch].store(0.0f);
        localChannelPeaksL[(size_t)ch].store(0.0f);
        localChannelPeaksR[(size_t)ch].store(0.0f);
    }

    localPeak.store(globalLocalMax);
    localPeakL.store(globalLocalMaxL);
    localPeakR.store(globalLocalMaxR);

    if (localChordAnalyzer && !fedLocalChordAnalyzer)
        localChordAnalyzer->markNoInput();

    const bool reverbOn = fxReverbEnabled.load();
    const bool delayOn = fxDelayEnabled.load();
    const bool fxSendActive = reverbOn || delayOn;

    if (fxTransmitBuffer.getNumSamples() < numSamples)
        fxTransmitBuffer.setSize(1, numSamples, false, true, true);
    if (fxReturnBuffer.getNumSamples() < numSamples)
        fxReturnBuffer.setSize(2, numSamples, false, true, true);
    fxTransmitBuffer.clear();
    fxReturnBuffer.clear();

    if (fxSendActive)
    {
        if (fxReverbInputBuffer.getNumSamples() < numSamples)
            fxReverbInputBuffer.setSize(1, numSamples, false, true, true);
        if (fxDelayInputBuffer.getNumSamples() < numSamples)
            fxDelayInputBuffer.setSize(1, numSamples, false, true, true);

        fxReverbInputBuffer.clear();
        fxDelayInputBuffer.clear();

        const int activeLocal = juce::jmin(actualLocal, numLocalChannels.load());
        for (int ch = 0; ch < activeLocal; ++ch)
        {
            const float reverbSend = localChannelReverbSends[(size_t)ch].load();
            const float delaySend = localChannelDelaySends[(size_t)ch].load();
            if (reverbSend <= 0.0001f && delaySend <= 0.0001f)
                continue;

            const float* src = localChannelBuffer.getReadPointer(ch);
            float* reverbDst = fxReverbInputBuffer.getWritePointer(0);
            float* delayDst = fxDelayInputBuffer.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
            {
                const float v = src[i];
                if (reverbSend > 0.0001f)
                    reverbDst[i] += v * reverbSend;
                if (delaySend > 0.0001f)
                    delayDst[i] += v * delaySend;
            }
        }

        float* fxSendMono = fxTransmitBuffer.getWritePointer(0);
        float* fxLeft = fxReturnBuffer.getWritePointer(0);
        float* fxRight = fxReturnBuffer.getWritePointer(1);

        if (reverbOn)
        {
            juce::Reverb::Parameters params;
            params.roomSize = fxReverbRoomSize.load();
            params.damping = fxReverbDamping.load();
            params.width = 1.0f;
            params.wetLevel = 1.0f;
            params.dryLevel = 0.0f;
            params.freezeMode = 0.0f;
            fxReverb.setParameters(params);

            const float wetDryMix = fxReverbWetDryMix.load();
            const float earlyAmount = fxReverbEarlyReflections.load();
            const float tailAmount = fxReverbTail.load();
            const float* reverbIn = fxReverbInputBuffer.getReadPointer(0);
            float* revMono = fxReverbInputBuffer.getWritePointer(0);
            fxReverb.processMono(revMono, numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                const float early = reverbIn[i] * earlyAmount;
                const float tail = revMono[i] * tailAmount;
                const float wet = early + tail;
                const float mixed = wet * wetDryMix + reverbIn[i] * (1.0f - wetDryMix);
                const float out = mixed * 0.8f;
                fxLeft[i] += out;
                fxRight[i] += out;
                fxSendMono[i] += out * 0.5f;
            }
        }

        if (delayOn)
        {
            const int delayBufferSamples = fxDelayBuffer.getNumSamples();
            if (delayBufferSamples > 1)
            {
                const int division = fxDelayDivision.load();
                const double bpm = (double)getBPM();
                double targetDelaySeconds = fxDelayTimeMs.load() / 1000.0;
                if (fxDelaySyncToHost.load() && bpm > 1.0)
                    targetDelaySeconds = (60.0 / bpm) * (4.0 / (double)division);
                const int delaySamples = juce::jlimit(1, delayBufferSamples - 1, (int)std::round(targetDelaySeconds * processingSampleRate));

                const bool pingPong = fxDelayPingPong.load();
                const float feedback = juce::jlimit(0.0f, 0.95f, fxDelayFeedback.load());
                const float wetDryMix = juce::jlimit(0.0f, 1.0f, fxDelayWetDryMix.load());
                const float delayWet = wetDryMix * 0.8f;

                float* delayMemoryL = fxDelayBuffer.getWritePointer(0);
                float* delayMemoryR = fxDelayBuffer.getWritePointer(1);
                const float* delayIn = fxDelayInputBuffer.getReadPointer(0);

                int writePos = fxDelayWritePosition;
                for (int i = 0; i < numSamples; ++i)
                {
                    int readPos = writePos - delaySamples;
                    if (readPos < 0)
                        readPos += delayBufferSamples;

                    const float readL = delayMemoryL[readPos];
                    const float readR = delayMemoryR[readPos];
                    const float input = delayIn[i];
                    const float wetL = readL * delayWet;
                    const float wetR = readR * delayWet;

                    fxLeft[i] += wetL;
                    fxRight[i] += wetR;
                    fxSendMono[i] += (wetL + wetR) * 0.25f;

                    if (pingPong)
                    {
                        delayMemoryL[writePos] = input + readR * feedback;
                        delayMemoryR[writePos] = input + readL * feedback;
                    }
                    else
                    {
                        const float mono = 0.5f * (readL + readR);
                        delayMemoryL[writePos] = input + mono * feedback;
                        delayMemoryR[writePos] = input + mono * feedback;
                    }

                    ++writePos;
                    if (writePos >= delayBufferSamples)
                        writePos = 0;
                }
                fxDelayWritePosition = writePos;
            }
        }
    }

    // Determine active encoding mode:
    // - multiChanAuto: >1 local channels + VST3 peers → Vorbis mix on ch0, Opus per-ch on ch1..N
    // - otherwise:     Vorbis only, single channel (mix folded into ch0 above)
    const bool multiChanAuto = numLocalChannels.load() > 1 && opusSyncAvailable.load() && isTransmittingLocal();

    if (!multiChanAuto && actualLocal > 1)
    {
        float* dst = localChannelBuffer.getWritePointer(0);
        for (int ch = 1; ch < actualLocal; ++ch)
        {
            const float* src = localChannelBuffer.getReadPointer(ch);
            for (int s = 0; s < numSamples; ++s)
                dst[s] += src[s];
        }
    }

    if (!multiChanAuto && fxSendActive)
        localChannelBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);

    const bool singleStereoLocal = !multiChanAuto
        && actualLocal == 1
        && monitorStereo[0]
        && isTransmittingLocal();

    if (multiChanAuto)
    {
        if (localMixBuffer.getNumSamples() < numSamples)
            localMixBuffer.setSize(1, numSamples, false, true, true);
        float* mix = localMixBuffer.getWritePointer(0);
        const float* src0 = localChannelBuffer.getReadPointer(0);
        for (int s = 0; s < numSamples; ++s)
            mix[s] = src0[s];
        for (int ch = 1; ch < actualLocal; ++ch)
        {
            const float* src = localChannelBuffer.getReadPointer(ch);
            for (int s = 0; s < numSamples; ++s)
                mix[s] += src[s];
        }
        if (fxSendActive)
            localMixBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);
    }

    float* inputs[32] = {};
    int actualInputChannels;
    if (multiChanAuto)
    {
        // syncLocalIntervalChannelConfig advertises this slot as the legacy Vorbis mixdown.
        const int n = juce::jlimit(1, maxLocalChannels, numLocalChannels.load());
        for (int i = 0; i < n; ++i)
            inputs[i] = localChannelBuffer.getWritePointer(i);
        inputs[n] = localMixBuffer.getWritePointer(0);
        inputs[n + 1] = fxTransmitBuffer.getWritePointer(0);
        actualInputChannels = n + 2;
    }
    else
    {
        if (singleStereoLocal)
        {
            if (localMixBuffer.getNumChannels() < 2 || localMixBuffer.getNumSamples() < numSamples)
                localMixBuffer.setSize(2, numSamples, false, true, true);

            const int sourceLeft = monitorSourceLeft[0];
            const int sourceRight = monitorSourceRight[0];
            const float gain = localChannelGains[0].load();

            localMixBuffer.clear();
            if (sourceLeft >= 0 && sourceLeft < totalAvailableInputChannels)
                localMixBuffer.copyFrom(0, 0, tempInputBuffer, sourceLeft, 0, numSamples);
            if (sourceRight >= 0 && sourceRight < totalAvailableInputChannels)
                localMixBuffer.copyFrom(1, 0, tempInputBuffer, sourceRight, 0, numSamples);
            else if (sourceLeft >= 0 && sourceLeft < totalAvailableInputChannels)
                localMixBuffer.copyFrom(1, 0, tempInputBuffer, sourceLeft, 0, numSamples);

            if (gain != 1.0f)
                localMixBuffer.applyGain(gain);

            if (fxSendActive)
            {
                localMixBuffer.addFrom(0, 0, fxTransmitBuffer, 0, 0, numSamples);
                localMixBuffer.addFrom(1, 0, fxTransmitBuffer, 0, 0, numSamples);
            }

            inputs[0] = localMixBuffer.getWritePointer(0);
            inputs[1] = localMixBuffer.getWritePointer(1);
            actualInputChannels = 2;
        }
        else
        {
            inputs[0] = localChannelBuffer.getWritePointer(0);
            actualInputChannels = 1;
        }
    }

    float* outputs[32];
    int totalOutputChannels = 0;
    int numOutputBuses = getBusCount(false);
    for (int bus = 0; bus < numOutputBuses; ++bus)
    {
        int busChans = getChannelCountOfBus(false, bus);
        if (busChans <= 0)
            continue;
        totalOutputChannels += busChans;
    }

    int actualOutputChannels = juce::jmin(totalOutputChannels, 32);

    int outputChanIndex = 0;
    for (int bus = 0; bus < numOutputBuses; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, false, bus);
        int busChans = busBuffer.getNumChannels();
        if (busChans <= 0)
            continue;
        for (int ch = 0; ch < busChans; ++ch)
        {
            if (outputChanIndex < actualOutputChannels)
            {
                outputs[outputChanIndex] = busBuffer.getWritePointer(ch);
                ++outputChanIndex;
            }
        }
    }

    for (int bus = 0; bus < numOutputBuses; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, false, bus);
        int busChans = busBuffer.getNumChannels();
        if (busChans <= 0)
            continue;
        for (int ch = 0; ch < busChans; ++ch)
            busBuffer.clear(ch, 0, numSamples);
    }

    bool gateForSync = false;
    bool runMonitorOnly = false;
    if (syncModeAtBlock == SyncMode::host)
    {
        bool hostValid = gotHostPosition;
        bool hostPlaying = hostValid && hostInfoAtBlock.isPlaying;
        const bool waitingForRestart = syncAwaitingHostRestart.load();

        bool prev = hostWasPlaying.load();
        if (!hostValid || !hostPlaying)
        {
            hostWasPlaying.store(false);
            syncAwaitingHostRestart.store(false);
            syncWaitForInterval.store(false);
            syncTargetInterval.store(-1);
            syncDisplayPositionOffset.store(0);
        }
        else if (waitingForRestart)
        {
            hostWasPlaying.store(false);
            gateForSync = true;
            syncDisplayPositionOffset.store(0);
        }
        else if (!prev)
        {
            hostWasPlaying.store(true);
            primeSyncTransportStart(&hostInfoAtBlock);
            syncWaitForInterval.store(false);
            syncTargetInterval.store(-1);
            syncDisplayIntervalOffset.store(intervalIndex.load());
        }

        if (!hostValid || !hostPlaying)
        {
            gateForSync = true;
        }
        runMonitorOnly = gateForSync;
    }
    else if (syncModeAtBlock == SyncMode::abletonLink)
    {
        const bool linkPlaying = gotLinkState && linkPlayingAtBlock;
        const bool prev = linkWasPlaying.load();
        const bool waitingForRestart = syncAwaitingHostRestart.load();

        if (!gotLinkState || !linkPlaying)
        {
            hostWasPlaying.store(false);
            linkWasPlaying.store(false);
            syncAwaitingHostRestart.store(false);
            syncWaitForInterval.store(false);
            syncTargetInterval.store(-1);
            syncDisplayPositionOffset.store(0);
            gateForSync = true;
        }
        else if (waitingForRestart)
        {
            hostWasPlaying.store(false);
            linkWasPlaying.store(false);
            syncDisplayPositionOffset.store(0);
            gateForSync = true;
        }
        else
        {
            if (!prev)
            {
                const auto requestedStartTime = linkSessionState->timeForIsPlaying();
                const auto alignedStartTime = getNextLinkQuantumTime(*linkSessionState,
                                                                     requestedStartTime,
                                                                     linkStartQuantum,
                                                                     linkTempoAtBlock);
                if (alignedStartTime > linkBufferTime)
                {
                    hostWasPlaying.store(false);
                    linkWasPlaying.store(false);
                    syncWaitForInterval.store(false);
                    syncTargetInterval.store(-1);
                    syncDisplayPositionOffset.store(0);
                    gateForSync = true;
                }
                else
                {
                    hostWasPlaying.store(true);
                    linkWasPlaying.store(true);
                    const double startPhaseBeats = linkSessionState->phaseAtTime(alignedStartTime, linkQuantum);
                    primeLinkTransportStart(startPhaseBeats, linkQuantum, linkTempoAtBlock);
                    syncWaitForInterval.store(false);
                    syncTargetInterval.store(-1);
                    syncDisplayIntervalOffset.store(intervalIndex.load());
                }
            }
            else
            {
                hostWasPlaying.store(true);
                linkWasPlaying.store(true);
            }
        }

        runMonitorOnly = gateForSync;
    }
    else
    {
        hostWasPlaying.store(false);
        linkWasPlaying.store(false);
        syncWaitForInterval.store(false);
        syncTargetInterval.store(-1);
        syncDisplayIntervalOffset.store(0);
        syncDisplayPositionOffset.store(0);
    }

    const bool monitorEnabled = localMonitorEnabled.load();
    const bool transmitEnabled = isTransmittingLocal();

    // Feed local input to engine only for transmit. Monitoring is handled below
    // with explicit per-channel routing so stereo doesn't collapse when transmit toggles.
    const bool allowEngineLocalInput = transmitEnabled;
    float** engineInputs = allowEngineLocalInput ? inputs : nullptr;
    int engineInputChannels = allowEngineLocalInput ? actualInputChannels : 0;
    ninjamClient.AudioProc(engineInputs, engineInputChannels, outputs, actualOutputChannels, numSamples, (int)getSampleRate(), runMonitorOnly);

    int numOutputBusesOut = getBusCount(false);
    if (gateForSync)
    {
        for (int bus = 0; bus < numOutputBusesOut; ++bus)
        {
            auto busBuffer = getBusBuffer(buffer, false, bus);
            const int busChans = busBuffer.getNumChannels();
            for (int ch = 0; ch < busChans; ++ch)
                busBuffer.clear(ch, 0, numSamples);
        }
    }

    if (linkSessionState.has_value() && isLinkAudioEnabled() && isLinkAudioSendEnabled())
    {
        const juce::SpinLock::ScopedTryLockType endpointLock(linkAudioEndpointLock);
        if (endpointLock.isLocked())
        {
            const double beatsAtBufferBegin = linkSessionState->beatAtTime(linkBufferTime, linkAudioQuantum);
            for (auto& pair : remoteLinkAudioOutputPairs)
            {
                auto sinkIt = remoteLinkAudioSinks.find(pair.first);
                if (sinkIt == remoteLinkAudioSinks.end() || sinkIt->second == nullptr)
                    continue;

                const int leftChannel = pair.second * 2;
                const int rightChannel = leftChannel + 1;
                if (leftChannel < 0 || rightChannel >= actualOutputChannels)
                    continue;
                if (outputs[leftChannel] == nullptr || outputs[rightChannel] == nullptr)
                    continue;

                sinkIt->second->requestMaxNumSamples((size_t) juce::jmax(2, numSamples * 2));
                ableton::LinkAudioSink::BufferHandle sinkBuffer(*sinkIt->second);
                if (!sinkBuffer || sinkBuffer.samples == nullptr || sinkBuffer.maxNumSamples < (size_t) numSamples * 2u)
                    continue;

                for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
                {
                    const float leftSample = juce::jlimit(-1.0f, 1.0f, outputs[leftChannel][sampleIndex]);
                    const float rightSample = juce::jlimit(-1.0f, 1.0f, outputs[rightChannel][sampleIndex]);
                    sinkBuffer.samples[(size_t) sampleIndex * 2u] = ableton::util::floatToInt16(leftSample);
                    sinkBuffer.samples[(size_t) sampleIndex * 2u + 1u] = ableton::util::floatToInt16(rightSample);
                }

                sinkBuffer.commit(*linkSessionState,
                                  beatsAtBufferBegin,
                                  linkAudioQuantum,
                                  (size_t) numSamples,
                                  2u,
                                  (uint32_t) std::llround(juce::jmax(1.0, getSampleRate())));

                juce::FloatVectorOperations::clear(outputs[leftChannel], numSamples);
                juce::FloatVectorOperations::clear(outputs[rightChannel], numSamples);
            }
        }
    }

    if (monitorEnabled || transmitEnabled)
    {
        if (numOutputBusesOut > 0)
        {
            auto mainBus = getBusBuffer(buffer, false, 0);
            int outChans = mainBus.getNumChannels();
            int numLocal = juce::jmin(numLocalChannels.load(), maxLocalChannels);
            for (int ch = 0; ch < numLocal; ++ch)
            {
                const int outLeft = ch * 2;
                const int outRight = outLeft + 1;
                if (outChans <= 0)
                    break;
                const int sourceLeft = monitorSourceLeft[(size_t)ch];
                const int sourceRight = monitorSourceRight[(size_t)ch];
                const float gain = localChannelGains[(size_t)ch].load();
                if (sourceLeft < 0 || sourceLeft >= totalAvailableInputChannels)
                    continue;

                if (monitorStereo[(size_t)ch] && sourceRight >= 0 && sourceRight < totalAvailableInputChannels)
                {
                    if (outLeft < outChans)
                        mainBus.addFrom(outLeft, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                    if (outRight < outChans)
                        mainBus.addFrom(outRight, 0, tempInputBuffer, sourceRight, 0, numSamples, gain);
                    else if (outLeft == 0 && outChans == 1)
                    {
                        mainBus.addFrom(0, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain * 0.5f);
                        mainBus.addFrom(0, 0, tempInputBuffer, sourceRight, 0, numSamples, gain * 0.5f);
                    }
                }
                else
                {
                    if (outLeft < outChans)
                        mainBus.addFrom(outLeft, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                    if (outRight < outChans)
                        mainBus.addFrom(outRight, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                    else if (outLeft == 0 && outChans == 1)
                        mainBus.addFrom(0, 0, tempInputBuffer, sourceLeft, 0, numSamples, gain);
                }
            }
        }
    }

    int mtcPos = 0;
    int mtcLength = 0;
    ninjamClient.GetPosition(&mtcPos, &mtcLength);
    emitMidiTimecode(midiMessages, numSamples, mtcPos, mtcLength);

    if (linkInputChannels > 0 && !anyLocalUsesLinkAudioInput)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int outputChannels = mainBus.getNumChannels();
        if (outputChannels > 0)
        {
            const int linkLeft = totalInputChannels;
            const int linkRight = linkInputChannels > 1 ? totalInputChannels + 1 : linkLeft;
            if (outputChannels > 1)
            {
                mainBus.addFrom(0, 0, tempInputBuffer, linkLeft, 0, numSamples);
                mainBus.addFrom(1, 0, tempInputBuffer, linkRight, 0, numSamples);
            }
            else if (outputChannels == 1)
            {
                const float foldGain = linkInputChannels > 1 ? 0.5f : 1.0f;
                mainBus.addFrom(0, 0, tempInputBuffer, linkLeft, 0, numSamples, foldGain);
                if (linkInputChannels > 1)
                mainBus.addFrom(0, 0, tempInputBuffer, linkRight, 0, numSamples, 0.5f);
            }
        }
    }

    if (numOutputBusesOut > 0 && fxSendActive)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        const int mainChans = mainBus.getNumChannels();
        if (mainChans >= 2)
        {
            mainBus.addFrom(0, 0, fxReturnBuffer, 0, 0, numSamples);
            mainBus.addFrom(1, 0, fxReturnBuffer, 1, 0, numSamples);
        }
        else if (mainChans == 1)
        {
            const float* l = fxReturnBuffer.getReadPointer(0);
            const float* r = fxReturnBuffer.getReadPointer(1);
            float* monoOut = mainBus.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
                monoOut[i] += 0.5f * (l[i] + r[i]);
        }
    }

    float masterGain = masterOutputGain.load();
    if (masterGain != 1.0f)
    {
        for (int bus = 0; bus < numOutputBusesOut; ++bus)
        {
            auto busBuffer = getBusBuffer(buffer, false, bus);
            int busChans = busBuffer.getNumChannels();
            for (int ch = 0; ch < busChans; ++ch)
                busBuffer.applyGain(ch, 0, numSamples, masterGain);
        }
    }

    bool limiter = dspLimiterEnabled.load() && (limiterThresholdDb.load() < 0.0f);
    if (limiter)
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        masterLimiter.process(context);
    }

    bool softClip = softLimiterEnabled.load();
    float maxSample = 0.0f;
    float maxSampleL = 0.0f;
    float maxSampleR = 0.0f;
    for (int bus = 0; bus < numOutputBusesOut; ++bus)
    {
        auto busBuffer = getBusBuffer(buffer, false, bus);
        int busChans = busBuffer.getNumChannels();
        for (int ch = 0; ch < busChans; ++ch)
        {
            float* data = busBuffer.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                float v = data[i];
                if (softClip)
                    v = softClipSample(v);
                float a = std::abs(v);
                if (a > maxSample)
                    maxSample = a;
                if (bus == 0 && ch == 0 && a > maxSampleL)
                    maxSampleL = a;
                if (bus == 0 && ch == 1 && a > maxSampleR)
                    maxSampleR = a;
                data[i] = v;
            }
        }
    }
    if (numOutputBusesOut > 0)
    {
        auto mainBus = getBusBuffer(buffer, false, 0);
        if (mainBus.getNumChannels() == 1)
            maxSampleR = maxSampleL;
        else if (mainBus.getNumChannels() == 0)
        {
            maxSampleL = maxSample;
            maxSampleR = maxSample;
        }
    }
    else
    {
        maxSampleL = maxSample;
        maxSampleR = maxSample;
    }

    if (linkSessionState.has_value() && isLinkAudioEnabled() && isLinkAudioSendEnabled() && numOutputBusesOut > 0)
    {
        const juce::SpinLock::ScopedTryLockType endpointLock(linkAudioEndpointLock);
        if (endpointLock.isLocked() && abletonLinkSink != nullptr)
        {
            abletonLinkSink->requestMaxNumSamples((size_t) juce::jmax(2, numSamples * 2));
            ableton::LinkAudioSink::BufferHandle sinkBuffer(*abletonLinkSink);
            if (sinkBuffer)
            {
                auto mainBus = getBusBuffer(buffer, false, 0);
                const int mainBusChannels = mainBus.getNumChannels();
                if (mainBusChannels > 0)
                {
                    const float* left = mainBus.getReadPointer(0);
                    const float* right = mainBus.getReadPointer(juce::jmin(1, mainBusChannels - 1));
                    const size_t requiredSamples = (size_t) numSamples * 2u;
                    if (sinkBuffer.maxNumSamples >= requiredSamples)
                    {
                        for (int sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
                        {
                            const float leftSample = juce::jlimit(-1.0f, 1.0f, left[sampleIndex]);
                            const float rightSample = juce::jlimit(-1.0f, 1.0f, right[sampleIndex]);
                            sinkBuffer.samples[(size_t) sampleIndex * 2u] = ableton::util::floatToInt16(leftSample);
                            sinkBuffer.samples[(size_t) sampleIndex * 2u + 1u] = ableton::util::floatToInt16(rightSample);
                        }

                        const double beatsAtBufferBegin = linkSessionState->beatAtTime(linkBufferTime, linkAudioQuantum);
                        sinkBuffer.commit(*linkSessionState,
                                          beatsAtBufferBegin,
                                          linkAudioQuantum,
                                          (size_t) numSamples,
                                          2u,
                                          (uint32_t) std::llround(juce::jmax(1.0, getSampleRate())));
                    }
                }
            }
        }
    }

    masterPeak.store(maxSample);
    masterPeakL.store(maxSampleL);
    masterPeakR.store(maxSampleR);
}

// Called from NJClient::on_new_interval() in the AUDIO THREAD at sample-accurate timing.
void NinjamVst3AudioProcessor::NewIntervalCallback_cb(void* /*userData*/, NJClient* /*inst*/)
{
}

void NinjamVst3AudioProcessor::IntervalChunkCallback_cb(void* /*userData*/, NJClient* /*inst*/,
    const char* /*username*/, int /*chidx*/, unsigned int /*fourcc*/,
    const unsigned char* /*guid*/, const void* /*data*/, int /*dataLen*/, int /*flags*/)
{
}

void NinjamVst3AudioProcessor::IntervalMediaItem_Callback(void* userData, NJClient* /*inst*/,
    const char* username, int /*chidx*/, unsigned int fourcc,
    const unsigned char* /*guid*/, const void* data, int dataLen)
{
    if (!username || !data || dataLen <= 0) return;
    if (fourcc == kSyncSignalFourcc)
    {
        auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
        const juce::String sender = juce::String::fromUTF8(username);
        const juce::String msg    = juce::String::fromUTF8(static_cast<const char*>(data), dataLen);
        const juce::var parsed    = juce::JSON::parse(msg);
        if (auto* obj = parsed.getDynamicObject())
        {
            const juce::String type    = obj->getProperty("sig").toString();
            const juce::String payload = obj->getProperty("data").toString();
            if (type.isNotEmpty() && payload.isNotEmpty())
                self->processSyncSignal(sender, type, payload);
        }
        return;
    }
    if (fourcc != kOpusSyncFourcc) return;
    auto* self = static_cast<NinjamVst3AudioProcessor*>(userData);
    const juce::String sender = juce::String::fromUTF8(username);
    const juce::String payload = juce::String::fromUTF8(static_cast<const char*>(data), dataLen);
    vlogStr("IntervalMediaItem opusSyncFourcc from=" + sender);

    juce::var parsed = juce::JSON::parse(payload);
    bool supportsOpus = false;
    bool multiChanEnabled = false;
    int peerNumChannels = 1;
    juce::String userId = normaliseOpusPeerId(sender);
    juce::String clientId;
    juce::String appFamily;
    int handshakeVersion = 0;
    juce::String runtimeFormat;
    juce::String pluginVersion;
    if (auto* obj = parsed.getDynamicObject())
    {
        const juce::String supports = obj->getProperty("supportsOpus").toString();
        supportsOpus = supports == "1" || supports.equalsIgnoreCase("true");
        const juce::String enabledStr = obj->getProperty("enabled").toString();
        multiChanEnabled = enabledStr == "1" || enabledStr.equalsIgnoreCase("true");
        const juce::var numChVar = obj->getProperty("numChannels");
        if (!numChVar.isVoid()) peerNumChannels = juce::jmax(1, (int)numChVar);
        juce::String payloadUserId = obj->getProperty("userId").toString();
        if (payloadUserId.isNotEmpty())
            userId = normaliseOpusPeerId(payloadUserId);
        clientId = obj->getProperty("clientId").toString().trim();
        appFamily = obj->getProperty("appFamily").toString().trim();
        handshakeVersion = (int)obj->getProperty("handshakeVersion");
        runtimeFormat = obj->getProperty("runtimeFormat").toString().trim();
        pluginVersion = obj->getProperty("pluginVersion").toString().trim();
    }
    else { vlogStr("[MCExit1] JSON parse failed from=" + sender + " payloadLen=" + juce::String((int)payload.length()) + " first100=" + payload.substring(0, 100)); return; }

    const bool isLocalClient = clientId.isNotEmpty() ? (clientId == self->opusSyncInstanceId)
                                                      : (userId == normaliseOpusPeerId(self->currentUser));
    const bool sameAppFamily = appFamily.isEmpty() || appFamily == opusSyncAppFamily;
    const bool compatibleHandshake = handshakeVersion <= 0 || handshakeVersion == opusSyncHandshakeVersion;
    const juce::String peerKey = clientId.isNotEmpty() ? clientId : userId;
    vlogStr("[MCGuard] peerKey='" + peerKey + "' userId='" + userId + "' isLocal=" + juce::String(isLocalClient ? 1 : 0) + " sameFamily=" + juce::String(sameAppFamily ? 1 : 0) + " compatHS=" + juce::String(compatibleHandshake ? 1 : 0) + " supportsOpus=" + juce::String(supportsOpus ? 1 : 0));
    if (peerKey.isEmpty() || userId.isEmpty() || isLocalClient) return;

    bool recognizedNow = false;
    juce::String recognizedMessage;
    {
        juce::ScopedLock lock(self->opusSyncPeerLock);
        if (supportsOpus && sameAppFamily && compatibleHandshake)
        {
            const bool wasKnown = self->opusSyncPeers.find(peerKey) != self->opusSyncPeers.end();
            auto& peer = self->opusSyncPeers[peerKey];
            const bool wasMultiChan = peer.multiChanEnabled;
            peer.userId = userId;
            peer.supportsOpus = true;
            peer.multiChanEnabled = multiChanEnabled;
            peer.numChannels = peerNumChannels;
            peer.appFamily = appFamily;
            peer.handshakeVersion = handshakeVersion;
            peer.runtimeFormat = runtimeFormat;
            peer.pluginVersion = pluginVersion;
            peer.lastSeenMs = juce::Time::getMillisecondCounterHiRes();
            const juce::String peerLabel = sender.isNotEmpty() ? sender : userId;
            if (!wasKnown)
            {
                juce::String peerInfo = peer.runtimeFormat;
                if (peer.pluginVersion.isNotEmpty())
                {
                    if (peerInfo.isNotEmpty()) peerInfo << " ";
                    peerInfo << peer.pluginVersion;
                }
                recognizedMessage = "Multi Client Detected: " + peerLabel;
                if (peerInfo.isNotEmpty()) recognizedMessage << " (" << peerInfo << ")";
                if (multiChanEnabled) recognizedMessage << " [MultiChannel ON]";
                recognizedNow = true;
            }
            else if (multiChanEnabled && !wasMultiChan)
            {
                recognizedMessage = "MultiChannel Detected: " + peerLabel;
                recognizedNow = true;
            }
            else if (!multiChanEnabled && wasMultiChan)
            {
                recognizedMessage = "MultiChannel Off: " + peerLabel;
                recognizedNow = true;
            }
        }
        else
            self->opusSyncPeers.erase(peerKey);
    }
    if (recognizedNow)
    {
        juce::ScopedLock lock(self->chatLock);
        self->chatHistory.add(recognizedMessage);
        self->chatSenders.add("");
        self->chatRevision.fetch_add(1);
        if (self->chatHistory.size() > 100)
        {
            self->chatHistory.removeRange(0, self->chatHistory.size() - 100);
            self->chatSenders.removeRange(0, juce::jmax(0, self->chatSenders.size() - 100));
        }
    }
    vlogStr("[MCRefresh] IntervalMediaItem_Callback updated peer cache. peerKey='" + peerKey + "' userId='" + userId + "' multiChanEnabled=" + juce::String(multiChanEnabled ? 1 : 0) + " nCh=" + juce::String(peerNumChannels));
}

void NinjamVst3AudioProcessor::setSyncToHost(bool shouldSync)
{
    setSyncMode(shouldSync ? SyncMode::host : SyncMode::off);
}

NinjamVst3AudioProcessor::SyncMode NinjamVst3AudioProcessor::getSyncMode() const
{
    return syncMode.load(std::memory_order_relaxed);
}

bool NinjamVst3AudioProcessor::isTransportSyncEnabled() const
{
    return getSyncMode() != SyncMode::off;
}

void NinjamVst3AudioProcessor::setSyncMode(SyncMode newMode)
{
    syncMode.store(newMode, std::memory_order_relaxed);
    hostWasPlaying.store(false);
    linkWasPlaying.store(false);
    bool hostIsPlayingNow = false;
    bool linkIsPlayingNow = false;
    double linkPhaseBeats = 0.0;
    const double linkQuantum = juce::jmax(1.0, (double) getBPI());

    if (newMode == SyncMode::host)
    {
        const juce::ScopedLock lock(transportLock);
        hostIsPlayingNow = lastHostPositionValid.load(std::memory_order_relaxed)
            && lastHostPosition.isPlaying;
    }

    refreshAbletonLinkActivation();

    if (newMode == SyncMode::abletonLink && abletonLink != nullptr)
    {
        auto sessionState = abletonLink->captureAppSessionState();
        const auto now = abletonLink->clock().micros();
        linkIsPlayingNow = sessionState.isPlaying();
        linkPhaseBeats = sessionState.phaseAtTime(now, linkQuantum);
        const juce::ScopedLock lock(linkTransportStateLock);
        lastLinkTempo = sessionState.tempo();
        lastLinkPhaseBeats = linkPhaseBeats;
        lastLinkPeerCount = (int) abletonLink->numPeers();
        lastLinkIsPlaying = linkIsPlayingNow;
    }

    const bool waitForRestart = (newMode == SyncMode::host && hostIsPlayingNow)
        || (newMode == SyncMode::abletonLink && linkIsPlayingNow);

    syncAwaitingHostRestart.store(waitForRestart);
    syncWaitForInterval.store(false);
    syncTargetInterval.store(-1);
    syncDisplayIntervalOffset.store(intervalIndex.load());
    syncDisplayPositionOffset.store(0);

    if (newMode == SyncMode::host)
        primeSyncTransportStart();
    else if (newMode == SyncMode::abletonLink && linkIsPlayingNow)
    {
        hostWasPlaying.store(false);
        linkWasPlaying.store(false);
    }
}

bool NinjamVst3AudioProcessor::isSyncToHostEnabled() const
{
    return getSyncMode() == SyncMode::host;
}

bool NinjamVst3AudioProcessor::isAbletonLinkTransportEnabled() const
{
    return getSyncMode() == SyncMode::abletonLink;
}

void NinjamVst3AudioProcessor::setSyncStartCompensationMs(float ms)
{
    syncStartCompensationMs.store(juce::jlimit(0.0f, 250.0f, ms));
}

float NinjamVst3AudioProcessor::getSyncStartCompensationMs() const
{
    return syncStartCompensationMs.load();
}

int NinjamVst3AudioProcessor::getSyncStartCompensationSamples() const
{
    const double sampleRate = getSampleRate();
    if (sampleRate <= 1.0)
        return 0;

    const double compensationSamples = (double) syncStartCompensationMs.load() * sampleRate / 1000.0;
    return juce::jmax(0, (int) std::llround(compensationSamples));
}

void NinjamVst3AudioProcessor::primeSyncTransportStart(const juce::AudioPlayHead::CurrentPositionInfo* hostInfo)
{
    ninjamClient.ResetTransportPhase();
    ninjamClient.ResetLocalBroadcastState();

    int intervalLength = 0;
    ninjamClient.GetPosition(nullptr, &intervalLength);

    int startPositionSamples = getSyncStartCompensationSamples();
    if (hostInfo != nullptr)
        startPositionSamples += computeJamTabaHostSyncStartPositionSamples(*hostInfo, getSampleRate());

    const int displayOffset = normaliseSignedIntervalPosition(startPositionSamples, intervalLength);

    ninjamClient.SetTransportPosition(displayOffset);
    syncDisplayPositionOffset.store(displayOffset);
}

void NinjamVst3AudioProcessor::primeLinkTransportStart(double phaseBeats, double quantum, double tempoBpm)
{
    ninjamClient.ResetTransportPhase();
    ninjamClient.ResetLocalBroadcastState();

    int intervalLength = 0;
    ninjamClient.GetPosition(nullptr, &intervalLength);
    if (intervalLength <= 0)
        return;

    int startPositionSamples = computeLinkSyncStartPositionSamples(phaseBeats,
                                                                   quantum,
                                                                   tempoBpm,
                                                                   getSampleRate());
    startPositionSamples += getSyncStartCompensationSamples();

    const int displayOffset = normaliseSignedIntervalPosition(startPositionSamples, intervalLength);
    ninjamClient.SetTransportPosition(displayOffset);
    syncDisplayPositionOffset.store(displayOffset);
}

bool NinjamVst3AudioProcessor::getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const
{
    const juce::ScopedLock lock(transportLock);
    if (!lastHostPositionValid.load(std::memory_order_relaxed))
        return false;

    info = lastHostPosition;
    return true;
}

void NinjamVst3AudioProcessor::setLinkAudioEnabled(bool shouldEnable)
{
    linkAudioEnabled.store(shouldEnable, std::memory_order_relaxed);
    refreshAbletonLinkActivation();
}

bool NinjamVst3AudioProcessor::isLinkAudioEnabled() const
{
    return linkAudioEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setLinkAudioSendEnabled(bool shouldEnable)
{
    linkAudioSendEnabled.store(shouldEnable, std::memory_order_relaxed);
    rebuildLinkAudioEndpoints();
}

bool NinjamVst3AudioProcessor::isLinkAudioSendEnabled() const
{
    return linkAudioSendEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setLinkAudioReceiveEnabled(bool shouldEnable)
{
    linkAudioReceiveEnabled.store(shouldEnable, std::memory_order_relaxed);
    rebuildLinkAudioEndpoints();
}

bool NinjamVst3AudioProcessor::isLinkAudioReceiveEnabled() const
{
    return linkAudioReceiveEnabled.load(std::memory_order_relaxed);
}

void NinjamVst3AudioProcessor::setLinkAudioReceiveSelection(const juce::String& channelKey)
{
    {
        const juce::ScopedLock lock(linkAudioSelectionLock);
        linkAudioReceiveSelection = channelKey.trim();
    }
    rebuildLinkAudioEndpoints();
}

juce::String NinjamVst3AudioProcessor::getLinkAudioReceiveSelection() const
{
    const juce::ScopedLock lock(linkAudioSelectionLock);
    return linkAudioReceiveSelection;
}

std::vector<NinjamVst3AudioProcessor::LinkAudioChannelInfo> NinjamVst3AudioProcessor::getLinkAudioAvailableChannels() const
{
    std::vector<LinkAudioChannelInfo> result;
    if (abletonLink == nullptr || !isLinkAudioEnabled())
        return result;

    const juce::String localPeerName = getLinkPeerName();
    for (const auto& channel : abletonLink->channels())
    {
        LinkAudioChannelInfo info;
        info.name = juce::String::fromUTF8(channel.name.c_str()).trim();
        info.peerName = juce::String::fromUTF8(channel.peerName.c_str()).trim();
        if (info.name.isEmpty())
            info.name = "Audio";
        if (info.peerName.isEmpty())
            info.peerName = "Link";
        if (info.peerName == localPeerName)
            continue;
        info.key = buildLinkAudioChannelKey(info.peerName, info.name);
        result.push_back(std::move(info));
    }

    return result;
}

double NinjamVst3AudioProcessor::getLinkTempoBpm() const
{
    const juce::ScopedLock lock(linkTransportStateLock);
    return lastLinkTempo;
}

bool NinjamVst3AudioProcessor::isLinkTransportPlaying() const
{
    const juce::ScopedLock lock(linkTransportStateLock);
    return lastLinkIsPlaying;
}

int NinjamVst3AudioProcessor::getLinkPeerCount() const
{
    const juce::ScopedLock lock(linkTransportStateLock);
    return lastLinkPeerCount;
}

void NinjamVst3AudioProcessor::refreshAbletonLinkActivation()
{
    if (abletonLink == nullptr)
        abletonLink = std::make_unique<ableton::LinkAudio>(120.0, getLinkPeerName().toStdString());

    abletonLink->setPeerName(getLinkPeerName().toStdString());

    const bool transportEnabled = isAbletonLinkTransportEnabled();
    const bool audioEnabled = isLinkAudioEnabled();

    abletonLink->enable(transportEnabled || audioEnabled);
    abletonLink->enableStartStopSync(transportEnabled);
    abletonLink->enableLinkAudio(audioEnabled);

    if (!transportEnabled)
    {
        linkWasPlaying.store(false);
        const juce::ScopedLock lock(linkTransportStateLock);
        lastLinkIsPlaying = false;
    }

    lastLinkAudioEndpointRefreshMs = 0.0;
    rebuildLinkAudioEndpoints();
}

void NinjamVst3AudioProcessor::rebuildLinkAudioEndpoints()
{
    std::unique_ptr<ableton::LinkAudioSink> newSink;
    std::unique_ptr<ableton::LinkAudioSource> newSource;
    std::map<juce::String, std::unique_ptr<ableton::LinkAudioSink>> newRemoteSinks;

    if (abletonLink != nullptr && isLinkAudioEnabled())
    {
        if (isLinkAudioSendEnabled())
            newSink = std::make_unique<ableton::LinkAudioSink>(*abletonLink, std::string("Main Mix"), linkAudioMaxNumSamples);

        if (isLinkAudioSendEnabled())
        {
            const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
            for (const auto& pair : remoteLinkAudioOutputPairs)
            {
                const std::string channelName = pair.first.trim().isNotEmpty()
                    ? pair.first.trim().toStdString()
                    : std::string("Remote");
                newRemoteSinks.emplace(pair.first,
                                       std::make_unique<ableton::LinkAudioSink>(*abletonLink, channelName, linkAudioMaxNumSamples));
            }
        }

        if (isLinkAudioReceiveEnabled())
        {
            const juce::String selectedKey = getLinkAudioReceiveSelection();
            if (selectedKey.isNotEmpty())
            {
                for (const auto& channel : abletonLink->channels())
                {
                    const juce::String peerName = juce::String::fromUTF8(channel.peerName.c_str()).trim();
                    const juce::String channelName = juce::String::fromUTF8(channel.name.c_str()).trim();
                    if (buildLinkAudioChannelKey(peerName, channelName) != selectedKey)
                        continue;

                    newSource = std::make_unique<ableton::LinkAudioSource>(
                        *abletonLink,
                        channel.id,
                        [this](ableton::LinkAudioSource::BufferHandle bufferHandle)
                        {
                            if (bufferHandle.samples == nullptr)
                                return;

                            const size_t totalSamples = bufferHandle.info.numChannels * bufferHandle.info.numFrames;
                            if (totalSamples == 0)
                                return;

                            constexpr size_t batchSize = 512;
                            float left[batchSize] {};
                            float right[batchSize] {};
                            const size_t sourceChannels = bufferHandle.info.numChannels;
                            const bool isStereo = sourceChannels >= 2;
                            size_t framesLeft = bufferHandle.info.numFrames;
                            size_t sourceFrameOffset = 0;

                            while (framesLeft > 0)
                            {
                                const size_t framesThisBatch = juce::jmin(framesLeft, batchSize);
                                for (size_t frame = 0; frame < framesThisBatch; ++frame)
                                {
                                    const size_t sourceIndex = (sourceFrameOffset + frame) * sourceChannels;
                                    left[frame] = ableton::util::int16ToFloat<float>(bufferHandle.samples[sourceIndex]);
                                    right[frame] = isStereo
                                        ? ableton::util::int16ToFloat<float>(bufferHandle.samples[sourceIndex + 1u])
                                        : left[frame];
                                }

                                const size_t writtenFrames = linkAudioReceiveRing.write(left, right, framesThisBatch);
                                linkAudioFramesReceived.fetch_add((juce::uint64)writtenFrames, std::memory_order_relaxed);
                                if (writtenFrames < framesThisBatch)
                                    linkAudioFramesDropped.fetch_add((juce::uint64)(framesThisBatch - writtenFrames), std::memory_order_relaxed);

                                sourceFrameOffset += framesThisBatch;
                                framesLeft -= framesThisBatch;
                            }
                        });
                    break;
                }
            }
        }
    }

    const bool hasNewReceiveEndpoint = newSource != nullptr;
    const bool receiveEndpointChanged = hasNewReceiveEndpoint || (abletonLinkSource != nullptr && newSource == nullptr);
    {
        const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
        abletonLinkSink = std::move(newSink);
        abletonLinkSource = std::move(newSource);
        remoteLinkAudioSinks = std::move(newRemoteSinks);
    }

    if (receiveEndpointChanged || !hasNewReceiveEndpoint)
        linkAudioReceiveRing.reset();

    if (hasNewReceiveEndpoint)
        linkAudioReceiveSelectedMissingSinceMs = 0.0;
}

void NinjamVst3AudioProcessor::mixReceivedLinkAudioIntoBuffer(juce::AudioBuffer<float>& buffer, int numSamples)
{
    juce::ignoreUnused(buffer, numSamples);
}

juce::String NinjamVst3AudioProcessor::buildLinkAudioChannelKey(const juce::String& peerName, const juce::String& channelName) const
{
    return peerName.trim() + "::" + channelName.trim();
}

juce::String NinjamVst3AudioProcessor::getLinkPeerName() const
{
    const juce::String user = currentUser.trim();
    return user.isNotEmpty() ? (juce::String(JucePlugin_Name) + " " + user) : juce::String(JucePlugin_Name);
}

void NinjamVst3AudioProcessor::setMtcOutputEnabled(bool shouldEnable)
{
    mtcOutputEnabled.store(shouldEnable);
}

bool NinjamVst3AudioProcessor::isMtcOutputEnabled() const
{
    return mtcOutputEnabled.load();
}

void NinjamVst3AudioProcessor::setMtcFrameRate(int fps)
{
    int mapped = 30;
    if (fps == 24 || fps == 25 || fps == 30 || fps == 2997)
        mapped = fps;
    mtcFrameRateFps.store(mapped);
}

int NinjamVst3AudioProcessor::getMtcFrameRate() const
{
    return mtcFrameRateFps.load();
}

bool NinjamVst3AudioProcessor::isStandaloneInstance() const
{
    return isStandaloneWrapper();
}

std::vector<NinjamVst3AudioProcessor::MidiControllerEvent> NinjamVst3AudioProcessor::popPendingMidiControllerEvents()
{
    std::vector<MidiControllerEvent> events;
    const juce::SpinLock::ScopedLockType midiQueueLock(midiEventQueueLock);
    events.swap(pendingMidiControllerEvents);
    return events;
}

std::vector<NinjamVst3AudioProcessor::OscRelayEvent> NinjamVst3AudioProcessor::popPendingOscRelayEvents()
{
    std::vector<OscRelayEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(inboundOscRelayQueueLock);
        events.swap(pendingInboundOscRelayEvents);
    }

    if (events.empty())
        return {};

    const juce::String learnSource = getMidiLearnInputDeviceId();
    if (!(learnSource == "__learn_relay__" || learnSource.startsWith("__learn_relay__:")))
        return {};

    if (learnSource == "__learn_relay__" || learnSource == "__learn_relay__:*")
        return events;

    const juce::String desired = learnSource.fromFirstOccurrenceOf("__learn_relay__:", false, false).trim();
    if (desired.isEmpty() || desired == "*")
        return events;

    const juce::String desiredKey = normaliseOpusPeerId(desired);
    if (desiredKey.isEmpty())
        return {};

    std::vector<OscRelayEvent> filtered;
    filtered.reserve(events.size());
    for (const auto& e : events)
        if (e.senderKey == desiredKey)
            filtered.push_back(e);
    return filtered;
}

void NinjamVst3AudioProcessor::setMidiRelayTarget(const juce::String& targetUser)
{
    const juce::ScopedLock lock(midiRelayTargetLock);
    midiRelayTarget = targetUser.isNotEmpty() ? targetUser : "*";
}

juce::String NinjamVst3AudioProcessor::getMidiRelayTarget() const
{
    const juce::ScopedLock lock(midiRelayTargetLock);
    return midiRelayTarget.isNotEmpty() ? midiRelayTarget : "*";
}

void NinjamVst3AudioProcessor::setMidiLearnStateJson(const juce::String& json)
{
    const juce::ScopedLock lock(learnStateLock);
    midiLearnStateJson = json;
}

juce::String NinjamVst3AudioProcessor::getMidiLearnStateJson() const
{
    const juce::ScopedLock lock(learnStateLock);
    return midiLearnStateJson;
}

void NinjamVst3AudioProcessor::setOscLearnStateJson(const juce::String& json)
{
    const juce::ScopedLock lock(learnStateLock);
    oscLearnStateJson = json;
}

juce::String NinjamVst3AudioProcessor::getOscLearnStateJson() const
{
    const juce::ScopedLock lock(learnStateLock);
    return oscLearnStateJson;
}

void NinjamVst3AudioProcessor::setMidiLearnInputDeviceId(const juce::String& deviceId)
{
    const juce::ScopedLock lock(learnStateLock);
    midiLearnInputDeviceId = deviceId;
}

juce::String NinjamVst3AudioProcessor::getMidiLearnInputDeviceId() const
{
    const juce::ScopedLock lock(learnStateLock);
    return midiLearnInputDeviceId;
}

void NinjamVst3AudioProcessor::setMidiRelayInputDeviceId(const juce::String& deviceId)
{
    const juce::ScopedLock lock(learnStateLock);
    midiRelayInputDeviceId = deviceId;
}

juce::String NinjamVst3AudioProcessor::getMidiRelayInputDeviceId() const
{
    const juce::ScopedLock lock(learnStateLock);
    return midiRelayInputDeviceId;
}

void NinjamVst3AudioProcessor::enqueueExternalMidiControllerEvent(const MidiControllerEvent& event, bool forLearn, bool forRelay)
{
    if (forLearn)
    {
        const juce::SpinLock::ScopedLockType midiQueueLock(midiEventQueueLock);
        pendingMidiControllerEvents.push_back(event);
        if (pendingMidiControllerEvents.size() > 512)
            pendingMidiControllerEvents.erase(pendingMidiControllerEvents.begin(), pendingMidiControllerEvents.begin() + (long long)(pendingMidiControllerEvents.size() - 512));
    }

    if (forRelay)
    {
        const juce::SpinLock::ScopedLockType relayQueueLock(outboundMidiRelayQueueLock);
        pendingOutboundMidiRelayEvents.push_back(event);
        if (pendingOutboundMidiRelayEvents.size() > 512)
            pendingOutboundMidiRelayEvents.erase(pendingOutboundMidiRelayEvents.begin(), pendingOutboundMidiRelayEvents.begin() + (long long)(pendingOutboundMidiRelayEvents.size() - 512));
    }
}

void NinjamVst3AudioProcessor::enqueueOutboundOscRelayEvent(const OscRelayEvent& event)
{
    if (event.address.isEmpty())
        return;
    const juce::SpinLock::ScopedLockType lock(outboundOscRelayQueueLock);
    pendingOutboundOscRelayEvents.push_back(event);
    if (pendingOutboundOscRelayEvents.size() > 512)
        pendingOutboundOscRelayEvents.erase(pendingOutboundOscRelayEvents.begin(), pendingOutboundOscRelayEvents.begin() + (long long)(pendingOutboundOscRelayEvents.size() - 512));
}

void NinjamVst3AudioProcessor::flushOutboundMidiRelayEvents()
{
    std::vector<MidiControllerEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(outboundMidiRelayQueueLock);
        events.swap(pendingOutboundMidiRelayEvents);
    }

    if (events.empty())
        return;

    const juce::String targetsRaw = getMidiRelayTarget().trim();
    juce::StringArray targets;
    if (targetsRaw.isEmpty() || targetsRaw == "*")
    {
        targets.add("*");
    }
    else
    {
        targets.addTokens(targetsRaw, ",", "");
        targets.trim();
        targets.removeEmptyStrings();
        targets.removeDuplicates(true);
        if (targets.isEmpty())
            targets.add("*");
    }

    const juce::String userId = currentUser;
    for (const auto& event : events)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("userId", userId);
        obj->setProperty("isController", event.isController);
        obj->setProperty("midiChannel", event.midiChannel);
        obj->setProperty("number", event.number);
        obj->setProperty("value", event.value);
        obj->setProperty("normalized", event.normalized);
        obj->setProperty("isNoteOn", event.isNoteOn);
        const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
        for (const auto& target : targets)
            sendSideSignal(target, "midiRelay", payload);
    }
}

void NinjamVst3AudioProcessor::flushOutboundOscRelayEvents()
{
    std::vector<OscRelayEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(outboundOscRelayQueueLock);
        events.swap(pendingOutboundOscRelayEvents);
    }

    if (events.empty())
        return;

    const juce::String targetsRaw = getMidiRelayTarget().trim();
    juce::StringArray targets;
    if (targetsRaw.isEmpty() || targetsRaw == "*")
    {
        targets.add("*");
    }
    else
    {
        targets.addTokens(targetsRaw, ",", "");
        targets.trim();
        targets.removeEmptyStrings();
        targets.removeDuplicates(true);
        if (targets.isEmpty())
            targets.add("*");
    }

    const juce::String userId = currentUser;
    for (const auto& event : events)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("userId", userId);
        obj->setProperty("address", event.address);
        obj->setProperty("normalized", (double)event.normalized);
        obj->setProperty("binaryOn", event.binaryOn);
        const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
        for (const auto& target : targets)
            sendSideSignal(target, "oscRelay", payload);
    }
}

void NinjamVst3AudioProcessor::injectInboundMidiRelayEvents(juce::MidiBuffer& midiMessages)
{
    std::vector<MidiControllerEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(inboundMidiRelayQueueLock);
        events.swap(pendingInboundMidiRelayEvents);
    }

    for (const auto& event : events)
    {
        if (event.isController)
            midiMessages.addEvent(juce::MidiMessage::controllerEvent(event.midiChannel, event.number, event.value), 0);
        else if (event.isNoteOn)
            midiMessages.addEvent(juce::MidiMessage::noteOn(event.midiChannel, event.number, (juce::uint8)event.value), 0);
        else
            midiMessages.addEvent(juce::MidiMessage::noteOff(event.midiChannel, event.number), 0);
    }
}

bool NinjamVst3AudioProcessor::isStandaloneWrapper() const
{
    return wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

int NinjamVst3AudioProcessor::getDisplayIntervalIndex() const
{
    const int absolute = intervalIndex.load();
    if (!isTransportSyncEnabled())
        return absolute;
    if (!hostWasPlaying.load())
        return 0;
    const int base = syncDisplayIntervalOffset.load();
    return juce::jmax(0, absolute - base);
}

void NinjamVst3AudioProcessor::emitMidiTimecode(juce::MidiBuffer& midiMessages, int numSamples, int pos, int length)
{
    const double sampleRate = getSampleRate();
    if (sampleRate <= 1.0 || numSamples <= 0)
        return;

    const bool mtcEnabled = isMtcOutputEnabled();
    const int fpsSetting = getMtcFrameRate();
    const double fps = fpsSetting == 2997 ? 29.97 : (double)fpsSetting;
    const juce::uint8 rateCode = fpsSetting == 24 ? 0x00 : fpsSetting == 25 ? 0x01 : fpsSetting == 2997 ? 0x02 : 0x03;

    const bool waitingForStart = isTransportSyncEnabled() && (!hostWasPlaying.load() || syncWaitForInterval.load());
    const bool shouldRun = (length > 0) && !waitingForStart;

    auto sendLocate = [&midiMessages, rateCode](int sampleOffset, int hours, int minutes, int seconds, int frames)
    {
        const juce::uint8 hr = (juce::uint8)(((rateCode & 0x03u) << 5) | ((juce::uint8)hours & 0x1Fu));
        const juce::uint8 sysex[] = { 0xF0, 0x7F, 0x7F, 0x01, 0x01,
                                      hr,
                                      (juce::uint8)minutes,
                                      (juce::uint8)seconds,
                                      (juce::uint8)frames,
                                      0xF7 };
        midiMessages.addEvent(juce::MidiMessage::createSysExMessage(sysex, (int)sizeof(sysex)), sampleOffset);
    };

    auto getTimecode = [sampleRate, fps](long long timelineSamples)
    {
        if (timelineSamples < 0)
            timelineSamples = 0;
        const double seconds = (double)timelineSamples / sampleRate;
        const long long totalFrames = (long long)std::floor(seconds * fps);
        const int frame = (int)(totalFrames % (long long)std::round(fps));
        const long long totalSeconds = (long long)std::floor((double)totalFrames / fps);
        const int second = (int)(totalSeconds % 60);
        const int minute = (int)((totalSeconds / 60) % 60);
        const int hour = (int)((totalSeconds / 3600) % 24);
        return std::array<int, 4> { hour, minute, second, frame };
    };

    if (!mtcEnabled)
    {
        if (mtcWasRunning)
        {
            midiMessages.addEvent(juce::MidiMessage::midiStop(), 0);
            sendLocate(0, 0, 0, 0, 0);
        }
        mtcWasRunning = false;
        mtcSamplesUntilNextQuarterFrame = 0.0;
        mtcQuarterFramePiece = 0;
        return;
    }

    if (mtcWasRunning && !shouldRun)
    {
        midiMessages.addEvent(juce::MidiMessage::midiStop(), 0);
        sendLocate(0, 0, 0, 0, 0);
        mtcSamplesUntilNextQuarterFrame = 0.0;
        mtcQuarterFramePiece = 0;
    }

    int displayInterval = getDisplayIntervalIndex();
    int timelinePos = 0;
    if (length > 0)
    {
        if (!waitingForStart)
            timelinePos = juce::jlimit(0, juce::jmax(0, length - 1), pos);
    }
    long long blockStartSamples = (long long)displayInterval * (long long)juce::jmax(0, length) + (long long)timelinePos;

    if (!mtcWasRunning && shouldRun)
    {
        const auto tc = getTimecode(blockStartSamples);
        sendLocate(0, tc[0], tc[1], tc[2], tc[3]);
        midiMessages.addEvent(juce::MidiMessage::midiStart(), 0);
        mtcSamplesUntilNextQuarterFrame = 0.0;
        mtcQuarterFramePiece = 0;
    }

    mtcWasRunning = shouldRun;
    if (!shouldRun)
        return;

    const double qfPerSecond = fps * 4.0;
    const double samplesPerQuarterFrame = sampleRate / qfPerSecond;
    double sampleCursor = mtcSamplesUntilNextQuarterFrame;
    if (sampleCursor <= 0.0)
        sampleCursor = samplesPerQuarterFrame;

    while (sampleCursor < (double)numSamples)
    {
        const int eventSample = juce::jlimit(0, numSamples - 1, (int)std::floor(sampleCursor));
        const long long eventTimelineSamples = blockStartSamples + (long long)eventSample;
        const auto tc = getTimecode(eventTimelineSamples);

        const int piece = mtcQuarterFramePiece & 0x07;
        int value = 0;
        switch (piece)
        {
            case 0: value = tc[3] & 0x0F; break;
            case 1: value = (tc[3] >> 4) & 0x01; break;
            case 2: value = tc[2] & 0x0F; break;
            case 3: value = (tc[2] >> 4) & 0x03; break;
            case 4: value = tc[1] & 0x0F; break;
            case 5: value = (tc[1] >> 4) & 0x03; break;
            case 6: value = tc[0] & 0x0F; break;
            case 7: value = ((tc[0] >> 4) & 0x01) | (0x03 << 1); break;
            default: break;
        }

        const juce::uint8 data = (juce::uint8)(((piece & 0x07) << 4) | (value & 0x0F));
        midiMessages.addEvent(juce::MidiMessage(0xF1, data), eventSample);
        mtcQuarterFramePiece = (piece + 1) & 0x07;
        sampleCursor += samplesPerQuarterFrame;
    }

    mtcSamplesUntilNextQuarterFrame = sampleCursor - (double)numSamples;
}

bool NinjamVst3AudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* NinjamVst3AudioProcessor::createEditor()
{
    return new NinjamVst3AudioProcessorEditor (*this);
}

void NinjamVst3AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state("NINJAM_STATE");
    state.setProperty("midiRelayTarget", getMidiRelayTarget(), nullptr);
    state.setProperty("midiLearnStateJson", getMidiLearnStateJson(), nullptr);
    state.setProperty("oscLearnStateJson", getOscLearnStateJson(), nullptr);
    state.setProperty("midiLearnInputDeviceId", getMidiLearnInputDeviceId(), nullptr);
    state.setProperty("midiRelayInputDeviceId", getMidiRelayInputDeviceId(), nullptr);
    state.setProperty("autoTranslate", isAutoTranslateEnabled(), nullptr);
    state.setProperty("translateSourceLang", getTranslateSourceLang(), nullptr);
    state.setProperty("translateTargetLang", getTranslateTargetLang(), nullptr);
    state.setProperty("fxReverbWetDryMix", (double)getFxReverbWetDryMix(), nullptr);
    state.setProperty("fxDelayWetDryMix", (double)getFxDelayWetDryMix(), nullptr);
    state.setProperty("syncMode", (int)getSyncMode(), nullptr);
    state.setProperty("syncStartCompensationMs", (double)getSyncStartCompensationMs(), nullptr);
    state.setProperty("linkAudioEnabled", isLinkAudioEnabled(), nullptr);
    state.setProperty("linkAudioSendEnabled", isLinkAudioSendEnabled(), nullptr);
    state.setProperty("linkAudioReceiveEnabled", isLinkAudioReceiveEnabled(), nullptr);
    state.setProperty("linkAudioReceiveSelection", getLinkAudioReceiveSelection(), nullptr);
    state.setProperty("metronomeMuted", isMetronomeMuted(), nullptr);
    state.setProperty("metronomeVolume", (double)getStoredMetronomeVolume(), nullptr);
    state.setProperty("transmitLocal", isTransmittingLocal(), nullptr);
    state.setProperty("chordDetectionEnabled", isChordDetectionEnabled(), nullptr);
    for (int channel = 0; channel < maxLocalChannels; ++channel)
        state.setProperty("localInput" + juce::String(channel), getLocalChannelInput(channel), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void NinjamVst3AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState == nullptr)
        return;

    const juce::ValueTree state = juce::ValueTree::fromXml(*xmlState);
    if (!state.isValid())
        return;

    setMidiRelayTarget(state.getProperty("midiRelayTarget", "*").toString());
    setMidiLearnStateJson(state.getProperty("midiLearnStateJson", "").toString());
    setOscLearnStateJson(state.getProperty("oscLearnStateJson", "").toString());
    setMidiLearnInputDeviceId(state.getProperty("midiLearnInputDeviceId", "").toString());
    setMidiRelayInputDeviceId(state.getProperty("midiRelayInputDeviceId", "").toString());
    setAutoTranslateEnabled((bool) state.getProperty("autoTranslate", false));
    setTranslateSourceLang(state.getProperty("translateSourceLang", "en").toString());
    setTranslateTargetLang(state.getProperty("translateTargetLang", "system").toString());
    setFxReverbWetDryMix((float)(double)state.getProperty("fxReverbWetDryMix", 1.0));
    setFxDelayWetDryMix((float)(double)state.getProperty("fxDelayWetDryMix", 1.0));
    setSyncMode((SyncMode) (int) state.getProperty("syncMode", (int) SyncMode::off));
    setSyncStartCompensationMs((float)(double)state.getProperty("syncStartCompensationMs", 0.0));
    setLinkAudioEnabled((bool)state.getProperty("linkAudioEnabled", false));
    setLinkAudioSendEnabled((bool)state.getProperty("linkAudioSendEnabled", true));
    setLinkAudioReceiveEnabled((bool)state.getProperty("linkAudioReceiveEnabled", false));
    setLinkAudioReceiveSelection(state.getProperty("linkAudioReceiveSelection", "").toString());
    storedMetronomeVolume.store(juce::jlimit(0.0f, 1.0f, (float)(double)state.getProperty("metronomeVolume", 1.0)));
    setMetronomeMuted((bool)state.getProperty("metronomeMuted", false));
    setTransmitLocal((bool)state.getProperty("transmitLocal", false));
    setChordDetectionEnabled((bool)state.getProperty("chordDetectionEnabled", true));
    for (int channel = 0; channel < maxLocalChannels; ++channel)
        setLocalChannelInput(channel, (int)state.getProperty("localInput" + juce::String(channel), -1));
}

void NinjamVst3AudioProcessor::processPendingIntervalSyncMarkers(int localMarkerBeat, long long localMarkerSampleCount, double intervalDurationMs)
{
    if (ninjamClient.GetStatus() != NJClient::NJC_STATUS_OK)
        return;

    const int safeLocalMarkerBeat = juce::jmax(0, localMarkerBeat);
    const double safeIntervalDurationMs = juce::jmax(1.0, intervalDurationMs);
    const double localMarkerAtMs = juce::Time::getMillisecondCounterHiRes();

    for (;;)
    {
        juce::String senderKey;
        PendingRemoteIntervalStart pending;
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            if (pendingRemoteIntervalStartsByUser.empty())
                break;
            for (auto staleIt = pendingRemoteIntervalStartsByUser.begin(); staleIt != pendingRemoteIntervalStartsByUser.end();)
            {
                if (staleIt->second.receivedSampleCount < 0 && staleIt->second.receivedAtMs <= 0.0)
                    staleIt = pendingRemoteIntervalStartsByUser.erase(staleIt);
                else
                    ++staleIt;
            }
            auto chosenIt = pendingRemoteIntervalStartsByUser.end();
            for (auto it = pendingRemoteIntervalStartsByUser.begin(); it != pendingRemoteIntervalStartsByUser.end(); ++it)
            {
                if (it->second.remoteBeat == safeLocalMarkerBeat && it->second.receivedSampleCount <= localMarkerSampleCount)
                {
                    chosenIt = it;
                    break;
                }
            }
            if (chosenIt == pendingRemoteIntervalStartsByUser.end())
                break;
            pending = chosenIt->second;
            senderKey = pending.senderKey.isNotEmpty()
                ? pending.senderKey
                : chosenIt->first.upToFirstOccurrenceOf(":", false, false);
            pendingRemoteIntervalStartsByUser.erase(chosenIt);
        }
        if (senderKey.isEmpty() || (pending.receivedSampleCount < 0 && pending.receivedAtMs <= 0.0))
            continue;
        double elapsedToNextLocalMarkerMs = -1.0;
        if (pending.receivedAtMs > 0.0 && localMarkerAtMs >= pending.receivedAtMs)
        {
            elapsedToNextLocalMarkerMs = localMarkerAtMs - pending.receivedAtMs;
        }
        else if (pending.receivedSampleCount >= 0)
        {
            const long long elapsedSamples = localMarkerSampleCount - pending.receivedSampleCount;
            if (elapsedSamples < 0)
                continue;
            const double sampleRate = juce::jmax(1.0, getSampleRate());
            elapsedToNextLocalMarkerMs = ((double)elapsedSamples / sampleRate) * 1000.0;
        }
        const double outlierLimitMs = safeIntervalDurationMs * 2.0;
        if (!std::isfinite(elapsedToNextLocalMarkerMs) || elapsedToNextLocalMarkerMs < 0.0 || elapsedToNextLocalMarkerMs > outlierLimitMs)
            continue;
        const int elapsedMs = (int)std::llround(juce::jlimit(0.0, safeIntervalDurationMs, elapsedToNextLocalMarkerMs));
        int averageMs = -1;
        int firmAverageMs = -1;
        int correctedDelayMs = -1;
        const int senderServerLatencyMs = pending.remoteServerLatencyMs >= 0 ? juce::jmax(0, pending.remoteServerLatencyMs) : 0;
        const int receiverServerLatencyMs = juce::jmax(0, localServerLatencyMs.load());
        const int serverRouteLatencyMs = senderServerLatencyMs + receiverServerLatencyMs;
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            auto& avgState = remoteLatencyAverageByUser[senderKey];
            avgState.lastMeasurementMs = (double)elapsedMs;
            bool includeInAverage = true;
            if (avgState.sampleCount >= 3)
            {
                const double baselineMs = avgState.firmAverageMs > 0.0 ? avgState.firmAverageMs : avgState.averageMs;
                const double deltaMs = std::abs((double)elapsedMs - baselineMs);
                const double spikeThresholdMs = juce::jlimit(5.0, 20.0, baselineMs * 0.30 + 2.0);
                if (deltaMs > spikeThresholdMs)
                    includeInAverage = false;
            }
            if (includeInAverage)
            {
                avgState.sampleCount += 1;
                avgState.sumMs += (double)elapsedMs;
                avgState.averageMs = avgState.sumMs / (double)juce::jmax(1, avgState.sampleCount);
                if (avgState.sampleCount == 1)
                    avgState.firmAverageMs = (double)elapsedMs;
                else
                    avgState.firmAverageMs = (avgState.firmAverageMs * 0.88) + ((double)elapsedMs * 0.12);
            }
            if (avgState.sampleCount >= 3)
            {
                averageMs = juce::jmax(0, (int)std::llround(avgState.averageMs));
                firmAverageMs = juce::jmax(0, (int)std::llround(avgState.firmAverageMs));
            }
            else if (avgState.lastMeasurementMs >= 0.0)
            {
                averageMs = juce::jmax(0, (int)std::llround(avgState.lastMeasurementMs));
            }
            if (firmAverageMs >= 0 || averageMs >= 0)
            {
                const double rawDelayMs = (double)(firmAverageMs >= 0 ? firmAverageMs : averageMs);
                correctedDelayMs = juce::jmax(0, (int)std::llround(rawDelayMs) + serverRouteLatencyMs);
            }
        }
        if (correctedDelayMs >= 0)
        {
            const int sourceInterval = pending.remoteIntervalAbsolute >= 0 ? pending.remoteIntervalAbsolute : pending.remoteInterval;
            const long long sourceMarkerKey = makeIntervalSyncMarkerKey(sourceInterval, pending.remoteBeat);
            const juce::String canonicalSenderKey = canonicalDelayUserKey(senderKey);
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            long long priorAppliedMarker = std::numeric_limits<long long>::min();
            auto appliedIt = remoteLatencyLastAppliedIntervalByUser.find(senderKey);
            if (appliedIt != remoteLatencyLastAppliedIntervalByUser.end())
                priorAppliedMarker = appliedIt->second;
            bool shouldApply = (appliedIt == remoteLatencyLastAppliedIntervalByUser.end());
            if (!shouldApply)
            {
                const long long markerDelta = sourceMarkerKey - priorAppliedMarker;
                const bool cadenceReached = markerDelta >= remoteLatencyUpdateCadenceIntervals;
                const bool markerSequenceReset = sourceMarkerKey + intervalSyncMarkerKeyBeatStride < priorAppliedMarker;
                shouldApply = cadenceReached || markerSequenceReset;
            }
            if (shouldApply)
            {
                remoteLatencyFirmDelayMsByUser[senderKey] = correctedDelayMs;
                if (canonicalSenderKey.isNotEmpty())
                    remoteLatencyFirmDelayMsByUser[canonicalSenderKey] = correctedDelayMs;
                lastRemoteServerLatencyMsByUser[senderKey] = senderServerLatencyMs;
                if (canonicalSenderKey.isNotEmpty())
                    lastRemoteServerLatencyMsByUser[canonicalSenderKey] = senderServerLatencyMs;
                remoteLatencyLastAppliedIntervalByUser[senderKey] = sourceMarkerKey;
                if (canonicalSenderKey.isNotEmpty())
                    remoteLatencyLastAppliedIntervalByUser[canonicalSenderKey] = sourceMarkerKey;
                vlogStr("[MCGuard] Applied firm delay for=" + senderKey
                    + " canonical=" + canonicalSenderKey
                    + " delayMs=" + juce::String(correctedDelayMs)
                    + " senderSrv=" + juce::String(senderServerLatencyMs)
                    + " receiverSrv=" + juce::String(receiverServerLatencyMs)
                    + " sourceMarker=" + juce::String((juce::int64)sourceMarkerKey)
                    + " priorApplied=" + juce::String((juce::int64)priorAppliedMarker));
            }
        }
        const juce::String displaySender = pending.displaySender.isNotEmpty() ? pending.displaySender : senderKey;
        const int displaySenderServerLatencyMs = pending.remoteServerLatencyMs >= 0 ? juce::jmax(0, pending.remoteServerLatencyMs) : 0;
        const int displayReceiverServerLatencyMs = juce::jmax(0, localServerLatencyMs.load());
        const int displayServerRouteLatencyMs = displaySenderServerLatencyMs + displayReceiverServerLatencyMs;
        const int displayCorrectedDelayMs = correctedDelayMs >= 0
            ? correctedDelayMs
            : juce::jmax(0, elapsedMs + displayServerRouteLatencyMs);
        juce::String line = displaySender + " " + formatIntervalSyncMarkerBeat(pending.remoteBeat)
            + "->our " + formatIntervalSyncMarkerBeat(safeLocalMarkerBeat) + " " + juce::String(elapsedMs) + "ms";
        if (firmAverageMs >= 0)
            line << " avg " << juce::String(firmAverageMs) << "ms";
        else if (averageMs >= 0)
            line << " avg " << juce::String(averageMs) << "ms";
        if (displayServerRouteLatencyMs > 0)
            line << " srv " << juce::String(displaySenderServerLatencyMs) << "+" << juce::String(displayReceiverServerLatencyMs) << "ms";
        line << " => " << juce::String(displayCorrectedDelayMs) << "ms";
        juce::DynamicObject::Ptr reportObj = new juce::DynamicObject();
        reportObj->setProperty("line", line);
        reportObj->setProperty("interval", pending.remoteIntervalAbsolute >= 0 ? pending.remoteIntervalAbsolute : pending.remoteInterval);
        reportObj->setProperty("beatIndex", pending.remoteBeat);
        reportObj->setProperty("targetUserId", canonicalDelayUserKey(displaySender));
        reportObj->setProperty("elapsedMs", elapsedMs);
        if (averageMs >= 0)
            reportObj->setProperty("avgMs", averageMs);
        if (firmAverageMs >= 0)
            reportObj->setProperty("firmMs", firmAverageMs);
        reportObj->setProperty("senderServerLatencyMs", displaySenderServerLatencyMs);
        reportObj->setProperty("receiverServerLatencyMs", displayReceiverServerLatencyMs);
        reportObj->setProperty("serverRouteLatencyMs", displayServerRouteLatencyMs);
        reportObj->setProperty("correctedDelayMs", displayCorrectedDelayMs);
        reportObj->setProperty("eventId", "latencyReport:" + senderKey + ":" + juce::String(++sideSignalEventCounter));
        const juce::String reportPayload = juce::JSON::toString(juce::var(reportObj.get()));
        sendIntervalSignal("intervalLatencyReport", reportPayload);
    }
}

void NinjamVst3AudioProcessor::timerCallback()
{
    int loopCount = 0;
    while (!ninjamClient.Run() && loopCount < 50)
    {
        loopCount++;
    }

    int status = ninjamClient.GetStatus();
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (status == NJClient::NJC_STATUS_OK && (nowMs - lastRemoteSyncUserPruneMs) >= 350.0)
    {
        pruneDisconnectedRemoteSyncState();
        lastRemoteSyncUserPruneMs = nowMs;
    }

    if (status != lastStatus)
    {
        if (status == NJClient::NJC_STATUS_CANTCONNECT || status == NJClient::NJC_STATUS_INVALIDAUTH)
        {
            juce::String err = juce::String::fromUTF8(ninjamClient.GetErrorStr());
            juce::Logger::writeToLog("NINJAM Error (" + juce::String(status) + "): " + err);
        }
        else if (status == NJClient::NJC_STATUS_OK)
        {
            juce::Logger::writeToLog("NINJAM Connected Successfully");
            opusSyncServerSupported.store(false);
            juce::Logger::writeToLog("Sending VIDEO_CAP 1");
            ninjamClient.ChatMessage_Send("VIDEO_CAP", "1", nullptr, nullptr, nullptr);
            {
                const juce::ScopedLock lock(opusSyncPeerLock);
                opusSyncPeers.clear();
            }
            {
                invalidateIntervalSyncLatencyState(false);
            }
            opusSyncAvailable.store(false);
            opusSyncHasLegacyClients.store(false);
            lastOpusSupportBroadcastMs = 0.0;
            localServerLatencyMs.store(-1);
            lastServerLatencyProbeInterval.store(-1);
            if (!isTransportSyncEnabled())
            {
                syncWaitForInterval.store(false);
                syncTargetInterval.store(-1);
                intervalIndex.store(0);
                lastIntervalPos.store(0);
            }
            lastBroadcastIntervalTag.store(-1);
            setIntervalSyncStatusText({});
            syncLocalIntervalChannelConfig();
        }
        else if (lastStatus == NJClient::NJC_STATUS_OK)
        {
            opusSyncServerSupported.store(false);
            {
                const juce::ScopedLock lock(opusSyncPeerLock);
                opusSyncPeers.clear();
            }
            {
                invalidateIntervalSyncLatencyState(false);
            }
            opusSyncAvailable.store(false);
            opusSyncHasLegacyClients.store(false);
            localServerLatencyMs.store(-1);
            lastServerLatencyProbeInterval.store(-1);
            setIntervalSyncStatusText({});
            lastBroadcastIntervalTag.store(-1);
            applyCodecPreference();
        }
        lastStatus = status;
    }

    if (isLinkAudioEnabled() && isLinkAudioReceiveEnabled())
    {
        const juce::String selectedReceiveKey = getLinkAudioReceiveSelection();
        if (selectedReceiveKey.isNotEmpty())
        {
            bool hasReceiveSource = false;
            std::optional<ableton::ChannelId> currentSourceId;
            {
                const juce::SpinLock::ScopedLockType endpointLock(linkAudioEndpointLock);
                hasReceiveSource = abletonLinkSource != nullptr;
                if (abletonLinkSource != nullptr)
                    currentSourceId = abletonLinkSource->id();
            }

            bool selectedChannelAvailable = false;
            bool sourceMatchesSelectedChannel = false;
            if (abletonLink != nullptr)
            {
                for (const auto& channel : abletonLink->channels())
                {
                    const juce::String peerName = juce::String::fromUTF8(channel.peerName.c_str()).trim();
                    const juce::String channelName = juce::String::fromUTF8(channel.name.c_str()).trim();
                    if (buildLinkAudioChannelKey(peerName, channelName) != selectedReceiveKey)
                        continue;

                    selectedChannelAvailable = true;
                    sourceMatchesSelectedChannel = currentSourceId.has_value() && *currentSourceId == channel.id;
                    break;
                }
            }

            if (selectedChannelAvailable)
                linkAudioReceiveSelectedMissingSinceMs = 0.0;
            else if (linkAudioReceiveSelectedMissingSinceMs <= 0.0)
                linkAudioReceiveSelectedMissingSinceMs = nowMs;

            const bool selectedMissingLongEnough = linkAudioReceiveSelectedMissingSinceMs > 0.0
                && (nowMs - linkAudioReceiveSelectedMissingSinceMs) >= 1500.0;
            const bool shouldCreateReceiveEndpoint = !hasReceiveSource && selectedChannelAvailable;
            const bool shouldSwitchReceiveEndpoint = hasReceiveSource && selectedChannelAvailable && !sourceMatchesSelectedChannel;
            const bool shouldClearReceiveEndpoint = hasReceiveSource && !selectedChannelAvailable && selectedMissingLongEnough;
            const bool shouldRefreshReceiveEndpoint = shouldCreateReceiveEndpoint
                || shouldSwitchReceiveEndpoint
                || shouldClearReceiveEndpoint;

            if (shouldRefreshReceiveEndpoint && (nowMs - lastLinkAudioEndpointRefreshMs) >= 250.0)
            {
                rebuildLinkAudioEndpoints();
                lastLinkAudioEndpointRefreshMs = nowMs;
            }
        }
        else
        {
            linkAudioReceiveSelectedMissingSinceMs = 0.0;
        }
    }

    if (status == NJClient::NJC_STATUS_OK)
    {
        // Rebuild peer capability state only after Run() finishes dispatching inbound callbacks.
        refreshOpusSyncAvailabilityFromUsers();
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        {
            const juce::ScopedLock lock(intervalSyncAnnouncementLock);
            const int displayInterval = getDisplayIntervalIndex();
            if (localIntervalStartMsByInterval.find(displayInterval) == localIntervalStartMsByInterval.end())
                localIntervalStartMsByInterval[displayInterval] = nowMs;
        }
        if (nowMs - lastOpusSupportBroadcastMs >= 1500.0)
        {
            broadcastOpusSyncSupport();
            lastOpusSupportBroadcastMs = nowMs;
        }
        if (nowMs - lastServerLatencyProbeAttemptMs >= 5000.0)
        {
            measureServerLatencyAsync();
            lastServerLatencyProbeAttemptMs = nowMs;
        }

        flushOutboundMidiRelayEvents();
        flushOutboundOscRelayEvents();
    }

    int pos = 0;
    int length = 0;
    ninjamClient.GetPosition(&pos, &length);
    if (length > 0)
    {
        bool forceIntervalHelperPayloadWrite = false;
        const int localBpi = juce::jmax(1, getBPI());
        const double localBpm = juce::jmax(1.0, (double)getBPM());
        const bool timingChanged = lastLatencyTimingBpi != localBpi
            || lastLatencyTimingLength != length
            || std::abs(lastLatencyTimingBpm - localBpm) > 0.001;

        if (timingChanged)
        {
            int timingDelayDeltaMs = 0;
            const bool hadPreviousTiming = lastLatencyTimingBpi > 0 && lastLatencyTimingBpm > 0.0;
            if (hadPreviousTiming)
            {
                const double previousIntervalDurationMs = (60.0 / lastLatencyTimingBpm) * (double)lastLatencyTimingBpi * 1000.0;
                const double newIntervalDurationMs = (60.0 / localBpm) * (double)localBpi * 1000.0;
                if (std::isfinite(previousIntervalDurationMs) && std::isfinite(newIntervalDurationMs)
                    && previousIntervalDurationMs > 0.0 && newIntervalDurationMs > 0.0)
                    timingDelayDeltaMs = (int)std::llround(newIntervalDurationMs - previousIntervalDurationMs);
            }
            if (hadPreviousTiming)
            {
                const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                if (!remoteLatencyFirmDelayMsByUser.empty())
                {
                    const auto refreshId = ++videoBufferRefreshCounter;
                    for (auto& userDelay : remoteLatencyFirmDelayMsByUser)
                    {
                        if (timingDelayDeltaMs != 0)
                            userDelay.second = juce::jmax(0, userDelay.second + timingDelayDeltaMs);
                        remoteVideoBufferRefreshIdByUser[userDelay.first] = refreshId;
                    }
                }
            }
            invalidateIntervalSyncLatencyState(true);
            lastLatencyTimingBpi = localBpi;
            lastLatencyTimingLength = length;
            lastLatencyTimingBpm = localBpm;
            lastBroadcastIntervalTag.store(-1);
            setIntervalSyncStatusText("Interval sync timing changed, recalculating delay...");
            forceIntervalHelperPayloadWrite = true;
            if (timingDelayDeltaMs != 0)
                vlogStr("[MCGuard] Timing changed; adjusted preserved video delays by " + juce::String(timingDelayDeltaMs) + "ms while recalculating");
        }

        if (status == NJClient::NJC_STATUS_OK)
            lastServerLatencyProbeInterval.store(getDisplayIntervalIndex());
        int last = lastIntervalPos.load();
        if (pos < last)
        {
            intervalIndex.fetch_add(1);
            forceIntervalHelperPayloadWrite = true;
            const int localDisplayInterval = getDisplayIntervalIndex();
            const double localIntervalStartMs = juce::Time::getMillisecondCounterHiRes();
            {
                const juce::ScopedLock lock(intervalSyncAnnouncementLock);
                localIntervalStartMsByInterval[localDisplayInterval] = localIntervalStartMs;
                const int minIntervalToKeep = localDisplayInterval - 64;
                for (auto it = localIntervalStartMsByInterval.begin(); it != localIntervalStartMsByInterval.end();)
                {
                    if (it->first < minIntervalToKeep)
                        it = localIntervalStartMsByInterval.erase(it);
                    else
                        ++it;
                }
            }
        }
        const int localDisplayInterval = getDisplayIntervalIndex();
        const int currentBeatIndex = getIntervalBeatIndexForPosition(pos, length, localBpi);
        const int localMarkerBeat = getIntervalSyncMarkerBeatForBeat(currentBeatIndex, localBpi);
        const long long localMarkerKey = makeIntervalSyncMarkerKey(localDisplayInterval, localMarkerBeat);
        const bool markerChanged = lastProcessedIntervalMarkerKey.load() != localMarkerKey;
        if (markerChanged)
        {
            lastProcessedIntervalMarkerKey.store(localMarkerKey);
            if (status == NJClient::NJC_STATUS_OK && currentBeatIndex == localMarkerBeat)
            {
                forceIntervalHelperPayloadWrite = true;
                const long long localMarkerSampleCount = intervalSyncSampleCounter.load(std::memory_order_relaxed);
                const double intervalDurationMs = (60.0 / localBpm) * (double)localBpi * 1000.0;
                processPendingIntervalSyncMarkers(localMarkerBeat, localMarkerSampleCount, intervalDurationMs);
                if (lastBroadcastIntervalTag.load() != localMarkerKey)
                {
                    broadcastIntervalSyncTag("*", localMarkerBeat);
                    lastBroadcastIntervalTag.store(localMarkerKey);
                }
            }
        }
        lastIntervalPos.store(pos);
        if (status == NJClient::NJC_STATUS_OK && videoHelperRunning.load())
        {
            const double elapsedSinceHelperWriteMs = nowMs - lastIntervalHelperPayloadWriteMs;
            if (forceIntervalHelperPayloadWrite
                || lastIntervalHelperPayloadWriteMs <= 0.0
                || elapsedSinceHelperWriteMs >= intervalHelperPayloadMinWriteMs)
            {
                lastIntervalHelperPayloadWriteMs = nowMs;
                writeIntervalHelperJson(pos, length);
            }
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NinjamVst3AudioProcessor();
}
