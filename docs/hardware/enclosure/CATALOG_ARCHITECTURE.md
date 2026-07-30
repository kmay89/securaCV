# Enclosure catalog architecture — versions, flavors, options & how a user finds the right case (2026-07)

How the Canary case catalog should model **versions / flavors / options /
alternatives / remixes**, and how a person goes from *"I have device X (in
environment Y, mounted Z)"* to **one correct printable case + its parts**. This
is an architecture spec — no code changes; it names the target model, maps the
(real, grounded) current state onto it, and gives a phased migration.

Companion inventory it's built on: the 29 `.scad` option axes and the
generator/UI plumbing (`gen_enclosures.py`, `enclosures.json`, `workshop.json`,
`enclosure-lab.js`, `workshop.js`) in this directory and `canary-local/`, plus
the website showroom (`securacv_website/js/showroom.js`).

---

## 1. Why this exists — the problem in one paragraph

The option data already exists, but it's **flat and scattered across five
schemas that don't share an identity**, and **no surface answers "which case do
I need?"**. "Product" is described independently by: `showroom.js PRODUCTS` (5
hand-authored, geometry live but metadata retyped), `registry.json` (24 devices
with `family`/`kind`), `enclosures.json` (~36 `sets[]` — the real variant data,
via a nullable `device` FK + a read-only `scads{}` param map), `workshop.json`
(the *only* real option picker — but 5 devices, `opt_*`+`mount_style` only),
`store.json` (indoor/outdoor SKUs, no link field) and `builds.json` (`remix_of`
— the one explicit remix relationship anywhere). "Registered" means four
different things; status is triplicated in three vocabularies; joins are
free-text device names. The result: a flat 5-item gallery with one global color
swatch, while the genuine variant/option model sits unsurfaced in the other repo.

**The fix is not a new taxonomy invented from scratch** — it's: (a) one
canonical identity, (b) lift the axes that already live in the SCADs into a
single generated manifest, (c) a guided selection funnel over it, (d) make the
showroom read the manifest instead of hand-copying it.

---

## 2. The one idea: six axes, not one list

The catalog conflates six *different kinds* of choice into peer list rows. They
differ by **what they change** and **how they cascade**. Naming them is 80% of
making it make sense.

| # | Axis | Definition | Changes… | Example (today) |
|---|---|---|---|---|
| 1 | **Use-case / device** | what the user *has* or wants to sense | nothing yet — it **filters** | `registry.json` `family`+`kind` (24 devices) |
| 2 | **Product / case family** | one designed enclosure (≈ one `.scad`) | the base geometry | `canary_vision_enclosure.scad` |
| 3 | **Variant / flavor** | discrete, **changes the printed part set** | *which parts you print* | `preset=vision_indoor\|vision_weather`; `host=xiao\|devkit`; board `model=1.47\|1.69` |
| 4 | **Option** | a toggle/enum inside a variant | maybe form, maybe just BOM | `opt_seal`, `opt_stand`, `mount_style=keyhole\|tabs\|both` |
| 5 | **Fit** | print-tolerance tier | nothing structural | `tol_slide/press/hole` + the fit coupon |
| 6 | **Remix / alternative** | a related-but-*different* thing | a different design | `builds.json remix_of`; "Hammond box **or** printed pod" |

### The cascade rules (this is the "top-level vs sub-level" answer)

- **1 & 2 are top-level; 3 & 4 are sub-level.** A top-level pick **constrains
  the set** of sub-level choices (McMaster-style narrowing): choosing *outdoor*
  hides indoor-only flavors **and force-requires** `opt_seal`+vent — a cascade,
  not independent checkboxes.
- **A variant (3) redefines the option set (4).** A `stand` exists only on desk
  flavors; a pole-mount only outdoors. So **options are per-variant, not
  global** — today they're global `opt_*` flags with no scoping, which is why
  invalid combinations are expressible.
- **Options (4) carry a dependency graph** (`requires` / `excludes`) — OpenSCAD's
  "interconnected constraints" idea, surfaced live so a user can't build an
  invalid case (e.g. `opt_seal ⇒ gasket part + vent`).
- **Fit (5) is orthogonal** — it applies to *any* selection; it belongs to the
  printer, not the product. One shared fit-tier + the coupon, not per-file
  copy-paste.
- **Remix (6) is sideways** — a "see also / alternative approach" link, never a
  step in the narrowing funnel.

A single flat `sets[]` list (today) can express none of these relationships —
that's the root cause.

---

## 3. The permutation matrix (grounded)

Every model's *real* axes, from the SCAD sources. "Variant axes" are discrete
part-set changers (axis 3); "Options" are toggles (axis 4). ⚠ = an axis that's
already **captured in `enclosures.json`'s read-only param map but surfaced in no
picker today** (see §5 for why — the workshop transform drops it).

| Model | Top (device) | Variant axes (3) | Key options (4) | Env |
|---|---|---|---|---|
| `canary_wap_enclosure` | canary-wap | `preset` {custom, battery_full, compact_plain, battery_weather} | camera·buzzer·led·battery·gps·tamper·touch·antenna·**seal**·usb_cover·`mount_style`{keyhole,tabs,both} | ~IP54 (weather) |
| `canary_vision_enclosure` | canary-vision | `preset`{custom,indoor,weather} · ⚠`host`{xiao,devkit} | led·buzzer·vent·tamper·hood·seal·`mount_style`{hinge,keyhole,both} | ~IP54 |
| `canary_sense_enclosure` | canary-sense | ⚠`radar`{bha2,fda2} | led·lux·vent·tamper·seal·`mount_style`{hinge,keyhole,both} | radome |
| `canary_vision_doorbell` | canary-vision | — | seal·vent·led·tamper | ~IP54 / button IP65 |
| `canary_c6_display` | display-watch | ⚠`model`{1.47,1.69} board | btn·keyhole·vent | — |
| `canary_s3_lcd7` | display-dash | — | bottom_ports·side_ports·vent_back·vent_sides·**stand** | hot→PETG/ASA |
| `canary_s3_touch169` | display-watch | — | usb·btn·side·keyhole·vent·**stand** | — |
| `canary_dash_display` | display-dash | — | term_open | — |
| `canary_watch_station` | display-watch | — | ⚠batt | — |
| `canary_field_case` | *(universal)* | — | bez_on | **CER-4 · IP67 · MIL-810H** *(design intent — untested; do not claim before the [field_ratings.md](./field_ratings.md) protocol)* |
| `canary_relay_solar` | *(universal)* | — | seal | **CER-2→3** |
| `canary_combo` | canary-vision | — | seal·hood·led·mount | — |
| `canary_hammond_chassis` | *(universal)* | ⚠`stack`{wap,vision,sense} | tie_slots | IP66/67 (bought box) |
| `canary_vision_pro_mount` | canary-vision | ⚠`mount`{tripod,magnet,both} | — | — |
| `canary_dock` | *(universal)* | ⚠`variant`{bare,sense} | — | — |
| `canary_templates_2d` | *(universal)* | ⚠`mode`{studs,bracket,doorbell} | — | 2D |
| `canary_sense_stand` · `canary_sense_gang` · `canary_outlet_cradle` · `canary_hub_din`{din} · `canary_jbox`{camera,led} · `canary_sign`{screws} · `canary_mount_adapters` · `canary_vehicle_mount` · `canary_wear_clip` · `canary_fit_coupon` · `canary_bench_fixture` · `canary_inserts` · `canary_shop_tools` | *(universal / accessory)* | mostly none | assorted `opt_*` | mixed |

> **Env values are design intent, not measured.** Every CER/IP/MIL figure above
> is a *target* pending the [`field_ratings.md`](./field_ratings.md) test
> protocol — none has been verified. When §5 promotes `env` to a structured,
> badgeable field it **must** carry a `verified: true|false` (or
> `status: target|tested`) so a catalog badge can never present an unearned
> ingress/drop rating as achieved.

**The headline defect this exposes:** the **same logical axis — "which display
board" — is encoded three ways**: a `model=` preset inside `canary_c6_display`,
*and* separate whole files (`canary_s3_touch169`, `canary_s3_lcd7`), *and* a
fourth file (`canary_watch_station`). And six real variant axes
(`host`/`radar`/`stack`/`model`/`mount`/`variant`/`mode`) are **⚠ invisible** —
the catalog only harvests `opt_*`-prefixed booleans + one hardcoded
`mount_style`, so those flavors can't be picked anywhere.

---

## 4. Canonical identity (the precondition for everything)

Nothing works until five id-spaces collapse to one. Define a **canonical
`device_id`** (the `registry.json` ids) and require every other record to key to
it:

| Record | Today's key | Fix |
|---|---|---|
| `registry.json` device | `id` (24) | **canonical** — the id space |
| `enclosures.json` set | `device` FK (nullable) | keep; `null` → explicit `device: "_universal"` (a type, not a hole) |
| `workshop.json` device | `id` (5) | must equal a `registry.id` |
| `showroom.js` PRODUCTS | hand `id` (5) | **delete** — derive from the manifest (§6) |
| `store.json` SKU | display name | add `device_id` + `variant_id` |
| `builds.json` build | free-text `device` | add `device_id`; keep `remix_of` |

Add the **forward link** a picker needs: `device → [variants]` (today only the
reverse `set.device` exists). It's derivable from the FK, but the manifest
should materialize it so the UI doesn't scan.

---

## 5. The generated manifest — one source of truth

Replace the flat `sets[]` with a **3-tier manifest**, still **generated** (so it
can't drift from the SCADs), emitted from `gen_enclosures.py`. Note *which* layer
does what today, because it's easy to target the wrong one: `parse_scad()`
already captures the **complete** parameter inventory (every `name = value;`
under a `/* [group] */` block, with enums and ranges) — that's why
`enclosures.json`'s `scads{}` map **already contains** `host`/`radar`/`stack`/
`model`/`mount`/`variant`/`mode`. The `opt_*` filtering that hides those axes
happens **downstream**, in `scad_options()`, the transform that builds the
`workshop.json` picker. So Phase 2 does **not** touch `parse_scad` (the raw
inventory is already complete); it **replaces/extends the picker-facing transform
(`scad_options`)** — or adds a new manifest emitter beside it — to promote the
enum axes into variant selectors instead of dropping everything that isn't
`opt_*`.

```jsonc
// catalog.json  (Product → Variant → Option), generated
{ "products": [{
  "id": "vision", "scad": "canary_vision_enclosure.scad", // ← DEFAULT source; a variant may override (see below)
  "device_compat": ["canary-vision"], "family": "canary-vision",
  "env": { "cer": 2, "ip": "~IP54(weather)", "verified": false }, // ← NEW: rating as a field; verified flag (never badge a target as achieved)
  "fit_tier": "standard",                              // ← NEW: enum, bound to the coupon
  "variants": [                                        // axis 3 — discrete, change the part set
    { "id": "xiao-indoor", "for": "stacked XIAO, desk/shelf",
      "selects": { "host": "xiao", "preset": "vision_indoor" },
      "parts": ["back","front","bracket","knob"],
      "valid_options": ["led","mount_style"],          // ← options SCOPED to this variant
      "status": "released", "preview": "…png", "meshes": ["…stl"] },
    { "id": "xiao-weather", "selects": {"host":"xiao","preset":"vision_weather"},
      "parts": ["back","front","gasket","bracket"], "requires": ["seal"], … } ],
  "options": [                                         // axis 4 — with a dependency graph
    { "id": "seal", "label": "Weather seal", "enum": false,
      "requires_parts": ["gasket"], "requires": ["vent"], "bom": ["G1"], "fw": [] },
    { "id": "mount_style", "enum": ["hinge","keyhole","both"], "default": "hinge" } ],
  "remix_of": null, "alternatives": ["hammond-chassis"] // axis 6 — sideways
}]}
```

**Variants carry their own source/render recipe.** The product-level `scad` is
only a *default*. Some products collapse variants that come from **different
files** — the display family is the live example: "which display board" today is
four separate SCADs (`canary_c6_display.scad`, `canary_s3_touch169.scad`,
`canary_s3_lcd7.scad`, `canary_watch_station.scad`). If a variant only held
`selects{}` against one product source, the generator could never locate or
render the other three. So a variant may set its own `scad` (+ `selects`,
`preview`, `meshes`); it inherits the product `scad` only when absent. This is
the schema half of §7 row 1 — the file split becomes an internal detail
*because* the recipe rides on the variant, not a single product source:

```jsonc
{ "id": "board-1_69", "for": "ESP32-S3-Touch-LCD-1.69 watch",
  "scad": "canary_s3_touch169.scad",   // ← overrides the product default source
  "parts": ["front","back"], "meshes": ["…stl"], "status": "released" }
```

Key moves vs today: **variants become first-class** (not peer `sets[]` rows);
**options are scoped to variants** and carry `requires`/`excludes`; the
**invisible enum axes** (`host`/`radar`/`stack`/`model`/`mount`) are captured as
variant selectors instead of dropped; **env-rating and fit-tier are fields** (so
the lab can filter/badge by them); **remix/alternatives are links**. The
`user-facing vs engineering` split is fixed by an explicit tag, not the leaky
`opt_` prefix (so `hinge_teeth`/`usb_cover` stop being silently dropped and
`lid_ribs` stops masquerading as a user option).

---

## 6. The selection experience (how a user picks the right case)

Three complementary entries over the manifest — mirroring the patterns you
pointed at — plus the model-page configurator and the remix rail.

**A. Guided funnel** *(McMaster-Carr / car-configurator "guided build")* — the
"which case do I need?" path that's missing entirely today. 2–4 questions,
each narrowing the next:
> **What are you building?** (device/peripherals) → **Where does it live?**
> (indoor / weather / field → sets `env` + force-requires seal/vent) →
> **Mounted how?** (desk/wall/pole/hinge → picks `mount_style`, adds `stand`)
> → **result:** one product + variant, sensible options pre-checked, the fit
> coupon offered, a parts list to print.

**B. Faceted browse** *(Printables / Thingiverse browse)* — the gallery, but the
flat pill list gains facets that narrow: device · env/CER · mount · has-stand ·
released-vs-dev · printable-parts. For people who'd rather look than answer.

**C. Model page = live configurator** *(OpenSCAD Customizer)* — the SCAD params
*become the controls* (they're auto-generated already): variant **tabs**
(flavors) + option **toggles/selects** with **constraint feedback** ("this adds
the gasket part; requires vent") + a **fit selector** + grouped **print files**
per variant (Printables-style). This is exactly what `workshop.js` does for 5
devices — generalize it to all via the manifest, and fold the read-only
"what this can adapt to" text in `enclosure-lab.js` into *actual* inputs.

**D. Remix / alternatives rail** *(Thingiverse remix DAG — ~half of all models
are remixes)* — a sideways "see also": alternates (`field_case` ↔
`hammond_chassis` ↔ `relay_solar` for "outdoor witness"), community `remix_of`
builds, "others also printed." Never inside the funnel.

**Cascade in the UI:** top choices (A's questions) **disable/relabel** lower
controls (C's toggles) live; a variant tab swap **swaps the valid option set and
the part list**. That live constraint propagation is the thing that turns "a
pile of flags" into "it makes sense."

---

## 7. How this resolves each current inconsistency

| # | Today | Resolved by |
|---|---|---|
| 1 | board variant = preset **or** separate file **or** third file | one **variant axis** in the manifest, **each variant carrying its own `scad`/render recipe** (§5); the file split becomes an impl detail, not a user-visible fork |
| 2 | `host/radar/stack/model/mount/variant/mode` invisible | manifest captures **all** enum axes as variant selectors, not just `opt_*` |
| 3 | "preset" overloaded (overrides toggles in some, absent in others) | preset = a **named variant** that sets a known option vector; uniform |
| 4 | universal = nullable `device` | `device:"_universal"` **type** + facet |
| 5 | options split across enclosures.json (read-only) vs workshop.json (5 devices) | **one manifest**, options first-class for all models |
| 6 | eng vs user toggles undistinguished (`opt_` prefix leaks) | explicit `audience: user\|engineering` tag |
| 7 | fit tolerances copy-pasted, drifting, no coupon link | **`fit_tier` enum** bound to the coupon; values centralized |
| 8 | CER/IP/MIL only as header prose | **`env` field** → filterable/badged, **with a `verified` flag** so a target rating never badges as achieved (field case stays *untested* per `field_ratings.md`) |
| 9 | previews are a magic 3-model subset | manifest marks `preview: none` explicitly (a known gap, not phantom-missing data) |

Plus the cross-repo win: **the showroom stops hand-copying** 5 products and
renders from the manifest — its geometry is already live; only the metadata was
duplicated.

---

## 8. Migration (phased — each phase ships value alone)

1. **Identity + fields (data only).** Canonical `device_id` across the five
   files; add `env`, `fit_tier`, `audience`, and `device:"_universal"`. No UI
   change; unblocks everything.
2. **Generate the manifest.** ✅ **Done** — `catalog_main()` in
   `canary-local/tools/gen_enclosures.py` emits
   `canary-local/devices/catalog.json`: one product per `.scad`, the enum axes
   (`host`/`radar`/`stack`/`model`/`variant`/`mode`/`mount`) promoted to
   first-class **variant selectors** (never `part` or `mount_style`), committed
   sets as **variants** (with a per-variant `scad` override hook), `opt_*` +
   `mount_style` **options** carrying `requires`/`excludes`/`requires_parts`, the
   **`env`** field with an honest `verified` flag, and one shared **`fit`** tier
   bound to the coupon. `parse_scad` was left untouched (its inventory was
   already complete). Guarded by the same CI diff gate as the other emitters plus
   `canary-local/tests/catalog.test.js`; `enclosures.json`/`workshop.json` remain
   the live views. It also self-reports coverage (`engineering_param_count`) so
   the manifest never pretends to surface knobs it drops.
3. **Unify the two pickers.** ✅ **Done.** ✅ **`enclosure-lab.js` done** —
   the lab now reads `catalog.json` (loaded by `app.js`) and renders a live
   configurator in place of the read-only param text: variant-axis selectors
   that jump to the matching committed STL (or flag a custom combo), user-option
   toggles/enums with live `requires`/`excludes`/`requires_parts` feedback,
   engineering knobs kept in a collapsed read-only block, and an OpenSCAD
   parameter export. Options come from `option.audience`, not the `opt_` prefix,
   so non-prefixed user options finally show. Guarded by
   `canary-local/tests/catalog_lab.test.js`. ✅ **Faceted browse done (§6B)** —
   a new `catalog.html` / `catalog-browse.js` renders a gallery over **all** 29
   manifest products with facets that narrow (family · fits-device · environment
   · status · type · has-options), plus per-card env badges (honest "target",
   never verified), sideways `alternatives` links, and "configure in the
   workshop" / "open in OpenSCAD" handoffs. This is the "generalize to every
   product" path the per-device workshop can't give (it only configures the five
   devices with firmware + a BOM). Guarded by
   `canary-local/tests/catalog_browse.test.js`. ✅ **`workshop.js` done** — its
   configurator now (a) classifies options by **audience, at the source**, not
   the `opt_` name prefix: the generator's `scad_options` shares one
   `is_user_option` predicate with `catalog_options` (a bool/enum that isn't a
   variant selector, the `part` render selector, or an engineering toggle), and
   `workshop.js`'s `hasConfigurator`/`optionVector`/seed filter on the resulting
   `audience` tag — so a non-`opt_` user bool on a configurable device would
   surface just the same. (For the five workshop devices the emitted set is
   unchanged — their user options are all `opt_*` + `mount_style`; their
   non-`opt_` bools are all engineering — so `workshop.json` is byte-identical
   and the strict `workshop_probe` stays green.) And (b) it reads `catalog.json`
   to show each device's manifest facts beside its packages — environment rating
   (design intent, never "verified") · flavors · **the options it can actually
   configure** (the workshop's own count, never the catalog's larger inventory —
   the workshop can't build an accessory enclosure) · see-also. The literal
   "configure all 29 products in the workshop" is **intentionally not pursued**:
   the workshop is a per-device build tool (it needs a BOM + firmware, which the
   accessory/display enclosures don't have), so the faceted browse (§6B) is the
   manifest-driven surface for every product it doesn't build. Guarded by the
   audience assertion in `canary-local/tests/workshop.test.js`.
4. **Add the guided funnel (§6A) + remix rail (§6D).** ✅ **Done** — a new
   `find.html` / `catalog-funnel.js` asks up to three questions (what you're
   building → where it lives → how you mount it), each narrowing the next over
   `catalog.json`, and resolves to **one** recommended case with its variant +
   options pre-checked (weather/field → seal + vent; desk → stand), the fit
   coupon offered, and a sideways **remix rail** (§6D: `alternatives` + the
   runner-up matches, never inside the funnel). It's honest about relaxation —
   e.g. a rugged vision build has no vision-specific field case, so it falls back
   to the device-agnostic carriers (`field_case`/`hammond`/`relay_solar`) and
   says so. Guarded by `canary-local/tests/catalog_funnel.test.js`.
5. **Make the showroom manifest-driven.** ✅ **Done** — the website showroom
   (`securacv_website/js/showroom.js`) now fetches `catalog.json` from the **same
   pinned commit** it pins every STL/GLB to and derives product **status** from
   the manifest's variant statuses (the five hand-typed `status` strings are
   deleted), plus an honest catalog line (env as *design intent, not verified* ·
   variants/flavors · options · sideways alternatives) and a drift guard when a
   referenced `.scad` isn't in the manifest. The 3D **scene/choreography** stays
   hand-authored — the manifest has no scene data, so `PRODUCTS` keeps the parts/
   positions/explode/camera, and only the retyped metadata moves to the manifest.
   Pure logic in `js/showroom-catalog.mjs`, gated by
   `tests/showroom-catalog.test.mjs`.

Sensible stop points: after 1–2 the catalog is *correct and complete* even
without new UI; after 3 the option story is coherent; 4–5 are the delight layer.
**All five phases have landed.** The one scope call worth stating out loud: the
per-device `workshop.js` builds only the five devices with a BOM + firmware, so
"configure *every* product" is served by the faceted browse (§6B), not by
forcing accessory enclosures into a build tool that has no parts list for them.

---

## 9. Status & what's next

**All five phases have landed.** The manifest is real and generated
(`canary-local/devices/catalog.json` — run
`python3 canary-local/tools/gen_enclosures.py` to rebuild), and it drives the UI
end to end: the enclosure lab's live configurator (§8.3), the faceted browse
(§6B, `catalog.html`), the guided funnel + remix rail (§6A/§6D, `find.html`), the
`workshop.js` configurator's audience-based options + manifest facts (§8.3), and
the website showroom's status/facts (§8.5, `securacv_website`). Each was its own
CI-gated, drift-checked PR.

**One deliberate scope call, not a gap:** the per-device `workshop.js` builds
only the five devices that have a BOM + firmware, so "configure *every* product"
is the faceted browse's job (§6B), not the workshop's — the workshop has no parts
list for an accessory enclosure. Both pickers now read the manifest and classify
options by `audience`, not the `opt_` prefix, so Phase 3 is closed.

Two honest gaps the manifest still marks rather than hides: `env` ratings come
from a small curated overlay in the generator (there is no parseable rating
source yet — a source-annotation pass would move them into the SCAD/README), and
`remix_of` is `null` everywhere until a `builds.json` remix source exists.
