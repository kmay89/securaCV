#!/usr/bin/env bash
# Archive, export, and upload the Witness Wall to App Store Connect.
#
# The one-button release from the tag: xcodebuild archives the signed tvOS app
# and xcrun altool uploads it. Apple's phased release then carries the build to
# every Apple TV — no artifact is hand-carried. See docs/tvos/AUTOPIPELINE.md.
#
# Reads (set by .github/workflows/tvos-release.yml):
#   APPLE_DEVELOPMENT_TEAM, APPLE_API_ISSUER, APPLE_API_KEY, APPLE_API_KEY_PATH,
#   EXPORT_METHOD (debugging | release-testing | app-store-connect)
#
# Honest stub: the WitnessWall/ Xcode project does not exist yet (see
# tvos/README.md — "design + pipeline scaffold, not yet built"). Fail loudly and
# specifically rather than pretending to upload.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -d WitnessWall ] || ! ls WitnessWall/*.xcodeproj >/dev/null 2>&1; then
  echo "::error::tvos/WitnessWall/ Xcode project does not exist yet."
  echo "The Witness Wall is still design-stage. Build the SwiftUI tvOS app, then"
  echo "this script archives, signs, and uploads it. See tvos/README.md and"
  echo "docs/tvos/README.md for the intended surfaces."
  exit 1
fi

: "${APPLE_API_KEY:?App Store Connect key ID missing}"
: "${APPLE_API_ISSUER:?App Store Connect issuer ID missing}"
: "${APPLE_DEVELOPMENT_TEAM:?Apple team ID missing}"
export_method="${EXPORT_METHOD:-app-store-connect}"

echo "Archiving WitnessWall (tvOS)…"
xcodebuild -project WitnessWall/WitnessWall.xcodeproj \
  -scheme WitnessWall \
  -destination 'generic/platform=tvOS' \
  -archivePath build/WitnessWall.xcarchive \
  DEVELOPMENT_TEAM="$APPLE_DEVELOPMENT_TEAM" \
  archive

echo "Exporting ($export_method)…"
xcodebuild -exportArchive \
  -archivePath build/WitnessWall.xcarchive \
  -exportOptionsPlist WitnessWall/ExportOptions-"$export_method".plist \
  -exportPath build/export

if [ "$export_method" = "app-store-connect" ]; then
  echo "Uploading to App Store Connect…"
  xcrun altool --upload-app --type tvos \
    --file build/export/*.ipa \
    --apiKey "$APPLE_API_KEY" \
    --apiIssuer "$APPLE_API_ISSUER"
  echo "Uploaded. Apple's phased release will carry it to every Apple TV."
else
  echo "Exported .ipa to build/export/ (smoke run — not uploaded)."
fi
