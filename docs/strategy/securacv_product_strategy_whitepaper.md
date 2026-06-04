# SecuraCV — Product Strategy Whitepaper

*A companion to the north-star summary in [`08-product-strategy.md`](08-product-strategy.md).
This document carries the depth: market, competition, personas, the friction and
satisfaction ledgers, business model, roadmap, and metrics. It is consistent with —
and cites — the analysis in docs [`04`](04-user-stories.md), [`05`](05-market-and-cost-comparison.md),
[`06`](06-feature-prioritization.md), and [`07`](07-timeline-events-privacy-design.md),
and is bound by the seven invariants in [`spec/invariants.md`](../../spec/invariants.md).*

*Status discipline: nothing here claims more than the code delivers. Where a
capability is in progress (RTSP-in-CI, vault setup UX, the v1 tag), it is named as
such — matching the project's own bar in [`v1-roadmap.md`](../../v1-roadmap.md).*

---

## 0. Executive summary

SecuraCV is not a better security camera. It is a different category: a **privacy
witness**. Cameras detect events; raw clips auto-delete; the only thing that
survives is a cryptographically signed, hash-chained log of *semantic* events — no
faces, no plates, no precise timestamps. The system can prove what happened without
building a searchable archive of *who*, and it can prove nobody — including the
owner — altered the record.

Two forces make this the right product at the right time:

1. **Subscription fatigue + privacy backlash.** The dominant brands monetize a live
   feed of your home and bill you monthly forever. The market increasingly resents
   both.
2. **The collapse of video as self-evident truth.** In the deepfake era, "I have it
   on video" is no longer a closing argument. Courts increasingly expect
   cryptographic hash verification and an unbroken chain of custody. Almost nobody
   sells *authenticatable, tamper-evident* perception to humans rather than
   enterprises.

The differentiator is **architectural, not a feature**: privacy is enforced in
code and in Rust's type system under seven non-negotiable invariants, and
tamper-evidence is demonstrable end-to-end (Ed25519 signatures + hash chain + a
working verifier and live tamper demo). A competitor whose business *is* retaining
your footage cannot copy "we can't see your data."

The gap is not invention — the hard half is built and test-backed. The gap is the
**last-mile product layer**: one-click install, a polished verification timeline,
productized hardware, a usable break-glass/export flow, and a shipped v1. This
document lays out how to close it for **least friction and highest satisfaction**,
and how to fund it without betraying the thesis.

---

## 1. The thesis (the one thing)

Every other camera asks *"who was that?"* SecuraCV answers a better question:
**"Is everything okay — and can I prove it?"**

We do not sell surveillance. We sell **peace of mind you can take to court.** Every
strategic choice below is judged against that one sentence. If a feature serves it,
it's a candidate. If it serves *"who was that?"*, it belongs to a competitor.

---

## 2. The product as it exists today (honest baseline)

A multi-language monorepo whose center of gravity is the Rust **Privacy Witness
Kernel** (`witnessd`); everything else feeds it, runs it, or displays its output.

| Layer | What's there | Maturity |
|------|--------------|----------|
| **Kernel (Rust, ~25k LOC)** | Frame isolation, hash-chained Ed25519-signed log, event-contract allowlist, break-glass quorum, vault sealing (opt-in), SQLCipher at rest, optional post-quantum (MLDSA/MLKEM) | **Works, demonstrable** |
| **Detection** | Pluggable backends: motion (stub/CPU, default) + Tract ONNX (feature-gated) | Works; object detection is opt-in, default is motion-only |
| **Ingestion** | File (CI-proven roundtrip), RTSP (GStreamer/FFmpeg), V4L2, ESP32 HTTP | File proven; RTSP implemented but **not yet exercised in CI** |
| **Adapters** | Vendor-neutral framework (Frigate, MQTT, webhook+auth/TLS/mTLS, BLE presence) into one privacy choke point | Works, well-tested |
| **Home Assistant** | HACS integration (3 modes, 5 sensors, 11 binary sensors, device PKI/TOFU), add-on with setup wizard, MQTT discovery, daily digest + pattern alerts | Works |
| **Firmware (ESP32)** | Canary Vision, Canary WAP (mesh, power, diagnostics, BLE, Beacon/Chirp), OTA | Vision works; WAP has enterprise-readiness items (MAC/GPS) open |
| **Device API** | Node/Express + SPA timeline, security middleware | Works |

**Engineering quality is high (external read: A‑/A+).** 114+ test functions across
~3,087 lines of test code; trybuild compile-fail tests that prove raw frames can't
be serialized at compile time; parser fuzz sweeps; seccomp sandboxing for untrusted
adapters; clippy `-D warnings`, rustfmt, a feature-matrix build, CodeQL across five
languages, SBOM, and secret scanning in CI. Cryptographic hygiene is notable:
domain-separated signatures, constant-time token comparison, a TOCTOU-hardened
`0600` database pre-create, a canonical-JSON (RFC-8785 subset) encoder that rejects
floats to prevent cross-language digest divergence, and `zeroize` on key material.
Dependency advisories are tracked with per-CVE reachability reasoning rather than
dismissed. There are **no hard `TODO`/`FIXME` markers in source.**

**The honest caveats** (the project states these itself): the default build detects
motion, not classified objects; RTSP lacks a CI roundtrip; vault sealing is wired
but key-management/setup UX is deferred; device keys are seed-derived, not
hardware-backed; and v1 is not yet tagged. The maturity is *pre-v1 with documented
gaps*, not half-finished scaffolding.

---

## 3. Market

### 3.1 Size and shape

- **Smart home** is large and growing — roughly **$90B (2026) → ~$139B (2032)** by
  one estimate, ~$175B in 2026 growing ~8.8%/yr by another.
- The **DIY** segment grows ~**17.9% CAGR** — precisely SecuraCV's lane.
- **Home Assistant** holds ~**10%** self-host share: tech-savvy, privacy-conscious
  users who already own the hardware SecuraCV needs.
- **~31%** of smart-home users cite **privacy** as a top concern; security
  influences **>60%** of purchase decisions.

### 3.2 The evidence / deepfake tailwind (the least-contested moat)

Courts in 2026 increasingly expect cryptographic hash verification, an unbroken
chain of custody, and the ability to rebut deepfakes — see FRE 901 and FRE
902(13)–(14) self-authentication and NIST SHA-256. Europol has projected that up to
~90% of online content could be synthetic by 2026. The ability to produce a
*provably untampered* record is becoming a legal necessity — and almost nobody is
selling it to consumers and prosumers. **This is SecuraCV's deepest, least-contested
moat.**

*(Figures and sources in [`05-market-and-cost-comparison.md`](05-market-and-cost-comparison.md).)*

---

## 4. Competitive landscape

### 4.1 Five-year cost (4 cameras)

| Product | Model | Subscription | 5-yr cost | Footage location |
|---------|-------|--------------|-----------|------------------|
| **Ring** | Cloud | $4.99–$19.99/mo | ~$300–$1,200+ in fees | Cloud |
| **Google Nest** | Cloud | ~$5–10/mo | ~$300–$600 in fees | Cloud |
| **Eufy** | Local-first | optional ~$2.99/mo/cam | $0–$700 | Local (HomeBase) + optional cloud |
| **Reolink** | Local-first | optional | $0+ | microSD/NVR + optional cloud |
| **UniFi Protect** | NVR-first | none | hardware only | Local |
| **Frigate** | Self-host | optional Frigate+ ~$5–10/mo | $0 (core free) | Local |
| **SecuraCV** | Self-host | **none** | **hardware only** | **Local, auto-deleting** |

### 4.2 Why the moat holds

The cloud incumbents (Ring, Nest) *are* the surveillance archive — their business
model is retaining and analyzing footage. They cannot credibly ship "we can't see
your data" without unwinding their economics. The local-first players (Eufy,
Reolink, UniFi, Frigate) solve *custody* but not *integrity* — they keep footage
local, but none ship cryptographic, tamper-evident, identity-stripped witnessing as
a structural guarantee. SecuraCV's differentiation is **an architecture, not a
toggle a competitor can flip on.** It is, however, a *complement* to Frigate (which
it wraps), not a head-on replacement — a friendly-coexistence wedge into an existing
enthusiast install base.

---

## 5. Personas — buy → install → use

Five personas, drawn from the audiences the code actually serves. Each flags
**current friction** vs **what "good" looks like**, with acceptance criteria that
can become product requirements. (Expanded from [`04-user-stories.md`](04-user-stories.md).)

### A — Priya, the privacy-conscious prosumer · **LEAD SEGMENT**
HA on a Pi 5, 3 RTSP cameras, hates subscriptions, fluent in YAML not Rust. The
largest reachable market today; SecuraCV already fits her stack.
- **Buy:** nothing — reuses gear (optional ~$25–60 Coral TPU).
- **Friction:** `curl | bash` + manual add-on; ONNX model hand-download; payoff
  lives as raw HA sensors.
- **Good:** one-click add-on, detection bundled, a polished timeline card + digest.
- **Acceptance:** installs from the add-on store with no terminal; first verified
  event within 15 minutes; no recurring cost.

### B — Marcus, the at-risk / evidence user · **THE "WHY WE EXIST" STORY**
A tenant documenting unauthorized entry; could be a journalist, activist, or abuse
survivor. Needs records that hold up and can't be quietly altered — by anyone,
including a coercive party with physical access.
- **Buy:** wants a cheap, unobtrusive, **pre-flashed** device.
- **Friction:** must self-source and flash an ESP32; break-glass + trustees are
  CLI-only; export is CLI with no legal-grade bundle.
- **Good:** pre-flashed kit; guided trustee setup; one-tap "export for evidence" →
  signed bundle + verifier link.
- **Acceptance:** a non-developer sets 2-of-3 trustees in a UI and produces a signed
  export a court/lawyer verifies independently — proving no tampering, revealing no
  unrelated data.

### C — The Chen family, mainstream homeowners · **BIGGEST TAM, BIGGEST GAP**
Want a camera that doesn't spy on them and has no monthly fee. Will not touch a
terminal, YAML, or "trustees." Today SecuraCV cannot serve them.
- **Buy:** a finished device in a box.
- **Friction:** no retail product; requires HA + terminal; "coarse time / no faces"
  conflicts with expectations.
- **Good:** app + QR onboarding; the privacy model explained as a *feature*.
- **Tension to win:** mainstream buyers expect facial recognition and exact clips.
  SecuraCV refuses both. Winning means *reframing* the refusal ("it can't be used to
  spy on you, even by us"), not apologizing. **A 3–5 year target, not a launch one.**

### D — Dana, the Canary hardware tinkerer
Wants a small mesh of tamper-aware sensors.
- **Friction:** DIY-only; flashing/provisioning is technical.
- **Good:** pre-flashed kit + guided provisioning; rich tamper/transport sensors
  already exist in the HA integration.
- **Acceptance:** a device joins the mesh and appears in HA within minutes of
  power-on; tamper events (enclosure, power loss, SD removal, jamming) raise
  distinct binary sensors.

### E — Sam, the small-business / civic operator · **UPSIDE, NOT LAUNCH**
A shop, co-op, or city pilot that needs accountable cameras without running a
surveillance apparatus. "We cannot mass-search this footage" is a compliance and
liability *advantage*.
- **Good:** multi-camera standalone; an export suitable for an insurer/police
  report; documentation framing non-queryability as a feature.

---

## 6. The friction ledger — *least friction* (ranked)

The user's explicit ask is least friction / highest satisfaction. This is the spine.

| # | Friction (today) | Who it loses | The fix | Persona |
|---|------------------|--------------|---------|---------|
| 1 | **The terminal** — `curl \| bash`, manual add-on, hand-downloaded ONNX model | everyone past homelab | One-click HA add-on; detection model **bundled**; zero terminal on the happy path | A, C |
| 2 | **Invisible payoff** — verified timeline/digest buried in raw sensors; README has a TODO where the screenshot goes | everyone (no "wow") | A real timeline + verified-badge surface; mobile digest/alerts out of the box | A, B |
| 3 | **Unbuyable device** — Canary is DIY-flash-only | B, C, D | A **pre-flashed Canary kit** | B, C, D |
| 4 | **Unusable proof** — break-glass + export are CLI | B, E | Guided trustee UI; one-tap signed export + **standalone verifier** | B, E |
| 5 | **Unshipped v1** — undercuts "rely on this for evidence" | all | Close the v1 gates and **tag it** | all |
| 6 | **Default = motion, not objects** — gap vs "AI detection" expectations | A, E | Make the object-detection path a first-class, documented install option (it's honest today, just not easy) | A, E |
| 7 | **Slow first install** — add-on builds from source (~5–10 min on Pi 4) | A | Pre-built Docker images | A, E |

Every fix above is achievable **within the invariants** — none requires weakening a
privacy guarantee.

## 7. The satisfaction ledger — *highest satisfaction*

Satisfaction here is not "more features." It is the *feeling of trustworthy calm*.

1. **The daily "all clear."** A morning digest — *"4 events, all zones normal, every
   witness verified ✓"* — is the product's heartbeat and the #1 reason to keep it.
   It is partly built; finish and feature it.
2. **Visible integrity, always.** Put the verified badge and chain status on every
   event. The one thing no other camera can claim is "this can't be edited — even by
   us." Don't hide the differentiator; make it the UI.
3. **Constraints sold as the point.** Coarse ~10-minute time buckets, no faces,
   24-hour memory — each is a *promise*, not a limitation. Marketing copy and UI
   copy should say so explicitly. Never apologize.
4. **Proof that travels.** A one-tap, court-grade signed export + a free standalone
   verifier turns the moat into something a non-engineer can *use* under stress —
   the single highest-leverage satisfaction feature for persona B.
5. **Patterns over identities.** "Front door active at 3am" or "garage silent for
   48h" is genuinely useful *without ever naming a person* — anomaly detection on
   coarse statistics, not identity. (See [`07`](07-timeline-events-privacy-design.md).)

---

## 8. Designing the timeline without becoming surveillance

The timeline is where surveillance normally creeps in (search, faces, exact times,
"who was here"). SecuraCV must deliver the *usefulness* people want while
structurally refusing the surveillance. People actually want a timeline to answer
three questions, all answerable within the invariants:

1. *"Is everything normal?"* → patterns & anomalies, not identities.
2. *"Did something happen while I was away?"* → event clusters per zone, not a face
   roster.
3. *"Can I prove what happened?"* → a verifiable, tamper-evident record, not a
   searchable archive.

**Build:** a review-oriented, *sequential* timeline (no search box, no person
filter) with zone, coarse time bucket, object size class, confidence, and a
verified ✓ badge per entry; patterns/anomalies as the headline value; break-glass
(quorum, as a UI not a CLI) as the rare deliberate path to a clip; integrity status
always visible.

**Refuse** (each refusal is a marketing asset): face thumbnails/name tags (Inv. II),
"search for the person in red" (non-queryable), exact-second timestamps (Inv. III),
cross-zone "follow this person" (identity + correlation), re-running new AI over old
events (Inv. VI), one-click playback for anyone (break-glass by quorum).

**Five-year ideas that *strengthen* privacy:** cross-device co-signed witnessing
(multiple Canaries attest one event — evidentiary weight without identity);
third-party RFC-3161 timestamping on the chain head (court-grade, hashes only);
C2PA/content-credential interop for sealed clips (self-authenticating against
deepfake challenges); ephemeral-ID community "Chirp" alerts; zero-knowledge-style
log summaries ("no gaps in this window," "N events in zone X") that prove properties
without revealing events.

> **Design rule:** make the timeline answer *"is this normal and can I prove it?"* —
> never *"who was that?"* — and every privacy constraint becomes a feature.

---

## 9. The line we will not cross

Saying no *is* the product. The following are **permanently out** — each breaks an
invariant and each refusal is a competitive asset (see [`06`](06-feature-prioritization.md)):

| Tempting | Invariant broken | Verdict |
|---|---|---|
| Facial recognition / re-ID / plates | II — no identity substrate | **Never** |
| Searchable/bulk-query archive ("find the person in red") | VII — non-queryable | **Never** |
| "Privacy off" / longer-retention convenience toggles | a toggle can be coerced | **Never** |
| Retroactive reprocessing of old events with new rules | VI — no retroactive expansion | **No** |
| Cloud custody of footage / managed video relay | IV — local ownership | **No** (metadata-only push relay OK; footage never) |

---

## 10. Business model

**Open-core software + sold hardware + an optional verification service.** Monetize
the *trust*; give away the *software*. This keeps incentives aligned *with* the
privacy thesis instead of against it.

- **Free & open (Apache-2.0):** kernel, HA integration, firmware, self-host path.
  The trust engine and top of funnel. **Never paywall privacy.**
- **Primary revenue — pre-flashed Canary hardware/kits.** Serves personas B/C/D who
  can't flash an ESP32; hardware margin is a clean, honest revenue line that doesn't
  compromise the thesis.
- **Optional paid service (margin, never required) — court-grade attestation.**
  Third-party RFC-3161-style timestamping, notarized export bundles, C2PA interop so
  a sealed clip is self-authenticating against deepfake challenges. Operates on
  **signatures and hashes, never raw footage**, preserving local ownership. Lawyers,
  insurers, journalists, and civic operators will pay for "provably untampered."

**Rejected:** pure donations (lowest sustainability; fine only as a fallback) and
any **managed cloud relay that touches footage** — it directly attacks the brand's
core promise. A metadata-free push relay (tokens only) is the *only* acceptable
cloud, and it must never become a data path.

### 10.1 Illustrative unit-economics sketch (directional, not a forecast)

*Numbers below are illustrative placeholders to frame the model, not projections —
real pricing requires a BOM and channel analysis.*

- **Canary kit:** if BOM + assembly lands around the low-tens of dollars and the kit
  sells in the prosumer-accessory band, hardware gross margin can fund the UX/app
  work that unlocks personas B and C. The strategic point is that hardware margin is
  *aligned* — selling more devices spreads more privacy, not more surveillance.
- **Attestation service:** priced per export bundle or as a low monthly tier for
  professional users (lawyers/insurers/journalists). Marginal cost is a timestamping
  call + storage of hashes, so gross margin is high and the data risk is ~zero
  (no footage ever leaves the owner).
- **Funnel:** free OSS self-host (Priya) → community trust + contributions →
  hardware purchases (B/C/D) → attestation upsell (B/E). Each tier feeds the next
  without any tier compromising the invariants.

---

## 11. Roadmap (sequenced for compounding trust)

### Phase 0 — Ship v1 (credibility unlock)
Close the documented v1 gates so "rely on this for evidence" stops being hollow:
- Frigate → HA MQTT **release gate** green (`integrations/ha_frigate_mqtt/verify_pipeline.sh` exits 0 against a live stack).
- **RTSP end-to-end** verified in CI (documented feature → in scope).
- **"Audit boundary vs security boundary"** documentation item closed.
- **Firmware** exposes no raw MAC / precise GPS (documented invariants hold on-device).
- Tag v1; align `CHANGELOG.md` and `v1-roadmap.md` so docs never outrun code.

### Phase 1 — Kill the terminal + reveal the payoff (least friction)
- One-click HA add-on install with the **detection model bundled**.
- A polished **timeline + verified-badge** surface; **mobile digest/alerts** out of
  the box.
- **Guided trustee setup** and **one-tap signed export** with a **standalone
  verifier**.

### Phase 2 — Make it buyable (revenue + reach)
- **Pre-flashed Canary kit**; pre-built Docker images; multi-camera standalone;
  configurable detection (Coral/GPU; confidence threshold is hardcoded 0.5 today).
- Launch the **attestation tier** (RFC-3161 + C2PA interop).

### Phase 3 — The mainstream (horizon)
- Consumer **app + QR onboarding** and a boxed device; cross-device co-signed
  witnessing; ephemeral-ID community alerts — privacy model marketed as the
  headline, never an apology.

---

## 12. Risks and how we hold the line

| Risk | Mitigation |
|------|-----------|
| **Feature pressure to add identity/search** (the most dangerous risk) | The invariants + `CONTRIBUTING.md` treat these as law; every refusal is reframed as marketing. Hold absolutely. |
| **Bus factor = 1** (single author, young repo) | The exceptional docs/specs lower onboarding cost; prioritize a contributor on-ramp and a second maintainer before scaling. |
| **"Missing features" perception** (no faces, no exact clips) | Reframe constraints as the product in every surface; lead persona is the prosumer who *wants* this. |
| **v1 slips** | Phase 0 is small and explicit; treat the four gates as the only thing between here and a credible launch. |
| **Hardware execution** (a new muscle) | Start with a pre-flashed *kit* (lower ops burden than a boxed consumer device); use it to fund the harder app/retail work. |
| **Cloud temptation** (remote access asks) | Metadata-only push relay is the firm ceiling; footage never leaves the device. |

---

## 13. Success metrics

v1 is successful, and the strategy is working, when:

1. A non-developer installs from the HA add-on store **with no terminal** and sees a
   first **verified ✓** event within 15 minutes.
2. The morning digest is the notification users **keep**, not mute.
3. A non-engineer sets 2-of-3 trustees in a UI and produces a signed export a lawyer
   verifies independently — proving no tampering, revealing nothing else.
4. An external auditor confirms the privacy claims **by reading the code**.
5. The documentation **never** outruns the implementation.

> **The decision rule for every future feature:** usefulness must come from
> **trust, patterns, and proof** — never from identifying people or enabling search.
> Build that, and every privacy constraint becomes the reason people choose us.
