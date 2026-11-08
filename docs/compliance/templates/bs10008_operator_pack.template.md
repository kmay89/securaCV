# BS 10008 operator pack — {{org.name}}

<!-- Operator-filled. Copy this file into the deployment's record store, fill
every {{field}}, and delete nothing: a line that does not apply says "not
applicable, because …". The {{…}} grammar is the CPS template's
(docs/security/CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md §0). Each line carries
exactly one status from docs/compliance/STATUS.md: structural, procedural,
designed, not-applicable or not-pursued. This file is linted like every file
under docs/compliance/. -->

A filled copy of this template is {{org.name}}'s engineering declaration of
how it handles the electronic information named below. It is not an
assessment; whether this practice is acceptable under BS 10008 is the
assessor's determination. The mapping it follows is
`docs/compliance/BS10008_OPERATOR_PACK.md`.

| Topic | What {{org.name}} does | Status |
|---|---|---|
| OP-1 Information management policy | {{policy.information_management}} | procedural |
| OP-2 Duty of care and roles | {{roles.records_manager}} | procedural |
| OP-3 Storage | {{storage.locations}} | procedural |
| OP-3 Retention | {{retention.period}} | procedural |
| OP-3 Disposal | {{disposal.procedure}} | procedural |
| OP-4 Authenticity and integrity | hash-chained, device-signed ledgers, checked with `log_verify` under the pinned device key every {{audit.review_cadence}} | structural |
| OP-5 Audit trails | the receipt ledgers, reviewed every {{audit.review_cadence}} | structural |
| OP-6 Transfer and disclosure | {{transfer.handoff_procedure}} | procedural |
| OP-7 Time | the anchor policy kept at {{storage.locations}} | procedural |
| OP-8 Identity of originators | requesters are identified before approval by {{roles.records_manager}} | procedural |
| OP-9 Change control of analytics | {{capture.devices_and_rulesets}} — for each ruleset id, a link to its validation sheet, or the words "no sheet" | procedural |
| OP-10 Business continuity | backups and the high-water-mark file at {{storage.locations}} | procedural |
| OP-11 Review | every {{audit.review_cadence}}, recorded in the record store | procedural |
| OP-12 Outsourcing | {{outsourcing}} | procedural |

Signed off by {{signoff.name}}, {{signoff.month}}.
