#include "NtpClient.h"

NtpClient::NtpClient() {}
NtpClient::~NtpClient() {}

bool NtpClient::sync()
{
    int responded = 0;
    double offsetSum = 0.0;

    for (const auto& server : servers)
    {
        const NtpResponse response = queryServer(server);
        if (response.valid)
        {
            offsetSum += response.offsetMs;
            ++responded;
            juce::Logger::writeToLog("NTP: " + server + " offset=" + juce::String(response.offsetMs, 1)
                                     + "ms roundTrip=" + juce::String(response.roundTripMs, 1) + "ms");
        }
        else
        {
            juce::Logger::writeToLog("NTP: " + server + " failed");
        }
    }

    if (responded > 0)
    {
        const double avgOffset = offsetSum / (double)responded;
        offsetMs.store(avgOffset, std::memory_order_release);
        synced.store(true, std::memory_order_release);
        respondedServerCount.store(responded, std::memory_order_release);
        juce::Logger::writeToLog("NTP: synced with " + juce::String(responded)
                                 + " servers, avg offset=" + juce::String(avgOffset, 1) + "ms");
        return true;
    }

    synced.store(false, std::memory_order_release);
    respondedServerCount.store(0, std::memory_order_release);
    juce::Logger::writeToLog("NTP: sync failed - no servers responded");
    return false;
}

double NtpClient::getOffsetMs() const
{
    return offsetMs.load(std::memory_order_acquire);
}

bool NtpClient::isSynced() const
{
    return synced.load(std::memory_order_acquire);
}

int NtpClient::getRespondedServerCount() const
{
    return respondedServerCount.load(std::memory_order_acquire);
}

NtpClient::NtpResponse NtpClient::queryServer(const juce::String& hostname)
{
    NtpResponse result;

#ifdef _WIN32
    WSADATA wsaData;
    const bool wsaInit = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    if (!wsaInit)
        return result;
#endif

    // DNS resolution using getaddrinfo
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;       // IPv4
    hints.ai_socktype = SOCK_DGRAM;  // UDP

    struct addrinfo* addrResult = nullptr;
    if (getaddrinfo(hostname.toRawUTF8(), "123", &hints, &addrResult) != 0 || addrResult == nullptr)
    {
#ifdef _WIN32
        if (wsaInit) WSACleanup();
#endif
        return result;
    }

    // Create UDP socket
#ifdef _WIN32
    SOCKET sock = socket(addrResult->ai_family, addrResult->ai_socktype, addrResult->ai_protocol);
    if (sock == INVALID_SOCKET)
    {
        freeaddrinfo(addrResult);
        if (wsaInit) WSACleanup();
        return result;
    }
    // Set receive timeout
    DWORD timeoutVal = (DWORD)timeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutVal, sizeof(timeoutVal));
#else
    int sock = socket(addrResult->ai_family, addrResult->ai_socktype, addrResult->ai_protocol);
    if (sock < 0)
    {
        freeaddrinfo(addrResult);
        return result;
    }
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Build NTP packet (48 bytes, version 4, client mode)
    uint8_t packet[ntpPacketSize] = {};
    packet[0] = 0x1B; // LI=0, VN=3, Mode=3 (client)

    // Record send time (T1) in milliseconds since Unix epoch
    const double t1SendMs = (double)juce::Time::currentTimeMillis();

    // Send packet
#ifdef _WIN32
    const int sendResult = sendto(sock, reinterpret_cast<const char*>(packet), ntpPacketSize, 0,
                                  addrResult->ai_addr, (int)addrResult->ai_addrlen);
#else
    const int sendResult = sendto(sock, reinterpret_cast<const char*>(packet), ntpPacketSize, 0,
                                  addrResult->ai_addr, addrResult->ai_addrlen);
#endif
    if (sendResult < 0)
    {
#ifdef _WIN32
        closesocket(sock);
        freeaddrinfo(addrResult);
        if (wsaInit) WSACleanup();
#else
        close(sock);
        freeaddrinfo(addrResult);
#endif
        return result;
    }

    // Receive response
    uint8_t response[ntpPacketSize] = {};
    const int bytesRead = recvfrom(sock, reinterpret_cast<char*>(response), ntpPacketSize, 0, nullptr, nullptr);

    // Record receive time (T4) in milliseconds since Unix epoch
    const double t4RecvMs = (double)juce::Time::currentTimeMillis();

    // Clean up socket
#ifdef _WIN32
    closesocket(sock);
    freeaddrinfo(addrResult);
    if (wsaInit) WSACleanup();
#else
    close(sock);
    freeaddrinfo(addrResult);
#endif

    if (bytesRead < ntpPacketSize)
        return result;

    // Parse NTP timestamps from response
    // T2 = server receive time (receive timestamp) - offset 32
    // T3 = server send time (transmit timestamp) - offset 40
    const NtpTimestamp t2 = readTimestamp(response, 32);
    const NtpTimestamp t3 = readTimestamp(response, 40);

    // Convert NTP timestamps to milliseconds since Unix epoch
    // NTP epoch is 1900-01-01, Unix epoch is 1970-01-01
    // Difference: 2208988800 seconds
    constexpr double ntpEpochToUnixMs = 2208988800.0 * 1000.0;
    const double t2UnixMs = timestampToMs(t2) - ntpEpochToUnixMs;
    const double t3UnixMs = timestampToMs(t3) - ntpEpochToUnixMs;

    // Calculate clock offset: offset = ((T2 - T1) + (T3 - T4)) / 2
    // This gives us how much our clock differs from the server's clock
    const double offsetMsVal = ((t2UnixMs - t1SendMs) + (t3UnixMs - t4RecvMs)) / 2.0;
    const double roundTripMsVal = (t4RecvMs - t1SendMs) - (t3UnixMs - t2UnixMs);

    result.valid = true;
    result.offsetMs = offsetMsVal;
    result.roundTripMs = roundTripMsVal;

    return result;
}

double NtpClient::timestampToMs(const NtpTimestamp& ts)
{
    // Convert NTP timestamp (seconds since 1900) to milliseconds
    const double seconds = (double)ts.seconds;
    const double fraction = (double)ts.fraction / 4294967296.0; // 2^32
    return (seconds + fraction) * 1000.0;
}

NtpClient::NtpTimestamp NtpClient::readTimestamp(const uint8_t* data, int offset)
{
    NtpTimestamp ts;
    ts.seconds = ((uint32_t)data[offset] << 24)
               | ((uint32_t)data[offset + 1] << 16)
               | ((uint32_t)data[offset + 2] << 8)
               | ((uint32_t)data[offset + 3]);
    ts.fraction = ((uint32_t)data[offset + 4] << 24)
                | ((uint32_t)data[offset + 5] << 16)
                | ((uint32_t)data[offset + 6] << 8)
                | ((uint32_t)data[offset + 7]);
    return ts;
}
