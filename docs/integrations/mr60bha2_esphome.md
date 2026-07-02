# Seeed MR60BHA2 (stock ESPHome) → SecuraCV Witness Integration

## 1) Overview

This guide connects a **Seeed Studio MR60BHA2 60GHz mmWave kit running its
factory ESPHome firmware** to the SecuraCV sealed witness log — Track B of
`docs/canary_sense_mr60bha2_design.md`. No custom firmware is required: the
kit keeps its stock firmware and the adapter host's generic `mqtt_sensor`
adapter turns its presence entities into `PresenceInRestrictedZone` claims.

One thing must be bridged first: **the stock firmware speaks the ESPHome
native API to Home Assistant — it does not publish MQTT on its own.** Pick
exactly one of the two config-only bridge paths in §4.

Trust posture (read this before deploying): Track B claims are
**kernel-signed at ingest, not device-signed**. The Home Assistant timeline
renders them as *adapter-attested*, not *device-verified*. If you need
per-device Ed25519 signing and hash-chaining at the sensor, that is the
native `canary-sense` firmware (Track A, see the design doc) — this guide is
the 10-minute on-ramp, not the full witness device.

Related docs:

- Capability assessment, privacy classes, roadmap: `docs/canary_sense_mr60bha2_design.md`
- Home Assistant integration + add-on setup: `docs/homeassistant_setup.md`
- Adapter host reference config: `adapter_host.example.toml`

---

## 2) Architecture Diagram

```
Path (a): unmodified kit                    Path (b): ESPHome mqtt: overlay

[MR60BHA2 kit, stock ESPHome]               [MR60BHA2 kit, +mqtt: block]
   |  (ESPHome native API)                     |  (MQTT: <prefix>/binary_sensor/...)
   v                                           v
[Home Assistant]                            [MQTT Broker]
   |  (mqtt_statestream republish)             |
   v                                           |
[MQTT Broker]                                  |
   |                                           |
   +---------> [adapter_host: mqtt_sensor] <---+
                    |  (PresenceInRestrictedZone claims,
                    |   privacy chokepoint, 10-min buckets)
                    v
               [SecuraCV sealed log] --(Event API / MQTT)--> [Home Assistant timeline]
```

---

## 3) Prerequisites

- An MR60BHA2 kit on its factory ESPHome firmware, already adopted by Home
  Assistant (Settings → Devices & Services → ESPHome). The stock device
  exposes: person presence (binary), target number, distance, breath rate,
  heart rate, illuminance.
- A reachable MQTT broker (e.g. HA's `core-mosquitto`).
- The SecuraCV adapter host built with `adapter-mqtt-sensor` and a config
  based on `adapter_host.example.toml`.
- Entity IDs for your unit: check HA → the kit's device page. This guide uses
  `binary_sensor.mr60bha2_person_information` and
  `sensor.mr60bha2_target_number`; substitute yours.

---

## 4) Bridge the kit to MQTT (pick one path)

### Path (a) — Home Assistant `mqtt_statestream` (kit stays unmodified)

Add to Home Assistant `configuration.yaml`, then restart HA:

```yaml
mqtt_statestream:
  base_topic: securacv_statestream
  include:
    entities:
      - binary_sensor.mr60bha2_person_information
      # Optional occupant counter (see §5):
      # - sensor.mr60bha2_target_number
```

HA now republishes each state change to
`securacv_statestream/binary_sensor/mr60bha2_person_information/state` with
the bare state (`on` / `off`) as payload.

**Scope the `include` list deliberately.** Only presence/occupancy entities
belong here. Do NOT statestream the breath-rate or heart-rate entities toward
the witness log — vitals are P1 wellbeing signals that stay in Home
Assistant (design doc §2).

### Path (b) — ESPHome `mqtt:` overlay (one OTA, no HA hop)

Take the kit's ESPHome YAML (Seeed publishes the reference config in
`Seeed-Studio/xiao-esphome-projects`, project `seeedstudio-mr60bha2-kit`),
add an `mqtt:` block, and push it over the air with the ESPHome
Dashboard/Web:

```yaml
mqtt:
  broker: 192.168.1.10        # your broker
  username: !secret mqtt_user
  password: !secret mqtt_pass
  topic_prefix: mr60bha2      # one prefix per device
  discovery: false            # HA keeps using the native API for entities
```

The device then publishes
`mr60bha2/binary_sensor/person_information/state` as `ON` / `OFF` directly —
no Home Assistant in the claim path. Keep the `api:` block so HA dashboards
and the wellbeing entities continue to work unchanged.

---

## 5) Route into the adapter host

Add routes to `adapter_host.toml` (full commented examples live in
`adapter_host.example.toml`). Path (a):

```toml
[[adapter]]
type = "mqtt_sensor"
mqtt_broker_addr = "127.0.0.1:1883"

[[adapter.route]]
topic = "securacv_statestream/binary_sensor/mr60bha2_person_information/state"
kind = "presence_in_restricted_zone"
zone = "bedroom"
require_truthy_state = true
```

Path (b) differs only in the topic:

```toml
[[adapter.route]]
topic = "mr60bha2/binary_sensor/person_information/state"
kind = "presence_in_restricted_zone"
zone = "bedroom"
require_truthy_state = true
```

Occupant-counter alternative (either path): the target-number sensor
publishes a bare count (`0`..`N`). Use `numeric_min` instead of the truthy
gate — the claim is emitted when the count clears the floor, and the count
itself is gating-only (never logged):

```toml
[[adapter.route]]
topic = "securacv_statestream/sensor/mr60bha2_target_number/state"
kind = "presence_in_restricted_zone"
zone = "bedroom"
numeric_min = 1.0
```

Route attributes (kind, zone, gates) hot-reload on SIGHUP; changing a
subscribed topic needs a restart — see the header of
`adapter_host.example.toml`.

---

## 6) Verify

1. Start the adapter host and watch the log for the `mqtt_sensor` adapter
   subscribing to your topic(s).
2. Walk into the sensor's field of view. Within one poll interval the host
   log shows a sealed claim; `cargo run --bin log_verify -- --db witness.db`
   confirms chain integrity.
3. In Home Assistant, the SecuraCV timeline card shows a
   `PresenceInRestrictedZone` event in the current 10-minute bucket. It will
   NOT carry the green device-verified badge — correct and expected for
   Track B. The card additionally renders an explicit `adapter` /
   `ha-bridged` attestation chip when the event payload carries the
   `attestation` field (stamped by the kernel export once the Phase 4
   backend wiring in the design doc lands).
4. Negative check: leave the room, confirm presence claims stop after the
   FSM clears, and confirm NO breath/heart entities appear anywhere in
   `witness.db` (`sqlite3 witness.db 'select distinct event_type from
   sealed_log;'` should list only event types from the §2 mapping).

---

## 7) Privacy contract summary

| Stock entity | Route it? | Where it lives |
|---|---|---|
| Person presence (binary) | ✅ `presence_in_restricted_zone` | Sealed log (P0) |
| Target number | ✅ optional, via `numeric_min` | Sealed log as presence (count never logged) |
| Distance | ❌ | HA only; zone gating belongs in Track A firmware |
| Breath rate / heart rate | ❌ never | HA only — P1 opt-in wellbeing signals |
| Illuminance | ❌ (v0) | HA only; lux-tamper corroboration is Track A work |

The kernel's contract enforcer is the backstop: the `mqtt_sensor` adapter
can only emit the coarse claim vocabulary, and every claim is re-coarsened
to 10-minute buckets regardless of what the routes attempt.
