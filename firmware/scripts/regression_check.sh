#!/usr/bin/env bash
set -euo pipefail
# ═══════════════════════════════════════════════════════════════════
# SecuraCV Canary — Regression Guard
#
# Runs on every PR via GitHub Actions. Catches known anti-patterns
# and past bugs BEFORE compilation.
#
# Exit 0 = all checks pass
# Exit 1 = regression detected
# ═══════════════════════════════════════════════════════════════════


# Resolve repo root (works from any working directory)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Directories to scan
CANARY_DIR="$FIRMWARE_DIR/canary"
COMMON_DIR="$FIRMWARE_DIR/common"
PROJECTS_DIR="$FIRMWARE_DIR/projects"
CONFIG_H="$CANARY_DIR/include/canary_config.h"

ERRORS=0
WARNINGS=0

red()    { echo -e "\033[0;31m✗ $1\033[0m"; }
green()  { echo -e "\033[0;32m✓ $1\033[0m"; }
yellow() { echo -e "\033[0;33m⚠ $1\033[0m"; }
blue()   { echo -e "\033[0;34mℹ $1\033[0m"; }

check_pass()  { green "$1"; }
check_fail()  { red "$1"; ERRORS=$((ERRORS + 1)); }
check_warn()  { yellow "$1"; WARNINGS=$((WARNINGS + 1)); }

echo "═══════════════════════════════════════════════════════════"
echo "  SecuraCV Canary — Regression Guard"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Collect all firmware source directories that exist
SRC_DIRS=()
for d in "$CANARY_DIR" "$COMMON_DIR" "$PROJECTS_DIR"; do
  [ -d "$d" ] && SRC_DIRS+=("$d")
done

if [ ${#SRC_DIRS[@]} -eq 0 ]; then
  red "No firmware source directories found under $FIRMWARE_DIR"
  exit 1
fi

# ── Check: Key files exist ──────────────────────────────────────
echo "── File structure ──"

# The live web UI is the canary-wap sketch's web_ui.h (checked by the
# size gate below); the unbuilt common/web/web_ui.h scaffold that used to
# be listed here was deleted as a dead duplicate (roadmap item 29).
REQUIRED_FILES=(
  "canary/src/main.cpp"
  "canary/include/canary_config.h"
  "canary/include/log_level.h"
  "canary/platformio.ini"
  "common/web/http_server.h"
)

for f in "${REQUIRED_FILES[@]}"; do
  if [ -f "$FIRMWARE_DIR/$f" ]; then
    check_pass "Found $f"
  else
    check_fail "MISSING: $f"
  fi
done

echo ""

# ── Check: mbedTLS API compatibility (ESP32 Core 3.x) ──────────
echo "── mbedTLS API (Core 3.x compatibility) ──"

# ESP32 Arduino Core 3.x removed _ret suffix from mbedTLS functions.
# Using _ret functions causes compile failure on Core 3.x.
# This was a painful lesson learned during initial development.

MBEDTLS_HITS=$(grep -rn "mbedtls_sha256_ret\|mbedtls_md_hmac_ret\|mbedtls_.*_ret(" "${SRC_DIRS[@]}" 2>/dev/null || true)
if [ -n "$MBEDTLS_HITS" ]; then
  check_fail "Found deprecated mbedtls _ret() functions — won't compile on Core 3.x"
  echo "$MBEDTLS_HITS" | while read -r line; do blue "  $line"; done
  blue "  Fix: Remove '_ret' suffix. E.g., mbedtls_sha256_ret() → mbedtls_sha256()"
else
  check_pass "No deprecated mbedtls _ret() functions"
fi

echo ""

# ── Check: No hardcoded AP password ────────────────────────────
echo "── Security: AP password ──"

AP_HITS=$(grep -rn '"witness2026"' "${SRC_DIRS[@]}" 2>/dev/null | grep -v "//.*witness2026" | grep -v "LEGACY\|REMOVED\|OLD" || true)
if [ -n "$AP_HITS" ]; then
  check_fail "Hardcoded AP password 'witness2026' found — must be device-unique"
  echo "$AP_HITS" | while read -r line; do blue "  $line"; done
  blue "  See: LESSONS_LEARNED.md → Security → AP password must be device-unique"
else
  check_pass "No hardcoded default AP password"
fi

echo ""

# ── Privacy guardrails (F-03): no raw MAC / no fine GPS in operator-facing output ──
# Every firmware tree now routes operator-facing identity through the shared salted
# device_pseudonym (firmware/common/identity/device_pseudonym.h) and GPS through
# gps_coarsen_deg (firmware/common/gnss/gps_privacy.h). Any raw MAC or un-coarsened
# lat/lon emission anywhere under the firmware source trees is a hard failure
# (Invariant III) — no longer a per-tree warning.

# report_privacy <subject> <newline-separated "file:line:..." hits>
# FAILs if any hits exist, passes if none.
report_privacy() {
  local subject="$1" hits
  hits=$(printf '%s\n' "$2" | grep -vE '^[[:space:]]*$' 2>/dev/null || true)
  if [ -n "$hits" ]; then
    check_fail "$subject (Invariant III)"
    printf '%s\n' "$hits" | while IFS= read -r l; do [ -n "$l" ] && blue "  $l"; done
  else
    check_pass "$subject: none found"
  fi
  return 0  # never trip `set -e`; failures are tallied via check_fail/ERRORS
}

echo "── Privacy: MAC address handling (F-03) ──"

# 1) The device's own efuse MAC must never be formatted as a raw MAC string. A file
#    that both reads the efuse MAC and contains a "%02X:..:%02X" format is emitting the
#    hardware address — use device_pseudonym (salted token) instead.
EFUSE_HITS=""
for f in $(grep -rlE 'esp_efuse_mac_get_default' "${SRC_DIRS[@]}" --include=*.h --include=*.cpp --include=*.ino 2>/dev/null | grep -viE 'test' || true); do
  m=$(grep -nE '%02[Xx]:%02[Xx]:%02[Xx]:%02[Xx]:%02[Xx]:%02[Xx]' "$f" 2>/dev/null | head -1 || true)
  if [ -n "$m" ]; then EFUSE_HITS="$EFUSE_HITS$f:$m"$'\n'; fi
done
report_privacy "Device efuse MAC formatted as a raw MAC string" "$EFUSE_HITS"

# 2) WiFi.macAddress() must not feed API payloads / logs (identity/derivation use is OK).
MAC_HITS=$(grep -rnE 'WiFi\.macAddress\(\)' "${SRC_DIRS[@]}" --include=*.h --include=*.cpp --include=*.ino 2>/dev/null | grep -viE 'fingerprint|device_id|ap_ssid|derive|token|hash|pseudonym|//|test' || true)
report_privacy "WiFi.macAddress() in a payload/log context" "$MAC_HITS"

# 3) ESP.getEfuseMac() is the 48-bit factory MAC — must not be emitted operator-facing
#    (e.g. as a "chip_id"). Hashing/derivation use (device_id, fingerprint) is allowed.
EFUSEMAC_HITS=$(grep -rnE 'getEfuseMac\(\)' "${SRC_DIRS[@]}" --include=*.h --include=*.cpp --include=*.ino 2>/dev/null | grep -viE 'fingerprint|device_id|derive|token|hash|pseudonym|//|test' || true)
report_privacy "ESP.getEfuseMac() in a payload/log context" "$EFUSEMAC_HITS"

echo ""

echo "── Privacy: GPS precision coarsening (F-03) ──"

# 1) Structured lat/lon emission (CBOR write_float / JSON ["lat"|"lon"] =) must pass
#    through gps_coarsen_deg(); the no-fix "= 0.0" sentinels are exempt.
GPS_RAW=$(grep -rnE 'write_float\([^)]*(lat|lon)|\["(lat|lon)"\][[:space:]]*=' "${SRC_DIRS[@]}" --include=*.ino --include=*.cpp --include=*.h 2>/dev/null \
  | grep -v 'gps_coarsen_deg' | grep -vE '=[[:space:]]*0\.0' | grep -viE '//|test' || true)
report_privacy "GPS lat/lon emitted without gps_coarsen_deg()" "$GPS_RAW"

# 2) No high-precision (>=4 dp) coordinate format strings in operator output.
# Match any precision >=4 dp: first digit 4-9, OR two-or-more digits (10, 14, 20, ...).
GPS_PREC=$(grep -rnE '%[0-9]*\.([4-9]|[1-9][0-9]+)f' "${SRC_DIRS[@]}" --include=*.ino --include=*.cpp --include=*.h 2>/dev/null \
  | grep -iE 'lat|lon|gps|coord' | grep -viE '//|test' || true)
report_privacy "High-precision lat/lon format string (>=4 dp)" "$GPS_PREC"

echo ""

# ── Check: Token not in witness chain ──────────────────────────
echo "── Security: Token isolation ──"

TOKEN_CHAIN_HITS=$(grep -rn 'api_token\|api_tkn' "${SRC_DIRS[@]}" 2>/dev/null | grep -i "chain\|witness\|record\|cbor\|payload" | grep -v "//" || true)
if [ -n "$TOKEN_CHAIN_HITS" ]; then
  check_fail "API token may be leaking into witness chain — tokens are transport-only"
  echo "$TOKEN_CHAIN_HITS" | while read -r line; do blue "  $line"; done
else
  check_pass "API token not referenced in chain/witness context"
fi

# ── Check: token / AP-password / device-id alphabet drops ambiguous glyphs ─
# Every human-read identifier (API tokens, AP passwords, device_id / AP-SSID
# suffix) must avoid ALL case variants of the confusion classes: 0/O/o and
# 1/I/i/l/L. Flag both the full base62 alphabet and the older 57-char alphabet
# that still leaked lowercase o/i and uppercase L.
# See LESSONS_LEARNED.md → "User-typed identifiers must use an unambiguous alphabet".
AMBIGUOUS_ALPHABET=$(grep -rnE '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz|23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz' "${SRC_DIRS[@]}" 2>/dev/null | grep -v "_archive" || true)
if [ -n "$AMBIGUOUS_ALPHABET" ]; then
  check_fail "Ambiguous alphabet in token/password/device-id path — must drop 0/O/o and 1/I/i/l/L"
  echo "$AMBIGUOUS_ALPHABET" | while read -r line; do blue "  $line"; done
else
  check_pass "Token/password/device-id alphabet free of ambiguous glyphs"
fi

# The device_id / AP-SSID suffix must NOT be raw hex (%02X%02X) — hex carries
# 0 and 1, which are exactly the glyphs users confuse. It has to flow through
# the unambiguous alphabet like the token/password paths do.
HEX_SUFFIX=$(grep -rnE 'snprintf\([^;]*"(SecuraCV-|%s)%02[Xx]%02[Xx]' "${SRC_DIRS[@]}" 2>/dev/null | grep -v "_archive" || true)
if [ -n "$HEX_SUFFIX" ]; then
  check_fail "device_id / AP-SSID built from raw hex — 0/1 glyphs reach users; use UNAMBIGUOUS_ALPHABET"
  echo "$HEX_SUFFIX" | while read -r line; do blue "  $line"; done
else
  check_pass "device_id / AP-SSID suffix avoids raw-hex glyphs"
fi

echo ""

# ── Check: Constant-time comparison for auth ───────────────────
echo "── Security: Auth implementation ──"

# If Bearer auth exists, it MUST use constant-time comparison
AUTH_PRESENT=$(grep -rn "Bearer\|Authorization\|authenticate" "${SRC_DIRS[@]}" 2>/dev/null | grep -v "//" | head -1 || true)
if [ -n "$AUTH_PRESENT" ]; then
  CT_COMPARE=$(grep -rn "constant_time_compare\|volatile.*result.*\|=\|crypto_verify" "${SRC_DIRS[@]}" 2>/dev/null | grep -v "//" || true)
  if [ -n "$CT_COMPARE" ]; then
    check_pass "Auth appears to use constant-time comparison"
  else
    STRCMP_AUTH=$(grep -rn "strcmp.*token\|== token\|\.equals.*token" "${SRC_DIRS[@]}" 2>/dev/null | grep -v "//" || true)
    if [ -n "$STRCMP_AUTH" ]; then
      check_fail "Token comparison uses strcmp/equals — vulnerable to timing attacks"
      blue "  Fix: Use constant_time_compare() with volatile accumulator"
    else
      check_warn "Auth present but couldn't verify constant-time comparison"
    fi
  fi
else
  blue "Auth not yet implemented (expected if pre-token-generation)"
fi

echo ""

# ── Check: Camera pin definitions ──────────────────────────────
echo "── Hardware: Camera configuration ──"

# XIAO ESP32S3 Sense camera pins are specific. Wrong pins = camera init fails silently.
if [ -f "$CONFIG_H" ]; then
  CAM_PWDN=$(grep -n "CAM_PIN_PWDN\|PWDN_GPIO_NUM" "$CONFIG_H" 2>/dev/null | head -1 || true)
  if [ -n "$CAM_PWDN" ]; then
    if echo "$CAM_PWDN" | grep -qE '[[:space:]=-]-1([[:space:]]|$)'; then
      check_pass "Camera PWDN pin set to -1 (correct for XIAO ESP32S3 Sense)"
    else
      check_warn "Camera PWDN pin may not be -1 — XIAO ESP32S3 Sense requires PWDN=-1"
    fi
  else
    blue "Camera pin definitions not found in canary_config.h"
  fi
else
  blue "canary_config.h not found (okay if camera feature disabled)"
fi

echo ""

# ── Check: SD card SPI pins ────────────────────────────────────
echo "── Hardware: SD card SPI pins ──"

# XIAO ESP32S3 Sense SD card SPI pins: CS=21, SCK=7, MISO=8, MOSI=9
if [ -f "$CONFIG_H" ]; then
  SD_PINS_OK=true
  grep -qE "SD_CS_PIN\s+21\b" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false
  grep -qE "SD_SCK_PIN\s+7\b" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false
  grep -qE "SD_MISO_PIN\s+8\b" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false
  grep -qE "SD_MOSI_PIN\s+9\b" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false

  if [ "$SD_PINS_OK" = true ]; then
    check_pass "SD SPI pins correct for XIAO ESP32S3 Sense (CS=21,SCK=7,MISO=8,MOSI=9)"
  else
    check_warn "SD SPI pins couldn't be fully verified — expected CS=21,SCK=7,MISO=8,MOSI=9"
  fi
else
  blue "canary_config.h not found (okay if SD feature disabled)"
fi

echo ""

# ── Check: Feature flags defined ───────────────────────────────
echo "── Architecture: Feature flags ──"

EXPECTED_FLAGS=(
  "FEATURE_SD_STORAGE"
  "FEATURE_WIFI_AP"
  "FEATURE_HTTP_SERVER"
  "FEATURE_CAMERA_PEEK"
  "FEATURE_WATCHDOG"
)

for flag in "${EXPECTED_FLAGS[@]}"; do
  if grep -rn "#define $flag\b\|#ifndef $flag" "$CANARY_DIR" 2>/dev/null | head -1 > /dev/null 2>&1; then
    check_pass "Feature flag $flag defined"
  else
    check_warn "Feature flag $flag not found"
  fi
done

echo ""

# ── Check: No outbound network connections ────────────────────
echo "── Security: Zero phone-home ──"

# The device must NEVER initiate outbound connections.
# WiFi.begin() connects to an external AP (station mode).
# HTTPClient, WiFiClient, mqtt.connect() are outbound patterns.
# WiFiAP, WiFi.softAP are acceptable (AP mode = inbound).
OUTBOUND_HITS=$(grep -rEn 'WiFi\.begin\(|HTTPClient|WiFiClient[[:space:]]|WiFiClientSecure|mqtt\.connect\(|\.connect\(' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -v "//.*WiFi\|WiFi\.softAP\|WiFiAP\|WiFiServer\|#if.*FEATURE_HA_MQTT\|#ifdef.*MQTT\|\.h:\|\.md:" \
  | grep -v "FEATURE_MESH_NETWORK\|mesh\|example\|test" \
  | head -10 || true)
if [ -n "$OUTBOUND_HITS" ]; then
  check_warn "Possible outbound network connections detected — verify these are gated by feature flags"
  echo "$OUTBOUND_HITS" | while read -r line; do blue "  $line"; done
  blue "  Principle 2: Device must make ZERO outbound connections by default"
  blue "  See: docs/security/THREAT_MODEL.md → Principle 2: Zero Phone-Home"
else
  check_pass "No ungated outbound network connection patterns found"
fi

echo ""

# ── Check: Private key never in API/export/log ────────────────
echo "── Security: Private key isolation ──"

# Ed25519 private key must never appear in any export, API response, log, or debug output.
# This check looks for private key bytes being printed, serialized to JSON, written to
# SD card, or included in API responses. It excludes legitimate internal uses like
# Ed25519::sign(), derive_api_token(), nvs_store_key(), and nvs_load_key().
PRIVKEY_LEAK=$(grep -rEn 'priv(ate)?_?key|privkey|NVS_KEY_PRIV' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -i 'print\|log\|serial\|json\|response\|send\|export\|write.*sd\|write.*file\|transmit\|broadcast' \
  | grep -v "//\|store_key\|load_key\|nvs_.*key\|\.h:\|\.md:\|derive_api_token\|Ed25519::sign\|crypto_sign\|HKDF\|hmac" \
  | head -10 || true)
if [ -n "$PRIVKEY_LEAK" ]; then
  check_fail "Private key may be leaking to log/API/export — CRITICAL security violation"
  echo "$PRIVKEY_LEAK" | while read -r line; do blue "  $line"; done
  blue "  Principle 1: Keys NEVER leave the device"
  blue "  See: docs/security/THREAT_MODEL.md → Principle 1: Keys Never Leave the Device"
else
  check_pass "No private key references in log/API/export contexts"
fi

echo ""

# ── Check: No raw MAC storage in presence detection ──────────
echo "── Privacy: Presence detection MAC handling ──"

# Presence detection must hash MACs before storage. Look for patterns
# that store or transmit raw BSSID/MAC data.
RAW_MAC_STORE=$(grep -rEn 'bssid|BSSID|macAddress' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -i 'store\|save\|write\|persist\|sd\|nvs\|put\|append\|push_back\|log' \
  | grep -v "//\|hash\|fingerprint\|derive\|digest\|sha256\|\.h:\|\.md:" \
  | head -10 || true)
if [ -n "$RAW_MAC_STORE" ]; then
  check_warn "Raw MAC/BSSID may be stored without hashing — verify privacy compliance"
  echo "$RAW_MAC_STORE" | while read -r line; do blue "  $line"; done
  blue "  Principle 3: No identifier leaks. MACs must be hashed before any storage."
else
  check_pass "No raw MAC storage patterns detected"
fi

echo ""

# ── Check: TLS required (no HTTP fallback) ────────────────────
echo "── Security: TLS enforcement ──"

# Look for patterns that might serve HTTP without TLS redirect
HTTP_FALLBACK=$(grep -rEn 'server\.begin[[:space:]]*\([[:space:]]*80|:80\b|HTTP_PORT[[:space:]]*=?[[:space:]]*80|listen.*80' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -v "//\|redirect\|301\|https\|\.md:" \
  | head -5 || true)
if [ -n "$HTTP_FALLBACK" ]; then
  check_warn "HTTP port 80 listener found — verify it only serves 301 redirect to HTTPS"
  echo "$HTTP_FALLBACK" | while read -r line; do blue "  $line"; done
  blue "  Principle 7: TLS required for all API access"
else
  check_pass "No unguarded HTTP listeners found"
fi

echo ""

# ── Check: secure_defaults.h exists ──────────────────────────
echo "── Security: Secure defaults header ──"

if [ -f "$CANARY_DIR/include/secure_defaults.h" ]; then
  check_pass "secure_defaults.h exists"
  # Verify key defaults are present
  DEFAULTS_OK=true
  for def in DEFAULT_BLE_ENABLED DEFAULT_TLS_REQUIRED DEFAULT_MQTT_ENABLED DEFAULT_PRESENCE_STORE_RAW_MAC; do
    if ! grep -q "$def" "$CANARY_DIR/include/secure_defaults.h" 2>/dev/null; then
      check_warn "Missing $def in secure_defaults.h"
      DEFAULTS_OK=false
    fi
  done
  if [ "$DEFAULTS_OK" = true ]; then
    check_pass "All critical security defaults defined in secure_defaults.h"
  fi
else
  check_fail "secure_defaults.h is missing! Security defaults must be centralized."
  blue "  See: docs/security/THREAT_MODEL.md → Implementation Review Checklist"
fi

echo ""

# ── Check: docs/security/SECURITY_MODEL.md exists ────────────
echo "── Documentation: Security Model ──"

REPO_ROOT="$(cd "$FIRMWARE_DIR/.." && pwd)"
if [ -f "$REPO_ROOT/docs/security/SECURITY_MODEL.md" ]; then
  SM_LINES=$(wc -l < "$REPO_ROOT/docs/security/SECURITY_MODEL.md")
  if [ "$SM_LINES" -lt 20 ]; then
    check_fail "docs/security/SECURITY_MODEL.md seems incomplete ($SM_LINES lines)"
  else
    check_pass "docs/security/SECURITY_MODEL.md exists ($SM_LINES lines)"
  fi
else
  check_fail "docs/security/SECURITY_MODEL.md is missing — must be included in every evidence export"
fi

if [ -f "$REPO_ROOT/docs/security/THREAT_MODEL.md" ]; then
  TM_LINES=$(wc -l < "$REPO_ROOT/docs/security/THREAT_MODEL.md")
  check_pass "docs/security/THREAT_MODEL.md exists ($TM_LINES lines)"
else
  check_warn "docs/security/THREAT_MODEL.md is missing — needed for developer/auditor reference"
fi

echo ""

# ── Check: No localStorage in web UI ──────────────────────────
echo "── Security: Dashboard storage ──"

WEB_UI_FILES=$(find "$FIRMWARE_DIR" -name "web_ui.h" -o -name "securacv_webui.*" 2>/dev/null || true)
STORAGE_HITS=""
for wf in $WEB_UI_FILES; do
  # Exclude lines that are comments (// or /* ... */)
  HITS=$(grep -n "localStorage\|sessionStorage\|document\.cookie" "$wf" 2>/dev/null | grep -v "^\s*[0-9]*:\s*//" | grep -v "^\s*[0-9]*:\s*\*" || true)
  if [ -n "$HITS" ]; then
    STORAGE_HITS="$STORAGE_HITS\n$wf: $HITS"
  fi
done

if [ -n "$STORAGE_HITS" ]; then
  check_fail "Dashboard uses browser storage — tokens must stay in JS variables only"
  echo -e "$STORAGE_HITS" | while read -r line; do [ -n "$line" ] && blue "  $line"; done
  blue "  Fix: Use 'let apiToken = null;' — never persist tokens"
else
  check_pass "No browser storage APIs in dashboard"
fi

echo ""

# ── Check: GPS coordinate precision ───────────────────────────
echo "── Privacy: GPS precision ──"

# SecuraCV coarsens GPS. Raw high-precision coordinates should not leak.
GPS_PRECISION=$(grep -rn '%.8f\|%.7f\|%.6f' "${SRC_DIRS[@]}" 2>/dev/null | grep -i "lat\|lon\|gps" | grep -v "//" || true)
if [ -n "$GPS_PRECISION" ]; then
  check_warn "High-precision GPS format found (>5 decimal places) — verify coarsening is applied"
  echo "$GPS_PRECISION" | while read -r line; do blue "  $line"; done
else
  check_pass "No high-precision GPS format strings found"
fi

echo ""

# ── Check: Watchdog configuration ──────────────────────────────
echo "── Reliability: Watchdog ──"

WDT_PRESENT=$(grep -rn "esp_task_wdt" "${SRC_DIRS[@]}" 2>/dev/null | head -1 || true)
if [ -n "$WDT_PRESENT" ]; then
  check_pass "Watchdog timer configured"
  # Verify ESP-IDF version guard if struct-based API is used
  WDT_CONFIG_T=$(grep -rn "esp_task_wdt_config_t\|esp_task_wdt_reconfigure" "${SRC_DIRS[@]}" 2>/dev/null | grep -v "//" || true)
  if [ -n "$WDT_CONFIG_T" ]; then
    WDT_VERSION_GUARD=$(grep -rn "ESP_IDF_VERSION" "${SRC_DIRS[@]}" 2>/dev/null | grep -i "wdt\|watchdog\|5.*0.*0" | head -1 || true)
    if [ -z "$WDT_VERSION_GUARD" ]; then
      # Check if the version guard is near the wdt code (within same file)
      for wdt_file in $(grep -rl "esp_task_wdt_config_t\|esp_task_wdt_reconfigure" "${SRC_DIRS[@]}" 2>/dev/null); do
        if grep -q "ESP_IDF_VERSION" "$wdt_file" 2>/dev/null; then
          WDT_VERSION_GUARD="found"
          break
        fi
      done
    fi
    if [ -n "$WDT_VERSION_GUARD" ]; then
      check_pass "Watchdog API uses ESP-IDF version guard"
    else
      check_fail "esp_task_wdt_config_t used without ESP_IDF_VERSION guard — breaks on ESP-IDF 4.x"
      blue "  Fix: #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) around struct-based API"
    fi
  fi
else
  check_warn "No watchdog timer found — device may hang without recovery"
fi

echo ""

# ── Check: web_ui.h size ──────────────────────────────────────
echo "── Build: web_ui.h size ──"

for wui in $(find "$FIRMWARE_DIR" -name "web_ui.h" 2>/dev/null); do
  LINES=$(wc -l < "$wui")
  BYTES=$(wc -c < "$wui")
  REL_PATH="${wui#$FIRMWARE_DIR/}"
  # If a sibling web_assets_gz.h exists, the raw literal is compiled out
  # (CANARY_WEB_ASSETS_GZIPPED) and the binary ships the gzip copy — so the
  # raw header size is no longer the flash footprint, only source-of-truth size.
  if [ -f "$(dirname "$wui")/web_assets_gz.h" ]; then
    check_pass "$REL_PATH source: ${LINES} lines, ${BYTES} bytes (shipped gzip — see web_assets_gz.h)"
  elif [ "$BYTES" -gt 65536 ]; then
    check_warn "$REL_PATH is ${BYTES} bytes (>64KB) — may cause PROGMEM issues"
    blue "  Consider: Split into web_ui_css.h + web_ui_js.h + web_ui_html.h"
  else
    check_pass "$REL_PATH size: ${LINES} lines, ${BYTES} bytes"
  fi
done

echo ""

# ── Check: No debug flags left on ─────────────────────────────
echo "── Build: Debug flags ──"

DEBUG_FLAGS_ON=$(grep -rn '#define DEBUG_\w\+\s\+1' "$CANARY_DIR" 2>/dev/null | grep -v "platformio\|//.*#define" || true)
if [ -n "$DEBUG_FLAGS_ON" ]; then
  check_warn "Debug flags enabled (okay for dev, should be 0 for release):"
  echo "$DEBUG_FLAGS_ON" | while read -r line; do blue "  $line"; done
else
  check_pass "All DEBUG_ flags are 0 or undefined"
fi

echo ""

# ── Check: Mesh secret persistence is gated on flash encryption ────────
# The ESP-NOW "Opera" mesh uses a long-lived shared secret (opera_secret). It must
# NEVER be written to NVS unless flash encryption is on, or the secret sits in
# plaintext at rest. The persistence layer (mesh_state.cpp) enforces this: its
# save_*/load_* paths return false when !esp_flash_encryption_enabled(), so on an
# FE-off board the secret is not persisted (the live in-RAM session is allowed for
# the current boot by design — see firmware/canary/src/main.cpp on_pairing_succeeded
# — but nothing confidential lands in unencrypted NVS). This guard asserts that FE
# check is not silently removed from the mesh persistence/impl files. It does NOT,
# and cannot statically, prove the *activation* path fails closed — see issue #610
# C2 / the bench runbook for the on-device check, and the open design question of
# whether live activation should also refuse on FE-off boards.
echo "── Security: Mesh secret persistence is FE-gated ──"

MESH_IMPL_FILES=$(find "$FIRMWARE_DIR" -type f \( -name "mesh_network.cpp" -o -name "mesh_state.cpp" \) \
  -not -path "*/_archive/*" 2>/dev/null)

if [ -z "$MESH_IMPL_FILES" ]; then
  check_warn "No mesh implementation files found (mesh_network.cpp / mesh_state.cpp)"
else
  while IFS= read -r mf; do
    [ -z "$mf" ] && continue
    rel=${mf#"$FIRMWARE_DIR/"}
    if grep -q "esp_flash_encryption_enabled" "$mf"; then
      check_pass "Mesh FE persistence gate present: $rel"
    else
      check_fail "Mesh impl '$rel' has no esp_flash_encryption_enabled() gate — the opera_secret must never be persisted to unencrypted NVS (#610)"
    fi
  done <<< "$MESH_IMPL_FILES"
fi

echo ""

# ── Check: on-glass text stays inside the display font's alphabet ──
echo "── Display: font glyph range ──"

# LVGL's built-in Montserrat covers 0x20-0x7F, 0xB0, U+2022 and the
# FontAwesome symbols — nothing else. An out-of-range codepoint draws a
# hollow box with no build error, which is how a middle dot shipped and
# every date line on the glass read "Sunday [] Aug 9". The check runs in
# firmware.yml too; having it here means you see it before you push.
GLYPH_CHECK="$SCRIPT_DIR/check_display_glyphs.py"
if [ -f "$GLYPH_CHECK" ]; then
  if python3 "$GLYPH_CHECK" >/dev/null 2>&1; then
    check_pass "on-glass text stays inside the font's glyph range"
  else
    check_fail "on-glass text uses characters the display font cannot draw"
    python3 "$GLYPH_CHECK" 2>&1 | sed 's/^/    /' || true
  fi
else
  check_warn "check_display_glyphs.py missing — glyph range unchecked"
fi

echo ""

# ── Check: LESSONS_LEARNED.md exists ──────────────────────────
echo "── Documentation: Lessons Learned ──"

if [ -f "$FIRMWARE_DIR/LESSONS_LEARNED.md" ]; then
  LL_LINES=$(wc -l < "$FIRMWARE_DIR/LESSONS_LEARNED.md")
  if [ "$LL_LINES" -lt 10 ]; then
    check_fail "LESSONS_LEARNED.md seems empty ($LL_LINES lines)"
  else
    check_pass "LESSONS_LEARNED.md exists ($LL_LINES lines)"
  fi
else
  check_fail "firmware/LESSONS_LEARNED.md is missing!"
fi

echo ""

# ── Summary ────────────────────────────────────────────────────
echo "═══════════════════════════════════════════════════════════"
if [ $ERRORS -gt 0 ]; then
  red "FAILED: $ERRORS errors, $WARNINGS warnings"
  echo ""
  echo "Fix the errors above before merging."
  exit 1
elif [ $WARNINGS -gt 0 ]; then
  yellow "PASSED with $WARNINGS warnings"
  echo ""
  echo "Warnings are non-blocking but should be addressed."
  exit 0
else
  green "ALL CHECKS PASSED"
  exit 0
fi
