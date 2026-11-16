<!-- The Supervisor shows this file on the app's page after an update. Newest first. -->
## 0.9.2

- **"Only Frigate alerts" is checked against your own Frigate config.**
  Where Frigate keeps its own camera config, the wizard now reads its
  record setting (a camera's own `record.enabled`, else the top-level one;
  Frigate's default is off) for the cameras SecuraCV seals. Frigate creates
  alerts only for cameras that record, so when it records none of them the
  choice is not applied, every tracked object is sealed instead, and the
  done screen says why and how to turn alerts on. When only some of them
  record, the choice is kept and the screen names the cameras that will
  seal nothing. A config the wizard cannot read, or a record setting it
  cannot resolve (YAML anchors, aliases, merge keys, a templated value), is
  taken at your word, as before.
- **The kernel, the integration and the app move to 0.9.2 together.**

## 0.9.1

- **The encrypted database runs on a fixed SQLite.** The kernel inside the
  app is built with SQLCipher 4.20.0 on SQLite 3.54.1 (`rusqlite` 0.43). The
  build before it carried SQLite 3.53.4, which could return stale memory when
  a database was opened beside a `-wal` file with a different page size
  (RUSTSEC-2026-0502). Nothing to do: the update replaces the binary.
- **The Frigate config the wizard writes is detection-only.** When no
  Frigate config was found, the wizard wrote one that gave every camera the
  `record` role, set `record.enabled: true` and kept motion video for as
  many days as you chose for sealed events (up to a year). It now writes the
  `detect` role alone and `record.enabled: false`, with a comment pointing
  at Frigate's record docs; the wizard's retention is the witness log's and
  is no longer passed to that file. Snapshots are not set, so they stay at
  Frigate's default (off). The camera screen and DOCS.md say so. Pinned by
  `test_write_frigate_config_is_detection_only` and a save-path test that
  picks one year and finds no `record` role, no `retain` block and no 365 in
  the written file.
- **"Only Frigate alerts" is not offered where it cannot seal.** Alerts mode
  listens to Frigate's review items instead of its events, and Frigate
  creates review items only for cameras that record. With the detection-only
  config above that choice would have left a log of heartbeats while the
  done screen promised a first sealed event. On the path where the wizard
  writes Frigate's config the choice is disabled with the reason, the page
  never sends it, and the server stores "every tracked object" and says why
  on the done screen. The choice, DOCS.md and the option list now say it
  needs recording on in Frigate. Pinned by three save-path tests (RTSP
  cameras with `enable_reviews: true`, with and without a file written, and
  the Frigate-owned path where the choice is kept) and four page tests.
- **Base image moved off an unsupported Alpine.** The runtime bases and both
  build stages were on Alpine 3.18, out of security support since May 2025.
  They are on 3.22 (GStreamer 1.26, SQLite 3.49, Python 3.12, jq 1.8.2,
  bashio 0.19.0), whose security support ends 2027-05-01; the date is
  written beside the pin. Tag and digest are literal on all four `FROM`
  lines (no `ARG` carries either), so a dependency updater can move them.
  `run.sh` reads the hostname through `bashio::app.hostname`. Pinned by
  `tests/test_base_image_pins.py`: one release across all bases, not older
  than 3.22, digest on every line, and the workflow's Python and jq equal to
  what the Dockerfile says the release ships.
- **A quote or a backslash in a standalone camera URL no longer breaks the
  kernel's config file.** `run.sh` pasted the URL between quotes, so such a
  password made `witness_config.json` invalid JSON and the kernel exited on
  a parse error at every start. The URL is now JSON-encoded by jq; the file
  parses and carries the exact URL. Not fixed here: the kernel still places
  the URL in a GStreamer launch string, where a backslash is read as an
  escape and a space ends the value, so a password holding either can still
  fail in the kernel's pipeline until `src/ingest/rtsp.rs` sets `location`
  as a property. Pinned in `tests/test_config_render.sh` with `pa"ss\word`
  rendered through the script's own encoding line (the file parses,
  `rtsp.url` round-trips byte for byte).
- **The done screen and the docs name the card and the button Home Assistant
  shows.** They sent you to a "SecuraCV discovered" card and its "Configure"
  button; no card has that title and a discovered card's button reads "Add".
  They now say: the *SecuraCV app* card under Discovered, press Add. Pinned
  in `tests/test_wizard_setup_flow.test.js` (both done-screen paths) and
  `tests/test_serve_wizard_setup.py` (the title is derived from the
  integration's `flow_title` and discovery step).
- **The add-on's tests run on what the new base ships.** `python-version`
  3.11 -> 3.12 and the pinned jq release binary 1.6 -> 1.8.2 (sha256 from
  the release's own `sha256sum.txt`, version asserted before it goes on
  PATH); `PWK_REQUIRE_JQ` is `jq-1.8.2` in both steps.
  `tests/test_base_image_pins.py` fails if the Dockerfile and these pins
  move apart.
- **The camera login stays out of the app's log.** In standalone mode the
  start-up log printed the half of the camera URL that holds the username
  and password and hid the host. It now prints the scheme, host and path
  with the login replaced by `***`, and drops the query string (some cameras
  take the login there). Pinned in `tests/test_run_lib.sh`
  (`camera_url_for_log`).
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
