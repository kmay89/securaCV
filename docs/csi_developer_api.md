# CSI Developer API

The endpoints exposed by the canary-wap firmware for consuming CSI sensing
data, listing the day's events, and (for power users) exploring or tuning
the live model. Privacy is a runtime gate: every endpoint is labelled with
the privacy class it can return, and the chokepoint enforces the class
based on the user's settings.

> Phase 4 of the WiFi CSI Tool plan. Endpoints land in
> `firmware/projects/canary-wap/arduino/canary_wap/csi_stream_api.{h,cpp}`.

---

## Live feed

### `GET /api/csi/stream`

Server-Sent Events, one JSON line per second, default privacy class `P0`.

```
event: csi
data: {"t":4,"motion":12,"breathing":3,"confidence":"observed","rssi_mean":-42,"frames":18}
```

| Field | Type | Notes |
| --- | --- | --- |
| `t` | int | seconds since the stream opened — never wallclock |
| `motion` | int 0..100 | reduced from the Doppler band of the feature vector |
| `breathing` | int 0..100 | reduced from the breathing FFT band |
| `confidence` | string | `tentative` / `observed` / `confirmed` |
| `rssi_mean` | int | dBm (negative) |
| `frames` | int | CSI frames in the closing window (~ 18-20 healthy) |

### `GET /api/csi/stream?include=window`

The same SSE stream, but each event also carries the raw 32-dimension
`int8` feature vector under `window`. Privacy class **P2** — never persists,
never leaves the device, never available unless the user has raised the
privacy ceiling in settings (the Tuning Lab is the typical caller).

### `GET /api/csi/window`

One-shot polling endpoint that returns the most recent feature window as
JSON. Same privacy class as the SSE variant requested.

---

## Today's events

### `GET /api/events/today`

Coarse-bucketed list of events from the in-memory ring (backed by the
witness chain for persistence). Reads through the existing
`witness_chain` export path — no new persistence layer.

```json
{
  "events": [
    {
      "id": 4012,
      "module": "core.presence",
      "type": "presence_changed",
      "category": "event",
      "state": "active",
      "confidence": "confirmed",
      "duration_sec": 1080,
      "bundled_count": 12,
      "time_bucket": 78,
      "dismissed": 0
    }
  ]
}
```

| Field | Notes |
| --- | --- |
| `time_bucket` | 10-minute bucket (0..143). No finer-grain timestamp ever. |
| `bundled_count` | how many raw observations the bundler collapsed into this row |
| `state`, `confidence`, ... | fields the originating module's manifest permits |

### `POST /api/events/dismiss`

Tells the ring "the user marked this row as 'that was nothing.'" Local-only.
The originating module's `on_event_dismissed()` runs and may nudge its own
thresholds. Nothing leaves the device.

```bash
curl -X POST http://canary.local/api/events/dismiss \
     -H 'Content-Type: application/json' \
     -d '{"event_id": 4012}'
```

---

## Tuning Lab (power users)

All routes are privacy class **P2**. Never reachable until the user opens
the Lab from the dashboard (long-press on the version chip, or
`?tune=1` query).

| Route | Purpose |
| --- | --- |
| `GET /tune` | the Tuning Lab UI |
| `GET /api/tune/coefficients` | every registered tuning knob, current values |
| `POST /api/tune/coefficients` | update one knob; persists to NVS |
| `GET /api/tune/preset` | export a signed JSON tuning bundle |
| `POST /api/tune/preset` | import a signed JSON tuning bundle |

Tuning bundles ride the existing witness-chain export format — no new
persistence layer.

---

## Optional CSV recorder

For researchers familiar with the [ESP32-CSI-Tool](https://github.com/StevenMHernandez/ESP32-CSI-Tool)
column convention.

### `POST /api/csi/record/start`

Begins recording the live feed to SD as CSV with the same column layout
that ESP32-CSI-Tool emits, so existing community Python / MATLAB notebooks
work unchanged. **P2** — must be explicitly enabled in settings.

### `POST /api/csi/record/stop`

Closes the file and emits a witness-chain entry pointing to the artifact.

---

## Privacy classes (recap)

| Class | What can be read by this API |
| --- | --- |
| `P0` | Aggregate counts and bucketed scalars only. The default ceiling. |
| `P1` | The above + numeric estimates (e.g. BPM). Requires opt-in in settings. |
| `P2` | The above + raw 32-dim feature vector and tuning state. Never persists; never leaves the device. |

The chokepoint enforces these at runtime. The fuzzer at
`firmware/common/csi/csi_event_invariants_test.cpp` proves the
enforcement is real, not aspirational.
