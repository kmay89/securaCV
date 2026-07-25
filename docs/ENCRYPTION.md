# Encryption & export note

**Status: notification not yet sent** — see the step below; tracked on the
[legal punch list](LEGAL.md#8-the-counsel-punch-list-decisions-docs-cant-close).
*Not legal advice* — this records the project's understanding and its one
outstanding action (from [audit finding L1](legal-audit-2026-07.md)).

## What ships

This repository contains and distributes strong cryptography, all from
standard, publicly available implementations (mbedTLS / RustCrypto-class
libraries — no custom primitives, per the security model):

- **Ed25519** signatures (witness chain, firmware/OTA manifests, device identity)
- **ChaCha20-Poly1305** authenticated encryption (sealed storage)
- **SHA-256** hashing (chain links, artifact digests)
- **TLS** (HTTPS device UI, DoH/DoT-capable components)
- Optional, feature-flagged **post-quantum**: ML-KEM / ML-DSA

## Export posture

The source code is publicly available at no cost, which places it in the
U.S. Export Administration Regulations' published-encryption-source
carve-out (EAR §734.3(b)(3) via §742.15(b)) — **conditioned on a one-time
email notification** to the U.S. Bureau of Industry and Security and the
NSA identifying where the source lives.

**The one outstanding step** (owner action, minutes of work): email
`crypt@bis.doc.gov` and `enc@nsa.gov` with the repository URL and project
name, stating that publicly available encryption source code is published
there, per §742.15(b). Keep the sent mail; update this file's status line
and the punch list with the date.

Until broad international distribution matters, this is hygiene rather
than urgency — but it is cheap, and "sent, on record" is the buttoned-up
state this project aims for everywhere else.
