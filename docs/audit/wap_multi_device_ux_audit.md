# WAP Multi-Device UX Audit

**Scope:** `firmware/projects/canary-wap/arduino/canary_wap/` (the user-facing
WAP firmware bundle) and the supporting captive-portal / companion-PWA flow
served from the device's SoftAP.

**Question this audit answers:**

> A real household will have more than one Canary. How does a person set up
> several of them? How do they find each other? What does "see everything at
> once" look like? And does the wizard already lead the user there?

Short answer: the **wires are there, the UI is not**. We already advertise
mDNS, already cap the peer cache at 8, already have a standalone fleet
manager — but the setup wizard treats the device as if it were the only
Canary the user will ever own, never mentions adding another, and the fleet
manager isn't reachable from any URL the user can find. This document is the
audit of that gap plus the design we are moving toward.

---

## 1. UI surface inventory (WAP firmware)

Every HTML page the WAP firmware can serve, and how a user reaches it today.

| Route | Source file | Title | Reached from setup wizard? | Status |
|---|---|---|---|---|
| `GET /` | `csi_dashboard_html.h` (2,889 lines) | "Canary · Sensing" — orb / 24h ribbon / "Today" sheet | **Yes** — wizard step 5 "Finish" links here via `http://canary.local/` | Live, primary post-setup surface |
| `GET /admin` | `web_ui.h` (4,246 lines) | "SecuraCV Canary" — 6-tab admin (Status / Camera / Presence / Community / Records / Settings) | **No** — never linked from any page in the setup flow | Live, **orphaned from new-user onboarding** |
| `GET /companion` | `companion_pwa.h` (2,239 lines) | "SecuraCV Canary" — BLE companion **and** the 5-step setup wizard (token mode) | **Yes** — this *is* the wizard (entered via captive-portal QR `?token=…`) | Live, primary onboarding surface |
| `GET /companion-sw.js`, `GET /companion-manifest.webmanifest` | `companion_pwa.h` | Service worker + PWA manifest | Indirectly — caches the companion for offline | Live |
| `GET /tune` | `tune_ui.h` (361 lines) | "Canary · Tuning Lab" — NVS coefficient editor | **No** — hidden, intentionally undocumented | Live, **dev-only / power-user**, orphaned from onboarding |
| `GET /enroll` | `device_identity_api::handle_enroll_html` | Device enrollment HTML | **No** — admin path | Live, admin-only |
| `GET /hotspot-detect.html`, `/generate_204`, `/connecttest.txt` | `setup_page_html.h` (102 lines) | "Set up your Canary" — captive-portal QR | **Yes** — this is the entry point | Live |
| `firmware/fleet-manager.html` (1,445 lines) | (standalone file on disk) | "SecuraCV Canary Fleet Manager" — list, ping, drill into many devices, with Camera-Peek tab per device | **No, and never served by the device** — user has to `open firmware/fleet-manager.html` from their checkout | Built, **not deployed**, not discoverable |

### What this means

1. **`fleet-manager.html` is the most striking orphan.** It's a polished,
   ready-to-run multi-Canary dashboard with auto-refresh, status grid,
   per-device drill-down, and a Camera-Peek tab — and it sits in
   `firmware/` waiting for someone to discover it on a shell. The device
   itself never serves it, the README mentions it in a casual aside
   (line 206), and no wizard step links to it. **This is the page that
   answers most of the user's question** about "see everything all at
   once like a normal security camera peek".
2. **`/admin` and `/tune` are also unreachable** through the wizard. The
   wizard's final-step link drops the user at `/` (the orb dashboard),
   which has its own Settings sheet — `/admin` is essentially a parallel
   universe that the new user never sees and the experienced user has to
   memorize the path for.
3. **The wizard's final step is single-device terminal.** It says
   "Setup's done. … tap a link below to open your Canary," gives one
   `canary.local` link, then leaves. There is no
   "set up another room" branch, no peer announcement, no fleet
   awareness.

---

## 2. Multi-Canary infrastructure — what's already built, what's missing

### Build A: `firmware/canary/` (PlatformIO / modular)

This build has real peer discovery wired up:

| Capability | Source | State |
|---|---|---|
| Per-device mDNS hostname (`canary-a3f7.local`) derived from `device_id` | `securacv_network.cpp:148-194`, `sanitize_mdns_hostname()` | Done |
| Advertises `_securacv._tcp` with TXT records (`device_id`, `name`, `fw`, `model`) | `securacv_network.cpp:185-193` | Done |
| Peer cache, **`PEER_CACHE_MAX = 8`** | `securacv_network.h:61` | Done — and yes, "let's say 8" is already the limit |
| Active mDNS browse on a 30 s cadence | `NetworkManager::browsePeers()` `securacv_network.cpp:373-427` | Done |
| Read-only API `GET /api/v1/peers` returning the cache | `securacv_network.cpp:787-…` | Done |
| Discovery protocol spec | `canary-vision/docs/discovery.md` | Done — defines mDNS + peer-list + manual-entry + provisioning-receipt paths |

### Build B: `firmware/projects/canary-wap/arduino/canary_wap/` (Arduino / monolith — **this audit's target**)

The WAP build is the one the captive portal and companion-PWA wizard
actually live in, and it is **behind** on multi-device infrastructure:

| Capability | State in WAP | Impact |
|---|---|---|
| Per-device mDNS hostname | **Missing.** `canary_wap.ino:5507` calls `MDNS.begin("canary")` with a hard-coded literal. Two Canaries on the same home network race for `canary.local` and the loser is unreachable by name. | **Show-stopper for multi-device.** Fix in this PR (small, surgical). |
| `_securacv._tcp` service advertisement | **Missing.** Only `http._tcp` is added. | Peers / SPA / fleet manager cannot discover this device by browsing. Fix in this PR. |
| TXT records (`device_id`, `name`, `fw`, `model`) | **Missing.** | Same blast radius. Fix in this PR. |
| `GET /api/v1/peers` peer cache | **Missing.** | Wizard can't *show* the other Canaries that exist. Deferred (large change). |
| Active mDNS browse | **Missing.** | Same. Deferred. |
| Wizard awareness of additional devices | **Missing.** Wizard text and steps are exclusively singular ("your Canary"), end is a single-link finish. | Fix wording + add a Step 6 CTA in this PR. |

### Why the WAP gap is acceptable for this PR

Implementing the full peer-browse + `/api/v1/peers` API on the WAP build
is non-trivial (it duplicates 200+ lines of mDNS query / cache code from
`securacv_network.cpp` into a flat Arduino sketch). The **highest-leverage
single change** for multi-device UX is making each WAP-built Canary
addressable by a *unique* mDNS name and advertising `_securacv._tcp` so
that the SPA / fleet manager / future companion-app can find them. Once
that is true, the user's question — "how do they find each other" —
already has an answer in the existing fleet-manager.html and
canary-vision SPA.

---

## 3. The flow we want — inspired by Philips Hue, sized for security

Hue gets multi-device setup right because it treats the bridge + bulb
constellation as one object: you commit to having a few of them at
purchase time, they auto-discover, the app names them by room. We don't
have a bridge — every Canary is its own bridge — so we cannot lift Hue's
flow wholesale. But we can borrow the shape.

### 3.1 Multi-device mental model

```
                ┌─────────────────────────────────────┐
                │  HOME (one WiFi, one household key) │
                │                                     │
   ┌──────────┐ │  ┌──────────┐    ┌──────────┐       │
   │ Canary 1 │─┼─▶│ Canary 2 │◀──▶│ Canary 3 │ … ≤8  │
   │ "Porch"  │ │  │ "Living" │    │ "Garage" │       │
   └──────────┘ │  └──────────┘    └──────────┘       │
        ▲       │       ▲                ▲            │
        └───────┼───────┴────────────────┘            │
                │       (mDNS _securacv._tcp)         │
                └─────────────────────────────────────┘
                            │
                       ┌────▼─────────┐
                       │ Phone / SPA  │  Loads any one device's IP →
                       │ /home view   │  /api/v1/peers → all the others
                       └──────────────┘
```

Each device is independently authenticated (its own API token); knowing
a peer exists does not grant access to it. The user pairs the SPA to
each device exactly once, by scanning that device's QR or receipt.

### 3.2 First-time-user journey (the "happy path" Apple-tone version)

1. **Open box → power on first Canary.** Phone notices a new WiFi
   network `SecuraCV-XXXX`. iOS captive-portal popup auto-opens to the
   QR page. *(Already implemented.)*
2. **Scan the QR.** Wizard opens at `/companion?token=…` and runs
   steps 1–5: greeting → pick home WiFi → password → connect → preflight.
   *(Already implemented.)*
3. **NEW — Step 6: "Where will it live?"** User types a *zone name*
   (e.g. "Front porch"). Same one-line input the wizard already
   persists via `wizard::set_zone_name`. The wizard then shows
   placement guidance (see §5 below).
4. **NEW — Step 7: "Most homes use more than one."** Two cards:
   - **Add another Canary** → "Power on the next one. We'll wait."
     When `SecuraCV-YYYY` appears on the air, the wizard pivots to a
     re-entry of Step 1 with the new device. Same captive-portal QR,
     same `?token=…`, same 5 steps.
   - **I'm done for now** → goes to the single-device dashboard.
5. **Repeat 2–4 up to 8 times.** After the second device, the wizard
   also shows the running list ("You've set up: Porch, Living Room")
   pulled from `/api/v1/peers` on whichever device the phone is
   currently on.

### 3.3 Re-entry path (adding a Canary after first-time setup)

The Hue equivalent is "Settings → Add bulb." Ours is:

- From the fleet view (see §4): tap **"+ Add another Canary"**. Page
  instructs: power on the device → connect phone to `SecuraCV-YYYY` →
  scan the QR. After the new device joins home WiFi the SPA detects it
  by mDNS browse and offers a one-tap "Adopt: Front Yard" card.

---

## 4. "See everything at once" — the timeline / peek view

This is the user's most evocative request:

> we maybe need a main timeline? Idk whats the best way for the user to see
> everything all at once like a normal security camera peek

We already have the pieces. We just have to wire them together and serve
them from somewhere a new user will find.

### 4.1 The /home view (NEW)

Serve `firmware/fleet-manager.html` from `GET /home` on the WAP build
(small change, large impact) so the wizard's "Finish" can link to
`http://<this-device>.local/home`. The fleet manager already:

- Renders one card per Canary with online/offline, chain status,
  uptime, free-heap, record count.
- Auto-refreshes every 30 s.
- Opens a per-device drill-down with **Status / Logs / Chain / Camera
  Peek / Export** tabs.
- Persists the device list in `localStorage` — survives reloads.
- Has zero dependencies, no cloud, works offline.

We *amend* it to:

- **Bootstrap from mDNS** (`fetch('/api/v1/peers')` against the device
  serving the page) so a fresh visit auto-populates the grid without
  the user typing IPs.
- Add a **"Today" timeline strip** along the top, scanning each
  device's last-24h ribbon (`/api/csi/ribbon` already exists per
  device — composite is just N parallel fetches).
- Add a **"Peek wall" toggle** that converts the grid into a 2×N MJPEG
  thumbnail wall (`/api/peek/stream` per device, capped to whichever
  devices have `peek` capability). This is the "normal security camera
  peek" the user described.
- Add a **per-card zone label** drawn from `/api/status` →
  `wizard.zone_name`.

### 4.2 Why the "timeline" wants to be a 24-hour ribbon, not a video file

We do not store video (Invariant I). The only continuous, queryable
timeline we *can* present is the per-second activity ribbon the orb
already produces. Stacked vertically — one ribbon per Canary — that's a
**spatial timeline**: a single horizontal time axis, one row per zone,
color-coded by state (empty / sensing / subtle / quiet / active /
together). It reads like a 24-hour weather chart. Tapping a cell opens
that zone's "Today" sheet which is already implemented in
`csi_dashboard_html.h` (the `<aside id="todaySheet">` block).

### 4.3 What we are NOT building

- **No multi-camera MJPEG composite recording.** Peek is on-demand,
  per-device, in browser memory only. We never tile streams server-side.
- **No cloud aggregation.** The SPA reaches each device directly over
  the LAN.
- **No "Live View" framing.** The hero affordance stays the calm orb
  ribbon — the peek wall is one tap behind a button, not the default,
  to preserve the privacy-first feeling.

---

## 5. Placement guidance to bake into the wizard copy

The user explicitly asked for "typical arrangement and distance they
need to be placed." These are the numbers we will surface in step copy
when we add the multi-device hint to the wizard. Sources: the existing
RF-presence sensitivity defaults in `csi_features.cpp` (sense radius
~6 m at balanced sensitivity) and ESP32-S3 antenna characteristics on
the XIAO-S3-Sense board.

| Zone shape | Suggested Canaries | Notes |
|---|---|---|
| Studio / 1-bedroom apartment | 1–2 | One in the main living area; second optional on the entry hallway |
| 2-bedroom apartment / small house | 3 | Living + Kitchen + Entry |
| Single-family house, 1 floor | 3–5 | One per major room you care about + one near the front entry |
| Multi-floor house | 5–8 | One per floor at minimum; plus garage / back patio if covered |
| Detached studio / garage / shop | +1 per outbuilding | Needs to be on the same WiFi |

**Placement rules (verbatim copy for the wizard):**

- **Head height.** Mount or rest near eye level. The sensor is happiest
  when motion in the room crosses its line of sight, not when it stares
  at a ceiling fan.
- **Three meters apart.** Two Canaries closer than ≈3 m
  (10 ft) will see the same person twice and double-count motion.
- **Away from the WiFi router.** Keep at least one good wall between
  the Canary and your router — the router's own 2.4 GHz chatter
  drowns out the subtle changes Canary listens for.
- **Not behind a TV or screen.** A loud RF emitter directly in
  front of the antenna blinds the sensor. The bookshelf is fine.
- **One room, one Canary.** Two in the same room don't add resolution,
  they fight each other. If a room is L-shaped, treat each leg as a
  room.

These can be presented as a brief "Why more than one" card in step 1
and then again as the placement check on step 6.

---

## 6. Microcopy — Apple-tone, drop-in ready

To use verbatim in `companion_pwa.h` and `setup_page_html.h`.

**Captive portal page** (current: "Set up your Canary"):

> Set up your first Canary
> *Most homes use 3 or 4. You can add the next one in a few minutes once this
> one is online.*

**Wizard step 1 — "We see your Canary"** (add one more bullet to the
existing intro list):

- Privacy-first: nothing leaves your home unless you ask.
- Setup takes about a minute.
- You'll need your home WiFi password handy.
- Most homes use 3 or 4 — one per room you care about. You can add more
  after this one is online.

**Wizard step 5 — after preflight passes** (NEW block above the open
link):

> **Where will this one live?**
> Pick a short name. It shows up in alerts and on your Today view.
> *Examples: Front porch · Kitchen · Garage · Back yard · Nursery*

**Wizard step 5 — after the zone name is set** (NEW CTA pair):

> **Add another room?**
> Power on your next Canary. We'll wait. About one minute per device.
>
> [ Set up another Canary ]   [ I'm done for now ]

**Placement card** (shown on the "another room" step):

> **Where to put the next one**
>
> · About head height. Crossing motion reads better than overhead.
> · Three meters from the last Canary. Closer and they hear each other.
> · One good wall away from your WiFi router.
> · Not behind a TV or large screen.
> · One Canary per room.

---

## 7. Concrete code changes shipping in this PR

Small, surgical, mostly copy + one mDNS fix. Larger items are scoped
for follow-ups.

| # | Change | File | Why |
|---|---|---|---|
| 1 | Per-device mDNS hostname (`canary-s3-A1B2`) + advertise `_securacv._tcp` with TXT records | `canary_wap.ino` | Two Canaries can finally coexist on `*.local`; SPAs & fleet manager can browse |
| 2 | Wizard step 1 intro: add "Most homes use 3 or 4" bullet | `companion_pwa.h` | Frame the multi-device expectation up-front |
| 3 | Wizard step 5: add zone-name input + "Add another Canary" CTA pair with placement card | `companion_pwa.h` | The terminal step now branches into "another room" rather than dead-ending at one device |
| 4 | Captive-portal lead copy: "Set up your **first** Canary" + multi-device hint | `setup_page_html.h` | Sets expectations on the literal first screen |
| 5 | This audit | `docs/audit/wap_multi_device_ux_audit.md` | Captures the orphan-page inventory and the design for follow-ups |

### Scoped for follow-up PRs

| Follow-up | Reason for splitting |
|---|---|
| `GET /home` route on WAP that serves `fleet-manager.html` and bootstraps from `/api/v1/peers` | Requires porting `fleet-manager.html` into a PROGMEM blob and wiring an httpd handler — mechanical but bulky |
| `GET /api/v1/peers` on WAP, with the mDNS browse loop | Direct port from `securacv_network.cpp`; ~250 lines on its own |
| 24-hour ribbon strip in the fleet view | Needs `/api/csi/ribbon` per-device exposed (some boards already; not all) |
| Peek wall toggle in fleet view | Needs N-way `peek/stream` budget logic to avoid blowing the device's HTTP backlog |
| "Adopt" affordance on the fleet view when an unowned device appears on mDNS | Token claim is per-device — the affordance is just an "Open setup" link to that device's `/companion` |
| BLE/CSI peer-presence cross-link (Canary A confirms Canary B is "alive" via 2.4 GHz, not WiFi) | Already half-built in `mesh_network.cpp` but not surfaced to UI |

---

## 8. Acceptance criteria the next reviewer can check

1. Boot two WAP-built Canaries on the same home WiFi. **Both are
   reachable** at distinct `canary-s3-XXXX.local` names. *(`avahi-browse -r
   _securacv._tcp`* lists both with their TXT records.)*
2. Fresh-out-of-box wizard run: the step-1 intro and the captive-portal
   page both reference "more than one" without making the user feel
   obligated.
3. After a successful single-device setup, the user can tap **"Set up
   another Canary"** and the wizard guides them back to the captive
   portal of the next device with no jargon and no app install.
4. Placement card text matches §5 verbatim (so it can be QA'd by
   reading the design doc).
5. None of the changes touch the device's witness chain, signing, or
   privacy invariants. (This is microcopy + one mDNS init line. No new
   data leaves the device.)
