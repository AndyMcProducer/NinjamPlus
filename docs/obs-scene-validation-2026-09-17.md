# OBS scene links

The helper's OBS mode hid a guest-mode VDO iframe without joining its room.
Its camera tiles also used bare `view` URLs without the room scope needed to
find room cameras.

The hidden roster iframe now uses `scene=0` only when the existing OBS mode is
enabled (`obs=1` or `view=obs`). Camera tiles use the standard
`room=...&view=...&solo=1` format. Normal guest links and `obs=0` retain their
original startup URLs. VDO.Ninja source and Electron Capture settings are unchanged.

## Validation

- `npm test`: all 22 tests passed with one worker; embedded helper assets match.
- Copied OBS URLs, both OBS aliases, room-scoped tile URLs, and unchanged guest
  startup are covered by the helper tests.
- Windows Electron Capture 2.23.3, Electron 39.2.12-qp20 / Chromium
  142.0.7444.235: the previous helper had zero room peers and no camera tiles.
  The fixed helper connected automatically and displayed 1280x720 video,
  advancing from 11 to 51 presented frames over two seconds. Its roster iframe
  did not publish a camera.
- The synthetic Chrome publisher joined through the normal guest controls.
  Both clients used the hosted VDO.Ninja alpha without source overrides.
- The live check serves the real helper HTML with synthetic native timing data.
  It is not a full native-plugin, OBS Browser Source, or Linux qualification.
  The Electron window is restored for the playback check; minimized rendering
  is not used as evidence of video failure.

Run the optional check against a local Electron Capture checkout with its
dependencies installed:

```text
node tests/run-obs-scene.cjs --electron-root=PATH --before-ref=f2d300c4
```

The runner creates an isolated room and profile, closes its clients afterward,
and saves before/after screenshots and frame counts under `test-results/`.
