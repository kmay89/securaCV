# Break-Glass Protocol (Invariant V)

The break-glass workflow provides quorum-gated access to sealed vault envelopes.
It is designed to be auditable, time-bounded, and usable only with trustee
approval. This document describes the protocol implemented by the
`break_glass` CLI and the kernel data model.

## Overview

The break-glass flow has four phases:

1. **Policy configuration**: Store an `n-of-m` trustee quorum policy in the
   kernel database.
2. **Request creation**: Generate a time-bucketed unlock request and share the
   request hash with trustees.
3. **Trustee approvals**: Trustees sign the request hash and produce approval
   files.
4. **Authorization + unseal**: An operator authorizes the request with the
   approvals, receives a time-bounded token, and unseals the envelope.

Every authorization attempt logs an immutable receipt (granted or denied), and
tokens are only valid for a specific envelope + ruleset + time bucket.

**Scope note — event exports.** The quorum gate is mandatory for sealed vault
envelopes and unsealing. For the privacy-filtered *event artifact*
(`export:events`), break-glass is one of two authorization modes: the owner may
also self-export it with the device key seed alone
(`export_events --self-export`), in which case the signed export receipt is
labeled `auth_mode: "self_export"` instead of `"break_glass"` (see
`spec/evidence_envelope.md` §8). Vault access never has a self-service path.

## Policy configuration

The quorum policy is stored in the kernel database and must be configured
before any authorization can occur.

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  break_glass policy set \
  --threshold 2 \
  --trustee alice:0123... \
  --trustee bob:4567... \
  --db witness.db
```

Each trustee entry is `id:HEX_PUBLIC_KEY`, where the public key is the
hex-encoded 32-byte Ed25519 verifying key. The policy is required by
`break_glass authorize` and by receipt verification (`break_glass receipts`).

## Policy changes — quorum-gated

Configuring a policy on an **empty** database is the bootstrap: there is no
quorum yet to consult, so the device key seed alone suffices, and the change
is recorded as a bootstrap in the policy-change history.

**Changing a live policy — roster, threshold, or `vault.crypto_mode` — is
quorum-gated**: it requires `n` distinct approvals from the *current*
trustees, signed under a dedicated policy-change domain (disjoint from unlock
approvals, so neither consent can stand in for the other). Without this gate,
one actor holding the device key seed could replace the roster and then
satisfy "quorum" with keys they minted (Invariant V; design rationale in
`spec/quorum_unseal_v2.md` §3.1).

```bash
# Operator: write a proposal file carrying the FULL current + proposed policies
break_glass policy propose \
  --threshold 2 --trustee alice:0123... --trustee bob:4567... \
  --output change.proposal --db witness.db

# Each current trustee: review the displayed diff, then sign.
# The tool recomputes the change hash from the proposal's full contents —
# a tampered proposal is refused, not signed.
break_glass policy approve \
  --proposal change.proposal --trustee alice \
  --signing-key alice.key --output alice.policy-approval

# Operator: apply with the collected approvals (within the same
# 10-minute freshness window the proposal was created in)
break_glass policy set \
  --threshold 2 --trustee alice:0123... --trustee bob:4567... \
  --approvals alice.policy-approval,bob.policy-approval --db witness.db
```

Every accepted change (bootstrap included) appends a chained, device-signed
record to the **policy-change history** (`break_glass policy history`
verifies and prints it), carrying the full previous and new policies and the
approvals, so an audit can reconstruct the roster's lineage and tie each
receipt's `policy_commitment` to the era that produced it. The record binds a
commitment to the authorizing approvals **inside** the signed payload, and the
audit **authenticates the whole ledger** before trusting any era: it checks the
hash chain, the device signature, that the recorded approvals match the signed
commitment, and that every non-bootstrap transition actually carries its prior
era's N-of-M consent. A fabricated era therefore cannot launder a forged
receipt — a device-key holder can sign a row but cannot manufacture the prior
trustees' consent.

Guided setup (`break_glass init` + `trustee enroll`) commits the policy once
the roster is **complete** — enrollment before that point edits only the
draft, never a live policy. Growing or shrinking a live roster afterwards is
an ordinary quorum-gated policy change.

Honest scope note: within the threat model's host-trust boundary this gate is
a procedural and audit control — an actor with host access can still rewrite
database rows out of band, but such a rewrite leaves no valid history record.
Making the quorum the *cryptographic* lock is the threshold-custody tier
(`docs/security/ENTERPRISE_CUSTODY.md` §1).

The policy also carries **vault crypto settings**. `vault.crypto_mode` controls
how v2 vault envelopes protect their per-object DEK:

- `classical`: master-key wrap only
- `pq`: ML-KEM (FIPS 203) encapsulation + KDF-derived DEK
- `hybrid`: both classical wrap and ML-KEM

If `vault.crypto_mode` is omitted, it defaults to `classical`.

## Request creation

An unlock request binds the envelope id, ruleset hash, purpose, a 10-minute
time bucket, and the **operator context** (`spec/quorum_unseal_v2.md` §3.6):
the printed name of the person asking (`--requested-by`), a structured
reason code from a closed vocabulary (`--reason`; the list is enumerated
once, in `REASON_CODES` in `src/break_glass/core.rs` — codes name the
process a disclosure serves, never a claim about what the evidence will
show), and optionally a case reference (`--case-ref`) and a claimed
requester public key (`--requester-key`). All of it is bound into the
request hash, so trustee consent covers exactly who asked and why. Write
the full request context to a file for trustees:

```bash
break_glass request \
  --envelope envelope_id \
  --purpose "incident response" \
  --requested-by "Alice Operator" \
  --reason incident-review \
  --case-ref case-2026-0042 \
  --ruleset-id ruleset:v0.3.0 \
  --output-request unlock.request
```

Every consent-bound text field (envelope id, purpose, requester name, case
reference) is capped at 512 bytes and may not contain control characters,
line or paragraph separators, or Unicode format and default-ignorable code
points (bidirectional controls, zero-width and other invisible characters,
variation selectors, the TAG block) — these strings are shown to trustees
before they sign and rendered into the permanent record, so no field may
forge a line or hide a character, and two names that differ only by an
invisible code point cannot hash differently while rendering identically.
Tools display them through `display_safe`, which neutralizes anything a
record from another build could carry; the served console's `displaySafe`
mirrors the same code-point set.

Send trustees the **context file** (format `securacv-unlock-request:v2`),
not a bare hash — their tool can then show them exactly what they are
consenting to. v1 files from older tools are still accepted, with a notice
that they carry no operator context; a file claiming v1 while carrying
context fields is refused (the v1 hash cannot bind them). (Without
`--output-request` the CLI still prints the request hash for the legacy
flow.)

**Approvals redeem only in the bucket the request was opened in.** A
request whose 10-minute window has rolled over is denied at authorization
with a full receipt (no trustees recorded as used) — approvals collected in
bucket T cannot be banked and spent later, and a served session cannot hold
a request open across windows. Any future cooling-off delay layers ON TOP
of this rule as explicit receipted state; the redemption window itself is
never widened.

## Trustee approvals — sign what you see

Each trustee's own tool recomputes the request hash locally from the context
file's fields, displays the decoded request (envelope, purpose, ruleset, UTC
validity window), and only then signs with the trustee's local Ed25519 key.
A context file whose fields do not hash to its claimed request hash is
refused, not signed — so a relay that swaps the displayed meaning while
keeping the hash gets caught at the trustee's machine, and a "just sign this
hash" social-engineering call has nothing the tool will accept.

```bash
break_glass approve \
  --request unlock.request \
  --trustee alice \
  --signing-key /path/to/alice.signing.key \
  --output alice.approval
```

`--request-hash <hex>` remains available for compatibility; it is **blind
signing** — the tool cannot show what it authorizes — and prints a warning
saying so. (Design rationale: `spec/quorum_unseal_v2.md` §3.2.)

On the served console, the trustee signing link carries every field the hash
binds (as the server normalized them) and the signer page recomputes the
request hash from those fields in the browser before enabling the signature;
Sign ships disabled and is enabled by exactly one path — the displayed fields
reproduce the hash. A link whose fields do not produce its hash is refused,
and so is a link carrying only a hash (an older console, or a full link
stripped down in transit): a signature over a number nobody can read is not
consent, and that trustee signs from the request file instead. The link
carries the request fields in clear text and persists in browser history, so
it travels over the trustees' precommitted channel like the request itself.
The status endpoint likewise exposes the bound operator context, so the
served surface shows what the hash covers rather than only the hash.

Approvals are collected by the operator and passed to the authorization step.

## Authorization and token issuance

Authorization checks the approvals against the stored quorum policy, logs a
receipt, and (if granted) issues a token bound to the envelope id and time
bucket. The token is sensitive and **must be written to a file** with
`--output-token`; it is not printed to stdout.

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  break_glass authorize \
  --request unlock.request \
  --approvals alice.approval,bob.approval \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --output-token /path/to/break_glass.token
```

`--request <file>` is the preferred input: authorize re-verifies the file
and reproduces the exact hash the trustees consented to, operator context
included. The field flags (`--envelope`, `--purpose`, and the §3.6 context
flags) remain available, but every value must match what `request` used or
the approvals will not count.

The receipt records the purpose and operator context alongside the outcome,
trustees used, and policy commitment — the receipt, not anyone's memory,
answers who asked and why. Verification re-derives the request hash from
those recorded fields and refuses a receipt whose recorded context is not
what the trustees signed.

## Unsealing

Use the token file to unseal the envelope. The output is written to the
specified output directory (default: `vault/unsealed`) with restricted file
permissions.

```bash
break_glass unseal \
  --envelope envelope_id \
  --token /path/to/break_glass.token \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --vault-path vault/envelopes \
  --output-dir vault/unsealed
```

## Auditing receipts

Receipts form an append-only, signed chain in the kernel database. Use the
`break_glass receipts` command (or `log_verify`) to validate the chain and
confirm that approvals align with the stored quorum policy. Verification
also re-derives each receipt's request hash from its recorded purpose and
operator context. The audit path tolerates a receipt that recorded no
context (a pre-§3.6 build's row has nothing to bind and is reported as it
is), but the unseal gate does not: a `Granted` receipt without a recorded
purpose and request bucket is refused before any cleartext is released,
because every receipt this build writes records both, so that shape is
either an older build's token or a re-signed row with the consent-bound
context stripped. Either way the remedy is to re-authorize.

`break_glass receipts --verbose` additionally prints each receipt's
deterministic human-readable record — printed name, UTC time, and the
meaning of every signature (the 21 CFR 11.50 triad) — so the same receipt
renders as the same record on every tool.
