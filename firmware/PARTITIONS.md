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

**Consequence:** a FULL binary does **not** fit in a 1.5–1.9 MB OTA app slot. Any A/B OTA layout
on 8 MB flash necessarily uses sub‑2 MB slots, so **FULL + OTA does not coexist on an 8 MB
board.** You must choose: FULL *or* OTA on 8 MB — or move FULL to a **16 MB** board to get both.

## Tables shipped in this repo

| Table | Used by | Flash used | App slots | OTA | FULL (~2.7 MB) fits? |
|---|---|---|---|---|---|
| [`canary/partitions_ota.csv`](canary/partitions_ota.csv) | `canary/platformio.ini` (the **ACTIVE/canonical** PlatformIO build) | **4 MB** of 8 | 2 × `0x1E0000` (1.9 MB) | A/B | ❌ no (slots < FULL) |
| [`projects/canary-ota/partitions.csv`](projects/canary-ota/partitions.csv) | `projects/canary-ota/` (SPECIALIZED OTA A/B subsystem) | 8 MB | factory + A + B, 3 × `0x180000` (1.5 MB) + `witness_log` | factory+A/B | ❌ no (slots < FULL) |
| [`provisioning/partitions_secure.csv`](provisioning/partitions_secure.csv) | `provisioning/platformio_secure.ini` (Phase‑2 secure/encrypted) | **4 MB** of 8 | 2 × `0x1E0000` (1.9 MB) + `nvs_keys` | A/B | ❌ no (slots < FULL) |
| _(none pinned)_ board default `default_8MB` | `projects/canary-wap/` Arduino sketch (the real **FULL** build) | 8 MB | 1 × ~3 MB | **none** | ✅ yes (no A/B) |

Note that **two** of the tables (`canary/partitions_ota.csv` and `partitions_secure.csv`) describe
only a **4 MB** layout — on an 8 MB board they leave half the flash unused. They are correct for
their *current* (non‑FULL) consumers but are **not** the layout to copy for a FULL deployment.

## Canonical scheme per deployment

Choose by what you are shipping:

| Deployment | Flash | Use this layout |
|---|---|---|
| **FULL, no OTA** (production WAP today) | 8 MB | Board default `default_8MB` (single ~3 MB app), as `projects/canary-wap/` does. FULL fits; updates are full‑image reflash, not A/B. |
| **DEV / release / minimal, with OTA A/B** | 8 MB | [`canary/partitions_ota.csv`](canary/partitions_ota.csv) — 1.9 MB A/B slots. Fits the non‑FULL profiles; **do not** select `[env:full]` against it expecting it to fit. |
| **Factory‑recovery + A/B + on‑device log** (smaller builds) | 8 MB | [`projects/canary-ota/partitions.csv`](projects/canary-ota/partitions.csv) — adds a `witness_log` data partition that survives OTA. |
| **Secure / flash‑encrypted** (Phase‑2 provisioning) | 8 MB | Start from [`provisioning/partitions_secure.csv`](provisioning/partitions_secure.csv) but **enlarge the app slots and push subsequent offsets** for an 8 MB target — the committed file is a 4 MB reference (see its in‑file note). |
| **FULL *and* OTA A/B together** | **16 MB** | Not shipped on 8 MB. Per [roadmap §4.1](../docs/review/02-roadmap.md), move FULL builds to a 16 MB‑flash S3 (e.g. XIAO ESP32‑S3 "Plus") to fit two ≥3 MB app slots **plus** a `witness_log` partition. Keep MINIMAL/DEV on 8 MB. |

## Rules for editing a partition CSV

- Partition **offsets are explicit** and contiguous. If you enlarge a slot, you must push every
  subsequent partition's offset forward by the same amount, or the build fails with a
  partition‑overlap error.
- Keep `nvs` (device identity, Ed25519 keys, chain state) and any `witness_log` partition in
  regions that **survive OTA** — never inside an app slot.
- The `encrypted` flag is enforced only after the Phase‑2 eFuse burn; in development it is ignored.

If you add a new table, add a row here and to [`README.md`](README.md) so this stays the single
source of truth.
