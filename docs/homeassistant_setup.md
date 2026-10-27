# Home Assistant Integration Guide

SecuraCV is **the verified witness log for Frigate and Home Assistant**.
Frigate keeps the video; SecuraCV keeps the proof. Every person, car, dog or
cat event Frigate reports is sealed into a signed, hash-chained log as a
coarse claim — which camera, which ten-minute window, what kind of thing —
and never a face, a plate, a name or a frame. In Home Assistant you get the
chain's state, one witness event entity per camera, a daily digest,
notifications with proof attached, and a one-tap signed evidence export
that anyone can verify offline. Canary devices — the project's own small
witnesses — join the same integration over MQTT, each with its own signed
chain.

> **Prefer the guided version?** [`canary-local/homeassistant.html`](../canary-local/homeassistant.html)
> ("The Hub") walks this same setup interactively — a 3D Raspberry Pi build you
> can scrub apart, a bench terminal that replays every command below, and a
> working sketch of the dashboard you end up with. Its entity names, topics,
> and versions are generated from this doc and drift-checked in CI, so the two
> can't disagree.

## The three ways in

| You run | Install this | What you type | Read |
|---|---|---|---|
| **Home Assistant OS** | the **SecuraCV** app from the App Store; it carries the integration and installs it on your click | nothing | [Path 1](#path-1-home-assistant-os--the-securacv-app) |
| **Home Assistant with HACS** already | the integration from HACS (a custom repository), then the app or the sidecar | nothing (app) or one 6-digit code (sidecar) | [Path 2](#path-2-hacs-first) |
| **Home Assistant Container** beside a Docker Frigate | the Docker sidecar next to Frigate, the integration from HACS | one 6-digit code | [Path 3](#path-3-home-assistant-container-and-the-docker-sidecar) |

Every path needs **Home Assistant 2026.5.0 or newer** — the integration's
`hacs.json`, the app's `config.yaml` and every blueprint's `min_version`
name the same floor — an **MQTT broker** Home Assistant and Frigate share
(Mosquitto on Home Assistant OS), and **Frigate 0.14 or newer with MQTT
enabled**. Only Canary devices and no Frigate? The integration works
MQTT-only; start at the [Canary walkthrough](#manual-walkthrough-canary-via-mqtt).

## What you need

| Item | Notes |
|------|-------|
| Raspberry Pi 4 (4 GB+) or x86 PC | Pi 5 works great; ~3 cameras at 10 fps |
| Home Assistant 2026.5.0 or newer | Home Assistant OS for the app path; Container for the sidecar path |
| IP camera(s) with RTSP | Hikvision, Dahua, Reolink, Amcrest, Ubiquiti, etc. — Frigate reads them |
| HA Companion App (optional) | For push notifications to your phone; every blueprint takes a Phone input |
| HDMI touchscreen (optional) | e.g. 7" 1024x600 IPS with USB touch — the hub runs headless by default, but the HAOSKiosk app (the `display` self-setup extra) can show your dashboard right on it |

## Path 1: Home Assistant OS — the SecuraCV app

1. **Settings → Apps → App Store → ⋮ → Repositories**, add
   `https://github.com/kmay89/securaCV`
   ([one click](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV)),
   then install **SecuraCV** — "The verified witness log for Frigate —
   signed events, no footage, no identity". The store slug is still
   `privacy_witness_kernel`, so an older install upgrades in place;
   "Privacy Witness Kernel" survives as the kernel device's model name.
2. **Start** the app and open its **Web UI**. The setup wizard checks that
   Mosquitto is running and Frigate is installed, reads Frigate's cameras
   from `/addon_configs/ccab4aaf_frigate/config.yml` (so there is no RTSP
   screen), and takes the broker credentials from the Supervisor — nothing
   to type. Anything missing is a link into your own App Store: "Install it
   for me" is a my.home-assistant.io link on purpose, because the app holds
   no Supervisor role and cannot install, start or stop other apps. You
   choose how long to keep sealed events (7 days, 30 days, 1 year), what to
   seal (every tracked object, or only Frigate alerts) and whether you want
   the daily digest. The device signing key is generated once and kept at
   `/config/.securacv/device_key`, inside your Home Assistant backups —
   back it up.
3. The done screen's one primary button is **Install the integration and
   restart Home Assistant**. It copies the integration the app carries into
   `/config/custom_components/securacv`, marks the copy as the app's own,
   and restarts Home Assistant Core — the only time the app ever restarts
   Core, and only on that click
   ([what the carried copy does and never does](../privacy_witness_kernel/DOCS.md#the-integration-carried-in-this-app)).
   With the integration already installed from HACS the button reads **Open
   Settings → Devices & Services** instead; the app never touches a copy
   HACS or you installed.
4. After the restart, **Settings → Devices & Services** shows a **SecuraCV
   discovered** card. Press **Configure**. The form has no fields — *"The
   SecuraCV app announced its witness kernel. Submit to connect — nothing to
   type. Frigate events appear as sealed witness events within a minute of
   the next detection."* Submit.
5. A one-time notification appears, **SecuraCV: add the dashboard** —
   *"SecuraCV is connected. Add the dashboard: Settings → Dashboards → Add
   dashboard → SecuraCV. It builds itself from your cameras and the witness
   chain."* Do that ([the dashboard](#add-the-dashboard)).
6. Walk past a camera. The first event shows **Sealed ✓** within a minute of
   Frigate's detection; **Verified ✓** follows the integration's next chain
   check — [what the badges mean](#what-sealed--verified--and-failed--mean).

### The one-command installer (terminal)

The same result from the **Terminal & SSH** app (install it from
**Settings → Apps → App Store** if you haven't):

```bash
curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash
```

The installer narrates each step and does all of this through the
Supervisor API: registers both app repositories (Frigate's and this one);
installs and starts **Mosquitto** and mints a `canary` broker login for your
devices (the password is never printed — read it under **Settings → Apps →
Mosquitto broker → Configuration → Logins**); connects Home Assistant itself
to the broker; installs **Frigate** with a curated starting config at
`/addon_configs/ccab4aaf_frigate/config.yml`; installs the **SecuraCV** app
(slug `privacy_witness_kernel`) in frigate mode and starts it; installs the
integration, restarts Home Assistant Core and creates its config entry;
places the blueprints under `/config/blueprints/automation/securacv/` and
the dashboards under `/config/securacv/dashboards/`, registering the
strategy dashboard as a YAML dashboard when your `configuration.yaml`
allows it ([Step 4c](#step-4c-ready-made-dashboards)); and creates the
daily-digest automation when a mobile-app notify service exists. It is
idempotent: re-running it never repeats a finished step.

Two things stay yours, and the installer says so when it finishes:

1. **Point Frigate at your cameras.** Edit
   `/addon_configs/ccab4aaf_frigate/config.yml`, set your cameras' RTSP URLs,
   flip the example camera's `enabled: true`, and restart Frigate.
2. **Point your Canaries at the broker.** Host `homeassistant.local`, port
   `1883`, username `canary`, password from the Mosquitto Logins page above —
   Step 3 of the Canary walkthrough covers the device side. That link is
   plain MQTT: the installer leaves Mosquitto on its default `1883`
   listener, so the `canary` password crosses your LAN unencrypted unless
   you ask the plan for the broker's TLS listener (`--with broker_tls`,
   from a certificate you supply) and provision the Canaries for it
   (Step 3 says how).

## Path 2: HACS first

SecuraCV is a HACS **custom repository**; it is not in the default HACS
store yet.

1. HACS → **⋮ → Custom repositories** → add
   `https://github.com/kmay89/securacv-homeassistant` (the integration's
   distribution mirror) with type **Integration**
   ([one click](https://my.home-assistant.io/redirect/hacs_repository/?owner=kmay89&repository=securacv-homeassistant&category=integration)).
2. Search HACS for **SecuraCV**, install it, restart Home Assistant.
3. **Settings → Devices & Services → Add Integration → SecuraCV**. Keep the
   default **Automatic — detect what's installed** and Submit. Automatic:
   - probes the app at `http://d0491a67-privacy-witness-kernel:8799`
     through its token file `/config/api_token` and, when it answers,
     creates the kernel + MQTT entry keyed on the kernel's identity —
     nothing to type;
   - otherwise listens five seconds for a Docker sidecar's signed
     announcement on the broker (`witness/+/announce`) and, hearing one,
     continues to the pairing step of
     [Path 3](#path-3-home-assistant-container-and-the-docker-sidecar);
   - finding neither, it creates the MQTT-only entry for Canary devices,
     ends with *"No witness kernel is running yet. SecuraCV is set up and
     will connect by itself when one appears."* plus links to the app
     repository and the Docker quickstart — or, when Home Assistant has no
     MQTT integration at all, *"…and Home Assistant's MQTT integration is
     not set up; a sidecar can only be discovered once Home Assistant and
     the sidecar share a broker"*, because without one no announcement can
     ever be heard — and raises the Repairs issue
     **No SecuraCV witness kernel found** — dismissable, and cleared by
     itself once a kernel entry loads.

   Installing the app afterwards ([Path 1](#path-1-home-assistant-os--the-securacv-app))
   raises the discovered card, and the app's wizard sees the HACS copy and
   skips its own install step.

## Path 3: Home Assistant Container and the Docker sidecar

No Supervisor, no app: the sidecar runs the witness kernel in one container
beside Frigate and pairs with Home Assistant over your LAN. Home Assistant
must use the **same MQTT broker** as Frigate and the sidecar.

1. On the Docker host, fetch the quickstart and edit two lines — the broker
   Frigate publishes to, and its credentials:

   ```bash
   curl -fsSLO https://raw.githubusercontent.com/kmay89/securaCV/main/docker/sidecar/quickstart.compose.yml
   # edit FRIGATE_MQTT_HOST and MQTT_USERNAME / MQTT_PASSWORD, then
   docker compose -f quickstart.compose.yml up -d
   docker exec securacv doctor
   ```

   `doctor` runs the setup checks a fresh install passes — broker reachable,
   broker login accepted, the kernel's `/health`, the capability token
   present, the retained announcement published, the pairing state, the
   sealed log verifying. `docker exec securacv doctor --live` also waits
   30 s for a real Frigate event.
2. Issue the pairing code:

   ```
   $ docker exec securacv pair
   SecuraCV pairing code: 483921   (valid 10 minutes, 5 attempts)
   Kernel key fingerprint: ab12cd34   (Home Assistant shows the same fingerprint before it pairs)
   Kernel URL to confirm in Home Assistant: http://<this docker host>:8799
   In Home Assistant: Settings → Devices & Services → the discovered "SecuraCV sidecar" card → Configure → enter the code.
   ```

   The code is printed once, on that terminal only; the daemon's log never
   carries it.
3. In Home Assistant, **Settings → Devices & Services** shows the card
   under **Discovered** within a minute of the sidecar's start — *SecuraCV
   sidecar `pwk_…`*, or *SecuraCV sidecar at `<address>`* when the compose
   file sets `SECURACV_ANNOUNCE_HOST`. Press **Configure**: *"A SecuraCV
   sidecar announced its witness kernel `pwk_…` (kernel 0.8.0). Its key
   fingerprint is `ab12cd34` — the same eight characters `docker exec
   securacv pair` prints. Submit to pair; the next step asks for the 6-digit
   code and confirms the kernel URL."* Compare the fingerprint with the
   card `pair` printed, then Submit.
4. **Pair with the SecuraCV sidecar** has two fields. **Kernel URL** is
   prefilled — from `SECURACV_ANNOUNCE_HOST` when you set it in the compose
   file, otherwise from the host of Home Assistant's own MQTT broker — and
   is yours to correct if the sidecar runs elsewhere (`http://<docker
   host>:8799`). **Pairing code** takes the six digits. Submit.
   *"The code was not accepted. Run `docker exec securacv pair` again and
   enter the new code within 10 minutes."* means exactly that: a code is
   single-use, lives ten minutes and burns after five wrong tries, and the
   kernel answers every refusal identically.
5. What just happened: Home Assistant exchanged the code for a long-lived
   **paired token** (32 random bytes; the kernel keeps only its sha256 in
   `paired_tokens.json`), pinned the kernel's verifying key from that
   authenticated answer — never from the broadcast announcement — and
   claimed the kernel so the bridge's own `pwk_*` MQTT entities step
   aside. Revoke the token any time with `docker exec securacv unpair <id>`
   (`unpair` alone lists the ids), or by removing the integration entry,
   which revokes it for you. A kernel keeps at most **32** paired tokens:
   past that the pair form says so (*The kernel already holds its limit of
   32 paired tokens*), the code is **not** consumed, and `unpair` of a stale
   one lets the same code pair. Until TLS lands for the sidecar, that token
   crosses your LAN as plaintext HTTP, so keep port 8799 on your own
   network and never forward it from the internet
   ([the security model](security/SECURITY_MODEL.md#home-assistant-pairing-and-what-crosses-your-network)).
6. From here the product is the same as Path 1: the dashboard notification,
   **Sealed ✓** within a minute of a detection, **Verified ✓** after the next
   chain check.

The sidecar's full story — what the published port opens, every
environment variable, `doctor`, clean shutdown and the Witness Wall — is in
[the Frigate guide](frigate_integration.md#quick-start-docker-no-home-assistant).

## What Sealed ✓, Verified ✓ and Failed ⚠ mean

"Verified" is a reserved word in this project: *an Ed25519 signature checked
against a pinned key*, nothing looser. On a kernel event in Home Assistant it
means **the integration itself** did the checking:

| Badge | `verification` | `trust_reason` | Meaning |
|---|---|---|---|
| **Sealed ✓** (muted) | `sealed` | `kernel_sealed` | The kernel signed the event into its chain. Home Assistant has not yet walked the chain past it. |
| **Verified ✓** | `verified` | `ha_verified_chain` | The integration fetched the sealed log (`GET /api/sealed-log?from_seq=…`), recomputed every hash link and checked every Ed25519 signature against the key it pinned when you connected, and that walk covered this event's event type, zone and ten-minute window. `verified_bucket_start` is the walk's window. |
| **Failed ⚠** | `failed` | `ha_verify_failed` | The last walk rejected the chain. Every kernel row reads Failed until a walk passes again, `binary_sensor.securacv_chain_valid` turns off, and the **Chain verification** event entity fires `chain_verification_failed`. Export now: entries before the break still verify in the offline viewer. |

A **key change is never shown as Failed**. When the kernel serves a key
other than the pinned one — a restore to new hardware, a reinstalled app or
sidecar, or something else answering at that address — Settings → Repairs
gets **The witness kernel's key changed**; kernel rows stay Sealed ✓ with
`trust_reason: kernel_key_mismatch`, `binary_sensor.securacv_chain_valid`
reads unknown (not off) with `key_mismatch: true`, the dashboard header and
the daily digest read *Witness key changed — confirm it in Settings →
Repairs*, and the fix flow shows the pinned and the
received fingerprints and asks you to compare the new one with what the
kernel itself prints (`docker exec securacv pair`, the app's panel, or
`doctor`) before it rotates the pin. The previous key stays recorded in the
entry so earlier logs remain attributable.

The kernel's **own** self-check (`POST /verify`, the hourly
`verify_interval_hours`) is shown only as the attributes
`kernel_reported_chain_ok` and `kernel_verified_bucket_start`, and on an
unclaimed bridge as *Chain OK (kernel-reported)* — never as Verified. A
kernel vouching for itself with its own key is not a check you ran.

When the walk runs: the **Verify now** button, the `securacv.verify`
action, every 15 minutes, 60 seconds after new witness events stop arriving,
and (a walk, never a verdict) when the bridge's `chain_problem` sensor
clears. The walk is incremental — the newest verified entry's `seq` and
hash are the cursor, kept in `.storage/securacv.verify_<entry_id>` — so an
hour of events costs one small fetch, and it leaves no receipt on the
kernel. After a replaced or restored log (the chain head behind the last
verified entry) the automatic walks keep reporting the failure; pressing
**Verify now** re-anchors and walks the served log again.

The dashboard's chain-status header says the same in words: *Chain verified ✓
12 min ago · 1,204 entries · key pinned*, *Verifying…*, *Verification failed
at entry #1,190*, *Not verified yet*, *Witness key changed — confirm it in
Settings → Repairs* — and, for an unclaimed bridge, *Chain OK
(kernel-reported)*.

## Add the dashboard

**Settings → Dashboards → Add dashboard → SecuraCV** (under *Community
dashboards* on Home Assistant 2026.5 and later). The integration ships a
dashboard **strategy** that builds the whole page from whatever SecuraCV
entities exist when it opens, in this order: the chain-status header with
**Verify now** and **Export evidence**, **Today** (the verified timeline),
**Cameras** (one tile per camera with its last sealed event and an **Open
Frigate** link), **Last 24 hours** (the digest), **Health** (Frigate link,
kernel online, chain length, the witness key fingerprint, storage) and
**Canaries** when you have any. Nothing to configure, no entity ids to
type.

From YAML instead — a dashboard whose whole configuration is
`strategy: {type: custom:securacv}`, which is what
[`homeassistant/lovelace/securacv-dashboard.yaml`](../homeassistant/lovelace/securacv-dashboard.yaml)
holds and what the one-command installer registers; or
[`securacv-dashboard-plain.yaml`](../homeassistant/lovelace/securacv-dashboard-plain.yaml),
the same page from stock cards only. Options, the header copy, the badges,
and the timeline card on its own: [`docs/lovelace_timeline.md`](lovelace_timeline.md).

## Entity ids: `pwk_*` → `securacv_*`

Before the integration connects, the kernel's MQTT bridge registers its own
entities through MQTT discovery (`sensor.pwk_last_event`,
`sensor.pwk_daily_digest`, `binary_sensor.pwk_chain_problem`,
`button.pwk_verify_now`, per-zone counters) — Home Assistant's core `mqtt`
integration owns them, and they show the kernel's self-report, never
"Verified". When the integration connects it **claims** the kernel (a
retained `witness/claim/<kernel id>` message), the bridge withdraws those
discovery entities, and the integration's own take over:

| Before | Now |
|---|---|
| `sensor.pwk_last_event` | `sensor.securacv_last_event` |
| `sensor.pwk_daily_digest` | `sensor.securacv_daily_digest` |
| `binary_sensor.pwk_chain_problem` | `binary_sensor.securacv_chain_valid` (note the sense: **on** means the chain verified) |
| `button.pwk_verify_now` | `button.securacv_verify_now` |
| `sensor.pwk_<zone>_events` | `event.securacv_<zone>` — one event entity per camera, named after it |

On an instance where the `pwk_*` entities existed, Settings → Repairs gets
the notice **SecuraCV replaced the app's pwk_* entities** once, with this
table; a fresh install never sees it. Re-point automations and dashboards
that named the old ids. The shipped blueprints and the dashboard strategy
already use the new ones, and an automation created from an older digest
blueprint keeps working through the change (the digest reads a
problem-class sensor as "kernel-reported"). Removing the integration
withdraws the claim and the bridge republishes its `pwk_*` entities.

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

On Home Assistant OS the SecuraCV app installs the same integration on your
click instead ([Path 1](#path-1-home-assistant-os--the-securacv-app)).

### Step 2: Configure the Integration

The one-command installer and the app's discovered card create this config
entry for you — skip to Step 3 if you used either. By hand:

1. Go to **Settings > Devices & Services > Add Integration**
2. Search for "SecuraCV"
3. Keep the default, **"Automatic — detect what's installed"**, and click
   Submit: it probes the app at `http://d0491a67-privacy-witness-kernel:8799`
   through the token file `/config/api_token`, listens for a sidecar's
   announcement, and configures kernel + MQTT or MQTT-only to match
   ([Path 2](#path-2-hacs-first) spells out each outcome). Nothing to type.
4. Or pick **"Canary devices via MQTT"** explicitly. Its one option, the MQTT
   topic prefix, defaults to `securacv` — the prefix Canary firmware always
   publishes on. Leave it alone; there is nothing to invent or match. (Change
   it only if you have deliberately re-bridged topics under another prefix.)
5. The two manual kernel forms, **Witness Kernel Connection** and **MQTT +
   Kernel Connection**, exist for unusual layouts: a kernel on another host
   whose token file Home Assistant can read, or a re-bridged prefix. Their
   fields are the **Kernel URL**, the **API token file** (recommended;
   default `/config/api_token`, re-read whenever the kernel rotates its
   token) or a **Static API token** (goes stale within ten minutes unless a
   token file is also set), the **Witness MQTT prefix** the kernel's bridge
   publishes under (default `witness`), and an optional **Adapter host stats
   URL**. A Docker sidecar is better added through its announcement, which
   pairs with a 6-digit code instead ([Path 3](#path-3-home-assistant-container-and-the-docker-sidecar)).

### Step 3: Configure the Canary Device

> **Or over Bluetooth, from a phone (Improv via BLE).** A brand-new Canary
> Sense, Vision or WAP (FULL profile) opens a Bluetooth setup door for 30
> minutes after power-on — the open [Improv Wi-Fi](https://www.improv-wifi.com/)
> standard. The SecuraCV iPhone app shows a "new Canary is nearby" card;
> so does the **Home Assistant companion app**, and Home Assistant's own
> *Improv via BLE* integration can discover it when the Home Assistant
> machine has a Bluetooth radio of its own. Any of them can hand the Canary
> your Wi-Fi; it then appears here over MQTT like any other. Two honest
> limits: a standard Improv client gets **Wi-Fi only** — the SecuraCV
> pairing (a WAP's receipt) lives in our own companion service, so pair a
> WAP with the SecuraCV app afterwards from its receipt — and an **ESPHome
> Bluetooth proxy cannot complete** the one-tap (Just Works) pairing the
> door requires, so a Canary seen only through a proxy will show and then
> fail at the first write. Both expected; neither bench-tested yet. The
> door never opens on a Canary whose saved Wi-Fi is merely failing — that
> raises the setup network below. → [magic pairing (design)](design/magic_pairing.md)

Connect to your Canary's WiFi AP (SSID shown on device, password is device-unique):

1. On first boot, joining the AP from a phone pops the Canary's **setup
   wizard** on its own (the captive-portal sheet) — pick your home WiFi
   there and enter its password. The wizard's finish screen also offers an
   optional **"point it at your hub"** step, prefilled with the values that
   are right for a SecuraCV hub (`homeassistant.local`, port `1883`) — enter
   the broker credentials (if you ran the one-command installer: username
   `canary`, password from **Settings → Apps → Mosquitto broker →
   Configuration → Logins**), save, and let it restart.
2. If the wizard doesn't appear (or the Canary was set up before): a Canary
   WAP's first-boot wizard opens only at `http://canary.local` or
   `http://192.168.4.1` (under its unique hostname, `/` during first boot
   shows the static page that says to type `canary.local`), and a WAP has no
   `/setup` page. Otherwise open the Canary's unique hostname or
   `http://192.168.4.1` in your browser; once a WAP is set up, its unique
   hostname reaches the dashboard. On a Canary WAP
   the hostname is `http://canary-<name>.local` once you have named it, and
   until then `http://canary-` plus four lowercase hex digits, the first two
   bytes of its key fingerprint (e.g. `http://canary-7916.local`); at the
   default log level its serial log names it on the
   `[INFO/NETWORK] mDNS started | canary-7916` line. If you only have one
   Canary, plain `http://canary.local` may also resolve, but each Canary
   always advertises its unique hostname so multiple devices on the same
   network don't collide. On a `firmware/canary` unit, `/setup` on any of
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

Within 30 seconds of a Canary connecting to MQTT, and within a minute of a
kernel being configured, **Settings > Devices & Services > SecuraCV** shows
the devices below with their entities. This list is what the integration
itself creates (`custom_components/securacv/sensor.py`, `binary_sensor.py`,
`event.py` and `button.py`); each entity appears the first time its topic or
API answer is seen. The names in bold are what Home Assistant renders after
the device name; they come from each entity's translation key, not from the
code. To rename one, edit `custom_components/securacv/strings.json`, copy it
over `translations/en.json` (a test keeps the two byte-identical), and
mirror the rename in this list and in the names
`tests/test_entity_translations.py` pins; other languages go in their own
`translations/<lang>.json`. Entity ids are never set by the code — Home
Assistant derives them from the device and entity names, so a fresh install
gets `sensor.securacv_last_event`, `event.securacv_front_door` and so on.

#### The kernel device: "SecuraCV"

One device per witness kernel (the app or a sidecar): name **SecuraCV**,
model *Privacy Witness Kernel*, serial number the kernel id (`pwk_` plus
eight hex characters), software version the kernel's, configuration URL the
kernel's API. Every Canary device hangs off it (*via device*). Unique ids
are `securacv_<entry_id>_<key>` with the key shown in parentheses. Kernel
entities are available while the bridge's `witness/status` says online **or**
the last five-minute HTTP backstop succeeded; the **Online** sensor stays
available to report offline.

   Sensors:
   - **SecuraCV Last Event** — the newest sealed event, with the full attribute table of a witness event below (`kernel_last_event`; retained `witness/last_event`)
   - **Chain Length** — the sealed log's head: the newest entry's sequence number from the integration's last chain walk (`head.seq`), or the kernel's own row count from `/api/identity` until the first walk — `source` says which (`walk_head` / `kernel_identity`); the two differ once retention has pruned rows, so the sensor has no statistics class. Attributes `latest_hash`, `source`, `head_seq`, `kernel_chain_length`, `algorithm`, `verifying_key`, `pinned`, `verified_seq` (`chain_length`; unit blocks)
   - **Daily digest** — total sealed events over the kernel's rolling 24-hour window, with the whole `/digest` document as attributes: counts by event type, by zone and by coarse day period (`daily_digest`; retained `witness/digest`)
   - **Kernel key fingerprint** — the first eight hex characters of sha256 over the pinned verifying key, the same eight `docker exec securacv pair`, the app's panel and `doctor` print, with `verifying_key`, `pinned_bucket_start` and `key_mismatch` (`kernel_fingerprint`; diagnostic)
   - **Storage Health** — `healthy` / `warning` / `critical` for the kernel's database storage from `GET /status` (`storage_health`; diagnostic)
   - **Storage Free** — free space of the database directory in percent (`storage_free_pct`; diagnostic)
   - **Storage Wear Estimate** — estimated media wear in percent where the host exposes it, else unknown (`storage_wear_pct`; diagnostic)
   - **Storage Write Rate** — megabytes written per day where known, else unknown (`storage_write_rate`; diagnostic)
   - **SoC Temperature** — the kernel host's SoC temperature where the host exposes it (`soc_temperature`; diagnostic)
   - **SecuraCV Adapter Host** — per-adapter health read from an optional adapter-host stats URL; created only when that URL is set (`adapter_stats`; diagnostic)

   Binary sensors:
   - **Online** — the kernel is reachable: the bridge's `witness/status` last-will topic says online, or the five-minute HTTP backstop answered (`kernel_online`; connectivity)
   - **Chain Valid** — ON when the integration's own last chain walk verified the sealed log against the pinned key, OFF when it rejected it, unknown until the first walk and while the kernel serves a key other than the pinned one (`key_mismatch: true`, `trust_reason: kernel_key_mismatch`, `error` naming Settings → Repairs — a key change is never shown as a failure); attributes `verified_seq`, `verified_entries`, `verified_bucket_start`, `verified_bucket_size`, `head_seq`, `failed_seq`, `key_mismatch`, `trust_reason`, `verifying`, `last_trigger`, `kernel_reported_chain_ok`, `kernel_verified_bucket_start`, `error` (`chain_valid`)
   - **Frigate link** — ON when `frigate_bridge` holds its broker connection AND Frigate was heard within the last hour, so a quiet night and a Frigate that stopped publishing read differently; attributes are the retained `witness/frigate_link` payload (`frigate_link`; connectivity, diagnostic)
   - **Storage Replacement Recommended** — the kernel's `/status` recommends replacing its storage; Settings → Repairs says so too (`storage_replace`; problem)

   Events:
   - **Witness events, one per camera** — one event entity per camera or zone the kernel has sealed an event for, named after the zone (`event.securacv_front_door` for Frigate camera `front_door`), created on the first sealed event for that zone and restored on reload; its event types are the witness dictionary's ids (`boundary_crossing_object_large`, `boundary_crossing_object_small`, `presence_in_restricted_zone`, …) and it fires only on live `witness/events` messages, never on a retained replay after a restart (`witness`)
   - **Chain verification** — the integration's own chain walks: `chain_verified`, `chain_verification_failed` (also when the bridge's own `chain_problem` turns on, marked `kernel_reported: true`), and `witness_key_changed`; attributes `trigger`, `verified_seq`, `verified_entries`, `verified_bucket_start`, `verified_bucket_size`, `failed_seq`, `error` (`verification_events`)

   Buttons:
   - **Verify now** — run the integration's chain walk now; the same coroutine the `securacv.verify` action runs (`verify_now`; unavailable while the kernel is unreachable)
   - **Export evidence** — write a signed evidence pack for the last 24 hours; the same as `securacv.export_evidence` with its defaults (`export_evidence`; unavailable while the kernel is unreachable)

Every witness event — on the per-camera event entities and on **SecuraCV
Last Event** — carries its event type (the dictionary id: the event entity's
`event_type`, the sensor's state) and the same attributes: `zone_id` and
`zone` (`zone:front_door` and `front_door`), `time_bucket`
(`start_epoch_s`, `size_s` — a ten-minute window, never a precise time),
`confidence`, `attestation` (`adapter` for a Frigate event), `friendly_event`
(*Person or vehicle* and *Animal or small object* for the two Frigate
classes; the dictionary ids are unchanged), `kernel_version`, `ruleset_id`,
`kernel_device_id`, `seq` (the sealed-log row), `signed`, and the verdict:
`verification`, `verified`, `trust_reason`, `verified_bucket_start` from the
integration's walk, plus `kernel_reported_chain_ok` and
`kernel_verified_bucket_start` from the kernel's self-check
([what each means](#what-sealed--verified--and-failed--mean)). One sealed
event per camera, object class and ten-minute window is the kernel's rule,
not data loss.

#### Canary devices: "SecuraCV Canary <id>"

One device per Canary, created by the first publish the integration sees
from it on its `status`, `health`, `counts`, `chain`, `events`, `tamper`,
`transport`, `mesh` or `chirp` topic (the `availability` topic alone creates
none, and a device another topic created first takes the name its first
status gives), attached under the kernel device when one exists. Its model
is the product name for the `device_type` its status names (*Canary WAP*,
*Canary Sense*, *Canary Dash*, and so on; *SecuraCV Canary* when the status
names none, as the `firmware/canary` build's does, or names a type this
release does not know), and its firmware is the `firmware_version` its
`health` topic reports, which is where every Canary publishes it. A version
on its `status` (only the `firmware/canary` build's carries one, as
`firmware`) shows only until a health has reported one. The firmware, the
board and the name its status gives are unsigned text from the broker,
bounded to 64 characters; the model is picked from the product table by a
`device_type` that is unsigned too, and is never the wire string itself. No
serial number is set. Every
unique id is `securacv_canary_<device_id>_<suffix>` with the suffix shown in
parentheses. **Signed** means the publish carries the device's Ed25519
signature and the integration checks it against the pinned key
([Step 6](#step-6-verify-per-device-pki-optional-but-recommended)),
stamping `verified` / `trust_reason` / `pinned_fingerprint` /
`received_fingerprint` attributes. Every other topic is **unsigned** — the
firmware puts no signature on it — and those entities carry the same four
attributes with `verified: false`, `trust_reason: unsigned`, so a dashboard
can tell an unsigned publish from a verified one. Canary entities are
available while the device's own `status` / `availability` last-will topic
says online; the **Online** sensor stays available to report offline.

   Sensors:
   - **Witness Count** — total witness records created (`witness_count`; signed `counts` topic)
   - **Chain Length** — hash-chain length, with `latest_hash` and `algorithm` (`chain_length`; signed `chain` topic)
   - **Last Event** — latest witness event type, with zone, confidence, modality and attestation; a 23:55 daily summary reads `daily_summary` and a Quiet Hours held summary `held_summary` (the two stateless rows named by their `type`, HA28), every other stateless row `unknown` (`last_event`; signed `events` topic)
   - **Health** — `healthy` / `warning` / `critical` from battery and free memory, with `public_key`, uptime and firmware version, and the committed-event egress's counters (`csi_event_egress`, `offline_queue`) as attributes when the firmware sends them (`health_status`; `health` and `egress` topics, unsigned)
   - **GPS Fix** — GPS fix status, with satellites and HDOP (`gps_fix`; `health` topic, unsigned)
   - **SD Wear Estimate** — estimated SD-card wear percent, only when the firmware reports its `sd` object (`sd_wear`; `health` topic, unsigned; diagnostic)
   - **Radar Link** — `ok` / `stale` / `down` for the UART link to the radar module, canary-sense devices only (`radar_link`; `health` topic, unsigned; diagnostic)

   Binary sensors:
   - **Online** — device connectivity, from the `status` topic (`online`; unsigned)
   - **Chain Valid** — ON only when the device reports its chain intact and the chain publish verified against the pinned key; unknown until a chain publish has been checked (`chain_valid`; signed `chain` topic)
   - **Tamper** — any tamper detected, from the `health` and `tamper` topics (`tamper`; unsigned)
   - **Power Loss**, **SD Removed**, **SD Error**, **GPS Jamming**, **Unexpected Motion**, **Enclosure Open**, **GPIO Tamper**, **Watchdog Timeout**, **Unexpected Reboot**, **Memory Critical** — one sensor per tamper type, from the `tamper` and `health` topics (`tamper_<type>` with the type names in the [per-tamper-type catalog](#per-tamper-type-sensor-catalog) below, which also says which signals firmware emits today; unsigned)
   - **SD Replacement Recommended** — the device recommends replacing its SD card (`sd_replace`; `health` topic, unsigned)
   - **Event ID Space Low** — ON once the device's event-id counter nears the end of its space, and after it wraps (Home Assistant then refuses its events); only for devices whose health carries the flag, the Canary base and the canary-wap (`event_id_space_low`; `health` topic, unsigned; diagnostic)
   - **Motion** and **Occupancy** — standard `motion` / `occupancy` device classes for the HomeKit Bridge and any other consumer, asserted by the signed `events` topic and, for occupancy, the retained `state` snapshot (`motion`, `occupancy`; carry the events verdict)
   - **WiFi AP**, **WiFi Station**, **MQTT**, **Bluetooth**, **Mesh Network**, **Chirp Network** — per-transport connectivity, diagnostic and **disabled by default** — enable them from the device page (`transport_<type>`; `transport` topic, unsigned; no current firmware publishes it — see the [transport catalog](#transport-catalog))
   - **Mesh Connected** — Opera mesh peers present, with peer and relay counts (`mesh_connected`; `mesh` topic, unsigned)
   - **Chirp Active** — Chirp community network on: the canary-wap's channel is on (active, cooldown or muted) rather than disabled, with the firmware's own word in its `channel_state` attribute (`chirp_active`; `chirp` topic, unsigned)

   Events:
   - **Tamper events** — one event entity per Canary with the ten tamper types above as its event types; fires on the device's live `tamper` publishes, with `kind` and `device_id` (`tamper_events`)

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

### Step 4b: Add the dashboard and the timeline card

**Settings → Dashboards → Add dashboard → SecuraCV** gives you the whole
page ([above](#add-the-dashboard)). For the "single pane of glass" card on a
dashboard of your own, edit it → **Add Card** → search **"SecuraCV Verified
Timeline"**: a verified-✓ event timeline with the chain-status header,
auto-discovering the entities above. Options and a no-custom-card YAML
fallback: [`docs/lovelace_timeline.md`](lovelace_timeline.md).

### Step 4c: Ready-made dashboards

[`homeassistant/lovelace/`](../homeassistant/lovelace/) holds the strategy
dashboard (`securacv-dashboard.yaml`, one `strategy:` block), the same page
from stock cards only (`securacv-dashboard-plain.yaml`, for installs that
block auto-registered modules), and the Canary Vision views. The one-command
installer copies them to `/config/securacv/dashboards/` and, when your
`configuration.yaml` has no `lovelace:` key of its own, registers the
strategy dashboard in the sidebar (a clearly marked block, validated with
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
    securacv-witness:
      mode: yaml
      title: SecuraCV
      icon: mdi:shield-check
      show_in_sidebar: true
      filename: securacv/dashboards/securacv-dashboard.yaml
```

…then restart Home Assistant. Or skip registration entirely: **Settings →
Dashboards → Add dashboard → SecuraCV** is the same page with no file at
all, and any dashboard's **Add card → Manual** takes the cards out of the
plain YAML.

### Step 5: Set Up Notifications

The one-command installer and the SecuraCV app place every blueprint under
`/config/blueprints/automation/securacv/`, where Home Assistant loads them
without importing — if you used either, go straight to **Settings →
Automations → Blueprints** and pick one. Every blueprint that notifies takes
a **Phone** input (a device running the Home Assistant Companion app) and
derives its `notify.mobile_app_…` service by itself; the optional
*Notification service* text input overrides it for groups, Telegram,
Pushover and the like. The catalog, with a one-click import badge for each:
[`docs/blueprints/README.md`](blueprints/README.md).

For the witness log:

- **Witness notification with proof**
  ([`securacv_witness_notification.yaml`](blueprints/securacv_witness_notification.yaml)):
  Frigate raises an alert on a camera → the automation waits up to 90 s for
  SecuraCV to seal it → one push: *Front door · Person — 14:03 · Sealed ✓
  into the witness log (entry #1,204, window 14:00–14:10)*, or *seal pending*
  on timeout, with **Open Frigate** and **Export evidence** actions and an
  optional Frigate thumbnail through the Frigate integration's notification
  proxy. It renders only the camera, Frigate's object label, the zones and
  the seal state — never a recognized name or plate. One automation per
  camera.
- **Daily digest** ([`securacv_daily_digest.yaml`](blueprints/securacv_daily_digest.yaml)):
  every morning, *7 events: front door 4, driveway 3 · morning 2, evening 5 ·
  chain verified ✓ at 03:00 · 1,204 entries.* It always sends — a quiet night
  reads *Quiet night: no witness events* with the chain state, so silence is
  never ambiguous. Defaults: `sensor.securacv_daily_digest`,
  `binary_sensor.securacv_chain_valid`, `sensor.securacv_chain_length`.
- **Quiet-hours activity & silent zone**
  ([`securacv_pattern_break.yaml`](blueprints/securacv_pattern_break.yaml)).

For Canary devices, the **Canary alerts** blueprint
([import `securacv_alerts.yaml`](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_alerts.yaml)):

1. Go to **Settings > Automations > Blueprints > Import Blueprint**
2. Enter URL: `https://github.com/kmay89/securaCV/blob/main/docs/blueprints/securacv_alerts.yaml`
3. Create an automation from the blueprint
4. Pick the sensors you want monitored (each alert type is optional — leave
   an input empty to skip it): smoke/CO alarm heard (critical pushes that
   bypass silent mode), tamper, power loss, chain failure, offline, GPS loss
5. Pick your phone (or type a notification service)

Or copy automations from
[`docs/homeassistant_automations.yaml`](homeassistant_automations.yaml) for
manual setup; battery health has its own blueprint,
[`canary_battery_health.yaml`](blueprints/canary_battery_health.yaml).

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

The integration registers five Home Assistant actions. Try them from
**Developer tools → Actions**, or call them from an automation or a script.

Two are about the witness kernel:

| Action | Takes | Returns |
|---|---|---|
| `securacv.verify` | `entry_id` (optional; the kernel entry to verify, a config-entry selector — required only when more than one entry has a kernel) | `chain_valid`, `verdict`, `events_verified`, `verified_entries`, `verified_seq`, `verified_bucket_start`, `verified_bucket_size`, `key_mismatch`, `error`, `kernel_device_id` |
| `securacv.export_evidence` | `window_hours` (1–720; 24 if left out), `entry_id` (optional, as above) | `export_id`, `file`, `url` (a download link signed for 24 hours), `sha256`, `events`, `window_hours`, `kernel_device_id` |

`securacv.verify` walks the kernel's sealed log now and checks every entry's
hash link and Ed25519 signature against the key Home Assistant pinned when
it connected — the only walk that earns an event its **Verified ✓**
([above](#what-sealed--verified--and-failed--mean)). It is the same
coroutine the **Verify now** button runs.

`securacv.export_evidence` downloads the kernel's signed evidence bundle for
the last N hours (`GET /export/bundle`, a receipted disclosure on the kernel)
and writes **one zip** —
`securacv-evidence-<YYYY-MM-DD>-<8 hex>.zip` — under
`<config>/securacv/evidence` (directory `0700`, files `0600`, never under
`www/`), holding the kernel's bundle verbatim
(`securacv-events-<bucket>.json`), `evidence_viewer.html` (the offline
checker), `README.txt` (the recipient's recipe) and `WITNESS_KEY.txt`
(`verifying_key=…`, `fingerprint=…`, `kernel=…`). Nothing is added to,
removed from or re-serialized in the bundle. A persistent notification,
**SecuraCV evidence export ready**, reads *"Evidence pack ready — last 24
hours, 17 sealed events, chain verified ✓. Download the evidence pack (link
valid 24 hours; also under Media → SecuraCV evidence). Send the whole zip to
whoever needs to check it. Your witness key fingerprint, to confirm by phone:
`ab12cd34`."* The pack is served only by an authenticated Home Assistant
view (`/api/securacv/evidence/<export_id>`) through a link signed for 24
hours; an expired link needs a new export or **Media → SecuraCV evidence**,
where each pack resolves to a fresh link. The **Export evidence** button is
this action with its defaults; the timeline card's per-row **Export** action
calls it with a window covering that row. How many packs stay is the
`evidence_keep` option ([Configuration parameters](#configuration-parameters)):
the oldest zip is removed and the notification says *Removed N older
export(s) to stay within your limit of 20.* The recipient's side —
dropping the zip on the viewer, "self-attested" versus "authorship
confirmed" — is [`viewer/README.md`](../viewer/README.md).

Both refuse, with a message, when no kernel entry is loaded, when
`entry_id` is needed and missing, when the entry is not loaded, or when
`window_hours` is out of range; they fail with a message when the kernel is
unreachable, rejects Home Assistant's credential (re-authenticate the entry),
answers something other than the sealed log or the bundle, or the pack
cannot be written.

The other three are about [watches](design/watches.md): bounded attention
that ends by itself.

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

```yaml
# automations.yaml — a signed evidence pack every Sunday morning, kept to the
# evidence_keep limit; the download link arrives as a notification
- alias: "Weekly witness export"
  triggers:
    - trigger: time
      at: "07:00:00"
  conditions:
    - condition: time
      weekday: [sun]
  actions:
    - action: securacv.export_evidence
      data:
        window_hours: 168
```

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
- There are no actions for pinning, rotating or unpinning a device key, and
  none that pins or rotates the kernel's key. Those stay in the options flow
  and in Settings → Repairs on purpose
  ([why](device_trust.md#why-pin-rotate-and-unpin-are-not-actions)).
  Actions never mutate trust.

### Step 6: Verify per-device PKI (optional but recommended)

Each Canary that witnesses signs its `chain`, `events`, and `counts`
MQTT publishes with an on-device Ed25519 key (a canary-display signs
nothing). HA auto-pins the public key the first
time a device's health publish appears (TOFU), then verifies every
subsequent publish.

To check that verification is live: open the chain-length sensor's
attributes — you should see `verified: true`, `trust_reason: ok`, and
matching `pinned_fingerprint` / `received_fingerprint` values, both in
lowercase (a canary-wap on firmware 2.4.15 or older sends its
fingerprint in capitals, and HA reads that as the same value). If your
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
- **canary-sense** and **canary-sentinel**, on a firmware release after
  2.4.15: the boot log on the serial console. Right after
  `Ed25519 ready  fp=…` it prints `Ed25519 pubkey` and the 64-character
  key, once per boot, so open the serial monitor and then reset the
  board. The `Device ID` line later in the same log is the `device_id`.
  2.4.15 and older show only the fingerprint: compare it with the
  `pinned_fingerprint` attribute instead. The key line is compiled by CI
  but has not been read off a unit on a bench, and canary-sentinel has
  not run on hardware at all. Which port carries that console is
  unverified as well: read from the build flags, it may be the header
  pins the radar uses rather than the board's USB-C port (the table
  linked below has the detail).
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
| `securacv/{device_id}/egress` | Device → HA | canary-wap only: its committed-event egress counters, right after each health publish, naming that health's `firmware_version` and `uptime` (retained) |
| `securacv/{device_id}/events` | Device → HA | Witness record events |
| `securacv/{device_id}/chain` | Device → HA | Hash chain state |
| `securacv/{device_id}/tamper` | Device → HA | Tamper alerts (immediate) |
| `securacv/{device_id}/availability` | Device → HA | Online/offline (LWT, retained) |
| `securacv/{device_id}/update/state` | Device → HA | Firmware update entity state (retained) |
| `securacv/{device_id}/update/cmd` | HA → Device | `install` — start a firmware update |
| `homeassistant/*/securacv_*/config` | Device → HA | HA MQTT Discovery config (retained) |

Three keys for a bench run or a field report, which the integration also
shows (`mosquitto_sub -t 'securacv/+/health' -t 'securacv/+/egress'`):

- `event_id_space_low` (Canary base and canary-wap, `health`): `true` once
  the device's event-id counter nears the end of its space, about four
  years before it wraps at the most a device can commit, and after a wrap.
  Past the wrap Home Assistant refuses the device's events as replays. The
  integration shows it as the **Event ID Space Low** binary sensor. The
  recovery is not decided yet; the flag only warns.
- `csi_event_egress` (Canary base in `health`; canary-wap in its retained
  `egress` topic, since its health has no room for it, as
  `{"firmware_version":…,"uptime":…,"csi_event_egress":{…}}`, the version
  and uptime of the health publish it follows):
  what its committed-event egress did since boot — `dropped` (commits its
  egress queue had no room for),
  `held_dropped` (rows dropped from its 8-row RAM hold, oldest first),
  `ambient_dropped` (`wifi.channel_activity` rows that had to wait),
  `unsent_dropped` (rows no card kept that the MQTT layer refused, lost)
  and `planner`, the SD backfill's counters (`live`, `held`, `queued`,
  `replayed`, `skipped`, `untrusted`, `unsendable`, `truncated_unsent`,
  `read_giveups`).
- `offline_queue` (Canary base, `health`): what the MQTT layer's 12-record
  offline queue dropped since boot — `dropped_overflow` (evicted or refused
  when full: an outage longer than the queue loses its oldest events here),
  `dropped_oversize` and `dropped_flushed` (discarded when the broker
  changed). Events and tamper alerts together. The canary-wap has no
  offline queue.

The **Health** sensor carries `csi_event_egress` and `offline_queue` as
attributes of the same names, holding the counters the integration knows
(`custom_components/securacv/const.py`); a device that sends none shows
neither attribute. The counters start over at every boot and the `egress`
topic is retained, so the sensor shows a canary-wap's only while they pair
with its latest health: the same `firmware_version`, and an `uptime` no
later than the health's. A body an earlier boot left on the broker, or one
left behind by newer firmware after a rollback to firmware that publishes
no `egress` topic, is not shown as current, after a Home Assistant restart
too. Clearing the retained topic (`mosquitto_pub -r -n -t
securacv/<device_id>/egress`) removes the attribute. Both devices also
return `csi_event_egress` from their token-gated `GET /api/diagnostics`
(the canary since sweep F179), so a bench run without a broker can read
the counters too.

These count paths, not rows: `planner.queued` counts a row handed to the
offline queue even if the queue evicts it later (then it is in
`offline_queue.dropped_overflow` too), and `planner.held` minus
`planner.replayed` is not what is still owed. [The CSI developer
API](csi_developer_api.md) defines each counter and lists the rows none of
them counts.

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
  downloads the update over HTTPS, checks its SHA-256, verifies its Ed25519
  release signature against the release key built into its firmware,
  installs it to the inactive partition, restarts, and confirms itself
  healthy. A live progress bar tracks the whole cycle.
- If the new firmware fails its health check after the restart, the device
  boots its previous firmware again and reports what happened. An image
  whose Ed25519 release signature does not verify against that key is
  refused before the device ever boots it, and a download that fails leaves
  the running firmware as it was.
- Turn on the **Auto Update** switch (per device) to install new releases
  hands-free within a day of publication. It's off by default — a witness
  device never restarts unattended unless you choose that.
- Every update is signed into the device's witness chain
  (`fw_update_started` / `fw_update_applied` / `fw_update_rolled_back`),
  so the audit trail proves when firmware changed.

See `docs/firmware_ota.md` for the release pipeline, local/air-gapped
hosting, and the full security model.

---

## Configuration parameters

**Installation parameters** (asked once, in the config flow):

| Parameter | Where it comes from | Notes |
|---|---|---|
| Kernel URL | Supervisor discovery (the app: `http://d0491a67-privacy-witness-kernel:8799`), the sidecar's announcement or your MQTT broker's host (prefilled, editable), or typed in a manual form | Must carry its scheme: `http://192.168.1.20:8799` |
| Credential | **Pairing code** — the six digits `docker exec securacv pair` prints (single-use, ten minutes, five tries); **API token file** — the path of the kernel's rotating token, `/config/api_token` for the app, re-read when the token rotates; or a **Static API token** for a kernel whose file Home Assistant cannot read (goes stale within ten minutes) | A pairing code becomes a long-lived paired token Home Assistant keeps; the kernel stores only its hash |
| Witness MQTT prefix | the announcement or the discovery payload (`mqtt_topic_prefix`), default `witness` | The prefix the kernel's bridge publishes under |
| MQTT topic prefix (Canaries) | default `securacv` | The prefix Canary firmware always publishes on |
| Adapter host stats URL | optional | A read-only `adapter_host` stats endpoint, surfaced as the diagnostic **SecuraCV Adapter Host** sensor |

**Options** (Settings → Devices & services → SecuraCV → **Configure**), a
menu of four:

| Menu item | What it does |
|---|---|
| **Pin a device pubkey (manual)** | Pin a Canary's Ed25519 public key by hand from its `device_id` and 64-hex key, read off the device ([Step 6](#step-6-verify-per-device-pki-optional-but-recommended)) |
| **Rotate a pinned device key** | Replace a Canary's pin; the previous key stays in the audit trail and the device's mismatch notification clears |
| **Unpin a device** | Drop a Canary's pin; the next publish pins whatever key arrives (TOFU) |
| **Evidence export settings** | `evidence_keep`: how many evidence packs to keep under `<config>/securacv/evidence` — 1 to 200, default 20. When a new export exceeds it the oldest pack is removed and the export notification says so |

The kernel's key is not in that menu on purpose: it is pinned over the
authenticated channel at pairing or discovery and rotated only through the
Repairs fix flow after you have compared fingerprints.

**Reconfigure** (the entry's ⋮ menu) moves the kernel URL, the credential
(a fresh pairing code, or the token file path) and the witness prefix in
place; the entry stays bound to the same kernel and a different kernel is
refused (*That is a different witness kernel. Add it as a new entry instead
of moving this one.*). **Re-authenticate** starts by itself when the kernel
definitively rejects Home Assistant's credential (a revoked paired token, an
unreadable token file): *"The witness kernel rejected Home Assistant's
credential. For a paired sidecar, run `docker exec securacv pair` on the
Docker host and enter the new 6-digit code; for the app, check the token
file path."*

## How the integration gets its data

The integration is **push-fed**. The kernel's MQTT bridge publishes under
the witness prefix — `witness/last_event`, `witness/events`, `witness/digest`,
`witness/chain_problem`, `witness/status` (the last-will availability
topic), `witness/claim_state` and `witness/frigate_link` — and the
integration subscribes to all of them, so a sealed event reaches the event
entities on the bridge's next poll of the kernel (every 30 s by default,
`POLL_INTERVAL`).
Canary devices publish on `securacv/<id>/…` and are handled the same way
([MQTT Topic Reference](#mqtt-topic-reference)).

An **HTTP backstop** runs every five minutes against the kernel's API —
`GET /health`, `GET /api/identity`, `GET /status` — so a broker outage never
leaves the entities stale; a missed event the backstop finds is fired
exactly once. Fifteen minutes of consecutive backstop failures raise the
Repairs issue **SecuraCV can't reach the witness kernel**, which clears
itself on the next success. None of these reads leaves a receipt on the
kernel: the bridge's own polling uses `GET /events?since_bucket=…`, a state
read, and only the two disclosure routes (`GET /events` in bulk and
`GET /export/bundle`) are receipted.

The chain walk that earns **Verified ✓** runs every 15 minutes, 60 s after
new events go idle, on **Verify now** and on `securacv.verify`
([above](#what-sealed--verified--and-failed--mean)). The claim that keeps
the bridge's `pwk_*` entities withdrawn is republished on every load and
hourly (the bridge treats a claim older than 24 hours as cleared). The
**Frigate link** sensor turns off when Frigate has not been heard for an
hour, whatever the bridge says.

## Removing the integration

**Settings → Devices & services → SecuraCV → ⋮ → Delete.** Removing the
entry:

- withdraws the claim (an empty retained message on `witness/claim/<kernel
  id>`), so the bridge republishes its own `pwk_*` MQTT-discovery entities —
  within seconds if the broker answers, otherwise when the bridge's 24-hour
  freshness window lapses;
- revokes the paired token on a sidecar (`DELETE /api/pair/<id>`; `docker
  exec securacv unpair <id>` does the same from the Docker host);
- deletes the Canary trust store and the chain-walk cursor
  (`.storage/securacv.verify_<entry_id>`), clears the entry's Repairs issues
  and dismisses its export and dashboard notifications.

**Evidence packs on disk are left for you** under `<config>/securacv/evidence`.
Uninstalling the app or stopping the sidecar does **not** remove the entry
(it is keyed on the kernel's identity, not on the app): after fifteen
minutes it reports *SecuraCV can't reach the witness kernel* until you
delete it. The app's own uninstall leaves `/config/witness.db*`,
`/config/.securacv`, `/config/api_token` and the carried
`/config/custom_components/securacv` in place — they are yours and part of
your backups; the app's [documentation tab](../privacy_witness_kernel/DOCS.md#revoking-and-uninstalling)
lists what to delete by hand. A HACS install is removed from HACS as usual
after the entry is deleted. A Canary that left the fleet can be deleted
from its device page; one still reporting, or still pinned, comes back on
its next publish.

## Known limitations

- **The sidecar's API is plaintext HTTP.** Home Assistant's paired token
  crosses your LAN unencrypted on every request until TLS with a pinned
  fingerprint lands for the sidecar; keep port 8799 on your own network and
  never forward it. The app has no such exposure (its port mapping ships
  disabled and the integration reaches it over the Supervisor's internal
  network).
- **Behind Docker Desktop's userland proxy, or any reverse proxy, every
  client reaches the kernel from one source address**, so the kernel's
  per-address lockout becomes a shared bucket: a LAN host guessing pairing
  codes can hold Home Assistant's polling at `429` for the lockout's length.
- **A failed walk marks every kernel row Failed ⚠**, including rows sealed
  before the rejected entry, until a walk passes again; the offline viewer
  shows which entries before the break still verify.
- **One sealed event per camera, object class and ten-minute window** in
  events mode, one per camera and window in Frigate-alerts mode. Three
  people at the door inside ten minutes are one claim — the coarsening is
  the privacy guarantee, not a bug. The per-camera event entity fires once
  per sealed event.
- **Verified means the integration's walk.** A row stays **Sealed ✓** until a
  walk covers it (at most 15 minutes, 60 s after the chain goes idle);
  Frigate's own notification fires before the seal, which is why the
  witness-notification blueprint waits for it.
- **HACS lists SecuraCV as a custom repository**, not in its default store;
  the app installs the same integration on a click instead.
- **The `pwk_*` entities come back** if the integration is removed or
  Home Assistant stops claiming the kernel for 24 hours — that is the
  bridge's fallback, not a bug.
- **Sealed Frigate events are not Canary events.** The kernel's chain and
  each Canary's chain are separate trust roots
  ([above](#two-witness-logs-two-trust-roots)).

## Supported devices and functions

**Witness kernels** — one device per kernel, discovered through the
Supervisor (the SecuraCV app) or a signed MQTT announcement (the Docker
sidecar), or added by URL: the kernel's chain state, chain length, daily
digest, Frigate link, storage health, key fingerprint, one witness event
entity per camera, the chain-verification event, the **Verify now** and
**Export evidence** buttons, the `securacv.verify` and
`securacv.export_evidence` actions, evidence packs under **Media → SecuraCV
evidence**, redacted diagnostics, Repairs issues for the conditions you can
act on, re-authentication and reconfiguration.

**Canary devices** — every Canary that publishes on `securacv/<id>/…`: the
`firmware/canary` flagship, Canary WAP, Canary Sense, Canary Vision (and
the unreleased Canary Sentinel), with per-publish Ed25519 verification
against a pinned key, the tamper, health, GPS, radar-link, mesh and chirp
entities, the HomeKit-friendly motion and occupancy projections, the
firmware update entity the device announces itself, and the three watch
actions. Canary Display has no signing key and no entities of its own
beyond what its firmware announces.

**Not supported:** anything that would identify a person — there is no
face, plate, name or clip in any entity, attribute, notification, export or
diagnostics dump, by construction.

## Use cases

- **Proof beside the recording.** Frigate keeps a day of clips; SecuraCV keeps
  a year of signed claims. When something happens, **Export evidence** and
  send the zip; the recipient checks it offline and confirms authorship by
  phone with your fingerprint.
- **A notification you can rely on.** The witness-notification blueprint
  delivers *Sealed ✓ into the witness log (entry #…)* per camera, with
  **Open Frigate** and **Export evidence** actions.
- **Silence that means something.** The daily digest always sends, and the
  **Frigate link** sensor tells a quiet night from a Frigate that stopped
  publishing.
- **Automations on sealed events** — trigger on the per-camera event entity
  (`event.securacv_front_door`) rather than on Frigate's raw MQTT, so the
  automation fires once per sealed claim and carries the verdict.
- **Away watches** — `securacv.start_watch` for two weeks of attention on
  one Canary, ending by itself.

More YAML: [`docs/homeassistant_automations.yaml`](homeassistant_automations.yaml)
and the [blueprints](blueprints/README.md).

## Troubleshooting

### Repairs: what each issue means

| Issue (Settings → Repairs) | Severity | What to do |
|---|---|---|
| **No SecuraCV witness kernel found** | warning | Automatic found neither the app nor a sidecar; Canary devices still work. Install the app or the sidecar — the issue clears itself when a kernel entry loads. Only Canary devices? Dismiss it. |
| **SecuraCV can't reach the witness kernel** | error | Fifteen minutes of failed backstops. Check the app or sidecar is running and the URL is right (⋮ → Reconfigure). Clears on the next success. |
| **The witness kernel's key changed** | critical, fixable | Compare the received fingerprint with what the kernel prints (`docker exec securacv pair`, the app's panel, `doctor`). Submit the fix flow only if they match; it rotates the pin and keeps the previous key. Until then kernel rows show a key mismatch and are never shown as verified. |
| **The witness kernel's storage should be replaced** | warning | Export your evidence first, then replace the card or disk; the sealed log lives in the app's or sidecar's data directory and moves with it. |
| **SecuraCV replaced the app's pwk_* entities** | warning | Informational, once: re-point automations and dashboards to the new ids ([the table](#entity-ids-pwk_--securacv_)) and dismiss. |
| **SecuraCV can't subscribe to MQTT** | warning | Home Assistant's MQTT integration is not connected; install the broker, add the MQTT integration, reload SecuraCV. |

Facts that are not actionable — a failed chain walk, a tamper, Frigate
silent — are sensors and events, not issues.

### Symptoms

| Symptom | Check |
|---|---|
| No **SecuraCV app** card under Discovered after the app restart | The app must have started once with the kernel answering `/health` (its log says `discovery` posted); the integration must be installed (the done screen's button, or HACS) and Home Assistant restarted after installing it. Settings → Devices & Services → Add Integration → SecuraCV → Automatic finds the app without the card. |
| No **SecuraCV sidecar** card under Discovered | Home Assistant and the sidecar must share one broker. `docker exec securacv doctor` must show the retained announce published; a sidecar stopped uncleanly leaves no announce — restart it, or clear a stale one with `mosquitto_pub -r -n -t witness/<kernel id>/announce`. The manifest subscribes `witness/+/announce`, so a non-default witness prefix is not discovered — add it through the **Witness Kernel Connection** form, or set `MQTT_TOPIC_PREFIX` back to `witness`. |
| *The code was not accepted* | Codes are single-use, live ten minutes and burn after five wrong tries; run `pair` again. Behind one shared source address (Docker Desktop, a proxy) a `429` lockout from other guesses blocks pairing too — wait it out. |
| Kernel URL wrong on the pair form | It is a guess from the announcement or your broker's host. Type `http://<docker host>:8799`; set `SECURACV_ANNOUNCE_HOST` in the compose file to prefill it next time. |
| Events arrive but stay **Sealed ✓** | The walk runs every 15 minutes and 60 s after events go idle; press **Verify now**. If **Chain Valid** is unknown forever, the kernel's `/api/sealed-log` is not answering — check the entry's credential (Repairs would say) and the kernel log. |
| **Chain Valid** off / rows **Failed ⚠** | The walk rejected the chain. Export evidence now (entries before the break still verify in the viewer). A replaced or restored log needs **Verify now** to re-anchor; a key change is a Repairs issue instead, never a failure. |
| **Frigate link** off | Frigate has not been heard for an hour, or `frigate_bridge` lost the broker. Check Frigate's MQTT settings (host, `topic_prefix`), `docker exec securacv doctor --live`, the app's log. Its attributes carry the bridge's last-message window and mode. |
| The dashboard notification never appeared | It is posted once per entry, only when the strategy module was served this run. Add the dashboard by hand: **Settings → Dashboards → Add dashboard → SecuraCV**, or paste `strategy: {type: custom:securacv}` into a new dashboard's raw editor. |
| **SecuraCV** is missing from Add dashboard | Hard-refresh the browser (the module is cached), or register `/securacv_www/securacv-strategy.js` as a JavaScript module resource under **Settings → Dashboards → ⋮ → Resources** if your install blocks auto-registered modules. |
| The Export button is unavailable | The kernel is unreachable; both buttons follow the kernel's availability. |
| The export notification's link says expired | Links live 24 hours. **Media → SecuraCV evidence** resolves each pack to a fresh link; or export again. |
| `pwk_*` entities and `securacv_*` entities both show | The claim has not reached the bridge yet: it needs the broker, and a bridge from this release or later. `witness/claim_state` reads `claimed` once it has. |
| Diagnostics show redacted values | By design: tokens, hosts, ids, keys, hashes, MACs, SSIDs, names, coordinates and a Canary's hardware, chip, module and Bluetooth identifiers are redacted; the kernel key's fingerprint is shown as its eight characters, and each Canary's key fingerprint as a correlation id (`key_correlation_id`) that links rows of the same dump and carries none of its digits. The file's `issues` section is not redacted: Home Assistant adds every Repairs issue it holds for SecuraCV there itself, including ones you ignored and those of your other SecuraCV entries, each with the time it was raised. The id of an issue about an entry ends in that entry's id, and a kernel key-mismatch issue carries the kernel's URL, host included, and the whole key it served. Remove that section before posting the file. |

## Alternative Setup Modes

The integration's config flow offers four choices. **Automatic** is the
default and the right answer for almost everyone — the manual modes exist for
unusual layouts (a kernel on another host, a re-bridged topic prefix). A
Docker sidecar is added through its discovered card, which pairs with a
6-digit code, not through a manual mode.

| Mode | Best For | How It Works |
|------|----------|--------------|
| **Automatic** (default) | Everyone | Probes for the app, listens for a sidecar's announcement, and configures kernel + MQTT or MQTT-only to match what's installed — zero typing |
| **MQTT only** | Canary device users | Auto-discover devices via MQTT |
| **Kernel only** | A kernel on another host | The kernel's HTTP API, push-fed over its witness MQTT prefix when the broker is there |
| **Both** | Advanced users | MQTT for Canary + HTTP for the kernel |

---

> **Most people are done here.** The by-hand kernel sections below are
> collapsed — each repeats a step the one-command install (or the app +
> wizard) already did for you, kept for unusual layouts and for anyone
> untangling an older install; click one to expand it. The reference
> material stays open and applies to every install: the configuration
> reference, go2rtc, the kernel's MQTT topics, the
> [API reference](#api-reference), [privacy features](#privacy-features),
> and [troubleshooting the app](#troubleshooting-the-app).

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

> The [one-command installer](#the-one-command-installer-terminal) at the top of this
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
3. Install **SecuraCV**

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
anything missing is a link into your own App Store (the app holds no
Supervisor role). Nothing to type.

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
- **Kernel runtime:** run the witness kernel as the SecuraCV app, the Docker sidecar, or a service of your own. The integration connects to its Event API.

---

<details>
<summary><strong>Kernel Installation (Optional App)</strong> — the app repository by hand (the one-command installer already does this) or a local container build</summary>

## Kernel Installation (Optional App)

### Option 1: App Repository

1. In Home Assistant, go to **Settings → Apps → App Store**
2. Click the menu (⋮) → **Repositories**
3. Add: `https://github.com/kmay89/securaCV`
4. Find "SecuraCV" and click **Install**

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

[go2rtc](https://github.com/AlexxIT/go2rtc) is the recommended way to connect cameras in Home Assistant. In standalone mode the witness kernel can auto-discover cameras from go2rtc.

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

## The kernel's MQTT topics and the unclaimed bridge

The kernel's `event_mqtt_bridge` (in the app and in the sidecar) publishes
the kernel's state to your broker under the witness prefix (`witness`, the
`mqtt_publish.topic_prefix` option or `MQTT_TOPIC_PREFIX`). The integration
reads those topics; so can anything else on your broker.

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

### Kernel topics

| Topic | Retained | Payload |
|---|---|---|
| `witness/status` | yes (last will) | `online` / `offline` — the bridge's liveness; kernel entities follow it |
| `witness/<kernel id>/announce` | yes | the kernel's **signed announcement** (`v`, `device_id`, `kernel_version`, `verifying_key`, `api{scheme,bind,port[,host]}`, `pairing`, `mqtt_topic_prefix`, `discovery_prefix`, `availability_topic`, `announced_bucket_start`, `announced_bucket_size`, `sig`), Ed25519 by the kernel's device key over `securacv-announce\|v1\|` + the compact payload without `sig`. Published after `online`, republished on every reconnect, **cleared** (empty retained payload) on a clean shutdown. The integration discovers a sidecar by it and, once a key is pinned, refuses an announcement that is not signed by that key. `api.host` is present only when the operator set `SECURACV_ANNOUNCE_HOST` — a host is a network identifier |
| `witness/claim/<kernel id>` | yes | written by the **integration**: `{"v":1,"claimed":true,"nonce":"<16 hex>","claimed_bucket_start":<bucket start>}` on every load and hourly; an empty payload withdraws it. Never an entry id. The bridge honors a claim younger than 24 hours by withdrawing its MQTT-discovery configs, and republishes them when it clears |
| `witness/claim_state` | yes | `claimed` / `unclaimed` — what the bridge is doing about the claim |
| `witness/last_event` | yes | the newest sealed event (the payload below) |
| `witness/events` | no | every sealed event as it is published — the feed the per-camera event entities fire on. On a daemon start the bridge publishes from **one bucket back** (the previous and the current bucket), never the whole retained log; `--replay-from-genesis` (`REPLAY_FROM_GENESIS=1`) republishes everything since bucket zero as a migration. The integration fires an event entity only for a row whose bucket started within the last two bucket sizes; an older row updates the last-event sensor and nothing else |
| `witness/zone/<zone>/event` | no | the same event, per zone |
| `witness/zone/<zone>/count` | yes | the zone's event count since the bridge started (a restart counts from its start bucket, not from the whole log) |
| `witness/zone/<zone>/motion` | no | `ON` for the bridge's per-zone motion sensor (auto-off after 10 min) |
| `witness/digest` | — | the kernel's rolling 24-hour digest (`/digest`) |
| `witness/chain_problem`, `witness/chain_problem/attrs` | — | the kernel's **self**-verification: a problem flag and its `VerifyReport`; shown as `kernel_reported_chain_ok`, never as Verified |
| `witness/frigate_link` | yes | from `frigate_bridge`: `{"v":1,"device_id":"pwk_…","connected":true,"topic_prefix":"frigate","last_message_bucket_start":…,"last_message_bucket_size":600,"messages_since_start":…,"mode":"events"}` — republished on change, after a reconnect and at least every 300 s; `connected:false` on a clean shutdown ([the field table](frigate_integration.md#frigate-link-health-witnessfrigate_link)) |

The event payload on `witness/last_event`, `witness/events` and
`witness/zone/<zone>/event`, in wire order — time is a coarse bucket, never a
precise timestamp:

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "zone_id": "zone:front_door",
  "time_bucket": {"start_epoch_s": 1706140800, "size_s": 600},
  "time_bucket_start": 1706140800,
  "time_bucket_size": 600,
  "confidence": 0.85,
  "attestation": "adapter",
  "kernel_version": "0.8.0",
  "ruleset_id": "ruleset:frigate_v1",
  "device_id": "pwk_1a2b3c4d",
  "published_bucket_start": 1706141400,
  "published_bucket_size": 600,
  "verified_bucket_start": 1706140800,
  "verified_bucket_size": 600,
  "verified_ok": true,
  "seq": 1204
}
```

`verified_bucket_start`, `verified_bucket_size` and `verified_ok` are the
**kernel's** last self-verify (absent until one ran in this bridge process)
and become the `kernel_reported_chain_ok` / `kernel_verified_bucket_start`
attributes — the integration's own verdict is computed by its walk and never
read off this payload. `attestation` is `adapter` for a Frigate event (the
kernel's bridge attested it, not a device) and `ha-bridged` for an event that
transited Home Assistant; it is omitted when the kernel itself attested the
event.

### Before the integration claims the kernel

With `mqtt_publish.enabled` the bridge also publishes **MQTT discovery**
configs (`homeassistant/{sensor,binary_sensor,button}/<kernel id>/…/config`,
retained) so a Home Assistant with no SecuraCV integration still sees the
kernel through its core `mqtt` integration:

| Entity | Type | Description |
|--------|------|-------------|
| `sensor.pwk_<zone>_events` | Sensor | Total event count (state_class: total_increasing) |
| `binary_sensor.pwk_<zone>_motion` | Binary Sensor | Motion detected (auto-off after 10 min) |
| `sensor.pwk_last_event` | Sensor | Most recent event with full attributes |
| `sensor.pwk_daily_digest` | Sensor | The kernel's rolling 24-hour digest |
| `binary_sensor.pwk_chain_problem` | Binary Sensor | The kernel's **self**-report of a chain problem |
| `button.pwk_verify_now` | Button | Ask the kernel to verify itself |

The bridge asks Home Assistant for these ids with `default_entity_id`, which
Home Assistant 2025.10 and later honors when it first registers an entity;
`<zone>` is the zone id without its `zone:` prefix, as Home Assistant slugs
it (`zone:back-gate` becomes `back_gate`). An entity your Home Assistant
registered before keeps the id it has, and an older Home Assistant names the
entity from the device name and the entity name.

These entities show the kernel's **self-report**: the dashboard and the
blueprints label them *Chain OK (kernel-reported)*, never Verified. Once the
SecuraCV integration connects it claims the kernel and the bridge withdraws
every one of them (the state topics above are never paused); the
integration's `securacv_*` entities take over, and Settings → Repairs lists
the old → new ids once where they existed
([the migration table](#entity-ids-pwk_--securacv_)).

### Example Automation with the integration's entities

```yaml
# automations.yaml
automation:
  - alias: "Notify on a sealed front-door event"
    triggers:
      - trigger: state
        entity_id: event.securacv_front_door
    actions:
      - action: notify.mobile_app_your_phone
        data:
          title: "Witness event"
          message: >
            {{ state_attr('event.securacv_front_door', 'friendly_event') }}
            at the front door, {{ state_attr('event.securacv_front_door', 'verification') }}
            (confidence {{ state_attr('event.securacv_front_door', 'confidence') }})
```

The witness-notification blueprint does this per camera with the seal wait,
the proof line and the Frigate and export actions built in
([Step 5](#step-5-set-up-notifications)).

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
  SecuraCV → Configuration** (Network section), then enter
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

On the **Docker sidecar** the port is published from the first start —
that is how Home Assistant pairs — and the roll-call there serves **only the
kernel's own row** unless you set `SECURACV_FLEET_ROLLCALL=1` in the compose
file (`WITNESS_API_FLEET_ROLLCALL=full`): who is home is yours to opt into,
never a side effect of pairing. `GET /api/identity` reports the mode as
`fleet_rollcall`.

`online` on that roll-call is not a liveness proof: it means a signed chain
publish verified against the pinned key within the last 180 s, no more —
the exact wording is in
[`tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md).

---

## Manual Sensors (Alternative)

If you prefer not to use MQTT Discovery, you can create sensors manually using the REST API.

The app exposes an Event API on port 8799 inside its container. When Home
Assistant is running alongside the app, use the app hostname so HA can reach
it over the Supervisor network. The Supervisor derives that hostname from the full app
slug — repository hash plus app name, underscores becoming dashes — which for
this repository is `d0491a67_privacy_witness_kernel`, giving
`http://d0491a67-privacy-witness-kernel:8799`. You can confirm the hostname
in **Settings → Apps → SecuraCV → Info**, where Home Assistant
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

### REST Sensor (Event Count)

The per-zone count sensors are MQTT Discovery entities, so without MQTT
there is no count to read from another entity. Ask the app's `/digest`
endpoint instead: its `event_types` map counts each event type over the last
24 hours (a rolling window, not since midnight).

```yaml
# configuration.yaml
sensor:
  - platform: rest
    name: "PWK Boundary Crossings (24 h)"
    resource: http://d0491a67-privacy-witness-kernel:8799/digest
    headers:
      Authorization: Bearer YOUR_API_TOKEN
    value_template: >-
      {{ (value_json.event_types.BoundaryCrossingObjectLarge | default(0) | int)
         + (value_json.event_types.BoundaryCrossingObjectSmall | default(0) | int) }}
    unit_of_measurement: "events"
    icon: mdi:walk
    scan_interval: 300
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
when running as the Home Assistant app (the Supervisor resolves that
hostname), at `http://<docker host>:8799` on the Docker sidecar, or wherever
you configured a kernel of your own. The whole surface is JSON; errors are
`{"error":"<snake_case_code>"}` with an optional `retry_after` or `detail`.

### Authentication

Three credentials open the API, all presented the same way — `Authorization:
Bearer <64 hex>` (or `x-witness-token: <64 hex>`), never in the query string
(`?token=` is refused with `400 token_query_param_not_allowed` for every
credential):

- **The capability token** rotates every 10 minutes and is written to the
  kernel's token file — `/config/api_token` in the app, `/data/api_token` in
  the sidecar — so a client that can read the file re-reads it on every
  `401`. The token rotated out last is still accepted for 90 seconds, so a
  reader a moment behind is not refused. The integration handles this by
  itself when configured with the token-file path (the app's default).
- **A paired token** is what the Docker sidecar hands Home Assistant for a
  6-digit pairing code ([Pairing](#pairing-the-docker-sidecar-and-home-assistant)):
  32 random bytes, 64 hex on the wire, long-lived, kept by the kernel only as
  a sha256 in `paired_tokens.json` (`0600`, re-read per request so a
  revocation lands on the next call), and admitted to every route the
  capability token opens. The only differences are its receipts — an export
  under a paired token is receipted `auth_mode: paired` with the token id —
  and the revocation rule below.
- **A viewer token** opens exactly one route, `GET /api/sealed-log`
  ([Viewer tokens](#viewer-tokens-witness-wall)).

Every failed credential is a strike against the caller's address, and
enough strikes lock that address out with `429 {"error":"rate_limited",
"retry_after":…}` before the request is even read; a capability or paired
success clears the address's history, a viewer success does not. The kernel
reads a refused request's body (up to 64 KiB) before answering, so a client
gets its `400` or `401` rather than a connection reset. `GET /health` and the
fleet roll-call `GET /api/fleet` answer without a token; `POST /api/pair`
takes the pairing code as its credential. Every other route takes a token.

```bash
# Read the token
TOKEN=$(cat /config/api_token)

# Make authenticated requests (Bearer token only)
curl -H "Authorization: Bearer $TOKEN" http://d0491a67-privacy-witness-kernel:8799/api/identity
```

### Pairing (the Docker sidecar and Home Assistant)

`docker exec securacv pair` runs `witness_api issue-pairing-code` inside the
container: the kernel writes `pairing_code` (`0600`, beside the token file)
holding the code's **sha256**, its expiry (600 s by default,
`WITNESS_API_PAIRING_TTL_SECS`), a label and `attempts_left: 5`, and prints
the clear code once, with the kernel key fingerprint (the first eight hex of
sha256 over the raw 32-byte key — the same eight the integration shows
before it pairs, and the **Kernel key fingerprint** sensor afterwards). Only
one code is outstanding at a time; issuing again replaces it.

`POST /api/pair` with `{"code":"483921","label":"home-assistant"}` (body ≤
1024 bytes; `label` optional, ≤ 64 characters) redeems it. On success the
kernel deletes the code file **before** it writes the new bearer's hash to
`paired_tokens.json` and **before** it answers
`{"token","token_id","device_id","verifying_key","kernel_version"}` — the
token's only copy. Every refusal of a code — wrong, never issued, burned,
expired, or pairing not configured on this kernel (the app runs with pairing
off) — is the **same** `401 {"error":"invalid_pairing_code"}`, so an attacker
learns nothing about which it was, and each is a strike against the
address. Each wrong guess decrements `attempts_left` **durably before** the
refusal is written; at zero the file is deleted (the code is burned) and an
expired file is deleted on first touch. Malformed input (`400 bad_request`,
`body_required`, `body_too_large`) is not a strike. A pairing success does
not clear the address's strikes — only the first authenticated request with
the new token does.

Capacity: the paired set is bounded at **32 tokens** (`PAIRED_TOKEN_LIMIT`;
the file is read under a 64 KiB bound, past which every paired client would
be refused at once). A full set answers the **right** code with
`403 {"error":"paired_token_limit"}` and leaves the code file and its
attempt budget untouched — so the operator revokes one and enters the same
code — while a wrong guess against a full set is still the constant 401 and
a strike (the capacity answer never reaches a guesser). The integration
shows it as its own form error naming `unpair`.

Revocation: `DELETE /api/pair/<id>` (eight lowercase hex) answers
`200 {"revoked":"<id>"}`; it is accepted with the capability token for any
id, or with a paired token for **its own** id only — any other id is
`403 {"error":"forbidden"}`; an unknown id, or a kernel with pairing off, is
`404 not_found`. From the Docker host: `docker exec securacv unpair <id>`
(`witness_api revoke-paired-token`); `unpair` alone lists the paired tokens
(`witness_api list-paired-tokens`: id, label, minted — never the hash). The
daemon's log never prints a code or a token; `doctor` reports only *pairing
code outstanding: yes (expires in N min)* and *paired: yes (N tokens)*.

Environment: `WITNESS_API_PAIRED_TOKEN_PATH` / `WITNESS_API_PAIRING_CODE_PATH`
(both or neither; defaults `paired_tokens.json` and `pairing_code` beside
the token file — no token path means pairing is off and `/api/pair` answers
the constant `401`), `WITNESS_API_PAIRING_TTL_SECS` (600), and
`WITNESS_API_FLEET_ROLLCALL` (`full` | `self`; the sidecar sets `self`
unless `SECURACV_FLEET_ROLLCALL=1`).

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
commands (`docker exec securacv entrypoint.sh mint-viewer-token …`,
see [frigate_integration.md](frigate_integration.md)); the Home Assistant
app reads the file but has no control that mints one yet.

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Check daemon health (unauthenticated): `{"status":"ok"}` |
| `/api/fleet` | GET, OPTIONS | The fleet roll-call the Witness Wall reads — coarse words only (name, online, chain verdict, product, presence/occupants/breathing while proven online); **no token**, an origin allow-list for browsers; under `WITNESS_API_FLEET_ROLLCALL=self` (the sidecar's default) only the kernel's own row; see [Witness Wall: the fleet roll-call](#witness-wall-the-fleet-roll-call) |
| `/api/pair` | POST | Redeem a 6-digit pairing code for a paired bearer — no bearer token on this one route, the code in the body is the credential, and every refusal of a code is the identical `401 invalid_pairing_code` and a strike; the right code against a full set of 32 paired tokens is `403 paired_token_limit`, the code kept ([Pairing](#pairing-the-docker-sidecar-and-home-assistant)) |
| `/api/pair/<id>` | DELETE | Revoke a paired token by its 8-hex id; capability token for any id, a paired token for its own id only |
| `/api/identity` | GET | The kernel's identity, receipt-less: `v`, `device_id` (`pwk_…`), `verifying_key` (64 hex), `key_algorithm`, `kernel_version`, `ruleset_id`, `chain_length`, `latest_hash`, `last_verify` (`null`, or `at` as a bucket start, `bucket_size`, `ok`, `verdict`, `identity_verified` — the kernel's own self-check), `fleet_rollcall`, `paired_tokens`. The integration pins `verifying_key` from this answer (or from `/api/pair`), never from the MQTT announcement. Capability or paired token |
| `/api/sign-announce` | POST | Sign the MQTT announcement for the bridge, which holds no key: body ≤ 2048 bytes, a JSON object without `sig` whose `device_id` and `verifying_key` are this kernel's; `{"sig":"<128 hex>"}`, Ed25519 over `securacv-announce\|v1\|` + the exact body bytes. Capability or paired token |
| `/events` | GET | Without a query: the receipted bulk export of events as batched buckets. With `?since_bucket=<epoch_s>`: a receipt-less **state read** of events whose bucket start is at or after it, each carrying its `seq` — the cursor the bridge polls every cycle, so its polling adds no receipts |
| `/events/latest` | GET | The most recent event (single event JSON, with `seq`); receipt-less |
| `/digest` | GET | Coarse activity digest for dashboards (bucketed counts, no per-event detail); receipt-less |
| `/status` | GET | Daemon status snapshot — `kernel_version`, `storage` (free space of the database directory sampled every 600 s, wear and write rate where the host exposes them, else `null`), `thermal`, `time_bucket`; `404 not_available` only until the first sample. In Frigate mode `witness_api` attaches the sampler itself, so the app's and the sidecar's storage entities read |
| `/verify` | POST | Run the kernel's **own** sealed-log verification and return the `VerifyReport` — a self-report (shown as `kernel_reported_chain_ok`), never Home Assistant's "Verified" |
| `/export/bundle` | GET | Receipted export bundle (`?last=24h`, or `?start=&end=`; events reshaped for disclosure, correlation tokens stripped) — what `securacv.export_evidence` downloads verbatim; under a paired token the receipt reads `auth_mode: paired` with the token id |
| `/api/sealed-log` | GET | Checkpoint-anchored sealed-log tail for read-only verifiers — stored bytes verbatim, size-capped, each entry with its `seq`, plus `head: {seq, entry_hash}`. One query parameter, the incremental cursor `?from_seq=<n>` (entries with `seq > n`; when row `n` is in the served tail its hash becomes the anchor; `head.seq < n` means the log was rolled back); anything else in a query string is ignored — the log is non-queryable by design. Receipt-less. The one route a [viewer token](#viewer-tokens-witness-wall) also opens; capability and paired tokens open it too |

Receipts, in one line: `GET /events` without a cursor and `GET /export/bundle`
are disclosures and log a tamper-evident receipt each; `/events/latest`,
`/events?since_bucket=`, `/digest`, `/api/identity` and `/api/sealed-log`
are state reads and log none. Receipts are never pruned.

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

## Troubleshooting the app

### App Won't Start

1. Check logs: **Settings → Apps → SecuraCV → Logs**
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

The witness kernel runs as a separate process (the SecuraCV app) for isolation:

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

The app updates independently of Home Assistant, and an updated app
refreshes its own carried copy of the integration on start (never a copy
from HACS), then asks for a restart with a notification:

1. Go to **Settings → Apps → SecuraCV**
2. Click **Update** when available

Update notes are in the [CHANGELOG](../CHANGELOG.md).

---

## Getting Help

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
- [Home Assistant Community](https://community.home-assistant.io/)
- [Documentation](../README.md) · [the product design doc](design/ha_frigate_product.md)
