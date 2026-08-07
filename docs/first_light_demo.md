# First Light — the Vision + Nightlight pair demo

Two devices, one desk, nothing else: a **Canary Vision** (XIAO host + Grove
Vision AI V2 — the NPU camera module) and a **Canary Nightlight** (the 1.47"
pocket glass). Power both, wave at the camera, and the glass says **PERSON**
with the trigger timing in milliseconds — no WiFi, no hub, no broker, no app,
no account. It is the smallest possible demonstration of what SecuraCV *is*:
a camera that tells you **what it saw**, never shows anyone what it recorded,
and does it fast enough to watch.

This is also the pairing behind the lowest-cost entry package we plan to
offer (a Vision kit plus a Nightlight, boxed as a pair). The store cannot
sell radio-bearing kits until FCC authorization completes — see the website's
store page for the honest status — but the firmware below is real and anyone
with the two boards can run it today.

## What the demo shows

1. **Detection, on device.** The Grove Vision AI V2 runs person detection on
   its own NPU. No pixels leave the camera — the host firmware receives
   boxes and scores over I2C, nothing else.
2. **The edge, on air, immediately.** The Vision's presence FSM debounces
   the detection; the moment it flips, the fleet-link presence beacon
   (alert + class + confidence, 13 bytes) goes out on every carrier at once
   — including ESP-NOW, the router-free band this demo rides.
3. **The glass reacts.** The Nightlight's pair-demo card pulses amber,
   names the class and confidence, and prints **glass react** — the time
   from its own radio receive to the paint that showed you the edge,
   measured on its own clock.
4. **The camera's side of the clock.** The Vision logs its own numbers on
   serial: the NPU invoke round-trip in microseconds, and
   detection-edge-to-air in milliseconds. On host boards **with a user LED**
   it also pulses that LED on every edge — stand where you can see both and
   the LED-to-glass gap is the whole latency story, no instruments needed.
   Know your host before promising the LED: the ESP32-C3 DevKit
   (`canary-vision-default`) and the XIAO ESP32-S3 Sense
   (`canary-vision-xiao-s3`) have one; the **XIAO ESP32-C3 — the kit board —
   has no user LED**, so there the serial `PAIR` lines are the camera-side
   record and the glass carries the show.

## What the numbers honestly are

There is **no shared clock** between the two devices, and the beacon wire
format deliberately carries no timestamp — so no single "end-to-end
latency" figure is claimed anywhere. Each side reports only what it
measured on its own clock:

| Number | Where | What it measures |
|---|---|---|
| `invoke took N us` | Vision serial (`PAIR` lines) | One SSCMA invoke round-trip to the NPU module |
| `detection edge on air N ms` | Vision serial (`PAIR` lines) | FSM edge → ESP-NOW frame handed to the radio |
| `glass react N ms` | Nightlight card | Radio receive drain → the paint that shows the edge (the panel flush after it costs at most one more frame) |
| the LED → glass gap | Your eyes | The only true end-to-end reading in the room, and it needs no clock at all — hosts with a user LED only (the XIAO ESP32-C3 has none) |

Performance varies by hardware, distance, and channel congestion. Run it
before quoting it.

## Running it

**Flash the pair** (both are standard build environments):

```sh
# The camera (XIAO ESP32-C3 host + Grove Vision AI V2 module)
pio run -d firmware/projects/canary-vision -e canary-vision-xiao-c3 -t upload

# The glass (Waveshare ESP32-C3-LCD-1.47)
pio run -d firmware/projects/canary-display -e canary-display-nightlight-c3 -t upload
```

The Grove module itself needs a person-detection model loaded once (the
SenseCraft flow — see [the Vision guide](../canary-local/devices/vision.json)
data or the Lab's Vision bench).

**Then:**

1. Power both devices. Factory-fresh (no WiFi provisioned), both radios
   park on the same fallback channel — that is the whole rendezvous.
2. On a factory-fresh Nightlight the demo card **walks up to you**: the
   moment the glass hears a camera on the router-free band, the wizard
   opens by itself. On a provisioned Nightlight, hold BOOT for 5 seconds.
3. The card adopts the first Vision it hears and shows its witness name
   (`SCV-XXXX`). **Tap** to keep it — the lock is remembered across
   reboots, and the card ignores every other camera from then on.
   **Double-press** forgets it; **hold** leaves the demo.
4. Wave at the camera. On a host with a user LED, watch the LED, then the
   glass; on the XIAO ESP32-C3, watch the glass and read the camera's side
   on serial.

## Where the seams are (read before demoing to a customer)

- **The beacon is an unsigned hint channel.** Trust on it never rises above
  presence — the card says "unsigned presence hint" because that is what it
  is. Verified means an Ed25519 signature checked against a pinned key, and
  that machinery (chain verify over MQTT) is untouched by the demo; the
  demo never renders a verification badge.
- **Person-only today.** The beacon vocabulary is the `ObjectClass` set
  (person / vehicle / animal / package) and the Vision's pipeline currently
  classifies person; the card widens with the pipeline, never past that
  vocabulary (Invariant II — a face or a plate is a rejected PR).
- **Channel rendezvous has one gap by design.** A provisioned device rides
  its network's channel; a factory-fresh one parks on the fallback. Both
  fresh: they meet. Both on the same WiFi: they meet (and the LAN band
  carries the beacon too). One fresh + one on WiFi: they may sit on
  different channels — provision both or neither. The demo card's
  "listening…" face is the symptom.
- **The lamp doctrine holds.** The Nightlight's lamp is decor and never a
  status; the demo is an explicit, chip-labeled modal surface — words
  first, the amber wash only echoes them — and it never touches the lamp,
  the attention ladder, or the verification badges. A detection while the
  demo is *closed* lands exactly where it always did: the fleet model's
  event line and attention rules.
- **This is a demo surface, not a security feature.** It shows speed and
  honesty; it does not record, does not alert beyond the room, and closes
  itself the instant anything urgent needs the glass.

## How it is built (for the next engineer)

Nothing here invented a protocol. The pieces, in dependency order:

- `firmware/common/fleet_link/fleet_beacon.h` — the 13-byte v2 beacon
  (unchanged; the wire carries exactly what it always carried).
- `firmware/common/fleet_link/fleet_beacon_espnow.h` — the ESP-NOW band's
  constants: broadcast address, 5 s refresh, fallback channel (agrees with
  the mesh's `MESH_FALLBACK_CHANNEL`).
- `canary-vision src/net/fleet_espnow.cpp` — the transmitter (the receive
  half in canary-display existed first; this file completed the band). The
  payload module (`fleet_beacon_payload.cpp`) also stamps the edge time so
  carriers can log edge-to-air.
- `canary-display include/canary/pair/pair_demo.h` — the pure core: adopt/
  lock/forget, the mirrored edge rule, staleness, the 5 s HoldGate, the
  auto-open rule. Host-tested in `tests_host/test_pair_demo.cpp`.
- `canary-display src/ui/pair_demo_ui.cpp` — the LVGL card + the one NVS
  key (`scv-nl/pairfp`), on the commission_ui modal contract (urgent
  closes it, wake pinned, auto-orient parked, bright glass).
- The receive drains (`espnow_peer.cpp`, `fleet_udp.cpp`) tap each frame to
  the demo un-deduped — trigger timing is the point — while the fleet model
  keeps its 60 s semantic dedupe for the product face.
