# Unified Onboarding — One Wizard, Three Sensors

**Status:** Phase A landed 2026-07-11 (discovery + identify parity, type-aware wizard
branches). Phases B–C are roadmap.
**Scope:** canary-wap, canary-vision, canary-sense (+ canary-display as a
fleet advertiser/consumer).

The goal: plugging in *any* Canary produces the same onboarding shape — a
device card that appears on its own, a way to prove which physical unit the
card refers to, a short type-specific explanation of what makes this sensor
different, and an ending that shows the sensor actually sensing. The wizard
is the same skeleton for every variant; only the middle panel (the
"what's different" card and the proof-of-life finale) changes per type.

---

## 1. What we learned from Apple's add-device flows

We studied AirPods proximity pairing, HomeKit accessory setup, Apple Watch
camera pairing, and HomePod setup, specifically for the *psychology of
timing* and *how the user knows which device is which*. Full research notes
with sources are summarized here; the load-bearing principles:

**Timing choreography**
- **The device announces itself; the user never searches.** Opening the
  AirPods lid triggers the card — the phone was already listening. The
  physical act (plug in the sensor) and the digital response (card slides
  up) are welded together in time, so the user never wonders "did it see
  it?" Target: card appears within ~2 s of the device joining the network.
- **No spinners before discovery.** Discovery runs continuously while the
  add-device surface is open; the UI appears already knowing the answer.
  When waiting is unavoidable, the wait *is* the interaction (Watch's
  particle cloud), or the device performs "I'm alive" physically (HomePod
  chime + glow) before the screen does anything.
- **End on proof, not a checkmark (peak-end rule).** AirPods end on live
  battery levels — data that rides in the discovery advertisement itself.
  HomePod ends with Siri talking. People remember the ending; make the
  ending the product working.

**Device identity & disambiguation**
- **Proximity/timing is the first identity claim**; a visual render of the
  exact model is the second ("that's literally my thing").
- **Identify is a first-class verb.** HomeKit *requires* accessories to
  implement an Identify routine (blink an LED) so a user facing a list can
  ask each entry to reveal itself physically. With multiple candidates:
  "Which one is blinking?"
- **Out-of-band possession proof where it matters** (HomeKit's printed
  8-digit/QR setup code): only someone holding the device can add it.

**Explaining device differences**
- **Category is asserted at discovery time**, not inferred later — the icon
  precedes the name. HomeKit accessories declare a category that drives
  icon, tile, and which setup questions appear.
- **Capability-driven follow-ups:** cameras get camera questions during
  setup; a bulb never sees them. Wizard length adapts to the device.
- **Plain language.** "Senses breathing through the air" — never "60GHz
  FMCW mmWave," never model numbers as names.

**Naming ritual**
- Room first-class, asked near the end; suggested default of
  `<Room> <Type>` ("Bedroom Breathing Sensor"); technical hostnames stay
  underneath, never as the display name. Naming a *working* thing feels
  like christening; naming an unknown thing feels like a form.

---

## 2. The shared identity layer (landed)

Everything above needs one substrate: every device must announce **what it
is** the moment it joins the LAN, and every device must be able to
**physically identify itself** on request. That is what this change set
ships.

### 2.1 Canonical `_securacv._tcp` TXT schema

Every SecuraCV device advertises the same mDNS service with the same TXT
vocabulary:

| key | value | example |
|-----|-------|---------|
| `device_id` | stable id (NVS-backed, survives OTA) | `canary_sense_001` |
| `name` | friendly name (falls back to device_id) | `Bedroom Sense` |
| `host` | unique mDNS host label | `canary-sense-001-a1b2c3` |
| `fw` | firmware version | `2.2.0` |
| `model` | human model string | `Canary Sense (XIAO ESP32-C6 + MR60BHA2)` |
| `dt` | **canonical device type** — lowercase, hyphenated | `canary-sense` |
| `role` | `witness` (sensors) / `display` (glance surfaces) | `witness` |
| `broker` / `bport` | MQTT broker gossip; empty tombstone while the link is down | `192.168.1.10` / `1883` |

Canonical `dt` values: `canary-wap`, `canary-vision`, `canary-sense`
(matches the HA component's `DEVICE_TYPE_MODALITY` map and its
`canonical_device_type()` normalizer). `role` separates sensors from
displays so browsers don't render a watch puck as a witness.

Who advertises what:

| variant | advertises | browses | HTTP API |
|---------|-----------|---------|----------|
| canary-wap | full schema (3 announce sites) | yes — relays the fleet via `/api/fleet/scan` and `/api/v1/peers`, now including `dt`/`role` | yes |
| canary-vision | full schema (`src/net/mdns_mgr.cpp`) | no | no |
| canary-sense | full schema (`src/net/mdns_mgr.cpp`) | no | no |
| canary-display | full schema (was `role`/`dt` only) | broker referral only | no |

The WAP-as-relay matters: the companion app is HTTP-only, but any paired
WAP browses the LAN and hands the app a typed peer list — so vision and
sense units appear on the app's "discovered devices" step, correctly
badged, without the app needing an MQTT client.

### 2.2 Identify parity (landed)

| variant | trigger | physical behavior |
|---------|---------|-------------------|
| canary-wap | `POST /api/identify` (existing) | LED blink + chirp |
| canary-vision | HA **Identify** button → `securacv/<id>/identify/set` | 10 s 2 Hz LED flash (boards without a user LED echo-only) |
| canary-sense | HA **Identify** button → `securacv/<id>/identify/set` | 10 s 2 Hz **white** WS2812 flash, then the presence color returns |

Both MQTT variants publish a non-retained echo on `securacv/<id>/identify`
(`on`/`off`) bracketing the blink window, so any dashboard can pulse the
device card *in sync with the physical LED* — screen state and physical
state stay synchronized, which is the trust-building trick.

The identify button rides ordinary HA MQTT discovery
(`device_class: identify`), so it exists in Home Assistant with zero
integration work.

---

## 3. The wizard skeleton (same for every type)

Six beats, three of which never change:

1. **Appear** *(shared)* — the add-device surface lists discovered devices
   live (peer list refresh while open). Each row: type icon, type label,
   one-line tagline, friendly name/id. A device that powers up while the
   user is looking appears on its own — no Scan button.
2. **Claim** *(shared)* — the user picks a row (or the row arrived
   pre-picked via "Pair this Canary").
3. **Prove** *(shared verb, per-type transport)* — "Blink to confirm":
   WAP over HTTP identify; vision/sense via the HA identify button (the
   wizard tells the user exactly where it is). Multiple same-type devices →
   "which one is blinking?"
4. **Explain** *(per-type)* — the "What makes this one different" card:
   - **Canary WAP** — *Witness beacon.* GPS + signed event log; runs its
     own WiFi hotspot; pairs directly with this app (BOOT-tap).
   - **Canary Vision** — *Camera witness.* Detects people on-module;
     never stores or streams video; joins through Home Assistant.
   - **Canary Sense** — *Radar witness.* Senses presence and breathing
     through the air — no camera, no microphone; heart-rate only in the
     opt-in wellbeing build; joins through Home Assistant.
5. **Name** *(shared)* — room picker + name input prefilled
   `<Room> <Type label>`; hostname stays technical underneath.
6. **Finale** *(per-type, live proof)* —
   - WAP: first signed witness record / dashboard tiles live.
   - Vision: "walk in front of it — watch **Presence** flip on."
   - Sense: "sit still nearby for ten seconds — watch **Breathing** lock."

Steps 3/4/6 branch off `dt`; everything else is one code path. In the SPA
the branch decision is a pure function of the peer record
(`pairing: 'http' | 'mqtt'`), so it is host-testable.

---

## 4. Phasing

**Phase A (this change set):**
- Canonical TXT schema on all four variants; WAP relays `dt`/`role`.
- MQTT identify + HA identify button on vision/sense; echo topic for
  card-pulse sync.
- Companion app: type registry, badges/taglines on discovered and paired
  device cards, `device_type` persisted, per-type wizard branch — HTTP
  path unchanged for WAP; guided HA path (explain → identify → per-type
  proof-of-life) for vision/sense instead of a dead-end.
- Docs: getting-started sections for vision/sense; parity docs updated.

**Phase B (roadmap) — close the "instant card" gap for MQTT devices:**
- Companion app MQTT bridge (device-api subscribes to `securacv/#` via the
  gossiped broker) so vision/sense cards go *live* in the app: presence,
  breathing lock, identify round-trip from the app itself — no HA detour
  mid-wizard. This is the biggest single UX upgrade available next.
- Live proof-of-life datum on the discovery card (retained state fetch at
  render: "Presence: clear · Breathing: —").

**Phase C (roadmap) — possession proof + zero-typing everywhere:**
- Printed QR setup code on vision/sense enclosures (HomeKit-style
  possession proof) feeding the existing QR scan sheet; per-type suggested
  automations after setup (Apple pattern, deliberately deferred out of the
  wizard).
- Display: surface sense `occupants`/`range_band` as first-class glance
  fields (today: breathing/wellbeing only).

## 5. Bench checklist before calling Phase A "done" on hardware

- [ ] canary-sense (C6): mDNS advert visible (`dns-sd -B _securacv._tcp`),
      TXT complete, survives a WiFi outage/reconnect (STA_GOT_IP re-announce).
- [ ] canary-vision (C3 + XIAO C3/S3): same, plus identify on each host
      board (DevKit GPIO8, XIAO S3 GPIO21 active-low, XIAO C3 echo-only).
- [ ] canary-sense identify: white flash overrides presence color and
      hands back cleanly; echo bracketing verified in HA + app.
- [ ] WAP fleet-scan payload carries `dt`/`role`; SPA badges render from a
      live WAP relay.
- [ ] Broker gossip: stop mosquitto → vision/sense tombstone `broker` TXT;
      restart → re-advertised.
