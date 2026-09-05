// Runs actual chunked sender/receiver code with unequal device clocks.
// node tests/vdo-receiver-clock-probe.cjs <path-to-webrtc.js>
const fs = require('node:fs'), vm = require('node:vm'), assert = require('node:assert/strict');
const source = fs.readFileSync(process.argv[2], 'utf8');
const block = source.match(/file\.time = Date\.now\(\);\s*file\.theirtime = details\.timestamp;[\s\S]*?(?=\s*file\.dc = file\.channel;)/);
assert(block, 'Could not locate receiver clock initialization');
function run(skew) {
    let wall = 101000 + skew, monotonic = 5000, wakeCount = 0;
    const sent = [], timers = [];
    const file = { channel: { readyState: 'open', send: data => sent.push(JSON.parse(data)) },
        video: { controller: { wake() { wakeCount++; } } } };
    const receiverSession = { retransmit: false };
    vm.runInNewContext(block[0], { file, session: receiverSession, details: { timestamp: 100000 },
        Date: { now: () => wall }, performance: { now: () => monotonic },
        setInterval: (callback, ms) => { timers.push({ callback, ms }); return 1; }, errorlog() {} });
    wall += 5000; monotonic += 5000;
    assert.equal(file.getRemoteNow(), 105000, 'Baseline no longer reproduces the delayed header');
    assert.equal(typeof file.updateChunkedClock, 'function', 'No way to correct a delayed initial header');
    file.updateChunkedClock(106000);
    assert.equal(file.getRemoteNow(), 106000, 'Recovered delivery retained a one-second offset');
    assert.equal(wakeCount, 1, 'Pending video stayed scheduled against the old clock');
    file.updateChunkedClock(104000); file.updateChunkedClock(NaN); file.updateChunkedClock(Infinity);
    assert.equal(file.getRemoteNow(), 106000, 'Stale or invalid samples moved the clock');
    wall -= 30000; monotonic += 1000;
    assert.equal(file.getRemoteNow(), 107000, 'Local wall-clock step disturbed playout');
    assert.equal(timers.length, 1); assert.equal(timers[0].ms, 5000);
    timers[0].callback(); assert.equal(sent.at(-1).type, 'chunkedclock');
    const count = sent.length;
    receiverSession.retransmit = true; timers[0].callback(); assert.equal(sent.length, count);
    receiverSession.retransmit = false;
    file.channel.readyState = 'closed'; timers[0].callback(); assert.equal(sent.length, count);
}
run(0); run(3600000); run(-3600000);
const handler = source.match(/\n\t{2}session\.chunkedTransferChannels\[UUID\]\.onmessage = (event => \{[\s\S]*?\n\t{2}\});/);
assert(handler, 'Could not locate direct publisher control handler');
const replies = [];
const channel = { clockAnchor: { wall: 100000, monotonic: 5000 }, send: data => replies.push(JSON.parse(data)) };
const session = { chunkedTransferChannels: { peer: channel }, chunkedRecorder: {} };
const receive = vm.runInNewContext('(' + handler[1] + ')', { session, UUID: 'peer',
    Date: { now: () => 999999 }, performance: { now: () => 10000 }, warnlog() {} });
receive({ data: JSON.stringify({ type: 'chunkedclock' }) });
assert.equal(replies.length, 1);
assert.equal(replies[0].timestamp, 105000, 'Sender wall-clock step contaminated the sample');
receive({ data: JSON.stringify({ kf: true }) });
assert.equal(session.chunkedRecorder.needKeyFrame, true, 'Keyframe recovery regressed');
assert(source.includes('clearInterval(file.chunkedClockTimer)'), 'Clock polling leaks on close');
console.log('PASS: delayed header recovery, unequal clocks, stale samples, clock steps, polling, keyframe compatibility');
