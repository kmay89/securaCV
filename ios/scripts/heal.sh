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
  # A signing team in the env persists into git-ignored local.yml so the
  # regenerated project keeps it even when opened from Xcode's GUI (which has
  # no shell env).
  if [ -n "${APPLE_DEVELOPMENT_TEAM:-}" ] && [ ! -f local.yml ]; then
    printf 'settings:\n  base:\n    APPLE_DEVELOPMENT_TEAM: "%s"\n' \
      "$APPLE_DEVELOPMENT_TEAM" > local.yml
    echo "[heal] wrote local.yml with your signing team (git-ignored)"
  fi
  # XcodeGen has no optional include, so project.yml never references
  # local.yml (a bare `xcodegen generate` must always work — the release
  # workflow runs exactly that). When local.yml exists, merge it AFTER the
  # main spec via a wrapper so its settings win.
  if [ -f local.yml ]; then
    printf 'include:\n  - project.yml\n  - local.yml\n' > .local-spec.yml
    xcodegen generate --spec .local-spec.yml
  else
    xcodegen generate
  fi
  echo "[heal] project regenerated at build rev ${SECURACV_BUILD_REV}"
}

# Hard-coding a phone model rots yearly as Xcode retires simulators; pick the
# newest numbered iPhone simulator actually installed (override: SECURACV_SIM).
sim_destination() {
  local name="${SECURACV_SIM:-}"
  if [ -z "$name" ]; then
    name="$(xcrun simctl list devices available 2>/dev/null \
      | sed -n 's/^[[:space:]]*\(iPhone [0-9][0-9][^(]*\)(.*/\1/p' \
      | sed 's/[[:space:]]*$//' | sort -uV | tail -1)"
  fi
  echo "platform=iOS Simulator,name=${name:-iPhone 15}"
}

build() {
  generate
  xcodebuild \
    -project SecuraCV.xcodeproj \
    -scheme SecuraCV \
    -destination "$(sim_destination)" \
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
