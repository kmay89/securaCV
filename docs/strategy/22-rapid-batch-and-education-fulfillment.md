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

## 5. Sustainable education pricing (recommendation)

The rule that keeps it honest *and* survivable: **never sell below COGS + channel
fees**, and publish the tiers (the [`/LICENSING.md`](../../LICENSING.md)
commitment). Three rungs per device:

| Tier | WAP (COGS ~$26) | Vision (COGS ~$34) | Rationale |
|---|---|---|---|
| **Retail R2** | $69 | ~$99 *(down from $119 as Grove fell)* | curation + firmware + support margin |
| **Education batch (10+)** | **~$45** | **~$65** | ≈ COGS × 1.5–1.9; funds DOA units, restock, flat-rate shipping |
| **Title I / public-interest at-cost** | **~$33** | **~$49** | COGS + fees, ~zero margin — the doc's public-interest tier, made real |

Worked contribution (after ~8% processing + ~$2 shipping allowance):
- **24× WAP @ $45 education:** revenue ~$1,080, COGS ~$625, **contribution ~$320.**
- **60× Vision @ $65 education:** revenue ~$3,900, COGS ~$2,040, **contribution ~$1,470.**
- **At-cost tier** nets a few dollars/unit — a DOA/shipping buffer, not profit.
  That's the point: at-cost is a real offer we can sustain, not a loss leader.

None of these touch the software, the firmware, the STLs, or the verifier — all
free at any scale (doc 16 §0, LICENSING §1). A school that prints its own cases and
sources its own XIAO boards pays us **$0** and is a marketing asset, not a lost
sale.

## 6. Rush economics — what "quickly" actually costs *us*

A true quick-turn erodes margin in three specific ways; price a rush order at
**retail (not education) or quote it**, rather than discounting it:

- **Buying modules fast** (Amazon / US distributor, no time for the qty-10 break)
  ≈ +$1–3/unit.
- **Service-printed enclosures** instead of in-house ≈ +$7–18/set.
- **Express outbound + (if overseas) customs/duty** ≈ +$30–60/box + tariff.

So: **small in-house batches (Scenario A) hold their education price even on a
1–2-week turn** — COGS is stable and the work is a weekend. **Large or Vision
batches on a rush** should carry a rush line or move to retail pricing, because
the enclosure-service and express-shipping deltas are real. The honest promise to
a school is *"education price at 4–6 weeks' notice; rush is quotable."*

## 7. Open items (human decisions + follow-ups)

- [ ] **Maintainer to set the published education price points** (§5) before any
      number lands in `/LICENSING.md`. Prices stay human (doc 20).
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
