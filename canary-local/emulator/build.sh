#!/usr/bin/env bash
# canary-local/emulator/build.sh — compile the canary-display firmware to
# WebAssembly, one flavor per artifact.
#
#   ./build.sh watch     → dist/canary-display-watch.js   (240×240 round)
#   ./build.sh dash      → dist/canary-display-dash.js    (800×480 panel)
#   ./build.sh vision    → dist/canary-vision-core.js     (real witness core)
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
# Toolchain: Emscripten 6.0.3 (pinned because dist is byte-compared in CI).
# Dependencies (fetched once into third_party/, pinned):
#   lvgl v8.4.0                (same pin as canary-display.ini)
#   rweather/arduinolibs       (Crypto — same library the firmware uses)
#   ArduinoJson v7 single-header (same major as common.ini)
set -euo pipefail

# Deterministic inputs = deterministic bytes: glob expansion and find
# traversal must not depend on locale or filesystem directory order —
# link order shapes the wasm type section, and CI's drift gate compares
# bytes.
export LC_ALL=C

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
EMSCRIPTEN_VERSION="6.0.3"

# The git stamp written into dist/*.meta.json. NOT the checkout sha: the
# pinned-emsdk rebuild bot (.github/workflows/emulator-dist-refresh.yml) runs
# on PR branches, and after a squash-merge a PR-branch sha is unreachable
# from main — so the committed stamp named a commit nobody could check out.
# Stamp the newest MAIN commit these bytes descend from instead: the
# merge-base of HEAD and origin/main. On main itself that is HEAD; on a
# branch it is the fork point, which survives the squash. Falls back to HEAD
# when there is no origin/main to measure against (a fork whose upstream
# remote has another name, a shallow clone with no common history) and to
# "dev" outside git. Committed values move on the next bot rebuild — the
# drift gate compares the bundles and deliberately ignores meta.json.
stamp_sha() {
  local base=""
  if git -C "$REPO_ROOT" rev-parse --verify -q origin/main >/dev/null 2>&1; then
    base="$(git -C "$REPO_ROOT" merge-base HEAD origin/main 2>/dev/null || true)"
  fi
  git -C "$REPO_ROOT" rev-parse --short "${base:-HEAD}" 2>/dev/null || echo dev
}
GIT_SHA="$(stamp_sha)"

FLAVOR="${1:-watch}"
if [[ "$FLAVOR" == "all" ]]; then
  "$0" watch
  "$0" dash
  "$0" nightstand
  "$0" touch169
  "$0" amoled241
  "$0" vision
  "$0" audio
  exit 0
fi
[[ "$FLAVOR" == "watch" || "$FLAVOR" == "dash" || "$FLAVOR" == "nightstand" || \
   "$FLAVOR" == "touch169" || "$FLAVOR" == "amoled241" || \
   "$FLAVOR" == "vision" || "$FLAVOR" == "audio" ]] || {
  echo "usage: $0 [watch|dash|nightstand|touch169|amoled241|vision|audio|all]" >&2
  exit 2
}

command -v emcc >/dev/null || {
  echo "emcc not found — install Emscripten SDK $EMSCRIPTEN_VERSION" >&2
  exit 1
}
EMCC_VERSION="$(emcc --version | head -1)"
[[ "$EMCC_VERSION" == *" $EMSCRIPTEN_VERSION-git"* ||
   "$EMCC_VERSION" == *" $EMSCRIPTEN_VERSION ("* ]] || {
  echo "wrong Emscripten version — need $EMSCRIPTEN_VERSION for reproducible dist artifacts" >&2
  echo "found: $EMCC_VERSION" >&2
  exit 1
}

# ── Canary Vision core ─────────────────────────────────────────────────
# The Grove module/camera remains the staged sensor boundary. From the first
# SSCMA box onward this artifact is the production Canary Vision code:
# detection_pipeline.h + detect_config.cpp + presence_fsm.cpp +
# voxel_tracker.cpp. It is deliberately small and has no LVGL/crypto deps.
if [[ "$FLAVOR" == "vision" ]]; then
  VISION_PROJ="$FW/projects/canary-vision"
  VISION_CFG="$FW/configs/canary-vision/default"
  VISION_OBJ="$BUILD/vision"
  OUT_BASE="canary-vision-core"
  mkdir -p "$VISION_OBJ" "$DIST"

  VISION_INCLUDES=(
    -I "$EMU_DIR/shim"
    -I "$EMU_DIR/vision"
    -I "$VISION_PROJ/include"
    -I "$VISION_CFG"
  )
  VISION_FLAGS=(
    -std=gnu++17 -fno-exceptions -fno-rtti -O2 -Wall -Wextra
    -DARDUINO=10812
    -Wno-builtin-macro-redefined
    '-D__DATE__="emu"'
    '-D__TIME__="build"'
    -ffile-prefix-map="$REPO_ROOT"=/securacv
    -ffile-prefix-map="$EMU_DIR"=/securacv/canary-local/emulator
    "${VISION_INCLUDES[@]}"
  )
  VISION_SRCS=(
    "$VISION_PROJ/src/detect_config.cpp"
    "$VISION_PROJ/src/state/presence_fsm.cpp"
    "$VISION_PROJ/src/state/voxel_tracker.cpp"
    "$EMU_DIR/vision/vision_core_bindings.cpp"
    "$EMU_DIR/vision/vision_core_shim.cpp"
  )

  VISION_NEWEST_HDR=""
  while IFS= read -r -d '' h; do
    if [[ -z "$VISION_NEWEST_HDR" || "$h" -nt "$VISION_NEWEST_HDR" ]]; then
      VISION_NEWEST_HDR="$h"
    fi
  done < <(find "$VISION_PROJ/include" "$VISION_CFG" "$EMU_DIR/shim" \
                 "$EMU_DIR/vision" -name '*.h' -print0)

  VISION_OBJS=()
  for src in "${VISION_SRCS[@]}"; do
    rel="$(echo "$src" | sed 's|[/.]|_|g')"
    obj="$VISION_OBJ/$rel.o"
    VISION_OBJS+=("$obj")
    if [[ -f "$obj" && "$obj" -nt "$src" &&
          ( -z "$VISION_NEWEST_HDR" || "$obj" -nt "$VISION_NEWEST_HDR" ) ]]; then
      continue
    fi
    em++ -c "$src" "${VISION_FLAGS[@]}" -o "$obj"
  done

  echo "── linking $OUT_BASE from Canary Vision firmware ──"
  em++ "${VISION_OBJS[@]}" -O2 \
    --no-entry \
    -sALLOW_MEMORY_GROWTH \
    -sINITIAL_MEMORY=8388608 \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=createCanaryVisionCore \
    -sENVIRONMENT=web,node \
    -sSINGLE_FILE=1 \
    -sEXPORTED_RUNTIME_METHODS=cwrap,UTF8ToString \
    -o "$DIST/$OUT_BASE.js"

  FW_VERSION="$(sed -n 's/.*CANARY_FW_VERSION "\(.*\)".*/\1/p' "$VISION_PROJ/include/canary/version.h")"
  cat > "$DIST/$OUT_BASE.meta.json" <<EOF
{
  "flavor": "vision-core",
  "fw_version": "$FW_VERSION",
  "git": "$GIT_SHA",
  "source": "firmware/projects/canary-vision"
}
EOF
  ls -la "$DIST/$OUT_BASE.js"
  echo "OK: $OUT_BASE (real Canary Vision core $FW_VERSION @ $GIT_SHA)"
  exit 0
fi

# ── Canary WAP acoustic core ───────────────────────────────────────────
# The WAP's smoke/CO detector, exactly as it runs on-device: the real
# securacv_audio.cpp (DC-removed RMS, the 3.4 kHz alarm-band tone gate,
# envelope hysteresis, the NFPA-72 T3 / UL-2034 T4 cadence templates)
# behind a browser ABI. The mic-fed bench stages PCM through the same
# i2s_read seam the host test drives with synthesized audio. No LVGL, no
# crypto, no third-party — like the Vision core, it links against the
# proven audio stubs (the platform edges only: millis, Preferences, I2S).
if [[ "$FLAVOR" == "audio" ]]; then
  WAP_PROJ="$FW/projects/canary-wap/arduino/canary_wap"
  WAP_STUBS="$FW/projects/canary-wap/tests_host/stubs/audio"
  AUDIO_OBJ="$BUILD/audio"
  OUT_BASE="canary-wap-audio"
  mkdir -p "$AUDIO_OBJ" "$DIST"

  AUDIO_INCLUDES=(
    -I "$EMU_DIR/audio"
    -I "$WAP_STUBS"
    -I "$WAP_PROJ"
  )
  AUDIO_FLAGS=(
    -std=gnu++17 -fno-exceptions -fno-rtti -O2 -Wall -Wextra
    -DARDUINO=10812
    -Wno-builtin-macro-redefined
    '-D__DATE__="emu"'
    '-D__TIME__="build"'
    -ffile-prefix-map="$REPO_ROOT"=/securacv
    -ffile-prefix-map="$EMU_DIR"=/securacv/canary-local/emulator
    "${AUDIO_INCLUDES[@]}"
  )
  AUDIO_SRCS=(
    "$WAP_PROJ/securacv_audio.cpp"
    "$EMU_DIR/audio/audio_core_bindings.cpp"
  )

  AUDIO_NEWEST_HDR=""
  while IFS= read -r -d '' h; do
    if [[ -z "$AUDIO_NEWEST_HDR" || "$h" -nt "$AUDIO_NEWEST_HDR" ]]; then
      AUDIO_NEWEST_HDR="$h"
    fi
  done < <(find "$WAP_STUBS" "$EMU_DIR/audio" -name '*.h' -print0; \
           find "$WAP_PROJ" -maxdepth 1 -name 'securacv_audio.h' -o -maxdepth 1 -name 'log_level.h' -o -maxdepth 1 -name 'health_log.h' -print0)

  AUDIO_OBJS=()
  for src in "${AUDIO_SRCS[@]}"; do
    rel="$(echo "$src" | sed 's|[/.]|_|g')"
    obj="$AUDIO_OBJ/$rel.o"
    AUDIO_OBJS+=("$obj")
    if [[ -f "$obj" && "$obj" -nt "$src" &&
          ( -z "$AUDIO_NEWEST_HDR" || "$obj" -nt "$AUDIO_NEWEST_HDR" ) ]]; then
      continue
    fi
    em++ -c "$src" "${AUDIO_FLAGS[@]}" -o "$obj"
  done

  echo "── linking $OUT_BASE from Canary WAP acoustic firmware ──"
  em++ "${AUDIO_OBJS[@]}" -O2 \
    --no-entry \
    -sALLOW_MEMORY_GROWTH \
    -sINITIAL_MEMORY=8388608 \
    -sMODULARIZE=1 \
    -sEXPORT_NAME=createCanaryAudioCore \
    -sENVIRONMENT=web,node \
    -sSINGLE_FILE=1 \
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,HEAP16 \
    -o "$DIST/$OUT_BASE.js"

  FW_VERSION="$(sed -n 's/.*FIRMWARE_VERSION *= *"\(.*\)".*/\1/p' "$FW/projects/canary-wap/arduino/canary_wap/canary_wap.ino" | head -1)"
  cat > "$DIST/$OUT_BASE.meta.json" <<EOF
{
  "flavor": "wap-audio",
  "fw_version": "$FW_VERSION",
  "git": "$GIT_SHA",
  "source": "firmware/projects/canary-wap"
}
EOF
  ls -la "$DIST/$OUT_BASE.js"
  echo "OK: $OUT_BASE (real Canary WAP acoustic core $FW_VERSION @ $GIT_SHA)"
  exit 0
fi

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
elif [[ "$FLAVOR" == "nightstand" ]]; then
  # The portrait 172x320 face on the S3 stick's pin map (same glass as the
  # C6 sibling — one twin previews both boards).
  PINS_DIR="$FW/boards/waveshare-esp32s3-lcd147/pins"
  CFG_DIR="$FW/configs/canary-display/nightstand"
elif [[ "$FLAVOR" == "touch169" ]]; then
  # The 240x280 touch portrait: same nightstand app, CST816T glass — the
  # shim's pointer events ARE the touch panel here.
  PINS_DIR="$FW/boards/waveshare-esp32s3-touch-lcd169/pins"
  CFG_DIR="$FW/configs/canary-display/touch169"
elif [[ "$FLAVOR" == "amoled241" ]]; then
  # The 450x600 flagship AMOLED portrait: the same nightstand app on the
  # big emissive glass (CD_AMOLED_GLASS) — pointer events are the FT6336.
  PINS_DIR="$FW/boards/waveshare-esp32s3-amoled241/pins"
  CFG_DIR="$FW/configs/canary-display/amoled241"
else
  PINS_DIR="$FW/boards/waveshare-esp32s3-lcd43/pins"
  CFG_DIR="$FW/configs/canary-display/dash"
fi

OUT_BASE="canary-display-$FLAVOR"
case "$FLAVOR" in
  watch)      EXPORT_NAME="createCanaryEmuWatch" ;;
  nightstand) EXPORT_NAME="createCanaryEmuNightstand" ;;
  touch169)   EXPORT_NAME="createCanaryEmuTouch169" ;;
  amoled241)  EXPORT_NAME="createCanaryEmuAmoled241" ;;
  *)          EXPORT_NAME="createCanaryEmuDash" ;;
esac
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
  # The emulator is not real hardware, so the piezo-unpopulated gate that keeps
  # FEATURE_CHIME off in shipped firmware does not apply here: turn it ON so the
  # in-browser display actually VOICES the real Canary Voice grammar (boot
  # chirp, alerts, ack/page/mute) through the LEDC→onTone→Web Audio path. The
  # sound you hear is the real firmware driving frequency + duty per control
  # tick — it cannot drift from voice_score.h. Display flavors only (vision /
  # wap-audio exit above and never see this).
  -DFEATURE_CHIME=1
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
# The portrait flavors are the only ones whose UI calls into the shared
# color/look engine (the same LDF lesson the nightstand-s3 PlatformIO env
# documents) — adding these TUs to watch/dash would perturb their bytes
# for nothing, so they join per-flavor.
if [[ "$FLAVOR" == "nightstand" || "$FLAVOR" == "touch169" || \
      "$FLAVOR" == "amoled241" ]]; then
  FIRMWARE_SRCS+=(
    "$FW/common/color/color_engine.cpp"
    "$FW/common/color/look_engine.cpp"
    # The plumage TU joins its two siblings for the same reason: ui/look_state.cpp
    # holds a canary::color::Plumage, and portrait_ui.cpp / hal/ambient_led.cpp
    # call plumage_bands()/plumage_led(). All three call sites sit behind
    # CD_FLAVOR_NIGHTSTAND, which BOTH portrait configs define — so this belongs
    # in this block and not the watch/dash list, whose bytes it would move for a
    # TU they compile to nothing.
    "$FW/common/color/plumage.cpp"
    # The WS2812 beacon TU: emscripten-aware by design (the hardware write
    # is skipped in the browser) and double-gated, so it is real code on
    # the nightstand (HAS_RGBLED 1) and an empty TU on the touch169.
    "$PROJ/src/hal/ambient_led.cpp"
  )
fi
if [[ "$FLAVOR" == "touch169" || "$FLAVOR" == "amoled241" ]]; then
  FIRMWARE_SRCS+=(
    # The battery-backed RTC transport (FEATURE_RTC on these flavors):
    # main.cpp calls rtc_begin/rtc_loop under the same gate, so the TU
    # must link. On the shim's empty I2C bus (shim/Wire.h) the probe
    # NACKs and the firmware takes the same no-RTC path as hardware
    # with the pad unstuffed. Per-flavor so the other twins' bytes
    # don't move for a TU their configs compile to nothing anyway.
    "$PROJ/src/io/rtc.cpp"
  )
fi
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
  < <(find "$TP/lvgl/src" -name '*.c' -print0 | sort -z)

# ── Compile ─────────────────────────────────────────────────────────────
# Incremental correctness: an object is stale when ANY header in the
# include graph changed, not just its own source — a warm .build/ after a
# header-only edit (a new Character in the enum, a theme token) otherwise
# links stale inlined constants into dist, which CI's clean rebuild then
# refuses (drift-gate escape, PR #916). Third_party stays out of the
# check: it is pinned and immutable after clone.
NEWEST_HDR=""
while IFS= read -r -d '' h; do
  if [[ -z "$NEWEST_HDR" || "$h" -nt "$NEWEST_HDR" ]]; then NEWEST_HDR="$h"; fi
done < <(find "$PROJ/include" "$CFG_DIR" "$PINS_DIR" "$FW/common" \
             "$EMU_DIR/shim" "$EMU_DIR/src" -name '*.h' -print0)

OBJS=()
compile() {
  local src="$1" std_is_c="$2"
  local rel obj
  rel="$(echo "$src" | sed 's|[/.]|_|g')"
  obj="$OBJ/$rel.o"
  OBJS+=("$obj")
  if [[ -f "$obj" && "$obj" -nt "$src" &&
        ( -z "$NEWEST_HDR" || "$obj" -nt "$NEWEST_HDR" ) ]]; then return; fi
  if [[ "$std_is_c" == "c" ]]; then
    emcc -c "$src" "${CFLAGS[@]}" -o "$obj"
  else
    em++ -c "$src" "${CXXFLAGS[@]}" -o "$obj"
  fi
}

echo "── compiling LVGL ($LVGL_TAG) ──"
for f in "${LVGL_SRCS[@]}"; do compile "$f" c; done
echo "── compiling firmware (canary-display/$FLAVOR @ $GIT_SHA) ──"
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
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,HEAPU8 \
  -Wl,--wrap=time \
  -o "$DIST/$OUT_BASE.js"

# Build stamp for the page footer: which firmware these bytes are.
FW_VERSION="$(sed -n 's/.*CANARY_FW_VERSION "\(.*\)".*/\1/p' "$PROJ/include/canary/version.h")"
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
