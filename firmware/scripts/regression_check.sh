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
#
# --strict: every warning raised in a "Security:" or "Privacy:" section
# counts as a failure (the release-readiness bar — "zero critical warnings",
# firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md §7). Without it
# (PR CI) those stay advisory. A strict pass is only meaningful because the
# greps below match real call shapes, and because the accepted exceptions
# (documented plaintext listeners, the display line's disclosed outbound
# paths) live in reviewed allowlists that fail when an entry goes stale.
# ═══════════════════════════════════════════════════════════════════

STRICT=0
for arg in "$@"; do
  case "$arg" in
    --strict) STRICT=1 ;;
    -h|--help)
      echo "usage: $0 [--strict]"
      echo "  --strict  Security:/Privacy: warnings count as failures"
      exit 0 ;;
    *) echo "unknown argument: $arg (try --help)" >&2; exit 2 ;;
  esac
done


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

SECTION=""
section()     { SECTION="$1"; echo "── $1 ──"; }
check_pass()  { green "$1"; }
check_fail()  { red "$1"; ERRORS=$((ERRORS + 1)); }
# In --strict mode a Security:/Privacy: warning is a failure; everything else
# (hardware pin sanity, build hygiene) stays a warning in both modes.
check_warn()  {
  if [ "$STRICT" -eq 1 ] && { [[ "$SECTION" == Security:* ]] || [[ "$SECTION" == Privacy:* ]]; }; then
    check_fail "[strict] $1"
  else
    yellow "$1"; WARNINGS=$((WARNINGS + 1))
  fi
}

# Drop hits whose CONTENT (after file:line:) starts as a comment — a leading
# //, /*, * or # — so prose about a pattern is never mistaken for the pattern.
# A trailing comment does NOT exempt a line of code.
drop_comment_lines() {
  awk '{ s = $0; sub(/^[^:]*:[0-9]+:/, "", s); if (s !~ /^[[:space:]]*(\/\/|\/\*|\*|#)/) print }'
}

# allowlist_filter "<entries>" < hits
#   entries: one per line, "<path ERE><TAB><content ERE><TAB><reason>".
#   Prints "HIT<TAB><hit>" for every hit no entry covers, and
#   "STALE<TAB><path ERE>  <content ERE>" for every entry that covered
#   nothing — an allowlist that cannot rot, like the route audits'.
allowlist_filter() {
  # Passed through the environment, not `awk -v`: -v expands backslash
  # escapes, which would turn the entries' `\.` and `\(` into regex syntax.
  ALLOWLIST_ENTRIES="$1" awk '
    BEGIN {
      n = split(ENVIRON["ALLOWLIST_ENTRIES"], E, "\n")
      for (i = 1; i <= n; i++) {
        if (E[i] ~ /^[[:space:]]*$/) continue
        split(E[i], F, "\t"); P[i] = F[1]; C[i] = F[2]; used[i] = 0; live[i] = 1
      }
    }
    {
      file = $0; sub(/:[0-9]+:.*/, "", file)
      content = $0; sub(/^[^:]*:[0-9]+:/, "", content)
      covered = 0
      for (i in live) if (file ~ P[i] && content ~ C[i]) { used[i] = 1; covered = 1 }
      if (!covered) print "HIT\t" $0
    }
    END { for (i in live) if (!used[i]) print "STALE\t" P[i] "  " C[i] }'
}

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
section "File structure"

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
section "mbedTLS API (Core 3.x compatibility)"

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
section "Security: AP password"

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

section "Privacy: MAC address handling (F-03)"

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

section "Privacy: GPS precision coarsening (F-03)"

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

# ── Check: canary-wap event-time bucket floor (Invariant III) ──
# The ten-minute floor is written once, in config_logic.h (kTimeBucketFloorMs,
# pinned by its own static_assert and by test_config_logic.cpp). The sketch
# must define TIME_BUCKET_MS from it and the Device tab's number field must
# not offer a finer value, so the floor cannot drift back in one of the three
# places while the other two stay green.
echo "── Privacy: canary-wap time-bucket floor (Invariant III) ──"
WAP_SKETCH_DIR="$PROJECTS_DIR/canary-wap/arduino/canary_wap"
if [ -f "$WAP_SKETCH_DIR/config_logic.h" ]; then
  TB_FLOOR=$(sed -nE 's/^constexpr uint32_t kTimeBucketFloorMs = ([0-9]+)u?;.*/\1/p' "$WAP_SKETCH_DIR/config_logic.h")
  if [ -z "$TB_FLOOR" ]; then
    check_fail "config_logic.h: no 'constexpr uint32_t kTimeBucketFloorMs = <ms>' line"
  elif [ "$TB_FLOOR" -lt 600000 ] || [ $((TB_FLOOR % 600000)) -ne 0 ]; then
    check_fail "config_logic.h: kTimeBucketFloorMs = ${TB_FLOOR} is not a whole multiple of the ten-minute grid"
  else
    if grep -qE '^static const uint32_t TIME_BUCKET_MS[[:space:]]*=[[:space:]]*config_logic::kTimeBucketFloorMs;' "$WAP_SKETCH_DIR/canary_wap.ino"; then
      check_pass "canary_wap.ino TIME_BUCKET_MS is config_logic::kTimeBucketFloorMs (${TB_FLOOR} ms)"
    else
      check_fail "canary_wap.ino: TIME_BUCKET_MS must be defined as config_logic::kTimeBucketFloorMs, not a literal"
    fi
    UI_FIELD=$(grep -oE '<input[^>]*id="configTimeBucket"[^>]*>' "$WAP_SKETCH_DIR/web_ui.h" || true)
    UI_MIN=$(printf '%s' "$UI_FIELD" | sed -nE 's/.* min="([0-9]+)".*/\1/p')
    UI_VAL=$(printf '%s' "$UI_FIELD" | sed -nE 's/.* value="([0-9]+)".*/\1/p')
    if [ -n "$UI_MIN" ] && [ "$UI_MIN" = "$TB_FLOOR" ] && [ -n "$UI_VAL" ] && [ "$UI_VAL" -ge "$TB_FLOOR" ]; then
      check_pass "web_ui.h configTimeBucket min=${UI_MIN} value=${UI_VAL} (floor ${TB_FLOOR} ms)"
    else
      check_fail "web_ui.h configTimeBucket must have min=\"${TB_FLOOR}\" and a value at or above it (found min=\"${UI_MIN}\" value=\"${UI_VAL}\")"
    fi
  fi
else
  check_warn "canary-wap config_logic.h not found — time-bucket floor not checked"
fi

echo ""

# ── Check: canary time-bucket floor (Invariant III) ──
# The canary tree's chain bucket is canary_config.h's TIME_BUCKET_MS, the
# ten-minute grid. Two stragglers once offered the five-second grid it retired
# (F44): an unused DEFAULT_GPS_COARSENING_MS 5000 in secure_defaults.h, and a
# Device Configuration form whose Time Bucket field defaulted to 5000 and
# posted to an /api/config route the canary never registered. Both are gone.
# This holds the line: every canary `#define ..._MS <literal>` whose name says
# BUCKET or COARSEN sits on the ten-minute grid, and a web-UI time-bucket
# field, if one comes back, may not offer less than it.
section "Privacy: canary time-bucket floor (Invariant III)"
CANARY_TB_FLOOR=600000
TB_BAD=""
while IFS= read -r tb_line; do
  [ -n "$tb_line" ] || continue
  tb_val=$(printf '%s' "$tb_line" | sed -nE 's/.*#define[[:space:]]+[A-Z0-9_]+[[:space:]]+([0-9]+).*/\1/p')
  if [ -z "$tb_val" ] || [ "$tb_val" -lt "$CANARY_TB_FLOOR" ] || [ $((tb_val % CANARY_TB_FLOOR)) -ne 0 ]; then
    TB_BAD="${TB_BAD}${tb_line#"$FIRMWARE_DIR"/}\n"
  fi
done < <(grep -rnE '^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Z0-9_]*(BUCKET|COARSEN)[A-Z0-9_]*_MS[[:space:]]+[0-9]+' \
           "$CANARY_DIR/include" "$CANARY_DIR/src" "$CANARY_DIR/lib" 2>/dev/null || true)
if [ -n "$TB_BAD" ]; then
  check_fail "canary time-bucket constant below or off the ten-minute grid (${CANARY_TB_FLOOR} ms):"
  echo -e "$TB_BAD" | while read -r line; do [ -z "$line" ] || blue "  $line"; done
else
  check_pass "every canary BUCKET/COARSEN _MS literal is on the ten-minute grid"
fi
CANARY_TB_FIELDS=$(grep -rhoE '<input[^>]*id="configTimeBucket"[^>]*>' "$CANARY_DIR/lib" "$CANARY_DIR/src" 2>/dev/null || true)
if [ -z "$CANARY_TB_FIELDS" ]; then
  check_pass "canary web UI offers no time-bucket field (the bucket is not runtime-configurable)"
else
  while IFS= read -r tb_field; do
    UI_MIN=$(printf '%s' "$tb_field" | sed -nE 's/.* min="([0-9]+)".*/\1/p')
    UI_VAL=$(printf '%s' "$tb_field" | sed -nE 's/.* value="([0-9]+)".*/\1/p')
    if [ -n "$UI_MIN" ] && [ "$UI_MIN" = "$CANARY_TB_FLOOR" ] && [ -n "$UI_VAL" ] && [ "$UI_VAL" -ge "$CANARY_TB_FLOOR" ]; then
      check_pass "canary web UI configTimeBucket min=${UI_MIN} value=${UI_VAL} (floor ${CANARY_TB_FLOOR} ms)"
    else
      check_fail "canary web UI configTimeBucket must have min=\"${CANARY_TB_FLOOR}\" and a value at or above it (found min=\"${UI_MIN}\" value=\"${UI_VAL}\")"
    fi
  done <<< "$CANARY_TB_FIELDS"
fi

echo ""

# Keyword filters below look at a hit's CONTENT, never its path: `grep -rn`
# prefixes every line with `file:line:`, and a checkout path that happened to
# contain "witness" or "transmit" (a worktree name, a user's home directory)
# used to fail these checks on files nobody touched.
content_grep() { # content_grep <ERE> — case-insensitive match on the text after file:line:
  awk -v re="$1" '{ s = $0; sub(/^[^:]*:[0-9]+:/, "", s); if (tolower(s) ~ tolower(re)) print }'
}

# ── Check: Token not in witness chain ──────────────────────────
section "Security: Token isolation"

TOKEN_CHAIN_HITS=$(grep -rn 'api_token\|api_tkn' "${SRC_DIRS[@]}" 2>/dev/null | content_grep 'chain|witness|record|cbor|payload' | grep -v "//" || true)
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
section "Security: Auth implementation"

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
section "Hardware: Camera configuration"

# XIAO ESP32S3 Sense camera pins are specific. Wrong pins = camera init fails silently.
if [ -f "$CONFIG_H" ]; then
  # The #define line itself — the first mention used to be the `#ifndef`
  # guard above it, which carries no value and warned on every run.
  CAM_PWDN=$(grep -nE "^[[:space:]]*#[[:space:]]*define[[:space:]]+(CAM_PIN_PWDN|PWDN_GPIO_NUM)\b" "$CONFIG_H" 2>/dev/null | head -1 || true)
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
section "Hardware: SD card SPI pins"

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
section "Architecture: Feature flags"

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
section "Security: Zero phone-home"

# Phone-home is a connection to a destination COMPILED INTO the firmware: a
# string-literal URL or host handed to an HTTP client or a socket connect,
# a literal time server, a literal name lookup. Those are the call shapes
# matched here. A destination the OWNER provisions — the home router
# (WiFi.begin with stored credentials), an MQTT broker host, a signed OTA
# manifest — is an opt-in path gated by its feature flag and checked where it
# lives (check_ota_channels.py, the mqtt_transport host tests); the old grep
# flagged every WiFi.begin( and .connect( (WebAudio's node graph included),
# fired on every run, and so guarded nothing. Destinations that ARE compiled
# in and disclosed live in DISCLOSED_OUTBOUND below, each with its reason;
# anything else fails --strict.
DISCLOSED_OUTBOUND=$(printf '%s\t%s\t%s\n' \
  'canary-display/.*(tz_auto\.cpp|main\.cpp|canary_display\.ino)$' 'configTzTime\([^)]*"pool\.ntp\.org", *"time\.nist\.gov"' \
    'docs/security/SECURITY_MODEL.md disclosed outbound path 3 (SNTP, the display line)' \
  'canary-display/.*tz_auto\.cpp$' '\.begin\("http://ip-api\.com/' \
    'docs/security/SECURITY_MODEL.md disclosed outbound path 4 (timezone lookup, the display line: compile-time opt-in CD_TZ_WEB_LOOKUP)' \
  'canary-display/.*wx_direct\.cpp$' 'http\.begin\(client, WX_HOST, 443,.*WX_HOST = "api\.open-meteo\.com"' \
    'docs/security/SECURITY_MODEL.md disclosed outbound path 5 (standalone weather, the display line: runtime opt-in on the glass, FEATURE_STANDALONE_WEATHER; a named destination, pinned to its value)')
OUTBOUND_LITERAL=$(grep -rEn '\.begin\([[:space:]]*"(https?://|[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+")|\.connect\([[:space:]]*"|\.url[[:space:]]*=[[:space:]]*"https?://|config(Tz)?Time\([^)]*"|(esp_)?sntp_setservername\([^)]*"|getaddrinfo\([[:space:]]*"' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -v "\.md:\|/tests_host/\|/test_\|/examples\?/\|\.pio/" \
  | drop_comment_lines || true)

# The usual way a compiled-in destination is written is behind a NAME —
# `#define TELEMETRY_URL "https://…"` or `const char* WX_HOST = "api.…"` —
# and handed to the same client calls; the literal grep above cannot see
# that shape (the display's standalone-weather fetch is one). So resolve one
# level: collect every name bound to a URL literal (with a host after the
# scheme) or a dotted-host literal — a #define, backslash-continued or not,
# or a char/String initializer — then flag every client call site that
# passes one of those names. Each hit carries its resolution
# ("⇐ NAME = "value" (file:line)") so a DISCLOSED_OUTBOUND entry can match
# on the call text. Not resolved: a name bound to another name, or a
# destination copied into a runtime variable first — owner-provisioned
# destinations (broker host, OTA manifest URL) arrive that way and are
# checked where they live (check_ota_channels.py, mqtt_transport tests).
SRC_FILE_GLOBS=(--include='*.c' --include='*.cpp' --include='*.h' --include='*.hpp' --include='*.ino')
DEST_FILES=$(grep -rlE "${SRC_FILE_GLOBS[@]}" '"((https?|mqtts?|wss?)://[A-Za-z0-9]|[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+")' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -v "/tests_host/\|/test_\|/examples\?/\|\.pio/" || true)
DEST_NAMES=""
if [ -n "$DEST_FILES" ]; then
  # One logical line per #define (continuations joined); comment lines skipped.
  # shellcheck disable=SC2016  # an awk program run through xargs: $0 is awk's
  DEST_NAMES=$(printf '%s\n' "$DEST_FILES" | tr '\n' '\0' | xargs -0 awk '
    FNR == 1 { held = ""; held_at = 0 }
    {
      line = $0; at = FNR
      if (held != "") { line = held " " line; at = held_at; held = "" }
      if (line ~ /\\[[:space:]]*$/) { sub(/\\[[:space:]]*$/, "", line); held = line; held_at = at; next }
      if (line ~ /^[[:space:]]*(\/\/|\/\*|\*)/) next
      name = ""; val = ""
      if (match(line, /^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Za-z_][A-Za-z0-9_]*[[:space:]]+"[^"]*"/)) {
        d = substr(line, RSTART, RLENGTH)
        sub(/^[[:space:]]*#[[:space:]]*define[[:space:]]+/, "", d)
        name = d; sub(/[[:space:]].*/, "", name)
        val = d; sub(/^[^"]*/, "", val)
      } else if (match(line, /(char|String|string|auto)[^=;(){}]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*(\[[^]]*\])?[[:space:]]*=[[:space:]]*"[^"]*"/)) {
        d = substr(line, RSTART, RLENGTH)
        val = d; sub(/^[^=]*=[[:space:]]*/, "", val)
        name = d; sub(/[[:space:]]*(\[[^]]*\])?[[:space:]]*=.*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
      }
      if (name != "" && val ~ /^"((https?|mqtts?|wss?):\/\/[A-Za-z0-9][^"]*|[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+)"$/)
        print name "\t" val "\t" FILENAME ":" at
    }' || true)
fi
OUTBOUND_NAMED=""
if [ -n "$DEST_NAMES" ]; then
  OUTBOUND_NAMED=$(grep -rEn "${SRC_FILE_GLOBS[@]}" '\.begin\(|\.connect\(|\.url[[:space:]]*=|config(Tz)?Time\(|(esp_)?sntp_setservername\(|getaddrinfo\(|\.setServer\(' "${SRC_DIRS[@]}" 2>/dev/null \
    | grep -v "/tests_host/\|/test_\|/examples\?/\|\.pio/" \
    | drop_comment_lines \
    | DEST_NAMES="$DEST_NAMES" awk '
      # One hit per (call site, binding): a name bound in two places (a
      # sketch mirror and its source) must have BOTH bindings covered, so
      # changing one copy of a disclosed destination cannot hide.
      BEGIN {
        n = split(ENVIRON["DEST_NAMES"], L, "\n")
        for (i = 1; i <= n; i++) {
          split(L[i], F, "\t")
          if (F[1] == "") continue
          k = ++cnt[F[1]]; V[F[1], k] = F[2]; W[F[1], k] = F[3]
        }
      }
      {
        code = $0; sub(/^[^:]*:[0-9]+:/, "", code)
        gsub(/"([^"\\]|\\.)*"/, "\"\"", code)   # string contents are not names
        gsub(/\/\*[^*]*\*\//, " ", code)         # nor /* inline */ comments
        sub(/\/\/.*/, "", code)                  # nor a trailing // comment
        split("", seen)
        while (match(code, /[A-Za-z_][A-Za-z0-9_]*/)) {
          tok = substr(code, RSTART, RLENGTH); code = substr(code, RSTART + RLENGTH)
          if ((tok in cnt) && !(tok in seen)) {
            seen[tok] = 1
            for (k = 1; k <= cnt[tok]; k++) print $0 "  ⇐ " tok " = " V[tok, k] " (" W[tok, k] ")"
          }
        }
      }' || true)
fi
OUTBOUND_RAW=$(printf '%s\n%s\n' "$OUTBOUND_LITERAL" "$OUTBOUND_NAMED")
OUTBOUND_SCAN=$(printf '%s\n' "$OUTBOUND_RAW" | sed '/^$/d' | allowlist_filter "$DISCLOSED_OUTBOUND")
OUTBOUND_HITS=$(printf '%s\n' "$OUTBOUND_SCAN" | awk -F'\t' '$1=="HIT"{print $2}')
OUTBOUND_STALE=$(printf '%s\n' "$OUTBOUND_SCAN" | awk -F'\t' '$1=="STALE"{print $2}')
if [ -n "$OUTBOUND_HITS" ]; then
  check_warn "Compiled-in outbound destination not on the disclosed list"
  echo "$OUTBOUND_HITS" | while read -r line; do blue "  $line"; done
  blue "  Principle 2: Device must make ZERO outbound connections by default"
  blue "  See: docs/security/THREAT_MODEL.md → Principle 2: Zero Phone-Home"
else
  check_pass "No undisclosed compiled-in outbound destinations"
fi
if [ -n "$OUTBOUND_STALE" ]; then
  check_warn "DISCLOSED_OUTBOUND entry matches nothing — remove or fix it"
  echo "$OUTBOUND_STALE" | while read -r line; do blue "  $line"; done
fi

echo ""

# ── Check: Private key never in API/export/log ────────────────
section "Security: Private key isolation"

# Ed25519 private key must never appear in any export, API response, log, or debug output.
# This check looks for private key bytes being printed, serialized to JSON, written to
# SD card, or included in API responses. It excludes legitimate internal uses like
# Ed25519::sign(), derive_api_token(), nvs_store_key(), and nvs_load_key().
PRIVKEY_LEAK=$(grep -rEn 'priv(ate)?_?key|privkey|NVS_KEY_PRIV' "${SRC_DIRS[@]}" 2>/dev/null \
  | content_grep 'print|log|serial|json|response|send|export|write.*sd|write.*file|transmit|broadcast' \
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
section "Privacy: Presence detection MAC handling"

# Presence detection must hash MACs before storage. Look for patterns
# that store or transmit raw BSSID/MAC data.
# Not matched (they carry no address): comment lines, std::atomic flag
# stores (`s_bssid_known.store(true, ...)`), and log calls whose only
# argument text is a string literal with no format specifier.
RAW_MAC_STORE=$(grep -rEn 'bssid|BSSID|macAddress' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -i 'store\|save\|write\|persist\|sd\|nvs\|put\|append\|push_back\|log' \
  | grep -v "//\|hash\|fingerprint\|derive\|digest\|sha256\|\.h:\|\.md:" \
  | drop_comment_lines \
  | grep -vE '\.store\([[:space:]]*(true|false)[[:space:]]*[,)]' \
  | grep -vE 'LOG[A-Z_]*\((known[[:space:]]*\?[[:space:]]*)?"[^"%]*"[[:space:]]*(:|\)|$)' \
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
section "Security: TLS enforcement"

# A plaintext HTTP listener is a server bound to port 80 that is not a
# redirect-to-https server: an explicit `server_port = 80`, `WebServer(80)` /
# `WebServer{80}`, `HTTP_PORT = 80`, `server.begin(80)`. (The old grep also
# matched ":80" inside log strings and host-test fixtures.) THREAT_MODEL's
# HTTP row accepts plaintext on the LAN by default; each accepted listener
# is named in PLAINTEXT_OK with its reason, so a NEW plaintext listener fails
# --strict instead of hiding in a warning nobody reads.
PLAINTEXT_OK=$(printf '%s\t%s\t%s\n' \
  'canary/lib/securacv_network/src/securacv_network\.cpp$' 'config\.server_port = 80;' \
    'canary (PIO) plain server: the only server on release builds until FEATURE_HTTPS is flipped there (F15, maintainer decision); the HTTP-only fallback on dev/full when TLS is unavailable (tls_mode_reason says why)' \
  'canary-wap/arduino/canary_wap/canary_wap\.ino$' 'config\.server_port = 80;' \
    'canary-wap HTTP-only fallback when no TLS certificate is available' \
  'canary-display/.*glass_web\.cpp$' 'new WebServer\(80\)' \
    'display glass mirror: plaintext LAN, token-gated writes (THREAT_MODEL HTTP row)' \
  '(common/network/setup_portal\.cpp|canary-display/.*provision\.cpp)$' 'WebServer server\{80\}' \
    'first-boot setup portal: captive sheets cannot render a self-signed certificate (tls_policy.h)')
HTTP_RAW=$(grep -rEn 'server_port[[:space:]]*=[[:space:]]*80\b|WebServer[^;]*[({][[:space:]]*80[[:space:]]*[)}]|HTTP_PORT[[:space:]]*=?[[:space:]]*80\b|server\.begin[[:space:]]*\([[:space:]]*80' "${SRC_DIRS[@]}" 2>/dev/null \
  | grep -v "\.md:\|/tests_host/\|/test_\|\.pio/" \
  | drop_comment_lines \
  | grep -viE "redirect|301|307|https" || true)
HTTP_SCAN=$(printf '%s\n' "$HTTP_RAW" | sed '/^$/d' | allowlist_filter "$PLAINTEXT_OK")
HTTP_FALLBACK=$(printf '%s\n' "$HTTP_SCAN" | awk -F'\t' '$1=="HIT"{print $2}')
HTTP_STALE=$(printf '%s\n' "$HTTP_SCAN" | awk -F'\t' '$1=="STALE"{print $2}')
if [ -n "$HTTP_FALLBACK" ]; then
  check_warn "Plaintext HTTP listener on port 80 that is neither a redirect-to-https server nor on the reviewed list"
  echo "$HTTP_FALLBACK" | while read -r line; do blue "  $line"; done
  blue "  Principle 7: TLS for API access — see docs/security/THREAT_MODEL.md (HTTP row)"
else
  check_pass "Every port-80 listener is a redirect-to-https server or a reviewed plaintext listener"
fi
if [ -n "$HTTP_STALE" ]; then
  check_warn "PLAINTEXT_OK entry matches nothing — remove or fix it"
  echo "$HTTP_STALE" | while read -r line; do blue "  $line"; done
fi

echo ""

# ── Check: no partition table flags nvs `encrypted` ──────────────
# NVS is not compatible with flash encryption: ESP-IDF protects it with NVS
# encryption (CONFIG_NVS_ENCRYPTION, keys in an nvs_keys partition), and with
# flash encryption on, IDF v4.3+ refuses to open an nvs partition flagged
# `encrypted` (nvs_partition_lookup.cpp -> ESP_ERR_NVS_WRONG_ENCRYPTION). The
# provisioning kit's partitions_secure.csv carried that flag, so its image
# could not have opened NVS — the identity key's home — on a fused board
# (F42). nvs_keys SHOULD be flagged; only the `nvs` subtype is refused here.
section "Security: NVS partition not flash-encrypted"

NVS_ENC_HITS=$(find "$FIRMWARE_DIR" -name "*.csv" -not -path "*/.pio/*" -print0 2>/dev/null \
  | xargs -0 grep -nE '^[[:space:]]*[^#,]+,[[:space:]]*data[[:space:]]*,[[:space:]]*nvs[[:space:]]*,[^#]*encrypted' 2>/dev/null || true)
if [ -n "$NVS_ENC_HITS" ]; then
  check_fail "An nvs partition is flagged 'encrypted' — IDF refuses to open it once flash encryption is on:"
  echo "$NVS_ENC_HITS" | while read -r line; do blue "  ${line#"$FIRMWARE_DIR"/}"; done
  blue "  Fix: drop the flag; NVS is protected by NVS encryption (nvs_keys), not by flash encryption"
else
  check_pass "No partition table flags an nvs partition 'encrypted'"
fi

echo ""

# ── Check: secure_defaults.h exists ──────────────────────────
section "Security: Secure defaults header"

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
section "Documentation: Security Model"

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
section "Security: Dashboard storage"

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
section "Privacy: GPS precision"

# SecuraCV coarsens GPS. Raw high-precision coordinates should not leak.
GPS_PRECISION=$(grep -rn '%.8f\|%.7f\|%.6f' "${SRC_DIRS[@]}" 2>/dev/null | grep -i "lat\|lon\|gps" | grep -v "//\|\.md:" | drop_comment_lines || true)
if [ -n "$GPS_PRECISION" ]; then
  check_warn "High-precision GPS format found (>5 decimal places) — verify coarsening is applied"
  echo "$GPS_PRECISION" | while read -r line; do blue "  $line"; done
else
  check_pass "No high-precision GPS format strings found"
fi

echo ""

# ── Check: Watchdog configuration ──────────────────────────────
section "Reliability: Watchdog"

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
section "Build: web_ui.h size"

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
section "Build: Debug flags"

DEBUG_FLAGS_ON=$(grep -rn '#define DEBUG_\w\+\s\+1' "$CANARY_DIR" 2>/dev/null | grep -v "platformio\|//.*#define" || true)
if [ -n "$DEBUG_FLAGS_ON" ]; then
  check_warn "Debug flags enabled (okay for dev, should be 0 for release):"
  echo "$DEBUG_FLAGS_ON" | while read -r line; do blue "  $line"; done
else
  check_pass "All DEBUG_ flags are 0 or undefined"
fi

echo ""

# ── Check: PlatformIO extra_scripts can actually run ─────────────
# firmware/canary/platformio.ini once set `extra_scripts = pre:../../scripts/
# pre_build.py` under [platformio]: PlatformIO reads extra_scripts only in an
# environment ([env] / [env:NAME]), and the path named a file that did not
# exist, so the "pre-build tripwire" never ran and nothing said so (F40). The
# tripwires it duplicated are this script's own sections above. Any .ini under
# firmware/ that sets extra_scripts outside an environment fails here, and so
# does a project platformio.ini whose script path does not resolve against the
# project directory.
section "Build: PlatformIO extra_scripts"

XS_FOUND=0
XS_BAD=""
while IFS= read -r ini; do
  while IFS=$'\t' read -r xs_sec xs_line xs_val; do
    XS_FOUND=$((XS_FOUND + 1))
    rel_ini="${ini#"$FIRMWARE_DIR"/}"
    case "$xs_sec" in
      env|env:*) ;;
      *) XS_BAD="${XS_BAD}${rel_ini}:${xs_line}: extra_scripts under [${xs_sec}] is never read (it is an [env] option)\n" ;;
    esac
    if [ "$(basename "$ini")" = "platformio.ini" ]; then
      for xs_tok in $xs_val; do
        xs_path="${xs_tok#pre:}"
        xs_path="${xs_path#post:}"
        case "$xs_path" in *'$'*) continue ;; esac
        if [ ! -f "$(dirname "$ini")/$xs_path" ]; then
          XS_BAD="${XS_BAD}${rel_ini}:${xs_line}: extra_scripts names ${xs_path}, which does not exist\n"
        fi
      done
    fi
  done < <(awk '
    /^[ \t]*[;#]/ { next }
    /^\[[^]]+\][ \t]*$/ { sec = substr($0, 2, index($0, "]") - 2); inxs = 0; next }
    /^[ \t]*extra_scripts[ \t]*=/ {
      v = $0; sub(/^[^=]*=/, "", v); sub(/[ \t]*;.*/, "", v)
      printf "%s\t%d\t%s\n", sec, NR, v; inxs = 1; next
    }
    inxs && /^[ \t]+[^ \t]/ {
      v = $0; sub(/[ \t]*;.*/, "", v)
      printf "%s\t%d\t%s\n", sec, NR, v; next
    }
    { inxs = 0 }
  ' "$ini")
done < <(find "$FIRMWARE_DIR" -name "*.ini" -not -path "*/.pio/*" 2>/dev/null | sort)

if [ -n "$XS_BAD" ]; then
  check_fail "PlatformIO extra_scripts that can never run:"
  echo -e "$XS_BAD" | while read -r line; do [ -z "$line" ] || blue "  $line"; done
  blue "  Fix: set extra_scripts under [env] with a path relative to the project, or drop it"
elif [ "$XS_FOUND" -eq 0 ]; then
  check_pass "No PlatformIO extra_scripts (source tripwires run in this script, in CI)"
else
  check_pass "Every PlatformIO extra_scripts entry sits in an environment and resolves"
fi

echo ""

# ── Check: Mesh secret persistence is gated on flash encryption ────────
# The ESP-NOW "Opera" mesh uses a long-lived shared secret (opera_secret). It must
# NEVER be written to NVS unless flash encryption is on. The persistence layer
# (mesh_state.cpp) enforces this: its save_*/load_* paths return false when
# !esp_flash_encryption_enabled(), so on an FE-off board the secret is not
# persisted (the live in-RAM session is allowed for the current boot by design —
# see firmware/canary/src/main.cpp on_pairing_succeeded). On an FE-ON board the
# entry is still plaintext at rest: flash encryption does not cover NVS, and NVS
# encryption is unavailable under framework = arduino (mesh_state.h; roadmap
# item 9) — the gate keeps the secret off un-fused boards, it does not encrypt
# it on fused ones. This guard asserts that FE
# check is not silently removed from the mesh persistence/impl files. It does NOT,
# and cannot statically, prove the *activation* path fails closed — see issue #610
# C2 / the bench runbook for the on-device check, and the open design question of
# whether live activation should also refuse on FE-off boards.
section "Security: Mesh secret persistence is FE-gated"

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

# ── Check: first-boot identity keygen seeds the RNG before RF is up ─────
# Every tree generates its Ed25519 identity key during provisioning, BEFORE
# WiFi/BT start — so esp_fill_random() has no RF entropy source yet and a bare
# draw risks a predictable key on a fresh unit (roadmap §3.7 "Weak first-boot
# entropy"; issue #921 / PR #994 fixed the three project trees, this guard
# asserts the PIO canary tree stayed fixed too). The documented ESP-IDF pattern
# is bootloader_random_enable() / esp_fill_random() / bootloader_random_disable()
# around that one draw. This greps each keygen file for the enable CALL as a
# statement on its own line (a comment that merely names the function does not
# count); it cannot prove the call ORDER (enable must precede the draw, and must
# never run while RF is up) — that is code review plus the U1 bench.
echo "── Security: first-boot keygen is entropy-seeded ──"

KEYGEN_FILES=(
  "$CANARY_DIR/lib/securacv_crypto/src/securacv_crypto.cpp"
  "$PROJECTS_DIR/canary-sense/src/witness.cpp"
  "$PROJECTS_DIR/canary-vision/src/witness.cpp"
  "$PROJECTS_DIR/canary-wap/arduino/canary_wap/canary_wap.ino"
)

for kf in "${KEYGEN_FILES[@]}"; do
  rel=${kf#"$FIRMWARE_DIR/"}
  if [ ! -f "$kf" ]; then
    check_warn "Keygen file not found (moved?): $rel"
    continue
  fi
  if grep -Eq '^[[:space:]]*bootloader_random_enable[[:space:]]*\([[:space:]]*\)[[:space:]]*;' "$kf"; then
    check_pass "First-boot keygen seeds entropy: $rel"
  else
    check_fail "Keygen file '$rel' has no bootloader_random_enable() around its first-boot esp_fill_random() — predictable-key risk on fresh units (#921)"
  fi
done

echo ""

# ── Check: on-glass text stays inside the display font's alphabet ──
section "Display: font glyph range"

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
section "Documentation: Lessons Learned"

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
  if [ "$STRICT" -eq 1 ]; then
    yellow "PASSED (--strict: no Security/Privacy warnings) with $WARNINGS other warnings"
  else
    yellow "PASSED with $WARNINGS warnings"
  fi
  echo ""
  echo "Warnings are non-blocking but should be addressed."
  exit 0
else
  green "ALL CHECKS PASSED"
  exit 0
fi
