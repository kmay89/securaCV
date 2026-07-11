#!/bin/bash
set -euo pipefail
#
# SecuraCV Canary Display — Arduino sketch generator + staging.
#
# The canonical firmware is the PlatformIO tree in this directory (src/ +
# include/). This script GENERATES the Arduino-IDE-buildable parity sketch
# from that single source of truth — it never hand-maintains a second copy.
# What it does:
#   1. Flattens the namespaced tree into the flat layout Arduino needs
#      (main.cpp -> canary_display.ino; src/**/*.cpp and include/canary/**/*.h
#      -> sketch root), rewriting `#include "canary/..."` to basenames.
#   2. Resolves the one name collision the flat layout creates: BOTH
#      include/canary/config.h (composition header) and the flavor
#      configs/<flavor>/config.h flatten to "config.h". The flavor file
#      lives as flavor_config.h and `#include <config.h>` is rewritten to it.
#   3. Generates the zero-setup flavor dispatch: both flavors' pin maps and
#      configs are committed copies, and pins.h/flavor_config.h dispatch on
#      flavor_select.h (watch by default) — so a raw GitHub zip compiles
#      with no setup step. `arduino <flavor>` just writes the git-ignored
#      flavor_local.h override, plus secrets.
#
# A CI guard (firmware/scripts/check_display_arduino_sync.sh) re-runs this and
# fails if the committed sketch drifts from the canonical tree.
#
# Usage:
#   ./setup.sh arduino watch    # generate + stage the watch flavor
#   ./setup.sh arduino dash     # generate + stage the dash flavor
#   ./setup.sh check            # verify toolchain (arduino-cli)
#   ./setup.sh clean            # remove generated sketch sources

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
ok()   { echo -e "${GREEN}✓${NC} $1"; }
err()  { echo -e "${RED}✗${NC} $1"; }
warn() { echo -e "${YELLOW}!${NC} $1"; }
info() { echo -e "${BLUE}→${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SKETCH_DIR="${SCRIPT_DIR}/arduino/canary_display"

# Flavor -> board pins directory (matches the -I paths in the PlatformIO ini).
pins_dir_for() {
  case "$1" in
    watch) echo "${FIRMWARE_ROOT}/boards/xiao-esp32s3-round/pins" ;;
    dash)  echo "${FIRMWARE_ROOT}/boards/waveshare-esp32s3-lcd43/pins" ;;
    *) return 1 ;;
  esac
}

# Rewrite namespaced includes to the flat basenames Arduino resolves from the
# sketch root, and fix the two angle-bracket / subdir cases.
flatten_includes() {
  local f="$1"
  # canary/<subdir(s)>/<name>  ->  <name>   (strip up to the last slash)
  sed -i -E 's|#include "canary/[^"]*/|#include "|g' "$f"
  # canary/<name>              ->  <name>
  sed -i -E 's|#include "canary/|#include "|g' "$f"
  # Shared firmware/common headers, staged flat next to the sketch (self-
  # contained for the Arduino IDE, no external include path — the same
  # committed-copy approach the canary-wap sketch uses).
  sed -i -E 's|#include "boot/|#include "|g' "$f"
  sed -i -E 's|#include "identity/|#include "|g' "$f"
  sed -i -E 's|#include "core/|#include "|g' "$f"
  # <config.h> is the FLAVOR config (angle brackets skip this dir on purpose);
  # in the flat sketch it lives as flavor_config.h to avoid colliding with the
  # composition header canary/config.h (-> config.h).
  sed -i -E 's|#include <config.h>|#include "flavor_config.h"|g' "$f"
  # secrets ladder: the nested spelling collapses to the flat one.
  sed -i -E 's|#include "secrets/secrets.h"|#include "secrets.h"|g' "$f"
}

remove_generated() {
  # Only the generated sources — never sketch.yaml/README/.gitignore, and
  # never the per-user staged files (flavor_local.h, secrets.h): a regen
  # must not un-provision the sketch someone already set up.
  find "${SKETCH_DIR}" -maxdepth 1 -type f \
    \( -name '*.ino' -o -name '*.cpp' -o -name '*.h' \) \
    ! -name 'flavor_local.h' ! -name 'secrets.h' -delete 2>/dev/null || true
}

generate_shared() {
  mkdir -p "${SKETCH_DIR}"
  info "Generating flat sketch sources from the canonical PlatformIO tree…"

  # Entry point: main.cpp already IS setup()/loop() (Arduino framework), so it
  # becomes the .ino verbatim (with flattened includes).
  cp "${SCRIPT_DIR}/src/main.cpp" "${SKETCH_DIR}/canary_display.ino"

  # All other translation units, flattened to the sketch root.
  while IFS= read -r cpp; do
    cp "$cpp" "${SKETCH_DIR}/$(basename "$cpp")"
  done < <(find "${SCRIPT_DIR}/src" -name '*.cpp' ! -name 'main.cpp')

  # Headers (namespaced + top-level), flattened.
  while IFS= read -r h; do
    cp "$h" "${SKETCH_DIR}/$(basename "$h")"
  done < <(find "${SCRIPT_DIR}/include/canary" -name '*.h')

  # LVGL config (LV_CONF_INCLUDE_SIMPLE finds it at the sketch root).
  cp "${SCRIPT_DIR}/include/lv_conf.h" "${SKETCH_DIR}/lv_conf.h"

  # CI-safe secrets fallback (the __has_include ladder lands here if no real
  # secrets.h is staged).
  cp "${SCRIPT_DIR}/include/secrets.ci.h" "${SKETCH_DIR}/secrets.ci.h"

  # Shared firmware/common code the display consumes (via lib_extra_dirs / -I
  # in PlatformIO). Staged flat so the Arduino sketch is self-contained — no
  # --libraries path needed, IDE builds work from the sketch folder alone.
  # This is the exact transitive set the display's `#include`s reach; keep it
  # in step with the canonical includes if src/ starts using more of common.
  local common_files=(
    "${FIRMWARE_ROOT}/common/boot/boot_banner.h"
    "${FIRMWARE_ROOT}/common/boot/boot_banner.cpp"
    "${FIRMWARE_ROOT}/common/identity/device_pseudonym.h"
    "${FIRMWARE_ROOT}/common/core/feature_sanity.h"
    "${FIRMWARE_ROOT}/common/ota/src/securacv_ota.h"
    "${FIRMWARE_ROOT}/common/ota/src/securacv_ota.cpp"
    "${FIRMWARE_ROOT}/common/ota/src/ota_release_key.h"
  )
  for cf in "${common_files[@]}"; do
    cp "$cf" "${SKETCH_DIR}/$(basename "$cf")"
  done

  # ── Zero-setup flavor dispatch (the zip-download fix) ──────────────────
  # A raw GitHub zip must compile with NO setup step: both flavors' pin maps
  # and configs are committed as flavor copies, and pins.h/flavor_config.h
  # are thin dispatchers keyed on flavor_select.h (watch by default — the
  # entry product). ./setup.sh arduino <flavor> writes the git-ignored
  # flavor_local.h override instead of touching committed files, so git
  # checkouts stay clean. Secrets already degrade gracefully: no secrets.h
  # -> secrets.ci.h placeholders -> the on-glass onboarding wizard.
  cp "$(pins_dir_for watch)/pins.h" "${SKETCH_DIR}/pins_watch.h"
  cp "$(pins_dir_for dash)/pins.h"  "${SKETCH_DIR}/pins_dash.h"
  cp "${FIRMWARE_ROOT}/configs/canary-display/watch/config.h" "${SKETCH_DIR}/flavor_watch.h"
  cp "${FIRMWARE_ROOT}/configs/canary-display/dash/config.h"  "${SKETCH_DIR}/flavor_dash.h"

  cat > "${SKETCH_DIR}/flavor_select.h" << 'EOF'
#pragma once
// ── FLAVOR SELECTION: follows your BOARD choice ─────────────────────────
// No explicit override present? The flavor is inferred from the selected
// board, so every documented path just works:
//   XIAO_ESP32S3 board / watch(-core3) cli profile  -> WATCH firmware
//   ESP32S3 Dev Module / dash(-core3) cli profile   -> DASH firmware
// This is what makes `arduino-cli compile --profile dash-core3` and the
// IDE's Tools->Board menu safe: you cannot build watch firmware for dash
// hardware by forgetting a define (review catch).
//
// Overrides (for exotic boards): `./setup.sh arduino <watch|dash>` writes
// the git-ignored flavor_local.h, or define CD_BUILD_DASH 0/1 there
// yourself. An explicit override always wins over board inference.
// (GENERATED by setup.sh — do not hand-edit; use flavor_local.h.)
#if __has_include("flavor_local.h")
#include "flavor_local.h"
#endif
#ifndef CD_BUILD_DASH
  #if defined(ARDUINO_XIAO_ESP32S3)
    #define CD_BUILD_DASH 0
  #elif defined(ARDUINO_ESP32S3_DEV)
    #define CD_BUILD_DASH 1
  #else
    /* Unrecognized board (a vendor board package, e.g. Waveshare's own
     * "ESP32-S3-Touch-LCD-4.3B" entry, defines its own board macro we
     * can't know). Guessing here could flash the WRONG FLAVOR silently -
     * fail loud with the fix instead. */
    #error "Board not recognized. Pick Tools->Board 'XIAO_ESP32S3' (watch) or 'ESP32S3 Dev Module' (dash) from 'esp32 by Espressif Systems' - or create flavor_local.h next to this sketch with '#define CD_BUILD_DASH 0' (watch) or '#define CD_BUILD_DASH 1' (dash)."
  #endif
#endif
EOF

  cat > "${SKETCH_DIR}/pins.h" << 'EOF'
#pragma once
// Flavor-dispatching pin map (GENERATED by setup.sh — do not hand-edit;
// pick the flavor in flavor_select.h). Pin numbers live ONLY in the
// pins_<flavor>.h copies, which mirror firmware/boards/<board>/pins/pins.h.
#include "flavor_select.h"
#if defined(CD_BUILD_DASH) && CD_BUILD_DASH
#include "pins_dash.h"
#else
#include "pins_watch.h"
#endif
EOF

  cat > "${SKETCH_DIR}/flavor_config.h" << 'EOF'
#pragma once
// Flavor-dispatching config (GENERATED by setup.sh — do not hand-edit;
// pick the flavor in flavor_select.h). The flavor_<name>.h copies mirror
// firmware/configs/canary-display/<flavor>/config.h.
#include "flavor_select.h"
#if defined(CD_BUILD_DASH) && CD_BUILD_DASH
#include "flavor_dash.h"
#else
#include "flavor_watch.h"
#endif
EOF

  # Flatten every include in every generated file (flavor_local.h/secrets.h
  # are user-authored and skipped).
  find "${SKETCH_DIR}" -maxdepth 1 -type f \
    \( -name '*.ino' -o -name '*.cpp' -o -name '*.h' \) \
    ! -name 'flavor_local.h' ! -name 'secrets.h' -print0 |
    while IFS= read -r -d '' f; do flatten_includes "$f"; done

  ok "Flattened $(find "${SKETCH_DIR}" -maxdepth 1 \( -name '*.cpp' -o -name '*.h' \) | wc -l | tr -d ' ') sources + canary_display.ino"
}

stage_flavor() {
  local flavor="$1"
  local dash=0
  case "$flavor" in
    watch) dash=0 ;;
    dash)  dash=1 ;;
    *) err "Unknown flavor '$flavor' (watch|dash)"; exit 1 ;;
  esac
  # The committed dispatchers (pins.h/flavor_config.h) key on this one
  # git-ignored line — staging never dirties the tree.
  cat > "${SKETCH_DIR}/flavor_local.h" << EOF
#pragma once
// Written by ./setup.sh arduino ${flavor} — overrides flavor_select.h.
#define CD_BUILD_DASH ${dash}
EOF
  ok "Selected flavor: ${flavor} (flavor_local.h)"
}

stage_secrets() {
  if [ -f "${SCRIPT_DIR}/secrets/secrets.h" ]; then
    cp "${SCRIPT_DIR}/secrets/secrets.h" "${SKETCH_DIR}/secrets.h"
    ok "Staged secrets.h from secrets/secrets.h"
  elif [ -f "${SCRIPT_DIR}/secrets/secrets.example.h" ]; then
    cp "${SCRIPT_DIR}/secrets/secrets.example.h" "${SKETCH_DIR}/secrets.h"
    warn "No secrets/secrets.h — staged the example; edit ${SKETCH_DIR}/secrets.h with WiFi/broker"
  else
    info "No secrets file found; the build will fall back to secrets.ci.h (CI placeholders)"
  fi
}

# Arduino IDE builds (which ignore sketch.yaml profiles — arduino-ide#2573)
# need lv_conf.h ONE LEVEL ABOVE the lvgl library dir. Best-effort copy into
# the user's sketchbook; cli --profile builds don't need this (the profile
# build resolves lv_conf via the sketch), and CI does its own copy.
stage_lv_conf() {
  local sketchbook=""
  for c in "${HOME}/Documents/Arduino" "${HOME}/Arduino"; do
    [ -d "$c/libraries" ] && { sketchbook="$c"; break; }
  done
  if [ -z "$sketchbook" ]; then
    info "No Arduino sketchbook found — IDE users: copy ${SKETCH_DIR}/lv_conf.h to <sketchbook>/libraries/"
    return 0
  fi
  local dst="${sketchbook}/libraries/lv_conf.h"
  if [ -f "$dst" ] && ! cmp -s "${SKETCH_DIR}/lv_conf.h" "$dst"; then
    # The sketchbook lv_conf.h is global to every LVGL project the user
    # builds in the IDE — never destroy someone else's config silently.
    cp "$dst" "${dst}.bak"
    warn "Existing ${dst} differed — kept a copy at lv_conf.h.bak"
  fi
  cp "${SKETCH_DIR}/lv_conf.h" "$dst"
  ok "Copied lv_conf.h -> ${sketchbook}/libraries/ (Arduino IDE builds)"
}

setup_arduino() {
  local flavor="${1:-}"
  if [ -z "$flavor" ]; then
    err "Pick a flavor: ./setup.sh arduino watch   |   ./setup.sh arduino dash"; exit 1
  fi
  remove_generated
  generate_shared
  stage_flavor "$flavor"
  stage_secrets
  stage_lv_conf
  echo ""
  ok "Arduino sketch ready: ${SKETCH_DIR} (flavor: ${flavor})"
  info "Build:  arduino-cli compile --profile ${flavor}-core3   (or ${flavor} on core 2.0.17; or open canary_display.ino in the IDE)"
}

check_toolchain() {
  if command -v arduino-cli >/dev/null 2>&1; then
    ok "arduino-cli: $(arduino-cli version 2>/dev/null | head -1)"
  else
    warn "arduino-cli not found — https://arduino.github.io/arduino-cli/latest/installation/"
  fi
}

case "${1:-}" in
  arduino) setup_arduino "${2:-}" ;;
  # regen: flavor-agnostic shared sources only (what the sync guard commits +
  # checks). Does NOT stage pins/flavor_config/secrets — those are per-flavor
  # and gitignored.
  regen)   remove_generated; generate_shared ;;
  check)   check_toolchain ;;
  clean)   remove_generated; ok "Removed generated sketch sources" ;;
  *) echo "Usage: ./setup.sh {arduino <watch|dash>|regen|check|clean}"; exit 1 ;;
esac
