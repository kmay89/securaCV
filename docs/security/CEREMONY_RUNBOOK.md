# Ceremony runbook — custody and break-glass over the shipped commands

Status: Operational runbook v0.1 (2026-09-06)
Scope: procedure over **shipped** tooling — `break_glass`, `break_glass_serve`, `log_anchor`, `log_verify`, and the `SECURACV_VAULT_PASSPHRASE` / `SECURACV_HWM_PATH` environment.
Design source: [`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) §3 (ceremony), §4 (anchoring spine), §2 (threshold custody — design).
Tracker: [`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md). Companion: [`CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md`](CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md) — what a deployment declares; this file is how a declared ceremony is actually run.

The lesson SecuraCV takes from the root-zone KSK ceremonies
([`PROVENANCE_INTEROP.md`](PROVENANCE_INTEROP.md) §1.4) is that credibility
does not come from the mathematics. It comes from a script published before
the room fills, a witness whose only job is to record deviations, an artifact
for every step, and an archive that outlives the people. Some of that is code
in this repository today; the rest is paper. This runbook never blurs the two.

## Control-status badges

Every control statement below carries exactly one badge. There is no unbadged
"must" in this document.

- **CODE** — enforced by the tool today; the badge names the command or the
  refusal that enforces it.
- **PROC** — a human rule today; the badge names the spec section that will
  replace it with an enforced mechanism.
- **GAP** — no tooling exists; the runbook says how to live without it and
  the tracker item that covers it.

## 0. What this is, and what it is not

This is the operator's script for eight ceremonies over one kernel database,
one vault directory, and one trustee roster. It is not a threat model
([`../../spec/threat_model.md`](../../spec/threat_model.md)), not the protocol
specification ([`../../spec/break_glass.md`](../../spec/break_glass.md)), and
not a compliance claim (no ceremony here makes anything "admissible" or
"certified").

Three facts frame every step, stated once so no step has to hedge:

1. **The quorum is an authorization gate, not the cryptographic lock.** The
   vault master key is a single device-local secret — plaintext `master.key`,
   or `master.keyguard` wrapped under one custodian's passphrase. Possession
   of the vault directory (plus the passphrase, in keyguard mode) decrypts.
   What the quorum governs is who may *authorize* a disclosure, and what it
   guarantees is that every attempt leaves a signed, hash-chained record.
   Threshold custody of the key itself is design (`quorum_unseal_v2.md` §2;
   `ENTERPRISE_CUSTODY.md` §1).
2. **Host compromise is out of scope.** Every guarantee holds within the
   host-trust boundary. A host-level actor can rewrite rows; the promise is
   that such a rewrite leaves no valid record — tamper-evident, never
   tamper-proof.
3. **No automated agent constructs, transmits, or approves any artifact in
   these ceremonies.** An assistant may draft the script or explain a
   refusal; consent is human and non-delegable. **PROC** — this rule is not
   enforced by code (`quorum_unseal_v2.md` §1, §3.3).

## 1. Roles

The KSK ceremony roles, mapped onto ours. "Trustee" is the SecuraCV word;
"crypto officer" appears only in this table.

| KSK role | SecuraCV role | Holds | Runs | May not | Status |
|---|---|---|---|---|---|
| Ceremony Administrator | **Operator** | access to `DEVICE_KEY_SEED` on the host | `init`, `trustee enroll`, `policy propose` / `set`, `request`, `authorize`, `unseal`, `receipts`, `doctor`, `log_anchor`, `log_verify` | hold a trustee signing key in the same ceremony | **PROC** — nothing stops one person holding both; the record must say so |
| Crypto Officer | **Trustee** (one of *m*) | one Ed25519 signing-key file (32-byte hex seed, mode 0600) on their own machine | `approve --request`, `policy approve --proposal` | sign anything their own tool did not display | **CODE** — `approve --request` recomputes the hash from the file's fields before signing; `--request-hash` is labeled blind signing |
| Requester of the act | **Requester** | nothing cryptographic | supplies `--requested-by`, `--reason`, `--case-ref`, `--requester-key` | — | **CODE** — consent-bound and recorded in the receipt; the requester key is recorded, not checked against a registry. The requester is often the operator; write that down |
| Internal Witness | **Ceremony witness** — a person who is neither trustee nor operator | the exceptions register | nothing on the kernel | approve, operate | **PROC** → §3.4 witness co-signature. No kernel principal exists for this role |
| Safe Security Controller | **Device-key custodian**; in keyguard mode also the **Vault-passphrase custodian** | where `DEVICE_KEY_SEED` lives; `SECURACV_VAULT_PASSPHRASE` (environment or secrets manager, never on disk) | sets the environment for the operator's commands | — | **CODE** for the on-disk part (the keyguard never writes the plaintext key); custody itself **PROC** |
| Recovery Key Share Holder | **none** | — | — | — | **GAP** → §2.3 recovery quorum. If fewer than *n* current trustees remain, there is no in-band recovery |
| External witness / auditor | **Observer** | read-only tools | `receipts`, `policy history`, `log_verify`, `log_anchor verify` | change anything | **CODE** — these commands write nothing (except `log_anchor`, which creates its table if missing) |

Vocabulary: a *ceremony witness* is a person. The *witness kernel* and a
*tlog witness* are software. Beacon/Chirp co-signing is unrelated.

**The consumer-tier collapse.** In a household deployment one person is often
operator, a trustee, and the device-key custodian. Nothing in the code
prevents it. What is lost is separation, not integrity: every record is still
signed and chained. Write the overlap into the ceremony record and the CPS
(§1.3) rather than pretend otherwise.

## 2. Conventions shared by every ceremony

- **Time is bucketed** (**CODE**). Requests, approvals, and policy proposals
  live in 10-minute buckets. Approvals redeem only in the bucket the request
  was opened in; `policy set --approvals` must land in the same bucket as
  `policy propose`. Schedule so every signer is live in one window, or use the
  served console. Confirm the host clock (NTP) before starting (**PROC**).
- **Ceremony record directory** (**PROC** → §3.4 transcript). On
  operator-controlled media, never in the vault directory:
  `ceremonies/<YYYY-MM-DD>-<type>-<seq>/` containing `SCRIPT.md` (the verbatim
  copy of the ceremony section being run), `SCRIPT.sha256`, `ATTENDANCE.md`,
  `BUILD.md` (binary SHA-256, git commit, enabled features such as `tsa`,
  `api-tls`, `pqc-signatures`), `EXCEPTIONS.md`, `CLOSEOUT.md`,
  `MANIFEST.sha256`, plus every file artifact a step names.
- **Script hash-commitment** (**PROC** → §3.4). Hash `SCRIPT.md` before any
  approval is gathered and send the hash to every trustee over the
  precommitted channel. Interim binding, labeled a workaround: put
  `script=sha256:<hex>` inside the request's `--purpose` text so it rides
  inside the hash the trustees see and sign. The code binds the purpose
  (**CODE**); the convention is **PROC**; this is not the §3.4 mechanism.
- **Channels** (**PROC** → §3.3). The channel list is fixed at issuance and
  written into the CPS. Request-context files travel over it. A call may
  *initiate* a request; consent never travels by voice or video, and face or
  voice never identify anyone.
- **Exceptions** (**PROC** → §3.4 exception receipts). Numbered entries in
  `EXCEPTIONS.md` (`E-01`, `E-02`, …): step, what deviated, decision
  (continue / abort), who decided, witness initials. A tool refusal is **not**
  an exception — it is a control working. Log it as the step's outcome and
  restart the step.
- **Witness statement** (**PROC** → §3.4). `CLOSEOUT.md` ends with
  "observed, exceptions as listed" and the witness's signature — a scanned
  handwritten signature or a detached signature from a key the witness
  controls, kept outside the kernel.
- **Anchoring at close** (**CODE** primitives, **PROC** composition): §6.
- **Retention**: §7. Each trustee keeps their own approval files; the
  `log_verify` guidance to "compare against trustees' own records" depends on
  it.
- **Privacy of the record.** Names, reasons, and case references live only
  inside signed receipts and the ceremony record (Invariants II/III). Never in
  file names that leave the deployment, never in anchors (which carry only a
  hash). Consumer deployments may use pseudonymous trustee ids.
- **Every command here runs with `--ui plain`** so stage markers are
  deterministic lines (`==> <stage>` / `✔ <stage>`) rather than a spinner, and
  the transcript can be captured with `2>&1 | tee`.

## 3. Pre-ceremony checklist

Common to every ceremony; each ceremony adds its own preconditions.

| Check | Command | Expect | Status |
|---|---|---|---|
| Vault and policy health | `break_glass doctor --db witness.db --vault-path vault/envelopes --device-key-seed "$DEVICE_KEY_SEED"` | exit 0; capture output (the plaintext-`master.key` warning is expected unless C6 was run) | **CODE** |
| Ledger verdict | `log_verify --db witness.db --public-key-file device.pub [--high-water-mark hwm.bin]` | verdict `valid` — not `self-consistent; identity unverified` | **CODE** labels; distributing `device.pub` out of band is **PROC** |
| Roster matches the CPS | `break_glass policy show --db witness.db --device-key-seed "$DEVICE_KEY_SEED"` | ids and fingerprints match CPS App. C | **CODE** prints; the comparison is **PROC** |
| History intact | `break_glass policy history --db witness.db --device-key-seed "$DEVICE_KEY_SEED"` | `History chain VALID (<k> entries).` | **CODE** |
| Anchors | `log_anchor --db witness.db --device-key-seed "$DEVICE_KEY_SEED" verify --ca tsa-ca.pem` | no anchor `UNVERIFIED`; note the newest anchor's age | **CODE** |
| Receipt baseline | `break_glass receipts --db witness.db --device-key-seed "$DEVICE_KEY_SEED"` | `Total: <k>`, no INVALID; record *k* | **CODE** |
| Room | attendance, roles, channel check, script hash distributed, clock check, `BUILD.md` written | — | **PROC** |

## 4. Ceremony catalog

Each ceremony follows the same skeleton: purpose and trigger; roles present;
preconditions; numbered steps, each naming the artifact it produces and who
holds it; ceremony-specific exceptions; close-out; retention; and a controls
table.

### C1 — Quorum issuance (bootstrap)

**Purpose / trigger.** The first roster on a database that has no policy.
Bootstrap is device-key-only and the history row says so (**CODE** — the row
is labeled `bootstrap`).

**Roles present.** Operator, all *m* trustees (in person or on the
precommitted channel), ceremony witness, device-key custodian.

**Preconditions.** `policy show` reports no policy. Each trustee's key custody
decided in advance (see step 2).

**Steps.**

1. Operator opens the draft:
   ```sh
   break_glass --ui plain init --threshold 2 --trustees 3 \
     --db witness.db --device-key-seed "$DEVICE_KEY_SEED"
   ```
   → `witness.db.setup-draft.json` (public keys and counts only; idempotent —
   an existing draft is reported, never clobbered) and the pinned device
   identity, printed as `✓ device identity pinned: <fingerprint>`
   (**CODE**). Copy the fingerprint into `ATTENDANCE.md`: every later receipt
   is signed under this identity.
2. Per trustee, one of two key paths, chosen in advance and recorded:
   - **Trustee-generated (preferred).** The trustee mints a 32-byte hex seed
     on their own machine (mode 0600) and sends only the public half.
     Operator: `break_glass trustee enroll --id alice --public-key <hex> --db witness.db --device-key-seed "$DEVICE_KEY_SEED"`.
     **GAP**: there is no `trustee keygen` / public-key-print command; the
     trustee derives the public hex with an external Ed25519 tool.
   - **Operator-minted.** `break_glass trustee enroll --id bob --generate --output bob.key --db witness.db --device-key-seed "$DEVICE_KEY_SEED"`
     → `bob.key` (hex seed, mode 0600, written symlink-safe — **CODE**),
     handed to Bob in person; the operator's copy is securely deleted
     (**PROC**). Honest note for the record: the operator transiently held
     Bob's secret.
   A reused public key is refused (**CODE**).
3. Read-back. Each trustee confirms their own public-key fingerprint from the
   draft over the precommitted channel (**PROC** → §3.7 enrollment
   attestation). Verification and duress phrases, if the deployment uses
   them, are registered in the ceremony record only (**PROC** → §3.3; they
   are not in the policy today).
4. On the *m*-th enrollment the policy commits as history row 1 (`bootstrap`)
   and the tool prints `✓ quorum policy is live: 2-of-3` (**CODE** — only a
   complete roster commits; earlier enrollments edit the draft only).
   Artifacts: `policy history` and `policy show` output, captured.
5. `break_glass doctor …` → captured output.
6. `break_glass drill --threshold 2 --trustees 3` → `DRILL PASSED` (sandbox;
   proves the build, not the roster — see C4).
7. Publish the device public key and its randomart trust card to relying
   parties out of band (**PROC**). This is what makes their `log_verify`
   verdict `valid` rather than `self-consistent; identity unverified`.

**Exceptions.** Draft already complete → re-run `init` (it reports state).
Key-reuse refusal → not an exception; choose another key. A trustee who cannot
confirm their fingerprint → do not enroll them (E-entry).

**Close-out.** §6. Note that the policy-history chain has no printed head
hash today (**GAP**): anchor the sealed-log chain head and record the history
row count.

**Retain.** The draft (public data), the key-custody record (never the keys),
`policy history` and `policy show` captures, the drill output.

| Control | Status |
|---|---|
| Complete-roster commit | **CODE** |
| Bootstrap labeling in history | **CODE** |
| Distinct trustee keys | **CODE** |
| Trustee key at rest is a plaintext file | **GAP** → §3.7 (wrapping, hardware keys) |
| Verification / duress phrases | **PROC** → §3.3 |
| Enrollment attestation | **PROC** → §3.7 |

### C2 — Roster, threshold, or crypto-mode change

Also covers trustee key rotation and trustee removal: there is no
`policy revoke-trustee`; removal *is* revocation today (§2.4 resharing is
design).

**Purpose / trigger.** Any change to a live policy: add or remove a trustee,
replace a trustee's key, raise or lower *n*, change `vault.crypto_mode`.
The mode is never silently downgraded — `policy set` defaults to the stored
mode (**CODE**).

**Roles present.** Operator, at least *n* **current** trustees, witness; the
incoming trustee (if any) for read-back.

**Steps.**

1. Operator writes the proposal (full current and proposed policies):
   ```sh
   break_glass --ui plain policy propose --threshold 2 \
     --trustee alice:<hex> --trustee bob:<hex> --trustee dana:<hex> \
     --output change.proposal --db witness.db --device-key-seed "$DEVICE_KEY_SEED"
   ```
   → `change.proposal` (**CODE**).
2. Each current trustee, on their own machine, offline:
   ```sh
   break_glass --ui plain policy approve --proposal change.proposal \
     --trustee alice --signing-key alice.key --output alice.policy-approval
   ```
   The tool recomputes the change hash from the proposal's full contents and
   displays the diff; a tampered proposal is refused, not signed; the
   signature domain is disjoint from unlock approvals (**CODE**).
3. Operator, in the **same 10-minute bucket** as the proposal:
   ```sh
   break_glass --ui plain policy set --threshold 2 \
     --trustee alice:<hex> --trustee bob:<hex> --trustee dana:<hex> \
     --approvals alice.policy-approval,bob.policy-approval \
     --db witness.db --device-key-seed "$DEVICE_KEY_SEED"
   ```
   → history row `change` carrying previous and new policy plus the
   approvals. The prior-era commitment is recomputed from the **stored**
   policy, never taken from the file (**CODE**). Fewer than *n* valid current
   approvals → `policy change denied` (**CODE**).
4. `policy history` → `History chain VALID`; `policy show`; `doctor` —
   captured.

**Exceptions.** Bucket rolled over → re-propose (approvals cannot be banked;
**CODE**). Fewer than *n* current trustees alive → **no in-band path**
(**GAP** → §2.3 / §2.4); a host-level row rewrite would leave no valid history
record and is an incident, not a procedure. A cooling-off period before
applying a roster change is **PROC** → §3.5.

**Retain.** The proposal, every policy-approval file (each trustee keeps
theirs), the history capture.

### C3 — Unseal (break-glass)

Two profiles: **standard**, and **high-stakes**, which adds the witness, the
script hash, and a procedural cooling-off.

**Roles present.** Requester, operator, at least *n* trustees, witness
(high-stakes), and the named recipient of the cleartext.

**Steps (CLI).**

1. Operator opens the request with the operator context
   (`spec/break_glass.md` "Request creation"):
   ```sh
   break_glass --ui plain request --envelope cam-porch-0007 \
     --purpose "insurance claim, porch camera, sealed 2026-07-19" \
     --requested-by "Alice Operator" --reason legal-request \
     --case-ref CLM-2026-0042 --ruleset-id ruleset:v0.3.0 \
     --output-request unlock.request
   ```
   → `unlock.request` (format `securacv-unlock-request:v2`). Text fields are
   capped at 512 bytes and may not carry control or text-direction
   characters (**CODE**). Reason codes name the process a disclosure serves
   (`incident-review`, `legal-request`, `legal-hold`, `owner-recovery`,
   `safety-check`, `maintenance-audit`, `drill`), never what the evidence
   will show (**CODE** — the vocabulary is closed). Note that `request`
   accepts `--db` but does not consult the roster; a roster mismatch
   surfaces only at `authorize` (**GAP**).
2. Send the context **file** to each trustee over the precommitted channel
   (**PROC**). Never a bare hash.
3. Each trustee, on their own machine:
   ```sh
   break_glass --ui plain approve --request unlock.request \
     --trustee alice --signing-key alice.key --output alice.approval
   ```
   The tool recomputes the request hash from the file's fields and displays
   every field before signing (**CODE**). `--request-hash` prints a
   blind-signing warning: any use is an E-entry, and in the high-stakes
   profile an abort (**PROC**).
4. High-stakes only — cooling-off (**PROC** → §3.5). The spec's `not_before`
   delay cannot be honored between authorize and unseal today, because
   approvals expire with their bucket (**CODE**). So the procedural
   cooling-off happens **before step 1**: announce the intended request to
   all *m* trustees, wait the declared period, treat any trustee's objection
   on the channel as a veto, and only then open the request.
5. Operator authorizes from the file, which reproduces the consented hash
   exactly:
   ```sh
   break_glass --ui plain authorize --request unlock.request \
     --approvals alice.approval,bob.approval \
     --db witness.db --device-key-seed "$DEVICE_KEY_SEED" \
     --output-token unlock.token
   ```
   → a single-use token file (mode 0600) and a receipt row — granted **or**
   denied, both chained — recording purpose, operator context,
   `policy_commitment`, and the request's own bucket (**CODE**). A rolled-over
   window is denied *with* a receipt (**CODE**).
6. Operator unseals:
   ```sh
   break_glass --ui plain unseal --envelope cam-porch-0007 --token unlock.token \
     --db witness.db --device-key-seed "$DEVICE_KEY_SEED" \
     --vault-path vault/envelopes --output-dir vault/unsealed
   ```
   → cleartext in `--output-dir` with restricted permissions; the token
   nonce is durably consumed before any cleartext exists, so a replay is
   refused (**CODE**). At the gate the receipt's recorded context must
   re-derive the consented hash and its named trustees must match the valid
   approvals (**CODE**).
7. Audit:
   ```sh
   break_glass --ui plain receipts --db witness.db \
     --device-key-seed "$DEVICE_KEY_SEED" --verbose
   ```
   → per receipt: `prev_hash`, `entry_hash`, signature, and the deterministic
   human-readable record (printed name, UTC bucket, meaning of each
   signature) (**CODE**). Capture it into the ceremony record.
8. Disclosure handoff (**PROC** → §5 sidecars, **GAP**). Write into
   `CLOSEOUT.md`: the SHA-256 of every released file, the recipient, the
   purpose, and the receipt's `entry_hash`. `court_export` packages
   event-export bundles, not unseal outputs (**GAP**).

**Served-console variant** (`break_glass_serve --addr 127.0.0.1:8800 --db witness.db --device-key-seed "$DEVICE_KEY_SEED" --vault-path vault/envelopes --output-dir vault/unsealed --token-path serve.token`).
The operator connects with the capability token from `--token-path`; the
server computes the request hash over every field including the operator
context (**CODE**); the trustee signing link carries every field the hash
binds, and the signer page recomputes the hash in the browser and enables
Sign only on a match — a link whose fields do not produce its hash is refused,
and so is a link that carries only the hash (that trustee falls back to
`approve --request`) (**CODE**). The link carries the request fields in clear
text and persists in browser history, so it travels over the trustees'
precommitted channel, not a shared chat (**PROC**). Binding a non-loopback
address requires `--tls-cert` / `--tls-key` (**CODE**). Record that the
trustee pasted a key seed into a browser tab; offline trustees use the CLI. The console writes no per-request
log — the durable trace is the receipt row and the consumed-token row, so the
transcript must capture the HTTP replies itself (**GAP**).

**Exceptions.** A denied receipt is evidence; record it, never delete it.
Expired window → new request. Context-mismatch refusal → not an exception
(control working); investigate the relay. Duress phrase heard (**PROC**) →
abort and open an incident. The emergency profile (higher threshold, distinct
label) is **PROC** → §3.5: today, label it in `--purpose` and in the record.

**Retain.** The request file, every approval, the `receipts --verbose`
capture, the handoff record. The token file: record its SHA-256, then
securely delete it — it is sensitive and useless once consumed.

| Control | Status |
|---|---|
| Sign what you see (recompute + display before signing) | **CODE** |
| Consent-bound operator context, recorded in the receipt | **CODE** |
| Single-use token, bucket-bounded redemption | **CODE** |
| Receipt for every attempt, granted or denied | **CODE** |
| Truthful trustee attribution in the record | **CODE** |
| Precommitted channels | **PROC** → §3.3 |
| Cooling-off, notify-all, veto, emergency profile | **PROC** → §3.5 |
| Handoff sidecars for unseal outputs | **GAP** → §5 |

### C4 — Rehearsal

Two different things; never conflate them.

- **C4a — Sandbox drill.** `break_glass drill --threshold 2 --trustees 3`
  → output ending `DRILL PASSED`, exit 0 (**CODE**). Temporary database,
  temporary vault, ephemeral keys; it touches nothing real and leaves **no**
  receipt in the real ledger. It proves the build and host can execute
  break-glass end to end. With `SECURACV_VAULT_PASSPHRASE` set it exercises
  the keyguard path. Retain the captured output with `BUILD.md`.
- **C4b — Recorded rehearsal.** A full C3 against a designated rehearsal
  envelope, with `--reason drill` (**CODE** — a real reason code), the real
  roster, and the real channels → a real receipt in the real chain, labeled
  by its reason code. This is the shipped stand-in for §2.4 liveness
  attestations (**GAP**) and the only rehearsal that exercises trustee
  custody.

Cadence (**PROC**): C4a on every build change; C4b quarterly and after every
C2. The Lab's Operator's Bench is training material with recorded output; it
is never evidence.

### C5 — Anchoring (RFC 3161)

**Subjects** (**CODE**): `chain_head` (default — the newest sealed-event
entry hash) and `digest` (`--digest <hex>`: an export envelope's digest, or a
receipt head taken from the last `entry_hash` printed by
`receipts --verbose`).

**Steps, online** (a build with `--features tsa`):
```sh
log_anchor --db witness.db --device-key-seed "$DEVICE_KEY_SEED" request --url https://freetsa.org/tsr
log_anchor --db witness.db --device-key-seed "$DEVICE_KEY_SEED" request --url <second, independent TSA>
```
One of the two should be eIDAS-qualified (**PROC**; see `PROVENANCE_INTEROP.md`
§1.1).

**Steps, offline / air-gapped:**
```sh
log_anchor --db witness.db --device-key-seed "$DEVICE_KEY_SEED" query --out chain.tsq
# move chain.tsq to a connected machine, submit it, bring chain.tsr back
log_anchor --db witness.db --device-key-seed "$DEVICE_KEY_SEED" import --response chain.tsr --url <TSA>
```
`import` correlates the response's imprint against chain history before
storing it (**CODE**).

**Check:** `log_anchor … list`, then `log_anchor … verify --ca tsa-ca.pem` →
each anchor OK or `UNVERIFIED` (**CODE** — without `--ca` nothing is
cryptographically verified and the tool says so).

**Honest scope.** Chain membership is checked for `chain_head` anchors; a
receipt-head `digest` anchor is imprint-checked and openssl-verifiable but is
not cross-bound into `run_full_verify` (**GAP** → §4; `ENTERPRISE_CUSTODY.md`
§2). The anchors table is not itself chained: export `tsa_anchors.token_der`
with every backup (**PROC**). An OpenTimestamps leg and clock-provenance
events are **GAP** (§4). Anchor on a fixed schedule, never in reaction to
incidents, so timing carries no signal (**PROC**).

### C6 — Keyguard issuance (vault passphrase)

**When.** A **new** vault directory, before the first seal. There is no
migration tool for an existing plaintext `master.key` vault (**GAP**), and the
tool refuses to run in passphrase mode beside a plaintext key rather than
guess (**CODE**).

**Steps.**

1. The vault-passphrase custodian generates the passphrase and never writes
   it to disk (**PROC**).
2. Set `SECURACV_VAULT_PASSPHRASE` in the environment of `witnessd` and of
   every vault-touching tool (`break_glass unseal`, `break_glass_serve`,
   `doctor`).
3. The first open writes `master.keyguard` — an MKG1 container, Argon2id-wrapped,
   bound to the canonical vault path — and never writes a plaintext
   `master.key` (**CODE**). This is a deterrent against casual copying of the
   directory, not an anti-exfiltration control.
4. `break_glass doctor …` → capture. **GAP**: `doctor` reports only
   `master.key` today and does not recognize `master.keyguard`; in passphrase
   mode expect it to describe the vault as not initialized. Record the
   keyguard file's presence and SHA-256 manually.
5. A keyguard vault opened without the passphrase is refused (**CODE**).

**Custody record** (**PROC**). Who holds the passphrase, where (secrets
manager, TPM-backed store), and the escrow arrangement. A procedural split
("two people each hold half") is a paper split, not verifiable secret
sharing — say so. **Loss of the passphrase is permanent loss of every
envelope sealed under it.** Passphrase rotation: **GAP**.

Honesty line for the CPS: single-custodian confidentiality, quorum-gated
authorization (`quorum_unseal_v2.md` §2.5).

### C7 — Key rotation

Four different keys; the ceremony depends on which.

- **Trustee key** → C2 (replace the roster entry).
- **Device signing identity** → `Kernel::rotate_device_identity(new_seed)` —
  a library API with no operator command (**GAP**). Prerequisite:
  `SECURACV_DB_KEY_SEED` set as an independent secret so the database key does
  not change with the device key, and `rekey_database_file` (also
  library-only) if it must. A rotation produces a `KeyRotation` chain record
  signed by the retiring key plus a `device_key_history` row; verification
  follows the genesis-anchored lineage (**CODE**). Post-rotation duties
  (**PROC**): redistribute the pin and lineage to every relying party; note
  that post-quantum keys are not rotated and receipts keep verifying under
  the lineage.
- **Database encryption key** → `rekey_database_file` with the database
  closed (library-only; **GAP**). Stop every process first.
- **Vault master key / keyguard passphrase** → no rotation path (**GAP**).

Not ceremonies: capability tokens for `break_glass_serve` and `witness_api`
rotate per bucket automatically.

### C8 — Verification baseline

- **Out-of-band key pin.** Deliver the genesis public key
  (`device_metadata.public_key`) and its randomart card to every verifier
  (**PROC**); their `log_verify --public-key-file device.pub` then reports
  `valid` (**CODE** label). Without it the honest verdict is
  `self-consistent; identity unverified`.
- **High-water-mark.** Set `SECURACV_HWM_PATH` to append-only or external
  media; the kernel writes a device-signed, monotonic `(seq, head)` mark
  (`SCVHWM01`) as the sealed log grows (**CODE**).
  `log_verify --high-water-mark <path>` fails closed on truncation, rollback,
  or a wipe (**CODE**). Residuals to record in the CPS: a mark stored beside
  the database rolls back in lockstep with it; the writer is best-effort; a
  device-key holder can forge both; the mark advances on sealed-event appends
  only, so receipt-only activity during a ceremony does not move it.

## 5. Exception handling

- **Class A — tool refusal (fail-closed).** Tampered proposal or request file,
  expired bucket, keyguard beside a plaintext key, reused trustee key,
  unknown reason code, partial operator context. Not an exception: log the
  outcome, restart the named step.
- **Class B — procedural deviation.** Wrong channel, blind-signing use,
  witness absent, script hash not distributed. E-entry; continue or abort per
  the ceremony (blind signing → abort in the high-stakes profile).
- **Class C — environmental.** TSA unreachable → offline flow (C5); clock
  skew → wait for the next bucket; host reboot → re-run the pre-ceremony
  checklist.
- **Class D — security-relevant.** Duress phrase, an unexpected receipt or
  history row, INVALID anywhere, a verdict other than `valid`, an
  `UNVERIFIED` anchor at close → abort, open an incident, no override.
- Exception receipts as chain entries: **GAP** → §3.4.

## 6. Close-out

1. Receipt audit: `receipts --verbose`, `policy history`, and
   `log_verify [--high-water-mark …] --public-key-file …`, captured; counts
   before and after the ceremony.
2. Anchor (C5): the sealed-log chain head, and the newest receipt
   `entry_hash` as a `digest` anchor; then `log_anchor verify --ca` — nothing
   `UNVERIFIED`.
3. `CLOSEOUT.md`: participants, script hash, every artifact with its SHA-256
   (`MANIFEST.sha256`), the exceptions summary, the witness statement, anchor
   ids and TSA `genTime`.
4. Sanitized transcript (**PROC** → §3.4): redact names and case references
   from anything that leaves the deployment.
5. Distribute: each trustee keeps their own files; an observer receives the
   sanitized transcript.

## 7. Retention

| Artifact | Produced by | Holder | Keep | The question it answers |
|---|---|---|---|---|
| Setup draft (`<db>.setup-draft.json`) | `init` / `trustee enroll` | operator | life of the deployment | which public keys were enrolled, in what order |
| Trustee public keys and fingerprints | draft, `policy show` | operator, CPS App. C | life of the deployment | who the roster was |
| Proposal and policy-approval files | C2 | operator; each trustee keeps theirs | life of the deployment | who consented to each roster change |
| Request file and approval files | C3 | operator; each trustee keeps theirs | retention window + legal hold | who consented to each disclosure, and to exactly what |
| Token file | `authorize` | nobody — record its SHA-256, then delete | — | that a token existed and was consumed |
| Receipts | kernel database (authoritative); `receipts --verbose` capture | operator; capture in the record | life of the database; captures per legal hold | every attempt, its outcome, its context |
| Policy history | kernel database; `policy history` capture | operator | life of the database | the roster's lineage |
| Anchor tokens (`tsa_anchors.token_der`, `.tsq`, `.tsr`) | `log_anchor` | operator; exported with every backup | life of the deployment | that a state existed no later than a third party's time |
| High-water-mark copies | kernel (`SECURACV_HWM_PATH`) | operator, external media | rolling; snapshots at each ceremony | that the log was never shorter than this |
| `doctor`, `log_verify`, drill outputs | the tools | ceremony record | per ceremony | the state of the system on the day |
| `SCRIPT.md` + hash, `EXCEPTIONS.md`, `CLOSEOUT.md` + witness signature, `BUILD.md` | the ceremony | ceremony record | life of the deployment | what was supposed to happen, what did, who saw it |
| The CPS version in force (with its SHA-256) | the deployment | record store | every version, forever | what the deployment had declared at the time |

## 8. Control-status matrix

| Control | Status | Enforced by / stand-in |
|---|---|---|
| Sign what you see (WYSIWYS approval) | **CODE** | `approve --request` recomputes and displays; served signer page recomputes in-browser |
| Quorum-gated policy mutation | **CODE** (procedural against a host-level actor) | `policy set --approvals`, prior-era commitment from the stored policy |
| Bootstrap labeled in history | **CODE** | history row `bootstrap` |
| Complete-roster commit at issuance | **CODE** | `trustee enroll` |
| Single-use token; bucket-bounded redemption | **CODE** | durable nonce burn; same-bucket rule with receipted denial |
| Receipt for every attempt | **CODE** | `authorize` |
| Consent-bound operator context; closed reason vocabulary; field hygiene | **CODE** | `request`; `REASON_CODES`; 512-byte cap and character rules |
| Truthful trustee attribution | **CODE** | `verify_trustee_attribution` at audit and at the gate |
| Human-readable receipt rendering | **CODE** | `receipts --verbose` |
| Keyguard container for a new vault | **CODE** | `SECURACV_VAULT_PASSPHRASE` → MKG1 |
| Signed high-water-mark; fail-closed verify | **CODE** | `SECURACV_HWM_PATH`; `log_verify --high-water-mark` |
| Anchor verification labeling | **CODE** | `log_anchor verify --ca`; `UNVERIFIED` otherwise |
| Verdict honesty | **CODE** | `valid` vs `self-consistent; identity unverified` |
| Script hash-commitment | **PROC** → §3.4 | hash in `SCRIPT.sha256`; interim `script=` marker in `--purpose` |
| Exception receipts | **PROC** → §3.4 | `EXCEPTIONS.md` |
| Witness co-signature | **PROC** → §3.4 | signed `CLOSEOUT.md` |
| Sanitized transcript export | **PROC** → §3.4 | manual redaction |
| Precommitted channels | **PROC** → §3.3 | CPS §3.6 |
| Verification / duress phrases | **PROC** → §3.3 | ceremony record |
| Cooling-off, notify-all, veto, emergency profile | **PROC** → §3.5 | before-request announcement and wait |
| Trustee key wrapping, hardware keys, enrollment attestation | **GAP** → §3.7 | plaintext 0600 key files; read-back |
| Recovery quorum | **GAP** → §2.3 | none — loss of quorum has no in-band recovery |
| Proactive resharing, liveness attestations | **GAP** → §2.4 | recorded rehearsals (C4b) |
| Receipt-chain heads cross-bound into full verify | **GAP** → §4 | `digest` anchors of receipt heads |
| Handoff sidecars for unseal outputs | **GAP** → §5 | `CLOSEOUT.md` digests |
| Device-key rotation and database re-key commands | **GAP** | library APIs only |
| Trustee key generation / public-key helper | **GAP** | external Ed25519 tool |
| Keyguard migration and passphrase rotation | **GAP** | new vault only |
| `doctor` awareness of `master.keyguard` | **GAP** | manual keyguard record |
| Signed audit-trail review (`log_review attest`) | **GAP** → §4 | review log in the record store |

## 9. Keeping this document honest

Every command, flag, file format id, and quoted message above is taken from
the shipped source (`src/break_glass/cli.rs`, `src/bin/break_glass_serve.rs`,
`src/bin/log_anchor.rs`, `src/bin/log_verify.rs`, `src/vault/mod.rs`,
`src/log/high_water_mark.rs`). When a badge here says **PROC** or **GAP** and
the code later enforces the control, the badge changes in the same pull
request — the tracker entry in
[`REMEDIATION-2026-08.md`](REMEDIATION-2026-08.md) ("Ceremony runbook pass")
lists the gaps this document was written around.
