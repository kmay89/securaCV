# Encrypted-vault operator UX & hardware-backed keys (v1.1) — design

> **Status:** Draft RFC for review. This is a *scoping* document — a design and a
> task breakdown, not an implementation. Nothing here ships until the open
> decisions at the end are settled.
>
> **Scope:** the operator experience around the encrypted vault — first-time
> setup, trustee enrollment, and the request→approve→unseal flow — plus moving
> private key material off plaintext-on-disk onto hardware-backed stores.
>
> **Explicitly out of scope for this doc:** the sealing/unsealing *cryptography*.
> That is already built and wired (see below); we are not redesigning it.

---

## 1. Summary

The vault's crypto is done and the break-glass machinery works end-to-end today:
you can set a quorum policy, open a request, gather real Ed25519 trustee
approvals (CLI **or** an in-browser signer), authorize, and unseal — every step
logged as a tamper-evident receipt. What's missing is everything *around* that
core:

1. **There is no guided setup.** An operator assembles hex key files by hand,
   types `id:HEX` trustee strings into `policy set`, and distributes signing keys
   themselves. The web console literally starts at "paste a token" and returns
   *"No quorum policy is configured"* if you haven't already done the CLI dance.
   There is no `init`, no `enroll`, no `drill`, no `doctor`.
2. **Every private key is plaintext on disk.** The vault master key is a 32-byte
   `master.key` file (mode `0600`); the device signing key is derived from a
   config seed; trustee keys are hex files. Nothing is backed by a TPM, secure
   element, or hardware token.

This RFC proposes closing both gaps in four phases: a **setup/enrollment UX**
layered on the crypto that already exists (Phase 1, zero new crypto), then a
feature-gated **`KeyStore`** abstraction whose realistic hardware backing is a
**plugged-in PKCS#11 token** on the host and the **ESP32-S3's own on-die security
block** on the device — **not a TPM** (§5.5) (Phase 2), **hardware tokens for
trustees** (Phase 3), and a **guided web wizard + docs + drills** (Phase 4).
Phase 1 is the lowest-hanging, highest-value fruit; the hardware phases need
physical devices to validate and are the reason this is v1.1, not v1.

This matches the roadmap, which already carves this out:
`docs/v1-roadmap.md` lists *"Encrypted-vault setup UX + hardware-backed keys
(v1.1) — sealing itself is already wired"* under **out of scope for v1**, and
Stream B notes the remaining crypto gap is *"key management … and a trustee/seal
setup UI — not the encryption itself."*

---

## 2. Current state — what is already built

Everything in this section exists in the tree today. File references are so a
reviewer can verify each claim by inspection (the same standard the
witness-kernel page holds itself to).

### 2.1 The sealing crypto (done)

- **Device-side `.svlt` seal** (`docs/sealed_snapshot_vault.md`, canary-wap
  firmware): ephemeral X25519 → HKDF-SHA256 (`info = securacv/vault/seal/v1`) →
  ChaCha20-Poly1305, with the byte-exact `SVLT` header bound in as AAD. Write-only
  escrow — the device holds only the recipient *public* key and cannot read back
  what it sealed.
- **Kernel `VLT2` envelope** (`src/vault/crypto.rs`, `src/vault/format.rs`):
  `AEAD_ALG_CHACHA20POLY1305`, `seal_v2`, magic `b"VLT2"`; DEK-wrap construction
  with `VaultCryptoMode::{Classical, Pq, Hybrid}`.
- **Wired into the daemon:** `witnessd` seals buffered frames via `seal_frame`
  (`src/bin/witnessd.rs`), gated on a valid `BREAK_GLASS_SEAL_TOKEN`.

### 2.2 The break-glass quorum flow (done, operationally)

- **Quorum policy:** `QuorumPolicy { n, m }` with `TrusteeEntry { id, public_key:
  [u8;32] }`, stored in the kernel DB, validated on load
  (`src/break_glass/core.rs`). Approvals counted by
  `count_valid_distinct_approvals`; grant requires
  `trustees_used.len() >= policy.n`. Bounds: `MAX_TRUSTEES = 32`,
  `MAX_APPROVALS = 64`.
- **CLI** (`src/break_glass/cli.rs`), subcommands:
  `request` · `approve` · `authorize` · `receipts` · `unseal` ·
  `policy {set, show}`.
- **Domain-separated signatures** (`src/crypto/signatures.rs`):
  `securacv:pwk:{trustee-approval, break-glass-token, break-glass-receipt}:v2`.
  A trustee's approval can't be replayed into any other signature context.
- **Web console** (`src/break_glass/breakglass.html`, served by
  `break_glass_serve` at `http://127.0.0.1:8800/breakglass`): a four-phase SPA —
  Connect → Open request → Collect approvals → Quorum & unseal. It mints a
  **trustee signing link** (`…/breakglass#sign&hash=…`); the trustee's key seed
  signs in-browser and the fragment never hits the server.
- **Tamper-evident receipts:** every authorize attempt — granted *or* denied —
  is hash-chained, Ed25519-signed, and independently verifiable
  (`break_glass receipts`, and the full audit in `log_verify`).
- **Single-use tokens:** `consume_break_glass_token_durably` burns the nonce
  *before* any cleartext exists, so a granted token can't authorize repeated
  unseals within its validity bucket (`cmd_unseal`, `src/break_glass/cli.rs`).

### 2.3 Where the keys live today (the gap, stated precisely)

| Key | Where it lives now | Backing | Rotation story |
|-----|--------------------|---------|----------------|
| **Vault master key** | `<root>/master.key`, 32 bytes, mode `0600` (`load_or_create_master_key`, `src/vault/mod.rs:385`) | Plaintext file | None (create-on-first-use only) |
| **Device signing key** | Seed-derived from `DEVICE_KEY_SEED` / `--device-key-seed` (`kernel_config`, `src/break_glass/cli.rs:817`) | Config secret, not hardware-backed | Blocked — `Kernel::open` pins the pubkey in `device_metadata` (roadmap B2) |
| **Trustee signing keys** | Hex file passed to `approve --signing-key`, or a seed typed into the browser signer | Plaintext file / operator's browser | Manual: re-run `policy set` with a new roster |
| **DB encryption key** | Derived from `SECURACV_DB_KEY_SEED`; rotated in place by `rekey_database_file()` (`docs/db_key_rotation.md`) | Config secret | ✅ Already rotatable (decoupled from signing key) |

A grep of `src/` for `tpm`, `pkcs11`, `yubikey`, `fido2`, `piv`, `keystore`
returns nothing: there is no hardware-key abstraction anywhere yet.

---

## 3. The gap, restated as two problems

- **P1 — Setup is a bare-metal ceremony.** The primitives are all there, but an
  operator must know to run `policy set` with hand-built `id:HEX` strings before
  the web console does anything, must distribute trustee keys out of band, and
  has no way to *rehearse* a break-glass before a real 3 a.m. incident. There is
  no status/health command to answer "is my vault actually set up correctly?"
- **P2 — No hardware-backed keys.** Confidentiality of sealed evidence and the
  authority to sign as the device both rest on plaintext files. Invariant V
  asks that "vault confidentiality … rely on **distinct, device-local key
  material**"; a `0600` file technically satisfies "device-local," but the honest
  form of that promise is key material a plugged-in **PKCS#11 token** (host) or
  the **ESP32-S3's own eFuse / flash-encryption / DS peripheral** (device) holds —
  **we have no TPM and no discrete secure element** (§5.5). Trustee hardware
  tokens are likewise the honest form of "**independently controlled** principals
  or devices."

---

## 4. Goals & non-goals

**Goals**
- A first-run path that takes an operator from nothing → a working, *rehearsed*
  quorum vault without hand-editing key files.
- Trustee enrollment that captures a public key into policy through a ceremony,
  not a copy-paste of hex.
- A `KeyStore` seam so private keys *can* live in the hardware we actually have —
  a plugged-in PKCS#11 token / smartcard / HSM on the host, the ESP32-S3's on-die
  security block on the device — with the file backend as the unchanged default.
- Rotation and recovery that are documented and testable, not folklore.
- **Every invariant preserved** — hardware backing strengthens Invariants I & V;
  it must never weaken them or add a raw-export path.

**Non-goals (for v1.1)**
- Re-designing the seal/unseal cryptography (done).
- Remote attestation of device/host state (future; noted in the roadmap).
- A hosted/cloud trustee service — trustees stay independently controlled and
  local by design (Invariant IV/V).
- Replacing the CLI: the CLI stays the source of truth; the web wizard is a
  convenience layer over the same operations.

---

## 5. Proposed design

### 5.1 A feature-gated `KeyStore` trait

Introduce one seam that every private-key operation goes through. The trait
never exposes raw private bytes to callers that don't already have them —
signing and unwrapping happen *inside* the store, which is what lets a hardware
backend hold a non-extractable key.

```rust
/// Where a private key lives and how it is used. Implementations may hold
/// non-extractable key material (a PKCS#11 token / smartcard / HSM, or a TPM
/// where the host has one); callers must not assume the raw secret is ever
/// available. (This trait is host-side; the ESP32-S3 device identity has its own
/// firmware seam — see §5.5.)
pub trait KeyStore {
    /// Sign `msg` with the device identity key, domain-separated by `domain`.
    /// The signature is computed *inside* the store; the private key never
    /// crosses this boundary.
    fn sign_device(&self, domain: &[u8], msg: &[u8]) -> Result<Signature>;

    /// The device verifying key (safe to publish; pinned in device_metadata).
    fn device_public_key(&self) -> Result<VerifyingKey>;

    /// Unwrap the *per-envelope* DEK for a seal, using the persistent vault
    /// master key *inside* the store. Only the ephemeral, single-envelope DEK
    /// crosses this boundary (zeroized after use) — the persistent master secret
    /// never does, which is what preserves the "non-extractable" property for a
    /// hardware backend. (Returning the master key as bytes, as a naive
    /// `unwrap_vault_secret(...) -> [u8;32]` would, defeats that property; a
    /// strict backend MAY instead expose `open_envelope` below and never return
    /// key material at all.)
    ///
    /// NOTE: this is a *symmetric* unwrap (ChaCha20-Poly1305 under the 32-byte
    /// master key), so only a symmetric-capable backend — file, HSM, or TPM — can
    /// perform it in-store. A signing-only PIV/PKCS#11 token backs `sign_device`
    /// but NOT this (see the §5.1 signing-vs-symmetric rule).
    fn unwrap_envelope_dek(&self, wrapped_dek: &WrappedKey) -> Result<Zeroizing<[u8; 32]>>;

    /// Strictest form: perform the AEAD open entirely in-store, returning the
    /// (post-quorum, operator-authorized) plaintext so *no* key material —
    /// master or DEK — is ever returned. Optional; a file backend can default it
    /// to unwrap-then-decrypt.
    fn open_envelope(&self, envelope: &SealedEnvelope) -> Result<Zeroizing<Vec<u8>>>;

    /// Human-facing description for `doctor` ("file (0600)", "PKCS#11 slot 0",
    /// "TPM 2.0 @ 0x81.." where present). Purely informational.
    fn backend_label(&self) -> &str;
}
```

The vault is DEK-wrapped (`seal_v2`: a per-envelope DEK encrypts the payload; the
master key wraps the DEK), so the boundary falls naturally between the two: the
**persistent** master key stays in the store, and at most an **ephemeral**
per-envelope DEK — or nothing, with `open_envelope` — ever reaches process
memory.

Backends, each behind a cargo feature so the default build is unchanged. These
are **host-side** (the witness kernel on a Pi / x86 / container); the *device*
identity on the ESP32-S3 is a separate seam covered in §5.5. Crucially — **we
have no TPM guaranteed and no discrete secure element**, so the realistic host
backing is a token the operator plugs in, not a TPM:

| Backend | Feature | Holds | Notes |
|---------|---------|-------|-------|
| **File** (default) | *(always on)* | Today's `master.key` + seed-derived signing key | Behavior-identical to current code; the refactor is a no-op for existing installs. **The honest default** (§5.5). |
| **PKCS#11 token** (security key / smartcard / HSM) | `keystore-pkcs11` | Host device signing key and/or trustee keys | **The realistic host hardware:** a plugged-in YubiKey (PIV), smartcard, or HSM via the standard PKCS#11 interface. Non-extractable in the token. |
| **TPM 2.0** *(only if the host has one)* | `keystore-tpm` | Host device signing key + vault master key sealed to the TPM | **Optional, not assumed** — most Raspberry Pis have no TPM; some x86 hosts expose an fTPM. Use where present; never the required path. |
| **PIV / hardware Ed25519 token** | `keystore-fido2` | *Trustee* keys on a YubiKey-class token | For Phase 3 — makes each trustee an independently controlled *device*, exactly what Invariant V's "independently controlled principals or devices" wants. **Caveat:** only token modes that emit a real Ed25519 signature over the `trustee-approval:v2` message plug into the current schema; generic FIDO2/WebAuthn assertions do not (see §7 Phase 3). |

Design rules for the trait:
- **The default path does not change.** File backend = current bytes, current
  file layout, current `0600`. No migration required for existing vaults.
- **Trait objects, feature-selected at construction.** `KeyStore` is chosen once
  at startup from config (`vault.keystore = "file" | "pkcs11" | "tpm"`); the rest
  of the kernel is backend-agnostic.
- **Trustee keys and the device key are separate concerns.** A deployment can put
  the host device key on a PKCS#11 token (or a TPM if it has one) while trustees
  use their own tokens, or mix file + hardware during migration.
- **Signing keys ≠ the symmetric master key — a PIV token can't hold the master.**
  Device-identity and trustee keys are *asymmetric* (Ed25519 signing), which is
  exactly what a PIV / PKCS#11 token does — the token signs, the key never leaves.
  But the vault **master key is a 32-byte *symmetric* secret** that unwraps the DEK
  via ChaCha20-Poly1305 (`wrap_dek` / `unwrap_dek`, `src/vault/crypto.rs`), and a
  YubiKey-PIV PKCS#11 module performs *only* asymmetric private-key operations —
  it cannot hold or unwrap a symmetric key in-token. So the master key has **three
  honest options**, and only the third puts it in a plug-in token:
  1. **File-backed (default)** — the master stays a `0600` file; tokens still
     hardware-back the *signing* keys. Recommended baseline.
  2. **HSM / TPM with symmetric unwrap** — an HSM (or a TPM, where present) *can*
     hold the symmetric key and unwrap the DEK in-hardware. `keystore-pkcs11`
     against an HSM, or `keystore-tpm`.
  3. **Re-wrap the DEK to an *asymmetric* token key** (RSA-OAEP / ECDH to a PIV
     slot) so a PIV token can unwrap it — but this is a **VLT2 wrap-format change
     and therefore new crypto**, explicitly *outside* the "no new crypto" default
     path and tracked as an opt-in (open decision #1).

### 5.2 Setup & enrollment UX (CLI-first)

New `break_glass` subcommands, all built on the crypto that already exists:

- **`break_glass init`** — the guided first-run ceremony. Prompts for (or takes
  flags): threshold `n`, expected trustee count `m`, which `KeyStore` backend to
  use, and the vault root. Creates/loads the device identity in the chosen
  backend and prints the device public key plus a checklist of what's left
  (enroll `m` trustees, run a drill). Crucially it does **not** write a
  `QuorumPolicy` yet: `QuorumPolicy::validate` rejects an empty roster and any
  `n` greater than the trustee count *by design* (both are Invariant-V guards in
  `src/break_glass/core.rs`), and that validator must not be weakened to store a
  half-set-up "shell." Instead `init` records **draft setup state kept separate
  from the committed policy**; the real `QuorumPolicy` is committed only once
  enrollment can satisfy `n`-of-`m` (see `enroll`). Idempotent and safe to
  re-run — it reports state rather than clobbering.
- **`break_glass trustee enroll`** — enroll one trustee without hand-editing hex.
  Two modes: **generate** (mints a keypair, writes the private part to the
  trustee's chosen store — file or hardware token — and captures the public key),
  or **import** (takes a public key the trustee generated on their own machine /
  token). It appends the `TrusteeEntry` to the **draft roster**; once the roster
  reaches a valid `n`-of-`m` it commits (or updates) the real `QuorumPolicy` via
  the existing `set_break_glass_policy` path — so `validate` always runs against a
  complete roster, never a partial one. Complements, doesn't replace,
  `policy set`.
- **`break_glass drill`** — rehearse a full request→approve→authorize→unseal
  against a **throwaway** envelope with dummy contents, so operators prove the
  quorum works *before* they need it. Writes a receipt tagged as a drill, then
  cleans up. This is the single most valuable "it actually works" artifact for a
  nervous first-time operator.
- **`break_glass doctor`** (a.k.a. `status`) — one command that answers "is this
  vault set up correctly?": policy present? `n`-of-`m` with `m` trustees actually
  enrolled? each trustee key well-formed / reachable? which `KeyStore` backend,
  and **is any private key still plaintext-on-disk** (loud warning if so)? master
  key location + backing? last successful drill? Exit non-zero on any red so it
  can gate a deploy.

These four — `init`, `enroll`, `drill`, `doctor` — are Phase 1 and need **no new
crypto**: they orchestrate what's already in `cli.rs` / `core.rs`, plus one small
piece of new *state* — a **draft-setup store** distinct from the committed
`QuorumPolicy` — so a half-finished enrollment is never mistaken for a live
quorum gate (and `QuorumPolicy::validate` never has to be relaxed).

### 5.3 Guided web setup wizard

Today `breakglass.html` opens at **1 · Connect** and dead-ends on
*"No quorum policy is configured"* (the `409` branch in the page's connect
handler). Add a **0 · Set up** phase in front of Connect that mirrors the WAP
`/companion` wizard pattern we already ship:

- Detects the no-policy state and offers to create one in-browser: choose
  `n`/`m`, generate trustee signing links (reusing the existing
  `…/breakglass#sign&hash=…` signer that already keeps key material client-side),
  and confirm each enrollment.
- Surfaces `doctor` output as a health panel (green/red per check).
- A **"Run a drill"** button that drives `break_glass drill` through the server
  and shows the receipt — the web equivalent of the CLI rehearsal.
- Hardware flows (Phase 3+) appear here as "this trustee uses a security key" —
  but only for tokens that can produce the **existing Ed25519 trustee approval**
  (see the Phase 3 caveat in §7). Generic WebAuthn is *not* a drop-in here: a
  WebAuthn assertion signs `authenticatorData ‖ SHA-256(clientDataJSON)` and
  carries a credential id, which is not the 64-byte Ed25519 signature over the
  `trustee-approval:v2` message the kernel verifies. Supporting plain WebAuthn
  would require the credential-bearing trustee schema flagged in §7 first.

The wizard is a convenience over the CLI, not a second source of truth; it calls
the same server endpoints and writes the same policy.

### 5.4 Recovery & rotation

Extend `docs/db_key_rotation.md` (which already covers the DB key + the device
identity-pinning problem) with the vault-specific rotations:

- **Trustee rotation** — add / remove / replace a trustee: re-sign the policy,
  record the change in a receipt so the roster's history is auditable. Removing a
  compromised trustee must be a first-class, documented operation.
- **Device identity rotation** — the roadmap's open B2 item: `Kernel::open` pins
  the device public key in `device_metadata` and rejects a mismatch, so rotation
  needs identity-rotation support (record the new key, keep the log verifiable
  across the boundary). Hardware backing makes this *more* tractable, not less —
  a PKCS#11 token can hold the new key before the old one is retired.
- **Master-key rotation / re-wrap** — with the `KeyStore` seam, rotating the
  vault master secret becomes "unwrap under the old backend, re-wrap under the
  new," which also *is* the file→hardware (PKCS#11 token) migration path.

### 5.5 Hardware reality: no TPM, no secure element — what each platform gives us

The "hardware backend" rows above name *capabilities*, not parts we own. This
deserves stating plainly, because the keys live in **two different places** and
**neither has a TPM**:

**Host kernel (Pi / x86 / container)** — where `master.key`, the device *signing
seed*, and trustee keys live. No TPM is guaranteed: most Raspberry Pis have none;
some x86 hosts expose an fTPM. So the realistic backing is a **PKCS#11 token the
operator plugs in** — a YubiKey (PIV), a smartcard, or an HSM — with the **file
backend as the honest default**. `keystore-tpm` stays optional, use-it-if-present.
Note the split from §5.1: a plug-in **PIV/smartcard token hardware-backs the
*asymmetric signing* keys** (device identity, trustee approvals), but the
**symmetric vault master key** cannot live in such a token — it stays file-backed
unless you have an HSM/TPM with symmetric unwrap, or opt into an asymmetric-rewrap
of the DEK format (new crypto; open decision #1).

**The Canary device (Seeed XIAO ESP32-S3)** — where the device's *own* Ed25519
identity lives (`docs/device_trust.md`: generated on first boot from
`esp_fill_random`, stored in NVS, signs every MQTT publish). This is a **separate
key** from the host's, and the XIAO carries **no discrete secure element** (no
ATECC608 / SE050 in the BOM; no `esp_ds` / `esp_hmac` in the firmware today). Its
"TPM-equivalent" is the **ESP32-S3's own on-die security block**, which answers
"secure it" and "sign it" differently:

- **Secure it (protect the key at rest) — already tooled, not yet burned.**
  `docs/secure_provisioning.md` Phase 2 wraps Espressif's guide: **eFuse +
  XTS-AES-256 Flash Encryption + NVS encryption** make the NVS Ed25519 key
  unreadable off-chip; **Secure Boot v2 (RSA-3072)** stops an attacker flashing a
  key-dumping build; **JTAG disabled via eFuse** closes the debug port. It's
  irreversible and needs a physical device to validate — which is why it's
  post-v1. The provisioning kit (`generate_keys.sh`, `provision_canary.sh`,
  `verify_device.py`) already exists.
- **Sign it without the key ever entering software — available, unused.** The
  S3's **HMAC** and **Digital Signature (DS)** peripherals can sign with a private
  key decrypted *only inside the peripheral* (via a read-protected eFuse key), so
  the plaintext never reaches CPU RAM. **Caveat: the DS peripheral is RSA-only** —
  the ESP32-S3 has **no** hardware Ed25519 or ECDSA signing peripheral (those are
  on the C6 / H2 / P4). Our identity is Ed25519, so the S3 can protect an Ed25519
  key *at rest* but cannot do *non-extractable Ed25519 signing*.

**The Ed25519 fork for the device** (open decision #7):
- **(a) Ed25519, protected at rest** — flash-encryption + eFuse + secure boot.
  Keeps the entire existing signing scheme; the key can't be dumped off a locked
  device. **Recommended baseline** — largely already tooled in
  `secure_provisioning.md`.
- **(b) Add an RSA identity via the DS peripheral** — true non-extractable
  signing, but RSA ≠ our Ed25519 format, so it's a *second, parallel* attestation
  identity, not a drop-in for event signatures.
- **(c) External secure element** (ATECC608 = NIST P-256 — still not Ed25519) — a
  format change *plus* a board respin; skip unless a customer requires it.

The firmware seam for this already exists as a sketch — the `securacv_crypto_hal_t`
HAL in `docs/openipc_architecture_learnings.md` ("swap software Ed25519 for
hardware later, change one HAL impl") — and `docs/review/01-flag-report.md` (P3)
already lists "add hardware-backed keys (Secure Element/eFuse)." So the device
side is a **firmware provisioning + HAL** task, not new kernel crypto.

---

## 6. Invariant preservation

This work strengthens the two load-bearing invariants; it must never weaken
them. Explicit checks:

- **Invariant I — No Raw Export.** Hardware-backing changes only *where keys
  live*, never adding a path that streams or mirrors raw media. Unseal still
  writes cleartext only to an operator-chosen directory (mode `0600`) after a
  valid quorum token, exactly as `cmd_unseal` does today. The pre-roll buffer
  stays transient/zeroized. **No new export API.**
- **Invariant V — Break-Glass by Quorum.** The `n`-of-`m` gate, the
  distinct-trustee count, the single-use nonce burn, and the immutable receipt
  chain are unchanged. Hardware tokens make trustees *more* independently
  controlled (a physical YubiKey per trustee), directly serving "quorum approvals
  MUST originate from independently controlled principals or devices." Moving the
  master key onto a plugged-in PKCS#11 token — or the device key into the
  ESP32-S3's eFuse-locked block — is the literal form of "distinct,
  **device-local** key material."
- **Safety properties that must survive every phase:** two-check unseal (token
  validity *and* receipt-outcome check in `vault.unseal`), single-use token burn
  before cleartext, unique-key-per-role (no key does double duty), and — with a
  hardware backend selected — **no plaintext private key on disk** (the property
  `doctor` verifies). A drill must pass under each backend before that backend is
  declared supported.

An honesty note consistent with the rest of the project: until a backend has
been exercised on real hardware, `doctor` and the docs must say "file-backed"
plainly rather than implying hardware protection that isn't wired.

---

## 7. Phased task breakdown

Ordered by value-per-risk. Phase 1 delivers most of the operator-visible win
with zero crypto risk; the hardware phases are gated on physical validation.

### Phase 1 — Setup UX on the existing crypto  *(no new crypto; highest value)*
- `break_glass init` guided ceremony (idempotent, prints device pubkey + checklist).
- `break_glass trustee enroll` (generate | import) → policy, re-signed.
- `break_glass drill` (throwaway envelope, drill-tagged receipt, cleanup).
- `break_glass doctor` / `status` (health checks, exit-code gated, plaintext-key warning).
- Tests: a full `init → enroll×m → drill` round-trip in `tests/`; `doctor` red/green cases.
- **Est.:** ~1–1.5 weeks. **Depends on:** nothing new.

### Phase 2 — `KeyStore` trait + host PKCS#11 hardware
- Introduce the `KeyStore` trait; refactor the file path behind it **behavior-
  identically** (regression-tested against current byte layout).
- `keystore-pkcs11` backend for the **signing keys** (host device identity +
  trustee keys — asymmetric, which is all a PIV/smartcard token does). The
  **symmetric vault master key stays file-backed by default**; putting it in
  hardware needs an HSM/TPM with symmetric unwrap, *not* a PIV token (§5.1 rule +
  open decision #1). `keystore-tpm` is optional, built only for hosts that have a
  TPM — **not the required path** (§5.5).
- Wire `doctor` to report the live backend; add `vault.keystore` config.
- **Est.:** ~2–3 weeks. **Depends on:** Phase 1; **a PKCS#11 token to validate on.**
- **Device side (parallel, firmware track):** the ESP32-S3 identity is hardened by
  *burning* `secure_provisioning.md` Phase 2 (eFuse + flash encryption + Secure
  Boot v2), plus optionally routing signing through a crypto-HAL — see §5.5 and
  open decision #7. This is firmware provisioning, not kernel crypto, and shares
  no code with the host `KeyStore`.

### Phase 3 — Trustee hardware tokens
- **Trustee-credential decision (must settle first).** Today a trustee is a bare
  32-byte Ed25519 public key and an approval is a 64-byte Ed25519 signature over
  the `trustee-approval:v2` message (`TrusteeEntry` / `Approval` in
  `src/break_glass/core.rs`). Hardware tokens split two ways against that:
  - **(a) Ed25519-capable tokens** (PIV / PKCS#11 slots configured for Ed25519,
    or tokens exposing raw Ed25519 signing) can produce that exact approval —
    they slot into the current schema with **no format change**. Lowest-risk path.
  - **(b) Generic FIDO2 / WebAuthn** cannot: the assertion signs
    `authenticatorData ‖ SHA-256(clientDataJSON)` and carries a credential id, so
    it needs an **algorithm/credential-bearing trustee schema** (extend
    `TrusteeEntry` to carry an alg tag + credential id/pubkey, and teach approval
    verification the WebAuthn shape) — a real, versioned change to the quorum
    format and its verifier, not just a new backend.
  - **Recommendation:** ship (a) in v1.1 and treat (b) as a follow-up only if the
    demand for consumer FIDO2 keys justifies the format change.
- `keystore-fido2` / `keystore-pkcs11` for trustee keys; `enroll` learns the
  hardware modes (scoped to whichever of (a)/(b) is chosen).
- Approval signing via the token (CLI; and in the console for the chosen path).
- **Est.:** ~2–3 weeks for (a); **+1–2 weeks** if (b)'s schema change is in scope.
  **Depends on:** Phase 2; **hardware tokens to validate.**

### Phase 4 — Web wizard, rotation docs, drills-in-CI
- The **0 · Set up** wizard phase in `breakglass.html` + health panel + drill button.
- Rotation/recovery sections in `docs/db_key_rotation.md` (trustee, identity, master-key).
- A headless drill in CI (the existing probe pattern) so the setup flow can't rot.
- **Est.:** ~1.5–2 weeks. **Depends on:** Phases 1–3 for the hardware bits (the
  file-backed wizard + drill-in-CI can land right after Phase 1).

---

## 8. Open decisions for you

These change what gets built; I'd like your calls before writing code.

1. **Which host hardware backing first — and how to custody the *symmetric*
   master key?** We have **no guaranteed TPM and no secure element** (§5.5).
   - *Signing keys (asymmetric):* my lean is **PKCS#11 token for the host device
     key, trustee tokens second**; TPM as an opportunistic extra where present.
   - *Vault master key (symmetric):* a PIV token **can't** hold it, so choose
     **(i)** keep it file-backed (default; tokens still back the signing keys) —
     *my lean*; **(ii)** require an HSM/TPM with symmetric unwrap; or **(iii)**
     re-wrap the DEK to an asymmetric token key (RSA-OAEP/ECDH) so a PIV token can
     unwrap it — a deliberate **VLT2 wrap-format change = new crypto**, outside the
     "no new crypto" default. (§5.1 rule.)
2. **CLI-first or web-first?** I propose CLI-first (Phase 1 CLI, wizard in Phase
   4) because the CLI is the source of truth and needs no browser to validate. If
   the primary operator persona is non-technical, we could pull the web wizard
   earlier (file-backed) right after Phase 1.
3. **v1.1 scope cut.** Is v1.1 = *Phase 1 only* (setup UX, still file-backed,
   shippable in ~1.5 weeks and a huge usability jump), or v1.1 = *Phase 1 + 2*
   (adds host PKCS#11 backing + optional device provisioning, needs hardware)? The
   roadmap bundles "setup UX + hardware-backed keys" together, but Phase 1 stands
   alone and I'd argue for shipping it first.
4. **Security-vs-recovery tradeoff.** Non-extractable hardware keys mean a lost
   token/card (or a burned-out ESP32-S3) = unrecoverable material by design. How
   much recovery affordance do you want — e.g. an optional sealed off-box backup
   of the master key (weakens "non-extractable" but saves you from a dead board),
   or hard "hardware or nothing" (stronger, less forgiving)? This is a values
   call, not a technical one.
5. **Do drills belong in CI?** Running a headless drill on every change keeps the
   setup flow honest (our usual drift-gate ethos) but adds a job. Worth it?
6. **Trustee credential format (Phase 3).** Keep the current Ed25519-only trustee
   schema and support only tokens that emit that exact approval (PIV/PKCS#11
   Ed25519 — no format change), or extend `TrusteeEntry`/approval verification to
   a credential-bearing schema so generic FIDO2/WebAuthn keys work too (a
   versioned change to the quorum format)? My lean: **Ed25519-only for v1.1**,
   revisit WebAuthn later. (See §7 Phase 3.)
7. **ESP32-S3 device identity — how far to harden (§5.5).** The XIAO has no secure
   element and no hardware Ed25519 signing, so pick: **(a)** Ed25519 protected at
   rest (eFuse + flash encryption + Secure Boot v2 — keeps the existing signing
   scheme; mostly already tooled) — *my lean*; **(b)** additionally an RSA
   attestation identity via the DS peripheral (true non-extractable signing, but a
   second key/format); or **(c)** an external secure element (board respin, still
   not Ed25519). Also: do we commit to *burning* Phase 2 provisioning for v1.1, or
   keep it documented-but-optional?

---

## 9. Appendix — key inventory (target state)

Where each key ends up once all phases land (file backend stays the default and
the honest fallback):

| Key | v1 (today) | v1.1 target (hardware backend selected) |
|-----|-----------|------------------------------------------|
| Vault master key (host, **symmetric**) | `master.key`, `0600` | **File-backed by default** (a PIV token can't hold a symmetric key); HSM/TPM with symmetric unwrap where available, or opt-in asymmetric-rewrap of the DEK (new crypto) — decision #1 |
| Host device signing key (**asymmetric**) | Seed-derived from config | PKCS#11-token-held, non-extractable; pinned pubkey |
| Trustee signing keys | Hex file / browser seed | PIV/PKCS#11 token per trustee (independently held) |
| **ESP32-S3 device identity** (separate; §5.5) | Ed25519 in NVS (software) | Ed25519 protected at rest via eFuse + flash encryption + Secure Boot v2 (option (a)) |
| DB encryption key | `SECURACV_DB_KEY_SEED`, rotatable | Unchanged (already rotatable) |
| Break-glass token nonce | Single-use, burned pre-cleartext | Unchanged |

---

*Honest by construction: the "current state" claims above cite the code that
backs them (`src/vault/`, `src/break_glass/`, `src/crypto/signatures.rs`,
`spec/invariants.md`). The proposed sections are clearly marked as design, not
shipped. Nothing here is implemented yet.*
