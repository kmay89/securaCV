#!/usr/bin/env bash
# stamp_build.sh — compute the build identity for the About/Health panel
# ("never a guess, never stale", the same contract as the desktop app's
# build.rs SECURACV_BUILD_REV stamp).
#
# Prints `KEY=VALUE` lines for the two build settings and, when running in
# GitHub Actions, appends them to $GITHUB_ENV so the workflow can pass them to
# xcodebuild (e.g. `xcodebuild ... SECURACV_BUILD_REV=$SECURACV_BUILD_REV`).
# No file is generated, so there is nothing to drift or forget to ignore.
set -euo pipefail
cd "$(dirname "$0")/.."

REV="$(git rev-parse --short HEAD 2>/dev/null || echo dev)"
# The firmware TRAIN, read from the firmware's own version define — not the
# root Cargo.toml, which is the witness-kernel crate (0.7.x) and was what the
# About panel showed under "firmware train" until this line was fixed.
# scripts/lint_fw_version_sync.sh keeps every firmware copy of this string equal.
FW_TRAIN="$(sed -n 's/^#define FIRMWARE_VERSION *"\([0-9.]*\)".*/\1/p' ../firmware/canary/include/canary_config.h 2>/dev/null | head -1 || true)"
FW_TRAIN="${FW_TRAIN:-0.x}"

echo "SECURACV_BUILD_REV=${REV}"
echo "SECURACV_FW_TRAIN=${FW_TRAIN}"

if [ -n "${GITHUB_ENV:-}" ]; then
  {
    echo "SECURACV_BUILD_REV=${REV}"
    echo "SECURACV_FW_TRAIN=${FW_TRAIN}"
  } >> "$GITHUB_ENV"
fi
