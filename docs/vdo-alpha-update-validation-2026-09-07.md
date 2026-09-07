# Updated alpha validation (2026-09-07)

Fresh-profile installed Chrome, private two-client native synthetic-audio server,
and the rebuilt app's embedded helper pages. No helper response overrides were
used in this run. Both clients loaded identical deployed scripts, byte-identical
to the current local VDO source. `loaded-scripts.json` records response URLs,
headers, and SHA256 hashes in `test-results/alpha-update-validation/`.

- webrtc.js?ver=960: 2206e7df2a5ee1a7c2fe33681c73a854293ce5356da9791e5615c7383fce02f8
- lib.js?ver=14193: 07bf0db9ce1222d5bb36c7f95a735978bd427b0141f42505f72967e6693fe635
- main.js?ver=1082: d65c11e2178ada759a8b25dd8cd14d822bd9c122eeaff34b894fbc24785b75af

The future-timestamp injection retained independent pixel-identity matches for
all 13 observed callbacks in its measurement window. Media time rebased by
-4000 ms; median writer-to-expected-display time was 32 ms. The subsequent
bounded hold released 160 frames together. All 36 observed callbacks after
release matched content; median writer-to-expected-display time was 29.8 ms,
maximum 51.8 ms. Callback wall-clock delivery sometimes lagged much longer
(up to 869 ms), so callback delivery and expected display are kept separate.
These are sampled browser observations, not physical scanout measurements.

A late picture-age spike reached 8358 ms before network faults began. The saved
post-release window's median pixel age had returned near the 4.4-second baseline,
and the spike cleared. It does not establish a retained video-element queue.
The first network suite was cancelled to investigate, then source-preview
freshness observations were added before restarting a separate complete suite.
Do not combine the cancelled suite with the full network test.

Both test clients share one host/clock. This does not replace cross-machine or
physical audio/display validation, and cannot prove Bernd's four-bar lead fixed.

## Reproduced clock-origin fault

The system wall clock advanced about 1890 ms relative to the browser monotonic
timeline. Following the TCP reconnect, both receivers stayed approximately
1.84 seconds early. Independent source-to-output audio correlation confirmed
audio at about 5053 ms while video was about 3215 ms old. The requested video
buffer was still about five seconds; this was not merely a green-light issue.

The deployed publisher used old capture origins but fresh wall time in reconnect
headers and per-channel clock anchors. A local fix in the parent VDO `webrtc.js`
uses one session monotonic clock for capture origins, headers/retries, and clock
replies. `node ../tests/chunked-clock-regression.cjs` passes forward/backward
5053 ms jumps for video/audio/combined headers; the saved deployed code fails.

Live fixed-code testing used the same app/runtime with a local response override
for `webrtc.js`. A +5053 ms publisher-page clock injection and reconnect retained
final video/audio median differences of +88/+71 ms after source/recovery stalls
cleared. Details: `../tests/CHUNKED-CLOCK-REVIEW.md` in the parent VDO repository,
and `test-results/alpha-clock-fix-validation/clock-fix-summary.json`.

The fix is local and has not been deployed to alpha. The full UDP repeat was
deferred to investigate and validate this fault. These results identify a real
cause of early video after reconnects, not proof of Bernd's specific clock history.
