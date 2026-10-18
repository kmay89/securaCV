#!/usr/bin/env bash
# Bump the kernel / Home Assistant integration / add-on version in all four
# places it is written, together:
#   Cargo.toml                                (the witness-kernel package)
#   Cargo.lock                                (its witness-kernel entry)
#   custom_components/securacv/manifest.json  (the integration)
#   privacy_witness_kernel/config.yaml        (the add-on)
# then proves it with scripts/lint_version_sync.sh, the CI gate that checks
# the same four.
# Usage: ./scripts/bump_version.sh 0.6.0
#
# Portable on purpose: the edits are one python3 program, not `sed -i` (GNU
# and BSD/macOS sed disagree on -i, and GNU's `0,/re/` address does not exist
# on macOS at all). Every file is checked before any is written, so a file
# whose version line cannot be found leaves the whole tree untouched rather
# than half-bumped.
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

python3 - "$ROOT" "$NEW_VERSION" <<'PY'
import re
import sys
from pathlib import Path

root, new = Path(sys.argv[1]), sys.argv[2]
# (file, pattern whose FIRST match is the version line, replacement). Each
# pattern is the reading scripts/lint_version_sync.sh does for that file.
EDITS = [
    ("Cargo.toml", r'^version = "[^"]*"[ \t]*$', f'version = "{new}"'),
    ("Cargo.lock", r'^(name = "witness-kernel"\nversion = )"[^"]*"$', rf'\g<1>"{new}"'),
    ("custom_components/securacv/manifest.json", r'"version": "[^"]*"', f'"version": "{new}"'),
    ("privacy_witness_kernel/config.yaml", r'^version: "[^"]*"[ \t]*$', f'version: "{new}"'),
]

planned = []
for rel, pattern, repl in EDITS:
    path = root / rel
    text = path.read_text(encoding="utf-8")
    updated, count = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if count != 1:
        sys.exit(f"Error: no version line found in {rel} — nothing was changed")
    planned.append((path, rel, updated))

for path, rel, updated in planned:
    path.write_text(updated, encoding="utf-8")
    print(f"  ✓ {rel}")
PY

echo ""
bash "${ROOT}/scripts/lint_version_sync.sh"
echo ""
echo "Next:"
echo "  • add a [${NEW_VERSION}] entry to CHANGELOG.md"
echo "  • regenerate what embeds the integration/add-on version, in CI's order:"
echo "      python3 canary-local/tools/gen_homeassistant.py && \\"
echo "      python3 canary-local/tools/gen_hub_image.py && \\"
echo "      python3 canary-local/tools/gen_hub_seed.py && \\"
echo "      python3 canary-local/tools/gen_hub_provision_bundle.py && \\"
echo "      python3 canary-local/tools/gen_start.py"
echo "    (canary-local.yml's drift steps name any that moved)"
