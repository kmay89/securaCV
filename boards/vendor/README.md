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

## Licence

The Seeed XIAO series is **open hardware**; Seeed publishes CAD for these
products on their wiki and OPL. These files are redistributed here solely to
render the boards in SecuraCV's own offline viewer, with attribution to Seeed
Studio. **Verify the specific licence terms for each product on its vendor page
before redistributing these models outside this project.** If any vendor
requests removal, delete the STEP here and the viewer degrades gracefully to
"no board modeled for this device yet."
