# Severe network testing, September 6, 2026

## Method

Two native NINJAM clients and two app-configured Chrome VDO alpha frames run in
a private room at 190 BPM, 16 BPI (5052.6 ms per interval). Native audio contains
independent random signals. Correlation measures capture-to-decoded-audio delay;
camera pixels encode capture wall time. Read-only observers record selected ICE
pairs, sender preview age, decoded frame output, and video presentation metadata.
These tests exclude physical audio-interface latency and monitor scanout.

The UDP run retained native hotspot sync behavior but removed the helper launch
URL's `tcp` parameter. Both sending and receiving candidate pairs reported
`relayProtocol: udp`, `candidateType: relay`, and
`turn:turn-cae1.vdo.ninja:3478?transport=udp`. All recorded selected relay transports
remained UDP. Missing peer entries during reconnection are not protocol evidence.

Clumsy's UDP filter was:

```
udp and (ip.SrcAddr == 51.222.12.223 or ip.DstAddr == 51.222.12.223) and (udp.SrcPort == 3478 or udp.DstPort == 3478)
```

The combined cases additionally matched only the private NINJAM loopback TCP
port. Unrelated internet traffic was not intentionally impaired. This simulates
bad end-to-end conditions for the test session, not a geographically distributed
deployment or the whole machine's network. Because both clients share the host,
the configured per-direction delay can be traversed several times on a round trip.

`scripts/test-clumsy-sync.ps1 -Profile severe` runs:

- 20% packet loss for 25 seconds, then 35 seconds recovery.
- 40% loss + 250 ms lag + up to 400 ms batch holding for 25 seconds; 45 seconds recovery.
- 5% loss + 1000 ms lag + up to 800 ms batch holding for 20 seconds; 45 seconds recovery.
- Six consecutive six-second conditions affecting audio and video together:
  lag 50/400/900/1500/100/500 ms, loss 3/15/40/80/5/25%, holding 100/400/800/1000/200/600 ms;
  then 60 seconds recovery.
- Combined 15% loss + 300 ms lag + 500 ms holding for 25 seconds; 60 seconds recovery.
- Combined 100% loss for 20 seconds; 75 seconds recovery.

Holding uses Clumsy's throttle module at 35% trigger probability; batches are
released together. This creates variable packet delay/bursts, not a normally
distributed jitter model. Each phase is a new timed Clumsy process, so queued
packets/transport behavior at phase boundaries are also part of the test.

## Completed UDP result

The impairment reached the real UDP media path: selected-pair RTT peaked at
6.737 seconds. Video froze under severe loss, with roughly 24-second-old displayed
frames during the first two cases. Both directions recovered from the media-only
faults without a sustained whole-interval shift.

The following values compare independent audio correlation with advancing video
frames during the final ten seconds of each recovery period. Positive is video
later than audio. Most cells contain five measurements; the -188 ms cell has three.

| Recovery after | Alpha to bravo median offset | Bravo to alpha median offset |
| --- | ---: | ---: |
| 20% loss | +56 ms | +49 ms |
| 40% loss, lag and jitter | +63 ms | +48 ms |
| 1000 ms lag and jitter | +60 ms | +49 ms |
| Six changing combined conditions | +632 ms | +69 ms |
| Combined 15% loss, lag and jitter | +408 ms | -188 ms |
| Combined 20-second outage | +67 ms | +70 ms |

This reproduces prolonged recovery error. It does not reproduce a persistent
5053 ms video lead. The changing conditions altered audio playback phase. In one
direction, route compensation remained 836 ms shortly after recovery and decayed
to 48 ms over about 50 seconds. Raw GUID-matched delay approached the measured
3826 ms audio age, while the smoothed estimate lagged: 4629, 4447, then 4261 ms.

The relevant native implementation uses a nine-reading median, then an 88/12
exponential average, and waits for eight readings plus a >200 ms difference before
republishing an established buffer. At this tempo readings arrive roughly once
per five seconds. These layers explain a confirmed part of the delayed convergence;
they do not establish the cause of every freeze or Bernd's original report.

## Browser buffering check

Reading a video's pixels with `drawImage` is stronger evidence than a configured
buffer value, but it is not the same as observing the compositor or physical display.
Therefore three actual Chrome screenshots were also saved and their remote-camera
barcodes decoded. Settled video capture-age bounds were 5012–5218, 5060–5215, and
5051–5239 ms, bracketing screenshot-call timing. Independently measured audio was
approximately 5052 ms. This check found no hidden multi-second compositor backlog
at that settled point. It does not rule out additional browser delay during faults.

The frame callback's `expectedDisplayTime` sometimes differed from callback timing
by about 1.7 seconds even when the pixel/compositor checks were near 5.1 seconds.
That metadata needs further clock-domain validation before treating it as an
additional physical playback queue. Do not add it blindly to buffer compensation.

## TCP comparison validity

The first severe TCP attempt is stored separately as
`test-results/clumsy-tcp-severe-confounded/`. Its receiver stalled before the first
Clumsy launch: the displayed frame was already 28–31 seconds old at launch while
network RTT remained under 70 ms. Decoded output stopped, the receiver queue emptied,
and it waited for a keyframe. Offline waveform analysis was running concurrently;
whether that load caused the stall has not been established. This run cannot
support a clean causal TCP-versus-UDP comparison. A clean TCP repeat uses fresh
pages and no concurrent offline waveform analysis.

The clean repeat completed all eleven phases. Its pre-fault video ages stayed
4492–4528 ms and 5689–5749 ms over the last ten seconds before Clumsy launched.
Sender-preview capture ages stayed low throughout the fault sequence: 95th
percentiles 48/43 ms, maxima 232/207 ms. Thus the large receiver freezes in this
repeat were not stale synthetic source pictures. All selected relay transports
reported TLS/TCP, with the filter targeting TCP 443 at the same relay. Selected-pair
RTT peaked at 19.205 seconds. The largest readable frame age was 47.123 seconds,
and the longest missing-timestamp gap was about 39.4 seconds.

| TCP recovery after | Alpha to bravo median offset | Bravo to alpha median offset |
| --- | ---: | ---: |
| 20% loss | +27 ms | +48 ms |
| 40% loss, lag and jitter | +35 ms | +53 ms |
| 1000 ms lag and jitter | +24 ms | +53 ms |
| Six changing combined conditions | +32 ms | +260 ms |
| Combined 15% loss, lag and jitter | +37 ms | +61 ms |
| Combined 20-second outage | +61 ms | +40 ms |

Each cell uses five advancing-frame/audio comparisons in the final ten seconds
of its recovery period. Do not rank transports statistically from one stochastic
run: the fault sequence is shared, but Clumsy's individual random packet decisions
and reconnection timing differ.

Actual Chrome screenshots after TCP dynamic recovery decoded to video age bounds
7341–7470, 7372–7442, and 7344–7456 ms. Independent audio correlation around this
period was about 7358 ms. These are consistent with the then-current 7351 ms
target, not an extra several-second compositor queue.

The video-element probe detected a 197 ms-old frame at initial startup while
audio was 5657 ms old. After dynamic reconnection it also detected a 210 ms-old
frame while audio was 7798 ms old. These were brief startup/refill events, not a
persistent whole-interval shift. Because a pixel read can see a frame beneath a
buffering overlay, their visibility to a human is unverified; there is no matching
compositor screenshot at those exact instants. This is a follow-up test target,
not proof that Bernd saw those same events.

## Evidence and reproduction

Complete UDP artifacts are in `test-results/clumsy-udp-severe/`, including raw
audio, callback clocks, native GUID measurement provenance, helper snapshots,
browser pixel/frame/network traces, Clumsy phase times, compositor screenshots,
and `network-stress-summary.json`. Initial lighter TCP results moved to
`test-results/clumsy-tcp-initial/`.
The completed clean TCP repeat is in `test-results/clumsy-tcp-severe/`.

For another run, launch the native harness with `--live-vdo --hotspot --bpm=190`,
use its generated helper URLs, install the camera and observer scripts in the VDO
iframe main context before joining cameras, and verify the actual selected relay.
Use the observed relay address, relay service port and private NINJAM port:

```
scripts/test-clumsy-sync.ps1 -Protocol udp -TurnAddress 51.222.12.223 -TurnPort 3478 -NinjamPort <observed-port> -Profile severe
```

Run that command elevated, with no other Clumsy instance. The relay address filter
can affect other sessions on the same relay; close unrelated VDO sessions first.
The script refuses an already-running Clumsy instance and bounds every fault.
For subsequent runs, creating `test-results/clumsy-stop` requests cancellation;
the elevated runner can stop its own child without another elevation prompt.
This cancellation addition was syntax-checked after the completed live runs;
its cancellation path has not yet been exercised in an elevated live test.

After live collection stops, analyze both source directions with
`tests/analyze-live-sync.py --root <run-directory> --source alpha|bravo --window-seconds 900 --step-seconds 2`.
Then run `tests/analyze-network-stress.py --root <run-directory>` and
`tests/analyze-clumsy-sync.py --root <run-directory>`. Missing video and silent audio
are reported separately; neither is a valid sync measurement.

Cleanup: both completed suites logged success, every completed Clumsy child
reported exit code zero, test camera tabs closed, and the native test clients
stopped. The diagnostic test target rebuilt successfully, the passive observer's
two regression tests passed, and `git diff --check` passed. No sync algorithm
change was made during those live stress runs.

## Follow-up: confirmed-delay recovery change

The native delay estimator now accepts a stable change after five consecutive
readings spanning at most 150 ms, all more than 200 ms above or below its current
estimate. It uses their median and drops the older phase from its history.
Otherwise, the existing nine-reading median and 88/12 smoothing remain active.
The publication threshold and route-latency estimator are unchanged.

The native soak executable's `--filter-only` mode replays the eleven recorded UDP GUID
delay readings above, after a twelve-reading baseline at 4629 ms. Before the
change, the final estimate was 4325 ms, 499 ms above the recorded 3826 ms audio
delay. After the change it is 3922 ms, a 96 ms residual. This is a deterministic
estimator replay, not a new end-to-end network measurement. Route smoothing and
the 200 ms publication deadband can still leave residual error.

Additional regressions cover upward/downward stable changes, rejection of four
consecutive large outliers in either direction, startup outliers, ordinary jitter,
and smaller sustained changes. Five stable readings still take roughly 25 seconds
at the tested interval duration; this change removes the additional long EMA tail,
not the evidence-gathering time. Five or more consistently wrong measurements can
still be accepted; the filter cannot establish whether its input timing is correct.

The standalone diagnostic executable is rebuilt separately as
`build/NINJAMplus/Release/Standalone/NINJAMplusDiagnostics.exe`, preserving the
currently open original executable. This change does not establish or fix the
reported persistent one-cycle lead, and has not yet had a fresh Clumsy/browser
validation run. Two-machine capture remains necessary to investigate that report.

Validation also exercises the private two-client staggered join and reconnect
soak. One near-boundary run failed the old assertion that no refresh event could
appear during settling; an unchanged rerun passed. The test now checks for an
event identity persisting beyond its retention window, allowing a legitimate new
delay correction to generate a new event. The separate skipped-poll retention,
event-identity, and expiry assertions remain unchanged.

Reproduce the estimator regression without starting a server or camera:

```powershell
./build/NINJAMplus_SyncSoakTest_artefacts/Release/NINJAMplus_SyncSoakTest.exe --filter-only
```

## Whole-interval lead investigation

The initial native startup fixture compared a delayed GUID's matched and
unmatched paths at a 2000 ms interval and 3990 ms marker age. Before the fix,
the real native functions gave a matched capture-to-playback delay of 5990 ms, versus
4000 ms from the timeout fallback: a 1990 ms difference, almost one interval.
The fixture's sample origin was increased to 1000000 so a 3990 ms lookback remains
nonnegative at 48 kHz. This is an injected timing reproduction, not evidence that
the same timeout occurred in Bernd's session or an end-to-end video capture.

Previously, `processPendingIntervalSyncMarkers` waited 1.5 intervals for a GUID, then accepted
a local beat boundary and clamps the elapsed portion to one interval before
adding the recording interval. Thus fallback cannot request more than two
intervals, although GUID matching accepts up to three. The fallback also erases
the pending marker, so a later match cannot correct that particular measurement.
Simply removing the clamp is not sufficient: a guessed local boundary still
does not prove when that audio played.

### Validated late-GUID fix

The native regression first failed on the old code: an unmatched marker was
consumed by the beat fallback and could not later publish its actual 5990 ms
capture-to-playback delay. The fix excludes GUID-bearing tags from beat fallback.
They remain available to the existing GUID matcher until the existing three-
interval stale sweep expires them. Tags without a GUID retain legacy beat timing.
No arbitrary interval is added and no guessed boundary is relabeled as playback.

The regression injects three successive delayed GUIDs and checks that repeated
beat polls retain each pending marker without creating a measurement or initial
published delay; actual playback then measures each exactly once and publishes
5990 ms after three matches. It also checks expiration when audio never arrives.
The startup timeout test now expects no estimate for an unmatched GUID, while
legacy short-phase and near-boundary tests remain active.

Tradeoff: if channel-zero audio never reaches observable playback, GUID-based
sync remains unmeasured at startup, rather than inventing a buffer. A previously
established buffer is retained. The green indicator for legacy estimates and
stale established buffers is unchanged; it still must not be interpreted as an
independent measurement of audiovisual alignment. Cross-machine validation of
Bernd's particular report is still outstanding.

The regression fails before this fix and passes after it. The normal private
two-client stagger/reconnect soak passed. One subsequent near-boundary run
reported a 1177 ms alpha receiver buffer against approximately 636 ms actual
capture-to-playback timing (600 ms intervals). Two instrumented reruns passed;
further instrumentation then found an observation-cadence defect in the test:
the stable phase sampled GUID changes every 1000 ms, slower than its 600 ms audio
interval. If it skipped a GUID transition, it assigned the previous GUID to a
later completion boundary and could underestimate the comparison delay by an
interval. The test now samples audio GUIDs every 10 ms while retaining 1000 ms
helper polling. The earlier 636 ms comparator is therefore not reliable evidence
of an additional application defect. The soak logs alpha's GUID provenance and checks
absolute capture-to-playback timing in both directions, rather than allowing
the reverse direction to pass solely on a modulo-interval phase comparison.

After correcting that observer cadence, both the normal and near-boundary
stagger/reconnect runs passed with the absolute checks in both directions.
The late-GUID, startup, jitter, refresh retention/expiry, and reconnect assertions
passed; the standalone Release build, passive-observer tests, and whitespace
checks also passed. These are native/injected tests; no fresh Clumsy/browser
stress run or cross-machine session was performed for this late-GUID fix.

Both standalone filenames were rebuilt/updated to the same binary:
`build/NINJAMplus/Release/Standalone/NINJAMplus.exe` and
`build/NINJAMplus/Release/Standalone/NINJAMplusDiagnostics.exe`.

Other findings from the current source:

- GUID delay calculation rounds to a millisecond, not a beat or interval.
  Remote absolute interval counters order that sender's tags; they are not
  subtracted from local counters to calculate delay.
- The helper accepts `receiverBufferFinal` directly with millisecond rounding.
  Its repeating beat display wraps modulo BPI; that display cannot distinguish
  adjacent cycles of identical rhythmic content.
- Helper `buildSyncDiagnostics` marks the calculation OK when it has an interval
  measurement and a buffer. This does not independently establish audiovisual
  alignment. A fallback estimate can satisfy those conditions.
- The sync GUID source and remote playback lookup both use audio channel zero.
  Other channels, realtime/session modes, an external DAW's monitoring path,
  and a returned screen-share mix need separate verification before interpreting
  a comparison. No evidence currently proves that Bernd compared the wrong pair.
- The sender's completed GUID is read at the audio boundary from an identifier
  generated by the encoder/network processing path. A worker stall spanning an
  interval could make that association stale; this is an untested hypothesis,
  not a confirmed defect in the recorded live session.

For a remote reproduction, identify successive intervals uniquely (spoken
sequence numbers or a nonrepeating audiovisual reference), record the incoming
video and the same participant's received NINJAM audio on the receiving machine,
and retain the native `basis`, GUID, raw delay, and sample-boundary diagnostics.
Comparing a returned screen share with live/local monitoring is insufficient.
