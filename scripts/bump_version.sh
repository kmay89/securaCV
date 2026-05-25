#!/usr/bin/env bash
# Bump version across all four version locations simultaneously.
# Usage: ./scripts/bump_version.sh 0.6.0
set -euo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <new-version>" >&2
  echo "Example: $0 0.6.0" >&2
  exit 1
fi

NEW_VERSION="$1"

if ! echo "$NEW_VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
  echo "Error: version must be in semver format (e.g., 0.6.0)" >&2
  exit 1
fi

ROOT=$(git rev-parse --show-toplevel 2>/dev/null || pwd)

echo "Bumping version to ${NEW_VERSION}..."

# 1. Cargo.toml
sed -i "s/^version = \".*\"/version = \"${NEW_VERSION}\"/" "${ROOT}/Cargo.toml"
echo "  ✓ Cargo.toml"

# 2. HA integration manifest
sed -i "s/\"version\": \".*\"/\"version\": \"${NEW_VERSION}\"/" "${ROOT}/custom_components/securacv/manifest.json"
echo "  ✓ custom_components/securacv/manifest.json"

# 3. HA add-on config
sed -i "s/^version: \".*\"/version: \"${NEW_VERSION}\"/" "${ROOT}/privacy_witness_kernel/config.yaml"
echo "  ✓ privacy_witness_kernel/config.yaml"

# 4. Verify
echo ""
echo "Updated files:"
grep -n "^version" "${ROOT}/Cargo.toml" | head -1
grep -n "version" "${ROOT}/custom_components/securacv/manifest.json"
grep -n "^version" "${ROOT}/privacy_witness_kernel/config.yaml"
echo ""
echo "Don't forget to add a [${NEW_VERSION}] entry to CHANGELOG.md"
