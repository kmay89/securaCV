#!/usr/bin/env bash
# Validate and upload a signed .ipa to App Store Connect — shared by EVERY
# Apple target (iPhone/iPad today, Apple TV today, whatever comes next).
#
# It lives here rather than in one app's scripts/ because of the rule in
# CLAUDE.md: when a release/packaging bug is fixed, apply it to every app
# target, not just the one that broke. The retry policy below IS such a fix
# (the 2026-07-24 upload-5xx entry in .github/RELEASE_LESSONS.md); having two
# copies of it would guarantee they drift.
#
# Usage:
#   bash .github/scripts/asc_publish.sh <ipa-path> <platform>
#     platform: ios | appletvos     (altool's own names; Apple TV is NOT "tvos")
#
# Reads: APPLE_API_KEY (key id), APPLE_API_ISSUER
set -euo pipefail

ipa="${1:?usage: asc_publish.sh <ipa-path> <ios|appletvos>}"
platform="${2:?usage: asc_publish.sh <ipa-path> <ios|appletvos>}"

case "$platform" in
  ios|appletvos) ;;
  *)
    echo "::error::platform must be 'ios' or 'appletvos' (got '$platform')."
    exit 1
    ;;
esac

: "${APPLE_API_KEY:?App Store Connect key ID missing}"
: "${APPLE_API_ISSUER:?App Store Connect issuer ID missing}"

# altool takes only a key ID, never a key path — it searches ./private_keys,
# ~/private_keys, ~/.private_keys, and ~/.appstoreconnect/private_keys, and
# dies with Cocoa error -43 "Failed to load AuthKey file" when the .p8 lives
# anywhere else (like the $RUNNER_TEMP/keys file the workflows materialize).
# Stage the materialized key into a directory altool actually checks.
if [ -n "${APPLE_API_KEY_PATH:-}" ] && [ -s "$APPLE_API_KEY_PATH" ]; then
  mkdir -p "$HOME/.appstoreconnect/private_keys"
  cp "$APPLE_API_KEY_PATH" "$HOME/.appstoreconnect/private_keys/AuthKey_${APPLE_API_KEY}.p8"
fi

if [ ! -s "$ipa" ]; then
  echo "::error::no .ipa to upload at $ipa"
  exit 1
fi
echo "uploading $ipa ($(wc -c < "$ipa" | tr -d ' ') bytes) as $platform"

# Validate first. `altool --validate-app` catches the whole class of rejections
# (missing icons, bad entitlements, a duplicate build number) in seconds,
# instead of as a silent processing failure minutes after CI goes green.
echo "── validating with App Store Connect ──"
if ! xcrun altool --validate-app --type "$platform" \
      --file "$ipa" \
      --apiKey "$APPLE_API_KEY" \
      --apiIssuer "$APPLE_API_ISSUER"; then
  echo "::error::App Store Connect rejected the build during validation — not uploading."
  exit 1
fi

# A transient 5xx must not fail a publish. Retry with backoff, and treat
# "already uploaded" as success so a retry after a half-acknowledged upload
# converges instead of failing a release that actually landed.
echo "── uploading to App Store Connect ──"
log="$(mktemp)"
delay=15
for attempt in 1 2 3 4; do
  # ${attempt} braced: macOS bash 3.2 under `set -u` swallows a following
  # multibyte char (the … here) into the variable name and dies "unbound".
  echo "upload attempt ${attempt}…"
  if xcrun altool --upload-app --type "$platform" \
        --file "$ipa" \
        --apiKey "$APPLE_API_KEY" \
        --apiIssuer "$APPLE_API_ISSUER" 2>&1 | tee "$log"; then
    echo "Uploaded. Apple's phased release will carry it to users."
    exit 0
  fi
  if grep -qiE 'already been uploaded|redundant binary upload|bundle version.*already exists' "$log"; then
    echo "App Store Connect already has this build — treating as uploaded."
    exit 0
  fi
  if [ "$attempt" -lt 4 ]; then
    echo "::warning::upload attempt $attempt failed; retrying in ${delay}s."
    sleep "$delay"
    delay=$(( delay * 2 ))
  fi
done

echo "::error::upload failed after 4 attempts. The build was archived, signed, and"
echo "validated — only the upload failed, so re-running this job is safe."
exit 1
