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

## Where the device seed lives

Every process that opens the kernel resolves the device seed the same way
(`crypto::resolve_device_seed`):

1. `DEVICE_KEY_SEED` (or the binary's `--device-key-seed` flag), else
2. the seed file beside the database — `<db>.ed25519.seed`, e.g.
   `witness.ed25519.seed` for `witness.db` — else
3. for the write-side daemons only (`witnessd`, `witness_api`, `frigate_bridge`,
   `adapter_host`, `grove_vision2_ingest`, `break_glass_serve`), a fresh `devkey:` seed
   from the OS RNG, written to that file at mode 0600.

Only a **generated** seed is written. A seed supplied through the environment is used as given
and never copied to disk, so a deployment that keeps it in a secret store (a Docker secret, an
add-on option) does not find it on the data volume afterwards. The one exception is `witnessd`,
which keeps its historical behavior of writing an environment seed to the file when none exists
yet. A daemon refuses to start when the environment and an existing file disagree — two
identities cannot share one log.
Daemons log which source they used (`device key seed: seed file …`), never the value.
The verifier and export CLIs (`log_verify`, `export_verify`, `log_anchor`,
`court_export`, `export_events`) try the file when no seed flag is given but never create
one; the four verifiers use it for the database key only — a file beside the log under
audit is not an out-of-band identity anchor.

On Unix the seed file must be private to its owner. A file with any group or other
permission bit (after a `chmod 644`, or a copy made under a lax umask) is **refused**, and
the error names the fix: `chmod 600 <file>`.

## Rotating the device signing identity

Decoupling the DB key (above) removes the *storage* blocker. The operator command is
`break_glass rotate-identity`; stop every process that opens the database first:

```bash
export SECURACV_DB_KEY_SEED="<the independent DB secret>"   # prerequisite, see below
break_glass rotate-identity --db witness.db --generate
```

What it does, in order:

1. Reads the **retiring** seed — `--device-key-seed` / `DEVICE_KEY_SEED`, else
   `--seed-file`, else `<db>.ed25519.seed` — and opens the log with it. A retired seed is
   refused here, before anything changes.
2. Mints the successor from the OS RNG (`--generate`), or takes `--new-seed <seed>` /
   `NEW_DEVICE_KEY_SEED`, and validates it the way the kernel validates any seed.
3. **Stages** the successor as `<file>.new` (fresh, mode 0600, fsynced) beside every seed
   file that must follow the identity: `--seed-file`, and `<db>.ed25519.seed` whenever it
   exists (`--generate` always writes that one). The successor is durable on disk before
   the retiring seed stops opening the log.
4. Calls [`Kernel::rotate_device_identity`], renames each staged file over the live one,
   and reopens the log under the successor before reporting success.
5. Prints the retiring and current public keys and the lineage epoch — never a seed.

With `--new-seed` and no seed file anywhere, none is written: set the new value as
`DEVICE_KEY_SEED` where the kernel runs. A deployment that exports `DEVICE_KEY_SEED` from its
own key file (the HA add-on, the Docker sidecar) points `--seed-file` at that file. Restart
every process afterwards. A `<file>.new` left by an interrupted ceremony blocks the next one
until it is resolved: if the log still opens with the current seed, the staged seed was never
activated and can be removed; otherwise move it over the live file.

`--rekey-db-to <secret>` (or `SECURACV_NEW_DB_KEY_SEED`) performs the DB-key prerequisite in
the same ceremony when `SECURACV_DB_KEY_SEED` is not set yet: exactly what `rekey-db` does
([below](#rotating-the-db-key-itself)), after the preflight open and before the rotation.

The library call underneath is [`Kernel::rotate_device_identity`], which keeps the entire
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
> encrypted database would no longer open. `rotate-identity` refuses to start without it
> unless `--rekey-db-to` is given. Reopening a rotated log with a **retired** seed is
> rejected with `device public key mismatch`.

> **Scope / limitations.** Rotation applies to the **sealed event log + checkpoints**,
> and remains verifiable across retention **pruning**: the key lineage survives pruning in
> the genesis-anchored `device_key_history` table, so a checkpoint that prunes past a
> rotation still verifies. Post-quantum (`pqc-signatures`) keys are not rotated by this
> operation. Break-glass / export **receipts** continue to verify under the genesis key.

[`Kernel::rotate_device_identity`]: ../src/lib.rs

> `SECURACV_DB_KEY_SEED` is a *seed* the key is derived from. It is different from
> `SECURACV_DB_KEY`, which the verifier CLIs accept as the already-derived 64-char
> hex key directly.

## Rotating the DB key itself

To change the database encryption key (e.g. migrating an existing DB from a
signing-key-derived key to an independent secret, or rotating the independent
secret), stop every process that opens the database and run `break_glass rekey-db`:

```bash
break_glass rekey-db --db witness.db --new-db-key-seed "<new independent secret>"
```

The current key is `--old-db-key <hex>` (as `break_glass db-key` prints it), else derived
from `--old-device-key-seed` / `DEVICE_KEY_SEED` — honoring `SECURACV_DB_KEY_SEED` when it is
set, so rotating an already-independent secret works the same way — else from the seed file
beside the database. The new secret can come from `SECURACV_NEW_DB_KEY_SEED` instead of the
flag, which keeps it out of shell history. Re-keying to the key the database already has is
refused as a no-op; no key material is printed.

The command wraps `rekey_database_file()`, which must run **while the database is not open**:

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
2. `break_glass rekey-db --db <db> --new-db-key-seed <secret>` (the library call is
   `rekey_database_file(db, old_signing_derived_key, new_secret_derived_key)`).
3. Start the kernel with `SECURACV_DB_KEY_SEED` set to the new secret.

After this the DB key is independent of the signing key — the prerequisite for rotating
`DEVICE_KEY_SEED` itself with `break_glass rotate-identity` (see
[Rotating the device signing identity](#rotating-the-device-signing-identity) above).

## Verifier CLIs

`log_verify` / `export_verify` / `log_anchor` / `court_export` and
`break_glass receipts` / `policy history` / `policy show` / `unseal` take the DB
key directly via `--db-key` / `SECURACV_DB_KEY`. `break_glass db-key
--device-key-seed …` prints the key the kernel derives (honoring
`SECURACV_DB_KEY_SEED` when set), which is how an operator hands a verifier
the database key without the signing seed. Run on the device itself with
neither `--db-key` nor a seed, the first four derive the database key from the
seed file beside the database (never the verifying key: that still comes from
`--public-key` / `--public-key-file`, or the database, labeled self-consistent).
