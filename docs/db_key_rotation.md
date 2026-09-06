# Database key rotation & decoupling

SecuraCV stores its witness log in a **SQLCipher-encrypted** database. This document
explains how the database encryption key is chosen and how to rotate it.

## Two keys, deliberately separable

| Secret | Purpose | Source |
|--------|---------|--------|
| **Device signing key** | Ed25519 identity that signs every event + checkpoint | derived from `DEVICE_KEY_SEED` |
| **DB encryption key** | SQLCipher key protecting the database at rest | see below |

By default the DB encryption key is derived from the signing key
(`derive_db_encryption_key`), so a fresh install needs only `DEVICE_KEY_SEED`. The
downside is coupling: rotating the signing key would change the DB key and lock you
out of the existing database.

To **decouple** them, set an independent secret:

```bash
export SECURACV_DB_KEY_SEED="<a long, high-entropy secret, distinct from DEVICE_KEY_SEED>"
```

When `SECURACV_DB_KEY_SEED` is set (non-empty), the kernel derives the DB key from
**that** secret via HKDF-SHA256 (`resolve_db_encryption_key`), independent of the
signing key. The encrypted database now decrypts regardless of the signing key, which
is the **storage-layer prerequisite** for rotating the device identity.

## Rotating the device signing identity

Decoupling the DB key (above) removes the *storage* blocker. The device **signing**
identity is rotated with [`Kernel::rotate_device_identity`], which keeps the entire
hash-chained log verifiable across the change:

```rust
// Open under the current identity (DB key must be decoupled — see above — so the
// encrypted database still opens once the signing key changes).
let mut kernel = Kernel::open(&cfg)?;       // cfg.device_key_seed = current seed
kernel.rotate_device_identity(&new_seed)?;  // appends a signed rotation record
// Subsequent events are signed by the new key. Reopen with the new seed afterwards.
```

What happens under the hood:

1. A **`KeyRotation` record** is appended to the sealed log, hash-chained like any entry
   and **signed by the retiring (old) key** — proving the legitimate holder authorized
   the rotation and making it tamper-evident. Its payload carries the new public key plus
   a **possession attestation**: the *new* key's signature over `(old_pub ‖ new_pub)`, so
   a rotation cannot announce a key the rotator does not control.
2. The new key is recorded in an append-only `device_key_history` table (the durable
   lineage) alongside **two** signatures over `(old_pub ‖ new_pub)`: the new key's
   *attestation* (possession) **and** the retiring key's *authorization*. Because each
   epoch is signed by its predecessor and the chain is rooted at the genesis key, the
   lineage is reconstructible and **unforgeable** from an untrusted history table — a
   tamperer cannot forge the genesis key's authorization without the genesis private key.
   Checkpoints also record which key signed them (`signer_public_key`).
3. The kernel switches its active signing key; `device_metadata.public_key` stays as the
   immutable **genesis** anchor.

**Verification** anchors at the genesis key and reconstructs the validated key lineage
(`reconstruct_device_key_lineage`), then *follows* each rotation record — validating the
old-key entry signature, the retiring-key authorization, and the new-key attestation — so
the whole log verifies end-to-end across one or more rotations. Deleting a rotation record
is **fail-closed**: post-rotation signatures then no longer verify. Key selection (the
suffix seed after pruning, and the trusted checkpoint signer) comes only from the
genesis-anchored lineage — never from the unauthenticated checkpoint/history row being
verified, so a tampered checkpoint key is rejected rather than trusted.

> **Upgrade compatibility.** Rotation records and `device_key_history` rows written before
> the explicit predecessor-authorization existed carry none. They are not rejected: the
> lineage instead recovers the same genesis-anchored guarantee from the retained in-chain
> rotation record, whose entry is signed by the predecessor key. Such a legacy rotation
> only fails verification if its in-chain record was also pruned away — the one case that
> cannot be anchored.

> **Prerequisite — decouple the DB key first.** Because the default DB key is derived
> from the signing key, you must set `SECURACV_DB_KEY_SEED` (independent secret) *before*
> rotating; otherwise the rotated signing key would derive a different DB key and the
> encrypted database would no longer open. Reopening a rotated log with a **retired** seed
> is rejected with `device public key mismatch`.

> **Scope / limitations.** Rotation applies to the **sealed event log + checkpoints**,
> and remains verifiable across retention **pruning**: the key lineage survives pruning in
> the genesis-anchored `device_key_history` table, so a checkpoint that prunes past a
> rotation still verifies. Post-quantum (`pqc-signatures`) keys are not rotated by this
> operation. Break-glass / export **receipts** continue to verify under the genesis key.

[`Kernel::rotate_device_identity`]: ../src/lib.rs

> `SECURACV_DB_KEY_SEED` is a *seed* the key is derived from. It is different from
> `SECURACV_DB_KEY`, which the verifier CLIs accept as the already-derived 64-char
> hex key directly.

> `SECURACV_DB_KEY_SEED` is a *seed* the key is derived from. It is different from
> `SECURACV_DB_KEY`, which the verifier CLIs accept as the already-derived 64-char
> hex key directly.

## Rotating the DB key itself

To change the database encryption key (e.g. migrating an existing DB from a
signing-key-derived key to an independent secret, or rotating the independent
secret), use `rekey_database_file()` **while the database is not open**:

```rust
use witness_kernel::{derive_db_encryption_key, derive_db_encryption_key_from_secret,
                      rekey_database_file, signing_key_from_seed};

// Old key: whatever the DB is currently encrypted with.
let old = derive_db_encryption_key(&signing_key_from_seed(&old_device_seed)?);
// New key: derived from the independent DB secret.
let new = derive_db_encryption_key_from_secret(new_db_secret.as_bytes());

rekey_database_file("witness.db", &old, &new)?;   // re-encrypts every page in place
```

`rekey_database_file` authenticates with the old key first (a wrong key is rejected
before any change), then issues SQLCipher `PRAGMA rekey`. After it returns, the DB
opens only with the new key.

Typical migration to a decoupled key:

1. Stop all processes using the database.
2. `rekey_database_file(db, old_signing_derived_key, new_secret_derived_key)`.
3. Start the kernel with `SECURACV_DB_KEY_SEED` set to the new secret.

After this the DB key is independent of the signing key — the prerequisite for rotating
`DEVICE_KEY_SEED` itself via [`Kernel::rotate_device_identity`] (see
[Rotating the device signing identity](#rotating-the-device-signing-identity) above).

## Verifier CLIs

`log_verify` / `export_verify` / `log_anchor` / `court_export` and
`break_glass receipts` / `policy history` / `policy show` / `unseal` take the DB
key directly via `--db-key` / `SECURACV_DB_KEY`. `break_glass db-key
--device-key-seed …` prints the key the kernel derives (honoring
`SECURACV_DB_KEY_SEED` when set), which is how an operator hands a verifier
the database key without the signing seed.
