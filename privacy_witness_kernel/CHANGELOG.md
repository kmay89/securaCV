<!-- The Supervisor shows this file on the app's page after an update. Newest first. -->
## 0.9.0

The app is now "SecuraCV" in the App Store (the slug `privacy_witness_kernel`
is unchanged, so existing installs upgrade in place).

- **Frigate-aware setup wizard.** With Frigate installed, the wizard reads
  Frigate's cameras from its own config and asks for nothing; the RTSP screen
  is only for installs without Frigate. New settings: how long to keep sealed
  events (7 / 30 / 365 days), what to seal (every tracked object, or only
  Frigate alerts), and the daily digest. The pattern-break and integrity
  toggles nothing consumed are gone.
- **"Install it for me" is a link** into your own App Store (my.home-assistant.io).
  The app makes no Supervisor install calls and needs no `hassio_role`.
- **The integration is carried in the app** (option `install_integration`,
  default on): the done screen's *Install the integration and restart Home
  Assistant* copies it on your click; app starts update only the app's own,
  unedited copy and ask for a restart with a notification — never a copy from
  HACS, never an automatic restart, never a downgrade.
- **Re-running the wizard works** and keeps the device key; the Configuration
  tab's other settings survive a re-run.
- **Camera names** are slugged one way everywhere (`Front Door` →
  `front_door`), so a typed name with a space no longer silently drops every
  event of that camera.
- **Blueprints ship in the image** and are placed under
  `/config/blueprints/automation/securacv/` (an edited copy is never
  overwritten), so the digest automation can be created on a store install.
- **Supervision.** Each process (kernel, Frigate bridge, publisher, panel) is
  an s6 service with a finish script; a crash is visible and the Supervisor
  watchdog (`/api/health`) restarts the app when the kernel stops answering.
  The unconfigured app runs the wizard alone without flapping.
- **Store presentation:** documentation tab, option translations, icon and
  logo, Home Assistant floor 2026.5.0, typed `map:` entries with explicit
  paths (every existing path unchanged; `ssl` dropped), a custom AppArmor
  profile, official pinned base image, no `build.yaml`.
- **Discovery payload** now carries `token_file`, `mqtt_topic_prefix` and
  `device_id` beside host and port, and the MQTT announce says the API binds
  all interfaces and that Home Assistant reads the token file.
- `verify_interval_hours` defaults to 1 (was 24).
- Entity ids: once the integration connects, the MQTT-discovery `pwk_*`
  entities withdraw in favor of the integration's `securacv_*` entities — see
  the Documentation tab's migration table.

## 0.8.0 and earlier

See the repository's `CHANGELOG.md`.
