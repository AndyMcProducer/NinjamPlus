# Hotspot-off direct UDP validation

The hotspot-off path established and maintained direct UDP video, but the
15-minute run was not a clean continuous-playback pass. No persistent
one-interval video lead was established. Source-preview stalls prevent treating
the observed freezes as solely a receiver or network fault.

## Configuration

- Native harness built from the code subsequently merged as PR 10 and released
  as v0.11.1.4; private loopback NINJAM server, two real native processors and
  generated audio recorded before encoding and after remote decoding.
- Native mobile hotspot mode off; separate VDO TURN mode defaults off in these
  fresh processor instances. No artificial packet impairment.
- Installed Chrome with fresh profile and real embedded helpers; 190 BPM,
  16 BPI (5052.63 ms interval), generated timestamped camera sources, minimal
  diagnostics. No decoder/presentation/scheduler wrappers. Analysis ran only
  after browser and native capture stopped.
- Deployed VDO alpha webrtc 962, SHA-256
  `46a4f7a9af1044fae40c071bf95020b117d037c2d4608b05341c650f9f60544c`.
- Capture artifacts: `test-results/hotspot-off-direct-1788799202370/minimal`.

The previous browser companion unconditionally supplied `relay`, including for
its `--transport=udp` option. Those earlier UDP runs were TURN/UDP tests and
must not be cited as direct-UDP validation. A new `--transport=direct` option
omits both `relay` and `tcp`, matching the normal hotspot-off path. It permits
normal ICE selection, so route telemetry, not the option name, establishes
whether a connection actually used direct UDP. The network observer now records
both selected candidates' protocols, types, and ports.

Hotspot-off does not ban TURN fallback. The separate VDO TURN Mode option can
also force relay/TCP independently. NINJAM's server connection remains TCP;
the direct-UDP observations here concern VDO video transport.

## Results

The two receivers recorded 904.8/906.0 seconds of pixel samples. All 1806/1810
selected candidate-pair observations were UDP host-to-host on both ends, with
no relay candidate and no TCP selection. This is same-host connectivity, not
evidence of traversal through independent global NATs/firewalls.

Independent waveform/pixel comparison used 220 audio windows per direction:

| Direction | Advancing, fresh-source matched windows | Median video minus audio | Maximum absolute error among those windows |
| --- | --- | --- | --- |
| Bravo to alpha | 197/220 | +29.38 ms | 518.38 ms |
| Alpha to bravo | 194/220 | +47.38 ms | 920.38 ms |

Positive means video late. These medians exclude frozen/unmatched/stale-source
windows; the denominators retain them and prevent a blanket sync pass.
After the first 30 seconds, the longest observed repeated-picture sequences
were 6376/3676 ms, and maximum displayed picture ages were 11095/12116 ms.
Both source previews also became stale, reaching 5370/5337 ms. Source-preview
samples aged at least 500 ms accounted for 202/4231 and 200/4233 observations.
The selected direct-UDP route remained connected through these events.

Conclusion: hotspot-off/direct-UDP connectivity and generally close timing are
demonstrated here. Reliable uninterrupted presentation, packet-loss recovery in
this mode, and cross-network NAT traversal are not established by this run.
The next useful isolation is a source independent of the shared browser/host
scheduling, followed by direct-UDP impairment with a verified endpoint filter.
There is no justified production buffer change from this evidence alone.

## Repeat

Start the native harness without `--hotspot`, using
`--live-vdo --bpm=190 --duration-seconds=990`. Then use its reported room and
helper ports:

```powershell
node tests/run-native-vdo-browser.cjs --room=ROOM --alpha-port=8001 --bravo-port=8002 --transport=direct --diagnostics=minimal --duration-seconds=900 --output=test-results/NEW-DIRECT-RUN
```

Stop and archive native captures before offline waveform analysis. Verify both
candidate types and protocols throughout the run rather than assuming that
omitting relay parameters guarantees a particular route.
