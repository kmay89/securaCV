#!/usr/bin/env bash
# canary-local/emulator/test/glass_turn.sh — the turned glass, natively
# (F184, F204): builds glass_turn_test.cpp with g++ against the REAL
# ui/lvgl_port.cpp, the emulator's REAL display HAL and LVGL 8.4, and runs
# it twice: in the dash config (LVGL's software rotation) and in the
# nightlight config (the panel's hardware turn). No emsdk: this proves the sources the dist is built from,
# not the dist's bytes (the browser probe, onboard_probe.mjs, reads those).
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
# is never fetched into. GLASS_TURN_OUT keeps the objects between runs
# (default: a fresh temporary directory).
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

OUT="${GLASS_TURN_OUT:-$(mktemp -d)}"
mkdir -p "$OUT/lvgl"

# The dash flavor's wiring, as build.sh hands it to em++.
CFG="$FW/configs/canary-display/dash"
PINS="$FW/boards/waveshare-esp32s3-lcd43/pins"
DEFS=(-DARDUINO=10812 -DLV_CONF_INCLUDE_SIMPLE -DCONFIG_CANARY_DISPLAY)
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
"$OUT/nl_glass_turn_test"
