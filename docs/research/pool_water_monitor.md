# Research — pool water-chemistry monitor node (2026-07)

Feasibility + options for a **`canary-pool`** node: an outdoor ESP32 that reads
pool chemistry (pH first; ORP / temp / salinity next), publishes samples to the
fleet, and surfaces them indoors — continuous graphs in Home Assistant and an
at-a-glance "pool OK?" card + threshold alerts on the Dash.

> **Scope note (this is a research plan, not a built feature).** A `canary-pool`
> fork inherits the fleet's signed-chain *machinery*, but three things called out
> below are **net-new firmware work, not free**: (a) retained `state` samples are
> **not individually signed** — tamper-evidence for chemistry needs the samples
> folded into the signed chain or periodic signed digests (§6); (b) Dash cards
> and (c) threshold→event alerts both require Dash-side + node code (§6). The
> earlier drafts overstated these as "zero-change"; corrected here.

> **Prices are July-2026 USD list/street, agent-gathered — verify before
> purchase, they drift.** Each row keeps one representative source. This is a
> reference, not a locked BOM.

The user goal that shapes everything below: **6 months between recalibrations**,
outdoor capture enclosure, cost + reliability, and "capture more if we can."

---

## 1. The hard part: pH calibration stability (the 6-month bar)

pH is the fussy parameter and it sets the whole design. A plain glass pH
electrode drifts and needs frequent recal; the 6-month target is only reachable
with the right *probe + install*, never with a hobby probe.

| Sensor tech | Drift / recal reality | Life | Fit for a 6-mo unattended pool node |
|---|---|---|---|
| Cheap glass combo probe | ~0.02 pH/day; recal every few weeks–monthly ([Sea-Bird](https://www.seabird.com/technical-papers/ISFET-and-Glass-Electrode-pH-Sensors)) | ~6–12 mo | ✗ won't hold |
| Industrial glass (online) | 3–6 mo recal feasible with good install ([Rika](https://www.rikasensor.com/a-how-often-does-ph-sensor-for-water-quality-monitoring-need-calibration.html)) | ~1–2 yr | ~ borderline |
| **Differential pH** (double-junction, replaceable salt bridge) | Minimizes reference fouling = the main drift source; low-maintenance, replaceable reference ([Sensorex SD7000](https://sensorex.com/product/sd7500cd-universal-differential-ph-probe/)) | 1–2 yr, refreshable | ✅ the "6-mo" answer |
| **ISFET** | Ages slower than glass, no fragile bulb, tolerant of outdoor handling; still needs periodic buffer cal ([Sea-Bird](https://www.seabird.com/technical-papers/ISFET-and-Glass-Electrode-pH-Sensors)) | 1–2 yr | ✅ robust alternative |

Three design rules make 6 months real:

1. **Long-life probe** (differential or ISFET), not a hobby glass combo.
2. **Flow cell, not immersion** — the single biggest reliability lever (§4).
3. **Carry the sanitation signal on ORP**, which is far more stable than pH, so
   slow pH drift between cals never blinds the "is the pool safe?" readout (§3).

Honest expectation to set: **6 months between *recalibrations* is achievable;
the probe itself is a ~1–2 yr consumable**, plus an occasional 1-minute buffer
sanity-check.

---

## 2. Interfacing to an ESP32

Two clean paths — both mirror the existing `canary-sense` **I²C sensor** read
loop (BH1750 lux), so the firmware shape is already in the repo:

- **Digital I²C front-end (recommended for v0):** Atlas Scientific **EZO** circuits
  (pH / ORP / RTD) speak I²C or UART at **3.3 V — ESP32-native, no ADC, no level
  shifting** ([Atlas](https://atlas-scientific.com/blog/best-orp-probes-for-swimming-pools/)).
  Use their **electrical isolators** on the pH/ORP circuits to kill ground-loop
  noise. This drops straight into the BH1750 pattern.
- **Industrial 4-20 mA front-end:** a differential probe (e.g. Sensorex SD7420CD,
  direct 4-20 mA). The ADS1115 measures **voltage, not loop current**, so the
  loop needs a **precision shunt** (e.g. ~150–165 Ω, which maps 4–20 mA → ~0.6–3.3 V)
  or a current-loop receiver, plus a loop supply — sized to stay inside the ADC's
  input and common-mode limits — *then* into the ADS1115 and the same read loop.
  Higher cost, highest stability.

---

## 3. What to actually measure (pH → "capture more")

Adding parameters is cheap once the flow cell is plumbed:

| Parameter | Why | Target | Notes |
|---|---|---|---|
| **pH** | comfort, sanitizer efficacy, scale/corrosion | 7.2–7.6 | the fussy one (§1) |
| **ORP** | sanitizer-effectiveness proxy; **more stable than pH** | 650–750 mV | ±1 mV, ~2 yr probe ([Atlas](https://atlas-scientific.com/blog/best-orp-probes-for-swimming-pools/)); the cheap reliability win |
| **Water temp** | needed for pH temp-compensation *and* useful | — | RTD in the flow cell |
| **Free chlorine (amperometric)** | the real sanitizer ppm | 1–3 ppm | more accurate than ORP but **pH-dependent** (needs compensation), pricier ([Sensorex FCL](https://sensorex.com/product/fcl-amperometric-free-chlorine-sensor/), [PWTAG](https://www.pwtag.org/residual-control-september-2018/)) — add later |
| **Conductivity / TDS / salt** | **essential for salt pools** (3,000–3,500 ppm salt; TDS can top 5,000) | per system | use a **titanium** probe in salt ([In The Swim](https://www.intheswim.com/eguides/total-dissolved-solids.html)) |
| **Flow / pump pressure / level** | filter health *and* tells the node **when chem readings are valid** (only trust pH/ORP with real flow) | — | cheap adds |

Minimum viable: **pH + ORP + temp**. That already turns "a pH graph" into a
"pool OK / sanitizer low / out-of-range" panel.

---

## 4. Install: flow cell, not immersion (the reliability lever)

- An inline **flow cell** off the return line keeps probes **continuously wet**
  with slow steady flow, UV-shaded, and serviceable — and lets you isolate/valve
  it for calibration ([Poolside](https://poolside.support/kb/introducing-the-flow-cell/)).
- **Never let a pH/ORP junction dry** — it destroys the reference and voids
  probe warranty ([Sensorex care](https://sensorex.com/how-to-clean-ph-probe/)).
- Immersion probes need a UV-protected box and still dry out on pump-off; the
  flow cell is worth the plumbing.

---

## 5. Outdoor enclosure (repo-grounded)

Ratings framework: [`docs/hardware/enclosure/field_ratings.md`](../hardware/enclosure/field_ratings.md)
(CER ladder; a sealed pool case needs **both a cable gland and an ePTFE vent** —
day/night thermal swing pumps moisture past any static seal).

| Candidate (repo) | Rating | Gland? | Verdict |
|---|---|---|---|
| **`canary_hammond_chassis.scad`** — internal plate for a bought Hammond 1554/1555 box | **IP66/67/68, NEMA 4X/6P — from the bought box** ([Hammond 1554](https://www.hammfg.com/electronics/small-case/plastic/1554)) | drill the box for a real gland | ✅ **best** — only honest IP67 route with room for ESP32 + pH/ORP board |
| `canary_relay_solar.scad` — pole solar pod | CER-2 (self-noted); CER-3 after test | **yes — M8 gland + ePTFE vent already modeled** | ✅ printed/off-grid alternative; reparametrize the bay |
| `canary_wap_enclosure.scad` (weather) | CER-2/3 | no probe gland (USB notch only) | ~ only if front-end is tiny |
| `canary_jbox.scad` | sheltered only (fake conduit) | decorative | ✗ |
| `canary_field_case.scad` | IP67 intent | **zero penetrations by design** | ✗ **disqualified** — a probe cable must exit |

**Pick:** `canary_hammond_chassis.scad` in a bought IP66/67 Hammond box,
gland-drilled + ePTFE vent. Honor the LiPo 0–45 °C charge limit + light-color /
vent rules in `field_ratings.md` for a sun-exposed pool deck.

---

## 6. Integration: how it feeds the Dash + graphs (repo-grounded)

**Reality check:** the Dash has **no continuous chart** today — no `lv_chart`/line
widget anywhere in `firmware/projects/canary-display/`. It renders discrete state:
device cards show the **current** numeric value as text
([`dash_ui.cpp` card meta](../../firmware/projects/canary-display/src/ui/dash_ui.cpp)),
plus an events timeline and severity badges. The Time Machine journal
([`journal.h`](../../firmware/projects/canary-display/include/canary/fleet/journal.h))
is an **event log, not a numeric series** (no value field).

So graphing is two paths:

- **Path A — graph in Home Assistant (recommended for the graphs).** Every node
  self-describes sensors via MQTT discovery
  ([`canary-sense/src/ha/ha_discovery.cpp`](../../firmware/projects/canary-sense/src/ha/ha_discovery.cpp)),
  and **HA graphs any `sensor.*` entity automatically** — *this* part is ~zero new
  UI. The Dash's *current-value card* and *threshold alerts*, however, are **not**
  free (see build steps 2a/2b below): the Dash doesn't parse chemistry keys or
  card a new device type yet.
- **Path B — a trend widget on the Dash glass (net-new firmware).** A new
  `lv_chart` + a numeric ring buffer in the fleet model. Real work; only if the
  rolling graph must live on the wall display.

**Recommended build path — fork `canary-sense` → `canary-pool`** (inherits MQTT
connect/LWT/status/health/OTA/HA-discovery + the signed-chain *machinery* for
free — but see 2c: the chain today advances on **events**, and retained `state`
samples are **not** signed):

**Step 0 — strip the radar pipeline first (before any of the below).**
`canary-sense`'s *primary* sensor is the mmWave radar, **not** the BH1750, and
unlike the lux block it is **not** feature-gated. If you swap only the BH1750
loop, the resulting `canary-pool` still starts the radar UART, runs the presence
FSM, fills radar `state` fields, and publishes radar HA entities — i.e. it
reports a permanently stalled/unknown "presence" sensor and litters Home
Assistant with irrelevant entities. So the fork's **first** act is to remove or
compile-gate (cleanest behind a `FEATURE_RADAR` flag, so the fork is a config
flip): the radar UART bring-up + parser and the presence FSM in
[`main.cpp`](../../firmware/projects/canary-sense/src/main.cpp); the radar keys
in `publish_state_retained()`
([`net/mqtt_mgr.cpp`](../../firmware/projects/canary-sense/src/net/mqtt_mgr.cpp));
the radar config controls; and the radar discovery payloads in
[`ha/ha_discovery.cpp`](../../firmware/projects/canary-sense/src/ha/ha_discovery.cpp).
Only then do the sensor-swap + publish steps below make sense.

1. Swap the BH1750 read loop
   ([`canary-sense/src/main.cpp`](../../firmware/projects/canary-sense/src/main.cpp),
   the `Wire.begin` + threshold-gated sample pattern) for the EZO/ADS1115
   pH/ORP/temp read.
2. Add `ph` / `orp` / `water_temp_c` / `tds` keys (null-when-absent) to the
   retained `state` publish
   ([`canary-sense/src/net/mqtt_mgr.cpp` `publish_state_retained()`](../../firmware/projects/canary-sense/src/net/mqtt_mgr.cpp)).
   This gets you the HA graphs (step 3) but **nothing on the Dash by itself** —
   the three items below are the net-new Dash/node work:
   - **2a — Dash card (net-new):** the Dash `mqtt_mgr` parses `temperature`/`temp_c`
     but **not** `ph`/`orp`/`water_temp_c`
     ([`display .../mqtt_mgr.cpp`](../../firmware/projects/canary-display/src/net/mqtt_mgr.cpp)),
     and [`fleet_cards.cpp::has_cards()`](../../firmware/projects/canary-display/src/fleet/fleet_cards.cpp)
     recognizes **only** `canary-sense` — a `canary-pool` device gets no card. A
     pool card = parse the keys into the model + add a `canary-pool` card set
     (the file notes a card set is ~one builder branch).
   - **2b — threshold alerts (net-new):** publishing `state` fires no alerts. The
     node needs a **threshold/hysteresis state machine** that publishes named
     events to the `events` topic; and each new name must be added to
     [`fleet_model.cpp::classify_event()`](../../firmware/projects/canary-display/src/fleet/fleet_model.cpp)
     or it defaults to `Notice` severity.
   - **2c — tamper-evidence (design choice):** retained `state` values are **not
     individually signed** (a broker/subscriber could alter a graphed reading
     without breaking the chain). For tamper-evident chemistry, fold samples into
     the signed chain (advance it on sample commits) or emit **periodic signed
     digests**; otherwise treat graphed values as convenience data, not evidence.
3. Copy the illuminance HA entity → pH/ORP/temp entities (`value_json.ph`, unit
   `pH`; ORP `mV`; water temp `device_class: temperature`) for auto-graphs.
4. Register: `CD/CS_DEVICE_TYPE "canary-pool"` in
   `firmware/configs/canary-pool/default/config.h`; add the board to
   `firmware/boards/boards.json` `used_by[]` (+ `canary-local/devices/boards.json`);
   add the env to `firmware/flavors.json`; give it an OTA product string. Topic
   shape reused verbatim from
   [`topics.h`](../../firmware/projects/canary-display/include/canary/topics.h)
   (`securacv/<id>/{status,health,state,events,…}`). No allowlist to edit — the
   Dash auto-discovers via the `securacv/+/status` wildcard.

---

## 7. Reference BOMs + rough cost (July-2026 USD, verify)

**Tier A — DIY / fastest (Atlas EZO, I²C):**

| Item | ~Price | Source |
|---|---|---|
| ESP32-S3-DevKitC-1 | $15 | [DigiKey](https://www.digikey.com/en/products/detail/espressif-systems/ESP32-S3-DEVKITC-1-N8/15199021) |
| EZO-pH circuit + lab-grade probe | circuit ~$40 + probe ~$50–60 | [Atlas kits](https://atlas-scientific.com/kits/ph-kit/), [DigiKey EZO-PH](https://www.digikey.com/en/products/detail/atlas-scientific/EZO-PH/16003108) |
| EZO-ORP circuit + probe | ~$51 + probe | [Amazon EZO-ORP](https://www.amazon.com/Atlas-Scientific-EZO-ORP-Oxidation-Reduction-1019-9mV-1019-9mV/dp/B0078WOD2Y) |
| EZO-RTD temp circuit + probe | ~$41 + probe | DigiKey |
| pH/ORP isolators (×2) | ~$25–35 ea | Atlas |
| Flow cell (2-probe) | ~$40–120 generic; premium kits far more | see below |
| Hammond IP67 box + PG9 gland + ePTFE vent | box ~$25–50 + gland ~$1 + vent ~$2–8 | [Hammond 1554](https://www.hammfg.com/electronics/small-case/plastic/1554), [PG9 10-pk $8](https://www.amazon.com/pg9-cable-gland/s?k=pg9+cable+gland) |
| **Tier A total** | **≈ $350–550** | — |
| *(shortcut)* Atlas **Wi-Fi Pool Kit** (pH+ORP+temp+wifi, integrated) | verify current | [Atlas](https://atlas-scientific.com/kits/wi-fi-pool-kit/), [RobotShop](https://www.robotshop.com/products/atlas-scientific-wi-fi-pool-kit-w-standard-probes) |

**Tier B — reliability / true 6-mo (industrial differential):**

| Item | ~Price | Source |
|---|---|---|
| Sensorex **SD7420CD** differential pH (4-20 mA) | ~$500–700 (up to ~$1,130 some resellers) | [Sensorex](https://sensorex.com/product/sd7420cd-differential-ph-probe-direct-4-20ma-output/) |
| Sensorex **Pool Plus** pH+ORP electrodes | ~$749–778 | [Sensorex](https://sensorex.com/product/pool-plus-ph-and-orp-electrodes/) |
| Chemtrol pH / ORP probes (10 ft) | pH ~$314 / ORP ~$353 | [Poolweb](https://www.poolweb.com/collections/chemical-controller-probes) |
| 4-20 mA loop supply + precision shunt / current-loop receiver → ADS1115 + ESP32 + Hammond box | ~$50–90 | as above (ADS1115 reads voltage, not loop current — §2) |
| **Tier B total** | **≈ $700–1,500** | — |

**Reference points (context):** a premium turnkey *Chemistry Probe Kit + Flow
Cell* (pH+ORP) runs **$1,509** ([Shasta/Poolside Tech](https://shastapoolsupply.com/products/chemistry-probe-kit));
a $145 "7-in-1" WiFi monitor (pH/EC/CL/TDS/ORP/temp) exists but is
consumer-grade with weak cal stability — useful as a price floor, not a 6-mo
reference.

---

## 8. Calibration + maintenance cadence

- **pH:** 2-point buffer cal (7.0 + 4.0/10.0) at install; **re-cal ≤ 6 mo** (Tier
  B) — sooner if the flow cell is neglected. Replace probe ~1–2 yr.
- **ORP:** verify vs 240 mV / Zobell solution at install; stable for months;
  probe ~2 yr.
- **Keep the flow cell wet always.** Winterize by storing probes in KCl/buffer
  caps, never dry.
- **Trust gating:** only publish/trust pH/ORP when flow is confirmed — a
  pump-off reading is meaningless; mark it `null`, not stale-good.

---

## 9. Recommendation

Start **Tier A** (`canary-pool` fork + Atlas EZO pH/ORP/temp on I²C, flow cell,
Hammond box), graphs in Home Assistant, Dash card + alerts. If bench testing
shows pH won't hold ~6 months, step up to **Tier B differential/ISFET** — but
this is **not** a drop-in probe swap. The industrial differential probe named in
the Tier B BOM (SD7420CD) is a **4–20 mA loop** device, not an EZO I²C circuit,
so it changes the analog front end: add a loop supply + a precision shunt /
current-loop receiver feeding the ADS1115 (voltage, not current), with the
matching firmware scaling and wiring — exactly the front-end §2 and the Tier B
BOM already call out. (If you want a true drop-in, choose instead an
**I²C-native differential/ISFET** probe + carrier so the rest of the node stays
put.) Add free chlorine / salinity only once the core loop is trusted.
