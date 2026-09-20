// Read-only transport telemetry for the actual VDO iframe. No ICE configuration
// or message-delivery changes. Stop this recorder by closing the test page.
(() => {
    if (window.__njNetworkObserver) return;
    const state = window.__njNetworkObserver = { samples: [], busy: false };
    state.timer = setInterval(async () => {
        if (state.busy) return;
        state.busy = true;
        try {
            const row = { at: Date.now(), peers: [] };
            for (const direction of ['rpcs', 'pcs']) {
                for (const [id, peer] of Object.entries(session[direction] || {})) {
                    const entry = { id, direction, connection: peer.connectionState, ice: peer.iceConnectionState };
                    try {
                        const stats = await peer.getStats();
                        for (const transport of stats.values()) {
                            if (transport.type !== 'transport' || !transport.selectedCandidatePairId) continue;
                            const pair = stats.get(transport.selectedCandidatePairId);
                            const local = stats.get(pair.localCandidateId);
                            const remote = stats.get(pair.remoteCandidateId);
                            entry.pair = pair.id;
                            entry.rtt = pair.currentRoundTripTime;
                            entry.bytesReceived = pair.bytesReceived;
                            entry.bytesSent = pair.bytesSent;
                            entry.protocol = local.protocol;
                            entry.port = local.port;
                            entry.remoteProtocol = remote?.protocol;
                            entry.remoteCandidateType = remote?.candidateType;
                            entry.remoteAddress = remote?.address;
                            entry.remotePort = remote?.port;
                            entry.relayProtocol = local.relayProtocol;
                            entry.address = local.address;
                            entry.url = local.url;
                            entry.candidateType = local.candidateType;
                        }
                    } catch (error) { entry.error = String(error); }
                    row.peers.push(entry);
                }
            }
            state.samples.push(row);
            if (state.samples.length > 1800) state.samples.splice(0, 300);
        } finally { state.busy = false; }
    }, 1000);
})();
