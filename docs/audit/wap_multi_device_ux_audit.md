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

> **Status note (2026-08-21):** `firmware/fleet-manager.html` no longer exists
> in the tree — it was retired in favor of the canary-vision SPA (see
> `firmware/README.md`, "The former standalone `fleet-manager.html` has been
> retired in favor of the app"). Read every mention of it below as a
> description of the tree *at audit time*: the §1 inventory row, §4.1's
> "serve it from `GET /home`" recommendation, and the §7/effort-table items
> that port it to PROGMEM are **no longer actionable as written**. The intent
> they carry — a household-aware fleet view served from a URL a new user can
> find — still stands, but it would now be built from the canary-vision SPA,
> not from the retired file.

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
| TXT records (`device_id`, `fw`, `model` — `name` deferred until the zone-name wire-up lands) | **Partial.** | Three of four added in this PR; the `name` record needs a `wizard::get_zone_name` HTTP read which is part of the household-primitive PR (§9.5). |
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

> *(No longer actionable as written — the file was retired; see the status
> note at the top.)*

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
| 3 | Wizard step 5: add "Add another Canary" CTA pair with placement card (zone-name input is deferred to the household-primitive PR — see §9.5) | `companion_pwa.h` | The terminal step now branches into "another room" rather than dead-ending at one device |
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

---

## 9. Ring-class architecture — gap analysis

After §1–§8 it is worth zooming out: are we missing categories of
capability that any user coming from Ring, Nest, Arlo, eufy, or Wyze
expects to find? This section maps each of those vendors' top-level
primitives against ours, calls out the gaps, and ranks them.

### 9.1 The cloud-app data model these products converged on

Sources:
[Ring Modes](https://ring.com/support/articles/p0klz/Controlling-Your-Ring-Devices-with-Modes),
[Ring Locations](https://ring.com/support/articles/8y35i/managing-locations-in-the-ring-app),
[All Locations Dashboard](https://ring.com/support/articles/3li18/all-locations-dashboard),
[User Permissions](https://en-uk.ring.com/blogs/alwayshome/new-user-permissions-access-management-for-businesses-and-homes),
[dgreif/ring](https://github.com/dgreif/ring),
[Ooma geofencing](https://support.ooma.com/security/ooma-smart-security-geofencing/),
[eufy unified modes wishlist](https://community.security.eufy.com/t/unified-device-modes-geofencing-beyond-homebase2/205457),
[Arlo vs Ring 2026](https://top-home-security.com/arlo-vs-ring/),
[Tom's Guide AI comparison](https://www.tomsguide.com/home/smart-home/which-security-camera-has-the-best-ai-we-put-six-to-the-test-from-google-ring-blink-and-others-to-find-out).

Effectively every consumer security-camera app has converged on this
shape:

```
Account
  └── Location[]              ← top-level "Home" / "Cabin" container
        ├── mode              ← Disarmed | Home | Away (location-wide)
        ├── members[]         ← owner + role-based shared users
        ├── geofence?         ← multi-phone polygon → auto-mode
        ├── cameras[]
        │     └── per-mode behavior matrix
        │         (record? live-view? notify? lights?  per mode)
        ├── doorbells[]
        ├── sensors[]         ← contact, motion, glass-break, …
        ├── chimes[]
        ├── intercoms[]
        └── timeline          ← cross-device event scrub, AI-filtered
```

Five primitives carry the whole user mental model:

1. **Location** — the unit a person identifies with ("my house").
   Always plural-capable. Devices live inside a Location, not on the
   account.
2. **Modes** — Disarmed / Home / Away (Ring) or Custom (Arlo) —
   **set once at the Location, applied per-device** via a behavior
   matrix. "All cameras record in Away, only the doorbell in Home."
3. **Members + Permissions** — role-based, evolved past Ring's old
   binary share. Whether a shared user can change modes is a
   permission flag.
4. **Geofence** — multi-phone presence. Last-out triggers Away,
   first-in triggers Home. Phone GPS is the source of truth.
5. **Timeline** — one scrollable event ledger across all cameras in
   the Location, with AI labels (person / pet / vehicle / package /
   familiar face) as filters. This is the "see everything at once"
   surface Ring users return to daily.

Beyond the five, four cross-cutting features are table stakes:

- **Push notifications with snapshot + deep link.** Tap the notification,
  app opens directly to that camera's live view.
- **Live multi-camera grid.** Tile every camera's stream at once.
- **Activity zones.** Pixel polygons that gate which motion fires
  alerts.
- **Device health dashboard.** Unified offline / battery / signal view.

### 9.2 SecuraCV vs the Ring-class baseline — what we have, what we are missing

Where the same primitive exists with a different mechanism (because of
Invariants I–IV — no cloud, no video storage, no identity data, no
telemetry), we list both columns honestly.

| Category | Ring / Nest / Arlo | SecuraCV WAP today | Gap |
|---|---|---|---|
| **Account / identity** | Cloud account (email + password + 2FA) | Per-device API token in localStorage | **Missing**: no user/account concept above the device. No way to know "this phone owns these 8 Canaries". |
| **Location ("Home") object** | First-class. Multi-Home supported. | `mesh_network.cpp:opera_id` exists in firmware but is not surfaced in the wizard or any UI. | **Missing in UI.** The primitive exists at the byte level; we never name it for the user. |
| **Mode (Disarmed / Home / Away)** | Location-wide; per-device behavior matrix. | Per-device `notify::Context` (HOME / AWAY / QUIET_HOURS / TRAVELING). Each Canary holds its own. No propagation. | **Critical gap.** Tapping "Away" on one Canary does **not** flip the others. This is the most user-felt missing piece for a multi-device household. |
| **Members / shared users** | Owner + roles, per-Location. | Every API token is full-power. No "guest" tier. | **Missing.** Family sharing and dog-walker tokens both need this. |
| **Geofencing** | Multi-phone polygon, auto-mode. | Not implemented. | **Missing.** Privacy-preserving variant is trivial: phone-side `navigator.geolocation` + a polygon stored only on the phone; mode delta broadcast to devices. No GPS leaves phone. |
| **Notifications: push + snapshot + deep link** | FCM/APNS, low-res thumbnail in the push, deep link to camera. | `notify.cpp` fires alerts; HA / MQTT / BLE channels exist; no Web Push, no deep links. | **Missing.** Privacy-respecting equivalent: Web Push (VAPID, no third party) with zone name + state badge + deep link to that zone's Today view. We never send pixels. |
| **Multi-device timeline** | Single scroll across all cameras, AI-filtered. | Per-device 24h ribbon (`csi_dashboard_html.h`); no cross-device merge. | **Partial.** Scoped in §7 as the `/home` view follow-up. |
| **Multi-camera live grid** | Native tile view. | `fleet-manager.html` has per-device peek; no grid. | **Partial.** Scoped in §7 ("Peek wall toggle"). |
| **Activity zones** | Pixel polygons in camera frame. | Spatial zones — one Canary == one room (`wizard::zone_name`). | **Equivalent by design.** We don't have pixels, we have rooms. Different but correct for our sensor. |
| **AI labels (person / pet / vehicle / familiar face)** | Always-on, cloud-trained. | Pet Mode toggle; `familiar.cpp` Bloom-filter pattern suppression. No persons, no faces, no vehicles. | **Intentional rejection** (Invariant II). Document, don't fix. |
| **Device offline alerting** | "Your Front Yard cam went offline." | `mesh_alerts` tracks peer disappearance; never surfaced to phone. | **Missing the last mile.** Data is there; needs phone-side notify. |
| **Device health dashboard** | Unified across all cameras. | `fleet-manager.html` per device; no unified roll-up. | **Partial.** Same fix as the timeline. |
| **Snooze all** | "Pause notifications for 1 hour." | Per-device quiet hours; no household-wide snooze. | **Missing.** Tiny, high-value. |
| **Daily summary** | Push digest. | `meta_daily_summary.cpp` exists — does it reach a UI? (TBD; not visible in any served page.) | **Built but invisible.** Surface in `/home`. |
| **Settings backup / portability** | Cloud-mediated. Restore to a new device. | Each device's NVS is an island. Per-device `/api/export` is operational state, not setup. | **Missing.** A Household JSON the phone holds + restores. |
| **OTA firmware updates** | Cloud-pushed, signed, automatic. | BLE OTA + signed manifests. Manual. | **Have the signing primitive.** Missing the "all my devices update overnight" flow. |
| **Onboarding** | App-led BLE pairing or QR. | Captive-portal QR. Works, different model. | **OK.** §3.2 / §6 covers the multi-device version. |
| **Two-way audio** | Yes. | Not in spec; mic is RF only, never streams out. | **Intentional rejection** (Invariant I). |
| **Emergency response / monitoring** | Paid tier. | Out of scope. | **OK.** |
| **Witness chain / tamper evidence** | Optional E2E (some). | Always-on, default. | **We are ahead.** |
| **Telemetry / analytics** | Always-on. | None (Invariant IV). | **We are ahead.** |

### 9.3 The five missing primitives, ranked

Of the gaps above, five are *structural* — they don't fit elsewhere
without inventing a new concept — and the rest are features that
build on them. In dependency order:

1. **Household primitive.** The lynchpin. A 32-byte `household_id`
   plus an Ed25519 owner keypair, generated on the **first** device's
   wizard, written to that device's NVS, and *handed to* subsequent
   devices during their own wizard via a short code (or QR re-key)
   the user reads off the dashboard. Without this:
   - Modes cannot be unified.
   - Roles cannot exist (whose token are we scoping?).
   - Geofencing has no target audience.
   - Backup / restore has nothing to back up.
   The firmware infrastructure for this is already partly present in
   `mesh_network.cpp` (`opera_id` is a household-equivalent key used
   for mesh authentication); what's missing is exposing it through
   the wizard and API.

2. **Mode propagation.** Once a household exists, the phone can fan
   out a mode change to every device by walking `/api/v1/peers` and
   POSTing the new context. Or, better: any one device broadcasts via
   the existing mesh transport (`mesh_broadcast_witness` already
   exists) and peers update locally. Each Canary's `notify::Context`
   is **already** the right per-device state — we just don't fan
   them out today.

3. **Web Push notifications with deep links and zone snapshots.**
   Privacy-preserving variant of Ring's killer notification. The
   "snapshot" is a state badge (`"Front Porch · Active"`) plus a
   deep-link URL into that zone's Today sheet. No pixels. Uses VAPID
   — no third-party push relay. Each Canary can run its own VAPID
   subscription endpoint; the phone subscribes once per Canary via
   the existing service worker (`companion-sw.js`).

4. **Geofencing.** Pure phone-side. `navigator.geolocation` +
   a polygon the user paints once on the `/home` view. When the
   phone leaves the polygon, the SPA hits `POST /api/wizard/mode` (a
   new endpoint, scoped in #2 above) on every Canary in the
   household. **No GPS coordinate ever leaves the phone.**

5. **Role-scoped tokens.** The current API token is a single power
   class. Add `owner` (everything), `family` (status + peek + mode
   change), `guest` (status + chain verify, no peek, no mode). Each
   Canary stores a small ACL keyed off token prefix. Trivial NVS
   addition. The wizard's "Share" affordance prints a one-shot QR
   that encodes the device URL + a freshly-minted guest token.

### 9.4 Sketch of the household-aware data model on the phone

```js
// localStorage["securacv_household"]
{
  household_id: "8a3f...",                // 32-byte hex
  household_name: "Maple Street",
  owner_pubkey: "ed25519:...",
  owner_privkey: "ed25519:...",           // never sent over the wire
  mode: "home",                           // "home" | "away" | "quiet" | "disarmed"
  geofence: {                             // optional, phone-only
    center: [lat, lon],                   // never POSTed anywhere
    radius_m: 150
  },
  members: [
    { role: "owner",  pubkey: "...", added_at: ... },
    { role: "family", pubkey: "...", added_at: ... }
  ],
  canaries: [
    {
      device_id: "canary-s3-A1B2",
      base_url: "http://canary-s3-a1b2.local",
      token: "cv_...",                    // owner's full token for this device
      zone_name: "Front porch",
      added_at: ...,
      per_mode: {                         // per-Canary behavior matrix
        disarmed: { notify: false, peek_allowed: false },
        home:     { notify: true,  peek_allowed: true  },
        away:     { notify: true,  peek_allowed: true  },
        quiet:    { notify: false, peek_allowed: true  }
      }
    },
    // … up to 8
  ]
}
```

This object is the equivalent of Ring's "Location" — but **on the phone**,
not in a cloud. It is signed by `owner_privkey` and the resulting blob
is what shared-user QRs encode (minus the privkey, plus a role tag).

### 9.5 Suggested PR ordering after this one

| PR | Net new firmware lines | Primitive introduced |
|---|---|---|
| Next | ~150 | Household primitive: `wizard::household_id` + NVS key + a `GET/POST /api/v1/household` endpoint. Wizard step 1.5: "Is this the first Canary, or are you adding to an existing home?" |
| +1 | ~80 | `POST /api/v1/mode` on every Canary; phone-side fan-out. |
| +1 | ~600 | `/home` route serves a household-aware fleet view (port `fleet-manager.html` to PROGMEM with `/api/v1/peers` bootstrap + 24h ribbon strip). |
| +1 | ~200 | Web Push with VAPID, zone-name snapshot, deep link. |
| +1 | ~150 | Geofence polygon on the `/home` view; phone-side mode toggle. |
| +1 | ~100 | Token role scopes. "Share with…" QR generator. |
| +1 | ~80 | Device-offline cross-watch surface (peer A reports peer B missing). |
| +1 | ~120 | Household backup JSON + restore. |

This ordering keeps each PR <1 KLOC of firmware diff and lets QA bring
up one capability per release.

### 9.6 What we are deliberately *not* copying

| Ring/Nest/Arlo feature | Why we say no |
|---|---|
| Cloud video storage | Invariant I. We never store video; nothing to upload. |
| Familiar-face recognition | Invariant II. No identity data. |
| Person/pet/vehicle AI labels (frame-based) | Same reason; also, we don't have frames. Pet Mode is our affordance. |
| Always-on snapshot in push notifications | Privacy. Zone + state badge is our snapshot. |
| Two-way audio | Mic is internal-only by design. |
| Neighbors feed / community sharing | Privacy. Hard no. |
| Telemetry / "improve our service" toggle | Invariant IV. |
| Cloud-mediated emergency response | Out of scope; a Home Assistant integration handles this for users who want it. |

These rejections are features, not bugs. The audit calls them out so
future PRs don't accidentally reintroduce them under a different
banner.
