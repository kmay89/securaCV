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
field exactly as it would from an open row. The kind words are a gated
vocabulary: `system_integrity_kinds` in `spec/witness_dictionary.json`,
which `scripts/lint_dictionary_sync.py` holds equal to the module's
literals and to Home Assistant's per-type tamper sensors. Both firmware
trees register the module and feed it a live SD state, so both can narrate
`sd_error` and `sd_remove`.

### MQTT `securacv/<id>/events`

When a broker is configured, every committed row is also published on
`securacv/<id>/events` as one JSON body built by
`firmware/common/csi/src/csi_event_wire.h`. The canary-wap sketch
(`csi_mqtt.cpp`) and the canary PIO tree (`src/csi_event_egress.cpp`) share
that builder, and `firmware/tests_host/test_csi_event_wire.cpp` pins its
bytes. The body carries an Ed25519 signature over the `event` canonical
(`firmware/common/identity/device_signature`), which Home Assistant verifies
against the device's pinned key. Both trees publish that key as
`public_key` in their MQTT health payload, and the integration pins it on
first sight ([device_trust.md](device_trust.md)); until a key is pinned,
the body reads as unverified (`no_pubkey`). `"signed"` is `true` only when
a signature rides the body. `system.integrity` rows are also republished on
`securacv/<id>/tamper` as `{"type":"<kind>","severity":"tamper"}`, the shape
the integration's per-type tamper sensors match. On the canary base that
bridge carries the SD and enclosure kinds only: its boot story already
reaches the tamper topic through the power-events classifier. The canary-wap
also keeps an SD event log and backfills Home Assistant after a broker
outage, marking those bodies `"replay":true`. The canary base relies on its
MQTT offline queue (12 records), where tamper alerts outrank events: once
the queue is full, a new row pushes out the oldest queued event, never a
tamper alert. A body built while the broker is unreachable also says
`"replay":true`. SD backfill on the canary base is a recorded follow-up.

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
  "quiet_hours": { "enabled": false, "start_min": 0, "end_min": 480 },
  "privacy_ceiling": "p0",
  "filter_foreign": true,
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "tz_iana": "America/New_York"
}
```

`tz` / `tz_iana` are the household time zone (repo sweep F28, option A —
maintainer to confirm): the POSIX rule the device applies, and the IANA name
it was mapped from when it came from one. Both are `""` while no zone is set,
and then the device keeps **UTC**, exactly as before the setting existed.
Once a zone is set, three things that used to run on UTC follow household
time instead: the CSI day offset (`time_bucket` and quiet hours), the
30-day Chirp self-test's waking-hours gate (it only sounds between 06:00
and 22:00), and **Chirp night mode** (22:00 to 06:00, when templates not
allowed at night are refused with `night_restricted` and `GET /api/chirp`
reports `night_mode: true`).

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
| `"tz"` | string | Household time zone as a POSIX rule (≤ 47 characters, e.g. `"CET-1CEST,M3.5.0,M10.5.0/3"`). `""` alone clears the zone (back to UTC). The rule must fit the strict POSIX grammar in `tz_rule::posix_valid`: names of 3 to 10 letters (or `<…>`), offsets up to 24 h, and a zone with summer time names **both** change dates; nothing may trail it. Anything else is refused (`400`, `"bad time zone"`) and nothing in the body is written, because the C library under the ESP32 Arduino core 2.0.x (newlib 4.1) does not fall back to UTC on a rule it cannot read: it keeps part of the previous zone. |
| `"tz_iana"` | string | The same, as an IANA zone name (`"Europe/Berlin"`), mapped on the device through the fleet's table (`firmware/common/time/tz_rule.h`). A zone the table does not know is refused (`400`, `"unknown zone"`) — never stored, never silently UTC. A typed `"tz"` wins when both are sent. |
| `"filter_foreign"` | bool | CSI transmitter filter: accept frames only from the router this Canary is associated with (and registered peer Canaries); everything else is counted under `frames_dropped_foreign` on `/api/status` and never buffered. Default on. Off restores every decoded frame on the channel. Applied to the HAL at once and persisted; `/api/status` reports `filter_armed` (the setting is on and the Canary has associated, so the filter is comparing). |

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
is collected in the household's local time. The chokepoint compares it
against the device's own clock, which follows the household time zone once
one is set (`tz` above — seeded at setup from the phone's zone, which the
setup wizard sends as `tz_iana` on `/api/wifi/connect`; applied with
`setenv("TZ")` + `tzset()`, the device has no SNTP) and UTC until then. The
same zone sets where the 10-minute `time_bucket` day starts. The Quiet
Hours range is wired into the chokepoint via `csi_event_set_quiet_window(start_min,
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

---

## Household time zone on the canary PIO tree

The canary PlatformIO tree has no module-settings surface, so its
`/api/settings` (both methods, bearer-auth gated) carries only the zone:
`GET` answers `{ok, tz, tz_iana}` and `POST` takes the same `tz` /
`tz_iana` keys with the same rules as above (errors `unknown_zone` /
`bad_time_zone`), answering with the stored values. Its setup page sends
the phone's zone as `tz_iana` with the join, and the web UI's Settings
panel has a Time zone card (use this browser's zone, a POSIX rule, or back
to UTC). Storage: NVS `securacv`/`tz` and `tz_iana`.

## BLE Scout pairing (canary PIO tree, `[env:full]`)

Unlike the rest of this page, these five routes live in the **canary
PlatformIO tree** (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`),
compiled only where `FEATURE_BLE_SCAN=1` (`[env:full]`). The canary-wap
sketch carries the same Scout module and pairing window but no route for it
yet. All five are bearer-auth gated and rate limited like every other `/api`
route.

Pairing is a **proximity window**, never a typed address (repo sweep F27,
option B — maintainer to confirm): arm a window with a name, hold the tag
against the Canary, and the first advert from a beacon that is not already
paired, at or above the signal threshold, pairs inside the Bluetooth scan
callback. The MAC is hashed there with the device's own key and dropped, so
**no MAC crosses this API in either direction**. `hashed_id` is that keyed
hash as 32 lowercase hex characters; the same tag has a different
`hashed_id` on every other Canary. The paired list persists across reboots
(one versioned NVS blob, written from the loop task).

| Route | Body | Answer |
| --- | --- | --- |
| `GET /api/scout` | — | `{ok, count, max, beacons:[{hashed_id, label}]}` |
| `POST /api/scout/pair/start` | `{label, window_s?, rssi_min?}` | the window status below; `400 bad_label` / `400 bad_window` / `409 window_busy` / `409 registry_full` / `503 scout_not_ready` |
| `GET /api/scout/pair/status` | — | `{ok, state, label, window_s, remaining_s, rssi_min, hashed_id?}` |
| `POST /api/scout/pair/cancel` | — | the window status plus `canceled` (bool) |
| `POST /api/scout/unpair` | `{hashed_id}` | `{ok, count}`; `400 bad_hashed_id` / `404 not_paired` |

- `label`: 1 to 23 printable ASCII characters (refused, not rewritten).
- `window_s`: default and maximum 60 (larger values are clamped to 60,
  smaller than 5 to 5).
- `rssi_min`: default −45 dBm ("held against it"), clamped to −70..−20 dBm
  so a window can never pair "anything in the house".
- `state`: `idle`, `armed`, `pairing` (an advert won; finishing),
  `paired` (then `hashed_id` names the new tag), `failed` (every slot
  full), `expired`, `canceled`.

Limits worth saying out loud: the window pairs the **first** single advert
at or above the threshold from any unpaired device. There is no debounce
and no strongest-wins rule, and the scan callback sees only an address and
a signal level, not what kind of device sent it. So three things other
than the tag can win at −45 dBm:

- **your own phone**, in your hand after you pressed Pair;
- **another Canary** on the same shelf, whose fleet-link adverts are
  unpaired Bluetooth adverts like any other;
- someone else's phone held right against the Canary.

The window is at most 60 s, the name is yours, and forgetting the wrong
device is one call (`POST /api/scout/unpair`), so the remedy is to unpair
and pair again with the phone and other Canaries a step back. Most phones
rotate their Bluetooth address every few minutes, so a paired phone stops
matching; a tag with a fixed address is the reliable choice. Requiring two
or three qualifying adverts from the same device, and skipping adverts that
parse as a fleet-link or Chirp beacon, are the planned tightenings; both
wait on a live pair against a real beacon, which is bench work (U1).

```bash
curl -X POST http://canary.local/api/scout/pair/start \
     -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     -d '{"label": "Keys"}'
```
