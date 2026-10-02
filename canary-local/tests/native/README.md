# Page tests on the tree's core sources (`LAB_CORES=native`)

Three page tests drive a WebAssembly core built from firmware:
`vision.test.js` and `eyes.test.js` drive the Canary Vision core, and
`audio.test.js` drives the WAP's acoustic core. By default they load the
committed bundles in `canary-local/emulator/dist/`, the same bytes the Lab
serves. That is what CI checks, and it stays the default.

The committed bundle lags a source change until CI's pinned-emsdk rebuild
lands (Actions → "Rebuild emulator dist (pinned emsdk)"). Most working
environments can't install emsdk 6.0.3, so before that rebuild a change to a
core's sources can't be proven page-side: the tests run the old core and
fail. `LAB_CORES=native` runs them on this tree's sources instead:

```sh
LAB_CORES=native node --test canary-local/tests/vision.test.js \
  canary-local/tests/eyes.test.js canary-local/tests/audio.test.js
```

You need g++ (or set `CXX`, e.g. `CXX=clang++`) on Linux or macOS. Each test
process builds the core it uses once, in about three seconds, into a temp
directory that it removes on exit. Leave `LAB_CORES` unset (or set it to
`dist`) for the committed bundles. Any other value is refused, so a typo
can't fall back to the dist without you noticing.

## What it builds, and how the tests reach it

[`cores.js`](cores.js) reads [`build.sh`](../../emulator/build.sh) for the
core's flavor (`vision` or `audio`): the source array its `for src in` loop
walks, the flags and include paths its `em++ -c` line passes, and the link
line's `-sEXPORTED_RUNTIME_METHODS`. A source added to `build.sh` is compiled
here too, with nothing to edit. Those sources are compiled with g++ next to
a one-line `<emscripten.h>` and [`core_server.cpp`](core_server.cpp).

| Core | Sources (`build.sh`) | Against |
|---|---|---|
| `canary-vision-core` | `detect_config.cpp`, `presence_fsm.cpp`, `voxel_tracker.cpp` (canary-vision), `vision_core_bindings.cpp`, `vision_core_shim.cpp` | the emulator's Arduino shim |
| `canary-wap-audio` | `securacv_audio.cpp` (canary-wap), `audio_core_bindings.cpp` | the WAP's host audio stubs |

`core_server.cpp` serves the exports over stdin and stdout, one line per
call. Its table of exports is generated from the sources'
`EMSCRIPTEN_KEEPALIVE` signatures, so a new export in the bindings is served
without any edit here. A signature a wasm call can't carry (a `double`, a
`long long`, a string argument) is refused by name; the harness never
guesses. The test gets a factory with the dist's shape. You call it, await a
module, and `cwrap` its exports, and `ccall` and the `HEAP` views are there
when the dist exports them. Calls stay synchronous as they are in wasm,
because a worker thread ([`core_worker.js`](core_worker.js)) owns the pipe
and the test's thread waits on a `SharedArrayBuffer`. The audio core's PCM
frame, which the test writes through `HEAP16` at `audio_emu_frame_ptr()`,
is a window the harness sends to the core before each call and reads back
after it. A pointer export needs that window declared in `CORES` in
`cores.js`, or the build refuses it. If the core crashes, the call throws
with the exit status, and a call with no answer throws after 30 s.

## What a pass does and does not say

A pass says this tree's sources pass the page tests. It does not say the
dist is current, and it is not the dist: the compiler is different, and the
build is 64-bit where wasm is 32-bit. A page change that needs a core change
still ships with the rebuilt dist, and the default run keeps checking that
dist.

To see whether the committed dist matches this tree, run
`LAB_CORES=native node --test canary-local/tests/native_cores.test.js`. It
drives each native core next to its committed bundle, call for call (3000
Vision ticks, and the audio cadences plus noise), and the two agree while
the dist is current. Without the variable, the same file checks the loader
itself without compiling anything. It checks what the default returns, how
`build.sh` is read, that it finds every export and runtime method the dist
has, and that a signature a wasm call can't carry is refused.

CI runs both. `canary-local.yml`'s page logic job runs the three tests on
the committed dist, then again with `LAB_CORES=native`. Its runner has g++
and needs no emsdk. If the native run passes and the dist run fails after a
core change, the sources are right and the dist is stale.

## Which tests drive a dist core

Only the three above `require()` a core in Node, and all three take it
through `cores.js` (`native_cores.test.js` holds that). `LAB_CORES` applies
to those Node tests only. The browser probes that load the Vision and audio
cores (`vision_probe.mjs`, `eyes_probe.mjs`, `audio_probe.mjs`) load the
committed dist, because each page loads it with a `<script>` tag.

The display flavors (`dist/canary-display-*.js`) are booted only in
Chromium, by the probes (`boot_probe.mjs`, `bench_probe.mjs`,
`onboard_probe.mjs` and `csp_probe.mjs` among them), and aren't served
natively. Each of those bundles links the display firmware, LVGL and the
browser shims into one page under Asyncify, and the probes drive that page:
its canvas, Web Audio, serial panel and the phone's captive portal. A native
process behind a pipe can't stand in for that page.
