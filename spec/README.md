# Spec documents

Canonical specification documents (authoritative sources; do not duplicate without a pointer):

- `spec/invariants.md`
- `spec/event_contract.md`
- `spec/threat_model.md`
- `kernel/architecture.md`
- `spec/break_glass.md`
- `spec/sensor_adapter_contract_v0.md` — open, vendor-neutral sensor adapter interface
- `spec/witness_mesh_os_v0.md` — unified sensing platform positioning + capability mapping

Per-module conformance specs (the "Hello, World" module templates) live under
`spec/modules/` — e.g. `spec/modules/zone_crossing.md`. The repository's spec entry
point is the root `spec.md` index, which points back into this set.

Legacy HTML exports have been removed; refer to the canonical Markdown sources above.
