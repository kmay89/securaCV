# The full stack, end to end — hub + eyes + Canaries

**One page, one journey: an SD card in your hand → a self-healing hub → live
camera detection → signed witness claims on a dashboard.**

Other docs cover each piece in depth. This is the **golden path**: the order to
do things in, what "working" looks like at each step, and the gotchas we hit so
you don't. It is **built from real runs on real hardware** — every ✅ below was
observed, not assumed. Steps still marked ⏳ are honest about being unproven.

> **Why this order:** the hub is the brain (broker + witness kernel + dashboard).
> Everything else — cameras, detectors, Canaries — reports *to* it. Stand up the
> brain first and each later piece has somewhere to arrive.

---

## The map

```
   ┌─────────────────────────────────────────────────────────────┐
   │  1. THE HUB  — Raspberry Pi 4/5, Home Assistant OS           │
   │     Mosquitto (broker) + securaCV kernel + dashboards        │
   └───────────▲─────────────────────▲───────────────────────────┘
               │ MQTT                │ MQTT
   ┌───────────┴───────────┐  ┌──────┴────────────────────────┐
   │ 2. THE EYES           │  │ 3. THE CANARIES               │
   │   Frigate detection   │  │   ESP32 devices, flashed      │
   │   • on the Pi (CPU)   │  │   over USB-C                  │
   │   • or a Jetson (GPU) │  │                               │
   └───────────────────────┘  └───────────────────────────────┘
```

You need **step 1**. Steps 2 and 3 are independent — do either, both, or neither.

---

## What you need

| For | Hardware |
|---|---|
| **The hub** (required) | Raspberry Pi 4 (4 GB+) or Pi 5, a **32 GB+** microSD (or NVMe/SSD on Pi 5 — far better endurance), USB-C power, ethernet or Wi-Fi |
| **The eyes** (optional) | Any RTSP camera. Detection runs on the Pi's CPU (works, slow), a Coral USB TPU, or an **NVIDIA Jetson Orin Nano** (GPU, smoothest) |
| **The Canaries** (optional) | An ESP32-S3 board + a USB-C data cable |

---

## Step 1 — Flash the hub

Use the **SecuraCV Flasher** desktop app: it writes Home Assistant OS, bakes in
your Wi-Fi, and verifies every byte it wrote.

1. Get the Flasher — [`desktop/INSTALL.md`](../desktop/INSTALL.md). On macOS an
   unsigned build needs one command on first launch (the install doc has it);
   signed builds just open — see [`desktop/SIGNING.md`](../desktop/SIGNING.md).
2. Open **Build a Hub**, pick your board, type your **Wi-Fi**, insert the card.
3. The picker only ever offers **removable/external** disks — your computer's own
   drives are never listed. Confirm the card, type `ERASE`, and go.
4. **macOS will ask for Touch ID or your password.** That's Apple's `authopen`
   helper — the same mechanism Raspberry Pi Imager uses — letting us write the
   card without running as admin. Expected; approve it.
5. It downloads → verifies the checksum → writes → **reads every byte back** and
   re-hashes. A counterfeit card that lies about writes fails right here.

**Working looks like:** "written and read back — the card verifiably holds the
image", then the Wi-Fi seed step.

> **Gotcha (fixed):** on macOS, writes used to fail at
> `couldn't sync the device before verification: Inappropriate ioctl for device
> (os error 25)`. Raw `/dev/rdiskN` doesn't support `fsync`; we now flush the
> device's own cache instead. **Use a Flasher build newer than that fix** — if you
> hit this error, your build predates it.

## Step 2 — First boot

Put the card in the Pi, connect power (and ethernet if you're not using Wi-Fi).

- **First boot takes 10–20 minutes.** It's installing itself. The blinking light
  is normal; walk away. The Flasher watches for it and tells you when it's up.
- Then open **`http://homeassistant.local:8123`**.
- Create your owner account — unless you typed one in the Flasher: keep the
  Flasher open and it creates that account on the hub the moment it comes
  online (over Home Assistant's own setup API), checks the login works, and
  tells you. Your first visit is then a **sign-in**, not a setup wizard.

**Working looks like:** the Home Assistant dashboard in your browser.

> **Gotcha:** if `homeassistant.local` doesn't resolve (some networks/VPNs block
> mDNS), find the Pi's IP in your router's client list and use `http://<ip>:8123`.

## Step 3 — The brain: broker + securaCV

The hub needs an MQTT broker, then the securaCV kernel that turns detections into
signed claims.

> **The no-hands path (experimental):** if you ticked **"Let this app finish hub
> setup by itself"** when flashing, skip this whole step — keep the Flasher open
> and it does it for you. The card carries the self-setup bundle and a
> maintenance key; the moment the hub answers, the Flasher connects to the hub's
> service console over your network and installs the broker, the MQTT
> connection, Frigate, and securaCV, narrating every step in its console. The
> Pi never needs a monitor — the Flasher's screen is the screen. If a run stops
> partway it's safe to press "Run self-setup again": it never repeats a
> finished step. (Manual fallback from that same console:
> `sh /mnt/boot/CONFIG/securacv/host_provision.sh`.)
>
> The self-setup can also install **Pi-hole** (recommended, one untick to
> skip; manual: append `--with pihole`). Why it's paired with securaCV: our
> promise is devices that don't talk out, and Pi-hole's local DNS log is how
> you *check* that promise rather than take our word — one page shows every
> domain every device (Canaries included) asks for, and known ad/tracker
> domains get refused for the whole house as the side effect. Be clear-eyed
> about what that costs: to tell you *which* device asked, Pi-hole logs the
> client's IP (and hostname where the network supplies one) with the domain and
> the time — never page contents. That log is the feature and it is also a
> record of your household's lookups, so it stays on the hub, nothing is
> uploaded, and retention is yours to shorten or switch off in Pi-hole's
> settings (blocking still works with query logging off). It does nothing at
> all until you point your router's DNS at the hub's IP.
>
> **A screen on the hub (optional):** the hub never needs one — but if you've
> plugged an HDMI display into the Pi (say a 7" 1024x600 IPS touchscreen with
> USB touch), self-setup can also install **HAOSKiosk** (tick "Also install the
> hub display" in the Flasher; manual: append `--with display`). It runs a
> small browser on the hub itself and shows your dashboard on that screen
> full-screen, touch and all. No vendor cloud and no new account — though it is
> a real browser, so a dashboard that embeds outside content (a weather card, a
> remote image) fetches it on the screen exactly as your phone does opening the
> same dashboard. One step stays yours, and the installer never mints or
> carries a password: give the screen a **dedicated non-admin user** (Settings
> → People → Users → Add User — name it `screen`, leave Administrator off; the
> add-on keeps that password in its configuration, and a screen only needs to
> view dashboards), then open Settings → Apps → **HAOS Kiosk Display** →
> Configuration, enter that login, and press Start. Zoom, rotation, and
> screen-timeout for your particular panel live in the same tab. No screen
> attached? The add-on simply won't start, and nothing else cares. By hand
> instead: add `https://github.com/puterboy/HAOS-kiosk` as an app repository
> and install **HAOS Kiosk Display** from it — same result. Like Frigate and
> Pi-hole above, it's a community add-on tracking its own upstream releases.

By hand, it's two installs:

1. **Mosquitto** — Settings → Apps → App Store → **Mosquitto broker** →
   Install → Start.
2. **securaCV** — add this repo as an app repository
   (`https://github.com/kmay89/securaCV`), then install the **Privacy Witness
   Kernel** app and start it. It **auto-discovers** the broker; no MQTT config
   to type.
3. Open the **SecuraCV** panel in the sidebar — its wizard walks the rest.

Detail: [`homeassistant_setup.md`](homeassistant_setup.md).

**Working looks like:** a SecuraCV panel in the sidebar, app healthy.

> **Why this order, in machine-readable form:** the whole provisioning sequence —
> every step, what it does, *why it exists*, and what it buys you — is generated
> into [`canary-local/devices/hub_seed.json`](../canary-local/devices/hub_seed.json)
> from the repo's own sources. The installer that runs it, this guide, and the
> flasher UI all read that one plan, so they can't drift apart. If you ever wonder
> "what is this about to do to my house?", that file answers it line by line — and
> [`hub_seed_apply.py --dry-run`](../canary-local/tools/hub_seed_apply.py) prints
> it back to you, step by step with the exact API call each one makes, changing
> nothing.

## Step 4 — The eyes: camera detection

Frigate does the vision; securaCV subscribes to it over MQTT and turns detections
into identity-stripped, signed claims. **Pick where detection runs:**

### Option A — on the hub (simplest)
Install the **Frigate** app (its store repo:
`https://github.com/blakeblackshear/frigate-hass-addons`, app slug
`ccab4aaf_frigate`) and use our curated config as the starting point:
[`homeassistant/frigate/config.yaml`](../homeassistant/frigate/config.yaml) →
place it at `/addon_configs/ccab4aaf_frigate/config.yml`.

Then edit the `cameras:` block — **two edits, and the second is easy to miss**:

```yaml
cameras:
  front_door:                    # name it whatever you like
    enabled: true                # ← the template ships `false`; you MUST flip this
    ffmpeg:
      inputs:
        - path: rtsp://USER:PASS@10.0.0.20:554/stream   # ← your camera's real URL
          roles: [detect]
    detect: { width: 1280, height: 720, fps: 5 }
```

Restart the app. (The example camera ships **disabled** so the config is valid
before you've added anything — leave it `false` and Frigate silently ignores it.)

**Using a Coral USB TPU?** Plugging it in changes nothing on its own — the
curated config selects the **CPU** detector. Swap that block too:

```yaml
detectors:
  coral:
    type: edgetpu
    device: usb      # PCIe/M.2 Coral: use `pci`
```

Without this you stay on the slow CPU path with a TPU sitting idle.

### Option B — on a Jetson Orin Nano (GPU, smoothest)
A dedicated detector node — `docker compose up`, no Coral needed:
[`integrations/jetson-detector/`](../integrations/jetson-detector/README.md).
The Jetson does the vision on its GPU and publishes to the hub's broker. (Home
Assistant OS doesn't run on Jetson — it's the *eyes*, not a second hub.)

> **Note:** go2rtc is **built into Frigate** — it is not a separate app.
> **Running both A and B?** Give each a distinct `mqtt.client_id` *and*
> `topic_prefix` (Mosquitto allows one connection per client ID), and point
> securaCV's `frigate.topic_prefix` at the one you want it to read.

> **Privacy default:** recordings **and** snapshots ship **off** — securaCV is
> "claims, not recordings". Detection and events work fully with no raw imagery
> stored. Turn them on only if you deliberately want stored images (and use an
> SSD — continuous recording destroys SD cards).

**Working looks like:** the Frigate UI shows live detection boxes, and new
witness claims appear in the SecuraCV panel when it sees a person.

## Step 5 — The Canaries (optional)

ESP32 devices that witness locally and report to the same hub. Flash one over
USB-C with the same Flasher ("Flash a Canary"), then follow
[`getting_started_canary.md`](getting_started_canary.md). Freshly-flashed devices
appear on the Flasher's Witness Wall automatically.

---

## Step 6 — Give it a voice (optional)

The hub can answer "is the fleet OK?" out loud, with every stage — wake
word, Whisper speech-to-text, the answer, Piper text-to-speech — running
locally on the Pi. One command sets it up, and the guide covers the
microphone hardware worth buying and the honest wake-word trade:
[**Talking to your fleet**](voice_control.md).

---

## When it's all up

- **Dashboard:** the SecuraCV panel + the Lovelace cards
  ([`lovelace_timeline.md`](lovelace_timeline.md)).
- **Automations & alerts:** [`homeassistant_automations.yaml`](homeassistant_automations.yaml)
  and the blueprints in [`blueprints/`](blueprints/).
- **It maintains itself:** Home Assistant OS has an immutable A/B root that rolls
  back a failed boot, a supervisor that restarts crashed apps, and hands-off
  updates. The securaCV app rides that same update channel — this is why the
  hub is built on HAOS rather than a hand-rolled image.

## If something's stuck

| Symptom | Try |
|---|---|
| Flasher write fails with `Inappropriate ioctl` (macOS) | Your build predates the rdisk cache-sync fix — get a newer Flasher |
| `homeassistant.local` won't resolve | Use the Pi's IP from your router (mDNS is often blocked) |
| First boot seems hung | Give it the full 20 minutes before worrying |
| securaCV can't find the broker | Start the **Mosquitto** app first, then restart the kernel app |
| Frigate detections don't reach securaCV | Check `topic_prefix` matches on both sides; if two Frigates run, they need distinct `client_id`s |
| Camera won't open in Frigate | Verify the RTSP URL in VLC first; check credentials and the `/stream` path |

Deeper: [`frigate_integration.md`](frigate_integration.md),
[`homeassistant_setup.md`](homeassistant_setup.md), and the design record in
[`design/raspberry_pi_hub_flashing.md`](design/raspberry_pi_hub_flashing.md).

---

## Status of this guide

This page is **living** — kept honest as the stack is exercised on real hardware.

| Step | State |
|---|---|
| 1. Flash the hub | ✅ write + read-back verify proven on macOS (Pi 5, 64 GB card); the rdisk cache-sync fix is required |
| 2. First boot | ⏳ Wi-Fi seed acceptance on real HAOS not yet confirmed end to end |
| 3. Broker + securaCV | ⏳ documented from the shipped apps; not yet re-walked on a fresh flash. The Flasher's **self-setup** option now automates this step (see below) — same validation caveat |
| 4a. Frigate on the hub | ⏳ curated config committed **and** an executor that installs it via the Supervisor API (`hub_seed_apply.py`, host-tested, idempotent); the Flasher's first-boot companion now invokes it unattended |
| 4b. Jetson detector | ⏳ scaffold follows Frigate's official Jetson guidance; unvalidated on an Orin |
| 5. Canaries | ✅ shipping path, covered by its own guide |

The **one-flash dream** — Frigate + Mosquitto + securaCV all pre-installed so
first boot is already wired — now has all three pieces built: the curated
configs are committed, [`hub_seed_apply.py`](../canary-local/tools/hub_seed_apply.py)
installs the whole stack unattended via the Supervisor API, and the Flasher
seeds the bundle onto the card and **runs it itself** once the hub answers —
over the HAOS developer console (port 22222), unlocked by a maintenance key
seeded at flash time, so the hub never needs a monitor. What's left is honest:
hardware validation on a real Pi — the Wi-Fi seed, the key import, and a full
companion-driven install have not yet been watched end-to-end on a fresh flash,
which is why the option is labeled experimental and this guide keeps the
by-hand path.
