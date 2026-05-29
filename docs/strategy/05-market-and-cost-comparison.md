# 05 — Market, Cost Comparison & Product Verdict

## Cost comparison (2026)

The dominant consumer brands monetize via **subscription**. SecuraCV's local-first model has
**zero recurring fee**. Over a 5-year horizon — the design target — this is decisive.

| Product | Model | Subscription | 5-yr cost (4 cameras) | Footage location |
|---------|-------|--------------|-----------------------|------------------|
| **Ring** | Cloud | Basic $4.99 / Standard $9.99 / Pro $19.99 per mo | ~$300–$1,200+ in fees | Cloud |
| **Google Nest** | Cloud | Home Premium ~$5–10/mo | ~$300–$600 in fees | Cloud |
| **Eufy** | Local-first | Optional ~$2.99/mo per cam | $0–$700 | Local (HomeBase) + optional cloud |
| **Reolink** | Local-first | Optional | $0+ | microSD / NVR + optional cloud |
| **UniFi Protect** | NVR-first | None | Hardware only | Local (Dream Machine/Cloud Key) |
| **Frigate** | Self-host | Optional Frigate+ ~$5–10/mo | $0 (core free) | Local |
| **SecuraCV** | Self-host | **None** | **Hardware only** | **Local, auto-deleting** |

Industry framing: cloud 4-camera setups run **$144–$480/yr** in fees; the DVR/NVR/cloud math
"strongly favors local recording for anyone with 4+ cameras" over five years. Added hardware
for local AI is modest (Coral TPU $25–60; Raspberry Pi 4/5).

## Market context

- **Smart home** market ~**$90B (2026) → ~$139B (2032)**; another estimate ~$175B in 2026
  growing ~8.8%/yr. Large and growing.
- **DIY** segment growing ~**17.9% CAGR** — directly SecuraCV's lane.
- **Home Assistant** holds ~**10%** self-host share; tech-savvy, privacy-conscious users.
- **31%** of smart-home users cite **privacy** as a top concern; security influences **>60%**
  of purchase decisions.
- **Evidence tailwind**: courts in 2026 increasingly expect cryptographic hash verification,
  unbroken chain of custody, and the ability to rebut deepfakes (FRE 901, FRE 902(13)–(14)
  self-authentication, NIST SHA-256). Europol projects up to ~90% of online content could be
  synthetic by 2026. **This is SecuraCV's deepest, least-contested moat** — and almost nobody
  is selling it to consumers/prosumers.

## Why this IS a good product

- **Genuine, defensible differentiation.** Cryptographic witnessing + privacy-by-construction
  is not a feature competitors can toggle on — it's an architecture. Ring/Nest's business model
  *is* retaining and analyzing footage; they can't credibly copy "we can't see your data."
- **No subscription** in a market trained to resent subscriptions.
- **Rides two tailwinds**: privacy concern (31%+) and evidence authenticity / anti-deepfake.
- **Ecosystem fit**: lands natively in the fast-growing HA/DIY segment via HACS + Frigate.
- **Honest, coherent brand** ("witnessing not watching") that the target buyer trusts.

## Why it ISN'T (yet) a good product

- **Onboarding is developer-grade.** `curl | bash`, manual add-on, ONNX model hand-download,
  CLI-only break-glass. Excludes everyone past the homelab segment.
- **No polished UI/app.** The persuasive payoff (a verified ✓ timeline, a digest) is mostly
  HA sensors, not a product experience. No screenshots even in the README.
- **v1 hasn't shipped.** "Ready to rely on for evidence" is undercut by an unshipped v1 and
  unchecked roadmap boxes.
- **Hardware isn't productized.** Canary is DIY; the at-risk persona who most needs it can't buy it.
- **The privacy model fights mainstream expectations.** Buyers expect facial recognition and
  exact clips; SecuraCV deliberately refuses both. That's a *positioning* problem, not a
  product defect — it must be sold as the point, not apologized for.

## What it needs to become a great product (5-yr)

1. One-click install + bundled detection (kill the terminal).
2. A real timeline/verification UI and a working mobile experience.
3. Ship and tag v1; align `CHANGELOG.md` and `v1-roadmap.md`.
4. A productized, pre-flashed Canary device for non-builders.
5. A court-grade signed-export + standalone verifier — turn the moat into a feature people
   can *use*, not just an architecture they have to trust.
6. Positioning that reframes the constraints ("coarse time, no faces, 24-hour memory") as the
   selling proposition.

## Recommended business model (the owner left this undecided)

**Open-core software + sold Canary hardware + an optional verification service.**

- **Free & open (Apache-2.0)**: the kernel, HA integration, firmware, self-host path. Keep
  this genuinely free — it's the trust engine and the top of funnel. Never paywall privacy.
- **Hardware (primary revenue)**: sell **pre-flashed Canary devices / kits**. This serves the
  at-risk and mainstream personas who can't flash ESP32s, and hardware margin is a clean,
  honest revenue line that doesn't compromise the privacy thesis.
- **Optional paid service (margin, never required)**: a **court-grade attestation / verification
  service** — third-party RFC-3161-style cryptographic timestamping, notarized export bundles,
  C2PA / content-credential interop so a sealed clip is self-authenticating against deepfake
  challenges. Crucially it operates on **signatures and hashes, never raw footage**, so it
  preserves local-ownership and the invariants. Lawyers, insurers, journalists, and civic
  operators will pay for "provably untampered" — the deepfake era makes this only more valuable.

### Alternatives considered

- **Pure donations / pure OSS**: lowest friction, lowest sustainability; fine as a fallback but
  won't fund the hardware + UX work needed to reach personas B/C.
- **Managed cloud relay (paid)**: tempting for remote access/notifications, but any cloud custody
  of footage directly attacks the brand's core promise. **Reject** anything that puts footage or
  identity off-device. A *metadata-free relay* (push tokens only) could be an optional convenience,
  but it must never become a data path.

**Bottom line:** monetize the *trust* (hardware + attestation), give away the *software*. That
keeps incentives aligned with the privacy thesis instead of against it.

---

### Sources

- [Frigate vs Ring vs Arlo: Privacy and False Alarms (Privacy Smart Home, 2026)](https://www.privacysmarthome.com/guides/frigate-nvr-vs-ring-vs-arlo-privacy-and-false-alarms-2026/)
- [DVR vs NVR vs Cloud DVR: Full 2026 Comparison (PVRBlog)](https://pvrblog.com/dvr/vs-nvr-cloud/)
- [Security Camera Subscription 2026: Plans, Costs, Free Picks (PVRBlog)](https://pvrblog.com/cameras/tech/subscriptions/)
- [Ring Home Security Camera Costs & Pricing 2026 (Security.org)](https://www.security.org/security-cameras/ring/)
- [eufy Camera Systems 2026 Costs & Plans (Security.org)](https://www.security.org/security-cameras/eufy/)
- [How to Self-Host Frigate NVR (Localtonet)](https://localtonet.com/blog/how-to-self-host-frigate-nvr)
- [Smart Home Automation Market Size (Coherent Market Insights)](https://www.coherentmarketinsights.com/industry-reports/smart-home-automation-market)
- [Home Automation Market Size & Growth (Mordor Intelligence)](https://www.mordorintelligence.com/industry-reports/home-automation-market)
- [Smart Home Statistics 2026 (SQ Magazine)](https://sqmagazine.co.uk/smart-home-statistics/)
- [Video Evidence Authentication: Legal Standards for 2026 (DigitalEvidence.ai)](https://digitalevidence.ai/blog/video-evidence-authentication-standards-courts)
- [How Blockchain Secures Chain of Custody in an Era of AI Deepfakes (CPI OpenFox)](https://www.openfox.com/news/how-blockchain-secures-chain-of-custody-in-an-era-of-ai-deepfakes/)
- [Video Evidence, Deepfakes, and the New Burden of Proof (Mea Integrity)](https://www.mea-integrity.com/video-evidence-deepfakes-and-the-new-burden-of-proof/)
