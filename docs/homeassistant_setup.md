# Home Assistant Integration Guide

On Home Assistant OS, one command installs and wires everything — broker,
Frigate, the Privacy Witness Kernel, the SecuraCV integration, blueprints,
dashboards. That command is the first section below. The rest of this guide
is the same setup by hand: install the integration via HACS, then connect it
to your SecuraCV Canary devices via MQTT or to the Privacy Witness Kernel via
its HTTP API.

> **Prefer the guided version?** [`canary-local/homeassistant.html`](../canary-local/homeassistant.html)
> ("The Hub") walks this same setup interactively — a 3D Raspberry Pi build you
> can scrub apart, a bench terminal that replays every command below, and a
> working sketch of the dashboard you end up with. Its entity names, topics,
> and versions are generated from this doc and drift-checked in CI, so the two
> can't disagree.

## Quick start: one command

On Home Assistant OS, open the **Terminal & SSH** app (install it from
**Settings → Apps → App Store** if you haven't) and run:

```bash
curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash
```

The installer narrates each step — what it is doing and why — and does all of
this through the Supervisor API:

- registers both app repositories (Frigate's and this one)
- installs and starts **Mosquitto**, and mints a `canary` broker login for
  your devices — the password is never printed to the terminal; read it any
  time under **Settings → Apps → Mosquitto broker → Configuration → Logins**
- connects Home Assistant itself to the broker (the MQTT config entry)
- installs **Frigate** and places the curated starting config at
  `/addon_configs/ccab4aaf_frigate/config.yml`
- installs the **Privacy Witness Kernel** app in frigate mode and starts it
  (the device key is auto-generated on first start — nothing to type)
- installs the **SecuraCV integration**, restarts Home Assistant Core, and
  creates the integration's config entry automatically
- installs the notification blueprints to
  `/config/blueprints/automation/securacv/` and the dashboards to
  `/config/securacv/dashboards/`, registering them as a YAML dashboard when
  your `configuration.yaml` allows it (see
  [Step 4c](#step-4c-ready-made-dashboards))
- creates the daily-digest automation when a mobile-app notify service
  exists (install the HA Companion app first if you want that on day one)

It is idempotent: re-running it is safe and never repeats a finished step.

Two things stay yours, and the installer says so when it finishes:

1. **Point Frigate at your cameras.** Edit
   `/addon_configs/ccab4aaf_frigate/config.yml`, set your cameras' RTSP URLs,
   flip the example camera's `enabled: true`, and restart Frigate.
2. **Point your Canaries at the broker.** Host `homeassistant.local`, port
   `1883`, username `canary`, password from the Mosquitto Logins page above —
   Step 3 of the manual walkthrough covers the device side. That link is
   plain MQTT: the installer leaves Mosquitto on its default `1883`
   listener, so the `canary` password crosses your LAN unencrypted unless
   you ask the plan for the broker's TLS listener (`--with broker_tls`,
   from a certificate you supply) and provision the Canaries for it
   (Step 3 says how).

### Prefer clicking?

The same result from a browser, no terminal:

1. [Add the SecuraCV app repository](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV)
   in one click, then install **Privacy Witness Kernel** from the App Store.
2. Open its Web UI — the setup wizard walks keys, MQTT, and mode, and can
   install Mosquitto and Frigate itself if they are missing.
3. [Add the integration through HACS](https://my.home-assistant.io/redirect/hacs_repository/?owner=kmay89&repository=securacv-homeassistant&category=integration)
   in one click, restart Home Assistant, then **Settings → Devices &
   Services → Add Integration → SecuraCV** and keep the default
   **Automatic — detect what's installed**.

Everything below is the same setup done step by step — for reference, for
installs that aren't Home Assistant OS, and for troubleshooting.

## What you need

| Item | Notes |
|------|-------|
| Raspberry Pi 4 (4 GB+) or x86 PC | Pi 5 works great; ~3 cameras at 10 fps |
| Home Assistant OS | Installed from the official image |
| IP camera(s) with RTSP | Hikvision, Dahua, Reolink, Amcrest, Ubiquiti, etc. |
| HA Companion App (optional) | For push notifications to your phone |
| HDMI touchscreen (optional) | e.g. 7" 1024x600 IPS with USB touch — the hub runs headless by default, but the HAOSKiosk app (the `display` self-setup extra) can show your dashboard right on it |

## Two witness logs, two trust roots

> The kernel's sealed log covers the Frigate
> pipeline. Canary devices are independent witnesses: each keeps its **own**
> Ed25519-signed hash chain on-device and publishes signed events over MQTT, which the
> Home Assistant integration verifies against that device's pinned key. Canary events are
> *not* re-sealed into the kernel's log — a "fleet" today is N independently-signed
> canaries converging in your dashboard, each verifiable on its own.

## Manual walkthrough: Canary via MQTT

This is the Canary path the one-command installer sets up, spelled out step
by step. Canary devices auto-discover in Home Assistant via MQTT Discovery.
Even after the installer, Step 3 (pointing each Canary at the broker) is
yours — a witness device only ever joins your network with your say-so.

### Prerequisites

1. **MQTT broker** (Mosquitto recommended) running and configured in HA —
   the one-command installer above sets this up
2. **SecuraCV Canary** device powered on (any recent firmware; the update
   entity in [Firmware Updates](#firmware-updates-from-home-assistant) keeps
   it current)
3. **Home WiFi** credentials entered into the Canary via its web dashboard

### Step 1: Install the Integration

SecuraCV is distributed as a HACS **custom repository** (it is not in the
default HACS store yet):

1. Open HACS in Home Assistant
2. Click **⋮ → Custom repositories**
3. Add `https://github.com/kmay89/securacv-homeassistant` (the integration's
   distribution repository) with type **Integration**
4. Search HACS for "SecuraCV" and install it
5. Restart Home Assistant

### Step 2: Configure the Integration

The one-command installer creates this config entry for you — skip to Step 3
if you ran it. By hand:

1. Go to **Settings > Devices & Services > Add Integration**
2. Search for "SecuraCV"
3. Keep the default, **"Automatic — detect what's installed"**, and click
   Submit. The integration probes for the Privacy Witness Kernel (its
   `/health` endpoint at `http://d0491a67-privacy-witness-kernel:8799` and
   the token file at `/config/api_token`) and configures itself: kernel +
   MQTT when the kernel answers, MQTT-only otherwise. Nothing to type.
4. Or pick **"Canary devices via MQTT"** explicitly. Its one option, the MQTT
   topic prefix, defaults to `securacv` — the prefix Canary firmware always
   publishes on. Leave it alone; there is nothing to invent or match. (Change
   it only if you have deliberately re-bridged topics under another prefix.)

### Step 3: Configure the Canary Device

Connect to your Canary's WiFi AP (SSID shown on device, password is device-unique):

1. On first boot, joining the AP from a phone pops the Canary's **setup
   wizard** on its own (the captive-portal sheet) — pick your home WiFi
   there and enter its password. The wizard's finish screen also offers an
   optional **"point it at your hub"** step, prefilled with the values that
   are right for a SecuraCV hub (`homeassistant.local`, port `1883`) — enter
   the broker credentials (if you ran the one-command installer: username
   `canary`, password from **Settings → Apps → Mosquitto broker →
   Configuration → Logins**), save, and let it restart.
2. If the wizard doesn't appear (or the Canary was set up before), open
   `http://canary-<id>.local` (the device's unique hostname, shown in the
   boot banner — e.g. `http://canary-s3-ab7k.local`) or `http://192.168.4.1`
   in your browser. If you only have one Canary, plain `http://canary.local`
   may also resolve, but each Canary always advertises its unique hostname
   so multiple devices on the same network don't collide. `/setup` on any of
   those addresses reopens the wizard. On a `firmware/canary` unit that is
   already set up, a page loaded over your home WiFi arrives without its API
   token, so its saves are refused. The page carries the token only for the
   first-boot wizard, a request that already sends it, a device on the
   Canary's own WiFi, or one BOOT tap: tap the Canary's BOOT button and
   reload within 30 seconds (one tap unlocks one page load), or paste the
   token from your recovery kit into the dashboard. The Canary's own WiFi
   unlocks the page too, but it broadcasts only while the Canary is off
   your home network (before setup, or after it loses that network), so on
   a Canary that is running normally the BOOT tap is the way in. Reach it
   by an IP, its `.local` name, a single-label name or a router alias under
   `.lan`, `.internal` or `.home.arpa`: under a public DNS name the page
   loads without its token and every API call it makes, the recovery-kit
   receipt included, answers `403`, except over the Canary's own WiFi. Release images serve these pages as plain HTTP on port 80.
   Host-tested, not yet bench-tested.
3. In the dashboard, go to the **Network** tab and enter your home WiFi
   credentials (the Canary needs WiFi to reach the MQTT broker)
4. MQTT broker details (if you skipped the wizard's hub step):
   - **Host**: Your Mosquitto broker IP (e.g., `192.168.1.10` or `homeassistant.local`)
   - **Port**: `1883` (default)
   - **Username/Password**: whatever your broker requires — the one-command
     installer mints a `canary` login for exactly this (password under
     **Settings → Apps → Mosquitto broker → Configuration → Logins**)
   - **Encryption (optional, off by default)**: the broker link is plain
     MQTT unless you say otherwise, and the username/password above then
     cross your LAN in the clear. To encrypt it, run the hub plan with
     `--with broker_tls` (`sh provision.sh --with broker_tls` from the
     Terminal & SSH add-on, or `host_provision.sh --with broker_tls` from
     the developer console): it checks Home Assistant's `ssl` folder for the
     broker certificate and key you placed there (the two files the Let's
     Encrypt add-on writes by default — `hub_seed_apply.py --dry-run --with
     broker_tls` names them and the run refuses by name if either is
     missing), sets the Mosquitto add-on's two file options and restarts
     it; the plan step is the one place the file and option names are
     spelled. It mints no certificate, chooses no trust anchor, does not
     verify that the listener came up (the add-on's Log tab says why if the
     port stays closed), and leaves Home Assistant's own MQTT entry on the
     internal `1883`. Then provision each Canary with port `8883` and a TLS
     mode: **CA** (the PEM
     certificate that signed the broker's certificate — the one that always
     works) or, on canary-display / -sense / -vision and the
     `firmware/canary` build, a **SHA-256 fingerprint pin** of the broker
     certificate. On canary-wap this is the `/mqtt` page's "Encryption"
     setting; on canary-display / -sense / -vision it is the NVS keys
     `mqtt_tls` / `mqtt_ca` / `mqtt_fp` next to `mqtt_host`, set from the
     *Broker encryption* select in either flasher's broker block (the CA
     box or the fingerprint field appears for the mode that uses it; the
     plain-only nightstand-c6 is offered Plain only, with the reason); on
     the `firmware/canary` build (the wizard above) it is the hub step's
     own *Encryption* select, CA box and fingerprint field, or the API —
     `POST /api/mqtt/config` with `tls` (0 plain, 1 CA, 2 fingerprint, 3
     lab) and `fp`, and the PEM as the raw body of `POST /api/mqtt/ca`
     (`DELETE` forgets it), behind the same bearer token as the other
     endpoints; `GET /api/mqtt/status` reports the mode, the transport and
     any refusal, never the certificate or the pin. A Canary never falls
     back to plain or unverified on its own: an incomplete TLS setup
     refuses to connect and names the reason on its serial log (and, on the
     `firmware/canary` build, at save time and on its status endpoint). The
     unverified "lab" mode exists only as an explicit choice on display /
     sense / vision and the `firmware/canary` build and warns on every
     connect; the WAP offers plain or CA-verified only. None of this has
     been run against a TLS broker on hardware yet — the paths are
     compile-tested by CI and the decision is host-tested. Per-variant
     status: [firmware variant audit](FIRMWARE_VARIANT_AUDIT.md). Coming
     back to the wizard later (a new hub password, say) keeps the
     encryption you set: its *Encryption* select shows what the Canary
     holds and travels only when you change it, and a CA or pin the Canary
     already holds stands in for an empty box.
5. Save. The Canary drops its hub link and reconnects with the new settings
   on its own — no reboot. The wizard's *Restart the Canary now* button only
   closes the setup network once you are done there; the hub link does not
   need it.

### Step 4: Verify Discovery

Within 30 seconds of the Canary connecting to MQTT:

1. Go to **Settings > Devices & Services > SecuraCV**
2. You should see your Canary device listed with the entities below. This
   list is what the integration itself creates
   (`custom_components/securacv/sensor.py` and `binary_sensor.py`); each
   entity appears the first time its topic is seen, and every unique ID is
   `securacv_canary_<device_id>_<suffix>` with the suffix shown in
   parentheses. **Signed** means the publish carries the device's Ed25519
   signature and the integration checks it against the pinned key
   ([Step 6](#step-6-verify-per-device-pki-optional-but-recommended)),
   stamping `verified` / `trust_reason` / `pinned_fingerprint` /
   `received_fingerprint` attributes. Every other topic is **unsigned** — the
   firmware puts no signature on it — and those entities carry the same four
   attributes with `verified: false`, `trust_reason: unsigned`, so a
   dashboard can tell an unsigned publish from a verified one.

   The names in bold are what Home Assistant renders after the device
   name; they come from each entity's translation key, not from the code.
   To rename one, edit `custom_components/securacv/strings.json`, copy it
   over `translations/en.json` (a test keeps the two byte-identical), and
   mirror the rename in this list and in the names
   `tests/test_entity_translations.py` pins; other languages go in their
   own `translations/<lang>.json`.

   Sensors:
   - **Witness Count** — total witness records created (`witness_count`; signed `counts` topic)
   - **Chain Length** — hash-chain length, with `latest_hash` and `algorithm` (`chain_length`; signed `chain` topic)
   - **Last Event** — latest witness event type, with zone, confidence, modality and attestation (`last_event`; signed `events` topic)
   - **Health** — `healthy` / `warning` / `critical` from battery and free memory, with `public_key`, uptime and firmware version (`health_status`; `health` topic, unsigned)
   - **GPS Fix** — GPS fix status, with satellites and HDOP (`gps_fix`; `health` topic, unsigned)
   - **SD Wear Estimate** — estimated SD-card wear percent, only when the firmware reports its `sd` object (`sd_wear`; `health` topic, unsigned; diagnostic)
   - **Radar Link** — `ok` / `stale` / `down` for the UART link to the radar module, canary-sense devices only (`radar_link`; `health` topic, unsigned; diagnostic)

   Binary sensors:
   - **Online** — device connectivity, from the `status` topic (`online`; unsigned)
   - **Chain Valid** — ON only when the device reports its chain intact and the chain publish verified against the pinned key; unknown until a chain publish has been checked (`chain_valid`; signed `chain` topic)
   - **Tamper** — any tamper detected, from the `health` and `tamper` topics (`tamper`; unsigned)
   - **Power Loss**, **SD Removed**, **SD Error**, **GPS Jamming**, **Unexpected Motion**, **Enclosure Open**, **GPIO Tamper**, **Watchdog Timeout**, **Unexpected Reboot**, **Memory Critical** — one sensor per tamper type, from the `tamper` and `health` topics (`tamper_<type>` with the type names in the [per-tamper-type catalog](#per-tamper-type-sensor-catalog) below, which also says which signals firmware emits today; unsigned)
   - **SD Replacement Recommended** — the device recommends replacing its SD card (`sd_replace`; `health` topic, unsigned)
   - **Motion** and **Occupancy** — standard `motion` / `occupancy` device classes for the HomeKit Bridge and any other consumer, asserted by the signed `events` topic and, for occupancy, the retained `state` snapshot (`motion`, `occupancy`; carry the events verdict)
   - **WiFi AP**, **WiFi Station**, **MQTT**, **Bluetooth**, **Mesh Network**, **Chirp Network** — per-transport connectivity (`transport_<type>`; `transport` topic, unsigned; no current firmware publishes it — see the [transport catalog](#transport-catalog))
   - **Mesh Connected** — Opera mesh peers present, with peer and relay counts (`mesh_connected`; `mesh` topic, unsigned)
   - **Chirp Active** — Chirp community network enabled and ready, with the session emoji and alert counters (`chirp_active`; `chirp` topic, unsigned)

   With a kernel configured, the integration also creates the kernel's own
   device: SecuraCV Last Event, Storage Health, Storage Free, Storage Wear
   Estimate, Storage Write Rate and SoC Temperature sensors, Online and
   Storage Replacement Recommended binary sensors, and — when an adapter
   stats URL is set — a SecuraCV Adapter Host diagnostic.

   The Canary firmware also announces entities of its own through native
   MQTT discovery (`homeassistant/*/securacv_*/config`, retained). Those
   are created by the device, not by the integration, and carry no
   verification attributes; where the two overlap (Witness Count, Chain
   Valid, Online, GPS Fix) you will see both — see the `_2` row under
   [Troubleshooting](#troubleshooting-mqtt-setup). The firmware's own set
   includes:
   - **Chain Sequence** — current chain sequence number
   - **Uptime** — device uptime
   - **Free Heap** — available memory
   - **GPS Satellites** — satellite count
   - **Tamper Detected** — tamper event status
   - **SD Card Healthy** — storage health
   - **Die Temperature** — chip die temperature, whole degrees (silicon, not room)
   - **Thermal Performance** — `normal` / `throttled` / `paused` (the adaptive-performance ladder)
   - **Thermal Advisory** — ON when any thermal advisory is active (too hot, saturation, sensor fault, cold)

On canary-wap builds with the microphone enabled (XIAO ESP32-S3 Sense), the
device also announces acoustic entities via MQTT discovery:

   - **Smoke Alarm Heard** / **CO Alarm Heard** — binary sensors that latch ON
     when the mic matches an NFPA 72 T3 smoke or UL 2034 CO cadence, and clear
     about 30 seconds after the alarm stops. State rides the retained
     `securacv/<device_id>/sensing` topic (`acoustic_event` field plus
     detection counters and `mic_muted`).
   - **Knock / Doorbell / Glass Break Detected** — additional binary sensors on
     FULL builds with the transient detectors compiled in.
   - **Microphone Mute** — a switch entity for the hard mute. State is retained
     on `securacv/<device_id>/mic/state` (`muted`/`live`); commands go to
     `securacv/<device_id>/mic/cmd` (`mute`/`unmute`, or HA's default `ON` =
     muted / `OFF` = live). Every toggle — including ones from HA — is signed
     into the device's witness chain with its source, so an investigator can
     later verify when the mic was off and who turned it off.

Canary Sentinel (the multi-sensor fusion guardian) is **not released yet** —
its Phase 1a firmware is compiled by CI but has not run on a bench
([project README](../firmware/projects/canary-sentinel/README.md)). When a
unit is built by hand, its own discovery announces **Presence** and
**Anomaly** binary sensors, a **Channel blinded** problem sensor, **Level**,
**Confidence**, **Anomaly score**, **Occupancy**, **Range band** and
**Corroborating modalities** sensors, the uptime / RSSI / free-heap
diagnostics and the firmware update entity — all from the coarse fused
claim, never a raw measurement. Its events are signed over their own
`sentinel` canonical, which the integration verifies like every other signed
kind; its **Last Event** reads `level_changed`, and its sensing modality
shows as "Other sensor" because it fuses several media at once.

### Step 4b: Add the verified-✓ timeline card

For the "single pane of glass" view, add the bundled Lovelace card: edit a
dashboard → **Add Card** → search **"SecuraCV Verified Timeline"**. It renders a
verified-✓ event timeline with a hash-chain status header from the entities
above, and auto-discovers them. Full options and a no-custom-card YAML fallback:
[`docs/lovelace_timeline.md`](lovelace_timeline.md).

### Step 4c: Ready-made dashboards

Three ready-made dashboards ship in
[`homeassistant/lovelace/`](../homeassistant/lovelace/) — the kernel pipeline
view, the Canary fleet view, and the Vision view. The one-command installer
copies them to `/config/securacv/dashboards/` and, when your
`configuration.yaml` has no `lovelace:` key of its own, registers a YAML
dashboard in the sidebar (a clearly marked block, validated with
`ha core check` and rolled back if validation fails). If you already manage a
`lovelace:` key, the installer leaves it untouched and tells you so — the
files are still placed, and registering them is the snippet below.

To register one by hand: Home Assistant has no "import a dashboard from a
file" button in the UI, so a YAML dashboard is declared in
`configuration.yaml`:

```yaml
lovelace:
  mode: storage            # keeps your existing UI-editable dashboards
  dashboards:
    securacv-canary:
      mode: yaml
      title: SecuraCV
      icon: mdi:shield-check
      show_in_sidebar: true
      filename: securacv/dashboards/securacv-canary-dashboard.yaml
```

…then restart Home Assistant. Or skip registration entirely: open any
dashboard, enter edit mode, **Add card → Manual**, and paste the cards you
want out of the YAML files.

### Step 5: Set Up Notifications

The one-command installer pre-installs all the SecuraCV blueprints to
`/config/blueprints/automation/securacv/`, where Home Assistant loads them
automatically — if you ran it, skip straight to creating an automation
(**Settings > Automations > Blueprints**, pick a SecuraCV blueprint). It also
creates the daily-digest automation for you when a mobile-app notify service
exists.

To import the Alert Blueprint by hand instead, use the one-click badge —
[import `securacv_alerts.yaml`](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_alerts.yaml)
— or the URL flow:

1. Go to **Settings > Automations > Blueprints > Import Blueprint**
2. Enter URL: `https://github.com/kmay89/securaCV/blob/main/docs/blueprints/securacv_alerts.yaml`
3. Create an automation from the blueprint
4. Pick the sensors you want monitored (each alert type is optional — leave
   an input empty to skip it): smoke/CO alarm heard (critical pushes that
   bypass silent mode), tamper, chain failure, offline, GPS loss
5. Enter your notification service (e.g. `notify.mobile_app_your_phone`)

Or copy automations from `docs/homeassistant_automations.yaml` for manual setup.

**Have a Philips Hue (or any color) bulb?** The Alert Light blueprint is
pre-installed by the one-command installer too; to import it by hand,
[use the badge](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_hue_alert_light.yaml)
or the URL
`https://github.com/kmay89/securaCV/blob/main/docs/blueprints/securacv_hue_alert_light.yaml`.
Pick your Canary's sensors and a bulb, and the light becomes a silent
beacon: it pulses and holds red on tamper / smoke or CO heard / chain
failure, blinks green and restores itself when the sensors clear, and gives
three gentle amber pulses if the device goes offline. Pair the bulb first
via HA's built-in Hue integration (**Settings > Devices & Services > Add
Integration > Philips Hue**, press the bridge button).

**Have a Busy Bar?** The busy.app desk light works as an alert beacon too —
red alert word held until the sensors clear, amber note when a device goes
offline, all over your LAN. It needs a small `rest_command` package first
(the bar's address lives there, not in the blueprint), so follow
[`docs/integrations/busy-bar.md`](integrations/busy-bar.md), then import
[`securacv_busybar_alert.yaml`](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_busybar_alert.yaml)
the same way as the others.

**Radar (canary-sense / MR60BHA2) witnesses** get three extra blueprints —
after-hours presence (with optional two-physics corroboration),
lights-out-with-presence tamper, and a non-diagnostic welfare check — plus a
stock-card **wellbeing tile**. See
[`docs/blueprints/canary_sense_wellbeing.md`](blueprints/canary_sense_wellbeing.md).

### Actions

The integration registers three Home Assistant actions, all of them about
[watches](design/watches.md): bounded attention that ends by itself. Try
them from **Developer tools → Actions**, or call them from an automation or
a script.

| Action | Takes | Returns |
|---|---|---|
| `securacv.start_watch` | `subject` (required, in words: "the gate canary"), `duration` ("two weeks", in days, weeks, months, seasons or years; 14 days if left out, never more than a year, and refused rather than guessed when it names no unit, such as "48 hours"), `concern` (`stopped`, `unusual`, `more`, `less` or `every`; read off the subject's wording if left out) | the watch, including the `id` that `end_watch` takes |
| `securacv.end_watch` | `watch`: its id, or its label ("the gate canary") | the watch as it ended |
| `securacv.list_watches` | nothing (response only) | `watches`: each running watch with its `state`, `subject` and `days_left` |

```yaml
# automations.yaml — watch the gate while you're away; say if it goes quiet
- alias: "Away: watch the gate Canary"
  triggers:
    - trigger: state
      entity_id: input_boolean.away
      to: "on"
  actions:
    - action: securacv.start_watch
      data:
        subject: "the gate canary"
        duration: "10 days"
        concern: stopped
```

The example is written in the automation syntax of Home Assistant 2024.10
and newer. The integration supports 2024.4.1 and newer, and on a release
before 2024.10 the same automation is written with `trigger:` and
`- platform: state`, then `action:` and `- service: securacv.start_watch`.
The action and its fields don't change.

What to expect:

- A watch started this way is the same object as one you spoke: the same
  cap on how many run at once, kept across a clean restart (a crash or
  power cut can lose the last few seconds of changes), delivered as a
  persistent notification, and it ends by itself and says so.
- A subject that no Canary's name matches is accepted but cannot fire until
  something reports it. The returned watch says `subject.kind: unbound`, and
  the log warns.
- `end_watch` refuses a label that two watches share (end it by id) and a
  name it doesn't know. It never guesses. An early end is announced the
  same way an expiry is, with what the watch learned.
- Until the integration has loaded, all three refuse with a message rather
  than answering from an empty list. The same goes if the stored watches
  could not be read. The file is left as it is, nothing is written over
  it, and reloading the integration tries again.
- There are no actions for pinning, rotating or unpinning a device key.
  Those stay in the options flow on purpose
  ([why](device_trust.md#why-pin-rotate-and-unpin-are-not-actions)).

### Step 6: Verify per-device PKI (optional but recommended)

Each Canary that witnesses signs its `chain`, `events`, and `counts`
MQTT publishes with an on-device Ed25519 key (a canary-display signs
nothing). HA auto-pins the public key the first
time a device's health publish appears (TOFU), then verifies every
subsequent publish.

To check that verification is live: open the chain-length sensor's
attributes — you should see `verified: true`, `trust_reason: ok`, and
matching `pinned_fingerprint` / `received_fingerprint` values. If your
threat model needs stricter trust than TOFU, pin the device's pubkey
manually from **Settings → Devices & services → SecuraCV → Configure →
Pin a device pubkey**. The form takes the `device_id` and the full
64-character pubkey hex, read off the device rather than over MQTT, and
where that is depends on the product:

- **canary-wap**: its `/enroll` page on the local network
  (`canary-<name>.local/enroll`, or its IP).
- **The `firmware/canary` build** and **canary-vision**: the USB serial
  console, where `j` prints the self-manifest with `device_id` and
  `pubkey` (the `firmware/canary` build's `i` prints the key too).
- **canary-sense** and **canary-sentinel**: only the fingerprint, in the
  boot log (`Ed25519 ready  fp=…`). Neither shows its full key off the
  network, so you can't pin one by hand. Compare that fingerprint with
  the `pinned_fingerprint` attribute instead.
- **The canary-display line**: nothing to pin. A display has no signing
  key.

Addresses, caveats and the apps' part:
[where each product shows its key](device_trust.md#where-each-product-shows-its-key).

Full background, threat model, and rotation procedure: see
[`docs/device_trust.md`](device_trust.md).

### Troubleshooting: MQTT Setup

| Symptom | Check |
|---------|-------|
| Device not appearing in HA | Verify Canary is connected to home WiFi (check serial output or WAP dashboard) |
| MQTT not connecting | Verify broker host/port in Canary config matches your Mosquitto setup |
| Sensors show "unavailable" | Check MQTT broker logs for connection attempts from `securacv-canary-*` client IDs |
| Discovery not working | Ensure HA MQTT integration is configured and `securacv/#` topics are not blocked |
| Tamper alerts not firing | Verify the `tamper` binary sensor entity exists and automations are enabled |
| Two "SecuraCV Canary" devices / entities ending in `_2` | Known overlap: the firmware announces core entities via native MQTT discovery *and* the integration creates its own (with PKI verification attributes). Both work; disable the ones you don't use from the entity settings, and prefer the integration's set if you use signature verification |

### MQTT Topic Reference

| Topic | Direction | Content |
|-------|-----------|---------|
| `securacv/{device_id}/status` | Device → HA | Device state, GPS, chain sequence (every 30s) |
| `securacv/{device_id}/health` | Device → HA | System metrics (every 60s) |
| `securacv/{device_id}/events` | Device → HA | Witness record events |
| `securacv/{device_id}/chain` | Device → HA | Hash chain state |
| `securacv/{device_id}/tamper` | Device → HA | Tamper alerts (immediate) |
| `securacv/{device_id}/availability` | Device → HA | Online/offline (LWT, retained) |
| `securacv/{device_id}/update/state` | Device → HA | Firmware update entity state (retained) |
| `securacv/{device_id}/update/cmd` | HA → Device | `install` — start a firmware update |
| `homeassistant/*/securacv_*/config` | Device → HA | HA MQTT Discovery config (retained) |

### Transport catalog

The integration models multi-transport resilience (`custom_components/securacv/const.py`).
Status reflects what the code actually does today — **Implemented** means the transport
carries witness data end-to-end, **Experimental** means partially wired (one side shipped,
not verified end-to-end), **Planned** means the vocabulary is reserved and nothing emits it
yet.

> **Per-transport health entities do not appear yet.** The integration's
> `SecuraCVCanaryTransportSensor` is only created when a device publishes
> `securacv/{device_id}/transport` — and no current firmware calls its
> `mqtt_publish_transport()` helper, so the *health sensors* below are pending a firmware
> publisher for every row. The Status column describes the transport itself (does witness
> data actually flow over it), not the health entity.

| Transport | Description | Transport status | Health sensor |
|-----------|-------------|------------------|---------------|
| `wifi_sta` | WiFi station to your router (normal operation; carries all MQTT traffic) | Implemented | Pending firmware publisher |
| `wifi_ap` | Direct WiFi AP mode (device dashboard / setup) | Implemented | Pending firmware publisher |
| `mqtt` | MQTT broker connection — the primary witness-data path | Implemented | Pending firmware publisher |
| `ble` | Bluetooth Low Energy (device-side scanning, chirp alerts) | Experimental | Pending firmware publisher |
| `mesh` | Opera mesh network, peer-to-peer ([spec v0](../spec/canary_mesh_network_v0.md)) | Experimental | Pending firmware publisher |
| `chirp` | Community alert network ([spec v0](../spec/chirp_channel_v0.md)) | Experimental | Pending firmware publisher |
| `lora` | LoRa radio (see [Meshtastic notes](meshtastic_integration.md)) | Planned | — |
| `audio` | SCQCS audio squawks | Planned | — |

### Per-tamper-type sensor catalog

Each tamper type below gets its own HA binary sensor (`binary_sensor.py`). Status is based
on whether current firmware actually emits the corresponding signal, not on the sensor
existing: the integration listens for several signals no firmware publishes yet.

| Tamper type | HA sensor | Firmware signal today | Status |
|-------------|-----------|----------------------|--------|
| `sd_remove` | SD Removed | both publish `{"type":"sd_remove"}` on the tamper topic when their `system.integrity` module sees a mounted card leave (the per-kind bridge: `csi_mqtt.cpp` on the WAP, `csi_event_egress.cpp` on the canary base); both also publish `sd_mounted` in their MQTT health once a card has mounted this boot (a card-less boot sends no key, so it never reads as removed), so the sensor stays on until the card is back | Implemented (canary base + WAP) |
| `sd_error` | SD Error | both publish `{"type":"sd_error"}` on the tamper topic when their `system.integrity` module sees a mounted card fail; the canary base also publishes `sd_errors` in health | Implemented (canary base + WAP) |
| `memory_critical` | Memory Critical | derived HA-side from published `free_heap` | Implemented |
| `enclosure` | Enclosure Open | a reed/hall enclosure contact on the board map's `TAMPER_PIN_DEFAULT`, on builds with `FEATURE_TAMPER_GPIO=1` (off in every shipped profile until the pin is bench-validated): both publish `{"type":"enclosure"}` on the tamper topic when their `system.integrity` module commits an opening, and both also publish `enclosure_open` in health, so the sensor stays on while the lid is off. The canary base's capacitive-touch tamper is published on the tamper topic with `"kind":"enclosure_tamper","type":"enclosure"` | Experimental |
| `power_loss` | Power Loss | canary base publishes `{"type":"power_loss"}` on the tamper topic at boot (power-events classifier, `canary_power_events.h`); Canary WAP publishes the same shape on the tamper topic when its `system.integrity` module commits a brownout-boot tamper (`csi_mqtt.cpp`'s per-kind bridge) | Implemented (canary base + WAP) |
| `gps_jamming` | GPS Jamming | none found | Experimental |
| `motion` | Unexpected Motion | none found (accelerometer signal not published) | Experimental |
| `gpio` | GPIO Tamper | none by design: a tamper pin's contact is narrated as `enclosure` (one kind per physical fact) | Experimental |
| `watchdog` | Watchdog Timeout | Canary WAP publishes `{"type":"watchdog"}` on the tamper topic when its `system.integrity` module classifies a watchdog reset at boot | Implemented (WAP) |
| `unexpected_reboot` | Unexpected Reboot | canary base publishes `{"type":"unexpected_reboot"}` on the tamper topic at boot after a fault reset (power-events path); Canary WAP publishes the same shape from its `system.integrity` module after a panic reset | Implemented (canary base + WAP) |
| `battery_remove` | — (no sensor) | none | Planned |
| `gps_spoof` | — (no sensor) | none | Planned |
| `capacitive` | — (no sensor; folded into `enclosure` on-device) | touch pad tamper | Planned |
| `audio_anomaly` | — (no sensor) | none | Planned |

Each per-type sensor is set by the tamper topic and then follows the health
payload's level for its kind on every health publish; an absent key reads as
"clear". So a kind whose level the health does not carry lights its sensor
only until the next health publish (once a minute on the WAP, stretched in
its power-saving modes). Today that is
the WAP's `sd_error`, `watchdog`, `power_loss` and `unexpected_reboot`; the
canary base carries `sd_errors`, and holds `power_loss_detected` /
`unexpected_reboot` in health for a while after boot, and its watchdog resets
reach HA as `unexpected_reboot`.

---

## Firmware Updates from Home Assistant

Canaries with the signed pull-OTA firmware expose a native **Firmware**
update entity (plus an **Auto Update** switch) via MQTT Discovery — no
extra setup needed.

- When a new release is published, the device's update entity shows
  "Update available" with the release notes. Press **Install**: the device
  downloads the update over HTTPS, verifies its SHA-256 and Ed25519
  release signature, installs it to the inactive partition, restarts, and
  confirms itself healthy. A live progress bar tracks the whole cycle.
- If the update fails for any reason, the device restores its previous
  firmware automatically and reports what happened — it cannot be bricked
  by a bad update, and a forged image can never pass the signature check.
- Turn on the **Auto Update** switch (per device) to install new releases
  hands-free within a day of publication. It's off by default — a witness
  device never restarts unattended unless you choose that.
- Every update is signed into the device's witness chain
  (`fw_update_started` / `fw_update_applied` / `fw_update_rolled_back`),
  so the audit trail proves when firmware changed.

See `docs/firmware_ota.md` for the release pipeline, local/air-gapped
hosting, and the full security model.

---

## Alternative Setup Modes

The integration's config flow offers four choices. **Automatic** is the
default and the right answer for almost everyone — the manual modes exist for
unusual layouts (a kernel on another host, a re-bridged topic prefix).

| Mode | Best For | How It Works |
|------|----------|--------------|
| **Automatic** (default) | Everyone | Probes for the kernel and configures kernel + MQTT, or MQTT-only, to match what's installed — zero typing |
| **MQTT only** | Canary device users | Auto-discover devices via MQTT |
| **Kernel only** | PWK API users | Poll the Witness Kernel HTTP API |
| **Both** | Advanced users | MQTT for Canary + HTTP for the kernel |

---

> **Most people are done here.** The by-hand kernel sections below are
> collapsed — each repeats a step the one-command install (or the app +
> wizard) already did for you, kept for unusual layouts and for anyone
> untangling an older install; click one to expand it. The reference
> material stays open and applies to every install: the configuration
> reference, go2rtc, MQTT discovery, the
> [API reference](#api-reference), [privacy features](#privacy-features),
> and [troubleshooting](#troubleshooting).

<details>
<summary><strong>Legacy: Witness Kernel Setup</strong> — picking the kernel's frigate/standalone mode by hand, with the Frigate + MQTT checklist; the installer and the kernel's own wizard make this choice for you</summary>

## Legacy: Witness Kernel Setup

### Choose Your Mode

| Mode | Best For | How It Works |
|------|----------|--------------|
| **frigate** | Users with Frigate NVR | Subscribe to Frigate's MQTT events |
| **standalone** | Users without Frigate | Process RTSP streams directly |

### Frigate Mode (Recommended if you use Frigate)

```
Cameras → Frigate (detection) → MQTT → PWK (privacy logging)
```

- Uses Frigate's superior ML detection (Coral TPU, TensorFlow)
- PWK receives event notifications, not video
- Best accuracy, minimal resource usage
- The app always starts the Event API service in Frigate mode, even if MQTT publishing is disabled

#### Frigate + MQTT Configuration Checklist

- [ ] **Mode set to frigate**: `mode: "frigate"` is configured in the app options.
- [ ] **Frigate MQTT broker details match**: `frigate.mqtt_host`, `frigate.mqtt_port`, `frigate.mqtt_username`, and `frigate.mqtt_password` match the broker that Frigate uses.
- [ ] **Frigate event topic is correct**: `frigate.mqtt_topic` matches Frigate’s configured event topic (default `frigate/events`).
- [ ] **Home Assistant MQTT publish settings are aligned**: if you enable `mqtt_publish.enabled`, ensure `mqtt_publish.host`, `mqtt_publish.port`, `mqtt_publish.username`, and `mqtt_publish.password` match the same broker.
- [ ] **Topic + discovery prefixes are consistent**: `mqtt_publish.topic_prefix` is the prefix you expect for PWK events, and `mqtt_publish.discovery_prefix` matches Home Assistant’s discovery prefix (default `homeassistant`).
- [ ] **App options from the Configuration tab are configured**: `mode` is still `frigate`, a `device_key_seed` is present (the app auto-generates one on first start if you left it empty), and any Frigate-specific options (`frigate.cameras`, `frigate.labels`, `frigate.min_confidence`) are configured as needed.
- [ ] **Broker transport understood**: the bridges speak MQTT 3.1.1 over TCP, plain by default. In the app they connect to the broker set in its options or, left empty, the one the Supervisor hands them (by default `core-mosquitto` on `1883`, over Home Assistant's internal network); the app has no TLS option, and `--with broker_tls` adds the TLS listener the Canaries use (Step 3 of the manual walkthrough) while leaving that internal `1883` in place. A bridge run outside the app takes `MQTT_USE_TLS` / `MQTT_TLS_CA_PATH` for a TLS broker (the [TLS Settings](#tls-settings-optional) table below). MQTT v5 is not spoken by either bridge.

### Standalone Mode

```
Cameras → go2rtc → PWK (detection + logging)
```

- PWK processes camera streams directly
- Simpler setup without Frigate
- Built-in motion detection

</details>

---

<details>
<summary><strong>Kernel Setup by Hand (HACS + Kernel)</strong> — the same install the one-command installer runs, step by step: HACS, the kernel app, its wizard, the integration</summary>

## Kernel Setup by Hand (HACS + Kernel)

> The [one-command installer](#quick-start-one-command) at the top of this
> page does all of this unattended. This section is the same setup done
> manually.

### Step 1: Install the HACS Integration

1. Open **HACS → Integrations**
2. Click **⋮ → Custom repositories**
3. Add `https://github.com/kmay89/securacv-homeassistant` (the integration's
   distribution repository) as an **Integration**
4. Install **SecuraCV** and restart Home Assistant when prompted

### Step 2: Install the Kernel

Choose one runtime option:

**Option A: Home Assistant app (custom repository)**
1. Go to **Settings → Apps → App Store**
2. Click **⋮ → Repositories** → Add: `https://github.com/kmay89/securaCV`
3. Install **Privacy Witness Kernel**

**Option B: Docker / another host**
1. Run the kernel using your preferred deployment method
2. Ensure the Event API is reachable from Home Assistant

### Step 3: Start the Kernel and Run Its Wizard

Click **Start** (or start your container). There is no key ceremony: the
kernel generates its Ed25519 device key on first start and persists it —
nothing needs to exist before it will run.

Then open the app's **Web UI** (ingress). The setup wizard confirms the mode
(**frigate** when Frigate is present, **standalone** otherwise), discovers
the Mosquitto broker, and checks that Mosquitto and Frigate are installed —
it can install either one for you if they are missing. Nothing to type.

Back up the device key once the kernel is running: if it is lost, old event
signatures can no longer be verified.

#### Advanced: manual configuration (optional)

Skip the wizard only if you need a specific identity or a hand-tuned config —
for instance, restoring a kernel from backup with its known key. Set the app
options yourself before first start (`openssl rand -hex 32` produces a valid
seed):

**For Frigate users:**
```yaml
mode: "frigate"
device_key_seed: "your-64-char-key"  # optional — auto-generated if omitted
frigate:
  mqtt_host: "core-mosquitto"
  min_confidence: 0.5
mqtt_publish:
  enabled: true  # Optional: HA MQTT discovery
```

**For standalone users:**
```yaml
mode: "standalone"
device_key_seed: "your-64-char-key"  # optional — auto-generated if omitted
go2rtc_discovery: true
mqtt_publish:
  enabled: true  # Optional: HA MQTT discovery
```

### Step 4: Add the Integration

1. Go to **Settings → Devices & Services**
2. Click **Add Integration**, select **SecuraCV**, and keep the default
   **Automatic — detect what's installed**: it probes the kernel's `/health`
   at `http://d0491a67-privacy-witness-kernel:8799` and the token file at
   `/config/api_token`, then creates the entry — kernel + MQTT when the
   kernel answers, MQTT-only otherwise.
3. Choosing a kernel mode manually instead? Provide the Event API URL
   (`http://d0491a67-privacy-witness-kernel:8799` when the kernel runs as
   the app) and authentication:
   - **API token file (recommended):** the kernel rotates its capability token
     every 10 minutes and rewrites the token file. When the kernel runs as the
     app, that file is `/config/api_token` (the default), which Home
     Assistant can read directly — the integration re-reads it automatically
     whenever the token rotates.
   - **Static token:** only for kernels on another host whose token file Home
     Assistant cannot read. A pasted token goes stale at the next rotation
     (≤10 minutes), so for remote kernels either share the token file (e.g. a
     mounted volume) or expect to re-authenticate.

</details>

---

## Distribution

- **HACS integration (current):** install the SecuraCV integration in Home Assistant.
- **Kernel runtime:** run the Privacy Witness Kernel as an app, container, or service. The integration connects to its Event API.

---

<details>
<summary><strong>Kernel Installation (Optional App)</strong> — the app repository by hand (the one-command installer already does this) or a local container build</summary>

## Kernel Installation (Optional App)

### Option 1: App Repository

1. In Home Assistant, go to **Settings → Apps → App Store**
2. Click the menu (⋮) → **Repositories**
3. Add: `https://github.com/kmay89/securaCV`
4. Find "Privacy Witness Kernel" and click **Install**

### Option 2: Local Installation

```bash
# Clone the repository
git clone https://github.com/kmay89/securaCV.git
cd securaCV

# Build the app container (from the repo root)
docker build -f privacy_witness_kernel/Dockerfile -t privacy-witness-kernel .

# Copy to the HA /addons folder (the path keeps the historical name)
cp -r privacy_witness_kernel /addons/privacy_witness_kernel
```

</details>

---

## Configuration

### Step 1: The Device Key

The device key is a secret that establishes your device's cryptographic
identity. You don't need to create one — the app generates a key on first
start and persists it. Supply your own seed only when you want a specific
identity (for instance, restoring from backup):

```bash
openssl rand -hex 32
```

Either way, back the key up securely. If you lose it, you cannot verify old
event signatures.

### Step 2: Configure the App

The setup wizard (the app's Web UI) writes this configuration for you. To do
it by hand instead, in the app configuration panel:

```yaml
# Optional: pin your device key (auto-generated on first start if omitted)
device_key_seed: "your-64-character-hex-key-here"

# Camera discovery from go2rtc (recommended)
go2rtc_discovery: true
go2rtc_url: "http://homeassistant.local:1984"

# Or manual camera configuration
cameras:
  - name: front_door
    url: rtsp://admin:password@192.168.1.100:554/stream1
    zone_id: zone:front_door
    fps: 10
    width: 640
    height: 480

  - name: driveway
    url: rtsp://admin:password@192.168.1.101:554/stream1
    zone_id: zone:driveway

# How long to keep events (days)
retention_days: 7

# Time bucket size (minutes) - events are grouped into buckets
# Larger = more privacy, smaller = finer granularity
time_bucket_minutes: 10

# Logging verbosity
log_level: info
```

### Step 3: Start the App

Click **Start**. Check the logs for any errors.

---

## go2rtc Integration

[go2rtc](https://github.com/AlexxIT/go2rtc) is the recommended way to connect cameras in Home Assistant. The Privacy Witness Kernel can auto-discover cameras from go2rtc.

### If You Already Use go2rtc

1. Enable discovery in the app config:
   ```yaml
   go2rtc_discovery: true
   go2rtc_url: "http://homeassistant.local:1984"
   ```

2. The app will automatically find your cameras

### If You Don't Use go2rtc

Either:
1. Install the [go2rtc app](https://github.com/AlexxIT/go2rtc) (recommended)
2. Or configure cameras manually in the PWK app

### Frigate Users

If you use Frigate, it includes go2rtc. Point the discovery URL to Frigate:
```yaml
go2rtc_url: "http://frigate:1984"
```

---

## MQTT Discovery (Automatic Sensors)

PWK supports **Home Assistant MQTT Discovery**, which automatically creates sensors without any manual configuration.

### Enable MQTT Publishing

```yaml
mqtt_publish:
  enabled: true
  host: "core-mosquitto"    # HA's built-in MQTT broker
  port: 1883
  username: ""              # Optional: MQTT auth
  password: ""
  topic_prefix: "witness"   # State topics: witness/zone/*/event
  discovery_prefix: "homeassistant"  # HA discovery prefix
```

#### TLS Settings (Optional)

If your broker requires TLS, enable it via the bridge environment (or CLI flags):

| Environment Variable | Description |
|---|---|
| `MQTT_USE_TLS` | Enable TLS (required for `mqtts://` brokers) |
| `MQTT_TLS_CA_PATH` | Path to PEM-encoded CA certificate |
| `MQTT_TLS_CLIENT_CERT_PATH` | Path to PEM-encoded client certificate (mutual TLS) |
| `MQTT_TLS_CLIENT_KEY_PATH` | Path to PEM-encoded client private key (mutual TLS) |

### Auto-Created Entities

When enabled, PWK automatically creates these entities for each zone:

| Entity | Type | Description |
|--------|------|-------------|
| `sensor.pwk_<zone>_events` | Sensor | Total event count (state_class: total_increasing) |
| `binary_sensor.pwk_<zone>_motion` | Binary Sensor | Motion detected (auto-off after 10 min) |
| `sensor.pwk_last_event` | Sensor | Most recent event with full attributes |

### Entity Attributes

The `sensor.pwk_last_event` entity includes these attributes:
Time is reported as coarse buckets (typically 10 minutes), not precise timestamps.

```yaml
event_type: "BoundaryCrossingObjectLarge"
zone_id: "zone:front_door"
time_bucket_start: 1706140800
time_bucket_size: 600
confidence: 0.85
published_bucket_start: 1706141400
published_bucket_size: 600
```

### Availability Tracking

PWK publishes to `witness/status` with Last Will Testament (LWT):
- **Online**: PWK is running and publishing events
- **Offline**: PWK has disconnected (set automatically by MQTT broker)

All entities use this availability topic, so they show "unavailable" when PWK is offline.

### MQTT Topics Published

| Topic | Payload | Retained |
|-------|---------|----------|
| `witness/status` | `online` / `offline` | Yes |
| `witness/last_event` | JSON event details | Yes |
| `witness/zone/<name>/count` | Event count integer | Yes |
| `witness/zone/<name>/motion` | `ON` | No |
| `witness/zone/<name>/event` | Full event JSON | No |
| `witness/events` | All events (firehose) | No |

### Example Automation with MQTT Sensors

```yaml
# automations.yaml
automation:
  - alias: "Notify on Front Door Motion"
    trigger:
      - platform: state
        entity_id: binary_sensor.pwk_front_door_motion
        to: "on"
    action:
      - service: notify.mobile_app
        data:
          title: "Motion Detected"
          message: >
            Motion at front door.
            Confidence: {{ state_attr('sensor.pwk_last_event', 'confidence') }}
```

### Witness Wall: the fleet roll-call

The kernel's Event API also answers `GET /api/fleet`, the roll-call the
tvOS Witness Wall reads: the kernel's own row first, then every Canary the
app's MQTT publisher has heard on the broker. The app wires this itself —
its `run.sh` points the kernel and `event_mqtt_bridge` at one file,
`/config/fleet_peers.json` — so there is no option to set. What the Wall
still needs from you, and why:

- **Keep MQTT publishing enabled** (`mqtt_publish.enabled: true` above, the
  default). The publisher is the process that listens for Canaries; with it
  off, the roll-call lists the kernel alone — the kernel is not pointed at
  the file, so rows a past bridge pinned are kept for their keys, not served
  — and the app log says so at startup.
- **Open the port and type the address once.** The app advertises no
  `_securacv._tcp` Bonjour service, and its 8799 host port ships disabled
  (see [Event API exposure](../privacy_witness_kernel/README.md#event-api-exposure)),
  so the Wall cannot discover it. Enable the port under **Settings → Apps →
  Privacy Witness Kernel → Configuration** (Network section), then enter
  `http://<your-home-assistant-host>:8799` in the Wall — with the port,
  because the Wall adds only `http://` to a bare host and would otherwise
  poll port 80; a typed hub is remembered and never aged out. Know what the
  open port means: `/api/fleet` is the one data endpoint that answers with
  no token at all (`/health` answers only `{"status":"ok"}`), so once the
  host port is enabled anything on your LAN can read the roll-call — name,
  online, chain verdict, product, and the per-room
  presence/occupants/breathing words while a peer is proven online (the
  posture [`docs/security/THREAT_MODEL.md`](security/THREAT_MODEL.md) states
  for the one open read on the hub). Every other endpoint still requires the
  capability token — or, on `GET /api/sealed-log` alone, a
  [viewer token](#viewer-tokens-witness-wall).
- **The summary file is in your backups, on purpose.** `/config` is part of
  every Home Assistant backup, and `/config/fleet_peers.json` holds the
  public key pinned on first sight for each Canary plus the per-room
  wellbeing words the Wall shows — a restore therefore keeps that trust
  instead of re-pinning every device. The file is written `0600`, and
  `/api/fleet` serves the roll-call's coarse words only, never the keys.

`online` on that roll-call is not a liveness proof: it means a signed chain
publish verified against the pinned key within the last 180 s, no more —
the exact wording is in
[`tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md).

---

## Manual Sensors (Alternative)

If you prefer not to use MQTT Discovery, you can create sensors manually using the REST API.

The app exposes an Event API on port 8799. When Home Assistant is running
alongside the app, use the app hostname so HA can reach it over the
Supervisor network. The Supervisor derives that hostname from the full app
slug — repository hash plus app name, underscores becoming dashes — which for
this repository is `d0491a67_privacy_witness_kernel`, giving
`http://d0491a67-privacy-witness-kernel:8799`. You can confirm the hostname
in **Settings → Apps → Privacy Witness Kernel → Info**, where Home Assistant
lists the app hostname/slug.

### REST Sensor (Basic)

```yaml
# configuration.yaml
sensor:
  - platform: rest
    name: "PWK Last Event"
    resource: http://d0491a67-privacy-witness-kernel:8799/events/latest
    headers:
      Authorization: Bearer YOUR_API_TOKEN
    value_template: "{{ value_json.event_type }}"
    json_attributes:
      - zone_id
      - time_bucket
      - confidence
      - kernel_version
      - ruleset_id
    scan_interval: 30
```

### Template Sensor (Event Count)

```yaml
# configuration.yaml
template:
  - sensor:
      - name: "Boundary Crossings Today"
        state: "{{ states('sensor.pwk_boundary_crossing_count') | int }}"
        icon: mdi:walk
```

### Automation Example

```yaml
# automations.yaml
automation:
  - alias: "Notify on Large Object Detection"
    trigger:
      - platform: state
        entity_id: sensor.pwk_last_event
        to: "BoundaryCrossingObjectLarge"
    action:
      - service: notify.mobile_app
        data:
          title: "Motion Detected"
          message: "Large object detected in {{ state_attr('sensor.pwk_last_event', 'zone_id') }}"
```

---

## API Reference

The Event API is available at `http://d0491a67-privacy-witness-kernel:8799`
when running as a Home Assistant app (or your configured port), as this
hostname is automatically resolved by the Supervisor. If the kernel runs
elsewhere, replace the hostname with the reachable IP/DNS name for that host.

### Authentication

The API uses short-lived capability tokens as **Bearer** credentials. The token is written to `/config/api_token` when the app starts and rotates every 10 minutes; read it from the configured token file whenever you need to authenticate. If you run the kernel elsewhere, use the token path or secrets location configured for that deployment. The SecuraCV integration handles rotation automatically when configured with the token-file path (its default); scripts and other clients must re-read the file on every `401`.
`/health` and the fleet roll-call `GET /api/fleet` are the two routes that answer without a token. The kernel refuses to bind a non-loopback address without TLS or an explicit override and, when bound to loopback, answers only loopback peers (`403`); the app binds all interfaces inside its own container so Home Assistant core can reach it, and ships the 8799 host port disabled. Every other route takes the capability token — or, on `GET /api/sealed-log` alone, a [viewer token](#viewer-tokens-witness-wall). Query-string tokens are rejected—send the token only in the `Authorization: Bearer` header.

```bash
# Read the token
TOKEN=$(cat /config/api_token)

# Make authenticated requests (Bearer token only)
curl -H "Authorization: Bearer $TOKEN" http://d0491a67-privacy-witness-kernel:8799/events
```

### Viewer tokens (Witness Wall)

A television cannot re-read a token file every ten minutes, so the kernel
has a second, narrower credential for the tvOS Witness Wall: a **viewer
token**, minted once by the operator and honored on exactly one route,
`GET /api/sealed-log` (the non-queryable, size-capped, signed chain tail).
Presented anywhere else — another path, another method — it is an invalid
token that counts toward the per-address lockout, and `?token=` is refused
for it as for every token.

```bash
witness_api mint-viewer-token --label "living room tv" --base-url http://192.168.1.20:8799
# {"kernel":"witness-kernel","base_url":"http://192.168.1.20:8799","sealed_log_token":"<64 hex>","verifying_key":"<64 hex>","token_id":"<8 hex>"}
witness_api revoke-viewer-token <token_id>
```

The command runs with the same `WITNESS_CONFIG` and `DEVICE_KEY_SEED` as
the serving kernel. Its stdout is one JSON line, the pairing receipt, and
the only copy of the token that will ever exist: the kernel keeps its
sha256 in the viewer-token file — `api.viewer_token_path` /
`WITNESS_API_VIEWER_TOKEN_PATH`, defaulting to `viewer_tokens.json` beside
`api.token_path` — written `0600` and re-read on every request, so a mint
or a revoke takes effect without a restart. The receipt carries the
kernel's current verifying key, which the Wall pins at pairing: its
"Verified" then means Ed25519 signatures checked against that pinned key,
not whatever key the hub serves today. The Docker sidecar wraps both
commands (`docker compose exec securacv entrypoint.sh mint-viewer-token …`,
see [frigate_integration.md](frigate_integration.md)); the Home Assistant
add-on reads the file but has no control that mints one yet.

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/events` | GET | Export events as batched buckets |
| `/events/latest` | GET | Get the most recent event (single event JSON) |
| `/digest` | GET | Coarse activity digest for dashboards (bucketed counts, no per-event detail) |
| `/status` | GET | Daemon status snapshot (retention, verify state) |
| `/verify` | POST | Run sealed-log verification and return the `VerifyReport` |
| `/export/bundle` | GET | Receipted export bundle (events reshaped for disclosure; correlation tokens stripped) |
| `/api/sealed-log` | GET | Checkpoint-anchored sealed-log tail for read-only verifiers — stored bytes verbatim, size-capped, **no query parameters** (the log is non-queryable by design). The one route a [viewer token](#viewer-tokens-witness-wall) also opens |
| `/api/fleet` | GET, OPTIONS | The fleet roll-call the Witness Wall reads — coarse words only (name, online, chain verdict, product, presence/occupants/breathing while proven online); **no token**, an origin allow-list for browsers; see [Witness Wall: the fleet roll-call](#witness-wall-the-fleet-roll-call) |
| `/health` | GET | Check daemon health (unauthenticated) |

### `/events/latest` Response (Event)

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "time_bucket": {
    "start_epoch_s": 1706140800,
    "size_s": 600
  },
  "zone_id": "zone:front_door",
  "confidence": 0.85,
  "kernel_version": "0.4.2",
  "ruleset_id": "baseline"
}
```

### `/events` Response (Export Artifact)

```json
{
  "batches": [
    {
      "buckets": [
        {
          "time_bucket": {
            "start_epoch_s": 1706140800,
            "size_s": 600
          },
          "events": [
            {
              "event_type": "BoundaryCrossingObjectLarge",
              "time_bucket": {
                "start_epoch_s": 1706140800,
                "size_s": 600
              },
              "zone_id": "zone:front_door",
              "confidence": 0.85,
              "kernel_version": "0.4.2",
              "ruleset_id": "baseline"
            }
          ]
        }
      ]
    }
  ],
  "max_events_per_batch": 50,
  "jitter_s": 120,
  "jitter_step_s": 60
}
```

### `/health` Response

```json
{
  "status": "ok"
}
```

---

## Privacy Features

### What the App DOES:
- Detects motion/boundary crossing events
- Records coarse-grained event claims (zone + 10-minute bucket)
- Stores cryptographically signed event log locally
- Expires old events according to retention policy

### What the App DOES NOT:
- Export raw video frames
- Record faces, license plates, or identifying features
- Send data to any cloud service
- Allow bulk historical queries
- Create searchable recordings

### Break-Glass Access

In emergency situations (e.g., law enforcement request with warrant), raw frames can be accessed through a quorum-based break-glass process. This requires:
1. Multiple trustees to approve
2. A specific time window
3. Immutable audit logging

Operationally, the flow is:
1. Store the quorum policy (`break_glass policy set`)
2. Create a request (`break_glass request`) and share the request hash
3. Trustees sign approvals (`break_glass approve`)
4. Authorize and emit a token file (`break_glass authorize --output-token ...`)
5. Unseal with the token (`break_glass unseal`)

See [Break-Glass Documentation](../spec/break_glass.md) for details.

---

## Troubleshooting

### App Won't Start

1. Check logs: **Settings → Apps → Privacy Witness Kernel → Logs**
2. If you set `device_key_seed` yourself, verify it is valid (64 hex
   characters) — left empty, the app generates one on first start
3. Ensure cameras are reachable

### No Cameras Discovered

1. Verify go2rtc is running: `curl http://homeassistant.local:1984/api/streams`
2. Check go2rtc URL is correct
3. Try manual camera configuration

### Events Not Appearing

1. Check the camera is streaming (view in HA)
2. Verify zone_id format: `zone:[a-z0-9_-]{1,64}`
3. Check app logs for errors

### High Resource Usage

1. Reduce camera resolution (use sub-stream)
2. Lower FPS to 5
3. Increase time bucket size

---

## Architecture Notes

The Privacy Witness Kernel runs as a separate process (app) for isolation:

1. **No HA core access**: The app cannot access HA internals
2. **Local storage only**: All data stays on your HA instance
3. **Sandboxed modules**: Detection modules run in seccomp sandbox
4. **API isolation**: Only the Event API is exposed

This design means even if the app is compromised, it cannot:
- Access other HA apps
- Export raw video
- Modify HA configuration

---

## Updates

The app updates independently of Home Assistant:

1. Go to **Settings → Apps → Privacy Witness Kernel**
2. Click **Update** when available

Update notes are in the [CHANGELOG](../CHANGELOG.md).

---

## Getting Help

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
- [Home Assistant Community](https://community.home-assistant.io/)
- [Documentation](../README.md)
