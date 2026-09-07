# Standalone diagnostic control

The standalone app exposes an opt-in local JSON file interface when launched with
`NINJAMPLUS_DIAGNOSTIC_DIR` set to an absolute directory. Normal launches do not
enable it. This is a file API suitable for a local tool adapter, not an MCP server.

Diagnostic startup mutes the standalone input, disables local transmission and
monitoring, and mutes the metronome. Use a separate `NINJAMPLUS_SETTINGS_DIR` for
an isolated test configuration.

The app polls `command.json` every 250 ms and atomically replaces `status.json`.
Use one controller per directory. Commands contain `id` and `action`; publish
them by renaming a completed temporary file to `command.json`.

Supported actions: `connect` (nonempty `server` and `user`, anonymous/passwordless),
`disconnect`, `hotspot` (boolean `enabled`), `openVideo`, `recordStart`, and
`recordStop`. Recording uses the existing session recorder and creates a unique
capture directory under the diagnostic directory. `openVideo` uses the normal
application video launch path; joining a camera remains a browser action.

`commandResult: accepted` acknowledges dispatch, not completion. Inspect
`connectionStatus`, `connectionError`, and `recording` for the resulting state.
Status includes the running executable/version, BPM/BPI, hotspot flag, outgoing
audio state, remote user meters, and device sample rate/block/output latency.
The meters are low-rate snapshots and cannot establish clap synchronization.
Driver-reported output latency does not measure physical speaker/headset delay.

The `sync` object includes channel-zero playing audio GUIDs and the latest
measurement's basis (`audio-guid` or `beat-fallback`), marker receive sample,
matched playback sample, recording interval duration, route contribution, raw
delay, and smoothed delay. Empty evidence means no measurement has been observed.
These snapshots expose calculation provenance; they do not directly measure
physical output latency. Playback GUID reads and measurement reads are separate
snapshots and may straddle an audio interval boundary.

## Passive browser timing

Run `tests/live-playout-observer.js` in the app-launched VDO iframe's main context.
It observes decoder output and `requestVideoFrameCallback` presentation metadata;
it does not acquire a camera or change the transport/buffer. Export
`__njPlayoutObserver.export()` to JSON, and call `__njPlayoutObserver.stop()` to
restore writer methods and cancel timers/callbacks. Restart it after page reload.
Logs are bounded and contain timing metadata, not recorded image/audio content.

Collect the native helper's existing `/intervals` response alongside it as JSONL
objects with `at` (Unix milliseconds) and `intervals` (the response). Analyze with:

```powershell
python tests/analyze-playout-observation.py observer.json native.jsonl
```

The analyzer matches presented media timestamps to actual decoded frame timestamps,
includes the compositor's expected presentation delay, and compares estimated
frame age to a fresh native requested delay. It reports missing matches instead of
assuming sync. This can reveal browser stalls, underfill, and a retained wrong
buffer after network recovery, without anyone clapping. Sender timestamp origin
errors and native audio delay calculation errors still need independent evidence
(the native GUID trace or the generated audio/video test harness). Negative small
compositor deltas can occur because browser timing metadata is an estimate.

Media timestamps can be rebased by the browser. Treat that timestamp-based
analyzer as conditional evidence, not proof of frame identity. For the generated
camera in `tests/live-video-probe.js`, the observer also reads the capture barcode
at the final writer and video element. Save `{observer: ..., samples: ...}` and run
`python tests/analyze-presentation-timing.py snapshot.json` to match by picture
content instead. Missing or repeated identities remain unverified. Neither tool
measures physical monitor scanout or proves sync for unmarked real-world video.

Example commands from the repository root:

```powershell
./scripts/control-standalone.ps1 -Directory ./test-results/bernd-live-control
./scripts/control-standalone.ps1 -Directory ./test-results/bernd-live-control -Action recordStart
./scripts/control-standalone.ps1 -Directory ./test-results/bernd-live-control -Action recordStop
```

Keep the control directory local and private to the operator. Any process able
to write there can issue these commands. The API does not open a network listener.
