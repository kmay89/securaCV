# Frigate NVR Integration

SecuraCV turns [Frigate](https://frigate.video)'s detections into a **verified
witness log**: every tracked object Frigate reports over MQTT becomes a coarse,
signed, hash-chained claim — which camera, which ten-minute window, what kind
of thing — with every identity field Frigate 0.16+/0.17 emits discarded at
parse time. Frigate keeps the video; SecuraCV keeps the proof. This guide is
the Frigate side of the product: what the bridge subscribes to, what it keeps
and discards, how the Docker sidecar is run and paired, and how the witness
log reaches Home Assistant. The Home Assistant side — the three install
paths, the entities, the dashboard, the actions — is
[`homeassistant_setup.md`](homeassistant_setup.md).

## Frigate Compatibility

Tested against the Frigate 0.13–0.17 MQTT schema (fixtures for each version,
with a recognizable sentinel in every discarded field, are replayed in CI by
`tests/frigate_integration.rs` and `docker/sidecar/ci_e2e.sh`):

| Mode | Subscribes to | Frigate | Seals |
|------|---------------|---------|-------|
| **Events** (default) | `<prefix>/events` | every version | every tracked object, at its `new` message — one claim per camera, object class and ten-minute window |
| **Alerts** (`enable_reviews` / `--enable-reviews`) | `<prefix>/reviews` **instead of** `<prefix>/events` | 0.14+ | only review items of severity `alert`, once each, at the message that made them an alert — one claim per camera and window, confidence 1.0 |

The topic prefix follows Frigate's `mqtt.topic_prefix` (default `frigate`) —
set `frigate.topic_prefix` (app) or `FRIGATE_TOPIC_PREFIX` (Docker sidecar) if
you changed it. The bridge subscribes to exactly one topic per mode: never
`snapshot`, `tracked_object_update`, `camera_activity` or an audio topic.

## Why Use Both?

| System | Strengths | Limitations |
|--------|-----------|-------------|
| **Frigate** | Excellent ML detection (TensorFlow, Coral TPU), real-time alerts, recordings | Stores full recordings, detailed object tracking, recognized faces and plates |
| **SecuraCV** | Privacy-preserving logging, tamper-evident, verifiable offline, no raw video export | Simpler detection (without Frigate) |

**Together**: Frigate does the detection and keeps a day or two of clips;
SecuraCV keeps a year of signed claims that prove *that* something happened
without ever recording *who*.

---

## Quick Start (Home Assistant)

On Home Assistant OS the **SecuraCV** app does all of this with nothing to
type; the full walkthrough with the exact screens is
[Path 1 of the setup guide](homeassistant_setup.md#path-1-home-assistant-os--the-securacv-app).

### 1. Install the App

1. Go to **Settings → Apps → App Store → ⋮ → Repositories**
2. Add repository: `https://github.com/kmay89/securaCV`
3. Install **SecuraCV** (the store slug is still `privacy_witness_kernel`,
   so an older install upgrades in place)

### 2. Run the wizard

Open the app's Web UI. With Frigate installed the wizard reads your cameras
from Frigate's own config (`/addon_configs/ccab4aaf_frigate/config.yml`) and
skips the RTSP screen; the broker comes from the Supervisor; the device key
is generated and kept at `/config/.securacv/device_key` (back it up — Home
Assistant backups include it). You choose the retention (7 / 30 / 365
days), what to seal (every tracked object, or only Frigate alerts) and the
daily digest. Manual YAML is only needed for an external broker or
non-default Frigate topics:

```yaml
# Mode: use Frigate's detection instead of processing RTSP directly
mode: "frigate"

# Optional — auto-generated when left empty
device_key_seed: ""

# Frigate MQTT settings. Empty host/credentials = auto-discover from the
# Supervisor MQTT service (Mosquitto app).
frigate:
  mqtt_host: ""                 # set only for an external broker
  mqtt_port: 1883
  topic_prefix: "frigate"       # match Frigate's mqtt.topic_prefix
  enable_reviews: false         # true: seal <prefix>/reviews ALERTS instead of every tracked object (0.14+)
  mqtt_username: ""
  mqtt_password: ""
  min_confidence: 0.5           # ignore low-confidence detections
  cameras: []                   # camera slugs; empty = all cameras
  labels: ["person", "car", "dog", "cat"]

# The kernel's state and entities in Home Assistant over MQTT (on by default)
mqtt_publish:
  enabled: true

# How long to keep sealed events
retention_days: 7

# Hours between the kernel's own sealed-log self-checks (0 disables); the
# integration verifies independently, every 15 minutes
verify_interval_hours: 1
```

### 3. Finish in Home Assistant

The done screen's **Install the integration and restart Home Assistant**
button installs the carried integration on your click. After the restart,
**Settings → Devices & Services → SecuraCV discovered → Configure** (nothing
to type), then **Settings → Dashboards → Add dashboard → SecuraCV**. Walk
past a camera: the first event reads **Sealed ✓** within a minute,
**Verified ✓** after the integration's next chain check. The entities,
badges, blueprints and actions are in the
[setup guide](homeassistant_setup.md#step-4-verify-discovery); the daily
digest and the per-camera *witness notification with proof* are
[blueprints](blueprints/README.md).

---

## Quick Start (Docker, no Home Assistant)

Already running Frigate under Docker? The sidecar image wraps `witness_api`,
`frigate_bridge` and `event_mqtt_bridge` in one container that owns `/data`
— no `witnessd` (Frigate owns the cameras). Edit two lines, run three
commands, and the sealed log is running; Home Assistant, if you have one,
pairs with one 6-digit code.

```bash
# 1. Grab the quickstart compose file
curl -fsSLO https://raw.githubusercontent.com/kmay89/securaCV/main/docker/sidecar/quickstart.compose.yml

# 2. Edit two lines in it — FRIGATE_MQTT_HOST (the broker Frigate publishes
#    to, as seen from this container) and MQTT_USERNAME / MQTT_PASSWORD — then:
docker compose -f quickstart.compose.yml up -d

# 3. The setup checks a fresh install passes: broker, login, kernel health,
#    token, the retained announcement, the pairing state, log_verify.
docker exec securacv doctor
#    Add --live to also wait 30 s for a real Frigate event (walk past a camera).
docker exec securacv doctor --live
```

The device key is generated on first start into the `securacv_data` volume
(back it up — it is what makes the sealed log verifiable). No broker yet? Use
`docker/sidecar/quickstart-with-broker.compose.yml`, which bundles Mosquitto.
Its broker is reachable only from that compose network and from the Docker
host (loopback port binding); to let a Frigate or a Home Assistant on another
machine connect, set `SECURACV_MQTT_PASSWORD` and `SECURACV_MQTT_BIND=0.0.0.0`
in a `.env` file — the compose file's header walks through it. The full
environment-variable contract is at the top of
[`docker/sidecar/entrypoint.sh`](../docker/sidecar/entrypoint.sh) and in the
[table below](#sidecar-environment).

Before the sidecar is up, the same checks run as a one-off container —
but only the broker checks mean anything there, because the daemons live in
the running container; once it is up, `docker exec securacv doctor` is the
check to use:

```bash
docker compose -f quickstart.compose.yml run --rm securacv doctor
```

### What the published port opens

Port **8799 is published from the first start** (`ports: - "8799:8799"` in
both quickstarts) and the kernel binds every interface in the container
(`SECURACV_API_BIND=all`, the default) — that is how Home Assistant pairs
over your LAN. What that opens to anyone who can reach the host:

| Route | What it gives |
|---|---|
| `GET /health` | liveness, `{"status":"ok"}` — nothing else |
| `POST /api/pair` | redeems the 6-digit code `pair` printed: five wrong guesses burn the code, every refusal is the identical `401 {"error":"invalid_pairing_code"}` and a strike against the caller's address, a code lives 10 minutes and works once |
| `GET /api/fleet` | this kernel's **own row** (name, online, chain verdict); every Canary the bridge hears only with `SECURACV_FLEET_ROLLCALL=1` |

**Every data route** — `/events`, `/events/latest`, `/digest`, `/status`,
`/export/bundle`, `/api/identity`, `/api/sealed-log`, `POST /verify` — needs
a token: the rotating **capability token**, which never leaves the
container, or the **paired token** Home Assistant earned. The API is
**plaintext HTTP**, so that paired token crosses your LAN unencrypted on
every request until TLS with a pinned fingerprint lands for the sidecar
(a tracked follow-up): keep this port on your own network and never forward
it from the internet
([the security model](security/SECURITY_MODEL.md#home-assistant-pairing-and-what-crosses-your-network)).
One more honest limit: behind Docker Desktop's userland proxy, or any
reverse proxy, every client reaches `witness_api` from one source address,
so the per-address lockout becomes a shared bucket — a LAN host guessing
codes can hold Home Assistant's polling at `429` for the lockout's length.

To keep the API inside the container instead, set `SECURACV_API_BIND:
"loopback"` **and** delete the `ports:` block — then there is no Home
Assistant pairing and no Witness Wall. Any other value refuses to start, so
a typo never silently means an open or a closed port.

### Pair it with Home Assistant

Home Assistant must use the **same broker**. The sidecar's bridge publishes a
retained, kernel-**signed announcement** on `witness/<kernel id>/announce`
(the kernel's id and verifying key, where the API listens, that pairing is
required — never a token, a code or the seed), and the integration's MQTT
discovery filter picks it up: a **SecuraCV sidecar discovered** card appears
within about a minute of the start.

```
$ docker exec securacv pair
SecuraCV pairing code: 483921   (valid 10 minutes, 5 attempts)
Kernel key fingerprint: ab12cd34   (Home Assistant shows the same fingerprint before it pairs)
Kernel URL to confirm in Home Assistant: http://<this docker host>:8799
In Home Assistant: Settings → Devices & Services → SecuraCV (discovered) → Configure → enter the code.
```

That is the whole card, printed once on that terminal; the daemon's log never
carries a code. In Home Assistant: **Configure** shows the kernel id, its
version and the **fingerprint** — compare it with the card's second line —
then the pair form asks for the **Kernel URL** (prefilled from
`SECURACV_ANNOUNCE_HOST` when you set it, else from your broker's host; type
`http://<docker host>:8799` if the guess is wrong) and the **code**. On
success Home Assistant holds a **paired token** (32 random bytes; the kernel
stores only its sha256 in `/data/paired_tokens.json`, `0600`, re-read on
every request), pins the kernel's verifying key from that authenticated
answer — never from the announcement — and claims the kernel. Revoke it any
time:

```bash
docker exec securacv unpair            # lists paired tokens: id, label, minted
docker exec securacv unpair <id>       # revoke one; its next request is refused, no restart
```

Removing the integration entry in Home Assistant revokes its token too. A
revoked token sends Home Assistant into its re-authentication flow, which
asks for a fresh code from `pair`. The exact screens and the Repairs issue a
changed key raises are in
[Path 3 of the setup guide](homeassistant_setup.md#path-3-home-assistant-container-and-the-docker-sidecar);
the wire contract is under [Pairing](homeassistant_setup.md#pairing-the-docker-sidecar-and-home-assistant).

### Sidecar environment

Only `FRIGATE_MQTT_HOST` is required. `SECURACV_API_LAN_POLICY` and
`WITNESS_API_LAN_POLICY` do not exist: there is no LAN-policy gate, the
tokens are the boundary.

| Variable | Default | Meaning |
|---|---|---|
| `FRIGATE_MQTT_HOST` | *(required)* | broker hostname Frigate publishes to (hostname, IPv4 or IPv6 literal) |
| `FRIGATE_MQTT_PORT` | `1883` | broker port |
| `MQTT_BROKER_ADDR` | — | full `host:port` override replacing the two above (`[fd00::5]:1883`) |
| `MQTT_USERNAME`, `MQTT_PASSWORD` | none | broker login (empty = anonymous broker) |
| `FRIGATE_TOPIC_PREFIX` | `frigate` | Frigate's `mqtt.topic_prefix` |
| `FRIGATE_MQTT_TOPIC` | `<prefix>/events` | legacy full-topic override (events mode only) |
| `FRIGATE_ENABLE_REVIEWS` | `false` | alerts mode: seal `<prefix>/reviews` alerts **instead of** every tracked object (Frigate 0.14+) |
| `FRIGATE_CAMERAS` | all | comma-separated camera allowlist, compared as [slugs](#camera-names-the-slug-rule) |
| `FRIGATE_LABELS` | the bridge default | comma-separated label allowlist; `face` and `license_plate` seal nothing even when listed |
| `FRIGATE_MIN_CONFIDENCE` | `0.5` | minimum detection confidence |
| `RETENTION_DAYS` | `7` | sealed-event retention |
| `TIME_BUCKET_MINUTES` | `10` | timestamp coarsening bucket |
| `VERIFY_INTERVAL_SECS` | `3600` | the kernel's own self-verification cadence (0 disables); the integration verifies independently |
| `SECURACV_PUBLISH` | `true` | publish the kernel's state, the MQTT-discovery entities and the signed announcement |
| `HA_DISCOVERY_PREFIX` | `homeassistant` | Home Assistant's discovery prefix |
| `MQTT_TOPIC_PREFIX` | `witness` | the kernel's state-topic prefix (announce, claim, `frigate_link`, events) |
| `POLL_INTERVAL` | `30` | how often the publisher polls the kernel |
| `BROKER_WAIT_SECS` | `30` | how long to wait for the broker at start |
| `SECURACV_API_BIND` | `all` | `all`: `0.0.0.0:8799` in the container, the address the quickstart publishes so Home Assistant can pair; `loopback`: `127.0.0.1:8799`, this container only (no pairing, no Wall) |
| `SECURACV_ANNOUNCE_HOST` | unset | this Docker host's LAN name or address, named in the announcement so Home Assistant prefills the kernel URL; unset, the announcement carries no host (a host is a network identifier — Home Assistant redacts it in diagnostics) and Home Assistant prefills its broker's host |
| `SECURACV_FLEET_ROLLCALL` | unset | `1` → `GET /api/fleet` lists every Canary the bridge hears (the Witness Wall's roll-call, `WITNESS_API_FLEET_ROLLCALL=full`); unset → this kernel's own row only (`self`) |
| `DEVICE_KEY_SEED` | — | 64-hex signing seed; if unset, read from `/run/secrets/device_key_seed`, then `/data/device_key`, else generated and persisted (`0600`) |

Subcommands, all run inside the running container (the image installs
`pair`, `unpair` and `doctor` as shims, so `docker exec securacv <cmd>`
works): `pair [--ttl-secs n]`, `unpair [<id>]`, `doctor [--live]`,
`entrypoint.sh mint-viewer-token --label <s> [--base-url <url>]` and
`entrypoint.sh revoke-viewer-token <id>` for a Witness Wall. `doctor` reads
the pairing files the kernel keeps beside the token — the code's sha256 and
expiry, the paired tokens' ids — and never learns a code; it reports
*paired: yes (N tokens)* or *paired: no* and *pairing code outstanding: yes
(expires in N min)* or *no*.

### Clean shutdown and recovery

`docker stop` is forwarded to the daemons and the container exits 0: the
publisher **clears its retained announcement** (an empty retained payload)
and publishes `witness/status` `offline`; `frigate_bridge` seals a final
heartbeat and a `shutdown_clean` lifecycle record and publishes
`witness/frigate_link` with `connected:false`. A sidecar removed uncleanly
leaves a stale announcement and link on the broker; clear them by hand:

```bash
mosquitto_pub -r -n -t witness/<kernel id>/announce
mosquitto_pub -r -n -t witness/frigate_link
```

### The Witness Wall on the sidecar

The published port is also what the Apple TV Witness Wall reads — type
`http://<docker-host-ip>:8799` into the Wall, with the port. `GET /api/fleet`
there lists **only the kernel's own row** by default; set
`SECURACV_FLEET_ROLLCALL=1` to list every Canary the bridge hears on the
broker (names, online state, coarse per-room wellbeing words; no keys, no
events) — the roll-call file `/data/fleet_peers.json` holds each Canary's
first-seen public key and rides in the volume you back up.

**Listed is not verified: pair the Wall for the sealed log.** Checking the
sealed log's Ed25519 signatures on the TV needs `GET /api/sealed-log`, which
wants a credential; the Wall holds a *viewer token* — minted once, good for
that one read and nothing else:

```bash
docker exec securacv entrypoint.sh mint-viewer-token \
    --label "living room tv" --base-url http://<docker-host-ip>:8799
```

It prints one JSON line, the pairing receipt (the token, the kernel's
current verifying key, a short id), and that line is the token's only copy:
the kernel keeps its sha256 in `/data/viewer_tokens.json` (`0600`, beside
the capability token) and re-reads the file on every request. The Wall
takes the receipt under Settings → Pair ([`tvos/README.md`](../tvos/README.md));
`docker exec securacv entrypoint.sh revoke-viewer-token <id>` takes it
back. Like the paired token it crosses the LAN in plaintext.

Verify the sealed log from the host at any time:

```bash
docker exec securacv sh -c 'DEVICE_KEY_SEED=$(cat /data/device_key) log_verify --db /data/witness.db'
```

---

## What the bridge keeps and what it discards

Identity stripping happens **at parse time**, with tolerant deserializers:
every key outside the kept set is parsed to nothing, whatever its shape, and
never fails the message. So the `[name, score]` `sub_label` pair that an
older bridge could not deserialize — and therefore dropped the whole event
— now simply disappears, and the event is sealed. Nothing here is a filter
you could turn off; the fields are never read into a value.

### `<prefix>/events` (events mode)

Kept, from the `after` object (and `before` only for the message `type`):

| Field | Use |
|-------|-----|
| `camera` | the zone id when the object entered no named zone (`zone:<camera>`), and the camera allowlist |
| `label` | the base label only, mapped to the event type below |
| `score` / `top_score` | confidence — `top_score` wins when present |
| `entered_zones` / `current_zones` | the zone id, `entered_zones` preferred, `current_zones` the fallback |
| `false_positive` | an object Frigate marked false-positive seals nothing |
| the message `type` | `new` seals; `update` and `end` seal nothing; an unknown type is rejected; an absent type is accepted for older schemas |

Discarded at parse, never sealed, in any shape: `sub_label` (a recognized
face name, a known-plate alias, a delivery logo, a 0.17 custom-classification
class — null, a string or a `[name, score]` pair), `recognized_license_plate`,
`recognized_license_plate_score`, `attributes`, `current_attributes`,
`path_data`, `box`, `region`, `area`, `ratio`, `average_estimated_speed`,
`velocity_angle`, `current_estimated_speed`, `max_estimated_speed`, `id`,
`frame_time`, `start_time`, `end_time`, `snapshot_time`, `snapshot`,
`thumbnail`, `has_snapshot`, `has_clip`, `motionless_count`,
`position_changes`, `stationary`, `active`, `pending_loitering`,
`model_hash`, `model_type`, `detector_type`, **and any unknown key**. The
discard list is exported as data (`DISCARDED_EVENT_FIELDS`) so the tests can
prove every fixture populates each entry with a sentinel and that none
reaches the parsed claim, the sealed payload, `/events`, the MQTT payloads,
the Home Assistant attributes or the export.

Label → event type (`spec/witness_dictionary.json` ids; Home Assistant labels
the two classes *Person or vehicle* and *Animal or small object*):

| Frigate label | Event type |
|---|---|
| `person`, `car`, `truck`, `bus`, `motorcycle` | `boundary_crossing_object_large` |
| `dog`, `cat`, `bird`, `animal`, `bicycle`, `skateboard`, `package`, `box`, anything else allowed | `boundary_crossing_object_small` |
| `face`, `license_plate` | **refused** — no event, even when listed in the allowlist |

The default allowlist is `person, car, dog, cat` (the add-on's
`frigate.labels`; the bridge's own default adds `bird, bicycle,
motorcycle`). There is no `label:sub_label` fold any more: the sealed label is
the base class, and dedupe is per camera + base label + ten-minute window.

### `<prefix>/reviews` (alerts mode)

Kept: `camera`, `severity`, `data.objects`, `data.zones` (and
`before.severity`, read only to detect the transition). Discarded: `id`,
`start_time`, `end_time`, `thumb_path`, `data.detections`, `data.sub_labels`,
`data.audio`, and the 0.17 GenAI fields `metadata`, `description`, `title`,
`scene`, `other_concerns` (also under `data.metadata`), and any unknown key.
A review whose objects are only `face` / `license_plate` seals nothing; a
mixed list is filtered of them.

### Camera names: the slug rule

The camera allowlist (`--cameras` / `FRIGATE_CAMERAS`, the wizard's
`frigate.cameras`) is compared as **slugs on both sides**: ASCII-lowercase,
every other code point becomes `_`, truncated to 64 characters, no trimming
or collapsing — the same rule the app's wizard writes the list with, so a
name typed with a space no longer silently drops every event of that camera.

| You typed | Slug |
|---|---|
| `Front Door` | `front_door` |
| `front-door` | `front_door` |
| `Front  Door` | `front__door` |
| `Garage_2` | `garage_2` |
| `Café` | `caf_` |
| `BACK yard!` | `back_yard_` |

Zone ids keep their own rule (`zone:` + lowercase letters, digits, `_` and
`-`), so existing `zone:…` ids do not move.

## Events mode and alerts mode

**Events mode** (default) seals every tracked object at its `new` message:
one claim per camera, base label and ten-minute window; later `update` and
`end` messages for the same object seal nothing. Three people at the door in
one window are one claim — the coarsening is the privacy guarantee.

**Alerts mode** (`--enable-reviews` / `FRIGATE_ENABLE_REVIEWS` / the wizard's
*What to seal: only Frigate alerts*) subscribes to `<prefix>/reviews`
**instead of** `<prefix>/events` and seals only review items whose severity is
`alert` — exactly once, at the message that made them an alert: a `new`
message already at `alert`, or the `update` that promotes a `detection` to
`alert`. An `end` seals nothing, a detection seals nothing, and the claim
carries confidence 1.0 (Frigate's review items have no score). The event
type is the coarsest class any allowed object maps to (a person and a dog
in one alert is a large-object crossing); dedupe is per camera and window.
This is a change from earlier releases, where the flag added reviews
*beside* events.

## The bridge's housekeeping

In Frigate mode no `witnessd` runs, so `frigate_bridge` — the process that
appends — owns the chain's housekeeping, on timers that run between messages
and through broker outages:

- **Boot-time verification.** At start the bridge verifies the sealed log's
  tail (honoring `SECURACV_HWM_PATH`, the external high-water mark: it
  refuses to extend a rolled-back chain). A failing tail puts the process in
  **safe mode**: it idles, logs `SAFE MODE active — not appending` every
  60 s, connects to nothing and seals nothing, while `witness_api` keeps
  serving so the record can be read and exported. Appending onto an
  untrusted tail would launder the tampering.
- **Lifecycle records.** `start` at boot — a trailing `start` from the
  previous run seals a `PowerLoss` record first — and `shutdown_clean` on
  SIGTERM/SIGINT. s6 and `docker stop` deliver SIGTERM, so a restart no
  longer looks like a power cut.
- **Heartbeats.** One per ten-minute bucket, checked every 300 s, plus one
  at start and a final one at clean shutdown, carrying per-bucket counter
  deltas (`ingest_healthy` = MQTT connected; frames always 0; events and
  failures real).
- **Retention and checkpoints.** A retention pass at start and every
  `--retention-check-interval-secs` (`WITNESS_RETENTION_CHECK_INTERVAL_SECS`,
  default 3600) prunes rows older than `--retention-secs`
  (`WITNESS_RETENTION_SECS`, default 7 days; `RETENTION_DAYS` in the
  sidecar) by their bucket start and writes a signed checkpoint over the
  prune cutoff, so verify walks stay bounded and `log_verify` sees
  checkpoints. `--checkpoint-interval-secs`
  (`WITNESS_CHECKPOINT_INTERVAL_SECS`, default 86400) runs **that same
  pass**: the store's only checkpoint primitive is the retention pass, which
  checkpoints the cutoff of what it prunes — a checkpoint whose cutoff rows
  were not pruned would break every verifier's chain walk
  (`spec/evidence_envelope.md`) — so the timer matters only when the
  retention interval is set longer than a day.

Operator-visible consequence: a bridge restart now writes lifecycle and
heartbeat rows, and `retention_days` is actually enforced.

Flags and environment the bridge reads (clap; every flag has the env form):

| Flag | Env | Default |
|---|---|---|
| `--mqtt-broker-addr` | `MQTT_BROKER_ADDR` | `127.0.0.1:1883` |
| `--allow-remote-mqtt` | `ALLOW_REMOTE_MQTT` | off — a non-loopback broker needs it |
| `--mqtt-username` / `--mqtt-password` | `MQTT_USERNAME` / `MQTT_PASSWORD` | none |
| `--mqtt-use-tls`, `--mqtt-tls-ca-path`, `--mqtt-tls-client-cert-path`, `--mqtt-tls-client-key-path`, `--mqtt-tls-backend` | `MQTT_USE_TLS`, `MQTT_TLS_CA_PATH`, `MQTT_TLS_CLIENT_CERT_PATH`, `MQTT_TLS_CLIENT_KEY_PATH`, `MQTT_TLS_BACKEND` | off; `classic` |
| `--frigate-topic` | `FRIGATE_MQTT_TOPIC` | `frigate/events` — legacy full-topic override, events mode only |
| `--frigate-topic-prefix` | `FRIGATE_TOPIC_PREFIX` | `frigate` |
| `--enable-reviews` | `FRIGATE_ENABLE_REVIEWS` | off — alerts mode |
| `--mqtt-client-id` | `MQTT_CLIENT_ID` | the bridge's name |
| `--mqtt-topic-prefix` | `MQTT_TOPIC_PREFIX` | `witness` — the state prefix `frigate_link` is published under |
| `--ha-device-id` | `HA_DEVICE_ID` | `pwk_` + 8 hex of sha256(`securacv:ha-device-id:v1:` ‖ seed) — the derivation the app, the sidecar and `event_mqtt_bridge` share |
| `--db-path` | `WITNESS_DB_PATH` | `witness.db` |
| `--ruleset-id` | `WITNESS_RULESET_ID` | `ruleset:frigate_v1` |
| `--bucket-size-secs` | `WITNESS_BUCKET_SIZE` | `600` (minimum 300) |
| `--min-confidence` | `FRIGATE_MIN_CONFIDENCE` | `0.5` |
| `--cameras` | `FRIGATE_CAMERAS` | all (slugs) |
| `--labels` | `FRIGATE_LABELS` | `person, car, dog, cat, bird, bicycle, motorcycle` |
| `--retention-secs` | `WITNESS_RETENTION_SECS` | 7 days |
| `--retention-check-interval-secs` | `WITNESS_RETENTION_CHECK_INTERVAL_SECS` | `3600` |
| `--checkpoint-interval-secs` | `WITNESS_CHECKPOINT_INTERVAL_SECS` | `86400` |

The sidecar's `entrypoint.sh` and the app's `run.sh` pass
`--mqtt-topic-prefix` and `--ha-device-id` so `frigate_link` names the same
prefix and id as the publisher's announcement.

## Frigate link health (`witness/frigate_link`)

"Quiet night" and "Frigate stopped publishing" used to look the same. The
bridge now publishes one retained QoS 1 payload on `<state_prefix>/frigate_link`
(no last will of its own — liveness stays on `witness/status`):

| Field | Meaning |
|---|---|
| `v` | `1` |
| `device_id` | the bridge's Home Assistant device id (`pwk_…`) |
| `connected` | the bridge holds a live broker connection with its Frigate subscription; `false` is published on clean shutdown |
| `topic_prefix` | Frigate's `mqtt.topic_prefix` |
| `last_message_bucket_start` | start of the ten-minute window in which the last message on the subscribed Frigate topic arrived — any type (`new`, `update`, `end` all count as "Frigate heard"); `null` until one has; never event-precise |
| `last_message_bucket_size` | `600` |
| `messages_since_start` | messages received on the subscribed topic since the process started (rides along; never triggers a publish by itself) |
| `mode` | `events` \| `reviews` |

```json
{"v":1,"device_id":"pwk_1a2b3c4d","connected":true,"topic_prefix":"frigate","last_message_bucket_start":1760140800,"last_message_bucket_size":600,"messages_since_start":12,"mode":"events"}
```

Republished on a change of `connected` or of the last-message window, on
every reconnect, and at least every 300 s — never per Frigate message. The
integration's **Frigate link** sensor (diagnostic, connectivity) is **on**
iff `connected` is true **and** the last message is within the last 60
minutes, and shows the payload as attributes; the dashboard header says
*Frigate not heard for 35 min — check Frigate → Settings → MQTT*. A stale
retained payload from a bridge that died uncleanly:
`mosquitto_pub -r -n -t witness/frigate_link`. Safe mode is log-only: the
bridge does not connect in safe mode, so Home Assistant sees the absence of
`frigate_link` updates and, eventually, the publisher's last will.

## Receipts: what a read costs

Exports are disclosure acts and leave a tamper-evident receipt in the
sealed log; state reads do not. On the kernel API:

| Route | Receipt |
|---|---|
| `GET /events` (bulk, no cursor) and `GET /export/bundle` | **yes** — one each; under a paired token the receipt reads `auth_mode: paired` with the token id |
| `GET /events?since_bucket=…`, `/events/latest`, `/digest`, `/api/identity`, `/api/sealed-log[?from_seq=…]` | **no** — pure state reads |

The publisher polls `GET /events?since_bucket=<cursor>` every cycle, the
integration's chain walk reads `/api/sealed-log?from_seq=`, and its backstop
reads `/health`, `/api/identity` and `/status` — so a running Home Assistant
adds **zero** receipts; only `securacv.export_evidence` (one `/export/bundle`
per pack) does. Receipts are never pruned.

---

## Architectural Validity

**Question**: Does this integration violate the kernel's privacy invariants?

**Answer**: No. Here's why:

### What the frigate_bridge Does

```
Frigate publishes (0.17):                 SecuraCV seals:
┌──────────────────────────────────┐      ┌─────────────────────────────┐
│ id: "1706140823.4-abc123"        │  →   │ (discarded)                 │
│ start_time: 1706140823.4         │  →   │ time_bucket: 1706140800/600 │
│ camera: "Front Door"             │  →   │ zone_id: "zone:front_door"  │
│ label: "person"                  │  →   │ event_type: ...ObjectLarge  │
│ sub_label: ["Jane", 0.91]        │  →   │ (discarded)                 │
│ recognized_license_plate: "..."  │  →   │ (discarded)                 │
│ score: 0.92, top_score: 0.95     │  →   │ confidence: 0.95            │
│ box, region, area, path_data     │  →   │ (discarded)                 │
│ thumbnail, snapshot, has_clip    │  →   │ (discarded)                 │
│ current_zones: ["yard"]          │  →   │                             │
│ entered_zones: ["porch"]         │  →   │ zone_id: "zone:porch"       │
│ attributes, speeds, model_hash…  │  →   │ (discarded)                 │
└──────────────────────────────────┘      └─────────────────────────────┘
```

### Invariant Compliance

| Invariant | How It's Enforced |
|-----------|-------------------|
| **I. No Raw Export** | frigate_bridge never receives raw video — only MQTT event metadata, and `thumbnail` / `snapshot` are discarded at parse |
| **II. No Identity** | `sub_label`, `recognized_license_plate*`, `attributes` and the tracking `id` are parsed to nothing; `face` and `license_plate` labels are refused outright; `ObjectClass` is never `Face` or `LicensePlate` |
| **III. Metadata Min** | timestamps coarsened to ten-minute buckets; `frigate_link`'s last-message time is a bucket start; the announcement names no host unless the operator sets one |
| **IV. Local Ownership** | sealed log stored locally on the hub or in the sidecar's volume |
| **V. Break-Glass** | vault access still requires quorum (unchanged) |
| **VI. No Retroactive** | events bound to `ruleset:frigate_v1` at creation |
| **VII. Non-Queryable** | `/api/sealed-log` takes one sequential cursor and no selector; `/events?since_bucket=` is a bucket cursor; no bulk search |

### Trust Boundary

The frigate_bridge is classified as an **external tool** per `kernel/architecture.md` §3.3:
- It cannot access raw media (Frigate doesn't publish it to MQTT)
- It cannot bypass Contract Enforcement (`append_event_checked` validates all events)
- It cannot access the sealed log directly for reads (uses the kernel API); it appends through the kernel library and owns the chain's housekeeping because no `witnessd` runs in Frigate mode

---

## When to Use Frigate Mode vs Standalone

### Use Frigate Mode When:
- You already have Frigate set up
- You want better detection accuracy (Coral TPU, TensorFlow)
- You want real-time alerts AND privacy-preserving long-term logging
- You want to reduce Frigate's recording retention but keep event history

### Use Standalone Mode When:
- You don't have Frigate
- You want a simpler setup with just cameras
- You want complete independence from other systems

---

## Configuration Reference

### App Options (frigate mode)

| Option | Default | Description |
|--------|---------|-------------|
| `frigate.mqtt_host` | (empty = auto-discover) | MQTT broker hostname; empty means the Supervisor's Mosquitto service is auto-discovered — set only for an external broker |
| `frigate.mqtt_port` | `1883` | MQTT broker port |
| `frigate.topic_prefix` | `frigate` | Matches Frigate's `mqtt.topic_prefix`; events arrive on `<prefix>/events` |
| `frigate.enable_reviews` | `false` | Alerts mode: seal `<prefix>/reviews` alerts **instead of** every tracked object (Frigate 0.14+) — the wizard's *What to seal* |
| `frigate.mqtt_topic` | (empty) | Legacy full-topic override — wins over `topic_prefix` when set (events mode only) |
| `frigate.mqtt_username` | (empty) | MQTT authentication username |
| `frigate.mqtt_password` | (empty) | MQTT authentication password |
| `frigate.min_confidence` | `0.5` | Minimum detection confidence |
| `frigate.cameras` | `[]` (all) | Camera **slugs** to process (the wizard writes them; compared as slugs) |
| `frigate.labels` | `[person,car,dog,cat]` | Object types to process; `face` and `license_plate` are refused whatever is listed |
| `retention_days` | `7` | How long sealed events stay (the wizard offers 7 / 30 / 365) |
| `verify_interval_hours` | `1` | The kernel's own sealed-log self-check (0 disables); the integration verifies independently |
| `install_integration` | `true` | Let the app place its carried copy of the integration on your click and keep that copy updated ([what it does and never does](../privacy_witness_kernel/DOCS.md#the-integration-carried-in-this-app)) |

(Defaults mirror [`privacy_witness_kernel/config.yaml`](../privacy_witness_kernel/config.yaml) — the app schema is the source of truth.)

### MQTT Publishing Options (HA Discovery)

| Option | Default | Description |
|--------|---------|-------------|
| `mqtt_publish.enabled` | `true` | Publish the kernel's state to Home Assistant over MQTT — on by default so events are visible out of the box |
| `mqtt_publish.host` | (empty = auto-discover) | MQTT broker for publishing; auto-discovered like the Frigate broker |
| `mqtt_publish.port` | `1883` | MQTT port |
| `mqtt_publish.username` | (empty) | MQTT auth username |
| `mqtt_publish.password` | (empty) | MQTT auth password |
| `mqtt_publish.topic_prefix` | `witness` | Prefix for state topics |
| `mqtt_publish.discovery_prefix` | `homeassistant` | HA discovery prefix |

When `mqtt_publish.enabled` is `true`, the publisher (`event_mqtt_bridge`):
1. Publishes the kernel's **signed announcement** (retained
   `<prefix>/<kernel id>/announce`) so the integration discovers it; the app
   announces that its API binds all interfaces and that Home Assistant reads
   the token file, the sidecar that pairing is required
2. Publishes the kernel's state under `<prefix>/…` (last event, events,
   digest, chain problem, status, `claim_state`) and MQTT-discovery configs
   for the `pwk_*` entities Home Assistant shows before the integration claims
   the kernel — [the topic table](homeassistant_setup.md#the-kernels-mqtt-topics-and-the-unclaimed-bridge)
3. Honors the integration's **claim** (`<prefix>/claim/<kernel id>`, fresher
   than 24 h) by withdrawing those discovery configs, and republishes them
   when it clears
4. Publishes availability with a last will (`<prefix>/status`), QoS 1
   throughout, and clears its announcement on a clean shutdown

### MQTT TLS Options (Bridges)

Both `frigate_bridge` (subscribe) and `event_mqtt_bridge` (publish) support **MQTT v5** and TLS.
Use a `mqtts://` broker address or set `MQTT_USE_TLS=1` to enable TLS, then configure certificates
as needed:

| Environment Variable | Description |
|---|---|
| `MQTT_USE_TLS` | Enable TLS (required for `mqtts://` brokers) |
| `MQTT_TLS_CA_PATH` | Path to PEM-encoded CA certificate |
| `MQTT_TLS_CLIENT_CERT_PATH` | Path to PEM-encoded client certificate (mutual TLS) |
| `MQTT_TLS_CLIENT_KEY_PATH` | Path to PEM-encoded client private key (mutual TLS) |

### Standalone CLI Usage

```bash
# Generate device key
export DEVICE_KEY_SEED=$(openssl rand -hex 32)

# Run with Frigate (loopback MQTT), events mode
cargo run --bin frigate_bridge -- \
  --mqtt-broker-addr 127.0.0.1:1883 \
  --frigate-topic-prefix frigate \
  --db-path witness.db

# Alerts mode: seal only Frigate review alerts, from <prefix>/reviews
cargo run --bin frigate_bridge -- \
  --mqtt-broker-addr 127.0.0.1:1883 \
  --frigate-topic-prefix frigate \
  --enable-reviews \
  --db-path witness.db

# For the Home Assistant app (non-loopback MQTT) — explicitly allow, and
# name the state prefix and device id the publisher uses
cargo run --bin frigate_bridge -- \
  --allow-remote-mqtt \
  --mqtt-broker-addr core-mosquitto:1883 \
  --mqtt-username homeassistant \
  --mqtt-password your_password \
  --frigate-topic-prefix frigate \
  --mqtt-topic-prefix witness \
  --db-path witness.db

# TLS example (mqtts:// broker with custom CA)
cargo run --bin frigate_bridge -- \
  --allow-remote-mqtt \
  --mqtt-broker-addr mqtts://core-mosquitto:8883 \
  --mqtt-use-tls \
  --mqtt-tls-ca-path /config/mqtt/ca.pem \
  --frigate-topic-prefix frigate \
  --db-path witness.db
```

### Publish Events to HA with MQTT Discovery

```bash
# One-shot mode (publish current events; reads the receipt-less /events?since_bucket=0)
cargo run --bin event_mqtt_bridge -- \
  --allow-remote-mqtt \
  --mqtt-broker-addr core-mosquitto:1883 \
  --api-token-path /config/api_token

# Daemon mode (continuous publishing, the signed announcement, the claim)
cargo run --bin event_mqtt_bridge -- \
  --daemon \
  --allow-remote-mqtt \
  --mqtt-broker-addr core-mosquitto:1883 \
  --ha-discovery-prefix homeassistant \
  --mqtt-topic-prefix witness \
  --api-token-path /config/api_token \
  --announce-api-bind all \
  --announce-pairing token_file

# TLS example (mqtts:// broker with mutual TLS)
cargo run --bin event_mqtt_bridge -- \
  --daemon \
  --allow-remote-mqtt \
  --mqtt-broker-addr mqtts://core-mosquitto:8883 \
  --mqtt-use-tls \
  --mqtt-tls-ca-path /config/mqtt/ca.pem \
  --mqtt-tls-client-cert-path /config/mqtt/client.crt \
  --mqtt-tls-client-key-path /config/mqtt/client.key \
  --api-token-path /config/api_token
```

The announcement flags: `--announce-api-bind loopback|all`
(`ANNOUNCE_API_BIND`, default `loopback`), `--announce-api-host`
(`ANNOUNCE_API_HOST`, unset — a host is a network identifier),
`--announce-api-scheme http|https` (`ANNOUNCE_API_SCHEME`, default `http`),
`--announce-pairing required|token_file` (`ANNOUNCE_PAIRING`, default
`required`) and `--no-announce` (`NO_ANNOUNCE`). The app passes
`--announce-api-bind all --announce-pairing token_file --announce-api-host
<its hostname>`; the sidecar passes `--announce-api-bind "$API_BIND_MODE"
--announce-pairing required` and the host only when
`SECURACV_ANNOUNCE_HOST` is set. A word outside a flag's vocabulary refuses
to start: a typo must not publish a posture the integration misreads. The
bridge holds no signing key — it asks the kernel for the signature over
`POST /api/sign-announce`, and never publishes an announcement without one.

---

## Example: Privacy-First Setup

Here's how to configure Frigate + SecuraCV for maximum privacy:

### Frigate Config

> **Where this file lives:** the Frigate **Home Assistant app** (0.16+) reads
> `/addon_configs/ccab4aaf_frigate/config.yml` (editable from the Frigate Web UI's
> configuration editor or a file editor app) — *not* `/config/frigate.yml`, which
> SecuraCV only uses as a generated template. In a **Docker** deployment it's whatever
> config file you mount into the Frigate container.

```yaml
mqtt:
  enabled: true
  host: core-mosquitto

cameras:
  front_door:
    ffmpeg:
      inputs:
        - path: rtsp://admin:pass@192.168.1.100:554/stream
          roles: [detect]
    detect:
      enabled: true

# Minimal recording - just for real-time viewing
record:
  enabled: true
  retain:
    days: 1  # Delete raw video after 1 day

# Disable snapshots to reduce stored data
snapshots:
  enabled: false
```

### SecuraCV App Config

```yaml
mode: "frigate"
frigate:
  min_confidence: 0.6
  labels: ["person", "car"]  # Only seal these
  enable_reviews: false      # or true: only Frigate's alerts, one claim per camera and window
retention_days: 365          # a year of signed claims (the wizard's "1 year")
```

**Result**:
- Frigate handles detection and 1-day video retention
- SecuraCV keeps a privacy-preserving, signed event log for a year
- No long-term raw video storage
- No object tracking IDs, recognized faces or plates in long-term storage —
  they are never parsed into a value, so there is nothing to leak

---

## Troubleshooting

### Events Not Appearing

1. **Check Frigate MQTT is enabled**:
   ```yaml
   # Frigate config (see "Where this file lives" above)
   mqtt:
     enabled: true
   ```

2. **Verify MQTT connectivity**:
   ```bash
   mosquitto_sub -h core-mosquitto -t "frigate/events" -v
   ```
   You should see events when objects are detected. In alerts mode the
   bridge listens on `frigate/reviews` instead — a camera with no `alert`
   review items seals nothing there, by design.

3. **Check the Frigate link**: Home Assistant's **Frigate link** sensor and its
   attributes (`last_message_bucket_start`, `mode`), or
   `mosquitto_sub -t witness/frigate_link -v`. `connected: true` with no
   recent window means Frigate is not publishing; `false` means the bridge
   lost the broker.

4. **Check the bridge's log**: **Settings → Apps → SecuraCV → Logs**, or
   `docker logs securacv`. `SAFE MODE active — not appending` means the
   sealed log's tail failed boot verification: export and `log_verify` the
   database before anything else; the bridge seals nothing until the tail is
   trusted again.

5. **Camera allowlist**: names are compared as slugs — `Front Door` matches
   `front_door`. A camera missing from `frigate.cameras` seals nothing.

### "MQTT broker must be loopback"

This error occurs when running frigate_bridge standalone without `--allow-remote-mqtt`. Either:
- Use `--allow-remote-mqtt` flag for trusted networks
- Or run MQTT broker on localhost

### The pairing code is not accepted

Codes are single-use, live ten minutes and burn after five wrong tries; the
kernel answers every refusal the same way. Run `docker exec securacv pair`
again and enter the new code within ten minutes. Behind one shared source
address (Docker Desktop's proxy, a reverse proxy) another host's wrong
guesses can lock Home Assistant's address out with `429` for a while — wait
it out.

### No "SecuraCV sidecar discovered" card

Home Assistant and the sidecar must share one broker. `docker exec securacv
doctor` must report the retained announcement present; a sidecar removed
uncleanly leaves none — restart it, or clear a stale announcement with
`mosquitto_pub -r -n -t witness/<kernel id>/announce`. A non-default
`MQTT_TOPIC_PREFIX` is not discovered (the integration subscribes
`witness/+/announce`); add the kernel through the **Witness Kernel
Connection** form instead, or keep the default prefix.

### Low Detection Rate

- Check `min_confidence` - lower it to catch more events
- Verify camera names match Frigate's config (compared as slugs)
- In alerts mode only Frigate's `alert`-severity review items seal; a
  detection-only review never does — switch to events mode to seal every
  tracked object

---

## Security Considerations

### Why `--allow-remote-mqtt` is Safe in HA

The `--allow-remote-mqtt` flag is needed because HA runs services in separate containers. This is safe because:

1. **No raw media flows through MQTT** - Only sanitized event metadata
2. **Events are still validated** - Contract Enforcer rejects non-conforming events
3. **HA network is trusted** - All containers run on the same host
4. **No new attack surface** - The data received is already public to any HA addon

### What the sidecar puts on your network

- The published port 8799 and what each open route gives are
  [above](#what-the-published-port-opens); every data route needs a token,
  and the paired token is plaintext on the LAN until TLS lands for the
  sidecar.
- The retained **announcement** on your own broker carries the kernel's id,
  its verifying key (public) and where its API listens — a network identifier
  only when you set `SECURACV_ANNOUNCE_HOST` — signed by the kernel so the
  integration can refuse an announcement a pinned kernel did not sign. It is
  fleet-internal reconciliation on a broker you run, not telemetry: nothing
  leaves your network, and the broker's own access control is the boundary
  ([the threat model](security/THREAT_MODEL.md)).
- `witness/frigate_link` says when Frigate was last heard, as a ten-minute
  window — never which camera or what it saw.

### What's NOT Sent to SecuraCV

Even with Frigate integration, SecuraCV never keeps:
- Video frames, thumbnails or snapshots
- Object bounding boxes, regions, paths or speeds
- Recognized faces, plate strings or sub-labels of any kind
- Object tracking ids or trajectories
- Precise timestamps

---

## Further Reading

- [Home Assistant Setup](homeassistant_setup.md) - the three install paths, every entity, the dashboard, the actions
- [The product design](design/ha_frigate_product.md) - the journeys and decisions behind this release
- [Blueprints](blueprints/README.md) - the witness notification with proof, the daily digest
- [RTSP Camera Setup](rtsp_setup.md) - Standalone mode with cameras
- [Invariants Specification](../spec/invariants.md) - Privacy guarantees
- [Architecture](../kernel/architecture.md) - System design
