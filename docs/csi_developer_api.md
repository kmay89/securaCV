# CSI Developer API

The endpoints exposed by the canary-wap firmware for consuming CSI sensing
data, listing the day's events, and (for power users) exploring or tuning
the live model. Privacy is a runtime gate: every endpoint is labeled with
the privacy class it can return, and the chokepoint enforces the class
based on the user's settings.

> All endpoints land in
> `firmware/projects/canary-wap/arduino/canary_wap/csi_integration.{h,cpp}`.
> The same canary-wap reservation count comment in `start_http_server()`
> is the canonical inventory.

---

## Live feed

### `GET /api/csi/stream`

Polling-friendly snapshot at the library's natural 1 Hz cadence. Returns
the most recently committed event (or an "ambient" record derived from
the latest feature window if no event has fired yet) as JSON. Default
privacy class `P0`.

> **Why polling and not SSE?** ESP-IDF's httpd holds a worker per
> request until the handler returns. True long-lived SSE wants the
> async-handler API and on-device validation we haven't done yet. The
> client-side contract is identical to what an SSE upgrade would emit,
> so the Python listener and the dashboard work unchanged when SSE
> lands.

```json
{
  "t": 4,
  "motion": 12,
  "breathing": 3,
  "confidence": "observed",
  "rssi_mean": -42,
  "frames": 18,
  "category": "ambient",
  "privacy": "p0"
}
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

Every key appears on every row — clients decode one shape. Open bundles
(still collecting) are serialized ahead of the committed ring rows:

```json
{
  "events": [
    {
      "id": 4013,
      "module": "core.presence",
      "type": "presence_changed",
      "category": "event",
      "state": "active",
      "confidence": "confirmed",
      "motion": 61,
      "breathing": 0,
      "bpm": 0,
      "duration_sec": 0,
      "bundled": 3,
      "time_bucket": 78,
      "dismissed": 0,
      "open": 1
    },
    {
      "id": 4012,
      "module": "core.presence",
      "type": "presence_changed",
      "category": "event",
      "state": "empty",
      "confidence": "confirmed",
      "motion": 0,
      "breathing": 0,
      "bpm": 0,
      "duration_sec": 1080,
      "bundled": 12,
      "time_bucket": 74,
      "dismissed": 0,
      "open": 0
    }
  ]
}
```

| Field | Notes |
| --- | --- |
| `time_bucket` | 10-minute bucket (0..143). No finer-grain timestamp ever. Derived from the device's monotonic clock plus a clock offset once one is synced (`csi_event_set_clock_offset_minutes`); before a sync it is boot-relative — consistent within a session, not aligned to the wall-clock day. |
| `bundled` | how many raw observations the bundler collapsed into this row |
| `open` | `1` while the bundle is still collecting — the device's own present tense, serialized ahead of the ring; `0` for a committed ring row, which is history. Record vs siren: only an open row may drive live severity; a closed row must never latch it. |
| `state`, `confidence`, ... | fields the originating module's manifest permits |

One envelope field exists beside `events`, because one module's rows are
never open: **`"tamper":{"kind":"<word>"}`** carries the standing
`system.integrity` condition. Tamper rows are sealed-and-closed the moment
they commit (a power-loss record cannot wait out a RAM buffer), so "still
standing" cannot be read off an open row — the device says it outright
instead. Boot kinds stand for the whole boot; SD kinds stand until the card
recovers (and outrank a standing boot kind while they do). **Absent means
nothing to confess** — a client must treat the missing field as calm, never
as unknown-tamper, and may drive its level-triggered tamper flag from this
field exactly as it would from an open row.

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

## First-run pairing (Tier 5)

The captive-portal handler at `/` (and the iOS / Android probe URLs that
the captive-portal popup hits) renders a setup page with a QR code and
a manual fallback link. Both encode `http://192.168.4.1/companion?token=<64hex>`,
where the 64-character hex string is a one-shot pairing token minted on
each render.

### Pairing tokens

Tokens are 32 random bytes generated with `esp_fill_random()`, kept in a
4-slot RAM-only ring (no persistence — a reboot invalidates everything),
and expire after 10 minutes. Validation is constant-time. Single-use:
once consumed the slot is marked and the next `pair_token_consume()`
returns false.

The token is NOT a security boundary — the AP itself is the gate, and
the existing `/api/wifi/connect` handler is reachable to anyone on the
AP. The token is a UX gate that tells the companion PWA "you came
through the portal, run the wizard."

### `GET /api/pair/token`

Issue a fresh pairing token. The captive portal calls
`pair_token_issue()` directly to embed the token in its QR; this route
exists so the companion PWA can refresh a stale token without a full
page reload.

```json
{
  "ok": true,
  "token": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "expires_in_sec": 600,
  "pair_url": "http://192.168.4.1/companion?token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
}
```

### Companion PWA wizard

`/companion?token=<hex>` lights up an HTTP-only 4-card onboarding flow
inside the existing companion PWA. The wizard uses
`/api/wifi/scan` + `/api/wifi/connect` + `/api/wifi` (existing routes)
to drive provisioning; no Bluetooth pairing needed. URLs without a
`?token=` (or with a token that doesn't match `^[0-9a-fA-F]{64}$`)
keep the BLE-driven console flow intact.

---

## Module settings + privacy budget

These two routes are the dashboard's controls surface.

### `GET /api/settings`

Returns the persisted dashboard-surface settings — Pet Mode, sensitivity
preset, quiet-hours window — as a flat JSON envelope. Read-only NVS
open; falls back to declared defaults for unset keys.

```json
{
  "ok": true,
  "pet_mode": false,
  "preset": "balanced",
  "sensitivity": 50,
  "quiet_hours": { "enabled": false, "start_min": 0, "end_min": 480 }
}
```

### `POST /api/settings`

Writes one or more dashboard settings, then drives `reinit_module()`
for the affected module(s) so the new value lands on the next tick.
The wire keys are deliberately short (the dashboard's controls surface,
not the full module-tunable surface):

| Wire key | Type | Meaning |
| --- | --- | --- |
| `"pet_mode"` | bool | Pet Mode toggle. |
| `"preset"` | string | `"sensitive"` / `"balanced"` / `"quiet"`. |
| `"sensitivity"` | int 0..100 | Slider; ±20 around the preset baseline. |
| `"quiet_hours"` | object | `{ "enabled": bool, "start_min": int 0..1439, "end_min": int 0..1439 }`. |

```bash
curl -X POST http://canary.local/api/settings \
     -H 'Content-Type: application/json' \
     -d '{"pet_mode": true, "preset": "quiet"}'
```

For per-coefficient access (the full module-style keys like
`core.presence.motion_threshold`, `anomaly.baseline.spike_ratio`,
etc.), use `/api/tune/coefficients` from the Tuning Lab section
above — that surface is the authoritative read/write for every
NVS-backed coefficient.

### `GET /api/privacy-budget`

Literal byte counter for outbound traffic plus the current privacy
ceiling. The dashboard surfaces this as the "Today: 0 bytes left the
device" pill in the Today sheet.

```json
{
  "bytes_today": 0,
  "ceiling": "p0",
  "since_ms": 12345
}
```

The counter is incremented from the host when host code sends data to
an off-device destination (MQTT, SD-export, BLE-paired phone export).
Local fetches against the dashboard's own polling routes do NOT count.

---

## PWA shell

`/manifest.webmanifest` and `/sw.js` give the dashboard a real PWA
identity (Add to Home Screen, standalone display, offline shell).
`/api/csi/stream`, `/api/events/*`, `/api/settings`, `/api/privacy-budget`
are explicitly **passed through** by the service worker so live
data always hits the device, never a stale cache.

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
`firmware/common/csi/csi_event_invariants_test.cpp` (host-build) proves the
enforcement is real, not aspirational.

## Witness-chain payload format

Every committed P0 / P1 event is persisted to the witness chain via the
strong override of `csi_event_commit_witness()` in `csi_integration.cpp`,
which calls `csi_witness_emit_event()` in `canary_wap.ino`. The signed
body is an ASCII string built by `csi_witness_build_payload()`
(`firmware/common/csi/src/csi_witness_payload.{h,cpp}` — host-buildable
so the privacy-invariants fuzzer can assert the format every CI run):

```
csi <module> <type> <category> <state> <conf>
    m=<motion> b=<breathing> bpm=<bpm> d=<duration> bk=<bucket>
    kv=<firmware_version> rs=<ruleset_id> zn=<zone_id>
```

The trailing `kv=` / `rs=` / `zn=` fields satisfy
`spec/event_contract.md` §2 — every event MUST carry the firmware
version that produced it, the ruleset it was scored against, and the
zone it fired in. Verifiers can replay the chain against a different
ruleset by inspecting `rs=`; a mismatch invalidates the row.

`zn=` defaults to the compile-time `ZONE_ID` constant. Setting NVS key
`core.zone_id` (string, ≤ 31 bytes) to a non-empty value overrides it
without recompiling. The override is read once per boot.

## Quiet Hours gating

The dashboard's Quiet Hours range (NVS keys `qh.en`, `qh.start`, `qh.end`)
is wired into the chokepoint via `csi_event_set_quiet_window(start_min,
end_min, enabled)`. While the configured window is active, the chokepoint
suppresses non-anomaly emits and increments an internal hold counter
instead. At the first emit AFTER the window closes (or when the user
disables Quiet Hours mid-window), the chokepoint synthesizes one
`held_summary` row through the registered `meta.quiet_hours` module.
The summary's `note` is `"quiet_hours"` and `bundled` reflects
the number of suppressed events. **Anomaly events (`CSI_CATEGORY_ANOMALY`)
always pass through** — the night-time category is precisely when
unusual activity matters most.

The host wires this in `firmware/projects/canary-wap/arduino/canary_wap/
csi_integration.cpp::register_v1_modules()` (boot-time NVS read) and
the `/api/settings` POST handler (live re-apply on dashboard change).
