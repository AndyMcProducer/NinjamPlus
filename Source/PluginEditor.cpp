#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "lice/lice.h"
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
static juce::String normaliseColourPresetName(const juce::String& name);
static juce::Colour colourFromPresetName(const juce::String& preset, const juce::Colour& fallback);

static std::atomic<bool> ninjamAutomaticUpdateCheckStarted { false };

struct NinjamUpdateCheckResult
{
    bool requestOk = false;
    bool updateAvailable = false;
    juce::String latestVersion;
    juce::String releaseUrl;
    juce::String downloadUrl;
    juce::String errorMessage;
};

static juce::String stripNinjamVersionPrefix(juce::String version)
{
    version = version.trim();
    if (version.startsWithIgnoreCase("refs/tags/"))
        version = version.substring(10);
    if (version.startsWithIgnoreCase("v"))
        version = version.substring(1);
    return version.upToFirstOccurrenceOf("-", false, false).trim();
}

static std::array<int, 4> parseNinjamVersionParts(const juce::String& versionText)
{
    std::array<int, 4> parts {};
    juce::StringArray tokens;
    tokens.addTokens(stripNinjamVersionPrefix(versionText), ".", "");
    for (int i = 0; i < juce::jmin(4, tokens.size()); ++i)
        parts[(size_t)i] = juce::jmax(0, tokens[i].getIntValue());
    return parts;
}

static int compareNinjamVersions(const juce::String& currentVersion, const juce::String& latestVersion)
{
    const auto current = parseNinjamVersionParts(currentVersion);
    const auto latest = parseNinjamVersionParts(latestVersion);
    for (size_t i = 0; i < current.size(); ++i)
    {
        if (latest[i] > current[i])
            return 1;
        if (latest[i] < current[i])
            return -1;
    }
    return 0;
}

static juce::String getNinjamUpdateObjectString(juce::DynamicObject* obj, const char* property)
{
    return obj != nullptr ? obj->getProperty(property).toString().trim() : juce::String();
}

static int scoreNinjamReleaseAssetForThisBuild(const juce::String& assetName, bool standaloneBuild)
{
    const juce::String lower = assetName.toLowerCase();
#if JUCE_WINDOWS
    if (!lower.contains("windows"))
        return 0;
    if (standaloneBuild && lower.contains("setup") && lower.endsWith(".exe"))
        return 100;
    if (standaloneBuild && lower.contains("standalone"))
        return 80;
    if (!standaloneBuild && lower.contains("vst3"))
        return 80;
#elif JUCE_MAC
    if (!lower.contains("macos") && !lower.contains("mac"))
        return 0;
    if (standaloneBuild && lower.endsWith(".pkg"))
        return 100;
    if (standaloneBuild && lower.contains("standalone"))
        return 80;
    if (!standaloneBuild && lower.endsWith(".pkg"))
        return 90;
    if (!standaloneBuild && (lower.contains("vst3") || lower.contains("au")))
        return 80;
#elif JUCE_LINUX || JUCE_BSD
    if (!lower.contains("linux"))
        return 0;
    if (standaloneBuild && lower.contains("standalone"))
        return 90;
    if (!standaloneBuild && lower.contains("lv2"))
        return 85;
    if (!standaloneBuild && lower.contains("vst3"))
        return 80;
#else
    juce::ignoreUnused(standaloneBuild);
#endif
    return 0;
}

static juce::String chooseNinjamReleaseDownloadUrl(juce::DynamicObject* root, bool standaloneBuild)
{
    juce::String bestUrl;
    int bestScore = 0;
    if (root == nullptr)
        return bestUrl;

    if (auto* assets = root->getProperty("assets").getArray())
    {
        for (const auto& assetVar : *assets)
        {
            auto* asset = assetVar.getDynamicObject();
            const juce::String name = getNinjamUpdateObjectString(asset, "name");
            const juce::String url = getNinjamUpdateObjectString(asset, "browser_download_url");
            if (name.isEmpty() || url.isEmpty())
                continue;

            const int score = scoreNinjamReleaseAssetForThisBuild(name, standaloneBuild);
            if (score > bestScore)
            {
                bestScore = score;
                bestUrl = url;
            }
        }
    }

    return bestUrl;
}

static NinjamUpdateCheckResult fetchLatestNinjamReleaseInfo(const juce::String& currentVersion,
                                                            bool standaloneBuild)
{
    static constexpr const char* latestApiUrl = "https://api.github.com/repos/AndyMcProducer/NinjamPlus/releases/latest";
    static constexpr const char* latestReleasePageUrl = "https://github.com/AndyMcProducer/NinjamPlus/releases/latest";

    NinjamUpdateCheckResult result;
    result.releaseUrl = latestReleasePageUrl;

    int statusCode = 0;
    auto stream = juce::URL(latestApiUrl).createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(7000)
            .withNumRedirectsToFollow(3)
            .withStatusCode(&statusCode)
            .withExtraHeaders("User-Agent: NINJAMplus Update Checker\r\nAccept: application/vnd.github+json\r\n")
            .withHttpRequestCmd("GET"));

    if (stream == nullptr || (statusCode != 0 && (statusCode < 200 || statusCode >= 300)))
    {
        result.errorMessage = statusCode == 0
            ? "Could not reach GitHub releases."
            : "GitHub returned HTTP " + juce::String(statusCode) + ".";
        return result;
    }

    const juce::String response = stream->readEntireStreamAsString();
    const juce::var parsed = juce::JSON::parse(response);
    auto* root = parsed.getDynamicObject();
    if (root == nullptr)
    {
        result.errorMessage = "GitHub returned an unreadable release response.";
        return result;
    }

    const juce::String tagName = getNinjamUpdateObjectString(root, "tag_name");
    if (tagName.isEmpty())
    {
        result.errorMessage = "GitHub release response did not include a version tag.";
        return result;
    }

    const juce::String htmlUrl = getNinjamUpdateObjectString(root, "html_url");
    if (htmlUrl.isNotEmpty())
        result.releaseUrl = htmlUrl;

    result.requestOk = true;
    result.latestVersion = tagName.startsWithIgnoreCase("v") ? tagName : "v" + tagName;
    result.updateAvailable = compareNinjamVersions(currentVersion, tagName) > 0;
    result.downloadUrl = chooseNinjamReleaseDownloadUrl(root, standaloneBuild);
    return result;
}
#if JUCE_WINDOWS
#include <windows.h>
#include <shellapi.h>
#include <sapi.h>
#include <sphelper.h>
#pragma comment(lib, "sapi.lib")
#pragma comment(lib, "ole32.lib")
#else
#include <dlfcn.h>
#endif
class ChatTtsEngine final : private juce::Thread
{
public:
    ChatTtsEngine()
        : juce::Thread("NINJAMChatTTS")
    {
        startThread(juce::Thread::Priority::background);
    }

    ~ChatTtsEngine() override
    {
        signalThreadShouldExit();
        workAvailable.signal();
        stopThread(1000);
    }

    static void getAvailableVoices(juce::StringArray& voiceIds, juce::StringArray& voiceNames)
    {
        voiceIds.clear();
        voiceNames.clear();
#if JUCE_WINDOWS
        const HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialise = SUCCEEDED(coResult);

        IEnumSpObjectTokens* enumTokens = nullptr;
        if (SUCCEEDED(SpEnumTokens(SPCAT_VOICES, nullptr, nullptr, &enumTokens)) && enumTokens != nullptr)
        {
            ISpObjectToken* token = nullptr;
            ULONG fetched = 0;
            while (enumTokens->Next(1, &token, &fetched) == S_OK && token != nullptr)
            {
                LPWSTR id = nullptr;
                LPWSTR description = nullptr;
                if (SUCCEEDED(token->GetId(&id)) && id != nullptr)
                    voiceIds.add(juce::String(id));
                else
                    voiceIds.add({});

                if (SUCCEEDED(SpGetDescription(token, &description)) && description != nullptr)
                    voiceNames.add(juce::String(description));
                else
                    voiceNames.add("Voice " + juce::String(voiceNames.size() + 1));

                if (id != nullptr)
                    CoTaskMemFree(id);
                if (description != nullptr)
                    CoTaskMemFree(description);
                token->Release();
                token = nullptr;
            }
            enumTokens->Release();
        }

        if (shouldUninitialise)
            CoUninitialize();
#elif JUCE_MAC
        juce::ChildProcess say;
        if (say.start(juce::StringArray { "/usr/bin/say", "-v", "?" },
                      juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        {
            const juce::String output = say.readAllProcessOutput();
            juce::StringArray lines;
            lines.addLines(output);
            for (auto line : lines)
            {
                line = line.trim();
                if (line.isEmpty())
                    continue;

                const int firstSpace = line.indexOfAnyOf(" \t");
                if (firstSpace <= 0)
                    continue;

                const juce::String voice = line.substring(0, firstSpace).trim();
                if (voice.isNotEmpty())
                {
                    voiceIds.add(voice);
                    voiceNames.add(voice);
                }
            }
        }
#elif JUCE_LINUX || JUCE_BSD
        static constexpr const char* voiceTypes[] = {
            "male1", "male2", "male3", "female1", "female2", "female3", "child_male", "child_female"
        };
        for (auto* voiceType : voiceTypes)
        {
            voiceIds.add(voiceType);
            voiceNames.add(juce::String(voiceType).replace("_", " "));
        }
#endif
    }

    static void getAvailableOutputs(juce::StringArray& outputIds, juce::StringArray& outputNames)
    {
        outputIds.clear();
        outputNames.clear();
#if JUCE_WINDOWS
        const HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialise = SUCCEEDED(coResult);

        IEnumSpObjectTokens* enumTokens = nullptr;
        if (SUCCEEDED(SpEnumTokens(SPCAT_AUDIOOUT, nullptr, nullptr, &enumTokens)) && enumTokens != nullptr)
        {
            ISpObjectToken* token = nullptr;
            ULONG fetched = 0;
            while (enumTokens->Next(1, &token, &fetched) == S_OK && token != nullptr)
            {
                LPWSTR id = nullptr;
                LPWSTR description = nullptr;
                if (SUCCEEDED(token->GetId(&id)) && id != nullptr)
                    outputIds.add(juce::String(id));
                else
                    outputIds.add({});

                if (SUCCEEDED(SpGetDescription(token, &description)) && description != nullptr)
                    outputNames.add(juce::String(description));
                else
                    outputNames.add("Output " + juce::String(outputNames.size() + 1));

                if (id != nullptr)
                    CoTaskMemFree(id);
                if (description != nullptr)
                    CoTaskMemFree(description);
                token->Release();
                token = nullptr;
            }
            enumTokens->Release();
        }

        if (shouldUninitialise)
            CoUninitialize();
#endif
    }

    void enqueue(juce::String text, juce::String voiceId, float volume, juce::String outputId)
    {
        text = text.trim();
        if (text.isEmpty())
            return;

        const juce::ScopedLock lock(queueLock);
        while (queue.size() >= 6)
            queue.pop_front();
        queue.push_back({ text, voiceId.trim(), juce::jlimit(0.0f, 1.0f, volume), outputId.trim() });
        workAvailable.signal();
    }

private:
    struct SpeechJob
    {
        juce::String text;
        juce::String voiceId;
        float volume = 1.0f;
        juce::String outputId;
    };

    void run() override
    {
#if JUCE_WINDOWS
        const HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool shouldUninitialise = SUCCEEDED(coResult);

        ISpVoice* voice = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, (void**)&voice)) && voice != nullptr)
        {
            while (!threadShouldExit())
            {
                SpeechJob job;
                {
                    const juce::ScopedLock lock(queueLock);
                    if (!queue.empty())
                    {
                        job = queue.front();
                        queue.pop_front();
                    }
                }

                if (job.text.isEmpty())
                {
                    workAvailable.wait(200);
                    continue;
                }

                voice->SetVolume((USHORT) juce::jlimit(0, 100, (int) std::round(job.volume * 100.0f)));

                ISpObjectToken* token = nullptr;
                if (job.voiceId.isNotEmpty()
                    && SUCCEEDED(SpGetTokenFromId((LPCWSTR) job.voiceId.toWideCharPointer(), &token, FALSE))
                    && token != nullptr)
                {
                    voice->SetVoice(token);
                    token->Release();
                }

                if (job.outputId.isNotEmpty()
                    && SUCCEEDED(SpGetTokenFromId((LPCWSTR) job.outputId.toWideCharPointer(), &token, FALSE))
                    && token != nullptr)
                {
                    voice->SetOutput(token, TRUE);
                    token->Release();
                }
                else
                {
                    voice->SetOutput(nullptr, TRUE);
                }

                voice->Speak((LPCWSTR) job.text.toWideCharPointer(), SPF_ASYNC | SPF_PURGEBEFORESPEAK, nullptr);
                while (!threadShouldExit())
                {
                    SPVOICESTATUS status {};
                    if (FAILED(voice->GetStatus(&status, nullptr)) || status.dwRunningState == SPRS_DONE)
                        break;
                    wait(25);
                }

                if (threadShouldExit())
                    voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            }
            voice->Release();
        }

        if (shouldUninitialise)
            CoUninitialize();
#else
        while (!threadShouldExit())
        {
            SpeechJob job;
            {
                const juce::ScopedLock lock(queueLock);
                if (!queue.empty())
                {
                    job = queue.front();
                    queue.pop_front();
                }
            }

            if (job.text.isEmpty())
            {
                workAvailable.wait(200);
                continue;
            }

            juce::StringArray args;
           #if JUCE_MAC
            args.add("/usr/bin/say");
            if (job.voiceId.isNotEmpty())
            {
                args.add("-v");
                args.add(job.voiceId);
            }
            args.add(job.text);
           #elif JUCE_LINUX || JUCE_BSD
            args.add("spd-say");
            args.add("-i");
            args.add(juce::String(juce::jlimit(-100, 100,
                                               (int) std::round(job.volume * 200.0f - 100.0f))));
            if (job.voiceId.isNotEmpty())
            {
                args.add("-t");
                args.add(job.voiceId);
            }
            args.add(job.text);
           #endif

            if (args.isEmpty())
                continue;

            juce::ChildProcess process;
            if (!process.start(args))
                continue;

            while (!threadShouldExit() && process.isRunning())
                wait(25);

            if (threadShouldExit() && process.isRunning())
                process.kill();
        }
#endif
    }

    juce::CriticalSection queueLock;
    juce::WaitableEvent workAvailable;
    std::deque<SpeechJob> queue;
};

static juce::String makeChatTtsSpeechText(const juce::String& line, const juce::String& sender)
{
    const juce::String cleanSender = sender.trim();
    if (cleanSender.isEmpty() || cleanSender.equalsIgnoreCase("me"))
        return {};

    if (line.contains(" shared a "))
        return {};

    const int colon = line.indexOfChar(':');
    if (colon < 0)
        return {};

    juce::String body = line.substring(colon + 1).trim();
    if (body.isEmpty() || body.startsWithChar('!') || body.startsWithChar('/'))
        return {};

    if (body.startsWithIgnoreCase("http://") || body.startsWithIgnoreCase("https://"))
        return {};

    body = body.replace("\r", " ").replace("\n", " ").trim();
    while (body.contains("  "))
        body = body.replace("  ", " ");
    if (body.length() > 240)
        body = body.substring(0, 240).trim() + "...";

    return cleanSender + " says " + body;
}

static juce::File getThisModuleFile()
{
#if JUCE_WINDOWS
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

static juce::PropertiesFile::Options makeSettingsOptions()
{
    juce::PropertiesFile::Options options;
    options.applicationName = JucePlugin_Name;
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
#if JUCE_WINDOWS
    options.folderName = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                             .getChildFile("NINJAMplus")
                             .getFullPathName();
#elif JUCE_LINUX || JUCE_BSD
    options.folderName = "~/.config";
#else
    options.folderName = JucePlugin_Name;
#endif
    return options;
}

static bool renewSettingsFileIfCorrupt(const juce::PropertiesFile::Options& options,
                                       juce::Component* associatedComponent)
{
    const auto settingsFile = options.getDefaultFile();
    if (!settingsFile.existsAsFile())
        return true;

    juce::PropertiesFile probe(options);
    if (probe.isValidFile())
        return true;

    const juce::Component::SafePointer<juce::Component> safeAssociatedComponent(associatedComponent);
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::WarningIcon,
        "Settings corrupt",
        "NINJAMplus.settings could not be loaded.\n\nPress OK to renew it.",
        "OK",
        associatedComponent,
        juce::ModalCallbackFunction::create([options, settingsFile, safeAssociatedComponent](int)
        {
            bool removed = !settingsFile.existsAsFile();
            if (!removed)
                removed = settingsFile.deleteFile();

            if (!removed)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Settings renew failed",
                    "NINJAMplus.settings could not be deleted. Close NINJAMplus and remove the file manually:\n\n"
                        + settingsFile.getFullPathName(),
                    "OK",
                    safeAssociatedComponent.getComponent());
                return;
            }

            juce::PropertiesFile fresh(options);
            if (!fresh.save())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Settings renew failed",
                    "A fresh NINJAMplus.settings file could not be created:\n\n"
                        + settingsFile.getFullPathName(),
                    "OK",
                    safeAssociatedComponent.getComponent());
            }
        }));

    return false;
}

// Migrates settings from pre-Documents/NINJAMplus locations to the new location on first run.
static void migrateOldSettingsIfNeeded()
{
    auto newOpts = makeSettingsOptions();
    if (newOpts.getDefaultFile().existsAsFile())
        return; // already have a new-location settings file — nothing to migrate

    // Candidate old locations, tried in priority order:
    // 1. Plugin editor location: folderName = JucePlugin_Name, suffix = "settings" (no dot)
    const juce::PropertiesFile::Options candidates[] = {
        [&]() {
            juce::PropertiesFile::Options o;
            o.applicationName     = JucePlugin_Name;
            o.filenameSuffix      = "settings";   // old plugin editor: no leading dot
            o.folderName          = JucePlugin_Name;
            o.osxLibrarySubFolder = "Application Support";
            return o;
        }(),
        [&]() {
            juce::PropertiesFile::Options o;
            o.applicationName     = JucePlugin_Name;
            o.filenameSuffix      = ".settings";  // old standalone: empty folderName
            o.folderName          = "";
            o.osxLibrarySubFolder = "Application Support";
            return o;
        }()
    };

    for (const auto& oldOpts : candidates)
    {
        if (! oldOpts.getDefaultFile().existsAsFile())
            continue;

        juce::PropertiesFile oldProps(oldOpts);
        juce::PropertiesFile newProps(newOpts);

        const auto keys = oldProps.getAllProperties().getAllKeys();
        for (const auto& key : keys)
            newProps.setValue(key, oldProps.getValue(key));

        newProps.saveIfNeeded();
        return; // stop after first successful migration
    }
}

#if JUCE_WINDOWS
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objidl.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

/** Decodes video frames on a background thread using Windows Media Foundation.
    The main thread calls getLatestFrame() — it returns instantly (no blocking). */
struct WinVideoReader : public juce::Thread
{
    static constexpr int64 maxInMemoryVideoBytes = 50 * 1024 * 1024;

    WinVideoReader() : juce::Thread ("BgVideoDecoder") {}

    ~WinVideoReader() override
    {
        signalThreadShouldExit();
        stopThread (200);
        releaseReaderResources();
        if (mfStarted)           { MFShutdown();        mfStarted = false; }
    }

    bool open (const juce::File& file)
    {
        releaseReaderResources();

        if (! mfStarted)
        {
            if (FAILED (MFStartup (MF_VERSION)))
                return false;

            mfStarted = true;
        }

        IMFAttributes* attrs = nullptr;
        if (FAILED (MFCreateAttributes (&attrs, 1)) || attrs == nullptr)
            return false;

        attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        HRESULT hr = openSource (file, attrs);
        attrs->Release();
        if (FAILED (hr) || reader == nullptr) return false;

        if (! configureOutputType())
        {
            releaseReaderResources();
            return false;
        }

        IMFMediaType* outType = nullptr;
        if (SUCCEEDED (reader->GetCurrentMediaType (
                (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, &outType)))
        {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize (outType, MF_MT_FRAME_SIZE, &w, &h);
            frameWidth  = (int) w;
            frameHeight = (int) h;

            UINT32 num = 0, den = 0;
            MFGetAttributeRatio (outType, MF_MT_FRAME_RATE, &num, &den);
            if (num > 0 && den > 0)
                framePeriodMs = juce::jlimit (10, 200, (int) (1000.0 * den / num));

            outType->Release();
        }

        if (frameWidth <= 0 || frameHeight <= 0) return false;

        nextFrameDeadlineMs = juce::Time::getMillisecondCounterHiRes();
        startThread (juce::Thread::Priority::background);
        return true;
    }

    /** Called from the message thread. Returns a new frame image if one is ready,
        or an invalid Image if nothing new has been decoded since last call. */
    juce::Image getLatestFrame()
    {
        juce::ScopedLock sl (frameLock);
        auto f = pendingFrame;   // ref-counted copy — cheap
        pendingFrame = {};
        return f;
    }

    int getFramePeriodMs() const
    {
        return framePeriodMs;
    }

    //==============================================================================
    void run() override
    {
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        nextFrameDeadlineMs = juce::Time::getMillisecondCounterHiRes();

        while (!threadShouldExit())
        {
            if (eof)
            {
                seekToStart();
                nextFrameDeadlineMs = juce::Time::getMillisecondCounterHiRes();
                continue;   // go straight back to decode after looping
            }

            auto img = decodeNextFrame();
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if (img.isValid())
            {
                juce::ScopedLock sl (frameLock);
                pendingFrame = std::move (img);

                if (nextFrameDeadlineMs < nowMs - (double) framePeriodMs)
                    nextFrameDeadlineMs = nowMs;

                nextFrameDeadlineMs += (double) framePeriodMs;
            }
            else if (! eof && nextFrameDeadlineMs < nowMs)
            {
                nextFrameDeadlineMs = nowMs;
            }

            const int waitMs = (int) juce::jmax (0.0, nextFrameDeadlineMs - juce::Time::getMillisecondCounterHiRes());
            if (waitMs > 0)
                wait (waitMs);
            else
                juce::Thread::yield();
        }

        CoUninitialize();
    }

private:
    IMFSourceReader* reader = nullptr;
    IMFByteStream* byteStream = nullptr;
    IStream* backingStream = nullptr;
    bool mfStarted          = false;
    int  frameWidth         = 0;
    int  frameHeight        = 0;
    int  framePeriodMs      = 33;   // ~30 fps default
    bool eof                = false;
    bool needsOpaqueAlphaFill = true;
    double nextFrameDeadlineMs = 0.0;

    juce::CriticalSection frameLock;
    juce::Image           pendingFrame;
    juce::MemoryBlock     cachedVideoData;

    void releaseReaderResources()
    {
        if (reader != nullptr)      { reader->Release();      reader = nullptr; }
        if (byteStream != nullptr)  { byteStream->Release();  byteStream = nullptr; }
        if (backingStream != nullptr) { backingStream->Release(); backingStream = nullptr; }
        cachedVideoData.reset();
        frameWidth = 0;
        frameHeight = 0;
        framePeriodMs = 33;
        eof = false;
        needsOpaqueAlphaFill = true;
        nextFrameDeadlineMs = 0.0;
        {
            juce::ScopedLock sl (frameLock);
            pendingFrame = {};
        }
    }

    HRESULT openSource (const juce::File& file, IMFAttributes* attrs)
    {
        const auto fileSize = file.getSize();
        if (fileSize > 0 && fileSize <= maxInMemoryVideoBytes)
        {
            juce::MemoryBlock fileData;
            if (file.loadFileAsData (fileData) && fileData.getSize() > 0)
            {
                cachedVideoData = std::move (fileData);
                const auto hr = createReaderFromMemory (attrs);
                if (SUCCEEDED (hr) && reader != nullptr)
                    return hr;

                releaseReaderResources();
            }
        }

        return MFCreateSourceReaderFromURL (file.getFullPathName().toWideCharPointer(), attrs, &reader);
    }

    HRESULT createReaderFromMemory (IMFAttributes* attrs)
    {
        const auto numBytes = (SIZE_T) cachedVideoData.getSize();
        if (numBytes == 0)
            return E_INVALIDARG;

        HGLOBAL globalHandle = GlobalAlloc (GMEM_MOVEABLE, numBytes);
        if (globalHandle == nullptr)
            return E_OUTOFMEMORY;

        void* dest = GlobalLock (globalHandle);
        if (dest == nullptr)
        {
            GlobalFree (globalHandle);
            return E_FAIL;
        }

        std::memcpy (dest, cachedVideoData.getData(), numBytes);
        GlobalUnlock (globalHandle);

        HRESULT hr = CreateStreamOnHGlobal (globalHandle, TRUE, &backingStream);
        if (FAILED (hr) || backingStream == nullptr)
        {
            GlobalFree (globalHandle);
            return FAILED (hr) ? hr : E_FAIL;
        }

        cachedVideoData.reset();

        hr = MFCreateMFByteStreamOnStreamEx (backingStream, &byteStream);
        if (FAILED (hr) || byteStream == nullptr)
            return FAILED (hr) ? hr : E_FAIL;

        return MFCreateSourceReaderFromByteStream (byteStream, attrs, &reader);
    }

    bool configureOutputType()
    {
        if (trySetOutputType (MFVideoFormat_ARGB32))
        {
            needsOpaqueAlphaFill = false;
            return true;
        }

        if (trySetOutputType (MFVideoFormat_RGB32))
        {
            needsOpaqueAlphaFill = true;
            return true;
        }

        return false;
    }

    bool trySetOutputType (const GUID& subtype)
    {
        IMFMediaType* type = nullptr;
        if (FAILED (MFCreateMediaType (&type)) || type == nullptr)
            return false;

        const HRESULT majorTypeResult = type->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
        const HRESULT subTypeResult = type->SetGUID (MF_MT_SUBTYPE, subtype);
        HRESULT setTypeResult = E_FAIL;
        if (SUCCEEDED (majorTypeResult) && SUCCEEDED (subTypeResult))
            setTypeResult = reader->SetCurrentMediaType ((DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);

        type->Release();
        return SUCCEEDED (setTypeResult);
    }

    void seekToStart()
    {
        if (reader == nullptr) return;
        PROPVARIANT pv;
        PropVariantInit (&pv);
        pv.vt            = VT_I8;
        pv.hVal.QuadPart = 0;
        reader->SetCurrentPosition (GUID_NULL, pv);
        PropVariantClear (&pv);
        eof = false;
    }

    juce::Image decodeNextFrame()
    {
        if (reader == nullptr) return {};

        DWORD    streamIdx = 0, flags = 0;
        LONGLONG ts        = 0;
        IMFSample* sample  = nullptr;

        HRESULT hr = reader->ReadSample (
            (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, &streamIdx, &flags, &ts, &sample);

        if (FAILED (hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM))
        {
            if (sample != nullptr) sample->Release();
            eof = true;
            return {};
        }
        if (sample == nullptr) return {};

        IMFMediaBuffer* buf = nullptr;
        sample->ConvertToContiguousBuffer (&buf);
        sample->Release();
        if (buf == nullptr) return {};

        BYTE* data   = nullptr;
        DWORD maxLen = 0, curLen = 0;
        buf->Lock (&data, &maxLen, &curLen);

        juce::Image img (juce::Image::ARGB, frameWidth, frameHeight, false);
        {
            juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
            const size_t srcRowBytes = (size_t) frameWidth * 4;
            for (int y = 0; y < frameHeight; ++y)
            {
                auto* dstLine = bd.getLinePointer (y);
                auto* srcLine = data + y * srcRowBytes;

                if (! needsOpaqueAlphaFill)
                {
                    std::memcpy (dstLine, srcLine, srcRowBytes);
                    continue;
                }

                auto* src = reinterpret_cast<const uint32_t*> (srcLine);
                auto* dst = reinterpret_cast<uint32_t*> (dstLine);
                for (int x = 0; x < frameWidth; ++x)
                    dst[x] = src[x] | 0xFF000000u;
            }
        }

        buf->Unlock();
        buf->Release();
        return img;
    }
};
#endif  // JUCE_WINDOWS

namespace
{
class NoArrowCallOutLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawCallOutBoxBackground(juce::CallOutBox& box, juce::Graphics& g, const juce::Path&, juce::Image&) override
    {
        auto bounds = box.getLocalBounds().toFloat().reduced(1.0f);
        auto background = box.findColour(juce::ResizableWindow::backgroundColourId).withAlpha(0.97f);
        g.setColour(background);
        g.fillRoundedRectangle(bounds, 10.0f);
        g.setColour(juce::Colours::lightblue.withAlpha(0.5f));
        g.drawRoundedRectangle(bounds, 10.0f, 1.0f);
    }
};

static NoArrowCallOutLookAndFeel noArrowCallOutLookAndFeel;

class ReverbSettingsPopupComponent : public juce::Component
{
public:
    explicit ReverbSettingsPopupComponent(NinjamVst3AudioProcessor& p)
        : processor(p)
    {
        addAndMakeVisible(roomSizeLabel);
        roomSizeLabel.setText("Room Size", juce::dontSendNotification);
        addAndMakeVisible(roomSizeSlider);
        roomSizeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        roomSizeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        roomSizeSlider.setRange(0.0, 1.0, 0.01);
        roomSizeSlider.setValue(processor.getFxReverbRoomSize(), juce::dontSendNotification);
        roomSizeSlider.onValueChange = [this] { processor.setFxReverbRoomSize((float)roomSizeSlider.getValue()); };

        addAndMakeVisible(dampingLabel);
        dampingLabel.setText("Dampening", juce::dontSendNotification);
        addAndMakeVisible(dampingSlider);
        dampingSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        dampingSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        dampingSlider.setRange(0.0, 1.0, 0.01);
        dampingSlider.setValue(processor.getFxReverbDamping(), juce::dontSendNotification);
        dampingSlider.onValueChange = [this] { processor.setFxReverbDamping((float)dampingSlider.getValue()); };

        addAndMakeVisible(wetDryLabel);
        wetDryLabel.setText("Wet/Dry", juce::dontSendNotification);
        addAndMakeVisible(wetDrySlider);
        wetDrySlider.setSliderStyle(juce::Slider::LinearHorizontal);
        wetDrySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        wetDrySlider.setRange(0.0, 1.0, 0.01);
        wetDrySlider.setValue(processor.getFxReverbWetDryMix(), juce::dontSendNotification);
        wetDrySlider.onValueChange = [this] { processor.setFxReverbWetDryMix((float)wetDrySlider.getValue()); };

        addAndMakeVisible(earlyLabel);
        earlyLabel.setText("Early Reflections", juce::dontSendNotification);
        addAndMakeVisible(earlySlider);
        earlySlider.setSliderStyle(juce::Slider::LinearHorizontal);
        earlySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        earlySlider.setRange(0.0, 1.0, 0.01);
        earlySlider.setValue(processor.getFxReverbEarlyReflections(), juce::dontSendNotification);
        earlySlider.onValueChange = [this] { processor.setFxReverbEarlyReflections((float)earlySlider.getValue()); };

        addAndMakeVisible(tailLabel);
        tailLabel.setText("Tail", juce::dontSendNotification);
        addAndMakeVisible(tailSlider);
        tailSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        tailSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        tailSlider.setRange(0.0, 1.0, 0.01);
        tailSlider.setValue(processor.getFxReverbTail(), juce::dontSendNotification);
        tailSlider.onValueChange = [this] { processor.setFxReverbTail((float)tailSlider.getValue()); };

        setSize(340, 180);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        layoutRow(area, roomSizeLabel, roomSizeSlider);
        layoutRow(area, earlyLabel, earlySlider);
        layoutRow(area, tailLabel, tailSlider);
        layoutRow(area, dampingLabel, dampingSlider);
        layoutRow(area, wetDryLabel, wetDrySlider);
    }

private:
    void layoutRow(juce::Rectangle<int>& area, juce::Label& label, juce::Slider& slider)
    {
        auto row = area.removeFromTop(32);
        label.setBounds(row.removeFromLeft(130));
        slider.setBounds(row);
    }

    NinjamVst3AudioProcessor& processor;
    juce::Label roomSizeLabel;
    juce::Label dampingLabel;
    juce::Label wetDryLabel;
    juce::Label earlyLabel;
    juce::Label tailLabel;
    juce::Slider roomSizeSlider;
    juce::Slider dampingSlider;
    juce::Slider wetDrySlider;
    juce::Slider earlySlider;
    juce::Slider tailSlider;
};

class DelaySettingsPopupComponent : public juce::Component
{
public:
    explicit DelaySettingsPopupComponent(NinjamVst3AudioProcessor& p)
        : processor(p)
    {
        addAndMakeVisible(typeLabel);
        typeLabel.setText("Delay Type", juce::dontSendNotification);
        addAndMakeVisible(typeSelector);
        typeSelector.addItem("Standard", 1);
        typeSelector.addItem("Frippertronics", 2);
        typeSelector.setSelectedId(processor.getFxDelayMode() == NinjamVst3AudioProcessor::FxDelayMode::frippertronics ? 2 : 1,
                                   juce::dontSendNotification);
        typeSelector.onChange = [this]
        {
            processor.setFxDelayMode(typeSelector.getSelectedId() == 2
                ? NinjamVst3AudioProcessor::FxDelayMode::frippertronics
                : NinjamVst3AudioProcessor::FxDelayMode::standard);
        };

        addAndMakeVisible(modeLabel);
        modeLabel.setText("Time Mode", juce::dontSendNotification);
        addAndMakeVisible(modeSelector);
        modeSelector.addItem("Milliseconds", 1);
        modeSelector.addItem("Sync Host", 2);
        modeSelector.setSelectedId(processor.isFxDelaySyncToHost() ? 2 : 1, juce::dontSendNotification);
        modeSelector.onChange = [this]
        {
            processor.setFxDelaySyncToHost(modeSelector.getSelectedId() == 2);
            updateEnabledState();
        };

        addAndMakeVisible(timeMsLabel);
        timeMsLabel.setText("Delay Time (ms)", juce::dontSendNotification);
        addAndMakeVisible(timeMsSlider);
        timeMsSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        timeMsSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        timeMsSlider.setRange(20.0, 10000.0, 1.0);
        timeMsSlider.setValue(processor.getFxDelayTimeMs(), juce::dontSendNotification);
        timeMsSlider.onValueChange = [this] { processor.setFxDelayTimeMs((float)timeMsSlider.getValue()); };

        addAndMakeVisible(syncLabel);
        syncLabel.setText("Sync Division", juce::dontSendNotification);
        addAndMakeVisible(syncSelector);
        syncSelector.addItem("1/16", 16);
        syncSelector.addItem("1/8", 8);
        syncSelector.addItem("1/1", 1);
        syncSelector.setSelectedId(processor.getFxDelayDivision(), juce::dontSendNotification);
        syncSelector.onChange = [this]
        {
            int division = syncSelector.getSelectedId();
            if (division <= 0)
                division = 8;
            processor.setFxDelayDivision(division);
        };

        addAndMakeVisible(pingPongLabel);
        pingPongLabel.setText("Ping Pong", juce::dontSendNotification);
        addAndMakeVisible(pingPongToggle);
        pingPongToggle.setClickingTogglesState(true);
        pingPongToggle.setToggleState(processor.isFxDelayPingPong(), juce::dontSendNotification);
        pingPongToggle.onClick = [this] { processor.setFxDelayPingPong(pingPongToggle.getToggleState()); };

        addAndMakeVisible(wetDryLabel);
        wetDryLabel.setText("Wet/Dry", juce::dontSendNotification);
        addAndMakeVisible(wetDrySlider);
        wetDrySlider.setSliderStyle(juce::Slider::LinearHorizontal);
        wetDrySlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        wetDrySlider.setRange(0.0, 1.0, 0.01);
        wetDrySlider.setValue(processor.getFxDelayWetDryMix(), juce::dontSendNotification);
        wetDrySlider.onValueChange = [this] { processor.setFxDelayWetDryMix((float)wetDrySlider.getValue()); };

        addAndMakeVisible(feedbackLabel);
        feedbackLabel.setText("Feedback", juce::dontSendNotification);
        addAndMakeVisible(feedbackSlider);
        feedbackSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        feedbackSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        feedbackSlider.setRange(0.0, 0.95, 0.01);
        feedbackSlider.setValue(processor.getFxDelayFeedback(), juce::dontSendNotification);
        feedbackSlider.onValueChange = [this] { processor.setFxDelayFeedback((float)feedbackSlider.getValue()); };

        updateEnabledState();
        setSize(360, 254);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        layoutRow(area, typeLabel, typeSelector);
        layoutRow(area, modeLabel, modeSelector);
        layoutRow(area, timeMsLabel, timeMsSlider);
        layoutRow(area, syncLabel, syncSelector);
        layoutRow(area, pingPongLabel, pingPongToggle);
        layoutRow(area, wetDryLabel, wetDrySlider);
        layoutRow(area, feedbackLabel, feedbackSlider);
    }

private:
    template <typename ControlType>
    void layoutRow(juce::Rectangle<int>& area, juce::Label& label, ControlType& control)
    {
        auto row = area.removeFromTop(34);
        label.setBounds(row.removeFromLeft(130));
        control.setBounds(row);
    }

    void updateEnabledState()
    {
        const bool syncEnabled = modeSelector.getSelectedId() == 2;
        timeMsSlider.setEnabled(!syncEnabled);
        syncSelector.setEnabled(syncEnabled);
    }

    NinjamVst3AudioProcessor& processor;
    juce::Label typeLabel;
    juce::ComboBox typeSelector;
    juce::Label modeLabel;
    juce::ComboBox modeSelector;
    juce::Label timeMsLabel;
    juce::Slider timeMsSlider;
    juce::Label syncLabel;
    juce::ComboBox syncSelector;
    juce::Label pingPongLabel;
    juce::ToggleButton pingPongToggle;
    juce::Label wetDryLabel;
    juce::Slider wetDrySlider;
    juce::Label feedbackLabel;
    juce::Slider feedbackSlider;
};

class MidiOptionsPopupComponent : public juce::Component
{
public:
    MidiOptionsPopupComponent(NinjamVst3AudioProcessor& p, std::function<void()> onChangedCallback)
        : processor(p), onChanged(std::move(onChangedCallback))
    {
        addAndMakeVisible(learnDeviceLabel);
        learnDeviceLabel.setText("Midi Learn Device", juce::dontSendNotification);
        addAndMakeVisible(learnDeviceSelector);
        populateLearnSelector(learnDeviceSelector, learnDeviceByMenuId, processor, processor.getMidiLearnInputDeviceId());
        learnDeviceSelector.onChange = [this]
        {
            const int selected = learnDeviceSelector.getSelectedId();
            auto it = learnDeviceByMenuId.find(selected);
            processor.setMidiLearnInputDeviceId(it != learnDeviceByMenuId.end() ? it->second : juce::String());
            if (onChanged)
                onChanged();
        };

        addAndMakeVisible(relayDeviceLabel);
        relayDeviceLabel.setText("Midi Relay Device", juce::dontSendNotification);
        addAndMakeVisible(relayDeviceSelector);
        populateSelector(relayDeviceSelector, relayDeviceByMenuId, processor.getMidiRelayInputDeviceId());
        relayDeviceSelector.onChange = [this]
        {
            const int selected = relayDeviceSelector.getSelectedId();
            auto it = relayDeviceByMenuId.find(selected);
            processor.setMidiRelayInputDeviceId(it != relayDeviceByMenuId.end() ? it->second : juce::String());
            if (onChanged)
                onChanged();
        };

        addAndMakeVisible(padsDeviceLabel);
        padsDeviceLabel.setText("Pads MIDI Device", juce::dontSendNotification);
        addAndMakeVisible(padsDeviceSelector);
        populatePadsSelector(padsDeviceSelector, padsDeviceByMenuId, processor.getSamplePadsMidiInputDeviceId());
        padsDeviceSelector.onChange = [this]
        {
            const int selected = padsDeviceSelector.getSelectedId();
            auto it = padsDeviceByMenuId.find(selected);
            processor.setSamplePadsMidiInputDeviceId(it != padsDeviceByMenuId.end() ? it->second : juce::String());
            if (onChanged)
                onChanged();
        };

        addAndMakeVisible(looperInputLabel);
        looperInputLabel.setText("Looper Input", juce::dontSendNotification);
        addAndMakeVisible(looperInputSelector);
        populateLooperInputSelector(looperInputSelector, looperInputByMenuId, processor);
        looperInputSelector.onChange = [this]
        {
            const int selected = looperInputSelector.getSelectedId();
            auto it = looperInputByMenuId.find(selected);
            processor.setSamplePadLooperInput(it != looperInputByMenuId.end()
                ? it->second
                : NinjamVst3AudioProcessor::looperInputLocalChannel);
        };

        setSize(360, 188);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        layoutRow(area, learnDeviceLabel, learnDeviceSelector);
        layoutRow(area, relayDeviceLabel, relayDeviceSelector);
        layoutRow(area, padsDeviceLabel, padsDeviceSelector);
        layoutRow(area, looperInputLabel, looperInputSelector);
    }

private:
    template <typename ControlType>
    void layoutRow(juce::Rectangle<int>& area, juce::Label& label, ControlType& control)
    {
        auto row = area.removeFromTop(42);
        label.setBounds(row.removeFromLeft(140));
        control.setBounds(row.removeFromTop(28));
    }

    static void populateSelector(juce::ComboBox& selector,
                                 std::map<int, juce::String>& idByMenuId,
                                 const juce::String& selectedDeviceId)
    {
        selector.clear(juce::dontSendNotification);
        idByMenuId.clear();
        int menuId = 1;
        selector.addItem("Host MIDI / Any", menuId);
        idByMenuId[menuId] = {};
        int selectedMenuId = selectedDeviceId.isEmpty() ? menuId : 0;
        ++menuId;

        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& device : devices)
        {
            selector.addItem(device.name, menuId);
            idByMenuId[menuId] = device.identifier;
            if (device.identifier == selectedDeviceId)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        selector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    static void populatePadsSelector(juce::ComboBox& selector,
                                     std::map<int, juce::String>& idByMenuId,
                                     const juce::String& selectedDeviceId)
    {
        selector.clear(juce::dontSendNotification);
        idByMenuId.clear();
        int menuId = 1;
        selector.addItem("Host MIDI / Any", menuId);
        idByMenuId[menuId] = {};
        int selectedMenuId = selectedDeviceId.isEmpty() ? menuId : 0;
        ++menuId;

        selector.addItem("Remote MIDI Relay", menuId);
        idByMenuId[menuId] = NinjamVst3AudioProcessor::samplePadsMidiInputRelayId;
        if (selectedDeviceId == NinjamVst3AudioProcessor::samplePadsMidiInputRelayId)
            selectedMenuId = menuId;
        ++menuId;

        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& device : devices)
        {
            selector.addItem(device.name, menuId);
            idByMenuId[menuId] = device.identifier;
            if (device.identifier == selectedDeviceId)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        selector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    static void populateLearnSelector(juce::ComboBox& selector,
                                      std::map<int, juce::String>& idByMenuId,
                                      NinjamVst3AudioProcessor& processor,
                                      const juce::String& selectedDeviceId)
    {
        selector.clear(juce::dontSendNotification);
        idByMenuId.clear();
        int menuId = 1;
        selector.addItem("Host MIDI / Any", menuId);
        idByMenuId[menuId] = {};
        int selectedMenuId = selectedDeviceId.isEmpty() ? menuId : 0;
        ++menuId;

        selector.addItem("Relayed MIDI/OSC (Any user)", menuId);
        idByMenuId[menuId] = "__learn_relay__:*";
        if (selectedDeviceId == "__learn_relay__" || selectedDeviceId == "__learn_relay__:*")
            selectedMenuId = menuId;
        ++menuId;

        {
            std::set<juce::String> seen;
            for (const auto& user : processor.getConnectedUsers())
            {
                if (user.name.isEmpty() || seen.find(user.name) != seen.end())
                    continue;
                seen.insert(user.name);
                selector.addItem("Relayed MIDI/OSC (" + user.name + ")", menuId);
                idByMenuId[menuId] = "__learn_relay__:" + user.name;
                if (selectedDeviceId.startsWith("__learn_relay__:"))
                {
                    const juce::String desired = selectedDeviceId.fromFirstOccurrenceOf("__learn_relay__:", false, false).trim();
                    if (desired.equalsIgnoreCase(user.name))
                        selectedMenuId = menuId;
                }
                ++menuId;
            }
        }

        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& device : devices)
        {
            selector.addItem(device.name, menuId);
            idByMenuId[menuId] = device.identifier;
            if (device.identifier == selectedDeviceId)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        selector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    static void populateLooperInputSelector(juce::ComboBox& selector,
                                            std::map<int, int>& inputByMenuId,
                                            NinjamVst3AudioProcessor& processor)
    {
        selector.clear(juce::dontSendNotification);
        inputByMenuId.clear();

        int menuId = 1;
        const int selectedInput = processor.getSamplePadLooperInput();
        selector.addItem("Local Channel 1", menuId);
        inputByMenuId[menuId] = NinjamVst3AudioProcessor::looperInputLocalChannel;
        int selectedMenuId = selectedInput == NinjamVst3AudioProcessor::looperInputLocalChannel ? menuId : 0;
        ++menuId;

        selector.addItem("Sample Pads", menuId);
        inputByMenuId[menuId] = NinjamVst3AudioProcessor::looperInputSamplePads;
        if (selectedInput == NinjamVst3AudioProcessor::looperInputSamplePads)
            selectedMenuId = menuId;
        ++menuId;

        for (const auto& user : processor.getConnectedUsers())
        {
            if (user.name.isEmpty())
                continue;

            const int inputValue = NinjamVst3AudioProcessor::looperInputForRemoteUser(user.index);
            selector.addItem("User " + user.name, menuId);
            inputByMenuId[menuId] = inputValue;
            if (selectedInput == inputValue)
                selectedMenuId = menuId;
            ++menuId;
        }

        int totalInputs = processor.getTotalNumInputChannels();
        if (totalInputs <= 0)
            totalInputs = 2;
        for (int ch = 0; ch < totalInputs; ++ch)
        {
            selector.addItem("In " + juce::String(ch + 1), menuId);
            inputByMenuId[menuId] = ch;
            if (selectedInput == ch)
                selectedMenuId = menuId;
            ++menuId;
        }

        const int numPairs = totalInputs / 2;
        for (int pair = 0; pair < numPairs; ++pair)
        {
            const int left = pair * 2 + 1;
            const int right = left + 1;
            const int inputValue = -1 - pair;
            selector.addItem(juce::String(left) + "/" + juce::String(right), menuId);
            inputByMenuId[menuId] = inputValue;
            if (selectedInput == inputValue)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        selector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    NinjamVst3AudioProcessor& processor;
    std::function<void()> onChanged;
    juce::Label learnDeviceLabel;
    juce::ComboBox learnDeviceSelector;
    juce::Label relayDeviceLabel;
    juce::ComboBox relayDeviceSelector;
    juce::Label padsDeviceLabel;
    juce::ComboBox padsDeviceSelector;
    juce::Label looperInputLabel;
    juce::ComboBox looperInputSelector;
    std::map<int, juce::String> learnDeviceByMenuId;
    std::map<int, juce::String> relayDeviceByMenuId;
    std::map<int, juce::String> padsDeviceByMenuId;
    std::map<int, int> looperInputByMenuId;
};

class LinkAudioOptionsPopupComponent : public juce::Component
{
public:
    LinkAudioOptionsPopupComponent(NinjamVst3AudioProcessor& p)
        : processor(p)
    {
        addAndMakeVisible(enableToggle);
        enableToggle.setButtonText("Enable Link Audio");
        enableToggle.setToggleState(processor.isLinkAudioEnabled(), juce::dontSendNotification);
        enableToggle.onClick = [this]
        {
            processor.setLinkAudioEnabled(enableToggle.getToggleState());
            refreshChannelSelector();
            refreshStatusText();
            updateControlEnablement();
        };

        addAndMakeVisible(sendToggle);
        sendToggle.setButtonText("Publish Main Mix");
        sendToggle.setToggleState(processor.isLinkAudioSendEnabled(), juce::dontSendNotification);
        sendToggle.onClick = [this]
        {
            processor.setLinkAudioSendEnabled(sendToggle.getToggleState());
            refreshStatusText();
        };

        addAndMakeVisible(receiveToggle);
        receiveToggle.setButtonText("Receive Remote Audio");
        receiveToggle.setToggleState(processor.isLinkAudioReceiveEnabled(), juce::dontSendNotification);
        receiveToggle.onClick = [this]
        {
            processor.setLinkAudioReceiveEnabled(receiveToggle.getToggleState());
            updateControlEnablement();
        };

        addAndMakeVisible(receiveChannelLabel);
        receiveChannelLabel.setText("Receive Channel", juce::dontSendNotification);

        addAndMakeVisible(receiveChannelSelector);
        receiveChannelSelector.onChange = [this]
        {
            const int selectedId = receiveChannelSelector.getSelectedId();
            const auto it = receiveChannelKeyByMenuId.find(selectedId);
            processor.setLinkAudioReceiveSelection(it != receiveChannelKeyByMenuId.end() ? it->second : juce::String());
            refreshStatusText();
        };

        addAndMakeVisible(statusLabel);
        statusLabel.setJustificationType(juce::Justification::centredLeft);

        refreshChannelSelector();
        refreshStatusText();
        updateControlEnablement();
        setSize(420, 156);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        enableToggle.setBounds(area.removeFromTop(26));
        sendToggle.setBounds(area.removeFromTop(26));
        receiveToggle.setBounds(area.removeFromTop(26));

        auto selectorRow = area.removeFromTop(34);
        receiveChannelLabel.setBounds(selectorRow.removeFromLeft(130));
        receiveChannelSelector.setBounds(selectorRow.removeFromTop(24));

        statusLabel.setBounds(area.removeFromTop(28));
    }

private:
    void refreshChannelSelector()
    {
        const juce::String selectedKey = processor.getLinkAudioReceiveSelection();
        receiveChannelSelector.clear(juce::dontSendNotification);
        receiveChannelKeyByMenuId.clear();

        int menuId = 1;
        receiveChannelSelector.addItem("None", menuId);
        receiveChannelKeyByMenuId[menuId] = {};
        int selectedMenuId = selectedKey.isEmpty() ? menuId : 0;
        ++menuId;

        const auto channels = processor.getLinkAudioAvailableChannels();
        for (const auto& channel : channels)
        {
            juce::String label = channel.name;
            if (channel.peerName.isNotEmpty())
                label << " (" << channel.peerName << ")";
            receiveChannelSelector.addItem(label, menuId);
            receiveChannelKeyByMenuId[menuId] = channel.key;
            if (channel.key == selectedKey)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        receiveChannelSelector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    void refreshStatusText()
    {
        juce::String status;
        status << "Peers: " << juce::String(processor.getLinkPeerCount())
               << "   Tempo: " << juce::String(processor.getLinkTempoBpm(), 1)
               << " BPM";
        statusLabel.setText(status, juce::dontSendNotification);
    }

    void updateControlEnablement()
    {
        const bool linkEnabled = enableToggle.getToggleState();
        sendToggle.setEnabled(linkEnabled);
        receiveToggle.setEnabled(linkEnabled);
        receiveChannelLabel.setEnabled(linkEnabled && receiveToggle.getToggleState());
        receiveChannelSelector.setEnabled(linkEnabled && receiveToggle.getToggleState());
    }

    NinjamVst3AudioProcessor& processor;
    juce::ToggleButton enableToggle;
    juce::ToggleButton sendToggle;
    juce::ToggleButton receiveToggle;
    juce::Label receiveChannelLabel;
    juce::ComboBox receiveChannelSelector;
    juce::Label statusLabel;
    std::map<int, juce::String> receiveChannelKeyByMenuId;
};
}

static bool isDbThresholdFader(const juce::Slider& slider)
{
    return slider.getMinimum() < 0.0 && slider.getMaximum() <= 0.0;
}

static juce::String formatDbTickLabel(double db)
{
    const int dbInt = (int)std::round(db);
    if (dbInt > 0)
        return "+" + juce::String(dbInt) + " dB";
    return juce::String(dbInt) + " dB";
}

static int yForFaderProportion(juce::Rectangle<int> track, float proportion)
{
    const float p = juce::jlimit(0.0f, 1.0f, proportion);
    return juce::roundToInt(juce::jmap(p, 0.0f, 1.0f,
                                       (float)track.getBottom(), (float)track.getY()));
}

static int xForFaderProportion(juce::Rectangle<int> track, float proportion)
{
    const float p = juce::jlimit(0.0f, 1.0f, proportion);
    return juce::roundToInt(juce::jmap(p, 0.0f, 1.0f,
                                       (float)track.getX(), (float)track.getRight()));
}

void FaderLookAndFeel::drawLinearSliderBackground(juce::Graphics& g, int x, int y, int width, int height,
                                                  float sliderPos, float minSliderPos, float maxSliderPos,
                                                  const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    juce::Rectangle<int> bounds(x, y, width, height);
    if (style == juce::Slider::LinearVertical)
    {
        int trackWidth = 4;
        juce::Rectangle<int> track(bounds.getCentreX() - trackWidth / 2,
                                   bounds.getY() + 4,
                                   trackWidth,
                                   bounds.getHeight() - 8);

        juce::ColourGradient grad(juce::Colours::black.withAlpha(0.9f), (float)track.getCentreX(), (float)track.getY(),
                                  juce::Colours::darkgrey.darker(), (float)track.getCentreX(), (float)track.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRect(track);

        g.setColour(juce::Colours::black);
        g.drawRect(track);

        int tickX = track.getRight() + 6;

        if (isDbThresholdFader(slider))
        {
            const double minDb = slider.getMinimum();
            const double maxDb = slider.getMaximum();
            const double ticks[] { maxDb, (minDb + maxDb) * 0.5, minDb };

            for (double db : ticks)
            {
                const double value = juce::jlimit(minDb, maxDb, db);
                const float prop = juce::jlimit(0.0f, 1.0f, (float)slider.valueToProportionOfLength(value));
                const int yPos = yForFaderProportion(track, prop);
                const bool major = db == maxDb || db == minDb;
                const int labelY = juce::jlimit(bounds.getY(), bounds.getBottom() - 14, yPos - 7);

                g.setColour(juce::Colours::lightgrey.withAlpha(major ? 0.92f : 0.68f));
                g.drawLine((float)(track.getX() - 6), (float)yPos,
                           (float)(tickX + 4), (float)yPos);

                g.setFont(9.0f);
                g.drawText(formatDbTickLabel(db), tickX + 4, labelY, 40, 14,
                           juce::Justification::centredLeft, false);
            }
            return;
        }

        const int numTicksBelowZero = 3;
        float zeroProp = slider.valueToProportionOfLength(1.0);
        zeroProp = juce::jlimit(0.0f, 1.0f, zeroProp);

        for (int i = 0; i <= numTicksBelowZero + 1; ++i)
        {
            float prop = zeroProp * (float)i / (float)numTicksBelowZero;
            if (i > numTicksBelowZero) prop = 1.0f;
            double value = slider.proportionOfLengthToValue(prop);
            float gain = (float)value;
            float clampedGain = juce::jlimit(1.0e-6f, 2.0f, gain);
            float db = 20.0f * std::log10(clampedGain);

            int yPos = yForFaderProportion(track, prop);

            float alpha = 0.7f;
            if (i == numTicksBelowZero) alpha = 0.95f;

            g.setColour(juce::Colours::lightgrey.withAlpha(alpha));
            g.drawLine((float)(track.getX() - 6), (float)yPos,
                       (float)(tickX + 4), (float)yPos);

            juce::String label;
            if (i == numTicksBelowZero)
                label = "0 dB";
            else if (i > numTicksBelowZero)
                label = "+6 dB";
            else
                label = juce::String((int)std::round(db)) + " dB";

            const int labelY = juce::jlimit(bounds.getY(), bounds.getBottom() - 14, yPos - 7);
            g.setFont(9.0f);
            g.drawText(label, tickX + 4, labelY, 40, 14,
                       juce::Justification::centredLeft, false);
        }
    }
    else if (style == juce::Slider::LinearHorizontal)
    {
        int trackHeight = 4;
        juce::Rectangle<int> track(bounds.getX() + 4,
                                   bounds.getCentreY() - trackHeight / 2,
                                   bounds.getWidth() - 8,
                                   trackHeight);

        juce::ColourGradient grad(juce::Colours::black.withAlpha(0.9f), (float)track.getX(), (float)track.getCentreY(),
                                  juce::Colours::darkgrey.darker(), (float)track.getRight(), (float)track.getCentreY(), false);
        g.setGradientFill(grad);
        g.fillRect(track);

        g.setColour(juce::Colours::black);
        g.drawRect(track);

        if (isDbThresholdFader(slider))
        {
            const double minDb = slider.getMinimum();
            const double maxDb = slider.getMaximum();
            const double ticks[] { minDb, (minDb + maxDb) * 0.5, maxDb };

            for (double db : ticks)
            {
                const double value = juce::jlimit(minDb, maxDb, db);
                const float prop = juce::jlimit(0.0f, 1.0f, (float)slider.valueToProportionOfLength(value));
                const int xPos = xForFaderProportion(track, prop);
                const bool major = db == maxDb || db == minDb;

                g.setColour(juce::Colours::lightgrey.withAlpha(major ? 0.92f : 0.68f));
                g.drawLine((float)xPos, (float)(track.getY() - 6),
                           (float)xPos, (float)(track.getBottom() + 6));

                const int labelX = juce::jlimit(bounds.getX(), bounds.getRight() - 40, xPos - 20);
                g.setFont(10.0f);
                g.drawText(formatDbTickLabel(db),
                           labelX,
                           track.getBottom() + 8,
                           40,
                           14,
                           juce::Justification::centred,
                           false);
            }
            return;
        }

        const int numTicksBelowZero = 3;
        float zeroProp = slider.valueToProportionOfLength(1.0);
        zeroProp = juce::jlimit(0.0f, 1.0f, zeroProp);

        for (int i = 0; i <= numTicksBelowZero + 1; ++i)
        {
            float prop = zeroProp * (float)i / (float)numTicksBelowZero;
            if (i > numTicksBelowZero) prop = 1.0f;
            double value = slider.proportionOfLengthToValue(prop);
            float gain = (float)value;
            float clampedGain = juce::jlimit(1.0e-6f, 2.0f, gain);
            float db = 20.0f * std::log10(clampedGain);

            int xPos = xForFaderProportion(track, prop);

            float alpha = 0.7f;
            if (i == numTicksBelowZero) alpha = 0.95f;

            g.setColour(juce::Colours::lightgrey.withAlpha(alpha));
            g.drawLine((float)xPos, (float)(track.getY() - 6),
                       (float)xPos, (float)(track.getBottom() + 6));

            juce::String label;
            if (i == numTicksBelowZero)
                label = "0 dB";
            else if (i > numTicksBelowZero)
                label = "+6 dB";
            else
                label = juce::String((int)std::round(db)) + " dB";

            const int labelX = juce::jlimit(bounds.getX(), bounds.getRight() - 40, xPos - 20);
            g.setFont(10.0f);
            g.drawText(label,
                       labelX,
                       track.getBottom() + 8,
                       40,
                       14,
                       juce::Justification::centred,
                       false);
        }
    }
    else
    {
        juce::LookAndFeel_V4::drawLinearSliderBackground(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
}

void FaderLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                        float sliderPos, float minSliderPos, float maxSliderPos,
                                        const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    // Walk parent hierarchy to find the editor (sliders may be inside sub-components)
    NinjamVst3AudioProcessorEditor* editor = nullptr;
    for (auto* p = slider.getParentComponent(); p != nullptr && editor == nullptr; p = p->getParentComponent())
        editor = dynamic_cast<NinjamVst3AudioProcessorEditor*>(p);

    if (editor != nullptr && editor->faderKnobImage.isValid())
    {
        drawLinearSliderBackground(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        bool isVert = (style == juce::Slider::LinearVertical);
        float thumbW = isVert ? juce::jmin(22.0f, (float)width * 0.68f) : 40.0f;
        float thumbH = isVert ? 42.0f : (float)height * 0.95f;
        float thumbX = isVert ? (float)x + ((float)width - thumbW) * 0.5f : sliderPos - thumbW * 0.5f;
        float thumbY = isVert ? sliderPos - thumbH * 0.5f                 : (float)y + (float)height * 0.025f;
        // Clamp so thumb never clips outside the slider bounds
        thumbX = juce::jlimit((float)x,              (float)(x + width)  - thumbW, thumbX);
        thumbY = juce::jlimit((float)y,              (float)(y + height) - thumbH, thumbY);
        if (editor->sandSkinOpaqueKnobs)
        {
            // Sand 1 seashells are semi-transparent; draw an opaque shell-toned base behind them.
            g.setColour(juce::Colour::fromRGB(226, 206, 182));
            g.fillRoundedRectangle(thumbX + 1.0f, thumbY + 1.0f, thumbW - 2.0f, thumbH - 2.0f, 5.0f);
        }
        g.setOpacity(1.0f);
        g.drawImageWithin(editor->faderKnobImage, (int)thumbX, (int)thumbY, (int)thumbW, (int)thumbH,
                          juce::RectanglePlacement::centred);
        if (editor->sandSkinOpaqueKnobs)
        {
            g.setOpacity(1.0f);
            g.drawImageWithin(editor->faderKnobImage, (int)thumbX, (int)thumbY, (int)thumbW, (int)thumbH,
                              juce::RectanglePlacement::centred);
        }
        return;
    }

    if (style == juce::Slider::LinearVertical)
    {
        drawLinearSliderBackground(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);

        juce::Rectangle<int> bounds(x, y, width, height);
        int trackWidth = 4;
        juce::Rectangle<int> track(bounds.getCentreX() - trackWidth / 2,
                                   bounds.getY() + 4,
                                   trackWidth,
                                   bounds.getHeight() - 8);

        int thumbHeight = juce::jmin(52, track.getHeight() / 2);
        int thumbWidth = juce::jmin(22, juce::jmax(10, (bounds.getWidth() * 68) / 100));
        int thumbY = juce::jlimit(bounds.getY(), bounds.getBottom() - thumbHeight,
                                  (int)sliderPos - thumbHeight / 2);
        juce::Rectangle<int> thumb(track.getCentreX() - thumbWidth / 2, thumbY, thumbWidth, thumbHeight);

        const juce::Colour base = editor != nullptr ? editor->faderThemeColour : juce::Colour(0xff666666);
        juce::ColourGradient grad(base.brighter(0.5f), (float)thumb.getX(), (float)thumb.getY(),
                                  base.darker(0.4f), (float)thumb.getRight(), (float)thumb.getBottom(), false);
        if (editor != nullptr && normaliseColourPresetName(editor->faderColourPreset).startsWith("multi"))
            grad.addColour(0.4, juce::Colour::fromHSV((float)slider.getValue() * 0.8f, 0.8f, 1.0f, 1.0f));
        else
            grad.addColour(0.4, base.brighter(0.2f));
        g.setGradientFill(grad);
        g.fillRect(thumb);

        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.drawRect(thumb);

        g.setColour(juce::Colours::white.withAlpha(0.4f));
        int innerY = thumb.getY() + 4;
        for (int i = 0; i < 4; ++i)
        {
            g.drawLine((float)(thumb.getX() + 2), (float)innerY, (float)(thumb.getRight() - 2), (float)innerY);
            innerY += 4;
        }

        double v = slider.getValue();
        double db = (v <= 1.0e-6) ? -60.0 : 20.0 * std::log10(v);
        int dbInt = (int)std::round(db);

        juce::Rectangle<int> box(thumb.getX() + 3, thumb.getY() + 6, thumb.getWidth() - 6, 14);
        g.setColour(juce::Colours::black);
        g.fillRect(box);
        g.setColour(juce::Colours::white.withAlpha(0.9f));

        juce::String text;
        if (isDbThresholdFader(slider))
            text = formatDbTickLabel(v);
        else if (v <= 1.0e-6)
            text = "-inf";
        else if (dbInt > 0)
            text = "+" + juce::String(dbInt) + " dB";
        else
            text = juce::String(dbInt) + " dB";

        g.setFont(10.0f);
        g.drawText(text, box, juce::Justification::centred, false);
    }
    else if (style == juce::Slider::LinearHorizontal)
    {
        drawLinearSliderBackground(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);

        juce::Rectangle<int> bounds(x, y, width, height);
        int trackHeight = 4;
        juce::Rectangle<int> track(bounds.getX() + 4,
                                   bounds.getCentreY() - trackHeight / 2,
                                   bounds.getWidth() - 8,
                                   trackHeight);

        int thumbWidth = juce::jmin(52, track.getWidth() / 2);
        int thumbHeight = 24;
        int thumbX = juce::jlimit(bounds.getX(), bounds.getRight() - thumbWidth,
                                  (int)sliderPos - thumbWidth / 2);
        juce::Rectangle<int> thumb(thumbX, track.getCentreY() - thumbHeight / 2, thumbWidth, thumbHeight);

        const juce::Colour base = editor != nullptr ? editor->faderThemeColour : juce::Colour(0xff666666);
        juce::ColourGradient grad(base.brighter(0.5f), (float)thumb.getX(), (float)thumb.getY(),
                                  base.darker(0.4f), (float)thumb.getRight(), (float)thumb.getBottom(), false);
        if (editor != nullptr && normaliseColourPresetName(editor->faderColourPreset).startsWith("multi"))
            grad.addColour(0.4, juce::Colour::fromHSV((float)slider.getValue() * 0.8f, 0.8f, 1.0f, 1.0f));
        else
            grad.addColour(0.4, base.brighter(0.2f));
        g.setGradientFill(grad);
        g.fillRect(thumb);

        g.setColour(juce::Colours::black.withAlpha(0.7f));
        g.drawRect(thumb);

        g.setColour(juce::Colours::white.withAlpha(0.4f));
        int innerX = thumb.getX() + 4;
        for (int i = 0; i < 4; ++i)
        {
            g.drawLine((float)innerX, (float)(thumb.getY() + 2),
                       (float)innerX, (float)(thumb.getBottom() - 2));
            innerX += 4;
        }

        double v = slider.getValue();
        double db = (v <= 1.0e-6) ? -60.0 : 20.0 * std::log10(v);
        int dbInt = (int)std::round(db);

        juce::Rectangle<int> box(thumb.getX(), thumb.getY() - 16, thumb.getWidth(), 14);
        g.setColour(juce::Colours::black);
        g.fillRect(box);
        g.setColour(juce::Colours::white.withAlpha(0.9f));

        juce::String text;
        if (isDbThresholdFader(slider))
            text = formatDbTickLabel(v);
        else if (v <= 1.0e-6)
            text = "-inf";
        else if (dbInt > 0)
            text = "+" + juce::String(dbInt) + " dB";
        else
            text = juce::String(dbInt) + " dB";

        g.setFont(10.0f);
        g.drawText(text, box, juce::Justification::centred, false);
    }
    else
    {
        juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
    }
}

void SamplePadsButtonLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                                       const juce::Colour&,
                                                       bool shouldDrawButtonAsHighlighted,
                                                       bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
    const float corner = 4.0f;
    const bool down = shouldDrawButtonAsDown || button.getToggleState();

    auto top = juce::Colour(0xfff3f4f6);
    auto bottom = down ? juce::Colour(0xff6bd6ff) : juce::Colour(0xffb8bec4);
    if (shouldDrawButtonAsHighlighted && !down)
        top = top.brighter(0.1f);

    juce::ColourGradient shell(top, bounds.getX(), bounds.getY(),
                               bottom, bounds.getX(), bounds.getBottom(), false);
    shell.addColour(0.52, juce::Colour(0xffd7dbe0));
    g.setGradientFill(shell);
    g.fillRoundedRectangle(bounds, corner);

    g.setColour(juce::Colours::black.withAlpha(0.82f));
    g.drawRoundedRectangle(bounds, corner, 1.2f);

    auto inner = button.getLocalBounds().reduced(5, 4).toFloat();
    auto display = inner.removeFromTop(6.0f).withWidth(inner.getWidth() * 0.58f);
    g.setColour(juce::Colours::black.withAlpha(0.88f));
    g.fillRoundedRectangle(display, 1.8f);

    auto iconArea = button.getLocalBounds().reduced(5, 4);
    auto topRight = iconArea.removeFromTop(9).removeFromRight(19);
    auto drawCircleIcon = [&g](juce::Rectangle<int> r, bool reverseIcon)
    {
        auto rf = r.toFloat().reduced(0.5f);
        g.setColour(juce::Colours::black.withAlpha(0.88f));
        g.fillEllipse(rf);
        g.setColour(juce::Colours::white.withAlpha(0.95f));
        if (reverseIcon)
        {
            juce::Path arrow;
            arrow.startNewSubPath(rf.getRight() - 3.0f, rf.getY() + 3.0f);
            arrow.quadraticTo(rf.getX() + 2.0f, rf.getCentreY(), rf.getRight() - 3.0f, rf.getBottom() - 3.0f);
            g.strokePath(arrow, juce::PathStrokeType(1.3f));
            juce::Path head;
            head.addTriangle(rf.getX() + 2.0f, rf.getCentreY(),
                             rf.getX() + 6.0f, rf.getCentreY() - 3.0f,
                             rf.getX() + 6.0f, rf.getCentreY() + 3.0f);
            g.fillPath(head);
        }
        else
        {
            juce::Path loop;
            loop.addCentredArc(rf.getCentreX(), rf.getCentreY(), rf.getWidth() * 0.32f, rf.getHeight() * 0.32f,
                               0.0f, 0.2f, juce::MathConstants<float>::twoPi - 0.7f, true);
            g.strokePath(loop, juce::PathStrokeType(1.2f));
        }
    };
    drawCircleIcon(topRight.removeFromRight(8), false);
    topRight.removeFromRight(3);
    drawCircleIcon(topRight.removeFromRight(8), true);

    auto grid = button.getLocalBounds().reduced(6, 11);
    grid.removeFromTop(7);
    const int cols = 4;
    const int rows = (NinjamVst3AudioProcessor::numSamplePads + cols - 1) / cols;
    const int gap = 2;
    const int cellW = juce::jmax(3, (grid.getWidth() - gap * (cols - 1)) / cols);
    const int cellH = juce::jmax(3, (grid.getHeight() - gap * (rows - 1)) / rows);
    g.setColour(juce::Colours::black.withAlpha(0.86f));
    for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
    {
        const int rowFromBottom = pad / cols;
        const int row = rows - 1 - rowFromBottom;
        const int col = pad % cols;
        if (row < 0)
            continue;

        juce::Rectangle<int> cell(grid.getX() + col * (cellW + gap),
                                  grid.getY() + row * (cellH + gap),
                                  cellW,
                                  cellH);
        g.drawRoundedRectangle(cell.toFloat(), 1.6f, 1.2f);
    }
}

class ServerListComponent : public juce::Component,
                            public juce::ListBoxModel
{
public:
    ServerListComponent(NinjamVst3AudioProcessor& p,
                        std::function<void(const juce::String&)> onChooseServer,
                        std::function<void(const juce::String&)> onConnectServer)
        : processor(p),
          onServerChosen(std::move(onChooseServer)),
          onServerConnect(std::move(onConnectServer))
    {
        addAndMakeVisible(listBox);
        listBox.setModel(this);
        listBox.setRowHeight(34);

        addAndMakeVisible(statusLabel);
        statusLabel.setJustificationType(juce::Justification::centred);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        statusLabel.setText("Loading servers...", juce::dontSendNotification);
        statusLabel.setInterceptsMouseClicks(false, false);

        addAndMakeVisible(refreshButton);
        refreshButton.setButtonText("Refresh");
        refreshButton.onClick = [this] { refreshServers(); };

        addAndMakeVisible(connectButton);
        connectButton.setButtonText("Set Server");
        connectButton.onClick = [this]
        {
            int row = listBox.getSelectedRow();
            chooseServer(row);
        };

        refreshServers();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::darkgrey);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto controls = area.removeFromBottom(30);
        refreshButton.setBounds(controls.removeFromLeft(100));
        controls.removeFromLeft(8);
        connectButton.setBounds(controls.removeFromLeft(120));
        listBox.setBounds(area);
        statusLabel.setBounds(area);
    }

    int getNumRows() override { return (int)servers.size(); }

    void paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (rowNumber < 0 || rowNumber >= (int)servers.size())
            return;

        auto& s = servers[(size_t)rowNumber];

        if (rowIsSelected)
            g.fillAll(juce::Colours::darkblue.withAlpha(0.6f));
        else
            g.fillAll(juce::Colours::darkgrey);

        g.setColour(juce::Colours::white);

        juce::String text;
        text << s.name << "  "
             << s.userCount << "/" << s.userMax
             << "  " << juce::String(s.bpm, 1) << " BPM"
             << " / " << s.bpi << " BPI";

        auto row = juce::Rectangle<int>(4, 0, width - 8, height);
        auto top = row.removeFromTop(s.userNames.isEmpty() ? height : 18);
        g.drawText(text, top, juce::Justification::centredLeft, true);
        if (!s.userNames.isEmpty())
        {
            g.setColour(juce::Colours::lightgrey);
            g.drawText("Users: " + s.userNames.joinIntoString(", "),
                       row,
                       juce::Justification::centredLeft,
                       true);
        }
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { connectServer(row); }

private:
    NinjamVst3AudioProcessor& processor;
    juce::ListBox listBox;
    juce::Label statusLabel;
    juce::TextButton refreshButton;
    juce::TextButton connectButton;
    std::vector<NinjamVst3AudioProcessor::PublicServerInfo> servers;
    std::function<void(const juce::String&)> onServerChosen;
    std::function<void(const juce::String&)> onServerConnect;
    std::atomic<bool> refreshInProgress { false };

    void refreshServers()
    {
        bool expected = false;
        if (!refreshInProgress.compare_exchange_strong(expected, true))
            return;

        refreshButton.setEnabled(false);
        refreshButton.setButtonText("Refreshing...");
        statusLabel.setText("Refreshing servers...", juce::dontSendNotification);
        statusLabel.setVisible(true);

        auto safeThis = juce::Component::SafePointer<ServerListComponent>(this);
        std::thread([safeThis]
        {
            if (safeThis == nullptr)
                return;

            safeThis->processor.refreshPublicServers();
            auto fetched = safeThis->processor.getPublicServers();

            juce::MessageManager::callAsync([safeThis, fetched = std::move(fetched)]() mutable
            {
                if (safeThis == nullptr)
                    return;

                safeThis->servers = std::move(fetched);
                safeThis->listBox.updateContent();
                safeThis->statusLabel.setText(safeThis->servers.empty() ? "No servers found" : "",
                                              juce::dontSendNotification);
                safeThis->statusLabel.setVisible(safeThis->servers.empty());
                safeThis->repaint();
                safeThis->refreshButton.setButtonText("Refresh");
                safeThis->refreshButton.setEnabled(true);
                safeThis->refreshInProgress.store(false);
            });
        }).detach();
    }

    void chooseServer(int row)
    {
        if (row < 0 || row >= (int)servers.size())
            return;
        auto& s = servers[(size_t)row];
        juce::String hostPort = s.host + ":" + juce::String(s.port);
        if (onServerChosen)
            onServerChosen(hostPort);
    }

    void connectServer(int row)
    {
        if (row < 0 || row >= (int)servers.size())
            return;
        auto& s = servers[(size_t)row];
        juce::String hostPort = s.host + ":" + juce::String(s.port);
        if (onServerConnect)
            onServerConnect(hostPort);
    }
};

class ServerListWindow : public juce::DocumentWindow
{
public:
    ServerListWindow(NinjamVst3AudioProcessor& p,
                     std::function<void(const juce::String&)> onChooseServer,
                     std::function<void(const juce::String&)> onConnectServer)
        : DocumentWindow("NINJAM Servers", juce::Colours::black, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setContentOwned(new ServerListComponent(p,
                                                std::move(onChooseServer),
                                                std::move(onConnectServer)), true);
        centreWithSize(600, 400);
        setVisible(true);
    }

    void closeButtonPressed() override { setVisible(false); }
};

// Forward declarations (defined after ChatWindow)
static juce::Colour senderColour(const juce::String& sender);
static void applyColoredChat(RichChatDisplayComponent&, const juce::StringArray&, const juce::StringArray&, const NinjamVst3AudioProcessor&);

namespace
{
struct TranslationLanguageChoice
{
    const char* code;
    const char* label;
};

constexpr TranslationLanguageChoice translationLanguageChoices[] = {
    { "system", "System Default" },
    { "en", "English" },
    { "es", "Spanish" },
    { "fr", "French" },
    { "de", "German" },
    { "it", "Italian" },
    { "pt", "Portuguese" },
    { "nl", "Dutch" },
    { "pl", "Polish" },
    { "ru", "Russian" },
    { "tr", "Turkish" },
    { "ja", "Japanese" },
    { "ko", "Korean" },
    { "zh-cn", "Chinese (Simplified)" }
};

juce::String normaliseTranslateLangCode(const juce::String& code)
{
    juce::String normalised = code.trim().toLowerCase();
    if (normalised.isEmpty())
        normalised = "system";
    return normalised;
}

juce::String getSystemTranslateLanguageCodeForUi()
{
    juce::String language = juce::SystemStats::getDisplayLanguage();
    if (language.isEmpty())
        language = juce::SystemStats::getUserLanguage();

    language = language.trim().replaceCharacter('_', '-').toLowerCase();
    if (language.isEmpty())
        return "en";

    if (language.startsWith("zh-hant") || language.startsWith("zh-tw") || language.startsWith("zh-hk"))
        return "zh-hant";

    if (language.startsWith("zh"))
        return "zh-cn";

    if (language.startsWith("pt-br"))
        return "pt-br";

    if (language.startsWith("no") || language.startsWith("nb"))
        return "nb";

    const int dash = language.indexOfChar('-');
    if (dash > 0)
        language = language.substring(0, dash);

    return language;
}

juce::String getTranslationLanguageLabel(const juce::String& code)
{
    const juce::String normalised = normaliseTranslateLangCode(code);

    if (normalised == "system")
    {
        const juce::String systemCode = getSystemTranslateLanguageCodeForUi();
        return "System Default (" + getTranslationLanguageLabel(systemCode) + ")";
    }

    for (const auto& choice : translationLanguageChoices)
        if (normalised == choice.code)
            return choice.label;

    return normalised.toUpperCase();
}

juce::String buildTranslateTooltip(const juce::String& targetCode)
{
    return "Auto Translate Auto Detect -> " + getTranslationLanguageLabel(targetCode)
         + ". Left-click toggles. Right-click for language setup.";
}

constexpr int kLocalInputLinkSelectorId = 1000;
constexpr int kRemoteOutputLinkSelectorId = 1000;
constexpr float kChatDisplayMinFontHeight = 14.0f;
constexpr float kChatDisplayMaxFontHeight = 26.0f;

struct ChatEmojiChoice
{
    const char* utf8;
    const char* label;
};

constexpr ChatEmojiChoice commonEmojiChoices[] = {
    { "\xF0\x9F\x98\x80", "Smile" },
    { "\xF0\x9F\x98\x82", "Laugh" },
    { "\xF0\x9F\x98\x8A", "Happy" },
    { "\xF0\x9F\x98\x8E", "Cool" },
    { "\xF0\x9F\x91\x8D", "Thumbs Up" },
    { "\xF0\x9F\x91\x8F", "Clap" },
    { "\xF0\x9F\x94\xA5", "Fire" },
    { "\xE2\x9D\xA4\xEF\xB8\x8F", "Heart" },
    { "\xF0\x9F\x8E\xB8", "Guitar" },
    { "\xF0\x9F\xA5\x81", "Drums" },
    { "\xF0\x9F\x8E\xB9", "Keys" },
    { "\xF0\x9F\x8E\xA4", "Mic" },
    { "\xF0\x9F\x8E\xA7", "Headphones" },
    { "\xF0\x9F\x9A\x80", "Rocket" },
    { "\xE2\x9C\x85", "Done" },
    { "\xF0\x9F\x99\x8F", "Thanks" }
};

constexpr ChatEmojiChoice musicSoundEmojiChoices[] = {
    { "\xF0\x9F\x8E\xBC", "Musical Score" },
    { "\xF0\x9F\x8E\xB5", "Musical Note" },
    { "\xF0\x9F\x8E\xB6", "Musical Notes" },
    { "\xF0\x9F\x8E\x99\xEF\xB8\x8F", "Studio Mic" },
    { "\xF0\x9F\x8E\x9A\xEF\xB8\x8F", "Level Slider" },
    { "\xF0\x9F\x8E\x9B\xEF\xB8\x8F", "Control Knobs" },
    { "\xF0\x9F\x8E\xA4", "Microphone" },
    { "\xF0\x9F\x8E\xA7", "Headphones" },
    { "\xF0\x9F\x8E\xB7", "Saxophone" },
    { "\xF0\x9F\xAA\x97", "Accordion" },
    { "\xF0\x9F\x8E\xB8", "Guitar" },
    { "\xF0\x9F\x8E\xB9", "Keyboard" },
    { "\xF0\x9F\x8E\xBA", "Trumpet" },
    { "\xF0\x9F\x8E\xBB", "Violin" },
    { "\xF0\x9F\xAA\x95", "Banjo" },
    { "\xF0\x9F\xA5\x81", "Drum" },
    { "\xF0\x9F\xAA\x98", "Long Drum" },
    { "\xF0\x9F\xAA\x87", "Maracas" },
    { "\xF0\x9F\xAA\x88", "Flute" },
    { "\xF0\x9F\x94\x88", "Speaker Low" },
    { "\xF0\x9F\x94\x8A", "Speaker High" },
    { "\xF0\x9F\x94\x87", "Muted Speaker" },
    { "\xF0\x9F\x94\x94", "Bell" },
    { "\xF0\x9F\x94\x95", "Muted Bell" },
    { "\xF0\x9F\x93\xBB", "Radio" }
};

constexpr ChatEmojiChoice handsEmojiChoices[] = {
    { "\xF0\x9F\x91\x8D", "Thumbs Up" },
    { "\xF0\x9F\x91\x8E", "Thumbs Down" },
    { "\xF0\x9F\x91\x88", "Point Left" },
    { "\xF0\x9F\x91\x89", "Point Right" },
    { "\xF0\x9F\x91\x86", "Point Up" },
    { "\xF0\x9F\x91\x87", "Point Down" },
    { "\xF0\x9F\xAB\xB5", "Point At You" },
    { "\xF0\x9F\x96\x95", "Middle Finger" },
    { "\xE2\x9C\x8C\xEF\xB8\x8F", "Victory Hand" },
    { "\xF0\x9F\xA4\x9E", "Crossed Fingers" },
    { "\xF0\x9F\xA4\x98", "Sign Of The Horns" },
    { "\xF0\x9F\x91\x8C", "OK Hand" },
    { "\xF0\x9F\x91\x8B", "Waving Hand" },
    { "\xF0\x9F\x92\xAA", "Flexed Biceps" },
    { "\xF0\x9F\x91\x8A", "Oncoming Fist" },
    { "\xF0\x9F\xA4\x9B", "Left Fist" },
    { "\xF0\x9F\xA4\x9C", "Right Fist" },
    { "\xF0\x9F\x91\x8F", "Clap" },
    { "\xF0\x9F\x99\x8C", "Raising Hands" },
    { "\xF0\x9F\x99\x8F", "Folded Hands" },
    { "\xF0\x9F\xA4\x9D", "Handshake" },
    { "\xF0\x9F\xA4\x9F", "Love-You Gesture" },
    { "\xF0\x9F\xAB\xB6", "Heart Hands" },
    { "\xF0\x9F\xA4\x8C", "Pinched Fingers" }
};

constexpr ChatEmojiChoice facesBodyEmojiChoices[] = {
    { "\xF0\x9F\x90\xB5", "Monkey Face" },
    { "\xF0\x9F\x91\x80", "Eyes" },
    { "\xF0\x9F\x91\x85", "Tongue" },
    { "\xF0\x9F\xAB\xA6", "Biting Lip" },
    { "\xF0\x9F\x91\x82", "Ear" },
    { "\xF0\x9F\x91\x83", "Nose" },
    { "\xF0\x9F\x92\x8B", "Kiss Mark" },
    { "\xF0\x9F\x98\x98", "Blowing Kiss" },
    { "\xF0\x9F\xA5\xB0", "Smiling Hearts" },
    { "\xF0\x9F\x98\x8D", "Heart Eyes" },
    { "\xF0\x9F\xA4\xA9", "Star-Struck" },
    { "\xF0\x9F\xA5\xB3", "Party Face" },
    { "\xF0\x9F\xAB\xA0", "Melting Face" },
    { "\xF0\x9F\x98\x80", "Grinning Face" },
    { "\xF0\x9F\x98\x82", "Tears Of Joy" },
    { "\xF0\x9F\x98\x8E", "Sunglasses Face" },
    { "\xF0\x9F\xA4\xAF", "Mind Blown" }
};

constexpr ChatEmojiChoice peopleEmojiChoices[] = {
    { "\xF0\x9F\x92\x8F", "Kiss" },
    { "\xF0\x9F\x91\xA8\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x92\x8B\xE2\x80\x8D\xF0\x9F\x91\xA8", "Kiss: Man, Man" },
    { "\xF0\x9F\x92\x91", "Couple With Heart" },
    { "\xF0\x9F\x91\xA8\xE2\x80\x8D\xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x91\xA8", "Couple With Heart: Man, Man" },
    { "\xF0\x9F\x9B\x80", "Person Taking Bath" },
    { "\xF0\x9F\x8E\x85", "Santa Claus" }
};

constexpr ChatEmojiChoice foodPartyEmojiChoices[] = {
    { "\xF0\x9F\x8E\x89", "Party Popper" },
    { "\xF0\x9F\x8E\x82", "Birthday Cake" },
    { "\xF0\x9F\x8D\x95", "Pizza" },
    { "\xF0\x9F\x8E\x81", "Wrapped Gift" },
    { "\xF0\x9F\x8E\x88", "Balloon" },
    { "\xF0\x9F\x8E\x8A", "Confetti Ball" },
    { "\xF0\x9F\x8D\xBE", "Bottle With Popping Cork" },
    { "\xF0\x9F\xA5\x82", "Clinking Glasses" },
    { "\xF0\x9F\x8D\xBB", "Beer Mugs" },
    { "\xF0\x9F\x92\xAF", "Hundred Points" },
    { "\xF0\x9F\x92\xA5", "Collision" }
};

constexpr ChatEmojiChoice sportsObjectsEmojiChoices[] = {
    { "\xF0\x9F\xA5\x8A", "Boxing Glove" },
    { "\xE2\x9A\xBD", "Soccer Ball" },
    { "\xF0\x9F\x8F\x80", "Basketball" },
    { "\xF0\x9F\x8E\xBE", "Tennis" },
    { "\xF0\x9F\x8E\xB1", "Pool 8 Ball" },
    { "\xF0\x9F\x8E\xAE", "Video Game" },
    { "\xF0\x9F\x8E\xB2", "Game Die" },
    { "\xF0\x9F\x8E\xAF", "Bullseye" }
};

constexpr ChatEmojiChoice seasonalEmojiChoices[] = {
    { "\xF0\x9F\x8E\x83", "Jack-O-Lantern" },
    { "\xE2\x98\x83\xEF\xB8\x8F", "Snowman" },
    { "\xE2\x9B\x84", "Snowman Without Snow" },
    { "\xE2\x9D\x84\xEF\xB8\x8F", "Snowflake" },
    { "\xE2\xAD\x90", "Star" },
    { "\xF0\x9F\x9A\x80", "Rocket" },
    { "\xF0\x9F\x94\xA5", "Fire" },
    { "\xF0\x9F\x8C\x9F", "Glowing Star" },
    { "\xF0\x9F\x8C\xA0", "Shooting Star" }
};

constexpr ChatEmojiChoice clothingEmojiChoices[] = {
    { "\xF0\x9F\x91\x93", "Glasses" },
    { "\xF0\x9F\x95\xB6\xEF\xB8\x8F", "Sunglasses" },
    { "\xF0\x9F\xA5\xBD", "Goggles" },
    { "\xF0\x9F\xA5\xBC", "Lab Coat" },
    { "\xF0\x9F\xA6\xBA", "Safety Vest" },
    { "\xF0\x9F\x91\x94", "Necktie" },
    { "\xF0\x9F\x91\x95", "T-Shirt" },
    { "\xF0\x9F\x91\x96", "Jeans" },
    { "\xF0\x9F\xA7\xA3", "Scarf" },
    { "\xF0\x9F\xA7\xA4", "Gloves" },
    { "\xF0\x9F\xA7\xA5", "Coat" },
    { "\xF0\x9F\xA7\xA6", "Socks" },
    { "\xF0\x9F\x91\x97", "Dress" },
    { "\xF0\x9F\x91\x98", "Kimono" },
    { "\xF0\x9F\xA5\xBB", "Sari" },
    { "\xF0\x9F\xA9\xB1", "One-Piece Swimsuit" },
    { "\xF0\x9F\xA9\xB2", "Briefs" },
    { "\xF0\x9F\xA9\xB3", "Shorts" },
    { "\xF0\x9F\x91\x99", "Bikini" },
    { "\xF0\x9F\x91\x9A", "Woman's Clothes" },
    { "\xF0\x9F\x91\x9B", "Purse" },
    { "\xF0\x9F\x91\x9C", "Handbag" },
    { "\xF0\x9F\x91\x9D", "Clutch Bag" },
    { "\xF0\x9F\x9B\x8D\xEF\xB8\x8F", "Shopping Bags" },
    { "\xF0\x9F\x8E\x92", "Backpack" },
    { "\xF0\x9F\xA9\xB4", "Thong Sandal" },
    { "\xF0\x9F\x91\x9E", "Man's Shoe" },
    { "\xF0\x9F\x91\x9F", "Running Shoe" },
    { "\xF0\x9F\xA5\xBE", "Hiking Boot" },
    { "\xF0\x9F\xA5\xBF", "Flat Shoe" },
    { "\xF0\x9F\x91\xA0", "High-Heeled Shoe" },
    { "\xF0\x9F\x91\xA1", "Woman's Sandal" },
    { "\xF0\x9F\xA9\xB0", "Ballet Shoes" },
    { "\xF0\x9F\x91\xA2", "Woman's Boot" },
    { "\xF0\x9F\x91\x91", "Crown" },
    { "\xF0\x9F\x91\x92", "Woman's Hat" },
    { "\xF0\x9F\x8E\xA9", "Top Hat" },
    { "\xF0\x9F\x8E\x93", "Graduation Cap" },
    { "\xF0\x9F\xA7\xA2", "Billed Cap" },
    { "\xF0\x9F\xAA\x96", "Military Helmet" },
    { "\xE2\x9B\x91\xEF\xB8\x8F", "Rescue Worker Helmet" },
    { "\xF0\x9F\x93\xBF", "Prayer Beads" },
    { "\xF0\x9F\x92\x84", "Lipstick" },
    { "\xF0\x9F\x92\x8D", "Ring" },
    { "\xF0\x9F\x92\x8E", "Gem Stone" }
};

struct ChatEmojiCategory
{
    const char* label;
    const ChatEmojiChoice* choices;
    int count;
};

#define CHAT_EMOJI_COUNT(array) (int)(sizeof(array) / sizeof((array)[0]))

constexpr ChatEmojiCategory chatEmojiCategories[] = {
    { "Music & Sound", musicSoundEmojiChoices, CHAT_EMOJI_COUNT(musicSoundEmojiChoices) },
    { "Hands & Reactions", handsEmojiChoices, CHAT_EMOJI_COUNT(handsEmojiChoices) },
    { "Faces & Body", facesBodyEmojiChoices, CHAT_EMOJI_COUNT(facesBodyEmojiChoices) },
    { "People", peopleEmojiChoices, CHAT_EMOJI_COUNT(peopleEmojiChoices) },
    { "Food & Party", foodPartyEmojiChoices, CHAT_EMOJI_COUNT(foodPartyEmojiChoices) },
    { "Sports & Objects", sportsObjectsEmojiChoices, CHAT_EMOJI_COUNT(sportsObjectsEmojiChoices) },
    { "Seasonal", seasonalEmojiChoices, CHAT_EMOJI_COUNT(seasonalEmojiChoices) },
    { "Clothing", clothingEmojiChoices, CHAT_EMOJI_COUNT(clothingEmojiChoices) }
};

#undef CHAT_EMOJI_COUNT

juce::String emojiText(const ChatEmojiChoice& choice)
{
    return juce::String::fromUTF8(choice.utf8);
}

static bool openSystemCameraSettings()
{
#if JUCE_WINDOWS
    const auto result = ShellExecuteW(nullptr, L"open", L"ms-settings:camera", nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
    return false;
#endif
}

juce::String emojiCommandForLabel(juce::String label)
{
    label = label.trim().toLowerCase();
    juce::String command;
    bool lastWasDash = false;
    for (int i = 0; i < label.length(); ++i)
    {
        const auto c = label[i];
        const bool isAlphaNum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (isAlphaNum)
        {
            command << juce::String::charToString(c);
            lastWasDash = false;
        }
        else if (!lastWasDash && command.isNotEmpty())
        {
            command << "-";
            lastWasDash = true;
        }
    }

    while (command.endsWithChar('-'))
        command = command.dropLastCharacters(1);

    return command.isNotEmpty() ? ":" + command + ":" : juce::String();
}

juce::String emojiShortcutForLabel(juce::String label)
{
    label = label.trim().toLowerCase();
    if (label == "smile" || label == "grinning face" || label == "happy")
        return ":)";
    if (label == "laugh" || label == "tears of joy")
        return ":D";
    if (label == "cool" || label == "sunglasses face")
        return "B)";
    if (label == "heart")
        return "<3";
    if (label == "thumbs up")
        return ":+1:";
    if (label == "fire")
        return ":fire:";
    if (label == "guitar")
        return ":guitar:";
    if (label == "drums" || label == "drum")
        return ":drums:";
    if (label == "mic" || label == "microphone")
        return ":mic:";
    return emojiCommandForLabel(label);
}

juce::String emojiMenuText(const ChatEmojiChoice& choice)
{
    const auto shortcut = emojiShortcutForLabel(choice.label);
    return shortcut.isNotEmpty() ? emojiText(choice) + "  " + shortcut
                                 : emojiText(choice);
}

juce::String expandChatEmojiShortcuts(juce::String text)
{
    struct Replacement
    {
        const char* shortcut;
        const char* emojiUtf8;
    };

    static constexpr Replacement replacements[] =
    {
        { ":)", "\xF0\x9F\x98\x80" },
        { ":-)", "\xF0\x9F\x98\x80" },
        { ":D", "\xF0\x9F\x98\x82" },
        { ":-D", "\xF0\x9F\x98\x82" },
        { "B)", "\xF0\x9F\x98\x8E" },
        { "B-)", "\xF0\x9F\x98\x8E" },
        { "<3", "\xE2\x9D\xA4\xEF\xB8\x8F" },
        { ":fire:", "\xF0\x9F\x94\xA5" },
        { ":guitar:", "\xF0\x9F\x8E\xB8" },
        { ":drums:", "\xF0\x9F\xA5\x81" },
        { ":mic:", "\xF0\x9F\x8E\xA4" },
        { ":+1:", "\xF0\x9F\x91\x8D" }
    };

    for (const auto& replacement : replacements)
        text = text.replace(replacement.shortcut, juce::String::fromUTF8(replacement.emojiUtf8), true);

    auto expandChoices = [&text](const ChatEmojiChoice* choices, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            const auto command = emojiCommandForLabel(choices[i].label);
            if (command.isNotEmpty())
                text = text.replace(command, emojiText(choices[i]), true);
        }
    };

    expandChoices(commonEmojiChoices, (int)(sizeof(commonEmojiChoices) / sizeof(commonEmojiChoices[0])));
    for (const auto& category : chatEmojiCategories)
        expandChoices(category.choices, category.count);

    return text;
}

using EmojiMenuMap = std::map<int, const ChatEmojiChoice*>;

void addEmojiChoicesToMenu(juce::PopupMenu& menu,
                           int baseId,
                           const ChatEmojiChoice* choices,
                           int count,
                           EmojiMenuMap& idToEmoji)
{
    for (int i = 0; i < count; ++i)
    {
        const int id = baseId + i;
        idToEmoji[id] = &choices[i];
        menu.addItem(id, emojiMenuText(choices[i]));
    }
}

void populateEmojiMenu(juce::PopupMenu& menu, int baseId, EmojiMenuMap& idToEmoji)
{
    juce::PopupMenu commonMenu;
    addEmojiChoicesToMenu(commonMenu,
                          baseId,
                          commonEmojiChoices,
                          (int)(sizeof(commonEmojiChoices) / sizeof(commonEmojiChoices[0])),
                          idToEmoji);
    menu.addSubMenu("Common", commonMenu);

    constexpr int categoryIdStride = 200;
    const int categoryBaseId = baseId + 1000;
    for (int categoryIndex = 0; categoryIndex < (int)(sizeof(chatEmojiCategories) / sizeof(chatEmojiCategories[0])); ++categoryIndex)
    {
        const auto& category = chatEmojiCategories[categoryIndex];
        juce::PopupMenu categoryMenu;
        addEmojiChoicesToMenu(categoryMenu,
                              categoryBaseId + categoryIndex * categoryIdStride,
                              category.choices,
                              category.count,
                              idToEmoji);
        menu.addSubMenu(category.label, categoryMenu);
    }
}

void showEmojiMenuForTextEditor(juce::TextEditor& targetEditor, juce::Component& anchorComponent)
{
    constexpr int emojiBaseId = 100;
    juce::PopupMenu menu;
    auto idToEmoji = std::make_shared<EmojiMenuMap>();
    populateEmojiMenu(menu, emojiBaseId, *idToEmoji);

    juce::Component::SafePointer<juce::TextEditor> safeEditor(&targetEditor);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchorComponent),
                       [safeEditor, idToEmoji](int result)
                       {
                           auto it = idToEmoji->find(result);
                           if (it == idToEmoji->end() || it->second == nullptr || safeEditor == nullptr)
                               return;

                           safeEditor->grabKeyboardFocus();
                           safeEditor->insertTextAtCaret(emojiText(*it->second));
                       });
}

class EmojiGridComponent : public juce::PopupMenu::CustomComponent
{
public:
    explicit EmojiGridComponent(juce::TextEditor& targetEditor)
        : juce::PopupMenu::CustomComponent(false),
          safeEditor(&targetEditor)
    {
        for (const auto& choice : commonEmojiChoices)
        {
            auto button = std::make_unique<juce::TextButton>(emojiText(choice));
            const auto shortcut = emojiShortcutForLabel(choice.label);
            button->setTooltip(shortcut.isNotEmpty()
                ? juce::String(choice.label) + "  " + shortcut
                : juce::String(choice.label));
            button->onClick = [this, emoji = emojiText(choice)]
            {
                if (safeEditor != nullptr)
                {
                    safeEditor->grabKeyboardFocus();
                    safeEditor->insertTextAtCaret(emoji);
                }

                if (auto* callout = findParentComponentOfClass<juce::CallOutBox>())
                    callout->dismiss();
                else
                    juce::PopupMenu::dismissAllActiveMenus();
            };
            addAndMakeVisible(*button);
            buttons.push_back(std::move(button));
        }

        setSize(214, 104);
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        idealWidth = 214;
        idealHeight = 104;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        constexpr int columns = 6;
        constexpr int gap = 4;
        const int buttonW = (area.getWidth() - gap * (columns - 1)) / columns;
        const int buttonH = (area.getHeight() - gap * 2) / 3;
        for (int i = 0; i < (int)buttons.size(); ++i)
        {
            const int row = i / columns;
            const int col = i % columns;
            buttons[(size_t)i]->setBounds(area.getX() + col * (buttonW + gap),
                                          area.getY() + row * (buttonH + gap),
                                          buttonW,
                                          buttonH);
        }
    }

private:
    juce::Component::SafePointer<juce::TextEditor> safeEditor;
    std::vector<std::unique_ptr<juce::TextButton>> buttons;
};

void showEmojiGridForTextEditor(juce::TextEditor& targetEditor, juce::Component& anchorComponent)
{
    auto content = std::make_unique<EmojiGridComponent>(targetEditor);
    auto& callout = juce::CallOutBox::launchAsynchronously(std::move(content), anchorComponent.getScreenBounds(), nullptr);
    callout.setDismissalMouseClicksAreAlwaysConsumed(true);
}

bool isChatUrlTerminator(juce_wchar c)
{
    return juce::CharacterFunctions::isWhitespace(c)
        || c == '<'
        || c == '>'
        || c == '"'
        || c == '\'';
}

bool isTrailingChatUrlPunctuation(juce_wchar c)
{
    return c == '.'
        || c == ','
        || c == ';'
        || c == ':'
        || c == '!'
        || c == '?'
        || c == ')'
        || c == ']'
        || c == '}';
}

int findNextChatUrlStart(const juce::String& text, int searchStart)
{
    const int httpIndex = text.indexOfIgnoreCase(searchStart, "http://");
    const int httpsIndex = text.indexOfIgnoreCase(searchStart, "https://");

    if (httpIndex < 0)
        return httpsIndex;
    if (httpsIndex < 0)
        return httpIndex;
    return juce::jmin(httpIndex, httpsIndex);
}

bool findNextChatUrlRange(const juce::String& text, int searchStart, juce::Range<int>& range)
{
    const int totalLength = text.length();
    int start = findNextChatUrlStart(text, searchStart);

    while (start >= 0 && start < totalLength)
    {
        int end = start;
        while (end < totalLength && !isChatUrlTerminator(text[end]))
            ++end;

        while (end > start && isTrailingChatUrlPunctuation(text[end - 1]))
            --end;

        if (end > start)
        {
            range = { start, end };
            return true;
        }

        start = findNextChatUrlStart(text, start + 1);
    }

    return false;
}

bool isHttpOrHttpsChatInputUrl(juce::String url)
{
    url = url.trim().toLowerCase();
    return url.startsWith("http://") || url.startsWith("https://");
}

struct ChatMenuChoice
{
    const char* key;
    const char* label;
};

static constexpr ChatMenuChoice chatColourChoices[] = {
    { "aurora", "Aurora" },
    { "ocean", "Ocean" },
    { "sunset", "Sunset" },
    { "candy", "Candy" },
    { "lime", "Lime" },
    { "fire", "Fire" },
    { "violet", "Violet" },
    { "mono", "Mono" },
    { "ruby", "Ruby" },
    { "copper", "Copper" },
    { "lemon", "Lemon" },
    { "emerald", "Emerald" },
    { "cyan", "Cyan" },
    { "sapphire", "Sapphire" },
    { "plum", "Plum" },
    { "pearl", "Pearl" }
};

static constexpr ChatMenuChoice chatWindowColourChoices[] = {
    { "default", "Default" },
    { "graphite", "Graphite" },
    { "midnight", "Midnight" },
    { "charcoal", "Charcoal" },
    { "deepblue", "Deep Blue" },
    { "forest", "Forest" },
    { "burgundy", "Burgundy" },
    { "aubergine", "Aubergine" },
    { "espresso", "Espresso" },
    { "slate", "Slate" },
    { "black", "Black" }
};

juce::String normaliseChatWindowColourKey(juce::String key)
{
    key = key.trim().toLowerCase().removeCharacters(" _-");

    for (const auto& choice : chatWindowColourChoices)
        if (key == choice.key)
            return key;

    return "default";
}

juce::Colour chatWindowColourForKey(const juce::String& keyIn)
{
    const auto key = normaliseChatWindowColourKey(keyIn);
    if (key == "graphite")  return juce::Colour(0xff171b20);
    if (key == "midnight")  return juce::Colour(0xff0d1320);
    if (key == "charcoal")  return juce::Colour(0xff1a1a1a);
    if (key == "deepblue")  return juce::Colour(0xff101827);
    if (key == "forest")    return juce::Colour(0xff101b17);
    if (key == "burgundy")  return juce::Colour(0xff221016);
    if (key == "aubergine") return juce::Colour(0xff1d1424);
    if (key == "espresso")  return juce::Colour(0xff1c1713);
    if (key == "slate")     return juce::Colour(0xff1f2933);
    if (key == "black")     return juce::Colour(0xff07090b);
    return juce::Colour(0xff101417);
}

class VoteValueComponent : public juce::Component
{
public:
    VoteValueComponent(const juce::String& labelText, int minValue, int maxValue, int initialValue)
    {
        label.setText(labelText, juce::dontSendNotification);
        label.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(label);

        slider.setRange(minValue, maxValue, 1.0);
        slider.setValue(juce::jlimit(minValue, maxValue, initialValue), juce::dontSendNotification);
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 22);
        addAndMakeVisible(slider);

        setSize(300, 58);
    }

    int getValue() const
    {
        return (int)std::round(slider.getValue());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(2);
        label.setBounds(area.removeFromTop(20));
        slider.setBounds(area);
    }

private:
    juce::Label label;
    juce::Slider slider;
};

void showChatVoteDialog(NinjamVst3AudioProcessor& processor, const juce::String& voteKind)
{
    const bool isBpi = voteKind.equalsIgnoreCase("bpi");
    const int minValue = isBpi ? 4 : 40;
    const int maxValue = isBpi ? 128 : 240;
    const int initialValue = isBpi ? processor.getBPI() : (int)std::round(processor.getBPM());

    auto* alert = new juce::AlertWindow(isBpi ? "Vote BPI" : "Vote BPM",
                                        "Choose the value to vote for.",
                                        juce::AlertWindow::QuestionIcon);
    auto* valueComponent = new VoteValueComponent(isBpi ? "BPI" : "BPM", minValue, maxValue, initialValue);
    alert->addCustomComponent(valueComponent);
    alert->addButton("Vote", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    auto* processorPtr = &processor;
    alert->enterModalState(true,
                           juce::ModalCallbackFunction::create(
                               [alert, valueComponent, processorPtr, voteKind](int result)
                               {
                                   std::unique_ptr<VoteValueComponent> valueOwner(valueComponent);
                                   std::unique_ptr<juce::AlertWindow> alertOwner(alert);

                                   if (result != 1)
                                       return;

                                   const auto normalisedKind = voteKind.toLowerCase();
                                   processorPtr->sendChatMessage("!vote " + normalisedKind + " " + juce::String(valueOwner->getValue()));
                               }),
                           false);
}

class ChatTtsVolumeMenuComponent : public juce::PopupMenu::CustomComponent
{
public:
    ChatTtsVolumeMenuComponent(float initialVolume, std::function<void(float)> onVolumeChangedCallback)
        : juce::PopupMenu::CustomComponent(false),
          onVolumeChanged(std::move(onVolumeChangedCallback))
    {
        titleLabel.setText("Volume", juce::dontSendNotification);
        titleLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(titleLabel);

        volumeSlider.setRange(0.0, 100.0, 1.0);
        volumeSlider.setValue((double) juce::jlimit(0.0f, 1.0f, initialVolume) * 100.0, juce::dontSendNotification);
        volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 20);
        volumeSlider.setTextValueSuffix("%");
        volumeSlider.onValueChange = [this]
        {
            if (onVolumeChanged)
                onVolumeChanged((float) volumeSlider.getValue() / 100.0f);
        };
        addAndMakeVisible(volumeSlider);

        setSize(250, 48);
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        idealWidth = 250;
        idealHeight = 48;
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 5);
        titleLabel.setBounds(area.removeFromTop(18));
        volumeSlider.setBounds(area);
    }

private:
    juce::Label titleLabel;
    juce::Slider volumeSlider;
    std::function<void(float)> onVolumeChanged;
};

void showChatAttachmentMenu(NinjamVst3AudioProcessor& processor,
                            juce::TextEditor& targetEditor,
                            juce::Component& anchorComponent,
                            std::function<void()> showGifPicker,
                            std::function<void(const juce::String&)> onChatColourSelected,
                            std::function<void(const juce::String&)> onChatWindowColourSelected,
                            const juce::String& selectedChatWindowColourKey,
                            bool chatTtsEnabled,
                            const juce::String& selectedTtsVoiceId,
                            const juce::StringArray& chatTtsVoiceIds,
                            const juce::StringArray& chatTtsVoiceNames,
                            float chatTtsVolume,
                            const juce::String& selectedTtsOutputId,
                            const juce::StringArray& chatTtsOutputIds,
                            const juce::StringArray& chatTtsOutputNames,
                            std::function<void(bool)> onChatTtsEnabledChanged,
                            std::function<void(const juce::String&)> onChatTtsVoiceSelected,
                            std::function<void(float)> onChatTtsVolumeChanged,
                            std::function<void(const juce::String&)> onChatTtsOutputSelected)
{
    constexpr int emojiBaseId = 100;
    constexpr int colourBaseId = 9000;
    constexpr int windowColourBaseId = 9200;
    constexpr int ttsVoiceBaseId = 9400;
    constexpr int ttsOutputBaseId = 9600;
    const juce::String draft = targetEditor.getText().trim();
    const bool draftIsUrl = isHttpOrHttpsChatInputUrl(draft);

    auto idToColour = std::make_shared<std::map<int, juce::String>>();
    const juce::String selectedColour = processor.getLocalChatColourKey();
    juce::PopupMenu colourMenu;
    for (int i = 0; i < (int)(sizeof(chatColourChoices) / sizeof(chatColourChoices[0])); ++i)
    {
        const int id = colourBaseId + i;
        const juce::String key = chatColourChoices[i].key;
        (*idToColour)[id] = key;
        colourMenu.addItem(id, chatColourChoices[i].label, true, key == selectedColour);
    }

    auto idToWindowColour = std::make_shared<std::map<int, juce::String>>();
    const juce::String selectedWindowColour = normaliseChatWindowColourKey(selectedChatWindowColourKey);
    juce::PopupMenu windowColourMenu;
    for (int i = 0; i < (int)(sizeof(chatWindowColourChoices) / sizeof(chatWindowColourChoices[0])); ++i)
    {
        const int id = windowColourBaseId + i;
        const juce::String key = chatWindowColourChoices[i].key;
        (*idToWindowColour)[id] = key;
        windowColourMenu.addItem(id, chatWindowColourChoices[i].label, true, key == selectedWindowColour);
    }

    auto idToTtsVoice = std::make_shared<std::map<int, juce::String>>();
    juce::PopupMenu ttsVoiceMenu;
    const int voiceCount = juce::jmin(chatTtsVoiceIds.size(), chatTtsVoiceNames.size());
    for (int i = 0; i < voiceCount; ++i)
    {
        const int id = ttsVoiceBaseId + i;
        const juce::String voiceId = chatTtsVoiceIds[i];
        (*idToTtsVoice)[id] = voiceId;
        ttsVoiceMenu.addItem(id, chatTtsVoiceNames[i], true, voiceId == selectedTtsVoiceId);
    }

    auto idToTtsOutput = std::make_shared<std::map<int, juce::String>>();
    juce::PopupMenu ttsOutputMenu;
    const int outputCount = juce::jmin(chatTtsOutputIds.size(), chatTtsOutputNames.size());
    for (int i = 0; i < outputCount; ++i)
    {
        const int id = ttsOutputBaseId + i;
        const juce::String outputId = chatTtsOutputIds[i];
        (*idToTtsOutput)[id] = outputId;
        ttsOutputMenu.addItem(id, chatTtsOutputNames[i], true, outputId == selectedTtsOutputId);
    }

    juce::PopupMenu menu;
    menu.addSectionHeader("GIFs");
    menu.addItem(1, "GIF");
    menu.addItem(2, "Send pasted GIF URL", draftIsUrl, false);
    menu.addSeparator();
    menu.addSectionHeader("Images");
    menu.addItem(3, "Send pasted Image URL", draftIsUrl, false);
    if (!draftIsUrl)
        menu.addItem(4, "Paste an http:// or https:// URL in the message box first", false, false);
    menu.addSeparator();
    menu.addSectionHeader("Voting");
    menu.addItem(5, "Vote BPM...");
    menu.addItem(6, "Vote BPI...");
    menu.addSeparator();
    menu.addSubMenu("My Chat Colour", colourMenu);
    menu.addSubMenu("Chat Window Colour", windowColourMenu);
    menu.addSeparator();
    menu.addSectionHeader("Text to Speech");
    menu.addItem(8, "Speak User Chat", true, chatTtsEnabled);
    menu.addCustomItem(10,
                       std::make_unique<ChatTtsVolumeMenuComponent>(chatTtsVolume, std::move(onChatTtsVolumeChanged)),
                       nullptr,
                       "Volume");
    if (voiceCount > 0)
        menu.addSubMenu("Voice", ttsVoiceMenu, chatTtsEnabled);
    else
        menu.addItem(9, "No voices available", false, false);
    if (outputCount > 0)
        menu.addSubMenu("Output", ttsOutputMenu, chatTtsEnabled);
    else
        menu.addItem(11, "No outputs available", false, false);
    menu.addSeparator();
    auto idToEmoji = std::make_shared<EmojiMenuMap>();
    menu.addCustomItem(7, std::make_unique<EmojiGridComponent>(targetEditor), nullptr, "Emoji");
    juce::PopupMenu emojiMenu;
    populateEmojiMenu(emojiMenu, emojiBaseId, *idToEmoji);
    menu.addSubMenu("More Emoji", emojiMenu);

    juce::Component::SafePointer<juce::TextEditor> safeEditor(&targetEditor);
    auto* processorPtr = &processor;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchorComponent),
                       [safeEditor, processorPtr, idToEmoji, idToColour, idToWindowColour, idToTtsVoice, idToTtsOutput,
                        showGifPicker = std::move(showGifPicker),
                        onChatColourSelected = std::move(onChatColourSelected),
                        onChatWindowColourSelected = std::move(onChatWindowColourSelected),
                        onChatTtsEnabledChanged = std::move(onChatTtsEnabledChanged),
                        onChatTtsVoiceSelected = std::move(onChatTtsVoiceSelected),
                        onChatTtsOutputSelected = std::move(onChatTtsOutputSelected),
                        chatTtsEnabled](int result) mutable
                       {
                           if (result == 8)
                           {
                               if (onChatTtsEnabledChanged)
                                   onChatTtsEnabledChanged(!chatTtsEnabled);
                               return;
                           }

                           auto voiceIt = idToTtsVoice->find(result);
                           if (voiceIt != idToTtsVoice->end())
                           {
                               if (onChatTtsVoiceSelected)
                                   onChatTtsVoiceSelected(voiceIt->second);
                               return;
                           }

                           auto outputIt = idToTtsOutput->find(result);
                           if (outputIt != idToTtsOutput->end())
                           {
                               if (onChatTtsOutputSelected)
                                   onChatTtsOutputSelected(outputIt->second);
                               return;
                           }

                           auto colourIt = idToColour->find(result);
                           if (colourIt != idToColour->end())
                           {
                               processorPtr->setLocalChatColourKey(colourIt->second);
                               if (onChatColourSelected)
                                   onChatColourSelected(colourIt->second);
                               return;
                           }

                           auto windowColourIt = idToWindowColour->find(result);
                           if (windowColourIt != idToWindowColour->end())
                           {
                               if (onChatWindowColourSelected)
                                   onChatWindowColourSelected(windowColourIt->second);
                               return;
                           }

                           auto emojiIt = idToEmoji->find(result);
                           if (emojiIt != idToEmoji->end() && emojiIt->second != nullptr)
                           {
                               if (safeEditor == nullptr)
                                   return;

                               safeEditor->grabKeyboardFocus();
                               safeEditor->insertTextAtCaret(emojiText(*emojiIt->second));
                               return;
                           }

                           if (result == 1)
                           {
                               if (showGifPicker)
                                   showGifPicker();
                               return;
                           }

                           if (result == 5 || result == 6)
                           {
                               showChatVoteDialog(*processorPtr, result == 5 ? "bpm" : "bpi");
                               return;
                           }

                           if (result == 2 || result == 3)
                           {
                               if (safeEditor == nullptr)
                                   return;

                               const juce::String url = safeEditor->getText().trim();
                               const bool validUrl = isHttpOrHttpsChatInputUrl(url);
                               processorPtr->sendChatAttachment(result == 2 ? "gif" : "image", url);
                               if (validUrl)
                                   safeEditor->clear();
                           }
                       });
}

juce::String buildLinkAudioLocalInputLabel(NinjamVst3AudioProcessor& processor)
{
    const juce::String selectedKey = processor.getLinkAudioReceiveSelection();
    if (!processor.isLinkAudioEnabled())
        return selectedKey.isNotEmpty() ? "Link In (disabled)" : juce::String();

    if (selectedKey.isEmpty())
        return "Link In (select channel in Options)";

    for (const auto& channel : processor.getLinkAudioAvailableChannels())
    {
        if (channel.key != selectedKey)
            continue;

        juce::String label = "Link In: " + channel.name;
        if (channel.peerName.isNotEmpty())
            label << " (" << channel.peerName << ")";
        return label;
    }

    return "Link In";
}

juce::String buildSyncTooltip(NinjamVst3AudioProcessor::SyncMode syncMode, float compensationMs)
{
    juce::String sourceLabel = "Host Transport";
    if (syncMode == NinjamVst3AudioProcessor::SyncMode::abletonLink)
        sourceLabel = "Ableton Link";

    juce::String tooltip = "Click to toggle transport sync. Right-click for sync source and compensation. Source: ";
    tooltip << sourceLabel
            << ". Current advance: "
            << juce::String(compensationMs, compensationMs < 10.0f ? 1 : 0)
            << " ms.";
    return tooltip;
}

void populateTranslationLanguageMenu(juce::PopupMenu& menu,
                                     int baseId,
                                     const juce::String& selectedCode,
                                     std::map<int, juce::String>& idToCode)
{
    const juce::String normalised = normaliseTranslateLangCode(selectedCode);
    const size_t numChoices = sizeof(translationLanguageChoices) / sizeof(translationLanguageChoices[0]);

    for (size_t index = 0; index < numChoices; ++index)
    {
        const int id = baseId + (int) index + 1;
        const auto& choice = translationLanguageChoices[index];
        menu.addItem(id, choice.label, true, normalised == choice.code);
        idToCode[id] = choice.code;
    }
}

void showTranslateLanguageMenuForButton(NinjamVst3AudioProcessor& processor,
                                        juce::Component& anchorComponent,
                                        std::function<void()> onUpdated)
{
    auto idToCode = std::make_shared<std::map<int, juce::String>>();
    juce::PopupMenu targetMenu;
    populateTranslationLanguageMenu(targetMenu, 200, processor.getTranslateTargetLang(), *idToCode);

    juce::PopupMenu menu;
    menu.addSectionHeader("Translation");
    menu.addItem(1, "Source language is auto-detected from chat. Choose the translation target.", false, false);
    menu.addSeparator();
    menu.addSubMenu("Translate To", targetMenu);

    const auto screenPos = anchorComponent.getScreenPosition();
    juce::Rectangle<int> popupAnchor(screenPos.x,
                                     screenPos.y + anchorComponent.getHeight(),
                                     anchorComponent.getWidth(),
                                     1);
    auto* processorPtr = &processor;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchorComponent).withTargetScreenArea(popupAnchor),
                       [idToCode, processorPtr, onUpdated = std::move(onUpdated)](int result) mutable
                       {
                           if (result <= 0)
                               return;

                           auto it = idToCode->find(result);
                           if (it == idToCode->end())
                               return;

                           if (result >= 200)
                               processorPtr->setTranslateTargetLang(it->second);

                           if (onUpdated)
                               onUpdated();
                       });
}

void showSyncCompensationMenuForButton(NinjamVst3AudioProcessor& processor,
                                       juce::Component& anchorComponent,
                                       std::function<void()> onUpdated)
{
    static constexpr float presetValuesMs[] = { 0.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f, 32.0f, 40.0f,
                                                50.0f, 64.0f, 80.0f, 96.0f, 128.0f, 160.0f, 192.0f, 250.0f };

    auto idToMs = std::make_shared<std::map<int, float>>();
    juce::PopupMenu presetMenu;
    const float currentMs = processor.getSyncStartCompensationMs();

    int id = 100;
    for (float presetMs : presetValuesMs)
    {
        ++id;
        presetMenu.addItem(id,
                           juce::String(presetMs, presetMs < 10.0f ? 1 : 0) + " ms",
                           true,
                           std::abs(currentMs - presetMs) < 0.1f);
        (*idToMs)[id] = presetMs;
    }

    juce::PopupMenu menu;
    menu.addSectionHeader("Host Sync Compensation");
    menu.addItem(1, "Use this if Cubase/ASIO Guard makes NINJAM start late.", false, false);
    menu.addSeparator();
    menu.addSubMenu("Advance NINJAM Start", presetMenu);

    const auto screenPos = anchorComponent.getScreenPosition();
    juce::Rectangle<int> popupAnchor(screenPos.x,
                                     screenPos.y + anchorComponent.getHeight(),
                                     anchorComponent.getWidth(),
                                     1);

    auto* processorPtr = &processor;
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchorComponent).withTargetScreenArea(popupAnchor),
                       [idToMs, processorPtr, onUpdated = std::move(onUpdated)](int result) mutable
                       {
                           auto it = idToMs->find(result);
                           if (it == idToMs->end())
                               return;

                           processorPtr->setSyncStartCompensationMs(it->second);
                           if (onUpdated)
                               onUpdated();
                       });
}
}

class GifPickerPanel : public juce::Component,
                       private juce::Timer
{
public:
    explicit GifPickerPanel(std::function<void(const juce::String&)> onGifChosenIn)
        : onGifChosen(std::move(onGifChosenIn)),
          aliveFlag(std::make_shared<std::atomic<bool>>(true))
    {
        addAndMakeVisible(searchBox);
        searchBox.setTextToShowWhenEmpty("Search", juce::Colours::grey);
        searchBox.onReturnKey = [this] { startSearch(searchBox.getText().trim()); };
        searchBox.onTextChange = [this]
        {
            pendingSearchText = searchBox.getText().trim();
            pendingSearchAtMs = juce::Time::getMillisecondCounterHiRes() + 450.0;
            startTimerHz(20);
        };

        setVisible(false);
    }

    ~GifPickerPanel() override
    {
        aliveFlag->store(false);
    }

    void openWithSearchText(juce::String initialText)
    {
        if (isHttpOrHttpsChatInputUrl(initialText))
            initialText.clear();

        initialText = initialText.trim();
        setVisible(true);
        toFront(false);
        searchBox.setText(initialText, juce::dontSendNotification);
        searchBox.grabKeyboardFocus();
        startSearch(initialText);
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xfff8fafc));
        g.fillRoundedRectangle(bounds.reduced(0.5f), 9.0f);
        g.setColour(juce::Colour(0xffd7dee7));
        g.drawRoundedRectangle(bounds.reduced(0.5f), 9.0f, 1.0f);

        auto grid = getGridBounds();
        if (items.empty())
        {
            g.setColour(juce::Colour(0xff667085));
            g.setFont(14.0f);
            g.drawFittedText(statusText.isNotEmpty() ? statusText : "Search GIPHY",
                             grid.reduced(12),
                             juce::Justification::centred,
                             2);
        }
        else
        {
            g.saveState();
            g.reduceClipRegion(grid);

            for (int i = 0; i < (int)items.size(); ++i)
            {
                const auto tile = getTileBounds(i);
                if (!tile.intersects(grid))
                    continue;

                g.setColour(juce::Colour(0xffe6ebf1));
                g.fillRoundedRectangle(tile.toFloat(), 5.0f);

                if (items[(size_t)i].preview.isValid())
                {
                    g.drawImageWithin(items[(size_t)i].preview,
                                      tile.getX(),
                                      tile.getY(),
                                      tile.getWidth(),
                                      tile.getHeight(),
                                      juce::RectanglePlacement::fillDestination);
                }
                else
                {
                    g.setColour(juce::Colour(0xff7b8794));
                    g.setFont(12.0f);
                    g.drawFittedText("GIF", tile, juce::Justification::centred, 1);
                }

                if (i == hoveredIndex)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.20f));
                    g.fillRoundedRectangle(tile.toFloat(), 5.0f);
                    g.setColour(juce::Colour(0xff2b7fff));
                    g.drawRoundedRectangle(tile.toFloat().reduced(0.5f), 5.0f, 2.0f);
                }
            }

            g.restoreState();
        }

        auto attribution = getLocalBounds().removeFromBottom(24).reduced(10, 0);
        g.setColour(juce::Colour(0xff98a2b3));
        g.setFont(10.0f);
        g.drawText("POWERED BY", attribution.removeFromLeft(68), juce::Justification::centredLeft);
        g.setColour(juce::Colour(0xff667085));
        g.setFont(juce::Font(14.0f, juce::Font::bold));
        g.drawText("GIPHY", attribution, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10);
        searchBox.setBounds(area.removeFromTop(42));
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        const int nextHover = getTileIndexAt(e.getPosition());
        if (hoveredIndex != nextHover)
        {
            hoveredIndex = nextHover;
            setMouseCursor(hoveredIndex >= 0 ? juce::MouseCursor::PointingHandCursor
                                             : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hoveredIndex = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int index = getTileIndexAt(e.getPosition());
        if (index < 0 || index >= (int)items.size())
            return;

        const juce::String url = items[(size_t)index].sendUrl;
        if (url.isNotEmpty() && onGifChosen)
            onGifChosen(url);
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const int maxScroll = getMaxScrollOffset();
        if (maxScroll <= 0)
            return;

        scrollOffset = juce::jlimit(0, maxScroll, scrollOffset - (int)std::round(wheel.deltaY * 160.0f));
        repaint();
    }

private:
    struct GifResult
    {
        juce::String title;
        juce::String sendUrl;
        juce::String previewUrl;
        juce::Image preview;
    };

    juce::TextEditor searchBox;
    std::vector<GifResult> items;
    std::function<void(const juce::String&)> onGifChosen;
    std::shared_ptr<std::atomic<bool>> aliveFlag;
    juce::String pendingSearchText;
    juce::String statusText { "Search GIPHY" };
    double pendingSearchAtMs = 0.0;
    int searchGeneration = 0;
    int scrollOffset = 0;
    int hoveredIndex = -1;

    static juce::String getGiphyApiKey()
    {
        if (const char* key = std::getenv("NINJAM_GIPHY_API_KEY"))
            if (juce::String(key).trim().isNotEmpty())
                return juce::String(key).trim();

        if (const char* key = std::getenv("GIPHY_API_KEY"))
            if (juce::String(key).trim().isNotEmpty())
                return juce::String(key).trim();

        return "fn0jRWFUv4bvdfYmFaUIy6xkFipj5e82";
    }

    static juce::String getObjectString(juce::DynamicObject* obj, const char* property)
    {
        return obj != nullptr ? obj->getProperty(property).toString() : juce::String();
    }

    static juce::DynamicObject* getChildObject(juce::DynamicObject* obj, const char* property)
    {
        return obj != nullptr ? obj->getProperty(property).getDynamicObject() : nullptr;
    }

    static juce::String getImageUrl(juce::DynamicObject* images, const char* imageName, const char* property)
    {
        return getObjectString(getChildObject(images, imageName), property);
    }

    static juce::Image loadImageFromUrl(const juce::String& urlText)
    {
        if (!isHttpOrHttpsChatInputUrl(urlText))
            return {};

        int statusCode = 0;
        auto stream = juce::URL(urlText).createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(3500)
                .withNumRedirectsToFollow(3)
                .withStatusCode(&statusCode)
                .withExtraHeaders("User-Agent: NINJAMplus/1.0\r\nAccept: image/gif,image/webp,image/png,image/jpeg,*/*\r\n")
                .withHttpRequestCmd("GET"));

        if (stream == nullptr || (statusCode != 0 && (statusCode < 200 || statusCode >= 300)))
            return {};

        juce::MemoryBlock data;
        stream->readIntoMemoryBlock(data, 1024 * 1024);
        if (data.getSize() == 0)
            return {};

        juce::MemoryInputStream imageStream(data, false);
        return juce::ImageFileFormat::loadFrom(imageStream);
    }

    juce::Rectangle<int> getGridBounds() const
    {
        auto area = getLocalBounds().reduced(10);
        area.removeFromTop(50);
        area.removeFromBottom(22);
        return area;
    }

    juce::Rectangle<int> getTileBounds(int index) const
    {
        const auto grid = getGridBounds();
        constexpr int columns = 3;
        constexpr int gap = 4;
        const int tileWidth = juce::jmax(40, (grid.getWidth() - gap * (columns - 1)) / columns);
        const int tileHeight = 78;
        const int row = index / columns;
        const int column = index % columns;
        return { grid.getX() + column * (tileWidth + gap),
                 grid.getY() + row * (tileHeight + gap) - scrollOffset,
                 tileWidth,
                 tileHeight };
    }

    int getMaxScrollOffset() const
    {
        if (items.empty())
            return 0;

        const auto grid = getGridBounds();
        constexpr int columns = 3;
        constexpr int gap = 4;
        const int rows = ((int)items.size() + columns - 1) / columns;
        const int contentHeight = rows * 78 + juce::jmax(0, rows - 1) * gap;
        return juce::jmax(0, contentHeight - grid.getHeight());
    }

    int getTileIndexAt(juce::Point<int> position) const
    {
        const auto grid = getGridBounds();
        if (!grid.contains(position))
            return -1;

        for (int i = 0; i < (int)items.size(); ++i)
            if (getTileBounds(i).contains(position))
                return i;

        return -1;
    }

    void timerCallback() override
    {
        if (pendingSearchAtMs > 0.0 && juce::Time::getMillisecondCounterHiRes() >= pendingSearchAtMs)
        {
            const juce::String query = pendingSearchText;
            pendingSearchAtMs = 0.0;
            stopTimer();
            startSearch(query);
        }
    }

    void startSearch(juce::String query)
    {
        query = query.trim();
        const int generation = ++searchGeneration;
        scrollOffset = 0;
        hoveredIndex = -1;
        items.clear();
        statusText = query.isEmpty() ? "Loading trending GIFs..." : "Searching GIPHY...";
        repaint();

        const auto alive = aliveFlag;
        juce::Component::SafePointer<GifPickerPanel> safeThis(this);
        std::thread([alive, safeThis, generation, query]
        {
            juce::String error;
            std::vector<GifResult> results;
            const juce::String apiKey = getGiphyApiKey();
            juce::URL requestUrl(query.isEmpty()
                                     ? "https://api.giphy.com/v1/gifs/trending"
                                     : "https://api.giphy.com/v1/gifs/search");

            requestUrl = requestUrl.withParameter("api_key", apiKey)
                                   .withParameter("limit", "18")
                                   .withParameter("rating", "pg-13");

            if (query.isNotEmpty())
                requestUrl = requestUrl.withParameter("q", query).withParameter("lang", "en");

            int statusCode = 0;
            auto stream = requestUrl.createInputStream(
                juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                    .withConnectionTimeoutMs(5000)
                    .withNumRedirectsToFollow(3)
                    .withStatusCode(&statusCode)
                    .withExtraHeaders("User-Agent: NINJAMplus/1.0\r\nAccept: application/json\r\n")
                    .withHttpRequestCmd("GET"));

            if (stream == nullptr || (statusCode != 0 && (statusCode < 200 || statusCode >= 300)))
            {
                error = statusCode == 0 ? "GIPHY could not be reached."
                                        : "GIPHY returned HTTP " + juce::String(statusCode) + ".";
            }
            else
            {
                const juce::String response = stream->readEntireStreamAsString();
                const juce::var parsed = juce::JSON::parse(response);
                if (auto* root = parsed.getDynamicObject())
                {
                    if (auto* data = root->getProperty("data").getArray())
                    {
                        for (const auto& entry : *data)
                        {
                            auto* obj = entry.getDynamicObject();
                            auto* images = getChildObject(obj, "images");
                            GifResult item;
                            item.title = getObjectString(obj, "title");
                            item.sendUrl = getImageUrl(images, "fixed_width", "url");
                            if (item.sendUrl.isEmpty())
                                item.sendUrl = getImageUrl(images, "downsized", "url");
                            if (item.sendUrl.isEmpty())
                                item.sendUrl = getImageUrl(images, "original", "url");
                            if (item.sendUrl.isEmpty())
                                item.sendUrl = getObjectString(obj, "url");
                            item.previewUrl = getImageUrl(images, "fixed_width_still", "url");
                            if (item.previewUrl.isEmpty())
                                item.previewUrl = getImageUrl(images, "fixed_width_small_still", "url");
                            if (item.previewUrl.isEmpty())
                                item.previewUrl = getImageUrl(images, "downsized_still", "url");
                            if (item.sendUrl.isNotEmpty())
                            {
                                item.preview = loadImageFromUrl(item.previewUrl);
                                results.push_back(std::move(item));
                            }
                        }
                    }
                }

                if (results.empty() && error.isEmpty())
                    error = "No GIFs found.";
            }

            if (!alive->load())
                return;

            juce::MessageManager::callAsync([alive, safeThis, generation, results = std::move(results), error]() mutable
            {
                if (!alive->load() || safeThis == nullptr || generation != safeThis->searchGeneration)
                    return;

                safeThis->items = std::move(results);
                safeThis->statusText = error;
                safeThis->scrollOffset = 0;
                safeThis->hoveredIndex = -1;
                safeThis->repaint();
            });
        }).detach();
    }
};

void ClickableChatTextEditor::setLinkRanges(const juce::Array<juce::Range<int>>& ranges,
                                            const juce::StringArray& urls)
{
    linkRanges = ranges;
    linkUrls = urls;
    setTemporaryUnderlining(linkRanges);
}

void ClickableChatTextEditor::clearLinkRanges()
{
    linkRanges.clear();
    linkUrls.clear();
    pressedLinkIndex = -1;
    setTemporaryUnderlining({});
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

int ClickableChatTextEditor::findLinkIndexAt(juce::Point<int> position) const
{
    for (int i = 0; i < linkRanges.size() && i < linkUrls.size(); ++i)
    {
        if (getTextBounds(linkRanges.getReference(i)).containsPoint(position))
            return i;
    }

    return -1;
}

void ClickableChatTextEditor::mouseMove(const juce::MouseEvent& e)
{
    setMouseCursor(findLinkIndexAt(e.getPosition()) >= 0
                       ? juce::MouseCursor::PointingHandCursor
                       : juce::MouseCursor::NormalCursor);
    juce::TextEditor::mouseMove(e);
}

void ClickableChatTextEditor::mouseExit(const juce::MouseEvent& e)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
    juce::TextEditor::mouseExit(e);
}

void ClickableChatTextEditor::mouseDown(const juce::MouseEvent& e)
{
    pressedLinkIndex = (e.mods.isLeftButtonDown() && !e.mods.isPopupMenu())
        ? findLinkIndexAt(e.getPosition())
        : -1;

    if (pressedLinkIndex >= 0)
        return;

    juce::TextEditor::mouseDown(e);
}

void ClickableChatTextEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (pressedLinkIndex >= 0)
        return;

    juce::TextEditor::mouseDrag(e);
}

void ClickableChatTextEditor::mouseUp(const juce::MouseEvent& e)
{
    const int linkIndex = pressedLinkIndex;
    pressedLinkIndex = -1;

    if (linkIndex >= 0)
    {
        if (e.mouseWasClicked()
            && linkIndex == findLinkIndexAt(e.getPosition())
            && linkIndex < linkUrls.size())
        {
            juce::URL(linkUrls[linkIndex]).launchInDefaultBrowser();
        }
        return;
    }

    juce::TextEditor::mouseUp(e);
}

class ChatPopupComponent : public juce::Component
{
public:
    ChatPopupComponent(NinjamVst3AudioProcessor& p,
                       juce::String initialChatWindowColourKey,
                       std::function<void(const juce::String&)> onChatColourSelectedCallback,
                       std::function<void(const juce::String&)> onWindowColourSelectedCallback,
                       std::function<bool()> isChatTtsEnabledCallback,
                       std::function<juce::String()> getChatTtsVoiceIdCallback,
                       std::function<juce::StringArray()> getChatTtsVoiceIdsCallback,
                       std::function<juce::StringArray()> getChatTtsVoiceNamesCallback,
                       std::function<float()> getChatTtsVolumeCallback,
                       std::function<juce::String()> getChatTtsOutputIdCallback,
                       std::function<juce::StringArray()> getChatTtsOutputIdsCallback,
                       std::function<juce::StringArray()> getChatTtsOutputNamesCallback,
                       std::function<void(bool)> onChatTtsEnabledChangedCallback,
                       std::function<void(const juce::String&)> onChatTtsVoiceSelectedCallback,
                       std::function<void(float)> onChatTtsVolumeChangedCallback,
                       std::function<void(const juce::String&)> onChatTtsOutputSelectedCallback)
        : processor(p),
          onChatColourSelected(std::move(onChatColourSelectedCallback)),
          onWindowColourSelected(std::move(onWindowColourSelectedCallback)),
          isChatTtsEnabled(std::move(isChatTtsEnabledCallback)),
          getChatTtsVoiceId(std::move(getChatTtsVoiceIdCallback)),
          getChatTtsVoiceIds(std::move(getChatTtsVoiceIdsCallback)),
          getChatTtsVoiceNames(std::move(getChatTtsVoiceNamesCallback)),
          getChatTtsVolume(std::move(getChatTtsVolumeCallback)),
          getChatTtsOutputId(std::move(getChatTtsOutputIdCallback)),
          getChatTtsOutputIds(std::move(getChatTtsOutputIdsCallback)),
          getChatTtsOutputNames(std::move(getChatTtsOutputNamesCallback)),
          onChatTtsEnabledChanged(std::move(onChatTtsEnabledChangedCallback)),
          onChatTtsVoiceSelected(std::move(onChatTtsVoiceSelectedCallback)),
          onChatTtsVolumeChanged(std::move(onChatTtsVolumeChangedCallback)),
          onChatTtsOutputSelected(std::move(onChatTtsOutputSelectedCallback)),
          gifPickerPanel([this](const juce::String& url)
          {
              processor.sendChatAttachment("gif", url);
              gifPickerPanel.setVisible(false);
          })
    {
        addAndMakeVisible(chatDisplay);
        chatDisplay.setMultiLine(true);
        chatDisplay.setReadOnly(true);
        chatDisplay.setFont(juce::Font(14.0f));
        chatDisplay.setCommandLinkCallback([this](const juce::String& command)
        {
            processor.sendChatMessage(command);
        });
        setChatWindowColourKey(initialChatWindowColourKey);

        addAndMakeVisible(chatInput);
        chatInput.onReturnKey = [this] { sendClicked(); };

        addAndMakeVisible(chatEmojiButton);
        chatEmojiButton.setButtonText("+");
        chatEmojiButton.setTooltip("GIFs, images, and emoji");
        chatEmojiButton.onClick = [this]
        {
            showChatAttachmentMenu(processor, chatInput, chatEmojiButton, [this]
            {
                toggleGifPicker();
            },
            [this](const juce::String& key)
            {
                if (onChatColourSelected)
                    onChatColourSelected(key);
            },
            [this](const juce::String& key)
            {
                setChatWindowColourKey(key);
                if (onWindowColourSelected)
                    onWindowColourSelected(key);
            },
            chatWindowColourKey,
            isChatTtsEnabled ? isChatTtsEnabled() : false,
            getChatTtsVoiceId ? getChatTtsVoiceId() : juce::String(),
            getChatTtsVoiceIds ? getChatTtsVoiceIds() : juce::StringArray(),
            getChatTtsVoiceNames ? getChatTtsVoiceNames() : juce::StringArray(),
            getChatTtsVolume ? getChatTtsVolume() : 1.0f,
            getChatTtsOutputId ? getChatTtsOutputId() : juce::String(),
            getChatTtsOutputIds ? getChatTtsOutputIds() : juce::StringArray(),
            getChatTtsOutputNames ? getChatTtsOutputNames() : juce::StringArray(),
            onChatTtsEnabledChanged,
            onChatTtsVoiceSelected,
            onChatTtsVolumeChanged,
            onChatTtsOutputSelected);
        };

        addAndMakeVisible(sendButton);
        sendButton.setButtonText(juce::String::fromUTF8("\xE2\x86\xB5"));
        sendButton.setTooltip("Send");
        sendButton.onClick = [this] { sendClicked(); };

        addAndMakeVisible(atButton);
        atButton.setClickingTogglesState(true);
        atButton.setWantsKeyboardFocus(false);
        atButton.setLookAndFeel(&atPopupBtnLAF);
        atButton.onClick = [this] { atToggled(); };
        atButton.onPopupMenuRequest = [this] { showTranslateLanguageMenu(); };
        refreshTranslateButtonState();

        addChildComponent(gifPickerPanel);
        addMouseListener(this, true);
    }

    ~ChatPopupComponent() override
    {
        atButton.setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(chatWindowColour.withMultipliedBrightness(0.80f));
    }

    void setChatWindowColourKey(const juce::String& key)
    {
        chatWindowColourKey = normaliseChatWindowColourKey(key);
        chatWindowColour = chatWindowColourForKey(chatWindowColourKey);
        chatDisplay.setBackgroundColour(chatWindowColour);
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);
        auto inputArea = area.removeFromBottom(30);
        auto sendArea = inputArea.removeFromRight(26);
        inputArea.removeFromRight(1);
        auto atArea = inputArea.removeFromRight(26);
        inputArea.removeFromRight(1);
        auto attachArea = inputArea.removeFromRight(26);
        inputArea.removeFromRight(2);
        chatInput.setBounds(inputArea);
        chatEmojiButton.setBounds(attachArea);
        sendButton.setBounds(sendArea);
        atButton.setBounds(atArea);
        chatDisplay.setBounds(area);

        if (gifPickerPanel.isVisible())
        {
            auto pickerBounds = chatDisplay.getBounds().reduced(4);
            pickerBounds = pickerBounds.removeFromBottom(juce::jmin(290, juce::jmax(180, pickerBounds.getHeight())));
            gifPickerPanel.setBounds(pickerBounds);
            gifPickerPanel.toFront(false);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (!gifPickerPanel.isVisible())
            return;

        auto* start = e.originalComponent != nullptr ? e.originalComponent : e.eventComponent;
        for (auto* c = start; c != nullptr; c = c->getParentComponent())
        {
            if (c == &gifPickerPanel || c == &chatEmojiButton)
                return;
        }

        gifPickerPanel.setVisible(false);
    }

    void setChatText(const juce::StringArray& lines, const juce::StringArray& senders)
    {
        applyColoredChat(chatDisplay, lines, senders, processor);
    }

    void setDraftText(const juce::String& text)
    {
        chatInput.setText(text, juce::dontSendNotification);
    }

    juce::String getDraftText() const
    {
        return chatInput.getText();
    }

    void refreshTranslateButtonState()
    {
        atButton.setToggleState(processor.isAutoTranslateEnabled(), juce::dontSendNotification);
        atButton.setTooltip(buildTranslateTooltip(processor.getTranslateTargetLang()));
    }

private:
    NinjamVst3AudioProcessor& processor;
    std::function<void(const juce::String&)> onChatColourSelected;
    std::function<void(const juce::String&)> onWindowColourSelected;
    std::function<bool()> isChatTtsEnabled;
    std::function<juce::String()> getChatTtsVoiceId;
    std::function<juce::StringArray()> getChatTtsVoiceIds;
    std::function<juce::StringArray()> getChatTtsVoiceNames;
    std::function<float()> getChatTtsVolume;
    std::function<juce::String()> getChatTtsOutputId;
    std::function<juce::StringArray()> getChatTtsOutputIds;
    std::function<juce::StringArray()> getChatTtsOutputNames;
    std::function<void(bool)> onChatTtsEnabledChanged;
    std::function<void(const juce::String&)> onChatTtsVoiceSelected;
    std::function<void(float)> onChatTtsVolumeChanged;
    std::function<void(const juce::String&)> onChatTtsOutputSelected;
    juce::String chatWindowColourKey { "default" };
    juce::Colour chatWindowColour { 0xff101417 };
    GifPickerPanel gifPickerPanel;
    RichChatDisplayComponent chatDisplay;
    juce::TextEditor chatInput;
    juce::TextButton chatEmojiButton;
    juce::TextButton sendButton;
    TranslateMenuTextButton atButton{ "AT" };
    ATButtonLookAndFeel atPopupBtnLAF;

    void sendClicked()
    {
        auto msg = expandChatEmojiShortcuts(chatInput.getText());
        if (msg.isNotEmpty())
        {
            processor.sendChatMessage(msg);
            chatInput.clear();
        }
    }

    void atToggled()
    {
        processor.setAutoTranslateEnabled(atButton.getToggleState());
        refreshTranslateButtonState();
    }

    void showTranslateLanguageMenu()
    {
        juce::Component::SafePointer<ChatPopupComponent> safeThis(this);
        showTranslateLanguageMenuForButton(processor, atButton, [safeThis]()
        {
            if (safeThis != nullptr)
                safeThis->refreshTranslateButtonState();
        });
    }

    void toggleGifPicker()
    {
        if (gifPickerPanel.isVisible())
        {
            gifPickerPanel.setVisible(false);
            return;
        }

        gifPickerPanel.openWithSearchText({});
        resized();
    }
};

static juce::Rectangle<int> chatPopoutSizeForPreset(int presetIndex)
{
    presetIndex = juce::jlimit(0, 2, presetIndex);
    if (presetIndex == 0)
        return { 0, 0, 420, 340 };
    if (presetIndex == 2)
        return { 0, 0, 680, 540 };
    return { 0, 0, 520, 420 };
}

static juce::Rectangle<int> abletonEditorWindowSizeForPreset(int presetIndex)
{
    presetIndex = juce::jlimit(0, 2, presetIndex);
    if (presetIndex == 0)
        return { 0, 0, 1100, 540 };
    if (presetIndex == 2)
        return { 0, 0, 1380, 700 };
    return { 0, 0, 1240, 600 };
}

class ChatWindow : public juce::DocumentWindow
{
public:
    ChatWindow(NinjamVst3AudioProcessor& p,
               const juce::String& chatWindowColourKey,
               std::function<void(const juce::String&)> onChatColourSelected,
               std::function<void(const juce::String&)> onWindowColourSelected,
               std::function<bool()> isChatTtsEnabled,
               std::function<juce::String()> getChatTtsVoiceId,
               std::function<juce::StringArray()> getChatTtsVoiceIds,
               std::function<juce::StringArray()> getChatTtsVoiceNames,
               std::function<float()> getChatTtsVolume,
               std::function<juce::String()> getChatTtsOutputId,
               std::function<juce::StringArray()> getChatTtsOutputIds,
               std::function<juce::StringArray()> getChatTtsOutputNames,
               std::function<void(bool)> onChatTtsEnabledChanged,
               std::function<void(const juce::String&)> onChatTtsVoiceSelected,
               std::function<void(float)> onChatTtsVolumeChanged,
               std::function<void(const juce::String&)> onChatTtsOutputSelected,
               std::function<void()> onClosedCallback,
               bool abletonHostedWindow,
               int abletonChatWindowSizePreset)
        : DocumentWindow("NINJAM Chat (Ctrl + Mouse Wheel to Zoom)", juce::Colours::black, DocumentWindow::closeButton),
          onClosed(std::move(onClosedCallback)),
          abletonHosted(abletonHostedWindow)
    {
        setUsingNativeTitleBar(!abletonHosted);
        const auto initialSize = abletonHosted
            ? chatPopoutSizeForPreset(abletonChatWindowSizePreset)
            : juce::Rectangle<int>(0, 0, 500, 400);
        if (abletonHosted)
        {
            setResizable(false, false);
            setResizeLimits(initialSize.getWidth(),
                            initialSize.getHeight(),
                            initialSize.getWidth(),
                            initialSize.getHeight());
        }
        else
        {
            setResizable(true, true);
            setResizeLimits(360, 280, 1200, 900);
        }
        setContentOwned(new ChatPopupComponent(p,
                                               chatWindowColourKey,
                                               std::move(onChatColourSelected),
                                               std::move(onWindowColourSelected),
                                               std::move(isChatTtsEnabled),
                                               std::move(getChatTtsVoiceId),
                                               std::move(getChatTtsVoiceIds),
                                               std::move(getChatTtsVoiceNames),
                                               std::move(getChatTtsVolume),
                                               std::move(getChatTtsOutputId),
                                               std::move(getChatTtsOutputIds),
                                               std::move(getChatTtsOutputNames),
                                               std::move(onChatTtsEnabledChanged),
                                               std::move(onChatTtsVoiceSelected),
                                               std::move(onChatTtsVolumeChanged),
                                               std::move(onChatTtsOutputSelected)), true);
        centreWithSize(initialSize.getWidth(), initialSize.getHeight());
        setVisible(true);
    }

    ChatPopupComponent* getPopupComponent() const
    {
        return dynamic_cast<ChatPopupComponent*>(getContentComponent());
    }

    void applyAbletonSizePreset(int presetIndex)
    {
        if (!abletonHosted)
            return;

        const auto size = chatPopoutSizeForPreset(presetIndex);
        const auto centre = getBounds().getCentre();
        setResizeLimits(size.getWidth(), size.getHeight(), size.getWidth(), size.getHeight());
        setSize(size.getWidth(), size.getHeight());
        if (centre.x != 0 || centre.y != 0)
            setCentrePosition(centre.x, centre.y);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        auto callback = onClosed;
        juce::MessageManager::callAsync([callback = std::move(callback)]
        {
            if (callback)
                callback();
        });
    }

private:
    std::function<void()> onClosed;
    bool abletonHosted = false;
};

static ChatPopupComponent* getChatPopupComponent(juce::DocumentWindow* window)
{
    return window != nullptr ? dynamic_cast<ChatPopupComponent*>(window->getContentComponent()) : nullptr;
}

static bool isSupportedSamplePadFile(const juce::File& file)
{
    const auto ext = file.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff"
        || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
}

static juce::Rectangle<int> samplePadsWindowSizeForPreset(int presetIndex)
{
    presetIndex = juce::jlimit(0, 2, presetIndex);
    if (presetIndex == 0)
        return { 0, 0, 900, 540 };
    if (presetIndex == 2)
        return { 0, 0, 1200, 740 };
    return { 0, 0, 1040, 640 };
}

static juce::String getSamplePadLearnTargetId(int padIndex)
{
    return "samplepad.trigger." + juce::String(padIndex + 1);
}

static juce::String getSampleFxKnobLearnTargetId(int slotIndex)
{
    return "samplepad.fx.amount." + juce::String(slotIndex + 1);
}

static int getSampleFxKnobSlotFromLearnTargetId(const juce::String& targetId)
{
    if (!targetId.startsWith("samplepad.fx.amount."))
        return -1;

    return targetId.fromLastOccurrenceOf(".", false, false).getIntValue() - 1;
}

class WidePopupComboBox : public juce::ComboBox
{
public:
    void setPopupMinimumWidth(int width) { popupMinimumWidth = width; }
    std::function<void(bool)> onPopupActiveChanged;

    void showPopup() override
    {
        juce::PopupMenu menu;
        const int selectedId = getSelectedId();

        for (int i = 0; i < getNumItems(); ++i)
        {
            const int itemId = getItemId(i);
            menu.addItem(itemId, getItemText(i), isItemEnabled(itemId), itemId == selectedId);
        }

        if (menu.getNumItems() == 0)
            menu.addItem(1, "No banks", false, false);

        juce::Component::SafePointer<WidePopupComboBox> safeThis(this);
        if (onPopupActiveChanged)
            onPopupActiveChanged(true);

        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(this)
                               .withMinimumWidth(juce::jmax(getWidth(), popupMinimumWidth)),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr)
                                   return;

                               safeThis->hidePopup();

                               if (safeThis->onPopupActiveChanged)
                                   safeThis->onPopupActiveChanged(false);

                               if (result != 0)
                                   safeThis->setSelectedId(result);
                           });
    }

private:
    int popupMinimumWidth = 220;
};

class SampleFxKnobSlider : public LeftClickOnlySlider
{
public:
    using LeftClickOnlySlider::LeftClickOnlySlider;

    std::function<void(const juce::MouseEvent&)> onRouteMouseDown;
    std::function<void(const juce::MouseEvent&)> onRouteMouseDrag;
    std::function<void(const juce::MouseEvent&)> onRouteMouseUp;
    std::function<void(const juce::MouseEvent&)> onPopupMenuRequest;
    std::function<void()> onControlInteraction;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            if (onPopupMenuRequest)
                onPopupMenuRequest(e);
            return;
        }

        routeInteractionActive = e.mods.isLeftButtonDown() && e.mods.isShiftDown();
        if (routeInteractionActive)
        {
            if (onRouteMouseDown)
                onRouteMouseDown(e);
            return;
        }

        if (e.mods.isLeftButtonDown() && onControlInteraction)
            onControlInteraction();
        LeftClickOnlySlider::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (routeInteractionActive)
        {
            if (onRouteMouseDrag)
                onRouteMouseDrag(e);
            return;
        }

        LeftClickOnlySlider::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (routeInteractionActive)
        {
            if (onRouteMouseUp)
                onRouteMouseUp(e);
            routeInteractionActive = false;
            return;
        }

        LeftClickOnlySlider::mouseUp(e);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (onControlInteraction)
            onControlInteraction();
        juce::Slider::mouseWheelMove(e, wheel);
    }

private:
    bool routeInteractionActive = false;
};

class PopupTextButton : public LeftClickOnlyTextButton
{
public:
    using LeftClickOnlyTextButton::LeftClickOnlyTextButton;

    std::function<void(const juce::MouseEvent&)> onPopupMenuRequest;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            if (onPopupMenuRequest)
                onPopupMenuRequest(e);
            return;
        }

        LeftClickOnlyTextButton::mouseDown(e);
    }
};

class SamplePadToggleLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour&, bool highlighted, bool down) override
    {
        const bool active = button.getToggleState();
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        const auto base = active
            ? (button.getName() == "record" ? juce::Colour(0xffff3b3b) : juce::Colour(0xff49d5ff))
            : juce::Colour(0xff2f3337);
        juce::ColourGradient grad(base.brighter(active ? 0.4f : 0.18f), bounds.getX(), bounds.getY(),
                                  base.darker(active ? 0.15f : 0.45f), bounds.getX(), bounds.getBottom(), false);
        g.setGradientFill(grad);
        g.fillRoundedRectangle(bounds, 4.0f);
        g.setColour((highlighted || down) ? juce::Colours::white.withAlpha(0.7f)
                                          : juce::Colours::black.withAlpha(0.75f));
        g.drawRoundedRectangle(bounds, 4.0f, active ? 1.4f : 1.0f);

        auto icon = bounds.reduced(4.0f);
        g.setColour(active ? juce::Colours::black.withAlpha(0.9f)
                           : juce::Colours::white.withAlpha(0.88f));
        if (button.getName() == "record")
        {
            g.setColour(active ? juce::Colours::white.withAlpha(0.94f) : juce::Colour(0xff9b2020));
            g.fillEllipse(icon.reduced(3.5f));
            g.setColour(active ? juce::Colours::black.withAlpha(0.35f)
                               : juce::Colours::white.withAlpha(0.55f));
            g.drawEllipse(icon.reduced(3.5f), 1.2f);
        }
        else if (button.getName() == "matchbpi")
        {
            juce::Path path;
            path.addCentredArc(icon.getCentreX(), icon.getCentreY(), icon.getWidth() * 0.34f, icon.getHeight() * 0.34f,
                               0.0f, -0.35f, juce::MathConstants<float>::pi + 0.25f, true);
            path.addCentredArc(icon.getCentreX(), icon.getCentreY(), icon.getWidth() * 0.34f, icon.getHeight() * 0.34f,
                               0.0f, juce::MathConstants<float>::pi - 0.1f, juce::MathConstants<float>::twoPi - 0.45f, true);
            g.strokePath(path, juce::PathStrokeType(1.8f));
            juce::Path headA;
            headA.addTriangle(icon.getRight() - 3.0f, icon.getCentreY() - 5.0f,
                              icon.getRight() - 3.0f, icon.getCentreY() + 1.0f,
                              icon.getRight() + 2.0f, icon.getCentreY() - 2.0f);
            g.fillPath(headA);
            juce::Path headB;
            headB.addTriangle(icon.getX() + 3.0f, icon.getCentreY() + 5.0f,
                              icon.getX() + 3.0f, icon.getCentreY() - 1.0f,
                              icon.getX() - 2.0f, icon.getCentreY() + 2.0f);
            g.fillPath(headB);
        }
        else if (button.getName() == "loop")
        {
            const float cx = icon.getCentreX();
            const float cy = icon.getCentreY();
            const float w = icon.getWidth();
            const float h = icon.getHeight();
            juce::Path infinity;
            infinity.startNewSubPath(cx, cy);
            infinity.cubicTo(cx - w * 0.16f, cy - h * 0.36f,
                             icon.getX(), cy - h * 0.36f,
                             icon.getX(), cy);
            infinity.cubicTo(icon.getX(), cy + h * 0.36f,
                             cx - w * 0.16f, cy + h * 0.36f,
                             cx, cy);
            infinity.cubicTo(cx + w * 0.16f, cy - h * 0.36f,
                             icon.getRight(), cy - h * 0.36f,
                             icon.getRight(), cy);
            infinity.cubicTo(icon.getRight(), cy + h * 0.36f,
                             cx + w * 0.16f, cy + h * 0.36f,
                             cx, cy);
            g.strokePath(infinity, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (button.getName() == "duckroute")
        {
            g.setFont(juce::Font(12.0f, juce::Font::bold));
            g.drawFittedText("D", icon.toNearestInt().expanded(1), juce::Justification::centred, 1);
        }
        else
        {
            juce::Path arrow;
            arrow.startNewSubPath(icon.getRight() - 1.0f, icon.getY() + 3.0f);
            arrow.quadraticTo(icon.getX() + 2.0f, icon.getCentreY(), icon.getRight() - 1.0f, icon.getBottom() - 3.0f);
            g.strokePath(arrow, juce::PathStrokeType(2.0f));
            juce::Path head;
            head.addTriangle(icon.getX() + 1.0f, icon.getCentreY(),
                             icon.getX() + 8.0f, icon.getCentreY() - 5.0f,
                             icon.getX() + 8.0f, icon.getCentreY() + 5.0f);
            g.fillPath(head);
        }
    }

    void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override {}
};

class SamplePadComponent : public juce::Component,
                           public juce::FileDragAndDropTarget,
                           private juce::Timer
{
public:
    using FxRouteDragCallback = std::function<void(int, juce::Point<int>, bool)>;

    SamplePadComponent(NinjamVst3AudioProcessor& p,
                       NinjamVst3AudioProcessorEditor& editorIn,
                       int padIndexIn,
                       FxRouteDragCallback fxRouteDragCallback = {})
        : processor(p),
          editor(editorIn),
          padIndex(padIndexIn),
          onFxRouteDrag(std::move(fxRouteDragCallback))
    {
        setRepaintsOnMouseActivity(true);

        nameLabel.setEditable(false, false);
        nameLabel.setJustificationType(juce::Justification::centredLeft);
        nameLabel.setFont(juce::Font(13.0f, juce::Font::bold));
        nameLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.94f));
        nameLabel.setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        nameLabel.setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
        nameLabel.setTooltip("Right-click to rename pad");
        nameLabel.setInterceptsMouseClicks(false, false);
        nameLabel.onTextChange = [this]
        {
            processor.setSamplePadName(padIndex, nameLabel.getText());
            refreshFromProcessor();
        };
        addAndMakeVisible(nameLabel);

        recordButton.setName("record");
        recordButton.setClickingTogglesState(true);
        recordButton.setTooltip("Arm loop recording");
        recordButton.setLookAndFeel(&toggleLookAndFeel);
        recordButton.onClick = [this]
        {
            processor.setSamplePadRecordArmed(padIndex, recordButton.getToggleState());
        };
        addAndMakeVisible(recordButton);

        matchBpiButton.setName("matchbpi");
        matchBpiButton.setClickingTogglesState(true);
        matchBpiButton.setTooltip("Match BPI start position");
        matchBpiButton.setLookAndFeel(&toggleLookAndFeel);
        matchBpiButton.onClick = [this]
        {
            processor.setSamplePadMatchBpiEnabled(padIndex, matchBpiButton.getToggleState());
        };
        addAndMakeVisible(matchBpiButton);

        loopButton.setName("loop");
        loopButton.setClickingTogglesState(true);
        loopButton.setTooltip("Loop");
        loopButton.setLookAndFeel(&toggleLookAndFeel);
        loopButton.onClick = [this]
        {
            processor.setSamplePadLoopEnabled(padIndex, loopButton.getToggleState());
        };
        addAndMakeVisible(loopButton);

        reverseButton.setName("reverse");
        reverseButton.setClickingTogglesState(true);
        reverseButton.setTooltip("Reverse");
        reverseButton.setLookAndFeel(&toggleLookAndFeel);
        reverseButton.onClick = [this]
        {
            processor.setSamplePadReverseEnabled(padIndex, reverseButton.getToggleState());
        };
        addAndMakeVisible(reverseButton);

        duckRouteButton.setName("duckroute");
        duckRouteButton.setButtonText("D");
        duckRouteButton.setClickingTogglesState(true);
        duckRouteButton.setTooltip("Route this pad through the sampler duck");
        duckRouteButton.setLookAndFeel(&toggleLookAndFeel);
        duckRouteButton.onClick = [this]
        {
            const bool enabled = duckRouteButton.getToggleState();
            processor.setSamplePadDuckRouteEnabled(padIndex, enabled);
            if (enabled)
                processor.setSamplePadDuckEnabled(true);
            refreshFromProcessor();
        };
        addAndMakeVisible(duckRouteButton);

        lastTriggerFlashCounter = processor.getSamplePadTriggerFlashCounter(padIndex);
        refreshFromProcessor();
    }

    ~SamplePadComponent() override
    {
        stopTimer();
        recordButton.setLookAndFeel(nullptr);
        matchBpiButton.setLookAndFeel(nullptr);
        loopButton.setLookAndFeel(nullptr);
        reverseButton.setLookAndFeel(nullptr);
        duckRouteButton.setLookAndFeel(nullptr);
    }

    void setResizeLightweightMode(bool shouldUse)
    {
        if (resizeLightweightMode == shouldUse)
            return;

        resizeLightweightMode = shouldUse;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        const bool loaded = cachedLoaded;
        const bool recording = cachedRecording;
        const bool playing = cachedPlaying;
        const bool waitingForBpiLoop = cachedWaitingForBpiLoop;
        const bool hover = isMouseOverOrDragging();

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const float phase = (float)(nowMs * 0.001 * 2.0f * juce::MathConstants<double>::pi * 1.2 + padIndex * 0.37);
        const float pulse = 0.5f + 0.5f * std::sin(phase);

        bool activeTint = false;
        juce::Colour activeTop;
        juce::Colour activeMid;
        juce::Colour activeGlow;
        if (recording)
        {
            activeTint = true;
            activeTop = juce::Colour(0xffdf4f4f);
            activeMid = juce::Colour(0xff7a2424);
            activeGlow = juce::Colour(0xffff5454);
        }
        else if (waitingForBpiLoop)
        {
            activeTint = true;
            activeTop = juce::Colour(0xffffad45);
            activeMid = juce::Colour(0xff8b4b12);
            activeGlow = juce::Colour(0xffff9f2f);
        }
        else if (playing)
        {
            activeTint = true;
            activeTop = juce::Colour(0xff62de72);
            activeMid = juce::Colour(0xff246d35);
            activeGlow = juce::Colour(0xff55f06f);
        }

        auto top = activeTint ? activeTop : (loaded ? juce::Colour(0xff8f969d) : juce::Colour(0xff70767d));
        auto mid = activeTint ? activeMid : (loaded ? juce::Colour(0xff555b61) : juce::Colour(0xff444a50));
        auto bottom = juce::Colour(0xff25292d);
        if (activeTint)
        {
            top = top.brighter(0.06f + 0.18f * pulse);
            mid = mid.brighter(0.02f + 0.12f * pulse);
            bottom = bottom.interpolatedWith(activeGlow, 0.10f + 0.15f * pulse);
        }
        if (hover)
        {
            top = top.brighter(0.12f);
            mid = mid.brighter(0.08f);
        }

        juce::ColourGradient rubber(top, bounds.getX(), bounds.getY(),
                                    bottom, bounds.getX(), bounds.getBottom(), false);
        rubber.addColour(0.45, mid);
        rubber.addColour(0.78, juce::Colour(0xff30353a));
        g.setGradientFill(rubber);
        g.fillRoundedRectangle(bounds, 8.0f);

        if (resizeLightweightMode)
        {
            const auto outline = activeTint ? activeGlow : (loaded ? juce::Colour(0xff82d9ff) : juce::Colours::black);
            g.setColour(outline.withAlpha(activeTint ? 0.78f : (loaded ? 0.62f : 0.80f)));
            g.drawRoundedRectangle(bounds, 8.0f, loaded ? 1.7f : 1.2f);
            return;
        }

        g.setColour(juce::Colours::white.withAlpha(0.06f));
        for (int y = 4; y < getHeight(); y += 7)
            g.drawHorizontalLine(y, bounds.getX() + 5.0f, bounds.getRight() - 5.0f);

        for (int y = 5; y < getHeight(); y += 8)
        {
            for (int x = 5; x < getWidth(); x += 8)
            {
                const int hash = (x * 37 + y * 17 + padIndex * 29) & 7;
                if (hash == 0)
                {
                    g.setColour(juce::Colours::black.withAlpha(0.08f));
                    g.fillRect(x, y, 1, 1);
                }
                else if (hash == 3)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.045f));
                    g.fillRect(x, y, 1, 1);
                }
            }
        }

        // Pulsing / outline logic: recording (red) > waiting for BPI loop (orange) > playing (green) > static
        float outlineThickness = loaded ? 1.8f : 1.2f;
        juce::Colour outlineColour;
        float outlineAlpha = 1.0f;

        if (recording)
        {
            outlineColour = juce::Colour(0xffff5454);
            outlineAlpha = 0.55f + 0.45f * pulse;
        }
        else if (waitingForBpiLoop)
        {
            outlineColour = juce::Colour(0xffffa040);
            outlineAlpha = 0.46f + 0.44f * pulse;
        }
        else if (playing)
        {
            outlineColour = juce::Colour(0xff7ef57e);
            outlineAlpha = 0.44f + 0.46f * pulse;
        }
        else
        {
            outlineColour = loaded ? juce::Colour(0xff82d9ff) : juce::Colours::black;
            outlineAlpha = loaded ? 0.75f : 0.86f;
        }

        g.setColour(outlineColour.withAlpha(outlineAlpha));
        g.drawRoundedRectangle(bounds, 8.0f, outlineThickness);

        if (activeTint)
        {
            g.setColour(activeGlow.withAlpha(0.08f + 0.18f * pulse));
            g.fillRoundedRectangle(bounds.reduced(3.0f), 7.0f);
        }

        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.fillRoundedRectangle(bounds.reduced(7.0f).withTrimmedTop(bounds.getHeight() * 0.52f), 5.0f);

        const double now = nowMs;
        if (hitGlowUntilMs > now)
        {
            const float amount = (float)juce::jlimit(0.0, 1.0, (hitGlowUntilMs - now) / hitGlowDurationMs);
            g.setColour(juce::Colour(0xff8fe7ff).withAlpha(0.28f * amount));
            g.fillRoundedRectangle(bounds.reduced(3.0f), 8.0f);
            g.setColour(juce::Colours::white.withAlpha(0.55f * amount));
            g.drawRoundedRectangle(bounds.reduced(2.0f), 8.0f, 2.2f);
        }

        const int loopBeats = cachedLoopBeats;
        const bool showRecordCountdown = waitingForBpiLoop && cachedRecordStartCountdownBeats > 0;
        if (loopBeats > 0 || showRecordCountdown)
        {
            const float progress = showRecordCountdown ? cachedRecordStartCountdownProgress : cachedLoopProgress;
            const float remaining = showRecordCountdown
                ? juce::jlimit(0.0f, 1.0f, 1.0f - progress)
                : (playing ? juce::jlimit(0.0f, 1.0f, 1.0f - progress) : 1.0f);
            auto indicator = juce::Rectangle<float>(bounds.getRight() - 58.0f,
                                                    bounds.getBottom() - 30.0f,
                                                    23.0f,
                                                    23.0f);
            const auto centre = indicator.getCentre();
            const float radius = juce::jmin(indicator.getWidth(), indicator.getHeight()) * 0.39f;

            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.fillEllipse(indicator);
            g.setColour(juce::Colours::white.withAlpha(0.18f));
            g.drawEllipse(indicator.reduced(1.0f), 1.1f);

            juce::Path remainingArc;
            const float startAngle = -juce::MathConstants<float>::halfPi;
            const float endAngle = startAngle + juce::MathConstants<float>::twoPi * remaining;
            remainingArc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
            const auto ringColour = showRecordCountdown ? juce::Colour(0xffff9f2f)
                                                         : (activeTint ? activeGlow : juce::Colour(0xff82d9ff));
            g.setColour(ringColour.withAlpha((playing || showRecordCountdown) ? 0.92f : 0.55f));
            g.strokePath(remainingArc, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const int displayBeats = showRecordCountdown ? cachedRecordStartCountdownBeats : loopBeats;
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.setFont(juce::Font(displayBeats >= 100 ? 7.0f : 8.5f, juce::Font::bold));
            g.drawFittedText(juce::String(displayBeats), indicator.toNearestInt().reduced(3), juce::Justification::centred, 1);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(7);
        auto top = area.removeFromTop(22);
        recordButton.setBounds(top.removeFromLeft(21).reduced(1));
        top.removeFromLeft(4);
        reverseButton.setBounds(top.removeFromRight(21).reduced(1));
        top.removeFromRight(3);
        loopButton.setBounds(top.removeFromRight(21).reduced(1));
        top.removeFromRight(3);
        matchBpiButton.setBounds(top.removeFromRight(21).reduced(1));

        auto bottom = area.removeFromBottom(22);
        duckRouteButton.setBounds(bottom.removeFromRight(21).reduced(1));
        bottom.removeFromRight(4);
        auto nameArea = bottom.reduced(2, 0);
        nameLabel.setBounds(nameArea);
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (const auto& path : files)
            if (isSupportedSamplePadFile(juce::File(path)))
                return true;
        return false;
    }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        for (const auto& path : files)
        {
            juce::File file(path);
            if (isSupportedSamplePadFile(file))
            {
                loadFile(file);
                return;
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            if (nameLabel.getBounds().contains(e.getPosition()))
            {
                nameLabel.setInterceptsMouseClicks(true, true);
                nameLabel.setEditable(true, false);
                nameLabel.showEditor();
                return;
            }

            showPadMenu(e.getScreenPosition());
            return;
        }

        if (e.mods.isLeftButtonDown())
        {
            if (processor.isSamplePadRecording(padIndex) || processor.isSamplePadWaitingForBpiLoop(padIndex))
            {
                processor.triggerSamplePad(padIndex);
                pulseHitGlow();
                repaint();
                mouseDownActive = false;
                return;
            }

            // start hold detection; short/long press handled on mouseUp / refreshFromProcessor
            mouseDownActive = true;
            mouseHoldActionTriggered = false;
            fxRouteDragActive = false;
            mouseDownAtMs = juce::Time::getMillisecondCounterHiRes();
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!mouseDownActive || !e.mods.isLeftButtonDown())
            return;

        if (!fxRouteDragActive)
        {
            if (e.getDistanceFromDragStart() < 14)
                return;

            fxRouteDragActive = true;
            mouseHoldActionTriggered = true;
        }

        if (onFxRouteDrag)
            onFxRouteDrag(padIndex, getParentPointForEvent(e), false);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            return;

        if (!mouseDownActive)
            return;

        if (fxRouteDragActive)
        {
            if (onFxRouteDrag)
                onFxRouteDrag(padIndex, getParentPointForEvent(e), true);
            mouseDownActive = false;
            mouseHoldActionTriggered = false;
            fxRouteDragActive = false;
            return;
        }

        if (mouseHoldActionTriggered)
        {
            mouseDownActive = false;
            mouseHoldActionTriggered = false;
            return;
        }

        const double now = juce::Time::getMillisecondCounterHiRes();
        const double heldMs = now - mouseDownAtMs;
        if (processor.isSamplePadRecording(padIndex) || processor.isSamplePadWaitingForBpiLoop(padIndex))
        {
            processor.triggerSamplePad(padIndex);
            pulseHitGlow();
            repaint();
            mouseDownActive = false;
            return;
        }

        if (heldMs >= holdToScheduleRecordMs)
        {
            processor.scheduleSamplePadBpiRecordStartAtNextInterval(padIndex);
            refreshFromProcessor();
        }
        else
        {
            // short click -> trigger pad
            processor.triggerSamplePad(padIndex);
            pulseHitGlow();
            repaint();
        }

        mouseDownActive = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown())
        {
            if (processor.getSamplePadLoopLengthBeats(padIndex) > 0)
            {
                processor.clearSamplePad(padIndex);
                refreshFromProcessor();
            }
        }
    }

    void refreshFromProcessor()
    {
        const auto nextName = processor.getSamplePadName(padIndex);
        if (!nameLabel.isBeingEdited() && nameLabel.getText() != nextName)
            nameLabel.setText(nextName, juce::dontSendNotification);
        // If the editor was closed, ensure single-click editing is disabled again
        if (!nameLabel.isBeingEdited() && nameLabel.isEditable())
        {
            nameLabel.setEditable(false, false);
            nameLabel.setInterceptsMouseClicks(false, false);
        }

        if (mouseDownActive && !mouseHoldActionTriggered)
        {
            if (juce::Time::getMillisecondCounterHiRes() - mouseDownAtMs >= holdToScheduleRecordMs)
            {
                processor.scheduleSamplePadBpiRecordStartAtNextInterval(padIndex);
                mouseHoldActionTriggered = true;
            }
        }

        const bool recordArmed = processor.isSamplePadRecordArmed(padIndex);
        const bool recording = processor.isSamplePadRecording(padIndex);
        const bool matchBpiEnabled = processor.isSamplePadMatchBpiEnabled(padIndex);
        const bool loopEnabled = processor.isSamplePadLoopEnabled(padIndex);
        const bool reverseEnabled = processor.isSamplePadReverseEnabled(padIndex);
        const bool duckRouteEnabled = processor.isSamplePadDuckRouteEnabled(padIndex);
        const bool loaded = processor.hasSamplePadSample(padIndex);
        const bool playing = processor.isSamplePadPlaying(padIndex);
        const bool waitingForBpiLoop = processor.isSamplePadWaitingForBpiLoop(padIndex);
        const int loopBeats = processor.getSamplePadLoopLengthBeats(padIndex);
        const float loopProgress = loopBeats > 0 ? processor.getSamplePadLoopProgress(padIndex) : 0.0f;
        const int recordStartCountdownBeats = waitingForBpiLoop ? processor.getSamplePadRecordStartCountdownBeats(padIndex) : 0;
        const float recordStartCountdownProgress = waitingForBpiLoop ? processor.getSamplePadRecordStartCountdownProgress(padIndex) : 0.0f;

        const bool shouldShowRecordToggle = recordArmed || recording;
        if (recordButton.getToggleState() != shouldShowRecordToggle)
            recordButton.setToggleState(shouldShowRecordToggle, juce::dontSendNotification);
        if (matchBpiButton.getToggleState() != matchBpiEnabled)
            matchBpiButton.setToggleState(matchBpiEnabled, juce::dontSendNotification);
        if (loopButton.getToggleState() != loopEnabled)
            loopButton.setToggleState(loopEnabled, juce::dontSendNotification);
        if (reverseButton.getToggleState() != reverseEnabled)
            reverseButton.setToggleState(reverseEnabled, juce::dontSendNotification);
        if (duckRouteButton.getToggleState() != duckRouteEnabled)
            duckRouteButton.setToggleState(duckRouteEnabled, juce::dontSendNotification);

        bool visualStateChanged = loaded != cachedLoaded
            || recording != cachedRecording
            || playing != cachedPlaying
            || waitingForBpiLoop != cachedWaitingForBpiLoop
            || loopBeats != cachedLoopBeats
            || recordStartCountdownBeats != cachedRecordStartCountdownBeats;
        if (std::abs(loopProgress - cachedLoopProgress) >= 0.01f
            || std::abs(recordStartCountdownProgress - cachedRecordStartCountdownProgress) >= 0.01f)
            visualStateChanged = true;

        cachedLoaded = loaded;
        cachedRecording = recording;
        cachedPlaying = playing;
        cachedWaitingForBpiLoop = waitingForBpiLoop;
        cachedLoopBeats = loopBeats;
        cachedLoopProgress = loopProgress;
        cachedRecordStartCountdownBeats = recordStartCountdownBeats;
        cachedRecordStartCountdownProgress = recordStartCountdownProgress;

        const int triggerFlashCounter = processor.getSamplePadTriggerFlashCounter(padIndex);
        if (triggerFlashCounter != lastTriggerFlashCounter)
        {
            lastTriggerFlashCounter = triggerFlashCounter;
            pulseHitGlow();
        }

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const bool animatedVisual = recording || playing || waitingForBpiLoop || hitGlowUntilMs > nowMs;
        if (visualStateChanged || animatedVisual)
            repaint();
    }

private:
    juce::Point<int> getParentPointForEvent(const juce::MouseEvent& e) const
    {
        if (auto* parent = getParentComponent())
            return parent->getLocalPoint(this, e.getPosition());

        return e.getPosition();
    }

    void showPadMenu(juce::Point<int> screenPosition)
    {
        juce::PopupMenu menu;
        const bool hasSample = processor.hasSamplePadSample(padIndex);
        menu.addItem(1, "Load Sample");
        menu.addItem(2, "Delete Sample", hasSample);
        menu.addItem(3, "Rename Pad");
        menu.addSeparator();
        menu.addItem(4, "Auto BPM Sync", hasSample, processor.isSamplePadBpmSyncEnabled(padIndex));
        juce::PopupMenu speedMenu;
        const auto currentSpeed = processor.getSamplePadPlaybackSpeed(padIndex);
        speedMenu.addItem(20, "Half Speed", hasSample, currentSpeed == NinjamVst3AudioProcessor::SamplePadPlaybackSpeed::half);
        speedMenu.addItem(21, "Normal Speed", hasSample, currentSpeed == NinjamVst3AudioProcessor::SamplePadPlaybackSpeed::normal);
        speedMenu.addItem(22, "Double Speed", hasSample, currentSpeed == NinjamVst3AudioProcessor::SamplePadPlaybackSpeed::doubleSpeed);
        menu.addSubMenu("Playback Speed", speedMenu, hasSample);
        menu.addItem(5, "Resync to NINJAM BPM", hasSample && processor.isSamplePadLoopEnabled(padIndex));
        menu.addItem(6, "Undo BPM Resync", processor.canUndoSamplePadBpmResync(padIndex));
        menu.addItem(7, "Sync to Beat", hasSample && processor.isSamplePadLoopEnabled(padIndex));
        menu.addItem(8, "Undo Clear", processor.canUndoSamplePadClear(padIndex));
        menu.addSeparator();
        juce::PopupMenu clearWireMenu;
        bool hasFxWire = false;
        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
        {
            if (processor.isSamplePadFxSlotRouteEnabled(padIndex, slot))
            {
                clearWireMenu.addItem(100 + slot, "Knob " + juce::String(slot + 1));
                hasFxWire = true;
            }
        }
        if (!hasFxWire)
            clearWireMenu.addItem(99, "No knob wires", false);
        menu.addSubMenu("Clear Wire", clearWireMenu);
        menu.addSeparator();
        menu.addItem(10, "MIDI Learn");
        menu.addItem(11, "MIDI Forget", editor.hasSamplePadMidiLearn(padIndex));
        menu.addSeparator();
        menu.addItem(12, "OSC Learn");
        menu.addItem(13, "OSC Forget", editor.hasSamplePadOscLearn(padIndex));

        juce::Component::SafePointer<SamplePadComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options()
                               .withTargetComponent(this)
                               .withTargetScreenArea({ screenPosition.x, screenPosition.y, 1, 1 }),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr)
                                   return;
                               if (result == 1)
                                   safeThis->openFileChooser();
                               else if (result == 2)
                               {
                                   safeThis->processor.clearSamplePad(safeThis->padIndex);
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result == 3)
                               {
                                   safeThis->nameLabel.setEditable(true, false);
                                   safeThis->nameLabel.showEditor();
                               }
                               else if (result == 4)
                               {
                                   safeThis->processor.setSamplePadBpmSyncEnabled(
                                       safeThis->padIndex,
                                       !safeThis->processor.isSamplePadBpmSyncEnabled(safeThis->padIndex));
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result == 5)
                               {
                                   safeThis->processor.requestSamplePadResyncToNinjamBpm(safeThis->padIndex, true);
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result == 6)
                               {
                                   safeThis->processor.undoSamplePadBpmResync(safeThis->padIndex);
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result == 7)
                               {
                                   safeThis->processor.syncSamplePadLoopToBeat(safeThis->padIndex);
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result == 8)
                               {
                                   safeThis->processor.undoSamplePadClear(safeThis->padIndex);
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result == 20 || result == 21 || result == 22)
                               {
                                   auto speed = NinjamVst3AudioProcessor::SamplePadPlaybackSpeed::normal;
                                   if (result == 20)
                                       speed = NinjamVst3AudioProcessor::SamplePadPlaybackSpeed::half;
                                   else if (result == 22)
                                       speed = NinjamVst3AudioProcessor::SamplePadPlaybackSpeed::doubleSpeed;
                                   safeThis->processor.setSamplePadPlaybackSpeed(safeThis->padIndex, speed);
                                   safeThis->refreshFromProcessor();
                               }
                               else if (result >= 100
                                        && result < 100 + NinjamVst3AudioProcessor::numSamplePadFxSlots)
                               {
                                   safeThis->processor.setSamplePadFxSlotRouteEnabled(safeThis->padIndex,
                                                                                      result - 100,
                                                                                      false);
                                   safeThis->refreshFromProcessor();
                                   if (auto* parent = safeThis->getParentComponent())
                                       parent->repaint();
                               }
                               else if (result == 10)
                                   safeThis->editor.armSamplePadMidiLearn(safeThis->padIndex);
                               else if (result == 11)
                                   safeThis->editor.forgetSamplePadMidiLearn(safeThis->padIndex);
                               else if (result == 12)
                                   safeThis->editor.armSamplePadOscLearn(safeThis->padIndex);
                               else if (result == 13)
                                   safeThis->editor.forgetSamplePadOscLearn(safeThis->padIndex);
                           });
    }

    void openFileChooser()
    {
        chooser = std::make_unique<juce::FileChooser>("Load sample",
                                                      juce::File(),
                                                      "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        juce::Component::SafePointer<SamplePadComponent> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safeThis](const juce::FileChooser& fc)
                             {
                                 if (safeThis == nullptr)
                                     return;

                                 const auto file = fc.getResult();
                                 if (file.existsAsFile())
                                     safeThis->loadFile(file);
                             });
    }

    void loadFile(const juce::File& file)
    {
        juce::Component::SafePointer<SamplePadComponent> safeThis(this);
        processor.loadSamplePadAsync(padIndex,
                                     file,
                                     [safeThis](bool loaded, const juce::String& error)
                                     {
                                         if (safeThis == nullptr)
                                             return;

                                         if (!loaded)
                                         {
                                             juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                    "Sample load failed",
                                                                                    error.isNotEmpty()
                                                                                        ? error
                                                                                        : juce::String("That sample could not be loaded."));
                                         }
                                         else
                                         {
                                             safeThis->processor.requestSamplePadResyncToNinjamBpm(safeThis->padIndex, true);
                                         }

                                         safeThis->refreshFromProcessor();
                                     });
        refreshFromProcessor();
    }
    void pulseHitGlow()
    {
        hitGlowUntilMs = juce::Time::getMillisecondCounterHiRes() + hitGlowDurationMs;
        // rely on parent component's timer (24Hz) to refresh UI; just request a repaint
        repaint();
    }

    void timerCallback() override
    {
        if (juce::Time::getMillisecondCounterHiRes() >= hitGlowUntilMs)
            stopTimer();
        repaint();
    }

    NinjamVst3AudioProcessor& processor;
    NinjamVst3AudioProcessorEditor& editor;
    int padIndex = 0;
    FxRouteDragCallback onFxRouteDrag;
    juce::Label nameLabel;
    LeftClickOnlyTextButton recordButton{ "" };
    LeftClickOnlyTextButton matchBpiButton{ "" };
    LeftClickOnlyTextButton loopButton{ "" };
    LeftClickOnlyTextButton reverseButton{ "" };
    LeftClickOnlyTextButton duckRouteButton{ "D" };
    SamplePadToggleLookAndFeel toggleLookAndFeel;
    std::unique_ptr<juce::FileChooser> chooser;
    static constexpr double hitGlowDurationMs = 240.0;
    static constexpr double holdToScheduleRecordMs = 2000.0;
    double hitGlowUntilMs = 0.0;
    int lastTriggerFlashCounter = 0;
    bool cachedLoaded = false;
    bool cachedRecording = false;
    bool cachedPlaying = false;
    bool cachedWaitingForBpiLoop = false;
    int cachedLoopBeats = 0;
    float cachedLoopProgress = 0.0f;
    int cachedRecordStartCountdownBeats = 0;
    float cachedRecordStartCountdownProgress = 0.0f;
    // Hold-to-arm state
    double mouseDownAtMs = 0.0;
    bool mouseDownActive = false;
    bool mouseHoldActionTriggered = false;
    bool fxRouteDragActive = false;
    bool resizeLightweightMode = false;
};

class SamplePadsComponent : public juce::Component,
                            private juce::Timer
{
public:
    SamplePadsComponent(NinjamVst3AudioProcessor& p,
                        NinjamVst3AudioProcessorEditor& editorIn,
                        bool abletonHostedWindow,
                        int samplerWindowSizePreset,
                        std::function<void()> onPadsMidiChangedCallback,
                        std::function<void(int)> onSamplerSizePresetChangedCallback)
        : processor(p),
          editor(editorIn),
          abletonHosted(abletonHostedWindow),
          currentSamplerSizePreset(juce::jlimit(0, 2, samplerWindowSizePreset)),
          onPadsMidiChanged(std::move(onPadsMidiChangedCallback)),
          onSamplerSizePresetChanged(std::move(onSamplerSizePresetChangedCallback))
    {
        padCurrentAlpha.fill(1.0f);
        padTargetAlpha.fill(1.0f);

        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            auto component = std::make_unique<SamplePadComponent>(processor,
                                                                  editor,
                                                                  pad,
                                                                  [this](int draggedPad, juce::Point<int> point, bool finished)
                                                                  {
                                                                      handleFxRouteDrag(draggedPad, point, finished);
                                                                  });
            addAndMakeVisible(component.get());
            pads[(size_t)pad] = std::move(component);
        }

        padsMidiLabel.setText("MIDI:", juce::dontSendNotification);
        padsMidiLabel.setJustificationType(juce::Justification::centredLeft);
        padsMidiLabel.setFont(juce::Font(12.0f, juce::Font::bold));
        padsMidiLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(padsMidiLabel);

        padsMidiSelector.setTooltip("Pads MIDI input");
        padsMidiSelector.setPopupMinimumWidth(210);
        padsMidiSelector.onPopupActiveChanged = [this](bool active)
        {
            samplerPopupActive = active;
        };
        addAndMakeVisible(padsMidiSelector);
        padsMidiSelector.onChange = [this]
        {
            const int selected = padsMidiSelector.getSelectedId();
            const auto it = padsMidiDeviceByMenuId.find(selected);
            processor.setSamplePadsMidiInputDeviceId(it != padsMidiDeviceByMenuId.end() ? it->second : juce::String());
            displayedPadsMidiDeviceId = processor.getSamplePadsMidiInputDeviceId();
            if (onPadsMidiChanged)
                onPadsMidiChanged();
        };

        looperInputLabel.setText("Input:", juce::dontSendNotification);
        looperInputLabel.setJustificationType(juce::Justification::centredLeft);
        looperInputLabel.setFont(juce::Font(12.0f, juce::Font::bold));
        looperInputLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(looperInputLabel);

        looperInputSelector.setTooltip("Looper recording input");
        looperInputSelector.setPopupMinimumWidth(210);
        looperInputSelector.onPopupActiveChanged = [this](bool active)
        {
            samplerPopupActive = active;
        };
        addAndMakeVisible(looperInputSelector);
        looperInputSelector.onChange = [this]
        {
            const int selected = looperInputSelector.getSelectedId();
            const auto it = looperInputByMenuId.find(selected);
            processor.setSamplePadLooperInput(it != looperInputByMenuId.end()
                ? it->second
                : NinjamVst3AudioProcessor::looperInputLocalChannel);
            displayedLooperInput = processor.getSamplePadLooperInput();
        };

        bankLabel.setText("Bank:", juce::dontSendNotification);
        bankLabel.setJustificationType(juce::Justification::centredLeft);
        bankLabel.setFont(juce::Font(12.0f, juce::Font::bold));
        bankLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(bankLabel);

        bankSelector.setTextWhenNothingSelected("Select bank");
        bankSelector.setTooltip("Saved sample pad banks");
        bankSelector.setPopupMinimumWidth(240);
        bankSelector.onPopupActiveChanged = [this](bool active)
        {
            samplerPopupActive = active;
        };
        addAndMakeVisible(bankSelector);

        for (int i = 0; i < 2; ++i)
        {
            auto& button = samplerSizeButtons[(size_t)i];
            button.setButtonText(i == 0 ? "-" : "+");
            button.setTooltip(i == 0 ? "Smaller sampler window" : "Larger sampler window");
            button.setVisible(abletonHosted);
            button.onClick = [this, i]
            {
                currentSamplerSizePreset = juce::jlimit(0, 2, currentSamplerSizePreset + (i == 0 ? -1 : 1));
                updateSamplerSizeButtons();
                if (onSamplerSizePresetChanged)
                    onSamplerSizePresetChanged(currentSamplerSizePreset);
            };
            addAndMakeVisible(button);
        }
        updateSamplerSizeButtons();

        loadBankButton.setButtonText("Load");
        loadBankButton.setTooltip("Load selected bank or choose a bank folder");
        loadBankButton.onClick = [this] { loadSelectedBank(); };
        addAndMakeVisible(loadBankButton);

        saveBankButton.setButtonText("Save");
        saveBankButton.setTooltip("Save current pads as a bank");
        saveBankButton.onClick = [this] { showSaveBankDialog(); };
        addAndMakeVisible(saveBankButton);

        resetSettingsButton.setButtonText("Reset");
        resetSettingsButton.setTooltip("Reset sampler settings without clearing pads");
        resetSettingsButton.onClick = [this]
        {
            processor.resetSamplePadSettings();
            refreshSampleFxControls();
            for (auto& pad : pads)
                if (pad != nullptr)
                    pad->refreshFromProcessor();
            repaint();
        };
        addAndMakeVisible(resetSettingsButton);

        clearPadsButton.setButtonText("Clear");
        clearPadsButton.setTooltip("Clear all sample pads");
        clearPadsButton.onClick = [this]
        {
            processor.clearAllSamplePads();
            refreshSampleFxControls();
            for (auto& pad : pads)
                if (pad != nullptr)
                    pad->refreshFromProcessor();
            repaint();
        };
        addAndMakeVisible(clearPadsButton);

        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
        {
            auto& knob = sampleFxKnobs[(size_t)slot];
            knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
            knob.setRange(0.0, 1.0, 0.001);
            knob.setDoubleClickReturnValue(true, 0.0);
            knob.setLookAndFeel(&editor.getSamplerKnobLookAndFeel());
            knob.getProperties().set("disableImageKnob", true);
            knob.getProperties().set("forceGreyKnob", true);
            knob.setTooltip("Sampler FX amount " + juce::String(slot + 1));
            knob.setValue(processor.getSamplePadFxSlotAmount(slot), juce::dontSendNotification);
            knob.onControlInteraction = [this]
            {
                noteSampleFxKnobEdited();
            };
            knob.onValueChange = [this, slot]
            {
                noteSampleFxKnobEdited();
                processor.setSamplePadFxSlotAmount(slot, (float)sampleFxKnobs[(size_t)slot].getValue());
            };
            knob.onPopupMenuRequest = [this, slot](const juce::MouseEvent& e)
            {
                editor.showMidiLearnMenuForComponent(sampleFxKnobs[(size_t)slot], e.getScreenPosition());
            };
            knob.onRouteMouseDown = [this, slot](const juce::MouseEvent& e)
            {
                handleFxChainRouteDrag(slot, getLocalPoint(&sampleFxKnobs[(size_t)slot], e.getPosition()), false);
            };
            knob.onRouteMouseDrag = [this, slot](const juce::MouseEvent& e)
            {
                handleFxChainRouteDrag(slot, getLocalPoint(&sampleFxKnobs[(size_t)slot], e.getPosition()), false);
            };
            knob.onRouteMouseUp = [this, slot](const juce::MouseEvent& e)
            {
                handleFxChainRouteDrag(slot, getLocalPoint(&sampleFxKnobs[(size_t)slot], e.getPosition()), true);
            };
            addAndMakeVisible(knob);
            editor.registerSamplerFxKnobLearnTarget(knob, slot);

            auto& selector = sampleFxSelectors[(size_t)slot];
            selector.setPopupMinimumWidth(220);
            selector.setTooltip("Sampler FX type " + juce::String(slot + 1));
            selector.onPopupActiveChanged = [this](bool active)
            {
                samplerPopupActive = active;
            };
            populateSampleFxSelector(selector, processor.getSamplePadFxSlotType(slot));
            selector.onChange = [this, slot]
            {
                processor.setSamplePadFxSlotType(slot,
                                                 samplePadFxTypeForMenuId(sampleFxSelectors[(size_t)slot].getSelectedId()));
            };
            addAndMakeVisible(selector);
        }

        refreshQuickSelectors(true);
        refreshBankList({});

        volumeSlider.setSliderStyle(juce::Slider::LinearVertical);
        volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setRange(0.0, 2.0, 0.001);
        volumeSlider.setSkewFactorFromMidPoint(1.0);
        volumeSlider.setDoubleClickReturnValue(true, 1.0);
        volumeSlider.setValue(processor.getSamplePadVolume(), juce::dontSendNotification);
        volumeSlider.setLookAndFeel(&faderLookAndFeel);
        volumeSlider.setTooltip("Sample pad volume");
        volumeSlider.onValueChange = [this]
        {
            processor.setSamplePadVolume((float)volumeSlider.getValue());
        };
        addAndMakeVisible(volumeSlider);

        volumeLabel.setText("Vol", juce::dontSendNotification);
        volumeLabel.setJustificationType(juce::Justification::centred);
        volumeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible(volumeLabel);

        addAndMakeVisible(peakMeter);

        limiterButton.setClickingTogglesState(true);
        limiterButton.setButtonText("Limiter");
        limiterButton.setToggleState(processor.isSamplePadLimiterEnabled(), juce::dontSendNotification);
        limiterButton.setTooltip("Limit sample pads to -2 dB");
        limiterButton.onClick = [this]
        {
            processor.setSamplePadLimiterEnabled(limiterButton.getToggleState());
            updateLimiterButtonColour();
        };
        addAndMakeVisible(limiterButton);
        updateLimiterButtonColour();

        duckButton.setClickingTogglesState(true);
        duckButton.setButtonText("Duck");
        duckButton.setToggleState(processor.isSamplePadDuckEnabled(), juce::dontSendNotification);
        duckButton.setTooltip("Tempo-synced sampler duck before the sampler limiter");
        duckButton.onClick = [this]
        {
            processor.setSamplePadDuckEnabled(duckButton.getToggleState());
            updateDuckButtonColour();
        };
        duckButton.onPopupMenuRequest = [this](const juce::MouseEvent& e)
        {
            showDuckMenu(e.getScreenPosition());
        };
        addAndMakeVisible(duckButton);
        updateDuckButtonColour();

        defaultFxButton.setClickingTogglesState(true);
        defaultFxButton.setButtonText("NJ FX");
        defaultFxButton.setToggleState(processor.getSamplePadsUseDefaultFx(), juce::dontSendNotification);
        defaultFxButton.setTooltip("Route sample pads into the main NINJAM reverb/delay sends");
        defaultFxButton.onClick = [this]
        {
            processor.setSamplePadsUseDefaultFx(defaultFxButton.getToggleState());
            updateDefaultFxButtonColour();
        };
        addAndMakeVisible(defaultFxButton);
        updateDefaultFxButtonColour();
        monitorButton.setClickingTogglesState(true);
        monitorButton.setButtonText("Monitor");
        monitorButton.setToggleState(processor.isSamplePadMonitorModeEnabled(), juce::dontSendNotification);
        monitorButton.setTooltip("Private monitor for newly triggered pads");
        monitorButton.onClick = [this]
        {
            processor.setSamplePadMonitorModeEnabled(monitorButton.getToggleState());
            updateMonitorButtonColour();
            refreshSampleFxControls();
        };
        addAndMakeVisible(monitorButton);
        updateMonitorButtonColour();


        startTimerHz(24);
    }

    ~SamplePadsComponent() override
    {
        stopTimer();
        volumeSlider.setLookAndFeel(nullptr);
        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
        {
            auto& knob = sampleFxKnobs[(size_t)slot];
            editor.unregisterSamplerFxKnobLearnTarget(knob, slot);
            knob.setLookAndFeel(nullptr);
        }
    }

    void paint(juce::Graphics& g) override
    {
        juce::ColourGradient bg(juce::Colour(0xff171a1d), 0.0f, 0.0f,
                                juce::Colour(0xff090b0d), 0.0f, (float)getHeight(), false);
        bg.addColour(0.45, juce::Colour(0xff202428));
        g.setGradientFill(bg);
        g.fillAll();

        if (!isResizeRefreshSuppressed(juce::Time::getMillisecondCounterHiRes()))
            drawFxRouteLines(g);

        auto frame = getLocalBounds().toFloat().reduced(6.0f);
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawRoundedRectangle(frame, 7.0f, 1.0f);
    }

    void resized() override
    {
        samplerResizeSuppressUntilMs = juce::Time::getMillisecondCounterHiRes() + samplerResizeRefreshSuppressMs;
        setPadsResizeLightweightMode(true);

        auto area = getLocalBounds().reduced(14);
        auto controls = area.removeFromRight(104);
        area.removeFromRight(12);

        volumeLabel.setBounds(controls.removeFromTop(22));
        auto duckArea = controls.removeFromBottom(30);
        duckButton.setBounds(duckArea.reduced(0, 2));
        controls.removeFromBottom(4);
        auto defaultFxArea = controls.removeFromBottom(30);
        defaultFxButton.setBounds(defaultFxArea.reduced(0, 2));
        controls.removeFromBottom(4);
        auto monitorArea = controls.removeFromBottom(30);
        monitorButton.setBounds(monitorArea.reduced(0, 2));
        controls.removeFromBottom(4);
        auto limiterArea = controls.removeFromBottom(30);
        limiterButton.setBounds(limiterArea.reduced(0, 2));
        controls.removeFromBottom(8);

        auto meterArea = controls.removeFromRight(24);
        peakMeter.setBounds(meterArea.reduced(2, 8));
        volumeSlider.setBounds(controls.reduced(6, 2));

        auto bankRow = area.removeFromTop(30);
        area.removeFromTop(8);
        clearPadsButton.setBounds(bankRow.removeFromRight(48).reduced(2, 1));
        resetSettingsButton.setBounds(bankRow.removeFromRight(54).reduced(2, 1));
        saveBankButton.setBounds(bankRow.removeFromRight(50).reduced(2, 1));
        loadBankButton.setBounds(bankRow.removeFromRight(50).reduced(2, 1));
        if (abletonHosted)
        {
            bankRow.removeFromRight(4);
            for (int i = 1; i >= 0; --i)
                samplerSizeButtons[(size_t)i].setBounds(bankRow.removeFromRight(28).reduced(2, 1));
        }
        bankRow.removeFromRight(4);

        const int availableHeaderWidth = bankRow.getWidth();
        const int midiComboWidth = juce::jlimit(78, 112, availableHeaderWidth / 5);
        const int inputComboWidth = juce::jlimit(70, 94, availableHeaderWidth / 6);
        padsMidiLabel.setBounds(bankRow.removeFromLeft(50));
        padsMidiSelector.setBounds(bankRow.removeFromLeft(midiComboWidth).reduced(2, 1));
        bankRow.removeFromLeft(6);
        looperInputLabel.setBounds(bankRow.removeFromLeft(54));
        looperInputSelector.setBounds(bankRow.removeFromLeft(inputComboWidth).reduced(2, 1));
        bankRow.removeFromLeft(10);
        bankLabel.setBounds(bankRow.removeFromLeft(50));
        const int bankSelectorWidth = juce::jmin(bankRow.getWidth(), abletonHosted ? 104 : 124);
        bankSelector.setBounds(bankRow.removeFromLeft(bankSelectorWidth).reduced(2, 1));


        auto fxArea = area.removeFromLeft(250);
        area.removeFromLeft(12);
        layoutSampleFxRack(fxArea);

        const int cols = 4;
        const int rows = (NinjamVst3AudioProcessor::numSamplePads + cols - 1) / cols;
        const int gap = 10;
        const int padW = juce::jmax(76, (area.getWidth() - gap * (cols - 1)) / cols);
        const int padH = juce::jmax(74, (area.getHeight() - gap * (rows - 1)) / rows);
        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            const int rowFromBottom = pad / cols;
            const int row = rows - 1 - rowFromBottom;
            const int col = pad % cols;
            juce::Rectangle<int> padBounds(area.getX() + col * (padW + gap),
                                           area.getY() + row * (padH + gap),
                                           padW,
                                           padH);
            pads[(size_t)pad]->setBounds(padBounds);
        }
    }

private:
    bool isResizeRefreshSuppressed(double nowMs) const
    {
        return nowMs < samplerResizeSuppressUntilMs;
    }

    void setPadsResizeLightweightMode(bool shouldUse)
    {
        if (padsResizeLightweightMode == shouldUse)
            return;

        padsResizeLightweightMode = shouldUse;
        for (auto& pad : pads)
            if (pad != nullptr)
                pad->setResizeLightweightMode(shouldUse);
    }

    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (isResizeRefreshSuppressed(now))
            return;

        if (padsResizeLightweightMode)
        {
            setPadsResizeLightweightMode(false);
            repaint();
        }

        const bool canDoHeavyRefresh = !samplerPopupActive;

        peakMeter.setPeak(processor.getSamplePadPeak());
        if (canDoHeavyRefresh && (now - lastQuickSelectorRefreshMs) >= quickSelectorRefreshIntervalMs)
        {
            refreshQuickSelectors(false);
            lastQuickSelectorRefreshMs = now;
        }

        const bool samplerControlIsBusy = samplerPopupActive
            || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
        if (samplerControlIsBusy && !fxRouteDragActive && !fxChainRouteDragActive)
        {
            resetRouteFocusAlpha(true);
        }
        else
        {
            updateHoveredFxKnobSlot();
            updatePadAlphaFade();
        }

        if (canDoHeavyRefresh)
        {
            for (auto& pad : pads)
                if (pad != nullptr)
                    pad->refreshFromProcessor();
        }
    }

    static int samplePadFxMenuIdForType(NinjamVst3AudioProcessor::SamplePadFxType type)
    {
        using FxType = NinjamVst3AudioProcessor::SamplePadFxType;
        switch (type)
        {
            case FxType::reverb: return 1;
            case FxType::delay: return 2;
            case FxType::delayQuarter: return 3;
            case FxType::delayQuarterPingPong: return 4;
            case FxType::djFilter: return 5;
            case FxType::djFilterHp: return 6;
            case FxType::djFilterLp: return 7;
            case FxType::djFilterBp: return 8;
            case FxType::phaser: return 9;
            case FxType::phaserHalf: return 10;
        }

        return 1;
    }

    static NinjamVst3AudioProcessor::SamplePadFxType samplePadFxTypeForMenuId(int menuId)
    {
        using FxType = NinjamVst3AudioProcessor::SamplePadFxType;
        switch (menuId)
        {
            case 2: return FxType::delay;
            case 3: return FxType::delayQuarter;
            case 4: return FxType::delayQuarterPingPong;
            case 5: return FxType::djFilter;
            case 6: return FxType::djFilterHp;
            case 7: return FxType::djFilterLp;
            case 8: return FxType::djFilterBp;
            case 9: return FxType::phaser;
            case 10: return FxType::phaserHalf;
            case 1:
            default:
                return FxType::reverb;
        }
    }

    static void populateSampleFxSelector(WidePopupComboBox& selector,
                                         NinjamVst3AudioProcessor::SamplePadFxType selectedType)
    {
        selector.clear(juce::dontSendNotification);
        selector.addItem("Reverb", 1);
        selector.addItem("Delay 1/8", 2);
        selector.addItem("Delay 1/4", 3);
        selector.addItem("Delay 1/4 Ping Pong", 4);
        selector.addItem("DJ Filter LP+HP", 5);
        selector.addItem("DJ Filter HP", 6);
        selector.addItem("DJ Filter LP", 7);
        selector.addItem("DJ Filter BP", 8);
        selector.addItem("Phaser 1/4", 9);
        selector.addItem("Phaser 1/2", 10);
        selector.setSelectedId(samplePadFxMenuIdForType(selectedType), juce::dontSendNotification);
    }

    static juce::Colour sampleFxSlotColour(int slot)
    {
        static constexpr juce::uint32 colours[] =
        {
            0xff5ec8ff, 0xffffc14f, 0xff7cf27c, 0xffff6ea8,
            0xffb486ff, 0xffff895c, 0xff56e0c2, 0xffe4ef6a
        };

        return juce::Colour(colours[(size_t)juce::jlimit(0, 7, slot)]);
    }

    int findFxSlotAtPoint(juce::Point<int> point) const
    {
        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
        {
            if (sampleFxSlotBounds[(size_t)slot].contains(point)
                || sampleFxKnobs[(size_t)slot].getBounds().contains(point)
                || sampleFxSelectors[(size_t)slot].getBounds().contains(point))
                return slot;
        }

        return -1;
    }

    int findHoveredFxKnobSlot() const
    {
        if (!isMouseOver(true))
            return -1;
        if (fxChainRouteDragActive)
            return -1;
        if (samplerPopupActive)
            return -1;
        if (juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
            return -1;
        if (juce::Time::getMillisecondCounterHiRes() < suppressRouteFocusUntilMs)
            return -1;

        const auto point = getMouseXYRelative();
        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
            if (sampleFxKnobs[(size_t)slot].getBounds().contains(point))
                return slot;

        return -1;
    }

    void setHoveredFxKnobSlot(int nextSlot)
    {
        if (hoveredFxSlot == nextSlot)
            return;

        hoveredFxSlot = nextSlot;
        const bool focusingRoutes = hoveredFxSlot >= 0;
        bool targetChanged = false;
        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            auto* padComponent = pads[(size_t)pad].get();
            if (padComponent == nullptr)
                continue;

            if (!focusingRoutes)
            {
                targetChanged = setPadTargetAlpha(pad, 1.0f) || targetChanged;
                continue;
            }

            const bool routedToHoveredSlot = processor.isSamplePadFxSlotRouteEnabled(pad, hoveredFxSlot);
            targetChanged = setPadTargetAlpha(pad, routedToHoveredSlot ? 0.62f : 0.36f) || targetChanged;
        }

        if (targetChanged)
            repaintPadAlphaAreas();
    }

    void updateHoveredFxKnobSlot()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const int nextCandidate = findHoveredFxKnobSlot();

        if (nextCandidate != hoverCandidateFxSlot)
        {
            hoverCandidateFxSlot = nextCandidate;
            hoverCandidateSinceMs = now;
            if (hoveredFxSlot >= 0 && hoveredFxSlot != nextCandidate)
                setHoveredFxKnobSlot(-1);
            return;
        }

        if (nextCandidate < 0)
        {
            setHoveredFxKnobSlot(-1);
            return;
        }

        if (hoveredFxSlot != nextCandidate
            && now - hoverCandidateSinceMs >= routeFocusHoverDelayMs)
        {
            setHoveredFxKnobSlot(nextCandidate);
        }
    }

    void noteSampleFxKnobEdited()
    {
        suppressRouteFocusUntilMs = juce::Time::getMillisecondCounterHiRes() + routeFocusSuppressAfterEditMs;
        resetRouteFocusAlpha(true);
    }

    bool setPadTargetAlpha(int pad, float target)
    {
        target = juce::jlimit(0.0f, 1.0f, target);
        auto& currentTarget = padTargetAlpha[(size_t)pad];
        if (std::abs(currentTarget - target) <= 0.001f)
            return false;

        currentTarget = target;
        return true;
    }

    void repaintPadAlphaAreas()
    {
        for (const auto& pad : pads)
            if (pad != nullptr)
                repaint(pad->getBounds().expanded(3));
    }

    void resetRouteFocusAlpha(bool snapVisible)
    {
        hoverCandidateFxSlot = -1;
        hoverCandidateSinceMs = 0.0;
        hoveredFxSlot = -1;

        bool changed = false;
        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
            changed = setPadTargetAlpha(pad, 1.0f) || changed;

        if (snapVisible)
            changed = snapPadAlphaToTargets() || changed;

        if (changed)
            repaintPadAlphaAreas();
    }

    bool snapPadAlphaToTargets()
    {
        bool changed = false;
        lastPadAlphaUpdateMs = juce::Time::getMillisecondCounterHiRes();

        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            auto* padComponent = pads[(size_t)pad].get();
            if (padComponent == nullptr)
                continue;

            auto& current = padCurrentAlpha[(size_t)pad];
            const float target = padTargetAlpha[(size_t)pad];
            if (std::abs(current - target) <= 0.001f
                && std::abs(padComponent->getAlpha() - target) <= 0.001f)
                continue;

            current = target;
            padComponent->setAlpha(target);
            changed = true;
        }

        return changed;
    }

    void updatePadAlphaFade()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double previous = lastPadAlphaUpdateMs > 0.0 ? lastPadAlphaUpdateMs : now;
        lastPadAlphaUpdateMs = now;
        const float blend = (float)(1.0 - std::exp(-(now - previous) / padAlphaFadeMs));

        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            auto* padComponent = pads[(size_t)pad].get();
            if (padComponent == nullptr)
                continue;

            auto& current = padCurrentAlpha[(size_t)pad];
            const float target = padTargetAlpha[(size_t)pad];
            if (std::abs(current - target) <= 0.001f)
            {
                current = target;
            }
            else
            {
                current += (target - current) * juce::jlimit(0.0f, 1.0f, blend);
            }

            if (std::abs(padComponent->getAlpha() - current) > 0.001f)
            {
                padComponent->setAlpha(current);
                repaint(padComponent->getBounds().expanded(3));
            }
        }
    }

    void handleFxRouteDrag(int padIndex, juce::Point<int> point, bool finished)
    {
        if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
            return;

        if (finished)
        {
            const int targetSlot = findFxSlotAtPoint(point);
            if (targetSlot >= 0)
                processor.setSamplePadFxSlotRouteEnabled(padIndex, targetSlot, true);

            fxRouteDragActive = false;
            fxRouteDragPad = -1;
            fxRouteDragPoint = {};
            highlightedFxSlot = -1;
            repaint();
            return;
        }

        fxRouteDragActive = true;
        fxRouteDragPad = padIndex;
        fxRouteDragPoint = point;
        highlightedFxSlot = findFxSlotAtPoint(point);
        repaint();
    }

    bool isValidFxChainRouteTarget(int sourceSlot, int targetSlot) const
    {
        return targetSlot >= 0
            && targetSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots
            && processor.canRouteSamplePadFxSlotToSlot(sourceSlot, targetSlot);
    }

    void handleFxChainRouteDrag(int sourceSlot, juce::Point<int> point, bool finished)
    {
        if (sourceSlot < 0 || sourceSlot >= NinjamVst3AudioProcessor::numSamplePadFxSlots)
            return;

        if (finished)
        {
            const int targetSlot = findFxSlotAtPoint(point);
            if (isValidFxChainRouteTarget(sourceSlot, targetSlot))
                processor.setSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot, true);

            fxChainRouteDragActive = false;
            fxChainRouteSourceSlot = -1;
            fxChainRouteDragPoint = {};
            highlightedFxChainTargetSlot = -1;
            repaint();
            return;
        }

        fxChainRouteDragActive = true;
        fxChainRouteSourceSlot = sourceSlot;
        fxChainRouteDragPoint = point;
        const int targetSlot = findFxSlotAtPoint(point);
        highlightedFxChainTargetSlot = isValidFxChainRouteTarget(sourceSlot, targetSlot) ? targetSlot : -1;
        repaint();
    }

    void drawCable(juce::Graphics& g,
                   juce::Point<float> start,
                   juce::Point<float> end,
                   juce::Colour colour,
                   float alpha,
                   float thickness,
                   bool drawArrow = false) const
    {
        juce::Path path;
        path.startNewSubPath(start);
        const float controlOffset = juce::jlimit(40.0f, 180.0f, std::abs(end.x - start.x) * 0.45f);
        path.cubicTo(start.x - controlOffset, start.y,
                     end.x + controlOffset, end.y,
                     end.x, end.y);

        g.setColour(colour.withAlpha(alpha * 0.22f));
        g.strokePath(path, juce::PathStrokeType(thickness + 4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(juce::Colours::black.withAlpha(alpha * 0.35f));
        g.strokePath(path, juce::PathStrokeType(thickness + 1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour(colour.withAlpha(alpha));
        g.strokePath(path, juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (drawArrow)
            drawCableArrow(g, start, end, colour, alpha);
    }

    void drawCableArrow(juce::Graphics& g,
                        juce::Point<float> start,
                        juce::Point<float> end,
                        juce::Colour colour,
                        float alpha) const
    {
        auto direction = end - start;
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length < 1.0f)
            return;

        direction.x /= length;
        direction.y /= length;
        const auto perpendicular = juce::Point<float>(-direction.y, direction.x);
        const auto tip = end - direction * 8.0f;
        const auto base = tip - direction * 11.0f;

        juce::Path arrow;
        arrow.addTriangle(tip,
                          base + perpendicular * 5.0f,
                          base - perpendicular * 5.0f);
        g.setColour(juce::Colours::black.withAlpha(alpha * 0.42f));
        g.fillPath(arrow, juce::AffineTransform::translation(1.0f, 1.0f));
        g.setColour(colour.withAlpha(alpha));
        g.fillPath(arrow);
    }

    void drawFxRouteLines(juce::Graphics& g) const
    {
        for (int sourceSlot = 0; sourceSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++sourceSlot)
        {
            const auto sourceBounds = sampleFxKnobs[(size_t)sourceSlot].getBounds().toFloat();
            if (sourceBounds.isEmpty())
                continue;

            const auto start = sourceBounds.getCentre();
            for (int targetSlot = 0; targetSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++targetSlot)
            {
                if (!processor.isSamplePadFxSlotToSlotRouteEnabled(sourceSlot, targetSlot))
                    continue;

                const auto targetBounds = sampleFxKnobs[(size_t)targetSlot].getBounds().toFloat();
                if (targetBounds.isEmpty())
                    continue;

                drawCable(g,
                          start,
                          targetBounds.getCentre(),
                          sampleFxSlotColour(sourceSlot).interpolatedWith(sampleFxSlotColour(targetSlot), 0.35f),
                          0.62f,
                          2.2f,
                          true);
            }
        }

        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            if (pads[(size_t)pad] == nullptr)
                continue;

            const auto padBounds = pads[(size_t)pad]->getBounds().toFloat();
            const auto start = juce::Point<float>(padBounds.getX() + 8.0f, padBounds.getCentreY());
            for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
            {
                if (!processor.isSamplePadFxSlotRouteEnabled(pad, slot))
                    continue;

                const auto knobBounds = sampleFxKnobs[(size_t)slot].getBounds().toFloat();
                if (knobBounds.isEmpty())
                    continue;

                const auto end = juce::Point<float>(knobBounds.getRight() - 3.0f, knobBounds.getCentreY());
                drawCable(g, start, end, sampleFxSlotColour(slot), 0.48f, 2.0f);
            }
        }

        if (fxRouteDragActive
            && fxRouteDragPad >= 0
            && fxRouteDragPad < NinjamVst3AudioProcessor::numSamplePads
            && pads[(size_t)fxRouteDragPad] != nullptr)
        {
            const auto padBounds = pads[(size_t)fxRouteDragPad]->getBounds().toFloat();
            const auto start = juce::Point<float>(padBounds.getX() + 8.0f, padBounds.getCentreY());
            const auto end = fxRouteDragPoint.toFloat();
            const auto colour = highlightedFxSlot >= 0 ? sampleFxSlotColour(highlightedFxSlot)
                                                       : juce::Colour(0xff8fe7ff);
            drawCable(g, start, end, colour, highlightedFxSlot >= 0 ? 0.9f : 0.58f, highlightedFxSlot >= 0 ? 3.0f : 2.2f);
        }

        if (fxChainRouteDragActive
            && fxChainRouteSourceSlot >= 0
            && fxChainRouteSourceSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots)
        {
            const auto sourceBounds = sampleFxKnobs[(size_t)fxChainRouteSourceSlot].getBounds().toFloat();
            if (!sourceBounds.isEmpty())
            {
                const auto colour = highlightedFxChainTargetSlot >= 0
                    ? sampleFxSlotColour(fxChainRouteSourceSlot).interpolatedWith(sampleFxSlotColour(highlightedFxChainTargetSlot), 0.35f)
                    : sampleFxSlotColour(fxChainRouteSourceSlot);
                drawCable(g,
                          sourceBounds.getCentre(),
                          fxChainRouteDragPoint.toFloat(),
                          colour,
                          highlightedFxChainTargetSlot >= 0 ? 0.92f : 0.48f,
                          highlightedFxChainTargetSlot >= 0 ? 3.0f : 2.1f,
                          true);
            }
        }
    }

    void refreshSampleFxControls()
    {
        if (!volumeSlider.isMouseButtonDown())
            volumeSlider.setValue(processor.getSamplePadVolume(), juce::dontSendNotification);

        limiterButton.setToggleState(processor.isSamplePadLimiterEnabled(), juce::dontSendNotification);
        updateLimiterButtonColour();

        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
        {
            auto& knob = sampleFxKnobs[(size_t)slot];
            if (!knob.isMouseButtonDown())
                knob.setValue(processor.getSamplePadFxSlotAmount(slot), juce::dontSendNotification);

            auto& selector = sampleFxSelectors[(size_t)slot];
            const int targetId = samplePadFxMenuIdForType(processor.getSamplePadFxSlotType(slot));
            if (!samplerPopupActive && selector.getSelectedId() != targetId)
                selector.setSelectedId(targetId, juce::dontSendNotification);

            const bool lockActiveSlot = processor.isSamplePadMonitorModeEnabled()
                && processor.isSamplePadFxSlotInUseForLocalRoute(slot);
            knob.setEnabled(!lockActiveSlot);
            selector.setEnabled(!lockActiveSlot);
            const auto slotTooltip = lockActiveSlot
                ? juce::String("In use by a playing normal pad")
                : juce::String();
            knob.setTooltip(slotTooltip);
            selector.setTooltip(slotTooltip);
        }

        duckButton.setToggleState(processor.isSamplePadDuckEnabled(), juce::dontSendNotification);
        updateDuckButtonColour();
        defaultFxButton.setToggleState(processor.getSamplePadsUseDefaultFx(), juce::dontSendNotification);
        updateDefaultFxButtonColour();
        monitorButton.setToggleState(processor.isSamplePadMonitorModeEnabled(), juce::dontSendNotification);
        updateMonitorButtonColour();
    }

    void layoutSampleFxRack(juce::Rectangle<int> fxArea)
    {
        fxArea = fxArea.reduced(2);
        const int cols = 2;
        const int rows = 4;
        const int gap = 6;
        const int cellW = (fxArea.getWidth() - gap) / cols;
        const int cellH = (fxArea.getHeight() - gap * (rows - 1)) / rows;

        for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
        {
            const int row = slot / cols;
            const int col = slot % cols;
            juce::Rectangle<int> cell(fxArea.getX() + col * (cellW + gap),
                                      fxArea.getY() + row * (cellH + gap),
                                      cellW,
                                      cellH);
            sampleFxSlotBounds[(size_t)slot] = cell;

            auto selectorArea = cell.removeFromBottom(22);
            auto knobArea = cell.reduced(4, 1);
            const int knobSize = juce::jmin(knobArea.getWidth(), knobArea.getHeight());
            sampleFxKnobs[(size_t)slot].setBounds(knobArea.withSizeKeepingCentre(knobSize, knobSize));
            sampleFxSelectors[(size_t)slot].setBounds(selectorArea.reduced(0, 1));
        }
    }

    static void populatePadsMidiSelector(juce::ComboBox& selector,
                                         std::map<int, juce::String>& idByMenuId,
                                         const juce::String& selectedDeviceId)
    {
        selector.clear(juce::dontSendNotification);
        idByMenuId.clear();

        int menuId = 1;
        selector.addItem("Host / Any", menuId);
        idByMenuId[menuId] = {};
        int selectedMenuId = selectedDeviceId.isEmpty() ? menuId : 0;
        ++menuId;

        selector.addItem("Remote MIDI Relay", menuId);
        idByMenuId[menuId] = NinjamVst3AudioProcessor::samplePadsMidiInputRelayId;
        if (selectedDeviceId == NinjamVst3AudioProcessor::samplePadsMidiInputRelayId)
            selectedMenuId = menuId;
        ++menuId;

        const auto devices = juce::MidiInput::getAvailableDevices();
        for (const auto& device : devices)
        {
            selector.addItem(device.name, menuId);
            idByMenuId[menuId] = device.identifier;
            if (device.identifier == selectedDeviceId)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        selector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    static void populateLooperInputSelector(juce::ComboBox& selector,
                                            std::map<int, int>& inputByMenuId,
                                            NinjamVst3AudioProcessor& processor)
    {
        selector.clear(juce::dontSendNotification);
        inputByMenuId.clear();

        int menuId = 1;
        const int selectedInput = processor.getSamplePadLooperInput();
        selector.addItem("Local 1", menuId);
        inputByMenuId[menuId] = NinjamVst3AudioProcessor::looperInputLocalChannel;
        int selectedMenuId = selectedInput == NinjamVst3AudioProcessor::looperInputLocalChannel ? menuId : 0;
        ++menuId;

        selector.addItem("Sample Pads", menuId);
        inputByMenuId[menuId] = NinjamVst3AudioProcessor::looperInputSamplePads;
        if (selectedInput == NinjamVst3AudioProcessor::looperInputSamplePads)
            selectedMenuId = menuId;
        ++menuId;

        for (const auto& user : processor.getConnectedUsers())
        {
            if (user.name.isEmpty())
                continue;

            const int inputValue = NinjamVst3AudioProcessor::looperInputForRemoteUser(user.index);
            selector.addItem("User " + user.name, menuId);
            inputByMenuId[menuId] = inputValue;
            if (selectedInput == inputValue)
                selectedMenuId = menuId;
            ++menuId;
        }

        int totalInputs = processor.getTotalNumInputChannels();
        if (totalInputs <= 0)
            totalInputs = 2;
        for (int ch = 0; ch < totalInputs; ++ch)
        {
            selector.addItem("In " + juce::String(ch + 1), menuId);
            inputByMenuId[menuId] = ch;
            if (selectedInput == ch)
                selectedMenuId = menuId;
            ++menuId;
        }

        const int numPairs = totalInputs / 2;
        for (int pair = 0; pair < numPairs; ++pair)
        {
            const int left = pair * 2 + 1;
            const int right = left + 1;
            const int inputValue = -1 - pair;
            selector.addItem(juce::String(left) + "/" + juce::String(right), menuId);
            inputByMenuId[menuId] = inputValue;
            if (selectedInput == inputValue)
                selectedMenuId = menuId;
            ++menuId;
        }

        if (selectedMenuId == 0)
            selectedMenuId = 1;
        selector.setSelectedId(selectedMenuId, juce::dontSendNotification);
    }

    void refreshQuickSelectors(bool force)
    {
        if (samplerPopupActive && !force)
            return;

        const auto padsMidiDeviceId = processor.getSamplePadsMidiInputDeviceId();
        if (force || padsMidiDeviceId != displayedPadsMidiDeviceId)
        {
            displayedPadsMidiDeviceId = padsMidiDeviceId;
            populatePadsMidiSelector(padsMidiSelector, padsMidiDeviceByMenuId, padsMidiDeviceId);
        }

        const int looperInput = processor.getSamplePadLooperInput();
        const int totalInputs = processor.getTotalNumInputChannels();
        const juce::String looperUsersFingerprint = buildLooperUsersFingerprint(processor);
        if (force
            || looperInput != displayedLooperInput
            || totalInputs != displayedTotalInputs
            || looperUsersFingerprint != displayedLooperUsersFingerprint)
        {
            displayedLooperInput = looperInput;
            displayedTotalInputs = totalInputs;
            displayedLooperUsersFingerprint = looperUsersFingerprint;
            populateLooperInputSelector(looperInputSelector, looperInputByMenuId, processor);
        }
    }

    static juce::String buildLooperUsersFingerprint(NinjamVst3AudioProcessor& processor)
    {
        juce::StringArray parts;
        for (const auto& user : processor.getConnectedUsers())
            parts.add(juce::String(user.index) + ":" + user.name);
        return parts.joinIntoString("|");
    }

    static juce::String samplePadDuckShapeName(NinjamVst3AudioProcessor::SamplePadDuckShape shape)
    {
        using DuckShape = NinjamVst3AudioProcessor::SamplePadDuckShape;
        switch (shape)
        {
            case DuckShape::tightPump:    return "Tight Pump";
            case DuckShape::slowPump:     return "Slow Pump";
            case DuckShape::hardGate:     return "Hard Gate";
            case DuckShape::reverseSwell: return "Reverse Swell";
            case DuckShape::notchPulse:   return "Notch Pulse";
            case DuckShape::smoothPump:
            default:                      return "Smooth Pump";
        }
    }

    static int samplePadDuckShapeMenuId(NinjamVst3AudioProcessor::SamplePadDuckShape shape)
    {
        return 100 + (int)shape;
    }

    static NinjamVst3AudioProcessor::SamplePadDuckShape samplePadDuckShapeForMenuId(int menuId)
    {
        using DuckShape = NinjamVst3AudioProcessor::SamplePadDuckShape;
        const int shape = menuId - 100;
        switch ((DuckShape)shape)
        {
            case DuckShape::smoothPump:
            case DuckShape::tightPump:
            case DuckShape::slowPump:
            case DuckShape::hardGate:
            case DuckShape::reverseSwell:
            case DuckShape::notchPulse:
                return (DuckShape)shape;
        }
        return DuckShape::smoothPump;
    }

    static juce::String samplePadDuckLengthName(NinjamVst3AudioProcessor::SamplePadDuckLength length)
    {
        using DuckLength = NinjamVst3AudioProcessor::SamplePadDuckLength;
        switch (length)
        {
            case DuckLength::eighth:  return "1/8";
            case DuckLength::half:    return "1/2";
            case DuckLength::quarter:
            default:                  return "1/4";
        }
    }

    static int samplePadDuckLengthMenuId(NinjamVst3AudioProcessor::SamplePadDuckLength length)
    {
        return 200 + (int)length;
    }

    static NinjamVst3AudioProcessor::SamplePadDuckLength samplePadDuckLengthForMenuId(int menuId)
    {
        using DuckLength = NinjamVst3AudioProcessor::SamplePadDuckLength;
        const int length = menuId - 200;
        switch ((DuckLength)length)
        {
            case DuckLength::eighth:
            case DuckLength::quarter:
            case DuckLength::half:
                return (DuckLength)length;
        }
        return DuckLength::quarter;
    }

    void showDuckMenu(juce::Point<int> screenPosition)
    {
        using DuckShape = NinjamVst3AudioProcessor::SamplePadDuckShape;
        using DuckLength = NinjamVst3AudioProcessor::SamplePadDuckLength;

        juce::PopupMenu menu;
        menu.addItem(1, "Duck Enabled", true, processor.isSamplePadDuckEnabled());
        menu.addSeparator();

        juce::PopupMenu shapeMenu;
        const auto currentShape = processor.getSamplePadDuckShape();
        const DuckShape shapes[] =
        {
            DuckShape::smoothPump,
            DuckShape::tightPump,
            DuckShape::slowPump,
            DuckShape::hardGate,
            DuckShape::reverseSwell,
            DuckShape::notchPulse
        };
        for (const auto shape : shapes)
            shapeMenu.addItem(samplePadDuckShapeMenuId(shape), samplePadDuckShapeName(shape), true, currentShape == shape);
        menu.addSubMenu("Shape", shapeMenu);

        juce::PopupMenu lengthMenu;
        const auto currentLength = processor.getSamplePadDuckLength();
        const DuckLength lengths[] = { DuckLength::eighth, DuckLength::quarter, DuckLength::half };
        for (const auto length : lengths)
            lengthMenu.addItem(samplePadDuckLengthMenuId(length), samplePadDuckLengthName(length), true, currentLength == length);
        menu.addSubMenu("Length", lengthMenu);

        juce::Component::SafePointer<SamplePadsComponent> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea({ screenPosition.x, screenPosition.y, 1, 1 }),
                           [safeThis](int result)
                           {
                               if (safeThis == nullptr || result == 0)
                                   return;

                               if (result == 1)
                               {
                                   const bool enabled = !safeThis->processor.isSamplePadDuckEnabled();
                                   safeThis->processor.setSamplePadDuckEnabled(enabled);
                                   safeThis->duckButton.setToggleState(enabled, juce::dontSendNotification);
                               }
                               else if (result >= 100 && result < 200)
                               {
                                   safeThis->processor.setSamplePadDuckShape(samplePadDuckShapeForMenuId(result));
                               }
                               else if (result >= 200 && result < 300)
                               {
                                   safeThis->processor.setSamplePadDuckLength(samplePadDuckLengthForMenuId(result));
                               }

                               safeThis->updateDuckButtonColour();
                           });
    }

    void updateLimiterButtonColour()
    {
        const bool on = limiterButton.getToggleState();
        const auto colour = on ? juce::Colour(0xffd84d4d) : juce::Colour(0xff3a1515);
        limiterButton.setColour(juce::TextButton::buttonColourId, colour);
        limiterButton.setColour(juce::TextButton::buttonOnColourId, colour);
        limiterButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        limiterButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    }

    void updateDuckButtonColour()
    {
        const bool on = duckButton.getToggleState();
        const auto colour = on ? juce::Colour(0xff49d5ff) : juce::Colour(0xff17313a);
        duckButton.setColour(juce::TextButton::buttonColourId, colour);
        duckButton.setColour(juce::TextButton::buttonOnColourId, colour);
        duckButton.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        duckButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        duckButton.setTooltip("Tempo-synced sampler duck: "
                              + samplePadDuckShapeName(processor.getSamplePadDuckShape())
                              + ", "
                              + samplePadDuckLengthName(processor.getSamplePadDuckLength()));
    }

    void updateDefaultFxButtonColour()
    {
        const bool on = defaultFxButton.getToggleState();
        const auto colour = on ? juce::Colour(0xffa7e36e) : juce::Colour(0xff29351d);
        defaultFxButton.setColour(juce::TextButton::buttonColourId, colour);
        defaultFxButton.setColour(juce::TextButton::buttonOnColourId, colour);
        defaultFxButton.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        defaultFxButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    }
    void updateMonitorButtonColour()
    {
        const bool on = monitorButton.getToggleState();
        const auto colour = on ? juce::Colour(0xffffd24a) : juce::Colour(0xff3a3214);
        monitorButton.setColour(juce::TextButton::buttonColourId, colour);
        monitorButton.setColour(juce::TextButton::buttonOnColourId, colour);
        monitorButton.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        monitorButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
    }

    void updateSamplerSizeButtons()
    {
        for (int i = 0; i < 2; ++i)
        {
            auto& button = samplerSizeButtons[(size_t)i];
            const bool canStep = i == 0 ? currentSamplerSizePreset > 0 : currentSamplerSizePreset < 2;
            const auto colour = canStep ? juce::Colour(0xff49d5ff) : juce::Colour(0xff2f3337);
            button.setEnabled(canStep);
            button.setColour(juce::TextButton::buttonColourId, colour);
            button.setColour(juce::TextButton::buttonOnColourId, colour);
            button.setColour(juce::TextButton::textColourOnId, canStep ? juce::Colours::black : juce::Colours::grey);
            button.setColour(juce::TextButton::textColourOffId, canStep ? juce::Colours::black : juce::Colours::grey);
        }
    }

    void refreshBankList(const juce::String& selectBankName)
    {
        bankFolders.clear();
        bankSelector.clear(juce::dontSendNotification);

        const auto bankNames = processor.getSamplePadBankNames();
        for (int i = 0; i < bankNames.size(); ++i)
        {
            bankSelector.addItem(bankNames[i], i + 1);
            bankFolders.add(processor.getSamplePadBankDirectory(bankNames[i]));
        }

        if (selectBankName.isNotEmpty())
        {
            for (int i = 0; i < bankNames.size(); ++i)
            {
                if (bankNames[i] == selectBankName)
                {
                    bankSelector.setSelectedId(i + 1, juce::dontSendNotification);
                    break;
                }
            }
        }
    }

    void loadSelectedBank()
    {
        const int selected = bankSelector.getSelectedItemIndex();
        if (selected >= 0 && selected < bankFolders.size())
        {
            loadBankFolder(bankFolders[selected]);
            return;
        }

        bankChooser = std::make_unique<juce::FileChooser>("Load sample pad bank",
                                                          processor.getSamplePadBanksDirectory(),
                                                          "*");
        juce::Component::SafePointer<SamplePadsComponent> safeThis(this);
        bankChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                 [safeThis](const juce::FileChooser& chooser)
                                 {
                                     if (safeThis == nullptr)
                                         return;

                                     const auto folder = chooser.getResult();
                                     if (folder.isDirectory())
                                         safeThis->loadBankFolder(folder);
                                 });
    }

    void loadBankFolder(const juce::File& folder)
    {
        juce::Component::SafePointer<SamplePadsComponent> safeThis(this);
        processor.loadSamplePadBankAsync(folder,
                                         [safeThis, folder](bool loaded, const juce::String& error)
                                         {
                                             if (safeThis == nullptr)
                                                 return;

                                             if (!loaded)
                                             {
                                                 juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                        "Bank load failed",
                                                                                        error);
                                                 return;
                                             }

                                             safeThis->refreshBankList(folder.getFileName());
                                             safeThis->refreshSampleFxControls();
                                             for (auto& pad : safeThis->pads)
                                                 if (pad != nullptr)
                                                     pad->refreshFromProcessor();
                                         });
    }
    void showSaveBankDialog()
    {
        auto* alert = new juce::AlertWindow("Save Sample Bank",
                                            "Enter a folder name for this pad bank.",
                                            juce::AlertWindow::QuestionIcon);
        const auto selectedName = bankSelector.getText().trim();
        alert->addTextEditor("bankName", selectedName.isNotEmpty() ? selectedName : "New Bank", "Bank:");
        alert->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        juce::Component::SafePointer<SamplePadsComponent> safeThis(this);
        alert->enterModalState(true,
                               juce::ModalCallbackFunction::create(
                                   [safeThis, alert](int result)
                                   {
                                       std::unique_ptr<juce::AlertWindow> alertOwner(alert);
                                       if (safeThis == nullptr || result != 1)
                                           return;

                                       const auto bankName = alert->getTextEditorContents("bankName").trim();
                                       juce::String error;
                                       if (!safeThis->processor.saveSamplePadBank(bankName, error))
                                       {
                                           juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                                                  "Bank save failed",
                                                                                  error);
                                           return;
                                       }

                                       safeThis->refreshBankList(bankName);
                                   }),
                               false);
    }

    NinjamVst3AudioProcessor& processor;
    NinjamVst3AudioProcessorEditor& editor;
    bool abletonHosted = false;
    int currentSamplerSizePreset = 1;
    std::function<void()> onPadsMidiChanged;
    std::function<void(int)> onSamplerSizePresetChanged;
    std::array<std::unique_ptr<SamplePadComponent>, NinjamVst3AudioProcessor::numSamplePads> pads;
    FaderLookAndFeel faderLookAndFeel;
    juce::Label padsMidiLabel;
    WidePopupComboBox padsMidiSelector;
    juce::Label looperInputLabel;
    WidePopupComboBox looperInputSelector;
    juce::Label bankLabel;
    WidePopupComboBox bankSelector;
    std::array<LeftClickOnlyTextButton, 2> samplerSizeButtons { LeftClickOnlyTextButton{ "-" },
                                                                LeftClickOnlyTextButton{ "+" } };
    LeftClickOnlyTextButton loadBankButton{ "Load" };
    LeftClickOnlyTextButton saveBankButton{ "Save" };
    LeftClickOnlyTextButton resetSettingsButton{ "Reset" };
    LeftClickOnlyTextButton clearPadsButton{ "Clear" };
    std::map<int, juce::String> padsMidiDeviceByMenuId;
    std::map<int, int> looperInputByMenuId;
    juce::String displayedPadsMidiDeviceId;
    int displayedLooperInput = NinjamVst3AudioProcessor::looperInputLocalChannel;
    int displayedTotalInputs = -1;
    juce::String displayedLooperUsersFingerprint;
    juce::Array<juce::File> bankFolders;
    std::unique_ptr<juce::FileChooser> bankChooser;
    std::array<SampleFxKnobSlider, NinjamVst3AudioProcessor::numSamplePadFxSlots> sampleFxKnobs;
    std::array<WidePopupComboBox, NinjamVst3AudioProcessor::numSamplePadFxSlots> sampleFxSelectors;
    std::array<juce::Rectangle<int>, NinjamVst3AudioProcessor::numSamplePadFxSlots> sampleFxSlotBounds;
    bool fxRouteDragActive = false;
    int fxRouteDragPad = -1;
    int highlightedFxSlot = -1;
    int hoveredFxSlot = -1;
    int hoverCandidateFxSlot = -1;
    double hoverCandidateSinceMs = 0.0;
    juce::Point<int> fxRouteDragPoint;
    bool fxChainRouteDragActive = false;
    int fxChainRouteSourceSlot = -1;
    int highlightedFxChainTargetSlot = -1;
    juce::Point<int> fxChainRouteDragPoint;
    std::array<float, NinjamVst3AudioProcessor::numSamplePads> padCurrentAlpha {};
    std::array<float, NinjamVst3AudioProcessor::numSamplePads> padTargetAlpha {};
    double lastPadAlphaUpdateMs = 0.0;
    double suppressRouteFocusUntilMs = 0.0;
    double samplerResizeSuppressUntilMs = 0.0;
    bool padsResizeLightweightMode = false;
    bool samplerPopupActive = false;
    static constexpr double routeFocusSuppressAfterEditMs = 800.0;
    static constexpr double padAlphaFadeMs = 320.0;
    static constexpr double routeFocusHoverDelayMs = 300.0;
    static constexpr double samplerResizeRefreshSuppressMs = 220.0;
    static constexpr double quickSelectorRefreshIntervalMs = 250.0;
    NonlinearFaderSlider volumeSlider;
    juce::Label volumeLabel;
    MasterPeakMeter peakMeter;
    LeftClickOnlyTextButton limiterButton{ "Limiter" };
    PopupTextButton duckButton{ "Duck" };
    LeftClickOnlyTextButton defaultFxButton{ "NJ FX" };
    LeftClickOnlyTextButton monitorButton{ "Monitor" };
    double lastQuickSelectorRefreshMs = 0.0;
};

class SamplePadsWindow : public juce::DocumentWindow,
                         private juce::Timer
{
public:
    SamplePadsWindow(NinjamVst3AudioProcessor& p,
                     NinjamVst3AudioProcessorEditor& editor,
                     bool abletonHostedWindow,
                     int abletonSamplerWindowSizePreset,
                     juce::Rectangle<int> savedWindowBounds,
                     std::function<void()> onPadsMidiChangedCallback,
                     std::function<void(int)> onSamplerSizePresetChangedCallback,
                     std::function<void(juce::Rectangle<int>, bool)> onBoundsChangedCallback,
                     std::function<void()> onClosedCallback)
        : DocumentWindow("NINJAM Sample Pads", juce::Colours::black, DocumentWindow::closeButton),
          onClosed(std::move(onClosedCallback)),
          onBoundsChanged(std::move(onBoundsChangedCallback)),
          abletonHosted(abletonHostedWindow)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new SamplePadsComponent(p,
                                                editor,
                                                abletonHosted,
                                                abletonSamplerWindowSizePreset,
                                                std::move(onPadsMidiChangedCallback),
                                                std::move(onSamplerSizePresetChangedCallback)), true);
        if (abletonHosted)
        {
            const auto initialSize = samplePadsWindowSizeForPreset(abletonSamplerWindowSizePreset);
            setResizable(false, false);
            setResizeLimits(initialSize.getWidth(), initialSize.getHeight(), initialSize.getWidth(), initialSize.getHeight());
            centreWithSize(initialSize.getWidth(), initialSize.getHeight());
        }
        else
        {
            setResizable(true, true);
            setResizeLimits(840, 500, 1320, 840);
            if (savedWindowBounds.getWidth() >= 840 && savedWindowBounds.getHeight() >= 500)
            {
                savedWindowBounds.setSize(juce::jlimit(840, 1320, savedWindowBounds.getWidth()),
                                          juce::jlimit(500, 840, savedWindowBounds.getHeight()));
                setBounds(savedWindowBounds);
            }
            else
            {
                centreWithSize(980, 600);
            }
        }
        setVisible(true);
    }

    void applyAbletonSizePreset(int presetIndex)
    {
        if (!abletonHosted)
            return;

        const auto size = samplePadsWindowSizeForPreset(presetIndex);
        const auto centre = getBounds().getCentre();
        setResizeLimits(size.getWidth(), size.getHeight(), size.getWidth(), size.getHeight());
        setSize(size.getWidth(), size.getHeight());
        if (centre.x != 0 || centre.y != 0)
            setCentrePosition(centre.x, centre.y);
    }

    void closeButtonPressed() override
    {
        stopTimer();
        notifyBoundsChanged(true);
        setVisible(false);
        auto callback = onClosed;
        juce::MessageManager::callAsync([callback = std::move(callback)]
        {
            if (callback)
                callback();
        });
    }

    void moved() override
    {
        DocumentWindow::moved();
        scheduleBoundsChanged(false);
    }

    void resized() override
    {
        DocumentWindow::resized();
        scheduleBoundsChanged(false);
    }

private:
    void scheduleBoundsChanged(bool saveNow)
    {
        if (abletonHosted || onBoundsChanged == nullptr || getWidth() <= 0 || getHeight() <= 0)
            return;

        pendingBoundsSaveNow = pendingBoundsSaveNow || saveNow;
        pendingBoundsChanged = true;
        startTimer(boundsChangedDebounceMs);
    }

    void timerCallback() override
    {
        stopTimer();
        if (!pendingBoundsChanged)
            return;

        const bool saveNow = pendingBoundsSaveNow;
        pendingBoundsChanged = false;
        pendingBoundsSaveNow = false;
        notifyBoundsChanged(saveNow);
    }

    void notifyBoundsChanged(bool saveNow)
    {
        if (!abletonHosted && onBoundsChanged && getWidth() > 0 && getHeight() > 0)
            onBoundsChanged(getBounds(), saveNow);
    }

    std::function<void()> onClosed;
    std::function<void(juce::Rectangle<int>, bool)> onBoundsChanged;
    bool abletonHosted = false;
    bool pendingBoundsChanged = false;
    bool pendingBoundsSaveNow = false;
    static constexpr int boundsChangedDebounceMs = 250;
};

struct ChatStylePalette
{
    const char* key;
    juce::Colour primary;
    juce::Colour secondary;
    juce::Colour accent;
};

static ChatStylePalette chatStylePaletteForKey(juce::String key)
{
    key = key.trim().toLowerCase().removeCharacters(" _-");

    if (key == "ocean")
        return { "ocean", juce::Colour(0xff55c7ff), juce::Colour(0xff44f0d2), juce::Colour(0xffb4f6ff) };
    if (key == "sunset")
        return { "sunset", juce::Colour(0xffffa34d), juce::Colour(0xffff5e8a), juce::Colour(0xffffdc72) };
    if (key == "candy")
        return { "candy", juce::Colour(0xffff7bd5), juce::Colour(0xff8a7dff), juce::Colour(0xffffd1f1) };
    if (key == "lime")
        return { "lime", juce::Colour(0xffb8ff4d), juce::Colour(0xff4dff9a), juce::Colour(0xfff2ff79) };
    if (key == "fire")
        return { "fire", juce::Colour(0xffff4238), juce::Colour(0xffffa51f), juce::Colour(0xfffff176) };
    if (key == "violet")
        return { "violet", juce::Colour(0xffbd7bff), juce::Colour(0xff58a6ff), juce::Colour(0xffffb8ff) };
    if (key == "mono")
        return { "mono", juce::Colour(0xfff1f5f9), juce::Colour(0xffb8c2cc), juce::Colour(0xffffffff) };
    if (key == "ruby")
        return { "ruby", juce::Colour(0xffff5c7a), juce::Colour(0xffff9aaa), juce::Colour(0xffffd6dc) };
    if (key == "copper")
        return { "copper", juce::Colour(0xffffa15c), juce::Colour(0xffd97037), juce::Colour(0xffffd1a6) };
    if (key == "lemon")
        return { "lemon", juce::Colour(0xfffff070), juce::Colour(0xffd6ff5c), juce::Colour(0xffffffff) };
    if (key == "emerald")
        return { "emerald", juce::Colour(0xff54f0a0), juce::Colour(0xff9dffd0), juce::Colour(0xffd7ffee) };
    if (key == "cyan")
        return { "cyan", juce::Colour(0xff4df5ff), juce::Colour(0xff78a8ff), juce::Colour(0xffd4fbff) };
    if (key == "sapphire")
        return { "sapphire", juce::Colour(0xff74a9ff), juce::Colour(0xff9a7dff), juce::Colour(0xffdbe8ff) };
    if (key == "plum")
        return { "plum", juce::Colour(0xffd185ff), juce::Colour(0xffff8fda), juce::Colour(0xffffd6fb) };
    if (key == "pearl")
        return { "pearl", juce::Colour(0xfff5f7ff), juce::Colour(0xffd5e5ff), juce::Colour(0xffffffff) };

    return { "aurora", juce::Colour(0xff64d8ff), juce::Colour(0xffff70c8), juce::Colour(0xffb8ff69) };
}

static bool stringContainsEmojiCandidate(const juce::String& text)
{
    for (auto c : text)
    {
        const auto codepoint = (juce::uint32)c;
        if ((codepoint >= 0x1f000 && codepoint <= 0x1faff)
            || (codepoint >= 0x2600 && codepoint <= 0x27bf)
            || codepoint == 0xfe0f
            || codepoint == 0x200d)
            return true;
    }

    return false;
}

static juce::Font getEmojiCapableFont(const juce::Font& baseFont)
{
   #if JUCE_WINDOWS
    juce::Font font("Segoe UI Emoji", baseFont.getHeight(), baseFont.getStyleFlags());
   #elif JUCE_MAC
    juce::Font font("Apple Color Emoji", baseFont.getHeight(), baseFont.getStyleFlags());
   #else
    juce::Font font("Noto Color Emoji", baseFont.getHeight(), baseFont.getStyleFlags());
   #endif
    return font;
}

struct ChatMediaLoadResult
{
    juce::Image preview;
    std::vector<juce::Image> frames;
    std::vector<int> frameDurationsMs;
    int totalDurationMs = 0;
};

static bool memoryBlockLooksLikeGif(const juce::MemoryBlock& data)
{
    if (data.getSize() < 6)
        return false;

    const auto* bytes = static_cast<const char*>(data.getData());
    return std::strncmp(bytes, "GIF87a", 6) == 0 || std::strncmp(bytes, "GIF89a", 6) == 0;
}

static juce::Image imageFromLiceBitmap(LICE_IBitmap& bitmap)
{
    const int width = bitmap.getWidth();
    const int height = bitmap.getHeight();
    if (width <= 0 || height <= 0 || bitmap.getBits() == nullptr)
        return {};

    juce::Image image(juce::Image::ARGB, width, height, true);
    juce::Image::BitmapData dest(image, juce::Image::BitmapData::writeOnly);
    auto* source = bitmap.getBits();
    const int span = bitmap.getRowSpan();

    for (int y = 0; y < height; ++y)
    {
        const int sourceY = bitmap.isFlipped() ? (height - 1 - y) : y;
        const auto* sourceRow = source + sourceY * span;

        for (int x = 0; x < width; ++x)
        {
            const LICE_pixel pixel = sourceRow[x];
            auto* target = reinterpret_cast<juce::PixelARGB*>(dest.getPixelPointer(x, y));
            target->setARGB((juce::uint8)((pixel >> 24) & 0xff),
                            (juce::uint8)((pixel >> 16) & 0xff),
                            (juce::uint8)((pixel >> 8) & 0xff),
                            (juce::uint8)(pixel & 0xff));
            target->premultiply();
        }
    }

    return image;
}

static ChatMediaLoadResult decodeAnimatedGifFromMemory(const juce::MemoryBlock& data)
{
    ChatMediaLoadResult result;
    if (!memoryBlockLooksLikeGif(data))
        return result;

    auto tempFile = juce::File::createTempFile(".gif");
    if (!tempFile.replaceWithData(data.getData(), data.getSize()))
        return result;

    if (void* handle = LICE_GIF_LoadEx(tempFile.getFullPathName().toRawUTF8()))
    {
        LICE_MemBitmap bitmap;
        constexpr int maxFrames = 120;

        for (int i = 0; i < maxFrames; ++i)
        {
            const int delayMs = LICE_GIF_UpdateFrame(handle, &bitmap);
            if (delayMs < 0)
                break;

            auto frame = imageFromLiceBitmap(bitmap);
            if (!frame.isValid())
                continue;

            result.frames.push_back(frame);
            const int clampedDelay = delayMs > 0 ? juce::jmax(30, delayMs) : 100;
            result.frameDurationsMs.push_back(clampedDelay);
            result.totalDurationMs += clampedDelay;
        }

        LICE_GIF_Close(handle);
    }

    tempFile.deleteFile();

    if (!result.frames.empty())
        result.preview = result.frames.front();

    return result;
}

static int drawChatTextRun(juce::Graphics& g,
                           const juce::String& text,
                           const juce::Font& font,
                           juce::Colour colour,
                           int x,
                           int y,
                           int rightEdge,
                           int lineHeight)
{
    if (text.isEmpty())
        return x;

    const auto renderFont = stringContainsEmojiCandidate(text) ? getEmojiCapableFont(font) : font;
    juce::AttributedString attributed;
    attributed.setJustification(juce::Justification::centredLeft);
    attributed.setWordWrap(juce::AttributedString::none);
    attributed.append(text, renderFont, colour);

    juce::TextLayout layout;
    const float maxWidth = (float)juce::jmax(1, rightEdge - x);
    layout.createLayout(attributed, maxWidth);
    layout.draw(g, juce::Rectangle<float>((float)x,
                                          (float)y,
                                          maxWidth,
                                          (float)juce::jmax(lineHeight, (int)std::ceil(layout.getHeight()))));

    const float measuredWidth = layout.getWidth() > 0.0f ? layout.getWidth() : renderFont.getStringWidthFloat(text);
    return x + (int)std::ceil(measuredWidth);
}

static float measureChatTextRun(const juce::String& text, const juce::Font& font)
{
    if (text.isEmpty())
        return 0.0f;

    const auto renderFont = stringContainsEmojiCandidate(text) ? getEmojiCapableFont(font) : font;
    return renderFont.getStringWidthFloat(text);
}

struct ChatTextSegment
{
    juce::String text;
    juce::Colour colour;
    bool isLink = false;
    juce::String linkUrl;
};

struct ChatLayoutUnit
{
    juce::String text;
    bool isLineBreak = false;
    bool isWhitespace = false;
};

static bool isChatLineBreak(juce_wchar c)
{
    return c == '\r' || c == '\n';
}

static std::vector<ChatLayoutUnit> splitChatLayoutUnits(const juce::String& text)
{
    std::vector<ChatLayoutUnit> units;
    const int length = text.length();

    for (int i = 0; i < length;)
    {
        const auto c = text[i];
        if (isChatLineBreak(c))
        {
            if (c == '\r' && i + 1 < length && text[i + 1] == '\n')
                ++i;
            ++i;
            units.push_back({ {}, true, false });
            continue;
        }

        if (juce::CharacterFunctions::isWhitespace(c))
        {
            const int start = i;
            while (i < length && juce::CharacterFunctions::isWhitespace(text[i]) && !isChatLineBreak(text[i]))
                ++i;
            units.push_back({ text.substring(start, i), false, true });
            continue;
        }

        const int start = i;
        while (i < length && !juce::CharacterFunctions::isWhitespace(text[i]) && !isChatLineBreak(text[i]))
            ++i;
        units.push_back({ text.substring(start, i), false, false });
    }

    return units;
}

RichChatDisplayComponent::RichChatDisplayComponent()
    : aliveFlag(std::make_shared<std::atomic<bool>>(true))
{
    addAndMakeVisible(scrollBar);
    scrollBar.addListener(this);
    scrollBar.setAutoHide(true);
}

RichChatDisplayComponent::~RichChatDisplayComponent()
{
    stopTimer();
    aliveFlag->store(false);
    scrollBar.removeListener(this);
}

void RichChatDisplayComponent::setFont(const juce::Font& newFont)
{
    chatFont = newFont.withHeight(juce::jlimit(kChatDisplayMinFontHeight,
                                               kChatDisplayMaxFontHeight,
                                               newFont.getHeight()));
    contentHeight = estimateContentHeight();
    clampScroll();
    repaint();
}

void RichChatDisplayComponent::setBackgroundColour(juce::Colour newColour)
{
    backgroundColour = newColour;
    repaint();
}

void RichChatDisplayComponent::setCommandLinkCallback(std::function<void(const juce::String&)> callback)
{
    commandLinkCallback = std::move(callback);
}

static juce::String extractFirstIntegerText(juce::String text)
{
    text = text.trim();
    juce::String digits;
    bool started = false;
    for (int i = 0; i < text.length(); ++i)
    {
        const auto c = text[i];
        if (c >= '0' && c <= '9')
        {
            digits << juce::String::charToString(c);
            started = true;
        }
        else if (started)
        {
            break;
        }
    }
    return digits;
}

static bool extractVoteCommandFromVotingLine(const juce::String& line, juce::String& command)
{
    const auto lower = line.toLowerCase();
    if (!lower.contains("[voting system]"))
        return false;

    auto tryMarker = [&](const char* marker, const char* kind)
    {
        const juce::String markerText(marker);
        const int markerIndex = lower.indexOf(markerText);
        if (markerIndex < 0)
            return false;

        const auto valueText = extractFirstIntegerText(line.substring(markerIndex + markerText.length()));
        if (valueText.isEmpty())
            return false;

        command = "!vote " + juce::String(kind) + " " + valueText;
        return true;
    };

    return tryMarker("setting bpm to", "bpm")
        || tryMarker("setting bpi to", "bpi");
}

static bool extractChatMedia(const juce::String& line, juce::String& kind, juce::String& url)
{
    const juce::String gifMarker = " shared a GIF: ";
    int marker = line.indexOf(gifMarker);
    if (marker >= 0)
    {
        kind = "GIF";
        url = line.substring(marker + gifMarker.length()).trim();
        return isHttpOrHttpsChatInputUrl(url);
    }

    const juce::String imageMarker = " shared a image: ";
    marker = line.indexOf(imageMarker);
    if (marker >= 0)
    {
        kind = "Image";
        url = line.substring(marker + imageMarker.length()).trim();
        return isHttpOrHttpsChatInputUrl(url);
    }

    return false;
}

void RichChatDisplayComponent::setChatText(const juce::StringArray& lines,
                                           const juce::StringArray& senders,
                                           const NinjamVst3AudioProcessor& processor)
{
    std::map<juce::String, RichChatDisplayComponent::Entry> previousMediaEntries;
    for (const auto& existing : entries)
        if (existing.mediaUrl.isNotEmpty())
            previousMediaEntries[existing.mediaUrl] = existing;

    entries.clear();
    entries.reserve((size_t) lines.size());

    for (int i = 0; i < lines.size(); ++i)
    {
        RichChatDisplayComponent::Entry entry;
        entry.line = lines[i];
        entry.sender = i < senders.size() ? senders[i] : juce::String();
        entry.colourKey = processor.getChatColourKeyForSender(entry.sender);

        juce::String mediaKind;
        juce::String mediaUrl;
        if (extractChatMedia(entry.line, mediaKind, mediaUrl))
        {
            entry.mediaKind = mediaKind;
            entry.mediaUrl = mediaUrl;
            const int marker = entry.line.indexOf(" shared a ");
            if (marker > 0)
                entry.line = entry.line.substring(0, marker) + " shared a " + mediaKind;

            auto existing = previousMediaEntries.find(entry.mediaUrl);
            if (existing != previousMediaEntries.end())
            {
                entry.mediaPreview = existing->second.mediaPreview;
                entry.mediaFrames = existing->second.mediaFrames;
                entry.mediaFrameDurationsMs = existing->second.mediaFrameDurationsMs;
                entry.mediaTotalDurationMs = existing->second.mediaTotalDurationMs;
            }
        }

        entries.push_back(std::move(entry));
    }

    contentHeight = estimateContentHeight();
    scrollY = juce::jmax(0, contentHeight - getHeight());
    clampScroll();
    updateAnimationTimer();
    repaint();
}

int RichChatDisplayComponent::getTextLineHeight() const
{
    return juce::jmax(24, (int)std::ceil(chatFont.getHeight()) + 8);
}

int RichChatDisplayComponent::getEntryTextHeight(const RichChatDisplayComponent::Entry& entry, int textRightEdge) const
{
    return layoutEntryText(entry, 0, textRightEdge, nullptr, nullptr);
}

int RichChatDisplayComponent::layoutEntryText(const RichChatDisplayComponent::Entry& entry,
                                              int y,
                                              int textRightEdge,
                                              juce::Graphics* graphics,
                                              std::vector<RichChatDisplayComponent::PaintedLink>* links) const
{
    const int lineHeight = getTextLineHeight();
    const int leftEdge = 6;
    const int rightEdge = juce::jmax(leftEdge + 24, textRightEdge);
    int x = leftEdge;
    int lineY = y;

    const int prefixEnd = entry.line.indexOf(": ");
    const bool hasSender = entry.sender.isNotEmpty();
    const bool hasChosenStyle = entry.colourKey.isNotEmpty();
    const auto senderPalette = chatStylePaletteForKey(entry.colourKey);
    const auto defaultColour = hasChosenStyle ? senderPalette.primary : senderColour(entry.sender);

    std::vector<ChatTextSegment> segments;
    auto appendPlainRange = [&](int start, int end)
    {
        int index = start;
        while (index < end)
        {
            if (hasSender && hasChosenStyle && prefixEnd > 0 && index < prefixEnd)
            {
                const int prefixLimit = juce::jmin(end, prefixEnd);
                for (; index < prefixLimit; ++index)
                {
                    const int colourIndex = index % 3;
                    segments.push_back({
                        entry.line.substring(index, index + 1),
                        colourIndex == 0 ? senderPalette.primary
                                         : (colourIndex == 1 ? senderPalette.secondary : senderPalette.accent),
                        false,
                        {}
                    });
                }
            }
            else
            {
                segments.push_back({ entry.line.substring(index, end), defaultColour, false, {} });
                index = end;
            }
        }
    };

    juce::Range<int> urlRange;
    int cursor = 0;
    while (findNextChatUrlRange(entry.line, cursor, urlRange))
    {
        if (urlRange.getStart() > cursor)
            appendPlainRange(cursor, urlRange.getStart());

        const juce::String url = entry.line.substring(urlRange.getStart(), urlRange.getEnd());
        segments.push_back({ url, juce::Colour::fromRGB(80, 180, 255), true, url });
        cursor = urlRange.getEnd();
    }
    if (cursor < entry.line.length())
        appendPlainRange(cursor, entry.line.length());

    auto newLine = [&]()
    {
        x = leftEdge;
        lineY += lineHeight;
    };

    auto addLinkBounds = [&](const juce::String& url, juce::Rectangle<int> bounds)
    {
        if (links == nullptr || url.isEmpty() || bounds.isEmpty())
            return;

        if (!links->empty())
        {
            auto& previous = links->back();
            if (previous.url == url
                && previous.bounds.getY() == bounds.getY()
                && previous.bounds.getRight() >= bounds.getX() - 1)
            {
                previous.bounds = previous.bounds.getUnion(bounds);
                return;
            }
        }

        links->push_back({ bounds, url, {} });
    };

    auto drawPiece = [&](const juce::String& text,
                         juce::Colour colour,
                         bool isLink,
                         const juce::String& linkUrl,
                         float measuredWidth)
    {
        const int width = juce::jmax(1, (int)std::ceil(measuredWidth));
        if (graphics != nullptr)
            drawChatTextRun(*graphics, text, chatFont, colour, x, lineY, x + width + 2, lineHeight);

        if (isLink)
            addLinkBounds(linkUrl, { x, lineY, width, lineHeight });

        x += width;
    };

    auto drawLongUnitByCharacter = [&](const juce::String& text,
                                       juce::Colour colour,
                                       bool isLink,
                                       const juce::String& linkUrl)
    {
        for (int i = 0; i < text.length(); ++i)
        {
            const juce::String ch = text.substring(i, i + 1);
            const int charWidth = juce::jmax(1, (int)std::ceil(measureChatTextRun(ch, chatFont)));
            if (x > leftEdge && x + charWidth > rightEdge)
                newLine();

            drawPiece(ch, colour, isLink, linkUrl, (float)charWidth);
        }
    };

    const int lineWidth = rightEdge - leftEdge;
    for (const auto& segment : segments)
    {
        for (const auto& unit : splitChatLayoutUnits(segment.text))
        {
            if (unit.isLineBreak)
            {
                newLine();
                continue;
            }

            if (unit.text.isEmpty())
                continue;

            if (unit.isWhitespace && x == leftEdge)
                continue;

            const float measuredWidth = measureChatTextRun(unit.text, chatFont);
            const int unitWidth = juce::jmax(1, (int)std::ceil(measuredWidth));
            if (x > leftEdge && x + unitWidth > rightEdge)
            {
                newLine();
                if (unit.isWhitespace)
                    continue;
            }

            if (unitWidth <= lineWidth)
                drawPiece(unit.text, segment.colour, segment.isLink, segment.linkUrl, measuredWidth);
            else
                drawLongUnitByCharacter(unit.text, segment.colour, segment.isLink, segment.linkUrl);
        }
    }

    const int textHeight = juce::jmax(lineHeight, lineY - y + lineHeight);
    juce::String voteCommand;
    if (links != nullptr && extractVoteCommandFromVotingLine(entry.line, voteCommand))
        links->push_back({ { leftEdge, y, rightEdge - leftEdge, textHeight }, {}, voteCommand });

    return textHeight;
}

void RichChatDisplayComponent::paint(juce::Graphics& g)
{
    g.fillAll(backgroundColour);
    paintedLinks.clear();

    contentHeight = estimateContentHeight();
    clampScroll();

    const int textWidth = getTextWidthForLayout();
    const auto nowMs = juce::Time::getMillisecondCounter();
    int y = 6 - scrollY;

    for (int i = 0; i < (int)entries.size(); ++i)
    {
        const auto& entry = entries[(size_t)i];
        const int entryTextHeight = getEntryTextHeight(entry, textWidth);
        const int mediaHeight = entry.mediaUrl.isNotEmpty() ? getMediaTileHeight(entry, textWidth) + 6 : 0;

        if (y + entryTextHeight + mediaHeight >= 0 && y <= getHeight())
            layoutEntryText(entry, y, textWidth, &g, &paintedLinks);

        y += entryTextHeight;

        if (entry.mediaUrl.isNotEmpty())
        {
            const auto tile = getMediaTileBounds(entry, y, textWidth);
            if (tile.getBottom() >= 0 && tile.getY() <= getHeight())
            {
                juce::Image mediaImage = entry.mediaPreview;
                if (entry.mediaFrames.size() > 1 && entry.mediaTotalDurationMs > 0)
                {
                    int elapsed = (int)(nowMs % (juce::uint32)entry.mediaTotalDurationMs);
                    int accumulated = 0;
                    for (size_t frameIndex = 0; frameIndex < entry.mediaFrames.size(); ++frameIndex)
                    {
                        const int duration = frameIndex < entry.mediaFrameDurationsMs.size()
                                               ? entry.mediaFrameDurationsMs[frameIndex]
                                               : 100;
                        accumulated += duration;
                        if (elapsed < accumulated)
                        {
                            mediaImage = entry.mediaFrames[frameIndex];
                            break;
                        }
                    }
                }

                if (mediaImage.isValid())
                {
                    g.drawImageWithin(mediaImage,
                                      tile.getX(),
                                      tile.getY(),
                                      tile.getWidth(),
                                      tile.getHeight(),
                                      juce::RectanglePlacement::centred);
                }
                else
                {
                    g.setColour(juce::Colour(0xff202a30));
                    g.fillRoundedRectangle(tile.toFloat(), 6.0f);
                    g.setColour(juce::Colour(0xff44515c));
                    g.drawRoundedRectangle(tile.toFloat().reduced(0.5f), 6.0f, 1.0f);
                    g.setColour(juce::Colour(0xffd7dde5));
                    g.setFont(juce::Font(14.0f, juce::Font::bold));
                    g.drawFittedText(entry.previewLoading ? "Loading GIF..." : entry.mediaKind,
                                     tile.reduced(8),
                                     juce::Justification::centred,
                                     2);
                }
            }

            if (!entry.previewLoading && !entry.mediaPreview.isValid())
                const_cast<RichChatDisplayComponent*>(this)->loadPreviewIfNeeded(i);

            y += tile.getHeight() + 6;
        }
    }
}

void RichChatDisplayComponent::resized()
{
    contentHeight = estimateContentHeight();
    scrollBar.setBounds(getWidth() - 10, 0, 10, getHeight());
    clampScroll();
}

void RichChatDisplayComponent::timerCallback()
{
    repaint();
}

void RichChatDisplayComponent::mouseMove(const juce::MouseEvent& e)
{
    hoveredLinkIndex = getLinkIndexAt(e.getPosition());
    setMouseCursor(hoveredLinkIndex >= 0 ? juce::MouseCursor::PointingHandCursor
                                         : juce::MouseCursor::NormalCursor);
    repaint();
}

void RichChatDisplayComponent::mouseExit(const juce::MouseEvent&)
{
    hoveredLinkIndex = -1;
    setMouseCursor(juce::MouseCursor::NormalCursor);
    repaint();
}

void RichChatDisplayComponent::mouseDown(const juce::MouseEvent& e)
{
    const int linkIndex = getLinkIndexAt(e.getPosition());
    if (linkIndex >= 0 && linkIndex < (int)paintedLinks.size())
    {
        const auto& link = paintedLinks[(size_t)linkIndex];
        if (link.command.isNotEmpty())
        {
            if (commandLinkCallback)
                commandLinkCallback(link.command);
            return;
        }

        juce::URL(link.url).launchInDefaultBrowser();
        return;
    }
}

void RichChatDisplayComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCtrlDown() && std::abs(wheel.deltaY) > 0.0f)
    {
        const bool wasAtBottom = scrollY >= juce::jmax(0, contentHeight - getHeight() - 4);
        const float direction = wheel.deltaY > 0.0f ? 1.0f : -1.0f;
        const float newHeight = juce::jlimit(kChatDisplayMinFontHeight,
                                             kChatDisplayMaxFontHeight,
                                             std::round(chatFont.getHeight() + direction));

        if (std::abs(newHeight - chatFont.getHeight()) > 0.01f)
        {
            chatFont = chatFont.withHeight(newHeight);
            contentHeight = estimateContentHeight();
            if (wasAtBottom)
                scrollY = juce::jmax(0, contentHeight - getHeight());
            clampScroll();
            repaint();
        }
        return;
    }

    scrollY = juce::jlimit(0, juce::jmax(0, contentHeight - getHeight()), scrollY - (int)std::round(wheel.deltaY * 160.0f));
    scrollBar.setCurrentRangeStart(scrollY);
    repaint();
}

void RichChatDisplayComponent::scrollBarMoved(juce::ScrollBar*, double newRangeStart)
{
    scrollY = (int)std::round(newRangeStart);
    repaint();
}

void RichChatDisplayComponent::clampScroll()
{
    const int maxScroll = juce::jmax(0, contentHeight - getHeight());
    scrollY = juce::jlimit(0, maxScroll, scrollY);
    scrollBar.setRangeLimits(0.0, (double)juce::jmax(getHeight(), contentHeight));
    scrollBar.setCurrentRange(scrollY, getHeight());
    scrollBar.setVisible(contentHeight > getHeight());
}

int RichChatDisplayComponent::getTextWidthForLayout() const
{
    return juce::jmax(40, getWidth() - scrollBar.getWidth() - 10);
}

static int getRichChatMediaTileWidth(int textWidth)
{
    return juce::jmax(40, juce::jmin(420, textWidth - 8));
}

int RichChatDisplayComponent::getMediaTileHeight(const RichChatDisplayComponent::Entry& entry, int textWidth) const
{
    const int tileWidth = getRichChatMediaTileWidth(textWidth);
    int imageWidth = 0;
    int imageHeight = 0;

    if (entry.mediaPreview.isValid())
    {
        imageWidth = entry.mediaPreview.getWidth();
        imageHeight = entry.mediaPreview.getHeight();
    }
    else if (!entry.mediaFrames.empty() && entry.mediaFrames.front().isValid())
    {
        imageWidth = entry.mediaFrames.front().getWidth();
        imageHeight = entry.mediaFrames.front().getHeight();
    }

    if (imageWidth > 0 && imageHeight > 0)
        return juce::jlimit(96, 420, (int)std::ceil((double)tileWidth * (double)imageHeight / (double)imageWidth));

    return 120;
}

juce::Rectangle<int> RichChatDisplayComponent::getMediaTileBounds(const RichChatDisplayComponent::Entry& entry, int y, int textWidth) const
{
    const int tileWidth = getRichChatMediaTileWidth(textWidth);
    return { 8, y, tileWidth, getMediaTileHeight(entry, textWidth) };
}

int RichChatDisplayComponent::estimateContentHeight() const
{
    const int textWidth = getTextWidthForLayout();
    int height = 10;

    for (const auto& entry : entries)
    {
        height += getEntryTextHeight(entry, textWidth);
        if (entry.mediaUrl.isNotEmpty())
            height += getMediaTileHeight(entry, textWidth) + 6;
    }

    return height;
}

void RichChatDisplayComponent::updateAnimationTimer()
{
    bool hasAnimatedMedia = false;
    for (const auto& entry : entries)
    {
        if (entry.mediaFrames.size() > 1 && entry.mediaTotalDurationMs > 0)
        {
            hasAnimatedMedia = true;
            break;
        }
    }

    if (hasAnimatedMedia)
    {
        if (!isTimerRunning())
            startTimerHz(20);
    }
    else if (isTimerRunning())
    {
        stopTimer();
    }
}

int RichChatDisplayComponent::getLinkIndexAt(juce::Point<int> position) const
{
    for (int i = 0; i < (int)paintedLinks.size(); ++i)
        if (paintedLinks[(size_t)i].bounds.contains(position))
            return i;
    return -1;
}

void RichChatDisplayComponent::loadPreviewIfNeeded(int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int)entries.size())
        return;

    auto& entry = entries[(size_t)entryIndex];
    if (entry.previewLoading || entry.mediaPreview.isValid() || entry.mediaUrl.isEmpty())
        return;

    entry.previewLoading = true;
    const auto alive = aliveFlag;
    juce::Component::SafePointer<RichChatDisplayComponent> safeThis(this);
    const juce::String url = entry.mediaUrl;

    std::thread([alive, safeThis, entryIndex, url]
    {
        ChatMediaLoadResult result;
        int statusCode = 0;
        auto stream = juce::URL(url).createInputStream(
            juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
                .withConnectionTimeoutMs(4500)
                .withNumRedirectsToFollow(3)
                .withStatusCode(&statusCode)
                .withExtraHeaders("User-Agent: NINJAMplus/1.0\r\nAccept: image/gif,image/webp,image/png,image/jpeg,*/*\r\n")
                .withHttpRequestCmd("GET"));

        if (stream != nullptr && (statusCode == 0 || (statusCode >= 200 && statusCode < 300)))
        {
            juce::MemoryBlock data;
            stream->readIntoMemoryBlock(data, 8 * 1024 * 1024);
            if (data.getSize() > 0)
            {
                result = decodeAnimatedGifFromMemory(data);
                if (!result.preview.isValid())
                {
                    juce::MemoryInputStream imageStream(data, false);
                    result.preview = juce::ImageFileFormat::loadFrom(imageStream);
                }
            }
        }

        if (!alive->load())
            return;

        juce::MessageManager::callAsync([alive, safeThis, entryIndex, url, result = std::move(result)]() mutable
        {
            if (!alive->load() || safeThis == nullptr || entryIndex < 0 || entryIndex >= (int)safeThis->entries.size())
                return;

            auto& target = safeThis->entries[(size_t)entryIndex];
            if (target.mediaUrl != url)
                return;

            const bool wasAtBottom = safeThis->scrollY >= juce::jmax(0, safeThis->contentHeight - safeThis->getHeight() - 4);
            target.mediaPreview = result.preview;
            target.mediaFrames = std::move(result.frames);
            target.mediaFrameDurationsMs = std::move(result.frameDurationsMs);
            target.mediaTotalDurationMs = result.totalDurationMs;
            target.previewLoading = false;
            safeThis->contentHeight = safeThis->estimateContentHeight();
            if (wasAtBottom)
                safeThis->scrollY = juce::jmax(0, safeThis->contentHeight - safeThis->getHeight());
            safeThis->clampScroll();
            safeThis->updateAnimationTimer();
            safeThis->repaint();
        });
    }).detach();
}

static juce::Colour senderColour(const juce::String& sender)
{
    if (sender == "me")
        return juce::Colours::white;
    if (sender.isEmpty())
        return juce::Colour::fromRGB(160, 160, 120);   // dim amber – system

    // Deterministic hash → one of 8 distinct palette colours
    uint32_t h = 5381u;
    for (auto c : sender)
        h = h * 33u ^ (uint32_t)juce::CharacterFunctions::toUpperCase(c);

    static const juce::Colour palette[] = {
        juce::Colour::fromRGB(100, 180, 255),  // blue
        juce::Colour::fromRGB( 80, 210, 140),  // green
        juce::Colour::fromRGB(255, 165,  80),  // orange
        juce::Colour::fromRGB(190, 120, 255),  // purple
        juce::Colour::fromRGB( 80, 220, 215),  // teal
        juce::Colour::fromRGB(255, 130, 160),  // pink
        juce::Colour::fromRGB(230, 200,  80),  // gold
        juce::Colour::fromRGB(160, 200, 100),  // lime
    };
    return palette[h % 8u];
}

static juce::String normaliseColourPresetName(const juce::String& name)
{
    auto s = name.trim().toLowerCase();
    s = s.removeCharacters(" _-");
    return s;
}

static juce::Colour colourFromPresetName(const juce::String& preset, const juce::Colour& fallback)
{
    const auto key = normaliseColourPresetName(preset);
    if (key == "gold") return juce::Colour(0xffb8860b);
    if (key == "grey" || key == "gray") return juce::Colour(0xff909090);
    if (key == "sand" || key == "sandcolour" || key == "sandcolor") return juce::Colour(0xffc2b280);
    if (key == "yellow") return juce::Colour(0xffffd700);
    if (key == "orange") return juce::Colour(0xffff8c00);
    if (key == "red") return juce::Colour(0xffdc143c);
    if (key == "blue") return juce::Colour(0xff1e90ff);
    if (key == "pink") return juce::Colour(0xffff69b4);
    if (key == "purpleblue") return juce::Colour(0xff6a5acd);
    if (key == "black") return juce::Colour(0xff202020);
    if (key == "cream") return juce::Colour(0xfffff5dc);
    if (key == "white") return juce::Colour(0xfff2f2f2);
    return fallback;
}

static void applyColoredChat(RichChatDisplayComponent& display,
                             const juce::StringArray& lines,
                             const juce::StringArray& senders,
                             const NinjamVst3AudioProcessor& processor)
{
    display.setChatText(lines, senders, processor);
}
// --- end chat helpers ---

void CustomKnobLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                              float sliderPos, const float rotaryStartAngle,
                                              const float rotaryEndAngle, juce::Slider& slider)
{
    auto centreX = (float)x + (float)width * 0.5f;
    auto centreY = (float)y + (float)height * 0.5f;

    auto* editor = explicitEditor != nullptr
        ? explicitEditor
        : dynamic_cast<NinjamVst3AudioProcessorEditor*>(slider.getParentComponent());
    if (editor == nullptr)
    {
        auto* p = slider.getParentComponent();
        while (p != nullptr && editor == nullptr)
        {
            editor = dynamic_cast<NinjamVst3AudioProcessorEditor*>(p);
            p = p->getParentComponent();
        }
    }

    const int knobMinDim = juce::jmin(width, height);
    const bool forceImageKnob = slider.getProperties().contains("forceImageKnob")
        && (bool)slider.getProperties()["forceImageKnob"];
    const bool disableImageKnob = slider.getProperties().contains("disableImageKnob")
        && (bool)slider.getProperties()["disableImageKnob"];
    const bool matchReleaseStyle = slider.getProperties().contains("matchReleaseStyle")
        && (bool)slider.getProperties()["matchReleaseStyle"];
    const bool useImageKnob = (!disableImageKnob
                               && editor != nullptr
                               && editor->radioKnobImage.isValid()
                               && (knobMinDim >= 40 || forceImageKnob || matchReleaseStyle));
    if (useImageKnob)
    {
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        const float inset = matchReleaseStyle
            ? 0.0f
            : juce::jmax(1.0f, (float)juce::jmin(width, height) * 0.08f);
        const float radius = (float)juce::jmin(width / 2, height / 2) - inset;
        g.saveState();
        g.addTransform(juce::AffineTransform::rotation(angle, centreX, centreY));
        if (editor->sandSkinOpaqueKnobs)
        {
            // Opaque underlay for Sand 1 seashell knobs so they do not look washed out.
            g.setColour(juce::Colour::fromRGB(226, 206, 182));
            g.fillEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);
        }
        g.setOpacity(1.0f);
        g.drawImageWithin(editor->radioKnobImage,
                          (int)(centreX - radius), (int)(centreY - radius),
                          (int)(radius * 2.0f), (int)(radius * 2.0f),
                          juce::RectanglePlacement::fillDestination);
        if (editor->sandSkinOpaqueKnobs)
        {
            g.setOpacity(1.0f);
            g.drawImageWithin(editor->radioKnobImage,
                              (int)(centreX - radius), (int)(centreY - radius),
                              (int)(radius * 2.0f), (int)(radius * 2.0f),
                              juce::RectanglePlacement::fillDestination);
        }
        g.restoreState();
        return;
    }

    const bool smallKnob = !matchReleaseStyle && knobMinDim < 34;
    const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    const float outerRadius = (float)juce::jmin(width / 2, height / 2) - (knobMinDim < 30 ? 2.5f : 4.0f);

    const bool forceGreyKnob = slider.getProperties().contains("forceGreyKnob")
        && (bool)slider.getProperties()["forceGreyKnob"];
    const auto knobPreset = editor != nullptr ? normaliseColourPresetName(editor->knobColourPreset) : juce::String();
    const bool multiColourKnob = !forceGreyKnob && knobPreset.startsWith("multi");
    const juce::Colour knobBase = forceGreyKnob ? juce::Colour(0xff9ca3ad)
                                                : (editor != nullptr ? editor->knobThemeColour : juce::Colours::grey);
    const juce::Colour ringFill = knobBase.darker(1.1f);
    const juce::Colour ringStroke = knobBase.darker(1.4f);
    const juce::Colour tickColour = multiColourKnob
        ? juce::Colour::fromHSV(sliderPos * 0.8f, 0.8f, 1.0f, 1.0f)
        : knobBase.brighter(0.1f);

    // --- Tick marks ---
    if (!smallKnob)
    {
        const int numTicks = 11;
        for (int i = 0; i < numTicks; ++i)
        {
            float tickAngle = rotaryStartAngle + (float)i / (float)(numTicks - 1) * (rotaryEndAngle - rotaryStartAngle);
            float s = std::sin(tickAngle), c = -std::cos(tickAngle);
            float inner = outerRadius + 3.0f;
            float outer = outerRadius + 7.0f;
            g.setColour(tickColour);
            g.drawLine(centreX + s * inner, centreY + c * inner,
                       centreX + s * outer, centreY + c * outer, 1.2f);
        }
    }

    // --- Outer ring ---
    const float ringRadius = outerRadius;
    juce::Path ring;
    if (smallKnob)
    {
        ring.addEllipse(centreX - ringRadius, centreY - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f);
    }
    else
    {
        const int teeth = 24;
        for (int i = 0; i <= teeth * 2; ++i)
        {
            float a = (float)i / (float)(teeth * 2) * juce::MathConstants<float>::twoPi;
            float r = (i % 2 == 0) ? ringRadius : ringRadius - 3.0f;
            float px = centreX + std::sin(a) * r;
            float py = centreY - std::cos(a) * r;
            if (i == 0) ring.startNewSubPath(px, py);
            else        ring.lineTo(px, py);
        }
        ring.closeSubPath();
    }
    g.setColour(ringFill);
    g.fillPath(ring);
    g.setColour(ringStroke);
    g.strokePath(ring, juce::PathStrokeType(smallKnob ? 0.6f : 0.8f));

    // --- Inner cap with radial gradient ---
    const float capRadius = juce::jmax(2.0f, ringRadius - (smallKnob ? 1.2f : 5.0f));
    const juce::Colour capHighlight = multiColourKnob
        ? juce::Colour::fromHSV(sliderPos * 0.8f, 0.7f, 1.0f, 1.0f)
        : knobBase.brighter(smallKnob ? 1.05f : 0.75f);
    const juce::Colour capShadow = multiColourKnob
        ? juce::Colour::fromHSV(sliderPos * 0.8f, 0.9f, 0.45f, 1.0f)
        : knobBase.darker(smallKnob ? 0.45f : 0.8f);
    juce::ColourGradient capGrad(capHighlight, centreX - capRadius * 0.35f, centreY - capRadius * 0.35f,
                                 capShadow, centreX + capRadius * 0.5f,  centreY + capRadius * 0.6f, true);
    capGrad.addColour(0.45, knobBase.brighter(smallKnob ? 0.65f : 0.35f));
    g.setGradientFill(capGrad);
    g.fillEllipse(centreX - capRadius, centreY - capRadius, capRadius * 2.0f, capRadius * 2.0f);

    // Subtle rim shadow on cap
    g.setColour(juce::Colours::black.withAlpha(smallKnob ? 0.18f : 0.35f));
    g.drawEllipse(centreX - capRadius, centreY - capRadius, capRadius * 2.0f, capRadius * 2.0f, smallKnob ? 0.9f : 1.5f);

    // Specular highlight (top-left arc)
    juce::Path highlight;
    highlight.addArc(centreX - capRadius * 0.65f, centreY - capRadius * 0.65f,
                     capRadius * 1.3f, capRadius * 1.3f,
                     -juce::MathConstants<float>::pi * 0.9f,
                     -juce::MathConstants<float>::pi * 0.2f, true);
    g.setColour(juce::Colours::white.withAlpha(multiColourKnob ? 0.35f : (smallKnob ? 0.42f : 0.28f)));
    g.strokePath(highlight, juce::PathStrokeType(capRadius * 0.18f));

    // --- Indicator line ---
    const float lineStart = capRadius * 0.22f;
    const float lineEnd   = capRadius * 0.82f;
    g.setColour(juce::Colours::white.withAlpha(0.95f));
    g.drawLine(centreX + std::sin(angle) * lineStart, centreY - std::cos(angle) * lineStart,
               centreX + std::sin(angle) * lineEnd,   centreY - std::cos(angle) * lineEnd,
               2.2f);
    // Bright dot at tip
    g.setColour(juce::Colours::white);
    float dotX = centreX + std::sin(angle) * lineEnd;
    float dotY = centreY - std::cos(angle) * lineEnd;
    g.fillEllipse(dotX - 2.0f, dotY - 2.0f, 4.0f, 4.0f);
}

NinjamVst3AudioProcessorEditor::NinjamVst3AudioProcessorEditor (NinjamVst3AudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p), intervalDisplay(p), userList(p)
{
    setResizable(true, true);
    setResizeLimits(900, 500, 2200, 1500);
    customKnobLookAndFeel.setExplicitEditor(this);
    const bool settingsFileReady = renewSettingsFileIfCorrupt(makeSettingsOptions(), this);

    juce::LookAndFeel::setDefaultLookAndFeel(&outlinedLabelLAF);

    addAndMakeVisible(backgroundComponent);

    serverLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(serverLabel);
    serverField.setText("");
    serverField.setIndents(4, 8);
    serverField.onTextChange = [this] { markPersistentSettingsDirty(); };
    serverField.onReturnKey = [this] { connectClicked(); };
    addAndMakeVisible(serverField);

    addAndMakeVisible(serverListButton);
    serverListButton.setButtonText("Servers");
    serverListButton.setTooltip("Click to View Servers");
    serverListButton.onClick = [this] { serverListClicked(); };

    userLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(userLabel);
    userField.setText("user" + juce::String(juce::Random::getSystemRandom().nextInt(100)));
    userField.setIndents(4, 8);
    userField.onTextChange = [this] { markPersistentSettingsDirty(); };
    addAndMakeVisible(userField);

    addAndMakeVisible(anonymousButton);
    anonymousButton.setToggleState(true, juce::dontSendNotification);
    anonymousButton.onClick = [this] { anonymousToggled(); markPersistentSettingsDirty(); };

    passLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(passLabel);
    addAndMakeVisible(passField);
    passField.setIndents(4, 8);
    passField.onTextChange = [this] { markPersistentSettingsDirty(); };

    addAndMakeVisible(connectButton);
    connectButton.setButtonText("Connect");
    connectButton.onClick = [this] { connectClicked(); };

    addAndMakeVisible(statusLabel);

    addAndMakeVisible(transmitButton);
    transmitButton.setClickingTogglesState(true);
    transmitButton.onClick = [this] { transmitToggled(); };
    updateTransmitButtonColor();

    addAndMakeVisible(localMonitorButton);
    localMonitorButton.setClickingTogglesState(true);
    localMonitorButton.onClick = [this]
    {
        audioProcessor.setLocalMonitorEnabled(localMonitorButton.getToggleState());
        updateMonitorButtonColor();
    };
    updateMonitorButtonColor();

    addAndMakeVisible(voiceChatButton);
    voiceChatButton.setClickingTogglesState(true);
    voiceChatButton.setToggleState(false, juce::dontSendNotification);
    voiceChatButton.onClick = [this]
    {
        audioProcessor.setVoiceChatMode(voiceChatButton.getToggleState());
        voiceChatGlowPhase = 0.0f;
        updateVoiceChatButtonColor();
    };
    updateVoiceChatButtonColor();

    bitrateSelector.addItem("64 kbps",  1);
    bitrateSelector.addItem("96 kbps",  2);
    bitrateSelector.addItem("128 kbps", 3);
    bitrateSelector.addItem("160 kbps", 4);
    bitrateSelector.addItem("192 kbps", 5);
    bitrateSelector.addItem("256 kbps", 6);
    bitrateSelector.addItem("320 kbps", 7);
    bitrateSelector.setSelectedId(3, juce::dontSendNotification); // 128 kbps default
    bitrateSelector.onChange = [this]
    {
        const int bitrateValues[] = { 64, 96, 128, 160, 192, 256, 320 };
        int idx = bitrateSelector.getSelectedId() - 1;
        if (idx >= 0 && idx < 7)
            audioProcessor.setLocalBitrate(bitrateValues[idx]);
    };
    addAndMakeVisible(bitrateSelector);
    bitrateSelector.setTooltip("Quality");

    addAndMakeVisible(midiRelayTargetSelector);
    midiRelayTargetSelector.setTooltip("Send MIDI/OSC");
    midiRelayTargetSelector.onClick = [this] { showMidiRelayTargetMenu(); };
    refreshMidiRelayTargetSelector();
    addListener(this);
    if (!connect(9001))
        for (int port = 9002; port <= 9010; ++port)
            if (connect(port))
                break;

    addAndMakeVisible(videoButton);
    videoButton.setTooltip("Video Room");
    videoButton.onClick = [this] { videoClicked(); };

    addAndMakeVisible(samplePadsButton);
    samplePadsButton.setTooltip("Sample Pads");
    samplePadsButton.setLookAndFeel(&samplePadsBtnLAF);
    samplePadsButton.onClick = [this] { showSamplePadsWindow(); };

    addAndMakeVisible(layoutButton);
    layoutButton.setClickingTogglesState(true);
    layoutButton.setTooltip("Vertical Mixer");
    layoutButton.setLookAndFeel(&faderIconLookAndFeel);
    layoutButton.onClick = [this] { layoutToggled(); updateLayoutButtonColor(); markPersistentSettingsDirty(); };
    updateLayoutButtonColor();

    addAndMakeVisible(autoLevelButton);
    autoLevelButton.setClickingTogglesState(true);
    autoLevelButton.setTooltip("Auto Adjust Volume");
    autoLevelButton.onClick = [this]
    {
        bool newState = autoLevelButton.getToggleState();
        if (newState == autoLevelEnabled)
            return;

        autoLevelEnabled = newState;
        if (!autoLevelEnabled)
        {
            auto users = audioProcessor.getConnectedUsers();
            for (auto& u : users)
                audioProcessor.setUserVolume(u.index, u.volume);

            autoLevelCurrentGains.clear();
            autoLevelLastAppliedGains.clear();
            autoLevelPeakLevels.clear();
            autoLevelChannelActiveTicks.clear();
            autoLevelMeasureTicks.clear();
            autoLevelOverTargetTicks.clear();
            autoLevelUnderTargetTicks.clear();
            autoLevelUserNameById.clear();
            autoLevelWorkTickCounter = 0;
        }
        audioProcessor.setSoftLimiterEnabled(autoLevelEnabled);
        updateAutoLevelButtonColor();
        markPersistentSettingsDirty();
    };
    updateAutoLevelButtonColor();

    addAndMakeVisible(metronomeLabel);
    addAndMakeVisible(metronomeSlider);
    metronomeSlider.setRange(0.0, 1.0);
    metronomeSlider.setValue(0.5);
    metronomeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    metronomeSlider.setTooltip("Metronome Volume");
    metronomeSlider.onValueChange = [this] { metronomeChanged(); };

    addAndMakeVisible(metronomeMuteButton);
    metronomeMuteButton.setClickingTogglesState(true);
    metronomeMuteButton.setToggleState(true, juce::dontSendNotification);  // starts unmuted
    metronomeMuteButton.setTooltip("Mute Metronome");
    metronomeMuteButton.setLookAndFeel(&metronomeBtnLAF);
    metronomeMuteButton.onClick = [this]
    {
        if (metronomeMuteButton.getToggleState())
        {
            // unmuting: restore stored volume
            storedMetronomeVolume = audioProcessor.getStoredMetronomeVolume();
            metronomeSlider.setValue(storedMetronomeVolume, juce::dontSendNotification);
            audioProcessor.setMetronomeMuted(false);
        }
        else
        {
            // muting: store current volume and silence
            storedMetronomeVolume = (float)metronomeSlider.getValue();
            audioProcessor.setMetronomeMuted(true);
        }
        updateMetronomeButtonColor();
    };
    updateMetronomeButtonColor();

    addAndMakeVisible(syncButton);
    syncButton.setClickingTogglesState(true);
    syncButton.setToggleState(false, juce::dontSendNotification);
    syncButton.setLookAndFeel(&syncIconLAF);
    syncButton.onClick = [this] { syncToggled(); updateSyncButtonColor(); };
    syncButton.onPopupMenuRequest = [this] { showSyncCompensationMenu(syncButton); };
    updateSyncButtonTooltip();
    updateSyncButtonColor();

    addAndMakeVisible(fxButton);
    fxButton.onClick = [this] { showFxMenu(); };
    updateFxButtonLabel();
    addAndMakeVisible(optionsButton);
    optionsButton.onClick = [this] { showOptionsMenu(); };

    addAndMakeVisible(aboutButton);
    aboutButton.setTooltip("About NINJAMplus");
    aboutButton.onClick = [this] { showAboutWindow(); };

    addAndMakeVisible(tempoLabel);
    tempoLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(chatButton);
    chatButton.setClickingTogglesState(true);
    chatButton.setWantsKeyboardFocus(false);
    chatButton.setToggleState(true, juce::dontSendNotification);
    chatButton.setTooltip("Open Chat");
    chatButton.setLookAndFeel(&chatBtnLAF);
    chatButton.onClick = [this] { chatToggled(); markPersistentSettingsDirty(); };
    updateChatButtonColor();

    addAndMakeVisible(usersLabel);
    addAndMakeVisible(spreadOutputsButton);
    spreadOutputsButton.setClickingTogglesState(true);
    spreadOutputsButton.setToggleState(false, juce::dontSendNotification);
    spreadOutputsButton.onClick = [this]
    {
        audioProcessor.setSpreadOutputsEnabled(spreadOutputsButton.getToggleState());
        markPersistentSettingsDirty();
    };
    addAndMakeVisible(userList);

    addAndMakeVisible(addLocalChannelButton);
    addLocalChannelButton.setTooltip("Add Channel");
    addAndMakeVisible(removeLocalChannelButton);
    removeLocalChannelButton.setTooltip("Remove Channel");
    addAndMakeVisible(localFaderLabel);
    localFaderLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(localChordLabel);
    localChordLabel.setJustificationType(juce::Justification::centred);
    localChordLabel.setFont(juce::Font(14.0f, juce::Font::bold));
    localChordLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    localChordLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff202428));
    localChordLabel.setColour(juce::Label::outlineColourId, juce::Colour(0xff48515a));
    localChordLabel.setTooltip("Detected chord on local channel 1");
    addAndMakeVisible(localChordStatsLabel);
    localChordStatsLabel.setJustificationType(juce::Justification::centred);
    localChordStatsLabel.setFont(juce::Font(9.0f));
    localChordStatsLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    localChordStatsLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff15181b));
    localChordStatsLabel.setTooltip("Local chord detector CPU and memory estimate");

    addLocalChannelButton.onClick = [this]
    {
        int current = audioProcessor.getNumLocalChannels();
        if (current < NinjamVst3AudioProcessor::maxLocalChannels)
        {
            audioProcessor.setNumLocalChannels(current + 1);
            for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
                localChannelNameLabels[(size_t)i].setText(audioProcessor.getLocalChannelName(i), juce::dontSendNotification);
            resized();
        }
    };

    removeLocalChannelButton.onClick = [this]
    {
        int current = audioProcessor.getNumLocalChannels();
        if (current > 1)
        {
            audioProcessor.setNumLocalChannels(current - 1);
            for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
                localChannelNameLabels[(size_t)i].setText(audioProcessor.getLocalChannelName(i), juce::dontSendNotification);
            resized();
        }
    };

    int totalInputs = audioProcessor.getTotalNumInputChannels();
    if (totalInputs <= 0)
        totalInputs = 2;
    int numPairs = totalInputs / 2;

    for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
    {
        addAndMakeVisible(localFaders[(size_t)i]);
        addAndMakeVisible(localPeakMeters[(size_t)i]);
        addAndMakeVisible(localInputSelectors[(size_t)i]);
        addAndMakeVisible(localInputModeSelectors[(size_t)i]);
        addAndMakeVisible(localDbLabels[(size_t)i]);
        addAndMakeVisible(localReverbSendKnobs[(size_t)i]);
        addAndMakeVisible(localDelaySendKnobs[(size_t)i]);
        addAndMakeVisible(localReverbSendLabels[(size_t)i]);
        addAndMakeVisible(localDelaySendLabels[(size_t)i]);

        // Editable channel name label
        auto& nameLabel = localChannelNameLabels[(size_t)i];
        nameLabel.setText(audioProcessor.getLocalChannelName(i), juce::dontSendNotification);
        nameLabel.setEditable(true, false); // single-click to edit
        nameLabel.setJustificationType(juce::Justification::centred);
        nameLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        nameLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff1a1a1a));
        nameLabel.setColour(juce::Label::outlineColourId, juce::Colour(0xff333333));
        nameLabel.setTooltip("Click to name this channel");
        int ch = i;
        nameLabel.onTextChange = [this, ch]
        {
            audioProcessor.setLocalChannelName(ch, localChannelNameLabels[(size_t)ch].getText());
        };
        addAndMakeVisible(nameLabel);

        auto& fader = localFaders[(size_t)i];
        fader.setSliderStyle(juce::Slider::LinearVertical);
        fader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        fader.setRange(0.0, 2.0);
        fader.setSkewFactorFromMidPoint(0.25);
        fader.setSliderSnapsToMousePosition(false);
        fader.setValue(audioProcessor.getLocalChannelGain(i), juce::dontSendNotification);
        fader.setDoubleClickReturnValue(true, 1.0);
        fader.setLookAndFeel(&mixerFaderLookAndFeel);

        fader.onValueChange = [this, i]
        {
            float value = (float)localFaders[(size_t)i].getValue();
            audioProcessor.setLocalChannelGain(i, value);
            if (i == 0)
                audioProcessor.setLocalInputGain(value);
        };
        registerMidiLearnTarget(fader, "local.fader." + juce::String(i + 1), false);

        auto& revSend = localReverbSendKnobs[(size_t)i];
        revSend.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        revSend.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        revSend.setRange(0.0, 1.0);
        revSend.setValue(audioProcessor.getLocalChannelReverbSend(i), juce::dontSendNotification);
        revSend.setLookAndFeel(&customKnobLookAndFeel);
        revSend.getProperties().set("forceImageKnob", true);
        revSend.getProperties().set("matchReleaseStyle", true);
        revSend.setTooltip("Reverb Send");
        revSend.onValueChange = [this, i]
        {
            audioProcessor.setLocalChannelReverbSend(i, (float)localReverbSendKnobs[(size_t)i].getValue());
        };
        registerMidiLearnTarget(revSend, "local.send.reverb." + juce::String(i + 1), false);

        auto& dlySend = localDelaySendKnobs[(size_t)i];
        dlySend.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        dlySend.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        dlySend.setRange(0.0, 1.0);
        dlySend.setValue(audioProcessor.getLocalChannelDelaySend(i), juce::dontSendNotification);
        dlySend.setLookAndFeel(&customKnobLookAndFeel);
        dlySend.getProperties().set("forceImageKnob", true);
        dlySend.getProperties().set("matchReleaseStyle", true);
        dlySend.setTooltip("Delay Send");
        dlySend.onValueChange = [this, i]
        {
            audioProcessor.setLocalChannelDelaySend(i, (float)localDelaySendKnobs[(size_t)i].getValue());
        };
        registerMidiLearnTarget(dlySend, "local.send.delay." + juce::String(i + 1), false);

        auto& revLbl = localReverbSendLabels[(size_t)i];
        revLbl.setText("Rev", juce::dontSendNotification);
        revLbl.setJustificationType(juce::Justification::centred);
        revLbl.setFont(juce::Font(9.0f));
        revLbl.setColour(juce::Label::textColourId, knobThemeColour);

        auto& dlyLbl = localDelaySendLabels[(size_t)i];
        dlyLbl.setText("Dly", juce::dontSendNotification);
        dlyLbl.setJustificationType(juce::Justification::centred);
        dlyLbl.setFont(juce::Font(9.0f));
        dlyLbl.setColour(juce::Label::textColourId, knobThemeColour);

        auto& selector = localInputSelectors[(size_t)i];
        selector.clear(juce::dontSendNotification);

        for (int ch = 0; ch < totalInputs; ++ch)
            selector.addItem("In " + juce::String(ch + 1), ch + 1);

        int stereoBaseId = 100;
        for (int pair = 0; pair < numPairs; ++pair)
        {
            int left = pair * 2 + 1;
            int right = left + 1;
            selector.addItem(juce::String(left) + "/" + juce::String(right), stereoBaseId + pair);
        }

        const juce::String linkInputLabel = buildLinkAudioLocalInputLabel(audioProcessor);
        if (linkInputLabel.isNotEmpty())
            selector.addItem(linkInputLabel, kLocalInputLinkSelectorId);

        int currentInput = audioProcessor.getLocalChannelInput(i);
        if (audioProcessor.isLocalChannelUsingLinkAudioInput(i))
        {
            selector.setSelectedId(kLocalInputLinkSelectorId, juce::dontSendNotification);
        }
        else if (currentInput >= 0 && currentInput < totalInputs)
        {
            selector.setSelectedId(currentInput + 1, juce::dontSendNotification);
        }
        else if (currentInput < 0)
        {
            int pairIndex = -1 - currentInput;
            if (numPairs > pairIndex)
            {
                selector.setSelectedId(stereoBaseId + pairIndex, juce::dontSendNotification);
            }
            else if (numPairs > 0)
            {
                // Preferred pair unavailable, use first available stereo pair
                selector.setSelectedId(stereoBaseId, juce::dontSendNotification);
                audioProcessor.setLocalChannelInput(i, -1);
            }
            else if (totalInputs > 0)
            {
                // No stereo pairs at all, fall back to mono channel 1
                selector.setSelectedId(1, juce::dontSendNotification);
                audioProcessor.setLocalChannelInput(i, 0);
            }
        }

        selector.onChange = [this, i]
        {
            int id = localInputSelectors[(size_t)i].getSelectedId();
            if (id <= 0)
                return;

            int total = audioProcessor.getTotalNumInputChannels();
            if (total <= 0)
                total = 2;
            int numPairsLocal = total / 2;
            int stereoBase = 100;

            if (id >= 1 && id <= total)
            {
                audioProcessor.setLocalChannelUsesLinkAudioInput(i, false);
                audioProcessor.setLocalChannelInput(i, id - 1);
                applyRemoteMidiRelaySelection(i, id - 1);
                localInputModeSelectors[(size_t)i].setSelectedId(1, juce::dontSendNotification);
            }
            else if (id >= stereoBase && id < stereoBase + numPairsLocal)
            {
                int pairIndex = id - stereoBase;
                audioProcessor.setLocalChannelUsesLinkAudioInput(i, false);
                audioProcessor.setLocalChannelInput(i, -1 - pairIndex);
                applyRemoteMidiRelaySelection(i, -1 - pairIndex);
                localInputModeSelectors[(size_t)i].setSelectedId(2, juce::dontSendNotification);
            }
            else if (id == kLocalInputLinkSelectorId)
            {
                audioProcessor.setLocalChannelUsesLinkAudioInput(i, true);
                localInputModeSelectors[(size_t)i].setSelectedId(2, juce::dontSendNotification);
            }
        };

        auto& modeSelector = localInputModeSelectors[(size_t)i];
        modeSelector.addItem("Mono", 1);
        modeSelector.addItem("Stereo", 2);
        modeSelector.setSelectedId(audioProcessor.isLocalChannelUsingLinkAudioInput(i) || currentInput < 0 ? 2 : 1,
                                   juce::dontSendNotification);
    }

    addAndMakeVisible(masterFaderLabel);
    masterFaderLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(masterFader);
    masterFader.setSliderStyle(juce::Slider::LinearVertical);
    masterFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    masterFader.setRange(0.0, 2.0);
    masterFader.setSkewFactorFromMidPoint(0.25);
    masterFader.setSliderSnapsToMousePosition(false);
    masterFader.setValue(1.0, juce::dontSendNotification);
    masterFader.setDoubleClickReturnValue(true, 1.0);
    masterFader.setLookAndFeel(&mixerFaderLookAndFeel);
    masterFader.onValueChange = [this]
    {
        audioProcessor.setMasterOutputGain((float)masterFader.getValue());
    };
    registerMidiLearnTarget(masterFader, "master.fader", false);
    addAndMakeVisible(masterPeakMeter);
    addAndMakeVisible(masterDbLabel);
    masterDbLabel.setFont(juce::Font(9.0f));
    masterDbLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(limiterButton);
    addAndMakeVisible(limiterReleaseLabel);
    limiterReleaseLabel.setJustificationType(juce::Justification::centred);
    limiterReleaseLabel.setTooltip("Limiter Release Amount");
    limiterButton.setClickingTogglesState(true);
    limiterButton.setTooltip("Master Limiter Gain");
    limiterButton.setToggleState(false, juce::dontSendNotification);
    limiterButton.onClick = [this]
    {
        audioProcessor.setMasterLimiterEnabled(limiterButton.getToggleState());
        updateLimiterButtonColor();
    };
    updateLimiterButtonColor();
    addAndMakeVisible(limiterThresholdSlider);
    limiterThresholdSlider.setSliderStyle(juce::Slider::LinearVertical);
    limiterThresholdSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    limiterThresholdSlider.setRange(-6.0, 0.0);
    limiterThresholdSlider.setValue(0.0, juce::dontSendNotification);
    limiterThresholdSlider.setLookAndFeel(&mixerFaderLookAndFeel);
    limiterThresholdSlider.onValueChange = [this]
    {
        audioProcessor.setLimiterThreshold((float)limiterThresholdSlider.getValue());
    };
    registerMidiLearnTarget(limiterThresholdSlider, "limiter.threshold", false);

    addAndMakeVisible(limiterReleaseSlider);
    limiterReleaseSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    limiterReleaseSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    limiterReleaseSlider.setRange(10.0, 1000.0);
    limiterReleaseSlider.setValue(100.0, juce::dontSendNotification);
    limiterReleaseSlider.setTooltip("Limiter Release Amount");
    limiterReleaseSlider.setLookAndFeel(&customKnobLookAndFeel);
    limiterReleaseSlider.onValueChange = [this]
    {
        audioProcessor.setLimiterRelease((float)limiterReleaseSlider.getValue());
    };
    registerMidiLearnTarget(limiterReleaseSlider, "limiter.release", false);

    addAndMakeVisible(delayTimeLabel);
    delayTimeLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(delayTimeSlider);
    delayTimeSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    delayTimeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    delayTimeSlider.setRange(20.0, 10000.0);
    delayTimeSlider.setValue(audioProcessor.getFxDelayTimeMs(), juce::dontSendNotification);
    delayTimeSlider.setLookAndFeel(&customKnobLookAndFeel);
    delayTimeSlider.onValueChange = [this]
    {
        audioProcessor.setFxDelayTimeMs((float)delayTimeSlider.getValue());
    };
    registerMidiLearnTarget(delayTimeSlider, "fx.delay.time", false);

    addAndMakeVisible(delayDivisionSelector);
    delayDivisionSelector.addItem("1/16", 16);
    delayDivisionSelector.addItem("1/8", 8);
    delayDivisionSelector.addItem("1/1", 1);
    delayDivisionSelector.setSelectedId(audioProcessor.getFxDelayDivision(), juce::dontSendNotification);
    delayDivisionSelector.onChange = [this]
    {
        int division = delayDivisionSelector.getSelectedId();
        if (division <= 0)
            division = 8;
        audioProcessor.setFxDelayDivision(division);
    };

    addAndMakeVisible(delayPingPongButton);
    delayPingPongButton.setClickingTogglesState(true);
    delayPingPongButton.setToggleState(audioProcessor.isFxDelayPingPong(), juce::dontSendNotification);
    delayPingPongButton.onClick = [this]
    {
        audioProcessor.setFxDelayPingPong(delayPingPongButton.getToggleState());
    };
    registerMidiLearnTarget(delayPingPongButton, "fx.delay.pingpong", true);

    addAndMakeVisible(chatDisplay);
    chatDisplay.setMultiLine(true);
    chatDisplay.setReadOnly(true);
    chatDisplay.setFont(juce::Font(14.0f));
    chatDisplay.setCommandLinkCallback([this](const juce::String& command)
    {
        audioProcessor.sendChatMessage(command);
    });

    addAndMakeVisible(chatInput);
    chatInput.onReturnKey = [this] { sendClicked(); };

    addAndMakeVisible(chatEmojiButton);
    chatEmojiButton.setButtonText("+");
    chatEmojiButton.setTooltip("GIFs, images, and emoji");
    chatEmojiButton.onClick = [this]
    {
        showChatAttachmentMenu(audioProcessor, chatInput, chatEmojiButton, [this]
        {
            if (gifPickerPanel == nullptr)
                return;

            if (gifPickerPanel->isVisible())
            {
                gifPickerPanel->setVisible(false);
                return;
            }

            gifPickerPanel->openWithSearchText({});
            resized();
        },
        [this](const juce::String&)
        {
            markPersistentSettingsDirty();
        },
        [this](const juce::String& key)
        {
            setChatWindowColourKey(key, true);
        },
        chatWindowColourKey,
        chatTtsEnabled,
        chatTtsVoiceId,
        chatTtsVoiceIds,
        chatTtsVoiceNames,
        chatTtsVolume,
        chatTtsOutputId,
        chatTtsOutputIds,
        chatTtsOutputNames,
        [this](bool enabled)
        {
            setChatTtsEnabled(enabled, true);
        },
        [this](const juce::String& voiceId)
        {
            setChatTtsVoiceId(voiceId, true);
        },
        [this](float volume)
        {
            setChatTtsVolume(volume, true);
        },
        [this](const juce::String& outputId)
        {
            setChatTtsOutputId(outputId, true);
        });
    };

    addAndMakeVisible(sendButton);
    sendButton.setButtonText(juce::String::fromUTF8("\xE2\x86\xB5"));
    sendButton.setTooltip("Send");
    sendButton.onClick = [this] { sendClicked(); };

    gifPickerPanel = std::make_unique<GifPickerPanel>([this](const juce::String& url)
    {
        audioProcessor.sendChatAttachment("gif", url);
        if (gifPickerPanel != nullptr)
            gifPickerPanel->setVisible(false);
    });
    addChildComponent(*gifPickerPanel);
    addMouseListener(this, true);
    chatTtsEngine = std::make_unique<ChatTtsEngine>();
    chatTtsVoiceIds.add({});
    chatTtsVoiceNames.add("System Default");
    chatTtsOutputIds.add({});
    chatTtsOutputNames.add("System Default");
    {
        juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
        std::thread([safeThis]
        {
            juce::StringArray platformVoiceIds;
            juce::StringArray platformVoiceNames;
            juce::StringArray platformOutputIds;
            juce::StringArray platformOutputNames;
            ChatTtsEngine::getAvailableVoices(platformVoiceIds, platformVoiceNames);
            ChatTtsEngine::getAvailableOutputs(platformOutputIds, platformOutputNames);
            juce::MessageManager::callAsync([safeThis,
                                             platformVoiceIds = std::move(platformVoiceIds),
                                             platformVoiceNames = std::move(platformVoiceNames),
                                             platformOutputIds = std::move(platformOutputIds),
                                             platformOutputNames = std::move(platformOutputNames)]() mutable
            {
                if (safeThis == nullptr)
                    return;

                const int voiceCount = juce::jmin(platformVoiceIds.size(), platformVoiceNames.size());
                for (int i = 0; i < voiceCount; ++i)
                {
                    if (platformVoiceIds[i].isNotEmpty() && !safeThis->chatTtsVoiceIds.contains(platformVoiceIds[i]))
                    {
                        safeThis->chatTtsVoiceIds.add(platformVoiceIds[i]);
                        safeThis->chatTtsVoiceNames.add(platformVoiceNames[i]);
                    }
                }

                const int outputCount = juce::jmin(platformOutputIds.size(), platformOutputNames.size());
                for (int i = 0; i < outputCount; ++i)
                {
                    if (platformOutputIds[i].isNotEmpty() && !safeThis->chatTtsOutputIds.contains(platformOutputIds[i]))
                    {
                        safeThis->chatTtsOutputIds.add(platformOutputIds[i]);
                        safeThis->chatTtsOutputNames.add(platformOutputNames[i]);
                    }
                }
            });
        }).detach();
    }

    addAndMakeVisible(atButton);
    atButton.setClickingTogglesState(true);
    atButton.setWantsKeyboardFocus(false);
    atButton.setLookAndFeel(&atBtnLAF);
    atButton.onClick = [this] { atToggled(); };
    atButton.onPopupMenuRequest = [this] { showTranslateLanguageMenu(atButton); };
    updateTranslateButtonState();

    addAndMakeVisible(chatPopoutButton);
    chatPopoutButton.setButtonText("Popout");
    chatPopoutButton.onClick = [this] { chatPopoutClicked(); };

    addAndMakeVisible(videoBgToggle);
    videoBgToggle.setToggleState(true, juce::dontSendNotification);
    videoBgToggle.onClick = [this]
    {
        int idx = backgroundSelector.getSelectedItemIndex();
        if (idx >= 0 && idx < textureFiles.size())
            loadControlImages(textureFiles[idx]);
        markPersistentSettingsDirty();
    };

    addAndMakeVisible(backgroundSelector);
    backgroundSelector.setTooltip("Skin");
    {
        juce::Array<juce::File> roots;
        roots.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());
        {
            const auto moduleFile = getThisModuleFile();
            if (moduleFile.existsAsFile())
                roots.addIfNotAlreadyThere(moduleFile.getParentDirectory());
        }

        juce::File texturesDir;
        for (const auto& root : roots)
        {
            juce::File probe = root;
            for (int i = 0; i < 8; ++i)
            {
                const juce::File a = probe.getChildFile("textures");
                const juce::File b = probe.getChildFile("Resources").getChildFile("textures");
                const juce::File c = probe.getParentDirectory().getChildFile("Resources").getChildFile("textures");
                if (a.isDirectory()) { texturesDir = a; break; }
                if (b.isDirectory()) { texturesDir = b; break; }
                if (c.isDirectory()) { texturesDir = c; break; }
                probe = probe.getParentDirectory();
            }
            if (texturesDir.isDirectory())
                break;
        }

        if (texturesDir.isDirectory())
        {
            // Each subdirectory is a theme; its name shows in the dropdown
            auto dirs = texturesDir.findChildFiles(juce::File::findDirectories, false);
            dirs.sort();
            for (int i = 0; i < dirs.size(); ++i)
            {
                if (dirs[i].getFileName().equalsIgnoreCase("Skin Template"))
                    continue;
                // Only include dirs that contain a bg.* file
                auto bgFiles = dirs[i].findChildFiles(juce::File::findFiles, false, "bg.*");
                if (bgFiles.isEmpty()) continue;
                textureFiles.add(dirs[i]);
                backgroundSelector.addItem(dirs[i].getFileName(), textureFiles.size());
            }
        }
        if (backgroundSelector.getNumItems() == 0)
            backgroundSelector.addItem("Default", 1);

        // Determine which texture to select: saved preference > "Brushed Metal 1" > first item
        auto popts = makeSettingsOptions();
        juce::String savedTexture;
        if (settingsFileReady)
        {
            juce::PropertiesFile props(popts);
            savedTexture = props.getValue("texture", "");
            abletonWindowSizePreset = juce::jlimit(0, 2, props.getIntValue("abletonWindowSizePreset", 1));
        }

        int selectIdx = -1;
        if (savedTexture.isNotEmpty())
            for (int i = 0; i < textureFiles.size(); ++i)
                if (textureFiles[i].getFileName() == savedTexture) { selectIdx = i; break; }
        if (selectIdx < 0)
            for (int i = 0; i < textureFiles.size(); ++i)
                if (textureFiles[i].getFileName() == "Brushed Metal 1") { selectIdx = i; break; }
        if (selectIdx < 0 && backgroundSelector.getNumItems() > 0)
            selectIdx = 0;

        if (selectIdx >= 0)
            backgroundSelector.setSelectedId(backgroundSelector.getItemId(selectIdx), juce::dontSendNotification);

        // Load the selected texture immediately
        if (selectIdx >= 0 && selectIdx < textureFiles.size())
            loadControlImages(textureFiles[selectIdx]);
    }
    backgroundSelector.onChange = [this]
    {
        int idx = backgroundSelector.getSelectedItemIndex();
        if (idx >= 0 && idx < textureFiles.size())
        {
            // Persist the user's choice
            auto popts = makeSettingsOptions();
            juce::PropertiesFile props(popts);
            props.setValue("texture", textureFiles[idx].getFileName());
            props.saveIfNeeded();

            loadControlImages(textureFiles[idx]);
        }
        else
        {
            backgroundImage  = juce::Image();
            backgroundComponent.setBackgroundImage({});
            radioKnobImage   = juce::Image();
            faderKnobImage   = juce::Image();
            metronomeThemeColour = juce::Colour::fromRGB(80, 185, 255);
            windowThemeColour    = juce::Colour(0x00000000);
            applyThemeColours();
        }
        markPersistentSettingsDirty();
        repaint();
    };

    addAndMakeVisible(intervalDisplay);
    registerMidiLearnTarget(metronomeSlider, "metronome.level", false);
    registerMidiLearnTarget(transmitButton, "button.transmit", true);
    registerMidiLearnTarget(localMonitorButton, "button.monitor", true);
    registerMidiLearnTarget(voiceChatButton, "button.voicechat", true);
    registerMidiLearnTarget(metronomeMuteButton, "button.metronomemute", true);
    registerMidiLearnTarget(syncButton, "button.sync", true);
    registerMidiLearnTarget(chatButton, "button.chat", true);
    registerMidiLearnTarget(spreadOutputsButton, "button.spreadoutputs", true);
    registerMidiLearnTarget(autoLevelButton, "button.autolevel", true);
    registerMidiLearnTarget(limiterButton, "button.limiter", true);
    for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
    {
        NinjamVst3AudioProcessorEditor::MidiLearnTarget target;
        target.id = getSamplePadLearnTargetId(pad);
        target.component = nullptr;
        target.isToggle = true;
        midiTargetsById[target.id] = target;
    }
    for (int slot = 0; slot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++slot)
    {
        NinjamVst3AudioProcessorEditor::MidiLearnTarget target;
        target.id = getSampleFxKnobLearnTargetId(slot);
        target.component = nullptr;
        target.isToggle = false;
        midiTargetsById[target.id] = target;
    }
    syncUserStripMidiTargets();
    updateFxControlsVisibility();
    loadLearnMappingsFromProcessor();
    refreshExternalMidiInputDevices();

    // Startup default: show embedded chat on first open for both plugin and standalone.
    chatPoppedOut = false;
    chatButton.setToggleState(true, juce::dontSendNotification);
    updateChatButtonColor();

    // Initialize chat display from processor state so reopening the GUI reflects current history
    {
        const juce::ScopedLock lock(audioProcessor.chatLock);
        applyColoredChat(chatDisplay, audioProcessor.chatHistory, audioProcessor.chatSenders, audioProcessor);
        lastChatRevision = audioProcessor.chatRevision.load();
        lastChatTtsHistorySize = audioProcessor.chatHistory.size();
    }

    // Initialize metronome UI from processor state so mute/volume persist across GUI reopen
    {
        storedMetronomeVolume = audioProcessor.getStoredMetronomeVolume();
        const float vol = audioProcessor.getMetronomeVolume();
        if (audioProcessor.isMetronomeMuted())
        {
            metronomeMuteButton.setToggleState(false, juce::dontSendNotification); // muted
            metronomeSlider.setValue(0.0, juce::dontSendNotification);
        }
        else
        {
            metronomeMuteButton.setToggleState(true, juce::dontSendNotification); // unmuted
            metronomeSlider.setValue(vol, juce::dontSendNotification);
        }
        updateMetronomeButtonColor();
    }

    if (settingsFileReady)
        loadPersistentSettingsFromDisk();
    updateSamplePadsFeatureVisibility();
    refreshExternalMidiInputDevices();
    lastPersistentSettingsSaveMs = juce::Time::getMillisecondCounterHiRes();
    lastSavedUiSettingsFingerprint = buildPersistentSettingsFingerprint(false);
    persistentSettingsDirty = false;

    updateEditorTimerInterval();
    automaticUpdateCheckDueMs = juce::Time::getMillisecondCounterHiRes() + 4500.0;

    if (!audioProcessor.isStandaloneWrapper() && isAbletonLiveHost())
    {
        abletonWindowSizePreset = juce::jlimit(0, 2, abletonWindowSizePreset);
        const auto targetSize = abletonEditorWindowSizeForPreset(abletonWindowSizePreset);
        pendingDeferredResizeLayout = false;
        applyingDeferredResizeLayout = false;
        setResizable(false, false);
        setResizeLimits(targetSize.getWidth(),
                        targetSize.getHeight(),
                        targetSize.getWidth(),
                        targetSize.getHeight());
        suppressHeavyUiUntilMs = juce::Time::getMillisecondCounterHiRes() + 400.0;
        hostResizeLockedForConnection = true;
        setSize(targetSize.getWidth(), targetSize.getHeight());
    }
    else
    {
        setSize(getWidth() > 0 ? getWidth() : 1080,
                getHeight() > 0 ? getHeight() : 600);
    }
}

NinjamVst3AudioProcessorEditor::~NinjamVst3AudioProcessorEditor()
{
    savePersistentSettingsToDisk();

    // Stop periodic UI ticks first so timerCallback won't touch UI-owned resources
    stopTimer();
    chatTtsEngine.reset();

#if JUCE_WINDOWS
    if (videoFrameReader != nullptr)
    {
        videoFrameReader->signalThreadShouldExit();
        // brief wait for the background decoder thread to stop (avoid long UI blocking)
        videoFrameReader->stopThread(200);
        videoFrameReader.reset();
    }
#endif

    for (auto& pair : midiTargetsByComponent)
        if (pair.first != nullptr)
            pair.first->removeMouseListener(this);
    midiTargetsByComponent.clear();
    midiTargetsById.clear();
    midiSourceByTargetId.clear();
    oscSourceByTargetId.clear();
    midiLearnArmedTargetId.clear();
    oscLearnArmedTargetId.clear();
    midiLearnInputDevice.reset();
    midiRelayInputDevice.reset();
    samplePadsMidiInputDevice.reset();
    openedMidiLearnInputDeviceId.clear();
    openedMidiRelayInputDeviceId.clear();
    openedSamplePadsMidiInputDeviceId.clear();
    samplePadsWindow.reset();
    aboutWindow.reset();
    disconnect();
    atButton.setLookAndFeel(nullptr);
    chatButton.setLookAndFeel(nullptr);
    samplePadsButton.setLookAndFeel(nullptr);
    metronomeMuteButton.setLookAndFeel(nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
}

void NinjamVst3AudioProcessorEditor::setStandaloneOptionsMenuHandler(std::function<void(juce::Component*)> handler)
{
    standaloneOptionsMenuHandler = handler;
}

void NinjamVst3AudioProcessorEditor::stopBackgroundVideoReader()
{
#if JUCE_WINDOWS
    videoFrameReader.reset();
#endif
}

void NinjamVst3AudioProcessorEditor::armSamplePadMidiLearn(int padIndex)
{
    if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
        return;

    NinjamVst3AudioProcessorEditor::MidiLearnTarget target;
    target.id = getSamplePadLearnTargetId(padIndex);
    target.component = nullptr;
    target.isToggle = true;
    midiTargetsById[target.id] = target;
    midiLearnArmedTargetId = target.id;
}

void NinjamVst3AudioProcessorEditor::forgetSamplePadMidiLearn(int padIndex)
{
    if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
        return;

    const auto targetId = getSamplePadLearnTargetId(padIndex);
    midiSourceByTargetId.erase(targetId);
    if (midiLearnArmedTargetId == targetId)
        midiLearnArmedTargetId.clear();
    syncLearnMappingsToProcessor();
}

void NinjamVst3AudioProcessorEditor::armSamplePadOscLearn(int padIndex)
{
    if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
        return;

    NinjamVst3AudioProcessorEditor::MidiLearnTarget target;
    target.id = getSamplePadLearnTargetId(padIndex);
    target.component = nullptr;
    target.isToggle = true;
    midiTargetsById[target.id] = target;
    oscLearnArmedTargetId = target.id;
}

void NinjamVst3AudioProcessorEditor::forgetSamplePadOscLearn(int padIndex)
{
    if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
        return;

    const auto targetId = getSamplePadLearnTargetId(padIndex);
    oscSourceByTargetId.erase(targetId);
    if (oscLearnArmedTargetId == targetId)
        oscLearnArmedTargetId.clear();
    syncLearnMappingsToProcessor();
}

bool NinjamVst3AudioProcessorEditor::hasSamplePadMidiLearn(int padIndex) const
{
    if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
        return false;
    return midiSourceByTargetId.find(getSamplePadLearnTargetId(padIndex)) != midiSourceByTargetId.end();
}

bool NinjamVst3AudioProcessorEditor::hasSamplePadOscLearn(int padIndex) const
{
    if (padIndex < 0 || padIndex >= NinjamVst3AudioProcessor::numSamplePads)
        return false;
    return oscSourceByTargetId.find(getSamplePadLearnTargetId(padIndex)) != oscSourceByTargetId.end();
}

void NinjamVst3AudioProcessorEditor::registerSamplerFxKnobLearnTarget(juce::Component& component, int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= NinjamVst3AudioProcessor::numSamplePadFxSlots)
        return;

    const auto targetId = getSampleFxKnobLearnTargetId(slotIndex);
    MidiLearnTarget target;
    target.id = targetId;
    target.component = &component;
    target.isToggle = false;
    midiTargetsByComponent[&component] = target;
    midiTargetsById[targetId] = target;
    component.removeMouseListener(this);
}

void NinjamVst3AudioProcessorEditor::unregisterSamplerFxKnobLearnTarget(juce::Component& component, int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= NinjamVst3AudioProcessor::numSamplePadFxSlots)
        return;

    component.removeMouseListener(this);
    midiTargetsByComponent.erase(&component);

    const auto targetId = getSampleFxKnobLearnTargetId(slotIndex);
    auto it = midiTargetsById.find(targetId);
    if (it != midiTargetsById.end() && it->second.component == &component)
        it->second.component = nullptr;
}

void NinjamVst3AudioProcessorEditor::paint (juce::Graphics& g)
{
    juce::ignoreUnused(g);
}

void NinjamVst3AudioProcessorEditor::paintOverChildren(juce::Graphics& g)
{
    const bool abletonHostEditor = !audioProcessor.isStandaloneWrapper() && isAbletonLiveHost();

    // Helper: draw a small tight radial glow around any toggle button
    auto drawGlow = [&](juce::Button& btn, juce::Colour onColour, juce::Colour offColour)
    {
        if (!btn.isVisible()) return;
        bool isOn = btn.getToggleState();
        auto bc   = btn.getBounds().toFloat();
        auto centre = bc.getCentre();
        // Glow starts a few px outside the button edge
        float gap = 5.0f;
        float r   = bc.getWidth() * 0.55f + gap;   // compact radius
        juce::Colour col = isOn ? onColour : offColour;
        juce::ColourGradient grad(col, centre.x, centre.y,
                                  juce::Colours::transparentBlack, centre.x + r, centre.y, true);
        g.setGradientFill(grad);
        g.fillEllipse(centre.x - r, centre.y - r, r * 2.0f, r * 2.0f);
    };

    if (!(abletonHostEditor && !transmitButton.getToggleState()))
        drawGlow(transmitButton, juce::Colour(0x5532cc60), juce::Colour(0x22154420));  // green
    drawGlow(localMonitorButton, juce::Colour(0x55ff3232), juce::Colour(0x22441515));  // red
    drawGlow(autoLevelButton,    juce::Colour(0x55ffdd20), juce::Colour(0x22443a10));  // yellow
    drawGlow(limiterButton,      juce::Colour(0x55ff3232), juce::Colour(0x22441515));  // red
    drawGlow(layoutButton,       juce::Colour(0x5520c8e8), juce::Colour(0x220a3240));  // teal
    drawGlow(metronomeMuteButton,
             metronomeThemeColour.withAlpha(0.33f),
             metronomeThemeColour.withMultipliedBrightness(0.10f).withAlpha(0.13f)); // themed
    drawGlow(syncButton,          juce::Colour(0x55ff9820), juce::Colour(0x22301808)); // orange
    if (atButton.isVisible())
        drawGlow(atButton,        juce::Colour(0x5550c8ff), juce::Colour(0x220a2840)); // sky blue
    const bool chatShowing = chatButton.getToggleState()
        && (!chatPoppedOut || chatPopoutOpenPending || (chatWindow && chatWindow->isVisible()));
    if (chatShowing)
        drawGlow(chatButton,      juce::Colour(0x5550c8ff), juce::Colours::transparentBlack); // sky blue

    auto drawClipPulse = [&](juce::Rectangle<int> bounds, bool active)
    {
        if (!active || bounds.isEmpty())
            return;

        const double phase = juce::Time::getMillisecondCounterHiRes() * 0.001
                           * (juce::MathConstants<double>::twoPi * 0.85);
        const float pulse = 0.5f + 0.5f * (float)std::sin(phase);
        const float alpha = juce::jmap(pulse, 0.32f, 0.88f);
        auto r = bounds.toFloat().reduced(1.0f);

        g.setColour(juce::Colour(0xffff2424).withAlpha(alpha));
        g.drawRoundedRectangle(r, 5.0f, 2.0f + pulse * 1.4f);
        g.setColour(juce::Colour(0xffff2424).withAlpha(alpha * 0.35f));
        g.drawRoundedRectangle(r.expanded(2.0f), 7.0f, 1.0f);
    };

    for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
        drawClipPulse(localChannelPulseBounds[(size_t)i], localClipPulsing[(size_t)i]);
    drawClipPulse(masterChannelPulseBounds, masterClipPulsing);

    if (transmitButton.isVisible() && !transmitButton.getToggleState())
    {
        const double nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (abletonHostEditor)
        {
            const bool flashOn = std::fmod(nowSeconds, 1.0) < 0.5;
            if (flashOn)
            {
                const auto r = transmitButton.getBounds().toFloat().expanded(3.0f);
                g.setColour(juce::Colours::white.withAlpha(0.82f));
                g.drawRoundedRectangle(r, 6.0f, 2.0f);
            }
            return;
        }

        const double phase = nowSeconds * (juce::MathConstants<double>::twoPi * 0.5);
        const float pulse = 0.5f + 0.5f * (float)std::sin(phase);
        const float alpha = juce::jmap(pulse, 0.20f, 0.82f);
        const auto r = transmitButton.getBounds().toFloat().expanded(3.0f);

        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.drawRoundedRectangle(r, 6.0f, 2.0f);
        g.setColour(juce::Colours::white.withAlpha(alpha * 0.45f));
        g.drawRoundedRectangle(r.expanded(1.5f), 7.0f, 1.0f);

        // Keep the label glow in phase with the outline pulse.
        g.setColour(juce::Colours::white.withAlpha(alpha));
        g.setFont(getLookAndFeel().getTextButtonFont(transmitButton, transmitButton.getHeight()));
        g.drawText(transmitButton.getButtonText(), transmitButton.getBounds(), juce::Justification::centred, false);
    }
}

void NinjamVst3AudioProcessorEditor::resized()
{
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    backgroundComponent.setBounds(getLocalBounds());
    backgroundComponent.toBack();

    if (!audioProcessor.isStandaloneWrapper() && isAbletonLiveHost() && !applyingDeferredResizeLayout)
    {
        const bool hasCompletedInitialLayout = lastLaidOutEditorWidth > 0 && lastLaidOutEditorHeight > 0;
        if (hasCompletedInitialLayout)
        {
            if (getWidth() == lastLaidOutEditorWidth && getHeight() == lastLaidOutEditorHeight)
                return;
            pendingDeferredResizeLayout = true;
            lastResizeEventMs = juce::Time::getMillisecondCounterHiRes();
            return;
        }
    }

    auto area = getLocalBounds().reduced(10);

    // Bottom: Interval Display
    auto bottomRow = area.removeFromBottom(40);
    intervalDisplay.setBounds(bottomRow);
    area.removeFromBottom(10);

    auto topRow = area.removeFromTop(30);
    // Right side of top row: texture / video-bg controls only
    backgroundSelector.setBounds(topRow.removeFromRight(150));
    topRow.removeFromRight(4);
    videoBgToggle.setBounds(topRow.removeFromRight(90));
    topRow.removeFromRight(10);
    // Left side: server fields
    serverLabel.setBounds(topRow.removeFromLeft(75));
    serverField.setBounds(topRow.removeFromLeft(160));
    topRow.removeFromLeft(6);
    serverListButton.setBounds(topRow.removeFromLeft(72));
    topRow.removeFromLeft(6);
    userLabel.setBounds(topRow.removeFromLeft(55));
    userField.setBounds(topRow.removeFromLeft(90));
    topRow.removeFromLeft(6);
    anonymousButton.setBounds(topRow.removeFromLeft(110));
    if (!anonymousButton.getToggleState())
    {
        topRow.removeFromLeft(6);
        passLabel.setBounds(topRow.removeFromLeft(76));
        passField.setBounds(topRow.removeFromLeft(96));
        topRow.removeFromLeft(6);
    }
    connectButton.setBounds(topRow.removeFromLeft(80));
    topRow.removeFromLeft(10);
    statusLabel.setBounds(topRow);

    area.removeFromTop(4);

    // Controls Row: layout, auto-level, metronome, tempo — chat+video buttons on the right
    auto controlsRow = area.removeFromTop(30);
    videoButton.setBounds(controlsRow.removeFromRight(100));
    controlsRow.removeFromRight(5);
    if (samplePadsButton.isVisible())
    {
        samplePadsButton.setBounds(controlsRow.removeFromRight(42));
        controlsRow.removeFromRight(5);
    }
    else
    {
        samplePadsButton.setBounds({});
    }
    chatButton.setBounds(controlsRow.removeFromRight(80));
    controlsRow.removeFromRight(10);
    layoutButton.setBounds(controlsRow.removeFromLeft(40));  // icon-only button
    controlsRow.removeFromLeft(10);
    autoLevelButton.setBounds(controlsRow.removeFromLeft(110));
    controlsRow.removeFromLeft(10);
    metronomeLabel.setBounds(controlsRow.removeFromLeft(90));
    metronomeSlider.setBounds(controlsRow.removeFromLeft(80));
    auto metBtn = controlsRow.removeFromLeft(24);
    metronomeMuteButton.setBounds(metBtn.reduced(0, 3));
    controlsRow.removeFromLeft(6);
    auto syncBtn = controlsRow.removeFromLeft(24);
    syncButton.setBounds(syncBtn.reduced(0, 3));
    controlsRow.removeFromLeft(10);
    fxButton.setBounds(controlsRow.removeFromLeft(70));
    controlsRow.removeFromLeft(8);
    optionsButton.setBounds(controlsRow.removeFromLeft(78));
    controlsRow.removeFromLeft(8);
    aboutButton.setBounds(controlsRow.removeFromLeft(24));
    controlsRow.removeFromLeft(8);
    tempoLabel.setBounds(controlsRow);

    area.removeFromTop(10);

    // Keep chat state coherent: if the popout is not actually visible,
    // treat chat as docked when toggled on.
    if (chatPoppedOut && !chatPopoutOpenPending && (!chatWindow || !chatWindow->isVisible()))
        chatPoppedOut = false;

    bool showDockedChat = chatButton.getToggleState() && !chatPoppedOut;
    juce::Rectangle<int> chatArea;
    if (showDockedChat)
    {
        auto chatWidth = (int)(area.getWidth() * 0.20f);
        chatArea = area.removeFromRight(chatWidth);
        area.removeFromRight(10);
    }

    int numLocal = audioProcessor.getNumLocalChannels();
    numLocal = juce::jlimit(1, NinjamVst3AudioProcessor::maxLocalChannels, numLocal);

    int baseLocalWidth = 110;
    int extraPerTrack = 40;
    int localWidth = baseLocalWidth + (numLocal - 1) * extraPerTrack;
    int maxLocalWidth = area.getWidth() / 2;
    if (localWidth > maxLocalWidth)
        localWidth = maxLocalWidth;

    int masterWidth = 190;

    auto localArea = area.removeFromLeft(localWidth);
    auto masterArea = area.removeFromRight(masterWidth);
    auto masterPulseBounds = masterArea;
    auto userArea = area;

    auto usersHeader = userArea.removeFromTop(22);
    // Keep a clear visual gap from the local/transmit area while anchoring
    // Spread Outputs to the right above the remote-user pane.
    usersHeader.removeFromLeft(12);
    auto spreadArea = usersHeader.removeFromRight(118);
    spreadOutputsButton.setBounds(spreadArea.withTrimmedTop(1));
    usersHeader.removeFromRight(14);
    usersLabel.setBounds(usersHeader.removeFromLeft(170));
    userList.setBounds(userArea);

    // Transmit above local channels, monitor below it
    transmitButton.setBounds(localArea.removeFromTop(26));
    localArea.removeFromTop(3);
    localMonitorButton.setBounds(localArea.removeFromTop(26));
    localArea.removeFromTop(3);
    {
        auto row = localArea.removeFromTop(26);
        auto third = row.getWidth() / 3;
        voiceChatButton.setBounds(row.removeFromLeft(third));
        bitrateSelector.setBounds(row.removeFromLeft(third));
        midiRelayTargetSelector.setBounds(row);
    }
    localArea.removeFromTop(3);

    localChordLabel.setBounds(localArea.removeFromTop(24));
    localArea.removeFromTop(3);

    auto localHeader = localArea.removeFromTop(20);
    addLocalChannelButton.setBounds(localHeader.removeFromLeft(20));
    removeLocalChannelButton.setBounds(localHeader.removeFromLeft(20));
    localFaderLabel.setVisible(true);
    const bool showChordStats = localHeader.getWidth() >= 160;
    localChordStatsLabel.setVisible(showChordStats);
    localChordStatsLabel.setBounds(showChordStats ? localHeader.removeFromRight(84) : juce::Rectangle<int>());
    localFaderLabel.setBounds(localHeader);
    auto localInner = localArea.reduced(4);

    int meterWidth = 18;
    int totalWidth = localInner.getWidth();
    int columnWidth = totalWidth / numLocal;

    for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
    {
        bool visible = i < numLocal;
        localFaders[(size_t)i].setVisible(visible);
        localPeakMeters[(size_t)i].setVisible(visible);
        localInputSelectors[(size_t)i].setVisible(visible);
        localInputModeSelectors[(size_t)i].setVisible(visible);
        localDbLabels[(size_t)i].setVisible(visible);
        localChannelNameLabels[(size_t)i].setVisible(visible);
        localReverbSendKnobs[(size_t)i].setVisible(visible);
        localDelaySendKnobs[(size_t)i].setVisible(visible);
        localReverbSendLabels[(size_t)i].setVisible(visible);
        localDelaySendLabels[(size_t)i].setVisible(visible);
        if (!visible)
            localChannelPulseBounds[(size_t)i] = {};
    }

    for (int i = 0; i < numLocal; ++i)
    {
        juce::Rectangle<int> col = localInner.removeFromLeft(columnWidth);
        const auto pulseBounds = col;
        auto meterArea = col.removeFromLeft(meterWidth);
        auto nameArea = col.removeFromTop(18);
        auto dbArea = col.removeFromBottom(16);
        auto inputArea = col.removeFromBottom(20);
        auto inputModeArea = col.removeFromBottom(20);
        auto sendArea = col.removeFromBottom(44);
        auto revArea = sendArea.removeFromLeft(sendArea.getWidth() / 2);
        auto dlyArea = sendArea;
        auto revLabelArea = revArea.removeFromTop(9);
        auto dlyLabelArea = dlyArea.removeFromTop(9);
        localFaders[(size_t)i].setBounds(col);
        localPeakMeters[(size_t)i].setBounds(meterArea);
        localInputSelectors[(size_t)i].setBounds(inputArea);
        localInputModeSelectors[(size_t)i].setBounds(inputModeArea);
        localDbLabels[(size_t)i].setBounds(dbArea);
        localChannelNameLabels[(size_t)i].setBounds(nameArea);
        localReverbSendLabels[(size_t)i].setBounds(revLabelArea);
        localDelaySendLabels[(size_t)i].setBounds(dlyLabelArea);
        localChannelPulseBounds[(size_t)i] = pulseBounds.expanded(1);

        auto revKnobArea = revArea.expanded(1);
        auto dlyKnobArea = dlyArea.expanded(1);

        int revKnobSize = juce::jmin(revKnobArea.getWidth(), revKnobArea.getHeight());
        int dlyKnobSize = juce::jmin(dlyKnobArea.getWidth(), dlyKnobArea.getHeight());

        juce::Rectangle<int> revKnobRect(0, 0, revKnobSize, revKnobSize);
        juce::Rectangle<int> dlyKnobRect(0, 0, dlyKnobSize, dlyKnobSize);
        revKnobRect = revKnobRect.withCentre(revKnobArea.getCentre());
        dlyKnobRect = dlyKnobRect.withCentre(dlyKnobArea.getCentre());

        localReverbSendKnobs[(size_t)i].setBounds(revKnobRect);
        localDelaySendKnobs[(size_t)i].setBounds(dlyKnobRect);
    }

    masterFaderLabel.setBounds(masterArea.removeFromTop(20));
    auto masterInner = masterArea.reduced(4);
    auto masterMeterWidth = 18;
    auto masterMeterArea = masterInner.removeFromRight(masterMeterWidth);
    auto controlColumn = masterInner.removeFromLeft(70);
    auto fxColumn = masterInner;

    limiterButton.setBounds(controlColumn.removeFromTop(20));

    int bottomHeight = 70;
    if (bottomHeight > controlColumn.getHeight())
        bottomHeight = controlColumn.getHeight();

    auto threshArea = controlColumn.removeFromTop(controlColumn.getHeight() - bottomHeight);
    limiterThresholdSlider.setBounds(threshArea);

    auto releaseBlock = controlColumn;
    limiterReleaseLabel.setBounds(releaseBlock.removeFromTop(18));

    auto knobArea = releaseBlock.reduced(6, 0);
    int knobSize = juce::jmin(knobArea.getWidth(), knobArea.getHeight());
    juce::Rectangle<int> knobRect(0, 0, knobSize, knobSize);
    knobRect = knobRect.withCentre(knobArea.getCentre());
    limiterReleaseSlider.setBounds(knobRect);

    auto delayBlock = fxColumn.removeFromTop(70);
    delayTimeLabel.setBounds(delayBlock.removeFromTop(16));
    auto delayKnobBounds = delayBlock.reduced(4);
    int delayKnobSize = juce::jmin(delayKnobBounds.getWidth(), delayKnobBounds.getHeight());
    delayTimeSlider.setBounds(juce::Rectangle<int>(delayKnobSize, delayKnobSize).withCentre(delayKnobBounds.getCentre()));

    fxColumn.removeFromTop(2);
    delayDivisionSelector.setBounds(fxColumn.removeFromTop(22));
    fxColumn.removeFromTop(2);
    delayPingPongButton.setBounds(fxColumn.removeFromTop(22));

    masterFader.setBounds(masterInner.removeFromTop(masterInner.getHeight() - 16));
    masterDbLabel.setBounds(masterInner);
    masterPeakMeter.setBounds(masterMeterArea);
    masterChannelPulseBounds = masterPulseBounds.expanded(1);

    if (showDockedChat)
    {
        chatDisplay.setVisible(true);
        chatInput.setVisible(true);
        chatEmojiButton.setVisible(true);
        sendButton.setVisible(true);
        atButton.setVisible(true);
        chatPopoutButton.setVisible(true);

        chatPopoutButton.setBounds(chatArea.removeFromTop(20).removeFromRight(70));

        auto chatInputArea = chatArea.removeFromBottom(30);
        chatArea.removeFromBottom(5);
        chatDisplay.setBounds(chatArea);

        sendButton.setBounds(chatInputArea.removeFromRight(26));
        chatInputArea.removeFromRight(1);
        atButton.setBounds(chatInputArea.removeFromRight(26));
        chatInputArea.removeFromRight(1);
        chatEmojiButton.setBounds(chatInputArea.removeFromRight(26));
        chatInputArea.removeFromRight(2);
        chatInput.setBounds(chatInputArea);

        if (gifPickerPanel != nullptr && gifPickerPanel->isVisible())
        {
            auto pickerBounds = chatDisplay.getBounds().reduced(4);
            pickerBounds = pickerBounds.removeFromBottom(juce::jmin(290, juce::jmax(180, pickerBounds.getHeight())));
            gifPickerPanel->setBounds(pickerBounds);
            gifPickerPanel->toFront(false);
        }
    }
    else
    {
        chatDisplay.setVisible(false);
        chatInput.setVisible(false);
        chatEmojiButton.setVisible(false);
        sendButton.setVisible(false);
        atButton.setVisible(false);
        chatPopoutButton.setVisible(false);
        if (gifPickerPanel != nullptr)
            gifPickerPanel->setVisible(false);
    }

    lastLaidOutEditorWidth = getWidth();
    lastLaidOutEditorHeight = getHeight();
}

void NinjamVst3AudioProcessorEditor::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const bool abletonHostEditor = !audioProcessor.isStandaloneWrapper() && isAbletonLiveHost();

    if (automaticUpdateCheckDueMs > 0.0 && nowMs >= automaticUpdateCheckDueMs)
    {
        automaticUpdateCheckDueMs = 0.0;
        if (!ninjamAutomaticUpdateCheckStarted.exchange(true))
            beginUpdateCheck(false);
    }

    const double transmitPulseRepaintMs = abletonHostEditor ? 250.0 : 33.0;
    if (transmitButton.isVisible()
        && !transmitButton.getToggleState()
        && (!abletonHostEditor || (!pendingDeferredResizeLayout && !applyingDeferredResizeLayout))
        && nowMs - lastTransmitPulseRepaintMs >= transmitPulseRepaintMs)
    {
        lastTransmitPulseRepaintMs = nowMs;
        repaint(transmitButton.getBounds().expanded(8));
    }

    if (persistentSettingsDirty && nowMs - lastPersistentSettingsSaveMs >= 1500.0)
    {
        savePersistentSettingsToDisk(false);
        lastPersistentSettingsSaveMs = nowMs;
    }

    if (pendingDeferredResizeLayout && !audioProcessor.isStandaloneWrapper() && isAbletonLiveHost())
    {
        if (nowMs - lastResizeEventMs >= 85.0)
        {
            pendingDeferredResizeLayout = false;
            applyingDeferredResizeLayout = true;
            resized();
            applyingDeferredResizeLayout = false;
            suppressHeavyUiUntilMs = nowMs + 350.0;
            repaint();
        }
        else
        {
            return;
        }
    }

    // Startup/layout safety: if Chat is toggled on for docked mode but
    // the embedded widgets are not visible, force one layout refresh and
    // clear any stale popout state.
    if (chatButton.getToggleState()
        && !chatPoppedOut
        && (!chatDisplay.isVisible() || chatDisplay.getWidth() <= 2 || chatDisplay.getHeight() <= 2))
    {
        chatPoppedOut = false;
        const bool wasApplyingDeferredLayout = applyingDeferredResizeLayout;
        applyingDeferredResizeLayout = true;
        resized();
        applyingDeferredResizeLayout = wasApplyingDeferredLayout;
        updateChatButtonColor();
    }

    int status = audioProcessor.getClient().GetStatus();
    updateHostResizeModeForConnectionStatus(status);
    const juce::String currentLinkAudioInputLabel = buildLinkAudioLocalInputLabel(audioProcessor);
    if (currentLinkAudioInputLabel != lastLinkAudioLocalInputLabel)
    {
        lastLinkAudioLocalInputLabel = currentLinkAudioInputLabel;
        refreshLocalInputSelectors();
    }
    juce::String statusStr;
    switch (status)
    {
        case NJClient::NJC_STATUS_DISCONNECTED: statusStr = "Disconnected"; break;
        case NJClient::NJC_STATUS_INVALIDAUTH:
        {
            const juce::String errorText = juce::String::fromUTF8(audioProcessor.getClient().GetErrorStr()).trim();
            statusStr = errorText.containsIgnoreCase("server full") ? "Server Full" : "Invalid Auth";
            break;
        }
        case NJClient::NJC_STATUS_CANTCONNECT:  statusStr = "Can't Connect"; break;
        case NJClient::NJC_STATUS_OK:           statusStr = "Connected"; break;
        case NJClient::NJC_STATUS_PRECONNECT:   statusStr = "Connecting..."; break;
        default: statusStr = "Unknown (" + juce::String(status) + ")"; break;
    }
    if (statusLabel.getText() != statusStr)
        statusLabel.setText(statusStr, juce::dontSendNotification);

    const auto connectText = (status == NJClient::NJC_STATUS_OK || status == NJClient::NJC_STATUS_PRECONNECT)
        ? juce::String("Disconnect")
        : juce::String("Connect");
    if (connectButton.getButtonText() != connectText)
        connectButton.setButtonText(connectText);

    // Chat
    {
        const juce::ScopedLock lock(audioProcessor.chatLock);
        const auto& history = audioProcessor.chatHistory;
        const auto& senders = audioProcessor.chatSenders;
        const int revision = audioProcessor.chatRevision.load();

        if (revision != lastChatRevision)
        {
            applyColoredChat(chatDisplay, history, senders, audioProcessor);
            enqueueChatTtsForNewLines(history, senders);
            lastChatRevision = revision;

            if (chatWindow)
            {
                if (auto* popup = dynamic_cast<ChatPopupComponent*>(chatWindow->getContentComponent()))
                    popup->setChatText(history, senders);
            }
        }
        else if (chatTtsEnabled && !pendingChatTtsLines.empty())
        {
            enqueueChatTtsForNewLines(history, senders);
        }
    }

    updateTranslateButtonState();

    if (shouldDeferHeavyUiWork())
        return;

    const bool heavyUiAllowed = nowMs >= suppressHeavyUiUntilMs;
#if JUCE_WINDOWS
    const bool animatedBackgroundActive = videoFrameReader != nullptr;
#else
    const bool animatedBackgroundActive = false;
#endif
    const int heavyUiTickDivisor = abletonHostEditor ? 8 : (animatedBackgroundActive ? 11 : 6);
    const bool runHeavyUiTick = ((++heavyUiTickCounter % heavyUiTickDivisor) == 0);
    if (heavyUiAllowed && runHeavyUiTick)
    {
        userList.updateContent();
        syncUserStripMidiTargets();
        refreshMidiRelayTargetSelector();
    }
    applyMidiMappings();
    applyOscMappings();

    auto updateClipPulseState = [nowMs](float peak, double& startMs, bool& pulsing) -> bool
    {
        constexpr double holdMs = 500.0;
        const bool clipping = peak >= 1.0f;
        const bool wasPulsing = pulsing;

        if (clipping)
        {
            if (startMs <= 0.0)
                startMs = nowMs;
            pulsing = (nowMs - startMs) >= holdMs;
        }
        else
        {
            startMs = 0.0;
            pulsing = false;
        }

        return wasPulsing != pulsing;
    };

    bool clipPulseNeedsRepaint = false;
    int numLocal = audioProcessor.getNumLocalChannels();
    numLocal = juce::jlimit(1, NinjamVst3AudioProcessor::maxLocalChannels, numLocal);
    for (int i = 0; i < numLocal; ++i)
    {
        const float peakL = audioProcessor.getLocalChannelPeakLeft(i);
        const float peakR = audioProcessor.getLocalChannelPeakRight(i);
        const float peak = juce::jmax(peakL, peakR);
        localPeakMeters[(size_t)i].setPeak(peakL, peakR);
        clipPulseNeedsRepaint |= updateClipPulseState(peak, localClipStartMs[(size_t)i], localClipPulsing[(size_t)i]);

        float db = -60.0f;
        if (peak > 1.0e-6f)
            db = juce::jlimit(-60.0f, 6.0f, 20.0f * std::log10(peak));
        localDbLabels[(size_t)i].setText(juce::String(db, 1) + " dB", juce::dontSendNotification);
    }

    const auto localChord = audioProcessor.getLocalChordLabel();
    const auto localChordStats = "CPU " + juce::String(audioProcessor.getLocalChordCpuPercent(), 2)
                               + "%  MEM ~" + juce::String(audioProcessor.getLocalChordMemoryKb()) + " KB";
    const auto localChordText = "Chord: " + localChord;
    if (localChordLabel.getText() != localChordText)
        localChordLabel.setText(localChordText, juce::dontSendNotification);
    if (audioProcessor.isChordDetectionEnabled())
        localChordLabel.setTooltip("Local channel 1 chord: " + localChord + "\n" + localChordStats);
    else
        localChordLabel.setTooltip("Local chord detection is off. Enable it in Options.");
    const auto compactChordStats = localChordStats.replace("  MEM", " M");
    if (localChordStatsLabel.getText() != compactChordStats)
        localChordStatsLabel.setText(compactChordStats, juce::dontSendNotification);

    const float masterPeakL = audioProcessor.getMasterPeakLeft();
    const float masterPeakR = audioProcessor.getMasterPeakRight();
    const float masterPk = juce::jmax(masterPeakL, masterPeakR);
    masterPeakMeter.setPeak(masterPeakL, masterPeakR);
    clipPulseNeedsRepaint |= updateClipPulseState(masterPk, masterClipStartMs, masterClipPulsing);
    {
        float db = -60.0f;
        if (masterPk > 1.0e-6f)
            db = juce::jlimit(-60.0f, 6.0f, 20.0f * std::log10(masterPk));
        masterDbLabel.setText(juce::String((int)std::round(db)) + " dB", juce::dontSendNotification);
    }

    bool anyClipPulsing = masterClipPulsing;
    for (int i = 0; i < numLocal; ++i)
        anyClipPulsing = anyClipPulsing || localClipPulsing[(size_t)i];

    if (clipPulseNeedsRepaint || (anyClipPulsing && nowMs - lastClipPulseRepaintMs >= transmitPulseRepaintMs))
    {
        lastClipPulseRepaintMs = nowMs;
        for (int i = 0; i < numLocal; ++i)
            if (!localChannelPulseBounds[(size_t)i].isEmpty())
                repaint(localChannelPulseBounds[(size_t)i].expanded(4));
        if (!masterChannelPulseBounds.isEmpty())
            repaint(masterChannelPulseBounds.expanded(4));
    }

    const bool runAutoLevelTick = autoLevelEnabled
        && runHeavyUiTick
        && (!abletonHostEditor || ((++autoLevelWorkTickCounter % 3) == 0));
    if (runAutoLevelTick)
    {
        std::vector<NinjamVst3AudioProcessor::UserInfo> users = audioProcessor.getConnectedUsers();
        if (!users.empty())
        {
            const float timerIntervalMs = abletonHostEditor ? 1200.0f : 180.0f;
            const float noiseFloor = 0.015f;
            const float targetMasterLevel = 0.630957f; // -4 dBFS
            const float targetSoloLevel = 0.45f;
            const float perUserCeiling = 0.56f;
            const float minGain = 0.05f;
            const float maxGain = 2.0f;
            const float peakAttackCoeff = 1.0f - std::exp(-timerIntervalMs / 700.0f);
            const float peakReleaseCoeff = 1.0f - std::exp(-timerIntervalMs / 3500.0f);
            const float gainUpCoeff = 1.0f - std::exp(-timerIntervalMs / 5000.0f);
            const float gainDownCoeff = 1.0f - std::exp(-timerIntervalMs / 1400.0f);
            const float emergencyDownCoeff = 1.0f - std::exp(-timerIntervalMs / 450.0f);
            const float maxUpStep = juce::jmax(0.006f, (timerIntervalMs / 1000.0f) * 0.12f);
            const float maxDownStep = juce::jmax(0.035f, (timerIntervalMs / 1000.0f) * 0.45f);
            const float maxEmergencyDownStep = juce::jmax(0.08f, (timerIntervalMs / 1000.0f) * 1.35f);
            const int highPersistTicks = juce::jmax(2, juce::roundToInt(700.0f / timerIntervalMs));
            const int lowPersistTicks = juce::jmax(5, juce::roundToInt(1800.0f / timerIntervalMs));

            std::map<int, float> observedLevels;
            int audibleUsers = 0;
            for (const auto& u : users)
            {
                const float peakL = audioProcessor.getUserPeak(u.index, 0);
                const float peakR = audioProcessor.getUserPeak(u.index, 1);
                const float currentLevel = juce::jmax(peakL, peakR);
                const float sourceEstimate = juce::jlimit(0.0f, 4.0f, currentLevel / juce::jmax(minGain, u.volume));
                observedLevels[u.index] = currentLevel;
                if (sourceEstimate >= noiseFloor)
                    ++audibleUsers;
            }

            const float targetPerUserLevel = juce::jmin(targetSoloLevel,
                targetMasterLevel / std::sqrt((float)juce::jmax(1, audibleUsers)));
            const float masterPeak = audioProcessor.getMasterPeak();
            float masterReduction = 1.0f;
            if (masterPeak > targetMasterLevel)
                masterReduction = juce::jlimit(0.25f, 1.0f, targetMasterLevel / masterPeak);

            std::set<int> activeIds;

            for (auto& u : users)
            {
                int id = u.index;
                activeIds.insert(id);

                juce::String nameKey = u.name.trim().toLowerCase();
                if (nameKey.isEmpty())
                    nameKey = juce::String("user-") + juce::String(id);

                auto nameIt = autoLevelUserNameById.find(id);
                if (nameIt == autoLevelUserNameById.end() || nameIt->second != nameKey)
                {
                    autoLevelCurrentGains.erase(id);
                    autoLevelLastAppliedGains.erase(id);
                    autoLevelPeakLevels.erase(id);
                    autoLevelChannelActiveTicks.erase(id);
                    autoLevelMeasureTicks.erase(id);
                    autoLevelOverTargetTicks.erase(id);
                    autoLevelUnderTargetTicks.erase(id);
                    autoLevelUserNameById[id] = nameKey;
                }

                const float currentLevel = observedLevels[id];
                if (!autoLevelCurrentGains.count(id))
                    autoLevelCurrentGains[id] = juce::jlimit(minGain, maxGain, u.volume);

                const float currentGain = autoLevelCurrentGains[id];
                const float sourceLevel = currentGain > 1.0e-4f
                    ? juce::jlimit(0.0f, 4.0f, currentLevel / currentGain)
                    : currentLevel;

                if (!autoLevelPeakLevels.count(id))         autoLevelPeakLevels[id] = 0.0f;
                if (!autoLevelChannelActiveTicks.count(id)) autoLevelChannelActiveTicks[id] = 0;
                else                                         autoLevelChannelActiveTicks[id]++;

                bool isNew = autoLevelChannelActiveTicks[id] < 40;
                float& longTermPeak = autoLevelPeakLevels[id];
                int& measureTicks = autoLevelMeasureTicks[id];
                const bool firstMeasurement = measureTicks == 0;
                ++measureTicks;

                if (firstMeasurement && sourceLevel >= noiseFloor)
                    longTermPeak = sourceLevel;
                else
                {
                    const float peakCoeff = sourceLevel > longTermPeak ? peakAttackCoeff : peakReleaseCoeff;
                    longTermPeak += (sourceLevel - longTermPeak) * peakCoeff;
                }

                longTermPeak = juce::jlimit(0.0f, 4.0f, longTermPeak);

                float targetGain = currentGain;
                if (longTermPeak >= noiseFloor)
                {
                    const float safePeak = juce::jmax(longTermPeak, noiseFloor);
                    targetGain = targetPerUserLevel / safePeak;
                    targetGain = juce::jmin(targetGain, perUserCeiling / safePeak);
                    targetGain *= masterReduction;
                }

                targetGain = juce::jlimit(minGain, maxGain, targetGain);

                const float currentOutput = sourceLevel * currentGain;
                const float longTermOutput = longTermPeak * currentGain;
                const bool tooHigh = (longTermOutput > targetPerUserLevel * 1.12f) || (currentOutput > targetMasterLevel);
                const bool tooLow = longTermPeak >= noiseFloor && longTermOutput < targetPerUserLevel * 0.72f;

                if (tooHigh)
                    ++autoLevelOverTargetTicks[id];
                else
                    autoLevelOverTargetTicks[id] = 0;

                if (tooLow)
                    ++autoLevelUnderTargetTicks[id];
                else
                    autoLevelUnderTargetTicks[id] = 0;

                const bool sustainedHigh = autoLevelOverTargetTicks[id] >= highPersistTicks;
                const bool sustainedLow = autoLevelUnderTargetTicks[id] >= lowPersistTicks;

                if (targetGain < currentGain && !sustainedHigh)
                    targetGain = currentGain;
                else if (targetGain > currentGain && !sustainedLow)
                    targetGain = currentGain;

                const bool reducing = targetGain < currentGain;
                const bool emergencyReduction = reducing && sustainedHigh && currentOutput > targetMasterLevel;
                float smoothingCoeff = reducing ? (emergencyReduction ? emergencyDownCoeff : gainDownCoeff)
                                                : gainUpCoeff;
                if (isNew && !reducing)
                    smoothingCoeff *= 0.35f;

                const float unslewedGain = currentGain + (targetGain - currentGain) * smoothingCoeff;
                const float maxStep = reducing ? (emergencyReduction ? maxEmergencyDownStep : maxDownStep)
                                               : maxUpStep;
                const float slewedDelta = juce::jlimit(-maxStep, maxStep, unslewedGain - currentGain);
                autoLevelCurrentGains[id] = juce::jlimit(minGain, maxGain, currentGain + slewedDelta);

                const float nextGain = autoLevelCurrentGains[id];
                const auto appliedIt = autoLevelLastAppliedGains.find(id);
                if (appliedIt == autoLevelLastAppliedGains.end()
                    || std::abs(appliedIt->second - nextGain) >= 0.005f)
                {
                    audioProcessor.rememberUserVolume(id, nextGain, u.name);
                    audioProcessor.setUserVolume(id, nextGain);
                    autoLevelLastAppliedGains[id] = nextGain;
                }
            }

            for (auto it = autoLevelCurrentGains.begin(); it != autoLevelCurrentGains.end();)
            {
                if (!activeIds.count(it->first))
                {
                    int id = it->first;
                    autoLevelLastAppliedGains.erase(id);
                    autoLevelPeakLevels.erase(id);
                    autoLevelChannelActiveTicks.erase(id);
                    autoLevelMeasureTicks.erase(id);
                    autoLevelOverTargetTicks.erase(id);
                    autoLevelUnderTargetTicks.erase(id);
                    autoLevelUserNameById.erase(id);
                    it = autoLevelCurrentGains.erase(it);
                }
                else { ++it; }
            }
        }
        else
        {
            autoLevelCurrentGains.clear();
            autoLevelLastAppliedGains.clear();
            autoLevelPeakLevels.clear();
            autoLevelChannelActiveTicks.clear();
            autoLevelMeasureTicks.clear();
            autoLevelOverTargetTicks.clear();
            autoLevelUnderTargetTicks.clear();
            autoLevelUserNameById.clear();
        }
    }

    intervalDisplay.repaint();

    // Advance video background frame if active (Windows only)
#if JUCE_WINDOWS
    if (videoFrameReader != nullptr)
    {
        auto frame = videoFrameReader->getLatestFrame();
        const bool samplerVisible = samplePadsWindow != nullptr && samplePadsWindow->isVisible();
        const bool samplerMouseBusy = samplerVisible
            && juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();
        const double minimumBackgroundRepaintMs = abletonHostEditor ? 250.0
            : samplerMouseBusy ? 90.0
            : 0.0;
        if (frame.isValid()
            && (minimumBackgroundRepaintMs <= 0.0
                || nowMs - lastVideoBackgroundRepaintMs >= minimumBackgroundRepaintMs))
        {
            backgroundImage = std::move(frame);
            lastVideoBackgroundRepaintMs = nowMs;
            backgroundComponent.setBackgroundImage(backgroundImage);
        }
    }
#endif

    if (voiceChatButton.getToggleState())
    {
        updateVoiceChatButtonColor();
        voiceChatButton.repaint();
    }

    double hostBpm = 0.0;
    bool hostPlaying = false;
    {
        juce::AudioPlayHead::CurrentPositionInfo info;
        if (audioProcessor.getHostPosition(info))
        {
            hostBpm = info.bpm;
            hostPlaying = info.isPlaying;
        }
    }

    float njBpm = audioProcessor.getBPM();
    int bpi = audioProcessor.getBPI();

    juce::String text;
    text << "NJ " << juce::String(njBpm, 1) << " / " << bpi << " BPI";
    if (hostBpm > 0.0)
        text << " | Host " << juce::String(hostBpm, 1) << (hostPlaying ? " (Play)" : " (Stop)");

    int codecMode = audioProcessor.getCodecMode();
    juce::String codec;
    if (codecMode == 2)      codec = "Opus";
    else if (codecMode == 1) codec = "Vorbis+Opus";
    else                     codec = "Vorbis";
    text << " | Codec " << codec;

    if (tempoLabel.getText() != text)
        tempoLabel.setText(text, juce::dontSendNotification);

    if (!delayTimeSlider.isMouseButtonDown())
    {
        const auto delayTimeMs = audioProcessor.getFxDelayTimeMs();
        if (std::abs(delayTimeSlider.getValue() - delayTimeMs) > 0.01)
            delayTimeSlider.setValue(delayTimeMs, juce::dontSendNotification);
    }
    const auto delayDivision = audioProcessor.getFxDelayDivision();
    if (delayDivisionSelector.getSelectedId() != delayDivision)
        delayDivisionSelector.setSelectedId(delayDivision, juce::dontSendNotification);
    const auto pingPongEnabled = audioProcessor.isFxDelayPingPong();
    if (delayPingPongButton.getToggleState() != pingPongEnabled)
        delayPingPongButton.setToggleState(pingPongEnabled, juce::dontSendNotification);
    updateFxButtonLabel();
    updateFxControlsVisibility();
}

void NinjamVst3AudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    juce::Component* start = event.originalComponent != nullptr ? event.originalComponent : event.eventComponent;
    if (gifPickerPanel != nullptr && gifPickerPanel->isVisible())
    {
        bool keepGifPickerOpen = false;
        for (auto* c = start; c != nullptr; c = c->getParentComponent())
        {
            if (c == gifPickerPanel.get() || c == &chatEmojiButton)
            {
                keepGifPickerOpen = true;
                break;
            }
        }

        if (!keepGifPickerOpen)
            gifPickerPanel->setVisible(false);
    }

    if (!(event.mods.isPopupMenu() || event.mods.isRightButtonDown()))
        return;

    for (auto* c = start; c != nullptr; c = c->getParentComponent())
    {
        if (c == &syncButton)
            return;

        auto it = midiTargetsByComponent.find(c);
        if (it != midiTargetsByComponent.end())
        {
            showMidiLearnMenuForComponent(*c, event.getScreenPosition());
            return;
        }
    }
}

void NinjamVst3AudioProcessorEditor::registerMidiLearnTarget(juce::Component& component, const juce::String& targetId, bool isToggle)
{
    auto existing = midiTargetsByComponent.find(&component);
    if (existing != midiTargetsByComponent.end() && existing->second.id != targetId)
        midiTargetsById.erase(existing->second.id);

    MidiLearnTarget target;
    target.id = targetId;
    target.component = &component;
    target.isToggle = isToggle;

    midiTargetsByComponent[&component] = target;
    midiTargetsById[targetId] = target;
    component.removeMouseListener(this);
    component.addMouseListener(this, false);
}

void NinjamVst3AudioProcessorEditor::syncUserStripMidiTargets()
{
    for (auto it = midiTargetsById.begin(); it != midiTargetsById.end();)
    {
        if (it->first.startsWith("user."))
            it = midiTargetsById.erase(it);
        else
        {
            ++it;
        }
    }

    for (auto it = midiTargetsByComponent.begin(); it != midiTargetsByComponent.end();)
    {
        if (it->second.id.startsWith("user."))
            it = midiTargetsByComponent.erase(it);
        else
            ++it;
    }

    auto strips = userList.getStripPointers();
    for (auto* strip : strips)
    {
        const int userIdx = strip->getUserIndex();
        const juce::String prefix = "user." + juce::String(userIdx) + ".";
        registerMidiLearnTarget(strip->getVolumeSlider(), prefix + "volume", false);
        registerMidiLearnTarget(strip->getPanSlider(), prefix + "pan", false);
        registerMidiLearnTarget(strip->getMuteButton(), prefix + "mute", true);
        registerMidiLearnTarget(strip->getSoloButton(), prefix + "solo", true);
        for (int i = 0; i < 8; ++i)
            registerMidiLearnTarget(strip->getChannelSlider(i), prefix + "channel." + juce::String(i + 1), false);
    }
}

void NinjamVst3AudioProcessorEditor::showMidiLearnMenuForComponent(juce::Component& component, juce::Point<int> screenPos)
{
    auto it = midiTargetsByComponent.find(&component);
    if (it == midiTargetsByComponent.end())
        return;

    const juce::String targetId = it->second.id;
    juce::PopupMenu menu;
    const int sampleFxSlot = getSampleFxKnobSlotFromLearnTargetId(targetId);
    if (sampleFxSlot >= 0 && sampleFxSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots)
    {
        juce::PopupMenu clearWireMenu;
        bool hasWire = false;
        for (int targetSlot = 0; targetSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++targetSlot)
        {
            if (audioProcessor.isSamplePadFxSlotToSlotRouteEnabled(sampleFxSlot, targetSlot))
            {
                clearWireMenu.addItem(1000 + targetSlot, "To knob " + juce::String(targetSlot + 1));
                hasWire = true;
            }
        }
        for (int sourceSlot = 0; sourceSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots; ++sourceSlot)
        {
            if (audioProcessor.isSamplePadFxSlotToSlotRouteEnabled(sourceSlot, sampleFxSlot))
            {
                clearWireMenu.addItem(1100 + sourceSlot, "From knob " + juce::String(sourceSlot + 1));
                hasWire = true;
            }
        }
        for (int pad = 0; pad < NinjamVst3AudioProcessor::numSamplePads; ++pad)
        {
            if (audioProcessor.isSamplePadFxSlotRouteEnabled(pad, sampleFxSlot))
            {
                auto padName = audioProcessor.getSamplePadName(pad).trim();
                if (padName.isEmpty())
                    padName = "Pad " + juce::String(pad + 1);
                clearWireMenu.addItem(1200 + pad, "From " + padName);
                hasWire = true;
            }
        }

        if (!hasWire)
            clearWireMenu.addItem(1099, "No wires", false);
        menu.addSubMenu("Clear Wire", clearWireMenu);
        menu.addSeparator();
    }
    menu.addItem(3, "OSC Learn");
    menu.addItem(4, "OSC Forget", oscSourceByTargetId.find(targetId) != oscSourceByTargetId.end());
    menu.addSeparator();
    menu.addItem(1, "MIDI Learn");
    menu.addItem(2, "MIDI Forget", midiSourceByTargetId.find(targetId) != midiSourceByTargetId.end());
    menu.addSeparator();
    menu.addItem(10, "Save Learn Mappings");
    menu.addItem(11, "Load Learn Mappings");
    menu.addItem(12, "Forget All Learn Mappings");
    juce::Rectangle<int> popupAnchor(screenPos.x, screenPos.y, 1, 1);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&component).withTargetScreenArea(popupAnchor),
                       [this, targetId, sampleFxSlot](int result)
                       {
                           if (sampleFxSlot >= 0 && result >= 1000 && result < 1100)
                           {
                               audioProcessor.setSamplePadFxSlotToSlotRouteEnabled(sampleFxSlot, result - 1000, false);
                               if (samplePadsWindow != nullptr)
                                   samplePadsWindow->repaint();
                           }
                           else if (sampleFxSlot >= 0 && result >= 1100 && result < 1200)
                           {
                               audioProcessor.setSamplePadFxSlotToSlotRouteEnabled(result - 1100, sampleFxSlot, false);
                               if (samplePadsWindow != nullptr)
                                   samplePadsWindow->repaint();
                           }
                           else if (sampleFxSlot >= 0 && result >= 1200 && result < 1300)
                           {
                               audioProcessor.setSamplePadFxSlotRouteEnabled(result - 1200, sampleFxSlot, false);
                               if (samplePadsWindow != nullptr)
                                   samplePadsWindow->repaint();
                           }
                           else if (result == 3)
                           {
                               oscLearnArmedTargetId = targetId;
                           }
                           else if (result == 4)
                           {
                               oscSourceByTargetId.erase(targetId);
                               if (oscLearnArmedTargetId == targetId)
                                   oscLearnArmedTargetId.clear();
                               syncLearnMappingsToProcessor();
                           }
                           else if (result == 1)
                           {
                               midiLearnArmedTargetId = targetId;
                           }
                           else if (result == 2)
                           {
                               midiSourceByTargetId.erase(targetId);
                               if (midiLearnArmedTargetId == targetId)
                                   midiLearnArmedTargetId.clear();
                               syncLearnMappingsToProcessor();
                           }
                           else if (result == 10)
                           {
                               saveLearnMappingsToDisk();
                           }
                           else if (result == 11)
                           {
                               loadLearnMappingsFromDisk();
                           }
                           else if (result == 12)
                           {
                               clearLearnMappings();
                           }
                       });
}

void NinjamVst3AudioProcessorEditor::applyMidiMappings()
{
    auto events = audioProcessor.popPendingMidiControllerEvents();
    if (events.empty())
        return;

    for (const auto& event : events)
    {
        if (midiLearnArmedTargetId.isNotEmpty() && midiTargetsById.find(midiLearnArmedTargetId) != midiTargetsById.end())
        {
            MidiSourceMapping mapping;
            mapping.isController = event.isController;
            mapping.midiChannel = event.midiChannel;
            mapping.number = event.number;
            mapping.lastBinaryState = event.isNoteOn ? 1 : 0;
            midiSourceByTargetId[midiLearnArmedTargetId] = mapping;
            midiLearnArmedTargetId.clear();
            syncLearnMappingsToProcessor();
        }

        for (auto& pair : midiSourceByTargetId)
        {
            auto targetIt = midiTargetsById.find(pair.first);
            if (targetIt == midiTargetsById.end())
                continue;

            auto& mapping = pair.second;
            if (mapping.isController != event.isController)
                continue;
            if (mapping.midiChannel != event.midiChannel || mapping.number != event.number)
                continue;

            auto* component = targetIt->second.component;

            if (targetIt->second.isToggle)
            {
                int binaryState = event.isNoteOn ? 1 : (event.value >= 64 ? 1 : 0);
                if (pair.first.startsWith("samplepad.trigger."))
                {
                    const int padIndex = pair.first.fromLastOccurrenceOf(".", false, false).getIntValue() - 1;
                    if (binaryState != mapping.lastBinaryState)
                        audioProcessor.handleSamplePadMidiPadState(padIndex, binaryState == 1);
                    mapping.lastBinaryState = binaryState;
                    continue;
                }

                if (component == nullptr)
                    continue;

                if (binaryState == 1 && mapping.lastBinaryState != 1)
                    if (auto* button = dynamic_cast<juce::Button*>(component))
                        button->triggerClick();
                mapping.lastBinaryState = binaryState;
            }
            else
            {
                const int sampleFxSlot = getSampleFxKnobSlotFromLearnTargetId(pair.first);
                if (auto* slider = dynamic_cast<juce::Slider*>(component))
                {
                    if (slider->isMouseButtonDown())
                        continue;
                    auto range = slider->getRange();
                    const double norm = juce::jlimit(0.0, 1.0, (double)event.normalized);
                    const double value = juce::jlimit(range.getStart(), range.getEnd(), range.getStart() + norm * range.getLength());
                    slider->setValue(value, juce::sendNotificationSync);
                }
                else if (sampleFxSlot >= 0 && sampleFxSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots)
                {
                    audioProcessor.setSamplePadFxSlotAmount(sampleFxSlot, event.normalized);
                }
            }
        }
    }
}

void NinjamVst3AudioProcessorEditor::applyOscMappings()
{
    std::vector<PendingOscEvent> events;
    {
        const juce::SpinLock::ScopedLockType lock(oscEventQueueLock);
        events.swap(pendingOscEvents);
    }

    {
        const auto relayEvents = audioProcessor.popPendingOscRelayEvents();
        if (!relayEvents.empty())
        {
            events.reserve(events.size() + relayEvents.size());
            for (const auto& relayEvent : relayEvents)
            {
                PendingOscEvent e;
                e.address = relayEvent.address;
                e.normalized = relayEvent.normalized;
                e.binaryOn = relayEvent.binaryOn;
                events.push_back(e);
            }
        }
    }
    if (events.empty())
        return;

    for (const auto& event : events)
    {
        if (oscLearnArmedTargetId.isNotEmpty() && midiTargetsById.find(oscLearnArmedTargetId) != midiTargetsById.end())
        {
            OscSourceMapping mapping;
            mapping.address = event.address;
            mapping.lastBinaryState = event.binaryOn ? 1 : 0;
            oscSourceByTargetId[oscLearnArmedTargetId] = mapping;
            oscLearnArmedTargetId.clear();
            syncLearnMappingsToProcessor();
        }

        for (auto& pair : oscSourceByTargetId)
        {
            auto targetIt = midiTargetsById.find(pair.first);
            if (targetIt == midiTargetsById.end())
                continue;
            auto* component = targetIt->second.component;
            auto& mapping = pair.second;
            if (mapping.address != event.address)
                continue;

            if (targetIt->second.isToggle)
            {
                const int binaryState = event.binaryOn ? 1 : 0;
                if (pair.first.startsWith("samplepad.trigger."))
                {
                    const int padIndex = pair.first.fromLastOccurrenceOf(".", false, false).getIntValue() - 1;
                    if (binaryState == 1 && mapping.lastBinaryState != 1)
                        audioProcessor.triggerSamplePad(padIndex);
                    mapping.lastBinaryState = binaryState;
                    continue;
                }

                if (component == nullptr)
                    continue;

                if (binaryState == 1 && mapping.lastBinaryState != 1)
                    if (auto* button = dynamic_cast<juce::Button*>(component))
                        button->triggerClick();
                mapping.lastBinaryState = binaryState;
            }
            else if (auto* slider = dynamic_cast<juce::Slider*>(component))
            {
                if (slider->isMouseButtonDown())
                    continue;
                auto range = slider->getRange();
                const double norm = juce::jlimit(0.0, 1.0, (double)event.normalized);
                const double value = juce::jlimit(range.getStart(), range.getEnd(), range.getStart() + norm * range.getLength());
                slider->setValue(value, juce::sendNotificationSync);
            }
            else
            {
                const int sampleFxSlot = getSampleFxKnobSlotFromLearnTargetId(pair.first);
                if (sampleFxSlot >= 0 && sampleFxSlot < NinjamVst3AudioProcessor::numSamplePadFxSlots)
                    audioProcessor.setSamplePadFxSlotAmount(sampleFxSlot, event.normalized);
            }
        }
    }
}

void NinjamVst3AudioProcessorEditor::applyRemoteMidiRelaySelection(int channel, int inputIndex)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("channel", channel);
    obj->setProperty("inputIndex", inputIndex);
    const juce::String payload = juce::JSON::toString(juce::var(obj.get()));
    const juce::String targetsRaw = audioProcessor.getMidiRelayTarget().trim();
    if (targetsRaw.isEmpty() || targetsRaw == "*")
    {
        audioProcessor.sendSideSignal("*", "localInputSelect", payload);
        return;
    }

    juce::StringArray targets;
    targets.addTokens(targetsRaw, ",", "");
    targets.trim();
    targets.removeEmptyStrings();
    targets.removeDuplicates(true);
    for (const auto& t : targets)
        audioProcessor.sendSideSignal(t, "localInputSelect", payload);
}

void NinjamVst3AudioProcessorEditor::syncLearnMappingsToProcessor()
{
    juce::Array<juce::var> midiArray;
    for (const auto& pair : midiSourceByTargetId)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("target", pair.first);
        obj->setProperty("isController", pair.second.isController);
        obj->setProperty("midiChannel", pair.second.midiChannel);
        obj->setProperty("number", pair.second.number);
        obj->setProperty("lastBinaryState", pair.second.lastBinaryState);
        midiArray.add(juce::var(obj.get()));
    }

    juce::Array<juce::var> oscArray;
    for (const auto& pair : oscSourceByTargetId)
    {
        juce::DynamicObject::Ptr obj = new juce::DynamicObject();
        obj->setProperty("target", pair.first);
        obj->setProperty("address", pair.second.address);
        obj->setProperty("lastBinaryState", pair.second.lastBinaryState);
        oscArray.add(juce::var(obj.get()));
    }

    audioProcessor.setMidiLearnStateJson(juce::JSON::toString(juce::var(midiArray)));
    audioProcessor.setOscLearnStateJson(juce::JSON::toString(juce::var(oscArray)));
}

void NinjamVst3AudioProcessorEditor::loadLearnMappingsFromProcessor()
{
    midiSourceByTargetId.clear();
    oscSourceByTargetId.clear();

    const juce::var midiParsed = juce::JSON::parse(audioProcessor.getMidiLearnStateJson());
    if (auto* midiArray = midiParsed.getArray())
    {
        for (const auto& entry : *midiArray)
        {
            auto* obj = entry.getDynamicObject();
            if (obj == nullptr || !obj->hasProperty("target"))
                continue;
            MidiSourceMapping mapping;
            mapping.isController = obj->hasProperty("isController") ? (bool)obj->getProperty("isController") : true;
            mapping.midiChannel = obj->hasProperty("midiChannel") ? (int)obj->getProperty("midiChannel") : 1;
            mapping.number = obj->hasProperty("number") ? (int)obj->getProperty("number") : 0;
            mapping.lastBinaryState = obj->hasProperty("lastBinaryState") ? (int)obj->getProperty("lastBinaryState") : -1;
            midiSourceByTargetId[obj->getProperty("target").toString()] = mapping;
        }
    }

    const juce::var oscParsed = juce::JSON::parse(audioProcessor.getOscLearnStateJson());
    if (auto* oscArray = oscParsed.getArray())
    {
        for (const auto& entry : *oscArray)
        {
            auto* obj = entry.getDynamicObject();
            if (obj == nullptr || !obj->hasProperty("target") || !obj->hasProperty("address"))
                continue;
            OscSourceMapping mapping;
            mapping.address = obj->getProperty("address").toString();
            mapping.lastBinaryState = obj->hasProperty("lastBinaryState") ? (int)obj->getProperty("lastBinaryState") : -1;
            oscSourceByTargetId[obj->getProperty("target").toString()] = mapping;
        }
    }
}

void NinjamVst3AudioProcessorEditor::saveLearnMappingsToDisk()
{
    syncLearnMappingsToProcessor();
    auto popts = makeSettingsOptions();
    juce::PropertiesFile props(popts);
    props.setValue("midiLearnStateJson", audioProcessor.getMidiLearnStateJson());
    props.setValue("oscLearnStateJson", audioProcessor.getOscLearnStateJson());
    props.saveIfNeeded();
}

void NinjamVst3AudioProcessorEditor::loadLearnMappingsFromDisk()
{
    auto popts = makeSettingsOptions();
    juce::PropertiesFile props(popts);
    audioProcessor.setMidiLearnStateJson(props.getValue("midiLearnStateJson", {}));
    audioProcessor.setOscLearnStateJson(props.getValue("oscLearnStateJson", {}));
    loadLearnMappingsFromProcessor();
}

void NinjamVst3AudioProcessorEditor::markPersistentSettingsDirty()
{
    persistentSettingsDirty = true;
    lastPersistentSettingsSaveMs = juce::Time::getMillisecondCounterHiRes();
}

void NinjamVst3AudioProcessorEditor::setChatWindowColourKey(const juce::String& key, bool markDirty)
{
    chatWindowColourKey = normaliseChatWindowColourKey(key);
    applyChatWindowColourToDisplays();

    if (markDirty)
        markPersistentSettingsDirty();
}

void NinjamVst3AudioProcessorEditor::applyChatWindowColourToDisplays()
{
    chatDisplay.setBackgroundColour(chatWindowColourForKey(chatWindowColourKey));

    if (chatWindow)
        if (auto* popup = getChatPopupComponent(chatWindow.get()))
            popup->setChatWindowColourKey(chatWindowColourKey);
}

void NinjamVst3AudioProcessorEditor::setChatTtsEnabled(bool enabled, bool markDirty)
{
    chatTtsEnabled = enabled;
    {
        const juce::ScopedLock lock(audioProcessor.chatLock);
        lastChatTtsHistorySize = audioProcessor.chatHistory.size();
    }

    if (markDirty)
        markPersistentSettingsDirty();
}

void NinjamVst3AudioProcessorEditor::setChatTtsVoiceId(const juce::String& voiceId, bool markDirty)
{
    const juce::String trimmed = voiceId.trim();
    chatTtsVoiceId = trimmed;

    if (markDirty)
        markPersistentSettingsDirty();
}

void NinjamVst3AudioProcessorEditor::setChatTtsVolume(float volume, bool markDirty)
{
    chatTtsVolume = juce::jlimit(0.0f, 1.0f, volume);

    if (markDirty)
        markPersistentSettingsDirty();
}

void NinjamVst3AudioProcessorEditor::setChatTtsOutputId(const juce::String& outputId, bool markDirty)
{
    chatTtsOutputId = outputId.trim();

    if (markDirty)
        markPersistentSettingsDirty();
}

void NinjamVst3AudioProcessorEditor::enqueueChatTtsForNewLines(const juce::StringArray& history,
                                                               const juce::StringArray& senders)
{
    if (!chatTtsEnabled || chatTtsEngine == nullptr)
    {
        lastChatTtsHistorySize = history.size();
        pendingChatTtsLines.clear();
        return;
    }

    if (lastChatTtsHistorySize > history.size())
    {
        lastChatTtsHistorySize = 0;
        pendingChatTtsLines.clear();
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const int startIndex = juce::jlimit(0, history.size(), lastChatTtsHistorySize);
    for (int i = startIndex; i < history.size(); ++i)
    {
        const juce::String sender = i < senders.size() ? senders[i] : juce::String();
        if (makeChatTtsSpeechText(history[i], sender).isNotEmpty())
            pendingChatTtsLines.push_back({ i, nowMs + 1800.0 });
    }

    lastChatTtsHistorySize = history.size();

    for (auto it = pendingChatTtsLines.begin(); it != pendingChatTtsLines.end();)
    {
        if (it->historyIndex < 0 || it->historyIndex >= history.size())
        {
            it = pendingChatTtsLines.erase(it);
            continue;
        }

        if (nowMs < it->dueMs)
        {
            ++it;
            continue;
        }

        const juce::String sender = it->historyIndex < senders.size() ? senders[it->historyIndex] : juce::String();
        const juce::String speechText = makeChatTtsSpeechText(history[it->historyIndex], sender);
        if (speechText.isNotEmpty())
            chatTtsEngine->enqueue(speechText, chatTtsVoiceId, chatTtsVolume, chatTtsOutputId);

        it = pendingChatTtsLines.erase(it);
    }
}

juce::String NinjamVst3AudioProcessorEditor::buildPersistentSettingsFingerprint(bool includeProcessorState) const
{
    juce::StringArray parts;
    parts.add(serverField.getText());
    parts.add(userField.getText());
    parts.add(passField.getText());
    parts.add(anonymousButton.getToggleState() ? "1" : "0");
    parts.add(layoutButton.getToggleState() ? "1" : "0");
    parts.add(chatButton.getToggleState() ? "1" : "0");
    parts.add(videoBgToggle.getToggleState() ? "1" : "0");
    parts.add(autoLevelButton.getToggleState() ? "1" : "0");
    parts.add(spreadOutputsButton.getToggleState() ? "1" : "0");
    parts.add(juce::String(abletonWindowSizePreset));
    parts.add(juce::String(abletonChatWindowSizePreset));
    parts.add(juce::String(abletonSamplerWindowSizePreset));
    parts.add(samplePadsWindowBoundsValid ? samplePadsWindowBounds.toString() : juce::String());
    parts.add(audioProcessor.getLocalChatColourKey());
    parts.add(chatWindowColourKey);
    parts.add(chatTtsEnabled ? "1" : "0");
    parts.add(chatTtsVoiceId);
    parts.add(juce::String(chatTtsVolume, 3));
    parts.add(chatTtsOutputId);

    const int textureIdx = backgroundSelector.getSelectedItemIndex();
    parts.add((textureIdx >= 0 && textureIdx < textureFiles.size()) ? textureFiles[textureIdx].getFileName() : juce::String());

    if (includeProcessorState)
    {
        juce::MemoryBlock processorState;
        audioProcessor.getStateInformation(processorState);
        parts.add(processorState.getSize() > 0
            ? juce::Base64::toBase64(processorState.getData(), processorState.getSize())
            : juce::String());
    }

    return parts.joinIntoString("\n");
}

void NinjamVst3AudioProcessorEditor::savePersistentSettingsToDisk(bool includeProcessorState)
{
    const auto uiFingerprint = buildPersistentSettingsFingerprint(false);
    if (!includeProcessorState && uiFingerprint == lastSavedUiSettingsFingerprint)
    {
        persistentSettingsDirty = false;
        return;
    }

    auto popts = makeSettingsOptions();
    juce::PropertiesFile props(popts);

    props.setValue("server", serverField.getText());
    props.setValue("username", userField.getText());
    props.setValue("password", passField.getText());
    props.setValue("anonymous", anonymousButton.getToggleState());
    props.setValue("layoutVertical", layoutButton.getToggleState());
    props.setValue("chatVisible", chatButton.getToggleState());
    props.setValue("videoBgEnabled", videoBgToggle.getToggleState());
    props.setValue("autoLevelEnabled", autoLevelButton.getToggleState());
    props.setValue("spreadOutputs", spreadOutputsButton.getToggleState());
    props.setValue("abletonWindowSizePreset", abletonWindowSizePreset);
    props.setValue("abletonChatWindowSizePreset", abletonChatWindowSizePreset);
    props.setValue("abletonSamplerWindowSizePreset", abletonSamplerWindowSizePreset);
    props.setValue("samplePadsWindowBoundsValid", samplePadsWindowBoundsValid);
    if (samplePadsWindowBoundsValid)
    {
        props.setValue("samplePadsWindowX", samplePadsWindowBounds.getX());
        props.setValue("samplePadsWindowY", samplePadsWindowBounds.getY());
        props.setValue("samplePadsWindowW", samplePadsWindowBounds.getWidth());
        props.setValue("samplePadsWindowH", samplePadsWindowBounds.getHeight());
    }
    props.setValue("chatColourKey", audioProcessor.getLocalChatColourKey());
    props.setValue("chatWindowColourKey", chatWindowColourKey);
    props.setValue("chatTtsEnabled", chatTtsEnabled);
    props.setValue("chatTtsVoiceId", chatTtsVoiceId);
    props.setValue("chatTtsVolume", (double)chatTtsVolume);
    props.setValue("chatTtsOutputId", chatTtsOutputId);

    const int textureIdx = backgroundSelector.getSelectedItemIndex();
    if (textureIdx >= 0 && textureIdx < textureFiles.size())
        props.setValue("texture", textureFiles[textureIdx].getFileName());

    if (includeProcessorState)
    {
        juce::MemoryBlock processorState;
        audioProcessor.getStateInformation(processorState);
        if (processorState.getSize() > 0)
            props.setValue("pluginStateBase64", juce::Base64::toBase64(processorState.getData(), processorState.getSize()));
    }

    props.saveIfNeeded();
    lastSavedUiSettingsFingerprint = uiFingerprint;
    persistentSettingsDirty = false;
}

void NinjamVst3AudioProcessorEditor::loadPersistentSettingsFromDisk()
{
    migrateOldSettingsIfNeeded();

    auto popts = makeSettingsOptions();
    if (!renewSettingsFileIfCorrupt(popts, this))
        return;

    juce::PropertiesFile props(popts);

    const juce::String encodedState = props.getValue("pluginStateBase64");
    if (encodedState.isNotEmpty())
    {
        juce::MemoryOutputStream stateData;
        if (juce::Base64::convertFromBase64(stateData, encodedState))
        {
            const auto state = stateData.getMemoryBlock();
            if (state.getSize() > 0)
                audioProcessor.setStateInformation(state.getData(), (int) state.getSize());
        }
    }

    const juce::String savedServer = props.getValue("server", {});
    if (savedServer.isNotEmpty())
        serverField.setText(savedServer, juce::dontSendNotification);

    const juce::String savedUser = props.getValue("username", {});
    if (savedUser.isNotEmpty())
        userField.setText(savedUser, juce::dontSendNotification);

    passField.setText(props.getValue("password", {}), juce::dontSendNotification);
    anonymousButton.setToggleState(props.getBoolValue("anonymous", anonymousButton.getToggleState()), juce::dontSendNotification);
    anonymousToggled();

    layoutButton.setToggleState(props.getBoolValue("layoutVertical", layoutButton.getToggleState()), juce::dontSendNotification);
    layoutToggled();
    updateLayoutButtonColor();

    const bool chatVisible = props.getBoolValue("chatVisible", chatButton.getToggleState());
    chatButton.setToggleState(chatVisible, juce::dontSendNotification);
    chatToggled();

    videoBgToggle.setToggleState(props.getBoolValue("videoBgEnabled", videoBgToggle.getToggleState()), juce::dontSendNotification);

    autoLevelEnabled = props.getBoolValue("autoLevelEnabled", autoLevelEnabled);
    autoLevelButton.setToggleState(autoLevelEnabled, juce::dontSendNotification);
    audioProcessor.setSoftLimiterEnabled(autoLevelEnabled);
    updateAutoLevelButtonColor();

    const bool spreadOutputs = props.getBoolValue("spreadOutputs", spreadOutputsButton.getToggleState());
    spreadOutputsButton.setToggleState(spreadOutputs, juce::dontSendNotification);
    audioProcessor.setSpreadOutputsEnabled(spreadOutputs);

    audioProcessor.setLocalChatColourKey(props.getValue("chatColourKey", audioProcessor.getLocalChatColourKey()));
    setChatWindowColourKey(props.getValue("chatWindowColourKey", chatWindowColourKey), false);
    setChatTtsVoiceId(props.getValue("chatTtsVoiceId", chatTtsVoiceId), false);
    setChatTtsVolume((float)props.getDoubleValue("chatTtsVolume", chatTtsVolume), false);
    setChatTtsOutputId(props.getValue("chatTtsOutputId", chatTtsOutputId), false);
    setChatTtsEnabled(props.getBoolValue("chatTtsEnabled", chatTtsEnabled), false);

    abletonWindowSizePreset = juce::jlimit(0, 2, props.getIntValue("abletonWindowSizePreset", abletonWindowSizePreset));
    abletonChatWindowSizePreset = juce::jlimit(0, 2, props.getIntValue("abletonChatWindowSizePreset", abletonChatWindowSizePreset));
    abletonSamplerWindowSizePreset = juce::jlimit(0, 2, props.getIntValue("abletonSamplerWindowSizePreset", abletonSamplerWindowSizePreset));
    samplePadsWindowBoundsValid = props.getBoolValue("samplePadsWindowBoundsValid", false);
    if (samplePadsWindowBoundsValid)
    {
        samplePadsWindowBounds = juce::Rectangle<int>(props.getIntValue("samplePadsWindowX", 0),
                                                      props.getIntValue("samplePadsWindowY", 0),
                                                      juce::jlimit(840, 1320, props.getIntValue("samplePadsWindowW", 980)),
                                                      juce::jlimit(500, 840, props.getIntValue("samplePadsWindowH", 600)));
    }

    transmitButton.setToggleState(audioProcessor.isTransmittingLocal(), juce::dontSendNotification);
    updateTransmitButtonColor();

    localMonitorButton.setToggleState(audioProcessor.isLocalMonitorEnabled(), juce::dontSendNotification);
    updateMonitorButtonColor();

    voiceChatButton.setToggleState(audioProcessor.isVoiceChatMode(), juce::dontSendNotification);
    updateVoiceChatButtonColor();

    const int bitrates[] = { 64, 96, 128, 160, 192, 256, 320 };
    const int savedBitrate = audioProcessor.getLocalBitrate();
    int selectedBitrateId = 3;
    for (int i = 0; i < 7; ++i)
        if (bitrates[i] == savedBitrate)
            selectedBitrateId = i + 1;
    bitrateSelector.setSelectedId(selectedBitrateId, juce::dontSendNotification);

    const auto restoredSyncMode = audioProcessor.getSyncMode();
    if (restoredSyncMode != NinjamVst3AudioProcessor::SyncMode::off)
        preferredSyncMode = restoredSyncMode;
    audioProcessor.setSyncMode(NinjamVst3AudioProcessor::SyncMode::off);
    syncButton.setToggleState(false, juce::dontSendNotification);
    updateSyncButtonColor();
    updateSyncButtonTooltip();

    masterFader.setValue(audioProcessor.getMasterOutputGain(), juce::dontSendNotification);
    limiterButton.setToggleState(audioProcessor.isMasterLimiterEnabled(), juce::dontSendNotification);
    limiterThresholdSlider.setValue(audioProcessor.getLimiterThreshold(), juce::dontSendNotification);
    limiterReleaseSlider.setValue(audioProcessor.getLimiterRelease(), juce::dontSendNotification);
    updateLimiterButtonColor();

    // Re-apply the saved texture selection so the dropdown always reflects the loaded skin.
    // Something called above (setStateInformation, setSize, colour-scheme update) can
    // cause the ComboBox to lose its selection; re-selecting here fixes that without
    // triggering onChange (we don't want to re-save or reload images unnecessarily).
    {
        const juce::String savedTexture = props.getValue("texture", "");
        if (savedTexture.isNotEmpty())
        {
            for (int i = 0; i < textureFiles.size(); ++i)
            {
                if (textureFiles[i].getFileName() == savedTexture)
                {
                    backgroundSelector.setSelectedId(backgroundSelector.getItemId(i),
                                                    juce::dontSendNotification);
                    break;
                }
            }
        }
    }

    // Reapply the selected skin after restoring videoBgEnabled so a skin with
    // bg.mp4 cannot keep playing when the saved toggle state is off.
    {
        const int selectedTextureIndex = backgroundSelector.getSelectedItemIndex();
        if (selectedTextureIndex >= 0 && selectedTextureIndex < textureFiles.size())
            loadControlImages(textureFiles[selectedTextureIndex]);
        else
            stopBackgroundVideoReader();
    }
}

void NinjamVst3AudioProcessorEditor::clearLearnMappings()
{
    midiSourceByTargetId.clear();
    oscSourceByTargetId.clear();
    midiLearnArmedTargetId.clear();
    oscLearnArmedTargetId.clear();
    syncLearnMappingsToProcessor();
}

void NinjamVst3AudioProcessorEditor::connectClicked()
{
    const int status = audioProcessor.getClient().GetStatus();
    const bool isConnectedOrConnecting = (status == NJClient::NJC_STATUS_OK || status == NJClient::NJC_STATUS_PRECONNECT);
    if (!isConnectedOrConnecting)
    {
        juce::String user = userField.getText();
        juce::String pass = passField.getText();

        if (anonymousButton.getToggleState())
        {
            if (!user.startsWith("anonymous:"))
                user = "anonymous:" + user;
            pass = "";
        }

        audioProcessor.connectToServer(serverField.getText(), user, pass);
    }
    else
    {
        audioProcessor.disconnectFromServer();
        clearLearnMappings();
    }
}

void NinjamVst3AudioProcessorEditor::sendClicked()
{
    juce::String msg = expandChatEmojiShortcuts(chatInput.getText());
    if (msg.isNotEmpty())
    {
        audioProcessor.sendChatMessage(msg);
        chatInput.clear();
    }
}

void NinjamVst3AudioProcessorEditor::transmitToggled()
{
    audioProcessor.setTransmitLocal(transmitButton.getToggleState());
    updateTransmitButtonColor();
    savePersistentSettingsToDisk();
}

void NinjamVst3AudioProcessorEditor::layoutToggled()
{
    userList.setLayoutMode(layoutButton.getToggleState());
}

void NinjamVst3AudioProcessorEditor::metronomeChanged()
{
    // only update volume when not muted
    if (metronomeMuteButton.getToggleState())
        audioProcessor.setMetronomeVolume((float)metronomeSlider.getValue());
    else
    {
        storedMetronomeVolume = (float)metronomeSlider.getValue(); // update stored value silently
        audioProcessor.setStoredMetronomeVolume(storedMetronomeVolume);
    }
}

void NinjamVst3AudioProcessorEditor::chatToggled()
{
    if (!chatButton.getToggleState())
    {
        chatPopoutOpenPending = false;
        if (chatWindow)
        {
            if (auto* popup = getChatPopupComponent(chatWindow.get()))
                chatInput.setText(popup->getDraftText(), juce::dontSendNotification);
            chatWindow->setVisible(false);
            chatWindow.reset();
        }
        chatPoppedOut = false;
    }
    else
    {
        // Chat button toggles embedded chat on; any stale popout state should be cleared.
        if (chatPoppedOut && !chatPopoutOpenPending && (!chatWindow || !chatWindow->isVisible()))
            chatPoppedOut = false;
    }
    updateChatButtonColor();
    resized();
}

void NinjamVst3AudioProcessorEditor::openChatPopoutWindow(const juce::StringArray& history,
                                                          const juce::StringArray& senders,
                                                          const juce::String& draftText)
{
    chatPopoutOpenPending = false;

    if (!chatPoppedOut || !chatButton.getToggleState())
    {
        updateChatButtonColor();
        resized();
        return;
    }

    const bool abletonHostedWindow = !audioProcessor.isStandaloneWrapper() && isAbletonLiveHost();

    if (!chatWindow)
    {
        juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
        chatWindow.reset(new ChatWindow(audioProcessor,
                                        chatWindowColourKey,
                                        [safeThis](const juce::String&)
                                        {
                                            if (safeThis != nullptr)
                                                safeThis->markPersistentSettingsDirty();
                                        },
                                        [safeThis](const juce::String& key)
                                        {
                                            if (safeThis != nullptr)
                                                safeThis->setChatWindowColourKey(key, true);
                                        },
                                        [safeThis]() -> bool
                                        {
                                            return safeThis != nullptr && safeThis->chatTtsEnabled;
                                        },
                                        [safeThis]() -> juce::String
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsVoiceId : juce::String();
                                        },
                                        [safeThis]() -> juce::StringArray
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsVoiceIds : juce::StringArray();
                                        },
                                        [safeThis]() -> juce::StringArray
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsVoiceNames : juce::StringArray();
                                        },
                                        [safeThis]() -> float
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsVolume : 1.0f;
                                        },
                                        [safeThis]() -> juce::String
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsOutputId : juce::String();
                                        },
                                        [safeThis]() -> juce::StringArray
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsOutputIds : juce::StringArray();
                                        },
                                        [safeThis]() -> juce::StringArray
                                        {
                                            return safeThis != nullptr ? safeThis->chatTtsOutputNames : juce::StringArray();
                                        },
                                        [safeThis](bool enabled)
                                        {
                                            if (safeThis != nullptr)
                                                safeThis->setChatTtsEnabled(enabled, true);
                                        },
                                        [safeThis](const juce::String& voiceId)
                                        {
                                            if (safeThis != nullptr)
                                                safeThis->setChatTtsVoiceId(voiceId, true);
                                        },
                                        [safeThis](float volume)
                                        {
                                            if (safeThis != nullptr)
                                                safeThis->setChatTtsVolume(volume, true);
                                        },
                                        [safeThis](const juce::String& outputId)
                                        {
                                            if (safeThis != nullptr)
                                                safeThis->setChatTtsOutputId(outputId, true);
                                        },
                                        [safeThis]()
                                        {
                                            if (safeThis == nullptr)
                                                return;

                                            safeThis->chatPopoutOpenPending = false;
                                            if (safeThis->chatWindow)
                                                if (auto* popup = getChatPopupComponent(safeThis->chatWindow.get()))
                                                    safeThis->chatInput.setText(popup->getDraftText(), juce::dontSendNotification);

                                            safeThis->chatWindow.reset();
                                            safeThis->chatPoppedOut = false;
                                            safeThis->chatButton.setToggleState(false, juce::dontSendNotification);
                                            safeThis->updateChatButtonColor();
                                            safeThis->resized();
                                        },
                                        abletonHostedWindow,
                                        abletonChatWindowSizePreset));
    }
    else
    {
        if (auto* popout = dynamic_cast<ChatWindow*>(chatWindow.get()))
            popout->applyAbletonSizePreset(abletonChatWindowSizePreset);
        chatWindow->setVisible(true);
        chatWindow->toFront(false);
    }

    if (auto* popup = getChatPopupComponent(chatWindow.get()))
    {
        popup->setChatWindowColourKey(chatWindowColourKey);
        popup->setChatText(history, senders);
        popup->setDraftText(draftText);
    }

    updateChatButtonColor();
    resized();
}

void NinjamVst3AudioProcessorEditor::chatPopoutClicked()
{
    if (!chatButton.getToggleState())
    {
        chatButton.setToggleState(true, juce::dontSendNotification);
        updateChatButtonColor();
    }

    juce::StringArray history;
    juce::StringArray senders;
    {
        const juce::ScopedLock lock(audioProcessor.chatLock);
        history = audioProcessor.chatHistory;
        senders = audioProcessor.chatSenders;
    }

    if (!chatPoppedOut)
    {
        chatPoppedOut = true;
        const juce::String draftText = chatInput.getText();
        if (!audioProcessor.isStandaloneWrapper() && isAbletonLiveHost())
        {
            chatPopoutOpenPending = true;
            juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
            juce::MessageManager::callAsync([safeThis, history, senders, draftText]
            {
                if (safeThis != nullptr)
                    safeThis->openChatPopoutWindow(history, senders, draftText);
            });
        }
        else
        {
            openChatPopoutWindow(history, senders, draftText);
        }
    }
    else
    {
        chatPopoutOpenPending = false;
        chatPoppedOut = false;
        if (chatWindow)
        {
            if (auto* popup = getChatPopupComponent(chatWindow.get()))
                chatInput.setText(popup->getDraftText(), juce::dontSendNotification);
            chatWindow->setVisible(false);
            chatWindow.reset();
        }
    }

    updateChatButtonColor();
    resized();
}

void NinjamVst3AudioProcessorEditor::updateSamplePadsFeatureVisibility()
{
    const bool enabled = audioProcessor.isSamplePadsFeatureEnabled();
    samplePadsButton.setVisible(enabled);
    samplePadsButton.setEnabled(enabled);

    if (!enabled && samplePadsWindow)
    {
        samplePadsWindow->setVisible(false);
        samplePadsWindow.reset();
    }

    updateEditorTimerInterval();
}

void NinjamVst3AudioProcessorEditor::showSamplePadsWindow()
{
    if (!audioProcessor.isSamplePadsFeatureEnabled())
        return;

    if (samplePadsWindow)
    {
        if (auto* samplerWindow = dynamic_cast<SamplePadsWindow*>(samplePadsWindow.get()))
            samplerWindow->applyAbletonSizePreset(abletonSamplerWindowSizePreset);
        samplePadsWindow->setVisible(true);
        samplePadsWindow->toFront(false);
        updateEditorTimerInterval();
        return;
    }

    const bool abletonHostedWindow = !audioProcessor.isStandaloneWrapper() && isAbletonLiveHost();
    juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
    auto openWindow = [safeThis, abletonHostedWindow]
    {
        if (safeThis == nullptr
            || safeThis->samplePadsWindow
            || !safeThis->audioProcessor.isSamplePadsFeatureEnabled())
            return;

        auto* editorPtr = safeThis.getComponent();
        juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> callbackSafeThis(editorPtr);
        editorPtr->samplePadsWindow.reset(new SamplePadsWindow(editorPtr->audioProcessor,
                                                              *editorPtr,
                                                              abletonHostedWindow,
                                                              editorPtr->abletonSamplerWindowSizePreset,
                                                              editorPtr->samplePadsWindowBoundsValid
                                                                  ? editorPtr->samplePadsWindowBounds
                                                                  : juce::Rectangle<int>(),
                                                              [callbackSafeThis]
                                                              {
                                                                  if (callbackSafeThis != nullptr)
                                                                      callbackSafeThis->refreshExternalMidiInputDevices();
                                                              },
                                                              [callbackSafeThis](int preset)
                                                              {
                                                                  if (callbackSafeThis != nullptr)
                                                                      callbackSafeThis->setAbletonSamplerWindowSizePreset(preset);
                                                              },
                                                              [callbackSafeThis](juce::Rectangle<int> bounds, bool saveNow)
                                                              {
                                                                  if (callbackSafeThis != nullptr)
                                                                      callbackSafeThis->rememberSamplePadsWindowBounds(bounds, saveNow);
                                                              },
                                                              [callbackSafeThis]
                                                              {
                                                                  if (callbackSafeThis != nullptr)
                                                                  {
                                                                      callbackSafeThis->samplePadsWindow.reset();
                                                                      callbackSafeThis->updateEditorTimerInterval();
                                                                  }
                                                              }));
        editorPtr->updateEditorTimerInterval();
    };

    if (abletonHostedWindow)
        juce::MessageManager::callAsync(std::move(openWindow));
    else
        openWindow();
}

void NinjamVst3AudioProcessorEditor::anonymousToggled()
{
    const bool showPassword = !anonymousButton.getToggleState();
    passLabel.setVisible(showPassword);
    passField.setVisible(showPassword);
    passField.setEnabled(showPassword);
    resized();
}

void NinjamVst3AudioProcessorEditor::atToggled()
{
    audioProcessor.setAutoTranslateEnabled(atButton.getToggleState());
    updateTranslateButtonState();
}

void NinjamVst3AudioProcessorEditor::showTranslateLanguageMenu(juce::Component& anchorComponent)
{
    juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
    showTranslateLanguageMenuForButton(audioProcessor, anchorComponent, [safeThis]()
    {
        if (safeThis != nullptr)
            safeThis->updateTranslateButtonState();
    });
}

void NinjamVst3AudioProcessorEditor::showSyncCompensationMenu(juce::Component& anchorComponent)
{
    static constexpr float presetValuesMs[] = { 0.0f, 5.0f, 10.0f, 15.0f, 20.0f, 25.0f, 32.0f, 40.0f,
                                                50.0f, 64.0f, 80.0f, 96.0f, 128.0f, 160.0f, 192.0f, 250.0f };

    auto idToMs = std::make_shared<std::map<int, float>>();
    juce::PopupMenu presetMenu;
    const float currentMs = audioProcessor.getSyncStartCompensationMs();

    int compensationId = 100;
    for (float presetMs : presetValuesMs)
    {
        ++compensationId;
        presetMenu.addItem(compensationId,
                           juce::String(presetMs, presetMs < 10.0f ? 1 : 0) + " ms",
                           true,
                           std::abs(currentMs - presetMs) < 0.1f);
        (*idToMs)[compensationId] = presetMs;
    }

    const auto activeMode = audioProcessor.getSyncMode();
    const auto selectedMode = activeMode != NinjamVst3AudioProcessor::SyncMode::off ? activeMode : preferredSyncMode;

    juce::PopupMenu sourceMenu;
    sourceMenu.addItem(10, "Host Transport", !audioProcessor.isStandaloneWrapper(), selectedMode == NinjamVst3AudioProcessor::SyncMode::host);
    sourceMenu.addItem(11, "Ableton Link", true, selectedMode == NinjamVst3AudioProcessor::SyncMode::abletonLink);

    juce::PopupMenu menu;
    menu.addSectionHeader("Transport Sync");
    menu.addSubMenu("Sync Source", sourceMenu);
    menu.addSeparator();
    menu.addSectionHeader("Start Compensation");
    menu.addSubMenu("Advance NINJAM Start", presetMenu);

    juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&anchorComponent),
                       [safeThis, idToMs](int result) mutable
                       {
                           if (safeThis == nullptr || result == 0)
                               return;

                           if (result == 10 || result == 11)
                           {
                               safeThis->preferredSyncMode = (result == 11)
                                   ? NinjamVst3AudioProcessor::SyncMode::abletonLink
                                   : NinjamVst3AudioProcessor::SyncMode::host;
                               if (safeThis->syncButton.getToggleState()
                                   || safeThis->audioProcessor.getSyncMode() != NinjamVst3AudioProcessor::SyncMode::off)
                               {
                                   safeThis->audioProcessor.setSyncMode(safeThis->preferredSyncMode);
                                   safeThis->syncButton.setToggleState(true, juce::dontSendNotification);
                               }
                               safeThis->updateSyncButtonTooltip();
                               safeThis->updateSyncButtonColor();
                               return;
                           }

                           const auto it = idToMs->find(result);
                           if (it == idToMs->end())
                               return;

                           safeThis->audioProcessor.setSyncStartCompensationMs(it->second);
                           safeThis->updateSyncButtonTooltip();
                       });
}

void NinjamVst3AudioProcessorEditor::updateTranslateButtonState()
{
    atButton.setToggleState(audioProcessor.isAutoTranslateEnabled(), juce::dontSendNotification);
    atButton.setTooltip(buildTranslateTooltip(audioProcessor.getTranslateTargetLang()));

    if (chatWindow)
    {
        if (auto* popup = dynamic_cast<ChatPopupComponent*>(chatWindow->getContentComponent()))
            popup->refreshTranslateButtonState();
    }
}

void NinjamVst3AudioProcessorEditor::syncToggled()
{
    bool enabled = syncButton.getToggleState();
    if (!enabled)
    {
        audioProcessor.setSyncMode(NinjamVst3AudioProcessor::SyncMode::off);
        updateSyncButtonTooltip();
        return;
    }

    auto modeToEnable = preferredSyncMode;
    if (modeToEnable == NinjamVst3AudioProcessor::SyncMode::off)
        modeToEnable = audioProcessor.isStandaloneWrapper()
            ? NinjamVst3AudioProcessor::SyncMode::abletonLink
            : NinjamVst3AudioProcessor::SyncMode::host;

    preferredSyncMode = modeToEnable;
    audioProcessor.setSyncMode(modeToEnable);

    if (modeToEnable == NinjamVst3AudioProcessor::SyncMode::abletonLink)
    {
        const double linkBpm = audioProcessor.getLinkTempoBpm();
        const bool linkPlaying = audioProcessor.isLinkTransportPlaying();
        const float njBpm = audioProcessor.getBPM();
        juce::String message;

        if (linkBpm > 0.0 && njBpm > 0.0f && std::abs(linkBpm - (double) njBpm) > 0.5)
        {
            message << "Ableton Link tempo (" << juce::String(linkBpm, 1)
                    << ") is different from NINJAM BPM ("
                    << juce::String(njBpm, 1) << ").\n";
        }

        if (linkPlaying)
        {
            if (message.isNotEmpty())
                message << "\n";
            message << "Ableton Link is already playing.\n"
                    << "NINJAM will wait for the next Link restart before it starts.";
        }

        if (message.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Ableton Link Sync", message);

        updateSyncButtonTooltip();
        return;
    }

    double hostBpm = 0.0;
    bool hostPlaying = false;
    {
        juce::AudioPlayHead::CurrentPositionInfo info;
        if (audioProcessor.getHostPosition(info))
        {
            hostBpm = info.bpm;
            hostPlaying = info.isPlaying;
        }
    }

    float njBpm = audioProcessor.getBPM();
    juce::String message;
    bool anyWarning = false;

    if (hostBpm > 0.0 && njBpm > 0.0f)
    {
        double diff = std::abs(hostBpm - (double)njBpm);
        if (diff > 0.5)
        {
            anyWarning = true;
            message << "Host BPM (" << juce::String(hostBpm, 1)
                    << ") is different from NINJAM BPM ("
                    << juce::String(njBpm, 1) << ").\n";
        }
    }

    if (hostPlaying)
    {
        anyWarning = true;
        message << "The DAW is currently playing.\n"
                << "Stop the DAW, move the playhead to your desired start bar,\n"
                << "then press Play to hear NINJAM in sync with the host.";
    }

    if (!anyWarning)
    {
        updateSyncButtonTooltip();
        return;
    }

    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Sync to Host", message);
    updateSyncButtonTooltip();
}

void NinjamVst3AudioProcessorEditor::videoClicked()
{
    auto disableBackgroundVideo = [this]
    {
        if (videoBgToggle.getToggleState())
        {
            videoBgToggle.setToggleState(false, juce::dontSendNotification);
            stopBackgroundVideoReader();
            markPersistentSettingsDirty();
            repaint();
        }
    };

    const bool connectedToServer = audioProcessor.getClient().GetStatus() == NJClient::NJC_STATUS_OK;
    if (!connectedToServer)
    {
        disableBackgroundVideo();
        audioProcessor.launchVideoSessionAsync();
        return;
    }

    if (!audioProcessor.isNinjamZapServerSupported())
    {
        disableBackgroundVideo();
        audioProcessor.launchVideoSessionAsync();
        return;
    }

    juce::PopupMenu menu;
    menu.addItem(1, "VDO Video");
    menu.addItem(2, "NINJAMZap Video");

    juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&videoButton),
                       [safeThis](int result)
                       {
                           if (safeThis == nullptr)
                               return;

                           if (result == 1)
                           {
                               safeThis->videoBgToggle.setToggleState(false, juce::dontSendNotification);
                               safeThis->stopBackgroundVideoReader();
                               safeThis->markPersistentSettingsDirty();
                               safeThis->repaint();
                               safeThis->audioProcessor.launchVideoSessionAsync();
                           }
                           else if (result == 2)
                           {
                               safeThis->videoBgToggle.setToggleState(false, juce::dontSendNotification);
                               safeThis->stopBackgroundVideoReader();
                               safeThis->markPersistentSettingsDirty();
                               safeThis->repaint();
                               safeThis->audioProcessor.launchNinjamZapVideoSession();
                           }
                       });
}

void NinjamVst3AudioProcessorEditor::serverListClicked()
{
    if (serverListWindow == nullptr)
    {
        serverListWindow.reset(new ServerListWindow(
            audioProcessor,
            [this](const juce::String& hostPort)
            {
                serverField.setText(hostPort, juce::dontSendNotification);
            },
            [this](const juce::String& hostPort)
            {
                serverField.setText(hostPort, juce::dontSendNotification);
                if (audioProcessor.getClient().GetStatus() != NJClient::NJC_STATUS_DISCONNECTED)
                    audioProcessor.disconnectFromServer();

                juce::String user = userField.getText();
                juce::String pass = passField.getText();
                if (anonymousButton.getToggleState())
                {
                    if (!user.startsWith("anonymous:"))
                        user = "anonymous:" + user;
                    pass = "";
                }

                audioProcessor.connectToServer(serverField.getText(), user, pass);
                if (serverListWindow != nullptr)
                    serverListWindow->setVisible(false);
            }));
    }
    else
    {
        serverListWindow->setVisible(true);
        serverListWindow->toFront(true);
    }
}

void NinjamVst3AudioProcessorEditor::refreshLocalInputSelectors()
{
    for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
        refreshLocalInputSelector(i);
}

void NinjamVst3AudioProcessorEditor::refreshMidiRelayTargetSelector()
{
    const juce::String selectedTarget = audioProcessor.getMidiRelayTarget();
    const juce::String trimmed = selectedTarget.trim();
    if (trimmed.isEmpty() || trimmed == "*")
    {
        midiRelayTargetSelector.setButtonText("MIDI/OSC->All");
        return;
    }

    juce::StringArray parts;
    parts.addTokens(trimmed, ",", "");
    parts.trim();
    parts.removeEmptyStrings();

    if (parts.isEmpty())
    {
        midiRelayTargetSelector.setButtonText("MIDI/OSC->All");
        return;
    }
    if (parts.size() == 1)
    {
        midiRelayTargetSelector.setButtonText("MIDI/OSC->" + parts[0]);
        return;
    }
    if (parts.size() == 2)
    {
        midiRelayTargetSelector.setButtonText("MIDI/OSC->" + parts[0] + "," + parts[1]);
        return;
    }
    midiRelayTargetSelector.setButtonText("MIDI/OSC->" + juce::String(parts.size()) + " users");
}

void NinjamVst3AudioProcessorEditor::showMidiRelayTargetMenu()
{
    const juce::String selectedTarget = audioProcessor.getMidiRelayTarget().trim();
    const bool allSelected = selectedTarget.isEmpty() || selectedTarget == "*";

    juce::StringArray selectedUsers;
    if (!allSelected)
    {
        selectedUsers.addTokens(selectedTarget, ",", "");
        selectedUsers.trim();
        selectedUsers.removeEmptyStrings();
    }

    midiRelayTargetByMenuId.clear();
    juce::PopupMenu menu;

    int id = 1;
    menu.addItem(id, "All", true, allSelected);
    midiRelayTargetByMenuId[id] = "*";
    ++id;

    menu.addSeparator();

    std::set<juce::String> seen;
    for (const auto& user : audioProcessor.getConnectedUsers())
    {
        if (user.name.isEmpty() || seen.find(user.name) != seen.end())
            continue;
        seen.insert(user.name);

        bool ticked = false;
        if (!allSelected)
        {
            for (const auto& s : selectedUsers)
                if (s.equalsIgnoreCase(user.name))
                    ticked = true;
        }

        menu.addItem(id, user.name, true, ticked);
        midiRelayTargetByMenuId[id] = user.name;
        ++id;
    }

    const auto screenPos = midiRelayTargetSelector.getScreenPosition();
    juce::Rectangle<int> popupAnchor(screenPos.x, screenPos.y + midiRelayTargetSelector.getHeight(), midiRelayTargetSelector.getWidth(), 1);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&midiRelayTargetSelector).withTargetScreenArea(popupAnchor),
                       [this, allSelected, selectedUsers](int result) mutable
                       {
                           if (result <= 0)
                               return;
                           auto it = midiRelayTargetByMenuId.find(result);
                           if (it == midiRelayTargetByMenuId.end())
                               return;

                           const juce::String chosen = it->second;
                           if (chosen.isEmpty() || chosen == "*")
                           {
                               audioProcessor.setMidiRelayTarget("*");
                               refreshMidiRelayTargetSelector();
                               return;
                           }

                           if (allSelected)
                               selectedUsers.clear();

                           int existingIndex = -1;
                           for (int i = 0; i < selectedUsers.size(); ++i)
                               if (selectedUsers[i].equalsIgnoreCase(chosen))
                                   existingIndex = i;

                           if (existingIndex >= 0)
                               selectedUsers.remove(existingIndex);
                           else
                               selectedUsers.add(chosen);

                           if (selectedUsers.isEmpty())
                               audioProcessor.setMidiRelayTarget("*");
                           else
                               audioProcessor.setMidiRelayTarget(selectedUsers.joinIntoString(","));
                           refreshMidiRelayTargetSelector();
                       });
}

void NinjamVst3AudioProcessorEditor::oscMessageReceived(const juce::OSCMessage& message)
{
    PendingOscEvent event;
    event.address = message.getAddressPattern().toString();
    if (message.size() > 0)
    {
        const auto arg = message[0];
        float raw = 1.0f;
        if (arg.isFloat32()) raw = arg.getFloat32();
        else if (arg.isInt32()) raw = (float)arg.getInt32();
        event.normalized = raw > 1.0f ? juce::jlimit(0.0f, 1.0f, raw / 127.0f) : juce::jlimit(0.0f, 1.0f, raw);
        event.binaryOn = raw >= 0.5f;
    }
    else
    {
        event.normalized = 1.0f;
        event.binaryOn = true;
    }

    {
        NinjamVst3AudioProcessor::OscRelayEvent relayEvent;
        relayEvent.address = event.address;
        relayEvent.normalized = event.normalized;
        relayEvent.binaryOn = event.binaryOn;
        audioProcessor.enqueueOutboundOscRelayEvent(relayEvent);
    }

    const juce::SpinLock::ScopedLockType lock(oscEventQueueLock);
    pendingOscEvents.push_back(event);
    if (pendingOscEvents.size() > 512)
        pendingOscEvents.erase(pendingOscEvents.begin(), pendingOscEvents.begin() + (long long)(pendingOscEvents.size() - 512));
}

void NinjamVst3AudioProcessorEditor::refreshLocalInputSelector(int channel)
{
    if (channel < 0 || channel >= NinjamVst3AudioProcessor::maxLocalChannels)
        return;

    auto& selector = localInputSelectors[(size_t)channel];
    selector.clear(juce::dontSendNotification);

    int total = audioProcessor.getTotalNumInputChannels();
    if (total <= 0) total = 2;
    int numPairs = total / 2;

    for (int ch = 0; ch < total; ++ch)
        selector.addItem("In " + juce::String(ch + 1), ch + 1);

    int stereoBaseId = 100;
    for (int pair = 0; pair < numPairs; ++pair)
    {
        int left = pair * 2 + 1;
        int right = left + 1;
        selector.addItem(juce::String(left) + "/" + juce::String(right), stereoBaseId + pair);
    }

    const juce::String linkInputLabel = buildLinkAudioLocalInputLabel(audioProcessor);
    if (linkInputLabel.isNotEmpty())
        selector.addItem(linkInputLabel, kLocalInputLinkSelectorId);

    int currentInput = audioProcessor.getLocalChannelInput(channel);
    if (audioProcessor.isLocalChannelUsingLinkAudioInput(channel))
    {
        selector.setSelectedId(kLocalInputLinkSelectorId, juce::dontSendNotification);
    }
    else if (currentInput >= 0 && currentInput < total)
    {
        selector.setSelectedId(currentInput + 1, juce::dontSendNotification);
    }
    else if (currentInput < 0)
    {
        int pairIndex = -1 - currentInput;
        if (numPairs > pairIndex)
        {
            selector.setSelectedId(stereoBaseId + pairIndex, juce::dontSendNotification);
        }
        else if (numPairs > 0)
        {
            selector.setSelectedId(stereoBaseId, juce::dontSendNotification);
            audioProcessor.setLocalChannelInput(channel, -1);
        }
        else if (total > 0)
        {
            selector.setSelectedId(1, juce::dontSendNotification);
            audioProcessor.setLocalChannelInput(channel, 0);
        }
    }

    if (channel >= 0 && channel < NinjamVst3AudioProcessor::maxLocalChannels)
    localInputModeSelectors[(size_t)channel].setSelectedId(audioProcessor.isLocalChannelUsingLinkAudioInput(channel) || currentInput < 0 ? 2 : 1,
                                   juce::dontSendNotification);
}

bool NinjamVst3AudioProcessorEditor::isSidechainInputActive() const
{
    return audioProcessor.getTotalNumInputChannels() > 2;
}

void NinjamVst3AudioProcessorEditor::loadControlImages(const juce::File& themeDir)
{
    backgroundImage = juce::Image();
    backgroundComponent.setBackgroundImage({});
    lastVideoBackgroundRepaintMs = 0.0;

    // Try bg.mp4 when the Video BG toggle is on (Windows only)
    bool videoLoaded = false;

#if JUCE_WINDOWS
    videoFrameReader.reset();
    const bool allowAnimatedBackground = !(isAbletonLiveHost() && !audioProcessor.isStandaloneWrapper());
    if (videoBgToggle.getToggleState() && allowAnimatedBackground)
    {
        auto videoFile = themeDir.getChildFile("bg.mp4");
        if (videoFile.existsAsFile())
        {
            videoFrameReader = std::make_unique<WinVideoReader>();
            if (videoFrameReader->open(videoFile))
                videoLoaded = true;
            else
                videoFrameReader.reset();
        }
    }
#endif

    if (!videoLoaded)
    {
        // Fall back to bg.* image files (bg.jpg, bg.png, bg.gif, etc.)
        auto bgFiles = themeDir.findChildFiles(juce::File::findFiles, false, "bg.*");
        if (!bgFiles.isEmpty())
        {
            backgroundImage = juce::ImageFileFormat::loadFrom(bgFiles[0]);
            backgroundComponent.setBackgroundImage(backgroundImage);
        }
    }

    // fknob.png — fader knob image
    faderKnobImage = juce::ImageFileFormat::loadFrom(themeDir.getChildFile("fknob.png"));

    // rknob.png — radio/release knob image
    radioKnobImage = juce::ImageFileFormat::loadFrom(themeDir.getChildFile("rknob.png"));

    // Sand 1 shell controls can look too translucent; make them appear more solid
    // with an extra draw pass (safer than mutating source image pixels).
    sandSkinOpaqueKnobs = themeDir.getFileName().equalsIgnoreCase("Sand 1");

    // Reset theme colours to defaults before reading cfg
    metronomeThemeColour = juce::Colour::fromRGB(80, 185, 255);
    windowThemeColour    = juce::Colour(0x00000000);
    buttonThemeColour    = juce::Colour(0x00000000);
    menuBarThemeColour   = juce::Colour(0x00000000);
    knobColourPreset     = "grey";
    faderColourPreset    = "grey";
    knobThemeColour      = juce::Colours::grey;
    faderThemeColour     = juce::Colour(0xff666666);

    // Parse companion skin.cfg if present
    auto cfgFile = themeDir.getChildFile("skin.cfg");
    if (cfgFile.existsAsFile())
    {
        auto lines = juce::StringArray::fromLines(cfgFile.loadFileAsString());
        auto parseHex = [](const juce::String& val, juce::Colour& out) -> bool
        {
            auto s = val.trim().trimCharactersAtStart("#");
            if (s.length() == 6 && s.containsOnly("0123456789abcdefABCDEF"))
            {
                out = juce::Colour::fromString("ff" + s);
                return true;
            }
            return false;
        };
        for (const auto& line : lines)
        {
            auto trimmed = line.trim();
            if (trimmed.startsWith("#") || trimmed.isEmpty()) continue;
            auto val     = trimmed.fromFirstOccurrenceOf(":", false, false).trim();
            if (trimmed.startsWithIgnoreCase("Metronome Colour:"))
                parseHex(val, metronomeThemeColour);
            else if (trimmed.startsWithIgnoreCase("Window Colour:"))
                parseHex(val, windowThemeColour);
            else if (trimmed.startsWithIgnoreCase("Button Colour:"))
                parseHex(val, buttonThemeColour);
            else if (trimmed.startsWithIgnoreCase("MenuBar Colour:"))
                parseHex(val, menuBarThemeColour);
            else if (trimmed.startsWithIgnoreCase("Knobs:"))
            {
                // If an image knob is provided, ignore skin.cfg knob colours for that skin.
                if (!radioKnobImage.isValid())
                {
                    knobColourPreset = val;
                    knobThemeColour = colourFromPresetName(knobColourPreset, juce::Colours::grey);
                }
            }
            else if (trimmed.startsWithIgnoreCase("Faders:"))
            {
                // If an image fader knob is provided, ignore skin.cfg fader colours for that skin.
                if (!faderKnobImage.isValid())
                {
                    faderColourPreset = val;
                    faderThemeColour = colourFromPresetName(faderColourPreset, juce::Colour(0xff666666));
                }
            }
        }
    }

    applyThemeColours();
    updateEditorTimerInterval();
    backgroundComponent.repaint();
    repaint();
}

void NinjamVst3AudioProcessorEditor::applyThemeColours()
{
    metronomeBtnLAF.themeColour = metronomeThemeColour;
    metronomeMuteButton.repaint();

    backgroundComponent.setFallbackColour(windowThemeColour.getAlpha() > 0
        ? windowThemeColour
        : juce::Colour(0xff222222));

    // Apply Window Colour to the global LAF palette so all component backgrounds pick it up.
    // If no Window Colour is set, restore JUCE defaults.
    if (windowThemeColour.getAlpha() > 0)
    {
        auto bg   = windowThemeColour;
        auto bgDk = bg.darker(0.25f);
        auto bgLt = bg.brighter(0.15f);

        // drawDocumentWindowTitleBar reads widgetBackground from the colour scheme directly
        // (bypasses colour IDs entirely), so we must patch the scheme to change the title bar.
        auto titleBarBg = menuBarThemeColour.getAlpha() > 0 ? menuBarThemeColour : bg;
        auto scheme = outlinedLabelLAF.getCurrentColourScheme();
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::UIColour::windowBackground, titleBarBg);
        scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::UIColour::widgetBackground, titleBarBg);
        outlinedLabelLAF.setColourScheme(scheme); // also calls initialiseColours(), resetting colour IDs

        // Now apply per-component colour ID overrides (override what setColourScheme just initialised)
        outlinedLabelLAF.setColour(juce::ResizableWindow::backgroundColourId,     bg);
        outlinedLabelLAF.setColour(juce::DocumentWindow::backgroundColourId,       bg);
        outlinedLabelLAF.setColour(juce::ComboBox::backgroundColourId,             bgDk);
        outlinedLabelLAF.setColour(juce::ListBox::backgroundColourId,              bgDk);
        outlinedLabelLAF.setColour(juce::TextEditor::backgroundColourId,           bgDk);
        outlinedLabelLAF.setColour(juce::TextButton::buttonColourId,
            buttonThemeColour.getAlpha() > 0 ? buttonThemeColour : bgLt);
        outlinedLabelLAF.setColour(juce::Slider::backgroundColourId,               bgDk);
        outlinedLabelLAF.setColour(juce::GroupComponent::outlineColourId,          bg.brighter(0.3f));
        outlinedLabelLAF.setColour(juce::PopupMenu::backgroundColourId,            bgDk);
        outlinedLabelLAF.setColour(juce::PopupMenu::highlightedBackgroundColourId, bgLt);
        outlinedLabelLAF.setColour(juce::AlertWindow::backgroundColourId,          bg);
        outlinedLabelLAF.setColour(juce::AlertWindow::textColourId,                juce::Colours::white);
        outlinedLabelLAF.setColour(juce::AlertWindow::outlineColourId,             bg.brighter(0.45f));
    }
    else
    {
        // Restore the dark colour scheme (JUCE default); this also resets all colour IDs.
        // If a MenuBar Colour is set without a Window Colour, still patch widgetBackground.
        if (menuBarThemeColour.getAlpha() > 0)
        {
            auto scheme = juce::LookAndFeel_V4::getDarkColourScheme();
            scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::UIColour::windowBackground, menuBarThemeColour);
            scheme.setUIColour(juce::LookAndFeel_V4::ColourScheme::UIColour::widgetBackground, menuBarThemeColour);
            outlinedLabelLAF.setColourScheme(scheme);
        }
        else
        {
            outlinedLabelLAF.setColourScheme(juce::LookAndFeel_V4::getDarkColourScheme());
        }
        if (buttonThemeColour.getAlpha() > 0)
            outlinedLabelLAF.setColour(juce::TextButton::buttonColourId, buttonThemeColour);
        outlinedLabelLAF.setColour(juce::AlertWindow::backgroundColourId,          juce::Colour(0xff20282c));
        outlinedLabelLAF.setColour(juce::AlertWindow::textColourId,                juce::Colours::white);
        outlinedLabelLAF.setColour(juce::AlertWindow::outlineColourId,             juce::Colour(0xff8fa1aa));
    }

    repaint();
    sendLookAndFeelChange();

    for (int i = 0; i < NinjamVst3AudioProcessor::maxLocalChannels; ++i)
    {
        localReverbSendLabels[(size_t)i].setColour(juce::Label::textColourId, knobThemeColour);
        localDelaySendLabels[(size_t)i].setColour(juce::Label::textColourId, knobThemeColour);
    }

    // Force the DocumentWindow title bar to repaint using the updated scheme.
    if (auto* dw = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
    {
        auto effectiveTitleCol = menuBarThemeColour.getAlpha() > 0 ? menuBarThemeColour
                               : windowThemeColour.getAlpha()    > 0 ? windowThemeColour
                               : juce::LookAndFeel_V4::getDarkColourScheme()
                                   .getUIColour(juce::LookAndFeel_V4::ColourScheme::UIColour::windowBackground);
        dw->setBackgroundColour(effectiveTitleCol);
        dw->repaint();
    }
}

void NinjamVst3AudioProcessorEditor::parentHierarchyChanged()
{
    // Re-apply title bar colour now that we may be properly parented under the DocumentWindow
    if (auto* dw = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent()))
    {
        auto effectiveTitleCol = menuBarThemeColour.getAlpha() > 0 ? menuBarThemeColour
                               : windowThemeColour.getAlpha()    > 0 ? windowThemeColour
                               : juce::LookAndFeel_V4::getDarkColourScheme()
                                   .getUIColour(juce::LookAndFeel_V4::ColourScheme::UIColour::windowBackground);
        dw->setBackgroundColour(effectiveTitleCol);
        dw->repaint();
    }
}

bool NinjamVst3AudioProcessorEditor::shouldDeferHeavyUiWork() const
{
    if (audioProcessor.isStandaloneWrapper())
        return false;
    if (!isAbletonLiveHost())
        return false;
    if (pendingDeferredResizeLayout || applyingDeferredResizeLayout)
        return true;
    return juce::Time::getMillisecondCounterHiRes() < suppressHeavyUiUntilMs;
}

void NinjamVst3AudioProcessorEditor::updateEditorTimerInterval()
{
    const bool abletonHostEditor = !audioProcessor.isStandaloneWrapper() && isAbletonLiveHost();

#if JUCE_WINDOWS
    const bool animatedBackgroundActive = videoFrameReader != nullptr;
#else
    const bool animatedBackgroundActive = false;
#endif

    int nextIntervalMs = 30;
    if (abletonHostEditor)
    {
        nextIntervalMs = 50;
    }
    else if (animatedBackgroundActive)
    {
#if JUCE_WINDOWS
        const int videoFrameMs = videoFrameReader != nullptr ? videoFrameReader->getFramePeriodMs() : 33;
#else
        const int videoFrameMs = 33;
#endif
        nextIntervalMs = juce::jlimit(16, 50, videoFrameMs);
    }

    if (currentEditorTimerIntervalMs != nextIntervalMs)
    {
        currentEditorTimerIntervalMs = nextIntervalMs;
        startTimer(nextIntervalMs);
    }
}

bool NinjamVst3AudioProcessorEditor::isAbletonLiveHost() const
{
    return juce::PluginHostType().isAbletonLive();
}

void NinjamVst3AudioProcessorEditor::setAbletonWindowSizePreset(int presetIndex)
{
    if (audioProcessor.isStandaloneWrapper() || !isAbletonLiveHost())
        return;

    abletonWindowSizePreset = juce::jlimit(0, 2, presetIndex);

    const auto targetSize = abletonEditorWindowSizeForPreset(abletonWindowSizePreset);

    pendingDeferredResizeLayout = false;
    applyingDeferredResizeLayout = false;
    setResizable(false, false);
    setResizeLimits(targetSize.getWidth(),
                    targetSize.getHeight(),
                    targetSize.getWidth(),
                    targetSize.getHeight());
    setSize(targetSize.getWidth(), targetSize.getHeight());
    suppressHeavyUiUntilMs = juce::Time::getMillisecondCounterHiRes() + 400.0;
    hostResizeLockedForConnection = true;

    auto popts = makeSettingsOptions();
    juce::PropertiesFile props(popts);
    props.setValue("abletonWindowSizePreset", abletonWindowSizePreset);
    props.saveIfNeeded();
}

void NinjamVst3AudioProcessorEditor::setAbletonChatWindowSizePreset(int presetIndex)
{
    if (audioProcessor.isStandaloneWrapper() || !isAbletonLiveHost())
        return;

    abletonChatWindowSizePreset = juce::jlimit(0, 2, presetIndex);

    if (auto* popout = dynamic_cast<ChatWindow*>(chatWindow.get()))
        popout->applyAbletonSizePreset(abletonChatWindowSizePreset);

    auto popts = makeSettingsOptions();
    juce::PropertiesFile props(popts);
    props.setValue("abletonChatWindowSizePreset", abletonChatWindowSizePreset);
    props.saveIfNeeded();
}

void NinjamVst3AudioProcessorEditor::setAbletonSamplerWindowSizePreset(int presetIndex)
{
    if (audioProcessor.isStandaloneWrapper() || !isAbletonLiveHost())
        return;

    abletonSamplerWindowSizePreset = juce::jlimit(0, 2, presetIndex);

    if (auto* samplerWindow = dynamic_cast<SamplePadsWindow*>(samplePadsWindow.get()))
        samplerWindow->applyAbletonSizePreset(abletonSamplerWindowSizePreset);

    auto popts = makeSettingsOptions();
    juce::PropertiesFile props(popts);
    props.setValue("abletonSamplerWindowSizePreset", abletonSamplerWindowSizePreset);
    props.saveIfNeeded();
}

void NinjamVst3AudioProcessorEditor::rememberSamplePadsWindowBounds(juce::Rectangle<int> bounds, bool saveNow)
{
    if (bounds.getWidth() < 100 || bounds.getHeight() < 100)
        return;

    bounds.setSize(juce::jlimit(840, 1320, bounds.getWidth()),
                   juce::jlimit(500, 840, bounds.getHeight()));

    if (samplePadsWindowBoundsValid && samplePadsWindowBounds == bounds && !saveNow)
        return;

    samplePadsWindowBounds = bounds;
    samplePadsWindowBoundsValid = true;
    markPersistentSettingsDirty();

    if (saveNow)
        savePersistentSettingsToDisk(false);
}

void NinjamVst3AudioProcessorEditor::updateHostResizeModeForConnectionStatus(int status)
{
    if (audioProcessor.isStandaloneWrapper())
        return;

    const bool shouldLock = isAbletonLiveHost();
    if (shouldLock == hostResizeLockedForConnection)
        return;

    if (shouldLock)
    {
        const int currentWidth = getWidth();
        const int currentHeight = getHeight();
        pendingDeferredResizeLayout = false;
        applyingDeferredResizeLayout = false;
        setResizable(false, false);
        setResizeLimits(currentWidth, currentHeight, currentWidth, currentHeight);
        suppressHeavyUiUntilMs = juce::Time::getMillisecondCounterHiRes() + 500.0;
    }
    else
    {
        setResizable(true, true);
        setResizeLimits(900, 500, 2200, 1500);
    }

    hostResizeLockedForConnection = shouldLock;
}

void NinjamVst3AudioProcessorEditor::updateAutoLevelButtonColor()
{
    if (autoLevelButton.getToggleState())
    {
        juce::Colour on = juce::Colour::fromRGB(240, 220, 30);   // bright yellow
        autoLevelButton.setColour(juce::TextButton::buttonColourId,   on);
        autoLevelButton.setColour(juce::TextButton::buttonOnColourId, on);
        autoLevelButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::black);
        autoLevelButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    }
    else
    {
        juce::Colour off = juce::Colour::fromRGB(55, 50, 10);    // dim yellow
        autoLevelButton.setColour(juce::TextButton::buttonColourId,   off);
        autoLevelButton.setColour(juce::TextButton::buttonOnColourId, off);
        autoLevelButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::grey);
        autoLevelButton.setColour(juce::TextButton::textColourOffId, juce::Colours::grey);
    }
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateChatButtonColor()
{
    const bool chatShowing = chatButton.getToggleState()
        && (!chatPoppedOut || chatPopoutOpenPending || (chatWindow && chatWindow->isVisible()));
    if (chatShowing)
    {
        const juce::Colour on = juce::Colour::fromRGB(80, 190, 255);
        chatButton.setColour(juce::TextButton::buttonColourId, on);
        chatButton.setColour(juce::TextButton::buttonOnColourId, on);
        chatButton.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        chatButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
        chatButton.setButtonText("Chat");
        chatButton.setTooltip(chatPoppedOut ? "Chat is showing in popout window" : "Chat is showing");
    }
    else
    {
        const juce::Colour off = juce::Colour::fromRGB(15, 40, 55);
        chatButton.setColour(juce::TextButton::buttonColourId, off);
        chatButton.setColour(juce::TextButton::buttonOnColourId, off);
        chatButton.setColour(juce::TextButton::textColourOnId, juce::Colours::lightgrey);
        chatButton.setColour(juce::TextButton::textColourOffId, juce::Colours::lightgrey);
        chatButton.setButtonText("Chat");
        chatButton.setTooltip("Open Chat");
    }
    chatButton.repaint();
}

void NinjamVst3AudioProcessorEditor::updateTransmitButtonColor()
{
    if (transmitButton.getToggleState())
    {
        juce::Colour on = juce::Colour::fromRGB(50, 200, 80);    // bright green when transmitting
        transmitButton.setColour(juce::TextButton::buttonColourId,   on);
        transmitButton.setColour(juce::TextButton::buttonOnColourId, on);
        transmitButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::black);
        transmitButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
        transmitButton.setTooltip({});
    }
    else
    {
        juce::Colour off = juce::Colour::fromRGB(12, 50, 18);    // dim green
        transmitButton.setColour(juce::TextButton::buttonColourId,   off);
        transmitButton.setColour(juce::TextButton::buttonOnColourId, off);
        transmitButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::grey);
        transmitButton.setColour(juce::TextButton::textColourOffId, juce::Colours::grey);
        transmitButton.setTooltip("Transmit is off, others won't hear you");
    }
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateMonitorButtonColor()
{
    if (localMonitorButton.getToggleState())
    {
        juce::Colour on = juce::Colour::fromRGB(220, 55, 55);    // bright red when monitoring
        localMonitorButton.setColour(juce::TextButton::buttonColourId,   on);
        localMonitorButton.setColour(juce::TextButton::buttonOnColourId, on);
        localMonitorButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::white);
        localMonitorButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    }
    else
    {
        juce::Colour off = juce::Colour::fromRGB(60, 15, 15);    // dim red
        localMonitorButton.setColour(juce::TextButton::buttonColourId,   off);
        localMonitorButton.setColour(juce::TextButton::buttonOnColourId, off);
        localMonitorButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::grey);
        localMonitorButton.setColour(juce::TextButton::textColourOffId, juce::Colours::grey);
    }
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateLimiterButtonColor()
{
    if (limiterButton.getToggleState())
    {
        juce::Colour on = juce::Colour::fromRGB(220, 55, 55);    // bright red when active
        limiterButton.setColour(juce::TextButton::buttonColourId,   on);
        limiterButton.setColour(juce::TextButton::buttonOnColourId, on);
        limiterButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::white);
        limiterButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    }
    else
    {
        juce::Colour off = juce::Colour::fromRGB(60, 15, 15);    // dim red
        limiterButton.setColour(juce::TextButton::buttonColourId,   off);
        limiterButton.setColour(juce::TextButton::buttonOnColourId, off);
        limiterButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::grey);
        limiterButton.setColour(juce::TextButton::textColourOffId, juce::Colours::grey);
    }
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateVoiceChatButtonColor()
{
    if (voiceChatButton.getToggleState())
    {
        // Pulse between dim amber and bright amber (~1 s cycle)
        voiceChatGlowPhase += juce::MathConstants<float>::twoPi * 30.0f / 1000.0f;
        float t = (std::sin(voiceChatGlowPhase) + 1.0f) * 0.5f; // 0..1
        uint8 r = (uint8)(100 + (uint8)(155 * t));
        uint8 g = (uint8)(50  + (uint8)(100 * t));
        juce::Colour pulse = juce::Colour::fromRGB(r, g, 0);
        voiceChatButton.setColour(juce::TextButton::buttonColourId,   pulse);
        voiceChatButton.setColour(juce::TextButton::buttonOnColourId, pulse);
        voiceChatButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::black);
        voiceChatButton.setColour(juce::TextButton::textColourOffId, juce::Colours::black);
    }
    else
    {
        voiceChatGlowPhase = 0.0f;
        juce::Colour off = juce::Colour::fromRGB(50, 30, 0);
        voiceChatButton.setColour(juce::TextButton::buttonColourId,   off);
        voiceChatButton.setColour(juce::TextButton::buttonOnColourId, off);
        voiceChatButton.setColour(juce::TextButton::textColourOnId,  juce::Colours::orange);
        voiceChatButton.setColour(juce::TextButton::textColourOffId, juce::Colours::orange);
    }
}

void NinjamVst3AudioProcessorEditor::updateLayoutButtonColor()
{
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateMetronomeButtonColor()
{
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateSyncButtonColor()
{
    repaint();
}

void NinjamVst3AudioProcessorEditor::updateSyncButtonTooltip()
{
    const auto activeMode = audioProcessor.getSyncMode();
    const auto tooltipMode = activeMode != NinjamVst3AudioProcessor::SyncMode::off ? activeMode : preferredSyncMode;
    syncButton.setTooltip(buildSyncTooltip(tooltipMode, audioProcessor.getSyncStartCompensationMs()));
}

void NinjamVst3AudioProcessorEditor::updateFxButtonLabel()
{
    fxButton.setButtonText("FX");
}

void NinjamVst3AudioProcessorEditor::showFxMenu()
{
    audioProcessor.setFxReverbEnabled(true);
    audioProcessor.setFxDelayEnabled(true);

    juce::PopupMenu menu;
    menu.addItem(2, "Reverb");
    menu.addItem(3, "Delay");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&fxButton),
        [this](int result)
        {
            if (result == 0)
                return;

            audioProcessor.setFxReverbEnabled(true);
            audioProcessor.setFxDelayEnabled(true);
            if (result == 2)
                showReverbSettingsPopup();
            if (result == 3)
                showDelaySettingsPopup();
            updateFxButtonLabel();
            updateFxControlsVisibility();
            repaint();
        });
}

void NinjamVst3AudioProcessorEditor::showOptionsMenu()
{
    juce::PopupMenu menu;
    const auto activeMode = audioProcessor.getSyncMode();
    const auto selectedMode = activeMode != NinjamVst3AudioProcessor::SyncMode::off ? activeMode : preferredSyncMode;

    constexpr int metronomeClassicMenuId = 70;
    constexpr int metronomeSoftBeepMenuId = 71;
    constexpr int metronomeSoftTickMenuId = 72;
    constexpr int metronomeWoodTickMenuId = 73;
    constexpr int metronomeOpenFolderMenuId = 74;
    constexpr int metronomeOutputMonoMenuIdBase = 8000;
    constexpr int metronomeOutputStereoMenuIdBase = 8200;
    constexpr int metronomeOutputMonoFlag = 1024;
    constexpr int metronomeCustomMenuIdBase = 9000;
    const juce::String selectedMetronomeSoundKey = audioProcessor.getMetronomeSoundKey();
    const juce::Array<juce::File> customMetronomeFiles = audioProcessor.findCustomMetronomeSoundFiles();

    juce::PopupMenu syncSourceMenu;
    syncSourceMenu.addItem(44,
                           "VST Host",
                           !audioProcessor.isStandaloneWrapper(),
                           selectedMode == NinjamVst3AudioProcessor::SyncMode::host);
    syncSourceMenu.addItem(45,
                           "Ableton Link",
                           true,
                           selectedMode == NinjamVst3AudioProcessor::SyncMode::abletonLink);

    juce::PopupMenu metronomeSoundMenu;
    metronomeSoundMenu.addItem(metronomeClassicMenuId, "Classic", true,
                               selectedMetronomeSoundKey == NinjamVst3AudioProcessor::getClassicMetronomeSoundKey());
    metronomeSoundMenu.addItem(metronomeSoftBeepMenuId, "Soft Beep", true,
                               selectedMetronomeSoundKey == NinjamVst3AudioProcessor::getSoftBeepMetronomeSoundKey());
    metronomeSoundMenu.addItem(metronomeSoftTickMenuId, "Soft Tick", true,
                               selectedMetronomeSoundKey == NinjamVst3AudioProcessor::getSoftTickMetronomeSoundKey());
    metronomeSoundMenu.addItem(metronomeWoodTickMenuId, "Wood Tick", true,
                               selectedMetronomeSoundKey == NinjamVst3AudioProcessor::getWoodTickMetronomeSoundKey());
    metronomeSoundMenu.addSeparator();
    const int customFileCount = juce::jmin(customMetronomeFiles.size(), 64);
    for (int i = 0; i < customFileCount; ++i)
    {
        const juce::String key = NinjamVst3AudioProcessor::makeCustomMetronomeSoundKey(customMetronomeFiles[i]);
        metronomeSoundMenu.addItem(metronomeCustomMenuIdBase + i,
                                   customMetronomeFiles[i].getFileNameWithoutExtension(),
                                   true,
                                   selectedMetronomeSoundKey == key);
    }
    metronomeSoundMenu.addSeparator();
    metronomeSoundMenu.addItem(metronomeOpenFolderMenuId, "Open User Sounds Folder...");
    juce::PopupMenu metronomeOutputMenu;
    const int selectedMetronomeOutput = audioProcessor.getMetronomeOutputChannel();
    const bool selectedMetronomeOutputMono = (selectedMetronomeOutput & metronomeOutputMonoFlag) != 0;
    const int selectedMetronomeOutputChannel = selectedMetronomeOutput & ~metronomeOutputMonoFlag;
    const int totalMetronomeOutputs = juce::jmax(1, audioProcessor.getTotalNumOutputChannels());
    const int stereoPairCount = juce::jmax(1, (totalMetronomeOutputs + 1) / 2);
    for (int pair = 0; pair < stereoPairCount; ++pair)
    {
        const int leftChannel = pair * 2;
        const int rightChannel = juce::jmin(leftChannel + 1, totalMetronomeOutputs - 1);
        const auto label = pair == 0
            ? juce::String("Main Out 1/2")
            : juce::String("Out ") + juce::String(leftChannel + 1) + "/" + juce::String(rightChannel + 1);
        metronomeOutputMenu.addItem(metronomeOutputStereoMenuIdBase + pair,
                                    label,
                                    true,
                                    !selectedMetronomeOutputMono && selectedMetronomeOutputChannel == leftChannel);
    }

    metronomeOutputMenu.addSeparator();
    for (int channel = 0; channel < totalMetronomeOutputs; ++channel)
    {
        metronomeOutputMenu.addItem(metronomeOutputMonoMenuIdBase + channel,
                                    juce::String("Out ") + juce::String(channel + 1) + " (mono)",
                                    true,
                                    selectedMetronomeOutputMono && selectedMetronomeOutputChannel == channel);
    }

    if (standaloneOptionsMenuHandler)
    {
        menu.addItem(40, "Standalone Settings...");
        menu.addSeparator();
    }

    menu.addItem(41, "Midi Settings");
    menu.addItem(42, "Enable Chord Detection", true, audioProcessor.isChordDetectionEnabled());
    menu.addItem(46, "Enable Sample Pads / Looper", true, audioProcessor.isSamplePadsFeatureEnabled());
    menu.addItem(43, "Ableton Link Audio");
    menu.addItem(47, "Check for Updates...");
    menu.addSubMenu("Metronome Sound", metronomeSoundMenu);
    menu.addSubMenu("Metronome Output", metronomeOutputMenu);
    menu.addSubMenu("Transport Sync Source", syncSourceMenu);
    if (isAbletonLiveHost() && !audioProcessor.isStandaloneWrapper())
    {
        juce::PopupMenu sizeMenu;
        sizeMenu.addItem(51, "Small", true, abletonWindowSizePreset == 0);
        sizeMenu.addItem(52, "Medium", true, abletonWindowSizePreset == 1);
        sizeMenu.addItem(53, "Large", true, abletonWindowSizePreset == 2);
        menu.addSubMenu("Window Size", sizeMenu);

        juce::PopupMenu chatSizeMenu;
        chatSizeMenu.addItem(54, "Small", true, abletonChatWindowSizePreset == 0);
        chatSizeMenu.addItem(55, "Medium", true, abletonChatWindowSizePreset == 1);
        chatSizeMenu.addItem(56, "Large", true, abletonChatWindowSizePreset == 2);
        menu.addSubMenu("Chat Popout Size", chatSizeMenu);
    }

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&optionsButton),
        [this, customMetronomeFiles](int result)
        {
            if (result == 0)
                return;
            if (result == 40)
            {
                if (standaloneOptionsMenuHandler)
                    standaloneOptionsMenuHandler(&optionsButton);
                return;
            }
            if (result == 41)
                showMidiOptionsPopup();
            if (result == 42)
                audioProcessor.setChordDetectionEnabled(!audioProcessor.isChordDetectionEnabled());
            if (result == 43)
                showLinkAudioOptionsPopup();
            if (result == 46)
            {
                audioProcessor.setSamplePadsFeatureEnabled(!audioProcessor.isSamplePadsFeatureEnabled());
                updateSamplePadsFeatureVisibility();
                refreshExternalMidiInputDevices();
                resized();
                repaint();
                markPersistentSettingsDirty();
            }
            if (result == 47)
            {
                beginUpdateCheck(true);
                return;
            }
            constexpr int callbackMetronomeOutputMonoMenuIdBase = 8000;
            constexpr int callbackMetronomeOutputStereoMenuIdBase = 8200;
            constexpr int callbackMetronomeOutputCustomSoundMenuIdBase = 9000;
            constexpr int callbackMetronomeOutputMonoFlag = 1024;
            if (result >= callbackMetronomeOutputMonoMenuIdBase && result < callbackMetronomeOutputStereoMenuIdBase)
            {
                const int channel = result - callbackMetronomeOutputMonoMenuIdBase;
                audioProcessor.setMetronomeOutputChannel(channel | callbackMetronomeOutputMonoFlag);
                markPersistentSettingsDirty();
                return;
            }
            if (result >= callbackMetronomeOutputStereoMenuIdBase && result < callbackMetronomeOutputCustomSoundMenuIdBase)
            {
                const int pair = result - callbackMetronomeOutputStereoMenuIdBase;
                audioProcessor.setMetronomeOutputChannel(pair * 2);
                markPersistentSettingsDirty();
                return;
            }
            if (result >= 70 && result <= 73)
            {
                juce::String key = NinjamVst3AudioProcessor::getClassicMetronomeSoundKey();
                if (result == 71)
                    key = NinjamVst3AudioProcessor::getSoftBeepMetronomeSoundKey();
                else if (result == 72)
                    key = NinjamVst3AudioProcessor::getSoftTickMetronomeSoundKey();
                else if (result == 73)
                    key = NinjamVst3AudioProcessor::getWoodTickMetronomeSoundKey();

                if (!audioProcessor.setMetronomeSoundKey(key))
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                           "Metronome sound",
                                                           "That metronome sound could not be loaded.",
                                                           "OK",
                                                           this);
                markPersistentSettingsDirty();
            }
            if (result == 74)
            {
                const juce::File dir = audioProcessor.getUserMetronomeSoundsDirectory();
                if (!dir.isDirectory())
                    dir.createDirectory();
                dir.revealToUser();
                return;
            }
            if (result >= 9000)
            {
                const int index = result - 9000;
                if (index >= 0 && index < customMetronomeFiles.size())
                {
                    if (!audioProcessor.setMetronomeSoundKey(NinjamVst3AudioProcessor::makeCustomMetronomeSoundKey(customMetronomeFiles[index])))
                    {
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                               "Metronome sound",
                                                               "That metronome sound file could not be loaded.",
                                                               "OK",
                                                               this);
                    }
                    markPersistentSettingsDirty();
                }
            }
            if (result == 44 || result == 45)
            {
                preferredSyncMode = (result == 45)
                    ? NinjamVst3AudioProcessor::SyncMode::abletonLink
                    : NinjamVst3AudioProcessor::SyncMode::host;

                if (audioProcessor.isTransportSyncEnabled())
                    audioProcessor.setSyncMode(preferredSyncMode);

                updateSyncButtonTooltip();
                updateSyncButtonColor();
            }
            if (result == 51) setAbletonWindowSizePreset(0);
            if (result == 52) setAbletonWindowSizePreset(1);
            if (result == 53) setAbletonWindowSizePreset(2);
            if (result == 54) setAbletonChatWindowSizePreset(0);
            if (result == 55) setAbletonChatWindowSizePreset(1);
            if (result == 56) setAbletonChatWindowSizePreset(2);
        });
}

void NinjamVst3AudioProcessorEditor::showAboutWindow()
{
    if (aboutWindow != nullptr && aboutWindow->isVisible())
    {
        aboutWindow->toFront(true);
        return;
    }

    juce::String version = audioProcessor.getVersionString();
    aboutWindow = std::make_unique<AboutWindow>(version);
    aboutWindow->setVisible(true);
    aboutWindow->toFront(true);
    aboutWindow->enterModalState(true, nullptr, false);
}

void NinjamVst3AudioProcessorEditor::beginUpdateCheck(bool userInitiated)
{
    bool expected = false;
    if (!updateCheckInProgress.compare_exchange_strong(expected, true))
    {
        if (userInitiated)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "Check for Updates",
                                                   "An update check is already running.",
                                                   "OK",
                                                   this);
        }
        return;
    }

    const juce::String currentVersion = audioProcessor.getVersionString();
    const bool standaloneBuild = audioProcessor.isStandaloneWrapper();
    juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);

    std::thread([safeThis, currentVersion, standaloneBuild, userInitiated]
    {
        auto result = fetchLatestNinjamReleaseInfo(currentVersion, standaloneBuild);

        juce::MessageManager::callAsync([safeThis, userInitiated, result = std::move(result)]() mutable
        {
            if (safeThis == nullptr)
                return;

            safeThis->updateCheckInProgress.store(false);
            safeThis->completeUpdateCheck(userInitiated,
                                          result.requestOk,
                                          result.updateAvailable,
                                          result.latestVersion,
                                          result.releaseUrl,
                                          result.downloadUrl,
                                          result.errorMessage);
        });
    }).detach();
}

void NinjamVst3AudioProcessorEditor::completeUpdateCheck(bool userInitiated,
                                                         bool requestOk,
                                                         bool updateAvailable,
                                                         const juce::String& latestVersion,
                                                         const juce::String& releaseUrl,
                                                         const juce::String& downloadUrl,
                                                         const juce::String& errorMessage)
{
    if (!requestOk)
    {
        if (userInitiated)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Update Check Failed",
                                                   errorMessage.isNotEmpty() ? errorMessage : "The latest release could not be checked.",
                                                   "OK",
                                                   this);
        }
        return;
    }

    if (!updateAvailable)
    {
        if (userInitiated)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "NINJAMplus is Up to Date",
                                                   "You are running " + audioProcessor.getVersionString() + ".",
                                                   "OK",
                                                   this);
        }
        return;
    }

    showUpdateAvailablePrompt(latestVersion, releaseUrl, downloadUrl);
}

void NinjamVst3AudioProcessorEditor::showUpdateAvailablePrompt(const juce::String& latestVersion,
                                                               const juce::String& releaseUrl,
                                                               const juce::String& downloadUrl)
{
    if (updatePromptShowing)
        return;

    updatePromptShowing = true;
    const bool standaloneBuild = audioProcessor.isStandaloneWrapper();
    const juce::String actionUrl = (standaloneBuild && downloadUrl.isNotEmpty()) ? downloadUrl : releaseUrl;
    juce::String message;
    message << "Current version: " << audioProcessor.getVersionString() << "\n"
            << "Latest version: " << latestVersion << "\n\n";

    if (standaloneBuild)
    {
        message << "Click Download Update to open the installer or release download in your browser. "
                << "Close NINJAMplus before running the installer.";
    }
    else
    {
        message << "This plugin is running inside a DAW. Open the release page, download the plugin for your system, "
                << "then close the DAW before installing the new version.";
    }

    auto* alert = new juce::AlertWindow("NINJAMplus Update Available",
                                        message,
                                        juce::AlertWindow::InfoIcon);
    alert->addButton(standaloneBuild && downloadUrl.isNotEmpty() ? "Download Update" : "Open Release", 1,
                     juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Later", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<NinjamVst3AudioProcessorEditor> safeThis(this);
    alert->enterModalState(true,
                           juce::ModalCallbackFunction::create(
                               [safeThis, alert, actionUrl](int result)
                               {
                                   std::unique_ptr<juce::AlertWindow> alertOwner(alert);
                                   if (safeThis != nullptr)
                                       safeThis->updatePromptShowing = false;

                                   if (result == 1 && actionUrl.isNotEmpty())
                                       juce::URL(actionUrl).launchInDefaultBrowser();
                               }),
                           false);
}
void NinjamVst3AudioProcessorEditor::showReverbSettingsPopup()
{
    showSettingsCallout(std::make_unique<ReverbSettingsPopupComponent>(audioProcessor), fxButton);
}

void NinjamVst3AudioProcessorEditor::showDelaySettingsPopup()
{
    showSettingsCallout(std::make_unique<DelaySettingsPopupComponent>(audioProcessor), fxButton);
}

void NinjamVst3AudioProcessorEditor::showMidiOptionsPopup()
{
    showSettingsCallout(std::make_unique<MidiOptionsPopupComponent>(audioProcessor, [this] { refreshExternalMidiInputDevices(); }),
                        optionsButton.isShowing() ? static_cast<juce::Component&>(optionsButton)
                                                  : static_cast<juce::Component&>(fxButton));
}

void NinjamVst3AudioProcessorEditor::showLinkAudioOptionsPopup()
{
    showSettingsCallout(std::make_unique<LinkAudioOptionsPopupComponent>(audioProcessor),
                        optionsButton.isShowing() ? static_cast<juce::Component&>(optionsButton)
                                                  : static_cast<juce::Component&>(fxButton));
}

void NinjamVst3AudioProcessorEditor::showSettingsCallout(std::unique_ptr<juce::Component> content, juce::Component& anchorComponent)
{
    auto anchorOnScreen = anchorComponent.getScreenBounds();
    juce::Rectangle<int> target(anchorOnScreen.getX() + 8, anchorOnScreen.getBottom() + 2, 2, 2);
    auto& box = juce::CallOutBox::launchAsynchronously(std::move(content), target, nullptr);
    box.setLookAndFeel(&noArrowCallOutLookAndFeel);
    box.setArrowSize(0.0f);
    box.setTopLeftPosition(anchorOnScreen.getX(), anchorOnScreen.getBottom() + 4);
}

void NinjamVst3AudioProcessorEditor::refreshExternalMidiInputDevices()
{
    const juce::String desiredLearnId = audioProcessor.getMidiLearnInputDeviceId();
    const juce::String desiredRelayId = audioProcessor.getMidiRelayInputDeviceId();
    const bool samplePadsFeatureEnabled = audioProcessor.isSamplePadsFeatureEnabled();
    const juce::String desiredPadsId = samplePadsFeatureEnabled ? audioProcessor.getSamplePadsMidiInputDeviceId()
                                                                : juce::String();
    const bool desiredPadsRelay = desiredPadsId == NinjamVst3AudioProcessor::samplePadsMidiInputRelayId;

    if (desiredLearnId != openedMidiLearnInputDeviceId)
    {
        midiLearnInputDevice.reset();
        openedMidiLearnInputDeviceId.clear();
        if (desiredLearnId.isNotEmpty())
        {
            midiLearnInputDevice = juce::MidiInput::openDevice(desiredLearnId, this);
            if (midiLearnInputDevice != nullptr)
            {
                midiLearnInputDevice->start();
                openedMidiLearnInputDeviceId = desiredLearnId;
            }
        }
    }

    if (desiredRelayId == openedMidiLearnInputDeviceId && desiredRelayId.isNotEmpty())
    {
        midiRelayInputDevice.reset();
        openedMidiRelayInputDeviceId = desiredRelayId;
    }
    else if (desiredRelayId != openedMidiRelayInputDeviceId)
    {
        midiRelayInputDevice.reset();
        openedMidiRelayInputDeviceId.clear();
        if (desiredRelayId.isNotEmpty())
        {
            midiRelayInputDevice = juce::MidiInput::openDevice(desiredRelayId, this);
            if (midiRelayInputDevice != nullptr)
            {
                midiRelayInputDevice->start();
                openedMidiRelayInputDeviceId = desiredRelayId;
            }
        }
    }

    if (desiredPadsRelay)
    {
        samplePadsMidiInputDevice.reset();
        openedSamplePadsMidiInputDeviceId = desiredPadsId;
        return;
    }

    if (desiredPadsId.isNotEmpty()
        && (desiredPadsId == openedMidiLearnInputDeviceId || desiredPadsId == openedMidiRelayInputDeviceId))
    {
        samplePadsMidiInputDevice.reset();
        openedSamplePadsMidiInputDeviceId = desiredPadsId;
        return;
    }

    if (desiredPadsId != openedSamplePadsMidiInputDeviceId)
    {
        samplePadsMidiInputDevice.reset();
        openedSamplePadsMidiInputDeviceId.clear();
        if (desiredPadsId.isNotEmpty())
        {
            samplePadsMidiInputDevice = juce::MidiInput::openDevice(desiredPadsId, this);
            if (samplePadsMidiInputDevice != nullptr)
            {
                samplePadsMidiInputDevice->start();
                openedSamplePadsMidiInputDeviceId = desiredPadsId;
            }
        }
    }
}

void NinjamVst3AudioProcessorEditor::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message)
{
    if (source == nullptr)
        return;

    const juce::String sourceId = source->getIdentifier();
    const juce::String learnDeviceId = audioProcessor.getMidiLearnInputDeviceId();
    const juce::String relayDeviceId = audioProcessor.getMidiRelayInputDeviceId();
    const bool samplePadsFeatureEnabled = audioProcessor.isSamplePadsFeatureEnabled();
    const juce::String padsDeviceId = samplePadsFeatureEnabled ? audioProcessor.getSamplePadsMidiInputDeviceId()
                                                               : juce::String();
    const bool forPads = samplePadsFeatureEnabled && padsDeviceId.isNotEmpty() && sourceId == padsDeviceId;
    const bool forLearn = (learnDeviceId.isNotEmpty() && sourceId == learnDeviceId)
        || (forPads && midiLearnArmedTargetId.startsWith("samplepad.trigger."));
    const bool forRelay = relayDeviceId.isNotEmpty() && sourceId == relayDeviceId;
    if (!forLearn && !forRelay && !forPads)
        return;

    NinjamVst3AudioProcessor::MidiControllerEvent event;
    if (message.isController())
    {
        event.isController = true;
        event.midiChannel = message.getChannel();
        event.number = message.getControllerNumber();
        event.value = message.getControllerValue();
        event.normalized = (float)event.value / 127.0f;
        event.isNoteOn = event.value >= 64;
    }
    else if (message.isNoteOnOrOff())
    {
        event.isController = false;
        event.midiChannel = message.getChannel();
        event.number = message.getNoteNumber();
        event.value = message.getVelocity();
        event.normalized = message.isNoteOn() ? ((float)event.value / 127.0f) : 0.0f;
        event.isNoteOn = message.isNoteOn();
    }
    else
    {
        return;
    }

    if (forPads && !event.isController)
        audioProcessor.handleSamplePadMidiNote(event.number, event.isNoteOn);

    audioProcessor.enqueueExternalMidiControllerEvent(event, forLearn, forRelay);
}

void NinjamVst3AudioProcessorEditor::updateFxControlsVisibility()
{
    reverbRoomLabel.setVisible(false);
    reverbRoomSlider.setVisible(false);
    delayTimeLabel.setVisible(false);
    delayTimeSlider.setVisible(false);
    delayDivisionSelector.setVisible(false);
    delayPingPongButton.setVisible(false);
}

// ==============================================================================
// UserChannelStrip Implementation
// ==============================================================================

UserChannelStrip::UserChannelStrip(NinjamVst3AudioProcessor& p, int userIdx)
    : processor(p), userIndex(userIdx)
{
    // Initialise per-channel state
    for (int i = 0; i < kMaxRemoteCh; ++i)
    {
        perChannelGain[i] = 1.0f;
        channelPeaks[i]   = 0.0f;
    }

    setOpaque(false);
    addAndMakeVisible(nameLabel);
    nameLabel.setJustificationType(juce::Justification::centred);
    nameLabel.setColour(juce::Label::textColourId, juce::Colours::white);

    addAndMakeVisible(volumeSlider);
    volumeSlider.setSliderStyle(juce::Slider::LinearVertical);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    volumeSlider.setRange(0.0, 2.0);
    volumeSlider.setSkewFactorFromMidPoint(0.25);
    volumeSlider.setSliderSnapsToMousePosition(false);
    volumeSlider.setValue(1.0, juce::dontSendNotification);
    volumeSlider.setDoubleClickReturnValue(true, 1.0);
    volumeSlider.setLookAndFeel(&faderLookAndFeel);
    volumeSlider.onValueChange = [this] { volumeChanged(); };

    addAndMakeVisible(panSlider);
    panSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    panSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    panSlider.setRange(-1.0, 1.0);
    panSlider.setValue(0.0, juce::dontSendNotification);
    panSlider.setDoubleClickReturnValue(true, 0.0);
    panSlider.onValueChange = [this] { panChanged(); };

    addAndMakeVisible(muteButton);
    muteButton.setClickingTogglesState(true);
    muteBtnLAF.isMute = true;
    muteButton.setLookAndFeel(&muteBtnLAF);
    muteButton.onClick = [this] { muteChanged(); };

    addAndMakeVisible(soloButton);
    soloButton.setClickingTogglesState(true);
    soloBtnLAF.isMute = false;
    soloButton.setLookAndFeel(&soloBtnLAF);
    soloButton.onClick = [this] { soloChanged(); };

    addAndMakeVisible(outputSelector);

    addAndMakeVisible(chordLabel);
    chordLabel.setJustificationType(juce::Justification::centred);
    chordLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff202428));
    chordLabel.setColour(juce::Label::outlineColourId, juce::Colour(0xff48515a));
    chordLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    chordLabel.setFont(juce::Font(11.0f, juce::Font::bold));
    chordLabel.setInterceptsMouseClicks(false, false);
    chordLabel.setTooltip("Detected remote chord. Click to turn it off for this user.");

    addAndMakeVisible(dbLabel);
    dbLabel.setJustificationType(juce::Justification::centred);
    dbLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black);
    dbLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    dbLabel.setFont(juce::Font(11.0f));

    refreshOutputSelectorItems();

    // Expand button — shows ">" in list layout for multichan peers
    expandButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff333333));
    expandButton.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    addChildComponent(expandButton); // hidden until isMultiChanPeer
    expandButton.onClick = [this] { toggleExpanded(); };

    // Per-channel volume sliders and name labels (hidden until expanded)
    for (int i = 0; i < kMaxRemoteCh; ++i)
    {
        channelSliders[i].setSliderStyle(juce::Slider::LinearHorizontal);
        channelSliders[i].setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        channelSliders[i].setRange(0.0, 2.0);
        channelSliders[i].setSkewFactorFromMidPoint(0.25);
        channelSliders[i].setSliderSnapsToMousePosition(false);
        channelSliders[i].setValue(1.0, juce::dontSendNotification);
        channelSliders[i].setDoubleClickReturnValue(true, 1.0);
        channelSliders[i].setLookAndFeel(&faderLookAndFeel);
        int ch = i;
        channelSliders[i].onValueChange = [this, ch]
        {
            perChannelGain[ch] = (float)channelSliders[ch].getValue();
            float master = (float)volumeSlider.getValue();
            // NINJAM ch0 = Vorbis mixdown; individual channels start at ch1
            processor.setUserNjChannelVolume(userIndex, ch + 1, master * perChannelGain[ch]);
        };
        addChildComponent(channelSliders[i]);

        channelNameLabels[i].setFont(juce::Font(9.0f));
        channelNameLabels[i].setJustificationType(juce::Justification::centredLeft);
        channelNameLabels[i].setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        addChildComponent(channelNameLabels[i]);
    }

    startTimer(juce::PluginHostType().isAbletonLive() ? 120 : 50);
}

void UserChannelStrip::refreshOutputSelectorItems()
{
    int totalOutputs = processor.getTotalNumOutputChannels();
    if (totalOutputs <= 0)
        totalOutputs = 2;

    if (cachedTotalOutputs == totalOutputs)
        return;

    cachedTotalOutputs = totalOutputs;
    const int selectedId = outputSelector.getSelectedId();
    const int numPairs = totalOutputs / 2;
    const int stereoBaseId = 100;

    outputSelector.onChange = nullptr;
    outputSelector.clear(juce::dontSendNotification);

    for (int ch = 0; ch < totalOutputs; ++ch)
        outputSelector.addItem("Out " + juce::String(ch + 1), ch + 1);

    for (int pair = 0; pair < numPairs; ++pair)
    {
        const int left = pair * 2 + 1;
        const int right = left + 1;
        outputSelector.addItem("Out " + juce::String(left) + "/" + juce::String(right),
                               stereoBaseId + pair);
    }

    outputSelector.addItem("Link Out", kRemoteOutputLinkSelectorId);
    outputSelector.onChange = [this] { outputChanged(); };

    if (selectedId > 0 && outputSelector.indexOfItemId(selectedId) >= 0)
        outputSelector.setSelectedId(selectedId, juce::dontSendNotification);
}

UserChannelStrip::~UserChannelStrip()
{
    volumeSlider.setLookAndFeel(nullptr);
    panSlider.setLookAndFeel(nullptr);
    muteButton.setLookAndFeel(nullptr);
    soloButton.setLookAndFeel(nullptr);
    for (int i = 0; i < kMaxRemoteCh; ++i)
        channelSliders[i].setLookAndFeel(nullptr);
    stopTimer();
}

int UserChannelStrip::getUserIndex() const
{
    return userIndex;
}

juce::Slider& UserChannelStrip::getVolumeSlider()
{
    return volumeSlider;
}

juce::Slider& UserChannelStrip::getPanSlider()
{
    return panSlider;
}

juce::Button& UserChannelStrip::getMuteButton()
{
    return muteButton;
}

juce::Button& UserChannelStrip::getSoloButton()
{
    return soloButton;
}

juce::Slider& UserChannelStrip::getChannelSlider(int channel)
{
    return channelSliders[(size_t)juce::jlimit(0, kMaxRemoteCh - 1, channel)];
}

void UserChannelStrip::paintOverChildren(juce::Graphics& g)
{
    auto drawGlow = [&](juce::Button& btn, juce::Colour onColour, juce::Colour offColour)
    {
        bool isOn = btn.getToggleState();
        auto bc = btn.getBounds().toFloat();
        auto centre = bc.getCentre();
        float gap = 5.0f;
        float r = bc.getWidth() * 0.55f + gap;
        juce::Colour col = isOn ? onColour : offColour;
        juce::ColourGradient grad(col, centre.x, centre.y,
                                  juce::Colours::transparentBlack, centre.x + r, centre.y, true);
        g.setGradientFill(grad);
        g.fillEllipse(centre.x - r, centre.y - r, r * 2.0f, r * 2.0f);
    };

    drawGlow(muteButton, juce::Colour(0x55ff3030), juce::Colour(0x22200808));
    drawGlow(soloButton, juce::Colour(0x55ffd030), juce::Colour(0x22281e04));

    if (clipPulsing)
    {
        const double phase = juce::Time::getMillisecondCounterHiRes() * 0.001
                           * (juce::MathConstants<double>::twoPi * 0.85);
        const float pulse = 0.5f + 0.5f * (float)std::sin(phase);
        const float alpha = juce::jmap(pulse, 0.32f, 0.88f);
        auto r = getLocalBounds().toFloat().reduced(1.0f);

        g.setColour(juce::Colour(0xffff2424).withAlpha(alpha));
        g.drawRoundedRectangle(r, 5.0f, 2.0f + pulse * 1.4f);
        g.setColour(juce::Colour(0xffff2424).withAlpha(alpha * 0.35f));
        g.drawRoundedRectangle(r.expanded(2.0f), 7.0f, 1.0f);
    }
}

void UserChannelStrip::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.45f));
    g.setColour(juce::Colours::black.withAlpha(0.60f));
    g.drawRect(getLocalBounds(), 1);

    const int remotePeakCount = juce::jlimit(0, kMaxRemoteCh, numRemoteChannels);
    const bool multiChan = isMultiChanPeer && remotePeakCount > 1;
    const bool showMultiMeter = multiChan && !isExpanded;

    if (isHorizontalLayout)
    {
        auto sliderBounds = volumeSlider.getBounds();
        constexpr int meterWidth = 18;
        constexpr int meterGap = 4;
        juce::Rectangle<int> meterBounds(sliderBounds.getRight() + meterGap, sliderBounds.getY(),
                                         meterWidth, sliderBounds.getHeight());

        if (showMultiMeter)
            MasterPeakMeter::drawVerticalMultiMeter(g, meterBounds, channelPeaks, remotePeakCount);
        else
            MasterPeakMeter::drawVerticalStereoMeter(g, meterBounds, currentPeakL, currentPeakR);
    }
    else
    {
        auto sliderBounds = volumeSlider.getBounds();
        constexpr int meterHeight = 8;
        juce::Rectangle<int> meterBounds(sliderBounds.getX(), sliderBounds.getBottom(),
                                         sliderBounds.getWidth(), meterHeight);

        if (showMultiMeter)
            MasterPeakMeter::drawHorizontalMultiMeter(g, meterBounds, channelPeaks, remotePeakCount);
        else
            MasterPeakMeter::drawHorizontalStereoMeter(g, meterBounds, currentPeakL, currentPeakR);
    }

    if (isExpanded && multiChan)
    {
        for (int ch = 0; ch < remotePeakCount; ++ch)
        {
            if (!channelSliders[ch].isVisible())
                continue;

            auto sliderBounds = channelSliders[ch].getBounds();
            if (isHorizontalLayout)
            {
                constexpr int meterWidth = 8;
                juce::Rectangle<int> meterBounds(sliderBounds.getRight() + 1, sliderBounds.getY(),
                                                 meterWidth, sliderBounds.getHeight());
                MasterPeakMeter::drawVerticalMonoMeter(g, meterBounds, channelPeaks[ch]);
            }
            else
            {
                constexpr int meterHeight = 8;
                juce::Rectangle<int> meterBounds(sliderBounds.getX(), sliderBounds.getBottom() + 1,
                                                 sliderBounds.getWidth(), meterHeight);
                MasterPeakMeter::drawHorizontalMonoMeter(g, meterBounds, channelPeaks[ch]);
            }
        }
    }
}

void UserChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(2);
    const int remoteChannelCount = juce::jlimit(0, kMaxRemoteCh, numRemoteChannels);

    if (isHorizontalLayout)
    {
        // When multichan is expanded, restrict main strip to the left column
        if (isExpanded && isMultiChanPeer && remoteChannelCount > 1)
            area.setWidth(56); // 60px column minus 2px margin each side

        nameLabel.setBounds(area.removeFromTop(18));
        chordLabel.setBounds(area.removeFromTop(18).reduced(1, 1));
        outputSelector.setBounds(area.removeFromBottom(20));
        auto dbArea   = area.removeFromBottom(16);
        auto ctrlArea = area.removeFromBottom(20);
        muteButton.setBounds(ctrlArea.removeFromLeft(area.getWidth() / 2));
        soloButton.setBounds(ctrlArea);
        panSlider.setBounds(area.removeFromTop(20).reduced(4, 2));

        int sliderWidth  = juce::jmin(20, area.getWidth());
        constexpr int meterWidth = 18;
        constexpr int meterGap = 4;
        int sliderHeight = (int)(area.getHeight() * 0.85f);
        int sliderY      = area.getY() + (area.getHeight() - sliderHeight) / 2;
        int groupWidth   = sliderWidth + meterGap + meterWidth;
        int sliderX      = area.getX() + juce::jmax(0, (area.getWidth() - groupWidth) / 2);
        volumeSlider.setBounds(sliderX, sliderY, sliderWidth, sliderHeight);
        volumeSlider.setSliderStyle(juce::Slider::LinearVertical);
        dbLabel.setBounds(dbArea);
        dbLabel.setVisible(true);

        // Expand button at top-right of strip when multichannel peer
        if (isMultiChanPeer)
        {
            auto nameBounds = nameLabel.getBounds();
            expandButton.setBounds(nameBounds.getRight() - 14, nameBounds.getY(), 14, nameBounds.getHeight());
            expandButton.setVisible(true);
        }
        else
        {
            expandButton.setVisible(false);
        }

        // Per-channel faders as side columns to the right of main strip when expanded
        if (isExpanded && isMultiChanPeer && remoteChannelCount > 1)
        {
            auto subArea = getLocalBounds().reduced(2);
            subArea.removeFromLeft(60); // skip the narrower main column when expanded
            int colW = 36;
            for (int i = 0; i < remoteChannelCount; ++i)
            {
                auto col = subArea.removeFromLeft(colW);
                // Name label at top (18px), slider fills rest
                channelNameLabels[i].setBounds(col.removeFromTop(18).reduced(1, 0));
                channelNameLabels[i].setVisible(true);
                col.reduce(2, 4);
                auto sliderCol = col;
                sliderCol.removeFromRight(9);
                channelSliders[i].setBounds(sliderCol);
                channelSliders[i].setSliderStyle(juce::Slider::LinearVertical);
                channelSliders[i].setVisible(true);
            }
            for (int i = remoteChannelCount; i < kMaxRemoteCh; ++i)
            {
                channelSliders[i].setVisible(false);
                channelNameLabels[i].setVisible(false);
            }
        }
        else
        {
            for (int i = 0; i < kMaxRemoteCh; ++i)
            {
                channelSliders[i].setVisible(false);
                channelNameLabels[i].setVisible(false);
            }
        }
    }
    else
    {
        // List layout (strip is horizontal)
        // When multichan is expanded, restrict main strip to the top row
        if (isExpanded && isMultiChanPeer && remoteChannelCount > 1)
            area.setHeight(36); // 40px base minus 2px padding top/bottom
        // Reserve 18px at right for expand button if this is a multichan peer
        if (isMultiChanPeer)
        {
            expandButton.setBounds(area.removeFromRight(18));
            expandButton.setVisible(true);
        }
        else
        {
            expandButton.setVisible(false);
        }

        nameLabel.setBounds(area.removeFromLeft(80));
        chordLabel.setBounds(area.removeFromLeft(58).reduced(2, 6));
        outputSelector.setBounds(area.removeFromRight(60));
        auto ctrlArea = area.removeFromRight(40);
        muteButton.setBounds(ctrlArea.removeFromTop(ctrlArea.getHeight() / 2));
        soloButton.setBounds(ctrlArea);
        panSlider.setBounds(area.removeFromRight(40));

        // Leave a fixed room for the meter rows at the bottom of the main strip
        int meterH = 8;
        area.removeFromBottom(meterH);
        int sliderHeight = juce::jmin(18, area.getHeight());
        volumeSlider.setBounds(area.getX(), area.getCentreY() - sliderHeight / 2, area.getWidth(), sliderHeight);
        volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        dbLabel.setVisible(false);

        // Per-channel rows below the main strip when expanded
        if (isExpanded && isMultiChanPeer && remoteChannelCount > 1)
        {
            auto expandArea = getLocalBounds().reduced(2);
            expandArea.removeFromTop(40); // skip the main strip row
            int rowH = 36;
            for (int i = 0; i < remoteChannelCount; ++i)
            {
                auto row = expandArea.removeFromTop(rowH);
                row.removeFromLeft(6);
                // Left 80px: channel name; rest: slider
                channelNameLabels[i].setBounds(row.removeFromLeft(80));
                channelNameLabels[i].setVisible(true);
                auto sliderRow = row.reduced(0, 2);
                sliderRow.removeFromBottom(9);
                channelSliders[i].setBounds(sliderRow);
                channelSliders[i].setSliderStyle(juce::Slider::LinearHorizontal);
                channelSliders[i].setVisible(true);
            }
            for (int i = remoteChannelCount; i < kMaxRemoteCh; ++i)
            {
                channelSliders[i].setVisible(false);
                channelNameLabels[i].setVisible(false);
            }
        }
        else
        {
            for (int i = 0; i < kMaxRemoteCh; ++i)
            {
                channelSliders[i].setVisible(false);
                channelNameLabels[i].setVisible(false);
            }
        }
    }
}

int UserChannelStrip::getPreferredHeight() const
{
    int base = 40;
    const int remoteChannelCount = juce::jlimit(0, kMaxRemoteCh, numRemoteChannels);
    if (!isHorizontalLayout && isExpanded && isMultiChanPeer && remoteChannelCount > 1)
        return base + remoteChannelCount * 36;
    return base;
}

int UserChannelStrip::getPreferredWidth() const
{
    const int remoteChannelCount = juce::jlimit(0, kMaxRemoteCh, numRemoteChannels);
    if (isHorizontalLayout && isExpanded && isMultiChanPeer && remoteChannelCount > 1)
        return 60 + remoteChannelCount * 36; // narrower main + wider sub-channels
    return 80;
}

void UserChannelStrip::setOrientation(bool isHorizontal)
{
    isHorizontalLayout = isHorizontal;

    if (isHorizontalLayout)
    {
        panSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        panSlider.setLookAndFeel(&panLookAndFeel);
    }
    else
    {
        panSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        panSlider.setLookAndFeel(nullptr);
    }

    resized();
    repaint();
}

void UserChannelStrip::updateInfo(const NinjamVst3AudioProcessor::UserInfo& info)
{
    refreshOutputSelectorItems();

    const bool userChanged = userInfo.name.isNotEmpty() && userInfo.name != info.name;
    if (userChanged)
    {
        currentPeakL = 0.0f;
        currentPeakR = 0.0f;
        clipStartMs = 0.0;
        clipPulsing = false;
        chordToggleArmed = false;
        isExpanded = false;
        expandButton.setButtonText(isHorizontalLayout ? ">" : "v");
        dbLabel.setText("", juce::dontSendNotification);

        for (int i = 0; i < kMaxRemoteCh; ++i)
        {
            perChannelGain[i] = 1.0f;
            channelPeaks[i] = 0.0f;
            channelSliders[i].setValue(1.0, juce::dontSendNotification);
        }
    }

    userIndex = info.index;
    userInfo  = info;
    if (nameLabel.getText() != info.name)
        nameLabel.setText(info.name, juce::dontSendNotification);

    if (!volumeSlider.isMouseOverOrDragging())
    {
        const double targetVolume = juce::jmin(info.volume, 2.0f);
        if (std::abs(volumeSlider.getValue() - targetVolume) > 0.001)
            volumeSlider.setValue(targetVolume, juce::dontSendNotification);
    }

    if (!panSlider.isMouseOverOrDragging())
    {
        const double targetPan = info.pan;
        if (std::abs(panSlider.getValue() - targetPan) > 0.001)
            panSlider.setValue(targetPan, juce::dontSendNotification);
    }

    if (muteButton.getToggleState() != info.isMuted)
        muteButton.setToggleState(info.isMuted, juce::dontSendNotification);
    if (soloButton.getToggleState() != info.isSolo)
        soloButton.setToggleState(info.isSolo, juce::dontSendNotification);
    const bool clipEnabled = processor.isUserClipEnabled(info.index);
    if (clipButton.getToggleState() != clipEnabled)
        clipButton.setToggleState(clipEnabled, juce::dontSendNotification);

    // Sync multichan state — trigger layout refresh if anything changed
    const int newNCh = juce::jlimit(1, kMaxRemoteCh, info.numChannels);
    const bool multiStateChanged = (info.isMultiChanPeer != isMultiChanPeer) || (newNCh != numRemoteChannels);
    isMultiChanPeer   = info.isMultiChanPeer;
    numRemoteChannels = newNCh;

    // Update channel name labels
    for (int i = 0; i < kMaxRemoteCh; ++i)
    {
        juce::String name = i < info.channelNames.size() ? info.channelNames[i] : "";
        if (channelNameLabels[i].getText() != name)
            channelNameLabels[i].setText(name, juce::dontSendNotification);
    }
    if (multiStateChanged)
    {
        // Set button text to match current state
        if (isMultiChanPeer)
            expandButton.setButtonText(isHorizontalLayout ? ">" : "v");
        // If we lost multichan, collapse
        if (!isMultiChanPeer && isExpanded)
        {
            isExpanded = false;
            expandButton.setButtonText(isHorizontalLayout ? ">" : "v");
        }
        resized();
        repaint();
        // Walk up to UserListComponent so it recalculates strip heights
        for (auto* p = getParentComponent(); p != nullptr; p = p->getParentComponent())
        {
            if (auto* list = dynamic_cast<UserListComponent*>(p))
            {
                list->resized();
                break;
            }
        }
    }

    int totalOutputs = processor.getTotalNumOutputChannels();
    if (totalOutputs <= 0) totalOutputs = 2;
    int numPairs    = totalOutputs / 2;
    int stereoBaseId = 100;

    int id = 0;
    if (info.outputUsesLinkAudio)
    {
        id = kRemoteOutputLinkSelectorId;
    }
    else
    {
        int ch = info.outputChannel;
        bool isMono = (ch & 1024) != 0;
        int chanIdx  = ch & 1023;
        if (isMono)
        {
            if (chanIdx >= 0 && chanIdx < totalOutputs)
                id = chanIdx + 1;
        }
        else
        {
            int pair = chanIdx / 2;
            if (pair >= 0 && pair < numPairs)
                id = stereoBaseId + pair;
        }
    }

    if (id > 0 && outputSelector.getSelectedId() != id)
        outputSelector.setSelectedId(id, juce::dontSendNotification);
}

void UserChannelStrip::setClipEnabled(bool enabled)
{
    clipButton.setToggleState(enabled, juce::dontSendNotification);
    processor.setUserClipEnabled(userIndex, enabled);
}

void UserChannelStrip::timerCallback()
{
    if (!isShowing())
        return;

    refreshOutputSelectorItems();

    for (auto* c = getParentComponent(); c != nullptr; c = c->getParentComponent())
        if (auto* editor = dynamic_cast<NinjamVst3AudioProcessorEditor*>(c))
            if (editor->shouldDeferHeavyUiWork())
                return;

    auto peakL = processor.getUserPeak(userIndex, 0);
    auto peakR = processor.getUserPeak(userIndex, 1);
    const float pan = juce::jlimit(-1.0f, 1.0f, (float) panSlider.getValue());
    const float leftPanGain = (pan > 0.0f) ? (1.0f - pan) : 1.0f;
    const float rightPanGain = (pan < 0.0f) ? (1.0f + pan) : 1.0f;

    peakL = juce::jmax(0.0f, peakL * leftPanGain);
    peakR = juce::jmax(0.0f, peakR * rightPanGain);

    float displayedPeak = juce::jmax(peakL, peakR);
    bool needRepaint = false;

    if (std::abs(peakL - currentPeakL) > 0.001f || std::abs(peakR - currentPeakR) > 0.001f)
    {
        currentPeakL = peakL;
        currentPeakR = peakR;
        float peak = juce::jmax(currentPeakL, currentPeakR);
        float db = -60.0f;
        if (peak > 1.0e-6f)
            db = juce::jlimit(-60.0f, 6.0f, 20.0f * std::log10(peak));
        const auto dbText = juce::String(db, 1) + " dB";
        if (dbLabel.getText() != dbText)
            dbLabel.setText(dbText, juce::dontSendNotification);
        needRepaint = true;
    }

    // Update per-NINJAM-channel peaks for multichan peers (used by collapsed multi-meter + expanded rows)
    const int remoteChannelCount = juce::jlimit(0, kMaxRemoteCh, numRemoteChannels);
    if (isMultiChanPeer && remoteChannelCount > 1)
    {
        for (int ch = 0; ch < remoteChannelCount; ++ch)
        {
            // NINJAM ch0 = Vorbis mixdown; individual channels start at ch1
            float chPeak = processor.getUserChannelPeak(userIndex, ch + 1, -1); // -1 = both/max
            displayedPeak = juce::jmax(displayedPeak, chPeak);
            if (std::abs(chPeak - channelPeaks[ch]) > 0.001f)
            {
                channelPeaks[ch] = chPeak;
                needRepaint = true;
            }
        }
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const bool clipping = displayedPeak >= 1.0f;
    const bool wasPulsing = clipPulsing;
    if (clipping)
    {
        if (clipStartMs <= 0.0)
            clipStartMs = nowMs;
        clipPulsing = (nowMs - clipStartMs) >= 500.0;
    }
    else
    {
        clipStartMs = 0.0;
        clipPulsing = false;
    }
    if (clipPulsing || wasPulsing != clipPulsing)
        needRepaint = true;

    const auto remoteChord = processor.getUserChordLabel(userIndex);
    const auto remoteChordStats = "CPU " + juce::String(processor.getUserChordCpuPercent(userIndex), 2)
                                + "%  MEM ~" + juce::String(processor.getUserChordMemoryKb(userIndex)) + " KB";
    if (chordLabel.getText() != remoteChord)
    {
        chordLabel.setText(remoteChord, juce::dontSendNotification);
        needRepaint = true;
    }

    juce::String chordTooltip;
    if (!processor.isChordDetectionEnabled())
        chordTooltip = "Chord detection is off globally. Enable it in Options.";
    else if (!processor.isUserChordDetectionEnabled(userIndex))
        chordTooltip = "Chord detection is off for this user. Click to enable.";
    else
        chordTooltip = "Remote chord: " + remoteChord + "\n" + remoteChordStats + "\nClick to turn it off for this user.";

    if (chordLabel.getTooltip() != chordTooltip)
        chordLabel.setTooltip(chordTooltip);

    if (needRepaint)
        repaint();
}

void UserChannelStrip::mouseDown(const juce::MouseEvent& event)
{
    chordToggleArmed = event.mods.isLeftButtonDown()
        && chordLabel.isVisible()
        && chordLabel.getBounds().contains(event.getPosition());

    juce::Component::mouseDown(event);
}

void UserChannelStrip::mouseUp(const juce::MouseEvent& event)
{
    const bool clickedChordLabel = chordToggleArmed
        && chordLabel.isVisible()
        && chordLabel.getBounds().contains(event.getPosition());
    chordToggleArmed = false;

    juce::Component::mouseUp(event);

    if (!clickedChordLabel)
        return;

    const bool shouldEnable = !processor.isUserChordDetectionEnabled(userIndex);
    processor.setUserChordDetectionEnabled(userIndex, shouldEnable);
    chordLabel.setText(processor.getUserChordLabel(userIndex), juce::dontSendNotification);

    if (!processor.isChordDetectionEnabled())
        chordLabel.setTooltip("Chord detection is off globally. Enable it in Options.");
    else if (!shouldEnable)
        chordLabel.setTooltip("Chord detection is off for this user. Click to enable.");
    else
        chordLabel.setTooltip("Remote chord detection enabled.");

    repaint();
}

void UserChannelStrip::volumeChanged()
{
    applyVolumesToProcessor();
}

void UserChannelStrip::panChanged()
{
    applyVolumesToProcessor();
}

void UserChannelStrip::outputChanged()
{
    int selectedId = outputSelector.getSelectedId();
    if (selectedId <= 0)
        return;

    int totalOutputs = processor.getTotalNumOutputChannels();
    if (totalOutputs <= 0) totalOutputs = 2;
    int numPairs    = totalOutputs / 2;
    int stereoBaseId = 100;

    if (selectedId == kRemoteOutputLinkSelectorId)
    {
        processor.setUserOutputToLinkAudio(userIndex);
    }
    else if (selectedId >= 1 && selectedId <= totalOutputs)
        // Single (mono) channel: set the 1024 mono bit so njclient outputs to one channel only
        processor.setUserOutput(userIndex, (selectedId - 1) | 1024);
    else if (selectedId >= stereoBaseId && selectedId < stereoBaseId + numPairs)
        // Stereo pair: no mono bit, base channel is pair * 2
        processor.setUserOutput(userIndex, (selectedId - stereoBaseId) * 2);
}

void UserChannelStrip::muteChanged()
{
    applyVolumesToProcessor();
}

void UserChannelStrip::soloChanged()
{
    applyVolumesToProcessor();
}

void UserChannelStrip::clipChanged()
{
    processor.setUserClipEnabled(userIndex, clipButton.getToggleState());
}

void UserChannelStrip::toggleExpanded()
{
    isExpanded = !isExpanded;
    if (isHorizontalLayout)
        expandButton.setButtonText(isExpanded ? "<" : ">");
    else
        expandButton.setButtonText(isExpanded ? "^" : "v");
    resized();
    // Walk up to UserListComponent to trigger full height recalculation
    for (auto* p = getParentComponent(); p != nullptr; p = p->getParentComponent())
    {
        if (auto* list = dynamic_cast<UserListComponent*>(p))
        {
            list->resized();
            break;
        }
    }
}

void UserChannelStrip::applyVolumesToProcessor()
{
    float mv   = (float)volumeSlider.getValue();
    float pan  = (float)panSlider.getValue();
    bool mute  = muteButton.getToggleState();
    bool solo  = soloButton.getToggleState();
    processor.setUserLevel(userIndex, mv, pan, mute, solo);
    // Re-apply per-channel gain overrides for multichan peers
    const int remoteChannelCount = juce::jlimit(0, kMaxRemoteCh, numRemoteChannels);
    if (isMultiChanPeer && remoteChannelCount > 1)
    {
        for (int ch = 0; ch < remoteChannelCount; ++ch)
            // NINJAM ch0 = Vorbis mixdown; individual channels start at ch1
            processor.setUserNjChannelVolume(userIndex, ch + 1, mv * perChannelGain[ch]);
    }
}

// ==============================================================================
// UserListComponent Implementation
// ==============================================================================

UserListComponent::UserListComponent(NinjamVst3AudioProcessor& p)
    : processor(p)
{
    setOpaque(false);
    addAndMakeVisible(viewport);
    viewport.setViewedComponent(&contentComponent, false);
    viewport.setScrollBarsShown(true, true);
    contentComponent.setOpaque(false);
}

UserListComponent::~UserListComponent()
{
    strips.clear();
}

void UserListComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.30f));
}

void UserListComponent::resized()
{
    viewport.setBounds(getLocalBounds());

    int stripWidth  = isHorizontal ? 80 : viewport.getWidth() - 15;
    int defHeight   = isHorizontal ? viewport.getHeight() - 20 : 40;
    if (stripWidth < 10) stripWidth = 10;
    if (defHeight  < 10) defHeight  = 10;

    int x = 0, y = 0;
    for (auto& strip : strips)
    {
        int sw = isHorizontal ? strip->getPreferredWidth() : stripWidth;
        int sh = isHorizontal ? defHeight : strip->getPreferredHeight();
        strip->setBounds(x, y, sw, sh);
        if (isHorizontal) x += sw;
        else              y += sh;
    }

    if (isHorizontal)
        contentComponent.setBounds(0, 0, x, viewport.getHeight() - 20);
    else
        contentComponent.setBounds(0, 0, viewport.getWidth() - 15, juce::jmax(y, viewport.getHeight() - 20));
}

void UserListComponent::updateContent()
{
    auto users = processor.getConnectedUsers();

    if (users.size() != strips.size())
    {
        strips.clear();
        contentComponent.removeAllChildren();

        for (const auto& u : users)
        {
            auto strip = std::make_unique<UserChannelStrip>(processor, u.index);
            strip->setOrientation(isHorizontal);
            strip->updateInfo(u);
            strip->setClipEnabled(processor.isUserClipEnabled(u.index));
            contentComponent.addAndMakeVisible(strip.get());
            strips.push_back(std::move(strip));
        }
        resized();
    }
    else
    {
        for (size_t i = 0; i < users.size(); ++i)
            strips[i]->updateInfo(users[i]);
    }
}

void UserListComponent::setLayoutMode(bool horizontal)
{
    isHorizontal = horizontal;
    for (auto& strip : strips)
        strip->setOrientation(horizontal);
    resized();
}

void UserListComponent::setAllClipEnabled(bool enabled)
{
    for (auto& strip : strips)
        strip->setClipEnabled(enabled);
}

std::vector<UserChannelStrip*> UserListComponent::getStripPointers() const
{
    std::vector<UserChannelStrip*> pointers;
    pointers.reserve(strips.size());
    for (const auto& strip : strips)
        pointers.push_back(strip.get());
    return pointers;
}
#if 0
void FaderLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos, const juce::Slider::SliderStyle style, juce::Slider& slider) { if (auto* editor = dynamic_cast<NinjamVst3AudioProcessorEditor*>(slider.getParentComponent())) { if (editor->faderKnobImage.isValid()) { auto isVertical = style == juce::Slider::LinearVertical; auto thumbWidth = isVertical ? width * 0.8f : 30.0f; auto thumbHeight = isVertical ? 30.0f : height * 0.8f; auto thumbX = isVertical ? (float)x + (float)width * 0.1f : sliderPos - thumbWidth * 0.5f; auto thumbY = isVertical ? sliderPos - thumbHeight * 0.5f : (float)y + (float)height * 0.1f; g.drawImageWithin(editor->faderKnobImage, (int)thumbX, (int)thumbY, (int)thumbWidth, (int)thumbHeight, juce::RectanglePlacement::centred); return; } } auto thumbWidth = (style == juce::Slider::LinearVertical) ? width * 0.8f : 12.0f; auto thumbHeight = (style == juce::Slider::LinearVertical) ? 12.0f : height * 0.8f; auto thumbX = (style == juce::Slider::LinearVertical) ? (float)x + (float)width * 0.1f : sliderPos - thumbWidth * 0.5f; auto thumbY = (style == juce::Slider::LinearVertical) ? sliderPos - thumbHeight * 0.5f : (float)y + (float)height * 0.1f; g.setColour(juce::Colour(0xff666666)); g.fillRoundedRectangle(thumbX, thumbY, thumbWidth, thumbHeight, 2.0f); }
void CustomKnobLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos, const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) { auto centreX = (float)x + (float)width * 0.5f; auto centreY = (float)y + (float)height * 0.5f; auto* editor = dynamic_cast<NinjamVst3AudioProcessorEditor*>(slider.getParentComponent()); if (editor == nullptr) { auto* p = slider.getParentComponent(); while (p != nullptr && editor == nullptr) { editor = dynamic_cast<NinjamVst3AudioProcessorEditor*>(p); p = p->getParentComponent(); } } if (editor != nullptr && editor->radioKnobImage.isValid()) { const float radius = (float)juce::jmin(width / 2, height / 2) - 4.0f; auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle); g.drawImageWithin(editor->radioKnobImage, (int)(centreX - radius), (int)(centreY - radius), (int)(radius * 2.0f), (int)(radius * 2.0f), juce::RectanglePlacement::fillDestination); return; } auto radius = (float)juce::jmin(width / 2, height / 2) - 10.0f; g.setColour(juce::Colour(0xffdddddd)); g.fillEllipse(centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f); }
NinjamVst3AudioProcessorEditor::NinjamVst3AudioProcessorEditor (NinjamVst3AudioProcessor& p) : AudioProcessorEditor (&p), audioProcessor (p), intervalDisplay(p), userList(p) { setSize (1120, 620); setResizable(true, true); addAndMakeVisible(serverField); addAndMakeVisible(serverListButton); addAndMakeVisible(backgroundSelector); backgroundSelector.onChange = [this] { auto files = juce::File(\ C:\\\Users\\\mcand\\\Pictures\\\textures\).findChildFiles(juce::File::findFiles, false, \*.jpg\); int idx = backgroundSelector.getSelectedItemIndex(); if (idx >= 0 ; idx < files.size()) { backgroundImage = juce::ImageFileFormat::loadFrom(files[idx]); loadControlImages(files[idx]); repaint(); } }; loadControlImages(juce::File(\C:\\\Users\\\mcand\\\Pictures\\\textures\\\Brushed Metal 1.jpg\)); } NinjamVst3AudioProcessorEditor::~NinjamVst3AudioProcessorEditor() {} void NinjamVst3AudioProcessorEditor::paint (juce::Graphics& g) { if (backgroundImage.isValid()) g.drawImageWithin(backgroundImage, 0, 0, getWidth(), getHeight(), juce::RectanglePlacement::fillDestination); else g.fillAll(juce::Colour(0xff222222)); } void NinjamVst3AudioProcessorEditor::resized() { auto header = getLocalBounds().removeFromTop(40); backgroundSelector.setBounds(header.removeFromRight(150).reduced(2)); } void NinjamVst3AudioProcessorEditor::loadControlImages(const juce::File& f) { auto dir = f.getParentDirectory(); auto base = f.getFileNameWithoutExtension(); radioKnobImage = juce::ImageFileFormat::loadFrom(dir.getChildFile(base + \_radioknob.png\)); faderKnobImage = juce::ImageFileFormat::loadFrom(dir.getChildFile(base + \_faderknob.png\)); repaint(); }
#endif
