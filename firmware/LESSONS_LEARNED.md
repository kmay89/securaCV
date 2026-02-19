# SecuraCV Canary — Lessons Learned & Regression Notes

> **Read this file before modifying firmware.**
> Every entry here was learned the hard way. Each one cost debugging time.
> If you're about to make a change and something here is relevant, follow
> the guidance. If you learn something new, ADD IT HERE.

---

## ESP32 Arduino Core 3.x Migration

### mbedTLS API changes — NO `_ret` suffix
- **What happened:** Firmware compiled on Core 2.x but failed on 3.x
- **Root cause:** ESP32 Arduino Core 3.x (built on ESP-IDF 5.x) removed the
  `_ret` suffix from all mbedTLS functions
- **Fix:** `mbedtls_sha256_ret()` → `mbedtls_sha256()`, etc.
- **Regression check:** `regression_check.sh` greps for `_ret(`
- **Date learned:** 2026-01

### `esp_camera.h` include order matters
- **What happened:** Compile errors about undefined camera structs
- **Root cause:** `esp_camera.h` must be included AFTER WiFi/system headers
- **Fix:** Include order: system → WiFi → crypto → esp_camera → project headers

### Watchdog API differs between ESP-IDF 4.x and 5.x
- **What happened:** CI build failed with `esp_task_wdt_config_t` not declared
- **Root cause:** ESP-IDF 5.x introduced a struct-based watchdog API
  (`esp_task_wdt_config_t`, `esp_task_wdt_reconfigure`). ESP-IDF 4.x uses
  the simpler `esp_task_wdt_init(uint32_t timeout, bool panic)`.
  PlatformIO `espressif32 @ ^6.5.0` with Arduino framework uses ESP-IDF 4.4.x.
- **Fix:** Use `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)` to
  select the correct API at compile time. Include `esp_idf_version.h`.
- **Date learned:** 2026-02

---

## Hardware: XIAO ESP32S3 Sense

### Camera pins are board-specific
- **What happened:** Camera init returned ESP_FAIL silently
- **Root cause:** Used generic ESP32-CAM pin definitions instead of XIAO-specific
- **Fix:** XIAO ESP32S3 Sense pins:
  ```
  PWDN  = -1  (CRITICAL: must be -1, not 32)
  RESET = -1
  XCLK  = 10
  SIOD  = 40
  SIOC  = 39
  Y9-Y2 = 48,11,12,14,16,18,17,15
  VSYNC = 38
  HREF  = 47
  PCLK  = 13
  ```
- **Regression check:** Script verifies PWDN = -1

### SD card SPI pins are board-specific
- **What happened:** SD card init failed with no error message
- **Root cause:** Wrong SPI pins for XIAO ESP32S3 Sense
- **Fix:** CS=21, SCK=7, MISO=8, MOSI=9. Must call `SPI.begin(7,8,9,21)`
  before `SD.begin(21)`
- **Regression check:** Script verifies pin numbers

### GPS UART pins
- **What happened:** No GPS data received
- **Root cause:** Used default Serial1 pins instead of XIAO-specific
- **Fix:** L76K GNSS on UART1: RX=GPIO44 (D7), TX=GPIO43 (D6), 9600 baud

---

## Security

### AP password must be device-unique
- **What happened:** Default `witness2026` password was in the README; anyone
  could connect to any Canary
- **Fix:** Derive AP password from pubkey fingerprint at provisioning
- **Regression check:** Script greps for hardcoded `witness2026`

### Token must never enter witness chain
- **What happened:** (Preventive) API tokens are transport auth only
- **Rule:** Bearer tokens authenticate HTTP requests. They are NOT
  cryptographic evidence. They must NEVER appear in witness records,
  chain hashes, or SD card logs.
- **Regression check:** Script greps for token references in chain context

### Ed25519 private key must never be HMAC key directly
- **What happened:** (Preventive) First design used privkey as HMAC key
- **Fix:** Two-step HKDF derivation. Derive intermediate `token_key` first,
  then derive API token from intermediate key.
- **Why:** Key separation principle. If token derivation has a flaw, signing
  key is protected.

### BLE adds proprietary binary blobs
- **What happened:** Enabling CONFIG_BT_ENABLED pulls in Espressif closed-source
  BT stack, which includes CVE-2025-27840 attack surface
- **Rule:** BLE features must be compile-time opt-in (OFF by default)
- **Users must explicitly understand the tradeoff before enabling**

---

## Web UI (web_ui.h)

### No browser storage APIs
- **Rule:** No `localStorage`, `sessionStorage`, or `document.cookie`
- **Why:** Token must live only in JS variable. Tab close = token gone.
- **Regression check:** Script greps for storage APIs

### PROGMEM size limits
- **What happened:** Very large web_ui.h caused flash allocation issues
- **Rule:** Keep web_ui.h under 64KB. If larger, split into separate headers.
- **Regression check:** Script checks file size

### Every button must have a backend
- **Rule:** No UI element should exist without a working firmware handler
- **Why:** Dead buttons destroy user trust and waste debugging time
- **Process:** When adding UI elements, always implement the API endpoint
  in the same commit

---

## GPS & Time

### Time coarsening is mandatory
- **Rule:** SecuraCV coarsens timestamps to 5-second buckets (minimum)
- **Why:** Privacy by design. Precise timestamps enable correlation attacks.
- **Watch for:** High-precision format strings (`%.6f`, `%.7f`) near GPS data

### First GPS fix takes 8+ minutes
- **What happened:** Thought GPS was broken, but it was cold-start TTFF
- **Reality:** L76K cold start = 25-35 seconds typical, but can take 8+
  minutes if almanac data is lost
- **UX:** Dashboard should show "Acquiring satellites..." not "GPS Error"

---

## Build System

### Dual-build compatibility required
- **Rule:** Firmware must compile on BOTH Arduino IDE and PlatformIO
- **Why:** Different team members use different IDEs; CI tests both
- **Common pitfall:** PlatformIO auto-resolves includes via `lib_deps`;
  Arduino IDE requires libraries installed globally
- **Test:** CI runs both `arduino-cli compile` and `pio run`

### Feature flags for everything
- **Rule:** Every major feature has a `#define FEATURE_X` flag (see
  `firmware/canary/include/canary_config.h` for defaults)
- **Why:** Allows building stripped-down firmware for testing, and enables
  compile-time security decisions (e.g., BLE off)
- **Pattern:**
  ```cpp
  #define FEATURE_CAMERA_PEEK  1
  #if FEATURE_CAMERA_PEEK
    #include "esp_camera.h"
    // camera code here
  #endif
  ```

---

## How to Add an Entry

When you encounter a bug, regression, or hard-won lesson:

1. Add it to the appropriate section above
2. Include: what happened, root cause, fix, and regression check (if any)
3. If it can be automated, add a check to `firmware/scripts/regression_check.sh`
4. Date it so we know when it was learned

**Format:**
```markdown
### Short description
- **What happened:** The symptom or failure
- **Root cause:** Why it happened
- **Fix:** What solved it
- **Regression check:** How we prevent it from recurring
- **Date learned:** YYYY-MM
```
