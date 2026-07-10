#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical PlatformIO display tree
# (firmware/projects/canary-display/{src,include}) and the committed, flat
# Arduino parity sketch (firmware/projects/canary-display/arduino/canary_display).
#
# Unlike the CSI/OTA guards, the Arduino sketch is TRANSFORMED (flattened
# includes, renamed flavor config), not a byte copy — so this guard
# regenerates it and checks that the committed result is unchanged. If someone
# edited src/ or include/ without running `./setup.sh regen`, or hand-edited
# the generated sketch, git shows a diff and this fails.
#
# Run from the repo root:
#   firmware/scripts/check_display_arduino_sync.sh

PROJ="firmware/projects/canary-display"
SKETCH="$PROJ/arduino/canary_display"

if [ ! -x "$PROJ/setup.sh" ]; then
  echo "::error::$PROJ/setup.sh not found or not executable"
  exit 1
fi

# Regenerate the flavor-agnostic committed sources (no flavor/secrets staging).
( cd "$PROJ" && ./setup.sh regen >/dev/null )

# The per-flavor / secret files are git-ignored, so `git status` on the sketch
# reflects only the committed, flavor-agnostic set.
if ! git diff --quiet -- "$SKETCH" || \
   [ -n "$(git ls-files --others --exclude-standard -- "$SKETCH")" ]; then
  echo "::error::Arduino sketch is out of sync with the canonical PlatformIO tree."
  echo "         Regenerate and commit it:"
  echo "           cd $PROJ && ./setup.sh regen && git add arduino/canary_display"
  echo "--- git diff ($SKETCH) ---"
  git --no-pager diff -- "$SKETCH" || true
  git ls-files --others --exclude-standard -- "$SKETCH" | sed 's/^/  untracked: /' || true
  exit 1
fi

echo "✓ Arduino display sketch is in sync with the canonical PlatformIO tree."
