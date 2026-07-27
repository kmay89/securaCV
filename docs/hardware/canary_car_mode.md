# Canary Car Mode — retired (superseded by Canary Vehicle Guard)

**Status:** retired concept. Superseded by
[Canary Vehicle Guard](./canary_vehicle_guard_research.md). This page is kept as an honest record
of what we tried and why we changed our minds — not as a live product.

---

## What Car Mode was

Plug a stock, unmodified [Meshtastic](https://meshtastic.org) node into a car's **ignition-switched
USB port**. It booted when the car did, its Detection Sensor Module read "the chip has power" as a
detection, and it announced arrival to the mesh; departure was inferred from the heartbeats going
silent when the ignition (and the USB power) cut out.

## Why it was retired

The rethink was simple and damning: **Car Mode sensed the one state that barely matters — the car
running, with you in it — and drew power from a port that is dead exactly when a parked-car witness
would need it.** The states worth witnessing are the ones you're *not* present for:

- **Parked and moved without you** (theft, tow, a break-in shove).
- **Parked somewhere you'll need to find again** (a vast lot, a strange city, after a blackout).

Both happen with the car *off* and the ignition-USB *unpowered*. A witness for the parked,
owner-absent car has to run on **always-hot or battery power** and watch for **motion**, not power.
Once you accept that, the design flips entirely — from "announce I'm powered" to "watch a still car
and report movement."

## What replaced it

**[Canary Vehicle Guard](./canary_vehicle_guard_research.md)** — an onboard-IMU + LoRa witness that
deep-sleeps until the parked car *moves* (theft/tow), and carries a coarse **find-my-car** last-seen
over the off-grid mesh. It never taps the car's own systems (we checked: the car's BLE is locked, and
the only OBD data path transmits onto the diagnostic bus and needs the ignition on — off-thesis).
Hot-car child/pet safety stays with **[Canary Sense](./mr60bha2_radar_notes.md)**'s mmWave radar, not
a WiFi/CSI guess.

## What survives from Car Mode

The Meshtastic adapter's **`vehicle_arrival_departure`** claim path
([`src/adapter/meshtastic.rs`](../../src/adapter/meshtastic.rs)) — including its edge-triggered
heartbeat handling — remains a valid capability. It simply no longer headlines a concept of its own;
any Meshtastic node configured for a vehicle can still emit that claim. See
[`meshtastic_integration.md`](../meshtastic_integration.md).
