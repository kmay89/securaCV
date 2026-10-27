#!/usr/bin/env bash
# canary-local/emulator/test/glass_turn.sh — the turned glass, natively
# (F184, F204): builds glass_turn_test.cpp with g++ against the REAL
# ui/lvgl_port.cpp, the emulator's REAL display HAL and LVGL 8.4, and runs
# it twice: in the dash config (LVGL's software rotation) and in the
# nightlight config (the panel's hardware turn). Beside each run,
# backlight_cap_test.cpp holds the same HAL object's backlight to the board's
# ceiling (sweep A61: clipped as the C3-LCD-1.47 clips it in the nightlight
# config, passed through as asked in the dash's). Then it builds
# glass_splash_test.cpp (F185, F159) in the config of every display env that
# builds LVGL 8.4, and in the dash's (the emulator's dash flavor is 8.4 too),
# against the same LVGL objects: the REAL splash.cpp plays the first meeting,
# and the speech bubble's tail and the bird's seat over it are read off the
# frame; every one of those suites runs, and the script fails at its end if
# any did. No emsdk: this proves the sources the dist is
# built from, not the dist's bytes (the browser probe, onboard_probe.mjs,
# reads those).
#
#   bash canary-local/emulator/test/glass_turn.sh [LVGL_DIR]
#
# LVGL_DIR defaults to canary-local/emulator/third_party/lvgl, the checkout
# build.sh fetches at its pin. When that default is absent (a cold
# third-party cache), this fetches it there itself, at the LVGL_TAG build.sh
# names and the way build.sh does, so build.sh later finds it and skips its
# own clone. That is what lets CI (canary-local.yml) run this FIRST, before
# any step that reads the committed dist: a stale dist goes red in the
# browser probes, and a step after them would be skipped, hiding the one
# proof of these sources that does not need the dist. An explicit LVGL_DIR
# is never fetched into. GLASS_TURN_OUT keeps the objects between runs and
# is left as it is; without it the objects go to a fresh temporary directory
# that is removed on exit, pass or fail (sweep A82, as glass_turn_lvgl9.sh
# does).
set -euo pipefail
export LC_ALL=C

HERE="$(cd "$(dirname "$0")" && pwd)"
EMU="$(cd "$HERE/.." && pwd)"
FW="$(cd "$EMU/../../firmware" && pwd)"
PROJ="$FW/projects/canary-display"
if [[ $# -eq 0 && ! -d "$EMU/third_party/lvgl" ]]; then
  tag="$(sed -n 's/^LVGL_TAG="\([^"]*\)"$/\1/p' "$EMU/build.sh")"
  if [[ -z "$tag" ]]; then
    echo "glass_turn: build.sh names no LVGL_TAG to fetch" >&2
    exit 2
  fi
  mkdir -p "$EMU/third_party"
  git clone --depth 1 --branch "$tag" https://github.com/lvgl/lvgl.git "$EMU/third_party/lvgl"
fi
LVGL="$(cd "${1:-$EMU/third_party/lvgl}" 2>/dev/null && pwd || true)"
if [[ -z "$LVGL" || ! -f "$LVGL/lvgl.h" ]]; then
  echo "glass_turn: no LVGL checkout at ${1:-$EMU/third_party/lvgl} (run build.sh once, or pass one)" >&2
  exit 2
fi
ver="$(sed -n 's/^#define LVGL_VERSION_MAJOR *\([0-9]*\).*/\1/p;s/^#define LVGL_VERSION_MINOR *\([0-9]*\).*/\1/p' "$LVGL/lvgl.h" | tr '\n' '.')"
if [[ "$ver" != "8.4." ]]; then
  echo "glass_turn: $LVGL is LVGL ${ver%.}, not the 8.4 build.sh pins" >&2
  exit 2
fi
# The splash's pseudonym (F159, as glass_turn_lvgl9.sh holds it for F246):
# native9/identity/device_pseudonym.h stands in for the NVS-backed one with
# HEX_LEN = TOKEN_BYTES * 2 = 16 letters, the length the first meeting's line
# wraps around; the real header must agree.
PSEUDO="$FW/common/identity/device_pseudonym.h"
if ! grep -Eq '^constexpr size_t TOKEN_BYTES = 8;' "$PSEUDO" ||
   ! grep -Eq '^constexpr size_t HEX_LEN[[:space:]]+= TOKEN_BYTES \* 2;' "$PSEUDO"; then
  echo "glass_turn: $PSEUDO no longer makes a 16-letter pseudonym (TOKEN_BYTES 8, HEX_LEN TOKEN_BYTES * 2): update native9/identity/device_pseudonym.h" >&2
  exit 2
fi

if [[ -n "${GLASS_TURN_OUT:-}" ]]; then
  OUT="$GLASS_TURN_OUT"
else
  OUT="$(mktemp -d)"
  trap '[[ -n "$OUT" && -d "$OUT" ]] && rm -rf -- "$OUT"' EXIT
fi
mkdir -p "$OUT/lvgl"

# The dash flavor's wiring, as build.sh hands it to em++.
CFG="$FW/configs/canary-display/dash"
PINS="$FW/boards/waveshare-esp32s3-lcd43/pins"
DEFS=(-DARDUINO=10812 -DLV_CONF_INCLUDE_SIMPLE)
LVGL_INC=(-I "$PROJ/include" -I "$EMU/shim" -I "$LVGL")
INC=(-I "$HERE/native" -I "$EMU/shim" -I "$EMU/src" -I "$PROJ/include"
     -I "$CFG" -I "$PINS" -I "$FW/common" -I "$LVGL")

# LVGL, once per checkout (an object newer than its source is kept).
export OUT LVGL
export LVGL_FLAGS="-std=gnu11 -O1 -w ${DEFS[*]} ${LVGL_INC[*]}"
find "$LVGL/src" -name '*.c' -print0 | sort -z | xargs -0 -n 8 -P "$(nproc)" bash -c '
  for src in "$@"; do
    obj="$OUT/lvgl/$(echo "${src#$LVGL/}" | tr "/." "__").o"
    [[ -f "$obj" && "$obj" -nt "$src" ]] && continue
    gcc $LVGL_FLAGS -c "$src" -o "$obj"
  done' _

CXX=(g++ -std=gnu++17 -O1 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter
     "${DEFS[@]}" "${INC[@]}")
"${CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp" -o "$OUT/lvgl_port.o"
"${CXX[@]}" -c "$EMU/src/emu_hal_display.cpp" -o "$OUT/emu_hal_display.o"
"${CXX[@]}" -c "$HERE/glass_turn_test.cpp" -o "$OUT/glass_turn_test.o"
g++ -o "$OUT/glass_turn_test" "$OUT/glass_turn_test.o" "$OUT/lvgl_port.o" \
  "$OUT/emu_hal_display.o" "$OUT"/lvgl/*.o -lm
"$OUT/glass_turn_test"
# The board's backlight ceiling (sweep A61), against the same HAL object: the
# dash has no expander and no ceiling, so every level passes through as asked
# (backlight_cap_test.cpp; its nightlight run is below).
"${CXX[@]}" -c "$HERE/backlight_cap_test.cpp" -o "$OUT/backlight_cap_test.o"
g++ -o "$OUT/backlight_cap_test" "$OUT/backlight_cap_test.o" "$OUT/emu_hal_display.o" \
  "$OUT"/lvgl/*.o -lm
"$OUT/backlight_cap_test"

# The nightlight's wiring (F204), as build.sh hands it to em++: the same
# test, now holding the hardware turn (display_set_rotation and
# lvgl_port_set_panel_rotation) on its 180x320 panel. The LVGL objects above
# serve it unchanged (its lean budget only drops fonts this test never sets).
NL_CFG="$FW/configs/canary-display/nightlight"
NL_PINS="$FW/boards/waveshare-esp32c3-lcd147/pins"
NL_INC=(-I "$HERE/native" -I "$EMU/shim" -I "$EMU/src" -I "$PROJ/include"
        -I "$NL_CFG" -I "$NL_PINS" -I "$FW/common" -I "$LVGL")
NL_CXX=(g++ -std=gnu++17 -O1 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter
        "${DEFS[@]}" "${NL_INC[@]}")
"${NL_CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp" -o "$OUT/nl_lvgl_port.o"
"${NL_CXX[@]}" -c "$EMU/src/emu_hal_display.cpp" -o "$OUT/nl_emu_hal_display.o"
"${NL_CXX[@]}" -c "$HERE/glass_turn_test.cpp" -o "$OUT/nl_glass_turn_test.o"
g++ -o "$OUT/nl_glass_turn_test" "$OUT/nl_glass_turn_test.o" "$OUT/nl_lvgl_port.o" \
  "$OUT/nl_emu_hal_display.o" "$OUT"/lvgl/*.o -lm
# ...and the nightlight's backlight ceiling (sweep A61): the C3-LCD-1.47's HAL
# clips its expander's PWM byte at CD_BL_MAX_PCT (50%), and the twin's HAL
# must clip every day level and night duty where the board's does.
"${NL_CXX[@]}" -c "$HERE/backlight_cap_test.cpp" -o "$OUT/nl_backlight_cap_test.o"
g++ -o "$OUT/nl_backlight_cap_test" "$OUT/nl_backlight_cap_test.o" "$OUT/nl_emu_hal_display.o" \
  "$OUT"/lvgl/*.o -lm
"$OUT/nl_backlight_cap_test"
"$OUT/nl_glass_turn_test"

# The boot splash's speech-bubble tail and the bird's seat over it on LVGL
# 8.4 (F185, F159): glass_splash_test.cpp links the REAL splash.cpp,
# canary_mark.cpp, theme.cpp, character.cpp and lvgl_port.cpp (its LVGL 8
# branch) against the LVGL objects above, with test/native9's stand-ins for
# the board (a recording Arduino_GFX, Preferences, a one-letter pseudonym),
# in the config and pin map of each display env that builds LVGL 8.4: the
# round watch, the S3 nightstand, the touch169, the AMOLED and the nightlight
# (with its env's lean budget, which in LVGL itself drops only faces and
# widgets, and the refresh period, that a whole-frame read never sees). The
# nightlight's suite also plays its turned 320x180 canvas. Then the dash's:
# its hardware line builds LVGL 9.5 (glass_turn_lvgl9.sh renders that), but
# build.sh compiles the emulator's dash flavor against this 8.4, so its
# suite plays the 800x480 panel and the 480x800 canvas LVGL's software
# rotation turns it to. Every suite runs, and the script fails at its end if
# any did.
SPLASH_SRC=(lvgl_port splash canary_mark theme character)
splash_failed=()
splash_suite() {  # splash_suite <name> <config> <board> [defines...]
  local name="$1" cfg="$2" board="$3" f
  shift 3
  local cxx=(g++ -std=gnu++17 -O1 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter
             "${DEFS[@]}" "$@" -I "$HERE/native9" -I "$PROJ/include"
             -I "$FW/configs/canary-display/$cfg" -I "$FW/boards/$board/pins"
             -I "$FW/common" -I "$LVGL")
  local objs=()
  for f in "${SPLASH_SRC[@]}"; do
    "${cxx[@]}" -c "$PROJ/src/ui/$f.cpp" -o "$OUT/splash_${name}_$f.o"
    objs+=("$OUT/splash_${name}_$f.o")
  done
  "${cxx[@]}" -c "$HERE/glass_splash_test.cpp" -o "$OUT/splash_${name}_test.o"
  g++ -o "$OUT/splash_${name}_test" "$OUT/splash_${name}_test.o" "${objs[@]}" \
    "$OUT"/lvgl/*.o -lm
  "$OUT/splash_${name}_test" || splash_failed+=("$name")
}
splash_suite watch watch xiao-esp32s3-round
splash_suite nightstand nightstand waveshare-esp32s3-lcd147
splash_suite touch169 touch169 waveshare-esp32s3-touch-lcd169
splash_suite amoled241 amoled241 waveshare-esp32s3-amoled241
splash_suite nightlight nightlight waveshare-esp32c3-lcd147 -DCD_LEAN_BUILD=1
splash_suite dash dash waveshare-esp32s3-lcd43
if ((${#splash_failed[@]})); then
  echo "glass_turn: the splash's tail FAILED in: ${splash_failed[*]}" >&2
  exit 1
fi
