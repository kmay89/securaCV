#!/usr/bin/env bash
# canary-local/emulator/test/runtime_turn.sh — the nightlight turned while it
# runs, natively (F222): builds runtime_turn_test.cpp with g++ against
# build.sh's whole nightlight TU list (the REAL src/main.cpp and firmware, the
# emulator's src/*.cpp and shims) and LVGL 8.4, rweather's Crypto and
# ArduinoJson at build.sh's pins, then runs it: a provisioned nightlight
# booted at each rotation, and turned at 7 s from each rotation to each other
# one through nightlight_request_rotation(), must seat its companion where a
# boot at that rotation seats it. No emsdk: this proves the sources the dist
# is built from, not the dist's bytes.
#
#   bash canary-local/emulator/test/runtime_turn.sh [LVGL_DIR [ARDUINOJSON_DIR [ARDUINOLIBS_DIR]]]
#
# Each directory defaults to build.sh's checkout under
# canary-local/emulator/third_party (lvgl, ArduinoJson, arduinolibs). A
# default that is absent (a cold third-party cache) is fetched there, at the
# pin build.sh names and the way build.sh fetches it, so build.sh later finds
# it and skips its own clone — what lets CI (canary-local.yml) run this beside
# glass_turn.sh, before any step that reads the committed dist. An explicit
# directory is never fetched into. RUNTIME_TURN_OUT keeps the objects between
# runs (default: a fresh temporary directory).
set -euo pipefail
export LC_ALL=C

HERE="$(cd "$(dirname "$0")" && pwd)"
EMU="$(cd "$HERE/.." && pwd)"
FW="$(cd "$EMU/../../firmware" && pwd)"
PROJ="$FW/projects/canary-display"
TP="$EMU/third_party"

pin() {  # pin <NAME>: build.sh's NAME="value"
  local v
  v="$(sed -n "s/^$1=\"\\([^\"]*\\)\"\$/\\1/p" "$EMU/build.sh")"
  if [[ -z "$v" ]]; then
    echo "runtime_turn: build.sh names no $1 to fetch" >&2
    exit 2
  fi
  printf '%s' "$v"
}
if [[ $# -lt 1 && ! -d "$TP/lvgl" ]]; then
  mkdir -p "$TP"
  git clone --depth 1 --branch "$(pin LVGL_TAG)" https://github.com/lvgl/lvgl.git "$TP/lvgl"
fi
if [[ $# -lt 2 && ! -d "$TP/ArduinoJson" ]]; then
  mkdir -p "$TP"
  git clone --depth 1 --branch "v$(pin ARDUINOJSON_VER)" \
    https://github.com/bblanchon/ArduinoJson.git "$TP/ArduinoJson"
fi
if [[ $# -lt 3 && ! -d "$TP/arduinolibs" ]]; then
  mkdir -p "$TP"
  git clone https://github.com/rweather/arduinolibs.git "$TP/arduinolibs"
fi
if [[ $# -lt 3 ]]; then
  git -C "$TP/arduinolibs" -c advice.detachedHead=false checkout -q "$(pin ARDUINOLIBS_COMMIT)"
fi
LVGL="$(cd "${1:-$TP/lvgl}" 2>/dev/null && pwd || true)"
AJ="$(cd "${2:-$TP/ArduinoJson}" 2>/dev/null && pwd || true)"
AL="$(cd "${3:-$TP/arduinolibs}" 2>/dev/null && pwd || true)"
if [[ -z "$LVGL" || ! -f "$LVGL/lvgl.h" ]]; then
  echo "runtime_turn: no LVGL checkout at ${1:-$TP/lvgl}" >&2
  exit 2
fi
ver="$(sed -n 's/^#define LVGL_VERSION_MAJOR *\([0-9]*\).*/\1/p;s/^#define LVGL_VERSION_MINOR *\([0-9]*\).*/\1/p' "$LVGL/lvgl.h" | tr '\n' '.')"
if [[ "$ver" != "8.4." ]]; then
  echo "runtime_turn: $LVGL is LVGL ${ver%.}, not the 8.4 build.sh pins" >&2
  exit 2
fi
if [[ -z "$AJ" || ! -f "$AJ/src/ArduinoJson.h" ]]; then
  echo "runtime_turn: no ArduinoJson checkout at ${2:-$TP/ArduinoJson}" >&2
  exit 2
fi
CRYPTO="$AL/libraries/Crypto"
if [[ -z "$AL" || ! -f "$CRYPTO/Ed25519.cpp" ]]; then
  echo "runtime_turn: no rweather/arduinolibs checkout at ${3:-$TP/arduinolibs}" >&2
  exit 2
fi

OUT="${RUNTIME_TURN_OUT:-$(mktemp -d)}"
mkdir -p "$OUT/lvgl" "$OUT/fw"

# The nightlight flavor's wiring, as build.sh hands it to em++ (its pin map,
# its config, the lean budget its env sets), with runtime_turn/ first on the
# path: its emscripten.h hands every call into the page to the driver, and its
# lv_conf.h is the display's with a pool sized for 64-bit objects.
CFG="$FW/configs/canary-display/nightlight"
PINS="$FW/boards/waveshare-esp32c3-lcd147/pins"
DEFS=(-DARDUINO=10812 -DLV_CONF_INCLUDE_SIMPLE -DCONFIG_CANARY_DISPLAY
      '-DEMU_BUILD_FLAVOR="nightlight"' -DFEATURE_CHIME=1
      -DARDUINOJSON_ENABLE_ARDUINO_STRING=0 -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0
      -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0 -DARDUINOJSON_ENABLE_PROGMEM=0
      -DCD_LEAN_BUILD=1)
INC=(-I "$HERE/runtime_turn" -I "$EMU/shim" -I "$EMU/src" -I "$PROJ/include"
     -I "$CFG" -I "$PINS" -I "$FW/common" -I "$LVGL" -I "$CRYPTO" -I "$AJ/src")

# LVGL, once per checkout and output directory (an object newer than its
# source is kept), with the same configuration as the firmware TUs.
export OUT LVGL
export LVGL_FLAGS="-std=gnu11 -O1 -w ${DEFS[*]} ${INC[*]}"
# shellcheck disable=SC2016  # the inner script expands $OUT, $LVGL and $LVGL_FLAGS itself
find "$LVGL/src" -name '*.c' -print0 | sort -z | xargs -0 -n 8 -P "$(nproc)" bash -c '
  for src in "$@"; do
    obj="$OUT/lvgl/$(echo "${src#$LVGL/}" | tr "/." "__").o"
    [[ -f "$obj" && "$obj" -nt "$src" ]] && continue
    gcc $LVGL_FLAGS -c "$src" -o "$obj"
  done' _

# build.sh's FIRMWARE_SRCS for the nightlight (the base list and the color
# block the portrait flavors add), its CRYPTO_SRCS and its EMU_SRCS; emu_main.cpp
# is built with its main() renamed, so the driver owns the process.
# onboard.test.js holds these lists to build.sh's.
FIRMWARE_SRCS=(
  "$PROJ/src/main.cpp"
  "$PROJ/src/glass_settings.cpp"
  "$PROJ/src/runtime_config.cpp"
  "$PROJ/src/trust.cpp"
  "$PROJ/src/diagnostics.cpp"
  "$PROJ/src/hal/chime.cpp"
  "$PROJ/src/net/mqtt_mgr.cpp"
  "$PROJ/src/net/provision.cpp"
  "$PROJ"/src/ui/*.cpp
  "$PROJ"/src/care/*.cpp
  "$PROJ"/src/fleet/*.cpp
  "$FW/common/boot/boot_banner.cpp"
  "$FW/common/color/color_engine.cpp"
  "$FW/common/color/look_engine.cpp"
  "$FW/common/color/plumage.cpp"
  "$PROJ/src/hal/ambient_led.cpp"
)
CRYPTO_SRCS=(
  "$CRYPTO/Ed25519.cpp"
  "$CRYPTO/Curve25519.cpp"
  "$CRYPTO/SHA512.cpp"
  "$CRYPTO/BigNumberUtil.cpp"
  "$CRYPTO/Crypto.cpp"
  "$CRYPTO/Hash.cpp"
)
EMU_SRCS=("$EMU"/src/*.cpp)
EMU_C_SRCS=("$EMU"/src/*.c)

# <time.h> by force: emu_support.cpp calls tzset() and reads struct tm through
# what emscripten's own headers pull in, which glibc's do not.
CXX=(g++ -std=gnu++17 -O1 -w -fno-exceptions -fno-rtti -include time.h "${DEFS[@]}" "${INC[@]}")
objs=()
pids=()
for src in "${FIRMWARE_SRCS[@]}" "${CRYPTO_SRCS[@]}" "${EMU_SRCS[@]}" "$HERE/runtime_turn_test.cpp"; do
  obj="$OUT/fw/$(echo "$src" | tr '/.' '__').o"
  objs+=("$obj")
  extra=()
  [[ "$(basename "$src")" == "emu_main.cpp" ]] && extra=(-Dmain=emu_entry)
  "${CXX[@]}" "${extra[@]}" -c "$src" -o "$obj" &
  pids+=($!)
  if (( ${#pids[@]} >= $(nproc) )); then
    wait "${pids[0]}"
    pids=("${pids[@]:1}")
  fi
done
for p in "${pids[@]}"; do wait "$p"; done
for src in "${EMU_C_SRCS[@]}"; do
  obj="$OUT/fw/$(echo "$src" | tr '/.' '__').o"
  objs+=("$obj")
  gcc -std=gnu11 -O1 -w "${DEFS[@]}" "${INC[@]}" -c "$src" -o "$obj"
done
# The emulator's time() wrap, linked the way build.sh links it.
g++ -o "$OUT/runtime_turn_test" "${objs[@]}" "$OUT"/lvgl/*.o -Wl,--wrap=time -lm
"$OUT/runtime_turn_test"
