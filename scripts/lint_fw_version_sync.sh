#!/usr/bin/env bash
# lint_fw_version_sync.sh — one firmware version, typed in six places.
#
# Every fw-v* release tags ONE version for every Canary firmware (docs/firmware_ota.md),
# but the string is spelled out in six headers/sketches:
#
#   firmware/canary/include/canary_config.h                        FIRMWARE_VERSION
#   firmware/projects/canary-display/include/canary/version.h      CANARY_FW_VERSION
#   firmware/projects/canary-display/arduino/canary_display/version.h  (staged copy)
#   firmware/projects/canary-sense/include/canary/version.h        CANARY_FW_VERSION
#   firmware/projects/canary-vision/include/canary/version.h       CANARY_FW_VERSION
#   firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino FIRMWARE_VERSION ("X.Y.Z-wap")
#
# Until this lint, drift between them was caught only at RELEASE time — by a
# `strings | grep` in firmware-release.yml, and for the display only as a
# warning that silently skipped its OTA manifest. scripts/lint_version_sync.sh
# (the kernel / integration / add-on lockstep) never looked at firmware. This
# runs on every PR and names the odd one out.
#
# Run from the repo root (lint.yml does):  bash scripts/lint_fw_version_sync.sh
set -euo pipefail
cd "$(dirname "$0")/.."

declare -a FILES=(
  "firmware/canary/include/canary_config.h|#define FIRMWARE_VERSION"
  "firmware/projects/canary-display/include/canary/version.h|#define CANARY_FW_VERSION"
  "firmware/projects/canary-display/arduino/canary_display/version.h|#define CANARY_FW_VERSION"
  "firmware/projects/canary-sense/include/canary/version.h|#define CANARY_FW_VERSION"
  "firmware/projects/canary-vision/include/canary/version.h|#define CANARY_FW_VERSION"
  "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino|FIRMWARE_VERSION *="
)

fail=0
canonical=""
printf '%-70s %s\n' "file" "version"
for entry in "${FILES[@]}"; do
  file="${entry%%|*}"
  pattern="${entry#*|}"
  if [ ! -f "$file" ]; then
    echo "::error::$file is missing — the version list in scripts/lint_fw_version_sync.sh is stale"
    fail=1
    continue
  fi
  raw="$(grep -E "^[[:space:]]*(static const char\* )?${pattern}" "$file" | head -1 | grep -oE '"[^"]+"' | head -1 | tr -d '"')"
  if [ -z "$raw" ]; then
    echo "::error::no version string matched /${pattern}/ in $file"
    fail=1
    continue
  fi
  # The WAP sketch appends a product suffix ("2.4.14-wap"); the train is the
  # part before the first dash.
  train="${raw%%-*}"
  printf '%-70s %s\n' "$file" "$raw"
  if [ -z "$canonical" ]; then
    canonical="$train"
  elif [ "$train" != "$canonical" ]; then
    echo "::error::$file says $raw but firmware/canary/include/canary_config.h says $canonical — every Canary firmware ships under ONE version (docs/firmware_ota.md)"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "lint_fw_version_sync: FAIL"
  exit 1
fi
echo "lint_fw_version_sync: OK — all six firmware version strings agree on $canonical"
