# Spec documents

Canonical specification documents (authoritative sources; do not duplicate without a pointer).

> **Read the maturity column before treating any document as a contract.** Several specs carry
> `_v0` filenames or `Draft v0.x` headers. They are *not* interchangeable: some are stable,
> implemented, CI-exercised contracts; others are drafts whose wire format may still change; a few
> are positioning notes or specifications for capabilities that are **not yet implemented**. Each
> document also declares its own `Status:` / `Intended Status:` in its header — the table below is
> the index-level summary of those.

## Maturity legend

| Maturity | Meaning |
|----------|---------|
| 🟢 **Stable** | Implemented in shipping code and exercised by tests/CI. Treat as a contract; changes are versioned. |
| 🟡 **Draft** | Specified (and sometimes scaffolded in firmware), but **not** a frozen, validated contract — the wire format / behavior may still change. |
| ⚪ **Spec-only / Positioning** | Not a wire contract: either a capability spec with no implementation yet, a positioning/orientation doc, or a design note. |

## Index

| Document | Declared status | Role | Maturity |
|----------|-----------------|------|----------|
| [`invariants.md`](invariants.md) | Draft v0.1 | Foundational | 🟢 Stable — enforced in the kernel |
| [`event_contract.md`](event_contract.md) | Draft v0.1 | Normative | 🟢 Stable — the core event vocabulary/format |
| [`evidence_envelope.md`](evidence_envelope.md) | v1 (proposed) | Normative | 🟢 Stable — versioned, self-verifying; dual Rust↔JS verifier |
| [`sensor_adapter_contract_v0.md`](sensor_adapter_contract_v0.md) | Draft v0.1 | Normative | 🟢 Stable — implemented by `src/adapter/` + `adapter_host` |
| [`break_glass.md`](break_glass.md) | — | Normative | 🟢 Stable — implemented by the `break_glass` CLI + kernel |
| [`threat_model.md`](threat_model.md) | Draft v0.1 | Informative but binding | 🟢 Stable — the security/privacy boundary |
| [`co_signing.md`](co_signing.md) | Draft v0.1 | Design note | ⚪ Design note — not yet a normative wire contract |
| [`quorum_unseal_v2.md`](quorum_unseal_v2.md) | Draft v0.1 | Design specification | ⚪ Spec-only — the quorum/unseal v2 target design (threshold custody, ceremony, anchoring spine, disclosure surface); §3.1 quorum-gated policy mutation, §3.2 WYSIWYS approval, and §5 `court export` (event bundles) are implemented, the rest is direction |
| [`witness_mesh_os_v0.md`](witness_mesh_os_v0.md) | Draft v0.1 | Positioning + pointers | ⚪ Positioning — capability map, defers to the normative specs |
| [`beacon_channel_v0.md`](beacon_channel_v0.md) | Draft v0.1 | Normative (intended) | 🟡 Draft — RF beacon channel (firmware scaffolding; see flag report F-07) |
| [`chirp_channel_v0.md`](chirp_channel_v0.md) | Draft v0.2 | Normative (intended) | 🟡 Draft — acoustic chirp channel (see F-07) |
| [`canary_free_signals_v0.md`](canary_free_signals_v0.md) | Draft v0.1 | Normative (intended) | 🟡 Draft — RF/CSI presence signals (see F-07/F-08) |
| [`canary_mesh_network_v0.md`](canary_mesh_network_v0.md) | Draft v0.2 | Normative (intended) | 🟡 Draft — ESP-NOW/BLE mesh (see F-08) |
| [`gossip_replication_v0.md`](gossip_replication_v0.md) | Draft v0.1 | Normative (intended) | 🟡 Draft — mesh gossip replication (see F-08) |
| [`beacon_cap_gateway_v0.md`](beacon_cap_gateway_v0.md) | Draft v0.1, **specification only** | Informational | ⚪ Spec-only — explicitly **not implemented** until a future release |

External canonical source (not under `spec/`): [`kernel/architecture.md`](../kernel/architecture.md).

Per-module conformance specs (the "Hello, World" module templates) live under
[`spec/modules/`](modules/) — e.g. [`spec/modules/zone_crossing.md`](modules/zone_crossing.md),
a 🟡 template/example, not a shipped contract.

Legacy HTML exports have been removed; refer to the canonical Markdown sources above.
