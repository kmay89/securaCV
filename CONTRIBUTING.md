# Contributing to witness-kernel

This is not a typical open-source project.

`witness-kernel` is **infrastructure with constitutional guarantees**.  
Contributions are welcome **only** if they preserve those guarantees.

If you are here to add features quickly, this is the wrong project.

---

## First: Read These Documents

Before writing or modifying code, you **must** read:

1. `spec/invariants.md`
2. `kernel/architecture.md`
3. `spec/threat_model.md`
4. `SECURITY.md`

If a proposed change conflicts with any of these, it will not be accepted.

---

## The Core Rule

> **Invariants are law.**

They are not:
- Suggestions
- Best practices
- Configurable options

They are enforced mechanically in code, tests, and architecture.

Any change that weakens, bypasses, or makes an invariant optional is a **security regression**.

---

## What We Welcome

Contributions that strengthen or clarify guarantees, including:

- Additional **conformance tests**
- Stronger **compile-time enforcement**
- Improved **auditability or verification tooling**
- Clearer documentation of *why* something is forbidden
- Performance improvements that **do not** expand observability or retention

When in doubt, assume “no” and justify why it must be “yes.”

---

## What We Do Not Accept

The following will be closed without discussion:

- Identity, biometric, or tracking features
- Optional privacy modes or configuration flags
- Longer retention “for convenience”
- Cross-bucket correlation extensions
- Retroactive processing of historical data
- “Advanced” modules that weaken isolation
- Changes justified only by performance or product needs

If you need these, you are building a different system.

---

## How to Propose a Change

Every non-trivial PR **must** include:

1. **Invariant impact statement**
   - Which invariants does this touch?
   - Why are they still preserved?

2. **Threat model impact**
   - Does this introduce new metadata?
   - Does it change correlation surface?
   - Does it affect failure modes?

3. **Enforcement point**
   - Where is the guarantee enforced in code?
   - What test fails if it breaks?

PRs without these will not be reviewed.

## Issue Templates

When reporting regressions or drift, use the structured issue forms:
- [Security Report](.github/ISSUE_TEMPLATE/security_report.yml)
- [Conformance Report](.github/ISSUE_TEMPLATE/conformance_report.yml)

---

## Tests Are Not Optional

If your change adds behavior, it must add a test that proves:
- The allowed path works
- The forbidden path fails **loudly**

Tests that only cover the happy path are insufficient.

CI enforces formatting and linting. Before submitting, run:

- `cargo fmt --check`
- `cargo clippy -- -D warnings`

---

## Cleverness Warning

Many systems fail because of “reasonable” ideas like:
- Caching frames briefly
- Making tokens last longer
- Adding debug access
- Reusing hashes across windows

If you find yourself saying:
> “This should be fine…”

Stop. It probably violates an invariant.

---

## Review Philosophy

Expect reviews to focus on:
- Failure modes
- Adversarial interpretation
- Future misuse, not present intent

This is deliberate.

We optimize for **long-term civic safety**, not short-term velocity.

---

## Final Note

If reading this document makes you uncomfortable, that’s okay.

This project is designed to make unsafe ideas feel unwelcome.

That is a feature, not a bug.
