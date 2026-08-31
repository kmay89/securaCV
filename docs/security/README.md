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
| [`REMEDIATION-2026-08.md`](REMEDIATION-2026-08.md) | 2026-08 code-level remediation pass (PWK wizard secret disclosure/SSRF/injection, fail-closed network binds, seccomp fd/syscall hardening, timestamp-anchor honesty, and the spec corrections that make the invariants match the code). |
| [`ENTERPRISE_CUSTODY.md`](ENTERPRISE_CUSTODY.md) | The tracked custody/assurance tier: threshold/HSM vault key custody, the sealed-log external high-water-mark + out-of-band verify key, in-process TLS (shipped 2026-08 as the `api-tls` feature; §4 records what remains), the seccomp allowlist inversion, a versioned Argon2id seed scheme, image digest pinning, the `cargo audit` `--deny` triage, and the regulated-market (FIPS/PKI/RBAC/SIEM) gap. |
| [`PROVENANCE_INTEROP.md`](PROVENANCE_INTEROP.md) | The standards-compatibility map: how courts (FRE 902(13)/(14), eIDAS), medical/pharma integrity regimes (21 CFR Part 11, ALCOA+), C2PA content provenance, key-ceremony practice (KSK, FIPS 140-3), transparency-log witnessing, and the frontier-AI adversary map onto the chains/receipts/anchors — the prioritized compatibility moves, what we deliberately do not adopt, and how every invariant tension resolves. Companion to [`../../spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md). |
| [`PENTEST_SCOPE.md`](PENTEST_SCOPE.md) | Scope and rules of engagement for adversarial testing: what to attack, what a tester is handed (two Canaries **and their keys**, so a key-extraction finding is unambiguous), what's out of bounds, the severity bar, and the known-and-accepted list. Half its value is that writing it forces the answers. |
| [`../../fuzz/README.md`](../../fuzz/README.md) | The fuzz suite — the repo's only **dynamic** assurance. Everything else in security CI reads the code; these targets run the parsers that eat attacker-chosen bytes (the hand-rolled RFC-3161 DER reader, the vault container, Frigate MQTT payloads, canonical JSON) against input designed to break them. |
