# Display research — ESP32-S3-family screens for a SecuraCV monitoring station

> Status: **selection made** (Option A glance puck + Option B dashboard —
> both now have firmware). First firmware shipped as
> [`firmware/projects/canary-display`](../../firmware/projects/canary-display/)
> (flavors `watch`/`dash`); design goals + market landscape in
> [`display_ux_design.md`](./display_ux_design.md); parts in
> [`bom_canary_display.csv`](./bom_canary_display.csv). The enclosures
> (`enclosure/canary_watch_station.scad`, `enclosure/canary_dash_display.scad`)
> remain **IN DEVELOPMENT** (not print-validated).
>
> **Bench hardware**: ordered 2026-07-06; the Seeed Round Display for
> XIAO (watch glass; host XIAO ESP32-S3 required separately) **ARRIVED
> 2026-08-17** — Track W in
> [`display_bench_bringup.md`](./display_bench_bringup.md) is now
> runnable. Also ordered: Waveshare ESP32-S3-Touch-LCD-4.3**B**
> with-case SKU (dash; the B variant is pin-identical to the 4.3,
> mic-free, and its bundled case covers the bench build without the
> printed enclosure). With hardware in hand: flash
> `canary-display-watch` / `canary-display-dash`, run the bench tracks,
> clear the VERIFY notes in both pin maps, and update the DEV-STATUS
> flags here and in the firmware README — the tiers stay
> "compile-tested" until the tracks actually pass.

## Goal

A small "glance station" in the SenseCAP-Watcher style: a desk/shelf unit that
shows fleet status at a glance (witness online/offline ring, last-event age,
verified-chain ✓/✗, alert color), driven over MQTT from the same broker the
Canaries publish to. Not a video monitor — SecuraCV stores no raw video; the
station renders *semantic* status, which tiny displays do well.

## Candidates surveyed

| Option | Board + display | Res / size | Why / why not |
|--------|-----------------|------------|----------------|
| **A — SELECTED** | **XIAO ESP32-S3 + Seeed Round Display for XIAO** ([product](https://www.seeedstudio.com/1-28-Round-Touch-Display-for-Seeed-Studio-XIAO-ESP32.html), [wiki](https://wiki.seeedstudio.com/get_start_round_display/)) | 1.28" round, 240×240 GC9A01, capacitive touch (CST816S), **39 mm disc**; onboard RTC (PCF8563), LiPo charger, microSD, JST 1.25 | **Stays entirely in the XIAO family** — same board defs, flashing flow, supply-chain and size-guard story as every Canary (`firmware/boards/`); the XIAO plugs directly into the display's socket (zero wiring, like the Sense/Vision stacks); Watcher-style puck aesthetic; battery-capable. 240×240 is plenty for glance UI (LVGL arc gauges, status ring). |
| B — dashboard alternative | **Waveshare ESP32-S3-Touch-LCD-4.3** ([product](https://www.waveshare.com/esp32-s3-touch-lcd-4.3.htm), [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3)) | 4.3" IPS 800×480, 5-pt capacitive touch, LVGL-ready; ESP32-S3 with 8 MB PSRAM / 16 MB flash, TF slot, CAN/RS485 | The step-up when you want an actual **timeline dashboard** (event list, multiple witnesses) rather than a glance puck. Same LX7 S3 family; 5"/7" siblings exist for wall mounting. New board def outside the XIAO line — more integration work. Printable case: [`enclosure/canary_dash_display.scad`](./enclosure/canary_dash_display.scad) (dev). |
| C — style reference | Seeed **SenseCAP Watcher** (ESP32-S3 + Himax coprocessor, 1.46" round touch + rotary dial) | 412×412 round | The form-factor inspiration — but it's a closed retail product with its own firmware stack; we replicate the *shape*, not the platform. |
| D | LilyGO T-Display S3 / AMOLED (1.9"–2.4" bar) | 170×320 ish | Cheap and cute, but bar-shaped UI suits tickers more than status rings; non-XIAO board def. |

## Selection rationale (Option A)

1. **Ecosystem coherence** — every SecuraCV device is a XIAO; the station
   becomes "a Canary that shows instead of senses." Firmware reuses the
   existing S3 board def, MQTT client, and signed-status vocabulary.
2. **Zero-wiring hardware** — the XIAO seats in the display's back socket,
   exactly like the Vision/Sense stacks; the enclosure problem reduces to the
   proven stack-cradle pattern.
3. **Right-sized UI** — a 240×240 round LVGL face fits the glance job:
   center = fleet state (all-verified ✓ / attention), ring segments = one arc
   per witness (green presence, amber stale, red tamper/offline), touch = page
   through last events; long-press = acknowledge.
4. **microSD + RTC on the display board** cover event-cache and clock without
   extra parts; JST 1.25 + charger enable a battery-backed station.

**Firmware (SHIPPED, v0.1):** one SPECIALIZED project covers both options —
[`firmware/projects/canary-display`](../../firmware/projects/canary-display/)
with flavor `watch` (Option A: GC9A01/CST816S glance ring) and flavor `dash`
(Option B: 800×480 card grid + timeline). It subscribes to the fleet topics,
TOFU-pins witness pubkeys and verifies signed chain heads with on-device
Ed25519 (same canonical as HA's verifier), and renders with the
timeline-card color vocabulary. v0.1 uses a purpose-built glance renderer
(Arduino_GFX primitives) rather than full LVGL — see the project README's
roadmap.

## Enclosure

`enclosure/canary_watch_station.scad` (**IN DEVELOPMENT**): a Watcher-style
desk puck — straight drum holding the display disc + XIAO stack behind a
screwed bezel, sitting in a separate 25° tilted stand with a cable channel;
all parts print flat, and the drum's keyhole pockets let it wall-mount
without the stand.
