# SecuraCV

The verified witness log for Frigate and Home Assistant.

Frigate keeps the video. SecuraCV keeps the proof: every person, car, dog or
cat event Frigate reports is sealed into a signed, tamper-evident chain as a
coarse claim — which camera, which ten-minute window, what kind of thing —
and never a face, a plate, a name or a frame. Home Assistant shows the chain's
state, one witness event entity per camera, a daily digest, and a one-tap
signed evidence export anyone can verify offline.

The app runs the witness kernel. The SecuraCV **integration** (the thing that
puts entities in Home Assistant) is carried inside the app and installed on
your click, or comes from HACS — your choice, see below.

## The three ways in

**Home Assistant OS (this app).** Install, start, open the Web UI. The wizard
finds Mosquitto and Frigate, reads Frigate's cameras from its own config, and
has nothing for you to type. Its done screen offers *Install the integration
and restart Home Assistant* (one click). After the restart, Settings → Devices
& Services shows a *SecuraCV discovered* card: press Configure, nothing to
type. Walk past a camera: the first *Sealed ✓* event appears within a minute;
*Verified ✓* follows the next chain check.

**HACS first.** Already have the integration from HACS? Install this app the
same way; the wizard sees the integration is installed and skips the install
step. Or start from HACS → SecuraCV → Add Integration: it finds this app by
itself.

**Home Assistant Container + the Docker sidecar.** No Supervisor, no app. Run
the sidecar beside Frigate (`docker compose up -d`), then pair it from the
integration with the 6-digit code `docker exec securacv pair` prints. The
product is the same from there on.

## Before you start

- **Mosquitto** (the official MQTT broker app) running. The wizard links to
  its App Store page if it is missing.
- **Frigate** (the Frigate NVR app, repository
  `https://github.com/blakeblackshear/frigate-hass-addons`) with MQTT
  enabled. Also linked from the wizard if it is missing. No Frigate yet? The
  wizard asks for your cameras' RTSP URLs and writes Frigate's camera config
  for you.
- Home Assistant 2026.5.0 or newer.

## What the wizard does, step by step

1. **Prerequisites.** Mosquitto running? Frigate installed and running? Its
   cameras are read from `/addon_configs/ccab4aaf_frigate/config.yml`. The
   broker credentials come from the Supervisor — never typed, never shown.
   Anything missing is a link into your own App Store ("Install it for me" is
   a link on purpose: the app holds no Supervisor role and cannot install,
   start or stop other apps).
2. **Cameras.** With Frigate: a confirmation ("Frigate is running with 3
   cameras — front_door, driveway, backyard — and Mosquitto is connected.
   Nothing to type."). Without Frigate: name + RTSP URL per camera. Name
   cameras by place, not by person.
3. **Settings.** How long to keep sealed events (7 days, 30 days, 1 year —
   exports are self-contained, nothing is lost by exporting first). What to
   seal: every tracked object (default), or only Frigate alerts. The daily
   digest and its time.
4. **Confirm.** The device signing key: generated once, stored at
   `/config/.securacv/device_key` (in your Home Assistant backups). Back it
   up. Re-running the wizard keeps it.
5. **Done.** What actually happened — Frigate config written or left alone,
   digest automation created or honestly skipped and why — and the one next
   step in Home Assistant.

Re-run the wizard any time from the panel. It keeps your device key and the
Configuration tab's other settings.

## The integration carried in this app

The app image contains the SecuraCV integration (`custom_components/securacv`,
the same version as the app). This is unusual for an app, so here is exactly
what it does and does not do:

- **Nothing is copied until you click** *Install the integration and restart
  Home Assistant* on the wizard's done screen (or the same button on the
  panel). That click copies the integration into
  `/config/custom_components/securacv` and marks the copy as the app's own,
  then restarts Home Assistant Core — the only time the app ever restarts
  Core, and only on that click.
- **On each app start** the app updates **its own copy only**, and only to a
  newer version. It then raises a persistent notification *"SecuraCV
  integration x.y installed. Restart Home Assistant to load it."* It never
  restarts Home Assistant by itself.
- **A copy HACS or you installed is never touched.** The app recognizes its
  own copy by a marker file and a checksum manifest; an unmarked copy, or one
  you edited, is left alone — HACS (or you) is the updater from then on.
- **Never a downgrade.** A newer copy stays.
- Turn it all off with the `install_integration` option (Configuration tab);
  the done screen then links to HACS instead.

To stop using the app's copy: install the integration from HACS (HACS replaces
the tree and the marker goes with it), or delete
`/config/custom_components/securacv` and restart Home Assistant.

## Where things are

| What | Where |
|---|---|
| The sealed witness log | `/config/witness.db` |
| The device signing key | `/config/.securacv/device_key` (0600; back it up) |
| The rotating API token the integration reads | `/config/api_token` |
| The Frigate camera config the wizard writes (no Frigate yet) | `/addon_configs/ccab4aaf_frigate/config.yml`, never over an existing one (`config.yml.new` beside it) |
| The bundled blueprints | `/config/blueprints/automation/securacv/` (an edited copy is never overwritten) |
| The carried integration, when you asked for it | `/config/custom_components/securacv/` |
| The vault (break-glass only) | `/share/witness_vault/` |

## Options

Every option is explained in the Configuration tab. The ones that matter
day to day:

- `mode`: `frigate` (recommended; the wizard sets it) or `standalone` (the
  kernel reads one RTSP camera itself).
- `frigate.cameras`: camera slugs to seal (empty = every camera Frigate
  reports); `frigate.enable_reviews`: seal only Frigate alerts.
- `retention_days`: how long sealed events stay.
- `verify_interval_hours`: how often the kernel re-walks its own chain
  (hourly by default; the integration verifies independently).
- `install_integration`: see above.

The 8799 port mapping ships disabled: the integration reaches the kernel over
the internal add-on network. Enable it only for a client outside Home
Assistant (the Apple TV Witness Wall); every data endpoint still needs the
token.

## Entity ids: the migration note

Earlier releases showed the kernel in Home Assistant through MQTT discovery as
`sensor.pwk_*`, `binary_sensor.pwk_*` and `button.pwk_verify_now`. Once the
SecuraCV integration connects, it claims the kernel and those MQTT-discovery
entities withdraw; the integration's own entities take over:

| Before | Now |
|---|---|
| `sensor.pwk_last_event` | `sensor.securacv_last_event` |
| `sensor.pwk_daily_digest` | `sensor.securacv_daily_digest` |
| `binary_sensor.pwk_chain_problem` | `binary_sensor.securacv_chain_valid` |
| `button.pwk_verify_now` | `button.securacv_verify_now` |
| `sensor.pwk_<zone>_events` | `event.securacv_<zone>` |

Home Assistant raises a Repairs notice listing the ids the first time this
happens. Re-point automations and dashboards that named the old ids; the
bundled blueprints already use the new ones. The dashboard comes from the
strategy: Settings → Dashboards → Add dashboard → SecuraCV.

## Revoking and uninstalling

- **Revoke Home Assistant's access:** the integration reads the rotating
  token file; removing the SecuraCV integration entry in Home Assistant ends
  its access, and the kernel rotates the token every ten minutes regardless.
- **Uninstall the app:** Settings → Apps → SecuraCV → Uninstall. The witness
  log, the device key and the token stay under `/config` (they are yours and
  part of your backups); delete `/config/witness.db*`, `/config/.securacv`,
  `/config/api_token`, `/config/fleet_peers.json` and
  `/config/witness_config.json` if you want them gone. The integration copy
  under `/config/custom_components/securacv` and the blueprints stay too —
  remove them the same way if you no longer want them.
- **Export first** (the panel's *Download my events*, or the integration's
  *Export evidence*): the bundle verifies offline without the kernel.

## Security posture, in one paragraph

No `hassio_role`: the app calls only its own options, the shared MQTT service,
the `/addons/<slug>/info` reads the preflight needs, the discovery endpoint,
and Home Assistant's own API (for the one restart you click and the digest
automation). A custom AppArmor profile confines each process to the paths
listed above. The panel answers only the Supervisor's ingress proxy and its
own loopback. Secrets never reach the browser: `/api/status` masks the device
key and every broker password. The kernel's data endpoints require the
rotating token; only `/health` and the fleet roll-call `GET /api/fleet` answer
without it. The sealed log holds coarse claims only — see the repository's
`spec/invariants.md`.

## Support

- Issues: https://github.com/kmay89/securaCV/issues
- The repository's `docs/homeassistant_setup.md` and
  `docs/frigate_integration.md` go deeper.
