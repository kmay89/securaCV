# Custody Practice Statement — template

Status: Template v0.1 (2026-09-06)
Companion: [`CEREMONY_RUNBOOK.md`](CEREMONY_RUNBOOK.md) (how a declared ceremony is run)
Design source: [`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) §3.4; standards map in [`PROVENANCE_INTEROP.md`](PROVENANCE_INTEROP.md)

A Custody Practice Statement (CPS) is what a deployment **declares** about how
it keeps evidence: who holds which keys, how a disclosure is authorized, what
is anchored where, and what an auditor can check. The form is borrowed from
RFC 3647 and the IANA KSK DNSSEC Practice Statement, re-scoped from
certificate issuance to **evidence custody** — the sealed event log, vault
envelopes, the receipt chains, the anchors, and the keys that sign or wrap
them.

The point of a CPS is that it is **assessable from disk**. Every "we do X" in
it points at an artifact a reader can run a command against. A CPS that
describes controls the software does not provide is a false statement, so
every control line carries exactly one status:

- **shipped** — enforced by the tool named; a reader can reproduce the check.
- **procedural** — a human rule the deployment follows; name the runbook step.
- **not provided** — the software does not do this; name the tracker item
  ([`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md)) and say what the
  deployment does instead.

Nothing in this template is legal advice, and a completed CPS is an
engineering declaration, not a compliance certification. External regimes
(21 CFR Part 11 / ALCOA+, BS 10008, FIPS 140-3 split-knowledge vocabulary,
FRE 902(13)/(14), eIDAS) are cited as regimes the design is built to be
compatible with — never as regimes it is certified against.

## 0. How to use this template

1. Copy this file to `CUSTODY_PRACTICE_STATEMENT.md` in the deployment's
   record store (not into the vault directory, not into the kernel database).
2. Fill every `{{field}}`. Delete nothing: a section that does not apply says
   "not applicable, because …" so a reader can tell "considered and excluded"
   from "forgotten".
3. Record `{{cps.version}}`, `{{cps.effective_date}}`, and the file's SHA-256
   as `{{cps.sha256}}`; retain every version. Recommended: anchor each
   version's digest with `log_anchor request --digest <sha256>` so the
   statement in force at any ceremony is fixed in time by a third party.
4. A CPS change that alters the roster, the threshold, or `vault.crypto_mode`
   is not a document edit — it is a roster-change ceremony (Runbook C2), and
   the new policy-history row is the CPS change's evidence.

### Artifact → section index

Where each artifact the software already produces is cited, so a reader knows
where to look for the evidence behind a claim:

| Artifact | Produced / read by | Cited in |
|---|---|---|
| Policy-change history (rows `bootstrap` / `change`, prior-era approvals) | `break_glass policy history` | 1.3, 3.2, 4.5, 5.3, 5.6, 7.4, App. C |
| Break-glass receipts (outcome, `trustees_used`, `policy_commitment`, purpose, operator context, `entry_hash`, human rendering) | `break_glass receipts --verbose` | 4.6, 4.7, 5.4, 7.3, 8.4 |
| Export receipts (`auth_mode`, `window`, `artifact_hash`) | `export_verify` | 4.8, 7.5 |
| Anchors (`tsa_anchors` rows, `token_der`, `.tsr` files) | `log_anchor list` / `log_anchor verify --ca` | 4.3, 6.8, 7.6, 8.4 |
| Signed high-water-mark (`SCVHWM01`) | `SECURACV_HWM_PATH`; `log_verify --high-water-mark` | 5.4, 5.7, 7.7 |
| Device key lineage (genesis `device_metadata.public_key`, rotation records, `device_key_history`) | `log_verify --lineage` | 3.4, 5.6, 6.3, 7.8 |
| Vault health and key state | `break_glass doctor` | 6.2, 8.4 |
| Build identity (binary digest, commit, features) | `BUILD.md` in each ceremony record | 6.6 |

## 1. Introduction

### 1.1 Overview

`{{deployment.name}}` keeps the following under custody: the sealed event
log, the vault envelopes under `{{deployment.vault_path}}`, the break-glass
and export receipt chains, the policy-change history, the RFC 3161 anchors,
and the keys that sign or wrap them. It structurally excludes what the
invariants forbid ([`../../spec/invariants.md`](../../spec/invariants.md)):
no identity data, no raw-media path outside the quorum gate, no precise event
times outward.

Two honest sentences belong in every CPS, unchanged:

- The trustee quorum is an **authorization gate, not the cryptographic lock**.
  Possession of the vault directory (a plaintext `master.key`, or a
  `master.keyguard` plus its passphrase) is sufficient to decrypt; the quorum
  governs who may *authorize* a disclosure and leaves a signed, hash-chained
  record of every attempt. Threshold custody of the key itself is design
  (`quorum_unseal_v2.md` §2; tracker `ENTERPRISE_CUSTODY.md` §1).
- **Host compromise is out of the threat model**
  ([`../../spec/threat_model.md`](../../spec/threat_model.md)). Every
  guarantee here holds within the host-trust boundary; a host-level actor can
  rewrite rows, and what the design promises is that such a rewrite leaves no
  valid record — tamper-evident, never tamper-proof.

### 1.2 Document identification

| Field | Value |
|---|---|
| Name | `{{cps.name}}` |
| Version | `{{cps.version}}` |
| SHA-256 of this file | `{{cps.sha256}}` |
| Effective date | `{{cps.effective_date}}` |
| Supersedes | `{{cps.supersedes}}` |
| Anchor of this version (optional) | `{{cps.anchor_id}}` at `{{cps.anchor_tsa}}` |

### 1.3 Participants

| Role (see Runbook §1) | Who | Same person as |
|---|---|---|
| Operator | `{{roles.operator}}` | `{{roles.operator.overlaps}}` |
| Trustees (roster in App. C) | `{{roles.trustees}}` | — |
| Ceremony witness | `{{roles.witness}}` | must not be a trustee or the operator |
| Device-key custodian (`DEVICE_KEY_SEED`) | `{{roles.device_key_custodian}}` | `{{…}}` |
| Vault-passphrase custodian (`SECURACV_VAULT_PASSPHRASE`), if keyguard mode | `{{roles.passphrase_custodian}}` | `{{…}}` |
| Observers / auditors (read-only tools) | `{{roles.observers}}` | hold `device.pub`, `tsa-ca.pem`, and the database key (`break_glass db-key`, passed as `--db-key` / `SECURACV_DB_KEY`) — never `DEVICE_KEY_SEED` |
| Relying parties (verifiers, counsel, regulators) | `{{roles.relying_parties}}` | — |

Where one person fills several roles (the household case), say so here rather
than elsewhere: the CPS is where the consumer-tier collapse of separation is
declared, not hidden.

### 1.4 Scope of custody

| Field | Value |
|---|---|
| Devices / hubs | `{{scope.devices}}` |
| Kernel databases | `{{scope.databases}}` |
| Vault directories | `{{scope.vault_paths}}` |
| Ruleset identifiers in force | `{{scope.ruleset_ids}}` |
| `vault.crypto_mode` | `{{scope.crypto_mode}}` (classical / pq / hybrid) |
| Retention window | `{{scope.retention}}` |
| Explicitly excluded | `{{scope.excluded}}` |

### 1.5 Policy administration

`{{admin.maintainer}}` maintains this CPS. Contact: `{{admin.contact}}`.
Change control: editorial changes bump `{{cps.version}}`; any change to the
roster, threshold, or crypto mode is executed as Runbook C2 and its
policy-history row number is recorded in §9.6.

### 1.6 Definitions

Terms are defined once in [`../GLOSSARY.md`](../GLOSSARY.md). Three reserved
meanings are restated because a CPS is read by people outside the project:

- **verified** — an Ed25519 signature checked against a pinned public key.
  Nothing else is "verified".
- `log_verify` verdicts: `valid` (every chain verifies under an out-of-band
  key) versus `self-consistent; identity unverified` (the chains agree with
  themselves and with the key stored *inside* the database, which proves less).
- `log_anchor verify` labels an anchor `UNVERIFIED` unless a TSA CA
  certificate was supplied with `--ca` and the countersignature checked.

## 2. Publication and repository responsibilities

| # | Item | Status | Detail |
|---|---|---|---|
| 2.1 | What is published | procedural | The device genesis public key and its randomart trust card; the TSA CA certificates relied on; this CPS; sanitized ceremony transcripts, if any. `{{pub.items}}` |
| 2.2 | To whom, how | procedural | Out of band, by agreement: `{{pub.channels}}`. No public endpoint by default; per-device public witnessing is a rejected design (`quorum_unseal_v2.md` §7). |
| 2.3 | Frequency | procedural | On issuance, on every device-key rotation, on every CPS change. |
| 2.4 | Access to the record | shipped | The ledgers are non-queryable (Invariant VII); disclosure is by sequential export only. Who may run the read-only tools: `{{pub.readers}}`. |

## 3. Identification and authentication

| # | Item | Status | Detail |
|---|---|---|---|
| 3.1 | Naming | shipped | Trustee ids are short labels unique within the quorum; requester printed names ride only inside signed receipts (`--requested-by`). Pseudonymous ids are acceptable: `{{ident.naming_policy}}`. |
| 3.2 | Initial trustee identity validation | procedural | How each public key was tied to a person at enrollment: fingerprint read-back over the precommitted channel (Runbook C1 step 3). Who generated each key and where (`trustee enroll --generate` means the operator transiently held it): `{{ident.key_origin}}`. Enrollment attestation in the policy record: **not provided** (`quorum_unseal_v2.md` §3.7). |
| 3.3 | Authentication for ceremony acts | shipped | Possession of the trustee's Ed25519 signing key; approvals are domain-separated and re-derived at every audit. Hardware-held trustee keys: **not provided** (§3.7). Face and voice are never identity signals (§3.3) — state it: `{{ident.no_biometrics}}`. |
| 3.4 | Device identification | shipped / procedural | The genesis key pin (`device_metadata.public_key`) and the rotation lineage are verifiable (`log_verify --lineage`); how relying parties *obtain* the pin is procedural: `{{ident.pin_distribution}}`. |
| 3.5 | Requester identification | shipped (as recorded) | `--requested-by`, `--reason` (closed vocabulary), `--case-ref`, `--requester-key` are consent-bound and recorded; the requester key is recorded, **not** checked against a registry. |
| 3.6 | Channels | procedural | The precommitted channel list (spec §3.3): `{{ident.channels}}`. A call may initiate a request; consent travels only through the trustee's own tool. |

## 4. Evidence life-cycle operational requirements

| # | Item | Status | Detail |
|---|---|---|---|
| 4.1 | Creation and sealing | shipped | Events are signed and hash-chained at creation, bucketed to 10 minutes, bound to `ruleset_hash`. This is the object of custody, not a ceremony. |
| 4.2 | Retention and pruning | shipped | Checkpoints and the retention window; the key lineage survives pruning. `{{ops.retention_policy}}` |
| 4.3 | Anchoring | shipped (primitives) / procedural (cadence) | Runbook C5. Cadence `{{ops.anchor_cadence}}`; TSAs `{{ops.tsa_primary}}` (eIDAS-qualified: `{{yes/no}}`) and `{{ops.tsa_secondary}}`; subjects anchored: chain head, export digests, receipt heads as `digest`. Offline flow used: `{{yes/no}}`. `token_der` exported with every backup. OpenTimestamps leg: **not provided**. |
| 4.4 | Access requests | shipped | Who may request: `{{ops.requesters}}`. Requests carry a reason from `REASON_CODES` (`incident-review`, `legal-request`, `legal-hold`, `owner-recovery`, `safety-check`, `maintenance-audit`, `drill`), a printed name, and optional case reference; envelopes eligible: `{{ops.eligible_envelopes}}`. Runbook C3. |
| 4.5 | Approval | shipped / procedural | n-of-m from the **current** roster (`{{n}}`-of-`{{m}}`); trustees sign what their own tool displays (WYSIWYS, shipped); blind signing (`--request-hash`) policy: `{{ops.blind_signing_policy}}`. Cooling-off, notify-all, single-trustee veto, emergency profile: **procedural** until spec §3.5 ships — the deployment's rule: `{{ops.cooling_off_rule}}`. Each receipt records the policy era it was authorized under (`policy_commitment`). |
| 4.6 | Authorization and token handling | shipped | Single-use, bucket-bounded token written with `--output-token` (mode 0600); destroyed after use per `{{ops.token_disposal}}`; denied attempts are receipted and retained. |
| 4.7 | Unseal and disclosure handoff | shipped / procedural | Cleartext lands in `--output-dir` under `{{ops.output_dir_custody}}`; recipient, purpose, and per-file SHA-256 recorded in `CLOSEOUT.md` (procedural). Per-file handoff sidecars: **not provided** (`quorum_unseal_v2.md` §5). |
| 4.8 | Event exports | shipped | `auth_mode` `self_export` / `break_glass` / `api`; scheduled exports: `{{ops.export_schedule}}`; `window` bounds on every receipt. |
| 4.9 | Disclosure packaging | shipped (event bundles only) | `court_export` assembles a disclosure kit for event-export bundles and warns loudly when the bundle is unanchored ([`../court_export.md`](../court_export.md)). It does **not** package vault unseal outputs. |
| 4.10 | Receipt audit and review | procedural | Who reviews `break_glass receipts`, `policy history`, and `log_verify`, how often, and where the review is recorded: `{{ops.review_cadence}}`. A signed review attestation (`log_review attest`): **not provided** (§4). |
| 4.11 | Rehearsal | shipped (primitives) / procedural (cadence) | Sandbox drills (`break_glass drill`, no ledger record) and recorded rehearsals (a real unseal with `--reason drill`, real receipt) are shipped; running a drill on every build change and the rehearsal cadence `{{ops.rehearsal_cadence}}` are human rules. Runbook C4. |
| 4.12 | Incident and compromise handling | procedural | Device-key compromise → rotation (library API today; Runbook C7) plus relying-party notification `{{inc.device_key}}`; trustee-key compromise → C2 replace/remove `{{inc.trustee_key}}`; vault passphrase compromise → no rotation path; the deployment's response: `{{inc.passphrase}}`; suspected ledger tampering → `log_verify --high-water-mark`, `log_anchor verify --ca`, incident `{{inc.tamper}}`. |
| 4.13 | End of custody | procedural | Final anchor, `token_der` export, disposition of keys and passphrase, what relying parties receive: `{{ops.end_of_custody}}`. |

## 5. Facility, management, and operational controls

| # | Item | Status | Detail |
|---|---|---|---|
| 5.1 | Physical controls | procedural | Where the hub, devices, vault directory, and high-water-mark medium live; who has physical access: `{{fac.physical}}`. With host compromise out of scope, physical control *is* the control. |
| 5.2 | Trusted roles and separation | procedural | Role table per §1.3; persons-per-task rules (`{{n}}`-of-`{{m}}` for disclosure; two-person handling of `DEVICE_KEY_SEED` if declared: `{{fac.two_person}}`). |
| 5.3 | Personnel | procedural | Trustee selection and independence ("independently controlled principals", Invariant V): `{{fac.trustee_selection}}`. On/off-boarding is C2. Liveness: recorded rehearsals stand in for liveness attestations (**not provided**, §2.4). |
| 5.4 | Audit logging | shipped | Four ledgers — sealed log, break-glass receipts, export receipts, policy history — plus the anchors table (not chained) and the signed high-water-mark. Retention and review per §4.10. |
| 5.5 | Records archival | procedural | Ceremony record directories (Runbook §2), the retention table (Runbook §7), media and custody of the archive: `{{fac.archive}}`. |
| 5.6 | Key changeover | shipped (lineage) / not provided (tooling) | Device identity rotation exists as a library API with a verifiable lineage; there is no operator command yet (Runbook C7). Trustee rotation is C2. Passphrase rotation: **not provided**. |
| 5.7 | Compromise and disaster recovery | procedural | Backup set: database, vault directory, anchor `token_der` export, high-water-mark copy, ceremony archive: `{{fac.backup}}`. Restoration check: `log_verify --high-water-mark`, `log_anchor verify --ca`. Residual: a mark stored beside the database rolls back in lockstep with it. **Loss of quorum has no in-band recovery** (recovery quorum **not provided**, §2.3) — every CPS states this. |
| 5.8 | Termination | procedural | See 4.13. |

## 6. Technical security controls

| # | Item | Status | Detail |
|---|---|---|---|
| 6.1 | Key generation | shipped | Device key from `DEVICE_KEY_SEED` (full-entropy, e.g. `openssl rand -hex 32`, or the memory-hard `seed-argon2id:v1:` form — [`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md) §5); trustee keys minted by `trustee enroll --generate` or by the trustee. Where generated: `{{tech.keygen}}`. |
| 6.2 | Private-key protection | shipped / not provided | Device seed storage: `{{tech.seed_storage}}`. Trustee key files are plaintext 32-byte hex at mode 0600 — passphrase wrapping and hardware tokens are **not provided** (§3.7). Vault master key: plaintext `master.key`, or `master.keyguard` (MKG1, Argon2id-wrapped, bound to the vault path) when `SECURACV_VAULT_PASSPHRASE` is set — a deterrent against casual copying, not an anti-exfiltration control. Database key: derived from the device key, or decoupled via `SECURACV_DB_KEY_SEED`: `{{tech.db_key_mode}}`. |
| 6.3 | Other key aspects | shipped | Genesis anchor and rotation lineage; `vault.crypto_mode` `{{scope.crypto_mode}}`; post-quantum keys are not rotated. |
| 6.4 | Activation data | procedural | Vault passphrase custody and escrow: `{{tech.passphrase_custody}}`. Trustee passphrases: **not provided**. |
| 6.5 | Computer security controls | procedural | Host hardening baseline: `{{tech.host_baseline}}`. The served console binds loopback unless TLS material is configured (shipped). The detector sandbox is not a custody control. |
| 6.6 | Life-cycle controls | procedural | Build identity recorded in every ceremony's `BUILD.md`: binary SHA-256, git commit, enabled features (`tsa`, `api-tls`, `pqc-*`). Supply-chain transparency source: `{{tech.build_provenance}}`. |
| 6.7 | Network security | shipped (kernel and custody tools) / procedural (deployment inventory) | The kernel (`witnessd`) and the custody tools open no outbound connection except operator-initiated anchoring (`log_anchor request`). Bridges and relays a deployment enables do connect out (`alert_relay`, `event_mqtt_bridge`, `frigate_bridge`, the webhook/MQTT adapters); the enabled ones and their peers: `{{tech.outbound_components}}`. Served-console exposure `{{tech.console_exposure}}`. |
| 6.8 | Time | shipped / procedural | Event time is bucketed to 10 minutes; precise time exists only on anchoring and disclosure acts (TSA `genTime`). Clock source and NTP check before ceremonies: `{{tech.clock}}`. |

## 7. Artifact profiles

| Artifact | Format / version | Custody-relevant fields | Specified in | Verifier |
|---|---|---|---|---|
| 7.1 Sealed event entry | hash-chained, Ed25519 | `prev_hash`, `entry_hash`, bucket, `ruleset_hash` | `spec/event_contract.md` | `log_verify` |
| 7.2 Retention checkpoint | v3 cutoff-bound | `chain_head_hash`, `cutoff_event_id`, `signature` (the signer is resolved against the key lineage by the verifier, not carried in the record) | `spec/evidence_envelope.md` | `log_verify --checkpoints` |
| 7.3 Break-glass receipt | JSON payload, chained | envelope id, `request_hash`, `ruleset_hash`, `time_bucket`, `trustees_used`, `approvals_commitment`, `policy_commitment`, outcome, purpose, operator context, `request_bucket` | `spec/break_glass.md` | `break_glass receipts --verbose` |
| 7.4 Policy-change record | JSON payload, chained | previous and new policy, `change_hash`, approvals, `bootstrap` flag, device signature | `spec/break_glass.md` | `break_glass policy history` |
| 7.5 Export receipt | JSON payload, chained | `auth_mode`, `window`, `artifact_hash` | `spec/evidence_envelope.md` §8 | `export_verify` |
| 7.6 Anchor record | `tsa_anchors` row + DER token | `subject` (`chain_head` / `digest`), `subject_hash`, `tsa_url`, `gen_time`, `token_der` | [`../timestamping.md`](../timestamping.md) | `log_anchor verify --ca` |
| 7.7 High-water-mark | `SCVHWM01` | `seq`, head, bucket, signer, signature | `ENTERPRISE_CUSTODY.md` §2 | `log_verify --high-water-mark` |
| 7.8 Key-rotation record | sealed record + `device_key_history` row | new-key attestation, previous-key authorization | `spec/evidence_envelope.md` §11 step 2b | `log_verify --lineage` |
| 7.9 Ceremony files | `securacv-unlock-request:v2`, approval JSON, proposal, policy-approval, token (sensitive), setup draft | as named | `spec/break_glass.md` | the commands that consume them |

## 8. Compliance audit and other assessments

| # | Item | Detail |
|---|---|---|
| 8.1 | Frequency | Per ceremony (the witness) and periodically (an observer): `{{audit.cadence}}`. |
| 8.2–8.3 | Who; independence | `{{audit.who}}`; independent of trustees and operator: `{{yes/no, why}}`. |
| 8.4 | Topics | `break_glass receipts --public-key-file` VALID and `policy history --public-key-file` `History chain VALID`, both under the pinned key (a run without one is labeled `self-consistent; identity unverified` and does not count); every anchor checked with `--ca` and none `UNVERIFIED`; high-water-mark checked; `log_verify` verdict `valid` under the out-of-band key; drill and recorded-rehearsal evidence on file; exceptions register reviewed; retention table met; CPS-versus-practice deltas listed. |
| 8.5 | Action on deficiency | `{{audit.deficiency}}` |
| 8.6 | Communication of results | `{{audit.results_to}}` |
| 8.7 | External regimes | Mapping is "designed to be compatible with" only: 21 CFR Part 11 / ALCOA+, BS 10008, FIPS 140-3 split-knowledge vocabulary, FRE 902(13)/(14), eIDAS — see `PROVENANCE_INTEROP.md`. Never "certified", "compliant", or "admissible". |

## 9. Other business and legal matters

| # | Item | Detail |
|---|---|---|
| 9.1 | Fees | Not applicable. |
| 9.2 | Confidentiality of ceremony records | `{{legal.confidentiality}}`; sanitized transcripts only leave the deployment. |
| 9.3 | Privacy | The invariants bound what a CPS may require: never identity data, never precise event times outward, names and reasons only inside signed receipts. |
| 9.4 | Warranties and liability | Engineering guidance, not legal advice ([`../LEGAL.md`](../LEGAL.md)). `{{legal.liability}}` |
| 9.5 | Governing law | `{{legal.jurisdiction}}` |
| 9.6 | Amendments | Version history of this CPS, each version hashed (and, recommended, anchored); roster/threshold/crypto-mode changes reference their policy-history row: `{{legal.amendments}}` |
| 9.7 | Dispute handling | `{{legal.disputes}}` |

## Appendices

### A. Control-status matrix for this deployment

Copy the Runbook §8 matrix here and date it ("as of build `{{build.sha256}}`,
commit `{{build.commit}}`, `{{date}}`"). Any row a deployment changes from
the runbook's default must say why.

### B. Deployment inventory

Devices, databases, vault paths, ruleset ids, and public keys / fingerprints
only — never seeds, never passphrases.

### C. Trustee roster snapshot

Trustee ids and key fingerprints exactly as `break_glass policy show` prints
them, with the policy-history row that created this roster and the date.

### D. Ceremony log index

| Directory | Type (C1–C8) | Date | Anchor id / TSA `genTime` | Exceptions |
|---|---|---|---|---|
| `{{…}}` | | | | |

### E. Relying-party verification card

The exact commands a verifier runs, with the out-of-band inputs they need. A
relying party receives three things over the precommitted channel: the
device's genesis public key (`device.pub`), the TSA CA bundle (`tsa-ca.pem`),
and the **database key** — the 64-hex SQLCipher key the operator derives with
`break_glass db-key`. A relying party is **never** given `DEVICE_KEY_SEED`:
that is the signing seed, and whoever holds it can mint receipts, history
rows, and high-water-marks, so any verdict they produce afterwards is
self-signed. The database key reads the ledgers and signs nothing.

```sh
export SECURACV_DB_KEY=<64-hex database key from `break_glass db-key`>
log_verify --db witness.db --public-key-file device.pub [--high-water-mark hwm.bin]
break_glass receipts --db witness.db --public-key-file device.pub --verbose
break_glass policy history --db witness.db --public-key-file device.pub
log_anchor --db witness.db verify --ca tsa-ca.pem
sha256sum <disclosed files>            # against CLOSEOUT.md / MANIFEST.sha256
# a full TSA response as retained from the offline flow (.tsr):
openssl ts -verify -digest <hex> -in <anchor>.tsr -CAfile tsa-ca.pem
# a bare token — tsa_anchors.token_der, or a court_export kit's anchors/*.der:
openssl ts -verify -digest <hex> -in <anchor>.der -token_in -CAfile tsa-ca.pem
```

Expected results: verdict `valid` (not `self-consistent; identity unverified`);
receipts and history VALID under `Verifying key: pinned out of band` (the same
tools print `read from the audited database — … self-consistent; identity
unverified` when no key file is given, and that verdict is not evidence of
identity); no anchor `UNVERIFIED`; digests matching.
