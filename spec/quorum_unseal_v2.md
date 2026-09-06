# Quorum & Unseal v2 — Threshold Custody, Ceremony, Spine, Disclosure
Status: Draft v0.1
Intended Status: Design Specification (spec-only; not implemented except where noted)
Author: The Witness Project
Last Updated: 2026-09-06

## 0. Scope and status

This document specifies the target design for the next generation of the
break-glass quorum and unseal process. It is the engineering distillation of
the standards research recorded in
[`docs/security/PROVENANCE_INTEROP.md`](../docs/security/PROVENANCE_INTEROP.md),
and it upgrades — never replaces — the shipped v1 protocol in
[`break_glass.md`](break_glass.md).

**Maturity:** ⚪ spec-only, with four exceptions that are implemented and
normative as of this revision: **quorum-gated policy mutation** (§3.1),
**WYSIWYS approval** (§3.2), and **human-context fields** (§3.6) — all
documented operationally in `break_glass.md` — plus **`court export` for
event-export bundles** (§5, see its inline annotation). Everything else
here is design: treat it as the agreed direction, not a shipped contract. Known gaps this design closes are tracked
canonically in
[`docs/security/ENTERPRISE_CUSTODY.md`](../docs/security/ENTERPRISE_CUSTODY.md);
this spec is the "how", that tracker is the "whether it shipped yet".

The design is one coherent machine with four layers:

1. a **threshold lock** (quorum custody of key material, §2),
2. a **scripted human ceremony** around it (§3),
3. a **witnessed anchoring spine** under it (§4),
4. a **court-ready disclosure surface** on top (§5).

Nothing in it weakens an invariant (`invariants.md`); every externally
visible artifact adopts a ratified format rather than a bespoke one.

## 1. Design principles

- **The quorum must be the lock, not a gate in front of the lock.**
  Possession of the vault directory must be insufficient to decrypt
  (closes ENTERPRISE_CUSTODY §1).
- **Changing the approver set is at least as hard as what the approvers
  gate.** (Implemented; §3.1.)
- **Humans sign what they see** (WYSIWYS). A bare hash is never the primary
  input to consent. (Implemented; §3.2.)
- **Verify possession, knowledge, and context binding — never face or
  voice.** Real-time deepfake detection is a losing race; the protocol must
  not depend on winning it.
- **Attribution is sacred.** Authorization is always individual, per-trustee
  signatures. Threshold *signing* (e.g. FROST) is never the authorization
  record.
- **Time is a defense.** Deliberation windows with unilateral veto beat
  instant execution for a process whose entire purpose is exceptional.
- **No automated agent may construct, transmit, or approve a break-glass
  artifact.** Approval authority is human and non-delegable. AI assistance
  ends where consent begins.
- **Fail closed, receipt everything, anchor the receipts.**

## 2. Layer 1 — Threshold custody (the lock)

### 2.1 Key hierarchy

v2 introduces a **vault KEK** between trustee custody and per-envelope DEKs:

```
trustee shares (n-of-m, VSS)  ──reconstruct──►  KEK
KEK ──unwraps──► per-envelope DEK ──decrypts──► envelope payload
```

- The KEK is split n-of-m with **Shamir secret sharing hardened by
  verifiable secret sharing** (Feldman or Pedersen commitments). Raw Shamir
  is rejected: its silent-wrong-reconstruction failure mode is an
  operational hazard the VSS commitments eliminate (any share is provably
  valid or invalid against the public commitments, at issuance and at every
  unseal).
- VSS commitments are recorded in the receipt chain at the issuance
  ceremony.
- Every share lives in a **self-describing envelope** (split UUID, n, m,
  share index, checksum, digest of the master secret — the SLIP-0039
  lesson): a share found in a drawer years later identifies itself and its
  quorum.
- Shares are **encrypted at rest to their trustee's key** (baseline: the
  trustee's Ed25519-derived key; with credential hardening, the trustee's
  hardware token).
- Reconstruction happens **only in memory inside the unseal gate**, after
  quorum authorization, and is zeroized immediately after DEK unwrap — the
  KSK storage-master-key model; FIPS 140-3 Level 3–4 "split knowledge" in
  that vocabulary.

### 2.2 Post-quantum layer

The ML-KEM-768 decapsulation key is regenerated from its seed, and the seed
receives the **identical VSS split**. Both layers of the hybrid wrap are then
under quorum custody today, without waiting for threshold ML-KEM (research
grade as of 2026). The design leaves a clean seam: when a standardized
threshold KEM exists, it replaces the seed split without changing the
envelope format.

### 2.3 Two quorums

Following the KSK operational/recovery pattern:

- **Operational quorum**: the existing trustees, threshold n, used for
  unseal.
- **Recovery quorum**: a higher-threshold, organizationally independent set
  holding escrow shares of the same KEK (e.g. 5-of-7 vs the operational
  3-of-7), for trustee-set loss. Enrollment, storage, and exercise of the
  recovery quorum are ceremonies (§3.4) with their own receipts.

### 2.4 Share lifecycle

- **Proactive resharing is a first-class ceremony from day one** (same KEK,
  new polynomial, old commitments marked revoked in the receipt chain),
  triggered by any personnel change or by the liveness process below —
  trustee sets decay; the design assumes it.
- **Liveness attestations**: quarterly, each trustee signs a nonce;
  the verifier warns when the count of demonstrably live trustees
  approaches n.
- **Revocation**: `policy revoke-trustee` emits a signed policy-change
  receipt and forces resharing before the next unseal.

### 2.5 Consumer tier (before/without enrolled custody trustees)

Installs that do not enroll custody trustees get the cheap step first:
`master.key` wrapped under an Argon2id passphrase or a TPM/PKCS#11 KEK held
**outside** the vault directory — possession of the vault files alone stops
being sufficient (ENTERPRISE_CUSTODY §1 item 2). This tier is honest about
what it is: single-custodian confidentiality, quorum-gated authorization.

## 3. Layer 2 — The approval protocol (the ceremony)

### 3.1 Quorum-gated policy mutation *(implemented)*

Changing the trustee roster, the threshold, or vault crypto settings when a
policy already exists requires approvals from the **current** quorum:

- A **policy-change proposal** binds the current policy's commitment, the
  proposed policy's commitment, and a time bucket. Its hash is signed by
  trustees under a dedicated domain
  (`securacv:pwk:policy-change-approval:v1`) — disjoint from unlock-request
  approvals, so neither signature can stand in for the other.
- `policy set` verifies ≥ n distinct current-trustee keys over the proposal
  hash, where the previous-policy commitment is **recomputed from the stored
  policy**, never trusted from the proposal file.
- **Bootstrap:** on a database with no policy, device-key-only configuration
  is permitted (there is no quorum yet to consult) and the stored history
  row records that fact visibly.
- Every accepted mutation appends a **policy history record** (previous and
  new policy JSON, proposal hash, the approvals, device signature), so an
  audit can reconstruct the roster's full lineage and tie each break-glass
  receipt's `policy_commitment` to the era that produced it.

Rationale: without this gate, one actor holding `DEVICE_KEY_SEED` could
replace the roster and satisfy "quorum" with keys they minted. Every system
surveyed (IANA KSK, HSM quorum administration, MPC custody policy engines)
makes approver-set mutation at least as hard as the act the approvers gate.

### 3.2 WYSIWYS approval *(implemented)*

The dynamic-linking half of the protocol was already present: the request
hash binds envelope + ruleset + purpose + time bucket, so any change
invalidates approvals. v2 adds the display half:

- `request` emits a **request-context file** containing every field plus the
  derived hash.
- `approve --request <file>` **recomputes the hash locally from the
  fields**, verifies it against the file's claimed hash, displays every
  decoded field (envelope, purpose, ruleset, UTC window), and only then
  signs. The trustee's tool — not the requester — is the source of what is
  being consented to.
- Hash-only input (`--request-hash`) remains for compatibility and prints a
  loud blind-signing warning naming the risk.

This neutralizes the two documented top attack paths at once: a deepfaked
"urgent — sign this hash" call fails against a tool that wants full context,
and a compromised operator machine fails because each trustee independently
recomputes the hash from displayed fields (the Bybit lesson).

### 3.3 Human protocol (normative once implemented)

- Requests travel as **full context over channels precommitted at
  policy-set time** — never as bare hashes over voice or video.
- Nothing is signed on the strength of a live call. A call may *initiate*;
  consent flows only through the trustee's own tool.
- Each trustee registers a **verification phrase and a duress phrase** at
  enrollment (hashed into the policy record). Face and voice are never
  identity signals.
- **No automated agent** may construct, transmit, or approve a break-glass
  artifact; agent output may inform a human, never substitute for one.

### 3.4 Ceremony mode

For high-stakes unseals and all custody ceremonies (issuance, resharing,
recovery):

- The ceremony **script is hash-committed before approvals are gathered**;
  the script hash rides inside the signed request hash, so consent covers
  the procedure, not just the object.
- Steps are walked in order; deviations become **first-class exception
  receipts** rather than notes.
- An optional **non-trustee witness principal** co-signs "observed,
  exceptions as listed" — the KSK internal-witness role.
- The receipt head is **anchored (RFC 3161) at ceremony close**.
- A sanitized transcript is exportable. A ceremony runbook
  (`docs/security/CEREMONY_RUNBOOK.md`, planned) and a Custody Practice
  Statement template (modeled on the KSK DPS) make deployments assessable.

### 3.5 Token semantics and cooling-off

- Tokens are **single-use** (durably consumed at the unseal gate —
  implemented) and bucket-bounded as in v1.
- v2 adds a default-on **`not_before` delay** (default 24 h) between
  authorization and unseal, with:
  - notification of **all m trustees** (signers and non-signers) over their
    precommitted channels,
  - a **single-trustee signed veto** that cancels the token (n to open, 1 to
    stop — the asymmetry is the point),
  - prompt anchoring of the authorization receipt, so the pending unseal is
    externally fixed in time,
  - an **emergency profile** with a higher threshold (e.g. m-of-m) that
    bypasses the delay under a distinctly labeled receipt: speed costs
    consensus; it is never forbidden.
- Dead-man / auto-unseal mechanisms are **rejected**: automatic disclosure
  without distributed consent violates Invariant V.

### 3.6 Human-context fields *(implemented for break-glass)*

Unseal and export record, inside the signed receipts (and only there —
Invariants II/III):

- a **structured reason** (short vocabulary + free text),
- an **operator principal** (display name, optional registered key),
- and receipts gain a deterministic **human-readable rendering** — printed
  name, UTC time, meaning of each signature (the 21 CFR 11.50 triad).

*(Implemented on the break-glass path: `OperatorContext` — requester name,
closed `REASON_CODES` vocabulary, optional case reference and claimed
requester key — binds into the request hash so trustee consent covers it,
is recorded inside the signed receipt, is re-derived at audit and at the
unseal gate (a receipt whose recorded context does not re-derive its
consented hash is refused), and renders deterministically via
`BreakGlassReceipt::render_human`. Operationally documented in
`break_glass.md`. Export receipts do not carry an operator principal yet —
that lands with the §5 unseal-output/sidecar work.)*

### 3.7 Trustee credentials

- Baseline: trustee signing keys are **Argon2id-passphrase-wrapped at rest**
  (two-component consent; converges with ENTERPRISE_CUSTODY §5).
- Policy knobs: **hardware-held trustee keys** (FIDO2/PIV/OpenPGP,
  non-exportable, touch-gated), enforced per-trustee at authorize; and
  **enrollment attestation** recorded beside the trustee entry and surfaced
  in receipts.

## 4. Layer 3 — Anchoring and witnessing (the spine)

- An **RFC 9162 §2.1.1 Merkle tree** (0x00/0x01 domain separation) is
  maintained over the same sealed-event hashes as the linear chain. The
  break-glass and export receipt-chain heads are included as ordinary
  leaves — cross-binding all chains so the audit trail cannot be truncated
  independently (ENTERPRISE_CUSTODY §2).
- The head is emitted as a **C2SP tlog-checkpoint signed note** (origin,
  decimal size, root). A signed `(size, root)` pair can detect rollback; a
  bare head hash cannot.
- Every hub/Canary runs a small **tlog-witness endpoint**; fleet peers
  cross-cosign each other's checkpoints on the sealing cadence. The freshest
  cosigned checkpoint, stored outside the DB, is the **fail-closed
  high-water-mark** for `run_full_verify`. No metadata leaves the home.
- The hub aggregates per-device heads into **one fleet checkpoint**; only
  that aggregate goes outward: RFC 3161 to two TSAs (one eIDAS-qualified,
  one independent) plus **OpenTimestamps** as a trust-kind-independent leg.
  Public witness networks / Sigsum are enterprise-tier opt-ins with a
  rotatable origin.
- Each anchor seals a **clock-provenance event** (the measured device-clock
  vs TSA `genTime` offset) — the direct rebuttal to timestamp challenges.
- Verification trust is a **tlog-policy file** (device-key pin + witness
  quorum). Without one, `run_full_verify` reports "self-consistent,
  identity unverified" — vocabulary deliberately aligned with C2PA's
  Well-Formed / Valid / Trusted tiers.
- `log_review attest` lets a named principal sign
  `(reviewed_through_head, count, findings)` — simultaneously the GxP
  audit-trail-review artifact and a second, human-signed monotonic floor.

## 5. Layer 4 — Disclosure surface (the product of an unseal)

- Every unseal emits **per-file handoff sidecars** (SHA-256 + disclosure
  receipt naming recipient, purpose, approvals, anchor reference) so
  downstream chain-of-custody systems dock onto ours with a verifiable seam.
- **`court export`** assembles the litigation bundle: media in native
  container; digests; chain excerpt; receipts rendered as a
  custody-and-control record; both timestamp tokens; auto-filled draft
  FRE 902(13)/(14) certifications (28 U.S.C. § 1746 form); the versioned
  plain-English system description (the "silent witness" foundation);
  per-file acquisition-metadata sheets (SWGDE practice); and a
  VERIFICATION file whose steps need only `sha256sum` and `openssl`. It
  refuses — or loudly warns — when the covered period is unanchored.
  *(Implemented for event-export bundles: the `court_export` tool packages a
  verified bundle with custody/identity binding, anchor tokens, certification
  drafts, and an explicit unanchored warning — `docs/court_export.md`. Vault
  unseal outputs and OTS anchoring remain design.)*
- Exports optionally carry the frozen **C2PA witness-manifest profile**:
  COSE claim under a self-signed X.509 wrap of the device key, exactly one
  hard binding, `c2pa.created` with capture provenance bound to
  `ruleset_hash`, **typed declared redactions** with region maps for privacy
  filtering, the sealed original as an ingredient by claim-signature hash
  only, RFC 3161 countersignature — and nothing else: no EXIF/GPS, no
  thumbnails, no soft bindings, no precise event times. Stock validators
  report *Valid*; relying parties elevate to *Trusted* by pinning the device
  certificate.
- Machine **claims** ("person detected") are packaged separately from
  machine **recordings**, with per-ruleset validation sheets keyed to
  `ruleset_hash` (the durable answer to expert-reliability scrutiny of
  analytic output).

## 6. Sequencing

1. Gate fixes: §3.1 + §3.2 *(done)*, §3.6 fields *(done for break-glass)*,
   ceremony/runbook docs.
2. `court export` + anchoring upgrades (§5, §4 anchor items).
3. Merkle tree, checkpoints, fleet witnessing, tlog-policy, `log_review`.
4. Token delay/veto (§3.5) + trustee-credential hardening (§3.7).
5. VSS-Shamir threshold wrap with resharing (§2) — the one large build;
   everything before it makes it meaningful.
6. C2PA profile (§5).
7. Enterprise-tier projections (FHIR AuditEvent, CAWG, conformance-program
   and qualified-ledger tracking).

## 7. Explicit rejections

Recorded so they are decisions, not omissions: soft bindings
(watermark/fingerprint identifiers); FROST-as-authorization; dead-man
auto-unseal; face/voice trustee verification; per-home-device public
witnessing by default; any AI agent holding approval authority.
