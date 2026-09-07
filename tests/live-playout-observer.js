// Passive receiver diagnostics. Run in the app's real VDO iframe MAIN context.
// Does not acquire media, change buffers, or delay/drop any traffic.
// __njPlayoutObserver.stop() restores wrapped writers and cancels callbacks.
(() => {
    window.__njPlayoutObserver?.stop();
    const state = { started: Date.now(), decoded: [], presented: [], snapshots: [], bindings: [], callbacks: new Map() };
    const append = (rows, row) => { rows.push(row); if (rows.length > 18000) rows.splice(0, 3000); };
    // Read the synthetic source's barcode independently of media timestamps.
    // Unmarked media returns null; do not infer identity from a timestamp offset.
    const pixels = document.createElement('canvas');
    pixels.width = 40;
    pixels.height = 1;
    const reader = pixels.getContext('2d', { willReadFrequently: true });
    function frameIdentity(source) {
        const width = source.displayWidth || source.videoWidth;
        const height = source.displayHeight || source.videoHeight;
        if (!width || !height) return null;
        try {
            reader.drawImage(source, 0, height * 50 / 720, width, 1, 0, 0, 40, 1);
            const rgba = reader.getImageData(0, 0, 40, 1).data;
            let header = 0, identity = 0;
            for (let bit = 0; bit < 40; bit++) {
                const value = rgba[bit * 4] > 128 ? 1 : 0;
                if (bit < 8) header = (header << 1) | value;
                else identity = ((identity << 1) | value) >>> 0;
            }
            return header === 0xa5 ? identity : null;
        } catch (_) { return null; }
    }
    function scan() {
        for (const [peer, rpc] of Object.entries(session.rpcs || {})) {
            for (const file of rpc.chunkedChannels || []) {
                const writer = file.video?.frameWriter;
                if (writer && !state.bindings.some(binding => binding.writer === writer)) {
                    const original = writer.write;
                    const wrapped = function(frame) {
                        const remoteNow = file.getRemoteNow?.();
                        append(state.decoded, { peer, at: Date.now(), perf: performance.now(), timestamp: frame.timestamp,
                            frameIdentity: frameIdentity(frame),
                            origin: file.video?.realTime, remoteNow,
                            estimatedCaptureAgeMs: remoteNow - file.video?.realTime - frame.timestamp / 1000 });
                        return original.call(this, frame);
                    };
                    writer.write = wrapped;
                    state.bindings.push({ writer, original, wrapped });
                }
                const video = document.getElementById('videosource_' + peer);
                if (video?.requestVideoFrameCallback && !state.callbacks.has(video)) {
                    function presented(now, meta) {
                        append(state.presented, { peer, at: Date.now(), perf: now, mediaTime: meta.mediaTime,
                            frameIdentity: frameIdentity(video),
                            presentationTime: meta.presentationTime, expectedDisplayTime: meta.expectedDisplayTime,
                            presentedFrames: meta.presentedFrames, processingDuration: meta.processingDuration });
                        state.callbacks.set(video, video.requestVideoFrameCallback(presented));
                    }
                    state.callbacks.set(video, video.requestVideoFrameCallback(presented));
                }
                append(state.snapshots, { peer, label: rpc.label, at: Date.now(),
                    videoCurrentTime: video?.currentTime, readyState: video?.readyState,
                    origin: file.video?.realTime, clockOffsetMs: file.timedelta,
                    stats: { ...rpc.stats?.chunked_mode_video } });
            }
        }
    }
    state.stop = () => {
        clearInterval(state.timer);
        for (const { writer, original, wrapped } of state.bindings) if (writer.write === wrapped) writer.write = original;
        for (const [video, id] of state.callbacks) video.cancelVideoFrameCallback(id);
        state.callbacks.clear();
    };
    state.export = () => ({ started: state.started, stopped: Date.now(), decoded: state.decoded, presented: state.presented, snapshots: state.snapshots });
    window.__njPlayoutObserver = state;
    scan();
    state.timer = setInterval(scan, 500);
})();
