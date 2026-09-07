# Startup refresh preserves the NINJAM delay

The startup discard was caused by the helper's refresh routine, not an observed
Chrome scheduling stall. `scheduleBufferRefresh` sent a zero-buffer command,
requested refresh, and restored the calculated delay 150 ms later. A receiver
holding 15–16 seconds of valid video immediately discarded that queue when
asked for live playback, then waited for a full refill after the target returned.

Direct tracing at the real receiver's discard branch confirmed `target: 0` in
both directions. It discarded 447/480 frames with ages 15224/16491 ms. The call
stack led through the `setBufferDelay` iframe API and `playoutdelay`, not a timer
stall. Initial polling missed this because the target was restored between
snapshots. The browser recorded no long task explaining the event.

The helper now keeps the current delay during refresh, then applies the latest
calculated delay. Latency-estimate reset, refresh messages, debounce and duplicate
event handling remain. Explicit voice-chat mode can still request zero delay.
This fix is in NinjamPlus's embedded helper; updating VDO alpha alone does not
install it. The local standalone and native harness were rebuilt with the helper.

The existing helper test was extended to reject any zero-buffer pulse during
refresh, verify that a changed 16000 ms target applies, and preserve intentional
voice-chat zero. The old helper failed; the fixed helper and all 18 tests pass.

Live evidence uses installed Chrome, two native processors, generated audio and
timestamped video, 60 BPM / 16 BPI, and TURN/TCP hotspot mode. Alpha remains the
deployed version 962 without source overrides for fixed validation. Before-fix
diagnosis used a test-only instrumented receiver response to record the discard
decision, plus read-only scheduler/message traces. No production VDO source was
changed during this investigation.

Artifacts: `test-results/startup-scheduler-before`, `startup-wake-before`,
`startup-order-before`, `startup-discard-trace`, and `startup-refresh-fixed`.
Some cold/warm runs did not reproduce the flush, so it is the explicit refresh
event—not merely opening a page—that defines the deterministic regression.
This addresses startup/refill freezes; it does not establish that Bernd's
original persistent four-bar lead had the same cause.

Fixed live validation recorded no zero-buffer commands after a positive target
and no `playout_drops` in either direction, including a helper reload while
native audio continued. Independent waveform/pixel comparison: cold settled
windows had 12/12 advancing fresh-source samples per direction, medians +94.5
and +114 ms; post-reload windows had 14/14 per direction, medians +91.5 and
+68.5 ms. Maximum sampled absolute errors were 553/449 ms cold and 347/391 ms
after reload. These are sampled digital-output observations, not physical
display/audio guarantees. Clients were stopped before waveform analysis.

Rebuilt standalone: `build/NINJAMplus/Release/Standalone/NINJAMplus.exe`, SHA-256
`CB0A48BDA66F7C1F759C1856841765F3597DA3A23FE21DEEF53D8D22138535D8`.
Both the native integration target and standalone build succeeded; embedded
asset consistency and all 18 helper tests passed. No new alpha deployment is
needed for this helper fix; users need the updated NinjamPlus build.

Subsequent [network recovery validation](vdo-network-refresh-validation-2026-09-07.md)
kept the zero-delay regression fixed but exposed repeated playback freezes.
The combined stress test did not pass; the staged package has not been released.
