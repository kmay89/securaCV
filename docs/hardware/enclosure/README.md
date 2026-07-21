# Canary — 3D-Printable Enclosures

Printable housings, mounts and workshop tools for every SecuraCV **Canary**
witness device. Everything is a parametric [OpenSCAD](https://openscad.org)
"configurator": open a `.scad`, tick the options that match *your* hardware,
and the model rebuilds itself. Ready-to-print STLs for the common
configurations are committed next to the sources — **click any `.stl` link
to spin it in GitHub's 3D viewer, no download needed.**

## New here? Your first hour

You need: a filament printer (PETG recommended), the boards for whichever
Canary you're building (see the [hardware guide](../README.md)), and 4× M2
screws per case. Then:

1. **Print the [fit coupon](./canary_fit_coupon.scad)** (~25 min). Its
   labelled stations test every fit used across this folder; if a station is
   tight or loose it names the parameter to adjust. Calibrate once, reuse for
   every part below.
2. **Pick your case** from the [variant gallery](#pick-your-variant) —
   released variants have committed STLs; slice and print (flat, open side
   up, no supports — ever). New to slicing these? The
   [PETG Cura guide](./printing_petg_cura.md) has a settings sheet and a
   one-click profile.
3. **Fit your boards** — every case uses the same snap-clip cradles: boards
   press in, no screws. Each device section below has its assembly steps.
4. **Mount it** — print the wall bracket / plate for your case, or print a
   [paper template](#the-complete-file-map) at 100 % and drill from that.

Going **outdoors**? Read [Weather mode](#weather-mode-opt_seal) and the
[Engineering & materials](#engineering--materials-security-build) section
before printing — material choice and the gasket option matter out there.

## Table of contents

- [The complete file map](#the-complete-file-map) — every file, one line each
- [Pick your variant](#pick-your-variant) — gallery with previews + 3D viewers
- [Engineering & materials](#engineering--materials-security-build) — durability, materials, thermal kit, finish
- [Printing in PETG — Cura guide](./printing_petg_cura.md) — reasoned settings sheet, per-model cheat-sheet, importable profile
- [Field & environmental ratings](./field_ratings.md) — what "IP67"/"MIL-SPEC" honestly means here, the CER ladder + home test protocols
- Device deep-dives: [WAP](#canary-wap--enclosure-v07) · [Vision](#canary-vision--enclosure-v02) · [Doorbell](#canary-vision--doorbell-v01) · [Sense radome](#canary-sense--radome-enclosure-v01)

## The complete file map

**The four devices** (released — committed STLs, print-validated pipeline):

| File | What it makes |
|------|----------------|
| [`canary_wap_enclosure.scad`](./canary_wap_enclosure.scad) | **Canary WAP** box (XIAO ESP32-S3 Sense): peripheral bays for battery/GPS/buzzer/LED, three one-click presets, opt-in weather seal + wall mounts |
| [`canary_vision_enclosure.scad`](./canary_vision_enclosure.scad) | **Canary Vision** camera unit (Grove Vision AI V2 + OV5647): GoPro-compatible tilt hinge with locking detents, wall bracket + thumbscrew knob |
| [`canary_vision_doorbell.scad`](./canary_vision_doorbell.scad) | **Doorbell** form of the Vision build (Ring/Wyze size): camera + lit button, wedge-able wall plate, hidden security screw |
| [`canary_sense_enclosure.scad`](./canary_sense_enclosure.scad) | **Canary Sense** radar witness (MR60BHA2 + XIAO C6): the front is a thin RADOME window the 60 GHz beam passes through |

**In development** (geometry verified, not yet print-validated — render from
the `.scad`, no committed STLs; see the [dev gallery](#in-development)):

| File | What it makes |
|------|----------------|
| [`canary_watch_station.scad`](./canary_watch_station.scad) | Desk **monitoring puck** for the round-display XIAO stack ([display research](../display_research.md)) |
| [`canary_sense_stand.scad`](./canary_sense_stand.scad) | Weighted **bedside stand** for the Sense unit (wellbeing channel) |
| [`canary_sense_gang.scad`](./canary_sense_gang.scad) | **In-wall flush mount**: a single-gang faceplate that IS the radome |
| [`canary_outlet_cradle.scad`](./canary_outlet_cradle.scad) | **Outlet cradle**: hang any case off a USB wall adapter, zero hardware |
| [`canary_relay_solar.scad`](./canary_relay_solar.scad) | **Solar LoRa relay pod** for off-grid mesh backhaul (pole-mounted) |
| [`canary_combo.scad`](./canary_combo.scad) | **Radar + camera combo** witness in one housing |
| [`canary_hub_din.scad`](./canary_hub_din.scad) | **Server hub**: vented Raspberry Pi 5 box with a DIN-rail clip |
| [`canary_jbox.scad`](./canary_jbox.scad) | **Covert shell** styled as a utility junction box (lawful use; pair with the sign) |
| [`canary_sign.scad`](./canary_sign.scad) | **Witness signage plate** — "presence sensing in use, no video stored" |
| [`canary_hammond_chassis.scad`](./canary_hammond_chassis.scad) | **Chassis plate** carrying any Canary stack inside the off-the-shelf Hammond IP66 box |
| [`canary_mount_adapters.scad`](./canary_mount_adapters.scad) | **Mount adapters**: corner wedge, magnet plate, pole plate, drill template — all on the shared stud interface |
| [`canary_field_case.scad`](./canary_field_case.scad) | **Field case**: GoPro-class rugged witness — O-ring sealed, no external ports, TPU impact boot; the only CER‑4 (IP67 + drop) intent design ([ratings](./field_ratings.md)) |
| [`canary_dash_display.scad`](./canary_dash_display.scad) | **Dashboard display case** for the Waveshare 4.3" touch panel ([display research](../display_research.md) Option B): bezel frame, vented back with keyholes + 75 mm pair, desk stand |
| [`canary_vehicle_mount.scad`](./canary_vehicle_mount.scad) | **Vehicle mounts**: VHB dash plate (10° riser) + air-vent louver clip, on the shared stud interface — USB power only, no batteries on a hot dash |
| [`canary_wear_clip.scad`](./canary_wear_clip.scad) | **Body-worn carry**: belt leaf-spring clip + MOLLE/PALS adapter for the field case (or any keyhole case) |

**Workshop tools** (print/use alongside any build):

| File | What it makes |
|------|----------------|
| [`canary_fit_coupon.scad`](./canary_fit_coupon.scad) | **Print first**: one coupon that calibrates every fit in this folder |
| [`canary_bench_fixture.scad`](./canary_bench_fixture.scad) | Labelled **bring-up plate** holding the XIAO + buzzer/LED/button/reed while you wire and test ([bench guide](../bench_bringup.md)) |
| [`canary_dock.scad`](./canary_dock.scad) | Numbered **provisioning dock** for flashing a fleet of XIAOs in order |
| [`canary_inserts.scad`](./canary_inserts.scad) | Small **glue/press-in parts**: buzzer horn, anti-glare ring, printed cable gland |
| [`canary_shop_tools.scad`](./canary_shop_tools.scad) | Heat-set **insert press guide** + doorbell button accent ring |
| [`canary_templates_2d.scad`](./canary_templates_2d.scad) → [studs](./template_studs.svg) · [bracket](./template_bracket.svg) · [doorbell](./template_doorbell.svg) | **1:1 paper drill templates** — print the SVG at 100 % (check the 20 mm square), tape to the wall, drill |
| [`render.sh`](./render.sh) | Regenerates every STL, preview and SVG from source (CI runs it on every change) |

Everything shares one design language, so skills transfer between parts: the
same three **print-tolerance knobs** (`tol_slide`/`tol_press`/`tol_hole` —
the fit coupon tunes them), the same **snap-clip board cradles**, the same
**two-stud wall-hanging interface**, and the same opt-in **weather sealing**
(printed TPU gasket + drip-edge lid). A CI gate re-renders and mesh-checks
all ~60 parts on every change, so what's in this folder always builds.

## Pick your variant

One row per printable variant. **Every `.stl` link opens in GitHub's
interactive 3D viewer** — rotate and zoom it in the browser, no download
needed — and each preview shows the full printed set. Custom combinations
(other option mixes, `screw_insert`, labels…) render from the `.scad` in
minutes.

| Variant | For | Preview | Parts (click to view in 3D) |
|---------|-----|---------|------------------------------|
| **WAP · compact** | plain XIAO ESP32-S3, USB-powered, smallest box | <img src="./preview_compact.png" width="260"> | [base](./canary_wap_enclosure_compact_base.stl) · [lid](./canary_wap_enclosure_compact_lid.stl) |
| **WAP · battery** | XIAO Sense camera + LiPo + GPS, indoor | <img src="./preview_all.png" width="260"> | [base](./canary_wap_enclosure_battery_base.stl) · [lid](./canary_wap_enclosure_battery_lid.stl) |
| **WAP · weather** | battery build + TPU gasket seal, drip-edge lid, keyhole mounts (CER‑2 / ~IP54 target — [ratings](./field_ratings.md)) | <img src="./preview_weather.png" width="260"> | [base](./canary_wap_enclosure_weather_base.stl) · [lid](./canary_wap_enclosure_weather_lid.stl) · [gasket](./canary_wap_enclosure_weather_gasket.stl) |
| **WAP · clip coupon** | **print first** — 15 min snap-fit tuner | <img src="./preview_coupon.png" width="260"> | [coupon](./canary_wap_enclosure_clip_coupon.stl) |
| **Vision · xiao indoor** | stacked XIAO host (recommended), hinge mount, desk/shelf | <img src="./preview_vision_xiao_indoor.png" width="260"> | [back](./canary_vision_enclosure_xiao_indoor_back.stl) · [front](./canary_vision_enclosure_xiao_indoor_front.stl) |
| **Vision · xiao weather** | stacked XIAO, sealed + rain hood + vent, hinge & keyholes | <img src="./preview_vision_xiao_weather.png" width="260"> | [back](./canary_vision_enclosure_xiao_weather_back.stl) · [front](./canary_vision_enclosure_xiao_weather_front.stl) · [gasket](./canary_vision_enclosure_xiao_weather_gasket.stl) |
| **Vision · devkit indoor** | Grove-cabled ESP32-C3-DevKitM-1 host | <img src="./preview_vision_devkit.png" width="260"> | [back](./canary_vision_enclosure_devkit_indoor_back.stl) · [front](./canary_vision_enclosure_devkit_indoor_front.stl) |
| **Vision · mount kit** | wall bracket (GoPro-prong, tripod nut) + M5 thumbscrew knob | <img src="./preview_vision_bracket.png" width="180"> <img src="./preview_vision_knob.png" width="120"> | [bracket](./canary_vision_enclosure_bracket.stl) · [knob](./canary_vision_enclosure_knob.stl) |
| **Vision · DOORBELL** | Wyze/Ring form factor: camera + button, plate-mounted with a hidden security screw, sealed by default | <img src="./preview_doorbell.png" width="260"> | [body](./canary_vision_doorbell_body.stl) · [face](./canary_vision_doorbell_face.stl) · [plate](./canary_vision_doorbell_plate.stl) · [wedge 15°](./canary_vision_doorbell_plate_wedge15.stl) · [gasket](./canary_vision_doorbell_gasket.stl) |
| **Sense · radome** | MR60BHA2 mmWave witness: thin radar window, hinge + keyholes (shares the Vision bracket/knob) | <img src="./preview_sense.png" width="260"> | [back](./canary_sense_back.stl) · [front](./canary_sense_front.stl) |
| **Thermal / outdoor kit** | solar radiation shield (weather WAP) + universal desiccant tray | — | [shield](./canary_wap_enclosure_weather_shield.stl) · [tray](./canary_wap_enclosure_tray.stl) |

### In development

These designs are **render- and mesh-verified but not print-validated** — no
committed STLs yet (CI still renders and checks them on every change).
Open the `.scad`, measure your hardware, and render locally; feedback and
measurements welcome.

| Design | Status | Preview | Source |
|--------|--------|---------|--------|
| **Watch station** — SenseCAP-Watcher-style desk puck: XIAO ESP32-S3 pinned into the Round Display's back socket (see [display research](../display_research.md)); v0.2 bore sized to the measured Ø43.0 disc, snap bezel (no fasteners), and a true cradle divot | drum + snap bezel + 25° cradle | <img src="./preview_dev_station.png" width="230"> | [`canary_watch_station.scad`](./canary_watch_station.scad) |
| **Sense bedside stand** — weighted base + tilted stalk with the three-prong hinge head (wellbeing channel, ≤1.5 m) | ballast pockets, GoPro-compatible head | <img src="./preview_dev_stand.png" width="230"> | [`canary_sense_stand.scad`](./canary_sense_stand.scad) |
| **Sense in-wall plate** — single-gang flush mount; the faceplate IS the radome (check local code; low-voltage box only) | one-piece plate, 6-32 slots | <img src="./preview_dev_gang.png" width="230"> | [`canary_sense_gang.scad`](./canary_sense_gang.scad) |
| **Outlet cradle** — collar grips a USB wall wart; T-studs hang any keyhole-pocket Canary | measure your adapter | <img src="./preview_dev_cradle.png" width="230"> | [`canary_outlet_cradle.scad`](./canary_outlet_cradle.scad) |
| **Solar LoRa relay pod** — off-grid mesh backhaul: LoRa board + 18650, SMA top, solar roof bracket, pole straps | sealed body + roof | <img src="./preview_dev_relay.png" width="230"> | [`canary_relay_solar.scad`](./canary_relay_solar.scad) |
| **Combo witness** — Vision + Sense stacks in one face (lens + radome); radar-confirmed camera events | dual column, 3 USB ports | <img src="./preview_dev_combo.png" width="230"> | [`canary_combo.scad`](./canary_combo.scad) |
| **Hub (Pi 5, DIN rail)** — vented tray + cover for the server side; printed DIN spring clip | chimney vents, HAT headroom | <img src="./preview_dev_hub.png" width="230"> | [`canary_hub_din.scad`](./canary_hub_din.scad) |
| **Hammond chassis plates** — bring the Canary rail/clip cradles into the ENC1 polycarbonate route (`stack` = wap/vision/sense) | boss grid: MEASURE your box | <img src="./preview_dev_hammond.png" width="230"> | [`canary_hammond_chassis.scad`](./canary_hammond_chassis.scad) |
| **Mount adapters** — the T-stud interface everywhere: 90° corner wedge, magnet plate, pole strap plate, drill template | set `stud_gap` per case | <img src="./preview_dev_adapters.png" width="230"> | [`canary_mount_adapters.scad`](./canary_mount_adapters.scad) |
| **Functional inserts** — buzzer horn (louder chirp), matte anti-glare ring, 3-part printed cable gland (TPU bush) | glue-in / press-in parts | <img src="./preview_dev_inserts.png" width="230"> | [`canary_inserts.scad`](./canary_inserts.scad) |
| **Covert junction box** — compact WAP disguised as utility hardware; aperture hidden in a mock knockout | pair with the signage plate | <img src="./preview_dev_jbox.png" width="230"> | [`canary_jbox.scad`](./canary_jbox.scad) |
| **Witness signage plate** — "presence sensing in use / no video stored", debossed, 3 parametric lines | check local signage rules | <img src="./preview_dev_sign.png" width="230"> | [`canary_sign.scad`](./canary_sign.scad) |
| **Field case** — bag-carry rugged witness at the honest top of the FDM ceiling: Ø1.5 O-ring cord (27 % squeeze), six-lobe clamp, zero external ports (open to charge), bonded PC lens disc behind a contrast-color trim bezel, ePTFE vent, 4 mm walls, TPU impact boot + lanyard. CER‑4 intent: IP67 + MIL‑STD‑810H transit drop — [earn the rating, don't assume it](./field_ratings.md) | 72 × 39 × 21 body; boot on ≈ 94 wide | <img src="./preview_dev_field.png" width="230"> | [`canary_field_case.scad`](./canary_field_case.scad) |
| **Dashboard display case** — Waveshare ESP32-S3-Touch-LCD-4.3 (the [display research](../display_research.md) step-up dashboard): face-down bezel frame, vented screw-on back with keyholes + 75 mm M4 pair, free-standing 25° desk cradle. Panel dims are NOMINAL — measure yours | glass drops in, bezel lip 2.5 | <img src="./preview_dev_dash.png" width="230"> | [`canary_dash_display.scad`](./canary_dash_display.scad) |
| **Vehicle mount kit** — VHB-taped dash plate with a 10° stud riser + an air-vent louver clip (extruded spring prongs snap over one blade). ⚠️ cabins exceed +60 °C: USB power only, ASA, light colors | set `stud_gap` per case (36 = field) | <img src="./preview_dev_veh.png" width="230"> | [`canary_vehicle_mount.scad`](./canary_vehicle_mount.scad) |
| **Body-worn clips** — belt leaf-spring clip (prints on its side: flex stays in-plane) + MOLLE/PALS weave plate, both on the two-stud interface; made for the field case's floor keyholes | check local recording law; pair with the sign | <img src="./preview_dev_wear.png" width="230"> | [`canary_wear_clip.scad`](./canary_wear_clip.scad) |
| **Universal fit coupon** — ONE ~25 min print that calibrates every fit in the catalog: clip, keyhole+stud, slide, gasket, press, screw, insert — each station labelled with the parameter it tunes | **print this before any case** | <img src="./preview_dev_coupon.png" width="230"> | [`canary_fit_coupon.scad`](./canary_fit_coupon.scad) |
| **Bench bring-up fixture** — labelled stations for XIAO + BZ1/DLED1/SW1/SW2 with a sliding magnet carriage for repeatable tamper tests (companion to [bench_bringup.md](../bench_bringup.md)) | wire channels per §5 pin map | <img src="./preview_dev_fixture.png" width="230"> | [`canary_bench_fixture.scad`](./canary_bench_fixture.scad) |
| **Fleet provisioning dock** — N numbered reclined bays for bare XIAOs beside a USB hub (v1 runbook fleet flashing) | `n_bays` parametric | <img src="./preview_dev_dock.png" width="230"> | [`canary_dock.scad`](./canary_dock.scad) |
| **Shop tools** — heat-set insert press guide (keeps inserts square) + doorbell button accent ring | tiny prints | — | [`canary_shop_tools.scad`](./canary_shop_tools.scad) |
| **Paper install templates** — 1:1 SVGs ([studs](./template_studs.svg) · [bracket](./template_bracket.svg) · [doorbell](./template_doorbell.svg)): print ON PAPER at 100 % (verify the 20 mm square), tape to the wall, drill | no plastic needed | — | [`canary_templates_2d.scad`](./canary_templates_2d.scad) |

## Engineering & materials (security build)

These are housings for a *security* device — the enclosure is the first
physical attack surface — so the designs follow FDM packaging-engineering
practice for **durability, rigidity and low mass**. (For what these cases
can honestly claim against IP / MIL‑STD ratings — and the home test
protocols that earn each level — see
[**field_ratings.md**](./field_ratings.md).)

- **Stiffness from geometry, not bulk.** Plate stiffness scales with t³: the
  lids/fronts carry a perimeter **rib ring** (`lid_ribs`, on by default) just
  inside the lip, auto-routed around every feature — roughly trebling the flat
  face's bending stiffness against prying for ~1 g of material. Walls stay at
  2.0 mm (= 5 extrusion widths), with the corner posts gusseted into both walls.
- **Layup-aware loading.** FDM interlayer (Z) strength is only ~50–70 % of
  in-plane. Both shells print so the service loads — lid pry, wall impact,
  screw clamp — act *in-plane*; the bottom edge (the classic delamination
  initiation site) gets a 45° **`foot_cham`** chamfer that also removes
  elephant-foot.
- **Service-grade fastening.** The M2 self-tappers are fine for ~10 open/close
  cycles at ≤0.3 N·m. For a serviced fleet, set **`screw_insert = true`**: the
  corner posts auto-fatten (≥1.2 mm wall around the bore) for **M2 brass
  heat-set inserts** (3.5 × 4.0 short series; install at 220–240 °C for PETG,
  240–260 °C for ASA, flush to the post top) and use M2 machine screws.
  Plastic creeps under gasket preload — **re-torque sealed builds after 24 h**,
  or fit inserts and be done with it.
- **Anti-lift security.** Keyhole mounts can be defeated by lifting the unit
  off the wall screws. `kh_lock` (default on with keyholes) adds two blind
  **knockout bosses** (0.6 mm web — the seal stays intact until used): after
  hanging, drive #4/M3 screws through them into the wall from *inside* the
  case, so removal requires opening the lid first. Pair with the tamper
  magnet/reed option and, if you want tool-only access, security-drive (Torx
  pin) M2 screws. On the Vision hinge, swap the thumbscrew for an **M5 button
  head + nyloc** to make the angle tool-only and vibration-proof.

**Material selection** (the housings are unfilled-polymer friendly; the Vision
*bracket and prongs* are the highest-stressed parts):

| Material | Use for | Why / limits |
|----------|---------|--------------|
| **PETG** | default, indoor + sheltered outdoor | tough, easy, low moisture pickup; creeps under sustained clamp — use inserts for sealed builds |
| **ASA** | outdoor, sun-exposed | UV-stable, HDT ~95 °C; print hot and draft-free |
| **PC / PC-blend** | maximum impact + heat (vandal-prone spots) | highest toughness/HDT; needs an enclosed printer; pair with neutral-cure silicone only |
| **CF-PETG / CF-Nylon** | Vision **bracket + knob** | ~2× stiffness for the cantilevered hinge; hardened nozzle required |
| **TPU 90–95 A** | gaskets | 2 perimeters, 100 % infill, slow |
| PLA | clip coupon / fit checks **only** | creeps and softens ~55 °C — not for deployed housings |

**Thermal / outdoor kit** — the enemy outdoors is solar heat as much as
water: a dark case in sun can exceed the LiPo's comfort zone (build plan
§9.2). `part="shield"` prints a Stevenson-screen style **solar radiation
shield**: a second roof standing 6 mm above the lid on hollow standoffs,
fastened by the existing corner screws (swap in M2 × 16–18), with apertures
auto-opened over the camera/LED/touch. Print it in **white or light ASA**.
`part="tray"` is a slotted **desiccant tray** for a 1 g silica pack — clip or
VHB it into any Canary cavity to keep condensation off the boards. For insect
resistance, keep vent holes ≤ 1.0 mm (the Sense case defaults to 1.0 × 10;
on the others set `vent_hole_d = 1.0; vent_holes = 10`) or stick fine mesh
behind the GORE seat.

**Finish & printability details** (built into the models): curve quality is
driven by `$fa/$fs`, so large radii (the doorbell pill, the hood) come out
smooth instead of visibly faceted; horizontal hinge bores are **teardropped**
so their crowns print without sag; visible faces support a **two-stage soft
edge** (`lid_edge` + `lid_edge2` — default on for the doorbell face) that
approximates a roundover while still printing face-down without support.
For the nicest faces, print them on a **textured PEI plate** (the visible
surface is the first layer) and set the slicer's **seam position to the rear
edge** of each part.

**Consumer-grade looks — the finish ladder.** Store-bought cases read
"product" because of four things you can reproduce, in increasing effort:

1. **Free (slicer only)** — A-surfaces face-down on textured PEI (every lid
   and face here is modelled for it); **fuzzy skin ~0.3 mm on outside walls
   only** (hides layer lines completely and matches the PEI texture); seam
   painted to a rear corner; matte filament (matte ASA/PETG reads
   injection-moulded in a way silk/gloss never does).
2. **Two-tone (~zero effort)** — the parts are already split along color
   lines: shell in one color, **TPU boot / gaskets / bezel + button accent
   rings in a contrast color** (graphite + safety-orange boot is the classic
   field look; all-black with a graphite bezel is the Wyze/Ring look). The
   field case's **lens trim ring** (`part="bezel"`) exists exactly for this —
   it also hides the disc's silicone bond line, which is why real cameras
   have trim rings.
3. **Paint-filled deboss (10 min)** — every `label_text` deboss is 0.5 mm
   deep: brush acrylic into the recess, let it set, wipe the face with
   isopropyl — crisp printed-on branding.
4. **Full finish (hours)** — sand 240→400, filler-primer, 600 wet, matte 2K
   clear (or ASA acetone **vapor smooth** for gloss + extra water sealing —
   outdoor-safe, but mask the gasket grooves and re-check clip fits after).

Rule of thumb: fuzzy skin + textured first layer + two-tone + paint-filled
label gets ~90 % of the consumer look for ~0 % extra work.

**Security-build slicing spec:** 0.4 mm nozzle, 0.2 mm layers, **4 perimeters**,
5 top/bottom layers, **30 % gyroid** infill, ~30 % infill/perimeter overlap,
+5–10 °C over the material's default for interlayer adhesion, minimal cooling
on ASA/PC. Anneal PETG/PC parts (65 °C / 90 °C, 1 h, supported flat) for a
further ~20 % creep and stiffness margin if you have an oven. For the full
reasoning — every Cura setting with its *why*, a per-model orientation
cheat-sheet, PETG stringing/sealing notes, and a one-click importable profile —
see [**Printing in PETG — Cura guide**](./printing_petg_cura.md) (with a
one-click [importable Cura profile](./profiles/README.md)).

**Mass budget** (solid-volume upper bounds from the rendered STLs; PETG at
1.27 g/cm³ — multiply by 0.84 for ASA): WAP compact ≈ 12 g/pair, WAP battery
≈ 34 g/pair, WAP weather ≈ 57 g/pair + 0.6 g gasket; Vision xiao ≈ 31 g/pair
(weather ≈ 51 g), Vision devkit ≈ 42 g/pair, bracket ≈ 10 g, knob ≈ 3 g.
Even the heaviest full kit stays under ~75 g — wall anchors, not weight,
size the mounting.

---

# Canary WAP — Enclosure (v0.7)

Parametric, printable case for the Canary WAP (XIAO ESP32-S3 Sense), documented
in §6.6 of the [Peripheral Build Plan](../canary_peripheral_build_plan.md).
Authored in [OpenSCAD](https://openscad.org), and built as a **configurator**:
tick the peripherals you fitted and the case rebuilds itself — adding the right
bays, ports and cutouts and resizing to suit. Opt-in extras add a **TPU gasket +
drip-edge lid** for splash resistance and **wall-mount keyholes/tabs**.

## Configure it (you tick what you have)

Open `canary_wap_enclosure.scad` in the **OpenSCAD GUI** → the **Customizer**
panel shows your options as checkboxes. Tick what's on your device:

| Checkbox | When ticked, the case adds… |
|----------|------------------------------|
| `opt_camera` | sensor **window** on the lid (+ recessed seat for a clear disc) + extra internal height (Sense camera). Off ⇒ shorter cavity for a plain XIAO |
| `opt_buzzer` | **vent** (hole-ring + GORE seat) on the lid |
| `opt_led` | **light-pipe** port on the lid |
| `opt_battery` | **LiPo bay** beside the board (enlarges the case) |
| `opt_gps` | internal **GPS module bay** (L76K) |
| `opt_tamper` | **magnet pocket** on the lid underside |
| `opt_touch` | thinned **touch window** on the lid (capacitive sensing through the wall) |
| `opt_antenna` | **bulkhead hole** in the side wall for a u.FL/SMA antenna |
| `opt_seal` | **weather mode**: perimeter gasket groove + printable TPU gasket + drip-edge lid + USB plug recess (see [Weather mode](#weather-mode-opt_seal)) |
| `opt_mount` | **wall mounting**: blind keyholes and/or external screw tabs (see [Mounting](#mounting-opt_mount)) |

Prefer a one-click starting point? Set **`preset`** to `battery_full`,
`compact_plain` or `battery_weather` (overrides the checkboxes), or leave it
`custom` to use them. Then set `part` to `base`, `lid`, `all`, `coupon`, or
`gasket` and export.

The three committed example presets:

| Preset | Outer size (rendered) | What's on it |
|--------|----------------------|--------------|
| **battery_full** | **104.7 × 39 × 17.2 mm** | camera + buzzer + LED + LiPo bay + GPS bay + tamper |
| **compact_plain** | **33.7 × 37.6 × 13.7 mm** | plain board (no camera), buzzer + LED, USB-powered |
| **battery_weather** | **106.3 × 40.6 × 20.2 mm** | battery_full **+ gasket seal + drip-edge lid + keyhole mounts** |

> Corner posts always sit in **true corners beside the board** — that's why even
> the compact case isn't as tiny as the bare-board reference. The board is always
> parked against the **USB (−X) wall** so the connector reaches the opening.

![battery_full preset — base and lid](./preview_all.png)
![compact_plain preset — base and lid](./preview_compact.png)
![battery_weather preset — base, drip-edge lid and TPU gasket](./preview_weather.png)

| | |
|---|---|
| **Source** | [`canary_wap_enclosure.scad`](./canary_wap_enclosure.scad) (configurator) |
| **Example STLs** | [`..._battery_base/lid.stl`](./canary_wap_enclosure_battery_base.stl), [`..._compact_base/lid.stl`](./canary_wap_enclosure_compact_base.stl), [`..._weather_base/lid/gasket.stl`](./canary_wap_enclosure_weather_base.stl) |
| **Clip test coupon** | [`..._clip_coupon.stl`](./canary_wap_enclosure_clip_coupon.stl) — print first to tune the snap fit |
| **Fasteners** | 4 × **M2** self-tapping screws (8–10 mm) into the corner posts |

## Print tolerances (tune once for your printer)

All fits are driven by three per-side clearance parameters, applied uniformly.
Print the **clip coupon** and check the lid fit on a small test before adjusting:

| Param | Default | Governs | Symptom → change |
|-------|--------:|---------|------------------|
| `tol_slide` | 0.20 | lid lip ↔ base, drip skirt ↔ wall, camera-disc seat | lid binds → raise; lid rattles → lower |
| `tol_press` | 0.10 | tamper magnet pocket, LED light pipe | part won't press in → raise; falls out → lower |
| `tol_hole` | 0.30 | lid screw clearance holes | screw drags on the lid → raise |
| `clip_clear` | 0.25 | snap-clip face ↔ board edge | tune **on the coupon**, with `clip_t`/`clip_hook` |

Enter **true part sizes** in the dimension parameters (e.g. `mag_d` is the
*magnet* diameter, `lp_d` the *light-pipe* diameter) — the tolerance system adds
the clearance for you.

> **Migrating from ≤ v0.6:** `mag_d` used to be the *pocket* size (6.4); it is
> now the *magnet* size (6.0) and the pocket is a **press fit**
> (`mag_d + 2*tol_press`). `fit_gap` was renamed to `tol_slide`.

## Weather mode (`opt_seal`)

A printed case is **never IP-rated**; for harsh outdoor exposure use the
polycarbonate enclosures listed in build plan §9. Weather mode upgrades this case
from "indoor only" to **rain/splash-resistant (≈ IP54)** — *not* immersion:
the four corner screws can't clamp the long spans like a moulded box can.

What `opt_seal = true` adds:

- **Perimeter gasket groove** in the base rim (walls auto-thicken to host it)
  and a matching **printable TPU gasket** (`part = "gasket"`). The gasket stands
  0.6 mm proud and squeezes ~35 % under the lid screws.
- **Drip-edge lid**: the lid grows a 3 mm skirt that overlaps the base wall, so
  water sheds off the seam instead of sitting on it.
- **USB plug recess**: a shallow recess frames the USB-C opening so a flanged
  silicone dust plug seats flush (fit one for deployment; remove to charge/flash).
- **Rim headroom guarantee**: the cavity auto-grows so the gasket path stays
  continuous above the USB opening.

To finish the seal you also need to:

1. **Camera window**: glue a **12 × 1 mm clear PMMA or polycarbonate disc** into
   the recessed seat on the lid face with **neutral-cure silicone** (acid-cure
   attacks polycarbonate). Run the bead full-circle.
2. **Buzzer vent**: stick an **adhesive GORE-type membrane vent** over the
   recessed seat on the *outside* of the lid — it passes sound and equalises
   pressure while blocking water.
3. **Light pipe**: bed the pipe in a drop of the same silicone.
4. **Antenna** (if fitted): use a gasketed bulkhead or seal the hole with silicone.
5. **Mount it USB-down** (see below) so the lid is vertical, the vent faces
   sideways and the only wall opening points at the ground.

**Gasket print settings:** TPU 90–95A, 0.2 mm layers, 2 perimeters, 100 % infill,
slow (~25 mm/s). It's a 1.1 mm-wide ring — print it alone, brim optional.

**Assembly order with the seal:** board + battery in, gasket into the groove,
lid on (lip nests inside the gasket ring), then the 4 screws in two passes
(snug diagonally, then final quarter-turns) so the squeeze is even.

## Mounting (`opt_mount`)

`mount_style` selects `"keyhole"`, `"tabs"`, or `"both"`. The case hangs with
the **USB end facing down** — that's the water-shedding orientation and the
keyhole slots are oriented for it.

- **Keyholes** (default): two **blind** keyhole pockets in the case back — they
  never break into the cavity, so weather mode stays sealed. The back thickens
  by `kh_extra` (3 mm) to host them. Hang on **#6 / M3.5 pan-head** screws:
  pass the head through the round end, slide the case **down**. Short cases
  auto-merge to a single centred keyhole.
- **Tabs**: four external counterbored ears (M3/#6) on the long walls — for
  when you want visible, screw-it-flat mounting. Fully outside the seal.

## Dimensional basis (verified)

These dimensions were reconciled against **Seeed's official spec** and a
**reference enclosure STL** supplied for this work:

| Source | Figure | Used for |
|--------|--------|----------|
| Seeed XIAO ESP32-S3 ([p-5627](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html)) | PCB **21.0 × 17.5 mm**, 2.54 mm pitch, USB-C on a short edge | `board_l`, `board_w`, USB position |
| Reference case STL (`smallest_esp32_V2`) | bbox **20.0 × 35.1 × 9.8 mm**, internal cavity ≈ 9.8 mm deep, board sits 17.5 mm-width across, extra length = USB-C cable clearance | sanity check on cavity depth + USB-only footprint → the **compact** variant |

## What's in the box (features appear when their peripheral is ticked)

| Feature | Where | Toggle |
|---------|-------|--------|
| Board cradle + frame | base | always (standoffs + perimeter frame, PCB `standoff_h` off the floor) |
| **Board snap clips** | base, long edges | `board_clips` — 4 tabs hook over the PCB, **clicks in, no screws** |
| **USB-C port** | base, −X wall | always (10.5 × 6.5 mm, PCB-top height; board parked at this wall) |
| Battery bay + **wire channel** | base | `opt_battery` (lead notch in the ribs: `batt_wire_w`) |
| GPS module bay | base | `opt_gps` |
| Antenna bulkhead | base, +X wall | `opt_antenna` |
| **Camera/sensor window + disc seat** | lid | `opt_camera` (12 × 1 mm clear disc, `cam_disc_d/t`) |
| **Light-pipe / LED port** | lid | `opt_led` (press fit) |
| **Buzzer + pressure vent** | lid | `opt_buzzer` (GORE seat + hole-ring) |
| **Cap-touch window** | lid | `opt_touch` (thinned to `touch_wall`) |
| **Tamper-magnet pocket** | lid underside | `opt_tamper` (press fit — add a drop of glue) |
| **Gasket groove + drip skirt + USB plug recess** | base rim / lid edge | `opt_seal` |
| **Keyholes / screw tabs** | base back / walls | `opt_mount` + `mount_style` |
| **Chamfered lid edge, debossed label** | lid | `lid_edge`, `label_text` (label needs the font installed) |
| Screw lid | 4 corners | countersunk M2 into self-tapping posts; lip nests into base |

## Render / regenerate the STLs

Requires OpenSCAD (CLI). The helper renders all three example presets, the
gasket, the coupon and the preview PNGs (`--no-png` to skip the images;
`OPENSCAD=/path/to/openscad` to point at a non-PATH binary):

```bash
./render.sh
# or build your own config directly:
openscad --export-format binstl -o my_base.stl \
    -D 'preset="custom"' -D 'opt_gps=true' -D 'opt_camera=false' -D 'part="base"' \
    canary_wap_enclosure.scad
# the matching TPU gasket for a custom sealed config:
openscad --export-format binstl -o my_gasket.stl \
    -D 'preset="custom"' -D 'opt_seal=true' -D 'part="gasket"' \
    canary_wap_enclosure.scad
# or the clip test coupon:
openscad --export-format binstl -o coupon.stl -D 'part="coupon"' canary_wap_enclosure.scad
```

## Key parameters to check first

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `opt_*` | see table | **Tick the peripherals you fitted** — the case adapts |
| `preset` | `"custom"` | `battery_full` / `compact_plain` / `battery_weather` one-click configs (override the checkboxes) |
| `part` | `"all"` | `base` / `lid` / `all` / **`coupon`** (clip-fit tester) / `gasket` (TPU seal) |
| `tol_slide` / `tol_press` / `tol_hole` | 0.20 / 0.10 / 0.30 | Per-side fit clearances — tune once for your printer |
| `batt_l/w/h`, `gps_l/w/h` | — | Match **your** cell / GPS module. Keep `batt_h` ≥ 1 mm over the nominal cell — LiPos swell; **scrap any swollen cell** (build plan §6.5) |
| `cam_dx/dy`, `lp_dx/dy`, `vent_dx/dy`, `mag_dx/dy`, `touch_dx/dy` | — | Feature positions **from board centre** — set from a real measurement |
| `cam_disc_d/t` | 12.0 / 1.0 | Clear-disc seat on the lid face (0 = bare hole) |
| `usb_w/h/z` | 10.5/6.5/0 | Align to your USB-C cable boot |
| `clip_t` / `clip_hook` / `clip_clear` | 1.5 / 0.8 / 0.25 | Tab flex vs. grip — tune on the **coupon** first |
| `lid_edge`, `label_text` | 0.8 / `""` | Lid edge chamfer; debossed label text |
| `mount_style`, `kh_*`, `tab_*` | keyhole | Match your screws / wall plugs |

## Suggested print settings

- **Material:** PETG or ASA for heat/UV exposure (PLA only for indoor/bench).
  **Gasket:** TPU 90–95A, 2 perimeters, 100 % infill, slow. Deployed units:
  use the hardened spec in [Engineering & materials](#engineering--materials-security-build).
- **Layer height:** 0.2 mm. **Walls:** 3 perimeters. **Infill:** 20–30 %.
- **Orientation:** both parts flat, open side up — no supports. The lid prints
  **face-down**: the edge chamfer, disc seat and debossed label all land on the
  first layers, so a clean bed = a clean lid face.
- **Camera window:** the seat takes a 12 × 1 mm clear disc; bond with
  neutral-cure silicone (see [Weather mode](#weather-mode-opt_seal)).

## Assembly

1. (battery variant) Drop the LiPo in the bay; route the leads through the
   **wire channel** notched in the bay ribs along the +Y wall to the XIAO BAT
   pads. **Read the battery safety notice in build plan §6.5 first.**
2. **Press the PCB straight down** until the four edge clips snap over it,
   USB-C aligned to the wall cutout — no screws needed for the board. (To remove,
   gently splay the clips outward and lift.)
3. Press a 6 mm magnet into the lid pocket (press fit — a drop of CA glue makes
   it permanent); add a GORE vent over the seat if sealing; press the light pipe
   into the LED port.
4. (weather mode) Seat the TPU gasket in the rim groove; glue the clear disc
   into the camera seat.
5. Close the lid (lip nests into the base) and drive 4 × M2 screws — snug
   diagonally first, then final quarter-turns. Don't crank them: M2 self-taps
   strip printed posts beyond ~0.3 N·m (two fingers on the short end of the
   driver is plenty).

## Build history

**v0.2:** `board_w` corrected 17.8 → **17.5 mm** (Seeed official); USB-C opening
widened to **10.5 × 6.5 mm** to clear a real cable boot (the connector body is
~8.9 × 3.2 mm); added the **compact** variant.

**v0.3 — printability & connected structure:** the four corner **screw posts are
now fused to both adjacent walls** with gussets (no thin free-standing towers),
and the **board standoffs are joined into a perimeter frame and ribbed to the
screw posts** so the whole interior prints as one rigid piece with good bed
adhesion.

**v0.4 — screwless board retention:** four **cantilever snap clips** along the
board's long edges hold the PCB down by clicking over its top — **no screws to
mount the board**. A 45° lead-in cams them open on insertion. The screw posts
still take the **lid** screws; the lip already locates the lid, so the lid
screws remain optional indoors.

**v0.5 — peripheral configurator + clip coupon:** the case is driven by
**peripheral checkboxes** plus `preset`; toggling a peripheral adds/removes its
cutout or bay and **resizes the box**. Added the **clip test coupon**.

**v0.6 — mechanical fixes (from review):** M2 self-tap pilot tightened
`2.0 → 1.6 mm` so threads actually bite; broken battery cradle rim replaced with
**two transverse ribs**; **lid lip notched at the USB end** so the cable
plug/overmold can't jam it.

**v0.7.2 — finish & printability (family-wide):** `$fa/$fs` curve quality
(smooth large radii), teardropped hinge bores on the Vision mounts, two-stage
soft face edges (`lid_edge2`), cosmetic lead-in rims on the doorbell seats,
and finish guidance (textured plate, seam placement) in
[Engineering & materials](#engineering--materials-security-build).

**v0.7.1 — engineering hardening:** perimeter **rib ring** under the lid
(`lid_ribs`, t³ stiffening, feature-aware routing), **45° bottom-edge chamfer**
(`foot_cham`), **M2 heat-set insert option** (`screw_insert`, posts auto-fatten),
and **anti-lift knockouts** beside the keyholes (`kh_lock`, 0.6 mm sealed web).
See the shared [Engineering & materials](#engineering--materials-security-build)
section for the material table, security-build slicing spec and mass budget.

**v0.7 — fit, weather & mounting (this release):**

- **Fix — compact case USB was unreachable:** the board was centred when no bay
  was fitted, leaving the USB-C connector ~6.5 mm behind the wall (a plug only
  inserts 6–7 mm). The board is now **always parked at the USB wall**; the
  compact case got 4 mm shorter and slightly wider (the width now guarantees
  the snap clips clear the corner posts).
- **Fix — tamper-magnet ring exported as a separate shell** (coplanar-touch
  union); now embedded into the plate — every STL is a single watertight part.
- **Unified print tolerances** (`tol_slide`/`tol_press`/`tol_hole`) applied to
  every fit; **`mag_d` is now the magnet diameter** and the pocket is a press
  fit so the magnet stays put.
- **Camera window got a recessed seat** for a glued 12 × 1 mm clear disc.
- **Snap-clip underside chamfered 45°** across the clearance gap — cleaner FDM
  bridging; the retention seat stays flat on purpose (a sloped seat loses grip).
- **Battery wire channel** notched into the bay ribs; swelling allowance
  documented.
- **Weather mode** (`opt_seal`): gasket groove + printable TPU gasket +
  drip-edge lid + USB plug recess + rim-headroom guarantee. New
  **`battery_weather`** preset and `part="gasket"`.
- **Mounting** (`opt_mount`): blind seal-safe keyholes and/or external screw
  tabs, oriented for USB-down hanging.
- **Aesthetics:** chamfered lid edge (`lid_edge`), optional debossed label
  (`label_text`).
- README sizes are now taken from the rendered STLs (the old "86 mm" claim was
  stale); `render.sh` also emits preview PNGs and honours `$OPENSCAD`.

> ⚠️ **Still a reference — verify before printing.** Seeed publishes the PCB
> outline but not every component height; the camera-lens/LED/buzzer positions on
> the lid are nominal. **Print the clip coupon and test-fit, then measure your
> board and check the lid** before the full box. A printed case is **not
> IP-rated** — weather mode is splash resistance, not immersion; see climate/IP
> guidance in build plan §9.

---

# Canary Vision — Enclosure (v0.2)

Parametric camera unit for the Canary Vision stack — **OV5647 camera**
(Pi-cam v1.3 form factor) + **Grove Vision AI V2** (25 × 25 mm) + a selectable
**host** (the `host` parameter, matching the
[device guide](../grove_vision_ai_v2_guide.md) §3 options):

| `host` | Build | Case |
|--------|-------|------|
| **`xiao`** (default) | XIAO ESP32-C3/S3 **seated in the module's stacking socket** — recommended, zero wiring | compact single column, ≈ **47 × 59 × 24 mm**; the bottom wall carries **both USB-C ports** (module *model port* above, XIAO *firmware port* below) |
| `devkit` | ESP32-C3-DevKitM-1 joined by the **Grove I2C cable** | two columns, ≈ 80 × 61 × 18 mm |

The front face carries the lens aperture (recessed clear-disc seat + optional
rain hood) and the LED light pipe; the back shell carries the boards and the
mounts.

![vision_weather preset (xiao host) — back, front and gasket](./preview_vision_xiao_weather.png)

## Why not Seeed's foldable holder?

Seeed's official XIAO Vision AI Camera case uses a fold-out **friction hinge**:
fine on a desk, but the angle sags over time, there's no lock, no enclosure
sealing, and no real wall-mount story. This design replaces it with a
**GoPro-compatible two-prong hinge** on the top wall (3.0 mm fins, 6.35 mm
pitch, M5 axis):

- **Sag-proof**: optional radial **detent teeth** (`hinge_teeth`, on by
  default) interlock the mating faces in 15° steps — the set angle cannot
  drift. Set `hinge_teeth = false` for smooth faces and full compatibility
  with off-the-shelf GoPro accessories (arms, clamps, suction mounts…).
- **Locked, not rubbed**: the angle clamps with an **M5 thumbscrew** (buy a
  GoPro-style knurled screw, or print the included `knob` over an M5 × 25
  bolt + nut).
- **Typical mounting scenarios** out of the box:
  - **wall / eave** — print the `bracket` (three prongs + 4 countersunk #8/M4
    screws *or* two keyhole slots), click the case in, set the pitch, tighten;
  - **tripod / clamp** — the bracket has a captive **1/4-20 nut pocket**
    (`bracket_tripod`) behind the centre fin;
  - **flush wall** — `mount_style = "keyhole"` puts blind, seal-safe keyhole
    pockets in the case back instead of (or as well as) the hinge;
  - the whole **GoPro ecosystem**, with `hinge_teeth = false`.

## Water ingress

Same opt-in system as the WAP case (`opt_seal`): perimeter **TPU gasket** in a
groove on the back-shell rim, **drip-edge skirt** on the front so water sheds
off the seam, **flanged USB plug recess** on the bottom wall, plus two
camera-specific items:

- **Rain/glare hood** (`opt_hood`): a ~220° collar over the lens window, open
  at the bottom — keeps rain and skylight off the glass.
- **Lens window**: bond a **14 × 1 mm clear PMMA/PC disc** into the recessed
  seat with **neutral-cure** silicone (full-circle bead in weather mode).

Mount it **USB-down** (the hinge makes this natural) so the only wall opening
faces the ground, fit a **GORE vent** over the `opt_vent` cluster for
pressure equalisation, and treat the result as **rain/splash-resistant
(~IP54)** — for harsh exposure use the Hammond ENC1 path (build plan §9).

## Presets & parts

| Preset | What you get |
|--------|--------------|
| **vision_indoor** | hinge mount, LED port, no seal — desk/shelf unit |
| **vision_weather** | seal + hood + GORE vent + hinge **and** keyholes |

`host` is independent of the preset — any combination works. `part` = `back` /
`front` / `all` / `gasket` / `bracket` / `knob`. Committed STLs:
`xiao_indoor`, `xiao_weather` (+ gasket) and `devkit_indoor`; other combos
render via the Customizer or CLI. Outer sizes: xiao ≈ **47 × 59 × 24 mm**
(weather ≈ 49 × 60 × 30), devkit ≈ 80 × 61 × 18 (+20 mm prongs on all).

## Assembly

1. Screw the **OV5647** to the four posts inside the front face (M2
   self-tappers, lens through the aperture); bond the clear disc into the seat.
2. *(xiao host)* Seat the **XIAO** in the module's socket — **both USB-C ports
   must face the same direction; backwards seating feeds power into GPIO and
   can kill either board** (device guide §3). Click the stack into the tall
   side rails, module up, both ports aligned to the two bottom-wall openings
   (model port above, firmware port below).
   *(devkit host)* Click the **Vision AI V2** and the **DevKitM-1** into their
   snap-clip cradles (DevKit USB-C down, aligned to the wall opening); the
   module's own USB-C faces the middle gap — open the case to reflash models.
3. Route the camera FPC to the module's CSI connector (and, devkit host, the
   Grove cable across the middle gap to the DevKit pins).
4. (weather) Seat the TPU gasket in the rim groove.
5. Close the front (lip nests into the back) and drive the 4 × M2 corner
   screws — snug diagonally, then final quarter-turns.
6. Screw the **bracket** to the wall (or a tripod plate via the 1/4-20 nut),
   slot the case prongs into it, set the angle, tighten the M5 thumbscrew.

## Key parameters

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `host` | `"xiao"` | `"devkit"` for the Grove-cabled DevKitM-1 build |
| `stack_sock_h` | 11.5 | *(xiao)* module underside → XIAO underside when seated — **measure the stack** |
| `xiao_usb_drop` | 10.0 | *(xiao)* XIAO port centre below the module port centre — **measure** |
| `usb_dx` / `xiao_usb_dx` | 0 / 0 | Port offsets along the bottom wall — measure if either port is off-centre |
| `dk_l/dk_w`, `vm_l/vm_w`, `cam_w/cam_h` | 39×25.4 / 25×25 / 25×24 | **Measure your boards** — DevKit revisions differ |
| `standoff_h` | 3.0 | *(devkit)* **raise to ~10 if your DevKit has soldered pin headers** |
| `lens_dx/dy` | 0 / 2.5 | Lens centre offset from the camera-board centre — measure |
| `cam_hole_x/y` | 21 / 12.5 | Pi-cam v1.3 mounting grid |
| `lp/vent/mag_dx/dy` | — | Front-face feature offsets **from the module centre** (valid for both hosts) |
| `hinge_teeth` | true | `false` = smooth GoPro-compatible faces |
| `tol_slide/press/hole` | 0.20/0.10/0.30 | Same per-printer tolerance trio as the WAP case |
| `mount_style` | hinge | `keyhole` / `both` |
| `bracket_tripod` | true | captive 1/4-20 nut in the bracket |

**Print settings:** as the WAP case (PETG/ASA, 0.2 mm, 3 perimeters, no
supports — every part prints flat; the prongs print as part of the shell with
the fin round-overs self-supporting). Gasket in TPU 90–95A.

> ⚠️ **v0.2 — verify before printing.** Board dimensions are nominal and the
> hinge dimensions target GoPro compatibility but are printed parts: print the
> `bracket` + `knob` first and check the prong fit, then the shells. For the
> xiao host, **measure your seated stack first** (`stack_sock_h`,
> `xiao_usb_drop`, `xiao_usb_dx`) — socket and header heights vary between
> suppliers, and the two USB openings must land on your actual ports.

---

# Canary Vision — Doorbell (v0.2)

The stacked-XIAO Vision build in a **video-doorbell form factor**
([`canary_vision_doorbell.scad`](./canary_vision_doorbell.scad)): a slim
vertical pill, **31.8 × 113.2 × 24 mm** — the Ring Wired is 98 × 46 × 22, the
Wyze 93 × 41 × 22 — with the OV5647 behind a sealed disc at the top, the
module + stacked XIAO in the middle, and a **12 mm illuminated momentary
button** (short body, IP65, wired to the multifunction input; its LED ring
replaces the light pipe) at the bottom. **v0.2** sizes the board bay to the
measured Grove Vision AI V2 footprint (40 × 20 mm, mounted vertically — the
v0.1 bay assumed a 25 × 25 square the module never was): the side rails now run
the module's top half only so the 17.8 mm-wide XIAO clears as it hangs beneath,
and the T-studs moved out to ±34 mm so they clear the taller cable well.

![doorbell — body, face, plate and gasket](./preview_doorbell.png)

**Mounting is the doorbell pattern, not the hinge.** A thin **wall plate**
screws to the door frame (4 × #8/M4, counterbored — or print the included
**15° wedge** variant, `plate_wedge = 0/5/10/15`, to angle the camera toward
the walk-up). The body drops onto the plate's two **printed T-studs** (the
same blind, seal-safe pockets as the keyhole system) and locks with a hidden
**security screw** driven up through the plate's bottom foot into a blind
boss — Ring-style tool-only removal, and the pilot never breaches the seal
envelope.

**Power:** USB-C from the stack's ports loops through the internal cable well
and exits an oval in the **back**, through the matching plate hole, into the
wall or under trim — use a **right-angle USB-C plug** and seal the pass-through
with a grommet or silicone. (Native 16–24 VAC doorbell wiring needs a
rectifier/buck module — out of scope here; USB is the supported path.)

**Weather:** sealed **by default** (`opt_seal = true`): TPU gasket + drip-edge
face, no side openings at all — the only penetrations are the sealed lens
disc, the IP65 button, and the rear exit against the wall. Same honest ~IP54
rating; a porch or doorway soffit is its natural habitat.

**Assembly:** camera to the face posts + bond the disc → seat the XIAO in the
module (**USB-Cs same direction!**) and click the stack into the rails →
mount the button through the face, wire button/LED to the XIAO → gasket in
the groove, face on, 4 × M2 (black-oxide looks best) → plate on the frame,
cable through, hang the body on the studs, drive the security screw.

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `plate_wedge` / `plate_wedge_x` | 0 / 0 | wedge the plate vertically and/or left-right (corner installs) |
| `btn_d` / `btn_bez_d` / `btn_body_l` | 12 / 16.5 / 18 | match YOUR button (depth is assert-checked against the cavity) |
| `stack_sock_h`, `lens_dx/dy` | 11.5 / 0, 2.5 | **measure** your stack and lens, as with the Vision case |
| `usb_exit_*` | 12×7 oval | cable exit size/position (guarded against the stud pocket) |
| `sec_screw_d` | 2.2 | security screw — use a Torx/security drive |
| `opt_vent` | false | GORE vent cluster for wet climates |
| engineering trio | on | `lid_ribs`, `foot_cham`, `screw_insert` as on the other cases |

> ⚠️ **v0.1 — verify before printing.** Same rules as the Vision case: measure
> the seated stack and your button before printing; print the face first and
> test-fit the button and disc.

---

# Canary Sense — RADOME enclosure (v0.1)

Housing for the **canary-sense mmWave witness**
([design doc](../../canary_sense_mr60bha2_design.md)): the Seeed **MR60BHA2**
60 GHz radar carrier with a **stacked XIAO ESP32-C6**, ceiling- or
wall-mounted (bedside ≤ 1.5 m for the wellbeing/breathing channel). Outer
≈ **58 × 57 × 22.5 mm** + prongs; reuses the Vision case's GoPro hinge,
blind keyholes, bracket and knob (print those from the Vision file).

![canary-sense — back, radome front, bracket and knob](./preview_sense.png)

**The radome rule** — 60 GHz must pass through the front, so the window over
the antenna zone is a **thin, flat, uniform membrane** (`radome_t`, default
1.0 mm; ~1.5 mm ≈ a half-wave in plastic, also low-loss) with nothing
crossing it: the rib ring, label, gasket path and all features auto-clear it.
Use **unfilled PETG/ASA only** — carbon-filled filament, foil labels or paint
with metallic pigment in front of the antenna will blind the radar. The
**antenna-to-radome air gap is computed and asserted** (`rad_gap`, echoed at
render; ≈ 4.3 mm at defaults, ≥ 2.5 enforced — raise `cav_extra` for more)
from your measured `ant_h` (the AiP package top above the PCB). MEASURE the
antenna-zone position (`rad_dx/dy`) on your carrier revision.

Also on the face, outside the window: a **light pipe** for the onboard WS2812
and a small **aperture for the BH1750 lux sensor** (it needs to see room
light; glue a clear disc behind it when sealing). The stacked XIAO's USB-C
exits the bottom wall (`xiao_usb_z` — measure the seated stack).

| Param | Default | Why you'd change it |
|-------|--------:|---------------------|
| `radome_t` | 1.0 | radar window thickness; 1.5 ≈ half-wave alternative |
| `rad_win_x/y`, `rad_dx/dy` | 24×24 / 0, 6 | window size/position over the antenna — **measure** |
| `vm_l/vm_w`, `stack_sock_h`, `xiao_usb_z` | 44×36 / 11.5 / 4.0 | carrier + seated-stack dimensions — **measure** |
| `lux_dx/dy`, `lp_dx/dy` | — | sensor/LED positions from the board centre |
| `opt_seal`, `mount_style` | off / hinge | same systems as the Vision case |
| `radar` | `"bha2"` | `"fda2"` = the MR60**FDA2** fall-detection sibling: same carrier family and radome physics, but **ceiling-mount it** (keyholes), 2.4–3.1 m, facing straight down over the fall zone — and re-verify `rad_dx/dy` on that carrier |

> ⚠️ **v0.1 — verify before printing.** Carrier dimensions and the antenna
> zone are nominal; measure your kit revision. Print the front first and
> check the radome window lands over the antenna array.

## Links
- [Printing in PETG — Cura guide](./printing_petg_cura.md) — settings sheet, per-model cheat-sheet, importable profile
- [Field & environmental ratings](./field_ratings.md) — hardware limits, the CER ladder, home test protocols
- [Peripheral Build Plan & BOM](../canary_peripheral_build_plan.md) — parts, wiring, climate/IP guidance
- [Canary Sense design doc](../../canary_sense_mr60bha2_design.md) — the radar witness this houses
- [Bench bring-up](../bench_bringup.md) — get it chirping before you box it up
