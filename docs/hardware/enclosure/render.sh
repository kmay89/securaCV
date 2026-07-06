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

# --- Canary Vision enclosure (camera unit + GoPro-compatible hinge) ---------
# host=xiao (stacked XIAO, recommended) gets both presets; the Grove-cabled
# DevKit layout ships as indoor only (weather via the Customizer/CLI).
VSRC="canary_vision_enclosure.scad"
vstl() { local out=$1; shift; echo "Rendering $out ..."
  "$OPENSCAD" --export-format binstl -o "$out" "$@" "$VSRC"; }

for cfg in "xiao_indoor:xiao:vision_indoor" "xiao_weather:xiao:vision_weather" "devkit_indoor:devkit:vision_indoor"; do
  name=${cfg%%:*}; rest=${cfg#*:}; h=${rest%%:*}; preset=${rest##*:}
  for part in back front; do
    vstl "canary_vision_enclosure_${name}_${part}.stl" \
        -D "host=\"$h\"" -D "preset=\"$preset\"" -D "part=\"$part\""
  done
done
vstl "canary_vision_enclosure_xiao_weather_gasket.stl" \
    -D 'host="xiao"' -D 'preset="vision_weather"' -D 'part="gasket"'
vstl "canary_vision_enclosure_bracket.stl" -D 'part="bracket"'
vstl "canary_vision_enclosure_knob.stl"    -D 'part="knob"'

# --- Canary Vision DOORBELL (Wyze/Ring form factor; sealed by default) ------
DSRC="canary_vision_doorbell.scad"
dstl() { local out=$1; shift; echo "Rendering $out ..."
  "$OPENSCAD" --export-format binstl -o "$out" "$@" "$DSRC"; }
for part in body face plate gasket; do
  dstl "canary_vision_doorbell_${part}.stl" -D "part=\"$part\""
done
dstl "canary_vision_doorbell_plate_wedge15.stl" -D 'plate_wedge=15' -D 'part="plate"'

# --- Canary Sense (MR60BHA2 radar + stacked XIAO C6; RADOME front) ----------
SSRC="canary_sense_enclosure.scad"
sstl() { local out=$1; shift; echo "Rendering $out ..."
  "$OPENSCAD" --export-format binstl -o "$out" "$@" "$SSRC"; }
sstl "canary_sense_back.stl"  -D 'part="back"'
sstl "canary_sense_front.stl" -D 'part="front"'
# (bracket/knob are identical to the Vision case parts — reuse those STLs)

# --- Thermal / outdoor kit (WAP): solar shield + desiccant tray -------------
stl "canary_wap_enclosure_weather_shield.stl" -D 'preset="battery_weather"' -D 'part="shield"'
stl "canary_wap_enclosure_tray.stl" -D 'part="tray"' 

# --- IN DEVELOPMENT designs: rendered + mesh-checked in CI, STLs NOT committed
# (dev_*.stl is gitignored; print from the .scad while these mature)
devstl() { local out=$1 src=$2; shift 2; echo "Rendering $out (dev) ..."
  "$OPENSCAD" --export-format binstl -o "$out" "$@" "$src"; }
devstl dev_watch_drum.stl    canary_watch_station.scad -D 'part="drum"'
devstl dev_watch_bezel.stl   canary_watch_station.scad -D 'part="bezel"'
devstl dev_watch_stand.stl   canary_watch_station.scad -D 'part="stand"'
devstl dev_sense_stand.stl   canary_sense_stand.scad   -D 'part="base"'
devstl dev_sense_gang.stl    canary_sense_gang.scad    -D 'part="plate"'
devstl dev_outlet_cradle.stl canary_outlet_cradle.scad -D 'part="cradle"'
devstl dev_relay_body.stl    canary_relay_solar.scad   -D 'part="body"'
devstl dev_relay_lid.stl     canary_relay_solar.scad   -D 'part="lid"'
devstl dev_relay_roof.stl    canary_relay_solar.scad   -D 'part="roof"'
devstl dev_combo_back.stl    canary_combo.scad         -D 'part="back"'
devstl dev_combo_front.stl   canary_combo.scad         -D 'part="front"'
devstl dev_hub_tray.stl      canary_hub_din.scad       -D 'part="tray"'
devstl dev_hub_cover.stl     canary_hub_din.scad       -D 'part="cover"' 
devstl dev_hammond_wap.stl    canary_hammond_chassis.scad -D 'stack="wap"'
devstl dev_hammond_vision.stl canary_hammond_chassis.scad -D 'stack="vision"'
devstl dev_hammond_sense.stl  canary_hammond_chassis.scad -D 'stack="sense"'
devstl dev_adapter_corner.stl   canary_mount_adapters.scad -D 'part="corner"'
devstl dev_adapter_magnet.stl   canary_mount_adapters.scad -D 'part="magnet"'
devstl dev_adapter_pole.stl     canary_mount_adapters.scad -D 'part="pole"'
devstl dev_adapter_template.stl canary_mount_adapters.scad -D 'part="template"'
devstl dev_insert_horn.stl   canary_inserts.scad -D 'part="horn"'
devstl dev_insert_glare.stl  canary_inserts.scad -D 'part="glare_ring"'
devstl dev_gland_body.stl    canary_inserts.scad -D 'part="gland_body"'
devstl dev_gland_bush.stl    canary_inserts.scad -D 'part="gland_bush"'
devstl dev_gland_cap.stl     canary_inserts.scad -D 'part="gland_cap"'
devstl dev_jbox_body.stl     canary_jbox.scad -D 'part="body"'
devstl dev_jbox_lid.stl      canary_jbox.scad -D 'part="lid"'
devstl dev_sign.stl          canary_sign.scad -D 'part="sign"'

# --- Field case (CER-4 design intent: IP67 + transit drop; field_ratings.md) -
devstl dev_field_body.stl    canary_field_case.scad -D 'part="body"'
devstl dev_field_lid.stl     canary_field_case.scad -D 'part="lid"'
devstl dev_field_boot.stl    canary_field_case.scad -D 'part="boot"'
devstl dev_field_gasket.stl  canary_field_case.scad -D 'part="gasket"'
devstl dev_field_bezel.stl   canary_field_case.scad -D 'part="bezel"'

# --- User tools (fit coupon, bench fixture, dock, shop tools) ---------------
devstl dev_coupon_base.stl   canary_fit_coupon.scad    -D 'part="base"'
devstl dev_coupon_mate.stl   canary_fit_coupon.scad    -D 'part="mate"'
devstl dev_coupon_strip.stl  canary_fit_coupon.scad    -D 'part="strip"'
devstl dev_fixture_plate.stl  canary_bench_fixture.scad -D 'part="plate"'
devstl dev_fixture_slider.stl canary_bench_fixture.scad -D 'part="slider"'
devstl dev_dock.stl          canary_dock.scad
devstl dev_insert_guide.stl  canary_shop_tools.scad -D 'part="insert_guide"'
devstl dev_button_ring.stl   canary_shop_tools.scad -D 'part="button_ring"'

# --- 1:1 paper install templates (SVG — print at 100% scale) ----------------
svg() { local out=$1; shift; echo "Rendering $out ..."
  "$OPENSCAD" -o "$out" "$@" canary_templates_2d.scad; }
svg template_studs.svg    -D 'mode="studs"'
svg template_bracket.svg  -D 'mode="bracket"'
svg template_doorbell.svg -D 'mode="doorbell"' 

if [[ "${1:-}" != "--no-png" ]]; then
  # one preview per printable variant — these drive the README's variant-picker gallery
  png "preview_all.png"     -D 'preset="battery_full"'    -D 'part="all"'
  png "preview_compact.png" -D 'preset="compact_plain"'   -D 'part="all"'
  png "preview_weather.png" -D 'preset="battery_weather"' -D 'part="all"'
  png "preview_coupon.png"  -D 'part="coupon"'
  (SRC="$VSRC"
   png "preview_vision_xiao_indoor.png"  -D 'host="xiao"'   -D 'preset="vision_indoor"'  -D 'part="all"'
   png "preview_vision_xiao_weather.png" -D 'host="xiao"'   -D 'preset="vision_weather"' -D 'part="all"'
   png "preview_vision_devkit.png"       -D 'host="devkit"' -D 'preset="vision_indoor"'  -D 'part="all"'
   png "preview_vision_bracket.png"      -D 'part="bracket"'
   png "preview_vision_knob.png"         -D 'part="knob"')
  (SRC="$DSRC"; png "preview_doorbell.png" -D 'part="all"')
  (SRC="$SSRC"; png "preview_sense.png" -D 'part="all"')
  (SRC="canary_watch_station.scad"; png "preview_dev_station.png" -D 'part="all"')
  (SRC="canary_sense_stand.scad";   png "preview_dev_stand.png"   -D 'part="all"')
  (SRC="canary_sense_gang.scad";    png "preview_dev_gang.png"    -D 'part="plate"')
  (SRC="canary_outlet_cradle.scad"; png "preview_dev_cradle.png"  -D 'part="cradle"')
  (SRC="canary_relay_solar.scad";   png "preview_dev_relay.png"   -D 'part="all"')
  (SRC="canary_combo.scad";         png "preview_dev_combo.png"   -D 'part="all"')
  (SRC="canary_hub_din.scad";       png "preview_dev_hub.png"     -D 'part="all"')
  (SRC="canary_hammond_chassis.scad"; png "preview_dev_hammond.png"  -D 'stack="vision"')
  (SRC="canary_mount_adapters.scad";  png "preview_dev_adapters.png" -D 'part="corner"')
  (SRC="canary_inserts.scad";         png "preview_dev_inserts.png"  -D 'part="horn"')
  (SRC="canary_jbox.scad";            png "preview_dev_jbox.png"     -D 'part="all"')
  (SRC="canary_sign.scad";            png "preview_dev_sign.png"     -D 'part="sign"')
  (SRC="canary_field_case.scad";    png "preview_dev_field.png"   -D 'part="all"')
  (SRC="canary_fit_coupon.scad";    png "preview_dev_coupon.png"  -D 'part="all"')
  (SRC="canary_bench_fixture.scad"; png "preview_dev_fixture.png" -D 'part="all"')
  (SRC="canary_dock.scad";          png "preview_dev_dock.png")
fi

echo "Done: released STLs (WAP 10 / Vision 9 / Doorbell 5 / Sense 2) + 41 dev renders + 3 SVG templates + previews."
