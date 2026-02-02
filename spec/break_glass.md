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

The policy also carries **vault crypto settings**. `vault.crypto_mode` controls
how v2 vault envelopes protect their per-object DEK:

- `classical`: master-key wrap only
- `pq`: ML-KEM (FIPS 203) encapsulation + KDF-derived DEK
- `hybrid`: both classical wrap and ML-KEM

If `vault.crypto_mode` is omitted, it defaults to `classical`.

## Request creation

An unlock request binds the envelope id, ruleset hash, purpose, and a 10-minute
time bucket. The CLI prints the request hash that trustees must sign.

```bash
break_glass request \
  --envelope envelope_id \
  --purpose "incident response" \
  --ruleset-id ruleset:v0.3.0
```

Share the printed request hash with trustees.

## Trustee approvals

Each trustee signs the request hash with their local Ed25519 signing key to
produce a portable approval file.

```bash
break_glass approve \
  --request-hash <request_hash> \
  --trustee alice \
  --signing-key /path/to/alice.signing.key \
  --output alice.approval
```

Approvals are collected by the operator and passed to the authorization step.

## Authorization and token issuance

Authorization checks the approvals against the stored quorum policy, logs a
receipt, and (if granted) issues a token bound to the envelope id and time
bucket. The token is sensitive and **must be written to a file** with
`--output-token`; it is not printed to stdout.

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  break_glass authorize \
  --envelope envelope_id \
  --purpose "incident response" \
  --approvals alice.approval,bob.approval \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --output-token /path/to/break_glass.token
```

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
confirm that approvals align with the stored quorum policy.
