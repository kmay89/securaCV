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

# ── 6. Archive ──────────────────────────────────────────────────────────────
echo "── archiving WitnessWall (tvOS) ──"
rm -rf "$ARCHIVE" "$EXPORT_DIR"
xcodebuild -project "$PROJECT_DIR/WitnessWall.xcodeproj" \
  -scheme "$SCHEME" \
  -configuration Release \
  -destination 'generic/platform=tvOS' \
  -archivePath "$ARCHIVE" \
  "${asc_auth[@]}" \
  DEVELOPMENT_TEAM="$APPLE_DEVELOPMENT_TEAM" \
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

# Validate before uploading. `altool --validate-app` catches the whole class of
# rejections (missing icons, bad entitlements, a duplicate build number) in
# seconds, instead of as a silent processing failure minutes after CI goes green.
echo "── validating with App Store Connect ──"
if ! xcrun altool --validate-app --type appletvos \
      --file "$ipa" \
      --apiKey "$APPLE_API_KEY" \
      --apiIssuer "$APPLE_API_ISSUER"; then
  echo "::error::App Store Connect rejected the build during validation — not uploading."
  exit 1
fi

# The 2026-07-24 upload lesson, applied here: a transient 5xx must not fail a
# publish. Retry with backoff, and treat "already uploaded" as success, so a
# retry after a half-acknowledged upload converges instead of failing a release
# that actually landed.
echo "── uploading to App Store Connect ──"
upload_log="$BUILD_DIR/altool-upload.log"
uploaded=false
delay=15
for attempt in 1 2 3 4; do
  echo "upload attempt $attempt…"
  if xcrun altool --upload-app --type appletvos \
        --file "$ipa" \
        --apiKey "$APPLE_API_KEY" \
        --apiIssuer "$APPLE_API_ISSUER" 2>&1 | tee "$upload_log"; then
    uploaded=true
    break
  fi
  if grep -qiE 'already been uploaded|redundant binary upload|bundle version.*already exists' "$upload_log"; then
    echo "App Store Connect already has this build — treating as uploaded."
    uploaded=true
    break
  fi
  if [ "$attempt" -lt 4 ]; then
    echo "::warning::upload attempt $attempt failed; retrying in ${delay}s."
    sleep "$delay"
    delay=$(( delay * 2 ))
  fi
done

if [ "$uploaded" != "true" ]; then
  echo "::error::upload failed after 4 attempts. The build was archived, signed, and"
  echo "validated — only the upload failed, so re-running this job is safe."
  exit 1
fi

echo "Uploaded $marketing_version. Apple's phased release will carry it to every Apple TV."
