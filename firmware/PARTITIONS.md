# Firmware Partition Schemes — Canonical Reference

Single source of truth for **which ESP32 partition table to use for which deployment**.
The repo ships several partition tables for the "same" XIAO ESP32‑S3 board because the right
layout depends on three axes: **flash size × OTA-or-not × build profile**. Picking the wrong one
either wastes half the flash or fails to fit the binary. This document maps each table to its
intended deployment and states the canonical recommendation.

> Companion to [`docs/review/02-roadmap.md`](../docs/review/02-roadmap.md) §4 (hardware plan) and
> requirement **REQ‑FW‑031**. Resolves flag‑report **F‑06** (divergent partition tables).

## The binding constraint: app‑slot size vs. the FULL binary

On the XIAO ESP32‑S3 the limit is **flash, not RAM** (PSRAM is 8 MB OPI and ample). The
monolithic **FULL** profile (Mesh + NimBLE + camera + all radios) is **~2.4–2.9 MB** and sits near
the ~3.0–3.3 MB board‑default app ceiling — see
[`projects/canary-wap/FLASH_MEMORY_ANALYSIS.md`](projects/canary-wap/FLASH_MEMORY_ANALYSIS.md).

**Consequence:** a FULL binary does **not** fit in a 1.5–1.9 MB OTA app slot, so FULL + OTA
does not coexist with the 4 MB-style layouts below. It **does** coexist with arduino-esp32's
`default_8MB` table (2 × 3.2 MB `app0`/`app1`, the WAP sketch's board default) — at the cost of
having **no dedicated data partitions** beyond the stock FATFS region. A 16 MB board remains the
comfortable answer when FULL needs A/B **plus** a `witness_log`-style partition.

## Tables shipped in this repo

| Table | Used by | Flash used | App slots | OTA | FULL (~2.7 MB) fits? |
|---|---|---|---|---|---|
| [`canary/partitions_ota.csv`](canary/partitions_ota.csv) | `canary/platformio.ini` dev/release/minimal (the **ACTIVE/canonical** PlatformIO build; `[env:full]` pins `default_8MB` instead — see below) | **4 MB** of 8 | 2 × `0x1E0000` (1.9 MB) | A/B | ❌ no (slots < FULL) |
| [`projects/canary-ota/partitions.csv`](projects/canary-ota/partitions.csv) | `projects/canary-ota/` (SPECIALIZED OTA A/B subsystem) | 8 MB | factory + A + B, 3 × `0x180000` (1.5 MB) + `witness_log` | factory+A/B | ❌ no (slots < FULL) |
| [`provisioning/partitions_secure.csv`](provisioning/partitions_secure.csv) | `provisioning/platformio_secure.ini` (Phase‑2 secure/encrypted) | **4 MB** of 8 | 2 × `0x1E0000` (1.9 MB) + `nvs_keys` | A/B | ❌ no (slots < FULL) |
| [`projects/canary-display/partitions_display_4mb.csv`](projects/canary-display/partitions_display_4mb.csv) | `canary-display-nightstand-c6`, `canary-display-nightlight-c3` (4 MB C6/C3 display boards) | 4 MB (all of it) | 2 × `0x1F0000` (1.98 MB), no filesystem | A/B | ❌ no (and FULL never targets these boards) |
| _(none pinned)_ board default `default_8MB` | `projects/canary-wap/` Arduino sketch (the real **FULL** build) | 8 MB | 2 × `0x330000` (3.2 MB) + 9.5 MB FATFS region | A/B (`app0`/`app1`) | ✅ yes (fits a 3.2 MB slot, A/B intact) |

> **Correction (2026-06-10):** arduino-esp32's `default_8MB` table (the
> XIAO_ESP32S3 board default) has always carried `app0`/`app1` OTA slots of
> `0x330000` each — it is A/B-capable, which is what the WAP's BLE OTA
> (`ble_ota.cpp`) and the shared signed pull-OTA engine
> (`firmware/common/ota/`) write to. CI now enforces both slot budgets
> (`.github/workflows/firmware.yml`): the canary release image must fit
> `0x1E0000` and the canary-wap image must fit `0x330000` — an image that
> exceeds its slot could never be installed over the air. Since 2026-08-07
> the release workflow re-measures the exact bytes it publishes against the
> same `flavors.json` budgets (`firmware/scripts/check_slot_budget.py`) —
> a tag build is not the branch build, which is how fw-v2.4.6 shipped an
> oversized nightstand-c6 image past a green PR gate.

Note that **two** of the tables (`canary/partitions_ota.csv` and `partitions_secure.csv`) describe
only a **4 MB** layout — on an 8 MB board they leave half the flash unused. They are correct for
their *current* (non‑FULL) consumers but are **not** the layout to copy for a FULL deployment.

## Canonical scheme per deployment

Choose by what you are shipping:

| Deployment | Flash | Use this layout |
|---|---|---|
| **FULL with OTA A/B** (production WAP today) | 8 MB | Board default `default_8MB` (2 × 3.2 MB `app0`/`app1`), as `projects/canary-wap/` does. FULL fits the slot and updates arrive via signed pull-OTA or BLE OTA (see `docs/firmware_ota.md`). |
| **DEV / release / minimal, with OTA A/B** | 8 MB | [`canary/partitions_ota.csv`](canary/partitions_ota.csv) — 1.9 MB A/B slots. Fits the non‑FULL profiles; **do not** select `[env:full]` against it expecting it to fit — `canary/platformio.ini` now pins `board_build.partitions = default_8MB.csv` for `[env:full]` (per the row above), which also restores its signed pull-OTA path. |
| **Factory‑recovery + A/B + on‑device log** (smaller builds) | 8 MB | [`projects/canary-ota/partitions.csv`](projects/canary-ota/partitions.csv) — adds a `witness_log` data partition that survives OTA. |
| **Secure / flash‑encrypted** (Phase‑2 provisioning) | 8 MB | Start from [`provisioning/partitions_secure.csv`](provisioning/partitions_secure.csv) but **enlarge the app slots and push subsequent offsets** for an 8 MB target — the committed file is a 4 MB reference (see its in‑file note). |
| **FULL + OTA A/B + dedicated `witness_log` partition** | **16 MB** | Not shipped on 8 MB (`default_8MB` gives FULL its A/B slots but no custom data partitions). Per [roadmap §4.1](../docs/review/02-roadmap.md), a 16 MB‑flash S3 (e.g. XIAO ESP32‑S3 "Plus") fits two ≥3 MB app slots **plus** a `witness_log` partition. Keep MINIMAL/DEV on 8 MB. |
| **4 MB display boards, NVS‑only state** (nightstand‑c6, nightlight‑c3) | 4 MB | [`projects/canary-display/partitions_display_4mb.csv`](projects/canary-display/partitions_display_4mb.csv) — the stock `min_spiffs.csv` with its unused 128 KB spiffs folded into the A/B slots (`0x1E0000` → `0x1F0000`). Born of fw‑v2.4.6: the C6 nightstand image outgrew `min_spiffs`' slot by 5 KB and shipped anyway (`checkprogsize` measures the ELF, not the final `.bin`), boot‑looping every board it was flashed to. The byte‑accurate gates are `flavors.json` `size_guards` + `make_factory.py`'s slot‑fit refusal. |

## Rules for editing a partition CSV

- Partition **offsets are explicit** and contiguous. If you enlarge a slot, you must push every
  subsequent partition's offset forward by the same amount, or the build fails with a
  partition‑overlap error.
- Keep `nvs` (device identity, Ed25519 keys, chain state) and any `witness_log` partition in
  regions that **survive OTA** — never inside an app slot.
- The `encrypted` flag is enforced only after the Phase‑2 eFuse burn; in development it is ignored.
- **Growing a slot reaches only boards that get a USB factory flash.** OTA ships app‑only
  images; a fielded board keeps the table it was flashed with, so an image bigger than the
  *old* slot strands every old‑table board (its OTA install fails, forever) until someone
  re‑flashes it over USB. Grow a table and the image together only as a deliberate decision —
  and say so in the release notes.

If you add a new table, add a row here and to [`README.md`](README.md) so this stays the single
source of truth.
