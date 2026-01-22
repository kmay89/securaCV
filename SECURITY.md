# Security Model

This project treats the following as adversarial by default:

- Future maintainers
- Feature creep
- Accidental optimization
- Metadata correlation
- Retroactive reinterpretation of data

## Reporting issues

If you discover a way to violate an invariant, bypass an enforcement point,
or extract information the system claims is impossible to obtain:

**That is a security issue.**

Please report it with:
- A minimal reproduction
- The invariant you believe is violated
- Whether the failure is compile-time or runtime

Use the GitHub issue form for structured reports:
- [Security Report](.github/ISSUE_TEMPLATE/security_report.yml)

## Non-goals

The following are explicitly out of scope:

- Adding identity or biometric features
- Making guarantees optional or configurable
- Retroactively enabling new capabilities on historical data

If you need any of the above, you are building a different system.
