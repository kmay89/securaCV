#!/usr/bin/env bash
# Render the Canary WAP enclosure STLs from the parametric OpenSCAD source.
# Emits three example presets (battery_full, compact_plain, battery_weather),
# base & lid each (+ TPU gasket for the weather preset), the clip test coupon,
# and preview PNGs. Build your own config in the OpenSCAD Customizer.
# Requires: openscad (CLI). Override the binary with OPENSCAD=/path/to/openscad.
# Usage: ./render.sh [--no-png]
set -euo pipefail
cd "$(dirname "$0")"

SRC="canary_wap_enclosure.scad"
OPENSCAD=${OPENSCAD:-openscad}
command -v "$OPENSCAD" >/dev/null || { echo "openscad not found — install from https://openscad.org"; exit 1; }

stl() { # stl <output> <-D defines...>
  local out=$1; shift
  echo "Rendering $out ..."
  "$OPENSCAD" --export-format binstl -o "$out" "$@" "$SRC"
}

png() { # png <output> <-D defines...> — best-effort: previews are nice-to-have
  local out=$1; shift
  echo "Rendering $out ..."
  "$OPENSCAD" --render -o "$out" --imgsize 1200,800 --autocenter --viewall \
      --colorscheme Tomorrow "$@" "$SRC" 2>/dev/null \
    || xvfb-run -a "$OPENSCAD" --render -o "$out" --imgsize 1200,800 --autocenter --viewall \
          --colorscheme Tomorrow "$@" "$SRC" 2>/dev/null \
    || echo "WARNING: could not render $out (no GL context?) — STLs are unaffected"
}

# name:preset  (file prefix : OpenSCAD preset value)
for cfg in "battery:battery_full" "compact:compact_plain" "weather:battery_weather"; do
  name=${cfg%%:*}; preset=${cfg##*:}
  for part in base lid; do
    stl "canary_wap_enclosure_${name}_${part}.stl" -D "preset=\"$preset\"" -D "part=\"$part\""
  done
done

# TPU gasket ring for the weather preset
stl "canary_wap_enclosure_weather_gasket.stl" -D 'preset="battery_weather"' -D 'part="gasket"'

# snap-clip test coupon — print this FIRST to tune the clip fit
stl "canary_wap_enclosure_clip_coupon.stl" -D 'part="coupon"'

if [[ "${1:-}" != "--no-png" ]]; then
  png "preview_all.png"     -D 'preset="battery_full"'    -D 'part="all"'
  png "preview_compact.png" -D 'preset="compact_plain"'   -D 'part="all"'
  png "preview_weather.png" -D 'preset="battery_weather"' -D 'part="all"'
  png "preview_coupon.png"  -D 'part="coupon"'
fi

echo "Done: 6 enclosure STLs + 1 gasket + 1 clip coupon."
