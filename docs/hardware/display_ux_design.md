# Canary Display — UX design goals, market landscape, and BOM experience

> Status: **v0.1** — companion to the shipped `firmware/projects/canary-display`
> firmware, [`bom_canary_display.csv`](./bom_canary_display.csv), and the
> `canary_watch_station.scad` / `canary_dash_display.scad` enclosures.
> Hardware selection rationale lives in [`display_research.md`](./display_research.md).

## 1. The job

**A user should receive timely, relevant information from their Canaries
without consulting a phone.** A puck on the nightstand, a panel by the front
door or in the kitchen. Glance, know, go back to sleep / walk out the door.

Two devices, one firmware (`canary-display`, flavors `watch`/`dash`):

| | **Watch Station** (`watch`) | **Dash** (`dash`) |
|---|---|---|
| Glass | 1.28" round 240×240, touch | 4.3" 800×480 IPS, 5-pt touch |
| Home | bedside, desk | front door, kitchen wall |
| Face | witness ring + center state | card grid + event timeline |
| Night | PWM dim to near-dark, red-shifted | dark theme + backlight off, tap-wake |
| BOM (qty 1) | ~$34 required (+enclosure/battery opts) | ~$43 required |

## 2. What the market taught us

Full agent research trails: competitor panels/keypads, ambient-display DIY,
baby monitors, complaints + regulatory (2026-07). Condensed:

### The gap is real

- **No mainstream product does persistent, glanceable security status.**
  Echo Show and Nest Hub are on-demand viewers with server-enforced timeouts
  (Ring live view dies at ~10 min — even via ring-mqtt; Nest ~30 min) and
  cloud alert paths users measure at 15 s–3 min. Ring literally sells Echo
  Shows as its "display." UniFi's Viewport is the only persistent option and
  it's a $199 video-wall dongle for a UniFi-only NVR estate — video, not status.
- **Keypads/panels show one LED color.** Ring Keypad, Abode logo-LED, ADT
  Self-Setup ring: armed-state only, memorize-the-color, no *what* or *why*.
  SimpliSafe's keypad screen **sleeps to save power, so armed state is
  invisible at a glance** — a top user request for years. The one rich US
  panel (ADT Command 7") is locked behind ~$46–62/mo monitoring contracts.
- **Bedrooms are hostile territory.** SimpliSafe's base station "lights up
  entire rooms" with no dimmer (4+ years of forum requests); Echo Show glows
  and now serves **full-screen ads** in ambient mode; ADT's secondary
  touchscreen has a KB article about the display that won't turn off. Only
  Nest Hub 2 (camera-free, Ambient EQ near-black clock) got this right — and
  Google's display line is in limbo.
- **Phone-as-alerter structurally fails households.** DND swallows alerts
  (every vendor ships a workaround doc); Android battery optimization delays
  Wyze notifications 15–30 min; secondary users (kids, elders, partners
  without the app) get nothing. Vendors' own products concede the point:
  eufy's Smart Display E10 ($200, 2025) is marketed exactly as "see cameras
  without fishing out your smartphone."
- **Alert fatigue is the failure mode.** ~70% of default camera alerts are
  noise (Roku's own number for non-person motion); hospital alarm literature
  (72–99% false alarms → desensitization → deaths) is the cautionary
  ceiling. Users don't tune alerts; they disable them.

### Baby monitors — the mature "screen at the bedside" craft

The dedicated parent-unit segment (Infant Optics, eufy SpaceView, VTech,
HelloBaby; $60–200, perennial best-sellers against $300 WiFi/cloud rivals)
survives on exactly our pitch: **local link, <0.2 s latency (vs 10–20 s
cloud), no subscription, works when the internet doesn't.** Their patterns:

1. **Link loss is a first-class alarm.** Out-of-range triggers beeps within
   ~5–20 s, repeating every 10–20 s until resolved (Philips Avent: 3 beeps/20 s;
   Angelcare: 2 beeps/10 s). *Silence is never rendered as safety.*
2. **Positive confirmation state** (Owlet base): a steady green glow means
   "monitoring and OK" — trust needs an affirmative signal, not darkness.
3. **Two-tone semantics** (Owlet): technical fault = yellow + soft chime;
   real event = red + loud. A dead battery must never sound like an intruder.
4. **VOX/ECO**: screen sleeps, wakes on event; sensitivity in 3 levels with
   *behavioral anchors* ("rolling" vs "yawning"), not percentages.
5. **Physical, eyes-closed ergonomics**: no menus on critical paths; nothing
   fragile (eufy's snapping kickstands); factory pairing or 3-second-button
   pairing.
6. Cloud cautionary tales to never repeat: Miku's firmware brick + features
   paywalled post-acquisition; Nanit's can't-activate-without-subscription
   onboarding; Owlet's FDA takedown. Owlet's relaunch lesson: core alerts
   must be subscription-free.

## 3. Design goals (testable)

| # | Goal | Test |
|---|------|------|
| G1 | **Glance ≤ 1 s** — worst-severity state readable across a dark room | center glyph + ring/header legible at 3 m; no reading required for "all quiet" vs "attention" |
| G2 | **Alert latency ≤ 2 s** on-LAN (event publish → pixels) | MQTT push, no polling; render tick ≤ 200 ms after model change |
| G3 | **Silence ≠ safety** — link loss is an alarm | witness silent 3 min → amber, 10 min → red *lost*; WiFi/broker down → banner within one render tick; last-known state clearly labeled |
| G4 | **Bedroom-safe** | night mode ≤ 10 lux at 30 cm; red-shifted palette (no 460–500 nm melatonin band); zero always-on white LEDs; dash goes fully dark |
| G5 | **Alerts can wake, chatter can't** | only unacked Alert/Tamper overrides the night floor; Notice/Warn wait for morning or a tap |
| G6 | **No phone, no account, no app, no cloud, no ads, no subscription** | everything renders from the local broker; works internet-down |
| G7 | **Honest trust surface** | "verified ✓" strictly = on-device Ed25519 verify against a TOFU-pinned key (HA-equivalent bar); signed-unverified and failed states visibly distinct |
| G8 | **Ack ≠ amnesia** (Nest pattern) | long-press quiets emphasis; residual chip persists until the condition clears; tamper can be quieted, never dismissed; ack expires (1 h) if the condition persists |
| G9 | **Fatigue budget** | informational events decay (Notice 10 min, Warn 30 min); severity tiers use distinct modalities; conservative default alert set, add classes after burn-in |
| G10 | **Accessible** (WCAG 1.4.1; ~8% of men are colorblind) | color never sole carrier: severity is also position (spine/ring/center), glyph, and label |
| G11 | **Family-wide by default** | zero per-user setup; the device on the wall serves everyone, including people without phones |
| G12 | **Fails diagnosable** | headless boot (display init failure) keeps MQTT + logs alive; every degraded state is printed and published |
| G13 | **The Nth device is magic** | zero pairing (broker wildcards + retained topics); an unconfigured device adopts the broker from the fleet's mDNS gossip and persists it; a moved broker re-binds within ~2 min — see [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md) |

## 4. UX specification (v0.1 firmware)

### Shared vocabulary

Severity ladder `ok < notice < warn < alert < tamper` (collapsed from the HA
component's) with the **timeline-card palette** so a state is the same color
on the glass and in the app: green `#43a047` ok/verified, amber `#fb8c00`
attention/stale, red `#e53935` alert/tamper/offline/failed, blue `#03a9f4`
signed-but-unverified, gray muted. Trust badges: `✓ verified` (earned only),
`S signed`, `– unsigned`, `X failed` (loud).

Liveness: `online → stale (3 min silent, amber) → lost (10 min, red, treated
as alert)`; retained-LWT `offline` renders as warn. Battery <25% notice,
<10% warn. Chain-verify failure and pubkey mismatch are alerts — the display
is the household's second, independent verifier.

### Watch Station (240×240 round)

- **Page 0 — overview** (auto-returns after 20 s): witness ring, one arc per
  Canary colored by its effective severity; center = fleet worst state
  (check-in-circle "ALL QUIET · n canaries · verified" or "! TAMPER ·
  which-device"); footer clock + `wifi down`/`broker down` banner.
- **Pages 1..n — per witness**: id, type, link state, last event + age,
  chain badge, battery, `k/n` position.
- **Last page — events**: five most recent, `age name device`, `*` = signed.
- **Input**: tap = wake (in the dark, first tap only wakes) / next page;
  long-press 900 ms = acknowledge. That's the whole grammar — operable
  half-asleep, one-handed (baby-monitor lesson #5).
- **Night** (quiet hours, default 22:00–07:00 local): PWM floor (duty
  10/255), red-shifted palette; tap = full brightness 15 s; unacked
  Alert/Tamper overrides the floor.

### Dash (800×480)

- **Header**: one sentence — "All quiet · 5 canaries · verified" or
  "TAMPER — check the grid" — plus clock; 4 px severity strip under it.
- **Grid**: up to 8 cards (2×4; "+N more" beyond), each with severity spine
  (position + color), id, link state, chain badge, last event + age,
  battery/fw, TAMPER flag.
- **Timeline column**: newest-first events with severity dots, ages, signed
  markers — the wall version of the HA timeline card.
- **Footer honesty line**: `status display — not a life-safety device`, or
  the link-loss banner when degraded (G3, §6).
- **Input**: tap = wake; long-press = acknowledge.
- **Night**: dark theme + **backlight off** (hardware truth: the CH422G
  expander line is on/off — no PWM), tap-to-wake 20 s, alert override turns
  it back on.

### Design language — "Quiet Glass" (v0.2, LVGL)

The faces are rendered by LVGL v8 (anti-aliased type and arcs, dirty-region
repaints — which is also what makes the dash flicker-free). Tokens live in
`include/canary/ui/theme.h`; every choice below is enforced there, not by
convention.

**Ground.** True black `#000000` (the bezel disappears; night floors go
lower), surfaces `#141414`, hairline edges `#262626`, text `#EDEDED` /
muted `#8A8A8A` / faint `#4A4A4A`. Semantic hues are unchanged
(timeline-card parity). Emphasis is glow, never hard stripes.

**Type.** Montserrat (anti-aliased, LVGL built-in) in six roles:
hero 48 · title 28 · clock 20 · body 16 · label 14 · caption 12.
Uppercase micro-labels get +1..2 letter-spacing. The wire never reaches the
glass raw: `humanize_event()` turns `presence_in_restricted_zone` into
"Person in restricted zone", with curated copy for the known vocabulary and
sentence-cased fallback for anything new.

**Motion budget (rationed, each with a job).** Since the motion-engine wave
([display_motion_engine.md](./display_motion_engine.md)) the ration is
enforced mechanically: every motion has a class (Semantic / Transition /
Micro / Ambient), durations scale to a capability tier derived from the
board's physics, and a frame-time governor parks decorative motion on a
glass that runs out of headroom. Semantic motion is never traded away.

| Motion | Duration | Class | Why it exists |
|---|---|---|---|
| Page fade | 220 ms ease-out | Transition | orientation between pages |
| Ground swap veil | ~160 ms in, ~320 ms out (snap on Lean) | Transition | day/night, Character, rotation and clock-style changes dip through the target ground instead of hard-cutting |
| Alert breathing (arc/card glow) | 2 s in-out, repeat | Semantic | the one thing allowed to move at rest — and only while an Alert/Tamper is unacked |
| Hold-to-ack ring | 900 ms linear sweep (= long-press) | Semantic | makes acknowledge deliberate; a quick tap flashes a sliver of the ring — a silent hint that holding does more |
| The heartbeat | 1.6 s swell, once per minute, day only | Semantic | fires **only** when every witness is quiet AND every chain verified (its absence is information); [trailblazer spec §4](./display_trailblazer_spec.md) |
| Digit morph / minute sweep | ~160 / ~320 ms ease-out (snap on Lean) | Micro | the minute lands instead of teleporting; yields to any unacked alarm |
| Calendar month entrance | staggered ~320 ms fades | Micro | a new month arrives in reading order; a day change holds still |
| Backlight glide | ~300 ms between ladder rungs (PWM glass) | Micro | Ambient↔Active stops being a light switch; an alarm still lands instantly |
| Weather scene (7" bedside day face) | standing, condition-paced | Ambient | the forecast the wire already carries, moving — day only, calm only, no modal, rich governor only; clear weather is *still* |
| Everything else | none | — | calm technology: a wall object at rest is still |

**Iconography.** LVGL's built-in symbol set only (✓/✕/wifi/battery) — no
custom pictograms until bench validation says the metaphors read at 3 m.

**Night.** Same layouts, tokens swapped to the red-shifted set — the design
never relies on hue that the night palette can't carry (WCAG 1.4.1 again).

### Latency budget (G2)

| Hop | Budget |
|---|---|
| Canary event → broker (QoS0/1 LAN) | ≤ 300 ms |
| Broker → display (subscribed, pushed) | ≤ 200 ms |
| Parse + fleet model + dirty flag | ≤ 10 ms |
| Render tick (dirty path) | ≤ 100 ms (watch) / 200 ms (dash) |
| **Total glass-to-glass** | **≪ 2 s** (vs 15 s–3 min for Ring's cloud path) |

## 5. Sound design (v0.2 — piezo reserved in BOM, unpopulated)

IEC 60601-1-8-informed, Owlet-split, fatigue-budgeted:

- **Tier 1 (tamper/alarm)**: fast 10-pulse burst, 500–3000 Hz, repeats until
  ack; overrides quiet hours (the only sound that may).
- **Tier 2 (warn — witness lost, chain failed)**: 3 slow pulses, softer,
  ≤ 2 repeats, silent during quiet hours.
- **Tier 3 (notice)**: single soft tick, day only, rate-limited, off by default.
- **All-clear**: falling two-tone when a condition resolves — resolution
  deserves a sound so silence keeps meaning "nothing new," not "did I miss it".
- Distinct timbres per tier (never one volume knob for chirps *and* sirens —
  the Ring Keypad complaint), per-tier user gating, mode changes silent.

## 6. Regulatory & marketing guardrails

- **Positioning: an *information display*, not an alarm.** It must not claim
  smoke/CO/fire alerting (UL 985/2034 territory), intrusion *detection*
  (UL 1023/639), or monitoring service. The dash footer and docs carry the
  industry-standard disclaimer set (Ring/Wyze/Nest pattern): informational
  only; not an emergency service; no guarantee alerts are timely, delivered,
  or complete; not a substitute for UL-listed life-safety alarms.
- **Claims discipline (FTC/NAD precedent — ADT 2014, SimpliSafe 2020):**
  capability claims ("see your fleet's status at a glance, locally"), never
  outcome claims ("keeps you safe") or unsubstantiated speed comparisons.
- **Radio compliance path**: both boards use pre-certified ESP32-S3 modules
  (FCC modular grants) → finished product needs Part 15 Subpart B SDoC +
  "Contains FCC ID" labeling, no new intentional-radiator cert, antenna
  untouched. EU: RED self-declaration (EN 300 328 / 301 489 / 62368-1)
  **plus the 2025+ RED-DA cybersecurity requirements (EN 18031)** — our
  no-cloud, signed-OTA, no-default-password posture is most of that work.

## 7. Why this beats a Ring keypad / Echo Show (the pitch)

| | Ring Keypad + Chime | Echo Show 5/8 | **Canary Watch/Dash** |
|---|---|---|---|
| Persistent status | 1 LED mode light | no (ads + rotating cards) | yes — that's the whole product |
| What/why shown | none | on-demand video | per-witness state, event, age, trust |
| Alert path | cloud push (10 s+) | cloud (15 s–3 min) | local MQTT push (<2 s) |
| Bedroom | motion-lit keypad | backlight glow, ads, DND traps | <10 lux red-shifted night mode |
| Internet down | siren only, no app | dead | fully functional |
| Verification | trust the cloud | trust the cloud | Ed25519 on the device itself |
| Subscription | $4.99–19.99/mo tiers | free+ads / Ring plans | none, ever |
| Family without phones | keypad only | voice | full status, zero setup |
| BOM/price | $49.99 + $34.99 | $89.99+ | ~$34/$43 parts, printed enclosure |

## 8. Roadmap

- **v0.1 (this change)**: firmware for both flavors (fleet model, TOFU +
  Ed25519 verify, glance/dash faces, night mode, ack, OTA, CI + size guard),
  BOM, enclosures (dev), this spec.
- **v0.2**: piezo chime (§5) + link-loss beep pattern (baby-monitor
  semantics); NVS/HA-configurable quiet hours + per-class alert gating;
  bench validation of both boards → clear the VERIFY notes in the pin maps;
  print-validate enclosures.
- **v0.3**: passive **BLE Chirp scan** fallback (render heartbeat/tamper
  chirps when the broker is down — no other consumer display has an
  off-network fallback); PCF8563 RTC timekeeping; microSD event history.
- **v0.4**: zero-typing provisioning parity (captive portal / BOOT-tap flow
  like the other Canaries — today the display flashes with compiled
  secrets); multi-display awareness (an acked alert on one display shows
  acked on all, via retained ack topics).
- **Later**: battery/e-ink sibling for true zero-light bedrooms; Matter
  bridge exploration; optional vibration (deaf-household gold standard).

## 9. Open questions

1. **Ack scope**: today ack is fleet-wide on the acking display. Should ack
   publish (retained `securacv/<id>/ack`) so all displays + HA agree? (v0.4)
2. **Watch battery UX**: with the optional 302030 cell the puck survives
   outlet moves, but WiFi+screen drains it in hours — do we want a
   low-power "clock + wake on alert" battery mode, or document it as
   UPS-only?
3. **Dash orientation**: portrait wall mount (480×800) for narrow hallways?
   The enclosure supports it; the UI currently assumes landscape.
4. **Quiet-hours source of truth**: per-display compiled default now; HA
   schedule entity vs on-device settings page — which first?
