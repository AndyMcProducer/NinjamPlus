#include "PluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <atomic>
#include <thread>
#include <vector>

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
            while (audioRunning.load())
            {
                audio.clear();
                midi.clear();
                if (firstBlock)
                    std::cout << "phase: " << name << " first audio-thread processBlock" << std::endl;
                processor.processBlock(audio, midi);
                if (firstBlock)
                {
                    std::cout << "phase: " << name << " first audio-thread processBlock complete" << std::endl;
                    firstBlock = false;
                }
                juce::Thread::sleep(10);
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
    return shiftedMs >= 870;
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

    if (argc < 2)
        return fail("usage: NINJAMplus_SyncSoakTest <path-to-ninjamsrv>");

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
    const juce::String config =
        "Port " + juce::String(serverPort) + "\n"
        "MaxUsers 8\n"
        "MaxChannels 32 4\n"
        "AnonymousUsers no\n"
        "AllowHiddenUsers yes\n"
        "ACL 127.0.0.1/32 allow\n"
        "User alpha testpass *\n"
        "User bravo testpass *\n"
        "DefaultBPM 200\n"
        "DefaultBPI 2\n";
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

    int result = 0;
    {
        auto alpha = std::make_unique<Client>("alpha");
        std::cout << "phase: alpha processor created" << std::endl;
        if (result == 0 && !verifyRemoteLatencyJitterFilter(alpha->processor))
            result = fail("remote latency filter did not reject 0-80 ms jitter or adapt to a persistent shift");
        alpha->processor.setMobileHotspotModeEnabled(true);
        alpha->processor.connectToServer("127.0.0.1:" + juce::String(serverPort), "alpha", "testpass");
        std::cout << "phase: alpha connecting" << std::endl;
        if (result == 0 && !waitForConnected({ alpha.get() }, 5000))
            result = fail("alpha did not connect");
        if (result == 0 && !alpha->processor.startVideoSyncForIntegrationTest())
            result = fail("alpha loopback video helper did not start");
        std::cout << "phase: alpha helper port=" << alpha->processor.getVideoHelperPortForIntegrationTest() << std::endl;
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
        int stableAlphaSnapshotsWithRefresh = 0;
        juce::String lastStableAlphaPayload;
        std::vector<int> alphaBufferSamples;
        std::vector<int> bravoBufferSamples;
        if (result == 0)
        {
            // Model the real helper's HTTP polling, including a background-tab
            // cadence that can be slower than the native 500 ms payload writes.
            // Each response is a complete snapshot: a helper opening or reloading
            // after the initial refresh event must still recover the stable buffer.
            pump({ alpha.get(), bravo.get() }, 5200, [&]
            {
                audioGuidTiming.sample(*alpha, *bravo);
                const auto payload = fetchIntervals(alpha->processor.getVideoHelperPortForIntegrationTest());
                const auto alphaObservation = observeRemotePayload(payload, "bravo");
                if (alphaObservation.found && alphaObservation.bufferCalculated && alphaObservation.receiverBufferEmitted)
                    alphaBufferSamples.push_back(alphaObservation.receiverBufferMs);
                const auto bravoObservation = observeRemote(bravo->processor.getVideoHelperPortForIntegrationTest(), "alpha");
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
                            ++stableAlphaSnapshotsWithRefresh;
                    }
                }
            }, 1000.0);
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
        const double expectedAlphaBufferMs = positiveModulo(alphaIntervalStartMs - bravoIntervalStartMs, intervalDurationMs);
        const double expectedBravoBufferMs = positiveModulo(bravoIntervalStartMs - alphaIntervalStartMs, intervalDurationMs);
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
            && std::abs((double)stableAlphaBufferMs - audioGuidTiming.alphaPlaybackDelayMs.back()) > 120.0)
            result = fail("side-signal jitter moved alpha's buffer one interval away from actual audio playback");

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
        if (result == 0 && stableAlphaSnapshotsWithRefresh != 0)
            result = fail("the one-shot buffer refresh event remained in stable helper snapshots");

        if (result == 0)
        {
            alpha->processor.requestVideoBufferRefreshForIntegrationTest();
            // Skip the first nominal 500 ms poll. The same event must still be
            // present for the next poll, then expire without requiring an ACK.
            pump({ alpha.get(), bravo.get() }, 700);
            const auto retainedRefresh = observeRemote(alpha->processor.getVideoHelperPortForIntegrationTest(), "bravo");
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
                pump({ alpha.get(), bravo.get() }, 700);
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
