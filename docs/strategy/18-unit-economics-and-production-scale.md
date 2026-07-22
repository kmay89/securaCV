# Unit economics & production scale — what a Canary really costs, and at what volume each production strategy pays off

**Date:** 2026-07-21
**Status:** Analysis. Companion to
[`16-kit-commerce-pricing-and-fulfillment.md`](16-kit-commerce-pricing-and-fulfillment.md)
and [`05-market-and-cost-comparison.md`](05-market-and-cost-comparison.md).
Feeds the canary-local print bench's cost view (enclosure lab → print guide).

> **Two epistemic tiers, kept honest throughout.** The **qty-1 costs are firm** —
> they come straight from the priced BOMs
> ([`docs/hardware/bom_canary_*.csv`](../hardware/), carried into
> `canary-local/devices/build.json`). Everything about **scaling, NRE, and
> break-even is industry-typical estimation** from standard hardware cost
> models — good for deciding *direction*, not for committing capital. Get live
> RFQs (JLCPCB / a molder / a CM) before spending real money.

---

## 1. What one really costs today (maker scale) — firm

The print bench used to answer "how much does it cost?" with filament + power
(~$0.30–1.40). That's a rounding error. The device is dominated by its
electronics, which the BOM prices exactly:

| | Vision (flagship) | WAP | Display (round / 4.3") |
|---|---|---|---|
| Electronics (required BOM) | **$42.70** | **$26.90** | $33.49 / $48.49 |
| Enclosure (PETG + power) | ~$0.85 | ~$0.90 | ~$1.00 / ~$1.40 |
| Fasteners / hardware | ~$2 | ~$1 | ~$2 |
| **Materials all-in** | **~$45** | **~$29** | **~$36 / ~$52** |
| Print time (unattended) | ~3.3 h | ~1.3 h | ~1.5 / ~5.6 h |
| Hands-on assembly | ~25 min | ~15 min | ~20 min |

The honest answer is **~$45 + an afternoon**, not $0.83. On Vision, the **Grove
Vision AI V2 at $29 is 65 % of the cost** — that one module dominates every
decision below.

## 2. Why "buy in bulk" barely helps — the module trap

The BOM is **finished dev-modules, not chips**. Modules carry a vendor margin
already, so their price breaks are shallow:

| Part | qty 1 | ~qty 10 | ~qty 100 | Floor (as a module) |
|---|---|---|---|---|
| XIAO ESP32-S3 | $7.49 | ~$6 (3-pack) | ~$5 | ~$4–5 |
| ESP32-C3-DevKitM-1 | $8 | ~$7 | ~$6 | ~$5.50 |
| Grove Vision AI V2 | $29 | ~$27 | ~$25 | ~$24 |

Buying 100 kits' worth of modules moves Vision from ~$42 to only ~$35. The real
savings live one layer down — **bare silicon on a custom PCB** (estimated):

| Subsystem | As a module | Bare, ~1k (reel / LCSC) |
|---|---|---|
| MCU | XIAO $7.49 / DevKit $8 | ESP32-S3-WROOM-1 **~$2.20** |
| Camera | Grove AI $29 / onboard | OV2640 sensor **~$3–5** |
| Regs / passives / connectors | (on the module) | ~$2–3 |
| **Custom Vision board BOM** | ~$39–42 | **~$12–18** |

The catch: the Grove module's **Himax WiseEye2 vision pipeline is the hard,
risky part to replicate** — most of the $29 premium *is* engineering you'd be
taking on.

## 3. The three break-evens (estimated)

**(a) Custom PCB beats dev-modules.** Saves ~$15–25/unit, costs NRE: PCB design
$3–8k (or your time) + turnkey PCBA setup/stencil ~$50–100 + ~$3/unique-part
placement. Crossover = `NRE / per-unit-saving`: $6k @ $18 saved ≈ **330 units**;
$8k @ $12 ≈ **670**. → **≈ 300–800 units.**

**(b) Injection molding beats 3D printing.** Simple 2-part tool this size:
**$3,000–10,000**; per-shot ~$0.20–0.80. Printing *effectively* costs ~$3–8/set
once printer-hours + handling are counted (not the $1 raw material). $6k tool,
print $4 → IM $0.60: `6000/(4−0.6)` ≈ **1,760**. → **≈ 1,500–3,000 units.**
Between ~100 and ~1,000, use **MJF/SLS via a service** (~$3–10/part,
production-grade, no tooling).

**(c) Turnkey CM beats hand assembly.** Less a cost line than a ceiling — you
can't hand-build past a few hundred. CM box-build adds ~$5–15/unit at hundreds,
less at thousands, and wraps QC/test. → **crosses in the low hundreds; mandatory
past ~500.**

## 4. The practical scale ladder (estimated per-unit, Vision)

| Tier | Sourcing | Enclosure | Assembly | Cert | ~Per-unit | Capital at risk |
|---|---|---|---|---|---|---|
| **1 — maker** | modules retail | print | you | none (personal) | **$45** | ~$0 |
| **10–50 — kit seller** | multipacks | print / MJF | **sell as kits** | minimal (unassembled) | ~$38 | ~$1–2k |
| **100–500 — micro-brand** | modules or 1st PCB | MJF / bridge tool | jig / light CM | FCC/CE *if assembled radio* | ~$30–40 | ~$5–20k |
| **1k–10k — product** | custom PCB, pre-cert ESP32 module | injection mold | turnkey box-build + test | **full FCC/CE $5–20k** | **~$18–28** | ~$50–150k |
| **10k+ — mass** | reels, offshore CM | multi-cavity tool | offshore CM | full + safety / packaging | ~$12–20 | $200k+ |

Two shortcuts worth banking: a **pre-certified ESP32 module** (WROOM) keeps you
on **modular approval** and saves ~$5–15k of intentional-radiator testing; and
**kits (unassembled)** sidestep most assembled-product regulatory + liability
burden — which is why kits are the right small-scale play.

## 5. Pricing, margin, and the mission math

Indie/DTC hardware gross margin is thin — **30–50 % GM** is normal. At ~$25
landed (1k tier), 50 % GM → **~$50 sell**. A kit at ~$40 parts → **$60–75** (you
charge for curation + firmware + enclosure + support, not hardware markup).

The comparison that actually sells it: a typical cloud camera is ~$30–40
hardware but monetizes a **$40–100/yr subscription** — hardware near cost,
recurring is the business, and the video lives on someone else's servers. A
Canary is **once, ~$50–75, zero recurring, no cloud, nothing phones home.** Over
3 years that's **~$60 vs ~$40 + $120–300**, and your footage never leaves the
house. That TCO line is the pitch — and the bench now shows it.

## 6. Recommendation for SecuraCV's stage

**Stay module-based; sell kits + printed enclosures + the firmware, up to
~100–500 units. Do not spin a custom PCB or cut a mold until >500 committed
units.** The NRE ($6–8k PCB + $6k mold) and the certification cliff ($5–20k)
would sink a maker project chasing a ~$15/unit saving. The moat isn't hardware
margin — it's the **privacy firmware and the honest, self-printable design**,
where the enclosure COGS is ~$1, infinitely customizable, with zero tooling
risk. Lean into exactly what the print bench already makes tangible: *you can
make this yourself, today, for the price of the parts.*

---

## Appendix — where the numbers come from

- **Firm (repo):** all qty-1 part prices — `docs/hardware/bom_canary_*.csv` →
  `canary-local/devices/build.json` (`usd`, `qty`, `required`). Enclosure mass /
  filament / power — the print-guide estimator
  (`canary-local/assets/print-guide.js`), itself calibrated to the README mass
  budget.
- **Estimated (industry-typical):** module→bare-silicon deltas, JLCPCB-class
  PCBA fees, injection-mold tooling ranges, MJF/SLS service pricing, CM
  box-build rates, hardware gross margins, cloud-cam subscription ranges. These
  are standard cost-model figures, not live quotes. **Verify with real RFQs
  before committing capital.**
