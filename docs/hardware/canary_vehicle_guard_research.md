# Canary Vehicle Guard — parked-car witness (research & design dossier)

**Status:** concept — sourced research and a design, **no firmware, no bench unit**. Same honesty
tier as Fence Guard and Guardian: grounded in real hardware and real field data, nothing built yet.

**The one-sentence version:** a small IMU + LoRa node that lives in your parked car and answers two
questions your phone can't when the car is off-grid — *"did someone move my car?"* and *"where did I
last leave it?"* — over the mesh, with no cell and no tap into the car's own systems.

This **supersedes the earlier "Car Mode" concept.** Car Mode's whole idea was "the node boots when
the ignition-switched USB powers on, so *it having power* means *the car is here*." The rethink
below is why that was optimizing for the wrong moment — and what's actually worth putting hardware
in a car for.

---

## 1 · Why "Car Mode" was backwards

Car Mode sensed the one state that barely matters: **the car, running, with you in it.** You already
know you're in your car. The states worth witnessing are the ones you're *not* present for:

- **Parked and being moved without you** — theft, a tow, a break-in shove.
- **Parked somewhere you'll need to find again** — a vast lot, a strange city, after a blackout.

And here's the tell: Car Mode drew power from the **ignition-switched USB port**, which is **dead
exactly when those states happen** — the car is off and you're away. A witness for the parked,
owner-absent car must run on **always-hot power or its own battery**, not switched USB. Once you
accept that, the whole design flips from "announce I'm powered" to "watch a still car and report
motion."

We also looked hard at whether an in-car node could do anything by *talking to the car* — and the
answer is no. See §4.

---

## 2 · What it actually does

### 2.1 · Theft / tow witness (the flagship)

An onboard **IMU** (accelerometer + gyro) sits in a hardware low-power motion-wake mode; the node
**deep-sleeps** until the car *moves*. A move while the owner is away signs a **coarse "vehicle
moved" claim** and bursts it over LoRa to the home mesh. This is passive, cheap, low-power, and not
creepy — it's *your* car, it's *your* question, and the claim is coarse ("moved in zone X"), never a
track.

The IMU is the single best sensor for the role: ~200 Hz over I²C, a hardware motion/impact interrupt
so the MCU sleeps until something happens (essential for battery/parasitic-draw), and a deep bench of
prior art for vehicle move/impact detection. Reliability among candidate in-car sensors runs
**IMU ≫ temperature > light > sound**.

### 2.2 · Find-my-car / last-seen beacon

A low-rate (or on-motion) signed heartbeat carries a **coarse last-seen** to the mesh, so when the
car is parked off-grid — no cell, no home WiFi — your fleet still holds a "last heard here." This is
the one capability that genuinely **justifies dedicated in-car hardware over a phone**: a phone
tracker needs cell; LoRa reaches a home node from a driveway, a garage, or a trailhead lot when
cellular can't. Commercial Meshtastic vehicle trackers (SpecFive Voyager, RAK WisMesh Tag, SenseCap
T-1000E) exist for exactly this.

### 2.3 · Cabin temperature alert — as a *thermometer*, honestly scoped

A cheap temperature sensor (BME280-class) can alert on a hot/cold cabin threshold. Ship it as a
**temperature** alert ("cabin is 55 °C"), full stop.

**It is NOT a child/pet-presence safety device, and the docs must never imply otherwise.** Reliable
occupancy detection is a *hard, safety-critical* problem — the industry answer is **60 GHz mmWave
radar** (e.g. TI AWRL6432, the sensor class Euro NCAP steers child-presence detection toward), which
this fleet **already ships in [Canary Sense](./mr60bha2_radar_notes.md).** So "someone left a
child/pet in a hot car" is **not a new Vehicle Guard gadget** — it's Canary Sense pointed at a cabin,
optionally paired with this node's thermometer. Do not build occupancy on WiFi CSI: it's
power-hungry, thermally throttles in a sun-baked cabin, and a miss is a catastrophe.

---

## 3 · Relationship to the rest of the fleet

Vehicle Guard is the **motion/off-grid sibling** of the existing
[Canary Vehicle](./canary_vehicle_can.md), not a replacement for it:

| | **Canary Vehicle** (shipped adapter) | **Canary Vehicle Guard** (this concept) |
|---|---|---|
| Senses | vehicle CAN bus, passive read-only | onboard IMU (motion), optional temp |
| Answers | "the car's ignition changed → arrival/departure" | "the parked car was moved / here's its last-seen" |
| Best moment | driving up / leaving (bus is live) | parked, owner away (bus is asleep) |
| Radio | can_bus adapter → MQTT/signed chain | LoRa mesh (off-grid) + BLE when home |
| Power | vehicle/USB while active | always-hot or battery (parked state) |

Between them they cover the live car (CAN) and the still car (IMU/mesh) without either pretending to
do the other's job. Hot-car occupancy stays with **Canary Sense** (radar), by design.

---

## 4 · Why not talk to the car (BLE / OBD) — the road not taken

We checked whether an in-car ESP32/nRF node could pull useful data from the car itself. It can't, at
least not within the fleet's passive-witness rules:

- **The car's own Bluetooth is not a data API.** Modern vehicles expose BLE only for hands-free audio
  and phone-as-key; the phone-as-key stack is cryptographically locked specifically so a third party
  *can't* read or track it. An in-car node gets nothing useful by talking BLE to the car.
- **The only BLE-to-data path is an OBD-II dongle** (ELM327-class, node as client via ELMduino) — and
  it's **off-thesis**: reading OBD PIDs means *transmitting request frames onto the car's diagnostic
  bus* (every other sensor in the fleet is receive-only), it needs the OBD port and the ignition on,
  and it can keep modules awake / draw down the 12 V battery. It gives *nothing* when parked — the one
  time a parked-car witness matters. A legitimate telemetry project, but not this.
- **"Who's in the car" via scanning phones is out** — BLE/WiFi MAC randomization (iOS/Android rotate
  MACs) makes it unreliable, and fingerprinting arbitrary phones is squarely on the creepy side of the
  line. Presence via a tag *you own* (a Guardian, a beacon) is the honest way; scanning strangers is
  not.

So Vehicle Guard's value comes entirely from its **own IMU + a long-range radio**, treating the node
as a self-contained *sensor + off-grid reporter*, never as something that interrogates the vehicle.

---

## 5 · Hardware & radio

- **Host:** XIAO ESP32-S3 (the fleet's default; deep-sleep-friendly, WiFi for sync-when-home) or the
  nRF52840 sibling if battery-only operation is the priority. Unlike the worn Guardian, a car node can
  usually get **always-hot 12 V → USB power**, which relaxes the MCU choice.
- **Sensor:** an MPU/ICM/LSM-class IMU on the free I²C bus, in hardware motion-wake mode; optional
  BME280 for the thermometer.
- **Radio, by job** (the car's defining trait is that it moves in and out of home WiFi and parks
  off-grid):
  - **LoRa / Meshtastic** — the backbone for the parked/off-grid witness (theft/tow alert, find-my-car).
    The only transport that reaches a home mesh from a driveway or a strange lot with no cell.
  - **BLE** — the cheap short-range path when the car is home (arrival/departure to a home Canary, fleet
    discovery, ingesting sensors you own like BLE TPMS).
  - **WiFi** — opportunistic bulk sync / OTA / log upload when the car is home near a known AP. Not for
    off-grid, not for CSI-as-safety.
  - *One-line rule:* the **IMU** decides *whether* something happened; **LoRa** decides *that you hear
    about it* when the car is nowhere near your WiFi; **BLE** handles the cheap short-range stuff at home.
- **Mounting & heat:** the existing [`canary_vehicle_mount.scad`](./enclosure/canary_vehicle_mount.scad)
  already covers a cabin mount; its heat warnings apply doubly — a closed car exceeds +60 °C, past most
  SBCs'/LiPos' safe range. Always-hot vehicle power, light-colored case, and no LiPo baking on a sun-lit
  dash.

---

## 6 · Claim vocabulary

Vehicle Guard reuses existing kinds where honest, and needs at most one small addition:

| Signal | Proposed claim kind | New? | Notes |
|---|---|---|---|
| Parked car moved | reuse **`tamper_detected`**, or a new **`vehicle_moved`** | tbd | "Moved while I'm away" is a tamper-shaped event; whether it earns its own kind (to read as itself in the timeline) is the one open decision. |
| Find-my-car last-seen | *(liveness heartbeat, not a sealed claim)* | no | Coarse last-seen is a presence signal, consumer-side — same absence-inference idiom as Guardian/Car-Mode. |
| Cabin temperature threshold | reuse an environmental/telemetry field | no | A *temperature* alert, not a witness claim; never routed as occupancy. |

**Decision to lock during firmware design:** `tamper_detected` reuse vs. a `vehicle_moved` kind. No
new kind is strictly required to prototype.

---

## 7 · Never let it rot

- **Own-sensor, own-radio** — no dependency on proprietary, per-vehicle CAN IDs or on the car exposing
  anything; an IMU move and a LoRa burst work on any car, forever.
- **One canonical Meshtastic doc** — [`meshtastic_integration.md`](../meshtastic_integration.md) owns
  the transport mechanics; this doc only adds the parked-car specifics.
- **Shares mount + heat guidance** with [Canary Vehicle](./canary_vehicle_can.md) rather than carrying a
  second copy.
- **Honest tiering** — `concept` until an in-car unit is built and lived-with; the thermometer never
  graduates to "occupancy safety" (that's radar's job, in Canary Sense).

---

## 8 · Open items

- **No bench unit.** IMU move-detection thresholds (real theft vs. a passing truck's rumble vs. a car
  wash) are unmeasured — the classic false-alarm-tuning problem, to be settled on a real car.
- **Power source in practice** — an always-hot 12 V tap (add-a-fuse) vs. an internal battery is a real
  install decision with a parasitic-draw budget to measure.
- **`vehicle_moved` vs `tamper_detected`** is an open dictionary decision (§6).
- **Retiring Car Mode** — the [Car Mode concept](./canary_car_mode.md) is superseded by this; its
  Meshtastic `vehicle_arrival_departure` code path remains a valid capability, it just no longer
  headlines a concept of its own.
- The find-my-car beacon's optional GPS (if ever fitted) must ship **off by default** and emit only
  coarse last-seen — the same anti-tracking posture as Guardian.

---

*Sources: real ESP32/IMU vehicle move/impact-detection projects; ELM327/ELMduino OBD-over-BLE
documentation and its active-bus caveats; BLE/WiFi MAC-randomization limits; in-vehicle child-presence
detection via 60 GHz mmWave radar (TI AWRL6432, Euro NCAP direction); and commercial Meshtastic vehicle
trackers (SpecFive, RAK, SenseCap). The shared Meshtastic kit facts are cited in
[`canary_fence_guard_research.md`](./canary_fence_guard_research.md); the cabin-radar path in
[`mr60bha2_radar_notes.md`](./mr60bha2_radar_notes.md).*
