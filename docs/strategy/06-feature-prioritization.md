# 06 — Feature Prioritization

Prioritized for the **lead segment (privacy-conscious prosumer)** first, with a path to the
at-risk/evidence and mainstream segments over 5 years. Every item is checked against the seven
invariants (`spec/invariants.md`) — anything that needs an invariant weakened is **cut**, not
deferred.

## Must-have (blocks credibility / adoption)

| Feature | Why | Persona |
|---------|-----|---------|
| **Ship & tag v1** + align `CHANGELOG.md` and `v1-roadmap.md` | "Rely on this for evidence" is hollow until v1 exists | All |
| **One-click install** (HA add-on store, no terminal) + **bundled detection model** | Kills the single biggest onboarding drop-off; removes the ONNX hand-download | A, C |
| **Timeline / verification UI in HA** (events + verified ✓ + chain status) | The persuasive payoff currently lives as raw sensors | A, B |
| **Working mobile push out-of-the-box** (digest + pattern alerts) | The daily "it's working" signal; already partly built | A, B |
| **Court-grade signed export + standalone verifier** | Turns the moat into something a non-dev can actually *use* | B, E |

## Should-have (strong ROI, next)

| Feature | Why | Persona |
|---------|-----|---------|
| **Pre-flashed Canary hardware kit** | Unlocks personas B and C; primary revenue line | B, C, D |
| **Break-glass / trustee setup UI** (no CLI) | The evidence flow must be usable under stress by a non-dev | B |
| **Pre-built Docker images** | Removes a build step for self-hosters | A, E |
| **Multi-camera standalone mode** (without requiring Frigate) | Simpler topology; civic/SMB use | E |
| **GPU / Coral detection + configurable confidence** | Performance and accuracy at scale (threshold is hardcoded 0.5 today) | A, E |
| **Third-party cryptographic timestamping / C2PA interop** | Deepfake-era evidence credibility; basis for the paid attestation tier | B, E |

## Nice-to-have (later / opportunistic)

- Cross-device **co-signed witnessing** (multiple Canaries attest one event) — strong trust
  story, see [07](07-timeline-events-privacy-design.md).
- Community **Chirp** alerts with ephemeral IDs (privacy-preserving neighbor alerts).
- Consumer mobile app + QR onboarding (gate to the mainstream persona; large effort).

## Cut / keep-deferred (do NOT build as-is)

| Item | Reason |
|------|--------|
| **Facial recognition / re-ID / license plates** | Violates Invariant II (no identity substrate). The brand's whole promise. **Permanent no.** |
| **Searchable / bulk-query archive, "find the person in red"** | Violates non-queryable invariant; turns a witness into a surveillance index. **No.** |
| **Optional "privacy off" / longer-retention-for-convenience modes** | `CONTRIBUTING.md` rejects optional privacy; a toggle people can be coerced to flip defeats the model. **No.** |
| **Retroactive reprocessing of historical events with new rules** | Violates no-retroactive-expansion invariant. **No.** |
| **Cloud custody of footage / managed video relay** | Attacks local-ownership; the core anti-promise. **No.** (A metadata-only push relay is acceptable; footage is not.) |
| **LoRa transport, SCQCS audio transport** | Real but low-priority; keep deferred until a concrete need. |

## The guiding rule

Usefulness must come from **trust, patterns, and proof** — not from identifying people or
enabling search. Every "wouldn't it be convenient if…" request that adds identity, query, or
retention is a competitor's product, not this one. Saying no to those *is* the product.
