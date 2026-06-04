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
signing key. Now you can rotate the device signing key (`DEVICE_KEY_SEED`) without
re-encrypting the database — the DB key is unchanged.

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
4. You may now rotate `DEVICE_KEY_SEED` independently; the DB stays accessible.

## Verifier CLIs

`log_verify` / `export_verify` take the DB key directly via `--db-key` /
`SECURACV_DB_KEY`. When the database is keyed by an independent secret, pass the
derived hex key (from `derive_db_encryption_key_from_secret`) to those tools.
