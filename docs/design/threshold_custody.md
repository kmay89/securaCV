# Threshold custody of the vault KEK (design record)

Status: core primitives, share envelope and KATs in code (PR S); no command puts a vault under custody; the vault key is still a single device-local secret.

This is the design record for roadmap item 5, the threshold wrap of the
vault key that `spec/quorum_unseal_v2.md` §2 specifies. It records what PR S
built, the decisions every later change depends on, and the formats S froze
as v1. The protocol sections describe changes that are designed and not
built; each is marked with the change that lands it.

What S does **not** change: no policy field, no receipt field, no vault
store, no `witnessd`, and no byte of `master.key`, `master.keyguard`,
`kem-mlkem768.key` or any envelope. Nothing in the kernel calls the new
library outside its own tests and the holder-side commands, so S cannot put a
vault under custody and cannot make one unopenable.
`docs/security/ENTERPRISE_CUSTODY.md` §1 "State today" stands as written:
the quorum is an authorization gate, not the lock.

S amends `spec/quorum_unseal_v2.md` §2.1 (Pedersen over
ristretto255 and the KEK that wraps the unchanged master key; the key
hierarchy diagram, which now runs KEK → `MKC1` → master key and ML-KEM key →
per-envelope DEK; the digest of the master secret; the recipient key), §2.2
(the ML-KEM-768 key wrapped under the KEK, not seed-split) and §6 item 5.

## Status

| Change | Content | State |
|---|---|---|
| **S** | Pedersen VSS over ristretto255; the HPKE boxes; the `SHR1`, `TEK1`, `MKC1`, recipient and split-record formats; stdlib-Python KATs; three fuzz targets; `custody recipient keygen`, `custody rehearse`, `share inspect`, `share verify`, `share protect` | **built** (this record) |
| T | vault readers: the two HPKE `kem_alg` decrypt branches, the custody seal function (public keys only), the `MKC1` reader and sentinel detection in the store; no writer | designed |
| U | the single reader break: `custody.kek` in the policy with every shape frozen, issuance (deal-and-prove), holder acknowledgments, activation in dual custody, every verifier reader | designed |
| V | release at the gate: redemption-first collection, the holder's `share release`, resume, the drill | designed |
| W | revoke a trustee, reshare by redistribution, recovery reconstitution, rekey | designed |
| X | retirement, the only destructive step: the sentinel, then the legacy key files are removed | designed |
| Y | liveness proofs, device-signed release acknowledgments, court-kit lines, console | designed |

Merge order with items 3 and 4: O → S → T → M0 → M → U → V → W → X → (N, R,
G after F) → Y. S and T touch no shared format.

## 1. The decisions

| Question | Decision | Why |
|---|---|---|
| Reconstruct the KEK in memory, or threshold decryption? | **Reconstruct**, as spec §2.1 says, at the unseal gate only | Every existing envelope is encrypted under the master key or the ML-KEM secret; a threshold-ElGamal target opens none of them. Threshold decryption is kept as the named future seam for new envelopes (rejection 9). The residual is R-KEEP (§11). |
| When do releases happen? | **After the device-signed redemption row exists**, enforced on each holder's own device | The record precedes the capability. A collection that never reaches n spends the grant; a fresh request is needed. |
| Where does the vault X25519 key come from? | **Derived from the master key** | One secret, the master key, opens every envelope, old and new. During dual custody `witnessd` keeps sealing the legacy `kem_alg`s; the custody seal is first exercised by the drill (V). |
| Which sentinel marks a custody vault? | **A self-explaining text file**, `securacv-vault-custody-sentinel:v1` plus one English line | Older builds refuse it with their shipped messages; `MKG1` kind 2 stays reserved for a hardware KEK. |
| How is the recovery split related to the operational one? | **The same `(k, r)`**, dealt twice | Both splits have the identical `C_0`, so "same KEK" is a byte comparison. |
| How does the policy commitment grow? | `full_bytes ‖= 0x05 ‖ SHA-256(kek_bytes)` **only when `kek` is present** | A policy without `kek` keeps byte-identical commitments, so every earlier history row still re-derives. |
| One epoch or several? | **One, at U** | U ships every reader; V–Y add writers only. |
| The ML-KEM-768 key | **Wrapped under the KEK inside `MKC1`** | The binding exposes no seed-based keygen and existing keys have no recorded seed (spec §2.2 amended). |
| The hybrid combiner | **HPKE PSK mode** keyed by the ML-KEM shared secret | One implementation, pinned by the draft's PSK vector; `psk_id` binds the ML-KEM ciphertext. The draft states the hybrid property is not proven. |
| Which roster does a holder count approvals against? | **The one pinned in their share** | A hub-supplied roster would let the hub choose who counts. |
| Drills without a grant? | **Never** | Every reconstruction goes through an ordinary grant; there is no drill session type. |
| Store mode | **Detected from the sentinel**; `VaultConfig` unchanged | No configuration field can disagree with the files. |
| The ephemeral release key | **Process memory only** | A key derived from the device seed would let a seed thief open leftover release files. |
| A direct `curve25519-dalek` line | **Allowed**: same version, a subset of the features already compiled (§7) | Adds no package and no feature. |

## 2. The mathematics

**Group.** ristretto255 (prime order `ℓ = 2^252 + 27742317777372353535851937790883648493`)
from `curve25519-dalek` 5.0.0. Points are the canonical 32-byte encoding;
decoding refuses non-canonical encodings, and the identity is refused
wherever a point is a commitment. Prime order makes commitment equality a
byte comparison. `G` is the ristretto basepoint.

**H** = `RistrettoPoint::from_uniform_bytes(SHA-512("securacv:custody:pedersen-generator-H:v1"))`,
whose encoding is `b293a0f3f02ff22ef8bd4148b908d7e2629a2d16dfb690fe43e295a35574e562`.
The Rust code and the standard-library Python generator compute it
independently and agree. *Erratum to the design brief:* the brief's
prototype printed `2e99ac14…`. That prototype computed `SQRT_AD_MINUS_ONE`
as the even ("non-negative") square root, where RFC 9496 lists the odd one;
the constant is used only by the one-way map, so the prototype passed the
encoding table and failed all 11 one-way-map vectors of
draft-irtf-cfrg-ristretto255-decaf448-04 Appendix A.3. The shipped generator
pins the odd constant and checks it against those vectors before it writes
anything.

**Scalars** are 32-byte little-endian and canonical; a value at or above ℓ
is a format error. Random scalars are 64 bytes from the OS RNG reduced mod ℓ
(bias at most ℓ/2^512), redrawn on zero.

**Constant time.** Secret-dependent steps use dalek's `Scalar` arithmetic and
its constant-time `multiscalar_mul`; secret comparisons go through
`subtle::ConstantTimeEq`. Variable-time code touches public data only (the
public share point `P_x` and the Horner sum over commitments). This rests on
the crate's documentation, not on measurement.

**Pedersen VSS.** For the KEK pair `(k, r)`: `f(x) = k + a_1 x + … + a_{n−1} x^{n−1}`,
`g(x) = r + b_1 x + …`, commitments `C_j = a_j·G + b_j·H`. `C_0 = k·G + r·H`
is the same for every split of one KEK. The holder at index `x ∈ 1..=m`
(never 0) receives `(s, t) = (f(x), g(x))`, and `2 ≤ n ≤ m ≤ 32`.

- **Check:** `s·G + t·H == Σ_j C_j x^j`, the left side constant time.
- **Reconstruct** from exactly n distinct indices: indices refused before any
  secret is read, every share checked, Lagrange at zero, and the rebuilt
  pair must commit to the split's *pinned* `C_0` before k is used. (The Rust
  `reconstruct` takes that pinned commitment as an argument: against forged
  commitments the forged `C_0` would agree with the forged key.)
- **Deal-and-prove:** every dealt share passes its check, and every n-subset
  (or 64 of them when `C(m, n) > 64`) rebuilds `(k, r)`, compared in constant
  time, before anything is written.

**Pedersen, not Feldman.** Feldman publishes `k·G`. That point would sit in
the policy, every history row and every court kit, and a quantum adversary
running Shor on it recovers k, then the container, the master key and every
envelope. Pedersen commitments are perfectly hiding; the only public function
of k is the symmetric `MKC1` AEAD. Binding is computational: a forged share
that passes its check is caught at reconstruction (the `C_0` check, then the
AEAD) and discloses nothing.

**The digest of the master secret** (the SLIP-0039 lesson) is `C_0`, carried
in every share, the policy (from U) and the `MKC1` header. Given n−1 shares
and every commitment it reveals nothing (Pedersen hiding), where SLIP-0039's
4-byte digest costs 32 bits. A truncated digest is rejected.

## 3. The HPKE profile

RFC 9180 HPKE, single shot, sequence 0: KEM `0x0020` DHKEM(X25519,
HKDF-SHA256), KDF `0x0001` HKDF-SHA256, AEAD `0x0003` ChaCha20-Poly1305.
X25519 is dalek's `mul_clamped`; both sides refuse the all-zero DH output.
Pinned by the HPKE editor's Base and PSK vectors for this suite.

| Box | Name | Construction |
|---|---|---|
| `0x01` | `hpke-x25519` | `SealBase(pkR, info, aad, pt)`; `enc` 32 bytes |
| `0x02` | `hpke-x25519-psk-mlkem768` | `(ct_M, ss_M) = ML-KEM-768.Encaps(pkR_mlkem)`; `SealPSK(pkR_x25519, info, aad, pt, psk = ss_M, psk_id = "securacv:custody:mlkem768-psk-id:v1" ‖ SHA-256(ct_M))`; `enc = ct_M ‖ enc_X` (1120 bytes) |

Box 2 is expected to stay secret unless both the X25519 secret and the ML-KEM
secret are compromised; **the HPKE draft states that this hybrid property is
not proven** in its analysis. It is required whenever the vault's
`crypto_mode` is pq or hybrid, and compiles only with `pqc-vault`. X-Wing was
rejected (it needs SHA-3, which is not in the tree).

## 4. Domains and labels

The one signature domain S adds is `securacv:pwk:custody-recipient-binding:v1`
(`DOMAIN_CUSTODY_RECIPIENT_BINDING`): a holder signs their recipient-key hash
under it. The later domains (`custody-share-ack`, `policy-change-approval-recovery`,
`custody-release-request`, `custody-share-release`, `custody-reshare-request`,
`custody-reshare-deal`, `custody-liveness`, all `securacv:pwk:…:v1`) land with
U, V, W and Y.

| Use | Label |
|---|---|
| Generator H (SHA-512 input) | `securacv:custody:pedersen-generator-H:v1` |
| KEK wrap key (HKDF-SHA256 info; salt = `kek_salt`, ikm = k) | `securacv:vault:kek-wrap:v1` |
| Vault X25519 ikm (HKDF-SHA256 salt; ikm = master key) | `securacv:vault:x25519-ikm:v1` |
| HPKE info, share to holder | `securacv-custody-share:v1` |
| ML-KEM PSK id prefix | `securacv:custody:mlkem768-psk-id:v1` |
| Commitment-set hash | `securacv:custody:commitments:v1` |
| Recipient-key hash | `securacv:custody:recipient:v1` |
| Share file check | `securacv-share-file:v1` |
| `MKC1` AAD | `securacv-vault-custody-v1` |
| `TEK1` AAD | `securacv-custody-recipient-key-v1` |
| At-rest share AAD prefix | `securacv-share-at-rest-v1` |
| KAT stream | `securacv-threshold-custody-kat:v1` |

Reserved for later changes: `securacv-custody-release:v1`,
`securacv-custody-subshare:v1`, `securacv-vault-envelope:v1`,
`securacv:vault:dek:v1`, `securacv:custody:liveness-proof:v1`.

## 5. Byte formats (frozen as v1)

Integers are little-endian; `frame(x) = u32le(len x) ‖ x`; hex is lowercase.
A v1 reader refuses any other version by number; later versions are additive.

```
commitments_sha256 = SHA-256(frame("securacv:custody:commitments:v1") ‖ suite ‖ role ‖ split_id[16]
                             ‖ n ‖ m ‖ C_0 ‖ … ‖ C_{n−1})
recipient_key_hash = SHA-256(frame("securacv:custody:recipient:v1") ‖ frame(holder) ‖ holder_role (1 trustee | 2 recovery)
                             ‖ signing_public_key[32] ‖ x25519_public[32] ‖ (0x00 | 0x01 ‖ SHA-256(mlkem768_public)))
fingerprint        = the first 8 bytes of recipient_key_hash, printed "xxxxxxxx xxxxxxxx"
```

**Recipient public file** (`<holder>.recipient`, at most 8 KiB, unknown
fields refused, mode 0644): one line of compact JSON —
`format` (`securacv-custody-recipient:v1`), `holder`, `role`,
`signing_public_key`, `x25519`, `mlkem768` (box 2 only), `binding_signature`
(the holder's signature over `recipient_key_hash` under the binding domain).
Without the binding an operator could enroll their own recipient key under a
holder's name. A separate recipient key, not an Ed25519-to-X25519
conversion: FIDO2 signing credentials cannot do ECDH, and conversion cannot
carry ML-KEM.

**Recipient secret file** (`<holder>.reckey`, mode 0600): one line,
`securacv-custody-recipient-key:v1 <hex of TEK1>`. TEK1 reuses the `MKG1`
keyguard's KDF and AEAD pieces (Argon2id 65536/3/1, ChaCha20-Poly1305) in a
layout of its own, since it seals more than 32 bytes:

```
"TEK1" ‖ 0x01 version ‖ 0x01 kind ‖ m u32le (65536) ‖ t u32le (3) ‖ p u32le (1)   (refused BEFORE the KDF if different)
‖ salt_len u8 (16) ‖ salt[16] ‖ kek_commit[32] (SHA-256 of the Argon2id output)
‖ holder_len u8 (1..=64) ‖ holder ‖ role u8 (1 | 2) ‖ signing_public_key[32] ‖ recipient_key_hash[32]
‖ nonce[12] ‖ ct_len u32le (49 | 3633) ‖ ct
AAD = "securacv-custody-recipient-key-v1" ‖ every byte before the nonce
pt  = x25519_secret[32] ‖ (0x00 | 0x01 ‖ mlkem768_public[1184] ‖ mlkem768_secret[2400])
```

*Departure from the brief:* the header also carries the role and the signing
public key. The brief's open rule recomputes `recipient_key_hash` "from the
decrypted keys and the header's holder and role", but that hash covers the
role and the signing key, which the brief's layout did not record.

**`SHR1`, the share envelope** (`<holder>.<split8>.share`, or
`….rehearsal.share`; one line `securacv-share:v1 <hex>`, mode 0600):

| Off | Len | Field | Rule |
|---|---|---|---|
| 0 | 4 | magic `SHR1` | |
| 4 | 1 | version | `0x01` |
| 5 | 1 | suite | `0x01` |
| 6 | 1 | protection | `0x01` transport box 1 · `0x02` transport box 2 · `0x03` at rest (Argon2id) |
| 7 | 1 | role | `0x01` operational · `0x02` recovery · `0x03` rehearsal |
| 8 | 1 | release_box | `0x01`/`0x02` for operational; `0x00` for recovery and rehearsal |
| 9 | 16 | split_id | random |
| 25 | 32 | `C_0` | the KEK commitment |
| 57 | 32 | container_sha256 | zero only for rehearsal |
| 89 | 32 | device_public_key | zero only for rehearsal |
| 121 | 8 | issued_bucket_start | a multiple of 600 |
| 129 | 1 | n | `2..=m` |
| 130 | 1 | m | `n..=32` |
| 131 | 1 | index | `1..=m` |
| 132 | 1 | holder_len L | `1..=64` |
| 133 | L | holder | `[A-Za-z0-9._-]` |
| — | 32 | recipient_key_hash | this holder's |
| — | 32·(n−1) | `C_1..C_{n−1}` | canonical, non-identity |
| — | 1 | roster_count | `= m` |
| — | m × (1+1+id+32+32) | roster: index ‖ id_len ‖ id ‖ signing key ‖ recipient_key_hash | indices exactly `1..=m`; ids and keys unique; the entry at `index` repeats the holder |
| — | 2 + C | container_len u16 ‖ the `MKC1` copy | `C ≤ 8192`, hash equal to container_sha256; `C = 0` only for rehearsal |

Every byte above is the AEAD's associated data. The body is either HPKE
(`enc_len u16 (32 | 1120) ‖ enc ‖ ct_len u16 (80) ‖ ct`, info
`securacv-custody-share:v1`, pt `s ‖ t`) or, at rest, `m ‖ t ‖ p` (u32 each,
refused before Argon2id) `‖ salt[16] ‖ kek_commit[32] ‖ nonce[12] ‖ ct[80]`
under `Argon2id(passphrase, salt)` with AAD `"securacv-share-at-rest-v1" ‖
header`. The file ends with `SHA-256("securacv-share-file:v1" ‖ every
preceding byte)`. Caps: 16 384 bytes binary, 32 787 text, both checked
before any length is read. Every refusal is a closed reason printed as
`share file: {why}`; one reason was added to the brief's set, `the issued
bucket must be a multiple of 600 seconds`.

The roster pin is what a holder's tool counts approvals against (V). The
container copy means one share plus n−1 others recover a destroyed hub's
vault from the envelopes alone; the copy is ciphertext under k.

**`MKC1`, the custody container** (defined in S; written into a vault in U):

```
0 magic "MKC1" ‖ 4 version 0x01 ‖ 5 suite 0x01 ‖ 6 crypto_mode (1 classical · 2 pq · 3 hybrid)
‖ 7 kek_salt[16] ‖ 23 C_0[32] ‖ 55 vault_x25519_public[32] (zero iff pq)
‖ 87 mlkem_pk_len u16 (0 iff classical, else 1184) ‖ 89 mlkem768_public[P]
‖ 89+P nonce[12] ‖ 101+P ct_len u32 (payload + 16, ≤ 4096) ‖ 105+P ChaCha20-Poly1305 ciphertext
wrap_key = HKDF-SHA256(salt = kek_salt, ikm = k, info = "securacv:vault:kek-wrap:v1")
AAD      = "securacv-vault-custody-v1" ‖ bytes[0..105+P]
payload  = 0x01 ‖ master_key[32] ‖ (0x00 | 0x01 ‖ u32 pk_len ‖ pk ‖ u32 sk_len ‖ sk)   (the kem-mlkem768.key encoding)
```

Cap 8192 bytes (the worst case is 4931). Open order: header bounds; the KEK
commitment (a clean error before any AEAD runs); HKDF; the AEAD; the
payload; the header's public keys against the contents. There is no path
binding: custody exists to survive the loss of the hub.

**Split record** (`<split8>.split.json`, at most 64 KiB, unknown fields
refused): `format` (`securacv-custody-split-record:v1`), `role`, `split_id`,
`n`, `m`, `kek_commitment`, `commitments`, `commitments_sha256`,
`container_sha256`, `device_public_key`, `issued_bucket`, `holders`
(`index`, `id`, `signing_public_key`, `recipient_key_hash`). It is the public
record every holder compares their share against.

## 6. The holder tools (built in S)

| Command | What it does |
|---|---|
| `custody recipient keygen --holder ID --role trustee\|recovery --signing-key K [--pq] [--passphrase-fd N] --output-key F --output-public P` | Makes the recipient key, signs its binding with the holder's trustee key (TKG1 or legacy plaintext), wraps the secret as TEK1 under a new passphrase, re-opens it, writes both files (never overwriting) |
| `custody rehearse --threshold N --recipient P… --output-dir D` | Deals a REHEARSAL split of a throwaway key, proves it, writes one share per recipient and the split record |
| `share inspect --share F` | Prints what the file says about itself; a damaged file still shows its header, with `File check: FAILED` |
| `share verify --share F (--recipient-key K \| --at-rest) [--split-record R] [--passphrase-fd N]` | Checks, before any HPKE runs, that the share's roster pins for the holder the signing key their own recipient key file records; opens the share, runs the Pedersen check, compares with the split record, checks the container copy names the share's KEK |
| `share protect --share F --recipient-key K [--passphrase-fd N] --output G` | Rewraps a transport share at rest under Argon2id, then re-opens and re-checks it before writing |

Passphrases come only from the terminal (echo off) or `--passphrase-fd`,
never argv or the environment. A Pedersen check is reported as "checks
passed", never as "verified". No line these commands add carries a path, a
full hash or a secret: hashes and keys print as 16-hex prefixes, and where
the brief's lines named a file by path they name the flag instead (`the
--output-key file`, `the --output file`, `the output directory`). The one
path that can appear is the trustee-key release's existing unwrapped-key
warning: `custody recipient keygen` loads `--signing-key` through
`load_trustee_signer`, which still names a legacy plaintext key's path so the
operator can run the `break_glass trustee wrap` command it suggests. Every
printed block is a pure function pinned by a test.

**The roster pin is checked against the holder's own key file.** A recipient
key opened from its `TEK1` file carries the signing key that file records. A
transport share opens only if its own roster entry pins exactly that key and
its recorded recipient hash is the one computed under it; the hash is never
rebuilt from a signing key the share header supplies. A share that names the
holder and their recipient key but pins another signing key for them (a
faulty dealer pinning a key it holds, whether the recipient hash was
recomputed over that key or left honest) is refused before any HPKE runs:

```
share check FAILED: the roster in this share pins a different signing key for you than your recipient key records — do not acknowledge it; record a class-d-security exception (the dealer is faulty)
```

`share protect` opens the same way, so a forged pin never reaches an at-rest
file. Every text-form refusal of `share verify` and `share protect`, the
oversize one included, prints as `share check FAILED: share file: {why}`; an
I/O failure reading the file is not a share check and keeps its own line.
`custody recipient keygen` writes the secret file, then the public file; if
the public file cannot be written, it removes the secret file it just
created, so a retry is not refused by an orphan.

## 7. The dependency line

`Cargo.toml` names `curve25519-dalek = { version = "5.0.0", default-features = false, features = ["alloc", "precomputed-tables", "zeroize"] }`.
The audit argument: `ed25519-dalek` 3.0.0 already compiles the same
`curve25519-dalek` 5.0.0 into every build, enabling `digest` itself,
`precomputed-tables` through `fast`, `zeroize` through `zeroize` and `alloc`
through `pem`; the root already enables `pkcs8` and `pem`. The direct line
asks for a subset of that set, adds no package and no feature, and gives the
custody code `Scalar` and `RistrettoPoint`. `from_uniform_bytes`,
`from_bytes_mod_order_wide`, `from_canonical_bytes`, `invert` and
`mul_clamped` need no feature.

`cargo tree -e features -i curve25519-dalek@5.0.0`, before the line (at the
base commit). Cargo prints the root package's local checkout path; it is
written here as `<repo root>`, and the output is otherwise verbatim:

```
curve25519-dalek v5.0.0
├── curve25519-dalek feature "alloc"
│   └── ed25519-dalek feature "alloc"
│       └── ed25519-dalek feature "pem"
│           └── witness-kernel v0.9.0 (<repo root>)
│               └── witness-kernel feature "default" (command-line)
├── curve25519-dalek feature "digest"
│   └── ed25519-dalek v3.0.0
│       ├── ed25519-dalek feature "alloc" (*)
│       ├── ed25519-dalek feature "default"
│       │   └── witness-kernel v0.9.0 (<repo root>) (*)
│       ├── ed25519-dalek feature "fast"
│       │   └── ed25519-dalek feature "default" (*)
│       ├── ed25519-dalek feature "pem" (*)
│       ├── ed25519-dalek feature "pkcs8"
│       │   └── witness-kernel v0.9.0 (<repo root>) (*)
│       │   └── ed25519-dalek feature "pem" (*)
│       ├── ed25519-dalek feature "signature"
│       │   └── ed25519-dalek feature "alloc" (*)
│       └── ed25519-dalek feature "zeroize"
│           └── ed25519-dalek feature "default" (*)
├── curve25519-dalek feature "precomputed-tables"
│   └── ed25519-dalek feature "fast" (*)
└── curve25519-dalek feature "zeroize"
    └── ed25519-dalek feature "zeroize" (*)
```

After:

```
curve25519-dalek v5.0.0
├── curve25519-dalek feature "alloc"
│   └── witness-kernel v0.9.0 (<repo root>)
│       └── witness-kernel feature "default" (command-line)
│   └── ed25519-dalek feature "alloc"
│       └── ed25519-dalek feature "pem"
│           └── witness-kernel v0.9.0 (<repo root>) (*)
├── curve25519-dalek feature "digest"
│   └── ed25519-dalek v3.0.0
│       ├── ed25519-dalek feature "alloc" (*)
│       ├── ed25519-dalek feature "default"
│       │   └── witness-kernel v0.9.0 (<repo root>) (*)
│       ├── ed25519-dalek feature "fast"
│       │   └── ed25519-dalek feature "default" (*)
│       ├── ed25519-dalek feature "pem" (*)
│       ├── ed25519-dalek feature "pkcs8"
│       │   └── witness-kernel v0.9.0 (<repo root>) (*)
│       │   └── ed25519-dalek feature "pem" (*)
│       ├── ed25519-dalek feature "signature"
│       │   └── ed25519-dalek feature "alloc" (*)
│       └── ed25519-dalek feature "zeroize"
│           └── ed25519-dalek feature "default" (*)
├── curve25519-dalek feature "precomputed-tables"
│   └── witness-kernel v0.9.0 (<repo root>) (*)
│   └── ed25519-dalek feature "fast" (*)
└── curve25519-dalek feature "zeroize"
    └── witness-kernel v0.9.0 (<repo root>) (*)
    └── ed25519-dalek feature "zeroize" (*)
```

The feature set is identical (`alloc`, `digest`, `precomputed-tables`,
`zeroize`), and `Cargo.lock` changes by one line: `"curve25519-dalek 5.0.0"`
added to the root package's dependency list.

## 8. The known-answer tests

`spec/fixtures/gen_threshold_custody_v1.py` is a second implementation of
everything above in the Python 3 standard library: field arithmetic,
ristretto255, scalars, X25519, ChaCha20-Poly1305, HKDF, HPKE, Ed25519
signing and every format. Before writing it checks itself against dalek's
table of `i·G` encodings, dalek's Elligator vectors, the draft's one-way-map
vectors, the HPKE editor's Base and PSK vectors, and RFC 8032 test 1. It
writes `spec/fixtures/threshold_custody_v1.json` from a deterministic stream
(`block(label, i) = SHA-512("securacv-threshold-custody-kat:v1" ‖ u32le(len label) ‖ label ‖ u64le(i))`);
`--check` regenerates in memory and compares bytes (CI: `rust.yml`).

The fixture holds the KEK, a 3-of-5 operational and a 4-of-6 recovery split
of the same KEK (coefficients, commitments, shares, public shares, Lagrange
subsets), eleven recipients with their binding signatures, eleven box-1
share files, box 2's PSK layer, an at-rest share, TEK1, a classical and a
hybrid `MKC1`, both split records, and 41 negative cases with their exact
refusals. `src/vault/custody/kat.rs` rebuilds every value in Rust and
compares bytes; every comparison agreed on the first run.

The review of S added four negative cases and changed no other fixture byte
(the new cases draw from their own stream, `negative/forged-pin`, so every
earlier stream is unchanged): alice's share with its index flipped to 0 and
set to 6 (`share file: index must be between 1 and 5`), and two shares whose
roster pins a dealer-held signing key for alice, sealed honestly to her real
X25519 key, one with the recipient hash recomputed over the forged key and
one with the honest hash kept. Both must be refused by her key before HPKE
(`custody: the roster in this share pins a different signing key for you
than your recipient key records`). The Python generator wrote the cases, and
the Rust code agreed with them on the first run.

Two layers are not cross-implemented. Argon2id is not in the Python standard
library: the at-rest and TEK1 vectors pin the AEAD under a given Argon2id
output, and the Argon2id outputs themselves are pinned from the Rust
`argon2` 0.6 crate and labeled "not cross-implemented". ML-KEM-768 is not
implemented in Python: box 2 is pinned at its PSK layer under a given shared
secret and ciphertext, and the hybrid container carries opaque KEM bytes.

## 9. The share-release protocol (normative; lands in V)

Every arrow is a file carried by people; nothing uses the network.

1. Item 4's request, n approvals, authorization and the `Granted` row; the
   grant notice goes to every holder.
2. During the cooling-off any holder may veto; the veto file goes to the hub
   **and** to every other holder's veto folder.
3. After `not_before`, one hub process: the gate checks; the token is minted
   in memory and **redeemed first** (burn, then the device-signed
   `TokenRedeemed` row with a `custody` field, committed); then a fresh
   ephemeral X25519 key (and ML-KEM key for box 2) in process memory, and a
   device-signed release request carrying the grant notice, the redemption
   row, the carried policy and the session.
4. Each holder, on their own device: thirteen checks in order (the request is
   signed by the device key pinned in the share; the grant row and its terms;
   the carried policy is the one the approvers consented to and matches the
   share's roster; the request hash re-derives; approvals counted against
   the **pinned** keys; not a seal grant; the window on the holder's own
   clock; no veto in their folder; the redemption row is this grant's and
   names their split; the session matches their share; the share is
   operational; it opens and passes its check), a what-you-see display, a
   typed grant prefix, then the release: HPKE to the session key with AAD
   `RRQH ‖ index ‖ frame(holder)` and a holder signature over the release hash.
5. The hub keeps the first valid release per index; with n it zeroizes the
   ephemeral secrets, reconstructs, checks `C_0`, derives the wrap key, opens
   `MKC1`, compares with the legacy key form in dual custody, and unseals
   through the same token check as today.
6. The `Unsealed` row records the session's public parts, the request hash and
   each release's `{holder, index, ct_sha256, signature}`: never a share, a
   ciphertext or a secret.

A release for one request can never open another: the request hash binds the
grant, the redemption, the envelope, the split, the session's public keys and
a nonce; the AEAD's AAD carries it; the holder's signature covers it; and the
ciphertext opens only under a key that never left one process.

## 10. Privacy, threats and rejections

**Invariant I.** `export_for_vault()` behind a token stays the only raw exit;
custody adds a key, not an exit. **Invariant II.** New records hold ids
already in the policy, perfectly hiding commitments, public keys, ciphertext
hashes, 10-minute buckets and closed strings. **Invariant III.** No network
behavior is added; every new time is a bucket. **Invariant IV.** Shares,
recipient secrets and release plaintext never reach the hub's disk.
**Invariant V** becomes cryptographic only after retirement (X).

| Adversary | What stops it | Change |
|---|---|---|
| Stolen vault, database and device seed, after retirement | k is on no disk; shares never touch the hub; `MKC1` is an AEAD under HKDF(k); Pedersen hides k | X |
| The same during dual custody | nothing new: the legacy key form is on the hub, as today | U |
| A malicious operator with the device key | no unseal without n releases, each holder checking the device-signed grant, the approvals against the pinned roster, the window on their own clock, their veto folder and the redemption | V |
| The same with a modified binary at an unseal (R-KEEP) | nothing structural; detected only when the kept key is used through the device | V/X/W |
| One compromised holder laptop | one share is fewer than n; at rest it is under Argon2id | S |
| Fewer than n colluding holders | Pedersen hiding | S |
| A corrupted share | the Pedersen check at dealing, `share verify`, acknowledgment, release and collection; the file check; the AEAD | S/V |
| A sharing-code bug | the cross-implementation KATs, deal-and-prove, every holder's own check, the dual-custody drill | S/U/V/X |
| A quantum adversary harvesting now | Pedersen hides k; box 2 for pq and hybrid; `MKC1` is symmetric; at-rest shares under Argon2id | S/T/X |

Rejected, among others: splitting the master key itself (it is a uniform 32
bytes, not a scalar, and GF(2^8) Shamir has no group commitments); Feldman
commitments; a sharing crate (new audit surface, when the in-tree code is
small and pinned by KATs); Ed25519-to-X25519 conversion; an ML-KEM seed
split; X-Wing; releasing shares at approve time; releasing before the
redemption row; threshold decryption in v1 (kept as the future seam);
grantless drills; a holder tool that trusts a hub-supplied roster;
reconstruct-then-resplit resharing; a truncated SLIP-style digest; a `0x00`
presence byte in `full_commitment`; path-binding `MKC1`; shares or release
ciphertext on the hub; retiring at activation; "verified" for a Pedersen
check; custody on a 1-of-m quorum.

## 11. Residuals, named once

- **R-KEEP.** At every share-based unseal the device rebuilds k and holds the
  master key; a modified binary can keep it. Detected when the kept key is
  used through the device (a later unseal without releases) or when a
  redemption has no `Unsealed` row; **not detected** when used offline;
  bounded by rekey from a clean hub for future envelopes; removable only by a
  future threshold-decryption profile for new envelopes.
- Same-KEK resharing is not cryptographic revocation; rekey is, except for old
  containers kept in backups.
- Dual custody is no stronger than today until retirement.
- Classical custody seals new envelopes to X25519, which a future quantum
  adversary can open; the shipped add-on image builds without `pqc-vault`.
  Box-1 transport and release files, if harvested, fall the same way;
  at-rest shares do not.
- Holder clocks enforce the cooling-off and the deadline; a misset or coerced
  clock counts as that holder.
- The veto locks only where it reaches.
- Redemption first means a failed collection spends the grant.
- The emergency release deadline (`grant bucket + window_s`) awaits item 4's
  owners.
- Process memory during collection; `PR_SET_DUMPABLE` covers ptrace and core
  dumps only.
- The issuing hub is the dealer, which already holds the master key.
- ML-KEM-768 runs through RustCrypto's `ml-kem` 0.3.4, the crate the vault
  itself moved to when the PQClean bindings were removed (#1991); box 2
  scrubs the shared secret and the expanded key bytes it imports, and
  `ml-kem`'s own key types zeroize under its `zeroize` feature.
- The zero-overwrite at retirement depends on the medium.
- No automated agent holds share authority: a `won't`, not a `can't`.

## 12. What was checked, and what stays unverified

Checked while building S:

- **ristretto255.** Encode and decode against dalek's table of `i·G`; the
  one-way map against the 11 draft-04 Appendix A.3 vectors and dalek's 16
  Elligator vectors, by the Python generator and by the Rust KATs. The RFC
  9496 constants were taken from dalek's source and checked against their
  defining equations; RFC 9496 itself was not fetched.
- **H.** The Rust and Python values agree (`b293a0f3…`); the brief's prototype
  value was wrong (§2).
- **HPKE.** The editor's Base and PSK vectors for this suite reproduce in
  both implementations: keys, shared secret, key, base nonce, exporter
  secret, the sequence-0 ciphertext and three exports.
- **The `cargo tree` feature identity** was run (§7).
- **RFC 8032 test 1** reproduces in the Python generator and through
  `ed25519-dalek`; the constants were recalled, not fetched.

Still unverified: RFC 9496 and RFC 9180 as published (only the sources
above); RFC 7748 and RFC 8439 except through the HPKE vectors; FIPS 203 and
`ml-kem`'s conformance to it (its import makes FIPS 203's encapsulation-key
and decapsulation-key checks, as the vault relies on); SP 800-57 / FIPS 140-3 "split knowledge"; the literature
(Pedersen 1991, Feldman 1987, Desmedt–Jajodia 1997, Herzberg et al. 1995,
Okamoto 1992, eprint 2021/509) — the code is checked for correctness only,
never for security, and a cryptographer's review of §2–§5 and of S's code is
to be recorded before evidence depends on the release path (V); the
PSK-mode hybrid security, which the draft itself calls not proven; dalek's
constant-time behavior (documentation, not measurement); the coverage of
`PR_SET_DUMPABLE` and of a zero-overwrite; CTAP2 offering no ECDH over a
signing credential; item 3/4 shapes not yet merged (item 5 adopts whatever
spelling they land with); the emergency release deadline; every older-build
refusal string (checked by reading, pinned by T and X); and the Argon2id
pins, which come from one implementation only.
