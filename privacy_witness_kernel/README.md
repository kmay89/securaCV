# SecuraCV — Home Assistant app

The verified witness log for Frigate and Home Assistant. **Claims, not
recordings:** every event Frigate reports is sealed into a signed,
tamper-evident chain as a coarse claim — never a face, a plate, a name or a
frame.

> Home Assistant now calls these **apps**; older Home Assistant labels the same
> screens "Add-ons." The steps below are identical either way. The app's slug
> is still `privacy_witness_kernel`; "Privacy Witness Kernel" is the kernel's
> model name in Home Assistant.

The user-facing documentation the App Store shows is [`DOCS.md`](DOCS.md);
release notes are in [`CHANGELOG.md`](CHANGELOG.md). This file is the
developer's map of the directory.

## Quick Start

1. Add this repository to Home Assistant: **Settings → Apps → App Store → ⋮ →
   Repositories** → `https://github.com/kmay89/securaCV`.
2. Install **SecuraCV**. Installation pulls a pre-built image
   (`ghcr.io/kmay89/<arch>-addon-privacy_witness_kernel`, published for
   `amd64` and `aarch64` by [`Add-on image`](../.github/workflows/addon-image.yml));
   nothing is compiled on your device and nothing is fetched from a third
   party at runtime.
3. Start the app and open its **Web UI**. The wizard checks Mosquitto and
   Frigate (links into your App Store for anything missing), reads Frigate's
   cameras from its config, asks what to seal and for how long, and offers
   **Install the integration and restart Home Assistant** on its done screen.
   After the restart: Settings → Devices & Services → *SecuraCV discovered* →
   Configure, nothing to type.

## What is in this directory

| File | Role |
|---|---|
| `config.yaml` | The app definition: options, schema, `map:`, `watchdog:`, `homeassistant:` floor, `discovery:`, ingress |
| `translations/en.yaml` | Option and port names/descriptions for the Configuration tab |
| `Dockerfile` | Multi-stage build: Rust kernel (cargo-chef) → pruned integration carry → official HA base runtime (s6-overlay) |
| `rootfs/etc/services.d/*/{run,finish}` | The s6 services: `panel`, `witness_api`, `witnessd`, `frigate_bridge`, `event_mqtt_bridge`, `discovery` |
| `run.sh` | The one-time setup step (cont-init): renders the kernel config, the services' argv and `/run/securacv/setup.sh`, installs blueprints, updates the app's own integration copy |
| `run_lib.sh` | Pure, bashio-free helpers (tested by `tests/test_run_lib.sh`): value resolution, device id, discovery payload (C6), blueprint install |
| `serve_wizard.py` | The ingress panel: setup wizard, status panel, `/api/health` watchdog probe, proxies to the kernel and to Canaries |
| `integration_carry.py` | Option G: places/updates the carried integration under `/config/custom_components` (marker + sha256 manifest; never HACS/edited copies; no restarts) |
| `wizard/index.html` | The page; its `setup-flow` section is pure logic the node tests lift |
| `apparmor.txt` | Custom profile: one child profile per process, writes limited to the paths `DOCS.md` lists |
| `discover_cameras.sh` | go2rtc camera discovery (jq twin of the wizard's Python reference) |
| `verify_published_image.sh` | Release gate: every declared arch publicly pullable from GHCR |
| `icon.png`, `logo.png` | Store art (128×128 icon, 250 px wide logo), from `brands/` |

## Tests

```sh
python3 -m pytest privacy_witness_kernel/tests -q
node --test privacy_witness_kernel/tests/test_wizard_mesh_pairing.test.js
node --test privacy_witness_kernel/tests/test_wizard_setup_flow.test.js
bash privacy_witness_kernel/tests/test_run_lib.sh
bash privacy_witness_kernel/tests/test_config_render.sh
shellcheck --shell=bash --exclude=SC1091 privacy_witness_kernel/run.sh privacy_witness_kernel/run_lib.sh \
  privacy_witness_kernel/rootfs/etc/services.d/*/run privacy_witness_kernel/rootfs/etc/services.d/*/finish
```

CI runs all of it in [`pwk-wizard-tests.yml`](../.github/workflows/pwk-wizard-tests.yml);
the image build (and the check that the carried integration's version equals
the app's) is [`addon-image.yml`](../.github/workflows/addon-image.yml).

## Event API exposure

The kernel's Event API listens on port 8799 **inside the app's container**,
reachable by HA Core over the Supervisor's internal network at
`http://d0491a67-privacy-witness-kernel:8799` — how the SecuraCV integration
connects (the discovery service announces host, port, the token file path,
the MQTT state prefix and the device id to the Supervisor once the kernel
answers `/health`). The **host port mapping ships disabled**. Every data
endpoint requires the rotating capability token from `/config/api_token`;
only `/health` and the fleet roll-call `GET /api/fleet` answer without it.

**Witness Wall (Apple TV).** The same API answers `GET /api/fleet`, the
roll-call the tvOS Witness Wall reads: the kernel's own row first, then every
Canary the app's MQTT publisher has heard on the broker. Peers appear only
while `mqtt_publish.enabled` is on (the default). The app advertises no
Bonjour service and ships the 8799 host port disabled, so the Wall cannot find
it on its own: enable the port in the app's Network settings and type
`http://<your-home-assistant-host>:8799` into the Wall once. Enabling that
port opens the roll-call (names, online state, coarse per-room words; no keys,
no events) to anything on your LAN — the posture
[`docs/security/THREAT_MODEL.md`](../docs/security/THREAT_MODEL.md) states;
every other endpoint still wants the token. The roll-call file
`/config/fleet_peers.json` is written `0600` and is part of every backup on
purpose (it holds the public key pinned on first sight for each Canary). See
[`tvos/discovery/DISCOVERY.md`](../tvos/discovery/DISCOVERY.md).

## What Events Look Like

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "zone_id": "zone:front_door",
  "time_bucket": { "start_epoch_s": 1706140800, "size_s": 600 },
  "confidence": 0.85
}
```

No faces, no license plates, no precise timestamps, no raw video.

## Documentation

- [Full Setup Guide](../docs/homeassistant_setup.md)
- [Frigate integration](../docs/frigate_integration.md)
- [Privacy Architecture](../spec/invariants.md)

## Support

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
