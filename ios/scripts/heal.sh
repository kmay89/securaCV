#!/usr/bin/env bash
# heal.sh — regenerate the Xcode project from project.yml and prove it still
# builds against the CURRENT firmware contracts. This is the iOS analogue of the
# desktop app's anti-rot pattern (build.rs re-embeds the one canonical catalog
# every build so no committed copy drifts): here, the project is regenerated and
# the app is rebuilt so it can't silently rot between releases.
#
# Usage:
#   scripts/heal.sh generate     # just (re)generate the .xcodeproj
#   scripts/heal.sh build        # generate + build + test on a simulator
#   scripts/heal.sh check        # generate + fail if the tree ended up dirty
set -euo pipefail
cd "$(dirname "$0")/.."

ensure_xcodegen() {
  if ! command -v xcodegen >/dev/null 2>&1; then
    echo "[heal] installing xcodegen…"
    brew install xcodegen
  fi
}

generate() {
  ensure_xcodegen
  eval "$(scripts/stamp_build.sh)"        # export SECURACV_BUILD_REV / FW_TRAIN
  xcodegen generate
  echo "[heal] project regenerated at build rev ${SECURACV_BUILD_REV}"
}

build() {
  generate
  xcodebuild \
    -project SecuraCV.xcodeproj \
    -scheme SecuraCV \
    -destination 'platform=iOS Simulator,name=iPhone 15,OS=latest' \
    -configuration Debug \
    SECURACV_BUILD_REV="${SECURACV_BUILD_REV:-dev}" \
    SECURACV_FW_TRAIN="${SECURACV_FW_TRAIN:-0.x}" \
    CODE_SIGNING_ALLOWED=NO \
    clean build test
}

check() {
  generate
  # The .xcodeproj is gitignored, so a clean tree proves nothing tracked drifted.
  if [ -n "$(git status --porcelain -- . ':!*.xcodeproj')" ]; then
    echo "[heal] tracked files drifted after regenerate:"; git status --porcelain -- .
    exit 1
  fi
  echo "[heal] clean — nothing drifted."
}

case "${1:-generate}" in
  generate) generate ;;
  build) build ;;
  check) check ;;
  *) echo "usage: heal.sh [generate|build|check]"; exit 2 ;;
esac
