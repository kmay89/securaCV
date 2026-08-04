# The flagship board program — a custom carrier, and how to certify it cheaply

> **What this is:** the decision record for spinning SecuraCV's first custom
> PCB — the **Canary Witness S3 carrier** ([`boards/canary-witness-s3/`](../../boards/canary-witness-s3/)) —
> covering which EDA tool does what, what the board actually saves (computed,
> not typed), and the pipeline that gets it from intent to a certified unit
> using tooling this repo already runs.
>
> **Status: `idea`.** No fabrication files exist, nothing has been ordered, no
> board has been populated. The confidence ladder in
> [`canary-local/devices/figures.json`](../../canary-local/devices/figures.json)
> governs this the same as anything else: it renders as a ghost until a
> populated board passes [`v1_bench_validation_runbook.md`](./v1_bench_validation_runbook.md).
> This document authorizes nothing; it makes the case legible.

---

## 0 · The verdict, up front

**Design in Flux, but do not let Flux hold the design.** Flux's copilot is
genuinely good at the slow part of board work — turning a datasheet into a
correct symbol and footprint — and its parts carry live supply-chain state,
which is the very idea [`bom_pipeline.md`](./bom_pipeline.md) was built
around. What it cannot be is our source of truth: the design would live in a
vendor's cloud, behind a paid seat, in a format CI cannot diff. Every other
artifact in this repo is generated, committed, and byte-checked. A schematic
that only exists in a browser tab fails that test on all three counts.

**On cost: the interesting number is not the saving, it is the break-even.**
Doc 18 §6 says *"do not spin a custom PCB until >500 committed units,"* and
that conclusion is sound for the assumptions it makes — it budgets
**$3,000–8,000 of paid PCB design**. Take that line to zero by doing the
design in-house and the arithmetic changes completely: NRE falls to **$154**
and the board pays for itself at **23 units**, not 500. The saving per unit is
*smaller* than doc 18 estimated (**$6.78**, not $15–25) because the WAP's
module is a $15.90 XIAO, not the $29 Grove module doc 18 was reasoning about.
Both corrections point the same way — the barrier was never the per-unit
economics, it was the NRE, and the NRE is mostly a choice.

**On certification: the timing matters more than the amount.** A Part 15
Subpart B SDoC costs $1,500–4,000 **per SKU** and is owed for anything with a
radio in the box, custom board or not. It attaches to the SKU, so it is paid
again when the SKU's hardware materially changes. SecuraCV has certified
nothing yet ([`fcc_board_status.md`](./fcc_board_status.md) is ❌ down the
"grant conditions read" column). **That makes right now the cheapest moment in
this project's life to change the board** — every month spent shipping a
XIAO-based SKU first is a month that turns one SDoC into two.

---

## 1 · Do we know how to use Flux, and where does it fit?

Yes, and the honest answer has two halves.

**What Flux is good at.** Browser-native ECAD with real-time collaboration and
an AI copilot that generates symbols and footprints from datasheets, plus
parts that carry stock, price and lifecycle as live properties rather than
typed cells. That last idea is not new to us — `bom_pipeline.md` names
"Flux.ai's lesson" as its founding principle: *a part is a live object, not a
row in a file.* We already built the pipeline around it. Using the tool that
inspired it for the capture step is consistent, not a reversal.

**What disqualifies it as the source of truth.**

| Property | Flux | What this repo requires |
|---|---|---|
| Source format | project lives in Flux's cloud; exports are **outputs** (Gerber, drill, BOM, pick-and-place, `.d356`) | a committed, diffable file CI can byte-check |
| Round-trip | no export back into KiCad — manufacturing files out, design does not come back | rename/refactor must be reviewable in a PR diff |
| Access | private projects and AI features require a paid seat | any contributor, offline, at zero cost |
| Review | changes are not a diff | `git diff` is how every other decision here is reviewed |

None of that makes Flux a bad tool. It makes it the wrong place for the
*record*. So we split the work along the line where each side is strongest:

### What this repo generates, and what the EDA tool generates

**The repo owns intent.** [`board.json`](../../boards/canary-witness-s3/board.json)
holds the part list (MPN-keyed, so it joins the existing pricing snapshot), the
connectivity **keyed by signal name and GPIO**, and every fabrication
assumption with a stated basis.

**The EDA tool owns physics.** Package pin numbers, footprints, courtyards,
copper, impedance, the antenna keep-out. These come off datasheets and are
checked by the tool's own ERC/DRC.

The boundary is deliberate and it is drawn where accuracy is verifiable.
`board.json` carries **no ESP32-S3-WROOM-1 package pin numbers**, because
writing "IO42 is pin 35" from memory produces exactly the confident-but-wrong
artifact the rest of this repo exists to prevent. Signal names and GPIO
numbers we can verify against the firmware on every CI run. Pin numbers we
cannot — so we do not assert them, and the tool that reads the datasheet
supplies them at capture time.

---

## 2 · What the board is

An **ESP32-S3-WROOM-1 carrier that is pin-for-pin identical to the XIAO
ESP32-S3 Sense** on every signal the firmware names. That single constraint is
the whole program:

- **The firmware does not change.** `xiao-esp32s3-sense` is `verified` tier in
  [`firmware/boards/boards.json`](../../firmware/boards/boards.json). A carrier
  that keeps its pin map inherits that verification instead of restarting it.
- **The bench runbook does not change.** The existing v1 validation runbook is
  the acceptance test for the new board, unmodified.
- **A carrier that needs a firmware change has given back the saving it was
  built for** — so [`scripts/lint_board_design.py`](../../scripts/lint_board_design.py)
  fails CI if any GPIO drifts from
  [`firmware/boards/xiao-esp32s3-sense/pins/pins.h`](../../firmware/boards/xiao-esp32s3-sense/pins/pins.h).
  25 signals are held that way; only the chirp pin is unenforced, because the
  Sense's header declares no buzzer constant to hold it against.

### It also fixes a real defect, for free

`pins.h` defines **`SD_PIN_CS 21`** and **`LED_BUILTIN 21`** — the same pin —
and says so plainly:

> *"While the SD card is mounted, GPIO21 belongs to the SD driver: writing the
> LED asserts/deasserts the card's CS, and doing so mid-transaction corrupts SD
> I/O."*

On the XIAO that is unavoidable silicon, and the firmware pays for it forever
in gating logic. On a board we lay out, it simply does not exist: SD_CS stays
on GPIO 21 for pin-compatibility, and the only status indicator goes to GPIO 3
(`EXT_LED_PIN_DEFAULT`) — the escape hatch the firmware already documents and
already supports. **This is the clearest evidence that a carrier buys
something other than money.** No new code, no new risk, one less way to
corrupt evidence on disk.

### The radio is the part we do not design

The board uses the **pre-certified ESP32-S3-WROOM-1 module**, never bare
ESP32-S3 silicon. This is the one shortcut doc 18 §4 confirms is real: modular
approval saves the **$5,000–15,000** intentional-radiator test. It is
conditional — antenna keep-out honored, nothing metal or battery in it, host
labeled with the module's FCC ID — and violating any condition forfeits the
grant. The PCB-antenna variant (not `-1U`) is chosen deliberately so the
antenna condition is satisfied by the module itself, which also deletes the
$2.20 u.FL whip from the BOM.

---

## 3 · The cost case, computed

Run it yourself — nothing below is typed into a document:

```sh
python3 scripts/gen_board_design.py          # writes cost_model.json
python3 scripts/gen_board_design.py --check  # CI: fails if stale
```

[`gen_board_design.py`](../../scripts/gen_board_design.py) joins `board.json`
against the live snapshot in [`pricing.json`](./pricing.json) and against the
WAP's own BOM CSV, so when the XIAO's price moves, the break-even moves with
it.

| | |
|---|---|
| Carrier, per unit | **$11.68** (parts $9.68 + PCB $0.80 + assembly $1.20) |
| Module-build rows it replaces | **$18.46** |
| **Saving per unit** | **$6.78** |
| NRE | **$154** — design $0, PCBA setup $100, 18 unique parts × $3 |
| **Break-even** | **23 units** |

### Read it with its own confidence numbers

The model reports how much of its own answer it actually knows, because a cost
model that hides its confidence is how a project talks itself into tooling it
cannot afford.

- **58% of the compared dollars are distributor-verified**; $11.86 of $28.14
  is estimate. Every estimated line carries a `basis` string, and
  `lint_board_design.py` fails the build on any that does not.
- **The comparison is volume-mismatched, in the direction that flatters us.**
  The carrier is priced at 1k reel estimates; the replaced side is mostly
  qty-1, because Digi-Key publishes **no volume break for the XIAO** at all.
  The model names this explicitly: up to **$18.41** of the replaced side is
  priced below the 1k basis.

So the honest headline is not "$6.78." It is the sensitivity, which needs no
estimate at all — only the price of one part:

> **The carrier is cheaper while the XIAO ESP32-S3 Sense costs more than
> $9.12/unit.** It is modeled at $15.90 at qty 1.

That is the number to take to a real volume quote. If Seeed quotes the Sense
above ~$9.12 at the volume we would actually buy, the carrier wins and the
break-even is real. If they quote below it, this program should stop — and the
model will say so without anyone having to re-argue it.

---

## 4 · The pipeline

Every stage below either exists today or is a thin wrapper on something that
does. Nothing here is a new system.

```
board.json ──► lint_board_design.py ──► gen_board_design.py ──► cost_model.json
 (intent)      (pins match firmware)     (joins pricing.json)     connections.csv
                                                                        │
                                          ┌─────────────────────────────┘
                                          ▼
                                   capture in Flux/KiCad
                                   (footprints, layout, DRC)
                                          │
                                          ▼
                              Gerber + BOM + pick-and-place
                                          │
                                          ▼
                                    JLCPCB / PCBWay
                                          │
                                          ▼
                          flash existing firmware, unmodified
                                          │
                                          ▼
                          v1_bench_validation_runbook.md  ◄── acceptance
                                          │
                                          ▼
                          signed first-boot self-test = QC
```

**1 · Intent, gated.** `board.json` is reviewed as a diff. Two gates hold it:
pin drift against the firmware header, and netlist coherence (orphan nets,
undeclared parts, a net whose GPIO disagrees with the pin map). All four defect
classes are tested — inject one and the linter names it.

**2 · Cost, computed.** `gen_board_design.py --check` is a byte-diff gate, the
same pattern as every other generator here. Nobody can hand-edit a saving into
the record.

**3 · Capture.** Flux (or KiCad) reads `connections.csv` as the netlist
starting point and supplies footprints and layout. **This is the step to buy a
Flux seat for, and only this step.**

**4 · Fabricate.** Flux exports Gerbers, drill, BOM and pick-and-place directly
to JLCPCB/PCBWay — no lock-in on the manufacturing side, which is the half that
matters once the design record lives here.

**5 · Accept.** The firmware is unchanged, so the existing bench runbook is the
acceptance test. This is the payoff of the pin-compatibility constraint.

**6 · QC at volume, unfakeable.** Doc 25 §0.3 already makes this point: the
**signed first-boot self-test is cryptographic functional acceptance.** It is
the structural reason assembly can be outsourced without outsourcing trust —
and it costs nothing extra here, because it already exists.

---

## 5 · Certification — cheap, in the right order

Read [`docs/strategy/29-fcc-and-product-compliance-diligence.md`](../strategy/29-fcc-and-product-compliance-diligence.md)
first; this section only adds the sequencing.

| Item | Cost | Does the carrier change it? |
|---|---|---|
| Intentional-radiator test | $5,000–15,000 | **Avoided** — inherited from the module's grant, if its conditions are honored |
| Part 15B SDoC, per SKU | $1,500–4,000 | **No** — owed for any radio in the box, module or custom |
| Tooling / molds | $3,000–10,000 | **No** — enclosures stay printed; doc 18 §3b is untouched |

Three things follow, in order:

1. **Do the `fcc_board_status.md` homework now.** It has no row for
   ESP32-S3-WROOM-1 and no grant PDF has been opened. That lookup is free, it
   is the gate on the only five-figure saving in this program, and it can be
   done today. Until that column says ✅, modular approval is *promising and
   unconfirmed* — the posture that document requires.
2. **Change the board before the first SDoC, not after.** The SDoC attaches to
   the SKU. Certifying a XIAO-based SKU and then moving to a carrier pays it
   twice; moving first pays it once.
3. **Nothing here unlocks selling a radio.** The plastic-only posture (doc 29
   §8) holds unchanged: printed parts sell, radio SKUs stay display-only with
   the 2.803 disclosure. This program makes crossing that line cheaper and
   better-evidenced whenever it is deliberately chosen — it does not cross it.

---

## 6 · What is committed, and what is not

**Committed:** the design intent, the two gates, the computed cost model, and
the connection table.

```
boards/canary-witness-s3/board.json         design intent (hand-edited, reviewed)
boards/canary-witness-s3/cost_model.json    GENERATED — do not hand-edit
boards/canary-witness-s3/connections.csv    GENERATED — do not hand-edit
scripts/gen_board_design.py                 the generator (--check gates CI)
scripts/lint_board_design.py                pin-drift + netlist coherence gate
```

**Deliberately not committed:** no schematic, no layout, no Gerbers, no
footprints — and **no BOM under `docs/hardware/bom_*.csv`**. That last one is
not an oversight. Any `bom_*.csv` there must be wired into
`gen_enclosures.py`'s `BOM_MAP`, which flows it to `build.json` and onto the
website's Build-it page. This board is an `idea`; putting it there would offer
visitors parts for a device that does not exist. It joins the store when it
earns `shipping`, and not before.

### Open questions before capture

Both live in `board.json` with their reasoning; neither is resolvable from
documentation:

- **Rail headroom.** The regulator is specified at 600 mA. Camera capture plus
  a Wi-Fi TX burst is the worst case, and the XIAO's own regulator is the only
  evidence we have that it fits. *Measure the XIAO's rail current during a PEEK
  plus uplink before committing the part* — an under-specified rail browns out
  under exactly the condition the device exists for.
- **GPIO 2 is a strapping pin.** The chirp drive must not hold it at reset.
  `firmware/boards/PIN_BUDGET.md` already flags it conditional for this reason;
  check the boot-mode level at capture.

---

## 7 · The recommendation

1. **Look up the ESP32-S3-WROOM-1 grant** and fill in its
   `fcc_board_status.md` row. Free, today, and it gates $5–15k.
2. **Get one volume quote on the XIAO ESP32-S3 Sense.** One number decides
   this program, and the model already says which side of $9.12 it has to land
   on.
3. **If it clears, buy one Flux seat and capture the board.** The intent, the
   gates and the cost model are already here; capture is the only step that
   needs a tool we do not run.
4. **Do not order anything until the bench runbook has a plan for the two open
   questions.** They are cheap to answer on a XIAO and expensive to discover on
   a populated carrier.

## Sources

Internal: [`docs/strategy/18-unit-economics-and-production-scale.md`](../strategy/18-unit-economics-and-production-scale.md)
§2–§4 and §6 · [`docs/strategy/25-supplier-and-volume-production.md`](../strategy/25-supplier-and-volume-production.md)
§0–§1 · [`docs/strategy/29-fcc-and-product-compliance-diligence.md`](../strategy/29-fcc-and-product-compliance-diligence.md)
· [`bom_pipeline.md`](./bom_pipeline.md) · [`fcc_board_status.md`](./fcc_board_status.md)
· [`firmware/boards/xiao-esp32s3-sense/pins/pins.h`](../../firmware/boards/xiao-esp32s3-sense/pins/pins.h)

External, on Flux's capabilities and limits: [Flux vs KiCad (Flux's own comparison)](https://www.flux.ai/p/compare/flux-vs-kicad-comparison)
· [Flux PCB editor documentation](https://docs.flux.ai/faq/faq-s-about-the-pcb-editor)
· [Converting Flux.ai projects to KiCad — KiCad forum](https://forum.kicad.info/t/converting-flux-ai-projects-to-kicad/59947)
· [Designing a PCB with Flux.ai — JLCPCB](https://jlcpcb.com/blog/how-to-design-a-pcb-with-fluxai).
Flux's export set and the absence of a design round-trip are drawn from these;
seat pricing and free-tier terms change often and should be re-checked against
[flux.ai](https://www.flux.ai/) before a seat is bought.
