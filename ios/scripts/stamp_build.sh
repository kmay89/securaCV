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
FW_TRAIN="$(sed -n 's/^version *= *"\([0-9.]*\)".*/\1/p' ../Cargo.toml 2>/dev/null | head -1 || true)"
FW_TRAIN="${FW_TRAIN:-0.x}"

echo "SECURACV_BUILD_REV=${REV}"
echo "SECURACV_FW_TRAIN=${FW_TRAIN}"

if [ -n "${GITHUB_ENV:-}" ]; then
  {
    echo "SECURACV_BUILD_REV=${REV}"
    echo "SECURACV_FW_TRAIN=${FW_TRAIN}"
  } >> "$GITHUB_ENV"
fi
