# Canary Sense — Wellbeing Tile & Blueprints (MR60BHA2 radar)

The **canary-sense** witness is a 60GHz mmWave radar (Seeed MR60BHA2). It
produces presence, occupant count, a breathing-lock signal, optional
breath/heart rate, and ambient illuminance — with **no camera, no microphone,
and no identity surface**. This page documents the Home Assistant automation
blueprints that ship with it and a stock-card **wellbeing tile** for the
welfare-check deployment.

> **Vitals are wellbeing signals, not medical data.** Radar breathing/heart
> estimates are non-diagnostic: ~90% / ~85% accuracy, effective only at
> ≤1.5 m, for a single occupant, and mounting-sensitive. Everything here is a
> *prompt for a human*, never life-safety or medical monitoring.

## Entities

The integration names canary-sense entities on the usual
`securacv_canary_<id>_<key>` pattern (replace `<id>` with your device id, e.g.
`bedroom`). Presence, illuminance and the breathing-lock binary are **P0**
(always present). The numeric BPM sensors are **P1 opt-in** and exist **only**
when the device advertises the vitals opt-in in its discovery payload.

| Entity | Class | Privacy |
|--------|-------|---------|
| `binary_sensor.securacv_canary_<id>_presence` | occupancy | P0 |
| `sensor.securacv_canary_<id>_occupants` | bucketed 0 / 1 / 2+ | P0 |
| `binary_sensor.securacv_canary_<id>_breathing` | breathing-lock | P0 |
| `sensor.securacv_canary_<id>_breath_rate` | BPM | **P1 opt-in** |
| `sensor.securacv_canary_<id>_heart_rate` | BPM | **P1 opt-in** |
| `sensor.securacv_canary_<id>_illuminance` | illuminance (lx) | P0 |
| `sensor.securacv_canary_<id>_range_band` | near/mid/far (diagnostic) | P2 |
| `sensor.securacv_canary_<id>_radar_link` | radar-link health (diagnostic) | — |

Not sure of your exact ids? **Developer Tools → States**, filter `securacv`.

## Blueprints

Three blueprints ship alongside the canary alert and daily-digest ones. Import
each via **Settings → Automations → Blueprints → Import Blueprint** and paste
its URL:

### a) After-hours presence alert
`docs/blueprints/canary_sense_after_hours_presence.yaml`

Radar presence turns on inside a configured overnight window (default
22:00–06:00) → notify. Radar sees static, covered and sleeping occupants in
total darkness a camera can't. Optional **require corroboration** toggle: only
alert when a second, independent witness (e.g. a camera person sensor) also
reads on at the same time — a two-physics corroborated event.

Import URL:
`https://github.com/kmay89/securaCV/blob/main/docs/blueprints/canary_sense_after_hours_presence.yaml`

### b) Lights-out-with-presence tamper notice
`docs/blueprints/canary_sense_lights_out_tamper.yaml`

Illuminance falls below a threshold and stays there for a short window **while
presence stays on** → notify. The room went dark but the occupant didn't
leave: a possible enclosure cover-up or a light cut to blind a co-located
camera. Radar is unaffected by the darkness, which is what makes it a good
cross-check. Inputs: lux entity, presence entity, lux threshold, drop window,
notify service.

Import URL:
`https://github.com/kmay89/securaCV/blob/main/docs/blueprints/canary_sense_lights_out_tamper.yaml`

### c) Welfare check — no breathing lock during sleep window
`docs/blueprints/canary_sense_welfare_check.yaml`

**Welfare prompt, not a medical alarm.** During a configured sleep window,
presence is on but the breathing binary has been off for N consecutive minutes
→ notify a human to look in. A lost breathing lock means "the radar can no
longer confirm breathing" (person rolled over, sat up, left frame, or a second
person entered), **not** "the person stopped breathing". Works on the **P0
breathing-lock binary alone** — the P1 opt-in BPM sensors are not required.
Inputs: presence entity, breathing entity, sleep window, minutes-without-lock,
notify service.

Import URL:
`https://github.com/kmay89/securaCV/blob/main/docs/blueprints/canary_sense_welfare_check.yaml`

## Wellbeing tile (stock cards, no custom card)

The welfare-check product face is a small dashboard tile built entirely from
**built-in** Home Assistant cards — presence, breathing-lock, a "last
confirmed" timestamp, and (only if you opted into P1) a BPM history graph.
Stack them in a dashboard column. Replace `<id>` with your device id.

```yaml
# --- Wellbeing tile: always-available P0 signals ---
type: entities
title: Bedroom — wellbeing
show_header_toggle: false
state_color: true
entities:
  - entity: binary_sensor.securacv_canary_<id>_presence
    name: Present
  - entity: binary_sensor.securacv_canary_<id>_breathing
    name: Breathing confirmed
  - entity: sensor.securacv_canary_<id>_occupants
    name: Occupants
  - type: attribute
    entity: binary_sensor.securacv_canary_<id>_breathing
    attribute: last_changed
    name: Last confirmed
  - entity: sensor.securacv_canary_<id>_illuminance
    name: Light
  - entity: sensor.securacv_canary_<id>_radar_link
    name: Radar link
```

A `glance` card gives a more at-a-glance status header:

```yaml
type: glance
title: Bedroom — at a glance
state_color: true
entities:
  - entity: binary_sensor.securacv_canary_<id>_presence
    name: Present
  - entity: binary_sensor.securacv_canary_<id>_breathing
    name: Breathing
  - entity: sensor.securacv_canary_<id>_occupants
    name: Occupants
```

If — and only if — you opted into the **P1** BPM sensors, add a history graph
below the tile. These entities do not exist unless vitals are opted in, so this
card is optional:

```yaml
# --- Optional: P1 opt-in BPM trend (breath_rate / heart_rate) ---
type: history-graph
title: Bedroom — vitals (non-diagnostic, ~90% acc, ≤1.5 m, single occupant)
hours_to_show: 8
entities:
  - entity: sensor.securacv_canary_<id>_breath_rate
    name: Breaths/min
  - entity: sensor.securacv_canary_<id>_heart_rate
    name: Heart rate
```

> The graph title carries the non-diagnostic caveat on purpose — keep it there
> so nobody reads the trend as a medical vital.

Pair the tile with blueprint (c) so the dashboard shows the live state and the
automation does the prompting. For the wider verified-event view, see
[`docs/lovelace_timeline.md`](../lovelace_timeline.md).
