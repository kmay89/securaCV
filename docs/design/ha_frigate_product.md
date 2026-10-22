# SecuraCV for Home Assistant + Frigate — the product, as built

**Status: built.** This is the product spec in its final form, written for the
repository after the work merged: what a Frigate user installs, what they see,
and the sixteen decisions the implementation follows — with the places where
the review and the code moved a decision, and the one item deferred. The
user-facing guides are [`homeassistant_setup.md`](../homeassistant_setup.md)
and [`frigate_integration.md`](../frigate_integration.md); this document is
the *why* behind them and the contract the next change is held to.

## The goal

**The verified witness log for Frigate and Home Assistant.** A Frigate user
installs one thing, types nothing (Home Assistant OS) or one 6-digit code
(Docker), and within minutes sees Frigate events arriving as sealed, signed
witness events in Home Assistant: a chain-status header, per-camera witness
event entities, a daily digest, notifications with proof attached, and a
one-tap signed evidence export a recipient can verify offline. Privacy stays
structural: every identity field Frigate 0.16+/0.17 emits is discarded at
parse time.

## The three journeys

**J1 — Home Assistant OS.** App Store → add the repository → install
**SecuraCV** → Start → the ingress wizard finds Mosquitto and Frigate, reads
Frigate's cameras from its config (no RTSP screen), has nothing to type →
done screen, primary button **Install the integration and restart Home
Assistant** (option on by default; never automatic) → after the restart the
**SecuraCV discovered** card → Configure (zero fields) → the one-time
notification *Add the dashboard: Settings → Dashboards → Add dashboard →
SecuraCV* → the first Frigate event shows **Sealed ✓** within a minute of a
detection; **Verified ✓** follows the integration's next chain check.

**J2 — HACS first.** HACS → SecuraCV → Add Integration → **Automatic** finds
the app (Supervisor discovery, zero fields) or a sidecar (its signed MQTT
announcement, one code). Neither: the flow's final step says so with
my.home-assistant.io links, creates the MQTT-only entry for Canary devices,
and raises Repairs `no_kernel_found` (dismissable; cleared when a kernel
entry loads).

**J3 — Home Assistant Container + the Docker sidecar.** `docker compose up -d`
(the owner edits two lines: `FRIGATE_MQTT_HOST` and the broker credentials)
→ the sidecar announces itself on MQTT, signed → the **Add the SecuraCV
sidecar** card → Configure shows the kernel URL (prefilled from
`SECURACV_ANNOUNCE_HOST` when set, else the broker host of Home Assistant's
MQTT entry, editable) and asks for the 6-digit code `docker exec securacv
pair` prints beside the kernel's key fingerprint → the integration exchanges
it for a long-lived paired token (hashed at rest, revocable) → the same
product as J1.

## Non-negotiables

- Invariants I–VII; no identity reaches the sealed log, MQTT, Home Assistant,
  the export or diagnostics.
- **"Verified ✓" means the integration verified** Ed25519 signatures and the
  hash chain against the key it pinned over the authenticated HTTP channel at
  pairing or discovery. A kernel self-report is never "Verified"; it is
  *Chain OK (kernel-reported)*, in attributes only.
- Actions never mutate trust. Repairs only for fixable or actionable
  conditions (plus one informational, dismissable migration notice). Facts
  are event entities and PROBLEM binary sensors.
- No unauthenticated path to any evidence or coarse record: nothing under
  `/config/www` or `/local`; exports are served by an authenticated
  `HomeAssistantView` through 24-hour signed links.
- No automatic Core restart from the app, ever. No `hassio_role`.
- Coarse time on every new wire field (bucket starts, never event-precise
  epochs).
- US spellings; "fleet", never the bird word; commit format
  `<type>(<scope>): <description>`.

## The decisions, as built

Each decision is stated as the code implements it. *Review-driven* marks a
place where the review of the implementation, or the code itself, moved the
original decision; *as specified* means it landed unchanged.

**D1 — Home Assistant floor 2026.5.0** (`hacs.json`, the app's
`homeassistant:`, every blueprint's `min_version`). Canary devices hang off
the kernel device through `via_device_id` on cores from 2026.8 and
`via_device` before (chosen from the installed core's `DeviceInfo`
annotations). MQTT-discovery devices are never linked (the 2026.8
single-config-entry rule); the dashboard strategy groups them by the shared
id. *As specified.*

**D2 — No `hassio_role`.** The wizard's "Install it for me" buttons are
my.home-assistant.io links; a Core restart is `POST
/core/api/services/homeassistant/restart` (`homeassistant_api: true`
suffices) and only on a click. Mosquitto is detected through
`/services/mqtt`, Frigate through the mapped
`/addon_configs/ccab4aaf_frigate/config.yml` (fallback `/config/frigate.yml`).
*Review-driven:* the my-HA redirect that takes an `addon` parameter is
`supervisor_addon`, not `supervisor_store`, so the links are
`…/redirect/supervisor_addon/?addon=core_mosquitto` and
`…/redirect/supervisor_addon/?addon=ccab4aaf_frigate&repository_url=…`
(one click registers the Frigate repository too).

**D3 — The sidecar binds all interfaces and the quickstart publishes 8799
from the first start.** The security boundary is the tokens: every data route
needs one (rotating capability or paired); `POST /api/pair` is the only
unauthenticated write and is bounded per D4; `GET /api/fleet` under the
sidecar serves only the kernel's own row unless `SECURACV_FLEET_ROLLCALL=1`.
There is no "LAN policy" gate — a running listener cannot rebind, and the
gate added nothing the tokens do not. *As specified;* `SECURACV_API_BIND=loopback`
remains the opt-out, and a word outside the vocabulary refuses to start.

**D4 — Pairing.** A 6-digit code, TTL 600 s, a per-code attempt budget of 5
(the code is burned after the fifth wrong guess, the decrement written
durably before each refusal), single-use, consumed durably before the token
is returned, compared in constant time, never logged. `/api/pair` answers
the **same** `401 {"error":"invalid_pairing_code"}` for wrong, absent, burned
or expired codes and for a kernel with pairing off (no oracle); each wrong
guess is also a per-address strike; malformed input is a `400` and not a
strike. Paired tokens are 32 random bytes, sha256 at rest in
`paired_tokens.json` (`0600`), the same rights as the capability token,
re-read per request, revocable by id — CLI `revoke-paired-token`, HTTP
`DELETE /api/pair/<id>` with the capability token, or a paired token
revoking only itself (`403 forbidden` otherwise). The rotating capability
token gets a 90-second previous-bucket grace. Export receipts under a paired
token record `auth_mode: paired` with the token id. *Review-driven:* a
pairing success does **not** clear the address's strikes (the viewer rule —
only the first authenticated request with the new token does), so a code
success cannot reset the lockout that guards the wider credential; and the
receipt's token id is a trailing optional `token_id` field beside the
`auth_mode` string rather than data inside the enum, so every verifier that
treats `auth_mode` as an opaque word keeps working (the offline viewer's
serializer was extended to include it in the signed bytes).

**D5 — Receipts.** `/events/latest`, `/digest`, `/api/identity` and
`/api/sealed-log` are receipt-less state reads; `GET /events` in bulk and
`GET /export/bundle` log receipts; receipts are never pruned. The publisher
polls `/events?since_bucket=<cursor>` so its polling adds zero receipts, and
the integration's chain walk and backstop add none either. *Review-driven:*
the publisher's one-shot mode reads the receipt-less view as well — a
projection onto the owner's own broker is not a disclosure act.

**D6 — `frigate_bridge` owns the chain's housekeeping** in Frigate mode
(there is no `witnessd`): heartbeats checked every 300 s (one sealed per
bucket, plus one at start and at clean shutdown), a retention pass at start
and every 3600 s that prunes and checkpoints the cutoff, a
`--checkpoint-interval-secs` timer (86400 s), honoring `SECURACV_HWM_PATH`.
*Review-driven, twice:* the bridge also verifies the sealed log's tail at
boot and enters a report-but-not-append **safe mode** when it fails, and
seals `start` / `shutdown_clean` lifecycle records with `PowerLoss`
detection, because the timeline audit only checks heartbeat gaps inside
lifecycle segments and honoring the high-water mark means refusing to
extend a rolled-back chain, not only advancing the mark. And the checkpoint
timer **runs the retention pass**, not a head checkpoint: a
`write_checkpoint_at_head()` primitive was deliberately not built, because a
checkpoint whose cutoff rows are not pruned breaks every verifier's chain
walk (`spec/evidence_envelope.md`) — the store's only checkpoint primitive is
the retention pass, which checkpoints the cutoff of what it prunes. The
timer therefore matters only when the retention interval is set longer than
a day, and its help text says so.

**D7 — Identity stripping at parse time** through tolerant deserializers
(`serde_json::Value` / `IgnoredAny`, never `deny_unknown_fields`); the full
discard list is in the Frigate guide and exported from the code as data so
tests prove every fixture populates each entry. `face` and `license_plate`
labels are refused outright. Alerts (reviews) mode seals only severity
`alert`, at `new` when already an alert, else at the `update` that becomes
one, confidence 1.0, dedupe per camera + bucket; events mode (default) seals
every tracked object at `new`, dedupe per camera + base label + bucket. The
default self-verify interval became 3600 s. *Review-driven:* alerts mode is
**exclusive** — it subscribes to `<prefix>/reviews` instead of
`<prefix>/events` (the earlier bridge added reviews beside events); the flat
"legacy" review shape Frigate never published is no longer accepted; an
event with an unknown `type` is rejected rather than sealed (an absent type
is still accepted); and the `label:sub_label` fold is gone — the sealed label
is the base class.

**D8 — Camera slug rule** (ASCII-lowercase, every other code point `_`,
truncated to 64, no trimming or collapsing), applied by the wizard when it
writes the allowlist and by the bridge and adapter when they compare, so
`Front Door` matches `front_door`. *As specified.*

**D9 — The claim.** The integration publishes a retained
`witness/claim/<kernel id>` with `{"v":1,"claimed":true,"nonce":…,
"claimed_bucket_start":…}` (never an entry id) on every load and hourly, and
withdraws it only in `async_remove_entry`; the bridge withdraws its
discovery configs while a claim fresher than 24 h is present and republishes
them on clear, saying which on retained `witness/claim_state`. The migration
Repairs issue `entities_migrated` lists `pwk_*` → `securacv_*`.
*Review-driven:* the notice is raised only on an instance whose entity
registry actually holds the bridge's MQTT-discovered entities (unique id
`<kernel id>_*` on the `mqtt` platform, or an entity id `*.pwk_*`), read
before the claim goes out, so a fresh install is not told about a replacement
that never happened; the flag is set either way, and an unreadable registry
keeps the contract's default of raising it.

**D10 — Verification.** The integration fetches the sealed-log document
incrementally (`GET /api/sealed-log?from_seq=`) and verifies the hash chain
and the Ed25519 signatures in Python against the pinned key — a port of
`viewer/verify_core.js`, held byte-equal to it and to the kernel on the
shared fixture — on Verify now, on `securacv.verify`, on a backstop timer and
60 s after new entries go idle. An event row is **Verified ✓** iff its
(event type, zone, time bucket) matches a verified entry; else **Sealed ✓**;
a failed walk → **Failed ⚠** and the `chain_valid` sensor goes off; a key
change → Repairs `tofu_mismatch` (the fix flow has the person confirm the
fingerprint, then rotates the pin, keeping the previous key), never
"Failed". *Review-driven:* the backstop is **15 minutes**, not the hourly
cadence the first journey text used (the hourly figure survives as the
kernel's own `verify_interval_hours`); the walk also runs, as a walk and
never a verdict, when the bridge's `chain_problem` clears; a failed walk
renders every kernel row Failed, including rows before the failing entry —
the per-row "verified up to entry #N" nuance is left to the offline viewer;
and the document's `rotation_records` are not consumed: a served key other
than the pin always goes through the Repairs ceremony, never an
auto-authorized rotation lineage.

**D11 — Export.** `securacv.export_evidence(window_hours, entry_id?)` →
`GET /export/bundle` → one zip `securacv-evidence-<YYYY-MM-DD>-<8 hex>.zip`
(the bundle verbatim, `evidence_viewer.html`, `README.txt`,
`WITNESS_KEY.txt`) under `hass.config.path("securacv", "evidence")`
(`0700` / `0600`, atomic temp-and-rename), `index.json` newest first, keep-N
through the `evidence_keep` option (default 20, 1–200; the notification
names what was removed), served by `HomeAssistantView
/api/securacv/evidence/{export_id}` (`requires_auth`, `[0-9a-f]{8}` ids
only) with an `async_sign_path` 24-hour link in the persistent notification,
and listed by `media_source.py`. Response `{export_id, file, url, sha256,
events, window_hours, kernel_device_id}`. *Review-driven:* the media items
are `MediaClass.URL`, because the core's `MediaClass` has no `document`
member — the contract named a value that does not exist; `index.json` rows
carry `entry_id` so the media source can group packs per entry; the zip's
date is Home Assistant's local date and `created_bucket_start` is a bucket
start; and the offline viewer accepts the **zip itself** (central directory,
`DecompressionStream('deflate-raw')`, CRC check) so the zip is the hand-off
unit — the earlier `#bundle=&key=` autoload was dropped.

**D12 — Python lanes.** The stub lane runs on 3.11–3.13 without Home
Assistant; the real-Home-Assistant lane runs on Python 3.14 with
`pytest-homeassistant-custom-component` pinned to the floor (2026.5.x) and
to current (2026.10.x) in a two-cell matrix; `pyproject` says
`>=3.11,<3.15`. The real lane is the W7 work item, wired after the core
merge.

**D13 — Naming.** The kernel device is **SecuraCV**, model *Privacy Witness
Kernel*; entities are named through translation keys only (no explicit
entity ids anywhere); the two Frigate classes read *Person or vehicle* and
*Animal or small object* — the dictionary ids are unchanged. *As specified;*
the dictionary file itself still carries the older "Large/Small object
crossed boundary" labels, which the linter does not compare (ids only).

**D14 — Frigate link health.** `frigate_bridge` publishes retained
`witness/frigate_link` (`v`, `device_id`, `connected`, `topic_prefix`,
`last_message_bucket_start`, `last_message_bucket_size`,
`messages_since_start`, `mode`) on change of `connected` or of the
last-message bucket, after every reconnect and at least every 300 s — never
per message — and `connected:false` on a clean shutdown; the kernel device
gets a `frigate_link` connectivity sensor that is on iff connected and heard
within 60 minutes. *As specified.*

**D15 — Option G, the app carries the integration.** The image holds
`custom_components/securacv` pruned (no tests) at the app's own version
(enforced at build time and in CI). It is copied only on the wizard's
explicit click (first install), or on app start for a copy that carries the
app's marker and a matching sha256 manifest (an update, followed by the
persistent notification *Restart Home Assistant to load SecuraCV x.y*);
temp dir + atomic rename, `cp -RL`, a symlinked destination refused, never
an unmarked or HACS copy, never a downgrade, never an automatic restart.
*Review-driven:* Docker's `COPY` has no exclude, so an intermediate carry
stage copies and then prunes, and the installer prunes the same patterns
again on copy.

**D16 — TLS for the sidecar is deferred (W9).** An `api-tls` build with a
self-signed certificate, its fingerprint printed by `pair` and pinned by
Home Assistant, is the follow-up item after the core merge. Until it lands,
the paired bearer crosses the LAN as plaintext HTTP, which
[`SECURITY_MODEL.md`](../security/SECURITY_MODEL.md#home-assistant-pairing-and-what-crosses-your-network)
states plainly, the compose quickstarts state in their port comments, and
the setup guide lists under known limitations. When it lands, the `pair`
card gains a `tls_cert_fp` line, the announcement's `api.scheme` becomes
`https`, and the sidecar's end-to-end check's announce-key assertion grows
by that field.

## Other review-driven changes worth knowing

- **The dashboard is a strategy, not a generated file.** The app's old
  Lovelace YAML generator named the `pwk_*` ids the claim withdraws, so it
  was retired; the strategy (`custom:securacv`) builds the page from whatever
  entities exist, and the one-time notification tells the person where the
  picker entry is — posted only when the strategy module was actually served,
  so an install without it is never told about a picker entry it does not
  have.
- **Blueprints notify a phone.** Every notifying blueprint takes a *Phone*
  device selector and derives `notify.mobile_app_…` itself, with the text
  service as an override; the digest always sends (a quiet night is a
  sentence, not silence); the new witness notification waits up to 90 s for
  the seal and renders only camera, base label, zones and seal state. The
  "Yesterday" section became **Last 24 hours**, because the digest's window
  is rolling, not a calendar day.
- **`PARALLEL_UPDATES` is 0 on the push-fed platforms and 1 on `button`**,
  whose presses are actions against one kernel.
- **Tamper events fire from the Canary's `tamper` publishes only**, not from
  the health flags the contract also listed — a health publish repeats the
  flag every cycle and would re-fire one tamper indefinitely; the per-type
  tamper sensors still read the flag. The per-transport connectivity sensors
  became diagnostic and disabled by default; the per-type tamper sensors
  stay enabled because the alerts blueprint names them.
- **Entity icons come from `icons.json` alone** — a static icon in code wins
  over the translation at runtime, so the state icons the quality scale
  claimed were dead until every class-level `_attr_icon` was removed.
- **The options flow uses the core's `config_entry` property** (the
  `__init__(config_entry)` pattern was replaced in 2024.11).
- **Diagnostics project rather than copy:** the kernel URL as scheme and
  port, the kernel id as a four-hex prefix, the kernel key's fingerprint
  as its eight characters, each Canary's key fingerprint as a per-dump
  correlation id with none of its digits, `/status` storage as its
  endurance numbers (never the block-device path), and then
  `async_redact_data` over tokens, hosts, ids, keys, hashes, MACs, SSIDs,
  names, coordinates and a Canary's hardware, chip, module and Bluetooth
  identifiers.
- **The announcement names no host unless the operator sets one**
  (`SECURACV_ANNOUNCE_HOST`), because a host is a network identifier; Home
  Assistant then prefills its broker's host instead.
- **Every refused request's body is drained** (up to 64 KiB) before the
  kernel writes its `400` or `401`, so clients see the refusal rather than a
  connection reset; the lockout `429` is still written before the request is
  read.

## Ownership and the gates that hold it

| Area | Where | Held by |
|---|---|---|
| Frigate ingestion, the bridge's housekeeping, `frigate_link` | `src/transport/frigate.rs`, `src/bin/frigate_bridge.rs`, `src/adapter/frigate.rs` | `tests/frigate_integration.rs` (sentinel negative-containment on every fixture), `tests/frigate_mqtt_e2e.rs` (the real binary against an in-test broker) |
| Pairing, identity, signed announce, cursors, receipts, the publisher | `src/api/**`, `src/bin/witness_api.rs`, `src/bin/event_mqtt_bridge.rs`, `src/lib.rs` | `tests/api_*.rs`, `tests/witness_api_*.rs`, the receipt-count proof (30 state reads → 0 rows) |
| The integration | `custom_components/securacv/**` | the stub lane (`pytest … -p no:homeassistant`), `test_quality_scale.py`, `test_entity_translations.py`, ruff, mypy against two cores |
| The app | `privacy_witness_kernel/**` | `pwk-wizard-tests.yml`, `addon-image.yml` (carried version == app version) |
| The frontend | `custom_components/securacv/www/**`, `docs/blueprints/**`, `viewer/**` | the node suite, `viewer/build.mjs --check`, the parity fixture |
| The sidecar | `docker/sidecar/**` | `ci_e2e.sh` (fixture replay, pairing posture, announce contract, receipts, doctor, clean shutdown) |
| These docs | `docs/**`, `README.md`, `CHANGELOG.md` | `lint_docs_index.py`, `lint_md_links.py`, `lint_spelling.py`, `lint_fleet_word.py`, `gen_homeassistant.py` (the entity catalog is parsed from the setup guide), `gen_start.py` (the Docker commands are parsed from the Frigate guide) |

## Out of scope for this release

A C2PA manifest on the evidence pack, LLM tools, backup hooks, firmware
changes, a break-glass UI, HACS default-store submission (the maintainer's
mirror and release), Advanced Camera Card elements, and the
authenticated-file claim path (the claim is MQTT-only here).
