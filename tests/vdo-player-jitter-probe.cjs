// Read-only diagnostic for the actual VDO chunked player controller.
// Run: node tests/vdo-player-jitter-probe.cjs <path-to-vdoninja-webrtc.js>
// Guards against modest scheduler jitter discarding intact buffered video.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');

assert(process.argv[2], 'Supply the VDO webrtc.js source path');
const source = fs.readFileSync(process.argv[2], 'utf8');
const match = source.match(/file\.createChunkedVideoController = (function \(UUID\) \{[\s\S]*?\n\t{8}\});/);
assert(match, 'Could not locate the production controller; review source layout');

function run(latenessMs) {
    let now = 100000;
    let nextTimer = 0;
    const timers = new Map();
    const decoded = [];
    let keyframeRequests = 0;
    const file = {
        video: { decoder: { state: 'configured' }, realTime: now },
        stream_configVideo: { frameRate: 30 },
        getRemoteNow: () => now,
        decodeChunkedVideoFrame: frame => decoded.push(frame.timestamp),
        requestChunkedVideoKeyframe: () => { keyframeRequests++; },
    };
    const session = { rpcs: { peer: { stats: {} } }, stats: {} };
    const context = vm.createContext({
        file, session, Date: { now: () => now }, Math, Number,
        resolveChunkedBufferTarget: () => 8000,
        errorlog: error => { throw error; },
        setTimeout: (callback, delay) => {
            const id = ++nextTimer;
            timers.set(id, { callback, due: now + delay });
            return id;
        },
        clearTimeout: id => timers.delete(id),
        setInterval: () => ++nextTimer,
        clearInterval: () => {},
    });
    const controller = vm.runInContext('(' + match[1] + ')', context)('peer');
    // One initial keyframe, then an uninterrupted GOP already received in full.
    // A decoder can consume every delta frame: no transport packet is missing.
    for (let frame = 0; frame < 240; frame++) {
        controller.enqueue({ timestamp: frame * 1000000 / 30,
            type: frame === 0 ? 'key' : 'delta' });
    }
    const first = [...timers.entries()].sort((a, b) => a[1].due - b[1].due)[0];
    assert(first, 'Expected scheduled playout');
    timers.delete(first[0]);
    now = first[1].due + latenessMs;
    first[1].callback();
    // Deliver immediate catch-up callbacks without advancing the clock again.
    for (let step = 0; step < 20; step++) {
        const ready = [...timers.entries()].find(([, timer]) => timer.due <= now);
        if (!ready) break;
        timers.delete(ready[0]);
        ready[1].callback();
    }
    return { latenessMs, decoded: decoded.length, queued: file.video.queue.length,
        keyframeRequests, stats: session.rpcs.peer.stats.chunked_mode_video };
}

const onTime = run(0);
const late = run(70);
const veryLate = run(1000);
console.log(JSON.stringify({ onTime, late, veryLate }, null, 2));
assert(onTime.queued > 200 && onTime.keyframeRequests === 0, 'Control did not retain the buffer');
assert(late.queued > 0, '70 ms scheduler jitter discarded the entire intact 8-second video buffer');
assert(late.decoded >= 3, 'Retained frames did not catch up to the playout clock');
for (const latenessMs of [150, 250]) {
    const result = run(latenessMs);
    assert(result.queued > 200 && result.keyframeRequests === 0,
        `${latenessMs} ms jitter unnecessarily discarded the intact buffer`);
    assert(result.decoded >= Math.floor(latenessMs / (1000 / 30)), 'Catch-up stopped making progress');
}
assert(veryLate.queued === 0 && veryLate.keyframeRequests === 1, 'Large lateness stopped requesting recovery');
