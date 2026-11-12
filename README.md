# jpeglitch-obs

A native OBS Studio filter plugin that applies the "JPEGlitch" JPEG glitch
effect live to video sources (webcams, media sources, ...): the frame is
encoded as JPEG, bits in the compressed bitstream are flipped/set/cleared,
and the result is decoded again. Works with stream, recording, and the
virtual camera, because it's a real async video filter, not a shader, a
browser source, or a window-capture trick.

Ported from the original Qt desktop app
[607011/qlitch](https://github.com/607011/qlitch) (2013) via a browser
reference implementation (see `reference/qlitch.html`, not part of the
plugin itself).

## Status

This repository currently contains:

* `src/glitch/` -- the OBS-free glitch core (MT19937 + the bit-flip
  algorithm), with CTest unit tests. Builds and tests pass standalone.
* `src/codec/` -- a small, OBS-free TurboJPEG-3 wrapper tolerant of broken
  (glitched) JPEG data, used by both the CLI and the filter.
* `tools/jpeglitch-cli/` -- a small CLI around the core + codec, to compare
  output against `reference/qlitch.html` without needing OBS. Includes a
  codec robustness test (CTest) that intentionally mangles JPEGs across a
  wide parameter sweep and asserts a valid image always comes out.
* `src/jpeglitch-filter.c` -- the actual OBS async video filter: full
  properties UI, NV12 and UYVY pixel pipelines via TurboJPEG's planar YUV
  compress/decompress API, mutex-protected parameter updates, and the "new
  pattern every N frames" behavior.
* The full plugin builds successfully with `cmake --preset macos` +
  `cmake --build`, producing a genuine universal (arm64+x86_64)
  `jpeglitch-obs.plugin` bundle, linked against a real, from-source-built
  libobs 31.1.1 and a statically-linked universal libjpeg-turbo.
* **Verified live in OBS Studio 33.0.1 on macOS**: installed via
  `cmake --install`, loads without errors, "JPEGlitch" shows up
  under a Video Capture Device's Filters, and visibly glitches the webcam
  feed (the device delivered UYVY, not NV12, which is what prompted adding
  UYVY support). Note: a plugin built against newer libobs headers than the
  installed OBS Studio's own libobs can fail to register at all (log shows
  `obs_register_source: Tried to register obs_source_info with size ... which
  is more than libobs currently supports`) -- make sure OBS Studio itself is
  reasonably current (33.x was used here; the spec's own "OBS 30+" baseline
  should be fine too, 29.x was not).
* I420, YUY2, and I444 are not implemented yet; frames in those (or any
  other) formats are currently passed through unmodified with a one-time
  warning in the log, per spec.
* `.github/workflows/unit-tests.yaml` builds and runs the `src/glitch`
  and `tools/jpeglitch-cli` CTest suites on Linux/macOS/Windows, in addition
  to the obs-plugintemplate's own existing `build-project.yaml` (builds
  the actual plugin for all three platforms) and `check-format.yaml`
  (clang-format + gersemi, only on files changed in a given push/PR).
  Not yet verified green on GitHub itself, since this repository has no
  remote configured yet -- the steps were run locally instead and pass.

## Building the glitch core + CLI (no OBS needed)

```bash
cmake -S src/glitch -B src/glitch/build && cmake --build src/glitch/build
ctest --test-dir src/glitch/build --output-on-failure

cmake -S tools/jpeglitch-cli -B tools/jpeglitch-cli/build
cmake --build tools/jpeglitch-cli/build
ctest --test-dir tools/jpeglitch-cli/build --output-on-failure

./tools/jpeglitch-cli/build/jpeglitch-cli some-photo.jpg --seed 42 -o /tmp/out
```

By default, `FindOrFetchTurboJPEG.cmake` uses a system libjpeg-turbo found
via pkg-config (fast local iteration) and falls back to fetching and
building it statically from source otherwise (what CI and end users get).
Pass `-DJPEGLITCH_SYSTEM_JPEGTURBO=ON` to require the system library and fail
loudly if it's missing (e.g. for Linux distro packaging).

## Building the actual plugin

This follows the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate)
workflow: `cmake --preset macos` (or `windows-x64` / `ubuntu-x86_64`), which
downloads the matching OBS Studio sources and prebuilt dependencies the
first time it runs (a multi-hundred-MB OBS + Qt6 bundle). On macOS this was
run end to end in this environment and produces a working universal
(arm64+x86_64) `jpeglitch-obs.plugin` bundle; `windows-x64` and
`ubuntu-x86_64` have not been tried here.

Since libjpeg-turbo's own CMakeLists.txt refuses to be built for more than
one macOS architecture at once ("contains assembly code"), a multi-arch
build in `cmake/jpeglitch/FindOrFetchTurboJPEG.cmake` builds it once per
architecture and combines the results with `lipo`, mirroring upstream's
own release process.

`-DJPEGLITCH_BUILD_TOOLS=ON` additionally builds `tools/jpeglitch-cli` alongside
the plugin from the same configure.

### Verifying inside OBS Studio

Done, on macOS:

1. Build with the preset above, `cmake --install` it (installs straight
   into OBS Studio's own plugin directory on macOS).
2. Added as a filter to a Video Capture Device source; confirmed it shows
   up under "Filters" and visibly glitches the live webcam feed.

Still to do, per the project's acceptance criteria:

* Try it on a media source too.
* Check 720p/1080p performance, that every property updates live, that a
  fixed seed gives a stable pattern and no seed flickers, and that the
  virtual camera shows the glitched output in a video call app.
* Windows and Linux builds (only macOS has been built and run here).
* CI is wired up (see below) but not yet verified actually green on
  GitHub, since this repository has no remote yet.

**Done:** ran the OBS-free core/codec/CLI under ASan+UBSan (leak detection
isn't available on macOS, so that part of the check doesn't apply here),
including the existing `codec_robustness` CTest and a CLI sweep of ~850
combinations of seed/start/algorithm/quality/FF-options/image-size
(including quality 0, start 0%/100%, 200 defects, and a non-8-aligned
137x121 image) -- no sanitizer reports, no crashes. This covers the core
glitch/codec logic; the filter's OBS-specific buffer handling
(`src/jpeglitch-filter.c`) hasn't been run under a sanitizer, since that needs
an ASan-instrumented libobs build.

## Design decisions worth knowing about

* **The filter never mutates the incoming `obs_source_frame` in place.**
  libobs's `async_tick()` can call `filter_video()` more than once on the
  *same* cached frame object (e.g. if the display refresh rate exceeds the
  capture frame rate); in-place mutation would re-glitch an already
  glitched frame every repeat tick. The filter instead owns a separate,
  reused `out_frame` and returns either that or the untouched original
  (pass-through) -- see the comment block at the top of
  `src/jpeglitch-filter.c`.
* **TurboJPEG's `TJPARAM_QUALITY` only accepts 1-100**, not the project
  spec's UI range of 0-100; the value is clamped at the boundary in both
  `tools/jpeglitch-cli` and the filter (not in the OBS property itself, so the
  slider still matches the spec table).
* **libjpeg-turbo is resolved via `cmake/jpeglitch/FindOrFetchTurboJPEG.cmake`**:
  system library first (fast local builds, and the Linux "use the system
  library" alternative from the spec), source build via `FetchContent`
  otherwise (what CI/end users get, matching the "statically bundled"
  requirement).
* **Deliberate deviation from the spec: "Single-bit mode" was removed
  entirely** (core library, filter, CLI, tests, locale strings). Diagnosed
  live: it touches a single, *fixed* byte position every frame (`start` is
  not randomized the way `defects` positions are). If that position happens
  to be a JPEG byte-stuffing `0x00` right after a raw `0xFF` in the
  entropy-coded scan, every bit flip there -- regardless of which bit --
  un-stuffs that `0xFF`, which libjpeg-turbo reports as a fatal "Bogus
  marker length" error. Since the position doesn't change frame to frame,
  this failure was consistent rather than occasional, so the documented
  "fatal error -> show the last good frame" fallback made the video freeze
  solid instead of glitching, for as long as that byte stayed unlucky. This
  is inherent to the spec's own byte-flipping algorithm (also present in
  `reference/qlitch.html`), not a bug in this implementation; removed by
  explicit request rather than patched around, to keep the algorithm an
  exact match to the reference for the modes that remain.

## License

GPLv3 (or, at your option, any later version) -- same as the original
[qlitch](https://github.com/607011/qlitch), and compatible with OBS
Studio's own GPLv2-or-later licensing. `src/glitch` and `src/codec` are
also GPLv3; they merely don't *link against* libobs, which is why they can
be unit-tested without an OBS build.

libjpeg-turbo, statically linked into the distributed plugin binary, is
licensed under a BSD-style license plus the historical IJG License; both
are permissive and compatible with GPLv3. Its license text ships alongside
the plugin binary when packaged (`LICENSE.libjpeg-turbo`, to be added once
packaging is wired up in a later milestone).
