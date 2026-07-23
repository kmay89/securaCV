# Canary Vision Pro (Seeed reCamera Pro) — device guide & positioning

**Status:** concept / config-only integration, no new firmware required.
**SKU:** Seeed reCamera Pro 2GB, 100092895 (~$299.90).

Canary Vision Pro is **not new firmware** — it's a new *tier*, built entirely
on the existing [Sensor Adapter Contract](../../spec/sensor_adapter_contract_v0.md)
and the [`adapter_host`](../../src/bin/adapter_host.rs) HTTP webhook ingress
that already ships. The reCamera Pro runs its own Linux OS and does its own
on-device inference; SecuraCV's job is the same as it is for Frigate or an
ESPHome radar kit — accept its detections at the trust boundary, strip
everything but a coarse claim, and seal it.

---

## 1 · What it is

The reCamera Pro is a standalone AI camera, not an ESP32 peripheral:

- **SoC:** Rockchip RV1126B, 3 TOPS NPU — runs vision, sound classification,
  and (optionally) VLM/LLM inference **entirely on-device**.
- **Sensor:** 8 MP, starlight (low-light) imaging.
- **Model training:** SenseCraft AI, 1-click image/audio classification —
  no ML expertise required to add or retrain a class.
- **Integration surface:** built-in Web UI, event-driven workflows
  (Node-RED-style flow editor), open HTTP APIs, and broad industrial
  protocol support.
- **Power/mount:** PoE or USB-C; ships with the same two-T-stud enclosure
  interface Seeed uses across the reCamera line.

That last bullet — **event-driven workflows with an HTTP action node** — is
what makes it a zero-new-code fit: point its "on detection → HTTP POST" flow
at our existing webhook adapter and it's a witness source.

---

## 2 · How it plugs in (no new firmware, no new Rust)

Exactly like the MR60BHA2's ESPHome bridge or a Frigate install, the
reCamera Pro is configured, not flashed:

1. In the reCamera Pro's flow editor, build a flow: **classifier output →
   HTTP request node**.
2. Point the HTTP request node at your `adapter_host`'s webhook listener,
   with the request **path** naming the zone/sensor
   (e.g. `POST http://adapter-host:8800/sensors/loading_dock/camera`).
3. Set the request body to the shape the webhook adapter already parses —
   no translation layer needed:

   ```json
   {"confidence": 0.91}
   ```

   (Add `"zone": "..."` to override the route's default zone per-message, or
   `"state": "on"` for a binary sound-alarm class. See
   [`webhook.rs`](../../src/adapter/webhook.rs) /
   [`mqtt_sensor.rs`](../../src/adapter/mqtt_sensor.rs) for the full payload
   grammar.)
4. Add matching `[[adapter.route]]` entries to `adapter_host.toml` — see the
   worked example in
   [`adapter_host.example.toml`](../../adapter_host.example.toml).
5. Set `auth_token` or `hmac_secret` on the webhook adapter (required for
   anything off loopback) and, if the camera and host aren't on the same
   trusted LAN, TLS.

Because this rides the existing webhook adapter, it inherits everything that
adapter already does for free: rate limiting, bearer/HMAC auth, TLS/mTLS,
and — same as every other adapter — the kernel's zone-regex, confidence
bounds, and 10-minute time coarsening apply unconditionally on the way in.

### Claim mapping

| reCamera Pro class / workflow | `ClaimKind` | Notes |
|---|---|---|
| `person` | `LargeObjectBoundaryCrossing` | Matches the Frigate adapter's mapping — kept consistent across vendors. |
| `car` / `vehicle` | `LargeObjectBoundaryCrossing` | Pair with an after-hours schedule in the flow editor to route to `VehiclePresenceAfterHours` instead. |
| `package` / `dog` / `cat` / `bicycle` | `SmallObjectBoundaryCrossing` | |
| sound class: glass break / alarm / raised voice | `AcousticImpulseInZone` | Sound classification result only — never route raw audio or a waveform. |
| "person in \<restricted zone\>" flow | `PresenceInRestrictedZone` | Scope the flow's trigger condition to the camera's configured zone, not a face/re-id match. |

---

## 3 · What Canary Vision Pro deliberately does NOT do

The reCamera Pro *can* run a VLM/LLM and caption a scene in free text
("a person in a red jacket carrying a box"). SecuraCV does not route that.

- The webhook adapter's payload grammar has no free-text field — a caption
  literally cannot cross into a `Claim` (see
  `spec/sensor_adapter_contract_v0.md` §2: *"A `Claim` MUST NOT contain...
  Free-form descriptive text"*).
- Configure the on-device flow to emit **only** a classifier label + score,
  the same discipline already used for the Grove Vision AI V2's person
  detector and Frigate's object labels.
- Treat VLM/LLM captioning as a **local-only, opt-in convenience** (e.g. an
  operator pulling up the reCamera's own Web UI to look at a live caption
  during an active incident) — never as something that feeds the sealed
  witness log.

This is the same reasoning that kept CAN-bus vehicle telemetry
passive-only in the vehicle-mount concept: the more capable the sensor, the
more discipline the integration boundary has to enforce.

---

## 4 · Where it's the right sensor (and where it's overkill)

Canary Vision Pro is a **$300 flagship**, not a swap-in for the $10–50
sensors elsewhere in the fleet. It earns that price on three things the
cheap sensors can't do: **starlight low-light imaging**, **on-device sound
classification** (not just motion), and **industrial protocol support** that
pairs with the existing 43B dev-playground work
([`display_peripheral_catalog.md`](./display_peripheral_catalog.md),
[`board_capability_map_43b.md`](./board_capability_map_43b.md)) for sites
that already run RS485/Modbus/CAN.

**Best-suited deployments:**

- **Multi-tenant property / HOA common areas** — stairwells, parking
  structures, loading docks after dark. Starlight imaging is the
  differentiator: cheap PIR/RF sensors already cover "something moved,"
  but a property manager investigating a lit-vs-unlit incident needs a
  camera that actually resolves detail at night, on-device, without a
  cloud NVR subscription.
- **Small business / retail after-hours** — one Vision Pro at the one
  higher-value chokepoint (back door, register area, equipment yard) is a
  better spend than several cheap units, and `VehiclePresenceAfterHours` +
  glass-break/alarm sound classification cover the two claims that matter
  most for a break-in narrative.
- **Warehouse / light-industrial safety zones** — `PresenceInRestrictedZone`
  around a machine cell or loading bay, with the same industrial-protocol
  story as the 4.3B dash board; a natural companion device on a site that
  already has RS485/Modbus sensors wired in.
- **Community / neighborhood chirp-channel anchor nodes** — its
  reliability and low-light range make it a reasonable "trusted node" in a
  mesh of cheaper sensors, *provided* it stays disciplined to the same
  coarse-claim vocabulary as everything else — it must not become the
  mesh's one high-fidelity, re-identifying node.

**Where it's the wrong choice:**

- **A single-family home entry/porch** — the $10 XIAO + PIR-class sensors
  (or Canary Vision's Grove Vision AI V2 pairing) already cover "someone's
  on the porch" for a fraction of the cost; Vision Pro's extra imaging
  headroom and sound classification go mostly unused there.
- **Bedside / wellbeing monitoring** — that's Canary Sense's mmWave radar
  lane (`docs/hardware/mr60bha2_radar_notes.md`), deliberately camera-free.
  Don't reach for a camera, however privacy-disciplined the integration,
  for a room where the radar's "no pixels, ever" story is the whole point.

---

## 5 · Open items before calling this "supported," not just "concept"

- No bench validation yet — this doc plus the `adapter_host.example.toml`
  block is the integration *design*, not a verified build. Follow the
  `boards.json` tier convention (`compile-tested` → `verified`) once a real
  unit is on the bench: confirm the flow editor's HTTP action node actually
  produces the documented payload shape, and that auth/TLS survive a
  restart.
- Sound-classification class taxonomy (glass break, alarm, raised voice,
  etc.) needs to be pinned to specific SenseCraft audio model IDs once
  trained — this doc describes the target `ClaimKind` mapping, not a
  shipped model.
- No enclosure/mount work done yet; the reCamera Pro's two-T-stud interface
  is compatible with the same OpenSCAD mounting system used elsewhere
  (`docs/hardware/enclosure/`), but no profile exists for it yet.
