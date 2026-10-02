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

There is no `?include=window` variant of the stream. An earlier version of
this page documented one; it was never built, and `handle_stream` reads no
query string. The raw vector has exactly one route, the next one.

### `GET /api/csi/window`

The most recently closed window's raw 32-dimension `int8` feature vector,
one snapshot per request (the dashboard's Tinker view polls it about once a
second while that panel is open, and paints it as the raw heatmap).
Privacy class **P2**: the route answers `403` with
`{"error":"raw window requires P2 privacy ceiling"}` until the user raises
the ceiling to `p2` (`"privacy_ceiling": "p2"` through `POST /api/settings`),
and `204 No Content` before the first window has closed. Bearer- or
session-authenticated like the rest of `/api/csi/*`; nothing persists it.

```json
{ "frames": 19, "time_bucket": 57, "v": [3, 2, 0, 1, 4, 2, 1, 0, 1, -1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, -52, 2, -49, -56, 19, 0, 6, 0, 0, 0, 0, 0] }
```

| Field | Type | Notes |
| --- | --- | --- |
| `frames` | int | CSI frames in the window (~ 18-20 healthy) |
| `time_bucket` | int 0..143 | the window's 10-minute bucket of the day |
| `v` | int[32] | the feature vector; layout in `firmware/common/csi/src/csi_features.h` |

`v[0..7]` are amplitude variance per band, `v[8..11]` band rotation,
`v[12..19]` breathing bins, `v[20..23]` RSSI mean / std / max / min and
`v[24..27]` frames / dropped / channel / bandwidth code. `v[28]` (wander) and
`v[29]` (jitter) carry values only in a build compiled with
`-DCSI_WANDER_JITTER=1` and read `0` otherwise
([`csi_modules.md`](csi_modules.md#the-second-extractor-wander-and-jitter):
host-tested on synthetic frames; flag off in every shipped build; no bench
numbers, no thresholds, not read by any module). `v[30..31]` are reserved
and always `0`.

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
      "id": 2147483649,
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
      "id": 3221229508,
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
| `id` | A uint32 (never decode it as a signed 32-bit integer). A committed row's `id` is its event id: every committed row, bundled or not, takes it when it commits, from one allocator, so ids rise in commit order and keep rising across reboots, from 3221225472 (0xC0000000) on every device (`firmware/common/csi/src/csi_event_id_floor.h`, `kIdSpaceBase`). An open row has no event id yet: its `id` is the bundle's handle, in [2147483648, 3221225472) (0x80000000 up to 0xC0000000), stable while the bundle is open and never equal to an event id. Use a committed row's `id` for `POST /api/events/dismiss`; an open row cannot be dismissed. |
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
reaches the tamper topic through the power-events classifier.

Every row that names a state, other than an ambient one, goes through the
bundler: a `core.presence` state, `core.breathing`'s confirmed and lost,
`anomaly.baseline`'s unusual motion, `core.multilink_fusion`'s confirmed
motion, `meta.empty_room_baseline`'s status and, on BLE builds, a
`ble.scout` arrival or departure. Such a row is committed, and published,
when its bundle closes: two minutes after its last observation, or ten
minutes after it opened, whichever comes first. So it reaches the broker
two to ten minutes after its first observation, and a row that never merges
(an `anomaly.baseline` row, whose cooldown is ten minutes by default) two
minutes after it. The body's `timestamp` is the close, and rows arrive in
the order their bundles close, not the order their states began. A return
to a state within two minutes joins the bundle still open for it, so a
row's span can take in a brief other state. The row carries every
observation it collapsed (`bundled`, the same count live, from the offline
queue and in a replay, at least 1 for a row committed directly) and the
span from the first to the last (`duration_sec`). A bundle still open at a
reboot or a power cut is never committed; `system.integrity` closes its own
key the moment it emits for that reason, so a tamper commits at once. Both
trees close bundles the same way, once per main loop (`csi_bundler_tick`);
until sweep F81 the canary base closed every bundle after each CSI window,
so each of its observations was a row of its own, committed within a
second. A module's hourly ceiling counts rows: a bundle that opens spends
one slot, an observation merged into its open bundle spends none, and a
bundle that reopens after its ten minutes or its quiet gap spends one like
any other opening (sweep F80). The ceiling's hour is its six 10-minute
buckets, not a sliding 60 minutes, so one sliding hour can hold up to twice
the ceiling's rows (sweep F132). A state held for an hour therefore spends
six slots on its own rows, which is all of `core.presence`'s six an hour:
until one ages out, the next transition is refused, and so is every further
observation of the held state (measured on the host: after an hour or more
in one state the transition waits up to about ten minutes, 7 to 602 s
depending on when a slot ages out, and the held state's rows carry one
observation each).

Both trees also keep an SD event log, `/EVENTS/today.ndjson`, one committed
row per line in one shared format
(`firmware/common/csi/src/csi_event_log_line.h`, so a tool reads either
card), and backfill Home Assistant from it after a broker outage, marking
those bodies `"replay":true`. On the canary base
(`src/csi_event_egress.cpp` over the loop-task adapter
`src/csi_event_log.cpp`; the rules are
`firmware/common/csi/src/csi_event_backfill.h`, host-tested):

- a row goes out live only when nothing older is waiting on the card;
  otherwise it waits its turn, so a new row never overtakes an older one;
- the backfill runs once the MQTT offline queue has drained (queued tamper
  alerts first), walks the log in id order, sends at most two rows per loop
  pass and about twenty a second, and never sends an id at or below the
  highest one already handed to the broker, because Home Assistant's replay
  gate refuses an `event_id` below the last one it verified. That watermark
  survives a reboot through an NVS ceiling written with the event-id floor's
  policy, so a reboot inside an outage skips at most ten undelivered rows
  and republishes none. Every row, the bundled ones (presence,
  `system.integrity`) included, takes its id when it commits, from one
  allocator, so the log is in id order and ids rise across reboots (sweep
  F46). That allocator starts at 0xC0000000 on every device, above every id
  an older firmware handed out (its bundler's ids started at 0x80000000 each
  boot), so an upgraded device's next rows are above Home Assistant's
  stored mark and nothing is reset, on the device or in Home Assistant. The
  space holds 2^30 ids; at the wrap ids restart at 1 and Home Assistant
  refuses the device from then on. Both devices' MQTT health carries
  `event_id_space_low` (sweep F82), true once the allocator's next id
  reaches 0xF0000000 (2^28 ids before the wrap, about four years at the
  most a device can commit) and after a wrap. It only warns: what recovers
  the device is still a decision. At boot the id floor is held at or above the delivery ceiling (NVS
  `csi.evsent`), unless that ceiling is past 0xF0000000 (an older firmware
  wrote one for a forged card line): the floor and the backfill both treat
  such a ceiling as no record. A card line at or above the allocator's next
  id is never sent and never counted as delivered, so a forged id cannot
  push Home Assistant's mark or the id floor toward the wrap. Below that
  bound the log is trusted input: the backfill signs and sends what a line
  says;
- a row committed while the link was up but held behind the backlog is sent
  with `"replay":false`, since it is news; everything else the backfill sends
  says `"replay":true`;
- the tamper-topic bridge publishes at commit, whatever the backfill is
  doing;
- the log is bound to the device's witness key by `/EVENTS/owner`; a card
  whose log belongs to another device (or to a canary-wap) is left untouched
  and not replayed. The canary-wap writes no owner file and leaves a card
  that has one alone: it does not append to that log or replay it. A
  canary-wap on firmware from before that rule does not know the file, and
  would append its own rows to a canary's log, which that canary then
  replays under its own key;
- a row the card cannot take waits in RAM (8 rows, the oldest dropped
  first) instead of overtaking the card's rows (sweeps F103, F104): one
  committed while the card is not open but may hold older rows (from boot
  until its log first opens, and after it closes with rows still waiting,
  for at most 45 s, the canary-wap's wait), and one whose append failed
  while older rows wait or the broker is unreachable. It goes in id order
  with the card's rows once nothing older waits, and writes no NVS delivery
  ceiling until then, so a reboot never reads the card's rows as delivered
  on its account. Past the 45 s the card is given up: the waiting rows go,
  and rows on a card that comes back later are never sent (the backfill
  starts past them, and no counter shows them). An ambient row
  (`wifi.channel_activity`, "live UI only") is never held: one that cannot
  go at once is dropped and counted. RAM does not survive a reboot, and a
  broker change drops what waits (owed to neither broker, as the offline
  queue's flush).
  So on a canary with an SD slot but no usable card (none in, another
  device's, or one that never mounts), a row committed in the first 45 s
  after boot arrives up to 45 s late, or not at all if the canary reboots
  or its broker changes first: such a canary in a boot loop shorter than
  45 s delivers no events-topic row (tamper alerts do not wait). A failed
  append can still have landed every byte but the newline; the next append
  seals that line and the backfill sends it, and the waiting copy, now at
  or below the delivered watermark, is dropped, not sent twice;
- with no card, rows use the MQTT offline queue (12 records) as before, where
  tamper alerts outrank events: once the queue is full, a new row pushes out
  the oldest queued event, never a tamper alert. A body built while the
  broker is unreachable says `"replay":true`. While the queue still drains
  an outage, a row handed to it joins its back instead of going live, so it
  never overtakes the outage's rows (`mqtt_offline_queue.h`'s
  `publish_or_queue()`, sweep F107). With no broker configured, rows
  are logged and owed to nobody, and a broker configured later (or a changed
  one) does not receive the old backlog;
- what the egress did since boot rides its MQTT health (sweep F109) as a
  `csi_event_egress` object, under the names the canary-wap's
  `csi_event_egress::stats()` uses, beside an `offline_queue` object (the
  MQTT layer's queue). They count paths, not a ledger of rows: a row can
  pass through two of them, and some rows pass through none (below). They
  start over at every boot. The canary-wap keeps the same `csi_event_egress`
  counters but publishes none of them yet, and the Home Assistant
  integration reads neither. Each one counts:
  - `dropped`: commits the full egress queue refused (the loop task was
    stuck). The row is on neither the card nor the wire.
  - `held_dropped`: rows the RAM hold dropped to make room, oldest first,
    and, on the canary, rows that had to wait while the hold had no memory.
  - `ambient_dropped`: ambient rows that had to wait (they are never held).
  - `unsent_dropped`: rows no card kept that were lost at the hand-over to
    the MQTT layer: it refused them (an offline queue with no memory, or one
    full of tamper alerts while the broker is unreachable) and nothing kept
    them. The canary-wap has no offline queue (a refused row waits in its
    hold), so there it counts only rows whose body would not build, which a
    canary body always does.
  - `planner.live`: rows on the card sent at once.
  - `planner.held`: rows on the card left for the backfill. Each is counted
    again in `planner.replayed` when the backfill sends it, but `held` minus
    `replayed` is not "still owed": `replayed` also counts rows an earlier
    boot left on the card, and a broker change or a card given up after its
    45 s wait abandons held rows without counting them.
  - `planner.queued`: rows not on the card handed to the MQTT layer, sent
    live or into its offline queue, and rows from the RAM hold that went
    once nothing older waited. On a canary with no card every row lands
    here, sent at once or not; a row the offline queue later evicts is still
    counted here and is in `offline_queue.dropped_overflow`.
  - `planner.replayed`: rows the backfill sent from the card, an earlier
    boot's included.
  - `planner.skipped`: card lines (or damaged runs) the backfill walked
    past: delivered already, torn, foreign to the format, or (also counted
    as `planner.untrusted`) carrying an id this device never handed out. A
    card that opens with nothing owed on it is not walked, so its lines are
    not counted.
  - `planner.unsendable`: card lines the backfill could never send: a body
    that would not build, and on the canary-wap a dismissal line (never
    replayed).
  - `planner.truncated_unsent`: retention cuts of the log that dropped rows
    still waiting.
  - `planner.read_giveups`: walks abandoned after repeated failed card reads.
  - `offline_queue.dropped_overflow`, `dropped_oversize`, `dropped_flushed`
    (canary only): records the MQTT offline queue evicted or refused when
    full, refused as larger than a slot, or discarded at a broker change.
    Events and tamper alerts share the queue, so these count both. An event
    a full queue of tamper alerts refuses with the broker unreachable is in
    `dropped_overflow` and in `unsent_dropped`.

  Rows in no counter: a RAM-held row sent ahead of a card row (it leaves
  through the backfill's send, which counts only the card row), rows the
  hold drops at a broker change (owed to nobody), card rows abandoned when
  the 45 s card wait runs out or the broker changes, and a held row whose
  card copy was already sent.

The canary-wap (`csi_event_egress.cpp` over its `csi_event_log.cpp` adapter,
sweep F78) runs the same planner with the same order: a row goes out live
only when nothing older waits, the backfill walks the card in id order, two
rows per pass, and never below the delivered watermark, which survives a
reboot through the same NVS ceiling. As on the canary base, with no broker
configured, or after the broker changes (host, port, user or topic prefix),
the rows waiting are owed to nobody and the new broker is not sent them. It
differs from the canary base in four ways:

- the commit hook only queues the row (16 deep; a full queue drops and
  counts). Logging, publishing and the watermark all happen on the loop
  task, so a row ble.scout commits on the NimBLE host task neither
  publishes there nor races the backfill;
- it has no MQTT offline queue. Rows its card does not keep wait in RAM
  instead (8 rows, the oldest dropped first) while anything older waits or
  the broker is unreachable, and go out in id order with the card's rows.
  These are closed bundles (presence, `system.integrity` tampers), which
  never reach its card (sweep F77), every row when there is no card or the
  card is not open, and a row whose card append failed. "Anything older"
  includes a card that is not open but may hold older rows: from boot until
  its log first opens (a slow card mounts after boot), and after it closes
  with rows still waiting (an SD error's remount), for at most 45 s; past
  that the RAM rows go, and rows on a card that comes back later are never
  sent (no counter shows them). An ambient row (`wifi.channel_activity`, "live UI only") is never
  held: one that cannot go out at once is dropped and counted. RAM does not
  survive a reboot. The canary base holds the same rows the same way (its
  card keeps closed bundles, so those are not among them), but once nothing
  older waits and no card is open its offline queue takes them while the
  broker is unreachable;
- it writes no owner file and leaves a card that has one alone;
- the tamper-topic bridge publishes when the loop task takes the row from
  the queue, before the row itself, whatever the backfill is doing.

A dismissal line on its card (`"dismissed":1`) is the owner's local record
and is never replayed. A dismissal the log cannot take yet (no open log, or
a log at its size cap, which only a committed row's append cuts) waits in
RAM, up to eight, until it can; a reboot drops it.

### `POST /api/events/dismiss`

Tells the ring "the user marked this row as 'that was nothing.'" Local-only.
The originating module's `on_event_dismissed()` runs and may nudge its own
thresholds. Nothing leaves the device.

```bash
curl -X POST http://canary.local/api/events/dismiss \
     -H 'Content-Type: application/json' \
     -d '{"event_id": 3221229508}'
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
| `POST /api/tune/coefficients` | update one or more knobs; persists to NVS; applies at once and from every boot |
| `GET /api/tune/preset` | export every knob's current value as one flat, unsigned JSON object (a download named `tuning-preset.json`) |
| `POST /api/tune/preset` | import such an object: the same handler as `POST /api/tune/coefficients`, which checks no signature |

A tuning bundle is that flat object, one `"<full_key>": value` pair per
knob (`{"core.presence.preset":1,"core.presence.sensitivity":50,...}`). It
is not signed and it is not part of the witness chain or its export: an
import checks only that each key names a knob and its value is a number
(or `true` / `false`), and clamps each value to that knob's range. The Lab
saves a bundle as a file your browser downloads and loads one from a file
you pick; the device keeps no copy, only the knob values it stores.

A coefficient POST re-runs the `init()` of the module it belongs to
(`core.presence`, `core.breathing`, `anomaly.baseline`), so the new value
lands on the next tick, and every module reads its stored values in its
boot `init()` (sweep F93). The three `core.quiet_hours.*` knobs belong to
no module: a POST that stores one re-applies the stored Quiet Hours window
to the chokepoint at once, as a Quiet Hours change through
`POST /api/settings` does (sweep F128), and every boot applies it too.
Their declared defaults are the device's own, off and 23:00 to 07:00
(sweep F123), so a device that never stored them shows the Lab, and
exports, the window it runs. A bundle import is the same handler,
so it stores every coefficient in the bundle, the three presence
thresholds included. So do the Lab's per-row **reset** and **Reset all**,
which POST each coefficient's default as a stored value. A stored
`core.presence.motion_threshold`, `active_threshold` or
`breathing_threshold` wins over the dashboard's preset and sensitivity
(they set only the default of those reads), at boot as after a change.

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
  "pet_mode": false,
  "preset": "balanced",
  "sensitivity": 50,
  "quiet_hours": { "enabled": false, "start_min": 1380, "end_min": 420 },
  "privacy_ceiling": "p0",
  "filter_foreign": true,
  "tz": "EST5EDT,M3.2.0,M11.1.0",
  "tz_iana": "America/New_York"
}
```

Apart from the zone, that is what a device that never stored a setting
reports. Its Quiet Hours read off, 1380 to 420 (23:00 to 07:00): the one
default the chokepoint and the Tuning Lab share (sweep F123).

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
Saved values also apply from every boot: the modules read them in their
boot `init()` (sweep F93; before it, a boot ran on the modules' defaults
until the next settings change). The calibration's apply and the Tuning
Lab's coefficients behave the same way, its Quiet Hours knobs included
(sweep F128; see the Tuning Lab section above). The
preset and sensitivity set only the default of `core.presence`'s three
thresholds: once a threshold is stored directly (the calibration's apply
stores all three; so do the Tuning Lab's reset buttons and a bundle
import), the stored threshold wins, at boot as at once, while
`GET /api/settings` keeps reporting the saved preset.
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
csi_settings_nvs.cpp::apply_quiet_hours_from_nvs()`, which
`csi_integration.cpp`'s `register_v1_modules()` calls at boot, the
`/api/settings` POST handler calls on a Quiet Hours change, and the Tuning
Lab's POST (`csi_tune_lab.cpp`'s `tune_post()`, also the bundle import)
calls when it stores a `core.quiet_hours.*` knob (sweep F128). A device
that never stored the range runs it off, 23:00 to 07:00
(`kQuietHoursDefault*` in `csi_settings_nvs.h`, sweep F123).

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
