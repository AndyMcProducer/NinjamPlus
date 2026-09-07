# Automated capture-clock regression

The updated alpha includes the shared monotonic publisher clock fix. Its loaded
`webrtc.js?ver=961` SHA-256 is
`4961457ea69b2b3e9c77f81a22c64dec34aca0abcfd35069517cac04ea1d34fb`.

Run from NinjamPlus after installing its npm dependencies:

```powershell
npm run test:live-clock -- --transport=tcp
npm run test:live-clock -- --transport=udp
```

The test opens installed Chrome with an isolated profile, publishes generated
timestamp video through actual alpha WebCodecs and TURN, and reads timestamps
from the receiving video/canvas pixels. No physical camera or microphone is
used. It does not change the system clock. Close the test Chrome to abort.

A viewer stays connected to preserve the encoder while the publisher page's
wall clock jumps by +1890, +5053, and -5053 ms. New viewers join after each jump;
one also reloads. Capture barcodes and source-freshness measurements retain an
independent clock so injected wall-clock jumps cannot contaminate the reference.
Advancing pictures and a fresh source are required. Connection state or a green
buffer indicator alone cannot pass. The selected TURN protocol is checked.

The target is 5000 ms. Each case needs at least 25 advancing pixel observations,
an absolute age within 750 ms of target, and a change within 350 ms of baseline.
These tolerances detect second-scale errors; they are not a frame-accuracy claim.
JSON artifacts include cases, pixel samples, source freshness, network telemetry,
and hashes of loaded scripts under `test-results/clock-browser-*`.

For a before/after comparison, substitute a saved old script:

```powershell
npm run test:live-clock -- --transport=udp --webrtc-source=test-results/alpha-update-validation/deployed-alpha-webrtc.js
```

The saved old build is a local test artifact, not part of the repository. That
negative control must exit with a test failure. On September 7 it reproduced a
1893 ms lead: baseline picture age 5196 ms, then 3303 ms after the +1890 ms jump.
The updated alpha TCP/TURN run passed all five cases at 5187–5196 ms.
The updated alpha UDP/TURN run also passed all five cases at 5178–5203 ms.
Artifacts: `clock-automated-old-udp`, `clock-automated-alpha-tcp-2`, and
`clock-automated-alpha-udp` under `test-results/`. The 18 helper tests and embedded
asset consistency check passed after adding this runner.

The minimal fix keeps encoder capture origins, new-channel headers, and clock
replies on the same session-wide monotonic clock in the parent `webrtc.js`.
Adding one NINJAM interval to the requested buffer would hide one symptom while
misaligning healthy sessions. The deterministic companion test is
`node ../tests/chunked-clock-regression.cjs`.

This isolated test does not run native NINJAM audio, inject packet loss, measure
physical display latency, or establish that Bernd's original four-bar lead had
this cause. Earlier native waveform comparisons are documented in
`../tests/CHUNKED-CLOCK-REVIEW.md` in the parent VDO repository. Forced browser
playout backlog is covered separately in `vdo-presentation-stress-2026-09-07.md`.
