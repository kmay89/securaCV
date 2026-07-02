# canary-sense / wellbeing — presence + vitals

Everything in the `default` flavor plus the P1-gated wellbeing channel:
breathing-confirmed lock (P0 binary) and breath/heart-rate BPM numerics (P1).

## Intended behavior

- All `default` presence/count/lux behavior.
- Vitals lock FSM (`mr60_vitals`) compiled in via `-DCANARY_SENSE_VITALS=1` on
  the `canary-sense-wellbeing` env.
- P0 "breathing confirmed" binary lock; P1 BPM numerics surface as HA entities
  only when `FEATURE_VITALS_BPM_P1` is set in the discovery payload.
- Vitals are hard-suppressed unless exactly one target is present.

## Privacy constraints (design doc §2.2)

- Vitals are **wellbeing signals, not medical data**: never sealed-logged,
  never precise-timestamped, health/status channel only, P1 opt-in for numerics.
- The contract enforcer allowlists only `PresenceInRestrictedZone`
  (+ `TamperDetected`) for this device — a vitals payload physically cannot seal.

## Constraints

- Bound env: `canary-sense-wellbeing` (see `envs/platformio/canary-sense.ini`).
- `FEATURE_VITALS 1` here must stay consistent with the env's
  `-DCANARY_SENSE_VITALS=1`.
