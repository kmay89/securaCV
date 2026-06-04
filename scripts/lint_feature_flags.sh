#!/usr/bin/env bash
set -euo pipefail
# securaCV feature-flag hygiene lint
#
# Enforces the conventions documented in docs/feature-flags.md so that flags
# can't silently rot or over-advertise. Three checks, all cheap and grep-based:
#
#   A) Orphaned Cargo features  — every key under [features] in Cargo.toml must
#      be referenced somewhere in the Rust sources / tests / Cargo.toml itself
#      (cfg(feature=...), required-features, or as a dependency of another
#      feature). Catches features that linger after their code is deleted.
#
#   B) "Never advertise unbuilt" — no FUTURE_* transport may appear in
#      ALL_TRANSPORTS in custom_components/securacv/const.py. Locks in the
#      flag-report F-07 fix so a device can never advertise a transport no
#      firmware can report on.
#
#   C) Registry completeness — every Cargo [features] key must be listed in the
#      central registry docs/feature-flags.md, so the index can't drift behind
#      the build. (The registry is the single source of truth per layer.)
#
# This script is the executable companion to docs/feature-flags.md. Run it
# locally before pushing; CI runs it via .github/workflows/validate.yml.

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

CARGO_TOML="Cargo.toml"
CONST_PY="custom_components/securacv/const.py"
REGISTRY="docs/feature-flags.md"

EXIT_CODE=0

fail() {
  echo "[flag-lint][FAIL] $*"
  EXIT_CODE=1
}

for f in "$CARGO_TOML" "$CONST_PY" "$REGISTRY"; do
  if [ ! -f "$f" ]; then
    echo "[flag-lint] required file not found: $f"
    exit 1
  fi
done

# Extract the feature names declared in Cargo.toml's [features] table: every
# `name = [...]` line between the [features] header and the next [section].
features="$(awk '
  /^\[features\]/    { in_feat = 1; next }
  /^\[/              { in_feat = 0 }
  in_feat && /^[A-Za-z0-9_-]+[[:space:]]*=/ {
    sub(/[[:space:]]*=.*/, "", $0); print $0
  }
' "$CARGO_TOML")"

if [ -z "$features" ]; then
  fail "no [features] parsed from $CARGO_TOML (parser or table changed?)"
fi

# Check A + C for each declared feature.
feat_count=0
for feat in $features; do
  feat_count=$((feat_count + 1))

  # A) referenced in code/tests, or by Cargo.toml itself (required-features /
  #    feature deps). Search source trees plus Cargo.toml, excluding the bare
  #    `name =` declaration line so a feature can't "reference itself".
  refs="$(grep -RInE "\"${feat}\"|\b${feat}\b" src tests Cargo.toml 2>/dev/null \
            | grep -vE "^${CARGO_TOML}:[0-9]+:[[:space:]]*${feat}[[:space:]]*=" || true)"
  if [ -z "$refs" ]; then
    fail "Cargo feature '${feat}' is declared but never referenced (orphaned). Remove it or wire it up."
  fi

  # C) listed in the central registry.
  if ! grep -qF "${feat}" "$REGISTRY"; then
    fail "Cargo feature '${feat}' is missing from the registry ${REGISTRY}. Add a row for it."
  fi
done

# B) FUTURE_* transports must not leak into ALL_TRANSPORTS.
#    Pull the identifiers inside the ALL_TRANSPORTS = [ ... ] list and flag any
#    whose name matches a FUTURE_* transport constant.
all_block="$(awk '
  /ALL_TRANSPORTS[[:space:]]*=/ { grab = 1 }
  grab { print }
  grab && /\]/ { exit }
' "$CONST_PY")"

# The FUTURE list is explicitly declared; read its members and assert none are
# present in the ALL_TRANSPORTS block.
future_block="$(awk '
  /FUTURE_TRANSPORTS[[:space:]]*=/ { grab = 1 }
  grab { print }
  grab && /\]/ { exit }
' "$CONST_PY")"

future_members="$(echo "$future_block" | grep -oE 'TRANSPORT_[A-Z_]+' | grep -v 'FUTURE_TRANSPORTS' | sort -u)"

for member in $future_members; do
  if echo "$all_block" | grep -qE "\b${member}\b"; then
    fail "future transport '${member}' appears in ALL_TRANSPORTS (${CONST_PY}). Devices must never advertise an unbuilt transport — keep it in FUTURE_TRANSPORTS until wired end-to-end."
  fi
done

if [ "$EXIT_CODE" -eq 0 ]; then
  echo "[flag-lint] OK: ${feat_count} Cargo features referenced + registered; no FUTURE_* transport advertised."
fi

exit "$EXIT_CODE"
