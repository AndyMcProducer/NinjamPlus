#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>

// SessionRecorder captures all audio tracks during a NINJAM session and
// writes them as separate WAV files in a subfolder. Each track (master mix,
// local channels, remote users) gets its own .wav file.
//
// Audio is captured on the real-time audio thread via writeBlock() calls
// and buffered in lock-free ring buffers. A background writer thread
// drains the ring buffers and writes PCM to disk.
class SessionRecorder
{
public:
    static constexpr int maxRemoteUsers = 32;
    static constexpr int maxLocalChannels = 8;

    SessionRecorder();
    ~SessionRecorder();

    // Starts recording. Creates one WAV file per active track.
    // Heavy work (file creation, writer init) is done on a background thread.
    bool startRecording(const juce::File& outputFolder,
                        double sampleRate,
                        int numLocalCh,
                        const std::vector<int>& remoteUserIds,
                        const std::vector<int>& remoteUserChannelCounts);

    // Stops recording and finalises all WAV files. Returns true on success.
    bool stopRecording();

    bool isRecording() const { return recording.load(std::memory_order_acquire); }
    bool isStarting() const { return starting.load(std::memory_order_acquire); }
    bool isFinishing() const { return finishing.load(std::memory_order_relaxed); }
    float getFlushProgress() const;

    // --- Audio-thread write methods (non-blocking, drop on overflow) ---
    void writeMasterBlock(const float* left, const float* right, int numSamples);
    void writeLocalChannel(int channel, const float* data, int numSamples);
    void writeRemoteUser(int userIndex, const float* left, const float* right, int numSamples);
    void writeRemoteUserInterleaved(int userIndex, const float* interleaved, int numChannels, int numFrames);
    void writeRemoteUserMultichannel(int userIndex, const float* interleaved,
                                     int numChannels, int numFrames);

    juce::File getOutputFile() const { return outputFolder; }
    juce::String getStatusMessage() const;

private:
    struct Track
    {
        int trackId = -1;
        int numChannels = 0;
        juce::String name;

        // Lock-free ring buffer (interleaved float samples)
        std::vector<float> ringBuffer;
        int ringCapacity = 0;
        std::atomic<int> writePos { 0 };
        std::atomic<int> readPos { 0 };

        // WAV writer (owned by writer thread)
        std::unique_ptr<juce::AudioFormatWriter> writer;
        juce::FileOutputStream* fileStream = nullptr; // raw ptr, writer owns it
        juce::AudioBuffer<float> writeBuffer; // temp buffer for writer thread
        bool active = false;
    };

    std::vector<std::unique_ptr<Track>> tracks;
    std::atomic<bool> recording { false };
    std::atomic<bool> starting { false };
    std::atomic<bool> initFailed { false };
    std::atomic<bool> finishing { false };
    std::atomic<float> flushProgress { 0.0f };
    juce::File outputFolder;
    double sampleRate = 48000.0;

    int pendingNumLocalCh = 0;
    std::vector<int> pendingRemoteUserIds;
    std::vector<int> pendingRemoteChannelCounts;

    std::thread writerThread;
    std::atomic<bool> writerShouldExit { false };

    Track* findTrack(int trackId);
    Track* findRemoteUserTrack(int userIndex);

    void pushSamples(Track& track, const float* interleaved, int numFrames);

    void writerThreadFunc();
    void writeTrackBlock(Track& track, int numFrames);
    void cleanupTrack(Track& track);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionRecorder)
};
