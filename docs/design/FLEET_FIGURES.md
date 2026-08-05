# Fleet figures — one picture of every thing, everywhere (2026-08)

Every physical thing SecuraCV talks about gets a **figure**: the same object,
drawn from the same isometric camera, under the same light, in the same
palette, at every size and on every surface — the glass, the wrist, the
phone, the web, the emulator. When a page says "the radome front", the
radome front is next to the words. When a witness the app has never met
appears in the roll call, the app draws *that* device, not a generic dot.

The model is the one a Bambu printer uses: it never says "clean the nozzle"
without showing you the nozzle. The mental load of mapping a name onto a
shape is real, it is paid by every user on every screen, and it is
avoidable — once — by making the picture part of the data.

> **Status.** The system is real and CI-gated: the generator, the ledger, the
> 47 committed figures, and each surface's generated binding are in the tree.
> What consumes them today:
>
> | Surface | Binding | Drawn in a UI? |
> |---|---|---|
> | Web | the SVGs + the ledger | **Yes** — the website's `/figures` catalog |
> | iPhone · Watch · widgets | `ios/Shared/FleetFigures.swift` | **Yes** — the witness roster and the pairing rows |
> | Both flashers | `figure` in `canary-local/devices/flash.json` | **Yes** — the firmware picker, in the browser and the desktop app |
> | Firmware | `firmware/common/core/fleet_figures.h` | Not yet — the lookup is wired and each board names itself, but no screen draws it |
> | Emulator | the same SVGs | Not yet |
>
> §10 is what wiring the remaining two involves.
---

## 1. What was wrong

Nothing was missing, exactly — the trouble was that every surface solved it
separately, and none of them solved it well:

- The website showroom draws real geometry (STLs + vendor GLBs in three.js),
  which is beautiful and costs a WebGL context, a loader, and megabytes. It
  works on exactly one surface.
- `docs/hardware/enclosure/preview_*.png` are OpenSCAD screenshots at
  whatever camera each one happened to be rendered from, in whatever palette
  — so two parts of the same device don't look like they belong together.
- The glass, the wrist and the emulator have **no** picture at all. A
  witness is a name, a severity color and a chip.
- Nothing answers "which of these are real and which are still ideas?" in one
  place — status lived in three vocabularies that didn't agree, which
  `CATALOG_ARCHITECTURE.md` §1 named as the root defect and §5 fixed for
  *variants*. It never reached the pictures.

So: one description, one projector, one ladder, and every surface reads it.

---

## 2. The shape of the system

```
  committed CAD                     the one projector            every surface
  ─────────────                     ─────────────────            ─────────────
  docs/hardware/enclosure/*.stl ┐
  canary-local/devices/         │   tools/figures/iso.mjs   ┌──► canary-local/figures/*.svg
    registry.json               ├──►  massing.mjs        ───┼──► firmware/common/core/fleet_figures.h
    catalog.json                │     gen_figures.mjs       ├──► ios/Shared/FleetFigures.swift
  firmware/configs/*/           ┘                           └──► canary-local/devices/figures.json
```

One command builds all of it, and CI fails if the tree disagrees with it:

```sh
node canary-local/tools/figures/gen_figures.mjs          # build
node canary-local/tools/figures/gen_figures.mjs --check  # the CI gate
```

Nothing in the output is hand-edited. Editing a `.svg`, the header or the
Swift by hand is a mistake the gate catches on the next push.

---

## 3. One camera, one light, one palette

`iso.mjs` is the projector and nothing else: massing in millimeters in, flat
polygons out. It is pure and deterministic — no clock, no randomness, no
platform-dependent float formatting — because its output is committed and
CI diffs the bytes.

- **Frame.** `+X` right, `+Y` the thing's front (toward the viewer), `+Z` up.
  Millimeters, matching the SCADs and the STLs.
- **Camera.** True 30° isometric, fixed at `(+X, +Y, +Z)` for every figure in
  the fleet. The three visible faces are always right, front and top. There
  is no per-figure camera, on purpose: a per-drawing viewpoint is exactly the
  drift that made the OpenSCAD previews unusable as a set.
- **Light.** One direction, three separated tones — top brightest, right
  face mid, front face darkest. Flat facets, no gradients. This is technical
  illustration, not a render.
- **Palette.** Named materials (`shell`, `gasket`, `radome`, `lens`, `board`,
  `accent`, …), not per-figure colors, so a TPU gasket is the same purple in
  the doorbell figure and the WAP figure and reads as the same kind of thing.
  Colors are the physical object's, not the UI theme's — a printed case is
  off-white whether the app is in light or dark mode. Only the contact shadow
  and the ghost stroke adapt, via CSS variables with sane fallbacks.

**Why vector, not a 3D scene.** A watch complication, a 20 px list row and a
900 px hero need the same picture. Vector costs a few KB, needs no GPU, no
model loader and no decoder, scales without mud, and renders identically in
SwiftUI, a WKWebView, the emulator and a static page. The `.glb` models in
the website's `models/` stay exactly what they are — the AR "view in your
room" path, which is a different job.

---

## 4. A figure is a massing, not a model

Proportion, stack order, and the one or two features that make a part
recognizable: the radome window, the lens, the button, the keyhole. No
fillet detail, no tilt, no texture. That constraint is what lets a single
description drive a hero and a complication and still read as the same
object.

The vocabulary is four primitives, which are internally one thing — a closed
outline in two axes, extruded along the third:

| | |
|---|---|
| `box {at, size, r?, face?}` | `at` is the min corner. `face:'y'` rounds the outline in the plane you can see, which is what a screen or a radome window wants. |
| `cyl` / `disc {at, r, h, axis?}` | `axis:'z'` stands it up; `axis:'y'` lays it along the view axis — a lens, a button, a status LED looking at you. |

Solids are painted back to front by their center's depth along the view axis.
Within one convex solid the visible faces tile its silhouette exactly, so
they need no sorting among themselves.

**The frame is a promise.** Everything a figure draws — including the contact
shadow, which is a footprint scaled outward from the object — has to sit
inside the viewBox it declares, so the fit is computed over the shadow too. It
wasn't at first, and a round display's shadow reached y = 293.8 in a 256 box:
clipped on every surface that drew it.

**The coplanar rule.** Never author two different-material faces on the same
plane. The `.glb` generators learned this the hard way (z-fighting on the
GPU — see the website's working notes); here the failure is quieter and the
same defect, a tie in the paint order resolved by an accident of sort
stability. Hold every stacked element a hair proud of the one under it. The
generator refuses to emit a figure that breaks the rule.

---

## 5. Where the numbers come from — and why they can't rot

Almost nothing in `massing.mjs` is a typed dimension. A part declares the
**committed STL it is**, and the generator reads that STL's bounding box at
build time and hands it to the figure as its envelope. The figure is a
*function of* the CAD, not a copy of it: re-export the STL and the figure
follows.

On top of that, the **drift guard**: a single-part figure must fill the same
box as its part, or the picture and the print have parted company. The check
is tight in the plan (±0.75 mm, where a wrong number would actually mislead)
and generous in depth (+4 mm, because lenses and buttons legitimately stand
proud). Change the CAD without updating the figure and the generator stops.

That guard is the difference between this and a folder of drawings. It is
also why the drawings can be trusted enough to put in front of a user who is
about to print one.

Things with no committed CAD — the in-development display enclosures, and
the concept devices that are still only a research note — declare a `sketch`
envelope instead. The ledger records `dims_source: "sketch"` next to them, so
a surface can caption them honestly rather than implying measured geometry.

---

## 6. The confidence ladder

The other half of the ask: *which of these are confirmed and which are still
ideas?* One vocabulary, four rungs, **derived from evidence that exists on
disk** — never hand-typed, so it cannot flatter itself:

| Rung | Means | Evidence required |
|---|---|---|
| `shipping` | you can print it and flash it today | committed STLs **and** a firmware config **and** a released catalog variant |
| `confirmed` | the design is settled and released in the catalog | released variant; one of the three proofs still missing |
| `prototype` | it exists, in development | neither of the above; nothing committed to print |
| `idea` | a research note and a sketch — nothing is built | the registry calls it a concept |

The evidence is written into the ledger **next to the verdict**, so anyone
can check the ladder's arithmetic instead of trusting it:

```jsonc
{ "id": "device.canary-sense", "confidence": "shipping",
  "evidence": { "registry_kind": "witness",
                "committed_stls": ["canary_sense_back.stl", "canary_sense_front.stl"],
                "catalog_variants": [{ "product": "sense", "variant": "sense-…", "status": "released" }],
                "firmware_configs": ["default", "wellbeing"] } }
```

**The honesty invariant.** An idea renders as a **ghost**: a dashed
wireframe, no fills, no shading, no contact shadow. It cannot be mistaken for
something you can buy or print. This is enforced three ways — the generator
only emits ghost faces for that rung, `tests/figures.test.js` asserts no
built figure renders as a ghost and no idea renders with fills, and the same
assertion runs against the SwiftUI data so an idea can't come out solid on
the wrist and dashed on the web.

Ideas are *in* the catalog, drawn from the same camera, sitting beside the
shipping devices. Hiding them would be the other failure: the test asserts
every concept in `registry.json` has a figure, so one can't quietly fall off
the roadmap.

---

## 7. One plan, many surfaces

`planFigure()` decides the geometry exactly once and returns an ordered list
of flat polygons. Both emitters serialize that same plan, so two surfaces
cannot drift into showing a user two different pictures of one object.

**Web / emulator — `canary-local/figures/<id>.svg`.** Two per figure: the
full 256-unit drawing, and a `.glyph.svg` at 64 with the fine details and the
contact shadow dropped, because they turn to mud at 20 px. Self-contained, no
external references, CSP-safe.

**Firmware — `firmware/common/core/fleet_figures.h`.** A device's own answer
to *what am I?*. Pure C++, no Arduino/JSON dependency, no allocation: the same
rules `fleet_model.h` follows, so it stays host-testable.

```cpp
if (const auto* f = canary::figures::figure_for(w.device_type)) {
  draw_figure(f->figure_id);           // the right picture
  if (f->rev != cached_rev) refetch(); // …and a fresh one
}
```

**The table is deliberately incomplete, and that is the feature.** A device
type appears only when every firmware config that publishes it agrees on one
figure. Types shared by more than one board — `canary-dash` by both the 4.3″
and the 7″ panel, `canary-nightstand` by both the 1.47″ stick and the 1.69″
touch — are **absent**, `figure_for` returns `nullptr`, and the caller draws
its generic marker.

This is the one place the system nearly defeated itself. The first cut
resolved unknown types with a regex on the type string and handed both
rectangular nightstand boards the round 52 mm Watch Station drum — showing a
user the wrong physical device, precisely the confusion the figures exist to
remove. **A wrong picture is worse than no picture.** Unresolved types are
listed with reasons in the ledger's `device_types.unmapped`.

### The exact lookup: `figure_for_hardware()`

Every build env compiles against exactly one `boards/<id>/pins` header, and
that include is **load-bearing** — wrong pins, dead device. So it is already a
true, machine-readable statement of which physical board a build is for.

An earlier pass concluded firmware had no such declaration and would need one
added. It was wrong: the declaration was sitting in the build flags, one level
below where that pass was looking. Reading something the build already depends
on beats adding a parallel field that can drift — and it explains why the
config directory was the wrong key all along:

| build env | config directory | pins header | panel |
|---|---|---|---|
| `canary-display-dash` | `canary-display/dash` | `waveshare-esp32s3-lcd43` | 4.3″ |
| `canary-display-dash-b` | `canary-display/dash` | `waveshare-esp32s3-lcd43b` | 4.3B |
| `canary-display-dash-mic` | `canary-display/dash` | `waveshare-esp32s3-lcd43c` | 4.3C |

Three panels, one config directory between them. The pins column separates
them cleanly.

Each mapped board then names itself, in that same header, next to the pins it
travels with:

```c
#define CANARY_FIGURE_HARDWARE "xiao-esp32c6-mr60"
```

```cpp
if (const auto* me = canary::figures::my_figure()) { … }   // what I look like
```

**Exact about the board is not exact about the product.** One board can carry
several products: the 7″ glass is compiled by both `canary-display-dash7` and
`canary-display-nightstand7`, so a Nightstand 7 asking `my_figure()` gets a
figure whose *shape* is right and whose *title* says "Canary Dash 7". Rows
where that happens set `shared_across_products` and the ledger lists what the
board `serves`; to **name** the product, ask the device type, not the board.
A figure is a picture first.

The generator refuses to emit a figure for a board whose pins header is
missing that line, so the id and the pins can never drift apart. Boards we
can't draw yet (the 4.3B and 4.3C housings, the 1.47″ boards, the Sentinel
line) are listed in the ledger's `hardware.unmapped` with the builds they
cover — a gap you can query, not a silent `nullptr`.

> One candidate that looked right and wasn't: `SECURACV_OTA_PRODUCT`. It's an
> **update channel**, and it deliberately groups `dash-b` with `dash`. Keying
> figures on it would have reproduced the exact bug.

**Both flashers — the `figure` block in `flash.json`.** The picker is where a
wrong picture costs the most: somebody is holding a board and matching it
against a row. So the rule is stricter here than anywhere else — a figure
appears only when the catalog can point at *why*, and `gen_flash.py` records
that reason in `figure.via`:

1. **the pins header the build compiles** — the same load-bearing declaration
   the firmware lookup keys on. Derived; cannot drift.
2. **declared in the product table** — for a build that compiles no `boards/`
   pins header (the WAP's Arduino sketch is the only one today). Checked
   against the ledger, so a renamed figure fails the build rather than
   blanking a row.

The coarse PlatformIO `board` is deliberately **not** a fallback:
`seeed_xiao_esp32s3` alone is shared by the all-rounder Canary, the WAP, a
Vision host and the Watch Station, so resolving through it would put the wrong
device on a row. 11 of 15 flash targets resolve today; the other four show the
placeholder.

**Why the SVG is embedded rather than fetched.** The desktop Flasher bakes
`flash.json` into its binary at build time (`desktop/src-tauri/build.rs`), so a
file on disk is not a channel that reaches it. That is what the **picker tier**
is for: faces sharing a fill merge into one `<path>`, the hairline stroke is
dropped, and coordinates round to 1 decimal — 52% smaller than the glyph tier
for the eight figures the flasher needs. Both frontends then draw with no
network and no file.

**The transition rule.** Every row gets a slot of the same size whether or not
a figure exists, so the list never reflows as figures land for more of the
fleet — a placeholder simply becomes a drawing. The placeholder resembles no
product on purpose: a plausible generic board would be *matchable*, and
somebody matching their hardware against a picture of different hardware is
the single failure this system exists to prevent.

The two flashers share no UI code (`AGENTS.md` rule 7), so a test asserts both
render the slot, both carry the placeholder, and both use the same 46 px box.

**Phone and wrist — `ios/Shared/FleetFigures.swift`.** The same polygons as
flat data a `Canvas` paints at any size. Not an asset catalog and not an SVG
parser. Shared by `SecuraCV`, `SecuraCVWatch` and both widget bundles, so
none of them can drift from each other or from the web.

Beside it sits `FleetFigureBridge.swift` — hand-written, and the only piece of
judgment on the Apple side: which `DeviceType` maps to which figure, and
`DeviceFigureIcon`, which draws the figure at whatever size a row gives it and
falls back to the type's SF Symbol when it can't honestly draw the thing
itself. `.display` is deliberately nil: that enum case covers four different
products, so a figure there would be a coin flip. Same rule as the firmware
table, kept by a test named after it.

```swift
DeviceFigureIcon(witness.deviceType, size: 30)
```

The witness roster and the pairing rows draw it today.

```swift
if let fig = FleetFigure.forDeviceType(witness.deviceType) {
  FleetFigureView(fig).frame(width: 28, height: 28)
}
```

**The revision.** Each figure's `rev` is a content hash of everything that
decides the picture — its solids and the projector revision. Bump the
projector (a new camera, a new palette) and every rev moves, which is exactly
the signal a surface needs to drop a cached drawing.

---

## 8. What this deliberately is not

- **Not a replacement for the showroom or AR.** Real geometry in three.js and
  the `.glb` "view in your room" path answer "what will this look like on my
  wall". Figures answer "which thing are we talking about". Different jobs;
  both stay.
- **Not tilted or filleted.** Axis-aligned massing is the constraint that
  keeps figures readable at 20 px and reproducible byte for byte. The watch
  station's 25° stand is real and is not in its figure.
- **Not a second status vocabulary.** The ladder is derived from
  `registry.json`, `catalog.json` and the tree — it reads those, it doesn't
  compete with them. `CATALOG_ARCHITECTURE.md` remains canonical for
  variants, options and the six axes.
- **Not a claim that every surface draws them yet.** See the status table at
  the top: the bindings are generated and gated, the UIs that consume them
  are a separate change per surface (§10).
- **Not covering all 34 `.scad` files yet.** 44 figures cover the four
  shipping devices and every part you print for them, the bought modules, the
  fit coupon, the two display flavors, and all 16 concepts. The remaining
  accessory enclosures (`canary_hub_din`, `canary_jbox`, `canary_sign`, …)
  have no committed STLs — they're `dev_*.stl`, gitignored on purpose — so
  adding them means either committing CAD or sketching, and sketching a part
  someone might print is the one place a sketch would be dishonest. They come
  when their STLs do.

---

## 9. Where the numbers come from, for a bought-in board

A board is somebody else's product, so "committed STLs + our firmware + our
catalog" cannot apply to it. What *can* be checked on disk is whether we have
pinned the exact orderable part and committed its geometry, and
`canary-local/devices/boards.json` holds both:

- **Dimensions** come from the committed board mesh, read at build time —
  never retyped, exactly like an STL. `dims_source: "board-cad"`.
- **The rung** is earned the same way everything else's is: `shipping` needs a
  manufacturer part number *and* a committed mesh; anything less is
  `confirmed` ("the design names it, one proof is missing"). The first cut of
  this forced every board to `shipping` from its role alone, which asserted an
  availability the ledger could not substantiate.
- The evidence records `mesh_committed` and `vendor_step` **separately**. Most
  boards have both; at least one committed mesh was built without a vendor
  STEP to tessellate. The mesh is what the dimensions are read from; the STEP
  is where the mesh came from. They are two facts.

A board with no committed mesh (the MR60BHA2 today) falls back to a `sketch`
with a note saying what the sketch is based on, and sits at `confirmed`.

---

## 10. Wiring a surface up

The bindings exist; making a surface *use* one is a change in that surface's
own render path, and belongs in that surface's own review:

- **The glass.** `#include "core/fleet_figures.h"` in the display firmware,
  then draw the figure beside a witness in the roll call. The lookup is wired
  and every mapped board already names itself, so what's left is the renderer:
  a device that can't draw polygons can still use the *ids* to pick a bitmap.
  Costs flash for the two tables (~14 rows of `constexpr` pointers).
- **The emulator.** It already loads from `canary-local/`; the SVGs sit next
  to what it reads.

Each wants its own before/after and its own visual check, which is why neither
rides along with the generator. The Apple side is **done** — see §7.

---

## 11. Adding a figure

1. Add an entry to `FIGURES` in `canary-local/tools/figures/massing.mjs`. Give
   it the committed STL it is (`stl:` + `frame: 'scad-wall'`), or a `sketch`
   envelope if there is no CAD yet.
2. Write `build(E)` against the envelope the generator hands you — in the
   figure frame, already swapped out of the SCAD's print orientation. Use
   `slab`/`onFace`/`panel` where they fit; hold stacked elements proud by
   `EPS`.
3. Run the generator. It will tell you if the massing has drifted from the
   part, or if you put two materials on one plane.
4. Run `node --test canary-local/tests/figures.test.js`.
5. Commit the generated files with the change. They are checked in on
   purpose — the same contract the `.glb` models keep — so the surfaces don't
   need a build step.

A concept device needs no figure entry at all: add its proportions to
`CONCEPT_MASSING` and it becomes a ghost automatically. The test suite will
tell you when a new concept lands in `registry.json` without one.
