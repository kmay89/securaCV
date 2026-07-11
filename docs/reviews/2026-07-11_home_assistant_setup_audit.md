# Home Assistant Setup Audit — 2026-07-11

Full audit of the Home Assistant surface: the HACS custom integration
(`custom_components/securacv/`), the Docker sidecar + bundled MQTT broker
(`docker/sidecar/`), the HA↔Frigate MQTT stack (`integrations/ha_frigate_mqtt/`),
the Lovelace cards and shipped dashboards/automations
(`custom_components/securacv/www/`, `homeassistant/`), the alert blueprints
(`docs/blueprints/`), and the setup documentation (`docs/homeassistant_setup.md`
and related). Focus areas: security, config-wizard/setup friction, and
documentation accuracy.

Verification notes: findings below were cross-checked against the firmware's
native MQTT discovery (`firmware/canary/lib/securacv_mqtt/src/securacv_mqtt.cpp`),
which several first-pass findings missed — entities such as the `gps_fix`
binary sensor, `sd_healthy`, and the thermal sensors DO exist (they are created
by the firmware's discovery configs via HA's core MQTT integration, not by the
Python integration). Docker changes were validated with `docker compose config`
only — no Docker daemon is available in the audit environment, so the modified
broker startup command was not exercised at runtime.

## Fixed in this PR

| # | Severity | Area | Finding | Fix |
|---|----------|------|---------|-----|
| F1 | **High** | `docker/sidecar/quickstart-with-broker.compose.yml` | Bundled Mosquitto ran `allow_anonymous true` with the port published on `0.0.0.0` — any LAN host could use the broker with no credentials. | Port binding now defaults to `127.0.0.1` (`SECURACV_MQTT_BIND` to widen), and setting `SECURACV_MQTT_PASSWORD` switches the broker to authenticated mode and feeds the same credentials to the sidecar automatically. Zero-config `docker compose up` still works. |
| F2 | **High** | `homeassistant/lovelace/securacv-canary-dashboard.yaml`, `homeassistant/automations/*.yaml` | Entity IDs used `securacv_DEVICE_ID_*`, but both the integration (`has_entity_name` + device "SecuraCV Canary <id>") and firmware discovery generate `securacv_canary_<id>_*`. Tamper/transport rows used invented suffixes (`_tamper_power_loss`, `_transport_wifi_sta`) instead of the display-name slugs actually generated (`_power_loss`, `_wifi_station`, …). Every such row showed "entity not found". | All entity references corrected; dashboard header now documents the real pattern and points at Developer Tools → States. |
| F3 | **High** | `docs/blueprints/securacv_alerts.yaml` | Blueprint was uninstantiable as designed: a **device** selector was fed into `state:` trigger `entity_id`s (invalid), triggers watched attributes (`tamper`, `smoke_alarm`, `co_alarm`, …) that exist on no entity, and `enable_*` inputs were referenced in templates where blueprint inputs are undefined (all conditions always false). | Rewritten with per-signal optional entity selectors (`default: []` = alert off), matching the working `canary_sense_*` blueprint pattern. Critical-push payloads preserved. |
| F4 | Medium | `custom_components/securacv/services.yaml`, `strings.json`, `translations/en.json` | `securacv.export_chain` / `securacv.verify_chain` were declared and translated but never registered — calling them fails with "Service not found". The canary dashboard also advertised them as device actions. | Dead declarations removed (services.yaml deleted, strings pruned, dashboard markdown reworded). Implementing the services for real is tracked below (R7). |
| F5 | Medium | `custom_components/securacv/__init__.py` | TOFU health handler validated pubkey length but not hex: a 64-char non-hex `public_key` from the broker raised an unhandled `ValueError` inside a background task (attacker-triggerable log noise). | `bytes.fromhex` validation (plus a non-dict payload guard) before scheduling the pin task. |
| F6 | Medium | `custom_components/securacv/__init__.py` | The TOFU docstring claimed "a hostile broker can't forge a new device's first-sight pubkey without already controlling the device" — false. Anyone with broker publish access can pre-emptively pin their own key for a device_id before the genuine device first connects, after which their spoofed publishes verify green. | Docstring rewritten to state the honest trust model: TOFU detects *later* tampering on an initially-honest broker; hostile-broker threat models require manual pinning (Options → Pin a device pubkey) plus broker ACLs. `docs/device_trust.md` already describes manual pinning. |
| F7 | Medium | `custom_components/securacv/__init__.py` | `ap_ip`/`ip` from an untrusted MQTT status payload was interpolated directly into the device page's clickable `configuration_url` (phishing vector: `http://<attacker-host>`). | New `_safe_config_url()` accepts only a bare IP address or plain hostname — no schemes, ports, paths, or credentials. |
| F8 | Medium | `custom_components/securacv/__init__.py` | Config flow declares `VERSION = 2` but no `async_migrate_entry` existed — any v1 entry would fail with "Migration handler not found" and never load again. | Pass-through migration added (v1 → 2 version bump; refuses future-version downgrades). |
| F9 | Low | `custom_components/securacv/__init__.py` | No payload size cap before `json.loads` on broker data in the status/TOFU handlers. | 64 KiB cap (`MAX_MQTT_PAYLOAD_BYTES`) added to both `__init__.py` handlers. Platform-file handlers still uncapped — see R4. |
| F10 | Low | `custom_components/securacv/binary_sensor.py` | Kernel entity names ("SecuraCV Kernel Online") repeated the brand under `has_entity_name`, generating IDs like `binary_sensor.securacv_privacy_witness_kernel_securacv_kernel_online`. | Names shortened to "Online" / "Storage Replacement Recommended". Existing installs keep their registry entity IDs (keyed by unique_id); only the friendly name changes. |
| F11 | Medium | `docs/homeassistant_setup.md` | Quick Start Step 1 said to search HACS for "SecuraCV" (implying default-store availability), contradicting the custom-repository instructions later in the same file and in the README. The integration is a HACS **custom repository** (`hacs.json` has no default-store registration). | Quick Start now gives the custom-repository steps. |
| F12 | Medium | `docs/homeassistant_setup.md` | "Legacy: Witness Kernel Setup" had two "Step 2"s, a "Step 3: Start" after "Step 4", "Option B" orphaned under an unrelated heading, and a duplicated key-generation instruction. | Section restructured: Install → Generate Key → Configure → Start → Add Integration, with Options A/B together. Step 5 (blueprint) text updated for the new blueprint inputs. |
| F13 | Info | `docs/homeassistant_setup.md`, `docs/frigate_integration.md` | — | Troubleshooting row added for the duplicate-entity overlap (see R2); Frigate doc updated for the new broker security defaults. |

## Verified sound (no action)

- **Lovelace cards (XSS):** both `securacv-timeline-card.js` and
  `securacv-aim-card.js` escape all MQTT/entity-derived strings before
  `innerHTML` interpolation; icons and CSS classes come only from hardcoded
  allowlists; no `eval`/`fetch`/external loads. Cards are auto-served and
  auto-loaded by the integration (`add_extra_js_url`) — no manual Lovelace
  resource step, matching the docs.
- **MQTT topic documentation:** every topic in the setup guide's reference
  table matches the firmware (`securacv_mqtt.cpp`), including `sensing`,
  `mic/state|cmd`, and `update/state|cmd`. The three topic namespaces
  (`securacv/`, `witness/`, `frigate/`) are used consistently across all docs.
- **`docs/homeassistant_automations.yaml`:** targets firmware-discovery
  entities (`gps_fix` binary sensor, `sd_healthy`, `die_temperature`,
  `thermal_performance`, `thermal_advisory`) — all real; entity IDs correct.
- **HA↔Frigate stack (`integrations/ha_frigate_mqtt/`):** broker requires
  auth (`allow_anonymous false` + password file), every host port bound to
  `127.0.0.1`, secrets injected via `${SECURACV_MQTT_PASSWORD:?}` with a
  placeholder-only `.env.example`, README quickstart matches the files.
- **Sidecar/standalone images:** non-root (`USER 1001:1001`), no privileged
  mode/cap_add/docker-socket mounts, Event API bound to container loopback,
  `DEVICE_KEY_SEED` handled via 0600 secret file. All CRITICAL/HIGH findings
  from the 2026-06-10 docker review remain fixed.
- **Signature verification (`signature.py`):** structurally invalid payloads
  never verify; all casts guarded; no exceptions escape MQTT callbacks.
- **Config-flow error keys:** every error/abort key used by `config_flow.py`
  exists in `strings.json`; `strings.json` and `translations/en.json` are
  identical.
- **Blueprint/file references:** all blueprint files referenced by docs exist;
  `privacy_witness_kernel/Dockerfile` path in the docs is real.

## Open recommendations (not fixed here)

| # | Priority | Recommendation |
|---|----------|----------------|
| R1 | High | **TOFU hardening:** offer a config-flow option to require manual pinning (reject unpinned devices) for hostile-broker threat models, and document recommended Mosquitto ACLs (`pattern readwrite securacv/%u/#`) so only the device account can publish under its prefix. |
| R2 | High | **Duplicate entity universes:** the firmware's native MQTT discovery and the integration both create a device named "SecuraCV Canary <id>" with overlapping entities (Online, Chain Valid, Tamper, Witness Count, …) under different identifier namespaces, so users following the documented setup get two devices and `_2`-suffixed entities. Either have the integration adopt/claim the MQTT-discovery device (share the `securacv_canary_<id>` identifier), suppress its overlapping entities when discovery configs are retained, or ship a documented "pick one" toggle. |
| R3 | Medium | **Config flow gaps:** no `async_step_reauth` (a static-token kernel entry whose token rotates — ≤10 min — is unrecoverable without delete/re-add) and no `async_step_reconfigure` (URL/prefix/token changes require delete/re-add). Both are standard HA flows and would materially improve wizard UX. |
| R4 | Medium | **Payload caps in platform files:** apply `MAX_MQTT_PAYLOAD_BYTES` to the ~15 `json.loads` sites in `sensor.py`/`binary_sensor.py` (exceptions are already handled; the residual risk is memory pressure from a hostile broker). |
| R5 | Medium | **Kernel API TLS:** the Bearer capability token travels over `http://` by default with no `https` option or `verify_ssl` toggle; fine for the Supervisor network, weak for the documented remote-kernel case. Add scheme validation + a TLS option, or warn when a static token is paired with a remote `http://` URL. |
| R6 | Medium | **MQTT password on argv:** `docker/sidecar/entrypoint.sh` passes `--mqtt-password` on the command line (visible in `/proc/<pid>/cmdline`). Prefer env-var or file-based passing into the bridges. |
| R7 | Medium | **Implement or fully drop chain services:** `export_chain`/`verify_chain` were removed as dead declarations; if the device actions are wanted, implement them end-to-end (MQTT request/response + service registration). |
| R8 | Low | **manifest.json:** `mqtt` is a hard dependency, blocking kernel-only users who have no broker — consider `after_dependencies` plus a flow-level check; `cryptography` is imported but undeclared (HA bundles it today); `iot_class: local_push` is only half-true for the polling kernel mode. |
| R9 | Low | **Compose hygiene:** pin images by digest (`eclipse-mosquitto:2`, `frigate:stable`, `home-assistant:stable`, sidecar `:latest`), add resource limits, add a `HEALTHCHECK` to the sidecar image (the root image has one). |
| R10 | Low | **Frigate demo config:** `integrations/ha_frigate_mqtt/frigate.yml` still ships the loopback `rtsp://127.0.0.1:8554/demo` placeholder (documented, but Frigate spins in a retry loop until edited). |
| R11 | Low | **Diagnostics redaction:** `diagnostics.py` emits `CONF_URL` verbatim; adopt the standard `async_redact_data` pattern in case users embed credentials in URLs. |
| R12 | Low | **Untranslated selector labels:** the config flow's mode-picker option labels are hardcoded English in the schema rather than translated via `strings.json`. |
