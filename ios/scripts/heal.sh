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
  # -derivedDataPath is pinned (git-ignored build/) so the embed proof below
  # can find the products without guessing at DerivedData hashes.
  #
  # CODE_SIGNING_ALLOWED=NO and SECURACV_NO_CLOUDKIT are ONE decision, so they
  # are made on one line. A build without signing carries no entitlements, and
  # an app with no iCloud entitlement cannot construct a CKContainer — it does
  # not fail, it dies: `CKContainer.default()` raises an ObjC exception,
  # `CKContainer(identifier:)` traps in __allocating_init. Neither is catchable
  # from Swift, so the app aborted at launch and every test "failed" for
  # reasons that had nothing to do with iCloud (RELEASE_LESSONS (ab)).
  #
  # The flag compiles those paths out of exactly the builds that could never
  # run them. Signed builds — device, TestFlight, App Store — never see it, so
  # this cannot mask a real CloudKit fault where CloudKit actually works.
  xcodebuild \
    -project SecuraCV.xcodeproj \
    -scheme SecuraCV \
    -destination "$(sim_destination)" \
    -configuration Debug \
    -derivedDataPath build \
    SECURACV_BUILD_REV="${SECURACV_BUILD_REV:-dev}" \
    SECURACV_FW_TRAIN="${SECURACV_FW_TRAIN:-0.x}" \
    CODE_SIGNING_ALLOWED=NO \
    SWIFT_ACTIVE_COMPILATION_CONDITIONS='$(inherited) SECURACV_NO_CLOUDKIT' \
    clean build test

  # …AND THEN TYPE-CHECK THE CODE THAT FLAG JUST DELETED.
  #
  # The reasoning above is about RUNTIME: an unsigned app cannot construct a
  # CKContainer without dying. But `#if` does not hide code from the runtime,
  # it hides it from the COMPILER — so every CloudKit path in this app
  # (HouseholdShare, CloudSync, AwayPush's cloud branches) was never compiled
  # by CI at all. A signed release build was its first and only compiler, which
  # is a terrible place to learn you have a type error: it fails after the
  # archive step, minutes into a publish, on the one build a human can't run.
  #
  # That is not hypothetical. `db.record(for: zone.share)` — passing a
  # CKRecord.Reference where a CKRecord.ID belongs — sat in main through two
  # green PRs and only surfaced when the App Store release tried to archive it.
  #
  # So: build the same scheme WITHOUT the flag, and never launch it. `build`
  # rather than `build test` is the whole trick — compiling and linking an
  # unsigned app is fine, it just cannot RUN, and nothing here runs it. The
  # CKContainer trap needs a launch it never gets.
  echo "── type-checking the CloudKit paths the simulator build compiles out ──"
  xcodebuild \
    -project SecuraCV.xcodeproj \
    -scheme SecuraCV \
    -destination "$(sim_destination)" \
    -configuration Debug \
    -derivedDataPath build-cloudkit \
    SECURACV_BUILD_REV="${SECURACV_BUILD_REV:-dev}" \
    SECURACV_FW_TRAIN="${SECURACV_FW_TRAIN:-0.x}" \
    CODE_SIGNING_ALLOWED=NO \
    build

  # Prove the watch app actually EMBEDDED (Embed Watch Content), not merely
  # compiled — a watch app missing from the bundle ships an iPhone-only app
  # with nothing red anywhere (RELEASE_LESSONS Principle 4: verify a bundled
  # payload where it's bundled). The release workflow re-proves this on the
  # exported .ipa; this catches it at PR time, on the simulator products.
  local app="build/Build/Products/Debug-iphonesimulator/SecuraCV.app"
  local watch_app="$app/Watch/SecuraCVWatch.app"
  local complications="$watch_app/PlugIns/SecuraCVWatchWidgets.appex"
  for bundle in "$app" "$watch_app" "$complications"; do
    if [ ! -d "$bundle" ]; then
      echo "[heal] ERROR: expected bundle missing after build: $bundle" >&2
      exit 1
    fi
  done
  echo "[heal] watch app + complications embedded in SecuraCV.app"
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
