# Security documents

Long-form security documentation for SecuraCV. The repository-root
[`SECURITY.md`](../../SECURITY.md) remains the policy / vulnerability-reporting
entry point and the canonical protocol threat model lives at
[`spec/threat_model.md`](../../spec/threat_model.md).

| Document | Purpose |
|----------|---------|
| [`SECURITY_MODEL.md`](SECURITY_MODEL.md) | User-facing security guarantees. Included in every evidence export; existence enforced by `firmware/scripts/regression_check.sh`. |
| [`THREAT_MODEL.md`](THREAT_MODEL.md) | The Ten Security Principles + implementation review checklist (developer/auditor reference). Includes the **audit boundary vs security boundary** distinction under *Trust Boundaries*. |
| [`SECURITY-AUDIT.md`](SECURITY-AUDIT.md) | Historical security-audit reports (v2/v3 + mesh addenda + closeouts). |
| [`REMEDIATION-2026-07.md`](REMEDIATION-2026-07.md) | 2026-07 code-scanning + issue-backlog + supply-chain (#924) remediation sweep: what merged, what's still pending, and why. |
