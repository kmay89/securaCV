# Busy Bar (busy.app) → SecuraCV Alerts

## 1) Overview

This guide puts SecuraCV alerts on a **Busy Bar** — the desk status light
from [busy.app](https://busy.app), with a 72x16 RGB LED matrix on the front
and an open local HTTP API. A tamper report, a heard smoke/CO alarm, a
witness-chain failure, or a Canary going dark lights the bar on your desk:
severity color plus a fixed alert word, nothing else.

Two lanes, pick either or both:

- **Lane A — Home Assistant blueprint** (config only, no SecuraCV build):
  a `rest_command` package plus the
  [`securacv_busybar_alert.yaml`](../blueprints/securacv_busybar_alert.yaml)
  blueprint drive the bar from the same per-Canary sensors every other HA
  automation uses. This lane can hold a critical alert on the bar **until
  the sensor clears**, because HA can see the all-clear.
- **Lane B — the alert relay sink** (for hubs running the `alert_relay`
  bin): `--busybar-url` adds the bar as a delivery sink beside ntfy, with
  the relay's own debounce and retry. Alerts auto-expire after a bounded
  hold, because the relay is fire-and-forget by design.

Privacy posture (read this before deploying — it is the point of the
product): what reaches the bar is the same coarse vocabulary every SecuraCV
alert lane speaks — a severity class and a fixed word, never event content,
never a timestamp field, never a device fingerprint. And it reaches the bar
**over your LAN only**. The bar's API is also reachable through busy.app's
cloud proxy with an account-linked token; do not use it for this. Lane B
refuses any `busy.app` URL outright, and Lane A's `rest_command` should
only ever carry the bar's local address. A per-alert feed through a vendor
cloud is precisely the timing oracle Invariant III exists to prevent.

Related docs:

- The alert relay design and its rules: `docs/design/alert_relay.md`
- The same idea on a color bulb: [`securacv_hue_alert_light.yaml`](../blueprints/securacv_hue_alert_light.yaml)
- Home Assistant integration + add-on setup: `docs/homeassistant_setup.md`

---

## 2) Architecture

```
Lane A (Home Assistant)                    Lane B (alert relay)

[Canary] --MQTT--> [HA: securacv           [Canary] --MQTT--> [hub broker]
                    binary sensors]                               |
   |                                            [alert_relay --busybar-url ...]
   v                                              |               |
[blueprint automation]                        (ntfy poke)   POST /api/display/draw
   |  rest_command over the LAN                                   |
   v                                                              v
[Busy Bar 72x16 matrix]                                  [Busy Bar 72x16 matrix]
```

---

## 3) Prerequisites

- A Busy Bar on your network, reachable by IP. Over USB-Ethernet it ships
  at `http://10.0.4.20`; on Wi-Fi use the address your router gave it. Its
  web UI (same address) shows both.
- Optional but recommended: set an **HTTP access password** in the bar's
  web UI (Settings → HTTP Access) so not everything on the LAN can draw on
  your desk. It is sent as the `X-API-Token` header below.
- Lane A: a working SecuraCV Home Assistant setup (`docs/homeassistant_setup.md`).
- Lane B: the kernel built with the `alert-relay` feature, and a broker the
  relay can subscribe to.

---

## 4) Lane A — the Home Assistant blueprint

### 4.1 The rest_command package (one copy-paste)

The blueprint calls two services that hold the bar's address, so the
blueprint itself never needs it. Add this as a package (e.g.
`/config/packages/securacv_busybar.yaml`, with `packages: !include_dir_named
packages` under `homeassistant:`) or merge it into `configuration.yaml`,
replacing `10.0.4.20` with your bar's address:

```yaml
rest_command:
  securacv_busybar_alert:
    url: "http://10.0.4.20/api/display/draw"
    method: POST
    content_type: "application/json"
    # If you set an HTTP access password on the bar, uncomment:
    # headers:
    #   X-API-Token: !secret busybar_token
    payload: >-
      {"application_name": "securacv",
       "priority": {{ priority | int(50) }},
       "led_notification_color": {{ color | tojson }},
       "elements": [{
         "id": "securacv-alert",
         "type": "text",
         "text": {{ text | tojson }},
         "font": "small",
         "color": {{ color | tojson }},
         "x": 0, "y": 0, "width": 72,
         "scroll_rate": 20,
         "display": "front"
         {%- if timeout_ms | int(0) > 0 %},
         "timeout": {{ timeout_ms | int }}
         {%- endif %}
       }]}

  securacv_busybar_clear:
    url: "http://10.0.4.20/api/display/draw?application_name=securacv"
    method: DELETE
```

Everything is scoped to the `securacv` application name: a newer alert
replaces an older one instead of stacking, and the clear removes only our
content — the bar's own timers and status stay untouched.

Reload YAML (**Developer tools → YAML → Restart** or reload rest_command),
then test from **Developer tools → Actions**: call
`rest_command.securacv_busybar_alert` with

```yaml
text: "SECURACV TEST"
color: "#00BE50"
priority: 50
timeout_ms: 10000
```

The bar should show the green text for ten seconds.

### 4.2 The blueprint

Import
[`securacv_busybar_alert.yaml`](../blueprints/securacv_busybar_alert.yaml)
(**Settings > Automations > Blueprints > Import Blueprint**; the
one-command installer pre-installs it like every other SecuraCV blueprint),
create an automation from it, and pick your Canary's sensors — each one is
opt-in, same as the Hue alert-light blueprint. What it does:

- **Critical** (tamper / smoke heard / CO heard / chain failure): the alert
  word in red at top display priority, held until every configured sensor
  reads healthy — then a short green ALL CLEAR and the bar goes back to its
  own business.
- **Offline** (past your chosen delay): an amber note that expires on its
  own after three minutes.

The words on the bar are fixed — TAMPER, SMOKE, CO ALARM, CHAIN FAIL,
OFFLINE — plus an optional label you choose (a room name). That is the
whole vocabulary; event content never reaches the desk.

---

## 5) Lane B — the alert relay sink

Build and run the relay with a Busy Bar URL beside (or instead of) ntfy:

```sh
cargo build --release --features alert-relay
./target/release/alert_relay \
  --ntfy-url  https://ntfy.sh/<your-unguessable-topic> \
  --busybar-url http://10.0.4.20 \
  --busybar-token <the bar's HTTP access PIN, if set>
```

(`BUSYBAR_URL` / `BUSYBAR_TOKEN` work as environment variables, same as the
rest of the relay's flags. At least one sink is required; both are
optional individually.)

What you get, per the relay's rules (`docs/design/alert_relay.md`):

- Severity color and the poke's fixed title as scrolling text — red for
  tamper and heard alarms, amber for integrity and offline, green for the
  drill. Higher severities win the display if something else is drawing,
  and within SecuraCV's own drawings a lower-severity poke never replaces
  an active red alarm — it waits out the hold.
- **Auto-expiry**: the relay has no all-clear lane, so every alert leaves
  the bar on its own after a bounded hold (the class's debounce gap,
  60 s–15 min). A condition that persists re-lights it on the next
  allowed poke. If you want hold-until-clear semantics, that is Lane A.
- The bar is its own delivery lane: its per-class debounce and its retry
  queue are independent of ntfy's, so an unplugged bar never delays or
  consumes the phone poke.
- `--send-test` (the drill) exercises every configured sink, the bar
  included — green, and it says plainly that nothing is wrong.

The relay refuses a `busy.app` cloud URL. This is deliberate and not
configurable; see §1.

---

## 6) Troubleshooting

- **Nothing appears, no error**: the bar's HTTP access password is set but
  the token is missing or wrong — the API answers 401. Set
  `--busybar-token` (Lane B) or the `X-API-Token` header (Lane A).
- **Nothing appears, connection refused/timeout**: wrong address, or the
  bar moved to a different IP on Wi-Fi. Check the bar's web UI; give it a
  DHCP reservation.
- **Text is cut off**: the front matrix is 72 pixels wide; long labels
  scroll, but keep the label to one short word so the alert word itself is
  readable at a glance.
- **Lane A alert stuck red after an HA restart**: the automation's
  hold-until-clear wait was lost with the restart. The sensors clearing
  will not redraw green on their own in that state — call
  `rest_command.securacv_busybar_clear`, or press the bar's own controls.
  (Same failure mode as the Hue blueprint's snapshot, and arguably the
  right one for a witness alert.)
