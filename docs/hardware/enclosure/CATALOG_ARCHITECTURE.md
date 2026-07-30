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
part-set changers (axis 3); "Options" are toggles (axis 4). ⚠ = an axis that
exists in the SCAD but **is invisible to the catalog today**.

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
| `canary_field_case` | *(universal)* | — | bez_on | **CER-4 · IP67 · MIL-810H** |
| `canary_relay_solar` | *(universal)* | — | seal | **CER-2→3** |
| `canary_combo` | canary-vision | — | seal·hood·led·mount | — |
| `canary_hammond_chassis` | *(universal)* | ⚠`stack`{wap,vision,sense} | tie_slots | IP66/67 (bought box) |
| `canary_vision_pro_mount` | canary-vision | ⚠`mount`{tripod,magnet,both} | — | — |
| `canary_dock` | *(universal)* | ⚠`variant`{bare,sense} | — | — |
| `canary_templates_2d` | *(universal)* | ⚠`mode`{studs,bracket,doorbell} | — | 2D |
| `canary_sense_stand` · `canary_sense_gang` · `canary_outlet_cradle` · `canary_hub_din`{din} · `canary_jbox`{camera,led} · `canary_sign`{screws} · `canary_mount_adapters` · `canary_vehicle_mount` · `canary_wear_clip` · `canary_fit_coupon` · `canary_bench_fixture` · `canary_inserts` · `canary_shop_tools` | *(universal / accessory)* | mostly none | assorted `opt_*` | mixed |

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
can't drift from the SCADs), emitted by extending `gen_enclosures.py`'s existing
`parse_scad`/`scad_options` (it already reads `/* [group] */` params, enums and
ranges — the machinery exists; it just filters to `opt_*` and drops the rest).

```jsonc
// catalog.json  (Product → Variant → Option), generated
{ "products": [{
  "id": "vision", "scad": "canary_vision_enclosure.scad",
  "device_compat": ["canary-vision"], "family": "canary-vision",
  "env": { "cer": 2, "ip": "~IP54(weather)" },        // ← NEW: rating as a field, not header prose
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
| 1 | board variant = preset **or** separate file **or** third file | one **variant axis** in the manifest; the file split becomes an impl detail, not a user-visible fork |
| 2 | `host/radar/stack/model/mount/variant/mode` invisible | manifest captures **all** enum axes as variant selectors, not just `opt_*` |
| 3 | "preset" overloaded (overrides toggles in some, absent in others) | preset = a **named variant** that sets a known option vector; uniform |
| 4 | universal = nullable `device` | `device:"_universal"` **type** + facet |
| 5 | options split across enclosures.json (read-only) vs workshop.json (5 devices) | **one manifest**, options first-class for all models |
| 6 | eng vs user toggles undistinguished (`opt_` prefix leaks) | explicit `audience: user\|engineering` tag |
| 7 | fit tolerances copy-pasted, drifting, no coupon link | **`fit_tier` enum** bound to the coupon; values centralized |
| 8 | CER/IP/MIL only as header prose | **`env` field** → filterable/badged |
| 9 | previews are a magic 3-model subset | manifest marks `preview: none` explicitly (a known gap, not phantom-missing data) |

Plus the cross-repo win: **the showroom stops hand-copying** 5 products and
renders from the manifest — its geometry is already live; only the metadata was
duplicated.

---

## 8. Migration (phased — each phase ships value alone)

1. **Identity + fields (data only).** Canonical `device_id` across the five
   files; add `env`, `fit_tier`, `audience`, and `device:"_universal"`. No UI
   change; unblocks everything.
2. **Generate the manifest.** Extend `parse_scad`/`scad_options` to emit the
   3-tier `catalog.json` (variants first-class, all enum axes, scoped options +
   `requires`/`excludes`). Keep `enclosures.json`/`workshop.json` as derived
   views during transition. Guard it with the existing drift tests.
3. **Unify the two pickers.** Fold `enclosure-lab.js`'s read-only param text into
   real inputs; generalize `workshop.js`'s configurator to every product via the
   manifest. Add facets (§6B).
4. **Add the guided funnel (§6A) + remix rail (§6D).**
5. **Make the showroom manifest-driven** — delete the hand `PRODUCTS` array;
   render products/variants/status/choreography from the manifest at the pinned
   SHA (it already fetches from the repo).

Sensible stop points: after 1–2 the catalog is *correct and complete* even
without new UI; after 3 the option story is coherent; 4–5 are the delight layer.

---

## 9. Open decision for the owner

This spec is deliberately **model-and-UX only**. The immediate buildable next
step is **Phase 2** — stub the extended `gen_enclosures.py` so `catalog.json` is
runnable and the manifest is real (not just described). Say the word and that's
the next PR; otherwise this stands as the reference the incremental work maps to.
