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
   script amortizes tooling and shows the crossover. The nightmare is a
   spreadsheet; the fix is the doc-20 pattern.

## 1. The 1000-unit order, layer by layer

At ~1000 units you are past what hands can build (doc 18 §3c) and, for
enclosures, at the injection-molding crossover. Verified against mid-2026
market data (sources §7):

### 1.1 Enclosures — make-or-buy, and the crossover

| Route | Per small part (~35g) | Tooling (NRE) | When it wins |
|---|---|---|---|
| **In-house FDM** | ~$1–2 material + printer-hours (effectively ~$3–8 loaded) | $0 | < ~30–50 sets (doc 22) |
| **MJF/SLS service** (JLC3DP, PCBWay 3D, Slant 3D US) | ~$3–8 small part (industry $8–25 for larger parts) | $0 | **~50 to ~1,000+ — the no-tooling volume sweet spot** |
| **Injection molding** (aluminum bridge tool) | ~$2–5 | **$3–8k** | Recurring volume; break-even ~350–800 units on a cheap tool, up to ~1,500–3,000 on a pricier one |

**The fork the founder named** ("one-off vs. recurring"): a **one-off 1000**
stays on **MJF/SLS** — no mold risk, no NRE to strand if it never repeats. A
**recurring 1000** justifies a **cheap aluminum bridge tool**, which amortizes
across reorders and drops per-part below $5. Don't guess — §3's tool shows both
side by side and the exact crossover unit count. Overseas molding is cheapest;
a US bridge tool is the fast, customs-free hedge.

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

1000 **assembled** radios crosses FCC/CE intentional-radiator testing
(**$5–20k**, doc 18 §4). Two escapes we already hold:

- **Ship it as a kit.** Unassembled sidesteps most assembled-product regulatory
  + liability burden (doc 18 §4) — and assembling *is the lab* for the education
  buyer (doc 22 §3).
- **Standardize on a pre-certified ESP32 WROOM module** for modular approval,
  saving ~$5–15k of intentional-radiator work.

Decide this **before** committing, because it changes which SKU (kit vs.
assembled) the 1000 even is.

## 2. Testing the designs — four gates, cheapest first, and *who* does each

The fear is "who could possibly test 1000 units / all these designs?" The answer
needs no lab:

1. **DFM review — the supplier, free.** PCBWay / Seeed / JLC run design-for-
   manufacturing checks as part of quoting; their engineers flag un-moldable or
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

The nightmare is comparing apples (MJF: high per-part, zero NRE) to oranges
(molding: low per-part, big tooling) across quantities, regions, and customs —
by hand, in a spreadsheet that rots. The fix is the **exact pattern the store
already trusts from the margin guard**: one structured data file of RFQ quotes,
one small script that does the arithmetic and summons a human when a number is
missing.

- **`suppliers.json`** (website repo, next to `store.json`): each supplier as
  structured data — region, role (enclosure / assembly), MOQ, lead time,
  per-part or per-unit price, one-time NRE/tooling, per-unit shipping, a customs
  % on the overseas legs, and a `status` (`quote` / `estimate` / `placeholder`)
  that keeps the epistemic tier honest, doc-18 style. Named **routes** compose a
  supplier per layer — *All-overseas*, *Hybrid (US print + overseas kit)*,
  *All-US*, *In-house makers*.
- **`scripts/quote-compare.mjs`**: for a device and a quantity, it computes
  **landed cost per finished unit** for every route — module BOM (joined to the
  same `build.json` the margin guard uses) + enclosure + assembly + amortized
  NRE + shipping + customs — ranks them, and reports the **crossover quantity**
  where one route overtakes another. Paste RFQ numbers in; the apples-to-apples
  falls out. CI-checkable, no spreadsheet, and it flags a route whose MOQ your
  quantity doesn't meet or that's still running on placeholder estimates.

So the "one-off vs recurring" fork answers itself: run the tool at your quantity,
read the crossover, decide with the number in front of you. It ships seeded with
doc-18 estimates (clearly marked) so it runs today, and becomes truth the moment
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

1. Keep it a **kit** (or WROOM-module assembled) to dodge the FCC cliff.
2. Enclosures on **MJF/SLS service** if it's a one-off; price a **US bridge
   tool** only if reorders are real (let §3's tool decide).
3. **Dual-source**: Seeed/PCBWay (overseas, cost) + a US farm/box-build shop
   (hedge, customs-free).
4. **First-article + self-test** are the whole QC; no lab, no trust-me.
5. Route the money through the collective/commission rails (doc 19 §6) if a
   maker or partner fulfills.

## 6. Open items

- [ ] Build `suppliers.json` + `scripts/quote-compare.mjs` (§3) — **done in the
      companion website PR**; seed with doc-18 estimates, replace with RFQs.
- [ ] Get live RFQs: JLC3DP/PCBWay MJF for the WAP + Vision sets; Seeed Fusion
      + a US box-build shop for a 250/500/1000 kit quote (closes doc 16 §5's and
      doc 22 §7's open quote items too).
- [ ] Draft the first-article defect checklist (adapt Voron's; fit coupon as the
      sample part) and name a QC steward (doc 19 §3.1).
- [ ] Decide kit-vs-assembled for the first ≥1000 order (the FCC/liability fork).
- [ ] One-off vs recurring: run the quote tool at the real quantity and record
      the crossover before any tooling spend.

## 7. Sources (live, mid-2026)

Assembly/kitting: [Octopart US-vs-China PCBA cost](https://octopart.com/pulse/p/pcb-pcba-cost-comparison-us-versus-china) ·
[QueenEMS China vs USA 2026](https://www.queenems.com/blog/china-vs-usa-pcb-assembly/) ·
[Seeed Co-Create](https://www.seeedstudio.com/co-create.html) ·
[PCBWay assembly](https://www.pcbway.com/) · [Elecrow partner seller](https://www.elecrow.com/Elecrow_partner_seller_Sell_DIY_Eletronics_online).
Enclosure make-or-buy: [RapidDirect injection-molding costs 2026](https://www.rapiddirect.com/blog/injection-molding-costs/) ·
[Hotean 3D-printing vs injection molding, the 500-part rule](https://hotean.com/blogs/hotean-blog/3d-printing-vs-injection-molding-cost-guide) ·
JLC3DP / PCBWay 3D / Slant 3D service pages. Break-evens and epistemic tiers
inherited from doc 18. QC model from doc 19 §3.1 (Voron PIF + the signed
self-test).
