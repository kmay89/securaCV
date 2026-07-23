#!/bin/bash
# Host test for firmware/common/core/feature_sanity.h — asserts the #error
# checks fire (and don't fire) for representative flag combinations.
set -u
if ! command -v g++ >/dev/null 2>&1; then
  echo "Error: g++ is required to run the feature-sanity tests." >&2
  exit 1
fi
H=firmware/common/core/feature_sanity.h
pass=0 fail=0

expect() { # expect <ok|err> <desc> <defines...>
  local want=$1 desc=$2; shift 2
  local flags=(); for d in "$@"; do flags+=("-D$d"); done
  if echo "#include \"$H\"" | g++ -x c++ -fsyntax-only -I. "${flags[@]}" - 2>/dev/null; then
    got=ok
  else
    got=err
  fi
  if [ "$got" = "$want" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL[$want!=$got]: $desc"; fi
}

# No flags defined at all — must be a no-op (incremental adoption).
expect ok  "no flags defined"
# Feature on, capability present.
expect ok  "SD on, slot present"        FEATURE_SD_STORAGE=1 HAS_SD_CARD=1
# Feature on, capability absent — must fail.
expect err "SD on, no slot"             FEATURE_SD_STORAGE=1 HAS_SD_CARD=0
# Feature off, capability absent — fine.
expect ok  "SD off, no slot"            FEATURE_SD_STORAGE=0 HAS_SD_CARD=0
# Feature on, capability undefined — skipped (adoption-safe).
expect ok  "SD on, HAS undefined"       FEATURE_SD_STORAGE=1
# Camera needs camera AND psram.
expect err "camera on, no camera"       FEATURE_CAMERA_PEEK=1 HAS_CAMERA=0 HAS_PSRAM=1
expect err "camera on, no psram"        FEATURE_CAMERA_PEEK=1 HAS_CAMERA=1 HAS_PSRAM=0
expect ok  "camera on, both present"    FEATURE_CAMERA_PEEK=1 HAS_CAMERA=1 HAS_PSRAM=1
# WiFi family.
expect err "MQTT on, no wifi"           FEATURE_MQTT=1 HAS_WIFI=0
expect ok  "MQTT on, wifi"              FEATURE_MQTT=1 HAS_WIFI=1
expect err "mesh on, no wifi"           FEATURE_MESH_NETWORK=1 HAS_WIFI=0
expect err "AP on, no wifi"             FEATURE_WIFI_AP=1 HAS_WIFI=0
expect ok  "AP off on wifi-less board"  FEATURE_WIFI_AP=0 HAS_WIFI=0
# BLE, GNSS, tamper, vision.
expect err "BLE on, no radio"           FEATURE_BLE=1 HAS_BLE=0
expect err "bluetooth on, no radio"     FEATURE_BLUETOOTH=1 HAS_BLE=0
expect err "GNSS on, no uart"           FEATURE_GNSS=1 HAS_GNSS_UART=0
expect err "tamper on, no input"        FEATURE_TAMPER_GPIO=1 HAS_TAMPER_INPUT=0
expect err "vision on, no host"         FEATURE_VISION_AI=1 HAS_VISION_AI=0
expect ok  "vision on, host"            FEATURE_VISION_AI=1 HAS_VISION_AI=1
# Display family.
expect err "display on, no panel"       FEATURE_DISPLAY=1 HAS_DISPLAY=0
expect err "touch on, no controller"    FEATURE_TOUCH=1 HAS_TOUCH=0
expect err "dim on, on/off backlight"   FEATURE_BACKLIGHT_DIM=1 HAS_BACKLIGHT_PWM=0
expect ok  "dim off, on/off backlight"  FEATURE_BACKLIGHT_DIM=0 HAS_BACKLIGHT_PWM=0
# On-glass modes (demo/debug/arcade): glass + touch, nothing more.
expect err "demo on, no panel"          FEATURE_DEMO_MODE=1 HAS_DISPLAY=0 HAS_TOUCH=1
expect err "debug on, no touch"         FEATURE_DEBUG_MODE=1 HAS_DISPLAY=1 HAS_TOUCH=0
expect ok  "arcade on, glass + touch"   FEATURE_ARCADE=1 HAS_DISPLAY=1 HAS_TOUCH=1
expect ok  "modes on, HAS undefined"    FEATURE_DEMO_MODE=1
expect ok  "modes off, no panel"        FEATURE_DEBUG_MODE=0 HAS_DISPLAY=0

# Real-world matrix: every shipped Vision host board's pins.h against the
# REAL vision default flavor config (not simulated -D flags — the same pair
# the production include ladder assembles via canary/config.h + pins.h).
for b in xiao-esp32s3 xiao-esp32c3 esp32-c3; do
  if printf '#include "firmware/boards/%s/pins/pins.h"\n#include "firmware/configs/canary-vision/default/config.h"\n#include "%s"\n' "$b" "$H" \
    | g++ -x c++ -fsyntax-only -I. - 2>/dev/null; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL: vision default config on $b"
  fi
done

# Negative proof: the real vision flavor config on a host WITHOUT Vision AI
# capability must refuse to build.
if printf '#include "firmware/configs/canary-vision/default/config.h"\n#include "%s"\n' "$H" \
  | g++ -x c++ -fsyntax-only -I. -DHAS_VISION_AI=0 -DHAS_WIFI=1 - 2>/dev/null; then
  fail=$((fail+1)); echo "FAIL: vision config on non-vision host should not compile"
else
  pass=$((pass+1))
fi

# Real-world matrix: both display flavors' shipped configs against their
# boards' pin maps (these configs use live FEATURE_DISPLAY/TOUCH/DIM flags).
for pair in "xiao-esp32s3-round watch" "waveshare-esp32s3-lcd43 dash"; do
  set -- $pair
  if printf '#include "firmware/boards/%s/pins/pins.h"\n#include "firmware/configs/canary-display/%s/config.h"\n#include "%s"\n' "$1" "$2" "$H" \
    | g++ -x c++ -fsyntax-only -I. - 2>/dev/null; then
    pass=$((pass+1))
  else
    fail=$((fail+1)); echo "FAIL: display $2 config on $1"
  fi
done

echo "sanity header tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ] && echo "ALL FEATURE_SANITY TESTS PASSED"
exit $fail
