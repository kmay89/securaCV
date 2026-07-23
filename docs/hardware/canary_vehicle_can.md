# Canary Vehicle — passive CAN bus witness (arrival/departure)

**Status:** kernel vocabulary + adapter shipped and unit-tested (`src/adapter/can_bus.rs`,
`spec/witness_dictionary.json`); **not bench-validated against a real vehicle** — no CAN bus has
confirmed these frame IDs, because every vehicle's IDs are proprietary. This is a real,
compiling, tested Rust adapter — a different maturity tier from the pure-docs concept cards
elsewhere in this file (Vision Pro/Lite) — but the vehicle-specific configuration is still yours
to do on the bench.

---

## 1 · The hero feature: tell the fleet when you arrive or leave

A vehicle's ignition state, sensed passively off its own CAN bus, becomes a
[`vehicle_arrival_departure`](../../spec/event_contract.md) claim — the same coarse,
signed-and-sealed witness event every other Canary emits. Point it at a `zone:garage` or
`zone:driveway` and the rest of your fleet (Display, Home Assistant automations, whatever else is
listening) can react: lights on, alarm profile disarmed, a "welcome home" on the Display bird —
without a phone geofence, and without the vehicle ever being tracked anywhere it goes.

This got its own dedicated `ClaimKind`/`EventType` (`VehicleArrivalDeparture`) rather than
reusing `ContactStateChange` — see `spec/witness_dictionary.json`'s `event_types`/`claim_kinds`
entries and `spec/sensor_adapter_contract_v0.md` §6. That was a deliberate, larger change (touches
the kernel's closed vocabulary, its two Home Assistant mirrors, and the drift linter) in exchange
for a clearer timeline: "vehicle arrived" reads as itself, not as "a door opened."

---

## 2 · What actually ships today (and what's still an idea)

| Claim | Status |
|---|---|
| **Arrival/departure** (`vehicle_arrival_departure`) | ✅ Shipped: kernel vocabulary, `CanBusAdapter`, config parsing, SocketCAN reader. Unverified against real hardware. |
| **OBD-port tamper** | 💡 Idea only — not implemented. Would need a separate intrusion signal (voltage/contact), not something visible in CAN traffic itself. |
| **Rich local vehicle-health data** (speed, RPM, DTCs, trip length) | 💡 Idea only — not implemented. See §5's two-tier model for how this *should* be built when it is: HA-only, never the sealed log, mirroring how Canary Sense keeps breathing/heart-rate out of the log while presence goes in. |

Don't let §1's excitement outrun this table — the adapter today does exactly one thing:
frame-ID/byte-pattern → arrival/departure claim. Everything else here is scoped, honest future
work, not a hidden feature.

---

## 3 · Why passive-only, and why that's not a limitation to work around

This adapter **never transmits a CAN frame** — `CanBusAdapter` has no write path, and
`spawn_socketcan_reader` only ever calls `read()` on the socket. This was a real fork in the
design (see the "what would OBD get us" discussion this doc grew out of):

- **Passive listening** (what's built): read raw frames, match a configured byte pattern. Frame
  IDs and layouts are manufacturer-proprietary — there's no universal "ignition on" frame — so you
  sniff and configure your *own* vehicle's pattern (§4). Never writes to the bus.
- **Active OBD-II polling** (not built, not planned here): sending Mode 01 request frames (SAE
  J1979) gets you standardized speed/RPM/DTCs across any compliant vehicle, but means
  *transmitting* onto the vehicle's bus — a different risk class than every other sensor in the
  fleet, all of which are receive-only. Kept out of scope on purpose.

---

## 4 · Hardware: two paths, one shipped today

### Path A — Linux SBC + CAN HAT + SocketCAN (what `can_bus.rs` talks to)

The adapter reads a **Linux SocketCAN** interface (`can0`) via a raw `AF_CAN` socket. This is the
standard, kernel-supported way to read CAN on Linux — a Raspberry Pi (Zero 2 W is small/cheap
enough for a glovebox) with an MCP2515-based CAN HAT and the in-kernel `mcp251x` driver gets you
a `can0` interface with **no custom firmware at all**, the same "config, not code" posture as
every other adapter in this repo. This pairs naturally with the existing
[Raspberry Pi hub work](../design/raspberry_pi_hub_flashing.md) — a second, vehicle-mounted Pi
running `adapter_host` standalone (its own `witness.db`, its own device key), syncing to the home
hub over WiFi when in garage range.

Bring the interface up (once, or via a systemd unit / `/etc/network/interfaces`):

```bash
sudo ip link set can0 up type can bitrate 500000   # most vehicles: 500 kbps
```

Wire the OBD-II port to the HAT's CAN_H/CAN_L (most vehicles 2008+, US; most globally since
~2012): pin 6 = CAN High, pin 14 = CAN Low, pin 4/5 = ground. Check the HAT has a 120 Ω
termination resistor — most do, some are jumper-selectable.

### Path B — Seeed XIAO + CAN Bus Breakout Board (not built)

The $9.99 Seeed CAN Bus Breakout (MCP2515 + SN65HVD230, SPI to a XIAO) is a real, cheap board —
but it's an ESP32 microcontroller, not Linux, so it can't expose SocketCAN directly. Using it
would need **new embedded firmware** to bridge MCP2515 SPI frames to somewhere `adapter_host` can
read them (serial, or publish over MQTT/webhook in the same JSON shape `mqtt_sensor`/`webhook`
already accept). That firmware does not exist yet — Path A is the path that ships today with zero
new code.

---

## 5 · Finding YOUR vehicle's ignition frame

**Check [`canary_vehicle_profiles.md`](./canary_vehicle_profiles.md) first.** A real,
sourced-from-opendbc signal matrix now exists for Honda Pilot, Honda Odyssey (both generations),
Toyota Corolla, and Volkswagen MQB — `python3 scripts/dbc_signal_resolve.py emit-routes
<vehicle-id>` prints ready-to-paste routes for those. None are bench-confirmed yet, but starting
from a sourced signal beats a cold `candump` guess. If your vehicle isn't one of those (or the
sourced signal turns out wrong on the bench), hand-sniffing is still the fallback:

There is no universal answer for everything else — this is the one step every install does
differently. With `can0` up (§4) and `can-utils` installed:

```bash
candump can0
```

Turn the ignition on and off a few times and watch for a frame whose ID or byte value changes
consistently with it (`cansniffer can0` is often faster — it highlights changing bytes). Once
you've found it:

```
[[adapter.route]]
can_id = "0x3E8"      # your frame's ID
byte_offset = 0        # which byte changed
equals = "0x01"        # its "on" value
kind = "vehicle_arrival_departure"
zone = "garage"

[[adapter.route]]
can_id = "0x3E8"       # same ID...
byte_offset = 0
equals = "0x00"         # ...its "off" value
kind = "vehicle_arrival_departure"
zone = "garage"
```

Full worked example with comments: [`adapter_host.example.toml`](../../adapter_host.example.toml)
§"Adapter: Vehicle CAN bus".

---

## 6 · Mounting

[`canary_vehicle_mount.scad`](./enclosure/canary_vehicle_mount.scad) (dash plate + vent clip, on
the shared T-stud interface) already exists for mounting a Canary in a vehicle cabin — same
in-development status as the CAN adapter itself. Its heat/battery warnings apply doubly here: a
closed car exceeds +60 °C, well past most SBCs' and CAN HATs' comfort zone and past any LiPo's
safe range — **USB/vehicle-power only, no battery**, light-colored case, and don't leave a
battery-equipped build on a sun-baked dash regardless.

---

## 7 · Open items

- No bench validation against a real vehicle — every frame ID/byte pattern in this doc and the
  example config is illustrative, not measured.
- Path B (XIAO + CAN Bus Breakout) needs firmware before it's a real alternative to Path A.
- The two-tier local-telemetry idea (§2) is unbuilt — if you want it, the right shape is a
  second, HA-only-scoped adapter or a firmware-side local log, never routed through this
  adapter's narrow `VehicleArrivalDeparture`-only descriptor.

---

## 8 · Troubleshooting

Anticipated, not field-reported — no bench validation against a real vehicle yet (§7).

<details>
<summary><strong>SocketCAN interface won't come up (<code>ip link set can0 up</code> fails)</strong></summary>

- Confirm the `can` and `mcp251x` (or your HAT's specific driver) kernel modules are loaded:
  `lsmod | grep can`.
- Check the HAT's SPI overlay is actually enabled (Pi: `dtoverlay=mcp2515-can0` in
  `/boot/config.txt` or `/boot/firmware/config.txt`, plus a reboot).
- A bitrate mismatch with the vehicle's actual bus speed will often still let the interface come
  up but produce only error frames — try 500000 first (most common), then 250000.

</details>

<details>
<summary><strong><code>candump</code> shows nothing at all</strong></summary>

- Verify CAN_H/CAN_L aren't swapped — reversing them is a common wiring mistake and typically
  yields silence rather than an obvious error.
- Check the HAT has a 120 Ω termination resistor populated (§4) — many boards ship with it
  jumper-selectable, off by default.
- Confirm the vehicle's ignition is actually on — most vehicle CAN buses go quiet with the key out.

</details>

<details>
<summary><strong><code>adapter_host</code> runs but no claims appear</strong></summary>

- Confirm the route's `can_id`/`byte_offset`/`equals` actually match what you saw in `candump` —
  a single wrong hex digit silently means "never matches."
- Remember routes are edge-triggered (§ can_bus.rs's `route_frame_edge_triggered`): the FIRST
  matching frame after a differing one fires a claim; a byte that's already sitting in the
  matched state when the adapter starts won't re-fire until it changes.
- Check `adapter_host`'s logs for `registered can_bus adapter` — if that line is missing, the
  config didn't parse the way you expect.

</details>
