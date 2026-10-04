#!/usr/bin/env bash
# canary-local/emulator/test/glass_turn_lvgl9.sh — the turned dash glass on
# the LVGL the dash builds ship (F225): builds glass_turn_lvgl9_test.cpp with
# g++ against the REAL ui/lvgl_port.cpp (dash config, its LVGL 9 branch) and
# the real LVGL 9.x release the display's Arduino profiles pin, compiled with
# the display's own lv_conf.h, and runs it. glass_turn.sh is its sibling for
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
# it (lvgl/lvgl@^<pin>) with lv_conf.h found the way this build finds it
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
dash_env="$(awk '/^\[env:canary-display-dash\][[:space:]]*$/ { p = 1; next } /^\[/ { p = 0 } p' "$INI")"
[[ -n "$dash_env" ]] || die "$INI has no [env:canary-display-dash]"
grep -Eq "^[[:space:]]*lvgl/lvgl@\\^${pin//./\\.}[[:space:]]*$" <<<"$dash_env" ||
  die "the PlatformIO dash env does not ask for lvgl/lvgl@^$pin (sketch.yaml's pin)"
grep -Eq '^[[:space:]]*-DLV_CONF_INCLUDE_SIMPLE[[:space:]]*$' <<<"$dash_env" ||
  die "the PlatformIO dash env no longer finds lv_conf.h by -DLV_CONF_INCLUDE_SIMPLE"
grep -Eq '^[[:space:]]*-I\$\{PROJECT_DIR\}/include[[:space:]]*$' <<<"$dash_env" ||
  die "the PlatformIO dash env no longer puts include/ (lv_conf.h) on the path"
if grep -q 'CD_LEAN_BUILD' <<<"$dash_env"; then
  die "the PlatformIO dash env builds lean now; this test compiles lv_conf.h without CD_LEAN_BUILD"
fi
cmp -s "$PROJ/include/lv_conf.h" "$PROJ/arduino/canary_display/lv_conf.h" ||
  die "the Arduino sketch's lv_conf.h differs from include/lv_conf.h (./setup.sh regen)"
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
DEFS=(-DARDUINO=10812 -DLV_CONF_INCLUDE_SIMPLE -DCONFIG_CANARY_DISPLAY)

# LVGL, once per checkout (an object newer than its source is kept).
export OUT LVGL
export LVGL_FLAGS="-std=gnu11 -O1 -w ${DEFS[*]} -I $PROJ/include -I $LVGL"
# shellcheck disable=SC2016  # the inner script expands its own variables
find "$LVGL/src" -name '*.c' -print0 | sort -z | xargs -0 -n 8 -P "$(nproc)" bash -c '
  for src in "$@"; do
    obj="$OUT/lvgl/$(echo "${src#$LVGL/}" | tr "/." "__").o"
    [[ -f "$obj" && "$obj" -nt "$src" ]] && continue
    gcc $LVGL_FLAGS -c "$src" -o "$obj"
  done' _

CXX=(g++ -std=gnu++17 -O1 -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter
     "${DEFS[@]}" -I "$HERE/native9" -I "$PROJ/include" -I "$CFG" -I "$PINS"
     -I "$FW/common" -I "$LVGL")
"${CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp" -o "$OUT/lvgl_port.o"
"${CXX[@]}" -c "$HERE/glass_turn_lvgl9_test.cpp" -o "$OUT/glass_turn_lvgl9_test.o"
g++ -o "$OUT/glass_turn_lvgl9_test" "$OUT/glass_turn_lvgl9_test.o" "$OUT/lvgl_port.o" \
  "$OUT"/lvgl/*.o -lm
"$OUT/glass_turn_lvgl9_test"
