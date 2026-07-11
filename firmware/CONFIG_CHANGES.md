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

20260711: This log was introduced. Configuration changes older than this
date are not recorded here — see [CHANGELOG.md](../CHANGELOG.md) and the
per-config READMEs under [configs/](configs/) for historical context.
