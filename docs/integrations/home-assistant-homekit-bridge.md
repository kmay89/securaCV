# Apple Home via the Home Assistant HomeKit Bridge

Put your Canaries in the Home app using the Home Assistant you already run.
No new code, no flashing, no account — an Apple TV or HomePod on the same
network and about five minutes of YAML.

This is **bridge site A** of
[the Apple Home design](../design/apple_home_integration.md). If you do not
run Home Assistant, you want
[the native lane](../../src/bin/hap_bridge.rs) instead — see
[Which lane should I use?](#0-which-lane-should-i-use) below.

---

## 0) Which lane should I use?

| | **A — via Home Assistant** (this doc) | **B — `hap_bridge`, native HAP** |
|---|---|---|
| Needs | Home Assistant + the SecuraCV integration | Nothing but an Apple hub |
| New code | none | `--features bridge-homekit-server` |
| Pairing | HA's own HomeKit Bridge | SecuraCV's setup code / QR |
| Publication timing | **Home Assistant's** — on change | **Paced** — fixed metronome |
| Best for | households already running HA | anyone else, and venues |

Both end up in the same place: coarse sensors in the Home app that Siri and
automations can use. The difference that matters is the third row, and it is
explained honestly in [§5](#5-the-honest-part-this-lane-is-un-paced).

---

## 1) Prerequisites

- Home Assistant with **Settings → Devices & Services** reachable.
- The [SecuraCV integration](../../custom_components/securacv) installed and
  connected to your MQTT broker (see `docs/homeassistant_setup.md`).
- At least one Canary publishing to `securacv/<device-id>/…`.
- An **Apple TV or HomePod** signed into your Apple Account and set as a Home
  Hub. Without one, HomeKit accessories work only while you are on the local
  network — no automations, no remote access. You have this covered.

Check the integration is alive before going further: **Settings → Devices &
Services → SecuraCV** should list a device per Canary, with entities under it.

---

## 2) What the integration gives Apple Home

The integration publishes these with standard device classes, which is what
lets HA's HomeKit Bridge map them onto real HomeKit sensor types rather than
generic switches:

| Entity | Device class | Appears in Home as |
|---|---|---|
| `binary_sensor.<canary>_motion` | `motion` | Motion sensor |
| `binary_sensor.<canary>_occupancy` | `occupancy` | Occupancy sensor |
| `binary_sensor.<canary>_tamper` | `tamper` | Tamper status |
| `binary_sensor.<canary>_online` | `connectivity` | Connectivity status |

> **Version note.** Motion and occupancy are native as of integration
> **0.6.0**. On an older version they do not exist and you need the template
> workaround in [Appendix A](#appendix-a-template-sensors-pre-060); upgrading
> is much better, because the templates duplicate a mapping that CI keeps in
> sync in exactly one place.

Which events raise motion versus occupancy is not a choice made in your YAML.
It comes from `homekit_projection.event_signals` in
[`spec/witness_dictionary.json`](../../spec/witness_dictionary.json), mirrored
into the Rust kernel and the Python integration, with
`scripts/lint_dictionary_sync.py` failing CI on any drift between the three.

---

## 3) The recipe

### 3.1 Enable the HomeKit Bridge with an include-list

In `configuration.yaml`:

```yaml
homekit:
  - name: SecuraCV Bridge
    port: 21063
    mode: bridge
    filter:
      include_entities:
        # One line per Canary sensor you want in the Home app.
        - binary_sensor.porch_canary_motion
        - binary_sensor.porch_canary_occupancy
        - binary_sensor.porch_canary_tamper
        - binary_sensor.garage_canary_motion
        - binary_sensor.garage_canary_occupancy
        - binary_sensor.garage_canary_tamper
```

**Use `include_entities`, not `include_domains`.** Bridging the whole
`binary_sensor` domain sweeps in every entity HA happens to have — including
the chain-integrity and transport-health sensors, which are meaningful to you
and meaningless in the Home app. An explicit list is also a consent record:
what is in it is exactly what leaves for Apple's ecosystem.

Restart Home Assistant.

### 3.2 Pair

1. **Settings → Devices & Services → Integrations**, find *HomeKit Bridge*.
2. It shows a pairing code (and a QR).
3. On your iPhone: **Home app → `+` → Add Accessory → More options…**
4. Pick **SecuraCV Bridge**, enter the code.
5. Assign each sensor to a room as prompted.

Because HA's bridge is not an Apple-certified accessory, iOS says
*"uncertified accessory"* and offers **Add Anyway**. That is normal and is the
same prompt every Homebridge and HA user sees.

### 3.3 Try an automation

In the Home app: **Automation → When a sensor detects something →** *Porch
Canary Motion* **→ Detects Motion**, then pick a light and a time window.

Or ask Siri: *"Is there motion at the porch?"*

---

## 4) Verifying it actually works

```bash
# Watch what the Canary is really publishing.
mosquitto_sub -h <broker> -t 'securacv/+/events' -v

# Trip the sensor, then confirm HA saw it:
#   Developer Tools → States → binary_sensor.porch_canary_motion
```

If HA shows `on` but the Home app does not update:

| Symptom | Usual cause |
|---|---|
| Sensor missing from Home entirely | not in `include_entities`, or HA not restarted |
| Shows as a generic switch | entity has no `device_class` — check you are on integration 0.6.0+ |
| Never turns off | template without `auto_off` (see Appendix A); the native sensors handle this |
| Nothing at all after re-pairing | delete the bridge from Home, then remove `.storage/homekit.*` in HA and re-pair |

---

## 5) The honest part: this lane is un-paced

The kernel's own Apple Home projection publishes on a **metronome** — a fixed
cadence that does not vary with whether anything happened. That is what keeps
[Invariant III](../../spec/invariants.md) (Metadata Minimization) intact:
the finest external time resolution anything downstream can learn is one tick,
and the *rate* of publication carries no information.

**This lane does not do that.** Home Assistant's HomeKit Bridge publishes on
change, like every other HA integration. So on this path:

- the **payload** is still coarse — a boolean, no zone, no timestamp, no
  count, no confidence, no identity. That part is structural and holds here
  exactly as everywhere else.
- the **timing** is not blurred. Someone able to observe traffic between HA
  and your Apple hub learns roughly when a sensor changed, because the packet
  arrives when it changes.

For most households that is a fine trade — it is the same exposure every
other HA-bridged sensor in the house already has, and it is all inside your
LAN. But it is a real difference from what the kernel promises, so it is
written down here rather than glossed.

If the timing property matters to you, use
[the native lane](#0-which-lane-should-i-use): `hap_bridge` speaks HAP
directly and publishes on the metronome, so the bound is structural rather
than a matter of who is listening.

---

## 6) What this never sends

Worth stating plainly, because "camera in the Home app" usually means
something else:

- **No video, ever.** Not through this bridge, not through HomeKit Secure
  Video. The design's [§2](../design/apple_home_integration.md) records that
  decision and the triggers that would revisit it.
- **No identity.** No faces, no plates, no names, no re-identification. The
  vocabulary has no field for any of it, so there is nothing to leak and no
  setting to get wrong.
- **No location, no zone, no counts.** A Canary tells Apple Home *what kind
  of thing is true now*, and nothing else.

The four class-scoped signals (person / vehicle / animal / package) carry one
extra word — the coarse object class, never an identity — and are **off until
a human turns them on**.

---

## Appendix A: template sensors (pre-0.6.0)

Only if you are on an older integration and cannot upgrade yet. These
duplicate a mapping the linter otherwise keeps in one place, so treat them as
a stopgap.

```yaml
mqtt:
  - binary_sensor:
      name: "Porch Canary Motion"
      state_topic: "securacv/porch-canary/events"
      device_class: motion
      # The event types that assert motion, per the witness dictionary.
      value_template: >-
        {% set e = value_json.event_type | default('') %}
        {{ 'ON' if e in [
             'BoundaryCrossingObjectLarge',
             'BoundaryCrossingObjectSmall',
             'ObjectRemovedFromZone',
             'VehiclePresenceAfterHours',
             'VehicleArrivalDeparture'
           ] else 'OFF' }}
      # Without this the sensor latches on after the first event and every
      # automation written against it fires exactly once, forever.
      off_delay: 10

  - binary_sensor:
      name: "Porch Canary Occupancy"
      # Presence is a retained snapshot on `state`, NOT a `presence` topic —
      # that topic does not exist on the wire.
      state_topic: "securacv/porch-canary/state"
      device_class: occupancy
      value_template: "{{ 'ON' if value_json.presence | default(false) else 'OFF' }}"
```

---

## Trademarks

Apple, Apple Home, HomeKit, HomePod, Apple TV and Siri are trademarks of
Apple Inc., registered in the U.S. and other countries and regions. SecuraCV
is an independent project by Errer Labs and is **not affiliated with,
endorsed, sponsored, or certified by Apple Inc.** References are nominative —
for identification and interoperability only.
