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
