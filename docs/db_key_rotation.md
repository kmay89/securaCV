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

> ⚠️ **Signing-key rotation is not yet fully supported end-to-end.** Decoupling the DB
> key removes the *storage* blocker, but `Kernel::open` also pins the device identity:
> it records the device public key in `device_metadata` on first open and rejects a
> later open whose `DEVICE_KEY_SEED` derives a different key (`device public key
> mismatch`). So today, changing `DEVICE_KEY_SEED` on an existing database still fails
> at that check. Completing rotation needs identity-rotation support (recording the new
> public key and keeping the signed log verifiable across the boundary), which is
> tracked as the remaining Stream B2 work. What *is* available now: an independent DB
> key, and DB-key rotation via `rekey_database_file()` below.

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

After this the DB key is independent of the signing key. Rotating `DEVICE_KEY_SEED`
itself is still gated by the `device_metadata` identity pin described above and is not
yet supported end-to-end.

## Verifier CLIs

`log_verify` / `export_verify` take the DB key directly via `--db-key` /
`SECURACV_DB_KEY`. When the database is keyed by an independent secret, pass the
derived hex key (from `derive_db_encryption_key_from_secret`) to those tools.
