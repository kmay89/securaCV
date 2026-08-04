# Preset: door (Standard tier)

Front-door / entry-threshold guardian and the **reference preset** — the other
presets are expressed as deltas on this one.

**Intent.** Balanced weights across all five Standard-tier modalities (PIR,
radar, WiFi-CSI, WiFi-RF, BLE, light), a fast present-debounce (1.0 s) so a
real approach commits quickly, and a 30 s loiter timer so someone who lingers at
the door is surfaced. Range bands near/mid/far at 150/350 cm.

**Behavior.** Two independent modalities agreeing → `Confirmed`. A body seen
only by radar/CSI with nothing corroborating → `Present`, and if it dwells →
`Anomaly` (silent-body rule on). Blinding a sensor while a body is present →
`Anomaly` immediately.

**Constraints.** Standard tier: no contact/tamper/vision hardware
(`FEATURE_CONTACT/VISION/TAMPER = 0`). Values are data consumed by the project's
`sentinel_config.h`; tune at runtime over the same NVS number entities as
canary-sense.
