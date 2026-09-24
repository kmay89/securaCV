# Writing a CSI Module

A **module** is the smallest unit of expandable sensing behavior in the
SecuraCV CSI library. Each module receives the 1 Hz feature stream and
publishes domain events through the privacy chokepoint. The runtime
enforces a per-module manifest that declares exactly which fields each
event type can carry — that's the teeth behind the privacy contract.

This guide is for someone who wants to add a new sensing behavior (a sleep
detector, a fall detector, a "kettle just boiled" detector, anything that
fits inside one ESP32-S3) without touching SecuraCV core.

---

## The interface

```c
typedef struct csi_module {
  const char*               id;             // "<scope>.<name>"
  csi_privacy_class_t       default_privacy;
  const csi_event_decl_t*   events;
  size_t                    event_count;

  void (*init)(const csi_module_settings_t*);
  void (*tick)(const csi_features_t*);     // 1 Hz
  void (*on_event_dismissed)(uint32_t event_id);
  void (*deinit)(void);
} csi_module_t;
```

A module declares (a) the events it can emit, (b) the fields each event
carries, (c) its default settings. The runtime calls `tick()` once per CSI
window; modules may call `csi_event_emit()` from inside `tick()`. Modules
**must not** reach into each other's state.

### The breathing time base

Windows are loop-driven, so "once per window" is *about* once a second,
not exactly. The breathing bins (`v[12..19]`, one Goertzel tap per
0.05 Hz) assume one envelope sample per second, and would drift with CPU
load if the ring were fed per window. They are not: `csi_features.cpp`
resamples the envelope onto a fixed 1 Hz grid keyed by each window's
close timestamp — an early close in the same one-second slot is averaged
in, a late close first holds the previous sample across the skipped
seconds. `csi_stats_t` reports what that cost: `windows_merged`,
`windows_held` and `window_period_ms` (the mean close-to-close interval).
A loop that keeps pace shows 0 / 0 / ~1000.

The envelope itself is each subcarrier band's *share* of the
AGC-normalized frame, not raw received power. The receiver re-gains per
packet, so raw power would carry every gain step as a broadband transient;
a band share cannot move with gain, and a breath's frequency-selective
re-weighting moves the bands against each other. Both properties are
pinned by the host test (`tests_host/test_csi_features.cpp`) with
synthetic frames, including a simulated per-packet AGC and 700 ms /
1300 ms / stalled window cadences. That is a **compile- and host-tested**
claim: no bench numbers exist for the breathing path on hardware yet, so
treat the on-device breathing rate as unverified until a bench log says
otherwise.

### The second extractor (wander and jitter)

Off by default. [`csi_wander_jitter.h`](../firmware/common/csi/src/csi_wander_jitter.h)
defines two more per-window scalars, and only a build compiled with
`-DCSI_WANDER_JITTER=1` writes them, into two of the four slots the vector
kept in reserve:

| Slot | Name | Definition, within one window | Scale |
| --- | --- | --- | --- |
| `v[28]` | wander | Population standard deviation, over the window's frames, of the per-frame amplitude centroid Σ k·a<sub>k</sub> / Σ a<sub>k</sub> (a tone index). | 64 per tone of spread; clips at 127 (about 2 tones). |
| `v[29]` | jitter | Mean frame-to-frame \|Δa<sub>k</sub>\| per tone over the window's consecutive frame pairs. | 128 × (mean \|Δ\| ÷ the row mean); clips at 127. |

a<sub>k</sub> is the same AGC-normalized amplitude row `v[0..7]` is built
from (every frame rescaled to a mean of 64), so a per-packet gain step moves
neither. Both are **within one window**: no baseline and no memory across
windows. A room whose multipath profile is tilted but not changing reads
wander ≈ 0 (position is not motion). Anything that changes the rows during
the window raises both slots: motion, and front-end noise too. On the host
fixture, 8 LSB of extra I/Q noise on a still channel reads about half the
moving scatterer's wander and more than its jitter, so neither slot is a
motion-only reading. Without the flag `v[28..29]` are zero, which is the
layout every shipped build has, and `csi_features::wander_jitter_enabled()`
tells a host which build it is. `v[30..31]` stay reserved (v2.1 C6
sounding, v2.2 phase unwrap).

These are our own definitions, written in the style of espressif/esp-radar's
`waveform_wander` and `waveform_jitter`. esp-radar ships its metric math as
a binary, so this is not a port and makes no claim to match it.

**Both floors depend on the link.** Normalization rescales the
quantization noise along with the row, so a weak link reads a higher
still-room wander and jitter than a strong one: on the host fixture, a link
at 0.35× gain reads about like 2 LSB of extra I/Q noise on a full-strength
one. Compare either slot with that link's own still reading, never with a
fixed number.

**Where the values show up** in a flag-on build:

- the canary-wap's P2 route
  [`GET /api/csi/window`](csi_developer_api.md#get-apicsiwindow), and so the
  last cells of the dashboard's Tinker heatmap, which paints that vector
  cell by cell;
- `meta.empty_room_baseline`'s in-RAM mean, which sums all 32 slots while a
  calibration runs. No firmware path starts a calibration today, and
  nothing reads the mean;
- the canary PIO tree computes them too (it compiles the same
  `csi_features.cpp`), but its sensing bridge copies only `v[0..27]`
  (`securacv_sensing.cpp`), so nothing there shows them.

**What does not change:** no module reads either slot, and no event,
witness record, MQTT field, `/api/status` field or Home Assistant entity
carries them. No allow-list was touched.

**Status:** host-tested on synthetic frames; flag off in every shipped
build; no bench numbers, no thresholds, not read by any module.
`tests_host/test_csi_wander_jitter.cpp` pins the math on hand-built rows,
and `test_csi_features.cpp` is built a second time with the flag on to pin
the pipeline: a still room, per-packet gain flicker and a static tilt read
near-zero wander, and gain flicker leaves jitter at its floor; the
moving-scatterer fixture reads on both; a slow drift scales wander and
leaves jitter at its floor; extra I/Q noise scales jitter and raises
wander; a weak link raises both still floors. The flag-off build asserts
`v[28..31]` stay zero. CI compiles the flag-on path for the S3 on the
canary-wap's second Arduino pass.

**Bench recipe** (the bench half of roadmap §5 step 4, after step 3's bench
pass): build the canary-wap with `-DCSI_WANDER_JITTER=1`, raise the privacy
ceiling to `p2`, poll `GET /api/csi/window` once a second, and record
`v[28]` / `v[29]` beside `v[0..11]` for an empty room, a person sitting
still and a person walking. Do it on an S3 and a C3 (the canary-wap builds
for both). The C6 has no canary-wap build, and
`firmware/examples/csi_minimal` prints only the reduced scalars today, so a
C6 row first needs a sketch that prints the two slots. Note dropped or
all-zero frames beside each reading: a frame with no amplitude still counts
toward jitter, and one all-zero frame in a steady window reads jitter about
as high as the moving-scatterer fixture (13 against 14 on the host). Until
that log exists, a threshold on either slot would be a guess.

## The manifest

```c
const csi_event_decl_t EVENTS[] = {
  {
    .type_name                = "door_event",
    .allowed_fields           = CSI_FIELD_STATE_NAME
                              | CSI_FIELD_CONFIDENCE
                              | CSI_FIELD_TIME_BUCKET
                              | CSI_FIELD_MOTION_SCORE,
    .privacy                  = CSI_PRIVACY_P0,
    .default_ceiling_per_hour = 30,
  },
};
```

Every event carries:

- **`type_name`** — short, stable, ASCII, never user-typed.
- **`allowed_fields`** — bitmask of `csi_event_field_t` values. Anything a
  module tries to set outside this list is zeroed before the event is
  persisted, exported, or shown.
- **`privacy`** — `P0` (always-on, contract-conformant), `P1` (opt-in),
  `P2` (power-user / developer disclosure, never leaves the device).
- **`default_ceiling_per_hour`** — fail-safe cap. The bundler is the
  primary anti-noise mechanism; the ceiling is a secondary guard.

## Privacy class quick guide

| Class | When to pick it | Examples |
| --- | --- | --- |
| `P0` | Always-on aggregate counts and bucketed scalars. | presence state changes, ribbon bucket advances, daily summaries. |
| `P1` | Opt-in. Anonymous but more detailed (numeric estimates). | breathing rate in BPM. |
| `P2` | Power-user disclosure. Never persists. | the raw 32-dim feature vector, tuning knobs. |

If you're unsure, **pick `P0`** and keep the payload minimal.

## What `tick()` looks like

```c
static uint16_t s_consecutive = 0;

static void on_tick(const csi_features_t* f) {
  const uint8_t motion = reduce_doppler_band(f->v);
  if (motion >= TRIGGER) {
    if (++s_consecutive >= CONFIRM) {
      csi_event_values_t v;
      csi_event_values_init(&v);
      v.category       = CSI_CATEGORY_EVENT;
      v.present_fields = CSI_FIELD_STATE_NAME
                       | CSI_FIELD_CONFIDENCE
                       | CSI_FIELD_TIME_BUCKET
                       | CSI_FIELD_MOTION_SCORE;
      strncpy(v.state_name, "fall_suspected", sizeof(v.state_name) - 1);
      strncpy(v.confidence, "observed",       sizeof(v.confidence) - 1);
      v.motion_score = motion;
      (void)csi_event_emit("third.fall_detect", "fall_suspected", &v);
      s_consecutive = 0;
    }
  } else {
    s_consecutive = 0;
  }
}
```

## Bundling and ceilings come for free

You don't have to deduplicate same-state events yourself. The bundler in
`csi_event` collapses observations of the same `(module, type, state)`
tuple within a 10-minute window into one row with an aggregated duration.
You emit; the runtime does the rest.

If your module would naturally emit hundreds of events per hour during
a noisy period, set `default_ceiling_per_hour` defensively — the runtime
caps the burst and the bundler still surfaces a single summary row.

## Dismiss feedback

The dashboard's "That was nothing" swipe routes to your
`on_event_dismissed(event_id)`. A small nudge to a threshold is the
typical response — never log the dismissal off-device.

```c
static void on_dismiss(uint32_t /*event_id*/) {
  if (s_threshold < 100) s_threshold++;
}
```

## Settings (NVS-backed)

`init()` receives a settings handle. Read typed values with the helpers:

```c
static uint8_t s_threshold = 30;
static void on_init(const csi_module_settings_t* s) {
  s_threshold = (uint8_t)csi_module_settings_int(
      s, "third.fall_detect.threshold", 30);
}
```

Settings keys must start with the module id — that's the convention the
host uses to namespace NVS storage and the Tuning Lab UI.

## Registering at boot

```c
#include <csi_module.h>
#include "third_fall_detect.h"

void register_csi_modules() {
  csi_module_register(third_fall_detect_module());
}
```

That's the whole story. The chokepoint, bundler, ceiling, witness chain,
SSE stream, dashboard ribbon, and Tuning Lab all keep working without
further wiring.

## A complete worked example

See `firmware/examples/modules/stub_door_opens.{h,cpp}`.

---

## Modules currently shipped

These ship in `firmware/common/csi/src/` and register in any host that
calls the canary-wap or canary-PIO integration helpers. Add your own
alongside them — the host's `register_v1_modules()` is a one-line edit.

| Id | Privacy | Events emitted | What it does |
| --- | --- | --- | --- |
| `core.presence` | P0 | `presence_changed` | RF-presence FSM. Each transition emits `presence_changed` with `state_name` carrying the FSM state (`empty` / `sensing` / `subtle` / `quiet` / `active` / `together`). Honors the `pet_mode` toggle by gating breathing-confirmation on a sustained Goertzel lock in the human band. |
| `core.breathing` | P0 | `breathing_confirmed`, `breathing_lost` | Goertzel lock on the 0.15–0.45 Hz band. Promotes confidence to `confirmed` after the configured confirm-window of consecutive locks. |
| `core.activity_ribbon` | P0 | `ribbon_bucket_advanced` | Writes the 96-slot 15-minute ring that the dashboard renders as the aurora-strip activity ribbon. NVS-persisted. |
| `meta.daily_summary` | P0 | `daily_summary` | One row per day at the bucket boundary: total active minutes, longest quiet stretch, anomaly count. |
| `anomaly.baseline` | P0 | `unusual_motion`, `unusual_breathing` | 60-window rolling baseline of motion / breathing scalars; emits when the current sample exceeds the baseline by `spike_ratio` (default 2.5×) AND clears the absolute floor. Per-channel cooldown prevents notification floods; ranges are clamped at NVS read so a corrupt slot can't break the detector. |
| `core.multilink_fusion` | P0 | `motion_confirmed` | Two-link motion confirmation: emits on the rising edge when the local window and at least one fresh paired peer's window (under 3 s old) both clear the motion threshold. Registered in both trees, but no production path calls `core_multilink_fusion_ingest_peer_features()` yet, so with no peer windows it never fires. Host-tested (`firmware/common/csi/test_core_multilink_fusion.cpp`). |
| `meta.empty_room_baseline` | P0 | `baseline_status` | Empty-room calibration: while a calibration runs, accumulates each window's 32-slot vector into an in-RAM mean and emits `baseline_status` (`calibrated` / `canceled` / `failed`) with the window count. Registered in both trees; nothing in the firmware starts a calibration yet, nothing reads the mean, and the mean lives in RAM only. |
| `wifi.channel_activity` | P0 | `channel_active` | Ambient, unattributed "the airwaves got busy" glow from the RSSI spread (`v[21]`) and the dropped-frame estimate (`v[25]`) against the room's own rolling baseline. `CSI_CATEGORY_AMBIENT`: the chokepoint never persists it (live UI only). Registered in both trees. |
| `meta.quiet_hours` | P0 | `held_summary` | Manifest only: the chokepoint synthesizes one `held_summary` row when a configured quiet window closes, and registering this module is what lets that emit pass the allow-list. canary-wap only. |
| `system.integrity` | P0 | `tamper` | The device's own integrity story: `unexpected_reboot`, `watchdog`, `power_loss` from the reset reason, `sd_error` / `sd_remove` from the SD state, and `enclosure` only on a `FEATURE_TAMPER_GPIO` build that feeds a real contact. `CSI_CATEGORY_ANOMALY`; each emit is sealed at once. Registered in both trees. |
| `ble.events` | P0 | `ble_initialized`, `ble_init_failed`, `ble_client_connected`, `ble_client_disconnected`, `chirp_sent`, `chirp_received`, `canary_discovered`, `canary_lost` | Chokepoint-routed manifest for the eight BLE Discovery semantic events declared in `spec/event_contract.md` §10. Per-event allow-lists strip MAC-precision fields (motion / breathing / RSSI proxies); peer identification uses the truncated Ed25519 pubkey hash (≤16 hex chars) carried in `note`. Helpers in `ble_events_module.h` (e.g. `ble_events_emit_chirp_sent("boot")`) are the only legitimate BLE → witness-chain path going forward — direct `create_witness_record()` calls from the BLE stack would bypass the privacy contract. |

### Anomaly baseline tunables (live in the Tuning Lab)

| NVS key (full) | Default | Range | Meaning |
| --- | --- | --- | --- |
| `anomaly.baseline.spike_ratio` | 250 | 110..1000 | Percent of the rolling baseline the current sample must exceed (250 = 2.5×). |
| `anomaly.baseline.min_motion` | 60 | 1..100 | Absolute floor below which motion anomalies don't fire, regardless of baseline ratio. |
| `anomaly.baseline.min_breathing` | 50 | 1..100 | Same, for the breathing channel. |
| `anomaly.baseline.cooldown_sec` | 600 | 30..3600 | Per-channel cooldown after a fire. |

The two anomaly event types share an hourly limit (`ANOMALY_CEILING_PER_HOUR = 10`) because the chokepoint enforces the cap **per module**, not per event type — equal ceilings are how we stop one channel from starving the other.
