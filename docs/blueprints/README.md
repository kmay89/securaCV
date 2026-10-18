# SecuraCV blueprints

Automation blueprints for the SecuraCV Home Assistant integration. Each badge
imports one blueprint into **Settings → Automations → Blueprints** in a click;
then create an automation from it and pick your entities and your phone — no
entity-id surgery, no YAML. All of them need Home Assistant **2026.5.0** or
later (`homeassistant.min_version`), the same floor as the integration.

The one-command installer and the SecuraCV app (Home Assistant OS) also copy
every file here to `/config/blueprints/automation/securacv/`, where Home
Assistant loads them without importing. Setting up the integration itself,
and what *Sealed ✓* and *Verified ✓* mean on the events these blueprints
read, is in [the setup guide](../homeassistant_setup.md#what-sealed--verified--and-failed--mean);
the dashboard the digest and the witness notification point back to is
[Settings → Dashboards → Add dashboard → SecuraCV](../homeassistant_setup.md#add-the-dashboard).

**Notifications go to a phone.** Every blueprint that notifies takes a
*Phone* input (a device running the Home Assistant Companion app) and derives
its `notify.mobile_app_…` service by itself; the optional *Notification
service* text input overrides it for groups, Telegram, Pushover and the like.

## The witness log (Frigate + the kernel)

| Blueprint | Import | What it does |
|---|---|---|
| [`securacv_witness_notification.yaml`](securacv_witness_notification.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_witness_notification.yaml) | **Witness notification with proof.** Frigate raises an alert on a camera → wait up to 90 s for SecuraCV to seal it → one push: *Front door · Person — 14:03 · Sealed ✓ into the witness log (entry #1,204, window 14:00–14:10)*, or *seal pending* on timeout. Actions: Open Frigate, Export evidence. Optional Frigate thumbnail (Frigate's image, through the Frigate integration's notification proxy). Renders only the camera, Frigate's object label, the zones and the seal state — never a recognized name or plate (the file says why). One automation per camera. |
| [`securacv_daily_digest.yaml`](securacv_daily_digest.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_daily_digest.yaml) | **Daily digest.** Every morning: *7 events: front door 4, driveway 3 · morning 2, evening 5 · chain verified ✓ at 03:00 · 1,204 entries.* Always sends — a quiet night reads *Quiet night: no witness events* with the chain state, so silence is never ambiguous. Reads `sensor.securacv_daily_digest` and `binary_sensor.securacv_chain_valid` (the add-on's unclaimed `pwk_*` sensors still work and read "kernel-reported"). |
| [`securacv_pattern_break.yaml`](securacv_pattern_break.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_pattern_break.yaml) | **Quiet-hours activity & silent zone.** A witness event sealed inside the quiet window you set, or a zone that has witnessed nothing for N hours during the hours you expect activity. Works with a per-camera witness event entity or a last-event sensor. |

## Canary devices

| Blueprint | Import | What it does |
|---|---|---|
| [`securacv_alerts.yaml`](securacv_alerts.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_alerts.yaml) | **Canary alerts.** Smoke / CO alarm heard (critical pushes that bypass silent mode), tamper, power loss, chain failure, device offline, GPS loss — every alert opt-in by picking its sensor. |
| [`canary_battery_health.yaml`](canary_battery_health.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fcanary_battery_health.yaml) | **Battery health.** Low battery (only while discharging — mains and charging never trip it), worn-cell reminder from the cycle-fade estimate, recovered. |
| [`securacv_hue_alert_light.yaml`](securacv_hue_alert_light.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_hue_alert_light.yaml) | **Alert light.** Any color bulb (Philips Hue is the reference) as a silent beacon: red and held on tamper / smoke or CO heard / chain failure, green all-clear, three amber pulses when a device goes offline. |
| [`securacv_busybar_alert.yaml`](securacv_busybar_alert.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fsecuracv_busybar_alert.yaml) | **Busy Bar alert.** The busy.app desk light as a beacon, over your LAN. Needs the `rest_command` package from [`docs/integrations/busy-bar.md`](../integrations/busy-bar.md) first. |

## Radar (canary-sense / MR60BHA2)

Wellbeing signals, never medical data — see
[`canary_sense_wellbeing.md`](canary_sense_wellbeing.md) for the entities, the
limits and the stock-card wellbeing tile.

| Blueprint | Import | What it does |
|---|---|---|
| [`canary_sense_after_hours_presence.yaml`](canary_sense_after_hours_presence.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fcanary_sense_after_hours_presence.yaml) | **After-hours presence.** Radar presence inside an overnight window, optionally only when a second, independent witness agrees. |
| [`canary_sense_lights_out_tamper.yaml`](canary_sense_lights_out_tamper.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fcanary_sense_lights_out_tamper.yaml) | **Lights-out-with-presence.** The room went dark but the occupant did not leave: a possible cover-up or a light cut to blind a co-located camera. |
| [`canary_sense_welfare_check.yaml`](canary_sense_welfare_check.yaml) | [![Import](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV%2Fblob%2Fmain%2Fdocs%2Fblueprints%2Fcanary_sense_welfare_check.yaml) | **Welfare check.** Presence on but no breathing lock for N minutes during the sleep window → prompt a human to look in. A prompt, not a medical alarm. |

## Importing by URL instead

**Settings → Automations → Blueprints → Import Blueprint** and paste
`https://github.com/kmay89/securaCV/blob/main/docs/blueprints/<file>.yaml`.

## Entity ids: `pwk_*` → `securacv_*`

Once the SecuraCV integration claims the kernel (Home Assistant OS app or the
Docker sidecar), the add-on's MQTT-discovery entities (`sensor.pwk_last_event`,
`sensor.pwk_daily_digest`, `binary_sensor.pwk_chain_problem`,
`button.pwk_verify_now`) are withdrawn and the integration's own entities take
over — `sensor.securacv_last_event`, `sensor.securacv_daily_digest`,
`binary_sensor.securacv_chain_valid` (note the inverted sense: `on` means the
chain verified), `button.securacv_verify_now`, and one
`event.securacv_<camera>` per camera. The blueprints above default to the new
ids; an automation you created from an older digest blueprint keeps working
through the migration (the digest reads a problem-class sensor as
"kernel-reported"), and Home Assistant's Repairs panel lists the old → new ids
the first time the integration claims — on an instance where the `pwk_*`
entities existed; a fresh install never sees the notice
([the migration note](../homeassistant_setup.md#entity-ids-pwk_--securacv_)).
