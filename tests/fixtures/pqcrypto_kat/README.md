# Post-quantum known-answer fixtures (written by the pqcrypto crates)

**Written by `pqcrypto-mldsa` 0.1.2 and `pqcrypto-mlkem` 0.1.1** (the PQClean
C implementations of ML-DSA-44 and ML-KEM-768), through the kernel's own code,
on 2026-11-21, at `d648eb52` plus the tests that read these files. The build
was x86_64 Linux with the crates' default features, on a CPU with AVX2, so
key generation, signing and encapsulation ran PQClean's AVX2 code.

These files are what a device and a vault already hold: an ML-DSA-44 keypair
as `device_metadata` stores it, signatures over log entries, an ML-KEM-768 key
file, and envelopes sealed to it. The tests that read them hold whichever
implementation the kernel links to that material. **Never regenerate them.**
A file written by a later implementation proves nothing about the data the
earlier one left on disk. The keys here protect nothing; they exist to be
published.

| File | Size | SHA-256 | Contents |
|---|---|---|---|
| `mldsa44_kat.json` | 13467 B | `7723a25804210ad4eb58c32492c6c7ecb962fe63d6714681f1805a6251a3e69b` | `pq_public_key` (1312 B) and `pq_secret_key` (2560 B, the FIPS 204 private-key encoding) exactly as `PqKeypair::public_key_bytes()` and `secret_key_bytes()` return them and `device_metadata.pq_public_key` / `.pq_secret_key` store them; `pq_signature` (2420 B) and `ed25519_signature`, the signature set `sign_with_domain()` produced over `entry_hash` under `domain` (the sealed-log entry domain), with the Ed25519 key of seed `[42; 32]` |
| `mlkem768_kat.json` | 28393 B | `eeff92c423c35b739a1054d9d6d41cbf1aa54ea1d988995f77f9109b29377c14` | `key_file` (3592 B), the `kem-mlkem768.key` a vault directory holds: `le32(1184) ‖ public ‖ le32(2400) ‖ secret`, the secret being the FIPS 203 decapsulation key; `public_key` and `secret_key`, its two halves; `master_key`, that directory's `master.key`; `ciphertext` (1088 B) and `shared_secret`, one encapsulation to the key; `rejected_ciphertext` and `rejected_shared_secret`, the same ciphertext with one bit flipped and the implicit-rejection secret it decapsulates to; `envelopes`, three `.vault` files sealed to the key with their plaintexts |
| `hybrid_envelope.json` | 52260 B | `4d6b606eef9b02720bb5940314faebae174f6efcfb43b8da45facb99c4c462f3` | an evidence envelope (`envelope_format: securacv-evidence-envelope`) from a kernel built with `pqc-signatures`: a retention checkpoint, two sealed entries and an export receipt, each signed with Ed25519 and ML-DSA-44, and the ML-DSA-44 public key in `provenance.pq_public_key`. One line of JSON: the signatures are byte arrays |

The three envelopes in `mlkem768_kat.json`:

| `mode` | `kem_alg` in the file | How it was sealed |
|---|---|---|
| `pq` | `ml-kem-768` | `FilesystemVaultStore` in `pq` mode: the DEK is derived from the KEM shared secret |
| `hybrid` | `hybrid-ml-kem-768` | `FilesystemVaultStore` in `hybrid` mode: the DEK is derived from the KEM shared secret and a secret wrapped under the master key |
| `legacy-or` | `ml-kem-768` | the shape hybrid mode wrote before `hybrid-ml-kem-768` existed (a wrap of the DEK itself beside the KEM ciphertext); nothing seals it anymore, so it was assembled from `seal_v2` in `pq` mode and `wrap_dek`, as the unit test `legacy_or_shaped_envelope_still_opens_with_either_secret` does |

## How they were written

Three `#[ignore]` tests, each run once:

```sh
cargo test --features pqc-signatures,pqc-vault --lib write_pqcrypto_mldsa44_kat -- --ignored
cargo test --features pqc-signatures,pqc-vault --lib write_pqcrypto_mlkem768_kat -- --ignored
cargo test --features pqc-signatures,pqc-vault --test pqcrypto_kat_fixtures write_pqcrypto_hybrid_envelope -- --ignored
```

- `write_pqcrypto_mldsa44_kat` (`src/crypto/signatures.rs`) calls
  `PqKeypair::generate()` and `sign_with_domain()`.
- `write_pqcrypto_mlkem768_kat` (`src/vault/crypto.rs`) opens one vault
  directory through `FilesystemVaultStore::new` in `pq` and then `hybrid`
  mode, seals an envelope in each through the store, and reads back the files
  the store wrote (`kem-mlkem768.key`, `master.key`, `<id>.vault`).
- `write_pqcrypto_hybrid_envelope` (`tests/pqcrypto_kat_fixtures.rs`) opens a
  kernel, seals four entries around a retention checkpoint and builds the
  envelope with `build_evidence_envelope_for_api`.

## What reads them

| Test | Holds |
|---|---|
| `crypto::signatures::tests::pqcrypto_kat` (`src/crypto/signatures.rs`) | the stored keypair loads and re-encodes byte for byte; the stored signature verifies in strict mode; the stored key signs and those signatures verify; one flipped byte, another message, domain or key, or a wrong length fails |
| `vault::crypto::tests::pqcrypto_kat` (`src/vault/crypto.rs`) | the key file loads through `FilesystemVaultStore::new` and is not rewritten; the stored ciphertext decapsulates to the stored shared secret, and the rejected one to the stored rejection secret; all three envelopes open, through decapsulation and not only the master-key wrap |
| `tests/pqcrypto_kat_fixtures.rs` | the envelope verifies with `verify_envelope` in strict mode, and stops verifying when any one ML-DSA-44 signature in it is altered; a kernel whose database holds the stored keypair loads it, signs with it, and `log_verify --sig-mode strict` passes the log |
