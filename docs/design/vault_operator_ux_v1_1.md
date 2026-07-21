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
feature-gated **`KeyStore`** abstraction with a **TPM** backend for the device
keys (Phase 2), **hardware tokens for trustees** (Phase 3), and a **guided web
wizard + docs + drills** (Phase 4). Phase 1 is the lowest-hanging, highest-value
fruit; the hardware phases need physical devices to validate and are the reason
this is v1.1, not v1.

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
  material**"; a `0600` file technically satisfies "device-local" but a TPM or
  secure element is the honest form of that promise, and trustee hardware tokens
  are the honest form of "**independently controlled** principals or devices."

---

## 4. Goals & non-goals

**Goals**
- A first-run path that takes an operator from nothing → a working, *rehearsed*
  quorum vault without hand-editing key files.
- Trustee enrollment that captures a public key into policy through a ceremony,
  not a copy-paste of hex.
- A `KeyStore` seam so private keys *can* live in a TPM / secure element / HSM /
  hardware token, with the file backend as the unchanged default.
- Rotation and recovery that are documented and testable, not folklore.
- **Every invariant preserved** — hardware backing strengthens Invariants I & V;
  it must never weaken them or add a raw-export path.

**Non-goals (for v1.1)**
- Re-designing the seal/unseal cryptography (done).
- Remote attestation of the TPM state (future; noted in the roadmap).
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
/// non-extractable key material (TPM, PIV, HSM); callers must not assume the
/// raw secret is ever available.
pub trait KeyStore {
    /// Sign `msg` with the device identity key, domain-separated by `domain`.
    fn sign_device(&self, domain: &[u8], msg: &[u8]) -> Result<Signature>;

    /// The device verifying key (safe to publish; pinned in device_metadata).
    fn device_public_key(&self) -> Result<VerifyingKey>;

    /// Unwrap a vault DEK / master secret for a seal envelope. For hardware
    /// backends this runs inside the device; the master key never leaves it.
    fn unwrap_vault_secret(&self, wrapped: &WrappedKey) -> Result<Zeroizing<[u8; 32]>>;

    /// Human-facing description for `doctor` ("file (0600)", "TPM 2.0 @ 0x81..",
    /// "PKCS#11 slot 0"). Purely informational.
    fn backend_label(&self) -> &str;
}
```

Backends, each behind a cargo feature so the default build is unchanged:

| Backend | Feature | Holds | Notes |
|---------|---------|-------|-------|
| **File** (default) | *(always on)* | Today's `master.key` + seed-derived signing key | Behavior-identical to current code; the refactor is a no-op for existing installs. |
| **TPM 2.0** | `keystore-tpm` | Device signing key + vault master key sealed to the TPM | Primary hardware target for the appliance/Pi/x86 host. Non-extractable, PCR-bindable. |
| **PKCS#11** | `keystore-pkcs11` | Device or trustee keys on an HSM / smartcard | Covers enterprise HSMs and PIV smartcards via a standard interface. |
| **FIDO2 / PIV token** | `keystore-fido2` | *Trustee* keys on a YubiKey-class token | For Phase 3 — makes each trustee an independently controlled *device*, which is exactly what Invariant V's "independently controlled principals or devices" wants. |

Design rules for the trait:
- **The default path does not change.** File backend = current bytes, current
  file layout, current `0600`. No migration required for existing vaults.
- **Trait objects, feature-selected at construction.** `KeyStore` is chosen once
  at startup from config (`vault.keystore = "file" | "tpm" | "pkcs11"`); the rest
  of the kernel is backend-agnostic.
- **Trustee keys and the device key are separate concerns.** A deployment can
  put the device key in a TPM while trustees use FIDO2 tokens, or mix file +
  hardware during migration.

### 5.2 Setup & enrollment UX (CLI-first)

New `break_glass` subcommands, all built on the crypto that already exists:

- **`break_glass init`** — the guided first-run ceremony. Prompts for (or takes
  flags): threshold `n`, expected trustee count `m`, which `KeyStore` backend to
  use, and the vault root. Creates/loads the device identity in the chosen
  backend, writes the (empty-roster) policy shell, and prints the device public
  key and a checklist of what's left (enroll `m` trustees, run a drill). Idempotent
  and safe to re-run — it reports state rather than clobbering.
- **`break_glass trustee enroll`** — enroll one trustee into the policy without
  hand-editing hex. Two modes: **generate** (mints a keypair, writes the private
  part to the trustee's chosen store — file or hardware token — and captures the
  public key), or **import** (takes a public key the trustee generated on their
  own machine/token). Either way it appends a `TrusteeEntry` and re-signs the
  policy. Complements, doesn't replace, `policy set`.
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
crypto**; they orchestrate what's already in `cli.rs` / `core.rs`.

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
- Hardware flows (Phase 3+) appear here as "this trustee uses a security key"
  using WebAuthn where the browser supports it.

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
  a TPM can hold the new key before the old one is retired.
- **Master-key rotation / re-wrap** — with the `KeyStore` seam, rotating the
  vault master secret becomes "unwrap under the old backend, re-wrap under the
  new," which also *is* the file→TPM migration path.

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
  MUST originate from independently controlled principals or devices." Putting the
  master key in a TPM is the literal form of "distinct, **device-local** key
  material."
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

### Phase 2 — `KeyStore` trait + TPM device keys
- Introduce the `KeyStore` trait; refactor the file path behind it **behavior-
  identically** (regression-tested against current byte layout).
- `keystore-tpm` backend: device signing key + vault master key sealed to the TPM.
- Wire `doctor` to report the live backend; add `vault.keystore` config.
- **Est.:** ~2–3 weeks. **Depends on:** Phase 1; **a TPM to validate on.**

### Phase 3 — Trustee hardware tokens
- `keystore-fido2` / `keystore-pkcs11` for trustee keys; `enroll` learns the
  hardware modes.
- Approval signing via the token (CLI and, where supported, WebAuthn in the console).
- **Est.:** ~2–3 weeks. **Depends on:** Phase 2; **YubiKey-class tokens to validate.**

### Phase 4 — Web wizard, rotation docs, drills-in-CI
- The **0 · Set up** wizard phase in `breakglass.html` + health panel + drill button.
- Rotation/recovery sections in `docs/db_key_rotation.md` (trustee, identity, master-key).
- A headless drill in CI (the existing probe pattern) so the setup flow can't rot.
- **Est.:** ~1.5–2 weeks. **Depends on:** Phases 1–3 for the hardware bits (the
  file-backed wizard + drill-in-CI can land right after Phase 1).

---

## 8. Open decisions for you

These change what gets built; I'd like your calls before writing code.

1. **Which hardware backend first?** TPM 2.0 (best fit for a Pi/x86 appliance,
   no extra hardware for the operator) vs. PKCS#11/PIV (enterprise HSMs &
   smartcards) vs. FIDO2 tokens (cheapest per-trustee, best "independently
   controlled device" story). My lean: **TPM for the device key first, FIDO2 for
   trustees second** — but this is yours to weight.
2. **CLI-first or web-first?** I propose CLI-first (Phase 1 CLI, wizard in Phase
   4) because the CLI is the source of truth and needs no browser to validate. If
   the primary operator persona is non-technical, we could pull the web wizard
   earlier (file-backed) right after Phase 1.
3. **v1.1 scope cut.** Is v1.1 = *Phase 1 only* (setup UX, still file-backed,
   shippable in ~1.5 weeks and a huge usability jump), or v1.1 = *Phase 1 + 2*
   (adds TPM, needs hardware)? The roadmap bundles "setup UX + hardware-backed
   keys" together, but Phase 1 stands alone and I'd argue for shipping it first.
4. **Security-vs-recovery tradeoff.** Non-extractable hardware keys mean a lost
   TPM/token = unrecoverable material by design. How much recovery affordance do
   you want — e.g. an optional sealed off-box backup of the master key (weakens
   "non-extractable" but saves you from a dead board), or hard "hardware or
   nothing" (stronger, less forgiving)? This is a values call, not a technical
   one.
5. **Do drills belong in CI?** Running a headless drill on every change keeps the
   setup flow honest (our usual drift-gate ethos) but adds a job. Worth it?

---

## 9. Appendix — key inventory (target state)

Where each key ends up once all phases land (file backend stays the default and
the honest fallback):

| Key | v1 (today) | v1.1 target (hardware backend selected) |
|-----|-----------|------------------------------------------|
| Vault master key | `master.key`, `0600` | Sealed to TPM; unwrap inside device |
| Device signing key | Seed-derived from config | TPM-held, non-extractable; pinned pubkey |
| Trustee signing keys | Hex file / browser seed | FIDO2/PIV token per trustee (independently held) |
| DB encryption key | `SECURACV_DB_KEY_SEED`, rotatable | Unchanged (already rotatable) |
| Break-glass token nonce | Single-use, burned pre-cleartext | Unchanged |

---

*Honest by construction: the "current state" claims above cite the code that
backs them (`src/vault/`, `src/break_glass/`, `src/crypto/signatures.rs`,
`spec/invariants.md`). The proposed sections are clearly marked as design, not
shipped. Nothing here is implemented yet.*
