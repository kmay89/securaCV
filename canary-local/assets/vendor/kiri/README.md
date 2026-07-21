# Vendored Kiri:Moto slicing engine (optional)

The print guide's estimate ([`../../print-guide.js`](../../print-guide.js)) is
always-on, offline, and exact on filament mass. The **one** number a
transparent model can only approximate is *print time* — real toolpath time
depends on acceleration, travel and cooling that only a slicer computes.
[`../../slicer.js`](../../slicer.js) hands that single number to a real slicer —
**[Kiri:Moto](https://grid.space/kiri)** — **when, and only when, an engine is
vendored here.** With nothing vendored, the lab falls back to the honest
estimate and behaves exactly as before.

This directory is where that engine goes. It is **empty by default on
purpose** — Kiri:Moto is a build-from-source application (a web worker + WASM +
bundled modules), not a single-file library like `esptool-js`, so its bundle is
added by a deliberate, verified step rather than committed casually.

## Why it isn't checked in already

Two of the Lab's promises constrain how this may be done:

- **"nothing here phones anywhere"** — so the engine must be served from *this
  directory*, self-contained, with **no runtime fetch to grid.space or any
  CDN**. Loading Kiri from its website (the easy path) is off the table.
- **"doesn't rot"** — so the bundle must be pinned and provenance-tracked, and
  the bridge must fail closed if the contract drifts.

## What vendoring actually takes (measured, not assumed)

Investigating grid-apps at the pinned commit turned up three things that make
this a real integration, not a file copy — recorded here so the next person
doesn't rediscover them:

1. **The engine is not a static file.** grid-apps packs the client
   (`/lib/kiri/run/engine.js` + `worker.js` + `minion.js`) into a single
   compressed `web/boot/bundle-*.bin` that its own `gs-app-server` unpacks and
   routes at runtime. Serving it statically means either building it and
   **extracting** those `/lib/...` modules out of the bundle, or `esbuild`-ing
   the three entry points (`src/kiri/run/{engine,worker}.js`,
   `src/kiri/run/minion.js`) directly into standalone ESM files.
2. **Worker + WASM paths are configurable — good.** `client.setWorkPath()` /
   `setPoolPath()` (`src/kiri/app/workers.js`) let the worker and minion load
   from `./` next to the vendored files, and the FDM WASM (`kiri-geo.wasm`
   et al., ~70 KB) can sit alongside. So a self-contained offline layout *is*
   reachable.
3. **It needs `SharedArrayBuffer` — the catch.** The slicing pool allocates
   `SharedArrayBuffer` directly (`src/kiri/run/minion.js`, `src/kiri/core/
   widget.js`). That global only exists on a **cross-origin-isolated** page
   (`Cross-Origin-Opener-Policy: same-origin` + `Cross-Origin-Embedder-Policy:
   require-corp`). canary-local is served as static files (GitHub Pages via
   `.github/workflows/pages.yml`), which don't set those headers — so enabling
   Kiri means adding cross-origin isolation, typically a small
   `coi-serviceworker` shim. **That is an architectural decision** (it changes
   how the whole page is served and interacts with the live-emulator canvas),
   so it is intentionally left to a human, not silently added.

Until someone makes that call and runs the vendor step, the print guide behaves
exactly as before — the honest estimate — and `⚡ slice for exact time`
degrades to a clear "not vendored" note. Nothing is broken; a feature is simply
absent.

## Vendoring it

See [`../../../tools/vendor_kiri.sh`](../../../tools/vendor_kiri.sh) — it clones
[`GridSpace/grid-apps`](https://github.com/GridSpace/grid-apps) at the pinned
commit, builds the standalone engine/worker/minion + WASM into this directory,
and **enforces the offline promise** (fails if the output references any
external origin) before writing `PROVENANCE.txt`. It stops short of the two
steps that need a human: enabling cross-origin isolation for the site, and the
in-browser slice check.

After vendoring + isolation, verify a real slice — the probe does it for you:

```sh
node canary-local/tests/kiri_slice_probe.mjs   # asserts the sliced result
```

or by hand: open a device's **Enclosure → print guide**, click **⚡ slice for
exact time**, and the time tile should switch to *sliced by Kiri:Moto*.

## The contract the bridge relies on

`slicer.js` is written against Kiri:Moto's documented
[Engine API](https://github.com/GridSpace/grid-apps/blob/master/docs/kiri-moto/engine-apis.md)
and its **real** FDM field names / output format (verified in
`src/kiri/mode/fdm/`), so it can't silently produce a wrong number:

| We depend on | Source of truth |
|---|---|
| `newEngine().setMode/setDevice/setProcess/parse/slice/prepare/export` | `docs/kiri-moto/engine-apis.md` |
| device: `bedWidth`, `bedDepth`, `maxHeight`, `extruders[].extNozzle/extFilament` | `src/kiri/mode/fdm/work/*.js` |
| process: `sliceHeight`, `sliceShells`, `sliceFillSparse`, `slice{Top,Bottom}Layers`, `outputFeedrate`, `outputTemp` | `src/kiri/mode/fdm/app/init-menu.js` |
| time: g-code comment `; --- print time: <sec>s ---` | `src/kiri/mode/fdm/work/export.js` |

If a future Kiri build changes any of these, `parseGcodeTime` /
`validSliceSeconds` fail closed and the estimate stands — a *missing* upgrade,
never a *wrong* number. `../../../tests/slicer.test.js` pins that behavior.

## License

Kiri:Moto (grid-apps) is **MIT** licensed. `vendor_kiri.sh` copies the upstream
`LICENSE` into this directory alongside the vendored bytes.
