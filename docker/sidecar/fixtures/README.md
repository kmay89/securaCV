# Frigate replay fixtures for the sidecar e2e

`ci_e2e.sh` publishes every `frigate/events_*.json` to `frigate/events` and
every `frigate/reviews_*.json` to `frigate/reviews` on a real broker, then
proves that nothing Frigate said about *who* was seen survived: not in the
sealed log, not on the kernel API, not on the bridge's MQTT topics, not in
the daemon log.

Every identity-bearing field Frigate 0.13–0.17 emits is populated with a
recognizable sentinel — strings begin with `SENTINEL_`, numbers begin with
`7770` — and the e2e extracts those tokens from the fixtures and greps the
outputs for them. The field list is the discard list in the kernel's
Frigate transport (`src/transport/frigate.rs`); the fixtures mirror the
Rust integration fixtures under `tests/fixtures/frigate/` with distinct
cameras so each sealed event is attributable.

What the events-mode container must seal: `0.13` (person, `backyard`),
`0.14` (person, `front_door`), `0.16` (car, `Driveway Cam`), `0.17`
(person, `porch_cam`) — four events. `events_0.16_new_face_refused.json`
(label `face`) must seal nothing.

What the reviews-mode container must seal: the three `alert` items (`0.14`
new, `0.16` update-to-alert, `0.17` new with GenAI metadata) — three events.
The detection-only update and the `face`-only alert must seal nothing.

Zone names (`porch`, `lawn`, `driveway`, `street`, `workbench`, …) are what
the owner called a zone and legitimately reach the sealed log, so they are
never sentinels.
