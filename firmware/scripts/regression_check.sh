#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════
# SecuraCV Canary — Regression Guard
#
# Runs on every PR via GitHub Actions. Catches known anti-patterns
# and past bugs BEFORE compilation.
#
# Exit 0 = all checks pass
# Exit 1 = regression detected
# ═══════════════════════════════════════════════════════════════════

set -euo pipefail

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

REQUIRED_FILES=(
  "canary/src/main.cpp"
  "canary/include/canary_config.h"
  "canary/include/log_level.h"
  "canary/platformio.ini"
  "common/web/web_ui.h"
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
  check_warn "Hardcoded AP password 'witness2026' found — should be device-unique"
  echo "$AP_HITS" | while read -r line; do blue "  $line"; done
  blue "  See: LESSONS_LEARNED.md → Security → AP password must be device-unique"
else
  check_pass "No hardcoded default AP password"
fi

echo ""

# ── Check: No raw MAC addresses stored ─────────────────────────
echo "── Privacy: MAC address handling ──"

# Presence detection should never store raw MACs
MAC_HITS=$(grep -rn 'WiFi.macAddress()' "${SRC_DIRS[@]}" 2>/dev/null | grep -v "fingerprint\|device_id\|ap_ssid\|derive\|token\|hash\|//" | head -5 || true)
if [ -n "$MAC_HITS" ]; then
  check_warn "WiFi.macAddress() used outside identity/derivation context — verify not storing raw MACs"
  echo "$MAC_HITS" | while read -r line; do blue "  $line"; done
else
  check_pass "MAC address usage appears safe"
fi

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

echo ""

# ── Check: Constant-time comparison for auth ───────────────────
echo "── Security: Auth implementation ──"

# If Bearer auth exists, it MUST use constant-time comparison
AUTH_PRESENT=$(grep -rn "Bearer\|Authorization\|authenticate" "${SRC_DIRS[@]}" 2>/dev/null | grep -v "//" | head -1 || true)
if [ -n "$AUTH_PRESENT" ]; then
  CT_COMPARE=$(grep -rn "constant_time_compare\|volatile.*result.*|=\|crypto_verify" "${SRC_DIRS[@]}" 2>/dev/null || true)
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
    if echo "$CAM_PWDN" | grep -q "\-1"; then
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
  grep -q "SD_CS_PIN.*21\|SD_CS_PIN *21" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false
  grep -q "SD_SCK_PIN.*7\|SD_SCK_PIN *7" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false
  grep -q "SD_MISO_PIN.*8\|SD_MISO_PIN *8" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false
  grep -q "SD_MOSI_PIN.*9\|SD_MOSI_PIN *9" "$CONFIG_H" 2>/dev/null || SD_PINS_OK=false

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
  if [ "$BYTES" -gt 65536 ]; then
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
