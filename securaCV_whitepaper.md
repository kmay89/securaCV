# SecuraCV
## A Unified Canonical White Paper

**Status:** Living Document  
**Repository:** github.com/kmay89/securaCV  
**Authority:** Derived verbatim and structurally from existing Markdown documents in the repository  
**Purpose:** This document consolidates all existing `.md` files into a single, coherent white-paper-style artifact without introducing new concepts, guarantees, or claims.

---

## Document Governance

**Sources:** [`spec.md`](https://github.com/kmay89/securaCV/blob/main/spec.md), [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md)

This document is **normative** where it reflects canonical specifications and **descriptive** elsewhere. It is intentionally conservative. Any future change MUST originate from edits to the underlying canonical sources and then be merged forward.

### Revision Control

- Each revision increments the document version.
- Sections map directly to original source files.
- No section may be modified independently of its source.

**Suggested version format:** `YYYY.MM.patch`

---

# Table of Contents

1. Introduction  
2. Purpose and Scope  
3. System Philosophy: Witnessing vs Watching  
4. Core System Overview  
5. Canonical Invariants  
6. Event Model and Contracts  
7. Threat Model  
8. System Architecture  
9. Integrity, Verification, and Trust  
10. Confidentiality and the Vault  
11. Break-Glass Mechanism  
12. Integrations Overview  
13. Ingestion Backends  
14. Operational Limitations (Root Paradox)  
15. Governance, Contributions, and Security  
16. Release Discipline and Roadmap  
17. Appendix A: Tooling and Verification Utilities  
18. Appendix B: AI and Agent Interaction  

---
## Source Map (Section → Original Docs)

| Section | Source documents |
|---|---|
| [1. Introduction](#1-introduction) | [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md) |
| [2. Purpose and Scope](#2-purpose-and-scope) | [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md)<br>[`spec.md`](https://github.com/kmay89/securaCV/blob/main/spec.md) |
| [3. System Philosophy: Witnessing vs Watching](#3-system-philosophy-witnessing-vs-watching) | [`docs/why_witnessing_matters.md`](https://github.com/kmay89/securaCV/blob/main/docs/why_witnessing_matters.md)<br>[`why_this_matters.md`](https://github.com/kmay89/securaCV/blob/main/why_this_matters.md) |
| [4. Core System Overview](#4-core-system-overview) | [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md)<br>[`kernel/architecture.md`](https://github.com/kmay89/securaCV/blob/main/kernel/architecture.md) |
| [5. Canonical Invariants](#5-canonical-invariants) | [`spec/invariants.md`](https://github.com/kmay89/securaCV/blob/main/spec/invariants.md) |
| [6. Event Model and Contracts](#6-event-model-and-contracts) | [`spec/event_contract.md`](https://github.com/kmay89/securaCV/blob/main/spec/event_contract.md) |
| [7. Threat Model](#7-threat-model) | [`spec/threat_model.md`](https://github.com/kmay89/securaCV/blob/main/spec/threat_model.md) |
| [8. System Architecture](#8-system-architecture) | [`kernel/architecture.md`](https://github.com/kmay89/securaCV/blob/main/kernel/architecture.md) |
| [9. Integrity, Verification, and Trust](#9-integrity-verification-and-trust) | [`log_verify_README.md`](https://github.com/kmay89/securaCV/blob/main/log_verify_README.md)<br>[`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md) |
| [10. Confidentiality and the Vault](#10-confidentiality-and-the-vault) | [`kernel/architecture.md`](https://github.com/kmay89/securaCV/blob/main/kernel/architecture.md) |
| [11. Break-Glass Mechanism](#11-break-glass-mechanism) | [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md)<br>[`spec/break_glass.md`](https://github.com/kmay89/securaCV/blob/main/spec/break_glass.md) |
| [12. Integrations Overview](#12-integrations-overview) | [`docs/integrations/home-assistant-frigate-mqtt.md`](https://github.com/kmay89/securaCV/blob/main/docs/integrations/home-assistant-frigate-mqtt.md)<br>[`docs/homeassistant_setup.md`](https://github.com/kmay89/securaCV/blob/main/docs/homeassistant_setup.md)<br>[`docs/frigate_integration.md`](https://github.com/kmay89/securaCV/blob/main/docs/frigate_integration.md) |
| [13. Ingestion Backends](#13-ingestion-backends) | [`docs/rtsp_setup.md`](https://github.com/kmay89/securaCV/blob/main/docs/rtsp_setup.md)<br>[`docs/v4l2_setup.md`](https://github.com/kmay89/securaCV/blob/main/docs/v4l2_setup.md) |
| [14. Operational Limitations (Root Paradox)](#14-operational-limitations-root-paradox) | [`docs/root_paradox.md`](https://github.com/kmay89/securaCV/blob/main/docs/root_paradox.md) |
| [15. Governance, Contributions, and Security](#15-governance-contributions-and-security) | [`CONTRIBUTING.md`](https://github.com/kmay89/securaCV/blob/main/CONTRIBUTING.md)<br>[`SECURITY.md`](https://github.com/kmay89/securaCV/blob/main/SECURITY.md) |
| [16. Release Discipline and Roadmap](#16-release-discipline-and-roadmap) | [`v1-roadmap.md`](https://github.com/kmay89/securaCV/blob/main/v1-roadmap.md)<br>[`CHANGELOG.md`](https://github.com/kmay89/securaCV/blob/main/CHANGELOG.md) |
| [17. Appendix A: Tooling and Verification Utilities](#17-appendix-a-tooling-and-verification-utilities) | [`log_verify_README.md`](https://github.com/kmay89/securaCV/blob/main/log_verify_README.md) |
| [18. Appendix B: AI and Agent Interaction](#18-appendix-b-ai-and-agent-interaction) | [`AGENTS.md`](https://github.com/kmay89/securaCV/blob/main/AGENTS.md)<br>[`codex-prompt.md`](https://github.com/kmay89/securaCV/blob/main/codex-prompt.md)<br>[`codex-plan.md`](https://github.com/kmay89/securaCV/blob/main/codex-plan.md) |

---


# 1. Introduction

**Sources:** [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md)

SecuraCV is a system for cryptographically verifiable computer vision events designed to preserve integrity while minimizing surveillance risk. It is not a traditional video capture or storage platform. Instead, it produces signed, append-only witness records describing perceptual events without retaining raw perceptual data by default.

This document unifies the project’s existing documentation into a single maintained reference.

---

# 2. Purpose and Scope

**Sources:** [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md), [`spec.md`](https://github.com/kmay89/securaCV/blob/main/spec.md)

The purpose of SecuraCV is to enable systems to answer the question:

> “Did something happen?”

without enabling:

> “Let me replay everything that ever occurred.”

The scope explicitly includes:
- Integrity verification
- Tamper evidence
- Minimal disclosure
- Interoperable event logging

The scope explicitly excludes:
- General surveillance
- Continuous raw video retention
- Post-hoc expansion of captured data

---

# 3. System Philosophy: Witnessing vs Watching

**Sources:** [`docs/why_witnessing_matters.md`](https://github.com/kmay89/securaCV/blob/main/docs/why_witnessing_matters.md), [`why_this_matters.md`](https://github.com/kmay89/securaCV/blob/main/why_this_matters.md)

SecuraCV is built around the distinction between **witnessing** and **watching**.

Watching prioritizes capture and review. Witnessing prioritizes truthful attestation.

The system is designed to preserve factual occurrence without preserving power over subjects. This philosophy informs every architectural and cryptographic decision in the project.

---

# 4. Core System Overview

**Sources:** [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md), [`kernel/architecture.md`](https://github.com/kmay89/securaCV/blob/main/kernel/architecture.md)

At the center of SecuraCV is the **Privacy Witness Kernel (PWK)**. The kernel:

- Accepts perceptual input
- Processes data in memory
- Emits structured, signed events
- Writes append-only logs

Raw frames are not retained by default. Events are the primary output.

---

# 5. Canonical Invariants

**Sources:** [`spec/invariants.md`](https://github.com/kmay89/securaCV/blob/main/spec/invariants.md)

The following invariants define SecuraCV’s identity:

- Raw perceptual data must not be retained by default
- Logs are append-only and verifiable
- Events must be cryptographically signed
- Time is intentionally coarsened
- Keys are device-bound
- Integrity takes precedence over convenience

Violation of these invariants constitutes departure from the system’s definition.

---

# 6. Event Model and Contracts

**Sources:** [`spec/event_contract.md`](https://github.com/kmay89/securaCV/blob/main/spec/event_contract.md)

An **event** is a structured, signed record representing a witnessed occurrence.

Event properties:
- Deterministic structure
- Cryptographic signature
- Non-invertible feature representation
- Designed for verification, not replay

The event contract defines the boundary between the kernel and all integrations.

---

# 7. Threat Model

**Sources:** [`spec/threat_model.md`](https://github.com/kmay89/securaCV/blob/main/spec/threat_model.md)

The threat model explicitly documents assumptions and limits.

Defended against:
- Log tampering
- Silent modification
- Post-hoc falsification

Not defended against:
- Fully compromised hosts
- Malicious kernel operators

This clarity is intentional and foundational.

---

# 8. System Architecture

**Sources:** [`kernel/architecture.md`](https://github.com/kmay89/securaCV/blob/main/kernel/architecture.md)

The system is composed of:

- Privacy Witness Kernel
- Append-only witness database
- Optional encrypted vault
- Verification tooling

Architectural boundaries enforce separation between integrity and confidentiality.

---

# 9. Integrity, Verification, and Trust

**Sources:** [`log_verify_README.md`](https://github.com/kmay89/securaCV/blob/main/log_verify_README.md), [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md)

Every event is signed using an Ed25519 device key.

Public verifying keys are stored alongside witness data. Verification tools operate independently of the runtime system and can detect tampering or alteration.

---

# 10. Confidentiality and the Vault

**Sources:** [`kernel/architecture.md`](https://github.com/kmay89/securaCV/blob/main/kernel/architecture.md)

Confidentiality is optional and secondary to integrity.

Encrypted payloads may be stored in the vault only under defined rules. Vault data is inaccessible without explicit authorization and does not undermine the integrity model.

---

# 11. Break-Glass Mechanism

**Sources:** [`README.md`](https://github.com/kmay89/securaCV/blob/main/README.md), [`spec/break_glass.md`](https://github.com/kmay89/securaCV/blob/main/spec/break_glass.md)

Break-glass enables exceptional access through quorum authorization.

Properties:
- Trustee-based authorization
- Threshold enforcement
- Explicit audit trail
- No silent unlocks

Break-glass exists to handle rare recovery scenarios without normalizing access.

---

# 12. Integrations Overview

**Sources:** [`docs/integrations/home-assistant-frigate-mqtt.md`](https://github.com/kmay89/securaCV/blob/main/docs/integrations/home-assistant-frigate-mqtt.md), [`docs/homeassistant_setup.md`](https://github.com/kmay89/securaCV/blob/main/docs/homeassistant_setup.md), [`docs/frigate_integration.md`](https://github.com/kmay89/securaCV/blob/main/docs/frigate_integration.md)

SecuraCV integrates with external systems via event streams.

Supported integrations include:
- Home Assistant
- MQTT
- Frigate

Integrations consume events, not raw perceptual data.

---

# 13. Ingestion Backends

**Sources:** [`docs/rtsp_setup.md`](https://github.com/kmay89/securaCV/blob/main/docs/rtsp_setup.md), [`docs/v4l2_setup.md`](https://github.com/kmay89/securaCV/blob/main/docs/v4l2_setup.md)

Supported ingestion paths include:

- RTSP streams
- V4L2 local devices
- ESP32-S3 edge devices

All ingestion backends adhere to the same invariants and event contract.

---

# 14. Operational Limitations (Root Paradox)

**Sources:** [`docs/root_paradox.md`](https://github.com/kmay89/securaCV/blob/main/docs/root_paradox.md)

If the host system is fully compromised, software-only guarantees fail.

This limitation is documented explicitly. SecuraCV does not claim to defeat physical or total system compromise.

---

# 15. Governance, Contributions, and Security

**Sources:** [`CONTRIBUTING.md`](https://github.com/kmay89/securaCV/blob/main/CONTRIBUTING.md), [`SECURITY.md`](https://github.com/kmay89/securaCV/blob/main/SECURITY.md)

Contribution guidelines emphasize:
- Respect for invariants
- Clarity over novelty
- Conservative change

Security disclosures follow documented procedures.

---

# 16. Release Discipline and Roadmap

**Sources:** [`v1-roadmap.md`](https://github.com/kmay89/securaCV/blob/main/v1-roadmap.md), [`CHANGELOG.md`](https://github.com/kmay89/securaCV/blob/main/CHANGELOG.md)

Releases are gated by verification requirements.

Before v1:
- End-to-end integration pipelines must verify
- Tooling must detect tampering
- Documentation must align with behavior

---

# 17. Appendix A: Tooling and Verification Utilities

**Sources:** [`log_verify_README.md`](https://github.com/kmay89/securaCV/blob/main/log_verify_README.md)

Included tools:
- `log_verify`
- `export_verify`

These tools operate independently and validate integrity guarantees.

---

# 18. Appendix B: AI and Agent Interaction

**Sources:** [`AGENTS.md`](https://github.com/kmay89/securaCV/blob/main/AGENTS.md), [`codex-prompt.md`](https://github.com/kmay89/securaCV/blob/main/codex-prompt.md), [`codex-plan.md`](https://github.com/kmay89/securaCV/blob/main/codex-plan.md)

The project includes guidance for AI and automated agents interacting with the codebase.

Agents are expected to respect invariants, avoid speculative expansion, and operate within documented constraints.

---

**End of Unified Document**
