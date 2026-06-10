#!/usr/bin/env bash
set -euo pipefail
#
# Guards against drift between the canonical acoustic-event module at
# firmware/canary/lib/securacv_audio/src/ and the vendored copy that lives
# next to the canary-wap Arduino sketch.
#
# Unlike the boot/CSI guards, the vendored .cpp intentionally diverges from
# the canonical source in four documented ways (see the header comment in
# the vendored file), so the comparison normalizes both sides first:
#
#   1. Everything up to and including `#include "securacv_audio.h"` is
#      stripped (covers the divergent header comment plus the vendored
#      copy's `#include "build_config.h"` + `#if FEATURE_ACOUSTIC_EVENTS`).
#   2. The vendored trailing `#endif  /* FEATURE_ACOUSTIC_EVENTS */` is
#      stripped.
#   3. `#include "health_log.h"` is rewritten to the canonical
#      `#include "securacv_witness.h"`.
#   4. The sketch's SCV_LOG_* / SCV_CAT_* log-enum vocabulary is rewritten
#      to the canonical LOG_LEVEL_* / LOG_CAT_* names.
#
# The .h files carry no divergences and must match byte-for-byte.
#
# Can be run from any directory (the repo root is resolved from the script's
# own location):
#   firmware/scripts/check_audio_sync.sh
#
# Exits non-zero on drift or missing files.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

CANONICAL_DIR="${REPO_ROOT}/firmware/canary/lib/securacv_audio/src"
STAGED_DIR="${REPO_ROOT}/firmware/projects/canary-wap/arduino/canary_wap"

drift=0

for name in securacv_audio.h securacv_audio.cpp; do
    for f in "$CANONICAL_DIR/$name" "$STAGED_DIR/$name"; do
        if [ ! -f "$f" ]; then
            echo "::error::Audio module file missing: $f"
            drift=1
        fi
    done
done
[ "$drift" -ne 0 ] && exit 1

# Header: byte-for-byte.
if ! cmp -s "$CANONICAL_DIR/securacv_audio.h" "$STAGED_DIR/securacv_audio.h"; then
    echo "::error::Drift detected: $STAGED_DIR/securacv_audio.h differs from canonical"
    echo "--- diff (canonical vs staged) ---"
    diff -u "$CANONICAL_DIR/securacv_audio.h" "$STAGED_DIR/securacv_audio.h" || true
    drift=1
fi

# Implementation: normalized comparison. POSIX sed/awk only — no GNU
# extensions (\b, \s) — so the script behaves the same under BSD sed on
# macOS as under GNU sed in CI. The SCV_ prefixes are unique strings, so
# no word-boundary anchor is needed.
normalize() {
    sed \
        -e '1,/^#include "securacv_audio.h"$/d' \
        -e '/^#endif  \/\* FEATURE_ACOUSTIC_EVENTS \*\/$/d' \
        -e 's/#include "health_log.h"/#include "securacv_witness.h"/' \
        -e 's/SCV_LOG_/LOG_LEVEL_/g' \
        -e 's/SCV_CAT_/LOG_CAT_/g' \
        "$1" \
    | awk '/^[[:space:]]*$/ {blank=blank $0 "\n"; next} {printf "%s", blank; blank=""; print}'
}

norm_canonical="$(mktemp)"
norm_staged="$(mktemp)"
trap 'rm -f "$norm_canonical" "$norm_staged"' EXIT

normalize "$CANONICAL_DIR/securacv_audio.cpp" > "$norm_canonical"
normalize "$STAGED_DIR/securacv_audio.cpp"    > "$norm_staged"

if ! cmp -s "$norm_canonical" "$norm_staged"; then
    echo "::error::Drift detected: $STAGED_DIR/securacv_audio.cpp diverges from canonical beyond the documented adaptations"
    echo "         Apply the change to BOTH copies (or re-vendor and re-apply the documented divergences)."
    echo "--- diff (canonical vs staged, both normalized) ---"
    diff -u "$norm_canonical" "$norm_staged" || true
    drift=1
fi

if [ "$drift" -ne 0 ]; then
    exit 1
fi

echo "OK: securacv_audio vendored copy matches canonical (modulo documented divergences)."
