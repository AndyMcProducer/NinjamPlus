# Native audio / VDO end-to-end test

The updated alpha did not reproduce a persistent four-bar video lead in these
runs. It did reproduce recurring video freezes and late pictures, including on
a clean TCP connection with long NINJAM intervals. Reliable recovery therefore
does not pass this test.

## Setup

- Two real native processor instances, a private loopback NINJAM server, generated
  audio recorded before encoding and after remote decoding, and independently
  timestamped generated video. No physical microphone or camera.
- Installed Chrome, fresh profiles, actual embedded `/buffer-room` and `/app`
  helpers, deployed alpha scripts without response overrides.
- Loaded `webrtc.js?ver=961` SHA-256:
  `4961457ea69b2b3e9c77f81a22c64dec34aca0abcfd35069517cac04ea1d34fb`.
- Actual selected TURN/UDP on port 3478 for the first run; TURN/TLS over TCP on
  port 443 with native hotspot mode enabled for the second.
- Clumsy ran bounded UDP faults: 20% loss / 300 ms lag / 500 ms variable hold;
  40% loss / 800 ms lag / 800 ms variable hold; and a 12-second combined video
  and native-audio outage. Each had 45 seconds of recovery. ICE changed relay
  during outage recovery, so the targeted relay filter did not block every
  possible fallback path.

## Results

The settled UDP baseline measured video 61 ms and 53 ms later than independently
correlated audio (7 and 6 samples). After the loss tests, some advancing windows
returned near audio timing, but freezes recurred even during clean recovery.
One outage-recovery window included advancing pictures about five seconds late.
It is not valid to report the whole recovery phase as synchronized.

Reloading one helper restored media. Changing 190 to 120 BPM changed the interval
from 5053 to 8000 ms; native GUID measurements and video followed that change.
In a final 30-second window, only 4/14 and 5/14 audio observations had matching
advancing video with a fresh source. Those matched medians were +12 and -72 ms,
but the low advancing counts are a failure of continuous playback.

The separate TCP/hotspot run changed BPI from 16 to 8 to 32 at 120 BPM, producing
8000, 4000, and 16000 ms intervals. Native GUID measurements followed all three.
At BPI 32, pictures sometimes exceeded 30 seconds of age while source preview
age remained tens of milliseconds. This is late/frozen video, not a demonstrated
one-interval early-video fault.
Independent waveform comparison at BPI 8 gave matched medians of +99/+75 ms
(8/9 and 9/9 advancing observations). At BPI 32, only 2/13 and 1/13 observations
had advancing pictures with fresh sources; timing medians from so few samples
must not be used to claim that the long-buffer case passed.

The UDP decoder trace provides a concrete investigation target: after tempo
changed, `playout_drops` jumped by roughly an entire buffered queue, decoded
output stopped while the queue refilled, and presentation resumed near target.
For example, alpha's drop count rose from 1047 to 1266; a fresh source continued
while picture age grew from about 8 to 15.8 seconds. The current receiver's
late-frame path can skip to a much newer keyframe, discarding the buffered GOP.
This supports investigating scheduler catch-up / GOP recovery before assuming
an extra hidden video-element buffer. The exact trigger needs an isolated
regression before changing receiver behavior.

These are same-host tests of digital native output and browser pixels, not
physical audio/display measurements. No claim is made that Bernd's original
four-bar lead is resolved. Heavy audio correlation was run partly alongside the
UDP recording; repeating with all analysis offline would further isolate load.
The TCP BPI 32 stalls also occurred after that analysis had completed.

## Repeat

Build `NINJAMplus_SyncSoakTest` with integration tests enabled, then run:

```powershell
build/NINJAMplus_SyncSoakTest_artefacts/Release/NINJAMplus_SyncSoakTest.exe build/ninjamsrv/Release/ninjamsrv.exe --live-vdo --bpm=190
```

Use its printed room and helper ports:

```powershell
node tests/run-native-vdo-browser.cjs --room=ROOM --alpha-port=8001 --bravo-port=8002 --transport=udp --output=test-results/NEW-RUN
```

For TCP, add `--hotspot` to the native command and use `--transport=tcp` in the
browser runner. Use a new output directory for each Chrome profile.

The browser runner accepts `command.json` in its output directory:
`{"action":"reload","label":"bravo"}` or `{"action":"stop"}`. Write a BPM to
`test-results/live-bpm`, or BPI to `test-results/live-bpi`; the native fixture
votes from both clients. Create `test-results/live-stop` to stop native capture.
The native harness bounds itself to 15 minutes and the browser to 10 minutes.
Archive `live-*.f32`, `live-*-clock.txt`, and native diagnostics before another run.

Use `scripts/test-clumsy-sync.ps1` with the observed relay address/protocol/port
and native server port; WinDivert requires administrator elevation. The runner
refuses another running Clumsy instance and supports `test-results/clumsy-stop`.

After capture, run `tests/analyze-live-sync.py --root=RUN --source=alpha` and the
same for bravo; choose `--window-seconds` to cover the run. Then run
`tests/analyze-native-vdo.py --root=RUN` with optional `--from-ms` / `--to-ms`.
Only advancing pictures with fresh sources contribute to timing summaries;
advancing does not itself mean correctly synchronized. Frozen and absent
pictures remain in the detailed results. The analyzer's frozen-picture exclusion
has a regression test: `python tests/test-native-vdo-analysis.py`.

Artifacts are in `test-results/e2e-alpha-udp/` and
`test-results/e2e-alpha-tcp-bpi/`. Clumsy, native clients, and owned test Chrome
instances were stopped after capture. No production receiver change was made
from these observations.

Follow-up isolated a 600 ms scheduler pause causing 479-frame discard from an
intact 16-second buffer. A local parent VDO patch allows bounded catch-up up to
one second for long-buffer streams. Real Chrome pause tests passed over TCP and
UDP; a native 16-second-interval run then recorded 20/20 advancing observations
per direction in its final 40 seconds, with +44/+59 ms median video/audio error.
One early large discard still occurred. See the parent repository's
`tests/CHUNKED-SCHEDULER-REVIEW.md` for reproduction and limitations. This patch
is local, with cache version 962 prepared; it has not been deployed to alpha.

After Steve deployed alpha version 962, fresh-profile tests without overrides
passed all six deliberate stalls across TCP/UDP. The native 16-second-interval
repeat measured +93/+95 ms median video/audio difference in the final 40 seconds
(20/20 and 19/20 advancing fresh-source observations). The initial large discard
remains. Loaded script hashes and raw evidence are in `test-results/deployed962-*`.
