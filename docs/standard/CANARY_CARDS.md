# Canary Cards — the shared widget-card schema — draft v1

> Status: **DRAFT v1**. Reference implementation:
> [`canary-local/assets/canary-cards.js`](../../canary-local/assets/canary-cards.js)
> (renderer + validator + the canary-sense card set), exercised live on the
> Sense Lab ([`canary-local/sense.html`](../../canary-local/sense.html)) and
> validated in CI by `canary-local/tests/sense.test.js`.
> Companion standard: [`AMBIENT_DISPLAY_STANDARD.md`](./AMBIENT_DISPLAY_STANDARD.md)
> (which constrains *behavior*; this document constrains the *data shape*).

## 0. Why cards

Every new peripheral so far has grown its own UI three times: once on the
canary-display glass (`dash_ui.cpp` / `glance_ui.cpp`), once on the device's
own web mirror (`glass_web.cpp` → `mirror_html.h`), and once in whatever
dashboard or app is watching. The fleet model already solved the *data* side
of this — any device that speaks the wire vocabulary appears as a generic
`Witness` — but presence buckets, range bands, breathing locks and BPM
numerics have nowhere type-aware to land, so today they get squeezed into
`last_event` strings or dropped on the floor
(`firmware/projects/canary-display/include/canary/fleet/fleet_model.h`).

Home Assistant solved this problem a decade ago: **devices announce
entities; surfaces render entities through a fixed card vocabulary.** Canary
Cards borrows exactly that move for the flock:

- a peripheral publishes the **same MQTT entity set it already announces to
  Home Assistant** (its `ha_discovery.cpp` is the single source of truth);
- every surface — the teaching bench, the display's dev mirror, the glass
  itself, the companion app — maps *one entity to one card* using the small
  kind vocabulary below;
- a brand-new peripheral therefore gets a complete, consistent UI on every
  surface by writing **zero** new UI code. The entity list *is* the UI.

## 1. The card descriptor (schema v1)

A card is a flat, JSON-serializable object. Field order is free; unknown
fields MUST be ignored (forward compatibility).

```jsonc
{
  "v": 1,                  // schema version, REQUIRED
  "id": "presence",        // REQUIRED lowercase slug — the HA object_id suffix
  "kind": "binary",        // REQUIRED, one of §2
  "title": "Presence",     // REQUIRED human label
  "icon": "radar",         // OPTIONAL icon id (§4); surfaces may substitute
  "privacy": "P0",         // OPTIONAL privacy class chip: P0 | P1 | P2
  "severity": "ok",        // OPTIONAL ok | notice | warn | alert
  "footer": "…",           // OPTIONAL one-line provenance / honesty note
  "absent": false          // OPTIONAL true = entity compiled out of this build
  // …plus per-kind fields, §2
}
```

`id` MUST equal the suffix of the HA `unique_id` / `object_id` the device
announces (e.g. `presence`, `range_band`, `heart_rate`) — that is the
join key that keeps a card, an HA entity, and a state-topic field the same
thing. The invariant, in one line:

> **A surface renders cards for exactly the entities the device announces —
> no more (no invented data), no fewer (no silently dropped signals) —
> except entities whose discovery payload is compiled out, which MAY render
> as `absent: true` "provably not in this build" cards.**

That last clause is load-bearing for the privacy story: a presence-only
canary-sense build doesn't just *lack* BPM cards — a surface can show that
the BPM entities are structurally absent, the same promise
`firmware/projects/canary-sense/README.md` makes about discovery payloads.

## 2. Card kinds (v1)

| kind | extra fields | renders as | example entities |
|---|---|---|---|
| `binary` | `state: bool\|null`, `onLabel`, `offLabel` | state pill, severity-colored | presence, radar_link, breathing |
| `stat` | `value: number\|null`, `unit` | big number + unit | frame_errors, illuminance, heap_free |
| `band` | `options: string[]`, `value: string\|null` | ordered slots, current lit | occupants (0/1/2+), range_band (near/mid/far) |
| `sparkline` | as `stat` + `trend: number[]` | stat + short trend ring | breath_rate, heart_rate, rssi |
| `event` | `value: string\|null`, `signed: bool\|null` | last event + signature state | last_event |
| `trust` | `chain: number`, `badge: verified\|signed\|unsigned\|failed\|unknown` | badge + chain length | the witness chain head |

`null` values render as "—" (unknown), never as zero: a stalled radar is
*unknown presence*, not *no presence* — the same honesty rule the firmware's
FSMs encode (AD-Core §2.1, "silence ≠ safety").

Vocabulary discipline for `trust.badge` follows AD-Core §2.5 (**no
overclaiming**): `verified` only after an actual signature verification on
the rendering surface; a merely well-formed signature is `signed`.

## 3. Where card data comes from

Nothing in this schema invents a transport. Cards are a *projection* of
data the wire vocabulary already carries
(`docs/standard/AMBIENT_DISPLAY_STANDARD.md` §6):

| card field | source |
|---|---|
| which cards exist | the device's HA discovery entity set (`ha_discovery.cpp`) |
| `state` / `value` | the retained `…/state` topic's JSON fields (same `value_template` fields HA reads) |
| `event.value`, `event.signed` | the `…/events` payloads |
| `trust.chain`, `trust.badge` | the retained `…/chain` topic + the surface's own verification |
| `absent` | entity missing from the build's discovery set |
| `privacy` | the device's privacy-class table (design doc §2) |

A surface that already consumes the fleet wire vocabulary (the display's
`fleet_model`, the wasm emulator, an MQTT dashboard) therefore has
everything it needs to build cards — this schema adds no topics and no
payload fields.

## 4. Icons

v1 ships a minimal id vocabulary (`radar`, `people`, `ruler`, `lungs`,
`heart`, `sun`, `link`, `alert`, `shield`, `clock`, `chip`, `pulse`) with
reference 24×24 paths in `canary-cards.js`. Surfaces MAY substitute their
own glyphs by id (LVGL surfaces will); unknown ids fall back to `chip`.

## 5. Surface guidance

- **Web (reference)**: `renderCardGrid(descs)` in `canary-cards.js` —
  Quiet Glass styling, `.ccard-*` classes in `canary-local.css`.
- **canary-display glass**: the fixed `Card` struct in `dash_ui.cpp` is the
  natural landing zone — one LVGL renderer per kind (six small functions),
  driven by a per-device-type entity table instead of today's hardcoded
  field list. The fleet model needs no change to carry v1 binary/band data
  it already parses; `stat`/`sparkline` values ride the state topic it
  already subscribes to.
- **glass_web mirror**: the `/api/glass` witness objects extend naturally
  to a `cards: [...]` array — the mirror page then renders any sibling with
  the same ~200-line renderer this repo ships.
- **Companion app / phone**: same JSON, native styling.

Conformance is behavioral, not pixel-level: kinds may render differently
per surface, but `null`-as-unknown, severity semantics, `absent` honesty,
and the one-entity-one-card invariant MUST hold.

## 6. Versioning

`v` is a monotonic integer. A consumer MUST ignore cards with a `v` it
does not understand, and MUST ignore unknown fields within a `v` it does.
Additions that only add optional fields do not bump `v`.
