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

### User-typed identifiers must use an unambiguous alphabet
- **What happened:** A user typed their API token from the serial monitor into
  the dashboard, hit Connect, got "Too many failed attempts" after a few tries.
  Forensics showed `I` (capital i) at position 26 in the real token vs `l`
  (lowercase L) in what they typed — same glyph in most fonts, different
  bytes, constant-time compare fails. The auth rate limiter compounded the
  pain.
- **Root cause:** The token / AP-password encoder used a full base62 alphabet
  (`0-9 A-Z a-z`), which contains glyph-confusion classes `0/O` and `1/I/l`.
- **Fix:** Switched both `format_api_token_string` (API tokens) and
  `derive_ap_password` (`cv-XXXXX` AP password) to a 57-char unambiguous
  alphabet that drops `0`, `O`, `1`, `I`, `l`. Rejection-sampling threshold
  moves from `248` (`62*4`) to `228` (`57*4`) so the result remains unbiased.
  Entropy drops from ~190 to ~187 bits across 32 chars — UX win > 3 bits.
  Dashboard token input also pinned to a monospace font.
- **Rule:** Any identifier a human will type, read aloud, or transcribe from
  a sticker MUST avoid `0/O` and `1/I/l`. Machine-only IDs (chain head,
  signatures, hex device IDs) are fine as-is.
- **Regression check:** Grep for `BASE62` or the literal
  `0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz` in
  user-facing token / password paths.
- **Date learned:** 2026-04

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

### PlatformIO libraries can't see project `include/` by default
- **What happened:** CI build failed: `canary_config.h: No such file or directory`
  when compiling library components (`securacv_camera`, `securacv_crypto`, etc.)
- **Root cause:** PlatformIO builds libraries in isolation. The project's
  `include/` directory is only on the compiler search path for `src/`, not
  for `lib/` components. All 6 library modules `#include "canary_config.h"`
  but can't find it.
- **Fix:** Add `-I${PROJECT_DIR}/include` to `build_flags` in `[env]` section
  of `platformio.ini`. This makes the include path global across project
  source and library compilation.
- **Why not `lib_ldf_mode = deep+`?** That's slower and less explicit. A
  direct `-I` flag is clear about what's happening.
- **Date learned:** 2026-02

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

## Security Architecture Principles

> These are non-negotiable. They override convenience, features, roadmap
> priorities, and business considerations. Any code change that violates
> a principle is a security defect. See `docs/security/THREAT_MODEL.md` for full details.

### Private keys must never leave the device
- **Principle:** The Ed25519 private key has no export, backup, or read interface
- **Why:** A key that can be exported can be compelled (court orders, coercion)
- **Rule:** No API endpoint, log statement, debug output, or export path may
  include private key material. There is no recovery mechanism by design.
- **Regression check:** Script greps for private key references in API/export context
- **Date established:** 2026-02

### Zero outbound network connections
- **Principle:** The device makes zero outbound connections — it is a server, never a client
- **Why:** Outbound connections reveal the device exists, create interceptable metadata,
  and enable server-side coercion (e.g., compelled malicious updates)
- **Rule:** No DNS lookups, no NTP, no telemetry, no update checks, no cloud sync.
  MQTT is off by default. OTA updates are user-initiated only.
- **Watch for:** Any `WiFi.begin()`, `HTTPClient`, `WiFiClient`, `mqtt.connect()` calls
- **Regression check:** Script greps for outbound connection patterns
- **Date established:** 2026-02

### No identifier leaks
- **Principle:** The device must not leak identifiers that enable tracking or correlation
- **Why:** WiFi probe responses, BLE beacons, mDNS, and raw MACs are all tracking vectors
- **Rule:** BLE off at compile time, MACs hashed before storage, GPS coarsened at
  capture, no manufacturer OUI in AP BSSID
- **Regression check:** Script checks for raw MAC storage, BLE enablement, high-precision GPS
- **Date established:** 2026-02

### Evidence must be self-verifying
- **Principle:** Exported evidence bundles verifiable without ERRERlabs involvement
- **Why:** If verification requires ERRERlabs, ERRERlabs can be shut down, compelled,
  or compromised — making all evidence unverifiable
- **Rule:** Every export includes: signed records, hash chain, public key, and
  an offline HTML+JS verification page
- **Date established:** 2026-02

### TLS required — no HTTP fallback
- **Principle:** All API traffic must be encrypted. No plaintext HTTP.
- **Why:** Even on a local AP, an attacker in WiFi range can sniff plaintext
- **Rule:** HTTP requests get 301 redirect to HTTPS. No `http://` endpoints.
  `DEFAULT_TLS_REQUIRED` must be 1 in `secure_defaults.h`.
- **Regression check:** Script checks for HTTP listener without TLS redirect
- **Date established:** 2026-02

### Fail secure, not fail open
- **Principle:** When something fails, the device fails toward MORE security
- **Why:** A device that falls back to HTTP on TLS failure, or stops recording
  when storage is full, betrays users at exactly the wrong moment
- **Rule:** SD full → witness to RAM. Auth fail → lockout. TLS fail → reject.
  Chain corrupt → tamper alert. Never degrade silently.
- **Date established:** 2026-02

### Secure defaults in secure_defaults.h
- **Principle:** All security-sensitive compile-time defaults are centralized
  in `firmware/canary/include/secure_defaults.h`
- **Why:** Scattered defaults are easy to misconfigure. Centralized defaults
  with static assertions prevent accidental weakening.
- **Rule:** Production builds should define `SECURACV_ENFORCE_SECURE_DEFAULTS=1`
  to trigger compile-time checks against insecure values.
- **Date established:** 2026-02

---

## Networking & Captive Portal

### Captive-portal probes are per-platform — answer them, don't blanket-redirect
- **What happened:** During first-boot setup, phones connected to the
  `SecuraCV-XXXX` AP but then dropped it (or routed everything over cellular,
  so `canary.local` stopped resolving) before the user could finish setup.
- **Root cause:** Every OS connectivity-check URL was answered with a 200 +
  HTML instruction page, including Android's `/generate_204`. Android only
  treats a network as "validated/online" when that probe returns **HTTP 204
  No Content**; any other response flips it to "Wi-Fi has no internet,"
  which de-prioritises the AP and eventually disassociates.
- **Fix:** Answer probes per-platform (the "hybrid" strategy):
  - **Apple** (`/hotspot-detect.html`, `/library/test/success.html`) → the
    instruction page. This pops the Captive Network Assistant sheet (which
    renders static HTML fine), and iOS/macOS keep the association while it's
    open, so they never disconnect — guidance *and* a live link.
  - **Android** (`/generate_204`, `/gen_204`) → **204 No Content**. No sheet,
    no cellular fallback, stays connected. User opens `canary.local` manually.
  - **Windows** (`/connecttest.txt`, `/ncsi.txt`) → exact NCSI bodies
    `Microsoft Connect Test` / `Microsoft NCSI`.
- **Gotcha:** Probes are always sent over **plain HTTP** — keep these handlers
  on the port-80 server, never behind the HTTP→HTTPS redirect, or detection
  breaks. `canary.local` itself resolves via mDNS on the AP netif plus the
  setup DNS hijack for non-`.local` lookups.
- **Date learned:** 2026-05

### Captive DNS redirector must answer A queries only — NODATA for AAAA/HTTPS
- **What happened:** Even with the per-platform probes, `canary.local` and the
  redirect resolved slowly or not at all on Android Chrome.
- **Root cause:** The setup DNS responder appended an **A record to every
  query regardless of QTYPE**. Android Chrome fires `AAAA` (type 28) and
  `HTTPS`/SVCB (type 65) lookups in parallel with the `A` query; replying to
  those with an A-record answer is malformed, so the client waits/retries
  instead of falling back to IPv4.
- **Fix:** Parse the question's QTYPE. Answer the redirect A record only for
  `A` (type 1) queries; for everything else return **NOERROR with ANCOUNT=0
  (NODATA)** so the client immediately falls back to its A lookup. QNAMEs in
  queries aren't compressed, so a simple label walk finds the QTYPE.
- **Date learned:** 2026-05

### The captive DNS redirector runs for the AP's lifetime, not just first boot
- **What happened:** After provisioning, a phone joining the always-on
  management AP (e.g. because home WiFi dropped) hit the disconnect again —
  the per-platform probe handlers existed but were never reached.
- **Root cause:** The probe domains (`connectivitycheck.gstatic.com`, etc.)
  only resolve to the device when the captive DNS hijack is running, and it
  was gated on `setup_wizard::is_active()` — true only on first boot. With no
  hijack, the probe never reached the device's 204 handler, so the OS saw "no
  internet" and disconnected.
- **Fix:** Start the DNS redirector whenever the AP comes up and service
  `dns_process()` in `loop()` unconditionally (it self-guards on
  `s_dns_running`). The first-boot *wizard* (landing gate + 15-min timeout)
  stays separately gated on `is_active()`.
- **Why it's safe:** The softAP doesn't NAT, so AP clients have no upstream
  regardless; hijacking all A queries to the device is the intended captive
  behavior, not a regression. The device's own outbound DNS is unaffected (it
  uses the STA's resolver, never the local port-53 listener).
- **Date learned:** 2026-05

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
