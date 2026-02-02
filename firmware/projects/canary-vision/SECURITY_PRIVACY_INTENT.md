# SECURITY & PRIVACY INTENT (Non-binding)

SecuraCV is designed to support **privacy-preserving sensing** and **tamper-evident auditability**.

This document states the project’s **intent** and **safety principles**. It is **not**
a substitute for legal compliance, and it is **not** an enforceable restriction in the Apache-2.0 license.

## Design intent
- Prefer **semantic outputs** (events/metrics) over raw audio/video storage
- Make “watching” harder and “witnessing” easier
- Provide cryptographic integrity that can be verified independently
- Keep failure modes obvious (misconfig, missing keys, degraded privacy)

## Non-goals
- Stealth surveillance tooling
- Secret exfiltration of raw frames/audio
- Bypassing user consent or local laws

## Recommended operator responsibilities
- Obtain consent where required
- Avoid deployments that create coercive monitoring environments
- Restrict and audit “break-glass” access where applicable
- Document retention and deletion policies
- Keep firmware/software updated and review logs for anomalies

## Reporting issues
If you discover a security flaw that could enable privacy harm or tampering:
- Please report responsibly via the upstream repository issue tracker (mark as security-sensitive),
  or use a private disclosure channel if one is published.

## Safety-by-default suggestions for forks
If you build on this project:
- Keep defaults conservative (no raw capture by default)
- Make “unsafe modes” explicit and opt-in
- Add clear UI/UX warnings where relevant
- Preserve provenance (don’t remove attribution)

---
**Note:** This file is not legal advice.
