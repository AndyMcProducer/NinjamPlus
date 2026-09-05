// Exercise the real main-thread WebCodecs publisher with delayed initial frames.
// node tests/vdo-video-origin-probe.cjs <path-to-webrtc.js>
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(process.argv[2], 'utf8');
const match = source.match(/session\.webCodec = (async function \(config = null\) \{[\s\S]*?\n\t\});/);
assert(match, 'Could not locate production WebCodecs publisher');

async function run(firstArrival, extraFrames = [], wallShift = 0) {
    let now = firstArrival;
    let wallOffset = 0;
    let deliver;
    const encoded = [], messages = [];
    const reader = { read: () => new Promise(resolve => { deliver = resolve; }), releaseLock() {} };
    const session = { chunkedVideoEnabled: null, stats: {}, getLocalStream: () => ({ getVideoTracks: () => [{}] }),
        chunkedRecorder: {}, chunkedTransferChannels: { peer: { readyState: 'open', send: value => messages.push(JSON.parse(value)) } } };
    const context = vm.createContext({
        session, counterWebCodec: 0, Date: { now: () => now + wallOffset }, performance: { now: () => now },
        MediaStreamTrackProcessor: function () { this.readable = { getReader: () => reader }; },
        VideoEncoder: class {
            constructor() { this.state = 'unconfigured'; }
            configure() { this.state = 'configured'; }
            encode(frame) { encoded.push(frame.timestamp); }
            close() { this.state = 'closed'; }
        },
        warnlog() {}, errorlog(error) { throw error; }, log() {},
    });
    const start = vm.runInContext('(' + match[1] + ')', context)({ codec: 'vp8', width: 1280, height: 720, frameRate: 30 });
    async function frame(timestamp, arrival) {
        now = arrival;
        assert(deliver, 'Publisher stopped requesting frames');
        deliver({ done: false, value: { timestamp, close() {} } });
        await Promise.resolve();
        await Promise.resolve();
    }
    await frame(0, firstArrival);
    await start;
    wallOffset = wallShift;
    await frame(800000, 100810);
    await frame(833333, 100845);
    for (const [timestamp, arrival] of extraFrames) await frame(timestamp, arrival);
    return { origin: session.stats.Chunked_video.realTime, encoded, messages };
}

(async () => {
    const receiverBlock = source.match(/if \(typeof metadata\.realTimeVideo === "number"\) \{[\s\S]*?\n\t{6}\}/);
    assert(receiverBlock, 'Could not locate the receiver timing update');
    let wakes = 0;
    const file = { video: { realTime: 100750, controller: { wake: () => wakes++ } } };
    vm.runInNewContext(receiverBlock[0], { metadata: { realTimeVideo: 100010 }, details: {}, file });
    assert.equal(wakes, 1, 'A corrected origin left the pending frame scheduled against the old clock');
    const delayed = await run(100750);
    console.log(JSON.stringify(delayed));
    assert(Math.abs(delayed.origin - 100010) < 2, 'A queued first frame permanently shifted the video clock by 750 ms');
    assert.deepEqual(delayed.encoded, [0, 800000, 833333], 'Clock refinement changed encoded frame timestamps');
    assert(delayed.messages.some(m => m.type === 'chunkedtiming' && Math.abs(m.realTimeVideo - 100010) < 2),
        'Connected viewers did not receive the corrected origin');
    const fresh = await run(100000);
    assert.equal(fresh.origin, 100000, 'Fresh capture clock moved forward because later frames arrived late');
    const clockStep = await run(100750, [], -5000);
    assert.equal(clockStep.origin, delayed.origin, 'A wall-clock adjustment corrupted startup calibration');
    const frames = Array.from({ length: 58 }, (_, i) => [900000 + i * 33333, 100920 + i * 33.333]);
    frames.push([4000000, 103900]);
    const bounded = await run(100750, frames);
    assert.equal(bounded.origin, delayed.origin, 'Calibration continued after its startup window');
    console.log('PASS: queued startup frame, connected viewer correction, fresh-frame control, wall-clock step, and bounded calibration');
})().catch(error => { console.error(error); process.exitCode = 1; });
