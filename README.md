# Witness Kernel

Core library and tools for the Privacy Witness Kernel (PWK).

## Documentation

* Canonical specifications:
  * `spec/invariants.md`
  * `spec/event_contract.md`
  * `spec/threat_model.md`
  * `kernel/architecture.md`
* Other docs should link to the canonical specifications rather than duplicate them.
* Contribution guidance is in `CONTRIBUTING.md`.
* Security policy is in `SECURITY.md`.
* Release notes are in `CHANGELOG.md`.

## Device public key location

The device Ed25519 **verifying key** is stored locally in the witness database at
`device_metadata.public_key` (row `id = 1`). External verification tools like
`log_verify` read this public key from the database by default, or accept an
explicit key via `--public-key` / `--public-key-file` as documented in
`log_verify_README.md`.
