# SecuraCV trademark & branding policy

The code, the hardware designs, the BOMs, and the enclosures are Apache-2.0 —
**you may build, modify, and sell them without asking anyone.** This document is
about the last thing the license doesn't cover: the *names and logos*, and how
to use them so a buyer always knows who actually made and stands behind a
product. It follows the pattern that healthy open-hardware ecosystems
(Meshtastic, ESPHome) have proven: **two tiers — a compatibility badge anyone
may use freely, and primary marks by simple public grant.**

The marks covered: the **SecuraCV** name, the **SecuraCV logo**, and the
**canary mascot** (owned by Errer Labs). "Canary" is a common word, but note
that others hold trademark rights in "CANARY" for security cameras; we use
it only in combination, as "SecuraCV Canary," and are reviewing our
long-term naming. Third parties should not rely on this document as
clearance to use "Canary" for camera products.

## 1. What never needs permission

Do any of these freely; don't write to ask — the answer is already yes:

- **Build and sell hardware from the open designs**, and say truthfully what
  it is: "a Canary WAP kit built from the open SecuraCV designs."
- **Truthful, descriptive statements**: "compatible with SecuraCV," "runs
  SecuraCV firmware," "based on the SecuraCV witness kernel," "enclosure for
  the SecuraCV Canary Vision."
- **Community content**: articles, videos, guides, talks, courses, forks,
  package names in software distributions.
- **The "Works with SecuraCV" badge** (§2), under its rules.

One ask for anything commercial: include a plain-sight line such as
*"independently made — not affiliated with or endorsed by Errer Labs"* wherever
a buyer might otherwise wonder.

## 2. The free badge — "Works with SecuraCV"

The badge files live in [`brands/`](brands/)
([`works-with-securacv.svg`](brands/works-with-securacv.svg) ·
[`works-with-securacv-mono.svg`](brands/works-with-securacv-mono.svg)).
**Anyone may use them — no permission, no fee, no registration** — on products,
listings, packaging, and pages, provided all of the following hold:

1. **It's true.** The product genuinely works with SecuraCV: it runs the
   firmware (or ships flash-ready for it), or is a functional part —
   enclosure, mount, kit — for a device that does.
2. **It stays open downstream.** Any shipped configuration is available to the
   buyer, and the buyer can re-flash stock firmware using the project's normal
   tools. No lock-in wearing our badge.
3. **It reads as compatibility, not origin.** The badge sits alongside *your*
   brand; it is never the product's primary logo and never implies the product
   is official.
4. **The badge is unmodified** (any size, either variant).

If a use stops meeting these rules, the right to show the badge simply lapses —
fix it and it's yours again.

## 3. Naming rule

The one-line rule, borrowed gratefully from ESPHome: a third-party product name
may include "SecuraCV" **only at the end, as "… for SecuraCV."**

- ✅ "NestBox Outdoor Case **for SecuraCV**", "Witness Kit **for SecuraCV**"
- ❌ "SecuraCV Pro Kit", "SecuraCV-Shop", "securacv-store.com", a company
  named "SecuraCV Labs", or any confusingly similar name or logo

No "SecuraCV" in company names, domain names, app-store names, or social-media
handles that could read as official.

## 4. Primary marks — by grant, in public

Using the SecuraCV logo or mascot as prominent branding — on packaging, a
storefront, a product line — needs a **trademark grant**: non-exclusive,
running two years, renewable, and revocable for cause. There is no fee; the
point is a public record, not a toll.

- **Request one** by opening a GitHub issue titled `Trademark grant request:
  <who> — <what>` describing the use.
- **Every grant is recorded** in
  [`docs/trademark-grants.md`](docs/trademark-grants.md), added by pull
  request, so the whole ecosystem can see who holds what. No private deals.

## 5. What we defend, and for whom

Trademark enforcement here exists to protect **buyers** (from counterfeit
"official" products) and **makers** (from having their work misrepresented) —
never to shake down honest sellers. The commitments:

- We will never use this policy against truthful compatibility claims, and
  changes to it never retroactively revoke a use that was in good faith under
  the rules at the time.
- If a use crosses a line, the first step is always a friendly note and time
  to fix it.
- The open licenses on the designs are a promise: a seller following this
  document's rules is *part of the plan*, not tolerated at its edges.

## 6. Housekeeping

This document is a community policy, not legal advice; it draws on the
[Model Trademark Guidelines](http://modeltrademarkguidelines.org/) and
[FOSSmarks](https://fossmarks.org/). Questions or edge cases: open a GitHub
issue — in the open, like everything else here. Context for why this exists:
[strategy doc 19](docs/strategy/19-open-fulfillment-network-and-builds-ecosystem.md)
and the website's [/ecosystem](https://securacv.com/ecosystem) page.
