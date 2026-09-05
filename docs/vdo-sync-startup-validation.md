# VDO startup sync: reproduction and review notes

Base: Andy's release commit `4ec2d458`, reviewed 2026-09-05.

Current result: the native capture-duration/startup fixes pass both native soak
modes, all 16 helper tests, and Release standalone/VST3 builds. The additional
VDO receiver-clock fix also passes the previously reproduced delayed-join case:
after removing a 1000 ms delivery delay, rendered video changed from 9644 to
8644 ms without reload. This VDO change is in the separate root webrtc.js checkout
and requires an alpha release; it is not part of the NINJAM binary or this PR's
production changes. Details and limits follow, including historical failed tests.

## Confirmed missing recording interval

A live test reproduced video almost one interval ahead of NINJAM audio despite
a green sync indicator. Two native processors connected to a loopback NINJAM
server at 120 BPM / 16 BPI (8000 ms per interval). The normal launchVideoSession()
path opened external Chrome and the real https://vdo.ninja/alpha/ player with
the app's chunked-video configuration. Generated timestamp cameras and audio
replaced physical devices; audio and video encoding, transport, and playback
were real.

Before the fix, waveform correlation measured audio 8621 ms after capture
(normalized correlation approximately 0.68-0.70). Rendered video was only
730-776 ms old, with a 617 ms native buffer: approximately 7.9 seconds ahead.

The tag identifies an audio interval that has finished recording. The calculation
measured completion-to-playback, omitting the interval spent recording those
samples. Add recording duration to the matched-GUID and legacy fallback paths.
This is not additional network-route latency; existing route calculations stay.

Regression expectations were changed before production code: a 2000 ms recording
plus a 250 ms playback phase must yield 2250 ms, not 250 ms. Both GUID and fallback
tests failed before the fix. The missing-GUID timeout also includes recording time.
The previous soak oracle compared completion-to-playback only and could pass
while real media were an interval apart. Its oracle now includes capture duration.

An initial live retest measured audio near 8624 ms and settled video at 8708 ms
(84 ms difference), and 8719 ms after helper reload (95 ms difference).

## VDO player scheduling-jitter defect

Repeated fresh Auto joins selected AV1 (av01.0.04M.08) on this Windows Chrome 152 /
GTX 1070 system. Video froze, entered rebuffering/awaiting-keyframe, and displayed
timestamps aged beyond 30 seconds while native audio stayed near 8.6 seconds.
Chrome media memory grew substantially. This occurred with Andy's original
refresh sequence and with an experimental desired-delay-before-refresh sequence.

VP8 initially advanced continuously for over a minute (video near 8846 ms versus
audio at 8613-8614 ms). A rebuilt Auto-to-VP8 experiment likewise started normally
(8843 ms video versus 8590 ms audio), but then froze after helper reload. Its
displayed timestamp aged to 33755 ms, with rebuffering/awaiting-keyframe set.
The proposed codec-default change was therefore removed, including its URL test.
It did not resolve the actual failure. Codec selection remains Andy's original.

A deterministic read-only probe of the actual VDO controller then reproduced a
specific recovery defect: after receiving an intact 8-second GOP, a 70 ms delayed
playout callback caused all 239 queued delta frames to be discarded. The on-time
control retained the queue. The controller's one-frame late tolerance (33 ms at
30 fps) treats ordinary scheduler jitter as grounds to throw away future frames
too when there is no later buffered keyframe. It then waits for a fresh keyframe
and another buffer fill, matching the live rebuffering state. This validates a
player defect; it does not prove every observed stall has the same cause.

Run the diagnostic against a local VDO checkout, without modifying it:

```powershell
node tests/vdo-player-jitter-probe.cjs ../webrtc.js
```

The command failed against the original source: 70 ms jitter discarded 239 frames.
Steve subsequently authorized changes within the chunked/WebCodecs sections.
The minimum tolerance in webrtc.js is now 250 ms. The same regression passes:
three overdue frames decode, the intact buffer remains, and 1000 ms lateness still
requests recovery. This permits brief catch-up instead of discarding seconds of
playable data. No RTP or other non-chunked production sections were modified.
Sustained overload and decoder backpressure remain relevant validation limits.

Live verification after approval used the edited checkout served on localhost:8765,
selected through the helper's existing vdoBase override in normal Chrome. Runtime
inspection confirmed the active controller contained the 250 ms tolerance. The
Auto/AV1 stream survived ten deliberate 70 ms main-thread delays with no additional
playout drops (counter stayed at 1) and no rebuffering. A real helper reload then
resumed advancing video without the previously observed persistent freeze.
The first reload frame was near-live (~738 ms old) before the buffer filled;
settled video was about 9230 ms old versus 8618 ms audio. Thus this verifies the
jitter-loss fix, not complete audiovisual calibration or first-frame sync.
The existing zero-delay refresh sequence is unchanged.

The experimental zero-delay refresh removal was discarded. One earlier reload
showed a near-live frame (242 ms old) while refilling. Changing that sequence alone
did not demonstrate reliable startup. Andy's flush/refresh/delayed-reapply sequence
is preserved.

## Other reproduced native defects

- Initial readings [0,800,800] produced 180 ms; [16000,800,800] produced 14176 ms.
  Seed the first publishable value from the three-sample median. Subsequent
  smoothing and significant-change rules stay.
- A first legacy marker 250 ms before a 2000 ms boundary was rejected by the
  half-interval gate. Independent client phases make this valid. Remove that gate
  while retaining the 100 ms near-boundary guard.
- A first GUID was consumed by fallback before playback because no previous GUID
  history existed. Give it the same bounded 1.5-interval matching window as later
  GUIDs. Missing audio still times out.
- Helper refresh erased measurements, pending markers, and duplicate suppression,
  causing recalculation and another flush. Retain native state on helper refresh.
  Actual peer reconnect/session replacement still resets state.

Each was reproduced by a failing native regression before its fix. The soak also
checks jitter, delayed/duplicate tags, real GUID playback, refresh snapshots,
retired-session tags, and rapid reconnects.

## Reproduction

Configure CMake with NINJAMPLUS_BUILD_INTEGRATION_TESTS=ON and run from the repo:

```powershell
cmake --build build --config Release --target NINJAMplus_SyncSoakTest --parallel 4
& ./build/NINJAMplus_SyncSoakTest_artefacts/Release/NINJAMplus_SyncSoakTest.exe "$PWD/build/ninjamsrv/Release/ninjamsrv.exe"
& ./build/NINJAMplus_SyncSoakTest_artefacts/Release/NINJAMplus_SyncSoakTest.exe "$PWD/build/ninjamsrv/Release/ninjamsrv.exe" --near-boundary
npm test
```

For live media, ensure test-results exists and add --live-vdo. It starts a private loopback NINJAM server, generates
audio, opens two normal browser helpers in a unique VDO room, and runs until
test-results/live-stop exists or 15 minutes elapse. Run tests/live-video-probe.js
in each VDO iframe's main execution context before Join Room with Camera / Start
streaming. Read window.__njLiveProbe.samples for remote frame ages. Repeat after
actual helper reload. Only media acquisition is replaced; closing the tabs removes
the instrumentation.

Run python tests/analyze-live-sync.py (NumPy and SciPy required) to correlate
source audio with decoded output. It writes test-results/live-audio-analysis.json.
Raw float32 mono 48 kHz captures and wall-clock block timestamps are under
test-results/live-*. Save recordings between runs; the next live run overwrites
them. Playwright output is isolated under test-results/playwright to prevent its
cleanup from deleting native evidence.

The simulated audio device now schedules blocks against an absolute clock.
Sleeping after processing each block previously accumulated processing time as
clock drift. The refresh-expiry assertion allows the existing 500 ms cached HTTP
publication cycle after the 1500 ms expiry. Neither changes the production audio
engine or timeout.

## Follow-up: encoder reconfiguration and startup capture clock

After the deployed alpha jitter fix, live Chrome instrumentation showed the
chunked sender calling VideoEncoder.configure repeatedly with identical settings.
The production-code regression `tests/vdo-encoder-config-probe.cjs` reproduced
600 configure calls for 600 unchanged sender ticks before the fix. It now records
zero, while bitrate, resolution, and legacy tuning changes still configure once.
The actual publisher made one initial configure call during the fresh live run.
This avoids redundant encoder work; it does not establish that every prior freeze
was caused by reconfiguration.

The main-thread publisher also anchored its capture clock to the arrival time of
the first queued frame. A stale first frame therefore permanently added its age
to playback timing. `tests/vdo-video-origin-probe.cjs` runs the actual publisher
with a first frame arriving 750 ms late: the old origin remained 100750; fresh
subsequent frames now refine it to 100010. Encoded timestamps remain unchanged.
The existing chunkedtiming message updates connected viewers and wakes their
pending scheduling timer. A receiver regression failed before that wake was added.
Additional controls verify fresh first frames, wall-clock steps, and a bounded
60-frame calibration window. Calibration can only move the origin earlier.

These follow-up changes are confined to chunked/WebCodecs sections of
../webrtc.js. The worker publisher path is unchanged and was not validated by
this clock regression. No new protocol message type or codec default was added.
After Steve deployed the jitter change, a cache-refreshed fetch confirmed alpha
contained it. The additional encoder and clock changes were tested locally via
the helper's existing vdoBase override in the same external Chrome runtime.

Fresh live AV1 720p30 results with all three VDO changes: native waveform
correlation measured 8620-8622 ms audio delay; 100 rendered-video samples over
20 seconds measured median 8681 ms (8673-8712 ms), about 60 ms behind audio.
These measurements use generated camera pixels and actual NINJAM audio processing.
The first displayed frame still arrives nearly live while buffering fills; this
is not evidence of synchronized first-frame presentation.

After a full receiving-helper reload and camera rejoin, the final 100 samples
over 20 seconds measured median 8663 ms (8650-8692 ms), versus approximately
8620 ms correlated audio delay. Playback resumed without manual buffer adjustment.
The old near-live preview appeared during refill, followed by correctly delayed
advancing video. Treat initial buffer-fill presentation as an outstanding UX
limitation, not as a fixed first-frame guarantee. The encoder and capture-clock
follow-ups require a further alpha deployment; the earlier jitter change alone
does not include them.

Run the separate VDO regressions from NinjamPlus with:

```text
node tests/vdo-player-jitter-probe.cjs ../webrtc.js
node tests/vdo-encoder-config-probe.cjs ../webrtc.js
node tests/vdo-video-origin-probe.cjs ../webrtc.js
```

## Validation and limits

### Deployed-alpha stress follow-up (2026-09-05)

The deployed alpha response contains all three VDO changes. A normal Chrome
reload initially reused old webrtc.js?ver=959, even with cache disabled on the
outer launcher target. A cache-reload fetch of that script followed by reload
produced an active publisher with startupOriginSamples and one initial encoder
configure call. Deployment freshness must be verified in the actual iframe;
the launcher cacheBust parameter does not version VDO's script dependencies.

Chrome CDP network emulation was NOT validated for the live media connection.
Both emulateNetworkConditionsByRule and the older emulateNetworkConditions
accepted loss/bandwidth settings, but a control requesting 100% loss and one
byte/second still delivered roughly 0.75 MB of chunked data in 16.6 seconds.
Do not report these runs as successful UDP-loss or bandwidth tests. Freezes and
eventual recovery observed during them cannot be attributed to the requested
network profile. CDP documents these knobs at
https://chromedevtools.github.io/devtools-protocol/tot/Network/ ; acceptance of
the command alone is insufficient evidence that it affected this connection.

The verified substitute is tests/live-chunk-delivery-probe.js, installed only in
the generated-camera test publisher. It delays the actual adapter-wrapped
chunked channel send calls in order, with counters and a bounded 32 MB test queue.
It does not emulate UDP loss or SCTP retransmissions. A prototype-only send hook
was ineffective because adapter.js gives channels their own send method; the
saved probe wraps that method and future channels after adapter initialization.

With 500-1000 ms ordered message delay, counters verified 914 queued / 858 sent
messages and 45,219 bytes pending. Settled video continued advancing near its
9131 ms pre-test delay. The transition incurred additional playout drops, so this
is a recovery result rather than proof of uninterrupted playback.

A precise 12-second pause held up to 629,066 bytes. Video used its existing buffer
for about eight seconds, then froze; displayed frame age peaked at 17,907 ms.
It resumed near its previous 9.13-second delay about six seconds after delivery
resumed, without reload or manual offset changes. The test queue fully drained
(3940 queued / 3940 delivered). NINJAM audio remained on its separate real native
processing path; waveform correlation earlier in this run measured 8592-8603 ms.
This deployment run therefore still had roughly half a second of residual video
delay, unlike the earlier local run's 40-60 ms. Do not treat the earlier result as
a universal calibration guarantee.

### Confirmed remaining startup-clock problem

With the publisher's verified chunk send delay set to 1000 ms BEFORE a fresh
receiver helper join, settled video delay became 10134 ms instead of 9131 ms.
The receiver's file.timedelta changed from 10 to 1010 ms. Removing the delivery
delay and draining the test queue did not remove this extra second: a later
75-sample window still measured median 10118 ms (10102-10182 ms), and timedelta
remained 1010 ms. Both peers continued streaming without a reconnect.

The receiver initializes its remote clock from the first header's send timestamp
and its arrival time, then advances that clock monotonically. Initial delivery
delay is indistinguishable from remote/local clock offset in that single sample
and is retained for the connection. The previous publisher capture-origin fix
addresses a different clock and does not solve this case.

The reproduction was subsequently converted to a failing acceptance regression
before implementing the receiver fix. `node tests/vdo-receiver-clock-probe.cjs
../webrtc.js` now passes recovery using fresh sender clock observations, including
remote/local clocks differing by plus or minus one hour, stale/invalid samples,
local and sender wall-clock steps, polling cleanup, and keyframe compatibility.

The new chunkedclock request/reply obtains a fresh publisher timestamp every five
seconds. The sender advances an anchor with performance.now(); receiver playout
also uses a monotonic clock. Only a fresher estimate can advance the remote clock;
congestion cannot push it backward and add delay. A correction wakes the pending
video schedule. Polling stops when the channel closes. Older publishers ignore
the request and retain previous behavior; both endpoints need the new code for
this correction. Relays do not request/forward upstream clock observations into
their different clock domain, so the existing relay path remains unchanged.

The actual local player with this fix repeated the same 1000 ms delayed join.
The test queue held 51,765 bytes, receiver timedelta was 1001 ms, and settled
video delay was 9644 ms. After removing the delivery delay, a later 75-sample
window measured median 8644 ms (8630-8678 ms), timedelta approximately zero, and
no active rebuffering. The extra second was removed without manual offset changes
or reconnect. Persistent one-way path delay is still inseparable from clock skew
using these samples; this fix removes excess initial delay once fresher samples
arrive. It does not promise zero-offset calibration on arbitrary networks.

The native fixes are in this PR; this receiver fix and the earlier VDO-specific
changes remain in the separate webrtc.js checkout for Steve's alpha deployment.
The first displayed near-live frame during buffer fill remains a limitation, so
these concrete fixes are not a guarantee of synchronized first-frame presentation.

Both complete native soak modes passed after the recording-duration fix,
including the startup regressions and reconnect checks. The near-boundary client
started at 95% of the first client's interval. All 16 helper tests and the embedded
asset check passed; Release standalone and VST3 builds passed. The helper suite uses a VDO stub;
the separate live tests above use the actual player. The jitter regression is
separate from the helper suite and passes against the edited VDO source.

These tests reproduce concrete failures matching the symptom, not Bern's exact
hardware or internet connection. Physical cameras, remote networks, and first-frame
presentation during buffer fill still need real-world testing. NINJAM must collect
an interval, establish timing, and fill video buffering. Residual video delay
was approximately 0.08-0.25 seconds in advancing live runs. Green sync currently
confirms timing exchange, not a measured audiovisual lock.

The approved VDO edits are limited to chunked/WebCodecs sections in ../webrtc.js.
The PR keeps the native fix independent from the separately deployed VDO code.
Review these fixes with their stated intent and limits; they are not a universal
startup cure.
