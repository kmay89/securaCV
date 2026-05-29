#!/usr/bin/env bash
# Render the Canary WAP enclosure STLs from the parametric OpenSCAD source.
# Produces both variants (battery bay + compact USB-only), base and lid each.
# Requires: openscad (CLI).  Usage: ./render.sh
set -euo pipefail
cd "$(dirname "$0")"

SRC="canary_wap_enclosure.scad"
command -v openscad >/dev/null || { echo "openscad not found — install from https://openscad.org"; exit 1; }

for variant in battery compact; do
  for part in base lid; do
    out="canary_wap_enclosure_${variant}_${part}.stl"
    echo "Rendering $out ..."
    openscad --export-format binstl -o "$out" \
        -D "variant=\"$variant\"" -D "part=\"$part\"" "$SRC"
  done
done
echo "Done: 4 STLs (battery/compact × base/lid)."
