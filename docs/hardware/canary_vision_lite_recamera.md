# Canary Vision Lite (Seeed reCamera 2002w) — device guide & positioning

**Status:** concept / config-only integration, no new firmware required.
**SKU:** Seeed reCamera 2002w 8GB (~$35–55 depending on eMMC/wireless config
— reseller-sourced estimate, not Seeed's own price page; confirm before
budgeting).

Canary Vision Lite is the coverage counterpart to
[Canary Vision Pro](./canary_vision_pro_recamera.md): same integration
pattern (the existing HTTP webhook sensor adapter, zero new firmware), a
tenth of the price, and a deliberately smaller on-device model. Where Vision
Pro earns a $300 spend on the *one* chokepoint that can't afford to guess,
Vision Lite is cheap enough to put in several mundane spots that individually
don't justify a flagship sensor.

---

## 1 · What it is

The reCamera 2002w is a modular, RISC-V-based AI camera — a different chip
family from the Pro's Rockchip RV1126B, not a cut-down version of it:

- **SoC:** SOPHGO SG2002 — dual RISC-V C906 cores (1 GHz + 700 MHz) plus an
  8051 MCU with 8 KB SRAM, **1 TOPS INT8**.
- **Sensor:** OV5647, 5 MP (2592×1944) — no starlight/low-light claim; this
  is a daylight/well-lit-scene sensor, not the Pro's night sensor.
- **Memory/storage:** 256 MB RAM / 8 GB eMMC (this SKU).
- **Connectivity (2002w variant):** WiFi 2.4G/5G, Bluetooth 4.2/5.0 — the
  "w" means wireless; the plain 2002 is wired-only.
- **Construction:** modular — a Core board, Sensor board, and Baseboard.
  USB-C, UART, and microSD are standard; **PoE and CAN bus are
  baseboard-dependent options, not guaranteed** — confirm which baseboard a
  given unit ships before assuming those pins exist.
- **On-device software:** YOLOv11 and a Node-RED-style flow editor ship
  built in, open-source (Seeed's `OSHW-reCamera-Series` repo).

Because the SG2002 is a 1 TOPS classifier chip with no published VLM/LLM
support, there is no captioning capability to discipline here the way Vision
Pro's flagship RV1126B needs — Vision Lite structurally can't produce a
free-text scene description even if someone wanted it to.

---

## 2 · How it plugs in (identical pattern to Vision Pro)

Same integration as Vision Pro — see
[that doc's §2](./canary_vision_pro_recamera.md#2--how-it-plugs-in-no-new-firmware-no-new-rust)
for the full walkthrough. In short: the 2002w's flow editor POSTs
`{"confidence": <score>}` (optionally `"zone"` / `"state"`) to the existing
webhook adapter (`src/adapter/webhook.rs`) — no new firmware, no new Rust.
Add a `[[adapter.route]]` per unit in `adapter_host.toml`; see
[`adapter_host.example.toml`](../../adapter_host.example.toml) for a worked
example.

### Claim mapping

| reCamera 2002w class / workflow | `ClaimKind` | Notes |
|---|---|---|
| `person` | `LargeObjectBoundaryCrossing` | Same convention as Vision Pro and the Frigate adapter. |
| `car` / `vehicle` | `LargeObjectBoundaryCrossing` | Pair with an after-hours schedule for `VehiclePresenceAfterHours`. |
| `package` / `dog` / `cat` / `bicycle` | `SmallObjectBoundaryCrossing` | |
| "person in \<zone\>" flow | `PresenceInRestrictedZone` | Scope the flow's trigger to the camera's zone, not a re-id match. |

No sound-classification row: unlike the Pro, this SKU's baseboard/sensor
combination has no confirmed onboard microphone in this repo's sources —
don't assume an acoustic claim path without checking the actual baseboard.

---

## 3 · Where it's the right sensor (and where it's the wrong one)

**Best-suited deployments:**

- **Coverage over a chokepoint** — several side doors, a stockroom, a row
  of interior hallways — where a $35 WiFi camera per spot beats either
  running several cheap PIR sensors *and* still wanting visual confirmation,
  or spending Pro money everywhere.
- **Well-lit interior spaces.** No starlight sensor here — this is a
  daylight/indoor-lit-scene camera. Use Vision Pro (or a non-camera sensor)
  for anywhere genuinely dark.
- **WiFi-reachable spots without existing Ethernet runs** — the 2002w
  variant's WiFi/BLE radio avoids a wiring project the Pro's Ethernet/PoE
  default doesn't need to avoid.

**Where it's the wrong choice:**

- **The one high-value chokepoint** (loading dock, equipment yard,
  after-dark entry) — that's Vision Pro's job; this chip's 1 TOPS budget and
  ordinary sensor won't match it.
- **Anywhere needing sound classification** — glass-break/alarm coverage is
  a Vision Pro capability, not confirmed here.
- **Bedside / wellbeing monitoring** — still Canary Sense's camera-free
  mmWave lane (`docs/hardware/mr60bha2_radar_notes.md`), regardless of price.

---

## 4 · 3D model — reuses the Vision Pro mount

Seeed's confirmed mount interface — magnetic and/or 1/4"-20 tripod thread —
is consistent **across the reCamera line**, not per-SKU. The existing
[`canary_vision_pro_mount.scad`](./enclosure/canary_vision_pro_mount.scad)
header already says "or any reCamera-family unit," so this SKU doesn't get
its own mount file: the same keyhole-back / tripod-or-magnet-front adapter
applies here too — the same **measure your unit first** caveat applies
even more, since the 2002w's modular baseboard changes its body dimensions
depending on which baseboard is fitted.

---

## 5 · Open items before calling this "supported," not just "concept"

- No bench validation — same gap as Vision Pro. No unit has confirmed the
  flow editor's HTTP action node payload shape, or which baseboard variant
  (and therefore which optional pins) a real purchase would ship with.
- Price is reseller-sourced, not confirmed against Seeed's own listing at
  authoring time — verify before budgeting a multi-unit deployment.
- No sound-classification path confirmed (§2) — don't route an acoustic
  `ClaimKind` from this SKU without checking the actual hardware first.
