#include "SshTunnel.h"
#include <juce_core/juce_core.h>

namespace ninjamplus
{

bool SshTunnel::isSshAvailable()
{
    // Check if ssh is in the system PATH
    juce::ChildProcess test;
    juce::StringArray args;
    args.add("ssh");
    args.add("-V");
    if (!test.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
        return false;

    // Wait briefly for it to produce output
    juce::Thread::sleep(200);
    auto output = test.readAllProcessOutput();
    test.waitForProcessToFinish(1000);
    return output.isNotEmpty();
}

int SshTunnel::start(const Config& config)
{
    stop();
    lastError.clear();

    if (config.sshHost.trim().isEmpty())
    {
        lastError = "No SSH host specified.";
        return 0;
    }

    if (config.sshUser.trim().isEmpty())
    {
        lastError = "No SSH user specified.";
        return 0;
    }

    if (config.remoteHost.trim().isEmpty())
    {
        lastError = "No remote (NINJAM) host specified.";
        return 0;
    }

    // Pick a local port
    int chosenLocalPort = config.localPort;
    if (chosenLocalPort <= 0 || chosenLocalPort > 65535)
    {
        // Try to find a free ephemeral port by binding a temporary socket
        juce::StreamingSocket probe;
        if (probe.bindToPort(0, "127.0.0.1"))
        {
            chosenLocalPort = probe.getBoundPort();
            probe.close();
        }
        else
        {
            // Fallback: try a range of ports
            for (int p = 49152; p <= 65535; ++p)
            {
                juce::StreamingSocket testSock;
                if (testSock.bindToPort(p, "127.0.0.1"))
                {
                    chosenLocalPort = p;
                    testSock.close();
                    break;
                }
            }
        }

        if (chosenLocalPort <= 0)
        {
            lastError = "Could not find a free local port for SSH tunnel.";
            return 0;
        }
    }

    // Build the ssh command
    juce::StringArray args;
    args.add("ssh");

    // Port forward: -L localPort:remoteHost:remotePort
    args.add("-L");
    args.add(juce::String(chosenLocalPort) + ":" + config.remoteHost + ":" + juce::String(config.remotePort));

    // No remote command, just forward
    args.add("-N");

    // Don't allocate a TTY
    args.add("-T");

    // SSH port
    if (config.sshPort > 0 && config.sshPort != 22)
    {
        args.add("-p");
        args.add(juce::String(config.sshPort));
    }

    // Key file
    if (config.keyFile.trim().isNotEmpty())
    {
        args.add("-i");
        args.add(config.keyFile.trim());
    }

    // Disable strict host key checking for ease of use (user can override in their ssh config)
    args.add("-o");
    args.add("StrictHostKeyChecking=accept-new");

    // BatchMode=yes prevents ssh from hanging on password prompts (no terminal available)
    args.add("-o");
    args.add("BatchMode=yes");

    // Connect timeout
    args.add("-o");
    args.add("ConnectTimeout=10");

    // ServerAliveInterval to keep the tunnel alive
    args.add("-o");
    args.add("ServerAliveInterval=30");

    args.add("-o");
    args.add("ServerAliveCountMax=3");

    // ExitOnForwardFailure so ssh exits if the port forward fails
    args.add("-o");
    args.add("ExitOnForwardFailure=yes");

    // User@host
    args.add(config.sshUser.trim() + "@" + config.sshHost.trim());

    // Start the process
    if (!sshProcess.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        lastError = "Failed to start ssh process. Is ssh installed and in your PATH?";
        return 0;
    }

    // Wait briefly to see if ssh exits immediately (e.g. bad host, auth failure)
    juce::Thread::sleep(1500);

    if (!sshProcess.isRunning())
    {
        auto output = sshProcess.readAllProcessOutput();
        sshProcess.waitForProcessToFinish(500);

        // Check for common error patterns
        if (output.contains("Permission denied") || output.contains("Authentication failed"))
            lastError = "SSH authentication failed. Check your user/key file.";
        else if (output.contains("Could not resolve hostname") || output.contains("Name or service not known"))
            lastError = "Could not resolve SSH host: " + config.sshHost;
        else if (output.contains("Connection refused"))
            lastError = "SSH connection refused to " + config.sshHost + ":" + juce::String(config.sshPort);
        else if (output.contains("Connection timed out") || output.contains("timed out"))
            lastError = "SSH connection timed out to " + config.sshHost;
        else if (output.contains("Host key verification failed"))
            lastError = "SSH host key verification failed. Remove the host from known_hosts or accept it.";
        else if (output.contains("administratively prohibited"))
            lastError = "SSH server refused port forwarding (administratively prohibited).";
        else if (output.isNotEmpty())
            lastError = "SSH tunnel failed: " + output.substring(0, juce::jmin(200, output.length()));
        else
            lastError = "SSH tunnel failed to start.";

        return 0;
    }

    localPort.store(chosenLocalPort);

    // Give the tunnel a moment to establish
    juce::Thread::sleep(500);

    return chosenLocalPort;
}

void SshTunnel::stop()
{
    if (sshProcess.isRunning())
    {
        sshProcess.kill();
        sshProcess.waitForProcessToFinish(2000);
    }

    localPort.store(0);
}

bool SshTunnel::isActive() const
{
    return sshProcess.isRunning() && localPort.load() > 0;
}

} // namespace ninjamplus
