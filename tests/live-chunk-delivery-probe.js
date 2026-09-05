// Inject in the actual VDO publisher iframe before joining a camera.
// This delays application messages on the chunked media channel, not UDP packets.
// Actual encoding, SCTP transport, decoding, and native NINJAM audio stay in use.
// Set __njDelivery.delayMs / jitterMs, or pauseUntil = Date.now() + duration.
// Closing this test tab removes the shim. Do not inject into a real user session.
(() => {
    const state = window.__njDelivery = {
        delayMs: 0, jitterMs: 0, pauseUntil: 0, queued: 0, delivered: 0,
        pendingBytes: 0, maxPendingBytes: 0, closedDrops: 0, seed: 12345,
        queue: [], timer: null, lastDue: 0,
    };
    function drain() {
        state.timer = null;
        while (state.queue.length) {
            const next = state.queue[0];
            const wait = Math.max(next.due, state.pauseUntil) - Date.now();
            if (wait > 0) {
                state.timer = setTimeout(drain, wait);
                return;
            }
            state.queue.shift();
            state.pendingBytes -= next.size;
            if (next.channel.readyState === 'open') {
                next.send(next.data);
                state.delivered++;
            } else state.closedDrops++;
        }
    }
    function wrap(channel) {
        if (channel.label !== 'chunked' || channel.__njDeliveryWrapped) return channel;
        channel.__njDeliveryWrapped = true;
        const send = channel.send.bind(channel);
        channel.send = function (data) {
            const size = typeof data === 'string' ? data.length : data.byteLength || data.size || 0;
            if (state.pendingBytes + size > 32 * 1024 * 1024) throw new Error('Test delivery queue limit exceeded');
            state.seed = (1664525 * state.seed + 1013904223) >>> 0;
            const jitter = state.jitterMs * state.seed / 4294967296;
            const due = Math.max(state.lastDue, Date.now() + state.delayMs + jitter);
            state.lastDue = due;
            state.queue.push({ channel, send, data, size, due });
            state.queued++;
            state.pendingBytes += size;
            state.maxPendingBytes = Math.max(state.maxPendingBytes, state.pendingBytes);
            if (!state.timer) drain();
        };
        return channel;
    }
    // adapter.js installs an own send method, so wrapping the prototype alone
    // does not intercept the real channel used by this application.
    Object.values(session.chunkedTransferChannels).filter(Boolean).forEach(wrap);
    const create = RTCPeerConnection.prototype.createDataChannel;
    RTCPeerConnection.prototype.createDataChannel = function () {
        return wrap(create.apply(this, arguments));
    };
})();
