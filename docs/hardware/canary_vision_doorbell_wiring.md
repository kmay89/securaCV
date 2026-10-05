# Canary Vision Doorbell — button and glow ring: wiring, parts, firmware

**Status:** the firmware is written and its logic is host-tested
([`test_doorbell_logic.cpp`](../../firmware/tests_host/test_doorbell_logic.cpp));
the CI firmware builds compile it for both XIAO hosts. **It has not been
bench-tested on a real doorbell yet**, and none of the timings below is a
measurement. This page is the wiring for the released
[Vision Doorbell case](./enclosure/canary_vision_doorbell.scad) (the
**Lite** tier of the [doorbell dossier](./canary_doorbell_research.md)).

![Wiring: XIAO D1 to the button through R5 with C2 to ground; XIAO D2 through R3 to a 2N3904 that sinks the button LED, powered from 5V](./canary_vision_doorbell_wiring.svg)

## What it does

| You do | The doorbell does |
|---|---|
| Press the button | Seals a `doorbell` event into the witness's signed chain (with the network down too), then the ring **swells** up and eases back: "the house heard you". Home Assistant gets a real doorbell event; every Canary display on the broker shows *Doorbell heard*. |
| Press again within 3 s | Counted, not sealed and not re-announced: one impatient visitor is one ring, and nobody can flood the record by jabbing. A press after 3 s rings again. |
| Hold it down | Still one ring. Held past 15 s (ice, tape, a jammed plunger), it's reported **stuck** once; the ring goes steady and low, and nothing rings until it's released. |
| Nothing | The ring **breathes**: the witness is on, nothing is being recorded. |
| The house's broker is unreachable | The ring breathes **slower and dimmer**. An empty log reads as "unsure", never as "quiet". Presses are still sealed on the device. |
| Switch the doorbell off in Home Assistant | The ring eases to dark. A dark ring means a doorbell that is off: brightness can't go below 10 % while it's on. |

The ring never flashes. That's enforced in code, not left to a setting: the
output slews at a bounded rate, so no mode change or swell can jump, and
the host test checks it frame by frame. The colors are the button's own
LED: yellow (canary) or warm white, never red or blue
([no impersonation](../research/harm_reduction_prior_art.md)).

**It turns itself on.** The doorbell starts switched off, and the first real
press switches it on (and rings). A plain Vision never sees a press, because
its D1 just idles on the pull-up, so it never grows doorbell entities. You
can also flip the **Doorbell enabled** switch in Home Assistant.

### In Home Assistant

These appear on the Vision's device page through MQTT discovery; no
integration update is needed:

| Entity | Kind | What it's for |
|---|---|---|
| **Doorbell** | `event` (device class `doorbell`) | Fires `press` once per sealed ring. Use it as an automation trigger: ring a smart chime, send a phone notification, turn on the porch light. HA's HomeKit bridge can expose it as a doorbell. |
| **Doorbell enabled** | `switch` (config) | Turns the doorbell (button + ring) on and off. |
| **Doorbell glow** | `number` 10–100 % (config) | Brightness of the ring. |
| **Doorbell button** | `sensor` (diagnostic) | `ok`, `stuck`, or `ring_fault` (the glow timer could not start: the ring holds a steady glow instead of breathing, so it still never reads dark while on). |

While the doorbell is off, only the switch exists. The other three are
removed, so a Vision that isn't a doorbell doesn't show doorbell controls.

## Parts

All in [`bom_canary_vision.csv`](./bom_canary_vision.csv), items 19–19e:

| Ref | Part | Notes |
|---|---|---|
| BTN2 | 12 mm illuminated momentary button, short body, IP65+, **6 V LED version** | Separate LED +/− terminals and a normally-open (NO) switch with its common (C). Yellow suits the canary; any color works. Buy the **6 V** LED version, which has its own resistor. A **3 V** version needs a 100 Ω resistor in series with LED +. Body and terminals ≤ 18 mm behind the panel (the case asserts it). |
| Q1 | 2N3904 NPN, TO-92 | Low-side switch for the ring. Any small NPN works (BC547, 2N2222, MMBT3904). |
| R3 | 1 kΩ | XIAO D2 → Q1 base. |
| R4 | 10 kΩ | Q1 base → GND: keeps the ring dark while D2 floats. |
| R5 | 1 kΩ | Button NO → XIAO D1: ESD protection for an outdoor metal button. |
| C2 | 100 nF ceramic | XIAO D1 → GND, at the XIAO end: noise filter (~0.1 ms, far below the 30 ms debounce). |

Plus about 20 cm of thin stranded wire, solder and heat-shrink.
Everything fits in the body's button zone and cable well. Sleeve Q1, R3
and R4 together in one piece of heat-shrink and park them in the well.

## Wiring

| From | To | Why |
|---|---|---|
| XIAO **5V** | BTN2 **LED +** | The ring runs from 5 V, not from a GPIO. |
| BTN2 **LED −** | Q1 **collector** | Q1 switches the ring's ground side. |
| Q1 **emitter** | **GND** | |
| XIAO **D2** | R3 → Q1 **base** | 20 kHz PWM, no flicker and no audible whine. |
| Q1 **base** | R4 → **GND** | Dark at boot and whenever the doorbell is off. |
| XIAO **D1** | R5 → BTN2 **NO** | The press pulls D1 low (internal pull-up). |
| XIAO **D1** | C2 → **GND** | Next to the XIAO. |
| BTN2 **C** | **GND** | |

2N3904 pins, flat face toward you, legs down: **E · B · C**.

Pin numbers by board:

| XIAO pad | XIAO ESP32-S3 | XIAO ESP32-C3 | Role |
|---|---|---|---|
| D1 | GPIO2 | GPIO3 | button in |
| D2 | GPIO3 | GPIO4 | glow PWM |

D1 and D2 are used because the Grove Vision AI V2's socket owns the rest of the
header: D4/D5 (I2C, which this firmware uses), D6/D7 (UART) and the SPI
group D8–D10 with chip select on D3 (unused by this firmware, but wired to
the module). **Check once before you solder:** with the XIAO removed, put a meter
in continuity mode between the module socket's D1 and D2 pads and each of
its other pads. Neither should beep to anything except where you expect.
If your board revision differs, the pins live in one place each,
`DOORBELL_BUTTON_PIN` / `DOORBELL_GLOW_PIN` in
[`firmware/boards/xiao-esp32s3/pins/pins.h`](../../firmware/boards/xiao-esp32s3/pins/pins.h)
and [`xiao-esp32c3/pins/pins.h`](../../firmware/boards/xiao-esp32c3/pins/pins.h).

On the XIAO ESP32-C3, D1 is also that board's `EXT_LED_PIN_DEFAULT`. Nothing
in the Vision firmware uses it, but don't hang a status LED there on a
doorbell.

## Power

As designed, the doorbell runs from **USB-C**: a right-angle plug out
through the back plate's oval exit (BOM GRM1) to a 5 V supply. That's the
tested path for the Vision stack.

**Using the existing doorbell wires.** A doorbell button is usually fed by
a 16–24 V AC transformer. To power the Canary from it, put a doorbell-to-USB
converter behind the plate: an AC-input module rated for 8–24 V AC in, 5 V
≥ 1 A out. Two things to know:

- The old **mechanical chime will not ring** from the button any more,
  because the wires now carry power, not a switch. Ring your house instead
  through Home Assistant (the **Doorbell** event → a smart chime, a speaker,
  or a relay across the old chime's terminals). The Pro tier's carrier
  board puts that chime relay on the doorbell itself (dossier §8); the Lite
  doesn't have one.
- Keep the converter outside the sealed body or in its own sealed box. The
  body's cavity is sized for the Vision stack and the button.

## Bring-up

1. Flash the Vision firmware (XIAO S3 or C3 build) as usual.
2. Open the serial console. At boot a `[BELL]` block says
   `Doorbell off - the first press turns it on (button D1, glow D2, glow 60%)`.
3. Press the button: `[BELL] Doorbell on.`, then a `[BELL]` block with
   `Ring (pressed at … ms, sealed)`. The ring swells, then settles into a
   slow breath.
4. In Home Assistant, the **Doorbell** event shows `press`. Unplug the
   broker and the breath slows and dims; press again and it still swells
   (the press is sealed on the device).
5. Hold the button for 15 s: `[BELL] … reported stuck`, and the ring goes
   steady and low. Release it: `[BELL] Button released - no longer stuck.`

If step 3 does nothing, check the button with a meter: NO and C should
close when pressed. D1 should read ~3.3 V released and ~0 V pressed.

## How it compares with Ring

What this doorbell **does**, all on the house's own network and with no
subscription:

- The press is a **signed, hash-chained record** on the device, sealed even
  with the network down. A Ring press is a cloud event.
- It **tells the house locally**: Home Assistant, every Canary display on
  the broker, and any automation you write. No WAN round-trip is in that
  path. "Under a second" is the dossier's design target (§4.5, open item
  14), and **nobody has measured it yet**.
- The ring is **honest**: it breathes while the witness is on, breathes
  differently when the hub is gone, swells only for a sealed press, and is
  dark only when the doorbell is off.
- It's **open**: you can read every line that decides what a press does.

What it **refuses**, by design (dossier §3): live view, recorded clips,
faces, and two-way audio. A Canary is a witness, not a camera feed. If you
need to see who's at the door, this isn't that product, and the dossier
explains why.

## What's not done

- **Bench validation**: nobody has pressed a real one yet. The host test proves the logic;
  CI compiles the firmware; a bench unit is the next step.
- **The fleet beacon** (the broker-free BLE / ESP-NOW channel the displays
  also listen on) carries object classes, not a doorbell press. A press
  reaches the displays through the broker, not broker-free. Adding a press
  to the beacon is a wire-format change across the display firmware, the
  Lab and the Apple apps.
- **Displays say "Doorbell heard"**: the same row the WAP's acoustic
  doorbell detector already uses.
- **The scad comment** still calls the button's input "the multifunction
  input". D1 is the doorbell's own input now; the case geometry is
  unchanged.

## Where the code is

- [`firmware/common/doorbell/doorbell_logic.h`](../../firmware/common/doorbell/doorbell_logic.h): the
  decisions (debounce, holdoff, stuck, glow curves, the slew limit), pure and host-tested.
- [`firmware/projects/canary-vision/src/doorbell.cpp`](../../firmware/projects/canary-vision/src/doorbell.cpp): pins, the edge
  interrupt, LEDC PWM, NVS settings.
- [`firmware/projects/canary-vision/src/main.cpp`](../../firmware/projects/canary-vision/src/main.cpp): seal, swell, announce.
- [`firmware/projects/canary-vision/src/ha/ha_discovery.cpp`](../../firmware/projects/canary-vision/src/ha/ha_discovery.cpp): the four Home Assistant entities.
