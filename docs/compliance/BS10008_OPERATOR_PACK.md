# BS 10008 operator pack — what SecuraCV supplies and what the operator documents

> Designed to support an operator's BS 10008 work. SecuraCV is not assessed against BS 10008. Clause numbers are not cited: the standard was not available to the authors; topics follow `docs/security/PROVENANCE_INTEROP.md` §1.1.

BS 10008 is about the evidential weight of electronic information: whether
an organization can show, later and to a skeptical reader, that what it holds
is what was captured, unchanged, and handled by the people its records name.
It is paywalled and was not read for this pack, so every topic below is our
reading of the standard's subject from public summaries, and every one is
unverified until an assessor who holds the standard checks it. Whether a
deployment's practice is acceptable under BS 10008 is the operator's
assessor's determination.

Each topic has three parts: what SecuraCV supplies (with its row in
[`bs10008.md`](bs10008.md)), what the operator must document, and the line of
the [fill-in template](templates/bs10008_operator_pack.template.md) where it
goes. Each template line carries exactly one status from the closed set
([`STATUS.md`](STATUS.md)).

### OP-1 Information management policy

- **SecuraCV supplies:** the inventory of what the policy has to name — the
  kernel database, the four receipt ledgers (break-glass, export, policy
  change, clock provenance), the anchor policy file and the record store the
  CPS lives in. Row `bs.policy`.
- **The operator documents:** the policy itself, who owns it, and when it is
  reviewed.
- **Template:** `{{policy.information_management}}`.

### OP-2 Duty of care and roles

- **SecuraCV supplies:** the roles the ceremonies need — operator, trustees,
  reviewer, witness — in the [ceremony runbook](../security/CEREMONY_RUNBOOK.md)
  and the [CPS](../security/CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md). Row
  `bs.duty-of-care`.
- **The operator documents:** the named people, and the separation of duties
  between them (a trustee is not the operator of the export they approve).
- **Template:** `{{roles.records_manager}}`.

### OP-3 Storage and retention

- **SecuraCV supplies:** events are pruned by retention up to a checkpoint;
  receipt ledgers are never pruned ([`docs/evidence_lifecycle.md`](../evidence_lifecycle.md)).
  Rows `bs.storage-integrity`, `bs.retention-disposal`.
- **The operator documents:** where the database, backups and exports are
  stored, the retention period, and how disposal is recorded.
- **Template:** `{{storage.locations}}`, `{{retention.period}}`,
  `{{disposal.procedure}}`.

### OP-4 Authenticity and integrity

- **SecuraCV supplies:** hash-chained, device-signed ledgers and the
  [verification methodology](VERIFICATION_METHODOLOGY.md). Row
  `bs.storage-integrity`.
- **The operator documents:** where the device key is pinned out of band,
  and the cadence on which `log_verify` is run under it.
- **Template:** `{{audit.review_cadence}}`. The ledger line is `structural`;
  the verification line is `procedural`, because a person runs it.

### OP-5 Audit trails

- **SecuraCV supplies:** the receipt ledgers; the optional SIEM projection is
  designed (item 7 CP2, [`SIEM_PROJECTION.md`](SIEM_PROJECTION.md)). Row
  `bs.audit-trail`.
- **The operator documents:** who reviews the trail and how the review is
  recorded.
- **Template:** `{{audit.review_cadence}}`. The ledger line is `structural`;
  the review line is `procedural`, because a person reviews.

### OP-6 Transfer and disclosure

- **SecuraCV supplies:** `court_export` and handoff sidecars, each proving the
  copy by hash ([`docs/court_export.md`](../court_export.md),
  [`docs/handoff_sidecars.md`](../handoff_sidecars.md)). Row `bs.transfer`.
- **The operator documents:** the handoff procedure: who receives what, and
  how the receipt is kept.
- **Template:** `{{transfer.handoff_procedure}}`.

### OP-7 Time

- **SecuraCV supplies:** the anchor policy — one TSA the operator declares to
  be a qualified TSA, one independent TSA, and OpenTimestamps
  ([`docs/timestamping.md`](../timestamping.md#two-tsas-and-the-anchor-policy)).
  Row `bs.time`.
- **The operator documents:** which TSAs, why the declared one was chosen,
  and the anchoring cadence.
- **Template:** `{{storage.locations}}` (the anchor policy file's location)
  and the time line of the template.

### OP-8 Identity of originators

- **SecuraCV supplies:** `requester_name` and `requester_key` in break-glass
  requests, bound into the request hash the trustees approve; the declared
  operator on a self-export. CAWG co-signing is designed (item 7 CP5,
  [`CAWG_COSIGNING.md`](CAWG_COSIGNING.md)). Row `bs.authentication`.
- **The operator documents:** how a requester's identity is checked before
  trustees approve their request.
- **Template:** `{{roles.records_manager}}`.

### OP-9 Change control of analytics

- **SecuraCV supplies:** every event carries its `ruleset_id` and a
  `ruleset_hash`, and history is never reprocessed under another ruleset
  (Invariant VI). The hash is SHA-256 of the id string, so it binds a label,
  not the detection configuration ([`CAPTURE_PROVENANCE.md`](CAPTURE_PROVENANCE.md)).
  Row `bs.capture`.
- **The operator documents:** **change `ruleset_id` whenever the detection
  configuration changes, and write a validation sheet before relying on
  events in proceedings.** The sheet format is designed (item 7 CP3,
  [`VALIDATION_SHEETS.md`](VALIDATION_SHEETS.md)).
- **Template:** `{{capture.devices_and_rulesets}}`.

### OP-10 Business continuity

- **SecuraCV supplies:** scheduled exports and the signed high-water-mark
  file, which fails closed on a truncated or rolled-back ledger
  ([`docs/scheduled_exports.md`](../scheduled_exports.md)).
- **The operator documents:** the backup schedule, the restore test, and the
  separate medium the high-water-mark file lives on.
- **Template:** `{{storage.locations}}`.

### OP-11 Review

- **SecuraCV supplies:** the verification procedure and the CPS's review
  section; a reviewer-signed `log_review attest` is designed (item 3 H).
- **The operator documents:** the periodic review and where it is recorded.
- **Template:** `{{audit.review_cadence}}`.

### OP-12 Outsourcing

- **SecuraCV supplies:** nothing beyond the record; a service provider that
  handles the database, backups or exports is the operator's to control. Row
  `bs.outsourcing`.
- **The operator documents:** each provider, what it handles, and the
  controls on it.
- **Template:** `{{outsourcing}}`.
