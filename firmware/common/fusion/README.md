# sentinel.fusion — the anti-evasion fusion brain

Board-agnostic evidence fusion for **Canary Sentinel**, the multi-sensor
doorway/window guardian. This module is the *decision core*: it takes coarse
votes from physically independent sensing channels and fuses them into one
debounced, privacy-preserving people-detection decision.

It is a **leaf** module — like `common/csi/core_multilink_fusion` — with no
Arduino / ESP-IDF / RTOS dependency, no allocation, and no reach into any other
module's state. The composition layer (`projects/canary-sentinel`) adapts each
sensor driver's output into a `Vote` and feeds it here; the coarsened result is
what the witness signs and publishes. Time is injected (`now_ms`) so the engine
is fully deterministic and host-tested under
`g++ -std=c++17 -Wall -Wextra -Werror` (`firmware/tests_host/test_sentinel_fusion.cpp`).

## Why this exists

OR-ing sensors maximises false alarms. AND-ing them hands an intruder a single
channel to defeat. Sentinel does neither — it **scores evidence** and rewards
agreement across *independent physical modalities*.

| Channel | Physical modality | How you'd evade it | What still catches you |
|---|---|---|---|
| PIR | thermal (body heat in motion) | move slowly, insulate | radar sees micro-motion; CSI sees you |
| 60GHz radar | radio reflection | hold utterly still | you still breathe — radar locks breathing; CSI perturbs |
| WiFi CSI | channel perturbation | — (device-free) | your body still bends the RF field crossing the door |
| WiFi RF / BLE | carried-radio emission | leave your phone at home | radar + CSI + PIR don't care about your phone |
| Ambient light | optical | cross in the dark | heat + radio still radiate |
| Contact / tamper | mechanical | don't touch the door | you didn't get in without touching it |

To be invisible to a fully-populated Sentinel you would have to, at the same
instant a body crosses a threshold, **emit no heat, reflect no radar, not
perturb the WiFi, carry no powered radio, cast no optical change, and touch
nothing mechanical.** Each is individually evadable; all of them at once is not.
That is the entire thesis: *corroboration across independent physics.*

## The fraud-detection posture

This is where the ATM / anti-skimming lineage shows up, and it is what makes the
engine more than an OR-gate:

1. **Independence is weighted, honestly.** The independence bonus counts distinct
   *modality classes*, not channels. WiFi-RF and BLE share the `CarriedRadio`
   class on purpose — leave your phone at home and both die together, so they
   must not each count as independent corroboration. Confirmation requires
   `min_confirm_modalities` (default 2) *independent* classes.

2. **A blinded channel is suspicion, not absence.** A `Denied` vote (the channel
   is enabled and expected to report but is covered / jammed / stalled) raises
   the anomaly accumulator instead of lowering the score. Blinding a sensor
   *while a body is present* is the textbook evasion attempt, so it is amplified
   and escalates straight to `Anomaly` — no debounce wait.

3. **Uncorroborated is not the same as clear.** A body-present modality (radar or
   CSI) that says *Strong* with nothing else corroborating is a **silent body**:
   plausible (a still, device-free person) but uncorroborated. It never reaches
   `Confirmed`, and if it dwells it is surfaced as `Anomaly` — looked at, never
   silently accepted.

Just like a transaction that is plausible on one axis but inconsistent across the
others: you don't trust one signal, you look for consistency, and you treat the
*absence of expected corroboration* as a flag.

## Decision levels

`Clear → Aware → Present → Confirmed → Loiter`, with `Anomaly` as an overlay that
wins over everything and latches.

- **Clear** — nothing corroborated.
- **Aware** — a single weak / low-independence indication.
- **Present** — corroborated evidence over the present threshold.
- **Confirmed** — ≥ `min_confirm_modalities` independent classes agree.
- **Loiter** — a *corroborated* body sustained past the dwell timer.
- **Anomaly** — inconsistent / blinded / tampered, or an uncorroborated body that
  lingers. The fraud flag.

## Privacy

The engine never sees a MAC, a distance in centimeters, a per-target track,
imagery, or vitals — it sees `Vote`s. It emits an ordinal `Level`, a `0/1/2+`
occupant bucket, a `near/mid/far` band, a `0..100` confidence, and a bitmask of
which *modality classes* corroborated — never which device, never who. That
coarse `FusionResult` is the only thing the composition layer is permitted to
publish.

## API sketch

```cpp
using namespace securacv::fusion;

FusionEngine engine(preset_config);      // ChannelSpec weights come from configs/

// each tick, for every channel that produced a reading:
engine.observe(Channel::Radar, Vote::Strong, /*quality*/ 90, now_ms);
engine.observe(Channel::Pir,   Vote::Weak,   70,            now_ms);
engine.set_range(RangeBand::Near);        // coarse side-band from radar
engine.set_occupancy(Occupancy::One);     // coarse side-band from a counter

FusionResult r = engine.evaluate(now_ms); // fuse + advance FSM
if (r.changed) publish(r);                // r.level, r.confidence, r.occupancy…
```

## Tests

```
cd firmware/tests_host && make    # builds + runs every host suite, incl. this one
```

`test_sentinel_fusion.cpp` pins: modality grouping honesty, single-channel →
Present (never Confirmed), independence → Confirmed, same-modality pair does not
fake independence, denied-channel suspicion + blind-while-present → Anomaly,
silent-body dwell → Anomaly, corroborated dwell → Loiter, rising/clear debounce,
and staleness decay.

## License

Apache-2.0 (repository license).
