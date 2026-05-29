#!/usr/bin/env bash
# Render the Canary WAP enclosure STLs from the parametric OpenSCAD source.
# Requires: openscad (CLI).  Usage: ./render.sh
set -euo pipefail
cd "$(dirname "$0")"

SRC="canary_wap_enclosure.scad"
command -v openscad >/dev/null || { echo "openscad not found — install from https://openscad.org"; exit 1; }

for part in base lid; do
  out="canary_wap_enclosure_${part}.stl"
  echo "Rendering $out ..."
  openscad --export-format binstl -o "$out" -D "part=\"$part\"" "$SRC"
done
echo "Done: canary_wap_enclosure_base.stl, canary_wap_enclosure_lid.stl"
