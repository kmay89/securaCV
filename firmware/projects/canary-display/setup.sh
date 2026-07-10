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
#      configs/<flavor>/config.h flatten to "config.h". The flavor file is
#      staged as flavor_config.h and `#include <config.h>` is rewritten to it.
#   3. Stages the per-flavor board pins.h + config, plus secrets.
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
  # <config.h> is the FLAVOR config (angle brackets skip this dir on purpose);
  # in the flat sketch it lives as flavor_config.h to avoid colliding with the
  # composition header canary/config.h (-> config.h).
  sed -i -E 's|#include <config.h>|#include "flavor_config.h"|g' "$f"
  # secrets ladder: the nested spelling collapses to the flat one.
  sed -i -E 's|#include "secrets/secrets.h"|#include "secrets.h"|g' "$f"
}

remove_generated() {
  # Only the generated/staged sources — never sketch.yaml/README/.gitignore.
  find "${SKETCH_DIR}" -maxdepth 1 -type f \
    \( -name '*.ino' -o -name '*.cpp' -o -name '*.h' \) -delete 2>/dev/null || true
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

  # Flatten every include in every generated file.
  find "${SKETCH_DIR}" -maxdepth 1 -type f \
    \( -name '*.ino' -o -name '*.cpp' -o -name '*.h' \) -print0 |
    while IFS= read -r -d '' f; do flatten_includes "$f"; done

  ok "Flattened $(find "${SKETCH_DIR}" -maxdepth 1 \( -name '*.cpp' -o -name '*.h' \) | wc -l | tr -d ' ') sources + canary_display.ino"
}

stage_flavor() {
  local flavor="$1"
  local pins_src config_src
  pins_src="$(pins_dir_for "$flavor")" || { err "Unknown flavor '$flavor' (watch|dash)"; exit 1; }
  config_src="${FIRMWARE_ROOT}/configs/canary-display/${flavor}/config.h"

  cp "${pins_src}/pins.h" "${SKETCH_DIR}/pins.h"
  cp "${config_src}" "${SKETCH_DIR}/flavor_config.h"
  ok "Staged ${flavor} board pins + flavor_config.h"
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

setup_arduino() {
  local flavor="${1:-}"
  if [ -z "$flavor" ]; then
    err "Pick a flavor: ./setup.sh arduino watch   |   ./setup.sh arduino dash"; exit 1
  fi
  remove_generated
  generate_shared
  stage_flavor "$flavor"
  stage_secrets
  echo ""
  ok "Arduino sketch ready: ${SKETCH_DIR} (flavor: ${flavor})"
  info "Build:  arduino-cli compile --profile ${flavor}   (or open canary_display.ino in the IDE)"
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
