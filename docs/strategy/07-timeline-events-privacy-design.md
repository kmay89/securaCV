# 07 — Making the Timeline & Events Useful Without Removing Privacy

This is the crux of the product. A timeline is normally where surveillance creeps in: search,
faces, exact timestamps, "who was here." SecuraCV must deliver the *usefulness* people want from
a timeline while structurally refusing the surveillance. The constraints are fixed by
`spec/invariants.md` (coarse ~10-minute buckets, local zone IDs, no identity, no bulk query,
local ownership, no retroactive expansion) and the two permitted event types
(`BoundaryCrossingObjectLarge` / `…Small`).

## Principle: usefulness comes from trust, patterns, and proof — not identification

People actually want a timeline to answer three questions:

1. *"Is everything normal?"* → patterns & anomalies, not identities.
2. *"Did something happen while I was away?"* → event clusters per zone, not a face roster.
3. *"Can I prove what happened?"* → a verifiable, tamper-evident record, not a searchable archive.

SecuraCV can answer all three within the invariants.

## What to build

### 1. A review-oriented, *sequential* timeline (not a search box)
A vertical timeline of event clusters per zone (the `canary-vision` SPA already renders this).
Each entry shows: zone, coarse time bucket, object size class, confidence, and a **verified ✓ /
chain-intact** badge. Value = at-a-glance "what happened where," plus visible proof of integrity.
Deliberately **no free-text search, no person filter, no cross-zone correlation UI** — browsing
is sequential by design, which is the non-queryable invariant made visible.

### 2. Patterns & anomalies as the headline value
The daily **digest** (counts per zone) and **pattern alerts** (unusual-hour activity,
unexpectedly silent zones) are already partly built and are the real day-to-day payoff. Lean into
these: "front door active at 3am" or "garage silent for 48h" is useful *without ever naming a
person*. Anomaly detection runs on coarse event statistics, not on identity.

### 3. Break-glass as the rare, deliberate path to a clip
For the genuine "I need to see the footage" moment, the timeline links to a **break-glass** flow
(quorum of trustees) rather than a one-click playback. Make it a **UI**, not a CLI. The friction
is the feature: accessing raw payload is rare, multi-party, and logged as a receipt.

### 4. Visible integrity, always
Surface chain status and the verified badge prominently. The timeline's differentiator over every
other camera is that it can *prove it wasn't edited* — show that, don't hide it.

## What to refuse (and why)

| Tempting feature | Invariant it breaks | Verdict |
|---|---|---|
| Face thumbnails / "name tags" on the timeline | II — no identity substrate | **Never** |
| "Search for the person in the red jacket" | non-queryable; identity | **Never** |
| Exact-second timestamps | metadata minimization | **No** (coarse buckets only) |
| Cross-zone "follow this person" tracking | identity + correlation | **Never** |
| Re-run new AI over old events to "find more" | no retroactive expansion | **No** |
| One-click clip playback for anyone | break-glass by quorum | **No** (quorum required) |

Each refusal is a *marketing asset*: "this timeline cannot be turned against you, even by us."

## Outside-the-box, 5-year ideas that *strengthen* privacy

- **Cross-device co-signed witnessing.** Multiple Canaries that observe the same boundary
  co-sign one event. This raises evidentiary weight ("three independent devices attest this")
  *without* adding any identity — trust through redundancy, not surveillance.
- **Third-party cryptographic timestamping** (RFC-3161-style) on the chain head, so the log is
  provably "no later than" a time an outside authority attests — court-grade, and the basis for
  the optional paid attestation tier. Operates on hashes only.
- **C2PA / content-credential interop** for sealed clips, so an unsealed clip is
  *self-authenticating* against deepfake challenges — directly answers the 2026 evidence/deepfake
  problem (FRE 902(13)–(14)).
- **Community Chirp alerts with ephemeral IDs**: privacy-preserving "something's happening on the
  block" signals that expire and can't be linked across time — neighborhood awareness without a
  neighborhood surveillance graph.
- **Zero-knowledge-style summaries**: prove properties of the log ("no gaps in this window,"
  "N events in zone X") to a third party without revealing the events themselves.

## The one-sentence design rule

> Make the timeline answer *"is this normal and can I prove it?"* — never *"who was that?"* —
> and every privacy constraint becomes a feature instead of a limitation.
