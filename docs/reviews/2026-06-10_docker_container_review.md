# Docker Container Review — Completeness, Correctness, and Feature Parity

**Date:** 2026-06-10
**Scope:** every container artifact in the repository — the standalone `witnessd` image
(`Dockerfile`), the Home Assistant add-on image (`privacy_witness_kernel/Dockerfile`), the
local integration stack (`integrations/ha_frigate_mqtt/docker-compose.yml` + supporting
configs/scripts), the CI workflows that build/verify images, and the documentation that
describes them.
**Method:** every documented build/run command was treated as untrusted and checked against
the files it references; every environment variable, flag, port, and endpoint used by a
container was verified to exist in source. Findings cite file (and line where stable).

---

## 1. Executive summary

**The two Dockerfiles are well-engineered; the glue around them was broken.** The add-on
image is the flagship and is internally consistent end-to-end. The standalone image **no
longer built at all** (its pinned `rust:1.77` toolchain cannot read the repo's version-4
`Cargo.lock`), and its healthcheck could never pass even when it did build. The compose
demo stack was broken at four
independent layers: it referenced a Dockerfile that has never existed in the repository's
history, so it could not build; its `command` was mangled by compose interpolation and
argv-splitting, so the bridges could never have started; no service could have
authenticated to the MQTT broker, so no events could have flowed; and the documented
broker bootstrap crashed before a password could ever be set.

All breakages found are **fixed in the change that accompanies this review** (§5). Parity
gaps that are design decisions rather than defects are cataloged in §4 with
recommendations, not patches.

| Artifact | Verdict before this review | Verdict after fixes |
|---|---|---|
| `Dockerfile` (standalone witnessd) | Unbuildable (rust 1.77 vs lock file v4); permanently `unhealthy` healthcheck | Toolchain bumped; healthcheck repaired; CI build gate added |
| `privacy_witness_kernel/Dockerfile` (HA add-on) | Sound | Unchanged |
| `integrations/ha_frigate_mqtt/` compose stack | Cannot build; cannot start bridges; cannot authenticate | Builds; pipeline + auth verified live |
| Container docs (`docs/container.md`, `docs/homeassistant_setup.md`, integration README) | Two phantom-path commands; broker bootstrap crashes; missing credential step | Corrected |

---

## 2. Findings

### 2.1 CRITICAL — compose referenced a Dockerfile that never existed

`integrations/ha_frigate_mqtt/docker-compose.yml` built the `securacv` service from
`homeassistant/Dockerfile` (context `../..`). `git log --all -- homeassistant/Dockerfile`
is empty: the file was never committed at any point in history. `homeassistant/` contains
only HA automation and Lovelace YAML. Consequences:

- `docker compose up -d --build` — step 4 of the integration README — failed immediately.
- `docs/homeassistant_setup.md` "Option 2: Local Installation" instructed
  `docker build -f homeassistant/Dockerfile …` and `cp -r homeassistant /addons/…`, both
  pointing at the wrong directory (the add-on lives in `privacy_witness_kernel/`).

Note the deeper inconsistency: even re-pointing compose at the root `Dockerfile` would not
have worked, because that image ships **only** `witnessd` while the compose `command` runs
`witness_api`, `frigate_bridge`, and `event_mqtt_bridge`.

### 2.2 CRITICAL — standalone image healthcheck always failed (two independent ways)

`Dockerfile` (pre-fix):

```dockerfile
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD ["witnessd", "--health-check"] || exit 1
```

1. **Malformed form.** Appending `|| exit 1` to exec-form JSON makes the whole string
   invalid JSON, so Docker silently falls back to shell form and runs
   `/bin/sh -c '["witnessd", "--health-check"] || exit 1'`. The shell cannot execute the
   literal token `["witnessd",` → command not found → `exit 1` on every probe.
2. **The flag does not exist.** `witnessd` parses only `--ui` (`src/bin/witnessd.rs`,
   `parse_ui_flag`) and ignores unknown arguments — `witnessd --health-check` would have
   started a **second full daemon** inside the container on every probe, to be killed by
   the 5-second timeout.

Either defect alone makes the container permanently `unhealthy`; orchestrators keyed on
health status (compose `depends_on: condition: service_healthy`, swarm, k8s liveness via
Docker health) would restart or never schedule it.

### 2.3 CRITICAL — `docker build` of the standalone image fails: toolchain older than the lock file

Caught by the new CI gate (§5.7) on its first run: both the root `Dockerfile` and the new
compose Dockerfile initially pinned `rust:1.77-slim`, but the repo's `Cargo.lock` is lock
file **version 4**, which requires cargo ≥ 1.78 —
`error: failed to parse lock file … lock file version 4 was found, but this version of
Cargo does not understand this lock file`. The documented `docker build -t witnessd:local .`
(`docs/container.md`) has therefore failed for everyone since the lock file was upgraded;
with no CI building these images, nothing noticed. Fixed by bumping both build stages to
`rust:1.93-slim-bookworm` (cargo ≥ 1.78 for the lock file, and the locked gstreamer/glib
crates declare `rust-version` 1.92; the `-bookworm` suffix pinned explicitly so the build-stage
glibc keeps matching the `debian:bookworm-slim` runtime even after `-slim` retags to a
newer Debian).

One layer deeper, also CI-caught: the build stage was missing `libssl-dev` — the bundled
SQLCipher (`rusqlite` `bundled-sqlcipher`) compiles against OpenSSL headers
(`fatal error: openssl/crypto.h: No such file or directory`) and links `libcrypto`
dynamically. The add-on Dockerfile already handled this on Alpine (it installs
`openssl-dev` and documents the runtime `libcrypto3` pin); the Debian Dockerfiles never
did. Fixed: `libssl-dev` added to both build stages, and `libssl3` listed explicitly in
both runtime stages instead of arriving as a transitive dependency of curl.

### 2.4 HIGH — MQTT credentials were never wired to any client

`mosquitto.conf` is correctly hardened (`allow_anonymous false`, `password_file`), and the
integration README documents creating a `securacv` broker user. But nothing consumed those
credentials:

- `frigate.yml` had no `user`/`password` under `mqtt:` → Frigate refused by broker → no
  `frigate/events` published.
- The `securacv` service set no `MQTT_USERNAME`/`MQTT_PASSWORD`, although both bridges
  support exactly these env vars (`src/bin/frigate_bridge.rs:82-87`,
  `src/bin/event_mqtt_bridge.rs:69-74`) → both bridges refused by broker.

Net effect: even with the build fixed, the pipeline advertised in the README
(Frigate → MQTT → `frigate_bridge` → sealed log → `event_mqtt_bridge` → HA) could not move
a single event. An earlier repo review (2026-06-10 full repo review §5.4) believed this
stack ran `allow_anonymous true`; that is not the case — auth is required and was simply
unsatisfiable.

Related: the README's credential bootstrap itself could not run. It said to start the
broker and then `docker compose exec mosquitto mosquitto_passwd …`, but mosquitto 2.x
**exits fatally when the configured `password_file` does not exist** (verified:
`Error: Unable to open pwfile` → `terminating`), so there is never a running container to
`exec` into. The password file must be created with a one-off
`docker compose run --rm --no-deps …` container before the broker first starts (and owned
by the in-container `mosquitto` user, since `mosquitto_passwd` in a root one-off creates
it root-owned with mode 0600 the broker can't read).

### 2.5 HIGH — the compose `command` could never start the bridges

Discovered while verifying the fixes; two layered defects in the `securacv` service's
`command` (pre-fix):

1. **Host-scope interpolation swallowed the variable.** Compose interpolates `$VAR` in any
   value at parse time from the *host* environment. `$WITNESS_API_TOKEN_PATH` is set for
   the container (in `environment:`) but not on the host, so compose substituted a blank
   string — turning the readiness gate into `until [ -f "" ]`, which is false forever: the
   loop never exits and neither bridge ever starts. (`docker compose config` warns
   "variable is not set, defaulting to blank" — the warning was the tell.)
2. **String-form `command` is shell-lexed into argv.** With
   `entrypoint: ["/bin/sh", "-c"]`, compose splits the folded string into tokens and
   appends them all, so only the first token (`set`) is the `-c` script; everything else
   becomes positional parameters. The container would have executed `set` and exited.

Fixed by making the script a **single list element** (literal block scalar) and escaping
container-shell variables as `$$VAR` so compose leaves them for the shell.

### 2.6 MEDIUM — feature parity gaps (by design; documented, not patched)

See the parity matrix in §4. Highlights:

- The standalone image carries no operator tooling: `log_verify`, `export_events`,
  `export_verify`, `break_glass` are absent, so integrity verification or evidence export
  cannot be done from inside the container that owns `/data`. (Workaround: bind-mount
  `/data` and run tooling on the host or in the add-on image.)
- Neither image enables the optional ingest backends (`ingest-v4l2`,
  `ingest-file-ffmpeg`, `ingest-esp32`), `adapter_host` (and its adapter feature family),
  `break_glass_serve`, `envelope_verify`, `grove_vision2_ingest`, or the PQC features.
  The root image accepts `--build-arg CARGO_FEATURES=…` so variants are buildable, but the
  runtime stage installs only GStreamer/seccomp libs — an ffmpeg/v4l2 variant would also
  need runtime packages.

### 2.7 LOW — demo camera placeholder

`frigate.yml` ships `rtsp://127.0.0.1:8554/demo`, which is loopback **inside the Frigate
container**; nothing serves it. The README does instruct users to replace it (step 3), so
this is acceptable for a demo config, but Frigate will sit in an ffmpeg retry loop and
emit no detections until it is edited. Kept as-is.

### 2.8 LOW — cosmetics and robustness notes

- `version: "3.9"` in the compose file is obsolete under Compose v2+ (warning noise).
  Removed.
- The `securacv` service `command` backgrounds three processes and ends with `wait`: if
  one bridge dies, the container keeps running degraded (dash has no `wait -n`). The new
  healthcheck catches a dead `witness_api`; a dead bridge is visible only in logs.
  Acceptable for a demo stack; a supervisor (or three single-process services) would be
  the production-grade shape.
- The build stage copies `docs/`, `tests/`, `examples/`, `spec/` into the image. This is
  deliberate (`.dockerignore` documents that the root Dockerfile COPYs them) but enlarges
  the build context; runtime images are unaffected.

---

## 3. What verifies as sound

- **Add-on image internal consistency** (`privacy_witness_kernel/Dockerfile`): every
  binary and script `run.sh` invokes is present in the image (8 binaries + wizard +
  `discover_cameras.sh`); the wizard's ingress port (8788) matches `config.yaml`; the
  readiness probe target `http://127.0.0.1:8799/health` exists and is unauthenticated
  (`src/api/mod.rs:339`). Build/runtime ABI pinning (same Alpine base both stages,
  `-C target-feature=-crt-static`) and cargo-chef dependency caching are correctly
  reasoned and commented.
- **CI publishability gate**: `.github/workflows/addon-image.yml` builds amd64 + aarch64
  via buildx/QEMU with per-arch cache scopes, and `verify_published_image.sh` anonymously
  probes GHCR for every declared arch and tag after publish — catching the
  "green build, private image" failure mode HA Supervisor would otherwise hit.
- **Documented env vars are real**: every variable in `docs/container.md` and the compose
  file resolves to code (`WITNESS_RTSP_URL`, `WITNESS_API_ADDR` → `src/config.rs`;
  `BREAK_GLASS_SEAL_TOKEN` → `src/bin/witnessd.rs:516`; `WITNESS_API_TOKEN_PATH`,
  `ALLOW_REMOTE_MQTT`, `DAEMON_MODE`, `HA_DISCOVERY_PREFIX`, `FRIGATE_MQTT_TOPIC`,
  `MQTT_TOPIC_PREFIX` → bridge arg definitions).
- **Security posture**: non-root (`USER 1001:1001`) in both repo-owned images; single
  exposed port (8799, Event API only); no raw-media endpoints; host port bindings in
  compose restricted to `127.0.0.1`; device key seed delivered via Docker secret, not
  environment; mosquitto hardened by default.
- **Non-loopback bind policy**: binding the API to `0.0.0.0:8799` without TLS logs a
  conformance warning but does not refuse (`src/api/mod.rs:151-163`), so the standalone
  container's default works; loopback-configured listeners reject non-loopback peers.

---

## 4. Feature parity matrix

Binaries declared in `Cargo.toml` vs. what each image ships:

| Binary | Standalone image (`Dockerfile`) | Add-on image (`privacy_witness_kernel/`) | Compose image (`integrations/ha_frigate_mqtt/`, new) |
|---|---|---|---|
| `witnessd` | ✅ (entrypoint) | ✅ | — (not needed; Frigate owns cameras) |
| `witness_api` | — | ✅ | ✅ |
| `frigate_bridge` | — | ✅ | ✅ |
| `event_mqtt_bridge` | — | ✅ | ✅ |
| `log_verify` | — | ✅ | ✅ |
| `break_glass` | — | ✅ | — |
| `export_events` / `export_verify` | — | ✅ / ✅ | — |
| `envelope_verify` | — | — | — |
| `break_glass_serve` | — | — | — |
| `grove_vision2_ingest` | — | — | — |
| `adapter_host` | — | — | — |
| `demo` / `tamper_demo` / `ingest_run` / `detect_eval` | — (dev tools) | — | — |

Cargo features: standalone and add-on images build with `rtsp-gstreamer` only; the compose
image builds feature-less (no RTSP in that stack, hence no GStreamer in the image at all).
Not enabled anywhere: `rtsp-ffmpeg`, `ingest-file-ffmpeg`, `ingest-v4l2`, `ingest-esp32`,
`backend-tract`, the `adapter-*` family, `pqc-*`. The add-on's two run modes
(`standalone`/`frigate` in `run.sh`) are both fully covered by what the image ships.

Non-container components with no image (and no claim of one): the HA custom component
(runs inside HA core), the Canary Vision Node.js device API (runs on-device), firmware,
and the offline evidence viewer (static HTML).

---

## 5. Fixes applied with this review

1. **Compose now builds a real image.** An interim
   `integrations/ha_frigate_mqtt/Dockerfile` was written for this review (the four
   pipeline binaries, no GStreamer, non-root, `/health` healthcheck), but while the
   review was in flight, main's PR #749 introduced `docker/sidecar/Dockerfile` — the
   same shape plus entrypoint supervision, automatic device-key generation, and a
   `doctor` diagnostic — so this branch adopts the sidecar (compose points at
   `docker/sidecar/Dockerfile`) and the interim Dockerfile was dropped.
2. **Standalone healthcheck repaired** — `curl` added to runtime deps; probe is now
   shell-form `curl -fsS http://127.0.0.1:8799/health` against the Event API `witnessd`
   already serves (`WITNESS_API_ADDR=0.0.0.0:8799`).
3. **Build toolchain unblocked** — both Dockerfiles' build stages bumped from
   `rust:1.77-slim` to `rust:1.93-slim-bookworm` so cargo can read the version-4
   `Cargo.lock` (§2.3), with the Debian release pinned to keep build/runtime glibc
   matched.
4. **MQTT credentials wired end-to-end** — `.env.example` added (gitignore updated to
   keep ignoring `.env` while allowing the template); compose injects
   `MQTT_USERNAME`/`MQTT_PASSWORD` into the sidecar and
   `FRIGATE_MQTT_USER`/`FRIGATE_MQTT_PASSWORD` into Frigate (both from
   `SECURACV_MQTT_PASSWORD`); `frigate.yml` consumes the placeholders; compose fails
   fast with a clear message if the password is unset.
5. **Compose `command` eliminated** — the §2.5 fix initially rewrote the inline script
   as a single `$$`-escaped list element; with the sidecar adoption (§5.1) the inline
   `command` is gone entirely — process supervision lives in the image's
   `entrypoint.sh`, which is the production-grade shape §2.8 asked for.
6. **Docs corrected** — `docs/homeassistant_setup.md` local-install commands now point at
   `privacy_witness_kernel/`; integration README gains the `.env` step, the working
   one-off `mosquitto_passwd` bootstrap (§2.4), and notes that HA's MQTT integration
   needs the same credentials. Obsolete compose `version:` key removed.
7. **Sidecar broker race fixed** — the sidecar entrypoint's broker preflight was a
   single-shot TCP probe; if mosquitto wasn't listening yet at sidecar start (compose
   `depends_on` and the e2e harness only order container *startup*, not readiness),
   the container died with "broker not reachable". Caught when the sidecar e2e flaked
   on this PR (it had passed on identical code with luckier timing). The probe now
   retries for up to `BROKER_WAIT_SECS` (default 30s) before giving up.
8. **New CI gate** — `.github/workflows/container-images.yml` build-validates the root
   `Dockerfile` on PRs/pushes touching it (it previously had **no** CI build at all,
   which is how both the phantom-Dockerfile reference and the stale toolchain pin
   shipped) and smoke-tests that the binary loads with no missing shared libraries and
   that curl — which the healthcheck depends on — is present. It caught §2.3 on its
   first run. The sidecar and add-on images are gated by their own workflows
   (`docker-sidecar.yml`, including a live e2e, and `addon-image.yml`).

### Recommended follow-ups (not in this change)

- Publish ingest-variant images (ffmpeg/v4l2) via `CARGO_FEATURES` build args **plus**
  matching runtime packages, if those backends are meant to be deployable by container.
- Add operator tooling (`log_verify`, `export_*`) to the standalone image, or document the
  bind-mount workaround.

---

## 6. Verification performed

The review environment's egress policy blocks `deb.debian.org`, so the Dockerfiles' apt
layers could not run locally; everything else was exercised for real, and the full image
builds are covered by the new CI workflow (§5.7) on this change's own PR.

- `cargo build --release --bin witness_api --bin frigate_bridge --bin event_mqtt_bridge
  --bin log_verify` — the new Dockerfile's exact build command — compiles clean with no
  features (confirming the compose image needs no GStreamer).
- `docker compose config` validates (and surfaced the §2.5 interpolation bug via its
  "variable is not set" warning); the fixed `command` renders as a single script element.
- Live auth chain, exactly as the compose service runs it: compose `mosquitto` service
  brought up with the repo `mosquitto.conf` (auth required); anonymous connect refused
  (`not authorized`), credentialed connect accepted. `witness_api` +
  `frigate_bridge --db-path …` + `event_mqtt_bridge --daemon --api-token-path …` started
  with the service's exact environment: token file written, both bridges log
  `Connected to MQTT broker (… auth: true)`, `Subscribed to frigate/events (QoS 1)`,
  `Published online status to witness/status`.
- Synthetic Frigate detection published to `frigate/events` →
  `Event logged: BoundaryCrossingObjectLarge zone=zone:demo conf=0.91` and encrypted
  `witness.db` created — end-to-end ingestion through the authenticated broker.
- `curl -fsS http://127.0.0.1:8799/health` → `{"status":"ok"}` — the endpoint both
  repaired healthchecks probe.
- Broker bootstrap: reproduced the fatal `Unable to open pwfile` crash of the documented
  `exec` flow; verified the one-off `docker compose run … mosquitto_passwd` flow brings
  the broker up authenticated.
- Add-on image not rebuilt here; it is CI-gated by `.github/workflows/addon-image.yml`
  including the post-publish public-pullability check.
