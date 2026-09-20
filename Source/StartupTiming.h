#pragma once

#include <juce_core/juce_core.h>

// Opt-in local timing log; no settings, device names, or user data are recorded.
inline void logNinjamStartupTiming(const char* stage, double durationMs = 0.0)
{
    static const auto path = juce::SystemStats::getEnvironmentVariable("NINJAMPLUS_STARTUP_LOG", {});
    if (!juce::File::isAbsolutePath(path))
        return;
    static juce::CriticalSection lock;
    const juce::ScopedLock scope(lock);
    juce::File(path).appendText(juce::String(juce::Time::currentTimeMillis()) + " "
        + stage + " " + juce::String(durationMs, 2) + " ms\n");
}

class NinjamStartupTiming
{
public:
    explicit NinjamStartupTiming(const char* stageToUse)
        : stage(stageToUse), start(juce::Time::getMillisecondCounterHiRes()) {}
    ~NinjamStartupTiming() { logNinjamStartupTiming(stage, juce::Time::getMillisecondCounterHiRes() - start); }
private:
    const char* stage;
    double start;
};
