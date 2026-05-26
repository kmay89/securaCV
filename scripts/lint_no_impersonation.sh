#!/usr/bin/env bash
set -euo pipefail
# securaCV non-impersonation lint
#
# Enforces the non-impersonation contract in
# spec/beacon_cap_gateway_v0.md §4 and docs/research/harm_reduction_prior_art.md
# §2. Fails the build if any of the forbidden phrases or forbidden audio
# frequency combinations appear in firmware sources, UI strings, or audio
# pattern tables.
#
# Run from repository root.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# Roots we scan.
SCAN_DIRS=(
  "firmware/projects/canary-wap"
  "firmware/projects/canary-vision"
  "firmware/common"
  "canary-vision"
  "homeassistant"
  "custom_components"
)

# Phrases forbidden in user-visible strings. We allow them in documentation
# (docs/, spec/, README.md, SECURITY-*.md, THREAT_MODEL.md) where they're
# referenced as prior-art names, but never in firmware or UI strings.
FORBIDDEN_PHRASES=(
  'Wireless Emergency Alert'
  'Presidential Alert'
  'AMBER Alert'
  'AMBER ALERT'
  'Silver Alert'
  'Civil Emergency Message'
  'This is a test of the Emergency Alert System'
  'IPAWS'
)

# Frequency pairs that, when found in a single source file in close proximity,
# indicate the WEA two-tone (853 + 960 Hz). We check for both numbers within a
# 50-line window of each other. We deliberately do not flag the bare presence
# of either frequency alone — they're common audio values.
WEA_FREQ_A='853'
WEA_FREQ_B='960'

EXIT_CODE=0

echo "[lint] scanning for forbidden phrases…"
for phrase in "${FORBIDDEN_PHRASES[@]}"; do
  for dir in "${SCAN_DIRS[@]}"; do
    if [ -d "$dir" ]; then
      hits="$(grep -rEn --include='*.h' --include='*.hpp' --include='*.c' --include='*.cpp' \
                       --include='*.ino' --include='*.js' --include='*.ts' --include='*.jsx' \
                       --include='*.tsx' --include='*.html' --include='*.css' --include='*.yaml' \
                       --include='*.yml' --include='*.json' --include='*.py' \
                       -- "$phrase" "$dir" 2>/dev/null || true)"
      if [ -n "$hits" ]; then
        echo "[lint][FAIL] forbidden phrase '$phrase' found in:"
        echo "$hits"
        EXIT_CODE=1
      fi
    fi
  done
done

echo "[lint] scanning for WEA-style two-tone frequency pairs (${WEA_FREQ_A} + ${WEA_FREQ_B})…"
for dir in "${SCAN_DIRS[@]}"; do
  if [ -d "$dir" ]; then
    while IFS= read -r f; do
      # Look for both numbers in the same file.
      has_a="$(grep -nE "\\b${WEA_FREQ_A}\\b" "$f" 2>/dev/null || true)"
      has_b="$(grep -nE "\\b${WEA_FREQ_B}\\b" "$f" 2>/dev/null || true)"
      if [ -n "$has_a" ] && [ -n "$has_b" ]; then
        line_a="$(echo "$has_a" | head -1 | cut -d: -f1)"
        line_b="$(echo "$has_b" | head -1 | cut -d: -f1)"
        diff=$((line_a > line_b ? line_a - line_b : line_b - line_a))
        if [ "$diff" -lt 50 ]; then
          echo "[lint][FAIL] WEA frequency pair ${WEA_FREQ_A} + ${WEA_FREQ_B} found within 50 lines in: $f"
          echo "    line $line_a and line $line_b"
          EXIT_CODE=1
        fi
      fi
    done < <(find "$dir" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cpp' -o -name '*.ino' \))
  fi
done

# Forbid pure red as a primary alert color in any styled output (we allow it
# as a syntax-highlighting color in build tooling but not in firmware/UI).
echo "[lint] scanning for pure red as a primary alert color…"
for dir in "${SCAN_DIRS[@]}"; do
  if [ -d "$dir" ]; then
    hits="$(grep -rEn --include='*.css' --include='*.html' --include='*.h' --include='*.cpp' \
                     --include='*.ino' --include='*.js' --include='*.ts' --include='*.jsx' --include='*.tsx' \
                     -- '#FF0000\|#ff0000\|#F00\|#f00' "$dir" 2>/dev/null || true)"
    if [ -n "$hits" ]; then
      # Allow if accompanied by 'lint:allow-red' on the same line.
      filtered="$(echo "$hits" | grep -v 'lint:allow-red' || true)"
      if [ -n "$filtered" ]; then
        echo "[lint][WARN] pure red color found (use #E67E22 amber or yellow instead):"
        echo "$filtered"
        # Warning, not failure — some places legitimately use red (e.g. test
        # error rendering). Promote to FAIL if the file is alert/chirp/beacon
        # related:
        critical_hits="$(echo "$filtered" | grep -E 'alert|chirp|beacon' || true)"
        if [ -n "$critical_hits" ]; then
          echo "[lint][FAIL] pure red color in alert/chirp/beacon context:"
          echo "$critical_hits"
          EXIT_CODE=1
        fi
      fi
    fi
  fi
done

if [ $EXIT_CODE -eq 0 ]; then
  echo "[lint] non-impersonation lint passed."
else
  echo "[lint] non-impersonation lint FAILED."
fi
exit $EXIT_CODE
