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

Building and verifying that self-contained offline bundle needs a machine with
unrestricted egress (Kiri's build pulls three.js, jszip, quickjs and a WASM
solver) and an in-browser slice check. That is the vendoring step below.

## Vendoring it

```sh
# from the repo root, on a machine with network + node:
canary-local/tools/vendor_kiri.sh
```

The script (see [`../../../tools/vendor_kiri.sh`](../../../tools/vendor_kiri.sh))
clones [`GridSpace/grid-apps`](https://github.com/GridSpace/grid-apps) at the
pinned commit, runs its production bundle, copies the engine bundle + WASM into
this directory, and then **enforces the offline promise**: it greps the
vendored output for `grid.space` / absolute `http(s)://` origins and refuses to
finish if the bundle would phone home. It writes `PROVENANCE.txt` with the
source commit and SHA-256s.

After vendoring, verify a real slice in a browser (Playwright probe or by hand):
open `canary-local/index.html`, a device's **Enclosure → print guide**, and
click **"slice for exact time"** — the time tile should switch to *sliced by
Kiri:Moto* with the real toolpath number.

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
