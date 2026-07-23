# SecuraCV licensing & usage — who pays, who doesn't, and why

The software is Apache-2.0. **You may run it, anywhere, at any scale, forever,
without paying anyone or asking permission** — one Canary on a shelf or ten
thousand across a campus, it's the same license and the same price: free. This
document is about the last small handful of things that *aren't* the software,
and the rare moments money changes hands. It's written for the people who have
to justify a deployment to a procurement officer, a principal, a dean, or a
board — so it's plain, it's public, and it doesn't move.

It follows the same pattern as our [trademark policy](TRADEMARK.md): a public
grant, not a toll. The whole point is a record you can rely on, not a gate.

## The one line that decides everything

> **We charge for atoms, hours, and liability — never for bits, and never for
> privacy.**

Software is bits. Verification is bits. Your right to witness your own space is
not for sale. When we do ask for money it's because a *physical thing* had to be
built, a *human* had to spend time, or an *outside authority* had to put its
name on something — costs that are real whether or not you buy from us. Every one
of them has a free path that never expires.

---

## 1. What is always free

No permission, no fee, no registration, no per-seat count, no license key. For
**everyone** — individuals, households, schools, universities, nonprofits,
churches, tenant groups, mutual-aid networks, newsrooms, clinics, small
businesses, large enterprises, and governments alike:

- **Run the software, at any scale, forever.** Self-host the kernel, the Home
  Assistant integration, the firmware, the desktop and viewer apps, and the CLI
  on as many devices, cameras, sites, or campuses as you like. There is no
  per-device, per-camera, per-seat, or per-site charge, and there never will be.
- **Build and flash your own hardware** from the open designs and BOMs. See
  [`TRADEMARK.md`](TRADEMARK.md) for the one courtesy line we ask on anything
  you sell.
- **Verify evidence with the standalone verifier — free, forever.** A signed
  export must stay checkable by anyone, on their own machine, with no account
  and no connection to us. This one is load-bearing: it's how your proof
  outlives us (see §4).
- **Teach and research with it.** Classroom use, coursework, dissertations,
  published research, and reproductions are all squarely inside the free grant
  and actively encouraged. Cite us if it helps you; you don't owe us anything.
- **Read the docs, run the [Lab](https://kmay89.github.io/securaCV/canary-local/start.html),
  and ask the community.** Issues, discussions, and the guides are open to all.

And three promises that matter more to institutions than to anyone else — spelled
out in full in §4: **no activation server, no phone-home, no remote kill
switch.** Nothing you self-host ever has to reach us to keep working.

---

## 2. When money changes hands (the whole list)

Four things, that's all. Each is **optional**, each serves a real cost, and each
has a free alternative that never expires.

### a) Hardware you'd rather buy than build

Pre-flashed Canaries and kits, for the people who can't (or don't want to) solder
and flash an ESP32. You're paying for parts, assembly, and shipping — atoms —
plus an honest margin, not for the software riding on them.

- **Education and volume pricing is posted publicly** — a published list once it's
  set, not a price negotiated in a back room. A school district and a Fortune 500
  read the same list. (Prices are marked up like any honest hardware line — they
  cover parts, shipping, returns, and support; the *free* option is always to
  build it yourself, never below-cost hardware.)
- **Free path, always:** build it yourself from the open designs. The
  self-built device is not a lesser citizen — it runs the identical firmware and
  earns the identical verified badge.

### b) Court-grade attestation *(planned)*

An optional service that adds *third-party* corroboration to a sealed record, so a
clip is self-authenticating against a deepfake challenge. It runs on **hashes and
signatures only, never your footage**, so it can't see your space and neither can
we. Honest status, so nobody buys a promise: **trusted timestamping (RFC-3161
anchoring) ships today**; **notarized and C2PA / Content-Credentials-interoperable
export bundles are planned, not yet available** — tracked in the roadmap, and this
line updates when they land.

- **A free public-interest tier** — when the service launches — will cover
  individuals, at-risk users, journalists, nonprofits, tenant groups, and
  classroom/research use up to a fair monthly volume. The people who most need
  proof that holds are the people least able to pay for it, and the model is built
  around that, not against it.
- **Metered above that** for high-volume commercial use, because each
  attestation carries a real cost from the timestamping authority.
- **Free path, always:** you don't need us for tamper-evidence at all. The local
  signed, hash-chained log plus the standalone verifier already proves a record
  wasn't edited, offline, with nobody's permission. Attestation only adds an
  *outside* witness on top of one you already fully control.

### c) White-glove support and SLAs

A named human, a guaranteed response time, help planning and rolling out a large
fleet, and someone to call at 2 a.m. This is time — engineering and support hours
— so it's the one thing that's genuinely sold by subscription.

- **Month-to-month, cancel anytime.** A support lapse never disables anything you
  run; you just lose the phone number, not the product.
- **Free path, always:** the docs, the Lab, and the community. Plenty of large,
  serious deployments will never need more than these.

### d) Reselling under our name and logo

If you want to sell devices or services using the **SecuraCV** name, logo, or
mascot as primary branding, that's a trademark grant — and it, too, is **free by
public grant**, recorded by pull request so the whole ecosystem can see who holds
what. The full rules and the always-free "Works with SecuraCV" badge are in
[`TRADEMARK.md`](TRADEMARK.md). No fee; the point is an honest record, not a cut.

---

## 3. The specific answers

| You are… | Do you pay to use SecuraCV? | When, if ever, does money enter? |
|---|---|---|
| **A K-12 school or district** | No. Self-host on any number of cameras and sites, free. | Only if you *buy* pre-flashed Canaries (education pricing) or want a support SLA. Attestation (once the service launches, §2b) is free for incident records at classroom volume. |
| **A university** | No — including research, teaching, and campus-wide deployment. | Only for bought hardware, an optional large-fleet SLA, or commercial-volume attestation from a spinout. Academic use stays free. |
| **A nonprofit, tenant union, newsroom, or mutual-aid group** | No — and you're the public-interest tier, so attestation (once it launches, §2b) is free too, within a fair monthly volume. | Realistically, never — unless you'd rather buy hardware than build it. This is who the product is *for*. |
| **A business or enterprise** | No, to run it — any size, any number of sites. | For bought hardware, a support SLA, high-volume attestation, or using our brand to resell (a free-but-recorded grant). |
| **A government or public agency** | No. Same free self-host grant, and the survival promises in §4 are written with you in mind. | Same as a business: hardware, support, or high-volume attestation — never a per-seat license. |

If your situation isn't here and you're unsure, the default answer is **it's
free** — [open an issue](https://github.com/kmay89/securaCV/issues) and we'll say
so in the open.

---

## 4. Designed to survive

The reason an institution can standardize on this — and the reason it's still
here in ten years — is that the free grant doesn't depend on us staying alive,
staying friendly, or staying independent. These are commitments, not vibes:

1. **No license keys, no activation, no phone-home, no remote kill switch —
   ever.** Anything you self-host runs air-gapped and keeps running whether or
   not we exist. Nothing we ship "calls home" to check that you're allowed.
2. **Everything free is Apache-2.0, which is irrevocable.** We cannot claw back a
   version already released, and a company that buys or inherits us cannot
   either. What's open stays open, including for the release you're running today.
3. **Your evidence stays checkable without us.** The export format is open and
   the verifier is standalone and free. If every server we own went dark tomorrow,
   a lawyer could still verify your five-year-old export on a laptop with no
   network. Proof that depends on a vendor's servers isn't proof.
4. **No secret deals.** Education and volume pricing is posted publicly once set;
   trademark grants are recorded by pull request. Nobody gets a quiet better price
   or a quiet exemption you can't see.
5. **Paid services are month-to-month with a documented fallback.** Attestation
   falls back to the local log + verifier; support falls back to the community;
   hardware is always re-flashable to stock. You can leave any paid tier without
   losing the product.
6. **Good-faith uses are grandfathered.** Changes to this document never
   retroactively revoke a free use that was fair under the rules at the time.

That's the whole trade: we keep the incentive to build good hardware and run a
useful attestation service, and you keep a witness layer that can't be
subscription-trapped, license-locked, or switched off from afar — not by us, not
by whoever comes after us.

---

## 5. Housekeeping

This is a community policy and a statement of intent, not legal advice or a
contract; the binding software terms are in [`LICENSE`](LICENSE) (Apache-2.0) and
the brand terms in [`TRADEMARK.md`](TRADEMARK.md). Where this document and those
disagree about the code or the marks, those win. Questions, edge cases, or "is
*this* free?" — [open a GitHub issue](https://github.com/kmay89/securaCV/issues),
in the open, like everything else here.

The reasoning behind every line above — the segments, the alternatives we
rejected, and the survival argument in full — lives in
[`docs/strategy/21-licensing-structure-for-institutions.md`](docs/strategy/21-licensing-structure-for-institutions.md).
