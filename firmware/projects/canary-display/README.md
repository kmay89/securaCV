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
- **Renders**:
  - *watch* — witness ring (one arc per Canary), center worst-state glyph,
    tap to page through per-device detail + recent events, long-press to
    acknowledge.
  - *dash* — header state sentence, witness card grid, event timeline
    column.
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

## Roadmap (post-v0.1)

- Passive **BLE Chirp scan** fallback: render heartbeat/tamper chirps when
  the broker is unreachable (`docs/ble_protocol.md` §5).
- Piezo chime (severity-tiered, falling "all-clear" tone) — watch enclosure
  has room; see the UX doc's sound spec.
- NVS/HA-configurable quiet hours + per-class alert gating; settings page.
- PCF8563 RTC as clock fallback across router outages (watch).
- Event cache on the watch's microSD for a scrollable history.
