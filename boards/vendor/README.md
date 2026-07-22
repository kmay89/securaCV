# Vendor board CAD — provenance

These are **vendor-published CAD models** of the off-the-shelf boards the Canary
family runs on. They are the *source of truth* for the local board viewer, the
way each enclosure's `.scad` is the source of truth for its printed parts:
`canary-local/tools/gen_boards.py` decompresses each gzipped STEP and tessellates it to a committed GLB
(`canary-local/boards/*.glb`) that the offline page renders in its own WebGL
viewer. Nothing here is fetched at runtime — the page ships the derived mesh
(Invariant IV).

| File | Board | Vendor | Product | Source |
|---|---|---|---|---|
| `seeed_xiao_esp32s3_sense.step.gz` | XIAO ESP32-S3 Sense | Seeed Studio | 102010469 | [wiki](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) |
| `seeed_xiao_esp32s3.step.gz` | XIAO ESP32-S3 (plain) | Seeed Studio | 113991114 | [wiki](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/) |
| `seeed_grove_vision_ai_v2.step.gz` | Grove Vision AI V2 (+ stand) | Seeed Studio | 101021040 | [wiki](https://wiki.seeedstudio.com/grove_vision_ai_v2/) |
| `seeed_round_display_xiao.step.gz` | Round Display for XIAO | Seeed Studio | 104040143 | [wiki](https://wiki.seeedstudio.com/get_start_round_display/) |
| `quectel_l76k_gnss.step.gz` | L76K GNSS Module for XIAO | Seeed Studio | — | [wiki](https://wiki.seeedstudio.com/Get_Started_with_L76K_GNSS_Module_for_XIAO/) |
| `raspberry_pi_5.step.gz` | Raspberry Pi 5 (the hub) | Raspberry Pi Ltd | SC1111 | [docs](https://www.raspberrypi.com/documentation/computers/raspberry-pi.html) |
| *(no file — procedural)* | Waveshare ESP32-S3-Touch-LCD-4.3B (the Dash) | Waveshare | ESP32-S3-Touch-LCD-4.3B | [wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.3B) |

## Notes

- The STEPs are stored **gzip-compressed** (`*.step.gz`, ~1.4 MB total vs ~9 MB raw) to keep them out of git as multi-MB text blobs; `gen_boards.py` decompresses to a scratch file at generation time. Raw `*.step` drops are gitignored.

- **Grove Vision AI V2** ships here inside a printable stand + camera shroud +
  adapter assembly. The loose camera drops by name (`rpi cam`), but
  `merge_primitives` fuses the printed stand's solids into unnamed material
  buckets that a name match can't reach — so the stand is removed by a Y-plane
  cut (`drop_below_y`) instead: the board's components all sit above a clean air
  gap over the stand. The result is just the ~22.5 mm module — see
  `boards/boards.config.json`.
- **Round Display** is a SolidWorks STEP export that did not carry per-solid
  colours through the tessellator, so its materials are assigned by geometry
  (dark PCB + grey glass). The *shape* is the vendor's exact model.
- The **XIAO ESP32-S3** comes in two entries: the **Sense** (with the stacked
  camera/mic daughterboard, used by the WAP) and the **plain** base module (no
  daughterboard, the Watch Station host that seats in the Round Display socket).
  Both are the same Seeed open-hardware board; the plain STEP sits in its own
  coordinate frame (USB-C at +X), so its 14 castellation pads are authored fresh
  against its own mesh.
- The **Raspberry Pi 5** is the largest source here (~7.5 MB gzipped, the vendor
  ships a very detailed ~12k-solid STEP). It is the Home Assistant **hub**, not a
  Canary device board, so it carries no device mapping and lives in the Board
  Room only. Its STEP has no per-solid colours, so `gen_boards.py`
  (`materials: "raspberry-pi"`) assigns them from the vendor's own solid names —
  green PCB, metal USB/Ethernet/HDMI shells, gold GPIO, black silicon — and
  concatenates by colour so the committed GLB stays a handful of parts. It is
  tessellated coarser (`tol_linear` 0.35 mm) than the small boards.
- The **Waveshare ESP32-S3-Touch-LCD-4.3B** (the Dash) has **no vendor CAD** —
  Waveshare doesn't publish a STEP for it — so there is no file in this
  directory. Instead `gen_boards.py` *builds* it procedurally
  (`source: "procedural"`, `builder: "waveshare_4_3b"` → `build_waveshare_4_3b`,
  using `shapely` + `manifold3d` for the rounded shell and boolean cutouts): a
  **dimensional model reverse-engineered** — a rounded charcoal ABS case with a
  recessed glossy 4.3″ panel on the front, the green 16-way pluggable terminal on
  the **rear** (opposite the screen, where the field wiring exits — as on the
  real board), TF + dual USB-C + BOOT/RESET on one short edge, and status LEDs +
  a power slide on the other. Its proportions and exposed-feature layout are taken from **MaffooClock's
  published shell for the 5″ sibling** (ESP32-S3-Touch-LCD-5/5B, on Printables) —
  used **as a dimensional reference only, not copied or redistributed** (see the
  licence note below) — scaled to the 4.3B and cross-checked against
  `firmware/boards/waveshare-esp32s3-lcd43b` and the owner's photos. The case and
  screen are measured approximations; the green **16-position terminal block** —
  its 3.81 mm pitch, silk order and group labels (`6~36V VIN·GND / I2C
  VOUT·GND·SDA·SCL / CAN L·H / RS485 B·A / Iso-I/O DO0·DO1·DI-COM·GND·DI0·DI1`) —
  is transcribed from the firmware silk table. The 16 terminal anchors are read
  straight off the model (it was built here, so their centres are known — no
  island detection). Honestly labelled a reverse-engineered dimensional model in
  its provenance and the viewer ribbon, like the Round Display's geometry-assigned
  materials. Being code-generated (not a nondeterministic STEP tessellation), its
  GLB is deterministic and byte-stable.

## Licence

The Seeed XIAO series is **open hardware**; Seeed publishes CAD for these
products on their wiki and OPL. These files are redistributed here solely to
render the boards in SecuraCV's own offline viewer, with attribution to Seeed
Studio. **Verify the specific licence terms for each product on its vendor page
before redistributing these models outside this project.** If any vendor
requests removal, delete the STEP here and the viewer degrades gracefully to
"no board modeled for this device yet."

The **Waveshare** board is the exception: no vendor CAD is redistributed (there
is none). Its GLB is our own geometry, authored procedurally from measurements
and the vendor's published silk/spec — so it carries no vendor-CAD licence, only
the usual caveat that it's a dimensional approximation, not the manufacturer's
drawing. Its proportions were informed by **MaffooClock's "Shell for Waveshare
ESP32-S3 5in Touch Display"** (Printables, **CC-BY-NC-SA 4.0**). That model is a
*noncommercial* work, so **none of its mesh is copied, tessellated, or
redistributed here** — it was used purely as a dimensional reference (the kind of
measurement/fact use that sits outside the licence's copyleft), with attribution
recorded here and in the board's provenance string. If a cleaner primary source
(vendor drawing, direct measurement) supersedes it, drop the reference.
