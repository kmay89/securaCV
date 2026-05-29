#!/usr/bin/env bash
# Render the Canary WAP enclosure STLs from the parametric OpenSCAD source.
# Emits two example presets (battery_full + compact_plain), base & lid each,
# plus the clip test coupon. Build your own config in the OpenSCAD Customizer.
# Requires: openscad (CLI).  Usage: ./render.sh
set -euo pipefail
cd "$(dirname "$0")"

SRC="canary_wap_enclosure.scad"
command -v openscad >/dev/null || { echo "openscad not found — install from https://openscad.org"; exit 1; }

# name:preset  (file prefix : OpenSCAD preset value)
for cfg in "battery:battery_full" "compact:compact_plain"; do
  name=${cfg%%:*}; preset=${cfg##*:}
  for part in base lid; do
    out="canary_wap_enclosure_${name}_${part}.stl"
    echo "Rendering $out  (preset=$preset) ..."
    openscad --export-format binstl -o "$out" \
        -D "preset=\"$preset\"" -D "part=\"$part\"" "$SRC"
  done
done

echo "Rendering canary_wap_enclosure_clip_coupon.stl ..."
openscad --export-format binstl -o "canary_wap_enclosure_clip_coupon.stl" -D 'part="coupon"' "$SRC"

echo "Done: 4 enclosure STLs + 1 clip coupon."
