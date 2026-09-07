# Recovery and independent frame-identity validation

The helper previously retained camera mappings after an empty VDO roster. Removing
the last camera also resurrected its legacy primary target. Both inner and outer
lights could remain green despite absent video. The helper now reconciles the
complete roster, clears the primary target, and propagates missing/buffering
video health to the outer light while retaining the native transport label.

A live installed-Chrome test then exposed a second failure: after `closeRPC`,
`requestStream` returned false because `waitingWatchList` retained the absent
stream. The compatibility recovery uses the existing helper eval/API bridge to
clear only that absent, previously advertised stream's pending flag, followed by
`requestStream`. It waits 15 seconds and retries at most every 30 seconds while
the native user remains active. It does not restart camera capture.

The presentation observer now reads the synthetic source barcode at the final
writer and from the video element. Analysis rejects unknown identities and
repeated content, rather than selecting a timestamp match or guessing an offset.
This is a diagnostic test fixture, not an identity signal available in arbitrary
unmarked video. Canvas reads and callbacks are not physical monitor scanout.

In the first live timestamp-shift run, all 14 observed frames matched by content
while element timestamps were exactly 4000 ms behind writer timestamps. Median
writer-to-callback delay was 19 ms; median writer-to-expected-display time was
30.95 ms. No persistent four-second presentation queue was observed. Callback
coverage was low; this is evidence about those observed frames, not every frame.

Artifacts: `test-results/recovery-identity-fixed/`. Two clients run on one Windows
host with private synthetic audio/video, installed Chrome, and the application's
native helper URLs. The updated pending-state compatibility fix is served at the
same native helper origins via a test route override until the final rebuild.
Cross-machine validation remains outstanding.

The deterministic stale-watch test dropped one watch request for ten seconds and
left its pending flag set, matching the observed failed state. At 17.83 seconds
after receiver removal, the helper's request returned true; advancing video
returned without reloading. The initial unfixed attempt returned false and
remained without a receiver. `targeted-recovery-log.json` records the accepted
retry and the bounded fault's restoration.

The repeat TURN/TCP suite used the selected TLS relay at 51.222.12.223:443:
20% loss + 300 ms lag + 500 ms random holds; 40% loss + 800 ms lag +
800 ms random holds; then a 12-second combined NINJAM/video outage. Both
receivers recovered after the loss stages. The outage required longer than the
45-second nominal recovery window on alpha: VDO removed the failed receiver,
and at 1788752668855 the helper's stream request was accepted (53.54 seconds
after faults stopped). At 1788752686724 both sides again displayed fresh frames
about 4.95-4.97 seconds old. No manual reload occurred during the TCP suite.
This validates eventual recovery, not rapid recovery under severe outages.

The clean TURN/UDP writer-overload repeat held eight seconds and released 200
frames without waiting for backpressure (200 pending writes). In the following
25.4-second observation window, all 35 observed presentation callbacks matched
independent content identities. Median writer-to-callback time was 22 ms and
median writer-to-expected-display time was 34.7 ms. Frame age temporarily rose
above ten seconds during recovery, then returned near five seconds. Sparse
callback delivery limits coverage; this does not prove every submitted frame was
presented, nor measure physical scanout. No persistent extra interval was seen.

## Final measurements and checks

Audio was independently matched from generated source waveforms to decoded
NINJAM output in both directions (5-second sampling). Comparing advancing video
pixel timestamps with nearby high-confidence audio matches, the last ten seconds
of the TCP/UDP loss recovery windows had median video-minus-audio offsets of
+31 to +44 ms. In the later TCP outage recovery window, medians were -36 and
-94 ms. These are recovery-window medians, not worst-case bounds: stalls and
large transient delays occurred during faults. The native interval was 5052.6 ms.
`recovery-summary.json` contains per-direction values and sample counts.

Checks passed: 18 Playwright helper tests; four independent-identity analyzer
regressions; two existing timing analyzer regressions; native latency-filter
replays; embedded asset consistency; Release standalone and soak-test builds;
waveform correlation in both directions; `git diff --check`.

Final standalone SHA256:
`DB5135036B0FF4269EC2E9659F0D00DC777084EBDC1DE4A54EBF5976001B6F2D`.
Both `NINJAMplus.exe` and `NINJAMplusDiagnostics.exe` in
`build/NINJAMplus/Release/Standalone/` contain the final helper changes.
Clumsy completed both bounded suites and is stopped. The private native test,
generated cameras, and dedicated debugging browser are closed.

Remaining limits: no persistent four-bar lead was reproduced; cross-machine
validation requires access to the second test client. A green indicator still
means the calculated buffer and available video-health evidence are healthy;
it is not independent proof of physical audio/video synchronization. The severe
TCP outage still takes roughly a minute to recover in this run.
