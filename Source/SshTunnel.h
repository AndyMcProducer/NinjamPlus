#pragma once

#include <juce_core/juce_core.h>

namespace ninjamplus
{

/**
 * Manages an SSH local port-forward tunnel by spawning the system ssh client.
 *
 * Before connecting to a NINJAM server, start() opens an SSH tunnel:
 *   ssh -L <localPort>:<ninjamHost>:<ninjamPort> -N -p <sshPort> <sshUser>@<sshHost>
 *
 * The NINJAM client then connects to localhost:<localPort> instead of the
 * remote server directly. All traffic (audio, chat, VDO sync, side signals)
 * flows through the encrypted SSH tunnel.
 *
 * Works on Windows 10+ (built-in ssh.exe), macOS, and Linux.
 */
class SshTunnel
{
public:
    struct Config
    {
        bool enabled = false;
        juce::String sshHost;       // e.g. "myserver.com"
        int sshPort = 22;           // SSH server port
        juce::String sshUser;       // SSH login user
        juce::String keyFile;       // optional path to private key
        juce::String remoteHost;    // NINJAM server host to forward to
        int remotePort = 2049;      // NINJAM server port
        int localPort = 0;          // 0 = auto-pick an ephemeral port
    };

    SshTunnel() = default;
    ~SshTunnel() { stop(); }

    /** Start the SSH tunnel. Returns the local port to connect to, or 0 on failure. */
    int start(const Config& config);

    /** Stop the SSH tunnel process. */
    void stop();

    /** Returns true if the tunnel process is currently running. */
    bool isActive() const;

    /** Returns the local port the tunnel is listening on (0 if not active). */
    int getLocalPort() const { return localPort.load(); }

    /** Returns the last error message, if any. */
    juce::String getLastError() const { return lastError; }

    /** Check if ssh is available on this system. */
    static bool isSshAvailable();

private:
    std::atomic<int> localPort { 0 };
    juce::String lastError;
    juce::ChildProcess sshProcess;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SshTunnel)
};

} // namespace ninjamplus
