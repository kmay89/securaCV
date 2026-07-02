# canary-sense / default — presence-only

Presence, target-count, and lux witness on the MR60BHA2 kit. Vitals code is
compiled out (`-DCANARY_SENSE_VITALS` is **not** set for the matching env).

## Intended behavior

- Debounced `PresenceInRestrictedZone` claims from the radar presence FSM.
- Bucketed occupant count (0 / 1 / 2+) — never a per-target track log.
- Coarse range band (near/mid/far) for host-side zone gating; raw distance
  never leaves the device.
- BH1750 lux for tamper corroboration ("lights-out + presence").

## Constraints

- Feature flags live here; pin maps live in `boards/xiao-esp32c6-mr60/`.
- `FEATURE_VITALS 0` is the readable mirror of the env build flag — keep the two
  consistent. The build flag, not this header, gates the vitals translation
  unit in `common/sensors/mmwave_mr60`.
- Bound env: `canary-sense-default` (see `envs/platformio/canary-sense.ini`).
