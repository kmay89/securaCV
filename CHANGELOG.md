# Changelog

## [Unreleased]

### canary-wap: BLE bring-up moves to a worker task (loop watchdog crash fix) + memory-budget instrumentation

Field regression from the deferred bring-up: ~21 s after boot,
`task_wdt: loopTask` with both cores idle — the loop was parked inside the
BLE bring-up (NimBLE controller/host init synchronizes with the WiFi
coexistence layer and can block its caller past the loop's 8 s watchdog
budget), two crashes from tripping safe mode.

- The deferred Bluetooth/BLE bring-up now runs on a **one-shot worker task**
  (same pattern as the SD mount worker and the MJPEG stream worker); the
  loop task only spawns it and can never be blocked by it.
- The `canary.local` steward skips its blocking mDNS operations while a
  fleet browse is in flight — the mDNS component serializes API calls, so
  stacking the loop's delegate ops behind the worker's ~3 s `_securacv._tcp`
  search parked the loop for the sum.
- **Memory-budget ledger**: one `[HEAP] after <phase>: internal free/largest`
  line after each heavy boot step (camera, SD, audio, network, mesh) and
  around the BLE bring-up, so the "can Bluetooth fit on this build?"
  question reads directly off any boot log instead of being reconstructed
  from crash forensics. Field measurement so far: the FULL/S3 profile
  reaches the BLE gate with ~40 KB free internal where the stack needs
  ~96 KB — Bluetooth on FULL requires a feature trade or a custom
  (non-prebuilt-core) build; the guard now proves it per-boot.
- Bluetooth API errors carry the recorded refusal reason: `/api/bluetooth/
  enable`, advertise/pair auto-enable, and scan-start now return "internal
  RAM too low (largest block N KB/48 KB, free N KB/96 KB)" instead of
  "Failed to enable Bluetooth" or the stale "check antenna / NimBLE
  library" guess.

### canary-wap: two Canaries on one WiFi now coexist — fleet discovery, canary.local dedupe, Bluetooth boot-order fix

Field report with two devices on the same home network: Bluetooth showed
"NimBLE init failed" on both (PSRAM enabled!), the Fleet sheet said "No other
Canaries found yet", the WiFi-QR generator rendered a bare red "Failed", and
`canary.local` reached an arbitrary device that could *change between
requests* — silently invalidating the session cookie mid-use.

- **Bluetooth initializes from the loop once the provisioning window clears
  — not from setup() at all.** (Corrected from the first cut of this change,
  which initialized BLE *before* WiFi: on the FULL build the stack's
  ~55–65 KB internal-RAM spend then starved the network — httpd couldn't
  create its socket (`ENOBUFS`), the SoftAP's WPA2 handshake failed so
  phones looped on the password prompt, and the heap monitor sat in
  EMERGENCY at 2 KB free.) The whole bring-up now runs one-shot from
  `ble_discovery_start_if_due()` after the setup AP is torn down — the point
  of *maximum* free internal memory — and the heap guard gained a total-free
  axis (`bt_defaults::MIN_INIT_TOTAL_FREE`, host-tested): BLE only starts if
  it leaves real operating margin, because "Bluetooth up, network dead" is
  strictly worse than "no Bluetooth, honest reason shown".
- **The self-test now says WHY Bluetooth is off** instead of the catch-all
  "NimBLE init failed": `bluetooth_channel::init_fail_reason()` distinguishes
  the heap-guard refusal ("internal RAM too fragmented (largest free block
  N KB, need 48 KB)") from a real stack failure, surfaced in `/api/selftest`,
  `/api/bluetooth`, and the Bluetooth settings tab.
- **Fleet now discovers Canaries on the LAN.** Every device already
  advertised a `_securacv._tcp` mDNS service with its identity — nothing
  consumed it; the Fleet sheet listed only ESP-NOW opera-mesh members (an
  explicit pairing flow), so WiFi-sharing devices were invisible to each
  other. New `GET /api/fleet/scan` browses the service from a short-lived
  worker task (the ~2 s blocking browse never stalls the single httpd task —
  same lesson as the MJPEG stream) with a 10 s cache; the Fleet sheet lists
  every Canary (self marked "this one") with name, unique
  `canary-<name>.local` hostname, IP, and an Open link to its dashboard.
- **canary.local can no longer be double-claimed.** The bare-hostname
  catch-all used a single 600 ms first-wins probe at STA join; two devices
  powering up together (power restored) both probed into silence and BOTH
  claimed it — the field's session-flip. The claim is now scheduled with a
  fingerprint-derived stagger (one claims first, the other's probe sees it),
  and a periodic conflict check applies a deterministic IP tie-break so a
  surviving double-claim resolves to exactly one keeper (host-tested
  antisymmetry, `catchall_logic.h` + `test_catchall_logic.cpp`).
- **WiFi-QR provisioning:** the button's opaque red "Failed" now reports the
  actual cause (session expired / rate-limited / error N); the generator
  refuses credentials containing `;` with a clear 400 instead of encoding a
  QR that silently truncates at scan time (the payload is `;`-delimited with
  no escape sequence).

### canary-wap: camera preview no longer freezes the whole dashboard, and its numbers are real

The MJPEG preview handler used to run its frame loop **on esp_http_server's
single worker task**, so for as long as a preview streamed, every other HTTP
request queued behind it: `/api/peek/status` polls (hence "Current: Unknown",
"THROUGHPUT 0 kbps", "STREAM UPTIME —" *while streaming*), the sensor tuning
sliders, and every other dashboard tab. Field-reported as "camera settings
don't even work".

- **Stream moved to a dedicated FreeRTOS worker task** via
  `httpd_req_async_handler_begin/_complete`, freeing the httpd task
  immediately — status polls, sliders, and the rest of the dashboard stay
  live during preview. The worker runs at priority 3 with an unconditional
  ≥20 ms `vTaskDelay` on every loop path (the WDT-subscribed IDLE tasks stay
  fed; the pace floor is host-test-pinned), uses an 8 KB internal-RAM stack,
  and exactly one stream runs at a time (second client → 409 Conflict).
- **Stream uptime freezes at stream end** instead of collapsing to 0, so
  "LAST STREAM" throughput/uptime stay truthful (`peek_stream_logic.h`,
  host-tested: `test_peek_stream_logic.cpp` + CI step).
- **Resolution controls fixed end-to-end**: the "320×240" button sent
  framesize 4 (which is 240×240) — now sends QVGA (5); `framesize_name()`
  learned `FRAMESIZE_240X240` so no supported size reads "unknown";
  `/api/peek/resolution` and `/api/peek/init` now *wait* for the stream
  worker to exit (bounded, fail-closed 503 on timeout) instead of a blind
  100–150 ms sleep, and report `stream_stopped` so the UI reconnects the
  preview — the old blind `g_peek_active = true` restore couldn't resurrect
  a finished HTTP response.
- **Preview looks professional**: live metric chips overlaid on the video
  (LIVE / resolution / fps / throughput / uptime, straight from
  `/api/peek/status`), the wall of raw sensor toggles collapsed behind one
  "Advanced sensor tuning" disclosure, throughput rendered human-readable
  (`fmtKbps`, node-tested in the WEBUI_LOGIC block), and resolution status
  falls back to the firmware-reported name for any framesize set via API.
- **Bluetooth settings tab fixes**: settings toggles (auto-advertise, allow
  pairing, long-range) now reload every time the tab is opened instead of
  only at first page load; the enable/disable toggle reverts on failure
  instead of showing a state the radio isn't in; advertise start/stop decides
  its verb from live device status instead of a possibly-stale cache.

### canary-wap: a FULL build without PSRAM no longer compiles (it could never run)

Field evidence from the SD-crash-loop aftermath: a FULL-profile XIAO ESP32-S3
build flashed with the Arduino IDE's default **PSRAM=Disabled** boots, but
internal heap collapses to EMERGENCY (<1 KB free) once WiFi + HTTP + camera +
CSI are up — BLE can't start (the heap guard refuses), SD writes fail until
the card is marked failed, mDNS/`canary.local` times out, dashboard pages
half-render, and the device limps instead of witnessing. The IDE toggle also
silently reverts when the board selection changes, so this misconfiguration
kept recurring.

- `build_config.h` now **refuses to compile** FULL + XIAO_ESP32S3 without
  `BOARD_HAS_PSRAM`, with the exact fix in the error message (Tools > PSRAM >
  "OPI PSRAM" / `--fqbn ...:PSRAM=opi` / `--profile xiao_sense`). Experts can
  define `SECURACV_ALLOW_NO_PSRAM` to bypass.
- Every CI/release Arduino build now compiles with `PSRAM=opi` — the bare
  board FQBN used before defaults to PSRAM=Disabled, so CI was validating
  exactly the broken-in-the-field configuration, **and the release workflow
  was shipping OTA binaries built without PSRAM**. Fixed in `firmware.yml`,
  `csi_module_disable_matrix.yml`, and `firmware-release.yml`; where a job
  overrides `build.extra_flags` (which replaces the board expansion carrying
  `-DBOARD_HAS_PSRAM`), the define is re-added explicitly. The guard itself
  is gated on `ESP_PLATFORM` so host g++ test builds that include
  `build_config.h` are exempt.

### canary-wap: a slow or wedged SD card can no longer crash-loop the device

`SD.begin()` is a chain of yield-free CPU spin loops in the SPI SD driver
(500–1000 ms card waits, ×3 retries, across two mount speeds plus FAT sector
reads) with **no overall deadline**. It ran directly on the watchdog-subscribed
loop task, and `sd_mount_safe`'s "2 s timeout" was checked only *between* the
two blocking attempts — it bounded nothing. A card that holds the bus (wedged,
dying, or incompatible) blew the 8 s panic watchdog mid-mount and rebooted the
device. Worse, the loop's periodic SD recheck re-ran the same blocking mount
**even in safe mode** at ~38 s — before safe mode's 60 s recovery window — so
safe mode itself crash-looped forever (observed in the field: `consecutive
crash count 7/3…` climbing, AP flapping every ~40 s, captive portal blank).

Fixes, layered:

- **All blocking mount work moved to a dedicated worker task** at
  `tskIDLE_PRIORITY` (both cores' IDLE tasks are watchdog-subscribed and the
  SD driver never yields — at any higher priority a stuck mount would just
  move the panic to IDLE0/IDLE1). The loop task polls a state byte, feeding
  the watchdog, up to a 4 s budget; a result that lands later is adopted by a
  subsequent loop pass instead of being lost. Only the loop task writes
  `g_hw`; the worker hands back a private result (single-writer model kept,
  no 64-bit tearing for httpd readers). Nothing ever cancels a running mount
  (`SD.end()` under the worker would be use-after-free).
- **Safe mode now truly skips SD**: the periodic recheck honors the same
  contract as boot (pure host-tested decision table, `sd_mount_logic.h`).
- **Watchdog fed between setup Phase-3 steps** (camera → SD → audio →
  network): they used to share ONE unfed 8 s budget from watchdog-arm to the
  first loop pass, which is how camera-init seconds plus a slow card panicked
  the very first boots after flashing.
- **Late/hot-plug mounts get provisioned**: `/WITNESS`, `/HEALTH`, `/CHAIN`,
  `/EXPORT` and the CSI event log are created on the mount transition in
  loop(), so a card that mounts after the boot budget still gets its layout.
- **GPIO21 hazard guarded**: on the XIAO ESP32-S3 the user LED *is* the SD
  chip-select pin (`LED_BUILTIN == GPIO21 == SD_CS`). LED writes (provisioning
  blink, visual chirps) are skipped while a mount is in flight so they can't
  glitch CS mid-transaction on the worker.

### canary-wap: BLE init no longer boot-loops a low-memory build

A build with PSRAM disabled (Arduino IDE default is easy to miss) boots with
only ~127 KB of internal heap. Once the WiFi AP and HTTP server are up, no
contiguous block remains for the BLE controller's ~30 KB init allocation, so
`NimBLEDevice::init()` fails — but the controller *asserts and panics* rather
than returning an error, tripping the interrupt watchdog. The device then
boot-loops on `BLE_INIT: Malloc failed`, and because the crash is below the
app, even safe mode can't recover it.

Every `NimBLEDevice::init()` call site (the pairing channel, BLE discovery, and
the CSI BLE Scout — the last of which ran even in safe mode) now checks the
largest free internal block first and skips the stack, leaving the radio off,
when there isn't enough room. BLE degrades to "off" instead of bricking, so the
device always comes up as a reachable AP + dashboard. The threshold and
decision are a pure, host-tested predicate (`bt_defaults::init_has_headroom`);
the heap read is thin glue (`ble_heap_guard.h`). Enabling PSRAM (Tools > PSRAM
> "OPI PSRAM") remains the real fix — this just makes a mis-set build fail safe
instead of unrecoverable.

### canary-wap provisioning: BLE scan no longer starves the SoftAP join

Joining the device's setup Wi-Fi was intermittently failing — the phone's
"enter password" sheet looping instead of associating. Root cause: BLE
Discovery's Nearby scanner runs a 5-second, ~99%-duty **active** scan
(window 99 / interval 100) pinned to the shared WiFi/BLE core, and the first
burst fired at boot. When a phone's WPA2 4-way handshake to the provisioning
SoftAP overlapped a burst, the handshake frames were starved and the join
failed; catch a gap and it worked — hence the flaky loop. The firmware itself
already rates AP + STA + BLE as unstable and drops the AP once the STA is up to
escape it, but during provisioning the AP has to stay up.

Fix: BLE Discovery still initializes at boot, but its radio activity (Opera
advertising, Nearby active scanning, boot chirp) is now **deferred out of the
join window** — brought up once the management SoftAP has actually been torn
down (the firmware keeps it up for a grace window after the STA gets an IP so
the phone can read the success card, *then* drops it to the stable STA+BLE
combo, so gating on AP-down rather than mere `WL_CONNECTED` also keeps the scan
out of that protected handoff). In AP-only standalone mode — where the AP is
permanent — it starts after a short settle so the operator's first association
lands cleanly, and a normal device whose home Wi-Fi never comes up starts after
a 5-minute max-hold fallback rather than staying disabled forever. BLE stays on
by default; it just doesn't transmit while a phone is mid-join. The decision is
a pure, wrap-safe predicate (`provisioning_logic::ble_discovery_start_due`) with
host-test coverage, and the self-test reports the pre-start window honestly as
"Radio up · all features idle" (SKIP, non-gating).

### canary-sense witness signing: Ed25519 events, hash chain, verified-green in HA

Completes the canary-sense design doc's Phase 2 trust items, reusing the
signing surface field-proven in the recent canary-wap PRs:

- **Device identity**: Ed25519 keypair generated from the hardware RNG on
  first boot, NVS-persisted (`securacv/privkey` — the wap storage
  contract), fingerprint via the wap `sha256_domain` formula. The signer
  itself is now a shared module (`firmware/common/identity/
  device_signature.{h,cpp}`, host-tested) with a new v1 `sense` canonical
  kind alongside the locked chain/event/counts formats.
- **Every witnessed transition advances a domain-separated SHA-256 hash
  chain** (`securacv:fw:chain:v1`, genesis bound to the device key),
  NVS-persisted — offline gaps show up as seq/length jumps, never lost
  tamper evidence. Events publish with `v`/`alg`/`fp`/`sig` over the
  `sense` canonical; the retained `chain` topic reuses canary-wap's exact
  wire schema, and the retained `health` topic carries `public_key` so
  Home Assistant TOFU-pins the device with its existing subscription.
- **HA integration verifies radar events**: `signature.py` gains the
  `sense` canonical + `verify_sense_event`, and the events handler
  dispatches by payload dialect (CSI vs radar) so each verifies against
  its own canonical.
- **Fingerprint derivation bug fixed (wap ⇄ HA)**: firmware fingerprints
  are `SHA256(domain || 0x00 || pubkey)` but HA's
  `fingerprint_from_pubkey_hex` omitted the NUL separator, so every
  pinned device rendered "Fingerprint changed without rotation" instead
  of the green badge. HA now matches the deployed firmware byte-for-byte
  and heals previously stored pins on load.
- **Task watchdog wired on canary-sense** (IDF5 `esp_task_wdt_reconfigure`,
  the canary-wap pattern; 30 s to clear the bounded broker-connect block).
- Host tests lock the canonical bytes + b64url encoding
  (`firmware/tests_host/test_device_signature_common.cpp`); HA pytest
  covers sense-event verify (happy path, missing fields, tampered
  payload) and the fingerprint healing migration.

### canary-wap admin console: the last dead controls now work

Follow-up to the panel revival — the three controls that were honestly
labeled "not available on this build" are now functional:

- **RF Presence tab**: `rf_presence` is wired up — `init()` in setup
  (privacy-preserving session/token state; no radio started), `update()`
  in loop, and the seven `/api/rf/*` routes registered behind the
  Bearer/session `auth_gated` trampoline the module's header mandated.
  Sensing stays opt-in (enable from the tab, persisted in NVS); BLE already
  feeds the fusion scorer. The v0 scoring FSM is unchanged.
- **Storage cleanup** ("Clean up old export bundles"): a real, auth-gated
  `POST /api/logs/rotate` trims `/sd/EXPORT` (witness-export bundles that
  otherwise accumulate unbounded) to the newest 20 via the tested
  count-based `datamgmt::rotate_dir`. It never touches `/sd/WITNESS` or
  `/sd/CHAIN` — the sealed evidence is untouchable (Invariant IV).
- **Device "Save Configuration"**: record interval, time bucket, and log
  level are now real NVS-persisted runtime settings via `POST /api/config`.
  The time bucket coarsens event timing (Invariant III) and is clamped so
  it can only be widened past its 5000 ms floor, never narrowed — privacy
  is monotonic. The log threshold is clamped to ≤ WARNING so ERROR/CRITICAL
  are always stored.

All clamps live in Arduino-free logic headers with host tests
(`config_logic.h`), the route-security/budget CI guards extend to the new
routes, and `web_assets_gz.h` is regenerated.

### canary-sense Phase 2: the mmWave radar witness publishes

- **Full network stack on the MR60BHA2 kit** (XIAO ESP32-C6), mirroring
  canary-vision: NVS-backed runtime config (OTA-safe identity/credentials),
  supervised WiFi STA, MQTT with LWT + Home Assistant discovery, heap-health
  diagnostics, and the shared signed pull-OTA engine with an HA `update`
  entity + auto-update switch. Presence-only and wellbeing flavors are
  distinct OTA products with separate signed manifests, so images can never
  cross-install between privacy surfaces.
- **Privacy chokepoint enforced at the publish layer**: events carry only
  `presence_detected` / `presence_cleared` / `occupancy_changed` with the
  coarse vocabulary (presence state, 0/1/2+ occupant bucket, near/mid/far
  range band) and a 10-minute-coarsened uptime bucket instead of a precise
  timestamp (metadata minimization; `seq` preserves ordering). Raw distance
  and vitals never leave the device via events;
  wellbeing builds publish the P0 breathing lock and P1-gated BPM numerics
  on the state channel only, suppressed unless exactly one target is present.
- **HA entity set** per the design doc: presence, occupants, range band,
  radar-link problem sensor, frame-error counter, BH1750 illuminance
  (new minimal vendored driver in `firmware/common/sensors/bh1750/`),
  uptime, RSSI + free-heap diagnostics, firmware update card.
- **CI + release wiring**: the wellbeing env joins the build matrix with a
  secrets pre-step and an OTA-slot size guard; `firmware-release.yml` now
  builds, signs, verifies, and publishes `canary-sense` +
  `canary-sense-wellbeing` binaries and manifests.

### canary-vision robustness parity with the ESP32-S3 tree

- **WiFi auto-reconnect with exponential backoff** (2 s → 30 s cap) replaces
  the reconnect-or-hang loop; a sustained 5-minute outage reboots as the
  recovery of last resort, and the blocking MQTT reconnect now defers to the
  WiFi supervisor instead of spinning while the link is down.
- **WiFi power policy**: optional modem sleep + TX power cap (config.h).
- **Heap monitor with 3-level degradation** (S3 thresholds + hysteresis):
  inference cadence stretches 2×/5× under critical/emergency pressure;
  heap + degradation level + RSSI ride the status heartbeat and surface as
  HA diagnostic entities.
- **Dev secrets actually compile in again**: the `-I secrets` include path
  lived in a project-level `[env]` block that PlatformIO overrides, so a
  user's `secrets/secrets.h` silently fell back to the CI stub; the path now
  lives in the effective env definitions and `runtime_config` accepts both
  include spellings (canary-sense inherits the fixed pattern).

### canary-wap dashboard/settings: revive the whole panel

- **The settings panel is functional again.** On the default Arduino-IDE
  build the active HTTP server budgeted 123 URI-handler slots but registered
  154; esp_http_server silently drops everything past the budget, so the
  last 31 routes 404'd for every client — the entire Presence tab, all
  `/api/audible-chirp*` (speaker), every `/api/chirp/*`, the three
  `/api/ble` routes, and Bluetooth "Clear All". The handler table is now
  itemized per feature flag and sized to fit, guarded by a CI check
  (`check_route_budget.py`) that emulates the preprocessor for FULL/S3,
  DEV/S3 and FULL/C3.
- **Correct credentials stop getting locked out.** A missing `Authorization`
  header was counted as a brute-force failure, so one dashboard tab left
  open after its session cookie expired polled the device into a permanent
  429 that rejected even a correct pasted token. Credential-less requests no
  longer feed the lockout; real token guesses still do.
- **Bluetooth is enabled by default** so the pairing channel (offline
  console, BLE Wi-Fi provisioning, OTA, log/witness export) is reachable out
  of the box. Pairing still requires an on-device PIN confirmation — the
  radio is on, not open.
- **Honest self-test.** The pre-flight health check no longer hard-FAILs
  Bluetooth during the boot window or in recovery/safe mode (SKIP, with a
  reason), distinguishes a Wi-Fi link-drop from a radio-off, counts SKIP
  rows in its summary, and surfaces `safe_mode` so a recovering-but-healthy
  device stops reading as broken. A "Device self-test" card in Settings
  re-runs the same checks on demand.
- **Fixed controls:** BLE Discovery Alert/Heartbeat now POST to
  `/api/ble/chirp/send` (not the community handler that 400s); the camera
  peek stream drops the bogus `token=null` and bounds its retry loop;
  Settings Wi-Fi "Connect"/standalone accept an admin credential (session
  cookie / Bearer) as an alternative to the wizard's pair token, so they
  work post-setup; the Community Chirp toggle is wired
  (`chirp_channel::init`/`update`); the dead "Breath sound" switch is gone;
  and dashboard `localStorage` access is guarded so a cookie-blocking
  browser degrades gracefully instead of killing every binding.
- **Security:** the three `/api/ble` routes (previously unauthenticated,
  reachable only once the budget fix resurrected them) and the `qr-scan`
  read/cancel endpoints are now gated; a CI check
  (`check_route_security.py`) asserts every registered route is credential-
  gated or on a documented public allowlist.
- **Honest-labeled** the controls fronting subsystems not wired into this
  build (RF signal presence, SD log rotation, runtime record/bucket config —
  the last touches time-coarsening, Invariant III) instead of firing
  requests that 404. Documented as follow-up feature work.
- Docs: the Arduino README and `sketch.yaml` now correctly list
  `NimBLE-Arduino` (2.x) as a required separate library.

### API hardening: per-IP rate limit + at-rest encryption pinned

- **Event API rate limiting**: every endpoint except `/health` is now capped
  per client IP (fixed one-minute window, default 120/min, 429 with
  `retry_after`). Applies before token validation, so neither a tokenless
  hammer nor a leaked capability token can drive the single-threaded API
  (`POST /verify` walks the whole sealed log). Configure via
  `[api] rate_limit_per_minute` or `WITNESS_API_RATE_LIMIT_PER_MINUTE`
  (0 disables); complements the existing auth-failure lockout.
- **At-rest encryption documented and pinned**: the kernel database has
  always been SQLCipher-encrypted with a seed-derived key — an early audit
  note claiming "unencrypted by default" was wrong. Now stated in
  SECURITY_MODEL.md and docs/why_secure.md, and pinned by regression tests
  (kernel DBs are never plaintext SQLite; opening without the key fails).

### canary-wap first-run wizard: truthful joins, standalone mode, calm portal

- **A successful WiFi join no longer looks like a failure.** Joining a home
  network on a channel other than the SoftAP's dragged the single radio —
  and the setup network — to that channel, kicking the provisioning phone;
  the AP was then torn down 8 s after connect, so the wizard timed out and
  reported "Couldn't connect" on a join that succeeded. The AP now survives
  120 s after connect (long enough to re-associate and see the success
  card), wizard activity resets the 15-minute setup window instead of the
  device rebooting mid-setup, and the timeout copy explains the network
  handoff honestly.
- **Standalone (AP-only) mode**: "Use without home WiFi" in the wizard.
  The device completes setup and lives permanently on its own
  `SecuraCV-XXXX` network (`canary.local` dashboard, captive DNS stays up,
  the AP is never torn down, no STA join attempts). Persisted via
  `wifi_ap_only` in NVS; saving real credentials later exits the mode.
  New pairing-token-gated `POST /api/wifi/ap-only`; `/api/wifi` reports
  `ap_only`.
- **Stale setup links self-heal**: the wizard's pairing token (RAM-backed,
  10-minute TTL, wiped by reboot) is silently re-issued via the new
  setup-only `GET /api/wifi/pair-token` and the credentials resent once —
  "This setup link has expired" now only appears when the wizard truly
  can't recover. Same Host-gated posture as the `/` redirect that mints
  the original token.
- **Scan without kicking the phone off**: the device pre-scans at boot
  (before anything joins the AP) and serves a cached list (5-min TTL,
  `cached`/`age_s` in the response); only an explicit "Scan again" sweeps
  the radio under a live client — the sweep is what used to drop the
  wizard's scan fetch ("Scan failed: Load failed").
- **Calm capability note**: the red "insecure origin / Web Bluetooth"
  banner is now an informational note ("WiFi setup and the dashboard work
  fine without it") and is gone entirely — along with the Bluefy footer —
  inside the WiFi wizard, where Web Bluetooth is irrelevant. Real errors
  still render red.
- **Password field hygiene**: the typed WiFi password is wiped on
  page-hide/tab-background (the app never stored it — Safari's page cache
  restored the form value; verified no credential ever touches
  localStorage/sessionStorage).
- **Compatibility**: no wire or NVS breakage — new NVS key and endpoints
  only; provisioned devices behave as before apart from the longer
  post-join AP grace.

### Fail-closed configuration and verification hardening

- **Unknown config keys are now parse errors.** A misspelled key in
  `adapter_host.toml` or `witness.toml`/`witness_config.json` used to fall
  back silently to the permissive default — `auth_toke` left the webhook
  listener unauthenticated, a `tls_key` typo fell back to plaintext, a
  `cameras` typo processed every camera, and `sensitve` under `[zones]`
  removed the sensitive-zone policy. All config-file structs now reject
  unknown keys with an error naming the key, at startup and on SIGHUP
  reload (reload keeps the running config).
- **Confidence-gated routes require stated confidence** (`mqtt_sensor` +
  webhook shared routing): a payload that omits or misspells `confidence`
  no longer sails past a `min_confidence` floor as 1.0. Routes without a
  floor keep accepting bare trigger payloads unchanged.
- **canary-vision config API fails closed**: `PUT /api/v1/config` (and
  `/:section`) rejects unknown sections/keys with 400 `invalid_config`
  instead of merging typo'd keys while the real setting kept its default
  (e.g. `auto_purge_hour` leaving auto-purge on the longer window).
- **Wizard POST hardening**: a malformed or negative `Content-Length` is a
  clean 400 (previously an unhandled traceback), and bodies over 1 MiB are
  refused with 413.
- **`verify_pipeline.sh` can no longer false-pass**: the live-stack smoke
  check now excludes retained MQTT messages, publishes a nonce-tagged
  event and requires the bridge to ingest *that* event, and requires
  `witness.db` to have been written during the run — stale logs, retained
  payloads, and schema-only databases all fail. A docker-shim regression
  suite replays the old false-pass scenarios in CI.
- New export-boundary absence tests pin that the Frigate object id
  (embedded precise timestamp) and below-floor confidence values never
  appear in a serialized export.
- **Compatibility**: configs carrying stray or misspelled keys now refuse
  to load, and sensors that never publish `confidence` no longer pass
  confidence-gated routes — both deliberate fail-closed breaks; correct
  existing configs and payloads are unaffected.

### Export & diagnosis follow-ups: one-click download, scheduling, inspectors, break-glass UX

- **One-click "Download my events"**: token-gated `GET /export/bundle` on the
  event API returns the full signed ExportBundle as a browser download, with
  optional `?last=24h` / `?start=&end=` windows (bucket-aligned; recorded on
  the `api`-labeled receipt). Surfaced as a window-picker download button in
  the HA add-on ingress panel (token never reaches the browser). The add-on
  proxy fails closed: an unknown or misspelled window parameter is rejected
  with 400 `bad_window` instead of silently widening the export to
  everything retained.
- **Scheduled exports**: `export_events --output-dir DIR --keep N` writes
  rotating `securacv-events-<bucket>.json` files; docs/scheduled_exports.md
  ships systemd timer/cron units and the add-on curl recipe.
  `ExportWindow::aligned`/`::last` + `parse_duration_s` move into the library
  (one alignment rule for CLI and API).
- **Lineage & checkpoint inspectors**: `log_verify --lineage` /
  `--checkpoints` walk everything instead of failing closed — per-epoch
  valid/invalid/unverifiable with reasons, per-checkpoint signer resolution
  against the genesis-anchored lineage, signature checks, and
  timestamp/cutoff regressions, with plain-language guidance (`--json`
  supported). The inspectors never panic on truncated/tampered key blobs —
  they exist to diagnose exactly that database.
- **Break-glass console UX**: shareable trustee signing links
  (`#sign&hash=…` — signer-only page, no token, no server calls), live
  auto-refreshing quorum status with per-trustee pills and a progress bar;
  the console resumes live polling by itself when it connects to an
  already-open request. No backend changes; operator guide now documents
  the console.

### Canary Vision: runtime detection settings (no-rebuild model swaps)

- **Runtime detection config** (`firmware/projects/canary-vision`): the
  person class index, score threshold, lost timeout, and dwell-start window
  are now NVS-backed and exposed as Home Assistant **number entities**
  (device Configuration section, MQTT Discovery). Swapping the SSCMA model
  on the Grove Vision AI V2 via SenseCraft no longer requires a firmware
  rebuild — adjust the class index from HA. Compiled constants in
  `config.h` seed the first boot only; values persist across reboots and
  OTA installs. New retained topic `securacv/<id>/cfg/state` mirrors the
  live values; `securacv/<id>/cfg/{target,score,lost,dwell}/set` accept
  writes (clamped, junk-rejected).
- **Boot banner** now reports the live (NVS) detection settings and the
  actual host board name (was hardcoded to the DevKit).
- **Unified firmware version bumped to 2.2.0** across canary, canary-wap,
  and canary-vision — the first release train that publishes the per-board
  canary-vision OTA images (`-xiao-c3`, `-xiao-s3`) introduced in #786.
  Tag `fw-v2.2.0` to ship.

### Export UX, owner self-export, and verification diagnosis

- **Owner self-export** (`export_events --self-export`): export the
  privacy-filtered event artifact with the device key seed alone — no trustee
  quorum. A signed, chained receipt is still always written; receipts now
  carry an optional `auth_mode` (`break_glass` / `self_export` / `api`) so
  owner-authorized and quorum disclosures stay distinguishable. Sealed-vault
  evidence and unsealing remain quorum-only.
- **Export time windows**: `--last 24h` / `--start`/`--end` on
  `export_events`. Windows are aligned outward to 600 s bucket boundaries,
  filtered on true (pre-jitter) buckets, and recorded on the signed receipt
  (optional `window` field).
- **Actionable verification failures**: `log_verify` and `envelope_verify`
  now print a plain-language diagnosis (where the chain broke, what kind of
  check failed, likely causes, next steps); timeline warnings carry one-line
  hints. `log_verify` gains `--json`. `VerifyReport` (also `POST /verify`)
  gains an additive structured `failure` object; the `error` string is
  unchanged.
- **Offline viewer**: shows where the chain broke with the same guidance,
  explains each note inline, and adds a "What verification proves — and what
  it can't" panel. New `docs/why_secure.md` plain-language explainer.
- **Compatibility**: legacy bundles (receipts without `auth_mode`/`window`)
  verify forever — pinned by a new `valid_envelope_legacy.json` fixture in
  both the Rust and JS suites. The one caveat: verifiers older than this
  release reject *new* bundles whose receipts carry the new fields (their
  receipt re-serialization drops unknown fields); verify new bundles with a
  current viewer/`envelope_verify`.

### RFC 3161 trusted timestamping (chain anchors)

- **New `log_anchor` CLI** anchors the witness chain head (or any export
  digest) at a public Time Stamping Authority: independent third-party
  proof of *when* the chain existed, removing the device clock — and even
  the device key — from the trust base for back-dating attacks. Online
  flow behind `--features tsa`; a query/import offline flow works
  air-gapped with no special build. `log_anchor verify` checks imprint
  consistency and chain membership in-tree and delegates the CMS
  countersignature to an independent implementation (`openssl ts
  -verify`). Anchors live in a new additive `tsa_anchors` table; the
  sealed-log schema and existing verifiers are untouched. Requests carry
  only a 32-byte digest plus a random nonce, and nothing in witnessd ever
  calls a TSA on its own (anchoring is operator/cron-initiated). See
  docs/timestamping.md.

### Physical tamper sealed into the witness chain

- **New `EventType::TamperDetected` / `ClaimKind::TamperDetected`**
  (`tamper_detected` in routes): tampering with the witnessing device itself
  — enclosure opened, camera covered/blinded, thermal-attack temp drift.
  Previously the Canary firmware signed tamper into its device-side chain
  but the kernel's sealed log never saw it (and the dedicated
  `securacv/<id>/tamper` MQTT topic had **zero publishers**).
- **Firmware**: the sensing witness callback now queues tamper alerts and
  the main loop publishes `{"state":"on","confidence":0..1,"kind":...}` on
  `securacv/<id>/tamper` (pending-flag pattern; re-arms on publish failure).
- **Adapter path**: `mqtt_sensor` / `webhook` allowlists include the new
  kind; example route in `adapter_host.example.toml`. Everything flows
  through the existing `Kernel::append_event_checked` gates — no new kernel
  surface.
- Specs updated (normative): `spec/event_contract.md` §11 vocabulary,
  `spec/sensor_adapter_contract_v0.md` §6 mapping.
- Compatibility: same statement as the heartbeat/lifecycle records — old
  DBs verify unchanged; old `log_verify` accepts new DBs; old
  `export_events` binaries error on records carrying the new event type.

### Logging & witnessd chain audit remediation

- **New sealed record types** `heartbeat` and `lifecycle`
  (spec/event_contract.md §12). One heartbeat per 10-minute bucket anchors
  the chain tail — previously, deleting the newest N records passed
  `log_verify` because checkpoints were only written when retention pruned.
  Lifecycle records seal daemon `start`/`shutdown_clean`; a boot that finds
  a trailing `start` seals a `PowerLoss` failure record (unclean-shutdown
  proxy). **Compatibility**: existing databases verify and read unchanged;
  databases written by the new witnessd still verify under older
  `log_verify` binaries (chain checks are payload-agnostic), but older
  `export_events` binaries will error on the new record types — upgrade
  tooling together with witnessd.
- **witnessd no longer dies silently on hardware faults**: a camera
  stall/disconnect is supervised (one sealed `GapMissingData` per outage
  after `ingest.failure_threshold_s`, reconnect with backoff, recovery
  visible in the next heartbeat) instead of crashing the daemon with no
  record; retention-enforcement and time-bucket errors are likewise
  witnessed instead of fatal. SIGINT/SIGTERM now seal a clean-shutdown
  record before exit.
- **Previously dead failure types now emitted**: `ClockSkew`
  (monotonic-vs-wallclock drift / bucket regression), `PowerLoss`
  (unclean-shutdown detection), `StorageFull` (free-space preflight,
  distinct from `StorageWriteFailed`). `SensorDisagreement` and
  `FirmwareIntegrity` remain explicitly deferred — no consensus/attestation
  infrastructure exists (docs/failure_semantics.md has the full mapping).
- **`log_verify` timeline audit**: warns on stale tails (possible tail
  truncation), missing heartbeat buckets, `created_at` regressions
  (softened when a ClockSkew record covers the jump), and back-dated or
  future-dated checkpoints. New `--strict` flag turns warnings into a
  non-zero exit. `VerifyReport` gains an additive `warnings` field (omitted
  when empty).
- **Operational log rate fixed**: the 5-second INFO health dump (~35k
  lines/day) is now transition-based — one WARN when ingest goes unhealthy,
  one INFO on recovery, a single key=value summary per
  `health.log_interval_s` (default 60s), detailed dumps at DEBUG. New
  docs/logging.md documents levels, volume, and RUST_LOG usage.
- New config sections (all defaulted, existing configs unchanged):
  `[health]` heartbeat/log_interval_s, `[storage]`
  min_free_mb/check_interval_s, `[clock]` skew_tolerance_s, and
  `[ingest]` failure_threshold_s/reconnect_backoff_max_s.

### Frigate zero-friction release (add-on 0.6.0)

- **Zero-config HA add-on**: the broker is auto-discovered from the
  Supervisor MQTT service (`services: mqtt:want`; explicit options still
  win), the device key is auto-generated and persisted (0600) when absent,
  `mqtt_publish.enabled` defaults to `true`, and the ingress Web UI is now
  a persistent status panel (chain badge, 24h digest, Verify Now, Lovelace
  dashboard generator) instead of a first-run-only wizard.
- **Docker sidecar** (`docker/sidecar/`, published as
  `ghcr.io/kmay89/securacv-sidecar`): one container (witness_api +
  frigate_bridge + event_mqtt_bridge + log_verify) for standalone Frigate
  users; only `FRIGATE_MQTT_HOST` is required. Includes an
  `entrypoint.sh doctor` diagnostic (broker reachability/auth, live
  Frigate traffic, sealed-log verification) and quickstart compose files.
  Repairs `integrations/ha_frigate_mqtt/docker-compose.yml`, which built a
  nonexistent Dockerfile.
- **One-click verification**: new `POST /verify` on the event API runs the
  full sealed-log check via the shared `verify_runner` (also the new core
  of `log_verify`); exposed in HA as `button.pwk_verify_now` +
  `binary_sensor.pwk_chain_problem`, with scheduled re-verification
  (`verify_interval_hours`, default 24).
- **Daily digest**: new `GET /digest` (rolling 24h, per-zone counts, 6-hour
  day periods, cached verify outcome — built solely from the
  privacy-filtered export path); exposed as `sensor.pwk_daily_digest` and
  deliverable via the new `docs/blueprints/securacv_daily_digest.yaml`
  blueprint (no entity-ID surgery).
- **Frigate reviews + topic prefix**: `frigate_bridge` can subscribe to
  `<prefix>/events` and `<prefix>/reviews` (`--frigate-topic-prefix`,
  `--enable-reviews`); the reviews parser now handles the real Frigate
  0.14+ before/after schema (the previous flat shape never matched live
  payloads). Compatibility statement: tested against the Frigate 0.14–0.17
  MQTT schema, with verbatim 0.17 fixtures in the test suite.
- **Fixes**: `event_mqtt_bridge` daemon no longer 401s after the
  10-minute capability-token rotation (token file re-read per request);
  `frigate_bridge` honors retention via `--retention-secs` instead of a
  hardcoded 7 days; `log_verify --device-key-seed` (env `DEVICE_KEY_SEED`)
  derives both the SQLCipher and verifying keys for operator-friendly
  verification of bridge-produced logs.

## [1.0.0] - Unreleased

### What v1.0 means

v1.0 means **everything documented works end-to-end**: every feature described in
the README/docs runs end-to-end, the install path succeeds on the first try, and
the test suite passes cleanly. This is the project's canonical definition of v1
(see `v1-roadmap.md`). It is **not** feature-complete — see "Explicitly deferred"
below — but nothing documented is allowed to be aspirational at the v1 tag.

### What's included

- **Privacy Witness Kernel** (Rust): hash-chained, Ed25519-signed append-only
  event log with break-glass N-of-M quorum access, vault sealing, event
  contract enforcement, module sandboxing (seccomp on Linux).
- **Vault sealing status is explicit at startup** (F-05): vault frame sealing is opt-in (it runs
  only when `BREAK_GLASS_SEAL_TOKEN` supplies a valid break-glass token). `witnessd` now logs whether
  sealing is ENABLED (with the crypto mode) or DISABLED at startup — and when disabled it states that
  boundary events are still signed/logged but no frame is sealed into the vault, plus how to enable
  it — so an operator is never silently led to believe evidence is being sealed when it is not.
- **DB key decoupled from the signing key** (F-04 / Stream B2 prerequisite): set
  `SECURACV_DB_KEY_SEED` and the SQLCipher key is derived from that independent secret
  (`resolve_db_encryption_key`) instead of the Ed25519 signing key, so the database key no longer
  pins the device identity — the storage-layer prerequisite for signing-key rotation.
  `rekey_database_file()` rotates the DB key itself in place (`PRAGMA rekey`). Backward compatible —
  without the env var, the legacy signing-key derivation is byte-identical, so existing databases
  open unchanged. (Full signing-key rotation additionally needs `device_metadata` identity-rotation
  support, which `Kernel::open` still pins; tracked as remaining Stream B2 work.) See
  [`docs/db_key_rotation.md`](docs/db_key_rotation.md).
- **CLI binaries**: 9 core — witnessd, log_verify, break_glass, export_events,
  export_verify, frigate_bridge, event_mqtt_bridge, witness_api,
  grove_vision2_ingest — plus the `adapter_host` daemon and `envelope_verify`,
  and the `demo` / `tamper_demo` / `ingest_run` / `detect_eval` helpers (15 total in `src/bin/`).
- **Sensor Adapter framework** (`src/adapter/`): an open, vendor-neutral interface that
  generalizes the `frigate_bridge` pattern so any source (acoustic/impulse, PIR/contact,
  presence, generic MQTT/webhook sensors, Frigate) can feed coarse, privacy-preserving claims
  into the same `append_event_checked` choke point — broad integration with no vendor lock-in and
  no new privilege. Includes the `adapter_host` binary (config-driven, one daemon, many adapters),
  Frigate + generic MQTT reference adapters, and the normative
  `spec/sensor_adapter_contract_v0.md` / `spec/witness_mesh_os_v0.md`. Expanded the event
  vocabulary with `acoustic_impulse_in_zone`, `presence_in_restricted_zone`,
  `vehicle_presence_after_hours`, `contact_state_change`, and `object_removed_from_zone`.
  - **Webhook ingress adapter** (`adapter-webhook`): a std-only HTTP `POST` listener so any
    device/script can register a sensor with a single `curl` — no MQTT broker required.
  - **Optional seccomp sandboxing** (`adapter-sandbox`, `with_sandbox(true)`): adapters can parse
    untrusted payloads inside the kernel's forked seccomp sandbox, upgrading the adapter audit
    boundary toward a security boundary for the parse step.
  - **Home Assistant surfacing**: the new claim types render in the "Last Event" sensor with
    friendly labels and per-type icons (`EVENT_TYPE_METADATA` in `const.py`).
  - **Webhook authentication + rate limiting + worker pool**: the webhook ingress (the one
    untrusted, network-facing surface) supports constant-time `Authorization: Bearer` or
    HMAC-SHA256 body-signature auth, per-path token-bucket rate limiting (`429`), and a bounded
    connection worker pool (`503` when saturated) that ends the unbounded per-connection thread
    spawn.
  - **BLE presence adapter** (`adapter-ble-presence`): turns ESPresense-style room-presence MQTT
    feeds into coarse presence claims, deliberately discarding device identity.
  - **Meshtastic LoRa-mesh adapter** (`adapter-meshtastic`): turns Meshtastic Detection Sensor
    Module nodes (PIR/contact/acoustic on a GPIO, alerting over LoRa) into kilometre-scale,
    off-grid witness sources via a gateway node's MQTT JSON uplink. Node ids are local routing
    keys only; positions, precise timestamps, RSSI/SNR, and alert text are never retained
    (export-scrub asserted in `tests/adapter_meshtastic.rs`). Inbound only; the outbound and
    LoRa-transport directions are specified in `docs/meshtastic_integration.md`.
  - **Adapter observability**: per-adapter counters (polls/emitted/sealed/filtered/rejected +
    last-seal time) on the host, a periodic stats log, and an optional read-only `/stats` +
    `/healthz` HTTP endpoint (`stats_addr`) — operational counts only, never event content.
  - **Webhook TLS** (`adapter-webhook-tls`): optional rustls TLS on the webhook listener
    (`tls_cert`/`tls_key`), so bearer tokens aren't sent in clear on non-loopback deployments.
  - **HMAC replay protection**: opt-in `X-Timestamp` + `X-Nonce` bound into the signature
    (`hmac_replay_window_secs`), rejecting replayed or stale signed requests.
  - **Home Assistant native adapter-stats sensor**: configuring an "Adapter Host stats URL" adds a
    diagnostic sensor (per-adapter counters as attributes) via a dedicated coordinator — no
    hand-written YAML needed.
  - **Parser fuzz sweep** (`tests/adapter_parser_fuzz.rs`): seeded, panic-free robustness tests
    over the untrusted webhook/mqtt/BLE/Frigate/Meshtastic parsers.
  - **Webhook mutual TLS**: optional client-certificate auth (`tls_client_ca`) — machine-to-machine
    sensors authenticate by certificate, with no shared secret on the wire.
  - **Prometheus metrics**: the stats endpoint serves `/metrics` (text exposition format) alongside
    JSON `/` and `/healthz`, for Grafana/Alertmanager scraping.
  - **SIGHUP config hot-reload**: `adapter_host` reloads `min_confidence` and each adapter's
    route/room/filter attributes (and webhook paths) live on SIGHUP, without restarting listeners
    or dropping connections; changing an mqtt_sensor's subscribed topic, or adapter topology,
    still requires a restart (and is logged).
- **Home Assistant integration** (HACS): 3 setup modes (MQTT / Kernel HTTP /
  both), MQTT auto-discovery, device PKI trust management (TOFU + manual pin +
  rotation), 5 sensor types, 11 binary sensor types (tamper + transport),
  Ed25519 signature verification, diagnostics, and a bundled **verified-✓
  timeline Lovelace card** (with a pure-YAML fallback for those who prefer
  built-in cards — see `docs/lovelace_timeline.md`).
- **Home Assistant add-on**: first-run setup wizard with preflight checks,
  camera TCP test, Frigate config generation, post-setup health verification,
  two operating modes (Frigate integration, standalone RTSP).
- **Install script**: single `curl | bash` command installs Mosquitto, Frigate,
  integration, add-on, generates device key, deploys automations + dashboard.
- **Firmware** (ESP32): canary-vision (ESP32-C3 + Grove Vision AI V2),
  canary-wap (XIAO ESP32-S3 Sense) — BLE discovery, Chirp community alerts,
  Beacon harm-reduction broadcast, Opera mesh networking, OTA updates.
- **Detection backends**: stub (testing), CPU (background subtraction),
  Tract ONNX (local inference).
- **Frame sources**: RTSP (GStreamer/FFmpeg), V4L2, ESP32 HTTP, local files.
- **Automations**: daily digest, pattern-break alerts, integrity failure alerts.
- **CI**: Rust tests + clippy, firmware builds, HACS/hassfest validation,
  SBOM generation, secrets scanning, CodeQL analysis, release workflow. Two
  real-decode ingest gates: `ingest-ffmpeg` (file → signed log) and
  `ingest-rtsp`, which serves the committed fixture over RTSP (MediaMTX + ffmpeg
  publisher) and drives the real `RtspSource` end-to-end through
  decode → detection → signed events → verify (`tests/rtsp_e2e.rs`). A third
  end-to-end gate, **`frigate-mqtt-e2e`**, covers the Frigate → MQTT path:
  `tests/frigate_mqtt_e2e.rs` drives a `frigate/events` payload through the bridge
  pipeline into a SQLCipher-encrypted sealed log and verifies it with the real
  `log_verify` binary, while the CI job runs the real `frigate_bridge` ingesting a
  message from a live mosquitto broker (`integrations/ha_frigate_mqtt/ci_smoke.sh`).
  `verify_pipeline.sh` was corrected (it queried the encrypted DB with plain
  sqlite3, expected vault envelopes the bridge never creates, and a break-glass
  export bundle nothing generates) and is now an honest manual operator smoke check.
- **Security docs**: the **audit boundary vs security boundary** distinction is now
  stated authoritatively in `docs/security/THREAT_MODEL.md` (*Trust Boundaries*):
  which producer surfaces (`DetectorBackend`, `SensorAdapter`, the `InferenceView`
  handoff) are hand-audited contracts vs. the mechanically enforced security
  boundary (the three fail-closed gates in `Kernel::append_event_checked`). Closes
  the corresponding v1 acceptance item.
- **Firmware privacy hardening (no raw MAC / no precise GPS, all trees)**: the salted, MAC-free
  device pseudonym and GPS coarsening are now shared helpers in `firmware/common/`
  (`identity/device_pseudonym.h`, `gnss/gps_privacy.h`) adopted across every firmware tree.
  `canary-vision` no longer leaks the efuse MAC in its MQTT client ID or boot banner (it shows a
  salted "Hardware ID" instead); `canary/src` routes all operator-facing lat/lon through
  `gps_coarsen_deg()` (3 dp ≈ 110 m); the `canary-wap/src` scaffold no longer reads the MAC. The
  `regression_check.sh` privacy guardrail now **hard-fails** on raw MAC or un-coarsened lat/lon in
  *any* tree (previously a per-tree warning), and both helpers are host-tested
  (`tests_host/test_device_pseudonym_common.cpp`, `test_gps_coarsen.cpp`). Closes the v1 firmware
  invariant gate.

### Explicitly deferred (not in v1.0)

- Multi-camera standalone mode (currently single-camera only in standalone;
  Frigate mode supports multiple cameras via Frigate's own config)
- LoRa transport
- SCQCS audio transport
- CAP gateway interop (specification exists, implementation deferred)
- GPU-accelerated detection
- Tract detection confidence threshold override (hardcoded at 0.5)
- Pre-built Docker images on ghcr.io / Docker Hub

### Known limitations

- v1 e2e pipeline verification (`verify_pipeline.sh`) requires a live
  docker-compose stack with Frigate + Mosquitto — not automated in CI.
- The HA add-on builds from source inside the container, which is slow on
  first install (~5-10 min on Pi 4). Pre-built images are planned for v1.1.
- Standalone RTSP mode processes one camera at a time. For multi-camera,
  use Frigate mode.

## [2.1.0] - 2026-05-27

### Added — Production Feature Plan (Phases 0-6) for ESP32-S3 Canary firmware

Seven-phase plan completing the firmware's production-readiness across both
PlatformIO (canary/) and Arduino WAP (canary-wap/) builds with full parity.

**New PlatformIO libraries:**

- **securacv_power** — Battery ADC (2:1 voltage divider on GPIO 1), 16-point
  LiPo discharge curve for SoC, software inference fallback, charge state
  machine with hysteresis, graceful brownout shutdown, battery health history
  persisted to NVS (charge cycles, voltage extremes, brownout count).
- **securacv_power_policy** — 6-mode runtime state machine (PLUGGED_IN,
  BATTERY_NORMAL, BATTERY_SAVER, LOW_POWER, SHUTDOWN, USB_ONLY). Per-mode
  CPU frequency scaling, WiFi power save, record interval tuning, progressive
  feature gating. Deep sleep cycling in emergency mode.
- **securacv_setup** — First-boot captive portal with DNS hijack, device
  naming, 15-minute timeout. NVS flag persists setup completion.
  - **Stays-connected onboarding**: OS connectivity probes are answered
    per-platform so the phone never flags the AP "no internet" and
    disconnects mid-setup — Apple gets the instruction page (Captive Network
    Assistant sheet), Android gets `204 No Content`, Windows gets the exact
    NCSI bodies. The captive DNS redirector runs for the whole life of the
    always-on AP (not just first boot), so rejoining the management AP after
    provisioning works too. The redirector answers only `A` queries and
    returns NODATA for `AAAA`/`HTTPS`, so `canary.local` resolves promptly on
    Android Chrome; `192.168.4.1` is the always-works fallback. The pure
    response logic is extracted into host-unit-tested headers — the DNS builder
    (`captive_dns.h`) and the per-platform probe policy (`captive_probe.h`,
    driving a single `handle_captive_probe` handler) — both run in CI.
- **securacv_diagnostics** — Heap monitoring (free/min/largest block/PSRAM/
  stack HWM/fragmentation), 3-level automatic feature degradation with 5KB
  hysteresis, SD health tracking (atomic write/error counters, space warnings),
  10-test boot self-test suite (NVS, heap, PSRAM, crypto, SD, WiFi, temp,
  uptime, watchdog, chain).
- **securacv_ble_status** — NimBLE GATT server with standard Battery Service
  (0x180F) and custom SecuraCV service exposing device name, firmware version,
  chain sequence, health score, degradation level, uptime, SD usage over BLE.
- **securacv_data_mgmt** — SD log rotation (witness 500, health 200, auto at
  85% SD usage), chain backup/restore with HMAC-SHA256 integrity (keyed by
  device private key), chain integrity verification (Ed25519 + hash continuity,
  capped at 100 records with watchdog yield), witness record export to /EXPORT/.

**New WAP header-only ports (full parity):**
power_monitor.h, power_policy.h, ble_status_api.h, data_mgmt_api.h, plus
sys_monitor.h enhanced with heap degradation levels and SD health tracking.

**New REST API endpoints (both builds):**
- `GET /api/diagnostics` — full diagnostic snapshot as JSON
- `GET /api/selftest` — re-run self-test suite on demand
- `GET /api/battery/history` — NVS-persisted battery health stats

**New serial commands:** `b` (battery), `p` (power policy), `d` (diagnostics),
`r` (data management).

**New feature flags:** FEATURE_POWER_MONITOR, FEATURE_POWER_POLICY,
FEATURE_SETUP_WIZARD, FEATURE_DIAGNOSTICS, FEATURE_BLE_STATUS,
FEATURE_DATA_MGMT.

### Security hardening

- Chain backup uses HMAC-SHA256 (was CRC-32) — prevents SD-level forgery
- Chain verification capped at 100 records with delay(1) yield — prevents
  watchdog timeout on large directories
- BLE GATT characteristics are read-only (no write/auth bypass possible)
- New REST endpoints (/api/diagnostics, /api/battery/history) auth-gated with
  rate limiting. Note: WAP's /api/selftest is intentionally unauthenticated
  (reachable on the captive-portal AP during setup, by design)
- Power policy rejects manual override to LOW_POWER/SHUTDOWN (anti-blinding)

## [0.5.0] - 2026-05-12

### Added — harm-reduction broadcast layer (Beacon channel) + audit artifacts

- **Beacon channel** (`spec/beacon_channel_v0.md`,
  `firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.{h,cpp}`):
  Smoke-detector-grade neighborhood harm-reduction broadcast layer with
  two-pubkey cryptographic co-signing on every origination, NFPA-72-style
  supervised health state (`Normal / Trouble / Alarm / Supervisory`),
  CAP-aligned wire fields, narrow life-safety-only template set (~13
  templates), daily Ed25519-signed self-test heartbeat with 36 h Trouble
  threshold, append-only chain-hashed audit log as a ring buffer
  NVS-persisted under the flash-encryption gate. Off by default
  (`FEATURE_BEACON_CHANNEL=0`).
- **Beacon REST API** (`beacon_api.h`): Bearer-token-gated surface —
  `/api/beacon`, `/set`, `/pair/*`, `/revoke`, `/originate`, `/cosign`,
  `/cancel`, `/active`, `/audit` (paginated), `/selftest`.
- **COSIGN_REQ/RESP encryption**: X25519 ECDH between paired device
  pubkeys, HKDF-SHA256 domain-separated key derivation
  (`securacv:beacon:cosign:v0`), ChaCha20-Poly1305 AEAD. X25519 keypair
  NVS-persisted across reboots.
- **Distinct Beacon airtime telemetry** in `airtime_governor`.
- **HA MQTT discovery** for both Chirp + Beacon NFPA states.
- **Audible `PATTERN_BEACON`** (1200/1700/2200 Hz ≤600 ms sequential),
  deliberately distinct from any reserved emergency-broadcast tone.
- **CAP gateway interop spec** (`spec/beacon_cap_gateway_v0.md`) —
  specification only; implementation deferred.
- `docs/audit/mesh_and_chirp_audit_v1.md` — full audit of Opera mesh +
  Chirp channel with per-finding traceability.
- `docs/audit/v0.3_closeout.md` — closure summary mapping each finding
  to source location, test, and PR.
- `docs/audit/hardware_verification_checklist.md` — outstanding
  hardware-bound verification recipes for the QA team.
- `docs/research/harm_reduction_prior_art.md` — CAP, IPAWS/WEA/EAS,
  NFPA 72, MUTCD DMS, Hawaii 2018 false-alert, harm-reduction movement,
  Meshtastic / GoTenna prior art.
- Host tests: `test_chirp_protocol_invariants.cpp`,
  `test_chirp_security.cpp`, `test_beacon_origination.cpp`,
  `test_mesh_opera_security.cpp` — per-finding regression coverage.
- CI lints: `scripts/lint_no_impersonation.sh` (reserved phrases /
  tones / colors) and `scripts/lint_cap_mapping.sh` (CAP template
  coverage).

### Changed — Chirp v0.2 hardening (closes audit C1–C17)

- End-to-end Ed25519 signature verification on every received witness /
  ACK / suppress-vote.
- `confirm_count` no longer carried on the wire; receivers track
  confirmations locally as a set of unique confirmer `session_pubkey`s.
- Relayers re-sign with their own session key; original signer's
  pubkey + signature preserved in `signed_origin` envelope so
  downstream receivers verify end-to-end.
- Signed `CHIRP_MSG_SUPPRESS_VOTE` wired end-to-end.
- Priority storage for received chirps — EMERGENCY survives a flood.
- 4 KB / 4-hash Bloom-filter nonce dedup with periodic reset.
- Wall-clock-anchored timestamps; origination refused when SNTP
  unsynced; conservative night-mode when unsynced.
- 5-emoji session display (~1 M distinct).
- `chirp_api.h` REST endpoints Bearer-gated via the standard
  template-trampoline pattern; wired into `canary_wap.ino`.
- Presence requirement also gates ACK origination.
- Per-`session_pubkey` rate limit on incoming witnesses.
- `TPL_AUTH_FEDERAL_PRESENCE` removed; 0x04 slot reserved.
- `PROTOCOL_VERSION` bumped from 0 to 1.

### Changed — Opera mesh v0.2 hardening (closes audit O1–O3)

- Message freshness anchored on per-peer monotonic counter; wall-clock
  TTL retired.
- `opera_secret` NVS persistence requires flash encryption enabled.
- `remove_peer()` now executes a full transactional rekey:
  generate new `opera_secret`, encrypt under each surviving member's
  session key, wait for ACKs, commit on all-ACK or 60 s timeout.

### Security

- Non-impersonation contract CI-enforced. No reserved
  emergency-broadcast phrases, no reserved-tone audio pair, no pure
  red as a primary alert color in any alert/chirp/beacon firmware or
  UI source.
- Beacon `scope = Private` always.
- No PII on the Beacon wire — templates only.
- 10 hardwired Beacon-channel invariants added to `AGENTS.md`.

## [0.4.0] - 2026-02-18

### Added
- **BLE Discovery subsystem** for Canary firmware (Opera/Chirp/Nearby):
  - **Opera**: BLE server advertising with SecuraCV custom GATT service — privacy-safe device
    identifier derived from Ed25519 pubkey hash, read-only status characteristics, writable
    command characteristic
  - **Chirp**: Connectionless BLE broadcast alerts between Canary devices — manufacturer-specific
    advertising data with truncated chain hash, coarsened timestamps, rate-limited (10s minimum)
  - **Nearby**: BLE scanner running on dedicated FreeRTOS task — discovers other Canaries via
    service UUID, tracks RSSI for proximity, thread-safe with mutex-protected shared state
  - New firmware files: `ble_config.h`, `ble_opera.h`, `ble_chirp.h`, `ble_nearby.h`, `ble_manager.h`
  - `FEATURE_BLE` compile flag in `build_config.h` (disabled in MINIMAL/DEV, enabled in FULL)
  - HTTP API endpoints: `GET /api/ble/status`, `GET /api/nearby`, `POST /api/chirp/send`
  - Web UI: BLE Discovery tab in Community panel with signal strength bars, nearby Canary list,
    chirp send buttons
  - NimBLE-Arduino library dependency (lighter than bluedroid, ~60% less RAM)
  - BLE protocol specification: `docs/ble_protocol.md`
  - BLE semantic events added to `spec/event_contract.md`

### Security
- BLE uses NimBLE only (no Bluetooth Classic — smaller binary blob surface)
- Device identity from Ed25519 pubkey hash, not hardware MAC address
- Non-Canary BLE devices counted only, never individually logged (privacy by default)
- All BLE code gated behind feature flag — compiles out completely when disabled
- Graceful degradation: firmware continues if BLE hardware unavailable

## [0.3.1] - 2026-01-21
### Fixed
- `log_verify` now verifies break-glass receipt chain (called from `main`)
- Receipt verification uses `[u8; 32]` device key signature input and supports `--verbose`


All notable changes to the Privacy Witness Kernel will be documented in this file.

## [0.2.0] - 2026-01-21

### Added
- **Frame isolation layer** (`src/frame.rs`):
  - `RawFrame`: Opaque container with private bytes (no Clone, no AsRef<[u8]>)
  - `InferenceView`: Restricted interface for modules (cannot export bytes)
  - `FrameBuffer`: Bounded ring buffer with build-time caps (30s, 300 frames)
  - `Detector` trait: Modules run inference without capturing pixel data
  - `StubDetector`: MVP motion detection via pixel hash comparison
  - `BreakGlassToken`: Placeholder for quorum-gated vault access

- **Ingestion layer** (`src/ingest/`):
  - `RtspSource`: Stub RTSP source with synthetic frames
  - `RtspConfig`: Configuration for RTSP streams
  - Timestamp coarsening at capture time
  - Non-invertible feature hash computation at capture time

- **Runtime improvements**:
  - `env_logger` for structured logging
  - Frame buffer stats logging
  - Verbose mode for `log_verify`
  - Conformance alarm checking in `log_verify`

### Changed
- `Module` trait now receives `InferenceView` instead of `Frame`
- `ZoneCrossingModule` uses `StubDetector` for motion detection
- `witnessd` uses `RtspSource` and `FrameBuffer` for frame handling

### Security
- Raw bytes are now physically inaccessible to modules (type-level enforcement)
- Frame buffer auto-zeroizes on drop and eviction
- Only path to raw bytes is `RawFrame::export_for_vault()` requiring `BreakGlassToken`

## [0.1.2] - 2026-01-21

### Fixed
- `validate_zone_id()` regex now compiled once via OnceLock
- Added negative test for module event-type allowlist rejection

## [0.1.1] - 2026-01-21

### Added
- `ReprocessGuard` wired into `read_events_ruleset_bound()`
- `conformance_alarms` actively written on contract/module violations
- `RawMediaBoundary` choke point scaffold
- Runtime module event-type authorization via `ModuleDescriptor`

### Changed
- Zone ID validation: blocklist → strict allowlist regex

## [0.1.0] - 2026-01-20

### Added
- Initial kernel: sealed log, contract enforcer, bucket key manager
- `witnessd` daemon and `log_verify` tool
- Spec documents: invariants, event contract, threat model, architecture