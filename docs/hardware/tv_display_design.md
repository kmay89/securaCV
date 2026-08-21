# Canary on the TV — a hub-free way to put securaCV on any television

> Status: **DRAFT / proposal.** Reference implementation shipped alongside
> this doc: [`firmware/projects/canary-display/tv/`](../../firmware/projects/canary-display/tv/)
> (the `/tv` page the Canary already serves). This doc covers the strategy,
> the competitive picture, and the "Canary TV" hardware proposal.

## The idea in one line

**The best screen for securaCV is the one already on the wall.** Most homes
own a television that's on the WiFi. If we can light it up with the fleet's
status — no hub, no account, no camera feed — we hand people a big, calm,
always-on security surface for roughly the cost of a cable they already have.

This splits cleanly into two products:

- **The universal software path** (ships today): a 10-foot dashboard the
  Canary serves at `/tv`. Point any smart-TV browser or a cheap cast stick
  at it. Works in *almost all* situations because it rides the on-device
  `/api/glass` snapshot — **no hub required.**
- **The "Canary TV" hardware path** (this proposal): a tiny HDMI dongle that
  *is* the "super easy, lasts for years, no issues" answer for the homes
  whose TV browser is junk or who just want plug-and-forget. This is our
  entry into the same category Seeed is chasing — except ours drives the big
  screen instead of being a little screen.

## Why this is even possible without a hub

securaCV already exposes the fleet four ways (see `custom_components/securacv/const.py`,
`docs/standard/AMBIENT_DISPLAY_STANDARD.md` §6). For a TV, one of them is the
unlock:

| Plane | Needs a hub? | Fit for a TV |
|-------|--------------|--------------|
| **On-device `/api/glass` JSON** (`glass_web.cpp`) | **No** | ★ The key. Each Canary Display already serves a whole-fleet snapshot over LAN — built for exactly this "live mirror" job. |
| MQTT `securacv/<id>/…` + `fleet/ack` | Yes (broker) | Great when a hub exists; a browser can't subscribe to raw MQTT without a bridge. |
| Kernel REST `/status` (token-gated) | Yes (kernel) | Deep data, but rotating-token auth is awkward for a wall display. |
| Home Assistant Lovelace | Yes (HA) | Fine if they already run HA; not the no-hub story. |

The `/api/glass` snapshot carries everything a glance needs — `worst`
severity, per-witness `{name, room, sev, age_s}`, `night`, clock, `wifi`/`hub`
flags, `acked`, and the active Character palette. Our TV page consumes that
verbatim, so it inherits the wall glass's honesty (the firmware already folds
staleness deadlines into `worst`) and never invents data.

**No-hub flow:** the page is served *by* the Canary, so it's same-origin with
`/api/glass` — no CORS, no config, no cloud. The TV asks `canary.local` for a
page; the page asks the same Canary for the snapshot; done.

## The universal software path (shipped in this branch)

`firmware/projects/canary-display/tv/index.html` — self-contained (no CDN, no
fonts, no internet), baked into `tv_html.h` and served at `GET /tv` next to
the existing phone mirror at `/`. It's a conformant Open Ambient Security
Display: hero worst-state readable in ≤1 s, a roll-call of every Canary with
word + glyph + color (never color alone), client-side silence→amber→red
degradation, an honest "signal lost — last known" banner, night dimming, a
sub-perceptual burn-in drift for 24/7 OLED safety, and zero ads/engagement.
See its [README](../../firmware/projects/canary-display/tv/README.md).

Three deployment tiers, cheapest first:

1. **Smart-TV browser** → open `http://<canary>.local/tv`. $0 hardware.
2. **Cast / HDMI stick** (~$30) → same URL in kiosk mode. Covers TVs whose
   built-in browser is unusable (most Fire TV / older sets — see market note
   below).
3. **"Canary TV" dongle** (our hardware) → ships the page, boots into it.

Tiers 1–2 are the "works for almost all situations" promise. Tier 3 is where
we make a product.

## The competition — what Seeed and friends are shipping

The category we'd be entering is **cheap always-on data displays**, and the
reference designs everyone copies are Seeed's reTerminal E line and TRMNL.

| Product | What it is | Price | Screen | Notable |
|---------|-----------|-------|--------|---------|
| **Seeed reTerminal E1001** | ESP32-S3 ePaper HMI, "SenseCraft" no-code dashboards, also speaks Home Assistant + TRMNL | ~$79 | 7.5" mono ePaper | ~3-month battery, self-contained little panel |
| **Seeed reTerminal E1002** | Same, full color | ~$109 | 7.3" 6-color ePaper | The "endgame productivity dashboard" reviews love |
| **Seeed reTerminal D1001** | ESP32-P4 LCD touch HMI, WiFi 6 | (higher tier) | 8" 1280×800 touch | Premium wall panel; camera + mics |
| **TRMNL** | "Read-only browser": device wakes, fetches a server-rendered PNG, sleeps. Open BYOD ($50) / BYOS self-host | ~$129 device | 7.5" ePaper | The *architecture* to steal — dumb device, server renders everything → cheap, durable, decade-long |
| **Signage sticks** (Yodeck/Juuno on Fire TV / Raspberry Pi) | Generic "put a web page on a TV" | $5/screen/mo + $30–70 stick | your TV | Proves the "drive the TV you own" market; none are security-aware |

Two takeaways:

1. **Every one of these is a small self-contained screen. None drives the
   television you already own.** That's our wedge: the reTerminal E is a
   lovely 7" panel, but a family reads a security status better from the 55"
   already in the room. "Use your TV" is a genuinely differentiated pitch and
   costs the customer almost nothing.
2. **TRMNL's "dumb renderer" philosophy is the durability answer.** The
   device holds no logic; the securaCV side already produces the snapshot.
   Keeping "Canary TV" a thin kiosk over `/tv` is exactly what lets it "last
   for years, no issues."

(There's also Seeed's *Interactive Signage / "Make a Sign" Contest 2026* — a
natural place to show a "Canary TV" build publicly if we want the marketing
beat. Its whole premise is interactive signs on Seeed hardware.)

## "Canary TV" — the hardware proposal

A palm-sized HDMI dongle that turns any TV into a securaCV surface. Design
tenets, in priority order:

1. **Dumb renderer, forever.** All product logic lives in `/tv` +
   `/api/glass`. The dongle is a kiosk browser pointed at a Canary. Firmware
   on the dongle changes ~never; the dashboard updates on the Canary. This is
   the TRMNL lesson and the reason it can "last for years."
2. **Zero-thought setup.** Plug HDMI + USB power. First boot shows a captive
   page (reusing the existing QR/captive-portal onboarding,
   `firmware/common/provision_qr/`): pick WiFi, and it auto-discovers Canaries
   over mDNS (`canary.local`) — the same discovery the fleet already uses. No
   app, no account.
3. **24/7 without regret.** Dark UI, night dimming from the `night` flag,
   burn-in drift, and honest "signal lost / last known" states mean it can
   stay on a bedroom or hallway TV indefinitely.
4. **Honest by construction.** It's a browser, so it renders trust as
   *reported* by the Canary — it never claims on-glass "verified" (AD-Core
   §2.5). For a fully AD-Verified TV we'd later add a small verifying agent;
   v1 is AD-Core + AD-Calm + AD-Resilient.

### Hardware options considered

| Option | Drives HDMI TV? | ~BOM | Pros | Cons |
|--------|-----------------|------|------|------|
| **Raspberry Pi Zero 2 W + mini-HDMI** | Yes | ~$18 + case | Real Chromium kiosk, trivial to build, huge community, mDNS/WiFi for free | Linux to maintain; boot ~20–30 s; needs an SD image we harden |
| **Rockchip Android TV stick** (RK3528-class) | Yes | ~$20–30 | Cheap, HDMI-native, hardware video | Android baggage; harder to lock to kiosk and to promise "no issues for years" |
| **ESP32-P4 + HDMI/DSI bridge** | Partial/complex | ~$15 | On-brand with the rest of the fleet (Seeed ESP32-P4), no OS | ESP32 can't run a real browser; we'd re-implement the renderer natively (loses the "dumb renderer" win) |
| **Just bless a $30 retail cast stick** | Yes | $0 to us | No hardware to build/support | No control over setup UX or longevity; not "our version" |

**Recommendation: Raspberry Pi Zero 2 W-class Linux dongle running a hardened
Chromium kiosk into `http://<discovered-canary>/tv`.** It's the shortest path
to "plug it in and it works for years," it reuses the web `/tv` we already
have (no second renderer to maintain), and it keeps the device dumb. The
ESP32-P4 route is tempting for brand tidiness but throws away the biggest
durability advantage — don't rebuild the renderer in C++ for a TV when the TV
can just run the page.

### Setup flow (target)

```
Plug HDMI + power ─▶ "Canary TV" splash + captive WiFi/QR
      │                       (reuses provision_qr onboarding)
      ▼
Join home WiFi ─▶ mDNS finds canary.local ─▶ fullscreen /tv
      │
      ▼
Runs 24/7. Canary pushes dashboard changes; dongle never needs touching.
```

## What it must never do

Per the Ambient Display Standard and securaCV's whole premise:

- **No camera feeds.** There are none — securaCV emits coarse, signed
  semantic events, not video. A "security TV" that people expect to show
  camera footage would misrepresent the product. The TV shows *status and
  witness roll-call*, and says so.
- **No "verified" it didn't check, no ads, no engagement mechanics, no
  silence-reads-as-safe.** All enforced in the shipped `/tv` page.

## Security posture on the LAN (be honest about the edge)

The no-hub story is built on serving the fleet snapshot openly on the home
network, and that convenience is a deliberate trust trade-off worth stating
plainly rather than hiding:

- **The reads are unauthenticated by design.** `GET /api/glass`, `GET /api/fleet`
  and the `/` and `/tv` pages are served without a credential — that is the LAN
  glance the product promises. `/api/glass` carries per-witness presence and
  wellbeing (`wb` = someone is home, `br` = breathing), so treat that as
  visible to anyone already on the WiFi. `/api/glass` sets no
  `Access-Control-Allow-Origin`, so the browser same-origin policy blocks a
  drive-by web page from *reading* it cross-origin; the residual reader is a
  host already on the LAN that knows the device address. That direct-LAN host
  is the documented trust boundary for the whole mirror surface — a read shows
  only what a glance at the wall display already shows anyone in the home.
- **The writes are gated.** The state-changing POSTs `/api/set` (brightness,
  Character, clock, orientation, nightlight) and `/api/tz` (timezone) are *not*
  open: each requires an `Origin`/`Host` allowlist match plus a per-boot CSRF
  token minted from the hardware RNG and handed only to the same-origin page in
  `/api/settings` (which carries no CORS header, so a cross-origin script cannot
  read it back). This closes the drive-by CSRF where a web page the owner
  visits blind-POSTs `http://<canary>.local/api/tz` to re-persist device
  settings. It does not — and is not meant to — stop a direct-LAN host that
  reads the page and its token itself; that is the same boundary the reads sit
  on. See the write-guard note in `glass_web.cpp`.
- **The display↔broker MQTT link is plaintext by default.** `mqtt_mgr.cpp`
  connects over TCP 1883 with a plain `WiFiClient` (no TLS), so a LAN sniffer
  can see the MQTT username/password and every retained status/health/chain and
  presence payload for the fleet. This is the common local-broker trade-off and
  it is a deliberate default, not an oversight: the intended deployment is a
  broker on the same trusted home LAN. Two honest caveats follow from it — (1)
  at-rest protection of the NVS-stored broker credentials relies on the
  flash-encrypted secure build (`provisioning/sdkconfig.defaults.secure`), not
  the default unencrypted NVS; and (2) an opt-in `WiFiClientSecure` path with a
  pinned broker CA is the hardening option for households that want the link
  encrypted end-to-end. Until that ships, do not describe the display's MQTT
  telemetry as confidential on the wire.

## Open decisions (for review)

1. **Bless the Pi-class dongle as "Canary TV," or ship only the software
   path for now?** The `/tv` page already delivers most of the value at $0
   hardware; the dongle is a productization + support commitment.
2. **Multi-hub / multi-Canary picker** on the TV when several Canaries serve
   `/api/glass` — v1 reads one; a mDNS chooser is a small follow-up.
3. **AD-Verified TV** (on-device signature checking) — worth a v2 verifying
   sidecar, or leave verification to the wall glass and phone?
4. **Enter the "Canary TV" build in Seeed's Interactive Signage Contest 2026**
   for a marketing beat?

---

### Sources (competitive research, July 2026)

- Seeed reTerminal E1001 — <https://www.seeedstudio.com/reTerminal-E1001-p-6534.html>
- reTerminal E1001/E1002 overview — <https://www.cnx-software.com/2025/09/06/reterminal-e1001-e1002-esp32-s3-monochrome-color-epaper-displays/>
- reTerminal D1001 (ESP32-P4 HMI) — <https://www.seeedstudio.com/reTerminal-D1001-p-6729.html>
- TRMNL (dumb-renderer architecture, BYOD/BYOS) — <https://trmnl.com/> · <https://docs.trmnl.com/go/diy/introduction>
- Seeed Interactive Signage / "Make a Sign" Contest 2026 — <https://www.seeedstudio.com/blog/2026/05/13/make-a-sign-contest-2026-win-2000-now/>
- Driving a web dashboard on a TV 24/7 (sticks vs Pi) — <https://www.geckoboard.com/resources/tv-dashboards/>
