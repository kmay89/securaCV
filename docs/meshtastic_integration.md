# Meshtastic Integration — LoRa mesh as a witness source (and, later, a transport)

**Status:** Direction 1 implemented (`adapter-meshtastic`); Directions 2–3 specified / roadmap
**Last updated:** 2026-06-10
**Companion docs:** [../spec/sensor_adapter_contract_v0.md](../spec/sensor_adapter_contract_v0.md) ·
[../spec/canary_mesh_network_v0.md](../spec/canary_mesh_network_v0.md) ·
[../spec/beacon_channel_v0.md](../spec/beacon_channel_v0.md) ·
[../spec/chirp_channel_v0.md](../spec/chirp_channel_v0.md) ·
[mesh_esp_now_evaluation.md](mesh_esp_now_evaluation.md) ·
[research/harm_reduction_prior_art.md](research/harm_reduction_prior_art.md)

## TL;DR

[Meshtastic](https://meshtastic.org) is an open LoRa mesh platform with kilometre-scale,
off-grid range — and **no camera or computer-vision story**: its closest feature is the
Detection Sensor Module, which turns a GPIO pin (PIR, contact switch) into a text alert.
SecuraCV is exactly the missing half: a privacy-preserving witness pipeline whose events are
*already* coarse enough to fit a single LoRa packet. The integration runs in both directions:

1. **Inbound (IMPLEMENTED)** — Meshtastic detection-sensor nodes become witness sources via the
   `adapter-meshtastic` sensor adapter, reaching kilometres beyond the household ESP-NOW mesh.
2. **Outbound (SPECIFIED, not implemented)** — sealed SecuraCV events broadcast over the mesh to
   phones/nodes with no internet, no cell service, no cloud.
3. **LoRa as an Opera/Beacon/Chirp transport (ROADMAP)** — long-range substrate for the existing
   canary mesh protocols, on ESP32-C6/S3 + SX1262 hardware.

Meshtastic is already cited as harm-reduction prior art for our mesh design (frame discipline,
airtime budgeting, public schema) in
[research/harm_reduction_prior_art.md](research/harm_reduction_prior_art.md) §9. This doc turns
the precedent into an integration.

## Why Meshtastic (and why the fit is unusually good)

| Constraint | Meshtastic reality | SecuraCV reality |
|---|---|---|
| Payload | ~233 bytes max per LoRa packet; images infeasible | Events are coarse by construction: type + zone + 10-min bucket fits in tens of bytes |
| Identity | Node ids are pseudonymous but stable | The `Claim` type structurally cannot carry identifiers |
| Timing | Duty-cycle limits force sparse transmission | Per-bucket dedup already collapses bursts to one event |
| Trust | Mesh input is unauthenticated (channel PSK is transport privacy, not authenticity) | Adapters are untrusted producers by design; the kernel's gates are the security boundary |

What Meshtastic gains: the "smart sensor" it lacks — CV-grade witnessing (via Frigate/ONNX on the
SecuraCV side) instead of bare GPIO triggers. What SecuraCV gains: off-grid sensor reach and,
later, off-grid alert delivery — no Wi-Fi, no broker on the sensor side, no cloud.

---

## Direction 1 — Inbound: mesh sensors → sealed events (IMPLEMENTED)

### Architecture

```
PIR / contact switch
        │ GPIO
Meshtastic node (Detection Sensor Module)
        │ LoRa (km-scale, private channel PSK)
Meshtastic gateway node (mqtt.enabled + mqtt.json_enabled)
        │ MQTT uplink: msh/<REGION>/2/json/<CHANNEL>/!<gatewayid>
MQTT broker (the same one Frigate/HA already use)
        │
adapter_host (type = "meshtastic", feature `adapter-meshtastic`)
        │ Claim { kind, zone_label, confidence }   ← no id, no time, no text
Kernel gates (append_event_checked: allowlist, contract enforcer, zone policy)
        │
Sealed log (Ed25519-signed, hash-chained) → Event API / Home Assistant
```

The adapter is `src/adapter/meshtastic.rs`; wiring and config live in
`src/bin/adapter_host.rs` and `adapter_host.example.toml`. It follows the same pattern as the
BLE-presence adapter: an MQTT forwarder feeds raw `(topic, payload)` frames into a pure,
I/O-free parser (sandboxable via `adapter-sandbox`), and the host stamps/dedups/seals.

### Mapping rules (JSON-mode frames)

| Meshtastic frame `type` | Handling |
|---|---|
| `detection` (Detection Sensor Module, portnum 10) | Look up the packet's **`from`** node in the configured node table → emit the node's claim kind in its zone, confidence 1.0 |
| `text` | Mapped **only** when the node entry sets `detection_name` and the text contains it (covers setups where detection alerts ride the plain text portnum); otherwise dropped — operator chat never asserts presence |
| `position` | **Dropped on the type tag alone** — coordinates are never deserialized (Invariant III) |
| `telemetry`, `nodeinfo`, `neighborinfo`, everything else | Dropped |

Routing detail: claims route on the payload's `from` field (the originating sensor), never the
topic's trailing `!nodeid` — that identifies the *gateway* that uplinked the packet.

Allowed claim kinds are deliberately minimal — what a GPIO LoRa node can plausibly assert:
`presence_in_restricted_zone`, `contact_state_change`, `acoustic_impulse_in_zone`.

### Privacy analysis (per invariant)

| Concern | Posture |
|---|---|
| Stable node ids vs. no-stable-IDs | Node ids are **local routing keys only**: they live in the operator's `adapter_host.toml` (same exposure class as Frigate camera names) and select a kind/zone. `Claim` has no field that could carry them; the export-scrub test (`tests/adapter_meshtastic.rs`) asserts neither hex nor decimal form survives into an export. |
| Precise time | The packet `timestamp`/`rx_time` are never deserialized; the host stamps a coarse bucket and the kernel re-coarsens to 10 minutes. |
| Location | `position` frames are dropped before any payload field is read. |
| RF metadata (RSSI/SNR fingerprinting) | `snr` may be consulted as an optional drop-floor (`min_snr`); it never propagates into a claim or the log. `rssi` is never read. |
| Free text | The alert text is *matched* against the configured `detection_name`, never copied — there is no text field to copy into. |

### Trust statement

The mesh is an **unauthenticated producer**. Anyone holding the channel PSK can inject detection
frames, and the default `LongFast` channel key is public. Operators MUST run sensor nodes and the
gateway on a **private channel with a non-default PSK** — and treat that as transport privacy,
not authenticity. The adapter's defenses are the standard ones: it is an audit boundary
(untrusted-producer-by-design), the kernel gates are the security boundary, the node allowlist
limits who can assert anything, and per-bucket dedup bounds a forged or replayed packet to at
most **one coarse event per zone per bucket**. Forgeability of *occurrence* remains — the same
trust level as any unauthenticated PIR publishing to MQTT. `sandbox = true` is recommended.

### Node-side setup runbook

1. **Sensor node** (any supported Meshtastic device with a free GPIO):
   - Detection Sensor Module: `enabled = true`, `monitor_pin = <GPIO>`,
     `detection_triggered_high` per your sensor, `name = "PIR"` (or similar),
     `minimum_broadcast_interval` ≥ 45 s (spam guard), `state_broadcast_interval = 0`
     (change-only; a heartbeat would just be deduped away on our side).
   - Join the private channel.
2. **Gateway node** (mains-powered, Wi-Fi in range of the broker):
   - MQTT module: `enabled = true`, `json_enabled = true`, server = your broker,
     uplink enabled on the private channel.
   - Scope the gateway's uplink to the dedicated channel — the wildcard subscription
     (`msh/+/2/json/+/+`) receives everything the gateway uplinks, including bystander packets;
     the parser drops them unread, but the broker still sees them.
3. **SecuraCV**: add the `[[adapter]] type = "meshtastic"` block (see
   `adapter_host.example.toml`) with one `[[adapter.node]]` per sensor. Node *attributes*
   hot-reload on SIGHUP; changing the subscribed topic requires a restart.

Firmware note: the expected JSON `type` string for detection frames is `"detection"`
(`DETECTION_SENSOR_APP`); some firmware/configurations carry detection alerts as plain `"text"`,
which the `detection_name` gate covers. When first bringing up hardware, capture a live frame
(`mosquitto_sub -t 'msh/#' -v`) and pin it as a unit-test fixture alongside the firmware version.

---

## Direction 2 — Outbound: sealed events → mesh alerts (SPECIFIED, NOT IMPLEMENTED)

The natural consumer is a sibling of `src/bin/event_mqtt_bridge.rs`: read the loopback Event API
and publish to the gateway's downlink. Two payload forms, in order of preference:

1. **JSON downlink** (`msh/<REGION>/2/json/mqtt/` with `"type":"sendtext"`) — human-readable text
   on the operator's channel, e.g. `back_gate: presence 18:40–18:50`. Zero custom software on the
   receiving phones.
2. **PRIVATE_APP binary** (portnum 256, ≤233 bytes) — a compact CBOR frame
   `{event_type, zone_id, bucket_start, bucket_size}` for machine consumers, leaving the
   registered-portnum upgrade path open if this ever becomes a public app.

The payload is `event_type + zone + bucket` **only** — already coarse by construction; never
confidence histories, never correlation tokens.

**Why this ships later, deliberately:** an RF broadcast at detection time is event-correlated
network behavior observable by *any* LoRa receiver in range — a metadata leak in tension with
Invariant III even when the payload itself is clean. The mitigations are known (operator opt-in
per event type; batching + jitter so emission time decouples from detection time; favoring
Beacon-grade life-safety templates over routine presence events) and belong in the implementation
PR, not retrofitted. LoRa duty-cycle/airtime budgets (EU 868: 1% duty cycle; `LongFast` ≈ 1 kbps)
also cap how chatty this may ever be.

---

## Direction 3 — LoRa as an Opera/Beacon/Chirp transport (ROADMAP)

The ESP-NOW mesh ([canary_mesh_network_v0.md](../spec/canary_mesh_network_v0.md)) is
single-transport and ~100 m scale; [mesh_esp_now_evaluation.md](mesh_esp_now_evaluation.md)
already names multi-transport resilience as the spec's promised end-state. LoRa is the natural
long-range rung.

**Hardware:** ESP32-C6 + SX1262 boards exist now (M5Stack **C6L Unit**, community-supported in
Meshtastic; note C6 BLE support in Meshtastic firmware is still incomplete). Heltec's official
boards (WiFi LoRa 32 V3/V4) are ESP32-S3 + SX1262 — closer to the existing `canary-wap` XIAO
ESP32-S3 target and the safer first port.

**Options considered:**

| Option | Sketch | Trade-off |
|---|---|---|
| (a) **Companion node** — recommended for experimentation | Stock Meshtastic device serial-bridged to a canary; Opera/Beacon/Chirp frames ride as PRIVATE_APP payloads | Fastest to try; inherits Meshtastic routing/dedup/airtime governor; but frames must fit ≤233 B (Opera's `MAX_MESSAGE_SIZE` is 250 B → slimming or fragmentation), and the radio TCB is someone else's firmware |
| (b) **Raw-LoRa Opera port** — the normative path | RadioLib + SX1262 as a second transport under the existing Opera framing, crypto (`opera_secret`, monotonic counters, flash-encryption gate), and airtime budgeting | Keeps the whole trust model intact and the airtime governor ours; the most work; requires a `canary_mesh_network` spec revision (§2.2 transport layer is ESP-NOW-only today) |
| (c) Meshtastic firmware fork | Custom module inside meshtastic/firmware | Rejected: permanent maintenance burden and TCB sprawl |

Any of these is a spec revision first, code second. Beacon's NFPA-72-style supervision
requirements (co-signing, heartbeat) translate to LoRa unchanged; what changes is the airtime
math.

---

## Risks and open questions

- **Replay across buckets** (inbound): a captured detection packet replayed in a later bucket
  forges at most one coarse event. Accepted for v0 and documented; a small ring buffer of recent
  Meshtastic packet `id`s in the adapter is a cheap follow-up if it matters in practice.
- **JSON `type` drift** across Meshtastic firmware versions: covered by the dual
  detection/text path; pin the firmware version + a captured fixture when hardware arrives.
- **Wildcard topic breadth**: the broker sees whatever the gateway uplinks. Scope the gateway to
  a dedicated channel; the parser drops unconfigured traffic unread.
- **Serial/protobuf transport** (no broker at all): the seam exists (`MeshFrameKind` selects the
  decode path), but protobuf decoding means vendoring Meshtastic protos + a codegen dependency —
  deferred until someone actually needs broker-less operation.
