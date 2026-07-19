# Home Assistant Platform Architecture — Audit, Principles, Wow Factors

**Scope:** every surface where securaCV touches Home Assistant — the HACS integration
(`custom_components/securacv/`), the add-on (`privacy_witness_kernel/`), the MQTT
pipeline (`event_mqtt_bridge`, `frigate_bridge`, canary firmware), dashboards,
blueprints, automations, cards, and the install paths — audited against the current
(2026.7) Home Assistant developer standards and against the design patterns of the
ecosystem's best-loved apps (Frigate, ESPHome, UniFi Protect, Music Assistant,
Zigbee2MQTT, Z-Wave JS).

**Questions this doc answers:**

> How do we use Home Assistant today, and is it laid out the way the best HA products
> are laid out? What are the best practices we should adopt as *architecture
> principles* so the logical layout is right from the beginning? And which wow factors
> would put the HA experience beyond what the market offers?

Short answer: the **cryptography and the pipeline are ahead of the market; the HA
projection of them is behind our own engine**. We ship three parallel entity universes
that don't know about each other, we poll where we could push, the wizard/options/config-flow
overlap three ways, and a handful of Quality Scale basics (subscription cleanup,
availability, reauth, entity translations, repairs) are missing — while genuinely hard
things (signed exports, offline verification, TOFU device PKI, broker credential
hygiene) are already done well. The fix is not more features; it is **one architecture
decision** (add-on = engine, integration = the single product face, paired by
Supervisor discovery, one device tree) plus a Quality-Scale-driven cleanup — and then
a short list of wow features (evidence packs, repairs-as-integrity-console, witness
event entities, a triage dashboard, privacy-floor LLM tools) that no competitor can
copy without our invariants.

---

## 1. How securaCV uses Home Assistant today

### 1.1 The core finding: three parallel entity universes

SecuraCV currently reaches HA through **three independent paths that produce three
disjoint entity sets**, each with its own naming, discovery, and trust story:

| # | Path | Who publishes | Discovery | Entities | Verified? |
|---|------|---------------|-----------|----------|-----------|
| 1 | **Kernel / add-on path** | `event_mqtt_bridge` (Rust) → `witness/*` topics | MQTT discovery (`homeassistant/…/pwk_*/config`) | `sensor.pwk_daily_digest`, `sensor.pwk_last_event`, `sensor.pwk_<zone>_events`, `binary_sensor.pwk_chain_problem`, `button.pwk_verify_now` | Chain-verified server-side; entities owned by HA's core `mqtt` integration, not ours |
| 2 | **Canary firmware direct** | ESP32 firmware (`ha_discovery.cpp`) → `securacv/<id>/*` topics | MQTT discovery (`homeassistant/…/config`, retained) | presence, occupancy, confidence, `update.*` firmware, `number.*` tuning, identify buttons… | Unverified (owned by core `mqtt` integration; no signature checking) |
| 3 | **HACS custom integration** | `custom_components/securacv` subscribing to raw `securacv/#` + polling the kernel HTTP API | Manifest MQTT discovery (`"mqtt": ["securacv/#"]`) → config flow | `securacv_canary_<id>_*` (witness count, chain valid, tamper…, Ed25519-verified) + kernel coordinator entities (storage health, last event, online) | **Yes** — per-device Ed25519 TOFU PKI (`signature.py`, `device_trust.py`) |

Consequences: a single Canary appears in HA as **two devices** (one from firmware
discovery, one from the integration); the add-on's `pwk_*` entities and the
integration's kernel-mode entities **duplicate the same kernel** (one over MQTT push,
one over 30-second HTTP polling); dashboards must know which universe they target
(`securacv-dashboard.yaml` targets `pwk_*`, `securacv-canary-dashboard.yaml` targets
`securacv_canary_*`, `securacv-vision-dashboard.yaml` targets firmware-discovery
entities); and the verified/unverified distinction — our core product value — depends
on *which copy of the entity the user happened to put on their dashboard*.

None of this is accidental sloppiness — each path exists for a real reason (works
without HACS; works without the add-on; control entities need MQTT discovery's
`number`/`update`/`switch` platforms). But the best HA products resolve exactly this
tension with a deliberate layering (see §5/§6), and we currently don't.

### 1.2 Surface inventory

| Surface | Where | State |
|---|---|---|
| HACS integration v0.6.0 | `custom_components/securacv/` | `local_push` hub, config flow (3 modes: mqtt / kernel / both), 2 platforms (sensor, binary_sensor), diagnostics, TOFU device PKI, 2 Lovelace cards auto-registered | 
| Add-on ("App") v0.6.0 | `privacy_witness_kernel/` | Prebuilt GHCR image (amd64/aarch64), ingress wizard + status panel on 8788, `services: mqtt:want`, frigate/standalone modes |
| Kernel MQTT bridge | `src/bin/event_mqtt_bridge.rs` | QoS 1, retained state + LWT `witness/status`, MQTT discovery for `pwk_*`, `witness/cmd/verify` button, 24h scheduled verify |
| Frigate ingestion | `src/bin/frigate_bridge.rs`, `src/transport/frigate.rs` | `frigate/events` + `frigate/reviews` (0.14+), label→semantic-event mapping, zone sanitization |
| Kernel HTTP API | `src/api/mod.rs` | Loopback-only `127.0.0.1:8799`, rotating capability token (10-min), `/events`, `/digest`, `/status`, `/verify`, `/export/bundle` |
| Adapter framework | `src/adapter/` | Track B: Frigate/mqtt_sensor/webhook/BLE/meshtastic → 8-kind claim vocabulary → sealed log; per-adapter seccomp sandbox |
| Dashboards | `homeassistant/lovelace/` (3 YAML files) | Manual paste; wizard can generate zone dashboard; `install.sh` copies one |
| Blueprints | `docs/blueprints/` (5 YAML + 1 doc) | Digest, alerts (smoke/CO/tamper/chain/offline), 3 canary-sense wellness — import URLs present, no My-HA badges |
| Automations | `homeassistant/automations/` (5 files) | `DEVICE_ID` placeholder templates; one deprecated in favor of blueprint |
| Custom cards | `custom_components/securacv/www/` | Timeline card (verification badges, modality chips) + aim card (boxes-only); zero-dep, CI-tested via `node --test` |
| Offline verifier | `viewer/` | Standalone HTML evidence viewer; `verify_core.js` byte-equivalent to the Rust verifier |
| Install paths | `scripts/install.sh`, `integrations/ha_frigate_mqtt/`, add-on wizard | Three coexisting philosophies: `curl\|bash`, Docker Compose, ingress wizard |

### 1.3 What is already excellent (protect these)

These are things top-tier integrations do — several of which even famous ones skip:

- **Real cryptographic verification in the integration** — Ed25519 signature checking
  against pinned keys with a TOFU + manual-pin + rotation trust store, an audit trail,
  and verdicts that annotate rather than hide state (`signature.py`, `device_trust.py`).
  No consumer camera integration in the ecosystem does this.
- **Broker credential hygiene** — the add-on resolves MQTT credentials fresh from
  Supervisor services each boot (`bashio::services`), the wizard never persists or
  returns broker passwords, Frigate configs use env placeholders. Better than most
  published add-ons.
- **Release-gate honesty** — `verify_published_image.sh` proves the GHCR image is
  anonymously pullable per-arch before release; catches the classic "installs on
  amd64, 403s on a Pi" trap.
- **Correct MQTT fundamentals in the bridge** — retained LWT availability
  (`witness/status`), QoS 1, retained state for restart survival, lazy per-zone
  discovery, verify-on-command topic.
- **Signed export + offline verification** — `GET /export/bundle` → self-verifying
  artifact, plus a dependency-free browser verifier kept byte-equivalent to the Rust
  verifier by fixture tests. This is the germ of the biggest wow factor we have (§7.1).
- **Honest UX language** — the timeline card distinguishes "Verified ✓" / "Signed
  (unverified)" / "Logged" / "Verification failed"; blueprints frame welfare checks as
  prompts, not medical alarms; docs mark features Implemented/Experimental/Planned.
- **Hardened wizard** — SSRF allowlists, content-length caps, YAML-injection
  sanitization, capability tokens never logged; wizard tests + shellcheck in CI.
- **Config-entry migration handler, unique-ID discipline, device registry usage,
  `has_entity_name` everywhere, hassfest + HACS actions in CI** — the skeleton is right.

---

## 2. Audit: the integration vs the Integration Quality Scale

HA's Integration Quality Scale (ADR-0022) is the codified answer to "what does a
great integration look like". Custom/HACS integrations can't be formally graded, but
the docs explicitly invite aligning with it — and it is the best available engineering
bar. Verdict at a glance: **we do not currently clear Bronze** (3 hard gaps), Silver
has 4 gaps, Gold is where the UX wins live. Every finding below was verified in code.

### 2.1 Bronze (baseline)

| Rule | Status | Evidence |
|---|---|---|
| config-flow | ✅ | 3-mode flow, `data_description` in strings |
| entity-unique-id | ✅ | consistent `{domain}_canary_{id}_{key}` scheme |
| has-entity-name | ✅ | all entities |
| unique-config-entry | ✅ | `async_set_unique_id` + abort in all modes |
| test-before-configure | ✅/⚠️ | kernel mode validates by real call; MQTT mode accepts prefix without broker check |
| test-before-setup | ✅ | `async_config_entry_first_refresh()` (`__init__.py:355`) |
| dependency-transparency | ⚠️ | `requirements: []` but imports `cryptography` relying on HA's bundled copy — works, but the dependency is invisible and unpinned |
| brands | ⚠️ | assets staged in `brands/submission/`, never submitted. Since HA 2026.3 an in-repo `custom_components/securacv/brand/` folder also works (icons served via `/api/brands/...` proxy) — we can self-serve this today |
| common-modules | ⚠️ | coordinators live in `__init__.py` (679 lines), no `coordinator.py` / `entity.py` split |
| appropriate-polling | ⚠️ | 30 s HTTP poll of a loopback API whose data changes on 10-minute buckets; and a push path (MQTT) exists for the same data (§4.3) |
| docs-* (5 rules) | ⚠️ | `docs/homeassistant_setup.md` is strong but not structured per-rule (install/removal/actions) |
| **action-setup** | ❌ | **The integration registers no services at all.** Verify-now / export / pin-device exist only as MQTT button, wizard endpoints, and options-flow forms — none are automatable as `securacv.*` actions |
| **entity-event-setup** | ❌ | Subscriptions are made in `async_added_to_hass` (correct) but the unsubscribe callbacks are **discarded** — see §2.5 bug #1 |
| **runtime-data** | ❌ | legacy `hass.data[DOMAIN][entry_id]` dict (`__init__.py:395`) instead of typed `entry.runtime_data` |
| config-flow-test-coverage | ❌ | zero config-flow tests (see §2.4 on the test harness) |

### 2.2 Silver (robustness)

| Rule | Status | Evidence |
|---|---|---|
| integration-owner | ✅ | `codeowners: ["@kmay89"]` |
| config-entry-unloading | ⚠️ | implemented, but leaks MQTT subscriptions (§2.5 #1) |
| log-when-unavailable | ⚠️ | coordinator handles the kernel side; MQTT side silent |
| **entity-unavailable** | ❌ | MQTT entities have **no availability logic** — a dead Canary shows stale state / `unknown` forever, never `unavailable`, despite firmware publishing an LWT `securacv/<id>/status` availability topic we don't consume for this |
| **reauthentication-flow** | ❌ | no `async_step_reauth`. Softened by the rotating-token-file re-read on 401 (`__init__.py:159-222`) — but a permanently wrong URL/token has no recovery path except delete-and-re-add |
| **parallel-updates** | ❌ | `PARALLEL_UPDATES` not set in either platform |
| action-exceptions | ❌ (latent) | no actions exist yet; when added they must raise translatable `HomeAssistantError`/`ServiceValidationError` |
| test-coverage ≥95 % | ❌ | crypto/trust/API-token logic is well covered; setup, unload, entities, diagnostics, config flow are not |
| docs-configuration/installation-parameters | ⚠️ | options flow (PKI menu) documented in `device_trust.md`, not in a parameters reference |

### 2.3 Gold (best UX)

| Rule | Status | Evidence |
|---|---|---|
| devices | ✅/⚠️ | good `DeviceInfo` incl. sanitized `configuration_url`; but no `via_device` topology — kernel, adapter host, and canaries are unrelated islands (§6.2) |
| discovery | ✅/⚠️ | MQTT manifest discovery works (`async_step_mqtt`); **no `hassio` (Supervisor) discovery from our own add-on** — the single highest-leverage missing piece (§5 P2), no zeroconf |
| dynamic-devices | ✅ | new canaries appear via MQTT discovery at runtime |
| entity-category | ✅ | `EntityCategory.DIAGNOSTIC` used consistently |
| entity-device-class | ✅ | tamper/problem/connectivity/temperature used well |
| diagnostics | ⚠️ | exists but **no `async_redact_data`** — dumps full kernel URL, full latest event, full device status incl. LAN IPs (`diagnostics.py`) |
| entity-disabled-by-default | ❌ | noisy diagnostics (GPS, SD wear, transport per-type, radar link) all enabled by default |
| **entity-translations** | ❌ | **no entity sets `translation_key`; every `_attr_name` is hardcoded English — and the `entity:` block shipped in `strings.json`/`translations/en.json` is dead data that never binds** |
| icon-translations | ❌ | `_attr_icon` + dynamic `icon` properties instead of `icons.json` |
| exception-translations | ❌ | not used |
| **reconfiguration-flow** | ❌ | no `async_step_reconfigure` (move the kernel to a new host ⇒ delete and re-add) |
| **repair-issues** | ❌ | trust mismatches surface as `persistent_notification` (`__init__.py:707`) — invisible in the Repairs center, not actionable, not translatable. Chain breaks / silent devices / clock drift raise no repair issues at all (§7.2) |
| stale-devices | ❌ | removed canaries live in the registry forever; no `async_remove_config_entry_device` |
| discovery-update-info | ❌ | discovery never updates a moved kernel's host |
| docs-* (7 rules) | ⚠️ | partial across existing docs |

### 2.4 Platinum + testing posture

- **inject-websession** ✅ (`async_get_clientsession` throughout) · **async-dependency** ✅
  (pure aiohttp; no sync client lib) · **strict-typing** ⚠️ (mypy runs in CI but with
  `ignore_missing_imports` and `ignore_errors` overrides on `config_flow` and tests).
- The test suite deliberately uses a **hand-rolled HA stub harness**
  (`tests/conftest.py` injects fake `homeassistant.*` modules) instead of
  `pytest-homeassistant-custom-component`. That keeps crypto tests hermetic — a
  legitimate choice — but it structurally **prevents** config-flow tests,
  `MockConfigEntry` lifecycle tests, entity-state tests, and diagnostics tests, which
  is exactly where our coverage gaps are. Adopt the real harness *alongside* the stub
  harness: stubs for pure logic, `pytest-homeassistant-custom-component` for flows and
  lifecycle.

### 2.5 Correctness bugs found (fix before any feature work)

1. **MQTT subscription leak on unload/reload (high).** Every per-entity subscription
   (`sensor.py:633,670,705,781,864,905,973`; `binary_sensor.py:363,389,431,436,497,502,590,633,675,717`)
   and both platform discovery subscriptions (`sensor.py:269`, `binary_sensor.py:221`)
   discard the unsubscribe callable returned by `mqtt.async_subscribe`. Unload only
   tears down the two subscriptions made in `__init__.py:415,428`. Reloading the entry
   **double-subscribes every handler**; repeated reloads multiply callbacks
   (duplicate state writes, duplicate TOFU processing). Fix: entities use
   `self.async_on_remove(await mqtt.async_subscribe(...))`; discovery unsubs go into
   `entry_data["unsub_mqtt"]` (or `entry.async_on_unload`).
2. **Diagnostics leak surface (medium).** No redaction (§2.3); add `async_redact_data`
   with a `TO_REDACT` covering URL, IPs, and event payload fields. Ironic gap for a
   privacy product — diagnostics files get attached to public GitHub issues.
3. **Blocking stat on the event loop (low).** `card_path.is_file()` in
   `_async_register_frontend` (`__init__.py:87`) runs in async context; wrap in
   executor.
4. **Bare-int aiohttp timeouts (low).** `session.get(..., timeout=10)` →
   `aiohttp.ClientTimeout` (`__init__.py:200,246,301`).
5. **On-loop crypto (watch).** Ed25519 verify runs inside MQTT `@callback`s. Fine at
   sub-ms today; move to executor if payload sizes or device counts grow.

---

## 3. Audit: the add-on vs App best practices

Context: HA renamed add-ons to **"Apps"** in 2026.2 (docs moved to
`developers.home-assistant.io/docs/apps`); the security scale is 1–6 (base 5);
`build.yaml`/`BUILD_FROM` are retired (Supervisor ≥ 2026.04 no longer injects
`BUILD_FROM`; base image goes in the Dockerfile `FROM` + `LABEL`s, built with the
`home-assistant/builder` actions).

| Area | Status | Finding |
|---|---|---|
| Prebuilt image + publish gate | ✅ | GHCR multi-arch, pinned bases, `verify-public` release gate — model citizen |
| Ingress | ✅ | `ingress: true`, port 8788, panel icon/title — correct surface (+2 security) |
| MQTT via services | ✅ | `services: ["mqtt:want"]` + bashio resolution; external brokers still work |
| First-run gate | ✅ | unconfigured → wizard-only mode; no crash-loop |
| **Watchdog** | ❌ | no `watchdog:` despite `/api/status` and kernel `/health` existing. One line (`watchdog: "http://[HOST]:[PORT:8788]/api/status"`) buys Supervisor auto-restart |
| **Process supervision** | ❌ | base is s6-overlay but everything runs from `run.sh` with bare `&` background jobs — if `event_mqtt_bridge` or the wizard panel dies, nothing notices or restarts it. Split into `/etc/services.d/` (or s6-rc) services: kernel, bridge, panel |
| **AppArmor** | ❌ | `apparmor: true` = default profile only; no `apparmor.txt`. A custom profile is +1 security rating and is exactly what a *tamper-evidence* product should ship. (We already write seccomp sandboxes for detector backends — the add-on shell deserves the same care) |
| **Dead host port** | ❌ | `ports: 8799/tcp: 8799` exposes the Event API port, but the kernel binds `127.0.0.1:8799` *inside* the container — the mapping is non-functional, costs security rating, and confuses network docs. Drop it (ingress + `/export/bundle` cover every consumer) |
| Root user | ⚠️ | add-on container runs as root while our own root `Dockerfile` and sidecar run UID 1001. Supervisor apps commonly run root, but with no custom AppArmor either, the process is fully unconfined |
| `hassio_role` | ⚠️ | wizard reads other add-ons' info (`/addons/core_mosquitto/info`, Frigate) with default role; degrade is graceful but preflight can silently under-report. Either request `hassio_role: manager` deliberately or drop foreign reads |
| `map: ssl:ro` | ⚠️ | declared, never referenced — remove until used |
| Store presentation | ❌ | no `DOCS.md` (empty Documentation tab), no add-on `icon.png`/`logo.png`, no in-folder `CHANGELOG.md`, no `translations/*.yaml` for option names |
| `homeassistant:` min version | ❌ | not pinned; hacs.json pins the integration only |
| Builder migration | ⚠️ | `build.yaml`/`BUILD_FROM` pattern is now legacy; our CI builds still work but should migrate to Dockerfile `FROM` + labels before Supervisor tightens further |
| Arch coverage | ⚠️ | amd64/aarch64 only — HA now only supports these two, so this is *fine going forward*; keep `install.sh`'s explicit armv7 rejection message |
| Wizard vs options duplication | ⚠️ | the wizard re-implements fields that already exist in the config.yaml schema (mode, cameras, retention, mqtt_publish) and silently overwrites the Options tab on re-run. Decide: wizard owns *ceremonies* (key generation, camera test, frigate.yml, pairing), Options tab owns *settings* — and the wizard should read/write through the same options it doesn't own (§5 P6) |

---

## 4. Audit: architecture-level findings

### 4.1 The three-universe fragmentation (from §1.1)

This is the root issue behind most UX friction: dashboards need per-universe entity
IDs; docs must explain `pwk_*` vs `securacv_canary_*` vs firmware entities; the
verified badge only exists in universe 3; renames/areas don't propagate; and a Canary
is two devices. §6 proposes the target layering and migration.

### 4.2 The add-on and the integration don't know about each other

Music Assistant / Mosquitto / Z-Wave JS pattern: the add-on POSTs a **Supervisor
discovery** payload (`POST http://supervisor/discovery`, `{"service": "securacv", "config": {...}}`)
→ HA fires the integration's `async_step_hassio` → user clicks **Configure** on a
"SecuraCV discovered" card → done. Today, an add-on user who wants verified entities
must *also* find HACS, add the custom repo, install the integration, and manually
enter the kernel URL + token file path that the add-on already knows. This handshake
is ~50 lines total on both sides and collapses our two install stories into one.
It can also carry the capability-token *path* (both sides already share `/config`),
the MQTT prefix, and the device ID — nothing sensitive travels beyond Supervisor.

### 4.3 Push exists, but the integration polls

The kernel data the integration polls every 30 s (events, digest, storage health,
chain status) is *already published* over MQTT by `event_mqtt_bridge` (retained,
QoS 1, LWT). Two clean options: (a) integration consumes `witness/*` topics and the
coordinator becomes push-fed (`async_set_updated_data`), HTTP retained only for
`/export/bundle` and on-demand `/verify`; (b) keep HTTP but add a long-poll/WS event
stream to the kernel API. Option (a) requires no kernel changes and honors our
declared `iot_class: local_push`. Polling then survives only as a slow health
backstop (e.g. 5-min storage-health poll), which also resolves `appropriate-polling`.

### 4.4 Placeholder-surgery UX

Automations and two of three dashboards require hand-editing `DEVICE_ID` /
`YOUR_PHONE` placeholders. Blueprints (which take inputs properly) already exist for
half of these — finish the migration: every YAML we ship should either be a blueprint
with a My-HA import badge or be generated (wizard, or a dashboard strategy — §7.4).

### 4.5 Contract coupling that will bite

- `Attestation` serde strings (`"adapter"`/`"ha-bridged"`) are a hard wire contract
  with `const.py:436-437` and with the timeline card's hand-mirrored JS constants —
  three copies of one vocabulary. Extract a generated single source (even a checked-in
  JSON the three consumers load/codegen from).
- Platform deprecation deadlines that touch us: MQTT publish must pass explicit
  `qos`/`retain` (breaks 2027.6); `entity_id` domain must match platform domain
  (2027.5); config-entry update-listener + reload-helper combo errors from 2026.12.
  None are violated today; pin them in CI notes so upgrades don't surprise us.

---

## 5. Design principles (the architecture we should hold ourselves to)

Each principle is grounded in a pattern proven by a best-in-class app. These are
proposed as **standing rules** for all HA-facing work — the "logical layout from the
beginning" the product deserves.

**P1 — The add-on is the engine; the integration is the product.**
(Zigbee2MQTT, Z-Wave JS, Music Assistant.) The add-on's job: run the kernel,
supervise processes, expose the expert ingress panel. The integration's job:
*everything the user touches in HA* — entities, devices, services, events, repairs,
diagnostics, media. Nothing user-facing should exist only in the wizard if it can be
a native HA surface; the wizard keeps only the ceremonies HA cannot host (device-key
generation/backup, camera RTSP testing, frigate.yml generation, mesh pairing).

**P2 — Pairing is a handshake, not a manual.** (Mosquitto → MQTT integration.)
The add-on announces itself via Supervisor discovery; the integration config-flow
auto-fills from the payload; MQTT credentials keep flowing through `bashio::services`.
Target first-run: *install add-on → wizard's three ceremonies → "SecuraCV discovered"
card → Configure → entities exist.* Zero copied URLs, zero token paths typed.

**P3 — One brand, one device tree.** (UniFi Protect.) A single "SecuraCV" hub/service
device is the root; canaries, the adapter host, and (as sub-devices) witness zones
hang off it with `via_device`. A physical thing appears in HA **exactly once**. Where
two data paths exist for the same thing (firmware discovery vs integration), one is
authoritative and the other defers (§6.1).

**P4 — Push first; poll only as a health backstop.** (ESPHome's native push API is
the reason it feels instant.) MQTT/event push feeds coordinators
(`async_set_updated_data`); polling intervals only for slow diagnostics. Never poll
data that changes on 10-minute buckets every 30 seconds.

**P5 — Every surfaced fact carries its trust state.** The product *is* the
verification. Verified/signed-unverified/logged/failed must be machine-readable
(attributes + event entities + repair issues), not only card chrome. A dashboard
built from stock cards should still be able to show trust state.

**P6 — Secrets are generated and handed over, never typed.** (ESPHome PSK, Z-Wave
SmartStart QR.) Device keys: generated + backup ceremony (already done). Broker creds:
service discovery (already done). Kernel token: discovery payload/shared path (to do).
Canary claiming: QR possession proof (roadmapped — keep it). Any flow that asks a
user to paste a token is a bug to be engineered away.

**P7 — Fail loudly, natively, once.** Silent devices go `unavailable` (consume the
LWT we already publish). Chain breaks, tamper, clock drift, silent canaries, storage
wear become **repair issues** with severity + `learn_more_url`. Log unavailability
once at INFO, once on recovery. `persistent_notification` is retired.

**P8 — The Quality Scale is our engineering bar, tracked in-repo.** Ship
`custom_components/securacv/quality_scale.yaml` marking every rule
done/exempt-with-comment, even though HACS can't grade us. Bronze+Silver complete =
merge bar for integration PRs; Gold = release bar for v1.x. (This doc's §2 tables are
the initial content of that file.)

**P9 — Ride the platform; build only what's uniquely ours.** Use update entities
(firmware OTA already publishes state — surface it via the integration too), event
entities, media source, blueprints + My-HA badges, sections dashboard strategies, the
LLM API, go2rtc live view via Frigate. Custom frontend only where witness semantics
are unique (timeline verification chips, boxes-only aim view). Explicitly: **don't
compete with Advanced Camera Card — integrate with it** (it supports arbitrary
menu elements/actions; a "verification badge + seal action" element reaches its
entire user base).

**P10 — The invariants bound every feature.** Any HA feature must pass
`spec/invariants.md`: no raw frames through our surfaces, no identity substrate,
coarse time only, local-only, no bulk query. Where HA offers a richer surface than
the invariants allow (e.g. media snapshots), we *pair* with Frigate for pixels and
attach our proof to them — we never become the pixel pipe. The constraint is the
moat, and every wow feature in §7 is designed inside it.

---

## 6. Target architecture (the logical layout)

### 6.1 Layered delivery — who owns which entities

```
┌────────────────────────────────────────────────────────────────────┐
│  Tier 2 · HACS integration  ("the product face")                   │
│  - consumes witness/* (kernel) + securacv/# (canaries) via MQTT    │
│  - verifies signatures, owns ALL verified entities                 │
│  - devices: SecuraCV Kernel (hub) ── via_device ── Canaries        │
│  - services, event entities, repairs, diagnostics, media source,   │
│    LLM tools, dashboard strategy                                   │
├────────────────────────────────────────────────────────────────────┤
│  Tier 1 · MQTT discovery  ("works with nothing installed")         │
│  - event_mqtt_bridge → pwk_* basic entities (unchanged)            │
│  - firmware → control/tuning entities (number/switch/update)       │
│  - migrates to device-based discovery (one payload per device)     │
│  - suppressed/adopted when Tier 2 claims the device                │
├────────────────────────────────────────────────────────────────────┤
│  Tier 0 · Engine (add-on / compose / bare)                         │
│  - kernel + bridges + API; ingress panel for ceremonies            │
│  - announces itself via Supervisor discovery                       │
└────────────────────────────────────────────────────────────────────┘
```

- **Tier 1 exists for integration-less users** (Container/Core installs, minimalists)
  and remains fully supported — it is also our ADR-0015 story (add-ons don't exist on
  Container/Core, so the compose path must stay first-class).
- **Tier 2, when installed, is authoritative.** Adoption mechanics (choose one during
  design): the integration publishes a retained claim topic the bridge/firmware honor
  by withdrawing their discovery configs (empty retained payload — the documented
  removal mechanism), or we use MQTT discovery's `migrate_discovery` handshake. Either
  way: one device, one set of entities, verified where the integration runs.
- **Control entities** (`number` tuning, auto-update switches) can stay
  discovery-published even under Tier 2 — they're commands, not claims; nothing to
  verify. They just need to join the same registry device (same identifiers), which
  MQTT discovery supports.

### 6.2 The device tree

```
SecuraCV Kernel  (service device; sw_version, storage diags, chain sensor,
 │                verify button/service, digest sensor, update entity for add-on)
 ├── via_device → Canary <name>   (tamper, witness count, chain, presence, RSSI†,
 │                                 GPS†, update entity, identify button)
 ├── via_device → Adapter Host    (stats diagnostics)
 └── zone entities live on the kernel device (not devices of their own)
        † disabled-by-default diagnostics
```

### 6.3 Naming and translation plan

All entities get `_attr_translation_key`; names move to `strings.json` `entity:`
(which already exists and is currently dead); icons move to `icons.json` with
state-based icons (chain ok/broken, tamper types). `en.json` stays generated from
`strings.json`. This unlocks community translations — a real lever for a
privacy product with strong EU resonance.

### 6.4 Config-flow surface (target)

- `async_step_hassio` (Supervisor discovery, new) → zero-field confirm.
- `async_step_mqtt` (existing) → canary-first setups.
- `async_step_user` (existing 3-mode) → manual/Container path.
- `async_step_reauth` + `async_step_reconfigure` (new) → token/URL lifecycle.
- Options flow keeps PKI management; candidates for **config subentries** (2025.3+):
  per-canary settings pages instead of one crowded menu.

---

## 7. Wow factors — beyond what the market offers

Ranked by differentiation-per-effort. Each is grounded in a proven pattern and stays
inside the invariants.

### 7.1 One-click Evidence Pack (market-first; mostly built already)

`button.securacv_export_evidence` + `securacv.export_evidence(window)` service →
kernel `/export/bundle` → signed, self-verifying bundle + the offline viewer,
delivered as a **media source item and a notification with the download link**.
Add a **C2PA-compatible content-credentials manifest** to the bundle: ONVIF and C2PA
are converging on signed capture (CISA endorsement, Leica/Canon/Pixel already
shipping); nobody self-hosted emits court-oriented provenance (FRE 901/902-aligned
verification report, access log, chain proof). This is the single feature reviewers
will not be able to compare to anything else. *(Precedent: Frigate's signed media
proxy URLs; UniFi's signed thumbnails — ours carries proof, not just pixels.)*

### 7.2 Repairs as the integrity console

"Chain broken at bucket 03:10–03:20", "Canary porch silent for 26 h", "Clock drift
exceeds evidentiary tolerance", "Storage wear critical", "Device key changed —
possible substitution (TOFU mismatch)" — each a **repair issue** with severity,
`learn_more_url` into our docs, and where safe a fix flow (re-verify, re-pin with
ceremony). The Repairs center becomes the product's trust dashboard without us
building any UI. Replaces today's `persistent_notification`. *(Precedent: HA's own
repairs philosophy — actionable, self-explaining; no camera product uses it for
integrity.)*

### 7.3 Witness moments as event entities

`event.securacv_<zone>_witness` (event_types = our 8 semantic claims),
`event.securacv_tamper`, `event.securacv_verification` (pass/fail). Event entities
appear in logbook, history, and the automation picker natively — automations become
"when a witness event happens", not "when this sensor's attribute changes".
*(Precedent: UniFi Protect doorbell `ring`; 2026.4 standardizes doorbell event
types — same idea, applied to evidence.)*

### 7.4 The triage inbox, auto-provisioned

A **dashboard strategy** (registerable since 2026.5) that generates a sections view
from live data — no YAML paste, no placeholder surgery: Alerts vs Detections split
(Frigate 0.14's review model — the best triage UX in self-hosted security), verification
chips on every row (timeline card), digest header, chain status badge, per-zone
sections. The wizard's "Generate dashboard" becomes obsolete; HACS users and add-on
users get the same dashboard for free. *(Precedent: HA area/energy strategies;
Frigate 0.14 review UI.)*

### 7.5 Notification stack with proof attached

One blueprint, My-HA import badge in the README: witness event → notification with
**Frigate thumbnail (pixels from Frigate, per P10) + "Sealed ✓" state + actions**
[View evidence] [Export pack] [Silence 1 h]. Critical-alert variants for tamper/chain
(already half-built in `securacv_alerts.yaml`). *(Precedent: SgtBatten's Frigate
notification blueprint — thousands of users; a blueprint became the product's UX.)*

### 7.6 Ask the witness (privacy-floor LLM tools)

Register an LLM API (`llm.py`, Frigate's `llm_functions.py` precedent):
`witness.summary(window)`, `witness.query(zone, window)`, `witness.verify_status()` —
answering from **semantic events and digests only**. Every competitor's camera-LLM
feature uploads frames to a vision model; ours can answer "what happened last night?"
with *provably no images involved* — the only LLM camera feature that is
privacy-preserving by construction, and it composes with local Ollama for fully
local voice. Curate exposed entities (token budget ≈ 30 entities).

### 7.7 ESPHome-grade canary adoption

Finish the roadmapped unified onboarding with the ecosystem's proven pieces: web
flash (ESP Web Tools), Improv Wi-Fi provisioning, zeroconf/mDNS → "Device
discovered" card, QR possession-proof claim (SmartStart pattern), identify buttons
(already spec'd), firmware `update` entities surfaced through the integration with
release notes. The round-display QR-join we already ship is *ahead* of the market —
wire it into the HA discovery flow so HA is where the claim completes.

### 7.8 Digest as a daily ritual

`sensor.pwk_daily_digest` → morning Assist/TTS brief and a digest notification
blueprint: "Quiet night. 3 events in driveway (all verified), chain intact,
14 h retention headroom." Cloud cameras charge subscriptions for AI summaries built
on uploaded video; ours falls out of a signed local digest.

### 7.9 Backup with proof of continuity

Implement the **backup platform hooks** (2025.1+): pre-backup seals a lifecycle
record, post-backup logs an export receipt — restores become *provable* rather than
silent. A witness that can attest "this backup contains an unbroken chain through
02:00" turns HA's backup feature into part of the evidence story.

### 7.10 Advanced Camera Card elements (reach its user base)

Ship card *elements/actions* for the de-facto standard camera card: a verification
badge overlay and a "Seal this moment / Export evidence" menu action. Integration
over competition — every Frigate power user sees our proof layer inside the UI they
already love.

---

## 8. Prioritized roadmap

**Phase 0 — Correctness (do first, small):**
subscription-leak fix (§2.5 #1) · diagnostics redaction · executor for the blocking
stat · `ClientTimeout` · `PARALLEL_UPDATES = 0` · `runtime_data` migration ·
MQTT-entity availability from the existing LWT topics · add
`quality_scale.yaml` (P8) · add-on: `watchdog:`, drop dead 8799 port mapping, drop
unused `ssl` map.

**Phase 1 — One architecture (the decision work):**
Supervisor discovery handshake add-on→integration (§4.2) · adopt/suppress mechanics
for the three universes (§6.1) · push-fed coordinators (§4.3) · reauth + reconfigure
flows · services (`verify`, `export_evidence`, `pin_device`) registered in
`async_setup` · entity translations + icons.json (kill the dead strings) ·
`via_device` tree (§6.2) · add-on store presentation (DOCS.md, icon, CHANGELOG,
option translations, `homeassistant:` min) · s6 service supervision + `apparmor.txt` ·
in-repo `brand/` folder (and still submit to home-assistant/brands) ·
`pytest-homeassistant-custom-component` for flow/lifecycle tests.

**Phase 2 — Gold + native UX:**
repair issues (§7.2) · event entities (§7.3) · stale-device removal + delete support ·
disabled-by-default diagnostics · media source for export bundles · notification
blueprint v2 with My-HA badges on every blueprint · dashboard strategy (§7.4) ·
device-based MQTT discovery migration for Tier 1 · GitHub releases + HACS default
store submission (needs: ≥1 release, brands, topics/description — then users install
without adding a custom repo).

**Phase 3 — Beyond the market:**
Evidence Pack productization + C2PA manifest (§7.1) · LLM tools (§7.6) · adoption
flow completion (§7.7) · digest ritual (§7.8) · backup hooks (§7.9) · Advanced Camera
Card elements (§7.10).

Sequencing rationale: Phase 0 removes known defects that every later phase would
build on top of; Phase 1 is the *one* structural decision (P1–P4) everything else
assumes; Phase 2 is systematically reaching Gold, where HA UX polish lives; Phase 3
is differentiation that only makes sense on a consolidated base. Phases 0+1 also
clear the HACS-default-store and (optional, later) core-submission paths — Music
Assistant's HACS→core graduation is the precedent worth keeping in view.

---

## 9. Sources

Repo evidence: file:line references throughout §1–§4 (verified against
`custom_components/securacv/`, `privacy_witness_kernel/`, `src/bin/event_mqtt_bridge.rs`,
`src/api/mod.rs`, `homeassistant/`, `docs/blueprints/`).

Platform standards: HA Integration Quality Scale + rules
(developers.home-assistant.io/docs/core/integration-quality-scale/), ADR-0010/0011/0022
(github.com/home-assistant/architecture), fetching-data / entity / device-registry /
config-entries docs, MQTT integration + discovery docs (www.home-assistant.io/integrations/mqtt/),
Apps (add-on) configuration/communication/security/presentation/publishing docs
(developers.home-assistant.io/docs/apps), Repairs/Diagnostics/System-health platform
docs, HACS publishing docs (hacs.xyz/docs/publish/), home-assistant/brands, release
blogs 2024.11–2026.7 (go2rtc, device-based MQTT discovery, subentries, backup
platform, LLM API, dashboard strategies, brands proxy, builder migration, Apps rename).

Best-in-class patterns: blakeblackshear/frigate-hass-integration (`update.py`,
`diagnostics.py`, `llm_functions.py`, `media_source.py`, notification proxy),
Frigate 0.14–0.16 review/semantic-search/GenAI direction, dermotduffy/advanced-camera-card,
SgtBatten/HA_blueprints, ESPHome native API + Improv + Made-for-ESPHome, UniFi
Protect integration, Music Assistant server+integration pairing, Zigbee2MQTT
device-based discovery + ingress panel, Z-Wave JS SmartStart.

Market context: EFF/NPR/CNBC on Ring police-access reversals (2024–2025), ONVIF×C2PA
signed-video collaboration + CISA content-credentials endorsement, FRE 901/902 video
authentication practice, insurer guidance on timestamped footage, Frigate-vs-cloud
false-positive and latency comparisons.
