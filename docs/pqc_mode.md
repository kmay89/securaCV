# Post-Quantum Cryptography

This document summarizes the optional post-quantum (PQ) cryptographic support in
the SecuraCV witness-kernel.

## Feature Flags

PQ features are **feature-gated**:

```
cargo build --features pqc-signatures   # ML-DSA-44 signatures
cargo build --features pqc-vault        # ML-KEM-768 vault encryption
cargo build --features pqc-tls          # Hybrid PQ key exchange for the MQTT bridges' client TLS
```

Dependencies are not compiled unless the corresponding feature is enabled. None
of the three is in the default build: a default build signs with Ed25519 only,
seals vaults with ChaCha20-Poly1305 only, and its MQTT TLS is classic
(`MQTT_TLS_BACKEND=hybrid_pq` exits with an error unless `pqc-tls` is compiled in).

## Signatures (pqc-signatures)

Uses **ML-DSA-44** (FIPS 204) via the [`ml-dsa`](https://crates.io/crates/ml-dsa)
crate (RustCrypto, pure Rust). Signing is the standard's hedged form with an
empty context string: fresh randomness goes into every signature, so two
signatures over the same entry differ.

When the feature is enabled, log entries are signed with a **SignatureSet**:

- **Ed25519** (always present, always required)
- **PQ signature** (ML-DSA-44) when a PQ key is configured

Each signature set carries explicit scheme identifiers, and all signatures use
domain separation (per log type) to prevent cross-context reuse.

### Verification Modes

- **Compat**: Ed25519 must verify. PQ signature is validated only if present
  (legacy entries without PQ signatures pass). The legacy `"dilithium2"` scheme
  identifier from pre-migration data is accepted (treated as an alias and not
  rejected as unsupported); the signature bytes are still verified with the
  ML-DSA-44 verifier — there is no separate legacy Dilithium2 algorithm.
- **Strict**: require **both Ed25519 _and_ PQ** signatures to verify.

Strict mode fails if a PQ signature is missing or if the verifier does not have
the PQ public key.

### Key Storage

When PQ signatures are enabled, the kernel generates an ML-DSA-44 keypair and
stores it in `device_metadata`:

- `pq_public_key` (public key bytes: the 1312-byte FIPS 204 encoding)
- `pq_secret_key` (secret key bytes, zeroized on drop: the 2560-byte FIPS 204
  private-key encoding)

PQ keys are local-only and are **never transmitted** by the kernel.

The stored secret key is the expanded encoding, not a seed. `ml-dsa` imports
and exports it through functions it marks deprecated in favor of 32-byte
seeds; a key that was stored expanded has no seed to return to, so the kernel
depends on that import for as long as such keys exist. A stored secret key
whose secret-vector coefficients are out of range is refused, as an error,
when the kernel opens.

## Vault Encryption (pqc-vault)

Uses **ML-KEM-768** (FIPS 203) via the [`ml-kem`](https://crates.io/crates/ml-kem)
crate (RustCrypto, pure Rust).

The vault's keypair is `kem-mlkem768.key` in the vault directory: the
1184-byte encapsulation key and the 2400-byte decapsulation key (the expanded
FIPS 203 encoding, not a seed; `ml-kem` marks its import deprecated in the
same way). Both FIPS 203 input checks are made when that file is loaded,
before anything is sealed to the key or opened with it: the modulus check on
the encapsulation key (section 7.2) and the hash check on the decapsulation
key (section 7.3). A file that fails either does not open the vault.

When enabled, vault envelopes can use hybrid encryption:

- **Classical**: ChaCha20Poly1305 with master-key-wrapped DEK
- **PQ**: KEM-encapsulated DEK (ML-KEM-768)
- **Hybrid** (labeled `hybrid-ml-kem-768`): the DEK is derived from the ML-KEM
  shared secret and a classical secret wrapped under the master key, so an
  envelope opens with both or not at all. There is no fallback to either half.

Envelopes written before that shape are labeled `ml-kem-768` and carry a
classical wrap of the DEK itself, so either secret alone opens one: an OR, no
stronger than its weaker half. They still decrypt and are never written again.
Where the KEM ciphertext in one is legacy Kyber768, decapsulation yields a
wrong secret without an error (ML-KEM rejects implicitly), the payload fails
its authentication check, and the envelope opens through the classical wrap.

## Builds through 0.9.3

Kernel builds through 0.9.3 linked the PQClean bindings for these two
features (`pqcrypto-mldsa` and `pqcrypto-mlkem`; 0.1.2 and 0.1.1 in 0.9.3).
Those crates are unmaintained and carry RUSTSEC-2026-0586 and
RUSTSEC-2026-0585; what that meant for a build that used them is in
[`SECURITY.md`](../SECURITY.md#known-dependency-advisories), Group 7.

Nothing they wrote changes meaning: keys, signatures, key files and envelopes
keep their encodings, labels and file names.
[`tests/fixtures/pqcrypto_kat/`](../tests/fixtures/pqcrypto_kat/README.md)
holds a keypair, signatures, a signed envelope, a vault key file and sealed
envelopes written by the kernel while it linked those crates, and the tests
beside it hold the current implementation to them.

## Limitations

- **Large signatures**: ML-DSA-44 signatures (~2.4 KB) are much larger than
  Ed25519 (64 B). Expect increased database size.
- **Key rotation**: No PQ key rotation or migration tooling is included yet.
- **Strict mode requires PQ public keys**: Verifiers must supply PQ public keys
  when strict mode is enabled.
- **Unaudited implementations**: `ml-dsa` and `ml-kem` say in their own
  READMEs that they have never been independently audited. Ed25519 stays
  required on every entry in both verification modes, and a `hybrid` envelope
  needs the master-key secret as well as the KEM secret; in `pq` mode an
  envelope rests on ML-KEM alone.
- **Stored keys are not seeds**: both crates read the stored private keys
  through imports they have deprecated (above). A later release of either
  that drops the import leaves existing keys unreadable until there is a way
  to replace them, which is the missing rotation tooling.
