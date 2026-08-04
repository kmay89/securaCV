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
| **Espressif ESP32-S3-WROOM-1** (PCB antenna) | **2AC7Z-ESPS3WROOM1** | [fccid.io](https://fccid.io/2AC7Z-ESPS3WROOM1), [fcc.report](https://fcc.report/FCC-ID/2AC7Z-ESPS3WROOM1) | ❌ not yet — **blocked, see below** | Grantee: Espressif Systems (Shanghai) Co., Ltd. Equipment class **DTS**. RF testing by Sporton International Inc. (Kunshan), report FR1N0920-01B, tested 2022-06-29 → 2022-07-27. Note the ID is `ESPS3WROOM1`, **not** `ESP32S3WROOM1` — the obvious guess is wrong and returns nothing. Not a board we ship; it is the module the proposed [`canary-witness-s3`](../../boards/canary-witness-s3/) carrier is built around, and the modular-approval path is the only five-figure saving in that program. Verified 2026-08 (this lookup). |
| **Espressif ESP32-S3-WROOM-1U** (u.FL antenna) | **2AC7Z-ESPS3WROOM1U** | [fcc.report](https://fcc.report/FCC-ID/2AC7Z-ESPS3WROOM1U) | ❌ not yet | **A separate grant from the -1.** The carrier's `alt` part. Choosing the -1U is therefore a certification decision, not just a connector decision — its antenna condition is its own and must be read separately. |

## The WROOM-1 lookup — what was found, and what stopped it

The 2026-08 pass established the **identity** of the ESP32-S3-WROOM-1 grant
but could not **read** it: `fcc.report`, `fccid.io` and
`documentation.espressif.com` are all unreachable from the build environment
(the agent proxy returns 403 at CONNECT for every one of them). Grant PDFs are
therefore unopened, and per this document's own rule the column stays ❌.

**One lead is recorded here precisely because it is unverified.** A search
summary of the grant material stated that the module *"is authorized only for
use in devices where the antenna maintains 20 cm distance between the antenna
and users"* — the standard **mobile**-classification RF-exposure condition. If
that is genuinely on the grant, it is not a footnote for this product line:

- a wall- or ceiling-mounted Canary is comfortably past 20 cm and unaffected;
- a **doorbell**, a **nightstand** unit, a **watch-puck** or anything worn or
  handheld is not, and a host that breaks the separation condition falls out of
  mobile classification into **portable**, which requires SAR evaluation the
  modular grant does not carry.

**Do not act on that paragraph.** It is a search snippet, not a grant, and
snippets routinely conflate the -1 and -1U filings or quote a host product's
manual rather than the grant note. It is written down so the next person checks
it first, not so anyone can cite it.

### To close this row (~15 minutes, from any unrestricted network)

1. Open `https://fccid.io/2AC7Z-ESPS3WROOM1` and download the **Grant of
   Equipment Authorization** PDF (not the test report).
2. Read and record four things: **single vs limited** modular approval; the
   **RF exposure classification** (mobile / portable) and any **separation
   distance**; the **antenna condition** (fixed PCB antenna vs any permitted
   substitution); and the exact **host labeling** string.
3. Confirm there is no 47 CFR 2.803 evaluation-only language — doc 29 §3 flags
   that as the trap, and it is the one thing that would void the whole
   modular-approval saving.
4. Repeat for `2AC7Z-ESPS3WROOM1U` if the u.FL variant is still a candidate.
5. Link the PDFs here and flip the column.

Until then the modular path for
[`canary-witness-s3`](../../boards/canary-witness-s3/) is **promising and
unconfirmed**, which is exactly the posture doc 29 requires — and the program's
$5–15k saving is contingent on step 2, not assumed by it.

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
