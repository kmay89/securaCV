# Network Coexistence

How SecuraCV's mesh networking (Opera ESP-NOW + Chirp broadcasts) shares the
2.4 GHz band with your home WiFi, why it used to fight it, and how the
current channel + airtime policy keeps multi-Canary deployments invisible to
your router.

## TL;DR

- Each Canary has **one 2.4 GHz radio**. ESP-NOW, Opera mesh, Chirp, AP mode,
  STA mode, and CSI sensing all share it.
- The mesh now **follows the radio's current channel** instead of pinning
  itself to channel 1 or 6. When STA is associated with your home WiFi, the
  mesh rides on that channel; the radio never has to retune mid-send.
- An **airtime governor** caps routine traffic (heartbeats, gossip, and the
  WAP's CSI active probe) at **≤ 2%** of any rolling 10-second window, by
  its own estimate of airtime. The probe stops at 1.60 % of that window so
  the heartbeats and presence beacons keep their room. Urgent traffic —
  tamper alerts, `OFFLINE_IMMINENT`, power-loss notifications — bypasses
  the cap but is still counted, so the `mesh_airtime_pct` telemetry stays
  honest.

If you only care about the upshot: **a SecuraCV deployment with a handful of
Canaries should be undetectable in your router's airtime stats.**

## The bug we fixed

Earlier firmware declared:

```c
// firmware/projects/canary-wap/arduino/canary_wap/mesh_network.h
static const uint8_t ESPNOW_CHANNEL = 1;   // Opera
static const uint8_t CHIRP_CHANNEL  = 6;   // Chirp broadcasts
```

…and registered ESP-NOW peers with `peer.channel = ESPNOW_CHANNEL`. The idea
was that Opera and Chirp should live on different channels so they wouldn't
crosstalk. The problem: on the ESP32, the 2.4 GHz radio is single-channel.
When STA is connected to a home WiFi AP on channel 11, the radio is *fixed*
to channel 11 by the WiFi MAC. Asking ESP-NOW to send "on channel 1" forces
the driver to retune the radio, which either silently drops your STA
association for a moment or — worse — keeps causing brief glitches that
your router sees as a flaky client.

In the field this looked like:

- Spotify drops out for half a second every 30 seconds when the mesh
  heartbeats fire
- Home-Assistant push notifications occasionally fail to deliver
- Wi-Fi Mesh / 802.11k roaming gets confused
- CSI sensing fires false "Drop: rate-limit" because the radio is too busy
  recovering from channel switches to receive its own probe replies

## The new policy

Two small modules in `firmware/projects/canary-wap/arduino/canary_wap/`:

### `mesh_channel_policy.{h,cpp}`

A pure decision function:

```
sta_connected && sta_channel > 0  →  channel = sta_channel  (locked to STA)
sta off, ap_active                →  channel = ap_channel   (locked to AP)
neither                           →  channel = 6            (fallback)
```

On the firmware build, the policy samples `WiFi.status()` / `WiFi.channel()`
each iteration of `mesh_network::update()`. When the effective channel
changes, listeners fire — the Opera implementation uses one to drop the
ESP-NOW broadcast peer so it re-registers cleanly on the new channel.

ESP-NOW peer entries are now created with `peer.channel = 0`, which the
ESP-IDF treats as "use current radio channel." This is the only correct
setting on a single-radio chip; the radio is owned by the WiFi MAC, and
ESP-NOW just rides it.

### `airtime_governor.{h,cpp}`

A rolling-window airtime accountant. Each TX is recorded with an estimated
airtime cost:

```
airtime_us ≈ 192us preamble + (bytes * 8) / 1 Mbps
```

(1 Mbps is the conservative fallback rate ESP-NOW uses for broadcasts; real
unicast traffic is faster.) It is an estimate, not a measurement of the air,
and it counts only the bytes the caller passes: the CSI probe adds its ~59 B
of ESP-NOW framing (a 16 B probe is charged as 75 B, 792 µs), while the mesh,
chirp and Beacon callers pass header + payload and do not yet, so their
estimates run about 59 B a frame short.

Sends are summed in 100 ms buckets: a send joins the newest bucket when it
falls in the same 100 ms, and a bucket leaves the window with its newest
send. The 256-slot ring therefore reaches back at least 25.6 s at any send
rate, so nothing still in the window is ever overwritten, and the window
reads 10.0–10.1 s — never less, so the cap can only err toward denying.
That holds for a reader whose clock trails the newest send too (the MQTT
telemetry reads the window with a time taken before that loop pass's mesh
and probe sends): a bucket stamped less than 100 ms after the reader's time
is counted whole.
(The ring used to hold one slot per send; above 25.6 reservations a second
it dropped in-window sends and the cap stopped holding.)

Routine traffic — heartbeats every 30 s, presence beacons every 60 s,
gossip, and the WAP's CSI active probe (a 10 Hz broadcast today, ~0.8 % by
the estimate) — calls `try_reserve_routine()`. If the projected airtime in
the rolling 10-second window would exceed the cap (default 2%), the send is
denied and the caller skips this tick. The peer-stale timer is 90 s, so a
few skipped heartbeats are harmless.

The probe goes through `probe_airtime.h`, which also stops it once the
window reads 1.60 %: a probe fanned out to a full peer table asks for far
more than the cap, and without the ceiling it took every microsecond the
window freed, so heartbeats and presence were refused outright. The rest of
the 2 % (about 0.39 %: all of the remaining 0.40 % but the one 792 µs probe
frame that may land just past the line) stays for them. The 1.60 % line is
also where the Beacon reports `airtime_saturated`, so the probe at its
ceiling does not hold the Beacon in trouble. On the DEV profile, which has
no mesh, or on a boot where the mesh did not initialize (safe mode, or
ESP-NOW refused), the probe also brings the governor up itself; otherwise
nothing would, and every reservation would pass.

Urgent traffic — tamper alerts, power-loss alerts, `OFFLINE_IMMINENT` —
calls `force_reserve_urgent()`. It always sends, but its cost is recorded
so the telemetry is honest.

The exact number you'll see is **percent**, with one decimal place — the
wire JSON carries `airtime_pct_x100` (e.g. `215` for 2.15%) and the HA
value_template divides by 100. Discovery is published automatically via
the existing MQTT bridge (`firmware/projects/canary-wap/arduino/canary_wap/csi_mqtt.cpp`),
which creates these three entities under the canary device:

| Entity | Type | What it shows |
|--------|------|---------------|
| `sensor.canary_<id>_mesh_airtime_pct` | sensor (`%`) | Rolling 10 s airtime utilization |
| `sensor.canary_<id>_mesh_channel` | sensor | 2.4 GHz channel the mesh is on (1–13) |
| `binary_sensor.canary_<id>_mesh_channel_locked_to_sta` | binary_sensor | On = mesh is following your home WiFi |

State is published every 30 s on `{prefix}/{device_id}/mesh`; the JSON
payload also carries `routine_allowed`, `routine_denied`, and
`urgent_sends` counters for deeper debugging via the MQTT Explorer.

## What this means for installers

| Question | Answer |
|----------|--------|
| Can I put 8 Canaries on the same home network? | Yes. Airtime stays ≤ 2% × 8 = 16% only if every Canary is *also* getting tampered with simultaneously. Routine cap is per-device; in practice you'll see < 1% per Canary at idle. |
| Will the mesh slow down my home WiFi? | No measurable impact. The airtime cap is one full order of magnitude below what a single phone uses streaming video. |
| What channel should I set my router to? | Anything. The Canaries follow you. |
| What if STA drops? | Mesh rides the AP channel (Canaries always run AP mode for local admin). If AP is also down, mesh falls back to channel 6. |
| Can I disable mesh entirely? | Yes — `mesh.enabled = false` in NVS / admin UI. Canaries still witness; they just don't gossip. |

## What's NOT solved yet

Closed steps so far: STA-following channel policy, airtime governor,
chirp-presence gating, HA MQTT-discovery telemetry. Remaining items from
the v1.0 roadmap:

- **Per-device certificates** between Canary and witnessd (vs. today's
  shared `opera_secret`) — Stream C step 2.
- **BLE-fallback for non-urgent gossip** — when channel-conflict events
  spike, demote heartbeats to BLE GATT and keep ESP-NOW for tamper alerts
  only.
- **"Add another Canary" wizard step** in the HA add-on UI, signed by an
  existing trustee Canary.

## How to verify

Host-side unit tests (no ESP32 required):

```bash
make -C firmware/projects/canary-wap/tests_host
```

Should print `ALL MESH COEXISTENCE TESTS PASSED` (the channel policy and
the governor's window) and `test_csi_probe_airtime: ALL PASSED` (the real
probe scheduler under the real governor: the gate starts a frame at
1.59 % and none at 1.60 %, the WAP's 10 Hz broadcast is never denied, one
peer at 20 Hz stays steady, and eight peers are held at the 1.60 % ceiling
while the heartbeat keeps its room). Both are host tests of
the estimate; whether it matches real air is a bench question.

In the field, with two Canaries paired into one Opera, both in STA on the
home network:

1. Set your router to channel 11 (or any non-default).
2. Observe `sensor.canary_<id>_mesh_channel` in HA — both Canaries should
   report 11 and `binary_sensor.canary_<id>_mesh_channel_locked_to_sta`
   should be ON.
3. Run an iperf3 flow at 30 Mbps between two laptops on the same WiFi.
4. Tamper one Canary; the other should report the tamper event within 5 s,
   and the iperf3 throughput should drop by < 5% during the burst.
5. Watch `sensor.canary_<id>_mesh_airtime_pct` — should stay ≤ 2% over a
   10-minute average.

See the v1.0 plan in this branch for the broader acceptance criteria.
