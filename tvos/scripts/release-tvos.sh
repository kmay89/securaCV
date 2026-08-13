#!/usr/bin/env bash
# Archive, export, and (only when asked) upload the Witness Wall.
#
# The one-button release from the tag: xcodebuild archives the signed tvOS app
# and `xcrun altool` uploads it. Apple's phased release then carries the build
# to every Apple TV — no artifact is hand-carried. See docs/tvos/AUTOPIPELINE.md.
#
# Reads (set by .github/workflows/tvos-release.yml):
#   APPLE_DEVELOPMENT_TEAM, APPLE_API_ISSUER, APPLE_API_KEY, APPLE_API_KEY_PATH
#   EXPORT_METHOD     debugging | release-testing | app-store-connect
#   PUBLISH           true = upload to App Store Connect; anything else = build only
#   EXPECTED_VERSION  optional: the version the tag claims (e.g. from tvos-v1.4.0)
#
# Release lessons this file is written against (.github/RELEASE_LESSONS.md):
#   P1  every payload copy dereferences symlinks (`cp -RL`)
#   P3  there is a build-only path, and it is the DEFAULT — publishing is opt-in
#   P4  every bundled input and every produced artifact is verified in the step
#       that makes it, so a failure names itself instead of aborting opaquely
#   2026-07-24 (upload 5xx) — every release upload retries with backoff, and
#       nothing is reported as published until it is verified as uploaded
set -euo pipefail

cd "$(dirname "$0")/.."

PROJECT_DIR="WitnessWall"
SCHEME="WitnessWall"
BUILD_DIR="build"
ARCHIVE="$BUILD_DIR/WitnessWall.xcarchive"
EXPORT_DIR="$BUILD_DIR/export"
export_method="${EXPORT_METHOD:-app-store-connect}"
publish="${PUBLISH:-false}"

case "$export_method" in
  debugging|release-testing|app-store-connect) ;;
  *)
    echo "::error::EXPORT_METHOD must be debugging, release-testing, or app-store-connect (got '$export_method')."
    exit 1
    ;;
esac

if [ ! -f "$PROJECT_DIR/project.yml" ]; then
  echo "::error::tvos/WitnessWall/project.yml is missing — there is no app to build."
  exit 1
fi

# ── 1. The app's own inputs, verified before anything expensive runs ─────────
# P4: a missing core is a one-line failure here, not a linker error in the
# middle of an Xcode build.
CORE_LIB="$BUILD_DIR/lib/appletvos/libsecuracv_witness_core.a"
if [ ! -s "$CORE_LIB" ]; then
  echo "::error::the witness core is not staged at $CORE_LIB."
  echo "Run: bash scripts/build-witness-core.sh   (TVOS_SLICES=device is enough for a release)"
  exit 1
fi
echo "witness core: $CORE_LIB ($(wc -c < "$CORE_LIB" | tr -d ' ') bytes)"

# P4 again: Apple TV rejects an archive with no app icon / top-shelf art, and
# `altool` reports it as an opaque validation failure minutes later. Catch it
# here, by name, in one line.
ICONS="$PROJECT_DIR/Support/Assets.xcassets/App Icon & Top Shelf Image.brandassets"
if [ ! -d "$ICONS" ]; then
  echo "::error::the tvOS asset catalog is missing at $ICONS."
  echo "App Store Connect will reject an archive with no app icon or top-shelf image."
  echo "Regenerate it with: python3 scripts/make_app_icon.py"
  exit 1
fi
icon_count="$(find "$ICONS" -name '*.png' | wc -l | tr -d ' ')"
if [ "$icon_count" -lt 10 ]; then
  echo "::error::the asset catalog has only $icon_count PNGs — the layered icon stacks are incomplete."
  echo "Regenerate it with: python3 scripts/make_app_icon.py"
  exit 1
fi
echo "asset catalog: $icon_count images"

# ── 2. Regenerate the Xcode project (never committed, so it cannot rot) ──────
if ! command -v xcodegen >/dev/null 2>&1; then
  echo "::error::xcodegen is not installed. The Xcode project is generated from"
  echo "WitnessWall/project.yml on every build — install it with: brew install xcodegen"
  exit 1
fi
echo "── regenerating the Xcode project ──"
( cd "$PROJECT_DIR" && xcodegen generate )

# ── 3. The tag and the binary must claim the same version ───────────────────
# The 2026-07-24 firmware lesson generalized: a release that ships under a
# version it doesn't carry is wrong in exactly the way nobody notices until a
# user reports it.
marketing_version="$(sed -n 's/^ *MARKETING_VERSION: *"\{0,1\}\([0-9][0-9.]*\)"\{0,1\} *$/\1/p' "$PROJECT_DIR/project.yml" | head -1)"
if [ -z "$marketing_version" ]; then
  echo "::error::could not read MARKETING_VERSION from $PROJECT_DIR/project.yml."
  exit 1
fi
echo "app version: $marketing_version"
if [ -n "${EXPECTED_VERSION:-}" ] && [ "$EXPECTED_VERSION" != "$marketing_version" ]; then
  echo "::error::the tag says $EXPECTED_VERSION but WitnessWall/project.yml says $marketing_version."
  echo "Bump MARKETING_VERSION in project.yml to match the tag (or tag the version the app actually is)."
  exit 1
fi

# ── 4. Build identity, stamped on the command line (no generated file) ──────
eval "$(bash scripts/stamp_build.sh)"
echo "build rev: ${SECURACV_BUILD_REV:-dev} (firmware train ${SECURACV_FW_TRAIN:-0.x})"

# ── 5. Signing ──────────────────────────────────────────────────────────────
# A fresh CI runner has no Apple accounts or provisioning profiles, so signing
# authenticates with the App Store Connect API key — hand xcodebuild the same
# .p8 the workflow materialized, and let it manage signing assets.
: "${APPLE_API_KEY:?App Store Connect key ID missing}"
: "${APPLE_API_ISSUER:?App Store Connect issuer ID missing}"
: "${APPLE_API_KEY_PATH:?App Store Connect key path missing (the workflow materializes it)}"
: "${APPLE_DEVELOPMENT_TEAM:?Apple team ID missing}"
if [ ! -s "$APPLE_API_KEY_PATH" ]; then
  echo "::error::APPLE_API_KEY_PATH points at $APPLE_API_KEY_PATH, which is missing or empty."
  exit 1
fi

asc_auth=(
  -allowProvisioningUpdates
  -authenticationKeyPath "$APPLE_API_KEY_PATH"
  -authenticationKeyID "$APPLE_API_KEY"
  -authenticationKeyIssuerID "$APPLE_API_ISSUER"
)

# Store builds archive signed for DISTRIBUTION, not development. Automatic
# signing's default is to archive with an "Apple Development" identity, and
# minting a *tvOS App Development* profile requires at least one registered
# Apple TV on the account — a thing this account has never needed (the iPhone
# archives fine because iPhones are registered; nobody ever pairs an Apple TV
# with Xcode just to ship). Apple's refusal surfaces as the baffling pair
# "Authentication failed: bearer token" + "No profiles for '<bundle id>'".
# App Store profiles need no devices, and this account demonstrably mints
# them — the iPhone export does it on every release. The debugging export
# keeps development signing, because that is what a debugging export is.
# RELEASE_LESSONS 2026-08-13.
sign_overrides=()
if [ "$export_method" != "debugging" ]; then
  sign_overrides=(CODE_SIGN_IDENTITY="Apple Distribution")
fi

# ── 6. Archive ──────────────────────────────────────────────────────────────
# THE BUILD NUMBER, WHICH project.yml CANNOT SUPPLY.
#
# `CURRENT_PROJECT_VERSION: "1"` is committed, so without an override every
# upload carries build 1 — and App Store Connect identifies a build by
# (marketing version, build number), so it can accept exactly ONE upload per
# version. There is no "build 2 of 0.1.0": respinning the same version after a
# transient upload failure, or with a one-line fix, is impossible without a
# marketing version bump. The iPhone app hit this first (RELEASE_LESSONS
# 2026-08-09); tvOS carries the identical setting and gets the identical fix.
#
# BUILD_NUMBER is `<run_number>.<run_attempt>`, not a commit count and not
# either half alone. The number has to count ATTEMPTS, not content: the respin
# case is "same commit, upload again", so a commit count emits the same value
# twice and is rejected identically. And run_number alone is not enough because
# GitHub's "Re-run jobs" reuses the run — run_number stays put and only
# run_attempt moves, which is exactly the retry an operator reaches for when an
# upload was accepted but a later step failed. Together they only ever
# increase, and a dotted CFBundleVersion is compared component by component.
# Falls back to 1 for a local run, where uniqueness is nobody's problem.
: "${BUILD_NUMBER:=1}"
echo "── archiving WitnessWall (tvOS), build $BUILD_NUMBER ──"
rm -rf "$ARCHIVE" "$EXPORT_DIR"
xcodebuild -project "$PROJECT_DIR/WitnessWall.xcodeproj" \
  -scheme "$SCHEME" \
  -configuration Release \
  -destination 'generic/platform=tvOS' \
  -archivePath "$ARCHIVE" \
  "${asc_auth[@]}" \
  ${sign_overrides[@]+"${sign_overrides[@]}"} \
  DEVELOPMENT_TEAM="$APPLE_DEVELOPMENT_TEAM" \
  CURRENT_PROJECT_VERSION="$BUILD_NUMBER" \
  SECURACV_BUILD_REV="${SECURACV_BUILD_REV:-dev}" \
  SECURACV_FW_TRAIN="${SECURACV_FW_TRAIN:-0.x}" \
  archive

# P4: "xcodebuild succeeded" is not the same as "there is an archive".
if [ ! -d "$ARCHIVE" ]; then
  echo "::error::the archive step reported success but $ARCHIVE does not exist."
  exit 1
fi

# ── 7. Export ───────────────────────────────────────────────────────────────
# Built with PlistBuddy rather than a heredoc or a committed plist: one fewer
# file to rot, and the export method can't disagree with what was requested.
echo "── exporting ($export_method) ──"
PLIST="$BUILD_DIR/ExportOptions.plist"
mkdir -p "$BUILD_DIR"
rm -f "$PLIST"
printf '%s' '{}' > "$PLIST"
/usr/libexec/PlistBuddy -c "Add :method string $export_method" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :teamID string $APPLE_DEVELOPMENT_TEAM" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :signingStyle string automatic" "$PLIST"
# Upload the symbols so a crash on a customer's Apple TV symbolicates. Only
# meaningful for a store build, and harmless otherwise.
/usr/libexec/PlistBuddy -c "Add :uploadSymbols bool true" "$PLIST"

xcodebuild -exportArchive \
  -archivePath "$ARCHIVE" \
  -exportOptionsPlist "$PLIST" \
  -exportPath "$EXPORT_DIR" \
  "${asc_auth[@]}"

# P4 again: find the .ipa and prove it, rather than globbing it into a command
# that would fail confusingly if the glob matched nothing.
ipa="$(find "$EXPORT_DIR" -name '*.ipa' -type f | head -1)"
if [ -z "$ipa" ] || [ ! -s "$ipa" ]; then
  echo "::error::the export step reported success but produced no .ipa in $EXPORT_DIR."
  find "$EXPORT_DIR" -maxdepth 2 -print || true
  exit 1
fi
echo "exported: $ipa ($(wc -c < "$ipa" | tr -d ' ') bytes)"

# ── 8. Publish (opt-in only) ────────────────────────────────────────────────
# P3: build-only is the default. A run that was not asked to publish stops
# here with a proven .ipa — that is the smoke test the lessons ask for.
if [ "$publish" != "true" ]; then
  echo "Build-only run: exported to $EXPORT_DIR and NOT uploaded (PUBLISH=$publish)."
  exit 0
fi
if [ "$export_method" != "app-store-connect" ]; then
  echo "::error::PUBLISH=true needs EXPORT_METHOD=app-store-connect (got '$export_method')."
  echo "A debugging/release-testing export is not something App Store Connect accepts."
  exit 1
fi

# Validate + upload with retries. Shared with every other Apple target so the
# retry policy can't drift between them (CLAUDE.md: apply a release fix to every
# app target, not just the one that broke).
bash "$(dirname "$0")/../../.github/scripts/asc_publish.sh" "$ipa" appletvos

echo "Uploaded $marketing_version. Apple's phased release will carry it to every Apple TV."
