#include "SessionRecorder.h"

SessionRecorder::SessionRecorder()
{
}

SessionRecorder::~SessionRecorder()
{
    stopRecording();
}

SessionRecorder::Track* SessionRecorder::findTrack(int trackId)
{
    for (auto& t : tracks)
        if (t->trackId == trackId)
            return t.get();
    return nullptr;
}

SessionRecorder::Track* SessionRecorder::findRemoteUserTrack(int userIndex)
{
    return findTrack(100 + userIndex);
}

bool SessionRecorder::startRecording(const juce::File& folder,
                                     double sr,
                                     int numLocalCh,
                                     const std::vector<int>& remoteUserIds,
                                     const std::vector<int>& remoteUserChannelCounts)
{
    if (recording.load() || starting.load())
        return false;

    if (writerThread.joinable())
    {
        writerShouldExit = true;
        writerThread.join();
        writerShouldExit = false;
    }

    // Clean up any leftover tracks from a previous session
    for (auto& t : tracks)
        cleanupTrack(*t);
    tracks.clear();

    outputFolder = folder;
    sampleRate = sr;
    pendingNumLocalCh = numLocalCh;
    pendingRemoteUserIds = remoteUserIds;
    pendingRemoteChannelCounts = remoteUserChannelCounts;

    starting = true;
    initFailed = false;
    recording = false;
    finishing = false;
    flushProgress = 0.0f;
    writerShouldExit = false;

    writerThread = std::thread(&SessionRecorder::writerThreadFunc, this);

    return true;
}

void SessionRecorder::writerThreadFunc()
{
    try
    {
        const int blockFrames = 1024;
        const double sr = sampleRate;
        const int ringSeconds = 2;

        // Create output subfolder with timestamp
        auto timestamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d_%H-%M-%S");
        auto sessionFolder = outputFolder.getChildFile("NINJAMplus_" + timestamp);
        if (!sessionFolder.createDirectory())
        {
            initFailed = true;
            starting = false;
            return;
        }

        // Build track list
        tracks.clear();

        // Master stereo track (id 0)
        {
            auto t = std::make_unique<Track>();
            t->trackId = 0;
            t->numChannels = 2;
            t->name = "Master";
            t->ringCapacity = static_cast<int>(sr * ringSeconds);
            t->ringBuffer.resize((size_t)t->ringCapacity * 2, 0.0f);
            t->writePos = 0;
            t->readPos = 0;
            tracks.push_back(std::move(t));
        }

        // Local channel tracks (ids 1..8)
        for (int i = 0; i < pendingNumLocalCh && i < maxLocalChannels; ++i)
        {
            auto t = std::make_unique<Track>();
            t->trackId = i + 1;
            t->numChannels = 1;
            t->name = "Local_" + juce::String(i + 1);
            t->ringCapacity = static_cast<int>(sr * ringSeconds);
            t->ringBuffer.resize((size_t)t->ringCapacity, 0.0f);
            t->writePos = 0;
            t->readPos = 0;
            tracks.push_back(std::move(t));
        }

        // Remote user tracks (ids 100+)
        for (size_t i = 0; i < pendingRemoteUserIds.size() && i < pendingRemoteChannelCounts.size(); ++i)
        {
            int userId = pendingRemoteUserIds[i];
            if (userId < 0 || userId >= maxRemoteUsers)
                continue;
            int nch = juce::jlimit(1, 16, pendingRemoteChannelCounts[i]);

            auto t = std::make_unique<Track>();
            t->trackId = 100 + userId;
            t->numChannels = nch;
            t->name = "User_" + juce::String(userId);
            t->ringCapacity = static_cast<int>(sr * ringSeconds);
            t->ringBuffer.resize((size_t)t->ringCapacity * (size_t)nch, 0.0f);
            t->writePos = 0;
            t->readPos = 0;
            tracks.push_back(std::move(t));
        }

        // Create WAV writers for all tracks
        juce::WavAudioFormat wavFormat;
        for (auto& t : tracks)
        {
            auto file = sessionFolder.getChildFile(t->name + ".wav");
            auto* fos = new juce::FileOutputStream(file);
            if (fos->failedToOpen())
            {
                delete fos;
                initFailed = true;
                for (auto& tc : tracks)
                    cleanupTrack(*tc);
                tracks.clear();
                starting = false;
                return;
            }
            t->fileStream = fos;
            t->writer.reset(wavFormat.createWriterFor(fos, sr, t->numChannels, 16, {}, 0));
            if (t->writer == nullptr)
            {
                initFailed = true;
                for (auto& tc : tracks)
                    cleanupTrack(*tc);
                tracks.clear();
                starting = false;
                return;
            }
            t->writeBuffer.setSize(t->numChannels, blockFrames);
        }

        // Initialization complete
        starting = false;
        recording.store(true, std::memory_order_release);

        // --- Write loop ---
        while (!writerShouldExit.load(std::memory_order_relaxed))
        {
            bool anyData = false;
            for (auto& t : tracks)
            {
                int rp = t->readPos.load(std::memory_order_relaxed);
                int wp = t->writePos.load(std::memory_order_acquire);
                int available = (wp - rp + t->ringCapacity) % t->ringCapacity;
                if (available > 0)
                {
                    writeTrackBlock(*t, juce::jmin(blockFrames, available));
                    anyData = true;
                }
            }

            if (!anyData)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        // Flush phase
        finishing = true;

        int totalTracks = (int)tracks.size();
        int flushedTracks = 0;

        for (auto& t : tracks)
        {
            // Drain remaining samples
            while (true)
            {
                int rp = t->readPos.load(std::memory_order_relaxed);
                int wp = t->writePos.load(std::memory_order_acquire);
                int available = (wp - rp + t->ringCapacity) % t->ringCapacity;
                if (available <= 0)
                    break;
                writeTrackBlock(*t, juce::jmin(blockFrames, available));
            }

            ++flushedTracks;
            flushProgress = (float)flushedTracks / (float)juce::jmax(1, totalTracks);
        }

        // Stop audio thread from accessing tracks
        recording.store(false, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        // Clean up
        for (auto& t : tracks)
            cleanupTrack(*t);

        finishing = false;
    }
    catch (...)
    {
        recording.store(false, std::memory_order_release);
        starting = false;
        finishing = false;
        initFailed = true;
        for (auto& t : tracks)
            cleanupTrack(*t);
    }
}

void SessionRecorder::writeTrackBlock(Track& track, int numFrames)
{
    if (numFrames <= 0 || track.writer == nullptr)
        return;

    const int nch = track.numChannels;
    const int capacity = track.ringCapacity;

    int rp = track.readPos.load(std::memory_order_relaxed);
    int wp = track.writePos.load(std::memory_order_acquire);
    int available = (wp - rp + capacity) % capacity;
    int toRead = juce::jmin(numFrames, available);
    if (toRead <= 0)
        return;

    // Ensure write buffer is big enough
    if (track.writeBuffer.getNumSamples() < toRead || track.writeBuffer.getNumChannels() < nch)
        track.writeBuffer.setSize(nch, toRead);

    // De-interleave from ring buffer into write buffer
    for (int ch = 0; ch < nch; ++ch)
    {
        float* dest = track.writeBuffer.getWritePointer(ch);
        for (int i = 0; i < toRead; ++i)
        {
            int frameBase = (rp + i) % capacity;
            dest[i] = track.ringBuffer[(size_t)frameBase * nch + ch];
        }
    }

    // Write to WAV
    track.writer->writeFromAudioSampleBuffer(track.writeBuffer, 0, toRead);

    // Advance read position
    rp = (rp + toRead) % capacity;
    track.readPos.store(rp, std::memory_order_release);
}

void SessionRecorder::pushSamples(Track& track, const float* interleaved, int numFrames)
{
    if (numFrames <= 0)
        return;

    const int nch = track.numChannels;
    const int capacity = track.ringCapacity;
    if (capacity <= 1)
        return;

    int wp = track.writePos.load(std::memory_order_relaxed);
    int rp = track.readPos.load(std::memory_order_acquire);

    int used = (wp - rp + capacity) % capacity;
    int freeFrames = capacity - 1 - used;

    if (freeFrames <= 0)
    {
        int drop = juce::jmin(numFrames, used);
        rp = (rp + drop) % capacity;
        track.readPos.store(rp, std::memory_order_release);
        used = (wp - rp + capacity) % capacity;
        freeFrames = capacity - 1 - used;
    }

    int toWrite = juce::jmin(numFrames, freeFrames);

    for (int i = 0; i < toWrite; ++i)
    {
        int frameBase = (wp + i) % capacity;
        for (int ch = 0; ch < nch; ++ch)
            track.ringBuffer[(size_t)frameBase * nch + ch] = interleaved[i * nch + ch];
    }

    wp = (wp + toWrite) % capacity;
    track.writePos.store(wp, std::memory_order_release);
    track.active = true;
}

void SessionRecorder::writeMasterBlock(const float* left, const float* right, int numSamples)
{
    if (!recording.load(std::memory_order_acquire))
        return;
    Track* t = findTrack(0);
    if (t == nullptr)
        return;

    float inter[512];
    int processed = 0;
    while (processed < numSamples)
    {
        int chunk = juce::jmin(256, numSamples - processed);
        for (int i = 0; i < chunk; ++i)
        {
            inter[i * 2] = left[processed + i];
            inter[i * 2 + 1] = right[processed + i];
        }
        pushSamples(*t, inter, chunk);
        processed += chunk;
    }
}

void SessionRecorder::writeLocalChannel(int channel, const float* data, int numSamples)
{
    if (!recording.load(std::memory_order_acquire))
        return;
    if (channel < 0 || channel >= maxLocalChannels)
        return;
    Track* t = findTrack(channel + 1);
    if (t == nullptr)
        return;
    pushSamples(*t, data, numSamples);
}

void SessionRecorder::writeRemoteUser(int userIndex, const float* left, const float* right, int numSamples)
{
    if (!recording.load(std::memory_order_acquire))
        return;
    Track* t = findRemoteUserTrack(userIndex);
    if (t == nullptr || t->numChannels < 2)
        return;

    float inter[512];
    int processed = 0;
    while (processed < numSamples)
    {
        int chunk = juce::jmin(256, numSamples - processed);
        for (int i = 0; i < chunk; ++i)
        {
            inter[i * 2] = left[processed + i];
            inter[i * 2 + 1] = right[processed + i];
        }
        pushSamples(*t, inter, chunk);
        processed += chunk;
    }
}

void SessionRecorder::writeRemoteUserInterleaved(int userIndex, const float* interleaved,
                                                  int numChannels, int numFrames)
{
    if (!recording.load(std::memory_order_acquire))
        return;
    Track* t = findRemoteUserTrack(userIndex);
    if (t == nullptr)
        return;

    if (t->numChannels == 2 && numChannels >= 2)
    {
        pushSamples(*t, interleaved, numFrames);
    }
    else if (t->numChannels == 2 && numChannels == 1)
    {
        float inter[512];
        int processed = 0;
        while (processed < numFrames)
        {
            int chunk = juce::jmin(256, numFrames - processed);
            for (int i = 0; i < chunk; ++i)
            {
                inter[i * 2] = interleaved[processed + i];
                inter[i * 2 + 1] = interleaved[processed + i];
            }
            pushSamples(*t, inter, chunk);
            processed += chunk;
        }
    }
    else
    {
        writeRemoteUserMultichannel(userIndex, interleaved, numChannels, numFrames);
    }
}

void SessionRecorder::writeRemoteUserMultichannel(int userIndex, const float* interleaved,
                                                  int numChannels, int numFrames)
{
    if (!recording.load(std::memory_order_acquire))
        return;
    Track* t = findRemoteUserTrack(userIndex);
    if (t == nullptr)
        return;

    if (t->numChannels == numChannels)
    {
        pushSamples(*t, interleaved, numFrames);
    }
    else if (t->numChannels == 2 && numChannels == 1)
    {
        float inter[512];
        int processed = 0;
        while (processed < numFrames)
        {
            int chunk = juce::jmin(256, numFrames - processed);
            for (int i = 0; i < chunk; ++i)
            {
                inter[i * 2] = interleaved[processed + i];
                inter[i * 2 + 1] = interleaved[processed + i];
            }
            pushSamples(*t, inter, chunk);
            processed += chunk;
        }
    }
    else
    {
        // Downmix or upmix to track channel count
        int outCh = t->numChannels;
        float inter[512];
        int processed = 0;
        while (processed < numFrames)
        {
            int chunk = juce::jmin(512 / outCh, numFrames - processed);
            for (int i = 0; i < chunk; ++i)
            {
                for (int ch = 0; ch < outCh; ++ch)
                {
                    if (ch < numChannels)
                        inter[i * outCh + ch] = interleaved[(processed + i) * numChannels + ch];
                    else
                        inter[i * outCh + ch] = 0.0f;
                }
            }
            pushSamples(*t, inter, chunk);
            processed += chunk;
        }
    }
}

bool SessionRecorder::stopRecording()
{
    if (!recording.load(std::memory_order_acquire) && !starting.load(std::memory_order_acquire))
        return false;

    writerShouldExit = true;
    if (writerThread.joinable())
        writerThread.join();

    return true;
}

void SessionRecorder::cleanupTrack(Track& track)
{
    track.writer.reset();
    // fileStream is owned by the writer, so don't delete it separately
    track.fileStream = nullptr;
}

float SessionRecorder::getFlushProgress() const
{
    return flushProgress.load(std::memory_order_relaxed);
}

juce::String SessionRecorder::getStatusMessage() const
{
    if (initFailed.load(std::memory_order_acquire))
        return "Recording failed!";
    if (starting.load(std::memory_order_acquire))
        return "Starting recording...";
    if (recording.load(std::memory_order_acquire) && !finishing.load(std::memory_order_relaxed))
        return "Recording to " + outputFolder.getFileName();
    if (finishing.load(std::memory_order_relaxed))
        return "Finishing... " + juce::String(juce::roundToInt(flushProgress.load() * 100.0f)) + "%";
    return {};
}
