# PR 10 review

Reviewed the native GUID matching and delay filter changes, opt-in standalone
diagnostics, camera recovery/health handling, embedded assets, and test tooling.

## Confirmed and fixed

**False healthy camera after an early disconnect.** A stream roster can arrive
before its owner appears in the native interval feed. If `push-connection:false`
then removes that stream, its cached announcement survived. Later native
discovery replayed the announcement and showed `video connected` with a green
indicator although the camera had disconnected. The new test failed on the PR
head with exactly that state. Removal now clears the cached announcement and
starts the existing missing-stream retry timer for previously resolved streams.
Both roster removal and explicit disconnect cases pass, along with all 19 helper
tests and the analyzer regression.

**The long control test could not run for 15 minutes.** The browser companion
had a hardcoded 10-minute deadline and always enabled full instrumentation.
It now accepts `--duration-seconds=900` and `--diagnostics=minimal|full`.
The native harness accepts a longer watchdog to allow browser startup and a
complete measurement window. Minimal mode keeps generated input, pixel age,
source freshness, and network observation but omits decoder/presentation and
scheduler wrappers. Both modes use the same Chrome launch configuration.

The rebuilt native harness and delay-filter regression passed. This review does
not validate all physical device configurations, or resolve the previously
recorded playback freezes. The no-loss comparison remains separate runtime
validation; a green indicator alone is not proof of synchronized presentation.
