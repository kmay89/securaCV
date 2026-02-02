# Post-Quantum Signature Mode

This document summarizes the optional post-quantum (PQ) signature support in the
SecuraCV witness-kernel.

## Feature Flag

PQ signatures are **feature-gated**. Enable them with:

```
cargo build --features pqc-signatures
```

The current implementation uses **Dilithium2** via the `pqcrypto-dilithium`
crate. The PQ dependency is not compiled unless the feature is enabled.

## Signature Sets

When the feature is enabled, log entries are signed with a **SignatureSet**:

- **Ed25519** (always present)
- **PQ signature** (Dilithium2) when a PQ key is configured

Each signature set carries explicit scheme identifiers, and all signatures use
domain separation (per log type) to prevent cross-context reuse.

## Verification Modes

Verification tools accept a signature mode flag:

- **Compat**: accept **Ed25519 _or_ PQ** (and also legacy Ed25519 signatures
  created before domain separation was introduced).
- **Strict**: require **both Ed25519 _and_ PQ** signatures to verify.

Strict mode fails if a PQ signature is missing or if the verifier does not have
the PQ public key.

## Key Storage

When PQ signatures are enabled, the kernel generates a Dilithium2 keypair and
stores it in `device_metadata`:

- `pq_public_key` (public key bytes)
- `pq_secret_key` (secret key bytes, local-only)

PQ keys are local-only and are **never transmitted** by the kernel. External
verification tools must be provided the PQ public key explicitly (or read it
from the local database).

## Limitations

- **Large signatures**: Dilithium2 signatures are much larger than Ed25519.
  Expect increased database size and slower verification.
- **Key rotation**: No PQ key rotation or migration tooling is included yet.
- **Strict mode requires PQ public keys**: Verifiers must supply PQ public keys
  when strict mode is enabled.
