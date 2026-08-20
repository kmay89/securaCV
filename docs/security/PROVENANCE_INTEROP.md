# Provenance & custody interop — speaking the world's evidence languages

SecuraCV's chains, receipts, and anchors were designed from first principles.
This document records what happened when we checked that design against the
regimes the rest of the world already trusts — courts and evidence law,
medical/pharma data integrity, content provenance, high-assurance key
ceremonies, and transparency logs — and against the adversary that frontier AI
models make cheap.

The result, stated honestly: **the technical substrate already meets or
exceeds the core of every regime surveyed, and what's missing is almost
entirely packaging, context, and format adoption — not cryptography.** The
strategy that follows from that is *elegant compatibility*: adopt the ratified
format wherever one exists, emit the artifact the regime actually consumes,
and never invent a bespoke mechanism where a standard one fits. Nothing below
weakens an invariant; where a regime's expectation collides with an invariant,
the resolution is recorded in §4.

Two companion documents carry the load-bearing detail:

- [`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md) — the canonical tracker of
  gaps and designed-but-unbuilt work. Items below reference it rather than
  restate it.
- [`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) — the
  target design for the quorum + unseal upgrade (threshold custody, ceremony,
  anchoring spine, disclosure surface), distilled from this research.

---

## 1. What each domain requires, and where we stand

### 1.1 Courts and evidence law

**The fast path already exists in the rules.** US Federal Rules of Evidence
902(13) and 902(14) (in force since December 2017) make electronic records and
data copies **self-authenticating** on a written certification of a "qualified
person" — no live engineer testimony — when authenticity is shown by "a
process of digital identification." The Advisory Committee notes name
hash-value comparison as the paradigm method, and the certifier only needs to
be familiar with the system and the verification performed: the operator who
ran `envelope_verify` and `log_anchor verify` qualifies. Hash comparison has
been treated as settled science since *United States v. Cartier* (8th Cir.
2008); machine-generated log entries avoid hearsay under the
*Lizarraga-Tirado* line; and camera footage with no human eyewitness comes in
under the "silent witness" theory (FRE 901(b)(9)) on a prose description of a
process that produces an accurate, unalterable result — which is a
description of the sealed chain plus receipts.

**What we satisfy:** whole-envelope SHA-256 digests, per-event hashes, signed
export receipts tied to disclosure acts (Invariant IV), dual independent
verifiers, and RFC 3161 anchors verifiable with plain openssl — everything the
certification must *describe* already exists and is third-party checkable.

**What's missing:** the document the rule consumes. Nobody currently generates
the certification draft, the plain-English system description, or
verification instructions runnable by opposing counsel with `sha256sum` and
`openssl` alone. That is the `court export` bundle (§2, move 4).

**Adjacent currents worth tracking:**

- **eIDAS (EU 910/2014, amended 2024/1183):** a *qualified* electronic
  timestamp (ETSI EN 319 421/422 — the same RFC 3161 wire protocol) carries a
  legal presumption of accuracy and integrity across the EU, shifting the
  burden of proof to the challenger. Pointing one of the two recommended
  anchors at a qualified TSA is nearly free (§2, move 5). eIDAS 2.0 also
  created **qualified electronic ledgers** (Arts. 45k/45l): records in one
  enjoy a presumption of unique, accurate sequential ordering and integrity —
  a legal category the sealed chain was unknowingly built for. Full
  qualification is a large, enterprise-tier move; the non-discrimination
  clauses (a ledger may not be denied effect merely for being unqualified)
  apply today.
- **UK:** BS 10008 (evidential weight of electronic information) is a
  controls-and-policy standard an operator pack can map to (§2, move 14). The
  post–Post Office Horizon direction of travel penalizes vendor say-so and
  rewards independent verifiability — the dual-verifier stance is the
  textbook answer.
- **Proposed FRE 707** (machine-generated *analytic* output faces
  expert-reliability scrutiny): the answer is already latent in Invariant VI.
  Events are permanently bound to `ruleset_hash`, so per-ruleset validation
  documentation can be written once and reused in every case — while the
  footage itself stays on the expert-free 902 path. Keep machine *claims*
  ("person detected") packaged separately from machine *recordings*.

### 1.2 Medical / pharma data integrity (the strictest audit-trail regime)

21 CFR Part 11 requires "secure, computer-generated, time-stamped audit
trails" where changes never obscure prior entries — the hash chain satisfies
this **by construction**, more strongly than typical validated pharma systems.
HIPAA's integrity specification (45 CFR 164.312(c)(2): "mechanisms to
corroborate that ePHI has not been altered or destroyed in an unauthorized
manner") is a literal description of the sealed chain.

What these regimes score records on that we do not yet record is **human
context**, three fields, all additive:

1. **The named operator** behind each state-changing action (Part 11 audit
   trails; EU Annex 11 §12.4; FDA's data-integrity Q&A prohibits shared
   accounts — which `DEVICE_KEY_SEED`-authenticated CLI operations
   structurally resemble).
2. **Signature manifestation** (11.50): signed records display printed name,
   date/time, and the *meaning* of the signature. Receipts today carry trustee
   IDs and hashes, not names and meanings in human-readable form.
3. **The reason** for the action (Annex 11 §9; the draft Annex 11 revision's
   "who changed what, when and why"). Break-glass has free-text `--purpose`;
   it needs to be structured and mandatory at unseal.

These live **only inside signed receipts** — already consented disclosure
artifacts — never in telemetry or anchored digests (see §4.1). The ALCOA+
rubric (Attributable, Legible, Contemporaneous, Original, Accurate, plus
Complete/Consistent/Enduring/Available) is the right organizing frame for the
conformance mapping (§2, move 14). One feature closes a compliance gap and a
security gap at once: a signed **audit-trail review attestation**
("reviewed-through head X, count N") is simultaneously the FDA/MHRA/PIC-S
review artifact and a second, human-signed high-water-mark for
`run_full_verify` (§2, move 8).

### 1.3 Content provenance (C2PA)

The industry consolidated on C2PA Content Credentials (spec 2.x, advancing
through ISO as ISO/DIS 22144). The important structural facts for us:

- Ed25519 is on the allowed algorithm list; the mandatory claim content is
  tiny (instance ID, generator info, **one hard binding** over the asset
  bytes, one actions assertion); RFC 3161 countersignatures ride in the COSE
  header — our existing TSA flow, including the offline/air-gapped path, maps
  directly onto the `c2pa.time-stamp` assertion.
- The trust model is three-tiered (Well-Formed → Valid → Trusted): a
  self-signed device with no CA still yields **Valid**, and a relying party
  elevates it by pinning the device certificate in a private credential store
  — exactly our out-of-band verify-key stance, in their vocabulary.
- **Redaction is a first-class, signed, typed operation** — removals are
  declared in the claim, typed by label (e.g. PII), with region maps, and the
  actions history is non-redactable. This is the best available external
  grammar for our "prove faithful privacy filtering, not tampering" story.
- Soft bindings (watermarks/fingerprints) are optional ("zero or more") —
  and we decline them by policy: content-comparable identifiers plus lookup
  infrastructure are a correlation substrate (Invariants II/IV). Omission is
  fully conforming (§4.4).
- The industry's hard lesson (an in-camera C2PA signer was defeated via a
  capture-pipeline abuse within days of release, forcing a fleet-wide
  certificate revocation): **the sealed capture pipeline, not the signature,
  is the hard part** — which is the ground the witness kernel already stands
  on.

A frozen "witness manifest profile" (what we include, what we structurally
omit) is §2, move 13.

### 1.4 Key ceremonies and quorum custody

Every high-assurance system studied — the ICANN/IANA DNSSEC root KSK
ceremony, FIPS 140-3 / ISO 19790 multi-party control, PCI key ceremonies, HSM
vendor quorums, MPC custody — converges on the same architecture: **n-of-m as
verifiably-secret-shared key material, plus a scripted, witnessed,
exception-logged ceremony, with every artifact receipted.** The KSK's
credibility rests not on math but on pre-published scripts, a witness role
whose job is recording deviations, serial-numbered tamper-evident bags, and
public per-ceremony archives. Notably, AWS CloudHSM's quorum tokens —
portable approval files signed outside the HSM with a default expiry measured
in minutes — are almost exactly the shape break-glass already has.

The field's two demands we do not yet meet:

1. **Make the quorum the cryptographic lock**, not a policy gate upstream of
   it (ENTERPRISE_CUSTODY §1). Best-practice 2026 construction: a vault KEK
   split n-of-m with Shamir hardened by **verifiable secret sharing** (VSS
   commitments recorded in the receipt chain; raw Shamir fails silently),
   self-describing share envelopes, shares encrypted to trustee keys,
   reconstruction only in memory at the unseal gate, proactive resharing as a
   first-class ceremony. The ML-KEM-768 seed gets the identical split, so
   both hybrid layers gain quorum custody without waiting for threshold
   ML-KEM (still research-grade). Threshold *signing* (FROST, RFC 9591)
   deliberately stays out of the authorization path: per-trustee attribution
   is what the receipt chain and every audit regime depend on.
2. **Make changing the approver set at least as hard as what the approvers
   gate.** Today `break_glass policy set` is gated by the device key seed
   alone — one actor can replace the roster and then satisfy "quorum" with
   keys they minted. Every system studied quorum-gates its own policy
   mutation. This is the sharpest single gap found, and a precondition for
   the threshold wrap being meaningful (§2, move 1).

### 1.5 Transparency logs and external anchoring

The transparency-log ecosystem converged on a small stack of formats the
sealed chain can adopt nearly for free: the C2SP **checkpoint** (origin,
decimal tree size, root hash) in a signed-note envelope, witness
**cosignatures**, a log→witness protocol whose atomic monotonic-size check
*is* the signed external high-water-mark ENTERPRISE_CUSTODY §2 plans, and a
client-side trust-policy grammar for witness quorums. Certificate
Transparency itself is migrating onto this stack, and a public witness
network (seeded by hardware witness devices) already cosigns checkpoints for
major logs. RFC 9162 explicitly leaves the split-view problem to gossip;
witness cosigning is the ecosystem's shipped answer — a single honest witness
enforcing append-only monotonicity makes rollback and equivocation
detectable.

The one structural prerequisite: witnesses require **Merkle consistency
proofs** (RFC 9162 §2.1.4), which a linear hash chain cannot produce
compactly. Maintaining an RFC 9162 §2.1.1 Merkle tree over the same sealed
hashes — with the break-glass and export receipt-chain heads included as
leaves, which is simultaneously the §2 cross-binding item — is the medium
engineering change that unlocks the entire ecosystem (§2, moves 6–7).

The right tiering for a privacy-first product (see §4.2): fleet-internal
cross-witnessing by default (peers already share the LAN; nothing leaves
home); one fleet-level aggregate checkpoint outward to two TSAs (one
eIDAS-qualified) plus OpenTimestamps as a trust-kind-independent second leg;
public witness networks and Sigsum as enterprise-tier opt-ins.

### 1.6 The frontier-AI adversary

Frontier models do not attack the cryptography; they attack the two places a
human stands between a request and a signature. The documented 2024–2026
record — the Arup deepfake video-call fraud (~$25.6M), FBI IC3's ~$893M in
AI-linked fraud losses, FinCEN's alert on GenAI defeating identity
verification, and the Bybit theft (~$1.4B) where a 3-of-3 hardware multisig
was drained by swapping the *displayed meaning* of what the signers signed —
maps directly onto break-glass:

| # | Attack path | Countermeasure | Status |
|---|---|---|---|
| 1 | Deepfake/social-engineer trustees into signing an attacker-supplied request hash | **WYSIWYS approval**: the trustee's own tool ingests full request context, recomputes the hash locally, displays every field; bare hashes are never the primary input | §2, move 2 |
| 2 | Bybit pattern: compromised operator/UI swaps meaning under a blind-signed hash | Same — each trustee independently recomputes from displayed context | §2, move 2 |
| 3 | Theft of trustee key files (plain files on disk today) | Passphrase-wrap at baseline; hardware-held keys (FIDO2/PIV, touch-gated) and enrollment attestation as policy knobs | §2, move 11 |
| 4 | AI-forged process artifacts, backdated history | Receipt chain + anchoring already largely defeat this; close the tracked §2 gaps (high-water-mark, anchor-in-verify, cross-binding) | §2, moves 6–7 |
| 5 | Vault-file exfiltration bypassing the quorum entirely | The threshold wrap (ENTERPRISE_CUSTODY §1) — agentic AI has made intrusion cheaper, raising this gap's priority | §2, move 12 |
| 6 | Machine-speed vulnerability discovery against parsers/CLI | Fuzz every cross-boundary parser; reject-don't-normalize; constant-time comparisons | §2, move 15 |
| 7 | Sensor-level synthetic injection | Honest documentation of the capture trust boundary; capture-pipeline provenance in sealed events; sensor-proximate signing as the long-term direction | §2, move 16 |

The unifying principle, taken from FIDO/NIST doctrine rather than invented
here: **stop verifying humans by face or voice at all** — official guidance
concedes real-time deepfake detection is a losing race. Verify *possession*
(hardware key), *knowledge* (precommitted phrase), and *context binding*
(WYSIWYS) — none of which a model can synthesize from scraped media. And the
strongest structural defense is time: a **default-on, cancelable unseal
delay** with notification of every trustee turns "n people fooled in
parallel" into "n people fooled and zero of m notice for 24 hours" (§2,
move 10). One rule costs a paragraph now and closes a whole class later: **no
automated agent may construct, transmit, or approve a break-glass artifact.**
Approval authority is human and non-delegable.

---

## 2. The prioritized compatibility moves

Ordered by sequencing tier, not importance; "small/medium/large" is
engineering effort. Detailed designs live in
[`spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md); tracked gaps
stay canonical in [`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md).

**Tier 1 — gate fixes (small, highest leverage):**

1. **Quorum-gate policy mutation.** Changing the roster/threshold requires
   current-quorum approvals; device-key-only bootstrap on an empty DB stays,
   visibly labeled. *(Implemented — see `spec/break_glass.md` §"Policy
   changes".)*
2. **WYSIWYS approval.** `approve` ingests full request context, recomputes
   the hash locally, displays every field; hash-only input is a loudly
   warned fallback. *(Implemented — see `spec/break_glass.md`.)*
3. **Human-context fields**: structured reason at unseal, operator principal
   in receipts, deterministic human-readable receipt rendering (name, UTC
   time, signature meaning per 11.50).
4. **`court export` bundle**: media + digests + chain excerpt + receipts
   rendered as a custody-and-control record + TSA/OTS tokens + auto-filled
   draft FRE 902(13)/(14) certifications (28 U.S.C. § 1746 form) + system
   description + `sha256sum`/`openssl`-only VERIFICATION instructions;
   per-file handoff sidecars at unseal so downstream custody chains onto
   ours. *(Implemented for event-export bundles — the `court_export` tool,
   see `docs/court_export.md`. Still open: OTS tokens, and handoff sidecars
   on `break_glass unseal` outputs.)*
5. **Anchoring upgrades**: scheduled anchoring on by default; "two TSAs"
   becomes "one eIDAS-qualified + one independent"; OpenTimestamps as a
   second, trust-kind-independent leg; clock-provenance events (measured
   device-clock-vs-TSA offset) sealed at each anchor.

**Tier 2 — the spine (medium):**

6. **RFC 9162 Merkle tree** over the sealed hashes, receipt-chain heads as
   leaves (= the cross-binding item).
7. **C2SP checkpoint + fleet witnessing**: chain head emitted as a signed
   checkpoint note; every hub/Canary runs a tiny witness endpoint; peers
   cross-cosign on the sealing cadence; the freshest cosigned checkpoint is
   the fail-closed high-water-mark for `run_full_verify`; the hub aggregates
   per-device heads into one fleet checkpoint, which is what gets anchored
   outward.
8. **Review-as-a-record**: `log_review attest` — a named principal signs
   (reviewed-through head, count, findings); doubles as the GxP review
   artifact and a human-signed monotonic floor.
9. **Ceremony packaging**: hash-committed ceremony scripts (script hash
   covered by the signed request), exception receipts, optional witness
   co-signer, anchor at close; a ceremony runbook and a Custody Practice
   Statement template modeled on the KSK DPS.

**Tier 3 — custody and credentials (medium/large):**

10. **Token semantics + cooling-off**: single-use tokens (already durable),
    default-on cancelable `not_before` delay (~24h) with all-trustee
    notification and one-trustee veto; an emergency profile (higher
    threshold, e.g. m-of-m) bypasses the delay under a distinctly labeled
    receipt.
11. **Trustee credential hardening**: Argon2id passphrase-wrapped key files
    at baseline (converges with ENTERPRISE_CUSTODY §5); hardware-held keys
    and enrollment attestation as policy knobs.
12. **VSS-Shamir threshold custody** (ENTERPRISE_CUSTODY §1 item 1): the KEK
    split described in §1.4, two-quorum structure (operational + recovery),
    proactive resharing, liveness attestations. The cheap step first:
    passphrase/TPM KEK for `master.key` held outside the vault directory.

**Tier 4 — outward formats (medium/large):**

13. **C2PA witness manifest profile** on exports: minimal assertion set, hard
    binding, typed redactions for privacy filtering, sealed original as
    ingredient by hash, TSA countersignature; no metadata/thumbnails/soft
    bindings; self-signed cert with the private-credential-store pinning
    path documented.
14. **Compliance mapping suite** (`docs/compliance/`): Part 11 / ALCOA+
    clause→mechanism→status table; verification-methodology whitepaper;
    BS 10008 operator pack ("designed to support conformance", never
    "certified"); FIPS split-knowledge vocabulary for the threshold wrap;
    per-ruleset validation sheets (the FRE 707 answer).
15. **Parser and comparison hardening**: fuzz targets for every
    cross-boundary parser; reject-don't-normalize; constant-time equality
    on secrets; the seccomp allowlist inversion (ENTERPRISE_CUSTODY §3).
16. **Enterprise-tier projections** (all opt-in, all derived from receipts,
    never authoritative): FHIR AuditEvent / IHE ATNA shape for SIEM export;
    CAWG identity assertions for co-signing; qualified-ledger and C2PA
    conformance-program tracking; capture-pipeline provenance toward
    sensor-proximate signing.

---

## 3. What we deliberately do NOT adopt

- **Soft bindings / "durable" content credentials** (watermarks,
  fingerprints): content-comparable identifiers + lookup infrastructure are
  a correlation substrate (Invariants II/IV). C2PA makes them optional;
  durability comes from the sealed chain + anchors instead.
- **FROST group signatures for authorization**: compact, but they erase
  per-trustee attribution — the thing courts, GxP, and ceremony audits all
  require. Optional additive endorsement only.
- **Dead-man / auto-unseal mechanisms**: recurring in the social-recovery
  literature, rejected here explicitly — automatic disclosure without
  distributed consent violates Invariant V. Recorded to preempt the feature
  request.
- **Face/voice verification of trustees, ever** — see §1.6.
- **Per-home-device public witnessing by default**: a public witness-list
  entry is a stable device identifier (Invariant III). Fleet-internal
  witnessing is the default; only aggregate fleet checkpoints go outward.

## 4. Invariant tensions and their resolutions

1. **Named humans (Part 11/ALCOA) vs. metadata minimization (II/III).**
   Names, operators, and reasons live only inside signed receipts — already
   local, consented disclosure artifacts — never in telemetry or anchored
   digests (anchors see only a hash). Consumer deployments may use
   pseudonymous trustee IDs; the 11.50 name rendering is generated at export
   time for regulated deployments.
2. **External witnessing vs. no stable device identifiers (III).** Tiered:
   fleet-internal by default; one aggregate checkpoint outward; public
   networks enterprise-tier with a rotatable origin; fixed-cadence heartbeat
   sealing removes signal from tree growth.
3. **C2PA device certificate = cross-export linkability (II).** Accepted and
   documented: export is a deliberate, receipt-bound disclosure act. Not
   mitigated with per-envelope certs — that would break the pinning trust
   path.
4. **Soft bindings (recommended by others) vs. II/IV.** Declined; see §3.
5. **Precise TSA times vs. III.** Already settled: precise time attaches to
   the anchoring/disclosure *act*, never to observed events; event times in
   outward formats are omitted or bucketed.
6. **Cooling-off delay vs. genuine emergencies.** The delay is a default
   with a labeled pressure valve: a higher-threshold emergency profile
   bypasses it and the receipt says so. Speed costs consensus; it is never
   forbidden.
7. **Roster privacy vs. attribution.** Individual signatures remain the
   authorization record forever; FROST endorsement is additive only.
8. **Centralized regimes (qualified ledgers, C2PA conformance, ATNA, SIEM)
   vs. local-first (IV) and non-queryability (VII).** All absorbed as
   enterprise-tier projections *derived from* receipts, never authoritative;
   the consumer kernel stays self-signed + pinned. Transparency exports use
   proof-oriented layouts that structurally support only sequential access —
   no query surface.

In every collision the invariant either wins outright, is satisfied by
tiering, or is satisfied by locating the sensitive data in an artifact that
is already a consented disclosure. **No domain surveyed demands anything that
requires weakening an invariant.**

---

*Research base: FRE 901/902 and advisory notes; eIDAS 910/2014 as amended and
ETSI EN 319 421/422; BS 10008; 21 CFR Part 11 and FDA data-integrity
guidance; EU Annex 11 (including the draft revision); 45 CFR 164.312; ALCOA+;
IHE ATNA; C2PA 2.x and its conformance program; SWGDE video-evidence
practices; the ICANN DNSSEC KSK ceremony corpus (Key Management Policy, DPS,
ceremony archives); FIPS 140-3 / ISO 19790; PCI DSS key-management controls;
RFC 9591 (FROST); SLIP-0039; RFC 9162 and the C2SP
checkpoint/cosignature/witness specifications; Sigsum and the public witness
network; OpenTimestamps; NIST SP 800-63B; PSD2 RTS (EU) 2018/389 Art. 5; and
the 2024–2026 public record of AI-enabled fraud and AI-driven vulnerability
discovery. Where this document states a legal conclusion it is engineering
guidance, not legal advice.*
