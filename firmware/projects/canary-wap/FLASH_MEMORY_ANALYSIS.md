# Canary WAP — Arduino Flash Memory Analysis

_Scope: the monolithic Arduino IDE sketch at `arduino/canary_wap/`, built with FQBN
`esp32:esp32:XIAO_ESP32S3:PSRAM=opi,FlashSize=8M,...` (see `Makefile`), profile **FULL**
(`build_config.h` — Mesh + NimBLE + camera + all radios). Figures are static estimates
from source bytes except the web-asset gzip numbers, which are measured._

## Why this exists

The Arduino build was reported near the program-storage ceiling. This documents where the
flash goes, what was trimmed, and how much headroom remains for new features.

## Where the flash goes (estimate)

App partition (program storage) on XIAO_ESP32S3 with no `PartitionScheme` pinned is the
board default, ~**3.0–3.3 MB**. Estimated binary before this change: **~2.4–2.9 MB
(~80–95%)** — consistent with "almost full".

| Bucket | Est. flash | Notes |
|---|---|---|
| Embedded web assets (uncompressed) | **~457 KB** | `web_ui.h` 208 KB, `csi_dashboard_html.h` 133 KB, `companion_pwa.h` 117 KB (HTML) — served verbatim, no compression |
| Arduino core + esp-idf (WiFi/BT/lwIP/httpd) | ~700 KB–1 MB | framework floor |
| NimBLE stack | ~100–150 KB | FULL only (`FEATURE_BLE*`) |
| App C/C++ (~66.6K lines) | ~0.8–1.2 MB | CSI, mesh, chirp, beacon, crypto, QR |
| QR libs | ~40–60 KB | `qrcodegen.c` / `quirc.c` / `decode.c` / `identify.c` |

RAM is not the constraint — PSRAM (8 MB OPI) is enabled and the large CSI/vision buffers are
bounded. Flash is the limit.

## Biggest lever: gzip the embedded web assets — DONE

The dashboard (`/`), settings/admin (`/settings`, `/admin`), and companion (`/companion`)
pages were shipped as raw `PROGMEM` string literals and served verbatim over
`esp_http_server`. No `Content-Encoding: gzip` anywhere. They are served byte-for-byte —
the auth token rides a `Set-Cookie` header, never injected into the HTML body — so
pre-compressing them is transparent to the browser.

**Result (measured by `gen_web_assets_gz.py`):**

| Asset | Raw | gzip | |
|---|---:|---:|---:|
| `CANARY_UI_HTML` (settings/admin) | 208,043 B | 42,459 B | 20% |
| `CSI_DASHBOARD_HTML` (`/`) | 132,581 B | 37,970 B | 29% |
| `COMPANION_HTML` (`/companion`) | 117,155 B | 33,502 B | 29% |
| **Total** | **457,779 B** | **113,931 B** | **saved ~336 KB** |

That frees roughly **10–11 percentage points** of the app partition. The decompressed bytes
are byte-identical to the originals (verified by round-trip), so every page renders exactly
as before.

### How it works
- `gen_web_assets_gz.py` reads the raw-string body of each source header, gzips it, and
  writes `web_assets_gz.h` (one `uint8_t[]` + `_LEN` per asset). Re-run after editing any
  HTML.
- The source headers stay editable; their literals are compiled out via
  `#define CANARY_WEB_ASSETS_GZIPPED 1` (set in `canary_wap.ino` before the includes), so
  the uncompressed copies never reach the binary.
- Serve sites add `Content-Encoding: gzip` and send the gz array with explicit length.
- `csi_integration.cpp` dropped its unused `#include "csi_dashboard_html.h"` (would
  otherwise recompile the raw literal into that translation unit).

## Remaining headroom options (not applied — flip if you need more)

- **Partition scheme:** flash is 8 MB but the app slot is ~3 MB. If the Arduino build
  doesn't need dual-OTA slots, pinning a larger-app `PartitionScheme` in the FQBN reclaims
  the second app partition. Config-only, no code.
- **Per-deployment feature gating:** dropping Mesh and/or `FEATURE_BLE_SCAN` from a given
  build frees ~150–225 KB. Only where those features aren't shipped — FULL is production.
- **Minify before gzip:** marginal after gzip; skip unless still tight.

## Verify on device (no toolchain in CI here)

1. `cd firmware/projects/canary-wap && make arduino-build` (or Arduino IDE Verify); read the
   "Sketch uses X bytes (Y%)" line — expect ~336 KB / ~10–11 pts lower than before.
2. Flash, then load `/`, `/settings`, and `/companion`; confirm each renders and DevTools →
   Network shows `content-encoding: gzip` with the reduced transfer size.
3. Confirm the `Set-Cookie` login flow + captive portal still work (asset bytes unchanged,
   only transport encoding differs).
