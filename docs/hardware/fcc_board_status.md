# FCC authorization status, per board — the doc 29 §10 lookup record

> The record that [strategy doc 29 §3/§10](../strategy/29-fcc-and-product-compliance-diligence.md)
> requires before any copy anywhere claims a board is certified: look each board
> up on `fccid.io`, write down what was actually found, and — critically — what
> was *not* checked. **An FCC ID existing is not the same as a grant that covers
> our use.** "Grant conditions read" means someone opened the actual grant
> documents and checked host/antenna/marketing conditions (including whether the
> board is marketed under 47 CFR 2.803 evaluation-kit provisions). Until that
> column says yes, treat the row as *promising, unconfirmed*.
>
> This record does not authorize anything. Every radio SKU stays gated behind
> its own Part 15 Subpart B SDoC regardless of what this table says (doc 29 §7).

| Board | FCC ID found | Source | Grant conditions read? | Notes |
|---|---|---|---|---|
| Seeed XIAO ESP32-S3 / ESP32-S3 Sense | **Z4T-XIAOESP32S3** | [fccid.io](https://fccid.io/Z4T-XIAOESP32S3), [fcc.report](https://fcc.report/FCC-ID/Z4T-XIAOESP32S3) | ❌ not yet | Grantee: Seeed Technology Co., Ltd. Listed as DTS (digital transmission system). Covers both plain and Sense per the filing title. Verified 2026-08 (this lookup). |
| Seeed XIAO ESP32-C3 | **Z4T-XIAOESP32C3** | [fccid.io](https://fccid.io/Z4T-XIAOESP32C3), [fcc.report](https://fcc.report/FCC-ID/Z4T-XIAOESP32C3) | ❌ not yet | Grantee: Seeed. Verified 2026-08 (this lookup). |
| Seeed XIAO ESP32-C6 (in MR60BHA2 kit) | — not looked up | — | ❌ | Sense/radar kit host board. |
| Seeed MR60BHA2 radar module | — not looked up | — | ❌ | 60 GHz mmWave — its own intentional radiator; do not assume the XIAO's grant covers it. |
| Seeed Grove Vision AI V2 (101021112) | n/a — expected no radio | — | ❌ | Camera + NPU on I2C; no Wi-Fi/BLE transmitter expected, which would make it a Part 15B concern only. Confirm no radio before relying on this. |
| Waveshare ESP32-S3-Touch-LCD-4.3 / 4.3B / 4.3C | — not looked up | — | ❌ | Doc 29 §3 flags dev boards as the trap: check for 2.803 evaluation-only marketing. |
| Waveshare ESP32-S3-Touch-AMOLED-2.06 | — not looked up | — | ❌ | Named explicitly in doc 29 §3 as required before any paid SKU. |
| Seeed Round Display for XIAO (104030087) | n/a — expected no radio | — | ❌ | Display expansion; no transmitter expected. |
| Seeed reCamera Pro (100092895) | — not looked up | — | ❌ | Finished Linux camera product; likely holds its own device-level authorizations — verify before the industrial channel resells or claims anything. |

## What this changes today

Nothing is unlocked. The store's plastic-only posture (doc 29 §8) holds:
printed parts sell; every radio SKU stays display-only with the 2.803
disclosure until a Part 15B SDoC test report exists for that SKU. The value of
this table is honesty and speed later — when the §9 decision is made to cross
the line on one SKU, the board homework is already started.

## How to extend it

One row per board in `canary-local/devices/registry.json`. Search the exact
board name and vendor on `fccid.io` / `fcc.report`; record the ID or its
absence, the URL, and the date. To flip "grant conditions read?" to ✅, open the
grant PDF and note: modular vs. device grant, antenna condition, host
restrictions, and any 2.803 evaluation-only language — then link the PDF here.
