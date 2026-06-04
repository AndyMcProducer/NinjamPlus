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
#include <deque>
#include <array>

#include "ninjam/njclient.h"
#include "ZapVideoCodec.h"

#ifndef NINJAMPLUS_HAS_H264_DECODE
#define NINJAMPLUS_HAS_H264_DECODE 0
#endif

#if defined(NINJAMPLUS_HAS_PROVIDEO) && NINJAMPLUS_HAS_PROVIDEO
#include "ProVideoDecoder.h"
#else
class ProVideoDecoder;
#endif

class NinjamVst3AudioProcessorEditor;
class LocalVideoHttpServer;
class ZapVideoDecodeWorker;
class ZapCameraSender;
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
    friend class ZapVideoDecodeWorker;
    friend class ZapCameraSender;
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
    void sendChatAttachment(const juce::String& kind, const juce::String& url);
    
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
    void setLocalChatColourKey(const juce::String& colourKey);
    juce::String getLocalChatColourKey() const;
    juce::String getChatColourKeyForSender(const juce::String& sender) const;
    void broadcastChatStyle();
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
    enum class FxDelayMode
    {
        standard = 0,
        frippertronics = 1
    };
    void setFxDelayMode(FxDelayMode mode);
    FxDelayMode getFxDelayMode() const;
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

    // Sample pads
    static constexpr int numSamplePads = 16;
    static constexpr int numSamplePadFxSlots = 8;
    enum class SamplePadFxType
    {
        reverb = 0,
        delay = 1,
        djFilter = 2,
        djFilterBp = 3,
        phaser = 4,
        djFilterHp = 5,
        djFilterLp = 6,
        delayQuarter = 7,
        delayQuarterPingPong = 8,
        phaserHalf = 9
    };
    static constexpr int looperInputLocalChannel = -10000;
    static constexpr int looperInputSamplePads = -10001;
    static constexpr int looperInputRemoteUserBase = -20000;
    static constexpr const char* samplePadsMidiInputRelayId = "__pads_relay__";
    static int looperInputForRemoteUser(int userIndex) { return looperInputRemoteUserBase - juce::jmax(0, userIndex); }
    static int remoteUserIndexForLooperInput(int inputIndex) { return looperInputRemoteUserBase - inputIndex; }
    static bool isLooperInputRemoteUser(int inputIndex)
    {
        const int userIndex = remoteUserIndexForLooperInput(inputIndex);
        return inputIndex <= looperInputRemoteUserBase && userIndex >= 0 && userIndex < maxRemoteChordUsers;
    }
    bool loadSamplePad(int padIndex, const juce::File& file);
    void clearSamplePad(int padIndex);
    void clearAllSamplePads();
    void resetSamplePadSettings();
    void undoSamplePadClear(int padIndex);
    bool canUndoSamplePadClear(int padIndex) const;
    void triggerSamplePad(int padIndex);
    void stopSamplePad(int padIndex);
    void setSamplePadRecordArmed(int padIndex, bool shouldArm);
    void armSamplePadLooper(int padIndex, bool matchBpi);
    void scheduleSamplePadBpiRecordStartAtNextInterval(int padIndex);
    bool isSamplePadRecordArmed(int padIndex) const;
    bool isSamplePadRecording(int padIndex) const;
    bool isSamplePadPlaying(int padIndex) const;
    bool isSamplePadWaitingForBpiLoop(int padIndex) const;
    void setSamplePadMatchBpiEnabled(int padIndex, bool shouldEnable);
    bool isSamplePadMatchBpiEnabled(int padIndex) const;
    void setSamplePadBpmSyncEnabled(int padIndex, bool shouldEnable);
    bool isSamplePadBpmSyncEnabled(int padIndex) const;
    enum class SamplePadPlaybackSpeed
    {
        half = -1,
        normal = 0,
        doubleSpeed = 1
    };
    void setSamplePadPlaybackSpeed(int padIndex, SamplePadPlaybackSpeed speed);
    SamplePadPlaybackSpeed getSamplePadPlaybackSpeed(int padIndex) const;
    void resyncSamplePadToNinjamBpm(int padIndex);
    void syncSamplePadLoopToBeat(int padIndex);
    void undoSamplePadBpmResync(int padIndex);
    bool canUndoSamplePadBpmResync(int padIndex) const;
    void setSamplePadLoopEnabled(int padIndex, bool shouldLoop);
    bool isSamplePadLoopEnabled(int padIndex) const;
    void setSamplePadReverseEnabled(int padIndex, bool shouldReverse);
    bool isSamplePadReverseEnabled(int padIndex) const;
    bool hasSamplePadSample(int padIndex) const;
    juce::String getSamplePadName(int padIndex) const;
    void setSamplePadName(int padIndex, const juce::String& name);
    int getSamplePadLoopLengthBeats(int padIndex) const;
    float getSamplePadLoopProgress(int padIndex) const;
    int getSamplePadTriggerFlashCounter(int padIndex) const;
    bool triggerSamplePadForMidiNote(int noteNumber);
    bool handleSamplePadMidiNote(int noteNumber, bool isNoteOn);
    bool handleSamplePadMidiPadState(int padIndex, bool isDown);
    void setSamplePadVolume(float gain);
    float getSamplePadVolume() const;
    void setSamplePadLimiterEnabled(bool shouldEnable);
    bool isSamplePadLimiterEnabled() const;
    void setSamplePadDuckEnabled(bool shouldEnable);
    bool isSamplePadDuckEnabled() const;
    enum class SamplePadDuckShape
    {
        smoothPump = 0,
        tightPump = 1,
        slowPump = 2,
        hardGate = 3,
        reverseSwell = 4,
        notchPulse = 5
    };
    enum class SamplePadDuckLength
    {
        eighth = 0,
        quarter = 1,
        half = 2
    };
    void setSamplePadDuckShape(SamplePadDuckShape shape);
    SamplePadDuckShape getSamplePadDuckShape() const;
    void setSamplePadDuckLength(SamplePadDuckLength length);
    SamplePadDuckLength getSamplePadDuckLength() const;
    void setSamplePadsUseDefaultFx(bool shouldUse);
    bool getSamplePadsUseDefaultFx() const;
    void setSamplePadDuckRouteEnabled(int padIndex, bool shouldEnable);
    bool isSamplePadDuckRouteEnabled(int padIndex) const;
    void setSamplePadFxSlotRouteEnabled(int padIndex, int slotIndex, bool shouldEnable);
    bool isSamplePadFxSlotRouteEnabled(int padIndex, int slotIndex) const;
    void setSamplePadFxSlotToSlotRouteEnabled(int sourceSlotIndex, int targetSlotIndex, bool shouldEnable);
    bool isSamplePadFxSlotToSlotRouteEnabled(int sourceSlotIndex, int targetSlotIndex) const;
    bool canRouteSamplePadFxSlotToSlot(int sourceSlotIndex, int targetSlotIndex) const;
    float getSamplePadPeak() const;
    void setSamplePadFxSlotType(int slotIndex, SamplePadFxType type);
    SamplePadFxType getSamplePadFxSlotType(int slotIndex) const;
    void setSamplePadFxSlotAmount(int slotIndex, float amount);
    float getSamplePadFxSlotAmount(int slotIndex) const;
    juce::File getSamplePadBanksDirectory() const;
    juce::File getSamplePadBankDirectory(const juce::String& bankName) const;
    juce::StringArray getSamplePadBankNames() const;
    bool saveSamplePadBank(const juce::String& bankName, juce::String& errorMessage);
    bool loadSamplePadBank(const juce::File& bankDirectory, juce::String& errorMessage);
    void beginStandaloneShutdown();

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
    bool isNinjamZapVideoAvailable();
    bool isNinjamZapVideoEnabled() const;
    void launchNinjamZapVideoSession();
    juce::StringArray getNinjamZapCameraDevices() const;
    ninjamplus::zap::CameraCodecPreference getNinjamZapCameraCodecPreference() const;
    void setNinjamZapCameraCodecPreference(ninjamplus::zap::CameraCodecPreference preference);
    ninjamplus::zap::VideoCodec getNinjamZapCameraActiveCodec() const;
    void startNinjamZapCameraSend();
    void startNinjamZapCameraSend(int deviceIndex);
    void startNinjamZapCameraSend(int deviceIndex, ninjamplus::zap::CameraCodecPreference preference);
    void stopNinjamZapCameraSend();
    bool isNinjamZapCameraSending() const;

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
    void setSamplePadsMidiInputDeviceId(const juce::String& deviceId);
    juce::String getSamplePadsMidiInputDeviceId() const;
    void setSamplePadLooperInput(int inputIndex);
    int getSamplePadLooperInput() const;
    void enqueueExternalMidiControllerEvent(const MidiControllerEvent& event, bool forLearn, bool forRelay);
    void enqueueOutboundOscRelayEvent(const OscRelayEvent& event);

    bool isOpusSyncAvailable() const;
    juce::String getIntervalSyncStatusText() const;

private:
    struct ZapVideoFrameInfo
    {
        juce::String streamKey;
        juce::String sender;
        int channelIndex = 0;
        juce::uint64 refreshId = 0;
        double lastUpdateMs = 0.0;
    };

    struct ZapVideoDecodeJob
    {
        juce::String streamKey;
        juce::String sender;
        int channelIndex = 0;
        ninjamplus::zap::VideoCodec codec = ninjamplus::zap::VideoCodec::unknown;
        juce::MemoryBlock payload;
    };

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
    mutable juce::CriticalSection chatStyleLock;
    juce::String localChatColourKey { "aurora" };
    std::map<juce::String, juce::String> chatColourKeyByUser;
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
    std::atomic<int> fxDelayMode { (int)FxDelayMode::standard };
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
    std::array<float, 2> fxDelayLowpassState {};
    double processingSampleRate = 44100.0;

    static constexpr int samplePadOneShotVoiceCount = 4;

    struct SamplePadOneShotVoice
    {
        bool active = false;
        double position = 0.0;
    };

    struct SamplePadState
    {
        juce::AudioBuffer<float> sample;
        juce::AudioBuffer<float> originalSample;
        juce::AudioBuffer<float> undoClearSample;
        juce::AudioBuffer<float> undoClearOriginalSample;
        juce::String name;
        juce::String undoClearName;
        juce::File file;
        juce::File undoClearFile;
        double sourceSampleRate = 44100.0;
        double originalSourceSampleRate = 44100.0;
        double sourceBpm = 0.0;
        double lastSyncedTargetBpm = 0.0;
        double undoClearSourceSampleRate = 44100.0;
        double undoClearOriginalSourceSampleRate = 44100.0;
        double undoClearSourceBpm = 0.0;
        double undoClearLastSyncedTargetBpm = 0.0;
        bool nameIsCustom = false;
        bool bpmSyncApplied = false;
        bool undoClearNameIsCustom = false;
        bool undoClearBpmSyncApplied = false;
        bool undoClearLoop = false;
        bool undoClearReverse = false;
        bool undoClearMatchBpi = false;
        bool undoClearBpmSyncEnabled = true;
        SamplePadPlaybackSpeed undoClearPlaybackSpeed = SamplePadPlaybackSpeed::normal;
        bool undoClearDuckRoute = false;
        std::array<bool, numSamplePadFxSlots> undoClearFxSlotRoutes {};
        bool canUndoClear = false;
        std::atomic<bool> loop { false };
        std::atomic<bool> reverse { false };
        std::atomic<bool> matchBpi { false };
        std::atomic<bool> bpmSyncEnabled { true };
        std::atomic<int> playbackSpeed { (int)SamplePadPlaybackSpeed::normal };
        std::atomic<bool> duckRoute { false };
        std::array<std::atomic<bool>, numSamplePadFxSlots> fxSlotRoutes {};
        std::atomic<bool> playing { false };
        std::atomic<bool> playbackScheduled { false };
        std::atomic<bool> recordArmed { false };
        std::atomic<bool> recordPendingStart { false };
        std::atomic<bool> recordPendingStop { false };
        std::atomic<bool> recordStartScheduled { false };
        std::atomic<bool> recording { false };
        std::atomic<double> position { 0.0 };
        std::atomic<int> activeOneShotVoices { 0 };
        std::atomic<int> triggerFlashCounter { 0 };
        std::array<SamplePadOneShotVoice, samplePadOneShotVoiceCount> oneShotVoices;
        int nextOneShotVoice = 0;
        bool midiHoldActive = false;
        bool midiHoldActionTriggered = false;
        bool midiPadDown = false;
        double midiHoldStartMs = 0.0;
        double lastAcceptedPressMs = -1000.0;
        double scheduledStartBeat = 0.0;
        double recordScheduledStartBeat = 0.0;
        double recordScheduledStopBeat = 0.0;
        double loopAnchorBeat = 0.0;
        double undoClearLoopAnchorBeat = 0.0;
        double recordedStartBeatInInterval = 0.0;
        double undoClearRecordedStartBeatInInterval = 0.0;
        int loopLengthBeats = 0;
        int recordLoopLengthBeatsOverride = 0;
        int undoClearLoopLengthBeats = 0;
        bool recordedLoop = false;
        bool recordAutoStopAtScheduledEnd = false;
        bool recordMatchBpiCanvas = false;
        bool undoClearRecordedLoop = false;
        juce::AudioBuffer<float> recordBuffer;
        int recordWritePosition = 0;
        double recordStartBeat = 0.0;
    };

    juce::AudioFormatManager samplePadFormatManager;
    mutable juce::CriticalSection samplePadsLock;
    std::array<SamplePadState, numSamplePads> samplePads;
    juce::AudioBuffer<float> samplePadsRenderBuffer;
    juce::AudioBuffer<float> samplePadsOneShotRenderBuffer;
    juce::AudioBuffer<float> samplePadRemoteLooperInputBuffer;
    std::array<std::atomic<int>, numSamplePadFxSlots> samplePadFxSlotTypes;
    std::array<std::atomic<float>, numSamplePadFxSlots> samplePadFxSlotAmounts;
    std::array<std::array<std::atomic<bool>, numSamplePadFxSlots>, numSamplePadFxSlots> samplePadFxSlotChainRoutes;
    std::array<std::array<juce::dsp::StateVariableTPTFilter<float>, numSamplePadFxSlots>, numSamplePads> samplePadPerPadDjFilters;
    std::array<std::array<juce::dsp::StateVariableTPTFilter<float>, numSamplePadFxSlots>, numSamplePads> samplePadPerPadDjBpFilters;
    std::array<std::array<juce::dsp::Phaser<float>, numSamplePadFxSlots>, numSamplePads> samplePadPerPadPhasers;
    std::array<std::array<juce::Reverb, numSamplePadFxSlots>, numSamplePads> samplePadPerPadReverbs;
    std::array<std::array<juce::AudioBuffer<float>, numSamplePadFxSlots>, numSamplePads> samplePadPerPadDelayBuffers;
    std::array<std::array<juce::AudioBuffer<float>, numSamplePadFxSlots>, numSamplePads> samplePadPerPadFxSlotInputBuffers;
    juce::AudioBuffer<float> samplePadFxScratchBuffer;
    juce::dsp::Oscillator<float> samplePadDuckOscillator;
    std::vector<float> samplePadDuckGainBuffer;
    std::array<std::array<int, numSamplePadFxSlots>, numSamplePads> samplePadPerPadDelayWritePositions {};
    std::atomic<float> samplePadsVolume { 1.0f };
    std::atomic<bool> samplePadsLimiterEnabled { false };
    std::atomic<bool> samplePadsDuckEnabled { false };
    std::atomic<int> samplePadsDuckShape { (int)SamplePadDuckShape::smoothPump };
    std::atomic<int> samplePadsDuckLength { (int)SamplePadDuckLength::quarter };
    std::atomic<bool> samplePadsUseDefaultFx { true };
    std::atomic<float> samplePadsPeak { 0.0f };
    std::atomic<int> samplePadLooperInput { looperInputLocalChannel };
    static constexpr int remoteAudioTapBufferSamples = 32768;
    juce::SpinLock remoteAudioTapLock;
    std::array<juce::AudioBuffer<float>, maxRemoteChordUsers> remoteAudioTapBuffers;
    std::array<int, maxRemoteChordUsers> remoteAudioTapWritePositions {};
    std::array<int, maxRemoteChordUsers> remoteAudioTapAvailableSamples {};
    bool samplePadTransportInitialised = false;
    int samplePadLastTransportPosition = 0;
    long long samplePadTransportInterval = 0;
    double lastSamplePadBpmSyncBpm = 0.0;

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
    std::atomic<bool> lastHostPositionValid { false };
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
    struct LinkAudioReceiveRing
    {
        explicit LinkAudioReceiveRing(size_t requestedCapacity = 32768)
        {
            setCapacity(requestedCapacity);
        }

        void setCapacity(size_t requestedCapacity)
        {
            size_t cap = 1;
            while (cap < requestedCapacity)
                cap <<= 1;

            left.assign(cap, 0.0f);
            right.assign(cap, 0.0f);
            capacity = cap;
            mask = cap - 1;
            reset();
        }

        void reset() noexcept
        {
            readIndex.store(0, std::memory_order_relaxed);
            writeIndex.store(0, std::memory_order_relaxed);
        }

        size_t available() const noexcept
        {
            const size_t r = readIndex.load(std::memory_order_relaxed);
            const size_t w = writeIndex.load(std::memory_order_acquire);
            return w - r;
        }

        size_t write(const float* leftIn, const float* rightIn, size_t count) noexcept
        {
            const size_t w = writeIndex.load(std::memory_order_relaxed);
            const size_t r = readIndex.load(std::memory_order_acquire);
            const size_t freeFrames = capacity - (w - r);
            const size_t framesToWrite = juce::jmin(count, freeFrames);

            for (size_t i = 0; i < framesToWrite; ++i)
            {
                const size_t index = (w + i) & mask;
                left[index] = leftIn[i];
                right[index] = rightIn[i];
            }

            writeIndex.store(w + framesToWrite, std::memory_order_release);
            return framesToWrite;
        }

        size_t readInterleaved(float* destination, size_t count) noexcept
        {
            const size_t r = readIndex.load(std::memory_order_relaxed);
            const size_t w = writeIndex.load(std::memory_order_acquire);
            const size_t framesToRead = juce::jmin(count, w - r);

            for (size_t i = 0; i < framesToRead; ++i)
            {
                const size_t sourceIndex = (r + i) & mask;
                destination[i * 2u] = left[sourceIndex];
                destination[i * 2u + 1u] = right[sourceIndex];
            }

            readIndex.store(r + framesToRead, std::memory_order_release);
            return framesToRead;
        }

        size_t discard(size_t count) noexcept
        {
            const size_t r = readIndex.load(std::memory_order_relaxed);
            const size_t w = writeIndex.load(std::memory_order_acquire);
            const size_t framesToDiscard = juce::jmin(count, w - r);
            readIndex.store(r + framesToDiscard, std::memory_order_release);
            return framesToDiscard;
        }

        std::vector<float> left;
        std::vector<float> right;
        size_t capacity = 0;
        size_t mask = 0;
        std::atomic<size_t> readIndex { 0 };
        std::atomic<size_t> writeIndex { 0 };
    };
    LinkAudioReceiveRing linkAudioReceiveRing { 32768 };
    std::atomic<juce::uint64> linkAudioFramesReceived { 0 };
    std::atomic<juce::uint64> linkAudioFramesDropped { 0 };
    size_t linkAudioMaxNumSamples = 8192;
    double lastLinkAudioEndpointRefreshMs = 0.0;
    double linkAudioReceiveSelectedMissingSinceMs = 0.0;

    std::atomic<int> intervalIndex { 0 };
    std::atomic<int> lastIntervalPos { 0 };
    std::atomic<juce::uint64> sideSignalEventCounter { 0 };
    juce::String currentServer;
    juce::String currentUser;
    juce::File videoHelperRootDir;
    mutable juce::CriticalSection intervalHelperPayloadLock;
    juce::String intervalHelperPayload { "[]" };
    double lastIntervalHelperPayloadWriteMs = 0.0;
    std::atomic<bool> videoHelperRunning { false };
    std::atomic<bool> videoLaunchInProgress { false };
    std::atomic<bool> ninjamZapVideoEnabled { false };
    std::atomic<bool> ninjamZapVideoReceivedNotice { false };
    juce::CriticalSection ninjamZapVideoChunkLock;
    std::map<juce::String, ninjamplus::zap::ChunkReassembler> ninjamZapVideoChunkReassemblers;
    // Per-user reassembly + decode state (used by helper HTTP server)
    std::map<juce::String, ninjamplus::zap::ChunkReassembler> remoteVideoChunkReassemblersByUser;
#if defined(NINJAMPLUS_HAS_PROVIDEO) && NINJAMPLUS_HAS_PROVIDEO
    std::map<juce::String, std::unique_ptr<ProVideoDecoder>> remoteVideoDecodersByUser;
#endif
    double lastNinjamZapVideoSubscriptionSyncMs = 0.0;
    juce::CriticalSection videoLaunchWorkerLock;
    std::future<void> videoLaunchFuture;
    std::unique_ptr<LocalVideoHttpServer> advancedVideoServer;
    std::unique_ptr<ZapVideoDecodeWorker> zapVideoDecodeWorker;
    std::unique_ptr<ZapCameraSender> zapCameraSender;
    std::atomic<bool> ninjamZapCameraSendEnabled { false };
    std::atomic<int> ninjamZapCameraCodecPreference { (int)ninjamplus::zap::CameraCodecPreference::autoCodec };
    std::atomic<int> ninjamZapCameraActiveCodec { (int)ninjamplus::zap::VideoCodec::mjpeg };
    std::atomic<bool> ninjamZapVideoStreamOpen { false };
    std::array<unsigned char, 16> ninjamZapVideoStreamGuid {};
    juce::SpinLock pendingNinjamZapIntervalLock;
    std::atomic<bool> pendingNinjamZapIntervalRotate { false };
    std::array<unsigned char, 16> pendingNinjamZapAudioGuid {};
    int pendingNinjamZapIntervalCounter = 0;
    juce::SpinLock ninjamZapCameraChunkQueueLock;
    std::vector<juce::MemoryBlock> pendingNinjamZapCameraChunks;
    juce::MemoryBlock ninjamZapCameraH264ConfigChunk;
    mutable juce::CriticalSection zapVideoFrameLock;
    std::map<juce::String, int> remoteLatencyFirmDelayMsByUser;
    std::map<juce::String, juce::uint64> remoteVideoBufferRefreshIdByUser;
    std::map<juce::String, ZapVideoFrameInfo> remoteVideoFrameInfoByUser;
    std::map<juce::String, juce::MemoryBlock> remoteVideoLatestJpegByUser;
    std::map<juce::String, juce::Image> remoteVideoLatestFrameByUser;
    juce::uint64 videoBufferRefreshCounter = 0;

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
    std::deque<juce::String> recentVideoTimingChangeEventIds;
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
    juce::String samplePadsMidiInputDeviceId;

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
    void resetIntervalSyncTimingCache();
    bool consumeVideoTimingChangeEvent(const juce::String& eventId);
    void broadcastVideoTimingChange(double previousBpm, double newBpm, int bpi, int length, int timingDelayDeltaMs);
    juce::File resolveVideoHelperRootDir() const;
    bool isAdvancedVideoClientAvailable() const;
    bool ensureAdvancedVideoClientStarted();
    bool ensureZapVideoClientStarted();
    void stopAdvancedVideoClient();
    void writeIntervalHelperJson(int pos, int length);
    void startZapVideoDecodeWorker();
    void stopZapVideoDecodeWorker();
    void enqueueZapVideoDecodeJob(ZapVideoDecodeJob job);
    int getNinjamZapVideoChannelIndex() const;
    void configureNinjamZapVideoLocalChannel();
    void beginNinjamZapVideoIntervalStream(const unsigned char audioGuid[16], int intervalCounter);
    void requestNinjamZapVideoIntervalRotateFromAudioThread();
    void processPendingNinjamZapVideoIntervalRotate();
    void rotateNinjamZapVideoIntervalStream(const unsigned char audioGuid[16], int intervalCounter);
    void flushPendingNinjamZapCameraVideo();
    void enqueueNinjamZapCameraFrameChunk(juce::MemoryBlock chunk);
    void publishLocalNinjamZapCameraFrame(const juce::Image& frame, const juce::MemoryBlock& encodedJpeg);
    void closeNinjamZapVideoIntervalStream();
    void publishDecodedZapVideoFrame(const ZapVideoDecodeJob& job,
                                     const juce::Image& frame,
                                     const juce::MemoryBlock& encodedJpeg);
    juce::String buildZapVideoFrameListJson() const;
    bool getZapVideoFrameJpeg(const juce::String& streamKey, juce::MemoryBlock& jpegData) const;
    void clearZapVideoFrameState();
    void stopNinjamZapVideoTransportForDisconnect();
    void syncLocalIntervalChannelConfig();
    bool isNinjamRemoteChannelVideoOnly(int userIndex, int channelIndex);
    int syncNinjamZapVideoSubscriptions(bool subscribe);
    void addSystemChatLine(const juce::String& message);
    void flushOutboundMidiRelayEvents();
    void flushOutboundOscRelayEvents();
    void injectInboundMidiRelayEvents(juce::MidiBuffer& midiMessages);
    void clearRemoteAudioTapBuffers();
    bool copyRemoteUserAudioForLooper(int userIndex, int numSamples);
    void updateSamplePadTransport(int transportPosition, int transportLength, int bpi);
    double getSamplePadBlockStartBeat(int transportPosition, int transportLength, int bpi, double& samplesPerBeat);
    void updateSamplePadMidiHolds();
    void processSamplePadLooperRecording(int numSamples,
                                         double blockStartBeat,
                                         double samplesPerBeat,
                                         int bpi,
                                         int totalAvailableInputChannels,
                                         int localLeftIndex = -1,
                                         int localRightIndex = -1);
    bool renderSamplePads(int numSamples, double blockStartBeat, double samplesPerBeat, int bpi);
    void applySamplePadInsertFx(int numSamples, double blockStartBeat, double samplesPerBeat, int bpi);
    float getSamplePadFxSendAmount(SamplePadFxType type) const;
    void resyncLoopedSamplePadsToBpm(double targetBpm);
    void resyncSamplePadToBpm(int padIndex, double targetBpm, bool force);
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

inline bool NinjamVst3AudioProcessor::isSamplePadPlaying(int padIndex) const
{
    if (padIndex < 0 || padIndex >= numSamplePads)
        return false;

    const auto& pad = samplePads[(size_t)padIndex];
    return pad.playing.load(std::memory_order_relaxed)
        || pad.activeOneShotVoices.load(std::memory_order_relaxed) > 0;
}

inline bool NinjamVst3AudioProcessor::isSamplePadWaitingForBpiLoop(int padIndex) const
{
    if (padIndex < 0 || padIndex >= numSamplePads)
        return false;

    const auto& pad = samplePads[(size_t)padIndex];
    return pad.recordStartScheduled.load(std::memory_order_relaxed)
        && pad.matchBpi.load(std::memory_order_relaxed);
}
