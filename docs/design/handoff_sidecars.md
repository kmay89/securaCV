# Handoff sidecars and the export receipt's context (design, as built)

This is the committed design record for per-file **handoff sidecars**
(`spec/quorum_unseal_v2.md` §5, first bullet) and the export receipt's
disclosure context (the closing sentence of §3.6). The operator and
verifier guide is [`docs/handoff_sidecars.md`](../handoff_sidecars.md); the
code is `src/handoff.rs`, `src/bin/handoff_sidecar.rs`, the `Unsealed`
outcome in `src/break_glass/core.rs` and `Kernel::record_unsealed` /
`ExportContext` in `src/lib.rs`.

"Handoff sidecar" is always two words in prose. The module is `handoff`,
the binary `handoff_sidecar`, the file suffix `.securacv-handoff.json`, and
the format literal `securacv-handoff:v1`.

> **Release coupling (merge gate).** The `Unsealed` outcome is a new
> `BreakGlassOutcome` variant. Every reader built from the token-redemption
> change (#1936) parses each break-glass row strictly and fails on it with
> `unknown variant` — a #1936-era `witnessd` boots into safe mode. The
> design gated this change on merging before the first release that
> contains #1936 (D10 below); as built, kernel 0.9.1, Flasher 0.12.5 and Lab
> app 0.4.1 shipped #1936 first, and the owner confirmed on 2026-11-16 that
> no deployment reads these ledgers yet. What still holds is that every
> reader (`witnessd`, `break_glass`, `log_verify`, `envelope_verify`,
> `court_export`, every observer host) is upgraded before the first unseal
> recorded by this build. `pre_d_reader_rejects_unsealed_rows`
> (`src/disclosure_tests.rs`) pins the break so this paragraph stays true.

## The decision in one paragraph

**The sidecar.** Every act that writes a disclosed file also writes a **handoff sidecar** beside it, `<file>.securacv-handoff.json`. Those acts are `break_glass unseal`, the served console unseal, the drill, and `export_events` (scheduled exports included). The sidecar is canonical `securacv-cjson-v1` bytes with no trailing newline, always mode 0600. It is an **unsigned carrier**: it embeds verbatim the device-signed receipt rows of that disclosure, references RFC 3161 tokens by their SHA-256, and labels everything else *declared*. PR D adds **no signing domain**.

**The `Unsealed` row.** Since #1936, `unseal` runs a signing `Kernel`. PR D adds the one device fact that exists only inside the unsealing process. After #1936's unchanged burn-first redemption and a successful decrypt, and **before any byte is written**, the kernel appends a device-signed `BreakGlassOutcome::Unsealed` row to `break_glass_receipts`. That row:
- names the `TokenRedeemed` row it completes;
- carries a **salted** commitment to the plaintext;
- optionally carries a salted commitment to a declared recipient.

Both salts exist **only in the sidecar**. They are never at rest in the database and never printed.

**`ExportReceipt` fields.** `ExportReceipt` gains two trailing optional fields:
- `context`: a self-export's declared operator, and/or a salted recipient commitment.
- `authorizing_receipt`: the hash of the `Granted` receipt whose token the kernel just validated and redeemed.

**Consent by reference.** A quorum export **references** its trustee-consented operator context through `authorizing_receipt` and never copies it. The type refuses an operator beside an authorizing receipt, and so does every verifier.

**What a downstream verifier needs.** With the file, the sidecar, the pinned device public key, `sha256sum`, `openssl` (≥ 3.0) and `jq` (no `xxd`), a downstream verifier re-derives:
- the file digest;
- every embedded row's entry hash and Ed25519 signature;
- the joins grant → redemption → unseal record, or grant → export receipt;
- the plaintext pairing;
- the recipient commitment.

`handoff_sidecar verify` adds the TSA countersignature check (`--ca`) and the deep database checks (`--db`).

**Invariant III.** The sidecar's only time value is a 600 s bucket. No `created_at`, row id, chain ordinal or TSA `genTime` enters it. Nothing in PR D opens a socket.

**Rejected, with reasons (keep rejected):**
- **Carrier-only, as in the September design.** Under it, the unseal file↔envelope pairing stays an assertion when the device knows the fact in-process. The vault envelopes record no plaintext digest (`src/vault/format.rs`).
- **A device-signed sidecar, or a signed row that names the recipient.** It reads as "the device attested the recipient", which is false. It needs a new signing domain. `emit` and `court_export` could never regenerate it, because they hold no seed.
- **An unsalted plaintext SHA-256 (or length) in the ledger** (Designs 0 and 2). `break_glass_receipts` is never pruned (`docs/evidence_lifecycle.md`) and ships from genesis in every evidence envelope (`src/envelope.rs`; `spec/evidence_envelope.md`). An unsalted media digest there is a hash "comparable across … long time horizons" (`spec/invariants.md`): anyone with an envelope and a leaked clip could attribute the clip to this hub.
- **Decrypt-before-burn, so the digest can ride on `TokenRedeemed`** (Design 2). It overturns #1936's normative rule that the burn is recorded "before it unseals anything" (`src/lib.rs`; `spec/break_glass.md` "Redemption").
- **A new ledger table for unseal records.** That would need a sixth anchor subject, a new envelope section, a new walk and a new kit section, all for one fact that belongs beside the redemption it completes.
- **Copying the grant's `OperatorContext` into the quorum export receipt** (Designs 0 and 2). It gives the requester's identity a second unprunable home and adds no tamper evidence.
- **Embedding `token_der_hex`** (Designs 0 and 1). It brings a one-second TSA `genTime` into the sidecar.
- **Printing the recipient name or salts on any error path** (all three designs). They would reach the terminal and, under systemd, the journal.
- **A `domain` key in the sidecar.** A verifier picks the signing domain from the ledger name by its own recipe, never from the sidecar.
- **Softening a verdict because of unknown receipt fields** (Design 0).
- **`--no-handoff`, in-place `refresh`, a JS sidecar verifier, an `emitted_by` key** (it is declared and checks nothing), **and a salt table at rest.**

---


## The consent and verifier frame

### 1.1 Every new signed byte: who agreed to it, how it is rendered

| New byte(s) | Where | Signed by | Trustee-consented? | Rendered as |
|---|---|---|---|---|
| `Unsealed.redemption_entry_hash` | break_glass_receipts row | device | n/a (a link) | "device-signed link to the redemption" |
| `Unsealed.plaintext_commitment` | same row | device | covered: it records the outcome of the act the grant consented to (unsealing envelope E); salted, so it names nothing | "device-signed: the device decrypted envelope E to the plaintext this commitment fixes" |
| `Unsealed.recipient_commitment` | same row | device signs; **operator declares** | **no** | "declared by the operator at unseal; the commitment fixes the declaration, it does not verify it" |
| `ExportReceipt.authorizing_receipt` | export receipt | device | n/a: the token the kernel itself validated and redeemed | "device-signed: authorized by Granted receipt `<hex>`" |
| `ExportReceipt.context.recipient_commitment` | export receipt | device signs; operator declares | **no** | as for the unseal recipient |
| `ExportReceipt.context.operator` | export receipt, **self-export only** | device signs; the device-key holder declares | no; no trustees exist on this path | "declared by the exporting operator (device-key holder); not trustee-consented" |
| an operator beside `authorizing_receipt` | — | — | — | refused at construction (by type) and by every verifier (rule R6) |

**Rendering rule.** It binds `render_human`, `break_glass receipts`, `handoff_sidecar verify`, `export_verify`, the court kit and the viewer.
- The word **"trustee-consented"** labels only fields read from a `Granted` row.
- **"declared"** labels every `context.operator` and every recipient.
- **"device-signed"** labels links and commitments.
- **"Verified"** appears only for an Ed25519 signature checked against a pinned key (AGENTS.md rule 4).
- There are no legal conclusions anywhere. The words used are "tamper-evident", "signed", "declared" and "consistent".

### 1.2 What a verifier checks, against what

| Fact | Check | Source of truth |
|---|---|---|
| The sidecar describes this file | `sha256sum FILE` = `.file.sha256`; `wc -c` = `.file.bytes` | the file |
| A row is genuine | `SHA256(prev_hash ‖ payload_json)` = `entry_hash`; Ed25519 over `SHA256(le32(len) ‖ domain ‖ entry_hash)` with `securacv:pwk:break-glass-receipt:v2` (35 bytes, `\043\000\000\000`) or `securacv:pwk:export-receipt:v2` (30 bytes, `\036\000\000\000`) | device signature (`src/crypto/signatures.rs`; `hash_entry`, `src/log/mod.rs`). The domain is chosen **by the verifier from `ledger`**, never read from the sidecar |
| **The device decrypted the envelope to exactly these bytes** (unseal) | `SHA256(le32(34) ‖ "securacv:pwk:unsealed-plaintext:v1" ‖ salts.plaintext ‖ FILE)` = `Unsealed.plaintext_commitment` | device signature over the `Unsealed` row |
| The file carries its own signed receipt (export) | the verbatim export `payload_json` occurs in FILE as `"receipt":<payload>,"prev_hash":`. The library check parses the bundle and runs `verify_export_bundle` | the bundle's device-signed receipt |
| Under whose consent | `Unsealed.redemption_entry_hash` = the redemption's `entry_hash`; `TokenRedeemed.receipt_entry_hash` = the grant's `entry_hash` (or export `authorizing_receipt` = the grant's `entry_hash`); the grant names its trustees, purpose and `OperatorContext` | the device-signed Granted row; its request hash binds the context the trustees approved (`verify_context_binding`, core.rs). The quorum is re-derived only with `--db` |
| Who received it | recompute the recipient commitment from `.recipient` and `.salts.recipient` = the commitment in the signed row | a **declaration** fixed at disclosure; the device did not check who received the file |
| Fixed in time | the referenced token's SHA-256 matches a token file; its imprint = the covered hash; `openssl ts -verify` with the TSA CA | the TSA's countersignature |

Docs and output say plainly where a deeper check needs the database: quorum re-derivation under the grant's policy era, PQ signatures, and key-rotation lineage.

---


## Decisions

### D1 — One format (retained)
- **Format and file.** `securacv-handoff:v1`, written to `<file>.securacv-handoff.json` beside the file.
- **Bytes.** `serde_json::to_value(&HandoffSidecar)`, then `canonical_json::to_canonical_bytes` (src/canonical_json.rs; JS `canonicalize`, viewer/verify_core.js). There is no trailing newline, so `sha256sum <sidecar>` is well-defined. Read it with `jq .`.
- **Parsing.**
  - Every struct is `deny_unknown_fields`, so any new key means a `v2`.
  - Anchor entries are parsed by `kind`, so PR E can add `"opentimestamps"` without a format bump.
  - The size cap is 1 MiB (`MAX_SIDECAR_BYTES`).
- **Non-canonical bytes are a FAIL.** A pretty-printed or re-serialized copy FAILs with an actionable reason: re-emit, never edit.
- **Mode.** Every sidecar is written **0600** through the restricted writer. It can carry a recipient's identity, so this holds beside raw media and beside bundles alike.

### D2 — Carrier + one device-signed `Unsealed` row, salted (option b′)
**Shape.** The row is a `BreakGlassReceipt` with:
- `outcome: Unsealed { redemption_entry_hash, plaintext_commitment, recipient_commitment? }`;
- `vault_envelope_id`, `request_hash`, `ruleset_hash`, `policy_commitment` and **`time_bucket`** restated from the redemption, so there is no new clock read;
- `trustees_used = []`, the empty approvals commitment, and `purpose`/`context`/`request_bucket` = `None`.

This mirrors `redemption_of` (core.rs).

**Order.** #1936's `redeem_token_durably` is unchanged and still burns first. Then `vault.unseal` decrypts into RAM. Then `record_unsealed` appends the row. Only then is the sidecar written, and only then the `.raw` (D7).

**What the row means.** "The device decrypted envelope E, under the redemption R, to the plaintext this commitment fixes." It does not claim where the bytes went.

**Salted commitment.**
- Formula: `plaintext_commitment = SHA256(le32(34) ‖ "securacv:pwk:unsealed-plaintext:v1" ‖ salt32 ‖ plaintext)`.
- The salt is 32 bytes from the OS RNG (`rand::random::<[u8; 32]>()`, never a zeroed buffer filled afterwards), fresh per row.
- **Pinned vector:** salt `22`×32 with plaintext `securacv-fixture` gives `926a5596f3ec19a0f56ad186d1e7a28a444694a184d506e37b28685e625da1cb`. This was recomputed during synthesis in Python and in the POSIX recipe under `dash` and `bash`.
- The recipient, who holds the file anyway, gets the unsalted `file.sha256` in the sidecar beside it.

**Fail-closed.** If the row cannot be appended, the cleartext is zeroized and nothing is written. The token is already spent and its `TokenRedeemed` row records that; the trustees re-approve. This is #1936's own rule: "a redemption that cannot be recorded does not happen" (lib.rs).

**A redemption with no `Unsealed` row is normal.** That covers export redemptions, pre-PR-D unseals, decrypt failures, and a dropped tail row (residual 9 below). The verifier never requires the row.

### D3 — `ExportReceipt.context`, then `authorizing_receipt` (retained, re-examined)
**Why keep `authorizing_receipt`.** `TokenRedeemed` links a redemption to its grant (core.rs), but nothing links an **export receipt** to either. One bucket can hold several `export:events` grants, so "which grant authorized export #n" is otherwise a guess. The export ledger should name its consent.

**Why it names the grant, not the redemption.**
- The token already carries the grant hash under the device token signature (`token.receipt_entry_hash()`).
- Consent lives on the grant.
- `RedemptionLedger` enforces one redemption per grant (verify.rs), so the redemption is reachable 1:1.
- It makes rules R1–R6 checkable (the R rules in `docs/handoff_sidecars.md`).

**Every post-PR-D quorum export carries it**: `export_events_authorized` delegates to the new path.

**Order and serde.** `context`, then `authorizing_receipt`, after `token_id`. Both are `Option` + `#[serde(default, skip_serializing_if = "Option::is_none")]`, LAST, per the struct rule at lib.rs.

**Hash encoding.** Hashes inside signed payloads stay `[u8; 32]` integer arrays, like every other chain hash. The sidecar carries hex, and the shell recipe converts with a jq `hex` function.

### D4 — Recipient: a per-record salted commitment (retained, extended)
- **Formula.**
  - `recipient_commitment = SHA256(le32(33) ‖ "securacv:pwk:handoff-recipient:v1" ‖ salt32 ‖ recipient_cjson)`.
  - `recipient_cjson` is the securacv-cjson-v1 of `{"key"?, "name", "organization"?}`: absent keys omitted, keys sorted, no whitespace.
  - `key` is a 64-char lowercase hex Ed25519 public key the operator **claims**. It is never verified.
- **Salt.** Fresh from the OS RNG per record.
- **Where the plaintext and salt live.** Only in the sidecar. They are never printed, logged or stored at rest.
- **Pinned vector.** Salt `11`×32 and `{"name":"Dana Reyes","organization":"Example County Records Office"}` give `5d02af2bfc1a29471141dbb6b118f2b2d1af259e7f4dcf8681d9dfc8e9e598ce`. This was recomputed in Python, under `dash`/`bash` with the POSIX decoder, and with `basenc`.
- **Field validation.** Every recipient field passes `check_text_field` (core.rs). That makes `jq -cjS .recipient` byte-equal to `recipient_cjson`; a unit test pins the equality.
- **Inputs.** `--recipient-file <PATH>` (JSON, `deny_unknown_fields`) is the **preferred** input, documented first, because it keeps names out of shell history. The `--recipient-name/--recipient-org/--recipient-key` flags remain.
- **Four recipient bindings**, checked by `verify`:

  | Binding | Meaning |
  |---|---|
  | `receipt_commitment` | the sidecar opens a signed commitment |
  | `committed_not_disclosed` | a signed row commits to a recipient this sidecar does not name; it is produced by `redact`, by a recovered `emit`, and by a `court_export`-generated sidecar |
  | `declared` | the sidecar names a recipient that no signed row commits to |
  | `none` | no recipient |

### D5 — Quorum exports reference the consent; they never copy it (re-decided)
- **Construction.** `export_events_bundle_authorized_with` takes only a recipient commitment. It cannot be passed an operator, so it sets `context.operator = None` by construction.
- **Rule R6.** Every verifier fails a receipt that carries `context.operator` beside `authorizing_receipt`.
- **clap.** `--requested-by` (and its dependents `--reason`, `--case-ref`, `--requester-key`) take `conflicts_with = "break_glass_token"`.
- **Why.**
  - The requester's identity stays in one ledger row, the grant.
  - `authorizing_receipt` binds the grant's entry hash, which binds its recorded context, which re-derives the request hash the trustees approved.
  - A verifier compares nothing, so nothing can be retyped.
- **Spec change.** Spec §3.6's closing sentence is rewritten to match (done in the same change).

### D6 — Context is a separate parameter (retained)
`ExportOptions` stays `Copy` and unchanged (lib.rs). New `*_with` kernel functions are added. The existing functions delegate with `None` and produce byte-identical receipts, except that `export_events_authorized` / `export_events_bundle_authorized` now always set `authorizing_receipt`.

### D7 — Emitters, ordering, and the stale-sidecar rule
| Emitter | Sidecar | Recipient input | Order |
|---|---|---|---|
| `break_glass unseal` | yes | `--recipient-file` / `--recipient-*` | stale check → burn → decrypt → `Unsealed` row → **sidecar** → `.raw` |
| console `authorize_unseal` | yes | none in PR D | the same, through the same helper |
| `break_glass drill` | yes, into its temp dir; then verified under the pinned drill key | none | the same helper |
| `export_events` (all modes, `--output-dir` scheduled runs included) | yes | yes (both modes) | stale check → export (receipt appended) → bundle → sidecar |
| `court_export` | verifies and copies a supplied sidecar, or generates one | none | — |
| `handoff_sidecar emit` | after the fact; read-only; never consumes; never overwrites | yes (declared only) | — |
| `witness_api GET /export/bundle` | none (HTTP bytes; no file on the hub) | — | `emit` on the saved bundle |

**Stale-sidecar rule.** Before anything is burned or exported, the emitter moves an existing `<path>.securacv-handoff.json` aside (a hidden `.<name>.stale-<rand>` in the same directory; `NotFound` is fine), then proves a sidecar can be created at that path (create `O_EXCL`, remove). If either fails, it refuses with nothing recorded. The moved-aside sidecar is restored on every failure before the new sidecar exists — a spent or wrong token, a wrong seed, a console denial, a failed export — and deleted once the new disclosure succeeds (for an export, once its bundle replaced the old one). An old sidecar is therefore never left beside new bytes, the sidecar path is known to be free and writable before the burn, and an operator mistake on a re-run cannot crypto-shred the earlier disclosure's salts. (As built; the plan removed the old sidecar outright — see "As built" below.)

**Unseal is sidecar-first and fail-closed.**
- The plaintext salt exists only in this process. A `.raw` without its sidecar would be a pairing that can never be opened.
- So the sidecar is written (create-new, 0600) **before** the `.raw`.
- If the sidecar write fails, the cleartext is zeroized and nothing is written. The stale check and the write probe before the burn catch an obstacle at the path and a directory or filesystem that cannot take the file; the writer opens the final path `O_CREAT|O_EXCL` directly (no hard link, so FAT, exFAT and SMB media work); and a disk too full for a few KiB cannot take the `.raw` either.
- If the `.raw` write fails after the sidecar, the sidecar is removed (best effort) and the error says nothing was released.
- **No salt and no recipient name is ever printed.**

**Exports keep today's order**, except that the handoff sidecar is written right after the bundle and before the optional C2PA sidecar (as built), so a C2PA failure can never skip it or its recovery text. The bundle carries its own signed receipt, so a missing sidecar loses no pairing. On a sidecar failure the bundle is kept and the tool exits 1. The error prints a secret-free `emit` recovery command. If a recipient was named, it says the commitment can no longer be opened.

### D8 — Anchors: referenced by token SHA-256, selected by hash only (changed)
**Shape.** Each entry is:

```json
{"covers":"file_bytes"|"disclosure_record","kind":"rfc3161","subject_hash":"<64 hex>","token_sha256":"<64 hex>"}
```

**Why references.** The sidecar's own bytes carry exactly one time value. The TSA `genTime` stays inside the token, a separate artifact the operator chooses to hand over.

**Selection.** Embedded are the `tsa_anchors` rows whose `subject_hash` equals either:
- the file's SHA-256 (`file_bytes`), or
- the disclosure record's entry hash (`disclosure_record`): the `Unsealed` row, or the export receipt.

Selection is **by hash only**, with **no exclusion of chain-head rows**. A `break_glass_receipt_head` anchor taken right after an unseal has exactly the `Unsealed` row's entry hash as its subject and is referenced as covering the disclosure record. A row is referenced only if `tsa::parse_token_imprint(token_der)` (tsa.rs) equals the hash: trust the token, not the row. No row id or chain ordinal is carried, because an ordinal discloses the count of disclosure acts.

**Resolving tokens.** `verify` resolves tokens from:
- `--db` (`tsa_anchors.token_der`);
- `--anchors-dir <DIR>` (any file whose SHA-256 matches; a court kit's `anchors/` works as is);
- the files `emit --anchors-out <DIR>` writes (`<DIR>/<token_sha256>.der`, create-new).

`--ca <pem>` checks the countersignature through the local `openssl` (`anchor_policy::openssl_ts_verify`, anchor_policy.rs).

**At unseal time** anchors are `[]`, because nothing has anchored a brand-new disclosure yet. `emit` and `court_export` fill them later.

**OpenTimestamps.** `kind: "opentimestamps"` with `proof_sha256` is reserved for PR E. Whichever of D and E merges second selects `ots_anchors` rows by the same hash rule. Until then, an unknown kind reads `UNVERIFIED anchor ({kind}): this build does not check that anchor kind` and never fails.

### D9 — Failure kinds
- **`Unsealed` rules U1–U5** concern the consistency of the break-glass chain. They use the existing `FailureKind::TokenRedemption` (verify.rs). Its doc comment and the `verify_explain` text gain one clause: "…, or an unseal record that names no redemption or repeats one."
- **Export-authorization rules R0–R6** use a **new** `FailureKind::ExportAuthorization` (serde `export_authorization`, appended last) under `FailedLedger::ExportReceipts`.

Neither touches `spec/witness_dictionary.json`: `FailureKind` is not a dictionary vocabulary.

### D10 — Release coupling with #1936 (a merge gate, not an aspiration)
`Unsealed` is a new `BreakGlassOutcome` variant. Every #1936-era Rust reader parses each row strictly with `serde_json::from_str::<BreakGlassReceipt>` (verify.rs; envelope.rs; `break_glass receipts`). Those readers fail with `unknown variant`, and a #1936-era `witnessd` boots into safe mode (boot verify, `src/bin/witnessd.rs`).

A trailing optional field on `TokenRedeemed` would avoid that, because `BreakGlassOutcome` is not `deny_unknown_fields` (core.rs). But it only works with decrypt-before-burn, which is rejected above.

So:
1. **Merge gate.** This change (PR D) merges to main before the first release tag that contains #1936. The PR description and checklist item 1 say so. The release checklist (`docs/RELEASE_BUTTONS.md` / `.github/AGENTS.md` lesson) gets one line: "a release that contains `TokenRedeemed` rows must also contain the `Unsealed` reader."
2. **Upgrade-order note** in the CHANGELOG, `docs/handoff_sidecars.md` and `spec/break_glass.md`: upgrade `witnessd`, `break_glass`, `log_verify`, `envelope_verify`, `court_export` and every observer host **before the first post-PR-D unseal**. This is the same note #1936 carries for `TokenRedeemed`.
3. **Pinning test.** `pre_d_reader_rejects_unsealed_rows` pins the documented break with a local mirror of #1936's enum, so the docs stay true.

The website carry ships in the same release (see Compatibility and migration).

---


## The privacy argument

**Invariant I (no raw export).**
- The sidecar carries no media.
- The `Unsealed` row carries a salted commitment, never bytes.
- Unseal stays quorum-gated: `record_unsealed` refuses anything but an existing, non-export redemption row.
- The burn still precedes any cleartext (#1936 unchanged).

**Invariant II (no identity substrate).**
- The recipient enters no ledger in plaintext. Only `SHA256(domain ‖ fresh 256-bit salt ‖ cjson)` does, per record, so two disclosures to one recipient produce unrelated commitments, and a ledger holder cannot test candidate names.
- The plaintext enters the ledger only salted, with a per-row salt that is never at rest in the database. So the never-pruned, envelope-shipped ledger holds no media fingerprint comparable across long time horizons (`spec/invariants.md`), and even a DB-key holder (a ceremony observer, `break_glass db-key`) cannot confirm a leaked clip.
- A quorum export references its grant instead of copying the requester's identity into a second ledger (D5).
- No identity-inferring capability is added. Nothing is computed from media except a salted hash, and the unsalted `file.sha256`, of the exact bytes being disclosed, which lives in the sidecar beside those bytes.

**Invariant III (metadata minimization).**
- The sidecar owns exactly one time value, `created_bucket` (600 s).
- Embedded payloads carry only 600 s buckets and bucket-aligned windows.
- The `Unsealed` row restates its redemption's bucket, so there is no new clock read.
- Row readers never select `created_at`, row ids or chain ordinals.
- Anchors are referenced by token SHA-256, so no `genTime` enters the sidecar. A token file the operator chooses to hand over carries its own `genTime`: the time of an anchoring act over these exact hashes.
- `handoff`, `handoff_sidecar` and every emitter open no sockets (a source-scan test). `--ca` runs the local `openssl`.

**Invariant IV (local ownership and custody).**
- Every unseal now leaves three device-signed facts: grant, spend and decrypt. Every export receipt can name its consent.
- Sidecars are 0600 and leave the host only when the operator hands them over.

**Invariant VII (non-queryability).** `emit` selects a disclosure only by the exact bundle a caller holds, or by a grant or unseal-record hash. There is no search by name, recipient or time.

**Hygiene.**
- No secret is ever printed: no salt, no recipient name on any error path, no token nonce.
- Every declared field passes `check_text_field`, and every printed field goes through `display_safe` (core.rs).

### Residuals (all go into `docs/handoff_sidecars.md`)
| # | Residual | Bound / mitigation |
|---|---|---|
| 1 | The sidecar holds the recipient's plaintext and both salts; whoever holds it learns the recipient and can open both commitments | 0600; meant for that recipient. Forward it with `handoff_sidecar redact` (`committed_not_disclosed`); `court_export` copies a supplied sidecar verbatim, so redact first to keep the name out of a kit |
| 2 | `--recipient-name` lands in shell history | `--recipient-file` is documented first |
| 3 | A lost unseal sidecar makes the signed pairing permanently unopenable (its salt existed only there) | Never lost at write time (sidecar-first, fail-closed). A sidecar deleted later degrades to `pairing: declared` via `emit`; the commitment stays in the ledger, an honest crypto-shred |
| 4 | `file.sha256` in the sidecar is an unsalted media digest | It sits beside the file it hashes. Docs advise anchoring the **disclosure record** (`log_anchor … --subject break_glass_receipt_head` right after the unseal), not the raw file's digest, which would hand a TSA a media fingerprint |
| 5 | The grant row travels whole: purpose, requester context, trustee ids, approval signatures | A signed row cannot be partially disclosed; spec §5 asks the sidecar to name purpose and approvals. Deployments wanting pseudonymous trustees enroll role ids |
| 6 | Every ledger reader sees that an unseal of envelope E happened, in which bucket | Already true via `TokenRedeemed`; `Unsealed` adds no time and no identity |
| 7 | `device_public_key` is a stable device identifier given to the recipient | Already in every bundle and kit; a disclosure is not telemetry |
| 8 | A recipient commitment binds a declaration; it does not verify who received the file | Rendered "declared … not verified". It stops later substitution only |
| 9 | A device-key holder can forge any row, including `Unsealed`; an `Unsealed` row at the chain tail can be dropped (the row is not required) | The same trust level as every receipt; the anchored `break_glass_receipt_head` sees the drop, and the sidecar holder holds the row (`verify --db` → `FAIL database: record unsealed is not in …`) |
| 10 | Fail-closed costs a trustee ceremony if the append or the sidecar write fails after decrypt | Chosen: no cleartext without its record and its sidecar. The stale check and the write probe before the burn catch the realistic causes (an obstacle at the path, a directory or filesystem that cannot take the file) |
| 11 | A decrypt failure after the burn spends the token (pre-existing in #1936) | Documented; follow-up the follow-ups below |
| 12 | Console unseals take no recipient | `recipient_binding: none`; a later `emit` cannot add one beside a commitment, and adds a `declared` one otherwise |
| 13 | Hub API exports get no sidecar | `handoff_sidecar emit --file <bundle>` |
| 14 | Unseals by #1936-era builds have no `Unsealed` row | `emit --grant` yields `pairing: declared` |
| 15 | The shell recipe checks Ed25519 only, not PQ | `handoff_sidecar verify --db` checks PQ |
| 16 | `created_bucket` of a recovery `emit` reveals when recovery happened | 600 s granularity |
| 17 | A lost export sidecar loses the recipient opening | The bundle still carries its signed receipt; the error says so; `emit` gives `committed_not_disclosed` |

---


## Compatibility and migration
| Mix | Behavior |
|---|---|
| Legacy and PR A receipts | Byte-identical (the generation pin, in Rust and JS). Self-exports without flags, scheduled exports and API/paired bundles are unchanged |
| A pre-PR-D Rust bundle verifier (`verify_export_bundle`, `export_verify`, `court_export`) or JS verifier, given a bundle with `context` / `authorizing_receipt` | **Rejects** (re-serialization drops the fields; entry-hash mismatch), per the documented rule at `spec/evidence_envelope.md`. Every post-PR-D quorum bundle carries `authorizing_receipt`, so it needs a PR D verifier. CHANGELOG upgrade note; the website carry ships in the same release |
| A #1936-era reader on a DB or envelope with an `Unsealed` row | Rust: `unknown variant` in every walk; **#1936-era witnessd boots into safe mode**. JS: counts the row as "denied", silently. **Merge gate D10**: PR D merges before the first release containing #1936. Upgrade order: every reader before the first post-PR-D unseal. Pinned by `pre_d_reader_rejects_unsealed_rows` |
| Pre-#1936 binaries | Already broken by #1936's `TokenRedeemed`; PR D adds no new cliff for them |
| A post-PR-D reader on pre-PR-D data | No `Unsealed` rows ⇒ counts omitted (`None`). Quorum receipts without `authorizing_receipt` ⇒ R-rules skipped. No sidecar ⇒ `court_export` generates one. `emit --grant` gives `pairing: declared`. No DDL change: a new variant in an existing table, and new optional receipt fields |
| MQTT/API `VerifyReport` | Byte-identical until the first unseal record or authorizing receipt exists (`Option`, skipped) |
| Frozen formats | ExportReceipt legacy and PR A bytes; #1936 redemption rows; `securacv-cjson-v1`; signing domains (none added); `tsa_anchors` DDL; court-kit v1 and PR A/B/C/E keys (additive only); `spec/witness_dictionary.json` untouched |
| Committed fixtures | `tests/fixtures/envelope/*` and `export_bundle/valid_bundle*.json` are untouched and keep verifying in both verifiers. A new example `examples/gen_handoff_fixtures.rs` (precedent: gen_export_bundle_fixture.rs never churns its neighbors) writes **only** `tests/fixtures/export_bundle/valid_bundle_self_context.json`, `tests/fixtures/export_bundle/valid_bundle_quorum.json` and `tests/fixtures/envelope/valid_envelope_unsealed.json` (Granted, TokenRedeemed, Unsealed with a recipient commitment, a second grant and redemption for `export:events`, and the quorum export receipt carrying `authorizing_receipt`). Non-PQ build, so WebCrypto can fully check them |
| Website (`securacv_website`) | A **separate website PR in the same release**: add `valid_bundle_quorum.json` to `BUNDLE_FIXTURES` and `valid_envelope_unsealed.json` to `ENVELOPE_FIXTURES` (`scripts/carry_to_site.py`) in **this** PR; run `python3 scripts/carry_to_site.py --site <checkout>`; in the website PR add one `tests/tv-wall.test.mjs` case verifying each new fixture. Until that PR merges the website's copy of `viewer/verify_core.js` and the two new fixtures stay stale: the weekly "Update everything" carry diff shows it, or run `python3 scripts/carry_to_site.py --site <checkout>` and then `git -C <checkout> diff` (`--check` covers only the downloads carry) |
| HA integration | Rebuild the viewer and copy it (the byte-gated copy, `viewer/build.mjs` then the copy step in `docs/GENERATED_FILES.md`); API bundles are unchanged |
| tvOS witness-core, canary-vision envelope bridge, backoffice | Unaffected (verbatim payload strings; no `ExportReceipt` parser) |
| PR C / PR E | Additive in `write_manifest`, `write_custody_record`, `write_verification`, `write_readme`, `VerifyReport`, `FailureKind` and `run_full_verify`. Insertion order in `run_full_verify`: export walk → export authorizations (D) → clock provenance (C) → anchors block (B/E). The sidecar embeds no clock-provenance record (the kit carries those). The OTS anchor kind is added by whichever PR lands second |

---


## Review findings and their resolutions

The design was synthesized from three competing drafts — Design 0
(verifier-first), Design 1 (privacy and consent first, the base) and
Design 2 (operations-first) — reviewed by two independent judges (J1, J2).
Each row names a finding, who raised it, and what the code does about it.
Test numbers refer to the test plan; the tests are listed under "As built".
| # | Finding (judge, design) | Resolution |
|---|---|---|
| 1 | Unsalted raw-media SHA-256 (and length) signed into the never-pruned, envelope-shipped `break_glass_receipts` is a long-horizon comparable hash, which Invariant II forbids (J1 and J2, fatal; D0, D2) | A salted `plaintext_commitment` with a per-row OS-random salt that exists **only in the sidecar**, never at rest in the DB (D2). No length is stored. Test 15 scans the ledger for the unsalted digest. Verified the claim: envelope.rs, spec/evidence_envelope.md, spec/invariants.md |
| 2 | Decrypt-before-burn contradicts #1936's normative burn-first rule (J1 and J2, fatal; D2) | Rejected. #1936's `redeem_token_durably` and its order are untouched. The `Unsealed` row comes **after** decrypt as a separate fail-closed append. The decrypt-after-burn defect stays a documented follow-up (follow-ups, below) |
| 3 | Unknown receipt keys downgrade "compromised" to "inconclusive" (J1, fatal; D0) | Unknown fields only **annotate** the failure text; `status` stays `'compromised'`, `ok` stays false. The no-softening pin is in the JS tests |
| 4 | The envelope verifiers skip the export-authorization cross-check, so an envelope can read valid while `log_verify` fails (J1 and J2, fatal; D2) | R0–R6 run in `run_full_verify`, the Rust envelope walk, `verify_core.js` and `handoff::verify_sidecar`, with byte-identical strings, through one pure function (`check_export_authorizations`) plus its JS mirror. Test 27 and the JS envelope cases |
| 5 | Copying the grant's `OperatorContext` into quorum export receipts gives identity a second unprunable home (J1, fatal; D0, D2) | Reference by `authorizing_receipt`; `operator` is impossible on the authorized path by type; R6 in every verifier (D5); §3.6 spec rewrite |
| 6 | Embedded `token_der_hex` brings a precise TSA genTime into the sidecar (J1, fatal; D0, D1) | Anchors are referenced by `token_sha256` (D8). Tokens are resolved from `--db`, `--anchors-dir` (a kit's `anchors/` works) or `emit --anchors-out`. Test 33 asserts no `token_der*` key. The genTime residual is stated in the Invariant III section above |
| 7 | `VerifyReport` plain `u64` fields change the MQTT bytes (J1; D1) | `break_glass_unsealed` and `export_receipts_authorized` are `Option<u64>`, skipped when `None` (zero). Test 26 |
| 8 | The recipient name and salt are printed on the error path and reach the terminal and journal (J1, fatal; all) | **Nothing secret is ever printed.** Unseal is sidecar-first and fail-closed (the unseal order in `docs/handoff_sidecars.md`); the stale check runs before the burn. Export recovery commands carry no secrets and say the commitment becomes unopenable. Test 42 scans every error for salts and names |
| 9 | The new `Unsealed` variant makes #1936-era readers fail strictly and boots witnessd into safe mode (J2, fatal; D0, D1) | D10: an explicit **merge gate** (before the first release containing #1936), an upgrade-order note in the CHANGELOG, spec, docs and RELEASE_LESSONS, and test 14 pinning the break. The field-on-`TokenRedeemed` alternative (no cliff, since the enums are not `deny_unknown_fields` (core.rs)) was checked and rejected because it needs finding 2's reorder |
| 10 | Blanket exclusion of chain-head anchors drops the natural `break_glass_receipt_head` anchor (J2, fatal; D0) | Selection **by hash only**; a head anchor whose subject is the `Unsealed` entry hash is referenced as `disclosure_record`. Test 39 |
| 11 | The shell recipe needs `xxd`, absent here, so its test always skips (J1 and J2, fatal; D1) | POSIX `hex2bin` (verified under `dash` and `bash` over all 256 bytes) and `basenc` documented. Tests 45–46 run here |
| 12 | `verify` has no countersignature check (J1; D1) | `--ca <pem>`, repeatable, through `anchor_policy::openssl_ts_verify` (anchor_policy.rs) |
| 13 | Broadening `TokenRedemption` to export failures muddies semantics (J1; D1) | New `FailureKind::ExportAuthorization` for R0–R6; `TokenRedemption` keeps break-glass-chain rules (U1–U5) only (D9) |
| 14 | A sidecar-supplied signing domain (J1 best idea; D0) | No `domain` key exists; the verifier and recipe choose the domain by `ledger` |
| 15 | Pinned recipient vector (J1 and J2 recomputed `5d02af2b…598ce`) | Pinned (test 28), plus a new plaintext-commitment vector `926a5596…5da1cb` (test 29), both recomputed during synthesis |
| 16 | Emit selects only by exact digest or hash, with no search (J1 best idea; D0) | Kept and tightened: by bundle, grant hash or unseal-record hash (Invariant VII). There is no digest search at all, because the ledger holds no unsalted digest |
| 17 | Stale-sidecar rule (J1 and J2 best idea; D2) | Adopted for unseal, console and export, **before** any burn or export (D7). Test 49 |
| 18 | Drill writes and verifies a sidecar; canary-local pins; release/container bin lists; HA viewer copy; `export_verify` lines (J1 and J2 best ideas; D2) | All adopted: the drill, the console, the bin lists, `viewer/build.mjs` then the copy step in `docs/GENERATED_FILES.md` (the HA copy is byte-gated, verified at securacv-evidence-viewer.test.js), `export_verify` |
| 19 | Fixture churn: an `--only` flag (J2; D2) | A separate example, `gen_handoff_fixtures.rs`, writes only the three new files (the repo's own precedent); existing generators are untouched |
| 20 | "Source of these fields" CUSTODY row + consent-frame labels (J1 and J2; D1, D2) | Adopted: the consent-frame rendering rule plus the CUSTODY table's "Source of these fields" row |
| 21 | `--recipient-file`, every sidecar 0600 (J2; D1, D2) | Adopted (D1; `RecipientArgs`); `write_sidecar_new` is 0600, O_EXCL, no-clobber, no symlink follow (test 43) |
| 22 | Crash between the `.raw` and the sidecar loses the salts (J2; D1) | Eliminated by sidecar-first ordering; a later deletion degrades honestly to `declared` (residual 3) |
| 23 | RecipientArgs spread over four binaries plus recovery selectors (J2; D1) | Three binaries flatten it (`court_export` takes none). Selectors are `--grant` or `--unseal-record`; `--token` dropped (spent token files are sensitive and add nothing); `--plaintext-salt`/`--recipient-salt` dropped (no salt ever leaves a sidecar); forwarding without the name is `redact` |
| 24 | Hex vs `[u8;32]` inconsistency (J2; D0) | Signed payloads use `[u8; 32]` arrays like every chain hash; the sidecar uses hex; the recipe converts with a pinned jq `hex` function |
| 25 | Pre-existing: decrypt failure after the burn spends the token (J2; D2) | Recorded in the follow-ups below with a proposed read-only pre-burn check; not changed here (it would touch #1936's sequence) |
| 26 | `emitted_by` (J1 praised; D2) | Not adopted: it is declared and checks nothing; `tool_version` is kept |
| 27 | Design 0's lack of `--recipient-file` / 0600-everywhere and Design 1's `--anchors-dir` drop | Both resolved: `--recipient-file` adopted; `--anchors-dir` restored because anchors are references |

---


## Pre-existing findings and follow-ups (not fixed here)
- **Decrypt failure after the burn spends the token** (#1936: cli.rs, backend.rs). Proposed follow-up: before `redeem_token_durably`, run a read-only `Kernel::check_token_redeemable` and a vault pre-check (envelope present, key material loadable, envelope header parses). This keeps burn-first while catching the operational failures, and needs #1936's author's review.
- **The console maps every `authorize_unseal` error to 403 `"denied"`** (http.rs), including failures after a Granted receipt was logged. Proposed: a 500 `{"outcome":"error"}` for post-grant failures, with `breakglass.html` copy to match.
- **`export_events` appends the receipt before the bundle write** (export_events.rs). A failed bundle write leaves a receipt for a file that does not exist, and the message does not say so.
- **A consent-bound recipient** (in the request hash, so trustees approve who receives) changes the request-hash framing (core.rs). It is a follow-up; PR D's recipient is declared and committed.
- **A console recipient input** (an optional `recipient` object in the `/breakglass/unseal` body, committed at release) is a follow-up.

## As built: where the code settled what the plan left open

- **The disclosure record of an unseal with no `Unsealed` row** (an unseal
  by a #1936-era build, recovered with `emit --grant`) is the redemption
  row: anchors `covers: "disclosure_record"` are selected and checked
  against the last embedded record's entry hash.
- **Rule order.** `check_export_authorizations` applies R0, R1 and R6 (the
  structural rules, shared with `verify_export_bundle`) before R2, R3, R4
  and R5; the JS mirror applies them in the same order with the same
  strings. The JS structural check reports R0 with the Rust string
  (`export receipt's authorizing_receipt is not a 32-byte array`).
- **U5 in the database walk.** For an `Unsealed` row the database walk
  applies U1–U5 before the per-receipt quorum and attribution check, so a
  forged row naming trustees fails as U5 (`token_redemption`) in every walk,
  with the same reason. Granted, Denied and #1936's `TokenRedeemed` rows keep
  their order.
- **The stale-sidecar rule is reversible.** The plan removed a stale
  sidecar before the burn; a re-run with a spent or wrong token then
  destroyed the earlier disclosure's only copy of its salts. As built, the
  emitters move it aside and restore it on every failure (D7), and probe the
  sidecar path before anything is spent. A process killed in between leaves
  the hidden `.<name>.stale-<rand>` file in place, salts intact.
- **The writer.** `write_sidecar_new` opens the final path with
  `O_CREAT|O_EXCL` at 0600 (never clobbers, never follows a symlink,
  dangling or not) and removes a partial file on a failed write; the plan's
  temp file plus `hard_link` failed with `EPERM` on FAT, exFAT and SMB media
  after the burn. Its overwrite error is neutral (`a file already exists at
  <path>; a handoff sidecar is never written over another file`);
  `handoff_sidecar emit` and `redact`, which take `--out`, keep the brief's
  `--out <PATH>` refusal.
- **`export_events` writes the handoff sidecar before the C2PA sidecar**
  (the plan put C2PA first): a C2PA failure returned before the handoff
  sidecar and its ERROR/recovery text, losing a named recipient's salt
  silently.
- **The cleartext is `Zeroizing` from the decrypt**, and the sidecar's
  `created_bucket` is read before the burn at all three unseal sites, so no
  fallible step sits between the decrypt and the fail-closed release.
- **Export grants never describe an unseal.** `emit --grant` refuses an
  `export:events` grant (`an export:events redemption is not an unseal —
  exports are receipted in the export chain (emit the export's handoff
  sidecar with --file <bundle>)`), and `verify` FAILs `links: an unseal
  sidecar embeds the redemption of an export:events token` even with no
  `Unsealed` row, where the walk's own U3 never fires.
- **A kit carries its bundle's export sidecar only.** `court_export`
  refuses an unseal sidecar beside a bundle as a custody break, and writes
  MANIFEST `handoff.pairing` from the sidecar.
- **An empty `ExportContext` is normalized** at
  `export_events_self_with`: the kernel never signs `"context":{}`.
- **R1 is display-safe.** The R1 detail runs a forged `auth_mode` through
  `display_safe` (JS `displaySafe`), and names a non-string mode `a
  non-string value` (serde and `JSON.stringify` render numbers
  differently). The JS ledger walk reads R0 from the signed payload text,
  so a float-encoded `authorizing_receipt` (`4.0`, `4e0`, `-0`) fails R0 in
  both verifiers.
- **Counts on a failed report.** `VerifyReport.break_glass_unsealed` and
  `export_receipts_authorized` are `None` when zero and on a failed report,
  like `clock_provenance_verified`.
- **The export sidecar-failure seam.** `export_events` plants an obstacle
  at the sidecar path when `SECURACV_TEST_HANDOFF_OBSTACLE` is set, in debug
  builds only (`#[cfg(debug_assertions)]`), so the failure path is tested
  end to end; a release build has no such seam.
- **Exit codes.** `handoff_sidecar` exits 2 for a refusal raised before any
  check (overwrite, a recipient beside a committing row, an unopenable
  database), 1 when a check FAILs, 0 otherwise.
- **Tests** live in `src/disclosure_tests.rs` (the `Unsealed` row, the
  receipt generations, `record_unsealed`, U1–U5, R0–R6, the reports),
  `src/handoff_tests.rs` (vectors, canonical form, Invariant III, verify
  lines, anchors, the fail-closed release), `tests/handoff_sidecar_cli.rs`,
  `tests/handoff_sidecar_shell.rs`, `tests/court_export.rs`, the fixture
  pins and `viewer/verify_core.test.js`.
