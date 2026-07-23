# Display peripheral catalog — what plugs into the 4.3B, why, and for what

The Waveshare **ESP32-S3-Touch-LCD-4.3B**'s terminal block (isolated DI/DO,
I²C sensor header, RS485, CAN) is the family's one *screwdriver-grade*
wiring surface — the board designed so a homeowner can attach real security
hardware without soldering, without a level shifter, and without being able
to kill the MCU. This catalog is the curated answer to "what should we
support on it?": each candidate with **why**, **what it's for**, and an
honest status — plus the combination plays that turn cheap single-purpose
sensors into a system.

The mindset here is borrowed from a field that solved this decades ago.
**ATM and branch security** never relied on one clever sensor; it layered
many dumb, reliable ones — contacts, mats, beams, shock sensors, supervised
loops — and put the intelligence in *how they were combined and monitored*.
Homes deserve the same discipline, with one extra constraint the bank never
had: **the person wiring it is not a technician.** So every "supported" row
below must stay inside the simplicity contract:

> **The simplicity contract.** A supported peripheral is: wired with ≤ 3
> screw-terminal wires; hot-testable on the [dev playground bench](./dev_playground_43b.md)
> (a station shows the wiring on the glass and proves the part works before
> it's trusted); documented in one paragraph; and harmless when miswired
> within the board's stated limits. If a candidate can't meet that, it
> doesn't get "easy" status — it gets an honest harder tier or a different
> home in the ecosystem.

**Electrical ground rules** (the full table + protections:
[`dev_playground_43b.md`](./dev_playground_43b.md) §Why this is safe; pin
truth: `firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h`):
DI0/DI1 are optocoupled 5–36 V inputs referenced to DI COM; DO0/DO1 are
optocoupled open-drain sinks ≤ 450 mA (external supply carries the load —
flyback diode on anything inductive); the I²C header is a shared 3.3 V bus
(mind the [address landmines](./dev_playground_43b.md#pin-tracker));
RS485 is an auto-direction half-duplex pair; CAN has a jumper-selectable
120 Ω terminator. **Low-voltage DC only, never mains**, wire with supplies
off.

Status vocabulary (matching the
[capability map](./board_capability_map_43b.md)): **Station** = a live
playground bench station exists today · **Runtime** = driven in the shipping
witness firmware · **Candidate** = fits the contract, wave-ready ·
**Research** = value clear, questions open · **Elsewhere** = real, but its
home is another device in the ecosystem, not this terminal block.

---

## 1 · Isolated inputs (DI0/DI1) — the contact universe

Anything that closes or opens a circuit. This is 90 % of physical security
sensing, it's the cheapest hardware in the catalog ($2–$15 parts), and the
firmware already ingests it honestly: a debounced DI edge becomes an
**unsigned local event** in the fleet model (`field_io.cpp` — the dash
verifies others' chains but holds no signing key, so it never forges witness
provenance; see capability map §2).

| Peripheral | Why / what for | Status |
|---|---|---|
| Doorbell button (dry contact) | The friendliest first wire; DO0 "ding" link demoed on the bench | **Station** (doorbell) |
| PIR motion module (relay/OC output) | The workhorse: presence in a garage, shed, hallway — pick relay-output modules (5–12 V), *not* bare 3.3 V hobby boards | **Station** (intrusion) |
| Reed + magnet (door/window contact) | The single highest value-per-dollar sensor in existence | **Station** (intrusion) |
| Laser/IR break-beam receiver | Driveway, gate, corridor tripwire; `held_ms` gives crossing time | **Station** (intrusion) |
| Glass-break detector (12 V alarm-grade) | The classic alarm-industry part, exactly what DI was built for; event name `glass_break` already classifies **Alert** | Candidate |
| Shock/vibration sensor (alarm-grade) | On a safe, a toolbox, a bike rack — the ATM skin sensor, domesticated | Candidate |
| Tilt sensor | Garage/roller doors and lids: open-angle beats a contact there | Candidate |
| Panic/holdup button (latching) | Bedside or under-desk; wired to the `tamper_contact`/panic class → **Tamper**, which nothing can silently dismiss | Candidate |
| Water-leak sensor (relay type) | Utility room; not security, but the same wire and the family already renders environment lines | Candidate |
| Smoke/CO **auxiliary relay** output | Many detectors offer a dry relay contact; `smoke`/`co_alarm` already classify **Alert**. *Supplements* a code-required alarm, never replaces it | Candidate |
| Pressure mat | Hallway/stair mat — the hotel-lobby trick; needs the NC wiring note below | Research (mat quality varies wildly) |

**Only two zones?** By design — and it's not the ceiling. The zone-expansion
path is a **Modbus DI module on RS485** (§4): 8–16 more supervised zones on
two wires, no new firmware concepts. Wire the two precious DI channels to
the two things that must work with the network *down*.

**Wire it like the bank did:** prefer **normally-closed** (NC) circuits for
every security contact. A cut or unplugged NC loop reads as a trip, not as
silence — fail-loud is already this firmware's ethos ("silence is never
rendered as safety"), and NC wiring extends it to the copper. (True
resistor-supervised EOL loops need an analog read this board doesn't expose;
NC gets most of the benefit with none of the complexity. An ADS1115 on I²C
or a Modbus alarm-input module is the honest upgrade path.)

## 2 · Isolated outputs (DO0/DO1) — the dash can act

≤ 450 mA open-drain each, external supply, bounded by firmware (pulse/latch
auto-release on the bench; the runtime siren is capped at 5 min and opt-in,
disarmed by default — capability map §2).

| Peripheral | Why / what for | Status |
|---|---|---|
| Chime / electronic sounder | The doorbell's other half | **Station** (chime) |
| Strobe / beacon | Visual alarm for loud environments or hearing-impaired households | **Station** (strobe) |
| Siren (12 V, < 450 mA) | The bounded, opt-in alarm output already in the runtime | **Runtime** (siren on DO0) |
| **IR illuminator bar (850/940 nm night-vision LEDs)** | The clever one: the dash powers infrared floodlight **only while a night alert stands** — invisible to the eye, it makes every camera on the property (any brand) see in the dark exactly when it matters, and draws nothing the rest of the year. A 12 V bar at < 450 mA covers a porch | Candidate |
| Interposing relay → bigger loads | Gate opener pulse, floodlight, mains-rated loads via a proper relay/contactor — the DO drives the coil (flyback diode!), an electrician owns the far side | Candidate |
| Smart-panel trigger input | Many alarm panels accept a dry "key-switch"/trigger zone: the fleet's worst-severity can arm a legacy panel's bell circuit | Research |
| Door strike / maglock | **Deliberately not recommended** as a DIY wire: locking hardware is life-safety (egress codes) and security-critical. If access control is the goal, integrate a real controller over RS485 (§4) and let it own the lock | Elsewhere (by policy) |

## 3 · I²C header — the sensor shelf

Shared 3.3 V bus with the touch controller and expander — hot-pluggable,
census-scanned every 3 s on the bench, and strictly for *board-adjacent*
sensing (centimeters of wire, not room runs). Respect the
[reserved addresses](./dev_playground_43b.md#pin-tracker); a TCA9548A mux
(0x70) fans out to 8 sub-buses when candidates collide.

| Peripheral | Why / what for | Status |
|---|---|---|
| VEML7700 / BH1750 (lux) | Ambient light: auto night-mode trust, "hallway light left on after midnight" as an event, and the gate for the IR illuminator | **Station** (light) |
| VL53L0X (time-of-flight) | Reflective beam-gap, package-on-porch depth check, tamper ("something now blocks me") | **Station** (tof) |
| MPR121 (12-pad cap touch) | Hidden touch controls through the printed shell; the coupon-thickness bench test | **Station** (captouch) |
| PCF8563 RTC | Trusted time when NTP is blocked — a *witness integrity* upgrade, not a convenience | Built · bench-gated (`FEATURE_RTC`, capability map §6) |
| SHT4x / AHT20 (temp/RH) | The care wave already renders room-comfort lines; this is its local sensor. **Mind 0x38**: AHT20 collides with the CH422G — SHT4x (0x44) is the safe pick | Candidate |
| BME280 (temp/RH/pressure) | One-part environment row (0x76/0x77, clear of landmines) | Candidate |
| SGP40 / ENS160 (air quality) | "Air got weird in the garage" is a security-adjacent event (solvents, exhaust) | Research (calibration honesty) |
| ADS1115 (4-ch ADC) | The **analog gateway**: pressure mats, resistive leak ropes, supervised EOL loops — analog sensing this board otherwise can't do | Candidate |
| TCA9548A (I²C mux) | Address-collision escape hatch; documented in the pin tracker already | Candidate (documented, not yet driven) |

## 4 · RS485 / Modbus — the industrial doorway

The lingua franca of building security and energy gear, already built and
bench-gated in firmware (`FEATURE_RS485`, pure Modbus core host-tested —
capability map §3). This is where the dash stops being a display and becomes
a **gateway**: it reads a professional device's registers and re-witnesses
them into the fleet's signed-and-verified event pipeline.

| Peripheral | Why / what for | Status |
|---|---|---|
| Modbus DI/DO expander module | **The zone-expansion answer**: 8–16 alarm zones + spare relays on two wires, $15–$40 | Candidate (first RS485 wave) |
| Energy meter (DIN-rail Modbus) | **Witness the mains**: "power cut at 02:14" vs "breaker 4 only" — the infrastructure attack (cut the power, then enter) becomes an event. The ATM lesson: monitor the utilities, not just the doors | Candidate |
| Alarm panel / access controller (Modbus-speaking) | Bridge the legacy install: zones and arm-state appear on the glass, chain-logged; the controller keeps owning locks | Candidate |
| Weather / wind / rain station | Perimeter context (was that gate bang wind?) and awning/gate automation input | Research |
| Industrial mmWave presence sensor (RS485 out) | Alarm-grade presence for a garage/warehouse bay where I²C can't reach | Research |
| HVAC / VFD / PLC gear | The long tail — read-only witnessing of whatever the building already speaks | Research |

Constraint to design around (capability map §3): RS485 shares GPIO43/44 with
the UART console — RS485 builds log on native USB CDC only.

## 5 · CAN bus — the vehicle & gate wave

Dedicated transceiver, terminator jumper, driver built and bench-gated
(`FEATURE_CAN`, capability map §4).

| Peripheral | Why / what for | Status |
|---|---|---|
| Gate / barrier controller | Driveways and small commercial: witness (and eventually command) the barrier on its native bus | Candidate |
| Vehicle presence (OBD-II / J1939 tap) | "The van started at 03:00" as a chain-logged event — fleet yards, tool trailers | Research |
| CANopen building automation | Same story as the Modbus long tail, different physical layer | Research |

## 6 · No wires at all — the radios

Already part of the peripheral story even though nothing lands on the block:
passive **BLE chirp** liveness/tamper (Driven), **ESP-NOW peer presence**
(built · bench-gated), and the finder-mesh idea in
[`display_platform_vision.md`](./display_platform_vision.md). Every radio
"peripheral" obeys the same honesty rule: unsigned observations, never
forged provenance.

## 7 · Not this board — and where it actually lives

The honest section. Saying no here is what keeps the rest simple.

- **Cameras and microphones — never, by promise.** `HAS_CAMERA 0` /
  `HAS_MICROPHONE 0` is a design commitment, not a gap: *the display shows,
  it doesn't watch* — that's what makes it welcome in a bedroom. Cameras
  live in **canary-vision** witnesses (on-sensor person detection, events
  not video) — see
  [`canary_vision_getting_started.md`](./canary_vision_getting_started.md).
- **RTSP / ONVIF cameras.** Real and worth supporting — **in the viewer /
  Home Assistant layer**, where video belongs and where the fleet's verified
  events can drive recording and overlays. The dash will not render camera
  streams: it breaks the privacy promise first, and only then the RAM
  budget. What the dash *does* offer any camera, tonight, is §2's
  alert-gated IR illuminator.
- **Night-vision LEDs** land in **two** honest places: the DO-driven
  illuminator bar here (§2), and camera-side IR on the vision devices' own
  BOM. Same LEDs, different owner.
- **Raw-GPIO hobby parts** (DHT22, WS2812 strips, HC-SR04…): the 4.3B
  breaks out **zero** raw GPIO — that's its safety story, not a flaw. Use
  the I²C equivalent (SHT4x, VL53L0X) or give the part to a Canary that has
  pins (see [`canary_peripheral_build_plan.md`](./canary_peripheral_build_plan.md)).
- **Direct analog sensors:** no exposed ADC. The ADS1115 (§3) or a Modbus
  analog module (§4) is the path.
- **Mains-voltage anything:** interposing relay + electrician, always.

## 8 · Combination plays — the ATM lessons, domesticated

The catalog above is parts; this is the intelligence. Each play uses only
supported/candidate rows and the fleet semantics that already exist
(severity ladder, unsigned local events, ack/mute, journaling).

1. **Two-technology confirmation.** The false-alarm killer: PIR *and*
   break-beam covering the same approach; either alone → Notice, both
   within a window → Alert. Banks never trusted one sensor; neither should
   a driveway. (Pure-logic candidate for `field_io_logic.h` — a
   host-testable coincidence gate.)
2. **Fail-loud copper.** NC loops everywhere (§1): cutting, unplugging, or
   crushing a cable *is* the event. Matches the firmware's existing
   baby-monitor honesty — now end to end, glass to screw terminal.
3. **The bait asset.** A reed contact under the thing nobody should touch —
   medicine cabinet, document drawer, gun safe, till drawer in a shop. Zero
   false alarms by construction (nobody legitimate opens it at 3 a.m.), the
   highest signal-per-dollar wire in the catalog. The ATM version was the
   bill-trap switch.
4. **Environment-aware alarming.** The lux sensor gates behavior: IR
   illuminator only in the dark; "light left on" notices only after
   midnight; glass-break sensitivity context. Cheap sensor, smarter system.
5. **Witness the infrastructure.** The energy meter (§4) plus the existing
   link-loss alarms: power, network, and broker each have a *first-class*
   failure event. The professional attack starts with the utilities — the
   ATM world learned to alarm the power feed, not just the vault.
6. **Timing as a sensor.** DI `held_ms` and beam-crossing times are already
   measured on the bench: a door held 40 s is a prop-open event, not an
   entry; two beams in sequence give direction (in vs out). Logic, not
   hardware.

## 9 · How a row moves left

Candidate → Station/Runtime follows the established rituals, in order: the
[playground station ritual](./dev_playground_todo.md) (drift-locked bench
station + sim + website carry), the
[capability-map shipping reality](./board_capability_map_43b.md) (pure core
host-tested, feature-gated runtime, dedicated CI env, byte-neutral
emulator), and a line in the
[activation bench checklist](./board_43b_activation_bench.md). BOM entries
join [`bom_canary_display.csv`](./bom_canary_display.csv) when a row
reaches Station. Nothing skips the bench, and this table updates in the
same PR as the code — the catalog and the firmware must never disagree.
