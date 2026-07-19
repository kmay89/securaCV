#!/usr/bin/env bash
# canary-local/emulator/build.sh — compile the canary-display firmware to
# WebAssembly, one flavor per artifact.
#
#   ./build.sh watch     → dist/canary-display-watch.js   (240×240 round)
#   ./build.sh dash      → dist/canary-display-dash.js    (800×480 panel)
#   ./build.sh all
#
# The build compiles the REAL firmware sources (src/main.cpp, the LVGL
# faces, care/fleet/trust) plus LVGL v8.4.0 and the same rweather/Crypto
# Ed25519 the device links, against the shim layer in shim/ (silicon
# boundary only: clock, NVS, radio, panel, entropy). -sSINGLE_FILE embeds
# the wasm, so each artifact is one self-contained .js that works from
# file:// — no server, no CDN, nothing leaves the machine (Invariant IV
# extends to the docs).
#
# Dependencies (fetched once into third_party/, pinned):
#   lvgl v8.4.0                (same pin as canary-display.ini)
#   rweather/arduinolibs       (Crypto — same library the firmware uses)
#   ArduinoJson v7 single-header (same major as common.ini)
set -euo pipefail

cd "$(dirname "$0")"
EMU_DIR="$PWD"
REPO_ROOT="$(cd "$EMU_DIR/../.." && pwd)"
FW="$EMU_DIR/../../firmware"
PROJ="$FW/projects/canary-display"
TP="$EMU_DIR/third_party"
DIST="$EMU_DIR/dist"
BUILD="$EMU_DIR/.build"

LVGL_TAG="v8.4.0"
ARDUINOJSON_VER="7.4.1"
# rweather/arduinolibs has no release tags; pin the audited commit so the
# Ed25519 verify path can never drift under the wasm build silently.
ARDUINOLIBS_COMMIT="37a76b8f7516568e1c575b6dc9268da1ccaac6b6"

FLAVOR="${1:-watch}"
if [[ "$FLAVOR" == "all" ]]; then
  "$0" watch
  "$0" dash
  exit 0
fi
[[ "$FLAVOR" == "watch" || "$FLAVOR" == "dash" ]] || {
  echo "usage: $0 [watch|dash|all]" >&2
  exit 2
}

command -v emcc >/dev/null || {
  echo "emcc not found — install emscripten (apt install emscripten, or emsdk)" >&2
  exit 1
}

# ── Pinned third-party fetch (idempotent) ───────────────────────────────
mkdir -p "$TP" "$DIST"
if [[ ! -d "$TP/lvgl" ]]; then
  git clone --depth 1 --branch "$LVGL_TAG" https://github.com/lvgl/lvgl.git "$TP/lvgl"
fi
if [[ ! -d "$TP/arduinolibs" ]]; then
  git clone https://github.com/rweather/arduinolibs.git "$TP/arduinolibs"
fi
git -C "$TP/arduinolibs" -c advice.detachedHead=false checkout -q "$ARDUINOLIBS_COMMIT"
if [[ ! -d "$TP/ArduinoJson" ]]; then
  git clone --depth 1 --branch "v${ARDUINOJSON_VER}" \
    https://github.com/bblanchon/ArduinoJson.git "$TP/ArduinoJson"
fi

# ── Flavor wiring (mirrors envs/platformio/canary-display.ini) ──────────
if [[ "$FLAVOR" == "watch" ]]; then
  PINS_DIR="$FW/boards/xiao-esp32s3-round/pins"
  CFG_DIR="$FW/configs/canary-display/watch"
else
  PINS_DIR="$FW/boards/waveshare-esp32s3-lcd43/pins"
  CFG_DIR="$FW/configs/canary-display/dash"
fi

OUT_BASE="canary-display-$FLAVOR"
if [[ "$FLAVOR" == "watch" ]]; then EXPORT_NAME="createCanaryEmuWatch"; else EXPORT_NAME="createCanaryEmuDash"; fi
OBJ="$BUILD/$FLAVOR"
mkdir -p "$OBJ"

INCLUDES=(
  -I "$EMU_DIR/shim"
  -I "$EMU_DIR/src"
  -I "$PROJ/include"
  -I "$CFG_DIR"
  -I "$PINS_DIR"
  -I "$FW/common"
  -I "$TP/lvgl"
  -I "$TP/arduinolibs/libraries/Crypto"
  -I "$TP/ArduinoJson/src"
)

DEFINES=(
  -DARDUINO=10812
  -DLV_CONF_INCLUDE_SIMPLE
  -DCONFIG_CANARY_DISPLAY
  -DEMU_BUILD_FLAVOR="\"$FLAVOR\""
  # Reproducible bytes: the boot banner prints __DATE__/__TIME__, which
  # would make every rebuild differ and trip CI's dist drift gate. The
  # honest wall-clock stamp lives in dist/*.meta.json instead.
  -Wno-builtin-macro-redefined
  '-D__DATE__="emu"'
  '-D__TIME__="build"'
  # ...and __FILE__/asserts embed absolute source paths, which differ
  # between a laptop and a CI runner. Map both roots to stable names so
  # the same sources produce the same bytes anywhere.
  -ffile-prefix-map="$REPO_ROOT"=/securacv
  -ffile-prefix-map="$EMU_DIR"=/securacv/canary-local/emulator
  # ArduinoJson in plain-C++ mode: the shim String is not the real one,
  # and the display parses from byte buffers only (no Stream/Print/Flash).
  -DARDUINOJSON_ENABLE_ARDUINO_STRING=0
  -DARDUINOJSON_ENABLE_ARDUINO_STREAM=0
  -DARDUINOJSON_ENABLE_ARDUINO_PRINT=0
  -DARDUINOJSON_ENABLE_PROGMEM=0
)

WARN=(-Wall -Wno-unused-parameter)
OPT=(-O2)
CFLAGS=(-std=gnu11 "${OPT[@]}" "${WARN[@]}" "${INCLUDES[@]}" "${DEFINES[@]}")
CXXFLAGS=(-std=gnu++17 -fno-exceptions -fno-rtti "${OPT[@]}" "${WARN[@]}" "${INCLUDES[@]}" "${DEFINES[@]}")

# ── Source lists ────────────────────────────────────────────────────────
FIRMWARE_SRCS=(
  "$PROJ/src/main.cpp"
  "$PROJ/src/glass_settings.cpp"
  "$PROJ/src/runtime_config.cpp"
  "$PROJ/src/trust.cpp"
  "$PROJ/src/diagnostics.cpp"
  "$PROJ/src/hal/chime.cpp"
  "$PROJ/src/net/mqtt_mgr.cpp"
  "$PROJ"/src/ui/*.cpp
  "$PROJ"/src/care/*.cpp
  "$PROJ"/src/fleet/*.cpp
  "$FW/common/boot/boot_banner.cpp"
)
# The two silicon HALs are replaced by emu_hal_display.cpp; everything
# else in src/ compiles verbatim. (net/* are replaced by emu_net/emu_mqtt.)

CRYPTO_SRCS=(
  "$TP/arduinolibs/libraries/Crypto/Ed25519.cpp"
  "$TP/arduinolibs/libraries/Crypto/Curve25519.cpp"
  "$TP/arduinolibs/libraries/Crypto/SHA512.cpp"
  "$TP/arduinolibs/libraries/Crypto/BigNumberUtil.cpp"
  "$TP/arduinolibs/libraries/Crypto/Crypto.cpp"
  "$TP/arduinolibs/libraries/Crypto/Hash.cpp"
)

EMU_SRCS=(
  "$EMU_DIR"/src/*.cpp
)
EMU_C_SRCS=(
  "$EMU_DIR"/src/*.c
)

LVGL_SRCS=()
while IFS= read -r -d '' f; do LVGL_SRCS+=("$f"); done \
  < <(find "$TP/lvgl/src" -name '*.c' -print0)

# ── Compile ─────────────────────────────────────────────────────────────
OBJS=()
compile() {
  local src="$1" std_is_c="$2"
  local rel obj
  rel="$(echo "$src" | sed 's|[/.]|_|g')"
  obj="$OBJ/$rel.o"
  OBJS+=("$obj")
  if [[ -f "$obj" && "$obj" -nt "$src" ]]; then return; fi
  if [[ "$std_is_c" == "c" ]]; then
    emcc -c "$src" "${CFLAGS[@]}" -o "$obj"
  else
    em++ -c "$src" "${CXXFLAGS[@]}" -o "$obj"
  fi
}

echo "── compiling LVGL ($LVGL_TAG) ──"
for f in "${LVGL_SRCS[@]}"; do compile "$f" c; done
echo "── compiling firmware (canary-display/$FLAVOR @ $(git -C "$FW/.." rev-parse --short HEAD 2>/dev/null || echo dev)) ──"
for f in "${FIRMWARE_SRCS[@]}"; do compile "$f" cpp; done
echo "── compiling Crypto (Ed25519) ──"
for f in "${CRYPTO_SRCS[@]}"; do compile "$f" cpp; done
echo "── compiling emulator shims ──"
for f in "${EMU_SRCS[@]}"; do compile "$f" cpp; done
for f in "${EMU_C_SRCS[@]}"; do compile "$f" c; done

# ── Link ────────────────────────────────────────────────────────────────
echo "── linking $OUT_BASE ──"
em++ "${OBJS[@]}" \
  -O2 \
  -sASYNCIFY \
  -sASYNCIFY_STACK_SIZE=32768 \
  -sALLOW_MEMORY_GROWTH \
  -sINITIAL_MEMORY=33554432 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME="$EXPORT_NAME" \
  -sENVIRONMENT=web \
  -sSINGLE_FILE=1 \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \
  -Wl,--wrap=time \
  -o "$DIST/$OUT_BASE.js"

# Build stamp for the page footer: which firmware these bytes are.
FW_VERSION="$(sed -n 's/.*CANARY_FW_VERSION "\(.*\)".*/\1/p' "$PROJ/include/canary/version.h")"
GIT_SHA="$(git -C "$FW/.." rev-parse --short HEAD 2>/dev/null || echo dev)"
cat > "$DIST/$OUT_BASE.meta.json" <<EOF
{
  "flavor": "$FLAVOR",
  "fw_version": "$FW_VERSION",
  "git": "$GIT_SHA",
  "lvgl": "$LVGL_TAG",
  "arduinojson": "$ARDUINOJSON_VER"
}
EOF

ls -la "$DIST/$OUT_BASE.js"
echo "OK: $OUT_BASE (fw $FW_VERSION @ $GIT_SHA)"
