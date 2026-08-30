# SecuraCV — Feature-Flag Registry & Conventions

> Single source of truth for **every feature flag in the repo**, across all four
> layers, plus the lifecycle/naming rules that govern them. Enforced by
> [`scripts/lint_feature_flags.sh`](../scripts/lint_feature_flags.sh) (run in CI
> via `.github/workflows/lint.yml`). If you add, rename, or remove a flag,
> update this file in the same PR — the lint fails otherwise.
>
> Companion review: [`docs/review/03-feature-flags-and-hygiene.md`](review/03-feature-flags-and-hygiene.md).

## Why a registry

SecuraCV gates behavior with flags at four different layers. Each layer was
internally consistent, but nothing tied them together — there was no one place
that said *what flags exist, what they default to, and when they get removed*.
This file is that place. It does **not** duplicate the per-layer detail (e.g. the
firmware parity matrix); it indexes it and points at the authoritative source.

## The four flag layers — pick the right one

| Layer | Use it for | Mechanism | Authoritative source |
|---|---|---|---|
| **Rust compile-time** | Optional native deps / backends that shouldn't be in every build (codecs, ONNX, PQC, adapters) | Cargo `[features]` + `#[cfg(feature=…)]` / `required-features` | [`Cargo.toml`](../Cargo.toml) `[features]` |
| **Firmware compile-time** | Per-board capability selection on memory-constrained ESP32 targets | `FEATURE_*` C macros / `build_flags` | [`firmware/FEATURES.md`](../firmware/FEATURES.md) (parity matrix) |
| **Runtime config** | Operator-tunable behavior that must change without rebuilding (backend choice, thresholds, retention, detection toggles) | TOML + `WITNESS_*` env (kernel) and JSON device-state (canary-vision) | [`config.example.toml`](../config.example.toml), [`canary-vision/device-api/lib/device-state.js`](../canary-vision/device-api/lib/device-state.js) |
| **Capability gating** | Hiding declared-but-unbuilt surface so the product never advertises what no device can do | `ALL_* ` vs `FUTURE_*` lists | [`custom_components/securacv/const.py`](../custom_components/securacv/const.py) |

**Decision rule:** if a flag selects *which code is compiled* → compile-time
(Cargo/firmware). If it tunes *running behavior* → runtime config. If it hides
*an unfinished feature from users* → capability gate (and it stays gated until
wired end-to-end — see the convention below). Don't reach for a runtime config
key to do a compile-time job, or vice-versa.

## Conventions (normative)

1. **Default off for unfinished work.** A flag guarding incomplete behavior
   ships **off by default**. Merging behind an off flag is fine; advertising it
   is not.
2. **Never advertise unbuilt features.** Declared-but-unimplemented capabilities
   live in an explicit `FUTURE_*` list and **must not** appear in the
   corresponding `ALL_*` list until wired end-to-end. Canonical example:
   `FUTURE_TRANSPORTS` vs `ALL_TRANSPORTS` (`const.py:49-65`). The lint enforces
   this for transports.
3. **Lifecycle.** Every flag is in one stage: **experimental** (off by default,
   may change/vanish) → **stable** (load-bearing, documented default) →
   **deprecated** (scheduled for removal, has a removal criterion) → **removed**
   (deleted in a PR that also drops its dead code paths). Record the stage in
   the tables below.
4. **Removal criteria.** A deprecated or experimental flag needs a written exit
   condition (e.g. "remove once `rtsp-ffmpeg` is the only RTSP path" — cf.
   flag-report F-11's two RTSP impls). No open-ended flags.
5. **Naming.** Cargo features are `kebab-case` grouped by domain prefix
   (`ingest-*`, `adapter-*`, `pqc-*`, `rtsp-*`). Firmware macros are
   `FEATURE_<SUBSYSTEM>`. Env overrides are `WITNESS_<AREA>_<KEY>`. Keep new
   flags inside an existing prefix where one fits.
6. **Register on creation.** New flag → new row here, same PR.

---

## Registry

Lifecycle key: `exp` experimental · `stable` · `dep` deprecated · `future` declared-but-unbuilt.

### Rust compile-time (Cargo `[features]`)

Source of truth: `Cargo.toml:55-71`. Default build enables **none** of these
(all are opt-in); the default detection path is the in-tree stub/CPU backend.

| Flag | Default | Lifecycle | Scope / purpose | Removal criteria |
|---|:---:|:---:|---|---|
| `rtsp-gstreamer` | off | stable | RTSP ingest via GStreamer | keep (one of two supported RTSP backends) |
| `rtsp-ffmpeg` | off | stable | RTSP ingest via ffmpeg | keep |
| `ingest-file-ffmpeg` | off | stable | Local file decode via ffmpeg (`ingest_run`) | keep |
| `ingest-esp32` | off | stable | ESP32-S3 HTTP/UDP camera ingest | keep |
| `ingest-v4l2` | off | stable | `/dev/video*` ingest | keep |
| `stub-frame-source` | off | stable | Synthetic frames for tests/dev | keep (test affordance) |
| `backend-tract` | off | exp | ONNX object detection (tract). Off-by-default; default build is frame-diff stub | promote to stable + bundle a model once detection is the documented default (flag-report F-01) |
| `detect-eval` | off | exp | Perception eval harness (`detect_eval` bin: precision/recall/AP/latency); pulls in `backend-tract` + `image` | keep as a dev/CI eval tool; promote only if it becomes a shipped runtime capability |
| `adapter-frigate` | off | stable | Frigate event adapter | keep |
| `adapter-mqtt-sensor` | off | stable | MQTT sensor adapter | keep |
| `adapter-webhook` | off | stable | Webhook adapter (implies `adapter-mqtt-sensor`) | keep |
| `adapter-webhook-tls` | off | stable | Webhook adapter + rustls TLS/mTLS | keep |
| `adapter-ble-presence` | off | stable | BLE presence adapter | keep |
| `adapter-meshtastic` | off | stable | Meshtastic LoRa-mesh detection-sensor adapter | keep |
| `adapter-can-bus` | off | exp | Passive-only vehicle CAN bus adapter (arrival/departure claims via Linux SocketCAN) | promote to stable once bench-validated against a real vehicle (docs/hardware/canary_vehicle_can.md) |
| `adapter-sandbox` | off | stable | Sandbox/test adapter | keep |
| `bridge-homekit` | off | exp | Apple Home egress projection core: the closed coarse-boolean vocabulary plus the pacer that publishes on a fixed cadence rather than on events (`src/bridge/homekit.rs`, docs/design/apple_home_integration.md). Pure Rust — no HAP server or socket | promote to stable once the server lane below has paired against real Apple controllers |
| `bridge-homekit-server` | off | exp | The HAP accessory server (bridge site B): mDNS discovery, Pair Setup/Pair Verify, the encrypted session and the accessory database, driven by the pacer above (`src/bridge/hap/`). Implies `bridge-homekit`; adds `srp`, `x25519-dalek`, `mdns-sd` — both published `hap` crates failed the FR-14 gate (one does not compile, the other cannot be resolved) | promote to stable once it has paired against real Apple controllers and open decision #2 (commercial licensing) is answered |
| `pqc-signatures` | off | exp | ML-DSA post-quantum signatures | promote once PQC signing is a supported deployment mode |
| `pqc-vault` | off | exp | ML-KEM post-quantum vault sealing | promote once PQC vault is supported |
| `pqc-tls` | off | exp | PQC-capable TLS stack | promote once PQC transport is supported |
| `tsa` | off | stable | Online RFC 3161 TSA client for `log_anchor request` (ureq + 30s timeout). The offline query/import anchoring flow needs no feature | keep (network egress stays opt-in at build time by design — see docs/timestamping.md) |
| `alert-relay` | off | exp | The hub-side metadata-only alert relay (`alert_relay` bin): coarse pokes fanned out to ntfy and/or a Busy Bar desk light (`--busybar-url`, LAN-only — the vendor cloud is refused), fingerprint-free topic list, per-class debounce and retry per sink (docs/design/alert_relay.md, docs/integrations/busy-bar.md). Zero new crates — rides the already-vetted ureq | promote to stable once the away-detection policy (alert_relay.md §7) is specced and the lane has run on a real hub |
| `c2pa-export` | off | exp | C2PA Content Credentials sidecar sign/verify for export bundles (`export_events --c2pa`, `export_verify --c2pa-manifest`); fully offline, no HTTP backend compiled in (docs/design/c2pa_export.md) | promote to stable once the sidecar format survives a release cycle of third-party verifier use |

> The full set of CI-built feature combinations is in `.github/workflows/rust.yml`
> (`--no-default-features`, `pqc-*`, per-ingest jobs).

### Firmware compile-time (`FEATURE_*`)

Source of truth: [`firmware/FEATURES.md`](../firmware/FEATURES.md) — the
per-variant parity matrix (see its "Feature Flags Comparison" section). Defaults
vary **per board variant**; do not duplicate them here. Representative flags:
`FEATURE_CAMERA_PEEK`, `FEATURE_GNSS`, `FEATURE_SD_STORAGE`, `FEATURE_WIFI_AP`,
`FEATURE_MESH_NETWORK`, `FEATURE_BLE`, `FEATURE_OTA_UPDATE`, `FEATURE_HA_MQTT`,
`FEATURE_WATCHDOG`, `FEATURE_TAMPER_GPIO`, `DEBUG_*`.

> Convention reminder: `firmware/FEATURES.md:33` defines a CI contract that a PR
> regressing a ✅→⚠️/❌ dashboard cell must cite an issue. Wiring that guard is
> tracked as a follow-up in the hygiene review (Part B).

### Runtime config (kernel TOML / `WITNESS_*` env)

Source of truth: `src/config.rs` + `config.example.toml`. These are tunables, not
build switches; defaults below match `config.example.toml`.

| Key (env override) | Default | Lifecycle | Purpose |
|---|---|:---:|---|
| `ingest.backend` (`WITNESS_INGEST_BACKEND`) | `rtsp` | stable | Select ingest backend (`file`/`rtsp`/`v4l2`/`esp32`) |
| `rtsp.backend` (`WITNESS_RTSP_BACKEND`) | `auto` | stable | `auto`/`gstreamer`/`ffmpeg` |
| `detect.backend` (`WITNESS_DETECT_BACKEND`) | `auto` | stable | `auto`/`stub`/`cpu`/`tract` |
| `detect.confidence` (`WITNESS_DETECT_CONFIDENCE`) | `0.5` | stable | Min detection confidence |
| `retention.seconds` (`WITNESS_RETENTION_SECS`) | `604800` | stable | Sealed-event retention |
| `zones.sensitive` (`WITNESS_SENSITIVE_ZONES`) | front_boundary | stable | Zones that trigger vault sealing |

> Device-side (canary-vision) runtime toggles live in
> `canary-vision/device-api/lib/device-state.js` (`privacy.*`, `detection.*`,
> `integrations.*`). **Immutable invariant:** `privacy.camera_peek_enabled` is in
> `IMMUTABLE_PRIVACY_KEYS` (`canary-vision/device-api/routes/config.js:12`) and
> cannot be changed via any API (Invariant I — No Raw Export). Do not move it out
> of that list.

### Capability gates (HA integration)

Source of truth: `custom_components/securacv/const.py`.

| Constant | List membership | Lifecycle | Notes |
|---|---|:---:|---|
| `TRANSPORT_LORA` | `FUTURE_TRANSPORTS` | future | LoRa radio — not on any firmware; must stay out of `ALL_TRANSPORTS` (lint-enforced) |
| `TRANSPORT_AUDIO` | `FUTURE_TRANSPORTS` | future | SCQCS audio squawks — same |

When a `FUTURE_*` transport is wired end-to-end on a device, move it into
`ALL_TRANSPORTS`, update its row here, and the lint will pass.

---

## Checking

```sh
bash scripts/lint_feature_flags.sh
```

Verifies: no orphaned Cargo feature, no `FUTURE_*` transport advertised in
`ALL_TRANSPORTS`, and every Cargo feature is listed in this registry.
