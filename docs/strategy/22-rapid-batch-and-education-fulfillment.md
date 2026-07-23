# 22 — Rapid batch & education fulfillment (schools, universities, STEM groups)

> Companion to [16 — kit commerce](16-kit-commerce-pricing-and-fulfillment.md) and
> [18 — unit economics](18-unit-economics-and-production-scale.md), and the
> operational answer to a question [`/LICENSING.md`](../../LICENSING.md) raises:
> *if a school needs a real batch quickly, can we say yes — how fast, and at what
> price we can sustain?* Two representative buyers: a **Science Olympiad / STEM
> specialty group** (~15–30 units) and a **college embedded-engineering course**
> (~60–120 units).
>
> **Epistemic tiers, kept honest (same discipline as doc 18).** Module prices are
> **firm** — live mid-2026 distributor listings. Print/assembly per-part costs and
> all lead times are **estimated** from published starting prices and industry
> benchmarks; get live instant-quotes and a live stock check before committing to
> a deadline. Prices to schools are a **human decision** (doc 20) — the tiers below
> are a recommendation, not a published rate until the maintainer signs off.

## 1. Refreshed COGS — and a stale-BOM flag

Live pricing moved the numbers since the BOM CSVs were seeded. The big one:

| Part | BOM seed | **Live (mid-2026)** | Note |
|---|---|---|---|
| **Grove Vision AI V2** (101021040) | $29.00 | **$15.99** | −$13; nearly halves the Vision device. **`bom_canary_vision.csv` / `pricing.json` should be refreshed.** |
| XIAO ESP32-S3 Sense (102010469) | $14.90 | $13.99 | ~10% off at qty 10+ (Seeed) |
| XIAO ESP32-S3 plain (113991114) | $7.49 | $7.49 | deep-stocked, low risk |
| SanDisk HE 32GB (SDSQQNR-032G) | $8.50 | ~$8–10 | widely stocked |
| Round Display (104040143) | $18.00 | ~$18 | ⚠️ SKU 104040143 didn't resolve to a live page; standalone is **104030087** ($18). Verify the exact variant. |

Rebuilt kit COGS at small-batch (10–100) sourcing, in-house-printed enclosure:

| Kit | Modules | +SD/cam | +cable/print/pkg/flash | **Kit COGS** | Was (doc 16) |
|---|---|---|---|---|---|
| **Canary WAP** | Sense ~$12.50 | SD ~$8 | ~$4 | **~$26** | ~$28 |
| **Canary Vision** | host ~$5 + Grove ~$14.50 | cam ~$8 | ~$6 | **~$34** | ~$44–53 |

The **module trap still holds** (doc 18 §2): volume barely bends module price
(~10% at qty 10+, floors ~$5 XIAO / ~$14 Grove). A batch price is set by the
module floor, not by volume magic — which is exactly why hardware carries an
honest, small margin and the *software* is what's free.

## 2. Scenario A — Science Olympiad / STEM specialty (~15–30 WAP)

Device: **Canary WAP** — cheapest capable witness; students get ESP32-S3 +
camera + mic + SD to build and flash. Small enough that there is no "production
run" — it's procurement + a weekend of printing.

**Delivery timeline (est.):**

| Step | Time | Pacing? |
|---|---|---|
| Modules (XIAO Sense, SD, cables — all deep-stocked; US distributor or Amazon) | 2–5 days ground / ~1 wk distributor | no |
| Enclosures: 15–30 sets in-house FDM (1–2 Bambu-class printers, ~4–8 sets/printer/day) | 2–8 days | **yes, if 1 printer** |
| Kit + flash (USB-hub jig ~20 s/board, assemble, pack) | ~½–1 day | no |
| Outbound to school (ground) | 2–5 days | no |
| **Realistic total** | **~1–2 weeks** (≈1 wk if modules on hand & printers free) | |

No print service, no customs, no PCBA. This batch is genuinely "yes, in a week
or two."

## 3. Scenario B — College embedded-engineering course (~60–120 Vision)

Device: **Canary Vision** (Grove Vision AI V2 = real on-device ML — an embedded
course's ideal platform) or **WAP** for a cheaper, larger class. The DIY kit (R2)
is the *pedagogically correct* SKU: assembling and flashing it **is the lab**, not
a cost we absorb.

**Delivery timeline (est.):**

| Step | 60 units | 120 units | Pacing? |
|---|---|---|---|
| XIAO hosts (deep-stocked) | ~1 wk | ~1 wk | no |
| **Grove Vision AI V2** — DigiKey depth looked thin ("check incoming stock"); may partial-backorder. Buy early / Seeed-direct / Contact-Sales volume quote | 1–3 wks | 1–3 wks | **yes — the pacing item** |
| Enclosures — in-house needs 2–4 printers; or **Slant 3D** (US farm, ~5 biz days print + 2–5 ship, no customs, ~$8–20/set est.); or **JLC/PCBWay MJF express** (~1–1.5 wk + customs risk, ~$12–30/set est.) | 1–2 wks | 1.5–2.5 wks | maybe |
| Kit + flash (100 kits ≈ ~1 hr flashing + a day packing, doc 16 §3.2) | ~1 day | ~1–2 days | no |
| Outbound (ground) | 2–5 days | 2–5 days | no |
| **Realistic total** | **~2–3 weeks** | **~2.5–3.5 weeks** | |

**Recommendation:** for a term deadline, place the order **4–6 weeks out**. True
build-and-ship once parts are in hand is ~2–3 weeks; the buffer absorbs the two
variables below. For enclosures on a hard academic deadline, prefer a **US route**
(in-house or Slant 3D) — overseas MJF is cheaper per part but adds customs/tariff
variance a fixed deadline can't absorb.

## 4. Critical path & the two things that can slip

1. **Grove Vision AI V2 stock.** The only deep-risk line. Mitigate: confirm live
   qty before promising a date; split across Seeed-direct + a US distributor; use
   Seeed Contact Sales for a volume quote on 50+ (they also offer up to ~20%
   education discount by application). WAP-based courses sidestep this entirely —
   every WAP part is deep-stocked.
2. **Enclosure capacity.** Under ~30 sets, in-house wins outright ($1–2
   material/set). Past ~50, either add printers or hand it to Slant 3D (US, no
   customs) / MJF express (durable nylon, watch customs). This is a make-or-buy
   flip, not a blocker.

Everything else (XIAO stock, flashing, kitting, ground shipping) is fast and
low-variance.

## 5. Sustainable pricing — normal markup, no at-cost tier

**We mark up; we do not sell at cost.** An earlier draft floated a Title-I
"at-cost" rung — that was wrong and is dropped. Assembled hardware carries real
per-unit costs *beyond parts* that a zero-margin price silently eats: outbound
shipping, payment processing, a returns/DOA reserve, product-liability insurance,
and support time. Below any healthy margin, one bad return wipes a whole batch.

Grounded in standard practice (sources §Sources):

- **Keystone (2× cost ⇒ 50% gross margin) is the retail rule-of-thumb, but for
  DTC consumer electronics it's a *floor, not a target*** — healthy brands run a
  **33–45% gross margin** after real costs.
- **Electronics return ~10%**, and each return **costs $30–65 to process** — so a
  returns/DOA reserve is a line item, not an afterthought.
- **Education pricing is a discount *off a marked-up list*, never a sale below
  cost** — exactly how the majors do it (Apple ~10%, Logitech 25%, Samsung 30%,
  HP up to 40% off list). We give a normal channel discount; we don't give away
  hardware.

**Fully-loaded per-unit cost** (what a unit actually costs us to put in a box and
support):

| | Base COGS | +Ship | +Fees ~3% | +Returns/DOA reserve | +Insurance/overhead | **Loaded** |
|---|---|---|---|---|---|---|
| **WAP** | $26 | ~$4 | ~$1.8 | ~$5 | ~$2 | **~$39** |
| **Vision** | $34 | ~$4 | ~$2.7 | ~$6 | ~$3 | **~$50** |

**Tiers (all marked up; GM = gross margin on price *after* the loaded cost):**

| Tier | WAP | Vision | |
|---|---|---|---|
| **Retail** (1–9) | **$69** (~43% GM) | **$99** (~49% GM) | list; keystone-ish |
| **Education / volume** (10–49) | **$59** (~34% GM) | **$85** (~41% GM) | ~15% off list — normal EDU discount |
| **District / large volume** (50+) | **$55** (~29% GM) | **$79** (~37% GM) | floor tier; still ≥ ~28% GM and ≥ 1.8× base COGS |

The floor rule (from doc 16): **never below ~1.8× base COGS or ~28% gross margin**,
whichever binds. Below that the returns reserve stops being covered.

Worked contribution:
- **30× WAP @ $59:** revenue ~$1,770, loaded ~$1,170, **contribution ~$600 (34%).**
- **60× Vision @ $79:** revenue ~$4,740, loaded ~$3,000, **contribution ~$1,740 (37%).**

**Where "free" actually lives — the self-build path, not at-cost hardware.** A
cash-strapped school prints its own cases and sources its own boards and pays us
**$0**, and the software, firmware, STLs, and verifier are free at any scale (doc
16 §0, LICENSING §1). That is the honest free tier and it's a marketing asset, not
a lost sale. Selling assembled hardware below cost is not sustainable and we don't
pretend it is.

## 6. Rush economics — a rush eats the margin, so it's retail or a quote

A true quick-turn adds cost in three specific ways, each of which comes straight
out of the gross margin above — so **a rush order is priced at retail or quoted,
never at the education discount**:

- **Buying modules fast** (Amazon / US distributor, no time for the qty-10 break)
  ≈ +$1–3/unit.
- **Service-printed enclosures** instead of in-house ≈ +$7–18/set.
- **Express outbound + (if overseas) customs/duty** ≈ +$30–60/box + tariff.

So: **small in-house batches (Scenario A) can hold the education price even on a
1–2-week turn** — the loaded cost barely moves and the work is a weekend. **Large
or Vision batches on a rush** move to retail pricing or a per-order quote, because
the enclosure-service and express-shipping deltas are real and would otherwise
erase the margin. The honest promise to a school is *"education price at 4–6
weeks' notice; rush is quotable at retail."*

## 7. Open items (human decisions + follow-ups)

- [ ] **Maintainer to set/confirm the published price points** (§5) before any
      number lands in `/LICENSING.md`. Prices stay human (doc 20); the tiers above
      are a research-grounded recommendation, not a committed rate.
- [ ] **Refresh `bom_canary_vision.csv` + `pricing.json`** for the Grove $29→$16
      drop and re-run `scripts/bom_pricing.py`.
- [ ] **Verify Round Display SKU** 104040143 vs 104030087.
- [ ] Get a live **Grove Vision AI V2 stock/lead** check + a **Seeed Contact-Sales
      volume + education-discount** quote for 50/100 units.
- [ ] Get one **Slant 3D** sample set (quality reputation is mixed — 2.9/5 app
      rating) and one **JLC3DP MJF** instant-quote for the WAP + Vision sets, so
      Scenario B's make-or-buy flip is a purchase order, not a research project
      (this also closes doc 16 §5's open MJF-quote item).

### Sources (live, mid-2026)

Module prices/stock: Seeed & DigiKey product pages for 102010469, 113991114,
101021040/101021112, 104030087; Seeed education-discount and Contact-Sales/volume
channels. Print/assembly: JLC3DP MJF (from $1.00, 1–3-day production, US-tariff
FAQ), PCBWay SLS/MJF (from 3 days), Slant 3D (no minimum, SlantBox ~5 biz days,
US-domestic), Craftcloud/Xometry MJF (aggregator, ~5–10 biz days), PETG filament
~$18–19/kg. Per-part print figures are estimates anchored to these starting prices
and industry benchmarks ($1–10/cm³ MJF) — confirm with live instant-quotes.
