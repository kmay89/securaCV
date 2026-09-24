# Privacy Witness Kernel - Home Assistant App

Privacy-preserving video surveillance that produces **claims, not recordings**.

> Home Assistant now calls these **apps**; older Home Assistant labels the same
> screens "Add-ons." The steps below are identical either way.

## Quick Start

1. Add this repository to Home Assistant:
   - Go to **Settings → Apps → App Store**
   - Click ⋮ → **Repositories**
   - Add: `https://github.com/kmay89/securaCV`

2. Install "Privacy Witness Kernel"

   Installation **pulls a pre-built image** (`ghcr.io/kmay89/<arch>-addon-privacy_witness_kernel`,
   published for `amd64` and `aarch64` by the [`Add-on image`](../.github/workflows/addon-image.yml)
   workflow) — the Supervisor does **not** compile the Rust kernel on your device, so installs are
   fast even on a Raspberry Pi. No `curl | bash`. The image is built and verified entirely from this
   repo on GitHub; nothing is fetched from a third party at runtime (local-only custody, Inv. IV).

3. Start the app and open its **Web UI** — the setup wizard handles the
   rest. There is nothing to type for a standard setup:
   - **Prerequisites**: the wizard checks for Mosquitto and Frigate and
     offers an **"Install it for me"** button for each one that is missing
     (Mosquitto is installed and started; Frigate's add-on repository is
     registered and the add-on installed, ready to start after setup)
   - **Cameras**: pre-filled from go2rtc when it is reachable — review the
     rows instead of typing RTSP URLs
   - **Device key**: auto-generated (persisted to
     `/config/.securacv/device_key`; included in HA backups — back it up)
   - **MQTT broker**: auto-discovered from the Supervisor when the
     Mosquitto app is installed (host, port, credentials)
   - **HA entities**: created automatically via MQTT Discovery
   - **Daily digest**: when the SecuraCV digest blueprint is installed and
     a Home Assistant companion app is registered, the wizard creates the
     digest automation for you; when either is missing, the done screen
     says so and names the manual step instead of pretending

   Manual YAML is only needed for an external broker or a non-default
   Frigate `topic_prefix` — see
   [docs/frigate_integration.md](../docs/frigate_integration.md).

After setup, the same Web UI is a **status panel**: chain-integrity badge,
24-hour digest, a **Verify now** button, and a dashboard generator that
emits Lovelace YAML for your live zones.

### Where the wizard writes Frigate's config

The current Frigate add-on reads its config from
`/addon_configs/ccab4aaf_frigate/config.yml`, and that is where the wizard
writes the generated camera config. An existing `config.yml`/`config.yaml`
is **never overwritten** — the wizard writes `config.yml.new` beside it and
tells you, so you can merge or rename deliberately. When the add-on config
directory does not exist (Frigate not installed yet, or a legacy setup),
the config goes to the legacy `/config/frigate.yml` path and the done
screen names both locations.

### Event API exposure

The kernel's Event API listens on port 8799 **inside the add-on's
container**, reachable by other add-ons and HA core over the Supervisor's
internal network at `http://d0491a67-privacy-witness-kernel:8799` — that is
how the SecuraCV integration connects. The **host port mapping ships
disabled**, so nothing on your LAN can reach the API unless you enable the
port in the add-on's Network settings. Every data endpoint requires the
rotating capability token from `/config/api_token` regardless of where the
request comes from; only `/health` and the fleet roll-call `GET /api/fleet`
answer without it. After startup the add-on announces the API to the
Supervisor (discovery), so the SecuraCV integration appears under
**Settings → Devices & Services** without typing a URL.

**Witness Wall (Apple TV).** The same API answers `GET /api/fleet`, the
roll-call the tvOS Witness Wall reads: the kernel's own row first, then
every Canary the add-on's MQTT publisher has heard on the broker. The add-on
wires that itself — `run.sh` points both the kernel and `event_mqtt_bridge`
at `/config/fleet_peers.json` — so there is no option to set. Three honest
limits. Peers appear only while `mqtt_publish.enabled` is on (the default):
with it off no bridge listens for Canaries, the roll-call lists the kernel
alone (the kernel is not pointed at the file, so rows a past bridge pinned
are kept for their keys, not served), and the add-on log says so at startup. The add-on advertises no
`_securacv._tcp` Bonjour service and, as above, ships the 8799 host port
disabled, so the Wall cannot find it on its own: enable the port in the
add-on's Network settings and type `http://<your-home-assistant-host>:8799`
into the Wall once — with the port, because the Wall adds only `http://` to
a bare host and would otherwise poll port 80 (a typed hub is remembered).
Enabling that port also opens the roll-call to anything on your LAN:
`/api/fleet` is the one endpoint that answers without the token, and it
serves name, online, chain verdict, product, and the per-room
presence/occupants/breathing words while a peer is proven online — the
posture [`docs/security/THREAT_MODEL.md`](../docs/security/THREAT_MODEL.md)
states for the one open read on the hub; every other endpoint still wants
the token. And the summary file sits under `/config`,
so it is part of every HA backup — on purpose, because it holds the public
key pinned on first sight for each Canary (a restore keeps that trust
instead of re-pinning) and the per-room wellbeing words the Wall shows; it
is written `0600`, and `/api/fleet` serves the roll-call's coarse words
only, never the keys. `online` there is not a liveness proof: it means a
signed chain publish verified against the pin within the last 180 s, no
more — see [`tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md).

Listed is not verified: for the Wall to walk the sealed log itself and say
**Verified**, it needs a viewer token — a credential good for
`GET /api/sealed-log` alone, minted once with `witness_api
mint-viewer-token` (see
[the API reference](../docs/homeassistant_setup.md#viewer-tokens-witness-wall)).
The add-on's kernel honors one as soon as it appears in
`/config/viewer_tokens.json` (read per request, `0600`), but the add-on has
no control that mints one yet, so on an add-on install the Wall shows the
roll-call and not a walk of its own.

## Features

- **Zero-config Frigate mode** - broker auto-discovery, auto device key
- **Auto-discovers cameras** from go2rtc/Frigate
- **Local processing** - no cloud, no external servers
- **Privacy by design** - produces event claims, not searchable recordings
- **Cryptographically signed** - tamper-evident event log
- **One-click verification** - `button.pwk_verify_now` in HA, plus a
  scheduled check every `verify_interval_hours` (default 24)
- **Daily digest** - `sensor.pwk_daily_digest` with per-zone counts and
  coarse day periods; deliver it to your phone with the
  [daily digest blueprint](../docs/blueprints/securacv_daily_digest.yaml)
- **Configurable retention** - automatic cleanup of old events

## Distribution & HACS

### Current Distribution

This repository ships both:
- A **Home Assistant app** (custom app repo URL) that runs the Privacy Witness Kernel service.
- A **Home Assistant integration via HACS** that connects to the kernel’s Event API.

### HACS Integration (Scope)

HACS support is intentionally limited:
- **Config flow + entities only**
- **No new data fields** and no expansion of the event schema
- No change to privacy guarantees or retention behavior

### HACS vs App (Quick Comparison)

| Aspect | App (today) | HACS (current) |
|--------|----------------|----------------|
| Runs the kernel service | ✅ Yes | ❌ No (frontend/config only) |
| Configuration location | App config UI | HA config flow |
| Entities in HA | ✅ Yes (via MQTT/REST) | ✅ Yes (same entities) |
| Data schema changes | ❌ Not allowed | ❌ Not planned |

## What Events Look Like

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "zone_id": "zone:front_door",
  "time_bucket": { "start_epoch_s": 1706140800, "size_s": 600 },
  "confidence": 0.85
}
```

**Note:** No faces, no license plates, no precise timestamps, no raw video.

## Documentation

- [Full Setup Guide](../docs/homeassistant_setup.md)
- [RTSP Configuration](../docs/rtsp_setup.md)
- [Privacy Architecture](../spec/invariants.md)

## Support

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
