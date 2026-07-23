# 21 — Licensing structure for schools, universities, groups, and businesses

> The reasoning behind [`/LICENSING.md`](../../LICENSING.md). That file is the
> public promise; this one is the argument for why the line sits where it does,
> the alternatives we rejected, and why the structure is built to outlive the
> company. It sits alongside doc 08 (north star), doc 17 (ecosystem), and doc 20
> (self-running company) and inherits their conclusions rather than reopening
> them.

## 1. Reframing the question

The instinct behind "let's design a licensing structure for institutions" is
usually a tiered EULA: free for personal use, per-seat above some size,
enterprise pricing at the top. **That instinct is wrong here, and not by a
little.**

Two hard facts kill it before we start:

1. **The software is already Apache-2.0.** That grant is irrevocable and
   scale-blind. A university with 10,000 cameras has *exactly* the same legal
   right to run it, free, as a person with one — and we could not add a per-seat
   restriction to already-released code even if we wanted to. Any "institutional
   license tier" for the software would be legal theater: unenforceable against
   the license already granted, and a lie to the buyer.
2. **Paywalling scale betrays the thesis.** Doc 08 §8 and doc 05's business-model
   verdict both land on the same rule: *never paywall privacy, monetize trust.*
   A per-camera or per-seat fee is a privacy paywall wearing an enterprise
   costume — it charges people more for witnessing more of their own space.

So the real question isn't "how do we tier the software." It's **"where can money
honestly enter a system whose software must stay free at any scale — and how do
we say so in a way a procurement officer can trust?"**

## 2. The dividing line: atoms, hours, and liability

The clean, defensible boundary — the one sentence the public doc is built around:

> **Charge for atoms, hours, and liability. Never for bits, and never for
> privacy.**

Everything we could conceivably monetize sorts into one of two bins:

| Bin | Marginal cost to us? | Decision |
|---|---|---|
| Running the software, any scale | ~zero (they self-host) | **Free forever.** Bits. |
| Self-hosting on N sites / M cameras | zero | **Free forever.** Bits, at scale. |
| Verifying an export | zero (standalone tool) | **Free forever** — load-bearing for survival (§5). |
| Teaching / research use | zero | **Free forever**, encouraged. |
| A pre-flashed Canary | real (parts, assembly, ship) | **Sold** — atoms. Published EDU/nonprofit pricing. |
| Court-grade attestation | real (TSA fees, notarization) | **Metered, with a free public-interest tier** — liability/authority. |
| White-glove support / SLA | real (human hours) | **Subscription, month-to-month** — hours. |
| Reselling under the brand | ~zero | **Free by public grant** (trademark), recorded, not a toll. |

The elegance is that the paid lines cost money *whether or not the customer buys
from us* — a school will pay someone for hardware and someone for a support
contact regardless. We're not inventing a toll; we're offering to be the honest
provider of costs that already exist. That keeps our incentives pointed the same
direction as the user's, which is the whole doc-08 argument.

## 3. The four revenue lines, and why each is safe

- **Pre-flashed hardware (primary revenue).** Already the plan in docs 16 and 18.
  Institutions are exactly the buyers who can't flash 200 ESP32s by hand.
  Education/nonprofit pricing is *published* (doc 20's "prices stay human, but
  they're posted"), so there are no back-room deals to leak or resent. The
  free path — build it yourself — is not a downgrade: same firmware, same badge.
- **Court-grade attestation (margin, never required).** Straight from doc 08 §8
  and doc 05. The institutional angle sharpens it: a school proving a hallway
  incident record is untampered, a university IRB proving a research log's
  integrity, a tenant union proving a landlord's timeline. All of them need an
  *outside* witness, which is the one thing the local log can't be. Crucially it
  touches only hashes/signatures, so it never becomes a footage path — the line
  doc 05 says to reject. The **free public-interest tier** is a deliberate design
  choice, not charity: Marcus (doc 08 §3) is the soul of the product and the
  worst-funded persona, so the pricing has to be inverted from the usual
  enterprise curve — free where the need is highest.
- **Support / SLA (the only true subscription).** Sold hours, month-to-month. The
  survival promise (§5) makes this safe: a lapsed contract removes a phone
  number, never a capability. This is the honest version of "enterprise" — you're
  buying our attention, not our permission.
- **Trademark grant (free, recorded).** Already solved by
  [`TRADEMARK.md`](../../TRADEMARK.md). Listed here only so the institutional
  reader sees the *complete* set of "when money enters" and finds that the fourth
  one costs nothing.

## 4. Segment notes — "when isn't it free?"

- **K-12 schools / districts.** Free to run at any scale. Money only for bought
  hardware (EDU pricing) or an optional SLA. Attestation free at classroom
  volume. Watch item: districts often *require* a vendor contract to purchase
  anything — the SLA line exists partly so a district that needs a PO-able
  contract has one to buy, without that being a tax on the software.
- **Universities.** Free, explicitly including research and teaching (Apache
  already grants this; we say it loudly because researchers ask). Money only for
  hardware, large-fleet SLA, or commercial-volume attestation from a spinout.
  Research reproductions and citations owe us nothing.
- **Groups (nonprofits, tenant unions, newsrooms, mutual aid, at-risk).** The
  most-free segment by design — the public-interest attestation tier is built for
  them. Realistically they never pay unless they'd rather buy hardware than build
  it. This is the persona the north star calls "the soul," so the pricing bends
  toward them on purpose.
- **Businesses / enterprises / government.** Free to run, any size, any number of
  sites. Money for hardware, SLA, high-volume attestation, or a brand grant to
  resell. No per-seat license — that's the offer that would betray the thesis,
  and refusing it is itself a selling point to a privacy-buying institution.

## 5. Why it's "designed to survive"

The user's phrase, and the part institutions actually gate on. A school or agency
will not standardize on something that dies when the vendor does — so the free
grant has to be independent of our survival, our goodwill, and our ownership. The
public doc turns that into six commitments; the load-bearing three:

1. **No activation, no phone-home, no remote kill switch.** This is a hard
   engineering invariant, not a marketing line — anything self-hosted must run
   air-gapped forever. It's the single most important sentence for a government
   or hospital buyer, and it's a tripwire: the day a "license check" ships, the
   survival promise is void. (Ties to doc 17's Bambu lesson — a 2025 lock-in
   burned years of trust overnight.)
2. **The verifier is standalone, open, and free forever.** Evidence that needs a
   living vendor's server to be checked is not evidence. The free verifier is
   what lets a five-year-old export be validated on an offline laptop after we're
   gone — the difference between the black-box-evidence model (doc 13) being real
   and being a brochure.
3. **Apache-2.0 is irrevocable, through acquisition.** An acquirer inherits the
   company but not the power to un-free what's released. Institutions are buying
   into a grant that a future owner can't rescind — which is exactly the
   assurance BlackBerry/Bambu couldn't offer (doc 17).

The remaining three — published pricing, month-to-month paid tiers with
documented fallbacks, and grandfathering good-faith uses — remove the softer ways
a buyer could get captured or surprised.

This is the same shape as doc 20's self-running company: the structure has to fit
in a context window, survive its author, and summon a human only for the few
things (prices, the brand's word) that must stay human. Licensing is now one of
those documented systems rather than a founder's memory.

## 6. Alternatives rejected

- **Tiered/per-seat software license.** Unenforceable against Apache-2.0 and a
  privacy paywall in disguise. Rejected in §1.
- **Open-core with a restrictive "commercial" re-license (BSL/SSPL-style).** Would
  require relicensing away from Apache — abandoning the trust engine and the top
  of funnel (doc 08 §8) to chase revenue we can get honestly from hardware and
  attestation. It also reads to a privacy buyer as exactly the bait-and-switch
  they came here to escape. Rejected.
- **Managed cloud for institutions.** Any cloud custody of footage attacks the
  core promise (doc 05, doc 08 §7). A metadata-only push relay is the only cloud
  we'd consider, and it's a convenience, never a license lever. Rejected as a
  monetization path.
- **Free-for-personal, paid-for-commercial.** The commercial/personal line is the
  wrong axis — it charges the small business running three cameras while a giant
  homelab pays nothing, and it's unenforceable anyway. The honest axis is
  atoms/hours/liability vs. bits, which is orthogonal to who you are. Rejected.

## 7. What this doc changes, and what's next

Changes: adds [`/LICENSING.md`](../../LICENSING.md) (the public promise) and this
reasoning doc; adds no code, no license keys, no gates — consistent with the
strategy folder being documentation only.

Next, when someone picks it up:
- Link `/LICENSING.md` from the website's institutional pages (corps/store) —
  **as a link to the canonical repo doc, not a copy** (doc 20: one canonical
  source, facts fetched not typed).
- When the attestation service is real (doc 08 §9 "then"), publish the
  public-interest volume threshold as a concrete number here.
- When EDU/nonprofit hardware pricing is set, publish the list next to the store
  BOM prices, per §2's "published, not negotiated" commitment.
