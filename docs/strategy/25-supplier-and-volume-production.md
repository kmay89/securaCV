# 25 — The supplier & volume path: two lanes, dual-sourced, with costs you can actually see

> Docs [16](16-kit-commerce-pricing-and-fulfillment.md) and
> [22](22-rapid-batch-and-education-fulfillment.md) run *our own* small batches;
> [18](18-unit-economics-and-production-scale.md) has the break-even math;
> [19](19-open-fulfillment-network-and-builds-ecosystem.md) is the passive
> supplier/commission network; [24](24-shipping-and-fulfillment-app.md) is the
> per-order label tool. This doc answers the next founder question directly:
> *once a design is verified, can a supplier print and/or assemble a big order
> (say 1000) as an alternate to the maker path — lowest cost, highest quality —
> and how do we ever test the designs and see the costs without it being a
> nightmare?*
>
> **The one-sentence verdict:** the supplier path and the maker path are two
> lanes of one road — the maker corps carries the long tail, a **dual-sourced
> (one US + one overseas) supplier** carries the big committed batch — and the
> Canary's **signed self-test makes 100% functional QC automatic and unfakeable
> at any supplier**, while a `suppliers.json` + `quote-compare.mjs` (the margin
> guard's shape) turns a pile of RFQs into one landed-cost-per-unit table.

## 0. Principles

1. **Two lanes, not a replacement.** The maker path (docs 15/19, `maker.html`,
   `request.html`) stays the answer for onesies, neighbor pairings, custom
   colors. The supplier path is for the *committed batch* — a school's 120, a
   reseller's 1000 — that would otherwise eat a person's month.
2. **Dual-source everything that matters.** One US + one overseas supplier per
   layer (enclosure, assembly). The overseas one wins on cost; the US one is the
   insurance a fixed commitment needs against customs, a bad lot, or a supplier
   going dark. Never single-thread a 1000-unit promise.
3. **The device is the QC.** The signed first-boot self-test (doc 19 §3.1) is
   functional acceptance you don't have to trust a CM for — it's cryptographic.
   This is the structural reason we can outsource assembly without outsourcing
   trust.
4. **Costs are computed, never eyeballed.** RFQ numbers go into one file; a
   script amortizes any capex (a printer) and shows the crossover. The
   nightmare is a spreadsheet; the fix is the doc-20 pattern.

## 1. The 1000-unit order, layer by layer

At ~1000 units you are past what hands can build (doc 18 §3c). The enclosures
are **3D-printed at a print farm** — this is print-on-demand fulfillment, *not*
injection molding (tooling only enters past ~1,500+ *recurring* units; doc 18
§3b keeps the math if it ever does). Verified against mid-2026 market data
(sources §7):

### 1.1 Enclosures — two print-farm models, and the only real crossover

A print farm prints plastic to order. Two ways to buy it:

| Model | Who | Per small part (~35g) | Inventory / labor | When it wins |
|---|---|---|---|---|
| **In-house FDM** | your own printers | ~$1–2 material (~$3 loaded w/ printer-hours) | you print + pack | while printer-hours aren't the bottleneck (doc 22) |
| **Dropship (zero-touch)** | **Slant 3D Teleport** (US) | ~$8–12, ships bundled | **none** — farm prints *and* ships direct | the long tail; **parts-pack SKUs only** (no board to insert) |
| **Batch-to-stock** | JLC3DP / PCBWay MJF (overseas), Slant (US) | ~$3–8 (industry $8–25 for larger FDM parts; MJF $1–10/cm³) | you hold sets + pack | the committed batch — best $/part |

**There is no tooling in printing, so the only capex crossover is a printer.**
The real fork is *buy another printer vs. keep paying the farm per part*: with
the seed numbers, a ~$800 Bambu-class printer pays for itself against the US
farm at **~120 units** — below that the farm's per-part is cheaper, above it the
printer is. §3's tool computes the exact number from your quotes.

**Dropship vs. batch, plainly:** dropship (Teleport) is unbeatable for
*parts-pack* orders — the farm prints and ships and you never touch it — but a
full kit needs the flashed board inserted after the case prints, so kits are
**batch-to-stock case + a kitting step** (§1.2). Overseas MJF is cheapest per
part; US (in-house or Slant) is the faster, customs-free path.

### 1.2 Assembly / kitting — module box-build, not bare PCBA

Our BOM is **finished modules** (XIAO + Grove), so "assembly at volume" is
**kitting / box-build**, not a custom PCBA run (that's a separate, later
decision — doc 18 §2–3, only past ~300–800 committed units). Turnkey candidates:

| Supplier | Region | Why it fits | Watch |
|---|---|---|---|
| **Seeed Fusion / Co-Create** | overseas | *They make the XIAO* — uniquely convenient to kit their own boards with our printed parts; XIAO designs get free PCBA prototyping | Contract/SKU approval (doc 19 §4) |
| **PCBWay / Elecrow** | overseas | Established open-hardware box-build + 3D + fulfillment; ~5–25% cheaper than US >100 units | Customs/tariff, 12–18 day freight |
| **US box-build shop (MacroFab-class)** | US | 5–10 day door-to-door, no customs, best for regulated/high-reliability | 15–25% dearer base |

Consensus 2026 playbook: **prototype/bridge in the US, mass-produce overseas** —
which is exactly the dual-source in Principle 2.

### 1.3 The regulatory cliff (don't get surprised)

**Corrected — this section previously named "ship it as a kit" as an escape. It
is not one.** Full reasoning in
[doc 29](29-fcc-and-product-compliance-diligence.md); the short version:

- **One escape is real.** **Standardize on a pre-certified ESP32 WROOM module**
  for modular approval, saving ~$5–15k of intentional-radiator work. This holds.
- **The kit escape is not real.**
  [15.23](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.23)
  excludes kits *by name* and covers only ≤5 units built for personal use and not
  marketed. Marketing an RF device is regulated by state of *marketing*, not
  state of *assembly* — a pre-flashed board and a printed case is a radio we
  sold. Assembling still *is* the lab for the education buyer (doc 22 §3); it
  just isn't a regulatory shield.

What that leaves for a committed 1000:

- Any SKU with a radio in the box — **kit or assembled** — needs its own
  **Part 15 Subpart B SDoC** on the finished product (~$1.5–4k per SKU, one
  time), plus enclosure labeling and manual statements (doc 29 §2, §4).
- Any SKU that is **printed enclosures only** carries none of it — a plastic
  shell is not a digital device (doc 29 §8). This is the posture to hold by
  default.
- **Check each board's own FCC status before committing** (doc 29 §3): some
  dev/eval boards are marketed under
  [2.803](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-2/subpart-I/section-2.803)
  provisions that **prohibit sale to end users in residential environments**.
  That is a hard blocker on a 1000-unit order, and it is a five-minute lookup.

Decide this **before** committing, because it changes what the 1000 even is —
and the fork is plastic-vs-radio, not kit-vs-assembled.

## 2. Testing the designs — four gates, cheapest first, and *who* does each

The fear is "who could possibly test 1000 units / all these designs?" The answer
needs no lab:

1. **DFM review — the supplier, free.** PCBWay / Seeed / JLC run design-for-
   manufacturing checks as part of quoting; their engineers flag un-printable or
   un-buildable geometry *before* a cent is spent.
2. **First-article samples — you, or one named QC steward.** Order 1–5 sets from
   *each* candidate supplier. Inspect against a published defect checklist (adapt
   Voron's, doc 19 §3.1) plus our **fit-coupon print**, which already names the
   exact parameter to correct per machine. This is an afternoon, not a program.
3. **Functional QC — the firmware, automatically.** Every unit runs the **signed
   self-test on first boot** and cryptographically vouches for its own build and
   health (doc 19; `maker.html` says exactly this). No assembler can fake it and
   you don't inspect boards by hand — it's 100% coverage, at any supplier.
4. **AQL sampling — statistical, on the full run.** Standard acceptance sampling
   for cosmetics/dimensions on the finished lot; the CM runs it to your AQL.

The self-test (gate 3) is the load-bearing one: it is *why* we can hand assembly
to a stranger. A normal hardware brand pays a CM for test coverage it must trust;
ours is built into the product.

## 3. Seeing the costs without a nightmare — `suppliers.json` + `quote-compare.mjs`

The nightmare is comparing apples (dropship: high per-part, zero inventory) to
oranges (in-house: a printer's capex amortized, plus your labor) to overseas
batch (cheap per-part, but customs and lead time) — by hand, in a spreadsheet
that rots. The fix is the **exact pattern the store already trusts from the
margin guard**: one structured data file of RFQ quotes, one small script that
does the arithmetic and summons a human when a number is missing.

- **`suppliers.json`** (website repo, next to `store.json`): each supplier as
  structured data — region, `model` (in-house / batch-to-stock / dropship), role
  (enclosure / assembly), MOQ, lead time, per-part or per-unit price, one-time
  NRE (a printer's capex — the only NRE in printing), per-unit shipping, a
  customs % on the overseas legs, and a `status` (`quote` / `estimate` /
  `placeholder`) that keeps the epistemic tier honest, doc-18 style. Named
  **routes** compose a supplier per layer — *In-house*, *Scale in-house (+1
  printer)*, *US/overseas farm case + you kit*, *farm case + Seeed/US box-build
  kits it*.
- **`scripts/quote-compare.mjs`**: for a device and a quantity, it computes
  **landed cost per finished unit** for every route — module BOM (joined to the
  same `build.json` the margin guard uses) + printed case + kitting + amortized
  capex + shipping + customs — ranks them, and reports the **crossover quantity**
  where one route overtakes another (in a print world, the meaningful one is
  buy-a-printer vs pay-the-farm). Paste RFQ numbers in; the apples-to-apples
  falls out. CI-checkable, no spreadsheet, and it flags a route whose MOQ your
  quantity doesn't meet or that's still running on placeholder estimates.

So the "print in-house vs pay a farm" fork answers itself: run the tool at your
quantity, read the crossover, decide with the number in front of you. It ships
seeded with doc-18 estimates (clearly marked) so it runs today, and becomes truth the moment
real RFQs replace the placeholders.

## 4. How the two lanes coexist operationally

- **Retail / neighbor / custom → maker path.** The store's weekly batch (doc 24
  tool) and the corps (doc 19) handle onesies and pairings.
- **Committed batch (≥ ~a few hundred) → supplier path.** Route to the
  dual-sourced supplier; the quote tool picks the cheapest route that meets the
  deadline (US when the calendar is tight, overseas when cost rules).
- **Both emit the same self-test acceptance**, so a unit is a Canary whether a
  neighbor built it or a CM did — the cryptographic QC is the through-line, and
  the customer can always re-flash from our web flasher (doc 19 §5) either way.

## 5. Recommendation for SecuraCV's stage

Per doc 18 §6, **stay module-based and kit-first** — do **not** spin a custom
PCB or cut a mold until >500 *committed* units. For a first 1000:

1. Stay on a **pre-certified WROOM-class module** — that is the escape that
   actually works. Budget a **Part 15B SDoC (~$1.5–4k)** for any SKU with a radio
   in the box, kit or assembled; a kit does not dodge it (§1.3, doc 29). If that
   budget isn't committed, the 1000 is an **enclosure-only** order.
2. Enclosures **3D-printed at a farm**: dropship parts-packs via **Slant 3D
   Teleport** (zero-touch); batch-to-stock **MJF (JLC3DP/PCBWay)** for kit
   cases. Buy another printer only when §3's tool says it beats the farm.
3. **Dual-source**: JLC/PCBWay (overseas, cost) + Slant 3D / a US box-build shop
   (hedge, customs-free).
4. **First-article + self-test** are the whole QC; no lab, no trust-me.
5. Route the money through the collective/commission rails (doc 19 §6) if a
   maker or partner fulfills.

## 6. Open items

- [ ] Build `suppliers.json` + `scripts/quote-compare.mjs` (§3) — **done in the
      companion website PR**; seed with doc-18 estimates, replace with RFQs.
- [ ] Get live quotes: a Slant 3D Teleport per-part estimate for the parts-pack
      STLs; JLC3DP/PCBWay MJF for the WAP + Vision sets; Seeed Fusion + a US
      box-build shop for a 250/500/1000 kit quote (closes doc 16 §5's and doc 22
      §7's open quote items too).
- [ ] Draft the first-article defect checklist (adapt Voron's; fit coupon as the
      sample part) and name a QC steward (doc 19 §3.1).
- [ ] Decide **plastic-only vs. radio-in-the-box** for the first ≥1000 order —
      that, not kit-vs-assembled, is the real FCC/liability fork (§1.3, doc 29
      §7). If radio: book the Part 15B SDoC before the PO.
- [ ] Look up every candidate board on `fccid.io` and record grant status; reject
      any marked evaluation-only / not-for-resale (doc 29 §3).
- [ ] Print in-house vs pay a farm: run the quote tool at the real quantity and
      record the buy-a-printer crossover before adding printers or committing a
      batch.

## 7. Sources (live, mid-2026)

Assembly/kitting: [Octopart US-vs-China PCBA cost](https://octopart.com/pulse/p/pcb-pcba-cost-comparison-us-versus-china) ·
[QueenEMS China vs USA 2026](https://www.queenems.com/blog/china-vs-usa-pcb-assembly/) ·
[Seeed Co-Create](https://www.seeedstudio.com/co-create.html) ·
[PCBWay assembly](https://www.pcbway.com/) · [Elecrow partner seller](https://www.elecrow.com/Elecrow_partner_seller_Sell_DIY_Eletronics_online).
Print-on-demand fulfillment: [Slant 3D Teleport (dropship)](https://teleportpod.com/) ·
[Slant 3D printing API](https://www.slant3d.com/slant-3d-printing-api) ·
[Unionfab: JLC vs PCBWay vs RapidDirect 2026](https://www.unionfab.com/blog/2026/03/jlc-vs-pcbway-vs-rapiddirect-vs-unionfab) ·
[3D printing cost 2026](https://3dprinting.com/how-much-does-3d-printing-cost/) ·
JLC3DP / PCBWay 3D / Slant 3D service pages. Injection molding (background only,
out of scope until recurring >1,500 units): [RapidDirect molding costs 2026](https://www.rapiddirect.com/blog/injection-molding-costs/) ·
[Hotean 3D-print vs molding 500-part rule](https://hotean.com/blogs/hotean-blog/3d-printing-vs-injection-molding-cost-guide). Break-evens and epistemic tiers
inherited from doc 18. QC model from doc 19 §3.1 (Voron PIF + the signed
self-test).
