# Legal, Safety, and Responsible Use (SecuraCV Provisioning)

This module provisions and hardens embedded devices by configuring security features
(e.g., secure boot, flash encryption, anti-rollback) and burning irreversible eFuses.

## Safety & Irreversibility

- Provisioning is **irreversible** once eFuses are burned.
- Misconfiguration can **permanently brick** devices or lock you out of updates.
- Practice on sacrificial hardware before provisioning production units.

## Responsible Use

SecuraCV is designed to be **owner-respecting** and **auditable**:
- Do not deploy this system for covert surveillance or owner-hostile operation.
- Do not misrepresent SecuraCV as providing anonymity or evasion guarantees.
- Make device behavior and data flows transparent to the device owner/operator.

## No Warranty

This project is provided **AS IS**, without warranties or conditions of any kind.
See the Apache-2.0 license for the full warranty disclaimer and limitation of liability.

## Cryptography Notice

This repository includes cryptographic software and configuration.
You are responsible for complying with any applicable laws, regulations, or policies
that apply to cryptography in your jurisdiction.

## Not Legal Advice

Nothing in this repository constitutes legal advice. If you need legal guidance
about deployment, compliance, or policy, consult qualified counsel.
