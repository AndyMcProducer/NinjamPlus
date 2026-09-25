# 1080p60 switching failure

Reproduced the reported up/down failure with H.264: 720p30 worked, selecting
1080p60 closed the encoder, and returning to 720p30 left both receivers frozen.
This establishes a real codec configuration bug, but Bernd's codec/browser is
not yet confirmed. AV1 also lost frame rate at 1080p60 in testing, but recovered
after both downgrades; that degradation is not the same demonstrated failure.

## Cause and fix

VDO alpha's `chunkedRecorder.updateVideoProfile()` changed dimensions, bitrate,
and metadata frame rate without changing the H.264 level selected at startup.
The 720p codec string `avc1.42E01F` specifies AVC level 3.1. Chrome rejected
1920x1080 because its coded area exceeds that level's limit. The encoder error
handler repeatedly restarted with the same invalid configuration. Returning to
720p reopened the sender encoder, but the receiver remained awaiting keyframes
with no displayed-frame progression throughout both recovery phases. Sender
timestamp origins changed during restarts while receiver origins remained old;
general restart/timeline repair is separate from preventing this invalid switch.

The local VDO source fix recalculates the H.264 level for the current encoded
dimensions and frame rate, preserving profile/compatibility bytes. It also sets
WebCodecs' lowercase `framerate` member; the existing `frameRate` metadata remains
for compatibility. 1080p60 now uses `avc1.42E02A` (level 4.2). No NTP, audio-delay,
receiver-buffer, or network-adaptation changes were made for this fix.

This is a VDO alpha deployment change, not a new NinjamPlus installer fix.
Already wedged sessions should refresh/rejoin after updating; the tested fix
prevents the bad profile transition and does not establish recovery from every
possible encoder failure.

## Tests

Fetched and merged AMP's current release (`0d5ec238`, including NTP changes) into
the existing local release branch, preserving our previous test-tool changes.
Rebuilt the native harness. All 19 helper tests and embedded consistency passed.
The native test used two processors, a private local NINJAM server, hotspot off,
190 BPM / 16 BPI, actual embedded helper pages, and installed Chrome.

The first camera simulation remained 720p despite requesting 1080p and was
discarded as 1080p evidence. Valid runs used Chrome's file-backed camera with a
1920x1080 60-fps Y4M source, and verified actual capture dimensions and rate.
No artificial network loss was applied. This is not a physical-camera,
cross-network, or BrowserStack device qualification.

Before-fix tests loaded deployed `webrtc.js?ver=964`, SHA-256
`e1eafc0728dc1fad9619658c2b9c2fc708f67e640c5e380f0482640aef375ce2`.
Fixed tests replaced only `updateVideoProfile` in that exact deployed script
through a test response override. The public alpha deployment is unchanged.

Each H.264 run performed two 720p30 -> 1080p60 -> 720p30 cycles. Excluding the
first eight observations of each phase to allow delayed frames to play through:

| Phase | Before: displayed fps, alpha/bravo | Fixed: displayed fps, alpha/bravo |
| --- | --- | --- |
| 1080p60, cycle 1 | 0 / 0 (encoders closed) | 54.4 / 55.0 |
| 720p recovery, cycle 1 | 0 / 0 | 30.0 / 30.0 |
| 1080p60, cycle 2 | 0 / 0 (encoders closed) | 55.7 / 57.1 |
| 720p recovery, cycle 2 | 0 / 0 | 29.9 / 29.8 |

The fixed run had no encoder restart errors. Some 1080p frames still dropped;
this is not a promise of perfect 60-fps presentation on all devices. Before-fix
AV1 delivered roughly 17-19 fps to one receiver and 51-52 to the other at 1080p60,
but both recovered to 30 fps at 720p. Sender network queues remained small in
that run; the exact processing/capture bottleneck is not isolated.

One additional AV1 cycle with the patch delivered 19.7/51.8 fps at 1080p and
30.0/30.0 fps after downgrade. The patch therefore preserves AV1 recovery but
does not resolve its uneven high-resolution throughput on this host.

Artifacts are under `test-results/quality-switch-native-{h264,av1}-{before,fixed}`.
The VDO unit regression `tests/chunked-quality-profile.cjs` fails on the old
profile and passes with the patch. The live analyzer rejects the old H.264 run
and accepts both patched cycles. The VDO repro page is
`examples/chunked-quality-switch.html`.

## Reproduce

Generate the camera using `python tests/create-quality-camera.py
test-results/quality-1080p60.y4m`. Start the native harness with `--live-vdo
--bpm=190 --duration-seconds=1200`, then use its room and helper ports:

```powershell
node tests/run-quality-switch.cjs --room=ROOM --alpha-port=8001 --bravo-port=8002 --file=test-results/quality-1080p60.y4m --codec=h264 --output=test-results/NEW-QUALITY-RUN
node tests/analyze-quality-switch.cjs test-results/NEW-QUALITY-RUN --expect-recovery
```

Use `--webrtc-source=FILE` only for explicit local-patch validation; save loaded
script hashes and distinguish it from a deployed-alpha test. Stop the native
harness with `test-results/live-stop` when browser testing is finished.

## Deployed alpha verification

After Steve deployed the update, repeated both H.264 quality-switch cycles
against public alpha without any response override. Both clients loaded
`webrtc.js?ver=965`, SHA-256
`2aecb5b1d5127927c7759d46ac50ba8d6a6be72d7166c8b59c786b22557524e3`.
The same native helper configuration, installed Chrome, file-backed 1080p60
camera, and hotspot-off mode were used; no artificial loss was applied.

| Phase | Displayed fps, alpha/bravo |
| --- | --- |
| 1080p60, cycle 1 | 56.8 / 55.9 |
| 720p recovery, cycle 1 | 30.0 / 29.9 |
| 1080p60, cycle 2 | 57.8 / 57.6 |
| 720p recovery, cycle 2 | 30.0 / 30.0 |

All encoder states stayed configured, capture dimensions/rates matched each
requested phase, and there were no media/encoder errors. The recovery analyzer
passed. Artifacts: `test-results/quality-switch-native-h264-deployed`.
This confirms the deployed H.264 transition fix on this host; it does not
establish perfect 60-fps performance or qualify Bernd's unconfirmed codec/device.
