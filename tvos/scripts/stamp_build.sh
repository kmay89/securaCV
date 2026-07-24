#!/usr/bin/env bash
# stamp_build.sh — compute the build identity for the Witness Wall's
# About/Health panel ("never a guess, never stale").
#
# Byte-for-byte the same contract as ios/scripts/stamp_build.sh and the desktop
# app's build.rs SECURACV_BUILD_REV stamp: print `KEY=VALUE` lines, and append
# them to $GITHUB_ENV under Actions so the workflow can pass them to xcodebuild.
# No generated file, so there is nothing to drift or forget to gitignore.
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
