# Third-Party Notices

This document is a working draft for NINJAMplus.

It lists third-party software, SDKs, assets, and hosted services that were
verified from the current repository and active build configuration. It is not
legal advice. Review the final distribution against the exact binaries, assets,
and services you ship.

## Verified In The Active Build

### NINJAM and WDL

- Source tree: `ninjam/`
- Active build path: `CMakeLists.txt` compiles `ninjam/ninjam/*.cpp` and
  `ninjam/WDL/*` sources directly into the plugin.
- Upstream notices already present in this repository include:
  - `ninjam/LICENSE`
  - `ninjam/ninjam/njclient.cpp`
  - `ninjam/ninjam/winclient/license.txt`
  - `ninjam/ninjam/cursesclient/license.txt`
- Attribution and compliance note:
  - Preserve upstream copyright and license notices in vendored source files.
  - Include the applicable upstream license text(s) with distributed binaries
    and source packages.
  - If upstream files were modified, keep clear modification notices with the
    changed files and review the corresponding source-distribution obligations
    of the upstream license(s).

### JUCE 8.0.0

- Source: fetched by CMake from `juce-framework/JUCE` tag `8.0.0`.
- License: JUCE 8 is dual-licensed under AGPLv3 and the JUCE commercial/EULA
  terms.
- Attribution and compliance note:
  - Distribution must comply with the JUCE license actually used for the
    shipped build.
  - JUCE's own license file also lists bundled JUCE dependencies and their
    licenses, including VST3 SDK licensing information.

### Steinberg ASIO SDK 2.3.x (Windows builds)

- Source: fetched by CMake from `audiosdk/asio` when building on Windows.
- License: dual licensing is described upstream as proprietary Steinberg ASIO
  SDK terms or GPLv3.
- Attribution and compliance note:
  - If ASIO-enabled builds are distributed, review and follow the Steinberg
    ASIO SDK license terms that apply to your distribution model.
  - If the ASIO trademark or logo is used, follow Steinberg's trademark and
    logo usage guidelines.

### libogg 1.3.5

- Source: fetched by CMake from `xiph/ogg` tag `v1.3.5`.
- License: BSD-style 3-clause license.
- Attribution note:
  - Retain copyright notice, license conditions, and disclaimer in source and
    binary distributions.

### libvorbis 1.3.7

- Source: fetched by CMake from `xiph/vorbis` tag `v1.3.7`.
- License: BSD-style 3-clause license.
- Attribution note:
  - Retain copyright notice, license conditions, and disclaimer in source and
    binary distributions.

### Opus 1.4

- Source: fetched by CMake from `xiph/opus` tag `v1.4`.
- License: BSD-style 3-clause license.
- Attribution note:
  - Retain copyright notice, license conditions, and disclaimer in source and
    binary distributions.

### Chord Detector and Chromagram

- Source tree: `third_party/chord-detector/`
- Upstream: `adamstark/Chord-Detector-and-Chromagram`
- Active build path: `CMakeLists.txt` compiles the chromagram and chord
  detector source directly into the plugin for local input analysis.
- License: GPLv3 or later. The upstream license text is included at
  `third_party/chord-detector/LICENSE.txt`.
- Bundled FFT dependency:
  - `third_party/chord-detector/libs/kiss_fft130/`
  - License: BSD-style 3-clause license, included at
    `third_party/chord-detector/libs/kiss_fft130/COPYING`.
- Attribution and compliance note:
  - Preserve upstream copyright and license notices in vendored source files.
  - Include the GPLv3-or-later and Kiss FFT license texts with distributed
    source and binary packages.
  - The local copy includes small real-time integration changes for this
    plugin; keep those changes available with the corresponding source.

## Local Helper Code And Packaged Assets

### Advanced VDO Client Helper Pages And Server

- Files:
  - `advanced-vdo-client/index.html`
  - `advanced-vdo-client/app.html`
  - `advanced-vdo-client/server.js`
- Status:
  - These files appear to be authored within this repository.
  - No separate third-party license header was identified in these files.
- Action:
  - If any portions were copied or adapted from external sources, add the
    original source, author, and license here before distribution.

### `advanced-vdo-client/icon.png`

- Files:
  - `advanced-vdo-client/icon.png`
  - `ninjam/extras/ninjam-vst3/advanced-vdo-client/icon.png`
- Use in shipping build:
  - Referenced as the plugin/app icon in `CMakeLists.txt`.
  - Copied into packaged Standalone and VST3 web-helper resources by CMake.
- Status:
  - Created in-house by Andy McProducer / AMP using an AI-assisted workflow.
  - No separate third-party asset license is required for this icon based on
    the current repository information.

### `ws` 8.20.0 (Conditional)

- Source:
  - `advanced-vdo-client/package.json`
  - `advanced-vdo-client/package-lock.json`
  - `advanced-vdo-client/node_modules/ws/package.json`
- License: MIT.
- Attribution note:
  - Include the MIT license text if the Node helper is distributed together
    with `node_modules` or bundled in an installer/package.
  - If `ws` is only used during local development and is not shipped, treat it
    as a development/distribution-conditional notice.

## Runtime Services And Hosted Platforms

### VDO.Ninja

- Referenced by:
  - `Source/PluginProcessor.cpp`
  - `advanced-vdo-client/server.js`
  - `advanced-vdo-client/app.html`
- Status:
  - This is a hosted external service reference, not bundled source code in the
    plugin repository.
- Action:
  - Review VDO.Ninja terms, branding, and embedding expectations for the way
    your product launches or embeds it.

### Translation Services

- Referenced by:
  - `Source/PluginProcessor.cpp`
- Current endpoints:
  - `https://translate.fedilab.app/translate`
  - `https://translate.googleapis.com/translate_a/single`
- Status:
  - These are external services, not bundled libraries.
- Action:
  - Review provider terms, acceptable-use policies, and branding requirements.
  - These are service dependencies rather than local third-party code notices.

## Conditional Or Not Confirmed In The Active VST3 Shipping Path

### FFmpeg / x264 / nvcodec / AMF Stack

- Declared in: `vcpkg.json`
- Status:
  - Present in the repository manifest, but not confirmed in the active VST3
    build path reviewed for this draft.
- Action:
  - If any distributed build links or bundles FFmpeg, x264, NVENC/NVCodec, or
    AMF-related binaries, add their notices and perform a separate license and
    redistribution review before release.
  - The `x264` feature in particular can materially change redistribution
    obligations.

### Additional Vendored License Files Present In The Repository

- `ninjam/WDL/libpng/LICENSE`
- `ninjam/WDL/giflib/COPYING`

These files are present in the vendored tree. Include them in release notices
if the corresponding code or assets are linked or redistributed in your shipped
artifacts.

## Release Checklist

- Include this file with source and binary distributions.
- Include the applicable upstream license texts already present in the vendored
  tree for the code actually compiled into the product.
- Preserve original copyright and license headers in vendored source files.
- Add missing provenance for shipped artwork, especially `icon.png`, before
  public release.
- If optional helper packages or binary stacks are shipped later, append their
  notices before distribution.
