# SIEM projection — design note (item 7 CP2; CP4 adds the runbook)

**Status: designed, not built.** This note is the design record the
`designed` rows `enterprise_assurance` siem-export and atna-syslog and
`hipaa_164_312` 164.312(b)-siem point at. Item 7's CP2 builds the projection
and rewrites this page into the operator guide; CP4 adds the shipping
runbook. Every FHIR and DICOM detail below that rests on a source we could
not fetch is marked unverified (the HL7, IHE and DICOM hosts were
unreachable when it was written).

## What it is, and what it is not

A **projection**: a file derived from the device-signed receipt ledgers that
reshapes each receipt row into a FHIR R4-shaped `AuditEvent`, one per line
(NDJSON), for a security-monitoring system to ingest. It adds no authority.
The receipts are the record; the file says so on every line and in its
header.

- It is written by a read-only binary, `audit_projection`, behind a Cargo
  feature `audit-projection` that is off by default, so the consumer build
  carries no SIEM shaper.
- It is a file the operator ships with the operator's own forwarder. There
  is no listener, no route, no push and no network code.
- It is not a validated FHIR profile: no `meta.profile` is claimed, because
  the IHE BALP disclosure profiles require a Patient entity this record does
  not have.

## The decisions

| Question | Decision |
|---|---|
| Which rows | Every device-signed receipt row: break-glass, export, policy change, clock provenance. Never `sealed_events`, `tsa_anchors`, `ots_anchors`, conformance alarms or checkpoints (not device-signed, or not receipts, or holding a precise time). |
| Header | A sidecar `<file>.header.json` (format `securacv-fhir-audit-projection:v1`): the verdict of the verify pass, the ledger heads, the device-key lineage, the names mode and the time rule. The NDJSON holds only AuditEvents. |
| Selection | None. Every run projects every ledger contiguously, from genesis or from the row after the previous header's heads (`--after`). There is no window, name, case, reason or outcome filter (Invariant VII). |
| Names | A forced choice, `--include-names` or `--omit-names`, with no default; the header records which. Omitted mode numbers trustees per file (`trustee-1`, …) so two files cannot be linked through labels. |
| Never projected | `requester_key`, `token_id`, keys, envelope ids, purpose text, denial text, commitments, nonces, `created_at`, genTime. |
| Denials | A closed `denial_code` by prefix: `approval-count-exceeded`, `request-window-expired`, `unrecognized-trustee`, `insufficient-approvals`, `other`. The reason string itself never reaches the file. |
| Newer rows | A key-set check turns any record with a field this build does not know into `unrecognized-record`, with its handles; an exhaustive match over the outcome enum has no wildcard arm. |
| Receipt for the projection | None. The projection exports no event record; forwarding it is the operator's disclosure, recorded procedurally in the CPS. |

## The mapping

Codes are DICOM's (DCM; meanings from pydicom 3.0.2's tables generated from
PS3.16, **unverified** against dicom.nema.org) and five `securacv-*` code
systems under a `.invalid` canonical base, a name nobody controls rather than
a locator.

| Receipt row | Receipt-kind code | type | DCM subtype | outcome |
|---|---|---|---|---|
| break-glass `Granted` | `break-glass-granted` | 110113 Security Alert | — | 0 |
| break-glass `Denied` | `break-glass-denied` | 110113 | — | 4 |
| `TokenRedeemed` | `break-glass-token-redeemed` | 110113 | 110127 Emergency Override Started | 0 |
| `Unsealed` | `break-glass-unsealed` | 110106 Export | 110132 Use of Restricted Function | 0 |
| export (self, api, paired, break-glass, unlabeled) | `export-self`, `export-api`, `export-paired`, `export-break-glass`, `export-unlabeled` | 110106 | — | 0 |
| policy bootstrap, policy change | `policy-bootstrap`, `policy-change` | 110113 | 110129 Security Configuration | 0 |
| clock provenance | `clock-provenance` | 110100 Application Activity | — | 0 |
| a record this build does not know | `unrecognized-record` | 110100 | — | absent |
| the verify pass failed | `source-verification-failed` | 110113 | — | 8 |

DCM 110144 and 110138 are not used: the first is outside the R4B sub-type
value set, and the second means an override stopped, which is not a veto.

## Time

Every time in a line is the start of the receipt's own signed 10-minute
bucket. `period.end` is the bucket start plus its size minus one second,
because FHIR's `Period.end` is inclusive. The database's `created_at` column
is never read; a source test will assert that the projection module names
neither `created_at` nor a clock call. No anchor's genTime is projected; the
clock-provenance offset is a clamped difference.

## Identity

Only names the signed receipts already carry (the requester, trustee ids, a
case reference), and only under `--include-names`. Never a key, a token id
or a recipient identity.

## The alarm

When the ledgers do not verify, the file still goes through the same pipe a
SIEM reads: exactly one tool-authored AuditEvent of kind
`source-verification-failed`, outcome 8, tagged `projection-alarm` (never
`derived-from-signed-receipt`), with the closed failure kind. Nothing is
projected, the exit status is 1, and a `--after` cursor does not advance.

## `check`

`audit_projection check --db … --in FILE.ndjson` re-derives every line byte
for byte from the database's verified receipts, checks each ledger's
`prev_hash` sequence, and checks the file digest against the header. Each
line also carries the receipt's entry hash, the device's Ed25519 signature
over it, its signing domain and an index into the header's key lineage, so a
reader without the database can check that the device signed that entry hash
(a fabricated handle fails), but not that the line's other fields match the
receipt. Only `check` against the database shows that.

## What stays unverified

The R4 (4.0.1) primary text (R4 = R4B for AuditEvent is inferred), the id
length limit, `Meta.tag` in R4, the `_recorded` primitive-extension form,
`application/fhir+ndjson`, `.invalid` canonical URLs, our codes on extensible
bindings, whether SIEM ingesters keep tags, and every RFC 5424 framing rule
CP4 documents.
