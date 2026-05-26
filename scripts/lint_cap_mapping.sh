#!/usr/bin/env bash
set -euo pipefail
# securaCV CAP-mapping conformance lint
#
# Verifies that every Beacon template ID declared in
# firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.h
# has a corresponding entry in the CAP mapping table in
# spec/beacon_cap_gateway_v0.md.
#
# Spec rule (spec/beacon_cap_gateway_v0.md §2.5): every Beacon template
# MUST map to a CAP (category, responseType, urgency, severity, certainty)
# tuple, and the spec table is normative — adding/removing templates
# requires a spec-table change in the same PR.


REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

HEADER="firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.h"
SPEC="spec/beacon_cap_gateway_v0.md"

if [ ! -f "$HEADER" ]; then
  echo "[cap-lint] $HEADER not found"
  exit 1
fi
if [ ! -f "$SPEC" ]; then
  echo "[cap-lint] $SPEC not found"
  exit 1
fi

# Extract template enum names declared in the header. Match the
# `BCN_*_* = 0xNN,` pattern in the BeaconTemplate enum.
templates=$(grep -E '^\s*BCN_[A-Z_]+\s*=\s*0x[0-9A-Fa-f]+' "$HEADER" \
            | sed -E 's/^\s*(BCN_[A-Z_]+)\s*=.*$/\1/' \
            | grep -v BCN_TPL_INVALID)

EXIT_CODE=0
missing=""
for tpl in $templates; do
  # Spec table uses backticked names. Look for the template name anywhere in
  # the spec.
  if ! grep -q "${tpl}" "$SPEC"; then
    echo "[cap-lint][FAIL] template '$tpl' has no CAP mapping in $SPEC"
    missing+="$tpl "
    EXIT_CODE=1
  fi
done

if [ $EXIT_CODE -eq 0 ]; then
  echo "[cap-lint] all $(echo "$templates" | wc -w | tr -d ' ') Beacon templates have CAP mappings."
else
  echo "[cap-lint] missing: $missing"
fi

exit $EXIT_CODE
