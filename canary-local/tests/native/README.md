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
can't fall back to the dist without you noticing. The three Chromium probes
that load those cores take the same variable (see "The browser probes on
the tree's sources" below).

## What it builds, and how the tests reach it

[`cores.js`](cores.js) reads [`build.sh`](../../emulator/build.sh) for the
core's flavor (`vision` or `audio`): the source array its `for src in` loop
walks, the flags and include paths its `em++ -c` line passes, and the link
line's `-sEXPORTED_RUNTIME_METHODS`. Those sources are compiled with g++ next
to a one-line `<emscripten.h>` and [`core_server.cpp`](core_server.cpp). A
source added to the array is compiled here too, with nothing to edit in
`cores.js`; `native_cores.test.js` pins each core's source list, so update
that list in the same change.

The reader takes `build.sh` in one shape and refuses any other by name,
rather than build something `build.sh` does not. Each array the build reads
(the sources, the flags and what they splice in) and each variable their
words expand is written once, by the literal the reader parsed: an append
(`VISION_FLAGS+=(…)`), a reassignment or an element write is refused. The
compile line is exactly `em++ -c "$src" "${FLAGS[@]}" -o "$obj"`, so an
extra flag on it is refused, and so are `EMCC_CFLAGS`, an unquoted glob and
a backquoted command. If you reshape `build.sh` that way, teach `cores.js`
the new shape and add the case to `native_cores.test.js`.

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
with the exit status. A call with no answer throws after 30 s, and the
stuck core is killed so it cannot outlive the test.

## What a pass does and does not say

A pass says this tree's sources pass the page tests. It does not say the
dist is current, and it is not the dist: the compiler is different, and the
build is 64-bit where wasm is 32-bit. A page change that needs a core change
still ships with the rebuilt dist, and the default run keeps checking that
dist.

To compare the committed dist with this tree, run
`LAB_CORES=native node --test canary-local/tests/native_cores.test.js`. It
drives each native core next to its committed bundle, call for call, on
fixed scenarios: 3000 Vision ticks of random boxes in and around the
frame, with out-of-range tuning, and the audio cadences plus noise. A difference means the dist is
stale, or that the native build and the wasm one disagree on that input.
They can: `long` is 64 bits on a 64-bit host and 32 in wasm, and a signed overflow
(undefined in C++) can come out one way from g++ and another from
emscripten's clang. A box two billion pixels wide reads `unknown` on the
dist and `near` natively today. So agreement covers the calls those
scenarios make, not every input. `CXX=clang++` is the closer compiler (on
that box it matches the dist's voxel row, where g++ does not), but the ABI
is still 64-bit. Without the variable, the same file checks the loader
itself without compiling anything. It checks what the default returns, how
`build.sh` is read, that it finds every export and runtime method the dist
has, and that a signature a wasm call can't carry is refused. It also drives
the pipe with a stand-in core, [`fake_core.js`](fake_core.js), to check that
answers come back synchronously with wasm's i32 conversion, and that a core
that dies, answers twice or hangs fails its call (a hung core is also killed).

CI runs both. `canary-local.yml`'s page logic job runs the three tests on
the committed dist, then again with `LAB_CORES=native`. Its runner has g++
and needs no emsdk. The native step runs whenever the dist step ran, even
when the dist step failed (its `if:`), so after a core change you see both
answers: native green and dist red means the sources are right and the dist
is stale.

## Which tests drive a dist core

Only the three above `require()` a core in Node, and all three take it
through `cores.js` (`native_cores.test.js` holds that).

## The browser probes on the tree's sources

Three Chromium probes drive a page that loads one of these cores with a
`<script>` tag: `vision_probe.mjs` and `eyes_probe.mjs` (the Vision core) and
`audio_probe.mjs` (the WAP's acoustic core). They take the same variable:

```sh
LAB_CORES=native node canary-local/tests/vision_probe.mjs
LAB_CORES=native node canary-local/tests/eyes_probe.mjs
LAB_CORES=native node canary-local/tests/audio_probe.mjs
```

With it, the probe asks [`probe_cores.js`](probe_cores.js) for its core,
which builds it with `cores.js` as above, before Chromium starts. The probe's
server then answers the dist URL (`emulator/dist/canary-vision-core.js`,
`canary-wap-audio.js`) with [`core_standin.js`](core_standin.js) instead of
the committed bundle, under every spelling of that URL the server would read
as the file (`dist//`, `%2e%2e`, a percent-encoded name), and refuses any
other method on it, so a native run never serves the committed core. The
stand-in is a factory of the same name and shape:
`createCanaryVisionCore()` resolves to a module whose `cwrap`, `ccall`,
`UTF8ToString` and `HEAP` views are the ones the dist has. Each call is a
synchronous request to the probe server, which runs it on the native core
through `cores.js`'s own module, so the return conversions and refusals are
the same code the Node tests use. A pointer export's window (the audio
frame) lives in a heap the stand-in keeps in the page. The page writes it
through `HEAP16`, each call carries it to the core and brings it back, and
the heap grows and detaches its old buffer the way wasm memory does.

The page itself is served as committed, policy and all. The stand-in is a
`'self'` script, its requests are `'self'` connects, and it needs no eval and
no inline code. Unset, `probeCores` returns `null` and the probe serves the
dist exactly as before. Two things differ for the page under the stand-in,
and the probes allow for both. Every core call is now a request, so
`vision.html`, which calls its core every animation frame, never reaches
Playwright's `networkidle`; the probe waits instead for the same 500 ms of
quiet with the core's own requests left out. And a native run proves nothing
if the page never ran the stand-in, so the probe fails one whose page never
loaded it or never called it, and prints the call count when it passes.

`native_cores.test.js` holds this in the default run with no compiler and no
Chromium. It runs the stand-in as served, in its own `vm` context, with an
`XMLHttpRequest` that hands each request to the bridge over the stand-in
core [`fake_core.js`](fake_core.js), which also hands out two buffers through
pointer exports. It checks the module's shape, the i32 arguments, the
return conversions, the refusals at `cwrap` time, a core that dies, the
window carried both ways and grown, and the bridge's refusal of memory
outside an open window. It also checks what the probe server's `handle()`
takes (each spelling of a dist URL, and nothing else), that a bridge whose
stand-in was never served or never called is not `used()`, that the quiet
wait leaves the core's requests out, that no probe opening a core page skips
the bridge, and that CI runs each probe both ways. Under `LAB_CORES=native` it also drives the stand-in
call for call against `cores.js`'s module on both real cores.

CI's wasm job runs each of the three probes on the dist, then again with
`LAB_CORES=native` on the runner's g++, whenever the dist step ran, red or
green, as the logic job does for the Node tests. A native pass in Chromium
proves the same thing as one in Node: this tree's sources pass, built by
another compiler for a 64-bit ABI. It is not the dist, and the default run
still checks the dist.

## The display flavors

The display flavors (`dist/canary-display-*.js`) are booted only in
Chromium, by the probes (`boot_probe.mjs`, `bench_probe.mjs`,
`onboard_probe.mjs` and `csp_probe.mjs` among them), and aren't served
natively. Each of those bundles links the display firmware, LVGL and the
browser shims into one page under Asyncify, and the probes drive that page:
its canvas, Web Audio, serial panel and the phone's captive portal. A native
process behind a pipe can't stand in for that page.
