# SecuraCV Canary Fence Guard (XIAO ESP32S3 + Wio-SX1262) — CONCEPT

> **Status: concept — research staged, firmware pending.** Nothing in this
> directory ships. There is no validated hardware, no BOM, no enclosure,
> and no buildable target — `src/main_stub.cpp` refuses to compile on
> purpose. This project exists so the coming-soon teaser on the Canary
> House and the concept card in canary.local point at something honest:
> the requirements, the architecture sketch, and the open questions,
> written down *before* the code. If you want this to exist, say so:
> [open a request issue](https://github.com/kmay89/securaCV/issues/new?title=Concept%20request%3A%20Canary%20Fence%20Guard)
> — concepts become candidates when people ask for them.

## What it would be

A **perimeter witness** clamped to a chain-link fence at the edge of the
property — past the WiFi, past mains power. It feels the fence's own
vibration signature (climb, cut, rattle, lean), debounces it into the
same small signed claims every Canary speaks, and carries them home over
**Meshtastic** — the LoRa mesh — with the solar relay pod as its first
hop. Solar over battery, weather-sealed, and mounted in **shade by
preference**: lithium cells age in heat, and a fence line usually has a
shaded run.

Same witness rules as the rest of the fleet, non-negotiable:

- **What leaves the device:** fence-event claims (climb/cut/rattle as
  debounced states), Ed25519-signed over a v1 canonical, hash-chained.
  No audio, no imagery, no identities — the fence knows *that*, never *who*.
- **No location leakage:** Meshtastic's position broadcast stays off.
  A fence guard that advertises its own coordinates is a map of your
  perimeter; ours says nothing but its claims.
- **Degrade honestly:** low sun, cold cell, lost mesh — each is a
  health claim, never a silent gap.

## Target hardware (research staged)

The staged platform is Seeed's **XIAO ESP32S3 + Wio-SX1262 kit for
Meshtastic & LoRa** — the same XIAO family as every other Canary host,
with a Semtech SX1262 LoRa transceiver on a board-to-board connector.
A vibration sense (accelerometer on the spare I²C, or a piezo/knock
sensor on an ADC pin) is the open hardware question below.

The full research dossier (verified specs, bands, TX power, power
budget, solar behavior, free pins, Meshtastic firmware support) lives in
[`docs/hardware/canary_fence_guard_research.md`](../../../docs/hardware/canary_fence_guard_research.md)
— numbers live there, not here, so they exist in exactly one place.
Its headline findings for the open questions below: a **zero-custom-code
Phase 0 exists** (stock Meshtastic + LIS3DH + Detection Sensor module),
and the S3-vs-nRF52 power gap is real and quantified (~11× idle) — the
field mule's data decides the platform.

## Requirements (draft v0)

| # | Requirement | Why |
|---|---|---|
| R1 | Sense fence vibration and classify climb / cut / rattle / wind, on-device | the raw waveform never leaves; only the debounced claim does |
| R2 | Speak Meshtastic (LoRa mesh) as primary transport; no WiFi dependency | the fence line is past the WiFi's edge by definition |
| R3 | Ed25519-sign every claim over a v1 `fence` canonical; hash-chain per device | same trust surface as canary-sense/wap — TOFU pin, ✓ verified |
| R4 | Meshtastic position broadcast disabled; no coordinates in any packet | a perimeter node must not map the perimeter |
| R5 | Solar + battery power plane; survive multi-day overcast; report cell health | off-grid or it doesn't exist |
| R6 | Shade-tolerant solar budget (panel sized for indirect light) | shade preferred — cool cells live longer |
| R7 | Weather-sealed enclosure, chain-link clamp mount, tool-free service | it lives outside on a fence |
| R8 | False-alarm discipline: wind and traffic never sound like a climb | the grammar is the product — same rule as every Canary |

## Architecture sketch (pending)

```
[vibration sensor]──ISR ring buffer──[feature FSM]         (R1, R8)
                                        │ debounced events
                                  [claim builder]          (R3)
                          common/identity/device_signature
                                        │ signed claims
                                  [mesh transport]         (R2, R4)
                     SX1262 ← Meshtastic firmware or radiolib?
                                        │
                              [power supervisor]           (R5, R6)
                        solar in → charge state → health claims
```

**The load-bearing open question:** whether the ESP32-S3 runs stock
Meshtastic firmware with our witness logic as a module, or our firmware
with a Meshtastic-compatible transport layer. Stock Meshtastic gets the
mesh for free but constrains the signing/claim pipeline; native keeps
the witness stack pure but re-implements mesh routing. The research
dossier's findings on Meshtastic's module API decide this.

## Open questions

1. Vibration sense: accelerometer (LIS3DH-class, I²C, interrupt-wake)
   vs piezo on ADC — which survives the power budget in deep sleep?
2. Firmware split: Meshtastic-module vs native-with-mesh-transport (above).
3. ESP32-S3 vs nRF52 power reality check — the community consistently
   favors nRF52 for battery Meshtastic nodes; does a shaded solar panel
   cover the S3's appetite? (Dossier has the numbers.)
4. Claim canonical: new `fence` domain vs reusing the `sense` event shape.
5. Enclosure: IP rating target, clamp geometry for common chain-link
   gauges, antenna position vs the metal fence plane (a fence is a
   reflector — mount standoff matters).

## Directory contents

| File | What it is |
|---|---|
| `README.md` | this — the concept's single source of truth |
| `include/fence_guard_requirements.h` | R1–R8 as named constants, so the future firmware imports its requirements instead of re-typing them |
| `src/main_stub.cpp` | honestly-guarded skeleton — `#error`s if built, so the stub can never be mistaken for firmware |

No `platformio.ini` on purpose: a concept has no build target. When the
open questions close, this project gets envs in `firmware/envs/platformio/`
like every real Canary, and this README's status line changes.
