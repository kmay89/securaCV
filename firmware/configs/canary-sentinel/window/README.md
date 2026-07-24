# Preset: window (Standard tier)

Window / sill guardian. **Deltas on `door`.**

**Intent.** A window is a smaller, closer zone where the telling signals are a
hand or face at the glass and a shadow across the sill. So vs `door`: range
tightened (80/200 cm), ambient-light weighted up (25 → 40), the anomaly
threshold lowered (55 → 45) so covering the light sensor at the glass trips
sooner, and a hair more present-debounce (1.3 s) since a window approach is
deliberate.

**Constraints.** Standard tier — no contact/tamper hardware. Everything else
inherits `door`.
