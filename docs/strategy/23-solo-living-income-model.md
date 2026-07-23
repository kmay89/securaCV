# 23 — The solo living: can one person actually make a living on this?

> The honest income model behind [22 — batch & education fulfillment](22-rapid-batch-and-education-fulfillment.md)
> and [20 — the self-running company](20-self-running-company.md). One person doing
> **every** job — product, sales, marketing, assembly, shipping, support, admin —
> as automated as possible, with **all** real costs counted: Claude, insurance,
> health care, taxes. The question isn't "is there margin" (doc 22 settled that);
> it's **"does the margin, minus everything, minus the hours one human has, add up
> to a living?"**
>
> **Epistemic note:** unit economics are firm (priced BOMs + doc 22). Overhead,
> health insurance, tax rates, and labor-per-unit are **estimates** — US-solo,
> mid-2026, single filer; they swing by state, age, and filing status. Treat the
> shape as sound and the exact dollars as a planning model, not a guarantee.

## 1. The answer, up front

**Yes — a modest living (~$50–60k take-home) is realistic solo at roughly
80–130 Canaries/month**, depending on product mix. But the ceiling isn't money,
it's **hours**: as long as one person hand-assembles, ~100–150 units/month is the
physical cap while still doing every other job. Earning a *comfortable* living
(~$75–100k) from hand-built hardware alone runs into that wall — and the way
through is **not grinding more WAP kits by hand**. It's:

1. **Mix up** — Vision and assembled units carry ~2–3× the contribution of a WAP kit.
2. **The attestation service** (doc 08 §8) — high-margin, near-zero marginal labor,
   recurring. This is the lever that turns a hardware grind into a living wage,
   because it makes money while you sleep instead of costing you an assembly hour.
3. **Outsource assembly** at volume (doc 16 Phase 3) — trade per-unit margin for
   removing the hour ceiling entirely.

Claude is what makes "one person doing every job" credible at all (§4): it's the
~$250/month that replaces the ~1–2 people of ops/support/marketing you'd otherwise
need — the difference between this being a business and a second full-time job.

## 2. Per-unit cash economics (founder labor stripped out)

For an income model, **don't count the founder's own labor as a cash cost** — the
leftover *is* their pay. (Doc 22's loaded-cost table folded labor in to justify
*price*; here we strip it back out to see what actually lands in a pocket.) Cash
cost = parts + outbound shipping + payment fees + returns/DOA reserve:

| Kit | Price | Parts | +Ship | +Fees ~3% | +Returns reserve | **Cash cost** | **Cash contribution** |
|---|---|---|---|---|---|---|---|
| **WAP** (education $89) | $89 | $26 | $4 | ~$2.9 | ~$5 | **~$38** | **~$51** |
| **WAP** (list $99) | $99 | $26 | $4 | ~$3.2 | ~$5 | ~$38 | ~$61 |
| **Vision** (education $135) | $135 | $34 | $4 | ~$4.2 | ~$6 | **~$48** | **~$87** |
| **Vision** (list $149) | $149 | $34 | $4 | ~$4.6 | ~$6 | ~$49 | ~$100 |
| **Assembled Vision** ($189) | $189 | $34 | $4 | ~$5.8 | ~$6 | ~$50 | ~$139 |

**Blended cash contribution per unit** depends entirely on mix:
- **Conservative** (mostly WAP kits): **~$55/unit.**
- **Optimistic** (Vision-heavy + some assembled): **~$90/unit.**

That 1.6× spread is the whole game — it's why "sell more Vision, not more WAP" and
"add the attestation service" matter more than any cost cut.

## 3. Fixed monthly overhead — the "everything"

Everything that costs money whether you sell 10 units or 200 (US solo, mid-2026,
estimated):

| Line | ~$/mo | Note |
|---|---|---|
| **Claude** (Max plan + some API for ops/support automation) | ~$250 | the "staff" — see §4 |
| **Health insurance** (ACA individual) | ~$450 | **the #1 swing variable** — $0 if on a spouse's plan, $600+ family |
| **Business + product liability insurance** | ~$100 | non-optional for a device marketed near "evidence" |
| **Legal / LLC / accounting** (annualized) | ~$200 | registered agent, CPA at tax time, occasional legal |
| **Printer depreciation + non-unit equipment** | ~$100 | 2–4 Bambu-class over 3 yrs; filament is in COGS |
| **Marketing / creator seeding / samples** | ~$150 | modest, mostly organic (doc 15 channels) |
| **Hosting / domain / email / misc SaaS** | ~$80 | static site is ~free; Workspace, analytics VPS, tools |
| **Total** | **~$1,330/mo** | ≈ **$16k/yr** |

Health insurance is ~a third of it and the most personal variable — the model
below shows take-home *with* it in; subtract ~$5,400/yr if it's covered elsewhere.

## 4. Automation — why one person can do every job (and where Claude fits)

The self-running-company thesis (doc 20) is what makes the hours math survivable.
The point of Claude here is **labor substitution**, not novelty:

- **Sales/marketing:** Claude drafts listings, channel posts, comparison copy,
  release notes — the doc-15 playbook executed without a marketing hire.
- **Support:** first-line triage and answers from the docs; the founder only
  touches escalations. This is the single biggest hour-saver at volume, where
  support otherwise scales linearly with units.
- **Ops:** BOM/stock/EOL monitoring (doc 19), fleet-manifest generation, the
  shipping/label flow, drift gates. Facts are fetched, not hand-typed (doc 20).
- **Checkout/fulfillment is already near-zero-touch:** Stripe Payment Links
  (checkout), PirateShip (labels), the batch flash jig (100 units ≈ 1 hr, doc 16
  §3.2). No storefront subscription, no per-order data entry.

At ~$250/month, Claude is replacing what would plausibly be **1–2 part-time hires**
(a VA for support + a marketing freelancer). That's the leverage that converts
"a second full-time job" into "a business one person runs."

**What Claude cannot remove:** screwing boards into cases and putting them in
boxes. Physical assembly is the residual human labor and the real ceiling (§5).

## 5. The living-wage table (take-home after tax)

Take-home ≈ `12 × (units × contribution − overhead) × (1 − tax)`, with overhead
~$1,330/mo and a blended **~25% effective tax** (15.3% self-employment + modest
federal; estimated for this income band). Units/month needed to hit a take-home:

| Target take-home | @ ~$55 contribution (WAP-heavy) | @ ~$90 contribution (Vision-heavy) | Feasible solo by hand? |
|---|---|---|---|
| **$50k** | **~126 units/mo** | **~77 units/mo** | Optimistic mix: comfortably. Conservative: at the edge. |
| **$75k** | ~176/mo | **~108/mo** | Only the optimistic mix — and near the ceiling. |
| **$100k** | ~227/mo | ~138/mo | Not by hand; needs outsourced assembly or attestation revenue. |

Reading it straight: **the realistic solo living is the $50–60k row** — ~80 units/mo
if you lean Vision, ~125/mo if you lean WAP. Everything above it is gated by hours,
not demand.

## 6. The hours ceiling (why the table stops where it does)

Human touch per unit — assemble + flash + QC + label + pack — is ~**30–40 min**,
plus support (~10% of units generate a ~15–30 min touch). If fulfillment gets
~20 hrs/week (the rest of a ~50-hr week goes to product, marketing, admin), that's
~80 hrs/month → **~120–140 units/month is the hand-built cap**, and that's a
*busy* month. Beyond it, more units means either fewer hours on the product (which
starves the funnel) or outside help.

Three ways through the ceiling, in order of leverage:

1. **Attestation service (highest leverage).** Software/service margin with almost
   no per-unit labor — the one revenue line that doesn't cost an assembly hour.
   A few hundred dollars/month of attestation is worth more take-home than a dozen
   extra hand-built kits, and it compounds. This is *the* answer to "make a good
   living solo."
2. **Richer mix.** Every WAP buyer nudged to Vision or an assembled unit raises
   contribution ~$35–90 with the *same* shipping and support overhead.
3. **Outsource enclosures, then assembly.** Print farm past ~50 sets/mo (doc 22
   §4) removes the printer-babysitting; a Phase-3 assembler (doc 16 §2) removes
   hand-assembly entirely — you trade ~$10–15/unit of margin for your hours back,
   which is the right trade once demand is proven.

## 7. Honest swing variables

- **Health insurance** (~$5.4k/yr here): the biggest single line you don't control.
  On a spouse's plan, the $50k row drops by ~20–30 units/mo.
- **Product mix:** the $55↔$90 contribution swing moves every number more than any
  cost cut could.
- **Capacity/burnout:** the model assumes a sustainable ~50-hr week. "Just work
  more" is not a strategy that survives a year.
- **Taxes:** ~25% is a planning placeholder; a good CPA (the $200/mo line) and an
  S-corp election at higher income can shift it.
- **Returns:** the ~10% reserve is a class average; a quality escape (bad batch,
  DOA cluster) can eat a month. The signed self-test (doc 19) is the mitigation.

## 8. Punchline

**One person can make a living here — a ~$50–60k take-home is a realistic,
defensible target at ~80–130 units/month — and Claude is what makes doing every
job at that volume possible for ~$250/month.** The path to a *comfortable* living
is not more hand-built hardware; it's the **attestation service plus a richer mix**,
because those add income without adding assembly hours. The free software and free
self-build path don't pay the bills directly — they're the funnel that fills the
two things that do: **hardware margin now, attestation margin next.** That's the
doc-08 business model, costed down to whether it feeds a household. It does.

### Sources / basis

Unit economics: priced BOMs (`docs/hardware/bom_canary_*.csv`) + doc 22 live
pricing. Overhead/insurance/tax figures: standard US-solo small-business planning
ranges (ACA individual premiums, general+product liability for small hardware,
SE tax 15.3%), mid-2026 — estimates, not quotes; confirm with a CPA and real
insurance quotes before banking on them.
