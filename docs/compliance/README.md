# Compliance mapping suite

SecuraCV's record is its device-signed, hash-chained receipt ledgers, checked against a key the relying party pins. Everything in this directory is derived from that record or describes it, and adds no authority. That means:
- maps from a regime's clauses to the SecuraCV mechanisms that exist, each with a status from a closed vocabulary and evidence a reader can open;
- a methodology for checking those mechanisms without trusting SecuraCV;
- templates an operator fills;
- and, as they land, projections that reshape receipts into formats other systems read.

SecuraCV holds no certification, approval, registration or qualification under any regime named here, and nothing here states that a deployment is acceptable under one. That determination belongs to the regulated organization and its regulator, to an assessor, or to a court. The consumer kernel stays self-signed and pinned. Enterprise-tier projections are opt-in files an operator command writes and the operator ships, and each says that the receipts, not the projection, are the record.

## Reader's guide

If you are an inspector of a pharmaceutical or medical record system, open
[`part11.md`](part11.md), [`eu_gmp_annex11.md`](eu_gmp_annex11.md) and
[`alcoa_plus.md`](alcoa_plus.md). If you are an assessor working to BS 10008,
open [`bs10008.md`](bs10008.md) and the
[operator pack](BS10008_OPERATOR_PACK.md). If you are a court or counsel, open
[`us_evidence.md`](us_evidence.md), then (once item 7's CP3 lands) the
`claims/` directory of a court kit. If you are a SIEM analyst, open
[`SIEM_PROJECTION.md`](SIEM_PROJECTION.md). If you rely on C2PA manifests,
open [`c2pa_conformance.md`](c2pa_conformance.md) and
[`CAWG_COSIGNING.md`](CAWG_COSIGNING.md). If you operate a deployment, read
this page and [`templates/README.md`](templates/README.md).

## Status vocabulary

Every row carries exactly one status from a closed set of five, defined once
in `spec/witness_dictionary.json` (`compliance_map_status`) and rendered,
with the crosswalk from the runbook's and the CPS's status words, in
[`STATUS.md`](STATUS.md). The definitions are not retyped here.

## Map schema

Each map is a JSON file in [`maps/`](maps/) with the schema tag
`securacv-compliance-map:v1`. The schema is closed: an unknown key or a
missing one is an error at every level, and the human tables beside this
page are generated from the maps by `scripts/gen_compliance_tables.py`, never
edited by hand.

| Object | Required keys | Optional keys |
|---|---|---|
| top level | `schema`, `regime`, `last_reviewed` (a month, `YYYY-MM`), `rows` | `preamble` |
| `regime` | `id` (the file stem), `title`, `kind`, `determination_by`, `source` | — |
| `regime.source` | `citation`, `access` (`fetched`, `blocked`, `paywalled` or `not-attempted`), `note` | — |
| row | `id`, `clause`, `clause_verified`, `requirement` (our paraphrase), `mechanism`, `status`, `evidence` | `operator_action`, `limits`, `reason`, `depends_on`, `alcoa` |
| evidence | `kind` (`test`, `code`, `procedure`, `design`, `decision`, `tracker` or `doc`), `path` | `test`, `symbol`, `anchor` |

What each status must carry: a `structural` row names a test function and a
code symbol; a `procedural` row names a procedure and the operator's action;
a `designed` row names a design record and the roadmap PR that builds it
(`depends_on`); a `not-applicable` row gives a reason; a `not-pursued` row
gives a reason and points at where the decision is recorded. A `tracker`
entry points at [`ENTERPRISE_CUSTODY.md` §6](../security/ENTERPRISE_CUSTODY.md#6-regulated-market-assurance-tier).
`clause_verified` may be true only when the regime's source was fetched, and
no source was: every clause in every map is marked unverified.

## Written by us, filled by the operator

| Written by SecuraCV (linted as statements about SecuraCV) | Filled by the operator (linted too, so no template carries an overstatement into a deployment) |
|---|---|
| the maps, the generated tables, `STATUS.md`, this page, the methodology, the BS 10008 mapping, the capture note, the design notes; from CP3, the validation sheets for the rulesets SecuraCV ships | [`templates/bs10008_operator_pack.template.md`](templates/bs10008_operator_pack.template.md); from CP3, operator validation sheets for their own ruleset ids; the existing [Custody Practice Statement template](../security/CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md) |

## What the operator decides

These are the operator's or the regulated organization's acts. No SecuraCV
document makes them:

- whether the deployment is an open or a closed system (Part 11 §11.30), and the added controls an open one needs;
- the written statement Part 11 §11.100(c) describes, where the predicate rule calls for it;
- whether bucket-level (10-minute) date and time is acceptable for the records at hand;
- which TSA, if any, is declared a qualified TSA in the anchor policy — the operator's declaration, checked by the operator on the trusted list, never SecuraCV's;
- whether to include names in a projection, once item 7's CP2 lands;
- whether and where to ship a projection;
- the retention period.

## When an auditor needs more than the kernel can give

The kernel cannot give these, because the capability was never written:
- event times finer than a 10-minute bucket (anchoring acts carry a TSA's time);
- footage outside a quorum-authorized unseal;
- the identity of anyone observed;
- a query interface over events;
- a live feed, push or listener;
- a central audit store;
- a validated cryptographic module;
- a SecuraCV statement that a deployment is acceptable under a regime.

What exists:
- the receipts themselves (`court_export`, `break_glass receipts --verbose`);
- `log_verify` under a key the auditor pins;
- projection files on the operator's cadence, once item 7's CP2 lands (designed,
  not built: [`SIEM_PROJECTION.md`](SIEM_PROJECTION.md));
- these maps, with their evidence.

Each refusal is a recorded design decision (`docs/security/PROVENANCE_INTEROP.md` §3–§4, `spec/invariants.md`). A regime that requires one of them describes a requirement the organization fulfills by other means, and the map row says so.

## Index

The generated tables, each with its scope and who makes the determination:

| Table | Regime | The determination belongs to |
|---|---|---|
| [`part11.md`](part11.md) | 21 CFR Part 11 — electronic records and electronic signatures | the regulated organization, and the FDA on inspection |
| [`eu_gmp_annex11.md`](eu_gmp_annex11.md) | EU GMP Annex 11 — computerized systems | the manufacturer, and the GMP inspectorate |
| [`alcoa_plus.md`](alcoa_plus.md) | ALCOA+ data-integrity attributes | the regulated organization and its inspectors |
| [`hipaa_164_312.md`](hipaa_164_312.md) | HIPAA Security Rule, 45 CFR 164.312 | the covered entity or business associate, and HHS |
| [`us_evidence.md`](us_evidence.md) | FRE 901, 902, proposed 707; 28 U.S.C. § 1746 | the court |
| [`eidas_qualified_ledger.md`](eidas_qualified_ledger.md) | eIDAS qualified electronic ledgers and time stamps | a supervisory body, and the courts |
| [`c2pa_conformance.md`](c2pa_conformance.md) | the C2PA conformance program, generator topics | the C2PA conformance program, and each relying party's validator configuration |
| [`bs10008.md`](bs10008.md) | BS 10008, by topic | the operator's assessor |
| [`fips_140_3_vocabulary.md`](fips_140_3_vocabulary.md) | FIPS 140-3 / ISO/IEC 19790 / SP 800-57 vocabulary | a CMVP testing laboratory and NIST, for a module; nobody, for a vocabulary mapping |
| [`capture_provenance.md`](capture_provenance.md) | capture-pipeline provenance (internal tracker) | each relying party |
| [`enterprise_assurance.md`](enterprise_assurance.md) | the regulated-market assurance tier (internal tracker) | the procuring organization |

And the rest of the directory:

- [`STATUS.md`](STATUS.md) — the five statuses, the crosswalk, and the counts per map (generated).
- [`VERIFICATION_METHODOLOGY.md`](VERIFICATION_METHODOLOGY.md) — how a third party checks the record with only the files.
- [`BS10008_OPERATOR_PACK.md`](BS10008_OPERATOR_PACK.md) and [`templates/README.md`](templates/README.md) — the operator pack and its fill-in template.
- [`CAPTURE_PROVENANCE.md`](CAPTURE_PROVENANCE.md) — what the kernel records about the capture path, and what sensor-proximate signing would cost.
- [`SIEM_PROJECTION.md`](SIEM_PROJECTION.md), [`VALIDATION_SHEETS.md`](VALIDATION_SHEETS.md), [`CAWG_COSIGNING.md`](CAWG_COSIGNING.md) — the three design notes for item 7's CP2, CP3 and CP5.

CP2, CP3 and CP5 add their guides here when they land.

## Keeping it honest

`scripts/lint_compliance_map.py` runs in CI on every pull request. A row may
be `structural` only while the test function and the code symbol it names
exist: rename or delete either and the build turns red until the row changes
in the same change, downgraded if the mechanism regressed. Evidence paths and
anchors must resolve. No clause is marked verified from a source nobody
fetched, and no BS 10008 clause number appears, because the standard was not
read. The words this directory never uses about SecuraCV are listed once, in
`scripts/lint_compliance_map.py`; quotes and code spans do not exempt them.
No validation result is written into a map: results live in the cited
report.

## What this suite cannot show

- that the host is uncompromised ([`docs/root_paradox.md`](../root_paradox.md));
- that a ruleset's claims are accurate;
- any precise event time;
- any identity beyond what a receipt records and a trustee or operator consented to or declared.
