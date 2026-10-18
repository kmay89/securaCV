#!/usr/bin/env bash
# canary-local/emulator/test/glass_turn_lvgl9.sh — the turned dash glass on
# the LVGL the dash builds ship (F225): builds glass_turn_lvgl9_test.cpp with
# g++ against the REAL ui/lvgl_port.cpp (dash config, its LVGL 9 branch) and
# the real LVGL 9.x release the display's Arduino profiles pin, compiled with
# the display's own lv_conf.h, and runs it; then builds the same test again
# in nightstand-c6's config (F248: the lean lv_conf, its C6 pin map, the
# port's static internal buffer, never turned) against LVGL compiled lean,
# and runs that. In each config it also builds glass_splash_lvgl9_test.cpp
# (F246), which links the REAL ui/splash.cpp and canary_mark.cpp, plays the
# first meeting and reads the speech bubble's tail off the frame. Every suite
# runs; the script fails at the end if any did. glass_turn.sh is its sibling for
# the emulator's LVGL 8.4; tests_host/test_lvgl_port_turn holds the same port
# against fake_lvgl9/lvgl.h, whose quotes of LVGL 9.5 this script also checks
# against the fetched source (check_lvgl9_quotes.py). No emsdk, no ESP32:
# this proves the port's sources against the library's sources on a 64-bit
# host, not an ESP32 build, and nothing here touches the emulator's dist.
#
#   bash canary-local/emulator/test/glass_turn_lvgl9.sh [LVGL9_DIR]
#
# The pin is sketch.yaml's dash-core3 profile ("- lvgl (9.5.0)", the exact
# release firmware.yml's core-3 build installs); every profile that names a
# 9.x LVGL must name the same one, and the PlatformIO dash env must ask for
# exactly it (lvgl/lvgl@<pin>; a caret or any other range is refused, F247:
# it would let `pio run` build a later 9.x than this renders) with lv_conf.h
# found the way this build finds it
# (-DLV_CONF_INCLUDE_SIMPLE, -I include/, not CD_LEAN_BUILD). The Arduino
# sketch's lv_conf.h copy must equal include/lv_conf.h. The native build
# defines no ESP_PLATFORM, so LVGL never reads the core's sdkconfig.h; the
# keys lv_conf.h sets win either way.
#
# LVGL9_DIR defaults to canary-local/emulator/test/third_party/lvgl, a
# gitignored checkout CI caches under its own key (canary-local.yml). When
# that default is absent this fetches it there, at v<pin>, the way
# glass_turn.sh fetches build.sh's 8.4; a default checkout at another release
# is refused (delete it and rerun), and an explicit LVGL9_DIR is never
# fetched into. GLASS_TURN9_OUT keeps the objects between runs (default: a
# temporary directory, removed on exit).
set -euo pipefail
export LC_ALL=C

HERE="$(cd "$(dirname "$0")" && pwd)"
EMU="$(cd "$HERE/.." && pwd)"
REPO="$(cd "$EMU/../.." && pwd)"
FW="$REPO/firmware"
PROJ="$FW/projects/canary-display"
SKETCH="$PROJ/arduino/canary_display/sketch.yaml"
INI="$FW/envs/platformio/canary-display.ini"

die() {
  echo "glass_turn_lvgl9: $*" >&2
  exit 2
}

# ── The pin, and the build it stands for ────────────────────────────────
# The lvgl release one sketch.yaml profile names.
profile_lvgl() {
  awk -v prof="  $1:" '
    $0 == prof { inside = 1; next }
    inside && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { inside = 0 }
    inside && match($0, /^[[:space:]]*- lvgl \(([0-9.]+)\)[[:space:]]*$/) {
      sub(/^[[:space:]]*- lvgl \(/, ""); sub(/\)[[:space:]]*$/, ""); print; exit
    }' "$SKETCH"
}
pin="$(profile_lvgl dash-core3)"
[[ "$pin" =~ ^9\.[0-9]+\.[0-9]+$ ]] ||
  die "sketch.yaml's dash-core3 profile names no LVGL 9.x release (got '${pin}')"
nines="$(sed -n 's/^[[:space:]]*- lvgl (\(9\.[0-9.]*\))[[:space:]]*$/\1/p' "$SKETCH" | sort -u | paste -sd' ')"
[[ "$nines" == "$pin" ]] ||
  die "sketch.yaml's profiles name LVGL 9 releases '$nines'; every one must be the dash-core3 pin $pin"
# A PlatformIO env asks for exactly the pin (F247): `lvgl/lvgl@^9.5.0` admits
# any 9.x, and v9.6.0 is tagged, so a caret (or ~, >=, a bare name) lets the
# release `pio run` builds be a LVGL this script never rendered.
require_exact_lvgl() {  # require_exact_lvgl <what> <ini lines>
  local asks
  asks="$({ grep -E '^[[:space:]]*lvgl/lvgl([@[:space:]].*)?$' <<<"$2" || true; } |
    sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | paste -sd' ')"
  [[ "$asks" == "lvgl/lvgl@$pin" ]] ||
    die "$1 asks for '${asks:-no lvgl/lvgl}', not exactly lvgl/lvgl@$pin (sketch.yaml's pin): a range such as ^$pin lets pio build a later 9.x than this renders"
}
dash_env="$(awk '/^\[env:canary-display-dash\][[:space:]]*$/ { p = 1; next } /^\[/ { p = 0 } p' "$INI")"
[[ -n "$dash_env" ]] || die "$INI has no [env:canary-display-dash]"
require_exact_lvgl "the PlatformIO dash env" "$dash_env"
grep -Eq '^[[:space:]]*-DLV_CONF_INCLUDE_SIMPLE[[:space:]]*$' <<<"$dash_env" ||
  die "the PlatformIO dash env no longer finds lv_conf.h by -DLV_CONF_INCLUDE_SIMPLE"
grep -Eq '^[[:space:]]*-I\$\{PROJECT_DIR\}/include[[:space:]]*$' <<<"$dash_env" ||
  die "the PlatformIO dash env no longer puts include/ (lv_conf.h) on the path"
if grep -q 'CD_LEAN_BUILD' <<<"$dash_env"; then
  die "the PlatformIO dash env builds lean now; this test compiles lv_conf.h without CD_LEAN_BUILD"
fi
cmp -s "$PROJ/include/lv_conf.h" "$PROJ/arduino/canary_display/lv_conf.h" ||
  die "the Arduino sketch's lv_conf.h differs from include/lv_conf.h (./setup.sh regen)"

# nightstand-c6 (F248), the other release that compiles lvgl_port.cpp's
# LVGL 9 branch: its core-3 base must ask for the same release, and the env
# must still be the lean nightstand on the C6 pin map with lv_conf.h found
# this way — the config the second build below compiles.
env_block() {  # env_block <section>: the lines of one [section] of the .ini
  awk -v want="[$1]" '$0 == want { p = 1; next } /^\[/ { p = 0 } p' "$INI"
}
c6_env="$(env_block env:canary-display-nightstand-c6)"
[[ -n "$c6_env" ]] || die "$INI has no [env:canary-display-nightstand-c6]"
grep -Eq '^extends[[:space:]]*=[[:space:]]*canary_display_c6_core3[[:space:]]*$' <<<"$c6_env" ||
  die "the nightstand-c6 env no longer extends canary_display_c6_core3"
require_exact_lvgl "canary_display_c6_core3" "$(env_block canary_display_c6_core3)"
for flag in '-DCD_LEAN_BUILD=1' '-DLV_CONF_INCLUDE_SIMPLE' \
            '-I${PROJECT_DIR}/include' \
            '-I${PROJECT_DIR}/../../configs/canary-display/nightstand' \
            '-I${PROJECT_DIR}/../../boards/waveshare-esp32c6-lcd147/pins'; do
  grep -Fxq "    $flag" <<<"$c6_env" ||
    die "the nightstand-c6 env's build_flags no longer carry '$flag' (this test builds that config)"
done
# The splash's pseudonym (F246): native9/identity/device_pseudonym.h stands
# in for the NVS-backed one with HEX_LEN = TOKEN_BYTES * 2 = 16 letters, the
# length the first meeting's line wraps around; the real header must agree.
PSEUDO="$FW/common/identity/device_pseudonym.h"
grep -Eq '^constexpr size_t TOKEN_BYTES = 8;' "$PSEUDO" &&
  grep -Eq '^constexpr size_t HEX_LEN[[:space:]]+= TOKEN_BYTES \* 2;' "$PSEUDO" ||
  die "$PSEUDO no longer makes a 16-letter pseudonym (TOKEN_BYTES 8, HEX_LEN TOKEN_BYTES * 2): update native9/identity/device_pseudonym.h"
tag="v$pin"

# ── The library ─────────────────────────────────────────────────────────
DEFAULT="$HERE/third_party/lvgl"
if [[ $# -eq 0 && ! -d "$DEFAULT" ]]; then
  mkdir -p "$HERE/third_party"
  git clone --depth 1 --branch "$tag" https://github.com/lvgl/lvgl.git "$DEFAULT"
fi
LVGL="$(cd "${1:-$DEFAULT}" 2>/dev/null && pwd || true)"
if [[ -z "$LVGL" || ! -f "$LVGL/lvgl.h" || ! -f "$LVGL/lv_version.h" ]]; then
  die "no LVGL 9 checkout at ${1:-$DEFAULT} (pass one, or let this fetch $tag)"
fi
ver="$(sed -n 's/^#define LVGL_VERSION_\(MAJOR\|MINOR\|PATCH\) *\([0-9]*\).*/\2/p' "$LVGL/lv_version.h" | paste -sd.)"
if [[ "$ver" != "$pin" ]]; then
  if [[ $# -eq 0 ]]; then
    die "$LVGL is LVGL $ver, not the $pin sketch.yaml pins: delete it and rerun to fetch $tag"
  fi
  die "$LVGL is LVGL $ver, not the $pin sketch.yaml pins"
fi

# test_lvgl_port_turn's quotes of this release, held to it — the checker
# first proving it fails on a drift, then reading the checkout.
python3 "$HERE/check_lvgl9_quotes.py" --self-test
python3 "$HERE/check_lvgl9_quotes.py" "$LVGL"

if [[ -n "${GLASS_TURN9_OUT:-}" ]]; then
  OUT="$GLASS_TURN9_OUT"
else
  OUT="$(mktemp -d)"
  trap '[[ -n "$OUT" && -d "$OUT" ]] && rm -rf -- "$OUT"' EXIT
fi
mkdir -p "$OUT/lvgl"

# The dash env's wiring, as its build_flags hand it to the compiler.
CFG="$FW/configs/canary-display/dash"
PINS="$FW/boards/waveshare-esp32s3-lcd43/pins"
DEFS=(-DARDUINO=10812 -DLV_CONF_INCLUDE_SIMPLE)

# LVGL, once per checkout and lv_conf (an object newer than its source is
# kept): build_lvgl <objdir> <extra flags...>. CD_LEAN_BUILD changes what
# lv_conf.h compiles (no 36/48 px faces, the stock refresh period), so the
# lean config has its own objects.
build_lvgl() {
  local objs="$1"
  shift
  mkdir -p "$objs"
  export OUT LVGL OBJS="$objs"
  export LVGL_FLAGS="-std=gnu11 -O1 -w ${DEFS[*]} $* -I $PROJ/include -I $LVGL"
  # shellcheck disable=SC2016  # the inner script expands its own variables
  find "$LVGL/src" -name '*.c' -print0 | sort -z | xargs -0 -n 8 -P "$(nproc)" bash -c '
    for src in "$@"; do
      obj="$OBJS/$(echo "${src#$LVGL/}" | tr "/." "__").o"
      [[ -f "$obj" && "$obj" -nt "$src" ]] && continue
      gcc $LVGL_FLAGS -c "$src" -o "$obj"
    done' _
}
build_lvgl "$OUT/lvgl"

CXX=(g++ -std=gnu++17 -O1 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter
     "${DEFS[@]}" -I "$HERE/native9" -I "$PROJ/include" -I "$CFG" -I "$PINS"
     -I "$FW/common" -I "$LVGL")
"${CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp" -o "$OUT/lvgl_port.o"
"${CXX[@]}" -c "$HERE/glass_turn_lvgl9_test.cpp" -o "$OUT/glass_turn_lvgl9_test.o"
g++ -o "$OUT/glass_turn_lvgl9_test" "$OUT/glass_turn_lvgl9_test.o" "$OUT/lvgl_port.o" \
  "$OUT"/lvgl/*.o -lm
# Every suite runs and the script fails at the end if any did, so a red
# dash turn cannot hide another config's result (a build error still stops
# it there).
failed=()
run_suite() {  # run_suite <name> <binary>
  "$2" || failed+=("$1")
}
run_suite "dash turns" "$OUT/glass_turn_lvgl9_test"

# The boot splash's tail through the same port and library (F246): the
# REAL splash.cpp and canary_mark.cpp, with the theme, the Character and
# the story engine they read, the splash clock driven by its own pump.
SPLASH_SRC=(splash canary_mark theme character)
build_splash() {  # build_splash <prefix> <port object> <lvgl objdir> <compiler...>
  local pre="$1" port="$2" objs="$3"
  shift 3
  local o=()
  for f in "${SPLASH_SRC[@]}"; do
    "$@" -c "$PROJ/src/ui/$f.cpp" -o "$OUT/${pre}$f.o"
    o+=("$OUT/${pre}$f.o")
  done
  "$@" -c "$HERE/glass_splash_lvgl9_test.cpp" -o "$OUT/${pre}glass_splash_lvgl9_test.o"
  g++ -o "$OUT/${pre}glass_splash_lvgl9_test" "$OUT/${pre}glass_splash_lvgl9_test.o" "${o[@]}" \
    "$port" "$objs"/*.o -lm
}
build_splash "" "$OUT/lvgl_port.o" "$OUT/lvgl" "${CXX[@]}"
run_suite "dash splash tail" "$OUT/glass_splash_lvgl9_test"

# nightstand-c6's wiring (F248), as its env's build_flags hand it to the
# compiler (the defines that reach the display UI; the OTA and secrets
# paths do not): the nightstand config on the C6 pin map, built lean. The
# flavor is its config.h on the -I path (CD_FLAVOR_NIGHTSTAND); the env has
# carried no CONFIG_* marker since 2026-10-09.
C6_CFG="$FW/configs/canary-display/nightstand"
C6_PINS="$FW/boards/waveshare-esp32c6-lcd147/pins"
C6_DEFS=(-DCD_LEAN_BUILD=1 -DFEATURE_CHIRP_SCAN=0 -DFEATURE_FLEET_LINK=0
         -DCANARY_MQTT_PLAIN_ONLY=1)
build_lvgl "$OUT/lvgl_lean" -DCD_LEAN_BUILD=1
C6_CXX=(g++ -std=gnu++17 -O1 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter
        "${DEFS[@]}" "${C6_DEFS[@]}" -I "$HERE/native9" -I "$PROJ/include" -I "$C6_CFG"
        -I "$C6_PINS" -I "$FW/common" -I "$LVGL")
"${C6_CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp" -o "$OUT/c6_lvgl_port.o"
"${C6_CXX[@]}" -c "$HERE/glass_turn_lvgl9_test.cpp" -o "$OUT/c6_glass_turn_lvgl9_test.o"
g++ -o "$OUT/c6_glass_turn_lvgl9_test" "$OUT/c6_glass_turn_lvgl9_test.o" \
  "$OUT/c6_lvgl_port.o" "$OUT"/lvgl_lean/*.o -lm
run_suite "nightstand-c6 glass" "$OUT/c6_glass_turn_lvgl9_test"
build_splash c6_ "$OUT/c6_lvgl_port.o" "$OUT/lvgl_lean" "${C6_CXX[@]}"
run_suite "nightstand-c6 splash tail" "$OUT/c6_glass_splash_lvgl9_test"

if ((${#failed[@]})); then
  echo "glass_turn_lvgl9: FAILED: ${failed[*]}" >&2
  exit 1
fi
