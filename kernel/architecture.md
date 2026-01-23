# Privacy Witness Kernel — `witnessd` Architecture
Status: Draft v0.1  
Intended Status: Normative (Internal Constitution)  
Last Updated: 2026-01-20

This document is the engineering constitution of the Privacy Witness Kernel implementation (`witnessd`).
It exists to make the civic guarantees mechanically enforceable and resistant to erosion.

If you are about to propose a change and you feel the urge to say:
- “just cache frames briefly,”
- “optional identity module,”
- “longer correlation windows,”
- “cloud indexing for convenience,”
you are in the wrong project.

Violations of this document are **conformance violations**.

---

## 0. Non-Negotiables (Binding References)

This architecture MUST enforce:
- `spec/invariants.md` (I–VII)
- `spec/event_contract.md`
- `spec/threat_model.md`

Where this document names a boundary, it is a boundary **in code**, not convention.

---

## 1. Component Isolation Diagram

```
                    (LOCAL ONLY)
┌─────────────────────────────────────────────────────────┐
│                        witnessd                          │
│                   (trusted kernel TCB)                   │
├─────────────────────────────────────────────────────────┤
│  Ingestion  →  Frame Buffer  →  Module Runtime  →  Events│
│     │                 │                 │            │   │
│     │                 │                 │            ▼   │
│     │                 │                 │        Contract │
│     │                 │                 │        Enforcer │
│     │                 │                 │            │   │
│     ▼                 ▼                 ▼            ▼   │
│  Raw Input        Ephemeral         Modules        Sealed │
│  (RTSP/USB/...)   Memory Only       (sandboxed)    Log    │
│                                                        │  │
│                                                        ▼  │
│                                         [Optional] Evidence│
│                                         Vault Trigger      │
│                                                        │  │
│                                                        ▼  │
│                                               Retention    │
│                                               Enforcer     │
└─────────────────────────────────────────────────────────┘
             │                               │
             │                               │
             ▼                               ▼
      Event API (LAN)                  Break-Glass API
      (read-only claims)               (quorum-gated)
```

### Isolation Walls (must hold)
- Raw media MUST NOT cross from Ingestion/Frame Buffer into Event API.
- Modules MUST NOT talk to the network.
- Vault MUST NOT be readable without break-glass.
- External tools MUST NOT have direct access to kernel internals beyond defined APIs.

---

## 2. Data Flow Rules (Raw Media Discipline)

### 2.1 Where raw media may exist
Raw media may exist only:
- in an in-memory frame buffer inside `witnessd`, and/or
- inside the Evidence Vault as encrypted envelopes, and only when triggered by an allowed event type.

Vault envelopes are persisted locally under a fixed path (default `vault/envelopes/`).
The vault does not expose raw bytes without a valid break-glass token.

Raw media MUST NOT be:
- written to disk in normal operation,
- exposed via API,
- logged,
- cached “for performance,”
- sent to any remote endpoint.

### 2.2 Lifetime of raw media
- In-memory frames MUST have a strict TTL.
- Frames MUST be zeroized or dropped immediately after module inference.
- A bounded ring buffer MAY exist solely to allow “pre-roll” for sealed evidence, but:
  - the ring buffer MUST remain in-memory,
  - the maximum duration MUST be short and fixed at build-time for conformance profiles,
  - buffer access MUST be restricted to the vault trigger path.

### 2.3 “No Raw Export” enforcement point
A single choke point MUST exist: `RawMediaBoundary`.
All attempted serialization/export of raw frames MUST pass through it and MUST fail closed.

---

## 3. Trust Boundaries

### 3.1 Kernel TCB (trusted)
The kernel includes only what is necessary to enforce invariants:
- ingestion scheduling
- ephemeral frame buffering
- module execution orchestration
- event contract enforcement
- sealed log writing
- retention enforcement
- break-glass gatekeeping (authorization verification and receipts)

The kernel is the **smallest piece we must trust**.

### 3.2 Modules (untrusted, sandboxed)
Modules are treated as potentially malicious or careless.

Modules MAY:
- read frames via a restricted interface
- output candidate events

Modules MUST NOT:
- access disk
- access network
- spawn subprocesses
- write arbitrary logs
- emit raw media, identity claims, or forbidden metadata

All module output MUST pass through the Event Contract Enforcer.

Enforcement point: `module_runtime::CapabilityBoundaryRuntime` validates module descriptors
before execution and refuses any module that requests filesystem or network access. Modules are
executed in a hardened sandbox boundary that applies a seccomp filter to deny filesystem and
network syscalls (open/openat, socket/connect, and related path or socket APIs). The sandbox is
non-optional: if the platform cannot apply the filter, module execution fails closed.

Sandbox boundary: `module_runtime::sandbox::run_in_sandbox` forks a short-lived worker process
for each module invocation and installs the syscall denylist before running module inference.
The child process is the untrusted boundary; it can read frames via the restricted inference
interface, but it cannot access the filesystem or network even if it attempts to do so.

### 3.3 External tools (least trusted)
External components (UI, dashboard, CLI, integrations) are untrusted:
- They can request event streams.
- They can request break-glass operations.
- They cannot access raw media or logs directly.

They interact only through defined APIs.

---

## 4. Enforcement Map (Where Each Invariant Lives)

### Invariant I — No Raw Export
Enforced by:
- `RawMediaBoundary` choke point
- No raw streaming endpoints exist in the API surface
- Vault is separate and unreadable without break-glass

### Invariant II — No Identity Substrate
Enforced by:
- Contract Enforcer rejects forbidden event types and fields
- No module may emit plate strings, face embeddings, or stable IDs
- Build-time allowlist of module capabilities

### Invariant III — Metadata Minimization
Enforced by:
- `time_bucket` canonicalization (no timestamps in events)
- `zone_id` only (no GPS/address fields exist in schema)
- export batching + optional jitter performed at export boundary
- networking layer constrained to constant-rate patterns if enabled

### Invariant IV — Local Ownership and Custody
Enforced by:
- sealed log stored locally with local keys
- no remote indexing endpoints
- exports are explicit artifacts with receipts

### Invariant V — Break-Glass by Quorum
Enforced by:
- vault key material is threshold-protected
- decrypt requires N-of-M trustee approvals
- every attempt logged and receipted

Threat assumption: the vault is local-only storage. Protecting the host filesystem
and trustee quorum process remains mandatory, because possession of the vault
files alone does not grant access without break-glass authorization.

### Invariant VI — No Retroactive Capability Expansion
Enforced by:
- each log entry binds `ruleset_id` + `kernel_version` + `ruleset_hash`
- kernel refuses to run new modules against old sealed entries
- any attempt to reprocess sealed logs under new rules MUST fail audibly and verifiably

### Invariant VII — Non-Queryability by Design
Enforced by:
- event API supports sequential review and bounded time windows, not bulk identity selectors
- no “search by token across history”
- no cross-zone or cross-device linking interfaces

---

## 5. Forbidden Optimizations (PR Rejection List)

The following “optimizations” violate invariants and are forbidden:

1. Frame caching on disk
2. Optional identity modules
3. Extending correlation windows
4. Adding precise timestamps
5. Cloud indexing / remote search
6. Retroactive reprocessing
7. Per-event outbound notifications without batching
8. “Helpful” debug logs containing raw/identity/precise time

If you need one of these, you are building a different system.

---

## 6. Conformance Checkpoints (Self-Tests)

A conforming build MUST include automated checks that fail closed when invariants are threatened:
- Contract enforcement tests
- No-raw-export tests
- Token non-linkability tests (prove bucket keys destroyed)
- No retroactive reprocessing tests
- API surface audit

---

## 7. Summary

`witnessd` is not a camera application.
It is a constraint engine.

Its purpose is not to maximize capability.
Its purpose is to **make certain capabilities impossible**.
