// Read-only regression against VDO's chunked sender reconfiguration block.
// node tests/vdo-encoder-config-probe.cjs <path-to-webrtc.js>
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(process.argv[2], 'utf8');
const block = source.match(/if \(session\.chunkedRecorder && session\.chunkedRecorder\.videoEncoder && session\.chunkedRecorder\.videoEncoder\.configure && session\.chunkedRecorder\.videoEncoder\.config\) \{[\s\S]*?\n\t{7}\}/);
assert(block, 'Could not locate production sender block');
let calls = 0;
const encoder = { config: { bitrate: 2500000, width: 1280, height: 720, tuning: { bitrate: 2500000 } },
    configure: () => { calls++; } };
const session = { stats: { adjustBitrate: 2500 }, chunkadaptresolution: false,
    chunkedRecorder: { videoEncoder: encoder, adaptiveBaseVideoWidth: 1280, adaptiveBaseVideoHeight: 720 } };
const context = vm.createContext({ session, applyChunkAdaptiveResolution: recorder => {
    recorder.videoEncoder.config.width = 640;
    recorder.videoEncoder.config.height = 360;
    return 1;
} });
function tick() { vm.runInContext(block[0], context); }
for (let i = 0; i < 600; i++) tick();
console.log('Stable settings configure calls:', calls);
assert.equal(calls, 0, 'Identical settings reconfigure the encoder on every sender tick');
session.stats.adjustBitrate = 1800;
tick(); tick();
assert.equal(calls, 1, 'Changed bitrate must configure once');
assert.equal(encoder.config.bitrate, 1800000);
assert.equal(encoder.config.tuning.bitrate, 1800000);
session.chunkadaptresolution = true;
tick(); tick();
assert.equal(calls, 2, 'Changed resolution must configure once');
encoder.config.tuning.bitrate = 1;
tick(); tick();
assert.equal(calls, 3, 'Legacy tuning update must configure once');
console.log('PASS: stable settings, bitrate change, resolution change, and legacy tuning');
