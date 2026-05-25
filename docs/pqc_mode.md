# Post-Quantum Cryptography

This document summarizes the optional post-quantum (PQ) cryptographic support in
the SecuraCV witness-kernel.

## Feature Flags

PQ features are **feature-gated**:

```
cargo build --features pqc-signatures   # ML-DSA-44 signatures
cargo build --features pqc-vault        # ML-KEM-768 vault encryption
cargo build --features pqc-tls          # Post-quantum TLS transport
```

Dependencies are not compiled unless the corresponding feature is enabled.

## Signatures (pqc-signatures)

Uses **ML-DSA-44** (FIPS 204) via the `pqcrypto-mldsa` crate.

When the feature is enabled, log entries are signed with a **SignatureSet**:

- **Ed25519** (always present, always required)
- **PQ signature** (ML-DSA-44) when a PQ key is configured

Each signature set carries explicit scheme identifiers, and all signatures use
domain separation (per log type) to prevent cross-context reuse.

### Verification Modes

- **Compat**: Ed25519 must verify. PQ signature is validated only if present
  (legacy entries without PQ signatures pass). Also accepts legacy
  `"dilithium2"` scheme identifiers from pre-migration data.
- **Strict**: require **both Ed25519 _and_ PQ** signatures to verify.

Strict mode fails if a PQ signature is missing or if the verifier does not have
the PQ public key.

### Key Storage

When PQ signatures are enabled, the kernel generates an ML-DSA-44 keypair and
stores it in `device_metadata`:

- `pq_public_key` (public key bytes)
- `pq_secret_key` (secret key bytes, zeroized on drop)

PQ keys are local-only and are **never transmitted** by the kernel.

## Vault Encryption (pqc-vault)

Uses **ML-KEM-768** (FIPS 203) via the `pqcrypto-mlkem` crate.

When enabled, vault envelopes can use hybrid encryption:

- **Classical**: AES/ChaCha20 with master-key-wrapped DEK
- **PQ**: KEM-encapsulated DEK (ML-KEM-768)
- **Hybrid**: Both classical wrap and KEM ciphertext; decryption tries KEM
  first, falls back to classical if AEAD verification fails (handles legacy
  Kyber768 envelopes transparently)

## Limitations

- **Large signatures**: ML-DSA-44 signatures (~2.4 KB) are much larger than
  Ed25519 (64 B). Expect increased database size.
- **Key rotation**: No PQ key rotation or migration tooling is included yet.
- **Strict mode requires PQ public keys**: Verifiers must supply PQ public keys
  when strict mode is enabled.
