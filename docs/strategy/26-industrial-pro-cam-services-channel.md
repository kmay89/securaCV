# 26 — Industrial Pro Cam services: sell the output, not the hours

> The B2B channel around **Canary Vision Pro** (Seeed reCamera Pro) for industry —
> designed around one constraint that decides everything: **one person who builds
> fast and hates the grind.** So this is not a consulting shop that sells days.
> It's a *productized* channel that sells outcomes, compounds every build into a
> reusable asset, and runs its own funnel — so the founder's time goes almost
> entirely into the part they're good at and enjoy (building detectors), not
> travel, meetings, sales, or hand-holding.
>
> Companion to [23 — solo-living income model](23-solo-living-income-model.md)
> (this is its highest-quality-of-life revenue lever), [`/LICENSING.md`](../../LICENSING.md)
> (charge for *hours and liability, never bits* — here the "hours" are sold as
> fixed output), and the device guide
> [`docs/hardware/canary_vision_pro_recamera.md`](../hardware/canary_vision_pro_recamera.md).
> Prices below are a recommendation; the maintainer sets them (doc 20). Estimates
> are marked.

## 1. The reframe (why the obvious model is wrong here)

The obvious move is "industrial vision consulting — day rate, on-site." **Reject
it.** Day-rate/on-site is the worst possible fit for this founder:

- **It punishes speed.** If you build a working detector in a day, an hourly model
  pays you for a day. Your leverage — *"I can build so much in a few days"* — is
  the exact thing hourly billing throws away.
- **It sells the thing you hate.** Travel, discovery calls, open-ended scope,
  babysitting. Every hour of that is an hour not building, and it doesn't scale
  past one body.
- **It doesn't compound.** The tenth robot-cell safety job is as much work as the
  first.

The model that fits: **sell fixed outcomes, turn every build into a reusable
recipe, make the channel async and self-running, and put the founder's hands only
on the build.** Income then scales with a growing *library* and *recurring
retainers* — not with hours in a van.

## 2. The wedge — why a factory buys this instead of Verkada

Every industrial-vision vendor puts **footage on their cloud, re-identifies
people, and bills a subscription.** In a workplace that's a surveillance
liability, a works-council/union fight, and a data-breach honeypot. The Pro Cam
inverts all three (see the device guide):

- **Inference is on-device** (Rockchip RV1126B, 3 TOPS NPU) — footage never leaves
  the site.
- **Only a coarse label+score crosses the trust boundary** into a sealed,
  tamper-evident event log. No faces, no re-ID, no free-text/VLM captions (the
  adapter has no field for them; `spec/sensor_adapter_contract_v0.md` §2).
- **No cloud, no subscription** on the data path.

The one-sentence pitch: **"Get the safety and ops signal without creating the
surveillance liability or the cloud bill — and get a signed record that proves
it."** That last clause — the un-editable event log — is what insurers, auditors,
and EU worker-privacy regimes actually pay for. Privacy-by-construction becomes a
*B2B selling point*, not a constraint.

## 3. How it works (grounded, no new code)

The reCamera Pro already does the hard technical part: **SenseCraft AI retrains a
classifier with no ML expertise**, and its Node-RED flow editor POSTs
`{"confidence":0.91}` at a path like `/sensors/robot_cell/camera` straight into the
existing `adapter_host` webhook — **configuration, not firmware.** Industrial
protocols (RS485/Modbus/CAN) are native, so it talks to their PLCs; the kernel's
zone-regex, confidence bounds, and time-coarsening apply on the way in.

What the technology does *not* solve — and where the value is: **what** to detect,
tuning to a messy real line (lighting, angle, occlusion), wiring it to their PLC,
and certifying it stays private. That's the product.

## 4. The Recipe Library — the asset that makes the founder valuable

The core idea. Every detector the founder builds is packaged once and sold many
times as a **Recipe**:

> a trained/tunable SenseCraft model + the Node-RED flow + the `adapter_host`
> route config + a one-page runbook + the claim-mapping — for one named detection
> need.

Examples: *person-in-robot-cell*, *forklift-in-pedestrian-aisle*,
*pallet-blocking-fire-exit*, *hard-hat/PPE-present-in-zone*, *machine-guard-open*,
*spill-on-floor*, *bin-full / line-stopped*, *after-hours-vehicle-in-yard*.

Why this is the whole strategy:

- **It compounds speed into equity.** The first customer with a need funds the
  build; every subsequent customer with the same need is **near-zero marginal
  work → almost pure margin.** The founder's "I build fast" becomes a library that
  earns while they sleep.
- **It is the moat and the catalog.** A growing set of validated,
  privacy-disciplined industrial detectors that competitors (cloud, re-ID) can't
  ethically or architecturally match.
- **It shrinks every future job to config.** "New" requests are mostly
  "recipe + retune," not from-scratch.

The library is to this channel what the BOM pipeline is to hardware (doc 20): the
canonical asset the business is built on.

## 5. The productized ladder (no day-rate, ever)

| Tier | What ships | Price shape | Founder time |
|---|---|---|---|
| **0 · Self-serve** (free) | Software, docs, the reCamera integration guide. DIY for shops with their own IT. | $0 — funnel + trust engine | none |
| **1 · Fit check** | An async written go/no-go: is it a valid on-device task, does it respect the invariants, what's the mounting/lighting/protocol reality? Ends in a fixed quote. | **Fixed, small** (or free, Claude-drafted) | ~review only |
| **2 · Recipe deploy** | An existing library Recipe, tuned to their site + validated. | **Fixed per Recipe / per site** — cheap because it's reused | hours |
| **3 · Custom detector build** | A need not yet in the library: they send samples async, you build + validate, it ships **and becomes a new Recipe** (double value). | **Fixed project**, priced on value not days | your few days |
| **4 · Re-tune retainer** | Lines drift (new products, seasonal light). Scheduled, batched re-tuning. | **$/month per site** — recurring | batched, low |
| **5 · Attestation / compliance** | The signed, tamper-evident event log packaged as insurer/audit evidence. | **Recurring / metered** | ~zero/unit |

Tiers 4–5 are the point: **recurring, low-marginal-labor income decoupled from
hours** — the doc-23 "comfortable living without more assembly" lever, in its
highest-margin form. Physical work — mounting, wiring, on-site commissioning — is
**never the founder's**: it's the customer's electrician/integrator, or a partner
from the [vendors channel](../../TRADEMARK.md) (doc 19), working from the Recipe's
runbook.

## 6. The quality-of-life engine (how the founder's time stays theirs)

This is a design requirement, not a nicety. The channel is built so the founder
touches only the build:

1. **Async intake, no discovery calls.** A structured brief (the `/industry` form,
   §7) captures everything up front. No "let's hop on a call."
2. **Claude runs the funnel.** It drafts the fit-check reply, the fixed quote, the
   runbook, the accuracy report, and first-line support from the docs — the
   doc-20 self-running-company pattern. The founder's manual steps shrink to
   *review a draft → build the detector → hit send.*
3. **Hard-edged fixed scope.** Every package states *what's included and what's
   not*, in writing. Out-of-scope is a new package, never free labor — this kills
   the open-ended hand-holding that makes this kind of work miserable.
4. **Batched, not on-demand.** Retainers re-tune on a schedule (e.g. a monthly
   window), so there are no fire drills.
5. **Remote-only.** No travel, ever. If hands are needed on the floor, that's a
   line item done by the customer or a vendor.
6. **Templated by default.** The Recipe Library means most work is config, so the
   founder spends their scarce, valuable time on genuinely new detectors — the fun
   part.

Net: the founder's calendar is *build sessions*, not meetings and airports.

## 7. Onboarding the channel (the async front door)

A single `/industry` page (website) with the wedge, 3–4 anchor Recipes as proof,
the fixed-outcome offer, and a **one-screen brief** that produces a structured
intake with no backend and no call:

- Fields: *what do you need to detect · environment (indoor/outdoor, lighting) ·
  how many cameras/sites · existing systems (PLC/Modbus/…) · timeline.*
- Submit builds a **prefilled private email** to `errerlabs@gmail.com` (primary —
  their detection need may be sensitive) with an optional prefilled **GitHub
  issue** for those who prefer public. Same dual pattern as `/feedback`.
- The brief is written so **Claude can draft the fit-check + quote from it
  directly** — the funnel starts itself.
- The **free self-serve path stays visible** — never gate the top of the funnel.

"What happens next," stated plainly on the page: *send the brief → written fit
check + fixed quote → you approve → we build & validate remotely → you (or your
integrator) mount it from the runbook → optional re-tune retainer.*

## 8. Pricing philosophy (value + recurring, not time)

Anchored to outcomes, not clocks (all **maintainer-set**; ranges are estimates):

- **Recipe deploy:** a modest fixed fee per detection need per site — low, because
  it's reused; volume/multi-camera scales the number, not the rate.
- **Custom detector build:** a fixed project fee in the low-thousands, priced on
  the value of the outcome (a prevented incident, an audit passed), *not* the day
  or two it takes. Fast delivery is your margin, not a discount.
- **Re-tune retainer & attestation:** monthly per site / metered. This is the
  compounding base.
- **Free forever:** the software, the integration guide, the STLs, the self-serve
  path. The funnel and the trust engine are never paywalled (LICENSING §1).

The shape to internalize: **one library + a handful of retainers out-earns a
calendar full of consulting days, at a fraction of the effort** — because you're
selling assets and outcomes, not hours.

## 9. Honest flags

- **Concept, not yet bench-validated.** The reCamera Pro integration is design,
  not a verified build (device guide §5). So the *first* few engagements are
  partly co-development — priced as a **founding-customer pilot**, framed as a
  strength ("you shape the product"), and each one seeds the first Recipes.
- **Safety-critical language.** Anything touching an e-stop or guard interlock
  carries explicit *"advisory signal, not a certified safety device"* wording
  until independently validated. This is a liability boundary, and it's where the
  attestation/evidence tier — not a promise — does the work.
- **Prices stay human** (doc 20). The founder sets every number above before it's
  published.

## 10. What this changes / next

- Adds this doc; adds the website `/industry` landing + async intake (a separate
  build).
- Seed the **Recipe Library** with the 3–4 anchor detectors from §4 as the
  catalog's first entries, each as a founding-customer pilot.
- When a Recipe is bench-validated, promote it from concept following the
  `boards.json` tier convention, same discipline as the rest of the hardware
  catalog.
