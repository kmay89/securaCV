# SecuraCV — Specification Index

This file is a **pointer, not a spec**. The repository's canonical, normative
specifications live in [`spec/`](spec/README.md) — start here and follow the links inward.

> Previously this root file held the `zone_crossing` *module template*, which was
> easily mistaken for the system spec. That template now lives at its proper home,
> [`spec/modules/zone_crossing.md`](spec/modules/zone_crossing.md), and this file is
> the index that points into the canonical spec set.

## Start here

- [`spec/invariants.md`](spec/invariants.md) — the non-negotiable privacy/security invariants
- [`spec/event_contract.md`](spec/event_contract.md) — what a conforming module may emit
- [`spec/threat_model.md`](spec/threat_model.md) — adversaries and mitigations

## Full catalog

See [`spec/README.md`](spec/README.md) for the complete list of specification documents
(sensor-adapter contract, evidence envelope, co-signing, and the mesh / beacon / chirp
channel specs).

## Module templates

Per-module conformance specs live in [`spec/modules/`](spec/modules/README.md). The reference
example is [`spec/modules/zone_crossing.md`](spec/modules/zone_crossing.md) — the
"Hello, World" of conforming witness modules, demonstrating the event contract and the
forbidden outputs every module must avoid.
