# 08 — Product Strategy (North Star)

> The canonical, executive summary that sits **above** docs 01–07. Written in one
> point of view. For depth — market data, competitive teardown, persona journeys,
> the friction/satisfaction ledgers, business model, and a sequenced roadmap — see
> the companion [`securacv_product_strategy_whitepaper.md`](securacv_product_strategy_whitepaper.md).

## 1. The one thing

Every other camera asks *"who was that?"* SecuraCV answers a better question:
**"Is everything okay — and can I prove it?"** We don't sell surveillance. We sell
**peace of mind you can take to court.** Everything below serves that sentence.

## 2. The belief

The home-security market made a quiet trade: in exchange for convenience, you
handed a corporation a live feed of your family and a monthly bill forever. People
are waking up to the cost. At the same time, the ground under *evidence itself* is
moving — in the deepfake era, a video "of what happened" is no longer self-
evidently true. SecuraCV is built for the world after both realizations: **your
footage is nobody's business, and your proof is unforgeable.**

## 3. Who we're for (and the order we earn them)

- **Now — Priya, the privacy-conscious prosumer.** Runs Home Assistant + RTSP
  cameras on a Pi, hates subscriptions. *Already* fits our stack. The beachhead:
  win it completely before reaching further.
- **The soul — Marcus, the person who needs proof that holds.** Tenant, journalist,
  activist, abuse survivor. Smaller market, but the *reason we exist* and the
  source of every differentiated feature. He is our brand.
- **The horizon — the Chen family.** Mainstream homeowners who'll never touch a
  terminal. The biggest market and the biggest gap. A 3–5 year prize, won only with
  boxed hardware and an app — never by compromising the model.

(Full journeys for these and two more personas are in [`04-user-stories.md`](04-user-stories.md)
and the whitepaper.)

## 4. Why we win

- **A moat that's architecture, not a feature.** "We can't see your data" is a
  design goal enforced in code and types under the seven invariants
  ([`spec/invariants.md`](../../spec/invariants.md)) — the kernel's privacy
  boundaries are structural, not a policy promise. One honest caveat we keep
  visible: detection backends are an *audited* boundary, not a sandbox (see the
  [README](../../README.md) and [`AGENTS.md`](../../AGENTS.md)), so a backend must
  be trusted/audited until process isolation lands. Even so, a competitor whose
  business *is* your footage cannot follow this architecture.
- **No subscription, ever**, in a market trained to resent subscriptions.
- **Two tailwinds at our back:** rising privacy concern and the collapsing
  trustworthiness of raw video. We are one of the only players selling
  *authenticatable, tamper-evident* perception to humans, not enterprises.
- **An honest brand the target buyer can verify** — the privacy rules are readable
  in the source, and the tamper-evidence is demonstrable end-to-end.

## 5. The friction map (where we lose people today, ranked)

1. **The terminal.** `curl | bash`, a hand-downloaded model, CLI-only trustees.
   *Fix:* one-click HA add-on install with the detection model bundled; zero
   terminal on the happy path.
2. **The invisible payoff.** The "verified ✓ timeline + morning digest" is the
   emotional product, hidden in raw sensors. *Fix:* a real timeline / verification
   surface and an out-of-the-box mobile digest + alert.
3. **The unbuyable device.** *Fix:* a pre-flashed Canary kit so Marcus and the
   Chens can *buy*, not build.
4. **The unusable proof.** Break-glass and "export for evidence" are CLI. *Fix:* a
   guided trustee setup and a one-tap signed export with a standalone verifier a
   lawyer can run.
5. **The credibility gap of an unshipped v1.** *Fix:* close the v1 gates, tag it,
   and make the docs match the code exactly.

## 6. The satisfaction map (what makes people *love* it)

- **The daily "all clear."** A morning digest — *"4 events, all zones normal, every
  witness verified ✓"* — is the product's heartbeat and the reason to keep it.
- **Visible integrity, always.** Surface the verified badge and chain status
  proudly. The one thing every other camera *can't* say is "this record can't be
  edited — even by us." Show it on every event.
- **The constraints, sold as the point.** Coarse time, no faces, 24-hour memory:
  reframe each as a promise — *"It can't be turned into a tool to spy on you, even
  by us."* Never apologize for the model; it **is** the value.
- **Proof that travels.** A one-tap, court-grade signed export bundle + a free
  standalone verifier turns the moat into something a non-engineer can *use* under
  stress.

## 7. The line we will not cross (saying no is the product)

Facial recognition, re-ID, plate reading, bulk/searchable archives, "follow this
person," exact-second timestamps, retroactive reprocessing, and any cloud custody
of footage are **permanently out** — each violates an invariant. Every "wouldn't it
be convenient if…" that adds identity, query, or retention is a competitor's
product. Refusing them *is* our moat made visible. (See [`06-feature-prioritization.md`](06-feature-prioritization.md).)

## 8. Make money without betraying the user

**Open-core + hardware + optional attestation.**

- **Free & open forever:** the kernel, HA integration, firmware, self-host path.
  This is the trust engine and the top of funnel. **Never paywall privacy.**
- **Primary revenue — pre-flashed Canary hardware/kits.** Honest margin that serves
  the personas who can't flash an ESP32, with no compromise to the thesis.
- **Optional margin — a court-grade attestation service** (RFC-3161-style
  timestamping, notarized / C2PA-interop export bundles) that operates **only on
  hashes and signatures, never raw footage.** Lawyers, insurers, journalists, and
  civic operators will pay for "provably untampered." **Reject** any managed cloud
  that touches footage; a metadata-only push relay is the *only* acceptable cloud.

## 9. The path (sequenced for compounding trust)

- **Now → ship v1.** Close the gates (Frigate→HA release gate green, RTSP
  end-to-end in CI, the audit-vs-security-boundary doc, firmware MAC/GPS check);
  tag it; align [`CHANGELOG.md`](../../CHANGELOG.md) and [`v1-roadmap.md`](../../v1-roadmap.md). *Without a shipped v1, "rely on
  this for evidence" is hollow.*
- **Next → kill the terminal + reveal the payoff.** One-click add-on with bundled
  detection; a polished timeline + verified badge; mobile digest/alerts out of the
  box; guided trustee + one-tap signed export with a standalone verifier.
- **Then → make it buyable.** Pre-flashed Canary kit; pre-built Docker images;
  multi-camera standalone; configurable detection (Coral/GPU); launch the
  attestation tier.
- **Horizon → the mainstream.** A consumer app + QR onboarding and a boxed device,
  with the privacy model marketed as the headline — never an apology.

## 10. How we'll know it's working

1. A non-developer installs from the HA add-on store **with no terminal** and sees
   a first **verified ✓** event within 15 minutes.
2. The morning digest is the notification users keep, not mute.
3. A non-engineer sets 2-of-3 trustees in a UI and produces a signed export a
   lawyer verifies independently — proving no tampering, revealing nothing else.
4. An external auditor confirms the privacy claims **by reading the code**.
5. The docs never outrun the implementation.

> **The rule that decides every future feature:** usefulness must come from
> **trust, patterns, and proof** — never from identifying people or enabling
> search. Build that, and every privacy constraint becomes the reason people
> choose us.
