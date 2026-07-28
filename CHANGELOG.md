# Changelog

## v0.9.13.7 - 2026-07-28

### Ableton Chat Focus Fix

- Give docked and popout chat inputs explicit keyboard focus when opened so Ableton Live users can type after one click.

## v0.9.13.6 - 2026-07-26

### ReaNINJAM and GUI Close/Open Fix

- Keep NINJAMplus control and Opus lanes hidden from legacy/ReaNINJAM channel lists while preserving raw sync subscriptions for NINJAMplus peers.
- Restore ReaNINJAM-compatible Vorbis audio routing, including voice fallback when a server only allows two channels.
- Re-layout the local mixer when the connected server channel limit changes so unavailable Voice strips are not shown.
- Reduce master chord timeline spam by requiring stronger confidence before publishing detected chords.
- Keep VDO room and timing sync on hidden control transport where available, and add a low-cost Video Room activity pulse.
- Preserve DAW sync state across GUI close/open paths and avoid the recent CPU-spike regression.

## v0.9.13.4 - 2026-07-19

### Integrated from AndyMcProducer/release

- Build macOS Standalone, AU, and VST3 artifacts as universal `x86_64`/`arm64` binaries, with a default Intel deployment target of macOS 10.13.
- Upgrade GitHub checkout and artifact-download actions to v5.
- Split local macOS and Linux build helpers into `build-macos.sh` and `build-linux.sh`.

### Build hardening

- Preserve caller-supplied macOS deployment targets and architecture lists instead of hiding and overriding them with `INTERNAL` CMake cache entries.
- Validate both architecture slices in every macOS CI and release executable.
- Anchor local build cleanup to the repository directory, support an explicit `NINJAM_BUILD_DIR`, and retain `build.sh` as a compatibility wrapper.
- Allow Linux developers to skip automatic Debian/Ubuntu dependency installation with `NINJAM_SKIP_SYSTEM_DEPS=1`.

### Session fixes included in v0.9.13.3

- Preserve the default 720p30 quality selection through the NinjamPlus helper into VDO.Ninja.
- Use `mfr=30` rather than a fixed `fps=30` camera request, allowing unsupported capture modes to fall back without `OverconstrainedError`.
- Send one canonical per-stream `setBufferDelay` command instead of invalid UUID and unsupported target variants that could also change the global buffer.
- Add regression tests for quality propagation and targeted buffer routing.

### Validation performed

- Six deterministic helper tests pass.
- Combined functional, Linux, Windows, and macOS CI jobs pass; Windows also passes strict pluginval validation.
- CI verifies the packaged macOS Standalone, AU, and VST3 executables each contain both `x86_64` and `arm64` slices.
- Live video started at 1280x720 around 2.1 Mbps without camera constraint errors.
- Live remote video delay remained stable at approximately one 30 fps frame with no UUID/stream-routing errors.
- TURN/UDP cellular impairment testing held the fixed NINJAM timing target with 9.2 ms p95 scheduling error overall and 10.2 ms after recovery.

### Outstanding validation

- Smoke-test the universal build on physical Intel macOS 10.13 hardware and on Apple Silicon. CI proves compilation and architecture contents, but not runtime behavior on the oldest supported OS.
