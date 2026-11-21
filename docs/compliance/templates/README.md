# Compliance templates — filled by the operator

The files here are templates a deployment's operator copies into its own
record store and fills. SecuraCV writes the template; the operator writes
every answer. They are linted by `scripts/lint_compliance_map.py` like every
other file under `docs/compliance/`, so no template carries an overstatement
into a deployment.

| Template | Who fills it | What it is |
|---|---|---|
| [`bs10008_operator_pack.template.md`](bs10008_operator_pack.template.md) | the operator, with the records manager | one line per topic of the [BS 10008 operator pack](../BS10008_OPERATOR_PACK.md) (a topic whose mechanism and operator act differ in status takes a line for each), each line with exactly one status from [`STATUS.md`](../STATUS.md) |

The [Custody Practice Statement template](../../security/CUSTODY_PRACTICE_STATEMENT_TEMPLATE.md)
is the other operator-filled document; it uses the same `{{…}}` grammar.
From item 7's CP3, an operator validation-sheet template joins this
directory.

A filled template is the operator's engineering declaration of what the
deployment does. It is not an assessment, and it says nothing about whether
the practice is acceptable under any regime: that determination belongs to
the assessor, the regulator or the court.
