# Configuration Changes

This document logs firmware configuration changes that are **not backwards
compatible** — renamed or removed feature flags, changed defaults with
behavioral impact, and new required settings. Review it when updating a
device you configured under an older firmware version, or when rebasing a
local `config_local.h` overlay.

The discipline (borrowed from Klipper's `Config_Changes.md`): any PR that
breaks an existing config **must**, in the same PR,

1. add a dated entry here (newest first, `YYYYMMDD:` prefix), and
2. update every shipped config under `configs/` (and every generated
   Arduino parity sketch) that the change breaks — CI builds all of them,
   so a missed one fails the build.

Entries say what changed, why, and what the user must do. Behavioral
context lives in [CHANGELOG.md](../CHANGELOG.md); this file is only the
"your config needs editing" list.

---

## Entries

20261019: The Vision flavor config drops its copies of the detection defaults.
No device behaves differently: the firmware loads the four from
`projects/canary-vision/include/canary/config.h` and never read the copies.
- `configs/canary-vision/default/config.h`: `CONFIG_PERSON_TARGET`,
  `CONFIG_SCORE_MIN`, `CONFIG_LOST_TIMEOUT_MS` and `CONFIG_DWELL_START_MS`,
  which only the flasher catalog's generator read.
  `canary-local/tools/gen_flash.py` now reads the project config.h's
  `PERSON_TARGET`, `SCORE_MIN`, `LOST_TIMEOUT_MS` and `DWELL_START_MS`,
  the constants `src/detect_config.cpp` loads. The `FEATURE_*` flags,
  `CV_WATCHDOG_TIMEOUT_SEC` and `CONFIG_DEVICE_TYPE` (the fleet figures
  read it) stay.
**Action:** if a `config_local.h`, a build flag or a script of yours sets
any of these four, delete it; it did nothing to the firmware before and
does nothing now. To change a detection default, edit the project's
`include/canary/config.h` (it seeds the first boot only: the live value is
kept in NVS and tuned from Home Assistant).
`scripts/tests/test_vision_flavor_config_readers.py` fails on a `CONFIG_*`
in the flavor file that nothing reads.

20261019: canary-vision's flavor config no longer names seventeen `CONFIG_*`
values nothing read (sweep F182, #1802; this entry was added by the
post-#1802 verification, which found the removal shipped without one). No
device behaves differently: each macro was read by no source before it went.
- `configs/canary-vision/default/config.h`: `CONFIG_DEVICE_ID`,
  `CONFIG_MANUFACTURER`, `CONFIG_MODEL`, `CONFIG_DWELL_END_GRACE_MS`,
  `CONFIG_INTERACTION_AFTER_LEAVE_WINDOW_MS`, `CONFIG_ZONE_INTERACTION_MS`,
  `CONFIG_VOXEL_COLS`, `CONFIG_VOXEL_ROWS`, `CONFIG_FRAME_W`,
  `CONFIG_FRAME_H`, `CONFIG_INVOKE_PERIOD_MS`, `CONFIG_HEARTBEAT_MS`,
  `CONFIG_MQTT_BUFFER_BYTES`, `CONFIG_HA_DISCOVERY_PREFIX`,
  `CONFIG_TOPIC_EVENTS`, `CONFIG_TOPIC_STATE` and `CONFIG_TOPIC_STATUS`. The
  project's own `projects/canary-vision/include/canary/config.h` (identity,
  detection and dwell defaults, voxel grid, frame size, timing, MQTT buffer,
  discovery prefix) and `include/canary/topics.h` (the topic patterns) own
  those values, as they always did: editing the flavor macro changed
  nothing. A local `config_local.h` overlay that set one of them changes
  nothing either; move the value to the project header. The flavor file
  keeps the `FEATURE_*` flags, `CV_WATCHDOG_TIMEOUT_SEC`, and five seeds
  that are copies of project constants tools read there
  (`CONFIG_DEVICE_TYPE` for `canary-local/tools/figures/gen_figures.mjs`;
  `CONFIG_PERSON_TARGET`, `CONFIG_SCORE_MIN`, `CONFIG_LOST_TIMEOUT_MS` and
  `CONFIG_DWELL_START_MS` for `canary-local/tools/gen_flash.py`), held equal
  to the project's by `scripts/tests/test_vision_flavor_config_readers.py`.

20261009: Flags, macros and one env that controlled nothing were removed.
No device behaves differently: each was read by no source before it went.
- `configs/canary-display/*/config.h`: `FEATURE_CHAIN_VERIFY`,
  `FEATURE_PROOF_QR`, `FEATURE_NIGHT_BLACKOUT`. The trust engine compiles on
  every flavor, the proof QR wherever its UI does (the watch glance and the
  dash), and night is a runtime setting that boots DIM on every flavor
  (`glass_settings.cpp`).
- `firmware/canary`: `FEATURE_BLUETOOTH`, `FEATURE_SYS_MONITOR`,
  `FEATURE_WIFI_PRESENCE`, `FEATURE_AUDIBLE_CHIRP`, `FEATURE_RF_PRESENCE` and
  `FEATURE_CHIRP` (from `canary_config.h` and `[env:full]`), and
  `VERIFY_INTERVAL_SEC` and `MQTT_TOPIC_PREFIX` (from `canary_config.h`).
  BLE in this tree is `FEATURE_BLE_SCAN` / `FEATURE_BLE_STATUS` (with
  `FEATURE_BLE` as the reported `ble` tag);
  `provisioning/platformio_secure.ini` now sets those three to 0 where it
  set the inert `FEATURE_BLUETOOTH=0`.
- `[env:standalone]` (the release image under another name): build
  `release`.
- The `-DCONFIG_CANARY_*`, `-DCONFIG_DASH` / `_WATCH` / `_NIGHTSTAND` /
  `_DEFAULT` / `_WELLBEING`, `-DSENTINEL_PRESET_*` and
  `-DSECURACV_BUILD_SECURE` markers in `envs/platformio/*.ini` and
  `platformio_secure.ini`: a flavor is its `config.h` on the `-I` path.
- `canary/include/secure_defaults.h`'s `DEFAULT_*` macros (nothing
  included the header); it is now a map of where each default is set.
**Action:** if a `config_local.h`, a build flag or a script of yours sets
any of these, delete it; it did nothing before and does nothing now. To
turn on a feature they named, use the flag that gates its code (the
comments where each stood say which). `firmware/scripts/check_config_feature_flags.py`
now fails a config `FEATURE_*` flag that no source reads.

20260723: `SD_SPI_FAST` default raised 4 MHz → 20 MHz
(`canary/include/canary_config.h`; the dead board duplicates
`SD_SPI_FREQ_FAST/SLOW` in `boards/xiao-esp32s3-sense/pins/pins.h` were
synced to match). The storage driver already falls back to `SD_SPI_SLOW`
(1 MHz) and retries the full `SD.begin` ladder when a card fails to init at
the fast clock, so a slower/older card degrades gracefully rather than
failing to mount. **Action:** none required for the vast majority of cards
(20 MHz is inside the 25 MHz SD-SPI ceiling and typical on the short
XIAO-Sense expansion traces). If a specific card mounts only intermittently
after updating, override `SD_SPI_FAST` back to `4000000` via a build flag.
Both macros are now `#ifndef`-guarded so a build flag or `config_local.h`
overlay can set them without editing the header. Hardware bench validation
across a card range is recommended before treating 20 MHz as fully verified.

20260711: This log was introduced. Configuration changes older than this
date are not recorded here — see [CHANGELOG.md](../CHANGELOG.md) and the
per-config READMEs under [configs/](configs/) for historical context.
