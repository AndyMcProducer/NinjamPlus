#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <juce_core/juce_core.h>
#include <atomic>

/**
 * Simple NTP client that queries multiple Google NTP servers and averages
 * the clock offsets. The offset is added to the local millisecond clock to
 * get an NTP-aligned absolute time.
 *
 * Usage:
 *   NtpClient ntp;
 *   ntp.sync();  // blocking, queries servers
 *   double offsetMs = ntp.getOffsetMs();  // local + offset = NTP time
 *   double ntpTimeMs = juce::Time::currentTimeMillis() + offsetMs;
 */
class NtpClient
{
public:
    NtpClient();
    ~NtpClient();

    /** Query all configured NTP servers and compute the average offset.
     *  Blocking - call from a background thread.
     *  Returns true if at least one server responded. */
    bool sync();

    /** Returns the offset in milliseconds: NTP time = localTime + offsetMs.
     *  Returns 0.0 if sync has not been performed or failed. */
    double getOffsetMs() const;

    /** Returns true if the last sync succeeded. */
    bool isSynced() const;

    /** Returns the number of servers that responded in the last sync. */
    int getRespondedServerCount() const;

private:
    struct NtpTimestamp
    {
        uint32_t seconds = 0;
        uint32_t fraction = 0;
    };

    struct NtpResponse
    {
        bool valid = false;
        double offsetMs = 0.0;
        double roundTripMs = 0.0;
    };

    NtpResponse queryServer(const juce::String& hostname);

    static double timestampToMs(const NtpTimestamp& ts);
    static NtpTimestamp readTimestamp(const uint8_t* data, int offset);

    std::atomic<double> offsetMs { 0.0 };
    std::atomic<bool> synced { false };
    std::atomic<int> respondedServerCount { 0 };

    static constexpr int ntpPort = 123;
    static constexpr int ntpPacketSize = 48;
    static constexpr int timeoutMs = 3000;

    const juce::StringArray servers {
        "time1.google.com",
        "time2.google.com",
        "time3.google.com",
        "time4.google.com"
    };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NtpClient)
};
