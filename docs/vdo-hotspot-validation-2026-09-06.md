# Hotspot and playback validation — 2026-09-06

## Result

Reproduced a nearly live first frame during buffer fill and temporary video stalls
after a receiver reload, a delivery interruption, and a live tempo change.
Did **not** reproduce a persistent one-BPI-cycle-early video offset in the tested
hotspot-on/off sessions. These results do not establish that the reported fault is fixed.

The strongest new finding is a tempo-transition mismatch: native code publishes
the new video delay while previously recorded audio still plays with the old
delay. The transition also refreshes video buffers. Actual video froze temporarily
and recovered; it was not merely a misleading numeric diagnostic.

## Test environment and measurement

- Native source: `f6e7ef6a` (merged PR #9), rebuilt integration executable.
- Two real processors, independent generated audio, a private loopback NINJAM
  server, 48 kHz / 480-sample audio callbacks. The first 120 BPM hotspot run
  generated audio in only alpha; subsequent runs measured both directions.
- Production `launchVideoSession()` opened the normal Windows Chrome helpers.
  No in-app browser or stub VDO player was used for these live measurements.
- Chrome 152; actual deployed `https://vdo.ninja/alpha/`, loaded
  `webrtc.js?ver=960`. The loaded script included receiver-clock correction,
  publisher-origin calibration, and chunk decoder-latency reporting.
- Auto selected AV1, 720p30, 2500 kbps target, indexed chunks/NACKs,
  `chunkchunksize=4096`, `chunkedbuffer=500`, adaptive receiver buffering disabled.
- Existing `tests/live-video-probe.js` replaced camera acquisition with timestamped
  canvas streams. Frame age was read back from actual rendered remote pixels.
  All encoding, network transport, decoding, and playback remained active.
- Audio delay came from waveform correlation of source and decoded output.
  Accepted windows had normalized correlation around 0.67–0.71.
- The 120 BPM hotspot connection was independently verified through selected ICE
  candidate statistics: relay at both ends, local `relayProtocol=tls`, TURN on
  port 443 with `transport=tcp`. Other cases used the corresponding native launch
  options; their selected ICE pairs were not separately archived.

Both native clients ran on one machine. This does not measure physical camera,
audio-device, display scanout, geographically separated NINJAM paths, or carrier
behavior. BrowserStack was not used; this pass focused on the app's native runtime.
The executable's cached display-version label remained `v0.11.1.1`; source identity
above, rather than that label, identifies what was tested. The users' exact
downloaded release binary was not tested.

## Settled measurements

Video values are medians of 100 samples over 20 seconds. Audio values are rounded
medians of ten correlated windows near the corresponding settled period.
Positive residual means video is later than audio.

| Case | Direction | Audio delay | Video age | Residual |
| --- | --- | ---: | ---: | ---: |
| Hotspot on, 120 BPM / 16 BPI | alpha → bravo | 8620 ms | 8685 ms | +65 ms |
| Same, receiver helper reloaded | alpha → bravo | 8620 ms | 8677 ms | +57 ms |
| Hotspot off, 120 BPM / 16 BPI | alpha → bravo | 8586 ms | 8617 ms | +31 ms |
| Hotspot off, 120 BPM / 16 BPI | bravo → alpha | 7414 ms | 7436 ms | +22 ms |
| Hotspot on, 90 BPM / 16 BPI | alpha → bravo | 11286 ms | 11356 ms | +70 ms |
| Hotspot on, 90 BPM / 16 BPI | bravo → alpha | 10048 ms | 10105 ms | +57 ms |
| Hotspot on, after 90 → 150 BPM | alpha → bravo | 7014 ms | 7057 ms | +43 ms |
| Hotspot on, after 90 → 150 BPM | bravo → alpha | 5785 ms | 5852 ms | +67 ms |

The two directions need not have identical delay: their recording/playback phases
differ. Comparing each video against its own correlated audio avoids mistaking
that difference for a sync error.

## Reproduced transient failures

### Startup and receiver reload

After a real outer-helper reload and camera rejoin, the first remote frame was
only **210 ms old**, despite an approximately 8.6-second audio delay. That frame
was held for about **8.4 seconds** while buffering filled. Playback then advanced
at median 8677 ms. Native timing exchange remained healthy across the reload.
A green timing indicator is therefore not a first-frame audiovisual-lock check.

### Delayed delivery and interruption

On the hotspot-off run, the existing delivery probe imposed 500–1000 ms ordered
delay on actual outgoing chunk messages. Counters verified queued data (7157
queued / 7101 delivered, 45,756 bytes pending). Settled video stayed near 8618 ms.

A subsequent 12-second delivery pause held up to **626,330 bytes**. The queue
drained completely after resumption (8042 queued / 8042 delivered at the check).
Video froze, reached **16,827 ms** frame age, and automatically recovered near
8619 ms. This is application-message delay/pause testing, not UDP-loss emulation.

### Live tempo change

The private server accepted 90 → 150 BPM at 16 BPI. Native snapshots showed
bravo's published video delay change from **11283 to 7016 ms** at Unix time
`1788739326884`. Correlated audio was still approximately **11280 ms** delayed
at `1788739330438`, then reached 7014 ms at `1788739332440`.

Video frame age peaked at **20125 ms** at `1788739334432`; audio measured
**7015 ms** at `1788739334439` (correlation 0.6788). That is approximately
**13.1 seconds of temporary video lateness**. The other direction also stalled,
peaking at 17691 ms. Both directions subsequently returned to the settled values
in the table without a manual offset adjustment or reconnect.

Code inspection matches the early target update: `PluginProcessor.cpp` around
line 23057 immediately adds the interval-duration difference to each firm delay,
requests refreshes, and invalidates measurement state. Receipt of the peer's
`videoTimingChange` also requests a refresh around line 18565. The helper's
existing refresh flow flushes/reloads playback. These paths warrant a focused
follow-up: coordinate delay transitions with the corresponding audio playback
boundary and avoid redundant refills. No production fix or causality-isolating
variant was applied in this validation pass.

An earlier tempo-change attempt was rejected because voting was disabled in the
test server. It remained at 90 BPM and is excluded from the transition result.
The harness now explicitly enables voting.

## Reproduction and artifacts

The test harness now accepts `--hotspot` and `--bpm=90`, generates distinct audio
in both directions, and accepts a live tempo vote through `test-results/live-bpm`.
All additions are confined to tests and this report.

```powershell
cmake --build build --config Release --target NINJAMplus_SyncSoakTest --parallel 4
& ./build/NINJAMplus_SyncSoakTest_artefacts/Release/NINJAMplus_SyncSoakTest.exe "$PWD/build/ninjamsrv/Release/ninjamsrv.exe" --live-vdo --hotspot --bpm=90
# In a second terminal, after joining the two generated cameras:
Set-Content test-results/live-bpm 150
# End the live run:
New-Item -ItemType File -Force test-results/live-stop
python tests/analyze-live-sync.py --source alpha
python tests/analyze-live-sync.py --source bravo
```

Install `tests/live-video-probe.js` in each VDO iframe's main execution context
before selecting the camera, as described in `vdo-sync-startup-validation.md`.
For a historical audio window, add `--end-ms <Unix-milliseconds>` to the analyzer.

Local ignored evidence is under `test-results/`: `hotspot-120-run/`,
`direct-120-run/`, `hotspot-90-run/`, and `hotspot-tempo-run/` hold raw audio and
analyses. Named `hotspot-*`, `direct-*`, and `tempo-*` JSON files hold pixel samples
and chunk statistics. `hotspot-tempo-enabled-native.jsonl` records both native
helpers through the accepted tempo change. `tempo-transition-audio-*.json`
contains audio measurements at the video-stall peaks.

Validation: rebuilt native target; standard and near-boundary native soaks passed;
all 16 helper tests and embedded-asset check passed; all four separate VDO source
regressions passed. Live test processes and camera tabs were closed afterward.

## Subsequent real participant test

Connected the local standalone to the user-specified public server with Bernd,
190 BPM / 16 BPI, hotspot enabled. The original executable had a stale
v0.11.1.1 label. After fetching origin, HEAD remained the PR #9 merge and its
tree matched v0.11.1.3. Reconfiguring and rebuilding corrected the running
version to v0.11.1.3. Added opt-in local diagnostic control, documented in
`standalone-diagnostics.md`, and verified connect, hotspot, video launch,
recording start/stop, status, and successful Release standalone build.

Bernd's camera eventually appeared in the app-launched Chrome alpha iframe.
Saved timestamped 10-fps video samples and native session WAVs for repeated
count-and-clap sequences under ignored `test-results/bernd-live-control/`.
Visual hand contact and audio transients occur in the same short time windows;
these samples do not reproduce a persistent 5.053-second (one interval) video
lead on this receiver. They are not proof of precise audiovisual lock.

Precision limits: video sampling is 100 ms; WAV wall-clock origin was inferred
from file creation rather than an audio callback timestamp; the remote-user
track has fewer samples than the master, and master metronome transients make
naive waveform correlation unreliable. Do not treat the exploratory correlation
script's offset as a calibrated result. The device reports 441 samples of output
latency at 44.1 kHz (10 ms), excluding any additional physical headset delay.

Recording and temporary video sampling were stopped after capture. With Steve's
authorization, OBS Virtual Camera subsequently became live in the same VDO room
to show the receiving viewpoint; its recursive preview was verified. That return
feed has another transport/buffer leg and is not an independent sync measurement.

## Actual Clumsy fault test after the live session

Ran two native clients against a private loopback NINJAM server, 190 BPM / 16 BPI,
using the app-generated Chrome/VDO alpha helper configuration with hotspot on.
Both selected ICE paths were relay candidates with `relayProtocol: tls`, using
`turns:turn-cae1.vdo.ninja:443?transport=tcp`. This run tested TURN/TCP; it did not
test TURN/UDP. Synthetic timestamp cameras replaced only media acquisition in
the VDO frames. No application message-delivery shim was installed.

`scripts/test-clumsy-sync.ps1` launched the installed Clumsy 0.3 with bounded
timeouts, logging each launch/exit/recovery. The TURN filter targeted TCP 443 at
the observed relay address, 51.222.12.223. The separate audio filter targeted only
the private loopback server port 64675. The TURN address filter can affect other
sessions using that same relay; ensure unrelated sessions are closed before reuse.
The script requires Windows elevation and refuses to run over an existing
Clumsy instance. A zero exit code alone is not evidence that packets were impaired.

Results:

| Fault | Observation |
| --- | --- |
| 300 ms lag in each direction, 20 seconds | Selected-pair RTT increased from roughly 50 ms to 1.28–1.33 seconds, confirming real impairment across both clients. Video froze, with displayed capture age eventually reaching roughly 26–27.5 seconds. Both recovered to their original delay. |
| 5% packet loss, 20 seconds | No sustained sync shift; bravo's displayed age peaked at 5847 ms versus a baseline around 5701 ms. |
| Complete TURN packet loss, 12 seconds | Peer connections restarted. Alpha had no readable remote timestamp frames for 43 seconds; inspection during that gap showed no receiving peer and only its local preview. Bravo's corresponding missing-frame gap was 8 seconds. Both eventually returned. |
| Complete private NINJAM packet loss, 12 seconds | Correlation showed real audio silence after queued audio drained. Audio playback later resumed at new capture ages: alpha-to-bravo about 5061 ms; bravo-to-alpha about 5044 ms. Video followed the new delay, after a settling period. |

The last ten audio correlation windows had median displayed-video-minus-audio
offsets of +46 ms and +54 ms respectively. The largest matched sample was +191 ms;
the other direction ranged +42 to +57 ms. Positive means video was later than
audio. This includes Chrome's actual displayed frame age, rather than assuming
the requested buffer equals actual playout. It excludes physical audio device
and display scanout latency because the native harness writes decoded audio
directly to disk.

This reproduces slow recovery and freezes, **not** Bernd's persistent one-BPI
video lead. Alpha's delayed TURN recovery overlapped the subsequent audio fault,
so its complete recovery timing must not be attributed solely to TURN. The
30-second recovery window was insufficient in that case. Early frames after
reconnection were briefly near-live while buffers filled. A green sync exchange
indicator is not proof that a decoded frame is currently aligned with audio.
No speculative sync algorithm change was made from this run.

Evidence: `test-results/clumsy-events.jsonl`, `clumsy-native.jsonl`,
`clumsy-alpha.json`, `clumsy-bravo.json`, `clumsy-summary.json`, final
`live-audio-analysis-*-to-*.json`, and the raw `live-*-{input,output}.f32` with
callback clocks. Run `python tests/analyze-clumsy-sync.py` for per-case frame ages.
The prior application-delivery-stall run is separate under
`test-results/hotspot-190-before-clumsy/`; it must not be described as packet loss.

All four Clumsy processes exited with code 0, the runner completed, and no Clumsy
or native test process remained after cleanup. Both synthetic camera tabs closed.
The two passive-observer regression tests and `git diff --check` passed.
