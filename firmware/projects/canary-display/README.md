# Canary Display

**"A Canary that shows instead of senses."** Fleet status display firmware —
the answer to *"I shouldn't need my phone to know the house is quiet."*

Two hardware flavors of one app (selection rationale:
[`docs/hardware/display_research.md`](../../../docs/hardware/display_research.md)):

| Flavor | Hardware | Where it lives | Env |
|--------|----------|----------------|-----|
| **watch** | XIAO ESP32-S3 + Seeed Round Display (1.28" 240×240 GC9A01, CST816S touch) | bedside table, desk | `canary-display-watch` |
| **dash** | Waveshare ESP32-S3-Touch-LCD-4.3 (800×480 IPS, GT911 5-pt touch) | by the front door, kitchen wall | `canary-display-dash` |

Companion docs: [BOM](../../../docs/hardware/display_bom.md) ·
[UX design goals](../../../docs/hardware/display_ux_design.md) ·
enclosures `canary_watch_station.scad` / `canary_dash_display.scad`.

> ⚠️ **DEV STATUS (v0.1):** compile/CI-verified; **not yet validated on
> bench hardware** — same status as the matching enclosures. Pin maps carry
> VERIFY notes where vendor documentation is thin (CH422G bits, RGB
> timings, round-display backlight line).

## What it does

- **Subscribes** to the fleet: `securacv/+/{status,availability,health,events,tamper,chain,state}`.
  Every Canary on the household broker appears automatically — retained
  topics repopulate the whole view in one round-trip. No pairing, ever.
- **Flock discovery** (`FEATURE_MDNS_DISCOVERY`, see
  [`display_discovery_and_resilience.md`](../../../docs/hardware/display_discovery_and_resilience.md)):
  advertises `_securacv._tcp` on the LAN and — once connected — gossips the
  broker address in its TXT records. A display flashed with **no broker
  configured asks the flock and adopts the referral** (persisted to NVS);
  a broker that goes dark for 2 min (moved DHCP lease) triggers re-ask +
  rebind. Fallback: any `_mqtt._tcp` advert. One hand-provisioned device
  makes every later one plug-and-play.
- **Fleet model**: per-witness liveness (online → stale 3 min → lost 10 min),
  last event with severity decay, tamper, battery — pure C++, host-testable.
- **Verifies on its own silicon**: TOFU-pins each witness pubkey from its
  retained health payload (persisted in NVS) and checks signed chain heads
  with Ed25519 — the "verified ✓" on the glass means the same thing it
  means in Home Assistant, and is never shown otherwise.
- **Renders ("Quiet Glass", LVGL v8)** — anti-aliased Montserrat type and
  smooth arcs, dirty-region repaints (flicker-free dash by construction),
  humanized event copy ("Person in restricted zone", never raw wire text),
  and a rationed motion budget: 220 ms page fades, a 2 s breathing glow
  only while an Alert/Tamper is unacked, and a hold-to-ack ring that
  sweeps closed as you long-press:
  - *watch* — witness halo (one arc per Canary), hero worst-state center,
    tap to page through per-device detail + recent events, long-press to
    acknowledge.
  - *dash* — header state sentence with severity glow, witness card
    gallery, event timeline column.
- **Night mode**: quiet hours render red-shifted and near-dark (watch dims
  via PWM; dash goes dark-theme + backlight-off — its expander backlight is
  on/off only). An **unacked Alert/Tamper overrides the night floor**.
- **Fails loudly, never silently**: silent witnesses go amber/red on
  deadlines; a dead WiFi or broker link is bannered on the glass
  ("showing last known state"). Link loss is a first-class alarm — the
  baby-monitor lesson.
- **Speaks minimally**: its own MQTT surface is a retained status heartbeat
  (LWT `offline`), a retained health row, and the shared signed pull-OTA
  update entity. It publishes no events and pins no keys of its own.

- **Trailblazer wave 1** ([spec](../../../docs/hardware/display_trailblazer_spec.md)):
  **Proof-on-Glass** — tap a witness card (dash) or reach the Proof page
  (watch) for a QR of the signed chain head + pinned pubkey, independently
  verifiable by any phone, no cloud; **household ack-sync** — acknowledge
  on one display, every display agrees (`securacv/fleet/ack`, retained,
  epoch-anchored); **illumination ladder + presence-wake** — Active /
  Ambient / Night, and a hallway presence event lights the watch before
  you reach it (G5 intact: only unacked Alert/Tamper breaks the Night
  floor); **the heartbeat** — a once-a-minute swell that fires only when
  everything is reachable *and* verified; **chime engine** — the full
  severity-tiered sound grammar, compiled and ready, `FEATURE_CHIME=0`
  until the piezo pad (`BUZZER_PIN`) is populated at bench.

- **Trailblazer wave 2** ([spec](../../../docs/hardware/display_trailblazer_spec.md)):
  **Chirp fallback** (`FEATURE_CHIRP_SCAN`) — when the broker link drops,
  a passive NimBLE scan picks up the Canaries' 17-byte BLE chirps, so
  liveness and tamper still reach the glass with the router unplugged
  (events honestly labeled "(chirp)", unknown chirpers surface as
  `SCV-XXXX`); **names & rooms** — retained `securacv/<id>/meta`
  `{"name","room"}` renders friendly names everywhere, `Name · room` on
  detail lines, ids as fallback; **wellbeing line** — witnesses that
  publish `breathing_locked` get "breathing ✓/—" on their detail line
  (radar-only aging-in-place reassurance, consent by construction);
  **time machine v1** — a rolling 24 h in-RAM histogram feeds the dash
  day line ("Past 24h · 14 events · worst: warn"), rendered only when
  time is SNTP-valid; and the **open standard draft** now lives at
  [`docs/standard/AMBIENT_DISPLAY_STANDARD.md`](../../../docs/standard/AMBIENT_DISPLAY_STANDARD.md).

## Build

```bash
cd firmware/projects/canary-display
cp secrets/secrets.example.h secrets/secrets.h   # then edit
pio run -e canary-display-watch     # or canary-display-dash
pio run -e canary-display-watch -t upload
pio device monitor -b 115200
```

Set your timezone for quiet hours by adding e.g.
`#define CD_TZ "EST5EDT,M3.2.0,M11.1.0"` to `secrets/secrets.h`.

## Layout

```
include/canary/
  config.h            flavor composition (CD_* -> constants)
  topics.h            own topics + fleet subscription wildcards
  fleet/fleet_model.h fleet state machine (pure, host-testable)
  trust.h             TOFU pin store + Ed25519 chain verify
  net/                wifi_mgr / mqtt_mgr / ota_mgr (canary-vision parity)
  hal/display.h       panel+touch HAL (UI never sees panel specifics)
  ui/                 theme (timeline-card palette) + glance/dash faces
src/                  implementations; hal+ui TUs are flavor-gated
```

## Roadmap (post-v0.2)

- ~~LVGL migration~~ — shipped ("Quiet Glass", see the UX doc's Design
  language section).
- Passive **BLE Chirp scan** fallback: render heartbeat/tamper chirps when
  the broker is unreachable (`docs/ble_protocol.md` §5).
- Piezo chime (severity-tiered, falling "all-clear" tone) — watch enclosure
  has room; see the UX doc's sound spec.
- NVS/HA-configurable quiet hours + per-class alert gating; settings page.
- PCF8563 RTC as clock fallback across router outages (watch).
- Event cache on the watch's microSD for a scrollable history.
