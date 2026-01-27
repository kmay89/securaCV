# PQC Mode Policy

This document describes the post‑quantum (PQC) policy knobs and what they protect.

## What is protected

* **Vault encryption**: `vault.crypto_mode` governs how raw media envelopes are encrypted at rest.
  The default is `classical`, which matches today’s ChaCha20‑Poly1305 vault encryption.
* **Transport security**: `transport.tls_mode` governs how transport TLS is selected when we
  connect to external services (e.g., MQTT bridges). The default is `classic-tls`.

## Long‑term integrity

* **Dual signatures**: `signatures.policy` controls signature verification requirements for
  audit tools (`log_verify`, `export_verify`). `dual-optional` allows a second PQ signature
  when available, while `dual-required` enforces both. This protects long‑term auditability
  once PQ signatures are implemented.

## What is *not* claimed

* **No FIPS 140 validation**: PQC modes and hybrid transport do **not** imply FIPS 140
  certification or validation of any cryptographic module.
* **No enforced PQ isolation**: Policies are configuration gates, not a security boundary.
  Implementations must still be audited.
