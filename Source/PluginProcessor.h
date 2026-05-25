#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <JuceHeader.h>
#include <future>
#include <atomic>
#include <memory>

#include "ninjam/njclient.h"

class NinjamVst3AudioProcessorEditor;
class LocalVideoHttpServer;
class AsyncChatTranslationWorker;
class LocalChordAnalyzer;

namespace ableton
{
class LinkAudio;
class LinkAudioSink;
class LinkAudioSource;
}

class NinjamVst3AudioProcessor : public juce::AudioProcessor,
                                 public juce::Timer
{
    friend class NinjamVst3AudioProcessorEditor;
    friend class AsyncChatTranslationWorker;
public:
    NinjamVst3AudioProcessor();
    ~NinjamVst3AudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Timer callback for NINJAM client Run()
    void timerCallback() override;

    NJClient& getClient() { return ninjamClient; }

    // NINJAM actions
    void connectToServer(juce::String host, juce::String user, juce::String pass);
    void disconnectFromServer();
    void sendChatMessage(juce::String msg);
    
    // Metronome
    void setMetronomeVolume(float vol);
    float getMetronomeVolume() const;
    void setMetronomeMuted(bool shouldMute);
    bool isMetronomeMuted() const;
    void setStoredMetronomeVolume(float vol);
    float getStoredMetronomeVolume() const;

    // Local Channel
    void setTransmitLocal(bool shouldTransmit);
    bool isTransmittingLocal() const;
    void setLocalBitrate(int bitrate);
    int getLocalBitrate() const;
    void setVoiceChatMode(bool enabled);
    bool isVoiceChatMode() const;

    // Chat
    juce::StringArray getChatMessages();
    void setAutoTranslateEnabled(bool shouldEnable);
    bool isAutoTranslateEnabled() const;
    void setTranslateSourceLang(const juce::String& langCode);
    juce::String getTranslateSourceLang() const;
    void setTranslateTargetLang(const juce::String& langCode);
    juce::String getTranslateTargetLang() const;

    struct PublicServerInfo {
        juce::String host;
        int port;
        juce::String name;
        int bpi;
        float bpm;
        int userCount;
        int userMax;
    };

    std::vector<PublicServerInfo> getPublicServers() const;
    void refreshPublicServers();

    // User List
    struct UserInfo {
        int index;
        juce::String name;
        float volume;
        float pan;
        bool isMuted;
        bool isSolo;
        int outputChannel; // 0=Main, 2=Out2, etc.
        bool outputUsesLinkAudio = false;
        int numChannels = 1;          // number of active NINJAM channels for this user
        bool isMultiChanPeer = false;  // has more than 1 NINJAM channel
        juce::StringArray channelNames; // name of each NINJAM channel (index 0..numChannels-1)
    };
    std::vector<UserInfo> getConnectedUsers();
    void setUserOutput(int userIndex, int outputChannelIndex);
    bool setUserOutputToLinkAudio(int userIndex);
    void setUserLevel(int userIndex, float volume, float pan, bool isMuted, bool isSolo);
    void setUserVolume(int userIndex, float volume);
    float getUserPeak(int userIndex, int channelIndex); // 0=L, 1=R
    float getUserChannelPeak(int userIndex, int njChanIdx, int lrSide); // per NINJAM channel L/R peak
    void setUserNjChannelVolume(int userIndex, int njChanIdx, float volume); // individual NINJAM channel volume
    juce::String getUserChordLabel(int userIndex) const;
    double getUserChordCpuPercent(int userIndex) const;
    int getUserChordMemoryKb(int userIndex) const;
    void setChordDetectionEnabled(bool enabled);
    bool isChordDetectionEnabled() const;
    void setUserChordDetectionEnabled(int userIndex, bool enabled);
    bool isUserChordDetectionEnabled(int userIndex) const;

    void setMasterOutputGain(float gain);
    float getMasterOutputGain() const;
    float getMasterPeak() const;
    float getMasterPeakLeft() const;
    float getMasterPeakRight() const;
    
    // Version information
    juce::String getVersionString() const;
    
    void setSoftLimiterEnabled(bool shouldEnable);
    bool isSoftLimiterEnabled() const;
    void setUserClipEnabled(int userIndex, bool enabled);
    bool isUserClipEnabled(int userIndex) const;
    void setMasterLimiterEnabled(bool shouldEnable);
    bool isMasterLimiterEnabled() const;
    float getLimiterThreshold() const { return limiterThresholdDb.load(); }
    float getLimiterRelease() const { return limiterReleaseMs.load(); }
    void setLimiterThreshold(float db);
    void setLimiterRelease(float ms);
    void setLocalInputGain(float gain);
    float getLocalInputGain() const;
    static constexpr int maxLocalChannels = 8;
    static constexpr int maxRemoteChordUsers = 32;
    void setNumLocalChannels(int num);
    int getNumLocalChannels() const;
    void setLocalChannelName(int channel, const juce::String& name);
    juce::String getLocalChannelName(int channel) const;
    void setLocalChannelGain(int channel, float gain);
    float getLocalChannelGain(int channel) const;
    NinjamVst3AudioProcessorEditor* getEditor() const { return (NinjamVst3AudioProcessorEditor*)getActiveEditor(); }
    void setLocalChannelInput(int channel, int inputIndex);
    int getLocalChannelInput(int channel) const;
    void setLocalChannelUsesLinkAudioInput(int channel, bool shouldUse);
    bool isLocalChannelUsingLinkAudioInput(int channel) const;
    float getLocalChannelPeak(int channel) const;
    float getLocalChannelPeakLeft(int channel) const;
    float getLocalChannelPeakRight(int channel) const;
    juce::String getLocalChordLabel() const;
    double getLocalChordCpuPercent() const;
    int getLocalChordMemoryKb() const;
    void setLocalMonitorEnabled(bool enabled);
    bool isLocalMonitorEnabled() const;
    void setFxReverbEnabled(bool enabled);
    bool isFxReverbEnabled() const;
    void setFxDelayEnabled(bool enabled);
    bool isFxDelayEnabled() const;
    void setFxReverbRoomSize(float roomSize);
    float getFxReverbRoomSize() const;
    void setFxReverbDamping(float damping);
    float getFxReverbDamping() const;
    void setFxReverbWetDryMix(float wetDryMix);
    float getFxReverbWetDryMix() const;
    void setFxReverbEarlyReflections(float earlyReflections);
    float getFxReverbEarlyReflections() const;
    void setFxReverbTail(float tail);
    float getFxReverbTail() const;
    void setFxDelayTimeMs(float timeMs);
    float getFxDelayTimeMs() const;
    void setFxDelaySyncToHost(bool enabled);
    bool isFxDelaySyncToHost() const;
    void setFxDelayDivision(int division);
    int getFxDelayDivision() const;
    void setFxDelayPingPong(bool enabled);
    bool isFxDelayPingPong() const;
    void setFxDelayWetDryMix(float wetDryMix);
    float getFxDelayWetDryMix() const;
    void setFxDelayFeedback(float feedback);
    float getFxDelayFeedback() const;
    void setLocalChannelReverbSend(int channel, float send);
    float getLocalChannelReverbSend(int channel) const;
    void setLocalChannelDelaySend(int channel, float send);
    float getLocalChannelDelaySend(int channel) const;

    // NINJAM callbacks
    static int LicenseAgreementCallback(void* userData, const char* licensetext);
    static void ChatMessage_Callback(void* userData, NJClient* inst, const char** parms, int nparms);
    static void IntervalMediaItem_Callback(void* userData, NJClient* inst, const char* username, int chidx, unsigned int fourcc, const unsigned char* guid, const void* data, int dataLen);
    static void IntervalChunkCallback_cb(void* userData, NJClient* inst, const char* username, int chidx, unsigned int fourcc, const unsigned char* guid, const void* data, int dataLen, int flags);
    static void NewIntervalCallback_cb(void* userData, NJClient* inst);

    // Interval / BPI
    int getBPI();
    float getIntervalProgress();
    float getBPM();
    int getIntervalIndex() const;
    int getCodecMode() const;
    unsigned int getVorbisMask() const;
    unsigned int getOpusMask() const;

    float getLocalPeak() const;
    float getLocalPeakLeft() const;
    float getLocalPeakRight() const;

    void sendSideSignal(const juce::String& target, const juce::String& type, const juce::String& payload);
    void sendIntervalSignal(const juce::String& type, const juce::String& payload);
    void processSyncSignal(const juce::String& sender, const juce::String& type, const juce::String& payload);
    void launchVideoSession();
    void launchVideoSessionAsync();

    void rememberUserVolume(int userIndex, float volume, const juce::String& name);
    void resetRemoteUserIndexState(int userIndex, const juce::String& userName);

    void setSpreadOutputsEnabled(bool shouldEnable);
    bool isSpreadOutputsEnabled() const;

    enum class SyncMode : int
    {
        off = 0,
        host = 1,
        abletonLink = 2
    };

    struct LinkAudioChannelInfo
    {
        juce::String key;
        juce::String name;
        juce::String peerName;
    };

    void setSyncMode(SyncMode newMode);
    SyncMode getSyncMode() const;
    bool isTransportSyncEnabled() const;
    void setSyncToHost(bool shouldSync);
    bool isSyncToHostEnabled() const;
    bool isAbletonLinkTransportEnabled() const;
    void setSyncStartCompensationMs(float ms);
    float getSyncStartCompensationMs() const;
    bool getHostPosition(juce::AudioPlayHead::CurrentPositionInfo& info) const;
    void setLinkAudioEnabled(bool shouldEnable);
    bool isLinkAudioEnabled() const;
    void setLinkAudioSendEnabled(bool shouldEnable);
    bool isLinkAudioSendEnabled() const;
    void setLinkAudioReceiveEnabled(bool shouldEnable);
    bool isLinkAudioReceiveEnabled() const;
    void setLinkAudioReceiveSelection(const juce::String& channelKey);
    juce::String getLinkAudioReceiveSelection() const;
    std::vector<LinkAudioChannelInfo> getLinkAudioAvailableChannels() const;
    double getLinkTempoBpm() const;
    bool isLinkTransportPlaying() const;
    int getLinkPeerCount() const;
    void setMtcOutputEnabled(bool shouldEnable);
    bool isMtcOutputEnabled() const;
    void setMtcFrameRate(int fps);
    int getMtcFrameRate() const;
    bool isStandaloneInstance() const;
    struct MidiControllerEvent
    {
        bool isController = false;
        int midiChannel = 1;
        int number = 0;
        int value = 0;
        float normalized = 0.0f;
        bool isNoteOn = false;
    };
    struct OscRelayEvent
    {
        juce::String senderKey;
        juce::String address;
        float normalized = 0.0f;
        bool binaryOn = false;
    };
    std::vector<MidiControllerEvent> popPendingMidiControllerEvents();
    std::vector<OscRelayEvent> popPendingOscRelayEvents();
    void setMidiRelayTarget(const juce::String& targetUser);
    juce::String getMidiRelayTarget() const;
    void setMidiLearnStateJson(const juce::String& json);
    juce::String getMidiLearnStateJson() const;
    void setOscLearnStateJson(const juce::String& json);
    juce::String getOscLearnStateJson() const;
    void setMidiLearnInputDeviceId(const juce::String& deviceId);
    juce::String getMidiLearnInputDeviceId() const;
    void setMidiRelayInputDeviceId(const juce::String& deviceId);
    juce::String getMidiRelayInputDeviceId() const;
    void enqueueExternalMidiControllerEvent(const MidiControllerEvent& event, bool forLearn, bool forRelay);
    void enqueueOutboundOscRelayEvent(const OscRelayEvent& event);

    bool isOpusSyncAvailable() const;
    juce::String getIntervalSyncStatusText() const;

private:
    struct LinkTimingState;
    int getSyncStartCompensationSamples() const;
    void primeSyncTransportStart(const juce::AudioPlayHead::CurrentPositionInfo* hostInfo = nullptr);
    void primeLinkTransportStart(double phaseBeats, double quantum, double tempoBpm);
    void refreshAbletonLinkActivation();
    void rebuildLinkAudioEndpoints();
    void mixReceivedLinkAudioIntoBuffer(juce::AudioBuffer<float>& buffer, int numSamples);
    juce::String buildLinkAudioChannelKey(const juce::String& peerName, const juce::String& channelName) const;
    juce::String getLinkPeerName() const;
    NJClient ninjamClient;
    juce::CriticalSection processLock;
    mutable juce::CriticalSection serverListLock;
    std::vector<PublicServerInfo> publicServers;
    
    // Chat storage
    juce::CriticalSection chatLock;
    juce::StringArray chatHistory;
    juce::StringArray chatSenders;  // parallel: "me", username, or "" for system
    std::atomic<int> chatRevision { 0 };
    bool autoTranslate = false;
    juce::String translateSourceLang = "auto";
    juce::String translateTargetLang = "system";
    std::atomic<juce::uint64> translationConfigRevision { 0 };
    bool translationFailureActive = false;
    double lastTranslationFailureNoticeMs = 0.0;
    juce::String lastTranslationFailureReason;
    std::unique_ptr<AsyncChatTranslationWorker> asyncChatTranslationWorker;
    
    // Local state
    bool isTransmitting = false;
    int localBitrate = 128;
    bool voiceChatMode = false;
    int lastStatus = 0;
    std::atomic<bool> metronomeMuted { false };
    std::atomic<float> storedMetronomeVolume { 1.0f };
    
    juce::AudioBuffer<float> tempInputBuffer;
    juce::AudioBuffer<float> localChannelBuffer;
    juce::AudioBuffer<float> localMixBuffer;   // 1-ch mix used by multiChanAuto Vorbis slot
    std::unique_ptr<LocalChordAnalyzer> localChordAnalyzer;
    std::array<std::unique_ptr<LocalChordAnalyzer>, maxRemoteChordUsers> remoteChordAnalyzers;
    std::atomic<bool> chordDetectionEnabled { true };
    std::array<std::atomic<bool>, maxRemoteChordUsers> remoteChordDetectionEnabled;
    std::array<juce::String, maxRemoteChordUsers> remoteChordUserKeys;
    std::atomic<float> masterOutputGain { 1.0f };
    std::atomic<float> localInputGain { 1.0f };
    std::atomic<float> masterPeak { 0.0f };
    std::atomic<float> masterPeakL { 0.0f };
    std::atomic<float> masterPeakR { 0.0f };
    std::atomic<float> localPeak { 0.0f };
    std::atomic<float> localPeakL { 0.0f };
    std::atomic<float> localPeakR { 0.0f };
    std::array<std::atomic<float>, maxLocalChannels> localChannelGains;
    std::array<std::atomic<float>, maxLocalChannels> localChannelPeaks;
    std::array<std::atomic<float>, maxLocalChannels> localChannelPeaksL;
    std::array<std::atomic<float>, maxLocalChannels> localChannelPeaksR;
    std::array<std::atomic<int>, maxLocalChannels> localChannelInputs;
    std::array<std::atomic<float>, maxLocalChannels> localChannelReverbSends;
    std::array<std::atomic<float>, maxLocalChannels> localChannelDelaySends;
    juce::CriticalSection localChannelNamesLock;
    std::array<juce::String, maxLocalChannels> localChannelNames; // user-defined channel names
    std::atomic<int> numLocalChannels { 1 };
    std::atomic<bool> localMonitorEnabled { true };
    std::atomic<bool> fxReverbEnabled { true };
    std::atomic<bool> fxDelayEnabled { true };
    std::atomic<float> fxReverbRoomSize { 0.45f };
    std::atomic<float> fxReverbDamping { 0.45f };
    std::atomic<float> fxReverbWetDryMix { 1.0f };
    std::atomic<float> fxReverbEarlyReflections { 0.25f };
    std::atomic<float> fxReverbTail { 0.75f };
    std::atomic<float> fxDelayTimeMs { 320.0f };
    std::atomic<bool> fxDelaySyncToHost { true };
    std::atomic<int> fxDelayDivision { 8 };
    std::atomic<bool> fxDelayPingPong { false };
    std::atomic<float> fxDelayWetDryMix { 1.0f };
    std::atomic<float> fxDelayFeedback { 0.38f };
    juce::Reverb fxReverb;
    juce::AudioBuffer<float> fxDelayBuffer;
    juce::AudioBuffer<float> fxReverbInputBuffer;
    juce::AudioBuffer<float> fxDelayInputBuffer;
    juce::AudioBuffer<float> fxTransmitBuffer;
    juce::AudioBuffer<float> fxReturnBuffer;
    int fxDelayWritePosition = 0;
    double processingSampleRate = 44100.0;
    std::atomic<bool> spreadOutputsEnabled { false };
    std::atomic<bool> softLimiterEnabled { true };
    std::atomic<bool> dspLimiterEnabled { false };
    std::atomic<float> limiterThresholdDb { 0.0f };
    std::atomic<float> limiterReleaseMs { 100.0f };
    juce::dsp::Limiter<float> masterLimiter;

    std::map<int, bool> userClipEnabled;
    std::map<int, float> userPanOverrides;
    std::map<juce::String, int> userOutputAssignment;
    std::map<int, float> userBaseVolume;
    std::map<juce::String, float> userVolumeByName;
    std::map<int, juce::String> remoteUserNameByIndex;

    std::atomic<SyncMode> syncMode { SyncMode::off };
    std::atomic<bool> hostWasPlaying { false };
    std::atomic<bool> linkWasPlaying { false };
    std::atomic<bool> syncAwaitingHostRestart { false };
    std::atomic<bool> syncWaitForInterval { false };
    std::atomic<int> syncTargetInterval { -1 };
    std::atomic<int> syncDisplayIntervalOffset { 0 };
    std::atomic<int> syncDisplayPositionOffset { 0 };
    std::atomic<float> syncStartCompensationMs { 0.0f };
    std::atomic<bool> mtcOutputEnabled { true };
    std::atomic<int> mtcFrameRateFps { 30 };
    bool mtcWasRunning = false;
    double mtcSamplesUntilNextQuarterFrame = 0.0;
    int mtcQuarterFramePiece = 0;
    mutable juce::CriticalSection transportLock;
    juce::AudioPlayHead::CurrentPositionInfo lastHostPosition;
    mutable juce::CriticalSection linkTransportStateLock;
    double lastLinkTempo = 120.0;
    double lastLinkPhaseBeats = 0.0;
    int lastLinkPeerCount = 0;
    bool lastLinkIsPlaying = false;
    std::unique_ptr<ableton::LinkAudio> abletonLink;
    std::unique_ptr<LinkTimingState> linkTimingState;
    std::atomic<bool> linkAudioEnabled { false };
    std::atomic<bool> linkAudioSendEnabled { true };
    std::atomic<bool> linkAudioReceiveEnabled { false };
    mutable juce::CriticalSection linkAudioSelectionLock;
    juce::String linkAudioReceiveSelection;
    juce::SpinLock linkAudioEndpointLock;
    std::unique_ptr<ableton::LinkAudioSink> abletonLinkSink;
    std::unique_ptr<ableton::LinkAudioSource> abletonLinkSource;
    std::map<juce::String, std::unique_ptr<ableton::LinkAudioSink>> remoteLinkAudioSinks;
    std::map<juce::String, int> remoteLinkAudioOutputPairs;
    juce::SpinLock linkAudioReceiveLock;
    std::vector<float> pendingLinkAudioSamples;
    int pendingLinkAudioNumChannels = 0;
    int pendingLinkAudioNumFrames = 0;
    bool pendingLinkAudioReady = false;
    size_t linkAudioMaxNumSamples = 8192;
    double lastLinkAudioEndpointRefreshMs = 0.0;

    std::atomic<int> intervalIndex { 0 };
    std::atomic<int> lastIntervalPos { 0 };
    std::atomic<juce::uint64> sideSignalEventCounter { 0 };
    juce::String currentServer;
    juce::String currentUser;
    juce::File videoHelperRootDir;
    mutable juce::CriticalSection intervalHelperPayloadLock;
    juce::String intervalHelperPayload { "[]" };
    std::atomic<bool> videoHelperRunning { false };
    std::atomic<bool> videoLaunchInProgress { false };
    juce::CriticalSection videoLaunchWorkerLock;
    std::future<void> videoLaunchFuture;
    std::unique_ptr<LocalVideoHttpServer> advancedVideoServer;
    std::map<juce::String, int> remoteLatencyFirmDelayMsByUser;

    std::atomic<bool> opusSyncAvailable { false };
    std::atomic<bool> opusSyncHasLegacyClients { false };
    std::atomic<bool> opusSyncServerSupported { false };
    mutable juce::CriticalSection intervalSyncStatusLock;
    juce::String intervalSyncStatusText;
    std::atomic<long long> lastBroadcastIntervalTag { -1 };
    std::atomic<long long> lastProcessedIntervalMarkerKey { -1 };
    juce::CriticalSection intervalSyncAnnouncementLock;
    std::map<juce::String, long long> lastAnnouncedRemoteIntervalByUser;
    std::map<int, double> localIntervalStartMsByInterval;
    struct PendingRemoteIntervalStart
    {
        int remoteInterval = -1;
        int remoteIntervalAbsolute = -1;
        int remoteBeat = 0;
        int remoteBpi = 0;
        int remoteServerLatencyMs = -1;
        juce::String senderKey;
        juce::String displaySender;
        long long receivedSampleCount = -1;
        double receivedAtMs = -1.0;
    };
    std::map<juce::String, PendingRemoteIntervalStart> pendingRemoteIntervalStartsByUser;
    std::map<juce::String, int> lastRemoteServerLatencyMsByUser;
    std::map<juce::String, double> pendingTransportProbeSentMsById;
    std::map<juce::String, long long> remoteLatencyLastAppliedIntervalByUser;
    int lastLatencyTimingBpi = -1;
    int lastLatencyTimingLength = -1;
    double lastLatencyTimingBpm = -1.0;
    std::atomic<int> localServerLatencyMs { -1 };
    std::atomic<int> lastServerLatencyProbeInterval { -1 };
    std::atomic<bool> serverLatencyProbeInProgress { false };
    std::future<void> serverLatencyProbeFuture;
    double lastServerLatencyProbeAttemptMs = 0.0;
    double lastRemoteSyncUserPruneMs = 0.0;
    struct RemoteLatencyAverageState
    {
        int sampleCount = 0;
        double sumMs = 0.0;
        double averageMs = 0.0;
        double firmAverageMs = 0.0;
        double lastMeasurementMs = -1.0;
        double baseMarkerCorrectedDelayMs = -1.0;
    };
    std::map<juce::String, RemoteLatencyAverageState> remoteLatencyAverageByUser;
    juce::CriticalSection opusSyncPeerLock;
    struct OpusSyncPeerState
    {
        juce::String userId;
        bool supportsOpus = false;
        bool multiChanEnabled = false;
        int numChannels = 1;           // number of local channels the peer is sending
        juce::String appFamily;
        int handshakeVersion = 0;
        juce::String runtimeFormat;
        juce::String pluginVersion;
        double lastSeenMs = 0.0;
    };
    std::map<juce::String, OpusSyncPeerState> opusSyncPeers;
    // Simple username→{isMultiChan, numChannels} snapshot updated by refreshOpusSyncAvailabilityFromUsers().
    // Keyed by normalised username (no @host, lowercase). Read without holding opusSyncPeerLock.
    struct PeerMultiChanInfo { bool isMultiChan = false; int numChannels = 1; };
    std::map<juce::String, PeerMultiChanInfo> peerMultiChanByName;
    juce::CriticalSection peerMultiChanLock;
    juce::String opusSyncInstanceId;
    double lastOpusSupportBroadcastMs = 0.0;
    std::atomic<juce::uint64> transportProbeCounter { 0 };
    std::atomic<long long> intervalSyncSampleCounter { 0 };
    juce::SpinLock midiEventQueueLock;
    std::vector<MidiControllerEvent> pendingMidiControllerEvents;
    juce::SpinLock outboundMidiRelayQueueLock;
    std::vector<MidiControllerEvent> pendingOutboundMidiRelayEvents;
    juce::SpinLock inboundMidiRelayQueueLock;
    std::vector<MidiControllerEvent> pendingInboundMidiRelayEvents;
    juce::SpinLock outboundOscRelayQueueLock;
    std::vector<OscRelayEvent> pendingOutboundOscRelayEvents;
    juce::SpinLock inboundOscRelayQueueLock;
    std::vector<OscRelayEvent> pendingInboundOscRelayEvents;
    mutable juce::CriticalSection midiRelayTargetLock;
    juce::String midiRelayTarget { "*" };
    mutable juce::CriticalSection learnStateLock;
    juce::String midiLearnStateJson;
    juce::String oscLearnStateJson;
    juce::String midiLearnInputDeviceId;
    juce::String midiRelayInputDeviceId;

    void addSystemChatMessage(const juce::String& message);
    void noteTranslationFailure(const juce::String& reason);
    void clearTranslationFailureState();
    juce::String translateText(const juce::String& text);
    juce::String translateTextForTarget(const juce::String& text, const juce::String& targetCode);
    void enqueueAsyncTranslation(const juce::String& originalLine,
                                 const juce::String& lineSender,
                                 const juce::String& linePrefix,
                                 const juce::String& lineBody);
    void applyAsyncTranslatedChatLine(const juce::String& originalLine,
                                      const juce::String& lineSender,
                                      const juce::String& translatedLine,
                                      juce::uint64 configRevision);
    bool isStandaloneWrapper() const;
    int getDisplayIntervalIndex() const;
    void emitMidiTimecode(juce::MidiBuffer& midiMessages, int numSamples, int pos, int length);
    void broadcastOpusSyncSupport(const juce::String& target = "*");
    void refreshOpusSyncAvailabilityFromUsers();
    void applyCodecPreference();
    void setIntervalSyncStatusText(const juce::String& text);
    void broadcastIntervalSyncTag(const juce::String& target = "*", int markerBeatIndex = -1);
    void broadcastTransportProbe(const juce::String& target = "*");
    void measureServerLatencyAsync();
    juce::String buildIntervalSyncTag(int interval, int length) const;
    void invalidateIntervalSyncLatencyState(bool keepRemoteServerLatency);
    void pruneDisconnectedRemoteSyncState();
    void processPendingIntervalSyncMarkers(int localMarkerBeat, long long localMarkerSampleCount, double intervalDurationMs);
    juce::File resolveVideoHelperRootDir() const;
    bool isAdvancedVideoClientAvailable() const;
    bool ensureAdvancedVideoClientStarted();
    void stopAdvancedVideoClient();
    void writeIntervalHelperJson(int pos, int length);
    void syncLocalIntervalChannelConfig();
    void flushOutboundMidiRelayEvents();
    void flushOutboundOscRelayEvents();
    void injectInboundMidiRelayEvents(juce::MidiBuffer& midiMessages);
    static void RemoteChannelAudioTap_Callback(void* userData,
                                               int useridx,
                                               const char* username,
                                               int channelidx,
                                               const float* interleaved,
                                               int numChannels,
                                               int numFrames,
                                               int sampleRate);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NinjamVst3AudioProcessor)
};
