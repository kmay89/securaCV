#!/usr/bin/env bash
# dev_flash.sh — the bench loop: build a PlatformIO env, put it on the board,
# watch it boot. One command from edit to running firmware.
#
#   firmware/scripts/dev_flash.sh <env> [options]
#
#   firmware/scripts/dev_flash.sh canary-display-nightstand-c6 -m
#   firmware/scripts/dev_flash.sh canary-display-dash7 -p /dev/ttyACM0
#   firmware/scripts/dev_flash.sh canary-display-nightstand-s3 -f -n
#
# Options:
#   -p, --port <dev>   serial port (default: PlatformIO auto-detect)
#   -m, --monitor      open the serial monitor after a successful upload
#   -f, --factory      also merge a factory image (bootloader + partition
#                      table + app) — the file the browser flasher's
#                      Advanced → "local .bin file" installs, and what
#                      `espflash write-bin 0x0 <file>` writes on a blank board
#   -n, --no-upload    build (and optionally merge) only — no board needed
#
# What it knows so you don't have to:
#   - which project an env lives in (firmware/flavors.json `build_envs`)
#   - which envs need an isolated PLATFORMIO_CORE_DIR (`isolated_core_envs` —
#     the pioarduino core-3.x platform cannot share a core dir with
#     espressif32, exactly as firmware.yml isolates them in CI)
#   - that a missing secrets/secrets.h just means dev placeholders (real
#     credentials live in NVS; export WIFI_SSID/WIFI_PASS/MQTT_HOST first to
#     bake bench values in)
#
# This is the DEV loop: nothing here signs anything. Shipping is a release
# button (docs/RELEASE_BUTTONS.md), and devices only self-update from signed
# manifests. A locally flashed board simply polls its channel like any other.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FLAVORS="$REPO/firmware/flavors.json"

die() { echo "dev_flash: $*" >&2; exit 1; }

ENV_NAME="${1:-}"
[ -n "$ENV_NAME" ] && [[ "$ENV_NAME" != -* ]] || {
  sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 2
}
shift

PORT="" MONITOR=0 FACTORY=0 UPLOAD=1
while [ $# -gt 0 ]; do
  case "$1" in
    -p|--port) PORT="${2:?-p needs a device}"; shift 2 ;;
    -m|--monitor) MONITOR=1; shift ;;
    -f|--factory) FACTORY=1; shift ;;
    -n|--no-upload) UPLOAD=0; shift ;;
    *) die "unknown option '$1'" ;;
  esac
done

# env → project dir + isolation, from the same manifest CI builds from.
read -r PROJ_DIR ISOLATED < <(python3 - "$ENV_NAME" "$FLAVORS" <<'PY'
import json, sys
env, flavors_path = sys.argv[1], sys.argv[2]
for f in json.load(open(flavors_path)):
    if env in f.get("build_envs", []):
        print(f["dir"], 1 if env in f.get("isolated_core_envs", []) else 0)
        break
else:
    sys.exit(f"dev_flash: env '{env}' is not in any firmware/flavors.json build_envs")
PY
) || exit 1

cd "$REPO/$PROJ_DIR"

# Dev placeholders unless the caller exports real bench credentials — the
# same NVS-backed scheme the release builds document: a generic image
# defers to whatever the device's NVS already holds.
if [ ! -f secrets/secrets.h ]; then
  mkdir -p secrets
  cat > secrets/secrets.h <<HEADER
#pragma once
// dev_flash.sh dev build. Real credentials live in device NVS.
#define WIFI_SSID "${WIFI_SSID:-dev-placeholder}"
#define WIFI_PASS "${WIFI_PASS:-dev-placeholder}"
#define MQTT_HOST "${MQTT_HOST:-127.0.0.1}"
#define MQTT_PORT ${MQTT_PORT:-1883}
#define MQTT_USER ${MQTT_USER:-nullptr}
#define MQTT_PASS ${MQTT_PASS:-nullptr}
HEADER
  echo "dev_flash: wrote $PROJ_DIR/secrets/secrets.h (dev placeholders — gitignored)"
fi

if [ "$ISOLATED" = 1 ]; then
  export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.pio-core-$ENV_NAME}"
  echo "dev_flash: isolated core dir $PLATFORMIO_CORE_DIR (pioarduino platform)"
fi

TARGETS=()
if [ "$UPLOAD" = 1 ]; then
  TARGETS+=(-t upload)
  [ -n "$PORT" ] && TARGETS+=(--upload-port "$PORT")
fi
pio run -e "$ENV_NAME" "${TARGETS[@]}"

if [ "$FACTORY" = 1 ]; then
  BUILD_DIR=".pio/build/$ENV_NAME"
  # board_build.mcu is authoritative for the merge tool's --chip.
  CHIP="$(pio project config --json-output 2>/dev/null \
    | python3 -c "import json,sys; d=json.load(sys.stdin); print(dict(dict(d)['env:$ENV_NAME']).get('board_build.mcu','esp32s3'))" \
    || echo esp32s3)"
  OUT="$BUILD_DIR/${ENV_NAME}-factory.bin"
  python3 "$REPO/firmware/scripts/make_factory.py" \
    --chip "$CHIP" --build-dir "$BUILD_DIR" --out "$OUT"
  echo "dev_flash: factory image → $PROJ_DIR/$OUT"
  echo "dev_flash: install it via the browser flasher (Advanced → local .bin file)"
  echo "dev_flash: or on a blank board: espflash write-bin 0x0 '$OUT'"
fi

if [ "$MONITOR" = 1 ]; then
  MON=()
  [ -n "$PORT" ] && MON+=(--port "$PORT")
  exec pio device monitor -e "$ENV_NAME" "${MON[@]}"
fi
