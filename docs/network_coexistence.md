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
- An **airtime governor** caps routine mesh traffic (heartbeats, gossip) at
  **≤ 2%** of any rolling 10-second window. Urgent traffic — tamper alerts,
  `OFFLINE_IMMINENT`, power-loss notifications — bypasses the cap but is
  still counted, so the `mesh_airtime_pct` telemetry stays honest.

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
unicast traffic is faster, so we never under-count.)

Routine traffic — heartbeats every 30 s, presence beacons every 60 s,
gossip — calls `try_reserve_routine()`. If the projected airtime in the
rolling 10-second window would exceed the cap (default 2%), the send is
denied and the caller skips this tick. The peer-stale timer is 90 s, so a
few skipped heartbeats are harmless.

Urgent traffic — tamper alerts, power-loss alerts, `OFFLINE_IMMINENT` —
calls `force_reserve_urgent()`. It always sends, but its cost is recorded
so the telemetry is honest.

The exact number you'll see in `mesh_airtime_pct` (published via MQTT
discovery as `sensor.securacv_{device}_mesh_airtime_pct` once the HA
integration step lands) is **percent × 100**: 215 means 2.15%.

## What this means for installers

| Question | Answer |
|----------|--------|
| Can I put 8 Canaries on the same home network? | Yes. Airtime stays ≤ 2% × 8 = 16% only if every Canary is *also* getting tampered with simultaneously. Routine cap is per-device; in practice you'll see < 1% per Canary at idle. |
| Will the mesh slow down my home WiFi? | No measurable impact. The airtime cap is one full order of magnitude below what a single phone uses streaming video. |
| What channel should I set my router to? | Anything. The Canaries follow you. |
| What if STA drops? | Mesh rides the AP channel (Canaries always run AP mode for local admin). If AP is also down, mesh falls back to channel 6. |
| Can I disable mesh entirely? | Yes — `mesh.enabled = false` in NVS / admin UI. Canaries still witness; they just don't gossip. |

## What's NOT solved yet

This is the *first* milestone of "secure useful sensing that doesn't fight
your WiFi." Open items that the v1.0 roadmap addresses next:

- **Chirp's presence beacons** are not yet routed through the governor. They
  fire every 60 s and cost ~1 ms each, so the contribution is small, but the
  wiring belongs here once the broader Chirp refactor lands.
- **Per-device certificates** between Canary and witnessd (vs. today's
  shared `opera_secret`) — Stream C step 2.
- **BLE-fallback for non-urgent gossip** — when channel-conflict events
  spike, demote heartbeats to BLE GATT and keep ESP-NOW for tamper alerts
  only.
- **HA telemetry surface** — publish `mesh_airtime_pct`, `mesh_channel`,
  `mesh_channel_locked_to_sta` as MQTT-discovery sensors.

## How to verify

Host-side unit tests (no ESP32 required):

```bash
make -C firmware/projects/canary-wap/tests_host
```

Should print `OK  all mesh coexistence tests passed`.

In the field, with two Canaries paired into one Opera, both in STA on the
home network:

1. Set your router to channel 11 (or any non-default).
2. Observe `sensor.securacv_{device}_mesh_channel` in HA — both Canaries
   should report 11.
3. Run an iperf3 flow at 30 Mbps between two laptops on the same WiFi.
4. Tamper one Canary; the other should report the tamper event within 5 s,
   and the iperf3 throughput should drop by < 5% during the burst.
5. Watch `sensor.securacv_{device}_mesh_airtime_pct` — should stay ≤ 2% over
   a 10-minute average.

See the v1.0 plan in this branch for the broader acceptance criteria.
