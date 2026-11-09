<!-- The Supervisor shows this file on the app's page after an update. Newest first. -->
## Unreleased

- **The device key leaves `/config`.** The seed the sealed log's identity
  derives from was kept at `/config/.securacv/device_key`, mode 0600 — which
  protects nothing there: every Home Assistant backup carries `/config`, and
  the Samba, SSH and File editor apps (all root) export it whole, so anyone
  holding a backup could forge the log's identity. It now lives in the app's
  own data (`/data/device_key`), which only this app's processes open and
  which a configuration-only backup does not carry (a backup that includes
  the app does; uninstalling the app removes it, so export or copy the seed
  from the Configuration tab first; a key the app generated itself is now
  recorded in the `device_key_seed` option on start, so that tab always
  shows it). On the first start after the update an
  existing `/config` copy is moved: copied byte-for-byte, held against the
  sealed log with the kernel's own verifier (the seed must open the log and
  derive its key), and only then removed, with one line in the log. A key
  the log refuses is kept in both places and the log says why. The API token
  the integration reads stays at `/config/api_token`.
- **The kernel's own copy of the key leaves `/config` too.** In standalone
  mode the kernel wrote the seed a second time, beside the log at
  `/config/witness.ed25519.seed`, at its first start — the same key the
  move above took out of `/config`, back inside every backup. The app now
  starts the kernel with `DEVICE_KEY_SEED_PERSIST=0`, a new kernel switch
  that says the seed is kept elsewhere: the kernel uses the seed it is given,
  writes no copy, and still refuses one that disagrees with a copy that
  exists. The first start after the update removes an existing copy once it
  is shown to hold, byte for byte, the key in use (one line in the log; the
  key itself is never logged). A copy holding a different key is kept, with
  a warning, and the kernel says why it will not open the log. The kernel's
  AppArmor profile no longer lets any kernel process write that file. After
  one start on this release, nothing under `/config` holds the key.
- **The two Frigate lists are declared as lists.** The options schema gave
  `frigate.cameras` and `frigate.labels` the Supervisor's select syntax,
  which takes one word from a fixed set (here only the word `str`), never a
  list. By the Supervisor's own option code that refuses the app's default
  options, which it checks before every start, and the wizard's save. Both
  are now lists of words. Checked against a copy of the Supervisor's option
  rules in the app's tests; not yet run on a Supervisor.
- **List options are read whole at start.** The setup step read `cameras`,
  `frigate.cameras` and `frigate.labels` through bashio, which prints a list
  one item per line, never as a list. In Frigate mode a label list (the
  default one included) stopped the setup before the kernel's services could
  start; in standalone mode a configured camera stopped it, and with no device
  key yet two cameras left the app running only the wizard. Each list is now
  read from the options file the Supervisor writes (`/data/options.json`). In
  Frigate mode, an options file the setup cannot read now stops it with a
  reason, instead of sealing every camera Frigate reports, and a Frigate
  block saved without `min_confidence` gets 0.5 instead of a value the bridge
  refuses. Host-tested against a copy of bashio's option code, on the app's
  jq 1.6 and on 1.7; not run in the app image.
- **Zone ids follow one rule.** The wizard page spells a camera's zone id as
  the server and go2rtc discovery do: an emoji is one `_`, `İstanbul` is
  `zone:_stanbul`, and the id is cut to 64 characters. The wizard's own saves
  store no zone id. A standalone save (only an API client sends one) that
  names none now keeps the stored id of a camera of that name, and only a new
  camera gets the rule. It used to get its raw name, which the setup refuses
  when the name holds a space, a capital or a non-ASCII letter.
- **Pairing another Canary:** when five status reads fail in a row, the wizard
  now reads both Canaries once more and says what it finds, as it does at a
  timeout. A pairing both report finished shows as done, and one still
  finishing says so, instead of asking for a retry the Canary would refuse.

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
