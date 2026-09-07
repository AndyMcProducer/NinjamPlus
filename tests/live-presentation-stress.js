// Private synthetic-camera tests only. Deliberately changes the final writer
// or video element, downstream of the unmodified decoder/chunk scheduler.
(() => {
    window.__njPresentationStress?.stop();
    const state = { events: [], bindings: [], timer: null, mode: 'idle', held: [], pending: 0 };
    const log = (type, extra = {}) => state.events.push({ at: Date.now(), type, mode: state.mode, ...extra });
    const videos = () => [...document.querySelectorAll('video[id^="videosource_"]')];
    state.stop = () => {
        clearTimeout(state.timer);
        for (const b of state.bindings) if (b.writer.write === b.wrapped) b.writer.write = b.original;
        state.bindings = [];
        for (const item of state.held) item.frame.close();
        state.held = [];
        for (const video of videos()) { video.playbackRate = 1; video.play().catch(() => {}); }
        log('stop'); state.mode = 'idle';
    };
    state.start = (mode, durationMs = 8000) => {
        if (!['hold-burst', 'future-pts', 'pause', 'slow-playback'].includes(mode)) throw Error('Unknown stress mode');
        if (!(durationMs > 0 && durationMs <= 15000)) throw Error('Duration out of bounds');
        state.stop(); state.mode = mode; log('start', { durationMs });
        if (mode === 'pause' || mode === 'slow-playback') {
            for (const video of videos()) {
                if (mode === 'pause') video.pause(); else video.playbackRate = .25;
                log('element', { id: video.id, paused: video.paused, playbackRate: video.playbackRate });
            }
            state.timer = setTimeout(state.stop, durationMs);
            return;
        }
        for (const [peer, rpc] of Object.entries(session.rpcs || {})) for (const file of rpc.chunkedChannels || []) {
            const writer = file.video?.frameWriter;
            if (!writer) continue;
            const original = writer.write;
            const submit = frame => {
                state.pending++;
                log('submit', { peer, timestamp: frame.timestamp, desiredSize: writer.desiredSize, pending: state.pending });
                return Promise.resolve(original.call(writer, frame)).catch(e => log('write-error', { error: String(e) }))
                    .finally(() => { state.pending--; frame.close(); });
            };
            const wrapped = frame => {
                if (state.mode === 'hold-burst') {
                    // Bound memory even if the sender exceeds the expected 30 fps.
                    if (state.held.length >= 240) state.held.shift().frame.close();
                    state.held.push({ frame: frame.clone(), submit });
                    return Promise.resolve();
                }
                return submit(new VideoFrame(frame, { timestamp: frame.timestamp + 4000000 }));
            };
            writer.write = wrapped;
            state.bindings.push({ writer, original, wrapped });
        }
        state.timer = setTimeout(() => {
            const held = state.held; state.held = [];
            log('release', { count: held.length });
            // Intentionally ignore backpressure for this one bounded burst.
            for (const item of held) item.submit(item.frame);
            state.stop();
        }, durationMs);
    };
    window.__njPresentationStress = state;
})();
