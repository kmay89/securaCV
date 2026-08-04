# Canary Witness S3 carrier — design intent

**Status: `idea`.** No schematic, no layout, no fabrication files, nothing
ordered, no board populated. Read
[`docs/hardware/flagship_board_program.md`](../../docs/hardware/flagship_board_program.md)
for the decision record — why this board, why Flux designs it but does not hold
it, and what has to be true before anyone spends money.

This directory is the *design intent* half of a custom PCB, in the same shape
as everything else here: a hand-edited source, a generator, and gates.

| File | Who writes it |
|---|---|
| `board.json` | **a human** — parts, connectivity, fabrication assumptions, open questions |
| `cost_model.json` | `scripts/gen_board_design.py` — **generated, do not hand-edit** |
| `connections.csv` | `scripts/gen_board_design.py` — **generated, do not hand-edit** |

```sh
python3 scripts/lint_board_design.py         # pins match firmware; nets cohere
python3 scripts/gen_board_design.py          # rebuild the model + connections
python3 scripts/gen_board_design.py --check  # what CI runs
```

## The one thing to understand before editing

The carrier is **pin-for-pin identical to the XIAO ESP32-S3 Sense** on every
signal the firmware names. That is not a nicety — it is the entire cost case.
It means the `verified`-tier firmware runs unmodified and
[`v1_bench_validation_runbook.md`](../../docs/hardware/v1_bench_validation_runbook.md)
is the acceptance test rather than a new one someone has to write.

So 25 signals in `pin_map` are gated against
[`firmware/boards/xiao-esp32s3-sense/pins/pins.h`](../../firmware/boards/xiao-esp32s3-sense/pins/pins.h)
on every CI run. **If that header moves and this file does not, the build goes
red** — because a carrier that needs a firmware change has given back the
saving it was built for.

## What is deliberately absent

- **No package pin numbers.** Nets are keyed by signal name and GPIO. WROOM-1
  pin numbers come off the datasheet during schematic capture, checked by the
  EDA tool's own ERC. Writing them here from memory would produce exactly the
  confident-but-wrong artifact these gates exist to prevent.
- **No `docs/hardware/bom_*.csv`.** A BOM there flows through
  `gen_enclosures.py`'s `BOM_MAP` to `build.json` and onto the website's
  Build-it page. This board is an `idea`; listing parts for it would offer
  visitors a device that does not exist. It joins the store at `shipping`.
