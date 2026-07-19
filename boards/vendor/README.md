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
| `seeed_grove_vision_ai_v2.step.gz` | Grove Vision AI V2 (+ stand) | Seeed Studio | 101021040 | [wiki](https://wiki.seeedstudio.com/grove_vision_ai_v2/) |
| `seeed_round_display_xiao.step.gz` | Round Display for XIAO | Seeed Studio | 104040143 | [wiki](https://wiki.seeedstudio.com/get_start_round_display/) |

## Notes

- The STEPs are stored **gzip-compressed** (`*.step.gz`, ~1.4 MB total vs ~9 MB raw) to keep them out of git as multi-MB text blobs; `gen_boards.py` decompresses to a scratch file at generation time. Raw `*.step` drops are gitignored.

- **Grove Vision AI V2** ships here inside a printable stand + camera shroud +
  adapter assembly. `gen_boards.py` drops those mount solids by name
  (`mounting_plate`, `camera_mounting_shroud`, `adapter`) and keeps only the
  board + camera — see `boards/boards.config.json`.
- **Round Display** is a SolidWorks STEP export that did not carry per-solid
  colours through the tessellator, so its materials are assigned by geometry
  (dark PCB + grey glass). The *shape* is the vendor's exact model.
- The **XIAO ESP32-S3** (Sense) STEP is the Seeed board that also underlies the
  plain XIAO used by the display host; a daughterboard-detached variant is a
  planned addition.

## Licence

The Seeed XIAO series is **open hardware**; Seeed publishes CAD for these
products on their wiki and OPL. These files are redistributed here solely to
render the boards in SecuraCV's own offline viewer, with attribution to Seeed
Studio. **Verify the specific licence terms for each product on its vendor page
before redistributing these models outside this project.** If any vendor
requests removal, delete the STEP here and the viewer degrades gracefully to
"no board modeled for this device yet."
