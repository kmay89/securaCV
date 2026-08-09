# Canary Display

**"A Canary that shows instead of senses."** Fleet status display firmware —
the answer to *"I shouldn't need my phone to know the house is quiet."*

Two hardware flavors of one app (selection rationale:
[`docs/hardware/display_research.md`](../../../docs/hardware/display_research.md)):

| Flavor | Hardware | Where it lives | Env |
|--------|----------|----------------|-----|
| **watch** | XIAO ESP32-S3 + Seeed Round Display (1.28" 240×240 GC9A01, CST816S touch) | bedside table, desk | `canary-display-watch` |
| **dash** | Waveshare ESP32-S3-Touch-LCD-4.3 (800×480 IPS, GT911 5-pt touch) | by the front door, kitchen wall | `canary-display-dash` |
| **playground** | Waveshare ESP32-S3-Touch-LCD-4.3**B** (dash hardware + isolated DI/DO, RS485, CAN, I2C terminals) | the workbench — a safe guided peripheral test mode, no network ([doc](../../../docs/hardware/dev_playground_43b.md)) | `canary-display-playground` |

Companion docs: [BOM](../../../docs/hardware/display_bom.md) ·
[UX design goals](../../../docs/hardware/display_ux_design.md) ·
enclosures `canary_watch_station.scad` / `canary_dash_display.scad`.

The playground/dev-mode pair is the first citizen of a five-gear **mode
system** (fleet / bench / demo / debug / arcade —
[`display_modes.md`](../../../docs/hardware/display_modes.md)), now **built
and compile-gated**: pure cores host-tested (`include/canary/mode/`,
`tests_host/test_{mode_registry,demo_script,arcade_logic}.cpp`), gear
runtimes in `src/mode/`, the Settings → modes doorway, and dedicated CI envs
(`canary-display-dash-modes` / `-watch-modes`). Every flag defaults off —
default builds are byte-identical — and each gear is bench-pending per the
spec's Waves ledger. What plugs into the 4.3B's terminals — and why — is
cataloged in
[`display_peripheral_catalog.md`](../../../docs/hardware/display_peripheral_catalog.md).

> ⚠️ **DEV STATUS (v0.1):** compile/CI-verified; **not yet validated on
> bench hardware** — same status as the matching enclosures. Pin maps carry
> VERIFY notes where vendor documentation is thin (CH422G bits, RGB
> timings, round-display backlight line). The
> [bench bring-up runbook](../../../docs/hardware/display_bench_bringup.md)
> is the step-by-step that clears every VERIFY note and retires this status.

## What it does

- **Subscribes** to the fleet: `securacv/+/{status,availability,health,events,tamper,chain,state}`.
  Every Canary on the household broker appears automatically — retained
  topics repopulate the whole view in one round-trip. No pairing, ever.
- **Fleet discovery** (`FEATURE_MDNS_DISCOVERY`, see
  [`display_discovery_and_resilience.md`](../../../docs/hardware/display_discovery_and_resilience.md)):
  advertises `_securacv._tcp` on the LAN and — once connected — gossips the
  broker address in its TXT records. A display flashed with **no broker
  configured asks the fleet and adopts the referral** (persisted to NVS);
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

- **First-boot onboarding** (`FEATURE_ONBOARDING`,
  [choreography](../../../docs/hardware/display_onboarding.md)): a fresh
  display never reboot-loops — it says hello, shows a **join QR on its own
  glass** (device-unique AP, per-session password), pops a captive portal on
  the phone (dark, self-contained, live status, specific failure reasons),
  persists credentials **on success only**, and cross-fades straight into the
  fleet UI — where the fleet referral lands the broker with zero further
  input. Plug in → scan → password → watching your canaries. No dead ends.

- **Trailblazer wave 3** ([spec](../../../docs/hardware/display_trailblazer_spec.md)):
  **the verifiable time machine** (`FEATURE_TIME_MACHINE`) — every event is
  recorded epoch-stamped with the *verbatim signed chain head that was live
  when it fired*, so the Proof-on-Glass QR works on a week-old event exactly
  as on a live one. The dash gains a history modal (tap "Past 24h" → a
  wall-clock log; tap a row → its chain as a QR; a two-tap "erase all" for
  sovereignty); the watch gains a read-only HISTORY page. Cross-reboot
  durability is `FEATURE_TIME_MACHINE_PERSIST` (LittleFS, failure-tolerant,
  bench-gated like the chime), and the dash's TF slot adds the **SD deep
  archive** (`FEATURE_SD_STORAGE`, `fleet/sd_archive.cpp`): every record also
  appended to the card as plain JSONL — same schema as the flash slice, so
  the card reads on any laptop with a text editor, and popping it out IS the
  export. SDMMC 1-bit (the CS-less path this hardware's expander-routed DAT3
  demands), failure-tolerant like every other tier (no card = nothing
  changes; hot insert archives from the next event), dash-only for now (the
  watch slot shares the panel's SPI bus — `fleet/sd_archive.h` has the full
  story), and bench-gated before the default flips. And on the *sensor* side
  `canary-wap` now
  **gossips the broker** (`FEATURE_MDNS_BROKER_GOSSIP`): configure one canary
  and every display self-discovers a provably-reachable broker with zero
  setup ([discovery doc](../../../docs/hardware/display_discovery_and_resilience.md) §5.1).

- **The care wave** (`FEATURE_CARE` + `FEATURE_RHYTHM`,
  [design + research trace](../../../docs/hardware/display_care_wave.md)):
  the **clock is the idle face** (all-quiet watch hero = the time — every
  glance at the clock absorbs the security state); the **attention policy**
  ends 2 a.m. maintenance chirps forever — Warn-class sounds are suppressed
  during quiet hours into an overnight ledger and surface as a **morning
  summary** ("While you slept: 2 notices"), while unacked Alert/Tamper
  remains the one sound that breaks the night, **ramping soft → full** across
  re-voicings; **per-witness mute** (long-press a witness) is the honest
  bypass — visible always, tamper punches through, persisted until morning;
  **Roll Call** is a live walk test (last word · battery · RSSI, rows light
  up as each canary answers); the **rhythm line** learns the home's first
  stir on-device and says "Quiet past the usual wake" when it matters (the
  post-Alexa-Together slot nobody fills); **escalation-on-no-ack** publishes
  one `securacv/fleet/escalation` event after 15 unacknowledged minutes so
  automations can widen the circle; and the **transparency page** mirrors on
  the glass exactly what this display consumes, speaks, stores — and never
  does. Plus: ack attribution ("acked · kitchen-dash"), room comfort lines,
  an emergency contact on the dash during unacked alarms, glance-first wake,
  and a cleaning-mode touch lockout.

## Build

This is the canonical **PlatformIO** tree. An Arduino-IDE-buildable **parity
sketch** is generated from it (single source of truth) — see
[`arduino/canary_display/`](./arduino/canary_display/) and
[`arduino/PARITY.md`](./arduino/PARITY.md).

### PlatformIO (canonical)

```bash
cd firmware/projects/canary-display
cp secrets/secrets.example.h secrets/secrets.h   # then edit
pio run -e canary-display-watch     # or canary-display-dash
pio run -e canary-display-watch -t upload
pio device monitor -b 115200
```

Set your timezone for quiet hours by adding e.g.
`#define CD_TZ "EST5EDT,M3.2.0,M11.1.0"` to `secrets/secrets.h`.

### Arduino IDE (generated parity sketch)

```bash
# A GitHub zip compiles with ZERO setup (watch flavor by default; missing
# credentials hand off to the on-glass onboarding wizard) — just open
# arduino/canary_display/canary_display.ino and pick a profile. From a git
# checkout, the setup script selects the flavor + stages a secrets template:
cd firmware/projects/canary-display
./setup.sh arduino watch     # or: ./setup.sh arduino dash
arduino-cli compile --profile watch-core3
```

Builds on **both** arduino-esp32 core lines: pick the `watch-core3`/`dash-core3`
profiles on core 3.x (the Boards Manager default; GFX 1.6.6 + NimBLE 2.5.0) or
`watch`/`dash` on core 2.0.17 (PlatformIO-matched; GFX 1.4.9 + NimBLE 1.4.3) —
see `arduino/canary_display/README.md` for the matrix. Edit firmware in `src/`,
then `./setup.sh regen` before committing; a CI guard enforces the sketch stays
in sync.

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

## Text on the glass: the font has a fixed alphabet

LVGL's built-in Montserrat is generated over one range — `0x20-0x7F`, `0xB0`
(°), `0x2022` (•), plus the FontAwesome glyphs the `LV_SYMBOL_*` macros expand
to. There is no fallback font. A codepoint outside that set draws a **hollow
box**, with no build error and nothing in the log.

So the obvious separator is a trap: U+00B7 MIDDLE DOT is not in the range, and
it shipped — every date line read `Sunday [] Aug 9` on real hardware. It sits
one codepoint from the degree sign that *is* in the range, and the WASM
emulator hides it entirely, because a browser has real fonts.

Reach for these instead:

| Want | Use | Not |
|---|---|---|
| separator dot | `\xE2\x80\xA2` (•, U+2022) | `\xC2\xB7` (·) |
| a dash | ASCII `-` | em/en dash |
| ellipsis | `...` | `…` |
| a check | `LV_SYMBOL_OK`, or `\xEF\x80\x8C` where including `lvgl.h` would be wrong layering | `✓` (U+2713) |

`firmware/scripts/check_display_glyphs.py` enforces this over `src/` and
`include/canary/`, and runs in `firmware.yml`. Text bound for the serial log
(`log_line`, `boot_kv`, `say_evt`), for the compiler (`#error`), or for a
browser (`*_html.h`, raw string literals) is exempt — those are read where
real fonts exist.

## Editing these sources moves a file you didn't open

`canary-local/emulator/dist/*.js` is generated **and committed**, and it is
compiled from these sources — `src/main.cpp`, the LVGL faces, `care/`,
`fleet/`, `trust`. Not `net/`. So an ordinary C++ edit here can leave `dist/`
stale and fail `canary-local.yml`'s "firmware → wasm → boots in a browser" on
a file that isn't in your diff.

Rebuilding it needs emsdk **6.0.3** exactly. Don't fight that locally — use
**Actions → "Rebuild emulator dist (pinned emsdk)"**, dispatched on your
branch. Three things it will not tell you:

- It pushes to the branch it was dispatched on. Prefer a feature branch.
- **Don't push while it runs.** It checks out, installs emsdk, compiles, then
  pushes. Any push of yours inside that window used to make its push a
  non-fast-forward and throw the whole build away with `fetch first` — which
  reads as "the emulator failed to compile" when it compiled fine. It rebases
  and retries now, but the window is still yours to avoid.
- Its push does not retrigger CI *usefully*, and cleaning up after it has two
  distinct failure modes that look identical from the outside (PR status
  `pending`, zero check runs on the head commit):
  - The push you make afterwards must **touch a watched path** — this
    directory qualifies, a repo-root doc does not. A docs-only commit runs
    nothing at all.
  - The bot's own push creates a full set of runs that GitHub parks at
    `action_required`, waiting for a human to press Approve in the checks
    panel. No commit of yours clears that; ask a maintainer.

  Look at whether runs exist on the branch before pushing again: none at all
  means paths, parked ones mean approval.

## Roadmap (post-v0.2)

- **Mode system waves 1–4** ([spec](../../../docs/hardware/display_modes.md)):
  runtime glue for the mode registry (NVS `mode` token, legacy `devmode`
  migration), then demo / debug / arcade gears — each feature-gated,
  default off, with a dedicated CI env.

- ~~LVGL migration~~ — shipped ("Quiet Glass", see the UX doc's Design
  language section).
- Passive **BLE Chirp scan** fallback: render heartbeat/tamper chirps when
  the broker is unreachable (`docs/ble_protocol.md` §5).
- Piezo chime (severity-tiered, falling "all-clear" tone) — watch enclosure
  has room; see the UX doc's sound spec.
- NVS/HA-configurable quiet hours + per-class alert gating; settings page.
- PCF8563 RTC as clock fallback across router outages (watch).
- Event cache on the watch's microSD for a scrollable history.
