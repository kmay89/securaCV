# Apple Home via the HA HomeKit Bridge — worked recipe

This is phase **A0** of [Apple Home & the fleet](../design/apple_home_integration.md)
(§3.2, bridge site A): put the fleet's coarse signals into Apple Home **with zero
new code in this repo**, using the Home Assistant hub you already run and the
`homekit` integration Home Assistant ships. With a home hub present (an Apple TV
4K or a HomePod), Apple then does what Apple is good at: notifications reach
every iPhone, iPad, and Apple Watch in the household — at home or away, app
closed — and automations run on Apple's hub ("lights before eyes" when motion
trips, tamper goes house-wide).

Everything here also works without Apple: the same entities drive Home
Assistant's own push notifications (iOS **and** Android, free, no subscription)
via the shipped [alert blueprints](../blueprints/securacv_alerts.yaml). Apple
Home is an *additional* renderer of the same truth, never the only one — that is
the degradation table in the RFC (§7), kept on purpose.

> **Not running Home Assistant?** The other lane needs nothing but the hub
> binary and an Apple TV or HomePod: `witnessd`'s own HAP bridge, with a
> two-minute setup wizard — see the
> [Apple Home quickstart](apple-home-quickstart.md). This recipe is the
> zero-new-code lane for households already on the HA hub; both lanes end
> with the same sensors in the same Home app.

## 1) Overview

What Apple Home gets — and all it gets — is the closed, present-tense signal
vocabulary from the RFC: motion, occupancy, smoke-alarm-heard, tamper, liveness.
No event history (HomeKit characteristics are current-state only, so Invariant
VII survives by construction), no zone geometry, no timestamps, no footage. The
[HKSV verdict](../design/apple_home_integration.md#2-the-hksv-verdict--argued-once-recorded-with-revisit-triggers)
stands: the events — absolutely; the video — never.

| Signal in Apple Home | Backing entity today | Source |
|---|---|---|
| Motion Sensor (per zone) | `binary_sensor.pwk_<zone>_motion` (`device_class: motion`, auto-off 10 min) | `event_mqtt_bridge` (runs by default in the add-on) |
| Smoke Sensor | `binary_sensor.<canary_id>_smoke_alarm` ("Smoke Alarm Heard", `device_class: smoke`) | Canary WAP acoustic detector (`FEATURE_ACOUSTIC_EVENTS`, NFPA 72 T3 cadence) |
| Carbon Monoxide Sensor | `binary_sensor.<canary_id>_co_alarm` ("CO Alarm Heard", `device_class: carbon_monoxide`) | Same detector, UL 2034 T4 cadence — a separate entity, so it needs its own include line |
| Occupancy Sensor | `binary_sensor.<canary_id>_occupancy` (`device_class: occupancy`) | The integration itself, natively — phase A1, shipped. A template is now only needed for a presence source the integration does not own (§5) |

## 2) Architecture

```
Canaries / Frigate ──MQTT──▶ Pi hub (HAOS)
                              ├─ SecuraCV add-on: witnessd → sealed log → event_mqtt_bridge
                              ├─ Home Assistant entities (securacv integration + MQTT discovery)
                              │    ├─ HA HomeKit Bridge ──HAP/LAN──▶ Apple home hub (Apple TV 4K / HomePod)
                              │    │                                   └─▶ iCloud push → iPhone / iPad / Watch (away too)
                              │    └─ HA companion-app push (FCM/APNs) ──▶ any phone, no Apple hub needed
                              └─ (optional) adapter_host route → the alarm heard becomes a sealed witness event
```

The HomeKit hop is LAN-only pairing between the Pi and the Apple hub; remote
delivery rides Apple's infrastructure between the user's own devices. No
SecuraCV cloud exists in this picture, same as everywhere else.

## 3) Prerequisites

- The Pi hub set up per [Home Assistant setup](../homeassistant_setup.md) —
  HAOS on a Pi 4/5, `scripts/install.sh` from the Terminal add-on installs
  Mosquitto, the SecuraCV add-on, and the custom integration. The add-on's
  `mqtt_publish` option (default **enabled**) is what creates the
  `pwk_<zone>_motion` sensors.
- At least one event source: a Canary, or Frigate via the
  [HA + Frigate recipe](home-assistant-frigate-mqtt.md).
- An Apple **home hub** on the same LAN: Apple TV 4K or HomePod. Without one,
  accessories still pair but remote access, notifications, and automations do
  not run — the Home app says so itself. (This mirrors our own away path:
  something always-on has to be home.)
- The Home app on an iPhone or iPad signed into the household's iCloud.

## 4) Expose the entities to Apple Home

Home Assistant's HomeKit Bridge is happiest with a short, explicit include
list — bridge the signals, not the plumbing. In `configuration.yaml`:

```yaml
homekit:
  - name: SecuraCV Bridge
    mode: bridge
    filter:
      include_entity_globs:
        - binary_sensor.pwk_*_motion        # per-zone motion
        - binary_sensor.*_smoke_alarm       # Canary WAP acoustic T3 (smoke)
        - binary_sensor.*_co_alarm          # Canary WAP acoustic T4 (CO)
      include_entities:
        - binary_sensor.*_occupancy         # native (A1); or the §5 template for a foreign source
```

Deliberately **excluded**: the connectivity/`problem`/storage/chain entities.
Apple Home has no honest rendering for attestation or chain state — that
vocabulary stays on surfaces that can render it (the app, the Wall, HA
dashboards). A Canary going dark still shows in Apple Home as the accessory
becoming unresponsive.

Restart Home Assistant, then in the Home app: **Add Accessory → More options →
SecuraCV Bridge**, and enter the pairing code shown in HA's notification
sidebar. Room-assign each sensor once; names follow the entity names.

## 5) Template occupancy — now only for foreign presence sources

**A1 has shipped**, so the integration publishes native
`binary_sensor.<canary_id>_motion` and `…_occupancy` entities itself. Occupancy
tracks the retained `state` snapshot (`securacv/<id>/state`, field `presence`)
and the events vocabulary; motion auto-clears after a hold window matching the
kernel's own, so an automation written against a Canary behaves like one
written against a sensor you already own. **Prefer the native entities** —
which events raise which signal is dictionary-governed and CI-gated across the
Rust kernel and the Python integration, so it cannot drift from what the
witness actually said.

A template is still the answer for a presence source the integration does not
own — an ESPHome mmWave kit, say, publishing over MQTT via the
`mqtt_statestream` path from the
[MR60BHA2 recipe](mr60bha2_esphome.md):

```yaml
template:
  - binary_sensor:
      - name: "SecuraCV Occupancy"
        unique_id: securacv_occupancy
        device_class: occupancy
        state: "{{ is_state('binary_sensor.mr60bha2_person_information', 'on') }}"
```

Swap the inner entity for whatever your presence source created, and include
it alongside the native entities rather than instead of them.

## 6) Notifications, three redundant lanes

Configure per-accessory alerts in the Home app under **Status and
Notifications**: motion sensors support time/person conditions ("only when
nobody's home"); a smoke sensor turning on is delivered by Apple as a
prominent safety alert on every device in the home, including Apple Watch.

| Lane | Needs | Character |
|---|---|---|
| Apple Home | home hub + iCloud | Rich, household-wide, works with every app closed; smoke/CO alerts are loud by design |
| HA companion app | nothing extra (free push, iOS + Android) | The [alert blueprint](../blueprints/securacv_alerts.yaml) sends smoke/CO as a critical push that bypasses Do Not Disturb |
| SecuraCV iOS app | a resident Apple device on the LAN | Content-free wake via the user's own iCloud ([AwayPush](../design/iphone_companion_app.md)); text composed on-device |

Run all three. They fail independently, which is the point.

**Smoke and CO, end to end:** the dumb alarm on the ceiling sounds → a Canary
WAP hears the cadence → the matching entity turns on
(`binary_sensor.<id>_smoke_alarm` for T3, `binary_sensor.<id>_co_alarm` for
T4 — two entities, both in the §4 include list) → Apple Home and the HA
blueprint both push, at home or away. The detector listens only for those two
legally mandated cadences and stores no audio — and it is *not* a UL-listed
life-safety device; it is a second messenger for the alarm you already own,
never a replacement.

**Seal the alarm as evidence (optional):** the push lanes above tell you *now*;
the adapter host can also make "an alarm was heard" part of the sealed witness
record. [`adapter_host.example.toml`](../../adapter_host.example.toml) ships
two routes for the WAP's `sensing` stream, gated on the `acoustic_event` field
via `state_field` + `state_equals` (never on the cumulative counters beside it,
which stay nonzero forever after the first detection):

```toml
[[adapter.route]]
topic = "securacv/canary-1/sensing"
kind = "acoustic_impulse_in_zone"
zone = "smoke_alarm_heard"
state_field = "acoustic_event"
state_equals = "smoke_alarm_t3"
```

One route per device (exact topic match); duplicate the pair with
`co_alarm_t4` for CO, and swap `canary-1` for each device id.

**Power outage, honestly:** a house that loses power cannot report its own
death — the hub, the router, and the Apple hub die with it. What works:

- **Today:** put the Pi, modem, and router on a small UPS and add Home
  Assistant's upstream UPS integration (NUT). That alone only creates
  entities — nothing shipped here alerts on them, and the shipped blueprint
  does not cover UPS state — so add the automation yourself and the push
  goes out over the HA companion lane during the minutes of battery you
  bought (the HomeKit lane has no honest rendering for a UPS; leave it out):

  ```yaml
  automation:
    - alias: "Mains lost — on battery"
      trigger:
        - platform: state
          entity_id: sensor.ups_status   # your NUT status entity
          to: "OB DISCHRG"
      action:
        - service: notify.mobile_app_your_phone
          data:
            title: "Power outage"
            message: "House is on UPS battery."
            data:
              push: { interruption-level: time-sensitive }
  ```
- **On restore:** the fleet's boot-lineage classifier
  ([power events](../design/power_events.md)) records the outage as evidence
  with an honest lower-bound duration, and on the canary base tree the
  classification now reaches Home Assistant as the `Power Loss` tamper
  sensor turning on — so "the power went out while you were away" is a push
  you get on power's return, from both lanes. Other firmware trees gain the
  same wiring hardware-validated, per the design doc's priority order.
- **Designed:** absence-inference and the powered mesh gateway in the
  [alert relay RFC](../design/alert_relay.md) — the only path that can speak
  *during* the outage — is design-only today, and says so.

## 7) Verify

1. `mosquitto_sub -t 'witness/#' -v` on the hub shows zone counts and motion.
2. Walk a zone: `binary_sensor.pwk_<zone>_motion` turns on in HA, and the
   matching Motion Sensor fires in the Home app within a second or two.
3. Hold your real alarm's test button: the smoke (or CO) entity trips in HA
   and the Home app raises the safety alert. (The Lab's Acoustic card is a
   staged in-browser simulation of this contract — it never reaches your
   broker, so it cannot stand in for this step.)
4. Leave the LAN (cellular), have a housemate press the test button again:
   the push still arrives — that's the home hub doing its job.
   And prove the ecosystem-neutral lane the same way, without needing a
   housemate or a real alarm:

   ```sh
   alert_relay --send-test --ntfy-url https://ntfy.sh/<your-topic>
   ```

   That sends one clearly-labeled drill down the identical path a real alert
   takes — same binary, same topic, your phone. It says "nothing is wrong"
   and arrives at a nudge, never at alarm priority. Nothing else in this
   stack can answer *"will I actually be told?"*: the app's own Test button
   posts a notification the phone made for itself, which proves iOS
   notifications work and nothing about the network.
5. Outage drill (canary base builds): unplug a Canary for a minute and plug
   it back in. On reboot the `Power Loss` tamper sensor turns on and the
   [alert blueprint](../blueprints/securacv_alerts.yaml)'s power input
   pushes the classifier's own sentence — with the lower-bound duration
   when the device had a clock to measure it.
5. Unplug the bridge briefly: accessories go "No Response" in Home, and HA's
   own availability topics recover on reconnect (the bridge re-asserts
   retained state after backoff — self-healing is already in
   `event_mqtt_bridge`).

## 8) The honest caveats

- **This lane is un-paced.** Home Assistant publishes change-driven, so packet
  timing on the HomeKit hop tracks event timing. The cover-traffic metronome
  (the built `bridge-homekit` projection core) is not in this path until phase
  A1 routes these entities through it — decision #8 in the RFC requires this
  recipe to say that plainly, so: said plainly.
- **Attestation tier is `ha-bridged`.** What Apple Home shows transited Home
  Assistant; the app and the Wall keep saying so. Nothing here is "verified"
  in our sense of the word — no signature is checked on this hop.
- **No HKSV, ever.** Cameras and footage do not appear in this bridge and
  never will; the argument is made once, in the
  [RFC §2](../design/apple_home_integration.md), with revisit triggers.
- Apple's remote hop is Apple's: their hubs, their push, their encryption
  claims. The no-Apple lanes above are the ones we can reason about
  end-to-end.

## 9) Troubleshooting

- **Pairing fails / bridge not found:** the Pi and the iPhone must share a
  subnet with mDNS working; VLANs and "AP/client isolation" are the usual
  culprits — same class of problem as [network coexistence](../network_coexistence.md).
- **Accessories show "No Response":** check the add-on is running and
  `witness/status` says `online`; the HomeKit hop sits on top of the same
  availability topic.
- **No notifications when away:** confirm the Home app shows a connected home
  hub (Home Settings → Home Hubs & Bridges) and that the Apple TV/HomePod is
  on the LAN and signed into the same iCloud household.
- **Motion never clears:** it auto-clears after 10 minutes (the discovery
  config sets `off_delay: 600`, matching the time bucket) — that is the
  designed behavior, not a stuck sensor.

## 10) Next steps

- **A1** — native motion/occupancy entities in `custom_components/securacv`,
  retiring §5 and putting this lane behind the pacer.
- **B1** — a HAP lane in `witnessd` for households that run no Home
  Assistant at all ([RFC §3.3](../design/apple_home_integration.md)).
- The [alert relay](../design/alert_relay.md) — the vendor-neutral away lane
  (ntfy-first) that owes nothing to any ecosystem.
