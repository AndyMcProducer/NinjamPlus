#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <atomic>
#include <thread>
#include <vector>

int NinjamVst3AudioProcessor::measurePendingIntervalForIntegrationTest(int ageMs, bool withAudioGuid, bool hasPlaybackBoundary)
{
    const juce::String sender = "startup-probe";
    const juce::String pendingKey = sender + ":0";
    const juce::String guid = "0123456789abcdef0123456789abcdef";
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    constexpr long long playbackSample = 1000000;
    {
        const juce::ScopedLock lock(intervalSyncAnnouncementLock);
        auto& pending = pendingRemoteIntervalStartsByUser[pendingKey];
        pending.senderKey = sender;
        pending.remoteInterval = 0;
        pending.remoteBeat = 0;
        pending.receivedAtMs = nowMs - ageMs;
        pending.receivedSampleCount = playbackSample - (long long)std::llround(ageMs * getSampleRate() / 1000.0);
        pending.audioGuidHex = withAudioGuid ? guid : juce::String();
        if (hasPlaybackBoundary)
            remoteAudioPlaybackBoundariesByUser[sender][guid] = { playbackSample, nowMs };
    }
    processPendingRemoteAudioPlaybackBoundaries(2000.0);
    processPendingIntervalSyncMarkers(0, playbackSample, 2000.0);
    bool diagnosticEvidenceValid = !hasPlaybackBoundary;
    if (hasPlaybackBoundary)
    {
        const auto diagnostic = getSyncDiagnosticState();
        const auto measurements = diagnostic.getProperty("measurements", juce::var());
        if (const auto* entries = measurements.getArray())
            for (const auto& entry : *entries)
                if (entry.getProperty("audioGuid", juce::var()).toString() == guid)
                    diagnosticEvidenceValid = entry.getProperty("basis", juce::var()).toString() == "audio-guid"
                        && (juce::int64) entry.getProperty("playbackSample", juce::var()) == playbackSample
                        && (double) entry.getProperty("captureIntervalMs", juce::var()) == 2000.0;
    }
    const juce::ScopedLock lock(intervalSyncAnnouncementLock);
    const auto measured = remoteLatencyAverageByUser.find(sender);
    const int result = measured == remoteLatencyAverageByUser.end()
        ? -1 : (int)std::llround(measured->second.lastMeasurementMs);
    pendingRemoteIntervalStartsByUser.erase(pendingKey);
    remoteAudioPlaybackBoundariesByUser.erase(sender);
    remoteLatencyAverageByUser.erase(sender);
    return diagnosticEvidenceValid ? result : -2;
}

bool NinjamVst3AudioProcessor::verifyLateAudioGuidForIntegrationTest()
{
    const juce::String sender = "late-guid-probe";
    constexpr long long playbackSample = 1000000;
    bool ok = true;
    for (int marker = 0; marker < 3; ++marker)
    {
        const auto key = sender + ":" + juce::String(marker);
        const auto guid = juce::String::repeatedString("a", 31) + juce::String(marker);
        const double now = juce::Time::getMillisecondCounterHiRes();
        auto& pending = pendingRemoteIntervalStartsByUser[key];
        pending.senderKey = sender;
        pending.remoteInterval = marker;
        pending.audioGuidHex = guid;
        pending.receivedAtMs = now - 3990.0;
        pending.receivedSampleCount = playbackSample - (long long)std::llround(3990.0 * getSampleRate() / 1000.0);
        // Repeated timer/beat polls must neither consume nor measure an
        // unmatched GUID, even after the former 1.5-interval timeout.
        processPendingIntervalSyncMarkers(0, playbackSample, 2000.0);
        processPendingIntervalSyncMarkers(0, playbackSample, 2000.0);
        const auto before = remoteLatencyAverageByUser.find(sender);
        ok = ok && pendingRemoteIntervalStartsByUser.count(key) == 1
            && remoteLatencyFirmDelayMsByUser.count(sender) == 0
            && (before == remoteLatencyAverageByUser.end() ? 0 : before->second.sampleCount) == marker;
        remoteAudioPlaybackBoundariesByUser[sender][guid] = { playbackSample, now };
        processPendingRemoteAudioPlaybackBoundaries(2000.0);
        processPendingRemoteAudioPlaybackBoundaries(2000.0);
        const auto after = remoteLatencyAverageByUser.find(sender);
        ok = ok && after != remoteLatencyAverageByUser.end()
            && after->second.sampleCount == marker + 1
            && std::abs(after->second.lastMeasurementMs - 5990.0) < 1.0
            && after->second.measurementBasis == "audio-guid"
            && pendingRemoteIntervalStartsByUser.count(key) == 0;
    }
    const auto published = remoteLatencyFirmDelayMsByUser.find(sender);
    ok = ok && published != remoteLatencyFirmDelayMsByUser.end() && published->second == 5990;
    auto& expired = pendingRemoteIntervalStartsByUser[sender + ":expired"];
    expired.senderKey = sender;
    expired.audioGuidHex = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    expired.receivedSampleCount = 1;
    expired.receivedAtMs = juce::Time::getMillisecondCounterHiRes() - 6100.0;
    processPendingIntervalSyncMarkers(0, playbackSample, 2000.0);
    ok = ok && pendingRemoteIntervalStartsByUser.count(sender + ":expired") == 0;
    for (int marker = 0; marker < 3; ++marker)
        pendingRemoteIntervalStartsByUser.erase(sender + ":" + juce::String(marker));
    remoteAudioPlaybackBoundariesByUser.erase(sender);
    remoteLatencyAverageByUser.erase(sender);
    remoteLatencyFirmDelayMsByUser.erase(sender);
    remoteLatencyLastAppliedIntervalByUser.erase(sender);
    lastRemoteServerLatencyMsByUser.erase(sender);
    remoteVideoBufferRefreshIdByUser.erase(sender);
    std::cout << (ok ? "PASS" : "FAIL") << ": late GUID retained, matched once, published 5990 ms, expired when absent" << std::endl;
    return ok;
}

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int blockSize = 480;

struct Client
{
    explicit Client(juce::String clientName) : name(std::move(clientName))
    {
        processor.setChordDetectionEnabled(false);
        processor.setSamplePadsFeatureEnabled(false);
        processor.setAutoTuneEnabled(false);
        processor.setRateAndBufferSizeDetails(sampleRate, blockSize);
        processor.prepareToPlay(sampleRate, blockSize);
        audioChannels = juce::jmax(2, juce::jmax(processor.getTotalNumInputChannels(),
                                                 processor.getTotalNumOutputChannels()));
        std::cout << "phase: " << name << " audio channels=" << audioChannels << std::endl;
    }

    ~Client()
    {
        stopAudio();
        processor.disconnectFromServer();
        processor.releaseResources();
    }

    void tick()
    {
        if (firstTick)
            std::cout << "phase: " << name << " first timerCallback" << std::endl;
        processor.timerCallback();
        if (firstTick)
        {
            std::cout << "phase: " << name << " first tick complete" << std::endl;
            firstTick = false;
        }
    }

    void startAudio()
    {
        if (audioRunning.exchange(true))
            return;

        audioThread = std::thread([this]
        {
            juce::AudioBuffer<float> audio(audioChannels, blockSize);
            juce::MidiBuffer midi;
            bool firstBlock = true;
            const double blockDurationMs = blockSize * 1000.0 / sampleRate;
            double nextBlockMs = juce::Time::getMillisecondCounterHiRes();
            std::ofstream inputCapture, outputCapture, clockCapture;
            juce::uint32 noiseState = name == "alpha" ? 57 : 93;
            if (captureLiveProbe)
            {
                const auto base = "test-results/live-" + name.toStdString();
                inputCapture.open(base + "-input.f32", std::ios::binary);
                outputCapture.open(base + "-output.f32", std::ios::binary);
                clockCapture.open(base + "-clock.txt");
            }
            while (audioRunning.load())
            {
                audio.clear();
                midi.clear();
                if (captureLiveProbe)
                {
                    clockCapture << juce::Time::currentTimeMillis() << '\n';
                    // Independent source signals allow correlation in both directions.
                    {
                        for (int sample = 0; sample < blockSize; ++sample)
                        {
                            noiseState ^= noiseState << 13;
                            noiseState ^= noiseState >> 17;
                            noiseState ^= noiseState << 5;
                            audio.setSample(0, sample, ((float)(noiseState & 65535u) / 32768.0f - 1.0f) * 0.2f);
                        }
                    }
                    inputCapture.write(reinterpret_cast<const char*>(audio.getReadPointer(0)), blockSize * sizeof(float));
                }
                if (referenceTone)
                {
                    const double wallMs = (double)juce::Time::currentTimeMillis();
                    for (int sample = 0; sample < blockSize; ++sample)
                    {
                        const double atMs = wallMs + sample * 1000.0 / sampleRate;
                        const auto event = (long long)std::floor(atMs / 5000.0);
                        const double phaseMs = atMs - (double)event * 5000.0;
                        const double frequency = 660.0 + (double)(event % 5) * 110.0;
                        const double envelope = phaseMs < 100.0
                            ? juce::jlimit(0.0, 1.0, juce::jmin(phaseMs / 5.0, (100.0 - phaseMs) / 5.0)) : 0.0;
                        audio.setSample(0, sample, (float)(0.15 * envelope * std::sin(phaseMs * frequency * juce::MathConstants<double>::twoPi / 1000.0)));
                    }
                }
                if (firstBlock)
                    std::cout << "phase: " << name << " first audio-thread processBlock" << std::endl;
                processor.processBlock(audio, midi);
                if (captureLiveProbe)
                    outputCapture.write(reinterpret_cast<const char*>(audio.getReadPointer(0)), blockSize * sizeof(float));
                if (firstBlock)
                {
                    std::cout << "phase: " << name << " first audio-thread processBlock complete" << std::endl;
                    firstBlock = false;
                }
                // Model an audio device's sample clock. Sleeping a full block
                // after processing adds CPU time to every block and makes the
                // sample clock drift away from the wall-clock sync markers.
                nextBlockMs += blockDurationMs;
                const double waitMs = nextBlockMs - juce::Time::getMillisecondCounterHiRes();
                if (waitMs > 0.0)
                    juce::Thread::sleep((int)std::ceil(waitMs));
            }
        });
    }

    void stopAudio()
    {
        audioRunning.store(false);
        if (audioThread.joinable())
            audioThread.join();
    }

    juce::String name;
    NinjamVst3AudioProcessor processor;
    bool firstTick = true;
    int audioChannels = 2;
    std::atomic<bool> audioRunning { false };
    std::thread audioThread;
    bool captureLiveProbe = false;
    bool referenceTone = false;
};

struct RemoteObservation
{
    bool found = false;
    bool bufferCalculated = false;
    bool receiverBufferEmitted = false;
    int receiverBufferMs = -1;
    int intervalSampleCount = 0;
    juce::String refreshEventId;
};

bool waitForPort(int port, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        juce::StreamingSocket socket;
        if (socket.connect("127.0.0.1", port, 100))
            return true;
        juce::Thread::sleep(20);
    }
    return false;
}

juce::String fetchIntervals(int port)
{
    int statusCode = 0;
    const juce::URL url("http://127.0.0.1:" + juce::String(port) + "/intervals");
    auto response = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(1000)
            .withStatusCode(&statusCode));
    if (response == nullptr || statusCode != 200)
        return {};
    return response->readEntireStreamAsString();
}

RemoteObservation observeRemotePayload(const juce::String& payload, const juce::String& remoteName)
{
    RemoteObservation result;
    const auto parsed = juce::JSON::parse(payload);
    const auto* entries = parsed.getArray();
    if (entries == nullptr)
        return result;

    const auto wanted = remoteName.trim().toLowerCase();
    for (const auto& entry : *entries)
    {
        auto* object = entry.getDynamicObject();
        if (object == nullptr || object->getProperty("type").toString() != "videoTimecode")
            continue;

        const auto userId = object->getProperty("userId").toString().trim().toLowerCase();
        const auto userKey = object->getProperty("userKey").toString().trim().toLowerCase();
        if (userId != wanted && userKey != wanted)
            continue;

        result.found = true;
        result.bufferCalculated = (bool)object->getProperty("bufferCalculated");
        result.intervalSampleCount = (int)object->getProperty("intervalSampleCount");
        if (object->hasProperty("receiverBufferMs"))
        {
            result.receiverBufferEmitted = true;
            result.receiverBufferMs = (int)object->getProperty("receiverBufferMs");
        }
        if (object->hasProperty("bufferRefreshEventId"))
            result.refreshEventId = object->getProperty("bufferRefreshEventId").toString();
        return result;
    }
    return result;
}

RemoteObservation observeRemote(int helperPort, const juce::String& remoteName)
{
    return observeRemotePayload(fetchIntervals(helperPort), remoteName);
}

void pump(const std::vector<Client*>& clients,
          int durationMs,
          const std::function<void()>& observer = {},
          double observationIntervalMs = 100.0)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + durationMs;
    double nextObservation = 0.0;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        for (auto* client : clients)
            client->tick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);

        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (observer && now >= nextObservation)
        {
            observer();
            nextObservation = now + juce::jmax(1.0, observationIntervalMs);
        }
        juce::Thread::sleep(9);
    }
}

bool waitForConnected(const std::vector<Client*>& clients, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (juce::Time::getMillisecondCounterHiRes() < deadline)
    {
        pump(clients, 50);
        bool allConnected = true;
        for (auto* client : clients)
        {
            auto& ninjamClient = client->processor.getClient();
            allConnected = allConnected
                && ninjamClient.GetStatus() == NJClient::NJC_STATUS_OK
                && ninjamClient.IsAudioRunning() != 0;
        }
        if (allConnected)
            return true;
    }
    return false;
}

int fail(const juce::String& message)
{
    std::cerr << "FAIL: " << message << std::endl;
    return 1;
}

double positiveModulo(double value, double modulus)
{
    const double remainder = std::fmod(value, modulus);
    return remainder < 0.0 ? remainder + modulus : remainder;
}

double circularDistance(double lhs, double rhs, double modulus)
{
    const double wrapped = positiveModulo(lhs - rhs, modulus);
    return juce::jmin(wrapped, modulus - wrapped);
}

int medianOfLastFive(const std::vector<int>& values)
{
    if (values.empty())
        return -1;

    const auto first = values.size() > 5 ? values.end() - 5 : values.begin();
    std::vector<int> tail(first, values.end());
    std::sort(tail.begin(), tail.end());
    return tail[tail.size() / 2];
}

bool verifyRemoteLatencyJitterFilter(NinjamVst3AudioProcessor& processor)
{
    bool startupOk = true;
    for (const int outlier : { 0, 16000 })
    {
        const auto key = "startup-outlier-" + juce::String(outlier);
        processor.applyRemoteLatencyMeasurementForIntegrationTest(key, outlier);
        processor.applyRemoteLatencyMeasurementForIntegrationTest(key, 800);
        const int firstStable = processor.applyRemoteLatencyMeasurementForIntegrationTest(key, 800);
        if (firstStable != 800)
        {
            std::cerr << "FAIL: first stable buffer retained startup outlier " << outlier
                      << ": expected 800, got " << firstStable << std::endl;
            startupOk = false;
        }
    }
    juce::uint32 state = 57;
    int minimumSettledMs = std::numeric_limits<int>::max();
    int maximumSettledMs = std::numeric_limits<int>::min();
    for (int sample = 0; sample < 100; ++sample)
    {
        state = state * 1664525u + 1013904223u;
        const int jitterMs = (int)((state >> 16) % 81u);
        const int filteredMs = processor.applyRemoteLatencyMeasurementForIntegrationTest("jitter-probe", 1000 - jitterMs);
        if (sample >= 10)
        {
            minimumSettledMs = juce::jmin(minimumSettledMs, filteredMs);
            maximumSettledMs = juce::jmax(maximumSettledMs, filteredMs);
        }
    }

    if (maximumSettledMs - minimumSettledMs > 50)
        return false;

    int shiftedMs = -1;
    for (int sample = 0; sample < 24; ++sample)
        shiftedMs = processor.applyRemoteLatencyMeasurementForIntegrationTest("persistent-shift-probe", sample < 10 ? 800 : 900);
    bool recoveryOk = true;
    const auto checkRecovery = [&](const juce::String& key, int baseline, const std::vector<int>& readings,
                                   int expected, int tolerance)
    {
        for (int i = 0; i < 12; ++i)
            processor.applyRemoteLatencyMeasurementForIntegrationTest(key, baseline);
        int actual = baseline;
        for (const int reading : readings)
            actual = processor.applyRemoteLatencyMeasurementForIntegrationTest(key, reading);
        std::cout << "filter replay " << key << ": " << actual << " ms (expected " << expected << ")" << std::endl;
        if (std::abs(actual - expected) > tolerance)
            recoveryOk = false;
    };
    // Raw GUID delays from the TURN/UDP dynamic-network recovery recording.
    // The audio waveform measured 3826 ms; route smoothing is already included
    // in these inputs. Do not add another long settling tail in this filter.
    checkRecovery("udp-recovery", 4629,
                  { 4629, 4386, 4225, 4096, 4012, 3923, 3932, 3915, 3873, 3849, 3861 }, 3826, 200);
    checkRecovery("step-down", 9000, { 5050, 5080, 5040, 5060, 5055 }, 5053, 50);
    checkRecovery("step-up", 5053, { 9000, 9030, 8990, 9010, 9005 }, 9000, 50);
    // Preserve the nine-reading median's rejection of up to four bad readings.
    checkRecovery("burst-down", 9000, { 5050, 5050, 5050, 5050, 9000, 9000, 9000, 9000, 9000 }, 9000, 0);
    checkRecovery("burst-up", 5053, { 9000, 9000, 9000, 9000, 5053, 5053, 5053, 5053, 5053 }, 5053, 0);
    return startupOk && shiftedMs >= 870 && recoveryOk;
}

juce::String guidString(const unsigned char guid[16])
{
    static constexpr char hex[] = "0123456789abcdef";
    char text[33] {};
    for (int index = 0; index < 16; ++index)
    {
        text[index * 2] = hex[(guid[index] >> 4) & 0x0f];
        text[index * 2 + 1] = hex[guid[index] & 0x0f];
    }
    return juce::String::fromUTF8(text);
}

juce::String currentLocalAudioGuid(Client& client)
{
    unsigned char guid[16] {};
    return client.processor.getClient().GetLocalChannelCurrentGuid(0, guid) ? guidString(guid) : juce::String();
}

juce::String currentRemoteAudioGuid(Client& client, const juce::String& remoteName)
{
    auto& ninjamClient = client.processor.getClient();
    for (int userIndex = 0; userIndex < ninjamClient.GetNumUsers(); ++userIndex)
    {
        const char* name = ninjamClient.GetUserState(userIndex);
        if (name == nullptr || !juce::String::fromUTF8(name).equalsIgnoreCase(remoteName))
            continue;

        unsigned char currentGuid[16] {};
        bool hasCurrent = false;
        if (ninjamClient.GetUserChannelPlaybackGuids(userIndex, 0, currentGuid, &hasCurrent,
                                                     nullptr, nullptr)
            && hasCurrent)
            return guidString(currentGuid);
    }
    return {};
}

struct AudioGuidTimingTracker
{
    void sample(Client& alpha, Client& bravo)
    {
        observeCompletedGuid(alpha, lastAlphaLocalGuid, alphaCompletedAtMs);
        observeCompletedGuid(bravo, lastBravoLocalGuid, bravoCompletedAtMs);
        observePlaybackGuid(alpha, "bravo", bravoCompletedAtMs, alphaMatchedGuids, alphaPlaybackDelayMs);
        observePlaybackGuid(bravo, "alpha", alphaCompletedAtMs, bravoMatchedGuids, bravoPlaybackDelayMs);
    }

    static void observeCompletedGuid(Client& sender, juce::String& previousGuid,
                                     std::map<juce::String, double>& completedAtMs)
    {
        const auto currentGuid = currentLocalAudioGuid(sender);
        if (currentGuid.isEmpty())
            return;
        if (previousGuid.isNotEmpty() && currentGuid != previousGuid)
            completedAtMs[previousGuid] = sender.processor.getLatestIntervalStartMsForIntegrationTest();
        previousGuid = currentGuid;
    }

    static void observePlaybackGuid(Client& receiver, const juce::String& senderName,
                                    const std::map<juce::String, double>& completedAtMs,
                                    std::set<juce::String>& matchedGuids,
                                    std::vector<double>& playbackDelayMs)
    {
        const auto playbackGuid = currentRemoteAudioGuid(receiver, senderName);
        if (playbackGuid.isEmpty() || matchedGuids.find(playbackGuid) != matchedGuids.end())
            return;
        const auto completion = completedAtMs.find(playbackGuid);
        if (completion == completedAtMs.end() || completion->second < 0.0)
            return;
        const double playbackAtMs = receiver.processor.getLatestIntervalStartMsForIntegrationTest();
        if (playbackAtMs >= completion->second)
        {
            playbackDelayMs.push_back(playbackAtMs - completion->second);
            matchedGuids.insert(playbackGuid);
        }
    }

    juce::String lastAlphaLocalGuid;
    juce::String lastBravoLocalGuid;
    std::map<juce::String, double> alphaCompletedAtMs;
    std::map<juce::String, double> bravoCompletedAtMs;
    std::set<juce::String> alphaMatchedGuids;
    std::set<juce::String> bravoMatchedGuids;
    std::vector<double> alphaPlaybackDelayMs;
    std::vector<double> bravoPlaybackDelayMs;
};
}

int main(int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    std::cout << "phase: initialise" << std::endl;

    if (argc == 2 && juce::String::fromUTF8(argv[1]) == "--filter-only")
    {
        auto processor = std::make_unique<NinjamVst3AudioProcessor>();
        return verifyRemoteLatencyJitterFilter(*processor) ? 0 : fail("latency filter regression");
    }

    if (argc < 2)
        return fail("usage: NINJAMplus_SyncSoakTest <path-to-ninjamsrv> [--near-boundary | --live-vdo [--hotspot] [--bpm=120]]");

    const juce::File serverExecutable(juce::String::fromUTF8(argv[1]));
    if (!serverExecutable.existsAsFile())
        return fail("ninjamsrv executable does not exist: " + serverExecutable.getFullPathName());

    juce::StreamingSocket portReservation;
    if (!portReservation.createListener(0, "127.0.0.1"))
        return fail("could not reserve a local NINJAM port");
    const int serverPort = portReservation.getBoundPort();
    portReservation.close();

    const juce::File tempRoot = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("ninjamplus-sync-soak", {}, false);
    if (!tempRoot.createDirectory())
        return fail("could not create temporary test directory");

    const juce::File configFile = tempRoot.getChildFile("soak.cfg");
    const bool liveVdo = argc >= 3 && juce::String::fromUTF8(argv[2]) == "--live-vdo";
    bool liveHotspot = false;
    int liveBpm = 120;
    for (int arg = 3; arg < argc; ++arg)
    {
        const auto option = juce::String::fromUTF8(argv[arg]);
        if (option == "--hotspot") liveHotspot = true;
        else if (option.startsWith("--bpm=")) liveBpm = juce::jlimit(40, 240, option.fromFirstOccurrenceOf("=", false, false).getIntValue());
    }
    const juce::String config =
        "Port " + juce::String(serverPort) + "\n"
        "MaxUsers 8\n"
        "MaxChannels 32 4\n"
        "AnonymousUsers no\n"
        "AllowHiddenUsers yes\n"
        "SetVotingThreshold 50\n"
        "ACL 127.0.0.1/32 allow\n"
        "User alpha testpass *\n"
        "User bravo testpass *\n"
        + (liveVdo ? "DefaultBPM " + juce::String(liveBpm) + "\nDefaultBPI 16\n" : juce::String("DefaultBPM 200\nDefaultBPI 2\n"));
    if (!configFile.replaceWithText(config))
        return fail("could not write local NINJAM server config");

    juce::ChildProcess server;
    juce::StringArray serverArgs { serverExecutable.getFullPathName(), configFile.getFullPathName() };
    if (!server.start(serverArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return fail("could not start local NINJAM server");
    if (!waitForPort(serverPort, 5000))
    {
        const auto output = server.readAllProcessOutput();
        server.kill();
        return fail("local NINJAM server did not listen: " + output);
    }
    std::cout << "phase: server listening on " << serverPort << std::endl;

    if (argc >= 5 && juce::String::fromUTF8(argv[2]) == "--reference-public")
    {
        Client reference("anonymous:SteveReference");
        reference.referenceTone = true;
        reference.processor.setMobileHotspotModeEnabled(true);
        reference.processor.setMetronomeMuted(true);
        reference.processor.setLocalMonitorEnabled(false);
        reference.processor.setLocalChannelInput(0, 0);
        reference.processor.setLocalChannelGain(0, 1.0f);
        reference.processor.setTransmitLocal(true);
        // This explicit diagnostic mode has no editor in which to show the
        // already-authorized room's license prompt. Print it to the operator.
        reference.processor.getClient().LicenseAgreementCallback = [](void*, const char* license) -> int
        {
            std::cout << "REFERENCE server license: " << (license != nullptr ? license : "") << std::endl;
            return 1;
        };
        reference.processor.getClient().LicenseAgreement_User = nullptr;
        reference.startAudio();
        reference.processor.connectToServer(juce::String::fromUTF8(argv[3]), reference.name, {});
        if (!waitForConnected({ &reference }, 60000))
        {
            server.kill();
            return fail("reference client did not connect: " + juce::String(reference.processor.getClient().GetErrorStr()));
        }
        reference.processor.launchVideoSession(juce::String::fromUTF8(argv[4]));
        const auto stop = juce::File::getCurrentWorkingDirectory().getChildFile("test-results/reference-stop");
        stop.deleteFile();
        const auto status = juce::File::getCurrentWorkingDirectory().getChildFile("test-results/reference-status.json");
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 600000.0;
        std::cout << "REFERENCE: generated 100ms tone every five seconds; no microphone; port="
                  << reference.processor.getVideoHelperPortForIntegrationTest() << std::endl;
        while (!stop.existsAsFile() && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            pump({ &reference }, 250);
            status.replaceWithText(juce::JSON::toString(reference.processor.getSyncDiagnosticState()));
        }
        reference.stopAudio();
        reference.processor.disconnectFromServer();
        server.kill();
        return 0;
    }

    if (liveVdo)
    {
        const juce::String room = "njplusbern" + juce::String(juce::Time::currentTimeMillis());
        const auto stopFile = juce::File::getCurrentWorkingDirectory().getChildFile("test-results/live-stop");
        stopFile.deleteFile();
        const auto bpmFile = juce::File::getCurrentWorkingDirectory().getChildFile("test-results/live-bpm");
        bpmFile.deleteFile();
        auto alphaOwner = std::make_unique<Client>("alpha");
        auto bravoOwner = std::make_unique<Client>("bravo");
        auto& alpha = *alphaOwner;
        auto& bravo = *bravoOwner;
        int liveResult = 0;
        for (auto* client : { &alpha, &bravo })
        {
            client->captureLiveProbe = true;
            client->processor.setMobileHotspotModeEnabled(liveHotspot);
            client->processor.setMetronomeMuted(true);
            client->processor.setLocalMonitorEnabled(false);
            client->processor.setLocalChannelInput(0, 0);
            client->processor.setLocalChannelGain(0, 1.0f);
            client->processor.setTransmitLocal(true);
            client->startAudio();
            client->processor.connectToServer("127.0.0.1:" + juce::String(serverPort), client->name, "testpass");
            if (!waitForConnected({ &alpha, &bravo }, 100))
                pump({ &alpha, &bravo }, 500);
        }
        if (!waitForConnected({ &alpha, &bravo }, 5000))
            liveResult = fail("live clients did not connect");
        if (liveResult == 0)
        {
            alpha.processor.launchVideoSession(room);
            pump({ &alpha, &bravo }, 1500);
            bravo.processor.launchVideoSession(room);
            std::cout << "LIVE room=" << room
                      << " hotspot=" << liveHotspot << " bpm=" << liveBpm
                      << " alphaPort=" << alpha.processor.getVideoHelperPortForIntegrationTest()
                      << " bravoPort=" << bravo.processor.getVideoHelperPortForIntegrationTest() << std::endl;
            const auto deadline = juce::Time::getMillisecondCounterHiRes() + 900000.0;
            const auto diagnosticsFile = juce::File::getCurrentWorkingDirectory().getChildFile("test-results/live-native-diagnostics.jsonl");
            diagnosticsFile.replaceWithText({});
            double nextDiagnostics = 0.0;
            while (!stopFile.existsAsFile() && juce::Time::getMillisecondCounterHiRes() < deadline)
            {
                const auto diagnosticNow = juce::Time::getMillisecondCounterHiRes();
                if (diagnosticNow >= nextDiagnostics)
                {
                    nextDiagnostics = diagnosticNow + 1000.0;
                    juce::DynamicObject::Ptr snapshot = new juce::DynamicObject();
                    snapshot->setProperty("at", juce::Time::currentTimeMillis());
                    snapshot->setProperty("alpha", alpha.processor.getSyncDiagnosticState());
                    snapshot->setProperty("bravo", bravo.processor.getSyncDiagnosticState());
                    diagnosticsFile.appendText(juce::JSON::toString(juce::var(snapshot.get()), true) + "\n");
                }
                if (bpmFile.existsAsFile())
                {
                    const int requestedBpm = bpmFile.loadFileAsString().trim().getIntValue();
                    bpmFile.deleteFile();
                    if (requestedBpm >= 40 && requestedBpm <= 240)
                    {
                        alpha.processor.sendChatMessage("!vote bpm " + juce::String(requestedBpm));
                        bravo.processor.sendChatMessage("!vote bpm " + juce::String(requestedBpm));
                        std::cout << "LIVE requested bpm=" << requestedBpm << std::endl;
                    }
                }
                pump({ &alpha, &bravo }, 100);
            }
        }
        alpha.stopAudio();
        bravo.stopAudio();
        server.kill();
        tempRoot.deleteRecursively();
        return liveResult;
    }

    int result = 0;
    {
        auto alpha = std::make_unique<Client>("alpha");
        std::cout << "phase: alpha processor created" << std::endl;
        const bool filterOk = verifyRemoteLatencyJitterFilter(alpha->processor);
        alpha->processor.setMobileHotspotModeEnabled(true);
        alpha->processor.connectToServer("127.0.0.1:" + juce::String(serverPort), "alpha", "testpass");
        std::cout << "phase: alpha connecting" << std::endl;
        if (result == 0 && !waitForConnected({ alpha.get() }, 5000))
            result = fail("alpha did not connect");
        if (result == 0 && !alpha->processor.startVideoSyncForIntegrationTest())
            result = fail("alpha loopback video helper did not start");
        if (alpha->processor.getClient().GetStatus() == NJClient::NJC_STATUS_OK)
        {
            if (!alpha->processor.verifyLateAudioGuidForIntegrationTest())
                result = fail("late audio GUID was lost or replaced by an unverified playback estimate");
            const int shortPhase = alpha->processor.measurePendingIntervalForIntegrationTest(250, false, false);
            const int waitingForAudio = alpha->processor.measurePendingIntervalForIntegrationTest(1250, true, false);
            const int matchedAudio = alpha->processor.measurePendingIntervalForIntegrationTest(250, true, true);
            const int missingAudioTimeout = alpha->processor.measurePendingIntervalForIntegrationTest(3500, true, false);
            const int boundaryGuard = alpha->processor.measurePendingIntervalForIntegrationTest(50, false, false);
            const int lateGuidPending = alpha->processor.measurePendingIntervalForIntegrationTest(3990, true, false);
            const int lateGuidMatched = alpha->processor.measurePendingIntervalForIntegrationTest(3990, true, true);
            if (lateGuidPending != -1 || lateGuidMatched != 5990)
                result = fail("late GUID must stay unmeasured until actual playback establishes 5990 ms");
            if (shortPhase < 2240 || shortPhase > 2300)
                result = fail("fallback buffer did not include the recording interval plus the valid short playback phase");
            if (waitingForAudio != -1)
                result = fail("first audio GUID marker was consumed before its playback boundary existed");
            if (matchedAudio != 2250)
                result = fail("audio GUID buffer measured from recording completion instead of capture start");
            if (missingAudioTimeout != -1)
                result = fail("missing audio GUID produced an unverified beat-fallback estimate");
            if (boundaryGuard != -1)
                result = fail("fallback lost its near-boundary guard");
            if (result == 0)
                std::cout << "PASS: startup phase, audio GUID matching, timeout, and boundary regressions" << std::endl;
        }
        std::cout << "phase: alpha helper port=" << alpha->processor.getVideoHelperPortForIntegrationTest() << std::endl;
        if (!filterOk)
            result = fail("remote latency filter failed startup, jitter, or persistent-shift regression");
        else
            std::cout << "PASS: startup outliers, jitter, and persistent-shift regressions" << std::endl;
        if (result == 0)
        {
            std::cout << "phase: alpha connected" << std::endl;
            alpha->startAudio();
        }

        // Let alpha's local absolute interval counter get well ahead of a later joiner.
        if (result == 0)
            pump({ alpha.get() }, 4200);
        if (result == 0)
            std::cout << "phase: alpha stagger complete" << std::endl;
        const int alphaBeforeJoin = alpha->processor.getIntervalIndex();
        if (result == 0 && alphaBeforeJoin < 5)
            result = fail("alpha did not advance enough intervals before staggered join");

        auto bravo = std::make_unique<Client>("bravo");
        std::cout << "phase: bravo processor created" << std::endl;
        // Put one side-signal path just across the receiver's nearby interval
        // boundary while leaving the audio path untouched.
        alpha->processor.setIntervalSyncTagArrivalOffsetForIntegrationTest(50);
        bravo->processor.setMobileHotspotModeEnabled(true);
        bravo->processor.connectToServer("127.0.0.1:" + juce::String(serverPort), "bravo", "testpass");
        std::cout << "phase: bravo connecting" << std::endl;
        if (result == 0 && !waitForConnected({ alpha.get(), bravo.get() }, 5000))
            result = fail("bravo did not connect");
        if (result == 0 && !bravo->processor.startVideoSyncForIntegrationTest())
            result = fail("bravo loopback video helper did not start");
        std::cout << "phase: bravo helper port=" << bravo->processor.getVideoHelperPortForIntegrationTest() << std::endl;
        if (result == 0)
        {
            std::cout << "phase: bravo connected" << std::endl;
            if (argc >= 3 && juce::String::fromUTF8(argv[2]) == "--near-boundary")
            {
                const double deadline = juce::Time::getMillisecondCounterHiRes() + 2000.0;
                while (alpha->processor.getIntervalProgress() < 0.94f
                       || alpha->processor.getIntervalProgress() > 0.98f)
                {
                    if (juce::Time::getMillisecondCounterHiRes() >= deadline)
                    {
                        result = fail("could not align the staggered join near alpha's boundary");
                        break;
                    }
                    pump({ alpha.get(), bravo.get() }, 1);
                }
                std::cout << "phase: near-boundary join at " << alpha->processor.getIntervalProgress() << std::endl;
            }
            if (result == 0)
                bravo->startAudio();
        }

        AudioGuidTimingTracker audioGuidTiming;
        if (result == 0)
        {
            std::cout << "phase: staggered soak" << std::endl;
            pump({ alpha.get(), bravo.get() }, 5200, [&]
            {
                audioGuidTiming.sample(*alpha, *bravo);
            }, 10.0);
        }

        int stableAlphaSnapshots = 0;
        int stableAlphaSnapshotsWithBuffer = 0;
        bool stableAlphaRefreshExpired = true;
        std::map<juce::String, double> refreshFirstObservedAt;
        juce::String lastStableAlphaPayload;
        std::vector<int> alphaBufferSamples;
        std::vector<int> bravoBufferSamples;
        double nextStableHelperPollMs = 0.0;
        if (result == 0)
        {
            // Model the real helper's HTTP polling, including a background-tab
            // cadence that can be slower than the native 500 ms payload writes.
            // Each response is a complete snapshot: a helper opening or reloading
            // after the initial refresh event must still recover the stable buffer.
            pump({ alpha.get(), bravo.get() }, 5200, [&]
            {
                audioGuidTiming.sample(*alpha, *bravo);
                // GUID changes must be observed faster than the 600 ms test
                // interval. Sampling them only at the background helper's
                // 1000 ms cadence can assign completion to a later interval.
                const double now = juce::Time::getMillisecondCounterHiRes();
                if (now < nextStableHelperPollMs)
                    return;
                nextStableHelperPollMs = now + 1000.0;
                const auto payload = fetchIntervals(alpha->processor.getVideoHelperPortForIntegrationTest());
                const auto alphaObservation = observeRemotePayload(payload, "bravo");
                if (alphaObservation.found && alphaObservation.bufferCalculated && alphaObservation.receiverBufferEmitted)
                    alphaBufferSamples.push_back(alphaObservation.receiverBufferMs);
                const auto bravoObservation = observeRemote(bravo->processor.getVideoHelperPortForIntegrationTest(), "alpha");
                const auto diagnostics = alpha->processor.getSyncDiagnosticState();
                const auto measurements = diagnostics.getProperty("measurements", juce::var());
                if (const auto* entries = measurements.getArray())
                    for (const auto& entry : *entries)
                        if (entry.getProperty("userKey", juce::var()).toString().startsWith("bravo"))
                            std::cout << "alpha playback evidence: " << juce::JSON::toString(entry, true) << std::endl;
                if (bravoObservation.found && bravoObservation.bufferCalculated && bravoObservation.receiverBufferEmitted)
                    bravoBufferSamples.push_back(bravoObservation.receiverBufferMs);
                if (payload != lastStableAlphaPayload)
                {
                    lastStableAlphaPayload = payload;
                    const auto& observation = alphaObservation;
                    if (observation.found && observation.bufferCalculated)
                    {
                        ++stableAlphaSnapshots;
                        if (observation.receiverBufferEmitted)
                            ++stableAlphaSnapshotsWithBuffer;
                        if (observation.refreshEventId.isNotEmpty())
                        {
                            // A newly confirmed delay can legitimately refresh
                            // during settling. Reject a retained event identity,
                            // rather than assuming no new events can occur.
                            const double now = juce::Time::getMillisecondCounterHiRes();
                            const auto first = refreshFirstObservedAt.emplace(observation.refreshEventId, now);
                            if (now - first.first->second > 2500.0)
                                stableAlphaRefreshExpired = false;
                        }
                    }
                }
            }, 10.0);
        }
        std::cout << "phase: staggered soak complete" << std::endl;

        const auto alphaSeesBravo = observeRemote(alpha->processor.getVideoHelperPortForIntegrationTest(), "bravo");
        const auto bravoSeesAlpha = observeRemote(bravo->processor.getVideoHelperPortForIntegrationTest(), "alpha");
        const int stableAlphaBufferMs = medianOfLastFive(alphaBufferSamples);
        const int stableBravoBufferMs = medianOfLastFive(bravoBufferSamples);
        const int alphaReceivedMarkers = alpha->processor.getReceivedRemoteSyncMarkersForIntegrationTest();
        const int alphaAcceptedMarkers = alpha->processor.getAcceptedRemoteSyncMarkersForIntegrationTest();
        const int bravoReceivedMarkers = bravo->processor.getReceivedRemoteSyncMarkersForIntegrationTest();
        const int bravoAcceptedMarkers = bravo->processor.getAcceptedRemoteSyncMarkersForIntegrationTest();
        const double intervalDurationMs = (60.0 / juce::jmax(1.0f, alpha->processor.getBPM()))
            * (double)juce::jmax(1, alpha->processor.getBPI()) * 1000.0;
        const double alphaIntervalStartMs = alpha->processor.getLatestIntervalStartMsForIntegrationTest();
        const double bravoIntervalStartMs = bravo->processor.getLatestIntervalStartMsForIntegrationTest();
        const double expectedAlphaBufferMs = intervalDurationMs + positiveModulo(alphaIntervalStartMs - bravoIntervalStartMs, intervalDurationMs);
        const double expectedBravoBufferMs = intervalDurationMs + positiveModulo(bravoIntervalStartMs - alphaIntervalStartMs, intervalDurationMs);
        std::cout << "staggered join: alpha interval=" << alpha->processor.getIntervalIndex()
                  << " bravo interval=" << bravo->processor.getIntervalIndex()
                  << " alpha->bravo buffer=" << stableAlphaBufferMs
                  << " calculated=" << alphaSeesBravo.bufferCalculated
                  << " samples=" << alphaSeesBravo.intervalSampleCount
                  << " bravo->alpha buffer=" << stableBravoBufferMs
                  << " calculated=" << bravoSeesAlpha.bufferCalculated
                  << " samples=" << bravoSeesAlpha.intervalSampleCount
                  << " expected buffers=" << (int)std::llround(expectedAlphaBufferMs)
                  << "/" << (int)std::llround(expectedBravoBufferMs)
                  << " markers alpha=" << alphaAcceptedMarkers << "/" << alphaReceivedMarkers
                  << " bravo=" << bravoAcceptedMarkers << "/" << bravoReceivedMarkers << std::endl;
        if (!audioGuidTiming.alphaPlaybackDelayMs.empty() || !audioGuidTiming.bravoPlaybackDelayMs.empty())
        {
            const double alphaGuidDelay = audioGuidTiming.alphaPlaybackDelayMs.empty()
                ? -1.0 : audioGuidTiming.alphaPlaybackDelayMs.back();
            const double bravoGuidDelay = audioGuidTiming.bravoPlaybackDelayMs.empty()
                ? -1.0 : audioGuidTiming.bravoPlaybackDelayMs.back();
            std::cout << "audio GUID playback delays alpha=" << alphaGuidDelay
                      << " bravo=" << bravoGuidDelay
                      << " samples=" << audioGuidTiming.alphaPlaybackDelayMs.size()
                      << "/" << audioGuidTiming.bravoPlaybackDelayMs.size() << std::endl;
        }

        if (result == 0 && (!alphaSeesBravo.found || !bravoSeesAlpha.found))
            result = fail("both clients were not present in each other's real helper payload");
        if (result == 0 && !alphaSeesBravo.bufferCalculated)
            result = fail("older alpha rejected later-joining bravo's sync tags");
        if (result == 0 && !bravoSeesAlpha.bufferCalculated)
            result = fail("later-joining bravo did not calculate alpha's buffer");
        if (result == 0 && (alphaSeesBravo.intervalSampleCount < 8 || bravoSeesAlpha.intervalSampleCount < 8))
            result = fail("both directions did not collect at least eight interval-sync samples before assertion");
        constexpr double loopbackPhaseToleranceMs = 85.0;
        if (result == 0
            && (alphaIntervalStartMs < 0.0 || bravoIntervalStartMs < 0.0
                || stableAlphaBufferMs < 0 || stableBravoBufferMs < 0
                || circularDistance((double)stableAlphaBufferMs, expectedAlphaBufferMs, intervalDurationMs) > loopbackPhaseToleranceMs
                || circularDistance((double)stableBravoBufferMs, expectedBravoBufferMs, intervalDurationMs) > loopbackPhaseToleranceMs))
            result = fail("computed receiver buffers did not match the two clients' measured interval phase");
        if (result == 0 && audioGuidTiming.alphaPlaybackDelayMs.empty())
            result = fail("did not observe bravo's audio GUID entering alpha playback");
        if (result == 0
            && std::abs((double)stableAlphaBufferMs - (intervalDurationMs + audioGuidTiming.alphaPlaybackDelayMs.back())) > 120.0)
            result = fail("receiver buffer did not include recording duration before actual audio GUID playback");
        if (result == 0 && audioGuidTiming.bravoPlaybackDelayMs.empty())
            result = fail("did not observe alpha's audio GUID entering bravo playback");
        if (result == 0
            && std::abs((double)stableBravoBufferMs - (intervalDurationMs + audioGuidTiming.bravoPlaybackDelayMs.back())) > 120.0)
            result = fail("bravo receiver buffer did not match absolute audio GUID playback timing");

        // Mobile-hotspot mode sends the primary tag plus +150 ms and +300 ms
        // retransmissions. More than ten accepted markers proves the long soak;
        // receiving more than we accept proves duplicate/late copies were tested.
        if (result == 0 && (alphaAcceptedMarkers < 10 || bravoAcceptedMarkers < 10
                            || alphaReceivedMarkers <= alphaAcceptedMarkers
                            || bravoReceivedMarkers <= bravoAcceptedMarkers))
            result = fail("late/duplicate interval tags were not exercised or deduplicated");

        if (result == 0 && stableAlphaSnapshots < 2)
            result = fail("stable helper snapshots were not observed at the throttled polling cadence");
        if (result == 0 && stableAlphaSnapshotsWithBuffer != stableAlphaSnapshots)
            result = fail("a helper opening or reloading after the refresh event missed the stable receiver buffer ("
                          + juce::String(stableAlphaSnapshotsWithBuffer) + "/"
                          + juce::String(stableAlphaSnapshots) + " snapshots carried it)");
        if (result == 0 && !stableAlphaRefreshExpired)
            result = fail("the one-shot buffer refresh event remained in stable helper snapshots");

        if (result == 0)
        {
            alpha->processor.requestVideoBufferRefreshForIntegrationTest();
            // Skip the first nominal 500 ms poll. The same event must still be
            // present for the next poll, then expire without requiring an ACK.
            pump({ alpha.get(), bravo.get() }, 700);
            const auto retainedRefresh = observeRemote(alpha->processor.getVideoHelperPortForIntegrationTest(), "bravo");
            if (retainedRefresh.intervalSampleCount < alphaSeesBravo.intervalSampleCount)
                result = fail("helper refresh discarded established native sync measurements");
            if (retainedRefresh.refreshEventId.isEmpty())
                result = fail("a skipped helper poll lost the buffer refresh event");
            if (result == 0)
            {
                pump({ alpha.get(), bravo.get() }, 500);
                const auto duplicateRefresh = observeRemote(alpha->processor.getVideoHelperPortForIntegrationTest(), "bravo");
                if (duplicateRefresh.refreshEventId != retainedRefresh.refreshEventId)
                    result = fail("the retained buffer refresh event changed identity between helper polls");
            }
            if (result == 0)
            {
                // Expiry is 1500 ms, but HTTP serves a cached snapshot refreshed
                // every 500 ms. Allow that publication cycle before asserting.
                pump({ alpha.get(), bravo.get() }, 1000);
                const auto expiredRefresh = observeRemote(alpha->processor.getVideoHelperPortForIntegrationTest(), "bravo");
                if (expiredRefresh.refreshEventId.isNotEmpty())
                    result = fail("the retained buffer refresh event did not expire");
            }
        }

        const auto bravoSessionBeforeReconnect = bravo->processor.getIntervalSyncSessionIdForIntegrationTest();
        const int alphaAcceptedBeforeReconnect = alpha->processor.getAcceptedRemoteSyncMarkersForIntegrationTest();
        if (result == 0)
        {
            bravo->processor.disconnectFromServer();
            // Rejoin before alpha's 350 ms roster prune. The sender's absolute
            // counter restarts at zero, so acceptance must follow peer lifecycle
            // rather than waiting for a periodic stale-user cleanup.
            pump({ alpha.get(), bravo.get() }, 150);
            bravo->processor.connectToServer("127.0.0.1:" + juce::String(serverPort), "bravo", "testpass");
            if (!waitForConnected({ alpha.get(), bravo.get() }, 5000))
                result = fail("bravo did not reconnect");
            if (result == 0 && !bravo->processor.startVideoSyncForIntegrationTest())
                result = fail("bravo loopback video helper did not restart after reconnect");
        }
        if (result == 0)
            pump({ alpha.get(), bravo.get() }, 5200);

        const auto alphaAfterReconnect = observeRemote(alpha->processor.getVideoHelperPortForIntegrationTest(), "bravo");
        if (result == 0 && !alphaAfterReconnect.bufferCalculated)
            result = fail("alpha did not reacquire bravo's buffer after a sub-prune reconnect reset its local interval counter");
        if (result == 0
            && alpha->processor.getAcceptedRemoteSyncMarkersForIntegrationTest() <= alphaAcceptedBeforeReconnect)
            result = fail("alpha retained a stale buffer but accepted no sync markers after bravo's sub-prune reconnect");

        const auto bravoSessionAfterReconnect = bravo->processor.getIntervalSyncSessionIdForIntegrationTest();
        if (result == 0
            && (bravoSessionBeforeReconnect.isEmpty()
                || bravoSessionAfterReconnect.isEmpty()
                || bravoSessionBeforeReconnect == bravoSessionAfterReconnect))
            result = fail("bravo did not create a distinct sync session identity after reconnect");

        if (result == 0)
        {
            const int acceptedBeforeRetiredMarker = alpha->processor.getAcceptedRemoteSyncMarkersForIntegrationTest();
            juce::DynamicObject::Ptr staleTag = new juce::DynamicObject();
            staleTag->setProperty("type", "intervalSyncTag");
            staleTag->setProperty("userId", "bravo");
            staleTag->setProperty("syncSessionId", bravoSessionBeforeReconnect);
            staleTag->setProperty("intervalIndex", 1000000);
            staleTag->setProperty("intervalAbsolute", 1000000);
            staleTag->setProperty("bpi", juce::jmax(1, alpha->processor.getBPI()));
            staleTag->setProperty("beatIndex", 0);
            staleTag->setProperty("sendOffsetMs", 0.0);
            alpha->processor.injectIntervalSyncTagForIntegrationTest(
                "bravo",
                juce::JSON::toString(juce::var(staleTag.get())));
            if (alpha->processor.getAcceptedRemoteSyncMarkersForIntegrationTest() != acceptedBeforeRetiredMarker)
                result = fail("alpha accepted a delayed marker from bravo's retired sync session");
        }
    }

    server.kill();
    tempRoot.deleteRecursively();

    if (result == 0)
        std::cout << "PASS: two-client stagger/reconnect sync soak" << std::endl;
    return result;
}
