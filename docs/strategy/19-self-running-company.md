# 19 — The self-running company: one human, GitHub, Claude — the rest is systems

**Thesis.** SecuraCV is operated as a company of one, and stays that way on
purpose. The design goal is not "small team efficiency" — it is that the
company's operations *are code*: every recurring task is a workflow, every
fact has exactly one source, every surface is generated from that source,
and a human appears only when a system raises an exception that needs a
decision. GitHub is the company's operating system; Claude is its operator
interface; the human is its judgment.

This doc is the constitution for that. Doc 12 (flight rules) governs how we
*engineer*; this governs how we *operate*. The public window into it is the
website's **Engine Room** page (securacv_website `engine.html`) — a separate
section from the Lab, because the Lab shows what the *devices* do and the
Engine Room shows what the *company* does.

## The five operating principles

1. **One canonical source per fact.** Every fact the company relies on —
   a part number, a price, a pin map, an enclosure dimension, a kit price —
   lives in exactly one file, and every page, table, or total that shows it
   is *generated*. This is Digi-Key's deep lesson: the magic users feel is
   one database feeding every surface, so no two surfaces ever disagree.
   (Repo practice already: `bom_*.csv` → `build.json` → Build-it page;
   `config.h` → `workshop.json`; enclosure `.scad` → the catalog.)

2. **Facts are fetched, never typed.** Anything the outside world knows
   better than we do — distributor stock, prices, lifecycle — is pulled by
   a scheduled system and stamped with provenance. A human typing a price
   into a file is a bug. This is Flux.ai's lesson: parts are live objects
   with supply-chain state attached by the system, at design time.
   (Implemented: `scripts/bom_pricing.py` + the nightly `bom-pricing.yml`
   snapshot — [`docs/hardware/bom_pipeline.md`](../hardware/bom_pipeline.md).)

3. **Drift is a red X, not a habit.** Wherever a generated artifact could
   go stale, CI regenerates and diffs it; wherever a hand-written file has
   a schema, a lint enforces it. The repo's existing drift gates
   (canary-local catalog, docs index, build matrix, dictionary sync) are
   the pattern; every new system ships with its gate in the same PR.

4. **Exceptions summon humans; schedules never do.** No calendar chores,
   no "weekly BOM review," no dashboards anyone must remember to look at.
   Systems watch, and when something crosses a threshold they open a
   *deduplicated GitHub issue* with a specific, actionable signal (e.g.
   `supply-chain [out-of-stock]: MR60BHA2`). Silence means healthy. The
   inbox of the company **is the issue tracker** — one queue, fully
   machine- and Claude-readable.

5. **The whole company fits in a context window.** Every runbook, policy,
   price rationale, and decision record lives in the repo, written so an
   agent can execute it: exact commands, exact file paths, explicit
   pass/fail criteria. That's what makes "1 person + Claude" real rather
   than aspirational — the operator interface can *read the whole business*
   and act on any part of it, and its work is forced through the same
   gates (lint, drift, review) as anyone else's.

## The system map — every function, its system, its state

| Function | System (source → automation → surface) | State |
|---|---|---|
| **Parts & sourcing** | `bom_*.csv` → `bom_pricing.py` nightly + exception issues → `pricing.json` → Build-it page | **running** (add distributor API keys to go live-verified) |
| **Hardware data** | `.scad` customizer + enclosure README → `gen_enclosures.py` → catalog/workshop JSONs, drift-gated | running |
| **Enclosure validation** | `enclosure.yml`: OpenSCAD render + mesh checks on every change | running |
| **Firmware** | per-project CI builds, size guards, host tests; signed pull-OTA manifests | running |
| **Software supply chain** | `sbom.yml`: CycloneDX SBOM per release | running |
| **Docs integrity** | `lint_docs_index.py`: no stragglers, no dead links | running |
| **Storefront** | `store.json` (single source: SKUs, prices, stock caps) → `store.js`; live mode = Stripe Payment Links, $0 fixed cost | preview mode by design |
| **Margin guard** | kit COGS from `pricing.json` vs `store.json` price → warn under target margin | **next** (roadmap item 2 in the BOM pipeline doc) |
| **Fulfillment** | weekly batch runbook (`store-README.md`): Payment Links → PirateShip labels → batch print/flash evenings | systemized, human-executed (physical) |
| **Support & community** | GitHub issues as the single front door; `builds.json` gallery; docs written to be self-serve | running |
| **Marketing site** | static pages + facts tests + screenshot workflow; every claim drift-tested against the repo | running |
| **Operations window** | the Engine Room page: the system map, live workflow links, this doctrine — public | **this PR** |

## The operator loop

```
      (world moves: part EOLs, price jumps, order lands, CI breaks)
                              ▼
              a workflow notices and opens an issue
                              ▼
   the human reads the issue — or points Claude at it ("fix #123")
                              ▼
        Claude session: reads runbook + sources, prepares the PR
                              ▼
      gates decide (lint, drift, tests) — not opinion, not memory
                              ▼
        merge → the nightly systems verify the fix themselves
```

The human's actual job reduces to three things systems cannot do:
**judgment** (is this ALT part acceptable? is $69 still the right price?),
**physical acts** (printing, kitting, shipping), and **being accountable**
(money, safety, legal — see below).

## What deliberately stays human

- **Money movement** — Stripe going live, refunds, distributor *orders*.
  The pipeline may prepare carts and quotes; a human clicks buy. (The
  Digi-Key Ordering API stays on the roadmap until batch volume justifies
  a machine spending money — and then only with caps.)
- **Safety-critical choices** — battery chemistry/certification rows in
  the BOMs are marked SAFETY-CRITICAL; no automation may substitute them.
- **Prices customers pay** — systems compute COGS and flag margin drift;
  the number on the store page is a human decision, on the record.
- **The brand's word** — honesty copy, privacy promises, this doctrine.

## What "fully documented" means here

Documentation is not a description of the company — it **is** the company.
The test for every doc: *could Claude, with only the repo, execute this
function tomorrow?* If not, the doc is missing steps, and that's a bug.
New systems therefore ship docs-first: source of truth, automation, gate,
exception policy, runbook — the BOM pipeline doc is the template.

## Tripwires (when this doctrine is wrong)

- If an exception issue fires nightly and keeps being closed without a CSV
  or policy change, the threshold is wrong — fix the system, not the habit.
- If any fact acquires a second source that must be kept in sync by hand,
  stop and collapse it — that's how companies grow humans.
- If a function's runbook needs tribal knowledge to execute, it isn't
  documented yet; the next Claude session on it should end in a doc PR.
