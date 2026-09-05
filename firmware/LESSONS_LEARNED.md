# SecuraCV Canary — Lessons Learned & Regression Notes

> **Read this file before modifying firmware.**
> Every entry here was learned the hard way. Each one cost debugging time.
> If you're about to make a change and something here is relevant, follow
> the guidance. If you learn something new, ADD IT HERE.

---

## A driver library's init table is a PANEL personality, not a chip default

### The stock RM690B0 table lit the T4-S3 and darkened the Waveshare 2.41 (2026-08)
- **What happened:** The AMOLED 2.41's first hardware boot showed a
  perfectly healthy serial log — QSPI up, FT6336 answering, "RM690B0
  AMOLED portrait up" — and a completely dark glass. The HAL used
  `Arduino_RM690B0::begin()`, whose built-in init table is the LilyGO
  T4-S3's: its `0x5A`/`0x5B` writes ("SWIRE FOR BV6804") program *that
  module's* AMOLED power chip through the controller. On the Waveshare
  panel those values misprogram a power stage the bench-tested init
  (CircuitPython's, this pin map's provenance) never touches. A wrongly
  powered emissive panel still ACKs every bus command — nothing in
  software can see the failure.
- **Also wrong, more quietly:** our one-register "vendor delta"
  (page-0x13 `0xEB=0x0E`) was replayed *after* `begin()` — the bench sets
  it before sleep-out. Patching a foreign personality after the fact is
  not the same init sequence.
- **The fix:** subclass and override `tftInit()` so `begin()` plays the
  full bench-tested sequence for this exact panel, in bench order
  (`display_amoled241.cpp`).
- **Guidance:** a display class named after the *controller* ships an init
  table tuned for one *panel module* — power programming (SWIRE/gamma/bias)
  travels with the module, not the silicon. Bringing up a new glass, diff
  the library's table against a bench-proven init for that exact product
  before trusting `begin()`; on emissive panels, assume "serial fine, zero
  photons" means panel power until proven otherwise.

---

## A diagnostic named after a thing must test that thing

### chain_ok never looked at the chain — on two products at once (2026-08, PR #1540)
- **What happened:** The diagnostics self-test named `chain_ok` returned
  `crypto_healthy`, and canary-wap's `/api/fleet` coarse chain answer was
  `!tamper && verify_failures == 0`. Neither ever read a chain byte, so a
  silently rewritten tail that tripped no verify still scored green on the
  Witness Wall, in the apps, and in the health score.
- **Second layer (the Codex catch):** the first fix re-walked
  `witness_get_last_record()` — but on the canary product records were
  created into CALLER-owned structs and the static that getter returns
  stayed at seq 0 forever, so the fixed test's "record not in RAM" bypass
  was the permanent path. A test that can never reach its assert is the
  same lie with more steps. `witness_create_record_gps` now retains the
  newest record; the tail check recomputes the canonical chain hash,
  matches it against the head, and verifies the signature.
- **Guidance:** when a check is a proxy (flag, counter, neighbor's health),
  either make it test the named thing or rename it to what it actually
  tests. And prove the check can FAIL: if no reachable state flips it red,
  it is decoration. Same lesson at CI scale in the same PR:
  `scripts/ci/conformance.sh` existed for months, ran in no workflow, and
  had quietly rotted against three kernel API changes — an unexecuted gate
  protects nothing.

---

## Blocking I/O vs the Task Watchdog

### SD.begin() has no deadline — a bad card crash-looped the device, and safe mode too
- **What happened:** A freshly flashed XIAO ESP32-S3 hit `task_wdt: loopTask`
  ~8 s into "[SD] Attempting mount...", rebooted, and after 3 crashes entered
  safe mode — which then crashed the same way every ~40 s ("consecutive crash
  count 7/3…" climbing forever). The AP flapped on every cycle, so the captive
  portal rendered blank and WiFi "took forever to broadcast".
- **Root cause (three layers):**
  1. `SD.begin()` is a chain of yield-free CPU spin loops in the SPI SD
     driver (500–1000 ms card waits, ×3 retries, two mount speeds, FAT sector
     reads) with NO overall deadline. `sd_mount_safe`'s `SD_MOUNT_TIMEOUT_MS`
     was checked only *between* the two blocking attempts — it bounded
     nothing. A wedged-but-present card exceeds 8 s realistically.
  2. Everything from `esp_task_wdt_add(NULL)` in setup() to the first loop()
     pass shared ONE unfed 8 s budget — camera init seconds + the SD mount
     easily blew it on the first boots after flashing.
  3. The loop's `sd_periodic_check` re-ran the blocking mount unconditionally
     — including in SAFE MODE (which had skipped SD init on purpose), at
     ~38 s, before safe mode's 60 s recovery window. Safe mode could never
     stabilize.
- **Fix:** blocking mount work runs on a dedicated worker task at
  `tskIDLE_PRIORITY` (both IDLE tasks are WDT-subscribed and the driver never
  yields — any higher priority just moves the panic to IDLE0/IDLE1); the loop
  task polls with WDT feeds up to a 4 s budget and adopts late results on
  later passes; the periodic recheck honors safe mode (host-tested decision
  table in `sd_mount_logic.h`); setup() feeds the WDT between Phase-3 steps;
  mount-success provisioning (mkdir + csi_event_log) moved to the loop-side
  mount transition so late mounts aren't half-initialized.
- **Three traps for future radio/storage work on the XIAO ESP32-S3:**
  - `LED_BUILTIN == GPIO21 == SD_CS`. Any LED write while another task is
    mid-SPI-transaction glitches chip-select. Check `sd_mount_in_flight()`
    before driving the LED.
  - Never call `SD.end()` (or tear down the bus) while a mount attempt is in
    flight on another task — `SDFS::end` frees the card struct under the
    driver (use-after-free). Wait for the attempt to conclude.
  - Never gate SD usability on raw `SD.cardType()`: during a background
    mount the card struct is mid-initialization and `cardType()` can read a
    garbage non-`CARD_NONE` value, so the follow-up `SD.open()` races
    `f_mount` on the worker. Gate on `sd_is_available()` (false until the
    loop adopts the result) or check `sd_mount_in_flight()` first — the
    csi_event_log and beacon-audit paths do both now (Codex P1 on #820).
- **Regression check:** `sd_mount_logic::periodic_action` +
  `mount_wait_expired` host tests (`test_sd_mount_logic.cpp`) pin the
  safe-mode gate, the in-flight guard, the recheck interval, and wrap safety.
- **Date learned:** 2026-07

### A shared mDNS hostname is a session killer, not just a naming nit
- **What happened:** With two Canaries on one WiFi, `canary.local` reached an
  arbitrary device — and could switch devices BETWEEN REQUESTS of one page
  session. The page loads from device A (cookie set by A), a later fetch
  resolves to device B, B rejects A's session cookie → 401. Field symptoms
  looked unrelated: Fleet QR button showing a bare "Failed", dashboards
  "logging out" randomly.
- **Root cause:** both devices claimed the bare `canary` delegated hostname.
  The first-wins probe (600 ms, at STA join) is racy for devices that boot
  together — power-restored-after-outage is the NORMAL multi-device boot.
- **Fix pattern (catchall_logic.h, host-tested):** (1) stagger the claim by a
  fingerprint-derived delay so simultaneous boots serialize; (2) while
  claimed, periodically re-probe and resolve a detected double-claim with a
  deterministic tie-break both sides compute identically from the same pair
  (lower IP keeps) — antisymmetry means exactly one withdraws, no protocol.
  And never surface only the shared name in UIs: the Fleet list shows each
  device's unique `canary-<name>.local`.
- **Date learned:** 2026-07

### Anything that can block >1 s does NOT belong on the loop task — spawn a worker
- **What happened:** the deferred BLE bring-up (post-join-window) ran inline
  from loop(); ~21 s after boot the panic watchdog fired on loopTask with
  both cores IDLE — the loop was parked inside NimBLE init, which
  synchronizes with the WiFi coexistence layer and can block its caller far
  past the 8 s budget. Two consecutive crashes; one more would have tripped
  safe mode.
- **The pattern (now used 4×):** SD mount worker (#820), MJPEG stream worker
  (#822), fleet mDNS browse worker (#823), BLE bring-up worker (this fix).
  The loop task is WDT-subscribed and owns the periodic state machine; its
  budget is milliseconds. Any call that *can* wait on another subsystem's
  semaphore (SD driver, httpd socket, mDNS component, BT controller/coex)
  runs on a one-shot or dedicated worker at low priority with an
  internal-RAM stack, communicating back through flags/state — never inline.
- **Corollary for mDNS specifically:** the mDNS component serializes API
  calls internally. A loop-side "quick" 600 ms probe queued behind a
  worker's 3 s service browse waits for both; check the browse's busy flag
  and skip the tick instead.
- **Instrumentation beats forensics:** the boot log now prints an internal-
  heap ledger line after every heavy phase and around the BLE bring-up.
  The multi-radio budget question ("does BLE fit?") is answered by reading
  a boot log, not by reconstructing it from ENOBUFS crashes.
- **Date learned:** 2026-07

### esp_http_server is single-task — a streaming handler starves every other endpoint
- **What happened:** While the camera peek preview streamed, the whole
  dashboard went dead: `/api/peek/status` polls hung (UI showed "Current:
  Unknown", "THROUGHPUT 0 kbps", "STREAM UPTIME —" *during* a live stream),
  sensor-tuning sliders did nothing, and other tabs stalled. Everything
  "worked" again the moment the stream stopped — which made the individual
  controls look broken rather than blocked.
- **Root cause:** `esp_http_server` runs ONE worker task. Any long-lived
  handler (an MJPEG `while` loop, a long poll, a big upload) occupies it for
  the connection's whole lifetime, and every other request on every other
  socket queues behind it. No amount of endpoint-side fixing helps — the
  requests never reach their handlers.
- **Fix:** detach long-lived responses with `httpd_req_async_handler_begin()`
  and serve them from a dedicated FreeRTOS task, calling
  `httpd_req_async_handler_complete()` on exit (`peek_stream_task` in
  canary_wap.ino). Rules that made it safe:
  - The worker must yield on **every** loop path (`vTaskDelay` ≥ 20 ms pace
    floor, host-pinned in `peek_stream_logic.h`) — both IDLE tasks are
    subscribed to the 8 s panic TWDT, and a yield-free priority-3 loop would
    starve them (same trap as the SD worker, opposite priority reasoning:
    this loop always yields, so priority 3 is fine).
  - Task stack in internal RAM (the prebuilt Arduino core can't put task
    stacks in PSRAM); 8 KB covers httpd chunk sends + JPEG headers.
  - One busy flag with acquire/release atomics: only the httpd task sets it
    (handlers are serialized on the single server task, so check-then-set
    can't race another handler), only the worker clears it, and it clears
    AFTER `httpd_req_async_handler_complete()` — so "busy is clear" means
    "the driver and the socket are truly free".
  - Anything that tears down what the worker uses (camera re-init, resolution
    change) must stop-and-WAIT on that flag with a bounded timeout and fail
    closed (503) on expiry — a blind `vTaskDelay(100)` "let it exit" sleep is
    a race, and a wedged client socket can hold the worker in
    `httpd_resp_send_chunk` for up to `send_wait_timeout` (30 s).
  - A stopped stream cannot be resumed by flipping its flag back on — the
    HTTP response is finished. Report `stream_stopped` and let the client
    reconnect.
- **Regression check:** `test_peek_stream_logic.cpp` pins the pace floor and
  the uptime-freeze/throughput math; the UI's throughput formatting is pinned
  in `web_ui_logic.test.js` (`fmtKbps`).
- **Date learned:** 2026-07

---

## Memory

### Init ORDER is a heap budget — and the network MUST win it
- **What happened, round 1:** Both field devices, freshly flashed WITH PSRAM
  enabled, showed "NimBLE init failed" in the self-test. No OOM panic — the
  #819 heap guard did its job — but the radio never came up. Cause: the BLE
  controller needs a ~30 KB *contiguous internal DMA* block (PSRAM can't
  host it), and Bluetooth initialized LAST in Phase 3, after camera, SD,
  audio, WiFi/lwIP, httpd, and mesh had fragmented internal RAM below the
  guard's threshold. PSRAM moves the big allocations, not the fragmentation
  of what stays internal.
- **What happened, round 2 (the fix that made it WORSE):** moving BLE init
  to right after the camera, BEFORE WiFi, made BLE init succeed — and then
  the network lost the same budget instead: `httpd_server_init: error in
  creating msg socket (105)` (ENOBUFS — no web server at all), the SoftAP's
  WPA2 handshake failed so phones looped on the password prompt, and the
  heap monitor sat in EMERGENCY at 2 KB free. On a FULL/S3 build,
  camera + audio + WiFi + httpd + the full BLE stack (~55–65 KB total
  internal: controller + host + six GATT services + discovery) do NOT all
  fit; the boot order only chooses which subsystem starves. "Bluetooth up,
  network dead" is strictly worse than "no Bluetooth, honest reason".
- **The actual fix (two parts):**
  1. The entire BLE bring-up (stack init AND radio activity) runs one-shot
     from the loop once the provisioning join window clears and the setup AP
     is torn down — the point of *maximum* free internal memory, because the
     AP interface's buffers have been returned. Network first, always.
  2. The heap guard gained a TOTAL-free axis next to the contiguous-block
     axis (`bt_defaults::init_has_headroom(largest, total)`,
     `MIN_INIT_TOTAL_FREE = 96 KB`, host-tested): a successful controller
     malloc is not consent to spend the system into the ground.
- **Traps this journey documented:**
  - csi_event_emit before csi_integration::init registers the module is a
    *silent drop* — anything that emits witness events must run after the
    web server phase (the deferred bring-up satisfies this for free).
  - A subsystem that "fails to init" in the field must record WHY where the
    UI can reach it — `bluetooth_channel::init_fail_reason()` in
    /api/selftest and /api/bluetooth. The old catch-all "NimBLE init failed"
    label cost a full field-debug cycle; the round-2 regression was
    diagnosed in minutes because the serial log carried errno 105.
- **Date learned:** 2026-07

### BLE controller init OOM boot-loops a no-PSRAM build (and defeats safe mode)
- **What happened:** A XIAO ESP32-S3 flashed with **PSRAM disabled** in the
  Arduino IDE boot-looped forever on `E BLE_INIT: Malloc failed` /
  `BLE assert emi.c 164` / `Interrupt wdt timeout`. Safe mode did not recover
  it — every boot, including safe-mode boots, crashed at the same point.
- **Root cause:** With PSRAM off, internal heap is ~127 KB. After the WiFi AP
  and HTTP server come up, no contiguous ~30 KB block (0x7800) remains for the
  BLE controller's init allocation. `NimBLEDevice::init()` doesn't return false
  here — the controller **asserts and panics**, so the graceful "init returned
  false → degrade" paths never run. The BLE Scout's `nimble_scan_init()` (via
  the CSI tick) brought the stack up even in safe mode, so safe mode crashed
  too. Enabling BLE by default (the dead-panel fix) assumed PSRAM was on.
- **Fix:** Guard every `NimBLEDevice::init()` call site
  (`bluetooth_channel.cpp`, `ble_manager.h`, `ble_scout_nimble.cpp`) with a
  free-memory check *before* calling init: if the largest free internal
  DMA-capable block is below `bt_defaults::MIN_INIT_FREE_BLOCK` (48 KB), skip
  the stack and leave the radio off. BLE degrades to "off" instead of bricking;
  the device always boots to a reachable AP + dashboard.
- **Regression check:** `bt_defaults::init_has_headroom` is a pure, wrap-safe
  predicate with host-test coverage in `test_bt_defaults.cpp` (below threshold
  → skip; at/above → allow; threshold clears the ~30 KB controller alloc).
- **The real fix is on the user's side:** Arduino IDE → Tools → PSRAM → "OPI
  PSRAM". The XIAO ESP32-S3 has 8 MB PSRAM; "PSRAM not found" in the boot
  banner means it's disabled in the build. The guard only keeps a mis-set
  build usable.
- **Now enforced at compile time:** the misconfiguration recurred in the
  field (the IDE toggle silently reverts when the board selection changes,
  and even a non-crashing no-PSRAM FULL build limps at <1 KB free heap —
  BLE refused, SD writes failing, mDNS timing out). `build_config.h` now
  `#error`s on FULL + XIAO_ESP32S3 without `BOARD_HAS_PSRAM`
  (`SECURACV_ALLOW_NO_PSRAM` to bypass), and the CI Arduino build compiles
  with `PSRAM=opi` instead of the bare board FQBN whose default is
  PSRAM=Disabled — CI had been validating the broken configuration.
- **Date learned:** 2026-07

---

## ESP32 Arduino Core 3.x Migration

### mbedTLS API changes — NO `_ret` suffix
- **What happened:** Firmware compiled on Core 2.x but failed on 3.x
- **Root cause:** ESP32 Arduino Core 3.x (built on ESP-IDF 5.x) removed the
  `_ret` suffix from all mbedTLS functions
- **Fix:** `mbedtls_sha256_ret()` → `mbedtls_sha256()`, etc.
- **Regression check:** `regression_check.sh` greps for `_ret(`
- **Date learned:** 2026-01

### `esp_camera.h` include order matters
- **What happened:** Compile errors about undefined camera structs
- **Root cause:** `esp_camera.h` must be included AFTER WiFi/system headers
- **Fix:** Include order: system → WiFi → crypto → esp_camera → project headers

### Watchdog API differs between ESP-IDF 4.x and 5.x
- **What happened:** CI build failed with `esp_task_wdt_config_t` not declared
- **Root cause:** ESP-IDF 5.x introduced a struct-based watchdog API
  (`esp_task_wdt_config_t`, `esp_task_wdt_reconfigure`). ESP-IDF 4.x uses
  the simpler `esp_task_wdt_init(uint32_t timeout, bool panic)`.
  PlatformIO `espressif32 @ ^6.5.0` with Arduino framework uses ESP-IDF 4.4.x.
- **Fix:** Use `#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)` to
  select the correct API at compile time. Include `esp_idf_version.h`.
- **Date learned:** 2026-02

---

## Hardware: XIAO ESP32S3 Sense

### Camera pins are board-specific
- **What happened:** Camera init returned ESP_FAIL silently
- **Root cause:** Used generic ESP32-CAM pin definitions instead of XIAO-specific
- **Fix:** XIAO ESP32S3 Sense pins:
  ```
  PWDN  = -1  (CRITICAL: must be -1, not 32)
  RESET = -1
  XCLK  = 10
  SIOD  = 40
  SIOC  = 39
  Y9-Y2 = 48,11,12,14,16,18,17,15
  VSYNC = 38
  HREF  = 47
  PCLK  = 13
  ```
- **Regression check:** Script verifies PWDN = -1

### SD card SPI pins are board-specific
- **What happened:** SD card init failed with no error message
- **Root cause:** Wrong SPI pins for XIAO ESP32S3 Sense
- **Fix:** CS=21, SCK=7, MISO=8, MOSI=9. Must call `SPI.begin(7,8,9,21)`
  before `SD.begin(21)`
- **Regression check:** Script verifies pin numbers

### GPS UART pins
- **What happened:** No GPS data received
- **Root cause:** Used default Serial1 pins instead of XIAO-specific
- **Fix:** L76K GNSS on UART1: RX=GPIO44 (D7), TX=GPIO43 (D6), 9600 baud

---

## Security

### AP password must be device-unique
- **What happened:** Default `witness2026` password was in the README; anyone
  could connect to any Canary
- **Fix:** Derive AP password from pubkey fingerprint at provisioning
- **Regression check:** Script greps for hardcoded `witness2026`

### Token must never enter witness chain
- **What happened:** (Preventive) API tokens are transport auth only
- **Rule:** Bearer tokens authenticate HTTP requests. They are NOT
  cryptographic evidence. They must NEVER appear in witness records,
  chain hashes, or SD card logs.
- **Regression check:** Script greps for token references in chain context

### Ed25519 private key must never be HMAC key directly
- **What happened:** (Preventive) First design used privkey as HMAC key
- **Fix:** Two-step HKDF derivation. Derive intermediate `token_key` first,
  then derive API token from intermediate key.
- **Why:** Key separation principle. If token derivation has a flaw, signing
  key is protected.

### BLE adds proprietary binary blobs
- **What happened:** Enabling CONFIG_BT_ENABLED pulls in Espressif closed-source
  BT stack, which includes CVE-2025-27840 attack surface
- **Rule:** BLE features must be compile-time opt-in (OFF by default)
- **Users must explicitly understand the tradeoff before enabling**

### Two headers from the same browser cannot vouch for each other
- **What happened:** The glass's LAN web API refused cross-site writes by
  comparing the Origin header's authority to the Host header. Both are the
  browser's, and DNS rebinding makes them agree for the attacker: a page on a
  public domain re-pointed at the device's LAN IP arrives with Origin == Host,
  reads the CSRF token from `/api/settings` as same-origin, and writes.
- **Fix:** `canary/net/host_guard.h` — the Host must be something that can only
  mean this device on this network (an IP literal, `.local`, a single label, or
  a private-use suffix); a public domain is foreign for the writes, the token,
  and the per-witness reads. Header-only, host-tested, no Arduino.
- **Rule:** A same-site check needs one side the attacker cannot choose.
  Against rebinding that side is the device's own identity, never a second
  header from the same request.
- **Date:** 2026-09

### User-typed identifiers must use an unambiguous alphabet
- **What happened:** A user typed their API token from the serial monitor into
  the dashboard, hit Connect, got "Too many failed attempts" after a few tries.
  Forensics showed `I` (capital i) at position 26 in the real token vs `l`
  (lowercase L) in what they typed — same glyph in most fonts, different
  bytes, constant-time compare fails. The auth rate limiter compounded the
  pain.
- **Root cause:** The token / AP-password encoder used a full base62 alphabet
  (`0-9 A-Z a-z`), which contains glyph-confusion classes `0/O` and `1/I/l`.
- **Fix (v1):** Switched `format_api_token_string` (API tokens) and
  `derive_ap_password` (`cv-XXXXX` AP password) to a 57-char unambiguous
  alphabet that dropped `0`, `O`, `1`, `I`, `l`.
- **Follow-up (v2):** The 57-char set still leaked lowercase `o`/`i` and
  uppercase `L` (also confusable with `0`/`1`/`l`), and the human-facing
  **device_id / AP-SSID suffix** was still raw hex (`%02X%02X`), so `0` and
  `1` reached users on `canary.local` and on the Wi-Fi list. Tightened the
  shared `UNAMBIGUOUS_ALPHABET` to **54 chars** — dropping every case variant
  of the confusion classes: `0/O/o` and `1/I/i/l/L` — and routed the device_id
  / AP-SSID suffix through it via a base-54 re-encoding of the same 16 identity
  bits (injective, so per-board uniqueness and reflash-stability are preserved).
  Rejection-sampling threshold moves `228` (`57*4`) → `216` (`54*4`) so the
  result stays unbiased. Entropy across 32 chars is ~184 bits — still a UX win.
  Applied identically in both firmware trees (`firmware/canary` PlatformIO and
  `projects/canary-wap` Arduino) so they stay byte-compatible.
- **Note (migration):** device_id is recomputed at boot, not persisted, so a
  device's externally-visible id (mDNS TXT, MQTT client id, HA unique_id)
  changes format on this update. The witness chain is unaffected (genesis
  `chain_head` is persisted in NVS). Acceptable pre-v1; HA may re-discover the
  device under its new id.
- **Rule:** Any identifier a human will type, read aloud, or transcribe from a
  sticker — tokens, passwords, **and the device_id / AP-SSID** — MUST avoid all
  of `0/O/o` and `1/I/i/l/L`. Machine-only IDs (chain head, signatures, raw
  hex) are fine as-is.
- **Regression check:** `regression_check.sh` greps for the full base62 string,
  the old 57-char alphabet, and any `%02X%02X` hex device_id / AP-SSID suffix in
  the firmware source trees.
- **Date learned:** 2026-04 (v1), 2026-06 (v2)

### The onboarding wizard must feed the TWDT — the loop()-path re-raise runs watched
- **What happened:** A Canary 7 (nightstand7) whose saved Wi-Fi could not join
  re-raised the on-glass setup wizard from loop() (`wifi_wants_setup`) — and
  panicked mid-wizard, every time. First boot (placeholder credentials) crashed
  the same way once anything else went wrong after the watchdog was armed.
- **Root cause:** `provision_run()` blocks for as long as a HUMAN takes to scan
  the QR and type a password — minutes. At boot it runs before
  `cc_task_wdt_arm()`, but the loop()-path re-raise enters with the 30 s TWDT
  already watching loopTask, and none of the wizard's wait loops ever called
  `esp_task_wdt_reset()`. Same class as the SD.begin() entry above: one unfed
  budget spanning an unbounded wait. `wifi_init_or_reboot()`'s bounded boot
  connect had the twin defect — its 30 s bound exactly equals the WDT timeout.
- **Fix:** `wdt_feed()` in every wizard wait loop (hello beat, portal loop,
  success linger) and in the boot-connect wait — a no-op before the WDT is
  armed, so the boot path is unchanged.
- **Regression check:** none automatable on host (the TWDT is silicon); the
  wizard loops all route through `wdt_feed()`, so a future loop added without
  it is the thing to catch in review.
- **Date learned:** 2026-08

### A signature over the payload says nothing about the header that carries it
- **What happened:** Four defects in the Beacon receive path
  (`beacon_channel.cpp::handle_alert_frame`), all found by reading the spec
  next to the code. (1) The two Ed25519 signatures cover
  `BeaconAlertCanonical`, which carries its own `msg_type`, but every
  decision — dispatch, the accept action, the audit line — read the
  *header's* `msg_type` instead. Rewriting that one unsigned byte on a
  captured drill or all-clear turned it into a real ALERT on every receiver,
  with both signatures still verifying. (2) There was no nonce dedup and no
  freshness window, so any captured in-window frame could be rebroadcast
  indefinitely; each replay ran the per-pubkey rate check, so four replays
  exhausted that neighbor's 24 h budget and silenced their next genuine
  alert *and* their all-clear. (3) The CANCEL branch's reference check was
  `memcmp(ref_canceled_nonce, "", 0) == 0` — a zero-length compare, which is
  vacuously true — so any valid CANCEL cleared any active alarm, and the
  accepted alarm's frame nonce was never stored anywhere to compare against.
  (4) Drills shared the per-pubkey rate bucket with real alerts, which
  AGENTS.md Beacon invariant 10 forbids: a morning of drills could
  rate-limit away that afternoon's fire alert.
- **Root cause:** one habit, four symptoms — trusting a field because the
  frame it arrived in was signed. The header is not covered by the
  signature; a zero-length `memcmp` is not a comparison; a counter with no
  class is a shared counter.
- **Fix:** cross-check `canonical->msg_type == hdr->msg_type` and key the
  action block off the signed value; require the EXERCISE flag bit exactly
  when the signed `msg_type` is EXERCISE, in both directions; a 32-entry
  seen-frame ring checked *before* signature verification (so a replay costs
  no crypto and never charges the rate bucket) plus the spec's
  `|now - effective| <= BEACON_FRESHNESS_S` window, keeping the
  unsynced-clock accept-but-flag branch; store the accepted alarm's header
  nonce and require CANCEL/UPDATE to carry a non-zero reference matching it;
  a second rate counter per pubkey selected by frame class.
- **And the first cut of the fix repeated the mistake it was fixing.** The
  ring was keyed on `hdr->nonce` — the field the spec's step 3 names, and a
  field nothing signs. A replay with one header byte changed walked past
  dedup, verified, charged the bucket, re-fired the alarm and overwrote the
  alarm's identity, so the originator's real CANCEL then missed. Caught in
  review. The ring now keys on the two signatures (deterministic under
  RFC 8032, so a copy has the same identity whatever its header says, and
  a new identity that verifies needs the keys), and the latest ALERT's
  identity is kept outside the ring so a copy held past the horizon cannot
  re-raise the alarm on an unsynced clock. The header nonce remains the
  CANCEL reference the spec requires — a relay that rewrites it can still
  desync a CANCEL, and only a wire-format change (nonce inside the signed
  canonical) closes that; it is listed as open in the audit.
- **Rule:** when a struct is signed and its envelope is not, every field the
  receiver acts on must come from inside the signature — and any field that
  exists in both places must be compared, not chosen. If a check's operands
  can't differ (a zero-length compare, a constant), it is decoration; prove
  it can fail.
- **Regression check:** `tests_host/test_beacon_origination.cpp` mirrors the
  whole receive pipeline and fails on the pre-fix logic for each of the
  four; `test_beacon_solo_origination.cpp` pins that the solo path answers
  to the same checks. Run `make` in
  `firmware/projects/canary-wap/tests_host/`.
- **Date learned:** 2026-09

---

## Web UI (web_ui.h)

### No browser storage APIs
- **Rule:** No `localStorage`, `sessionStorage`, or `document.cookie`
- **Why:** Token must live only in JS variable. Tab close = token gone.
- **Regression check:** Script greps for storage APIs

### PROGMEM size limits
- **What happened:** Very large web_ui.h caused flash allocation issues
- **Rule:** Keep web_ui.h under 64KB. If larger, split into separate headers.
- **Regression check:** Script checks file size

### Every button must have a backend
- **Rule:** No UI element should exist without a working firmware handler
- **Why:** Dead buttons destroy user trust and waste debugging time
- **Process:** When adding UI elements, always implement the API endpoint
  in the same commit

### `max_uri_handlers` silently drops routes past the budget
- **What happened:** The canary-wap dashboard's Presence tab, speaker,
  Chirp, and BLE routes all 404'd on the default build even though they were
  registered — "half the settings panel does nothing".
- **Root cause:** `esp_http_server` returns `ESP_ERR_HTTPD_HANDLERS_FULL`
  for every `httpd_register_uri_handler` past `config.max_uri_handlers`, and
  no call site checked the return value. The budget was hand-summed to 123
  while the active server registered 154, so the last 31 routes never
  installed — silently.
- **Fix:** Itemize the budget per feature flag so it tracks the build, size
  it to fit with headroom.
- **Regression check:** `firmware/projects/canary-wap/tests_host/check_route_budget.py`
  emulates the preprocessor for FULL/S3, DEV/S3 and FULL/C3 and fails CI if
  the budget doesn't cover the registrations with margin.
- **Date learned:** 2026-07

### A missing auth header must not feed the brute-force lockout
- **What happened:** A correct pasted token was rejected with "Too many
  failed attempts" — the device 429'd everything, including valid clients.
- **Root cause:** `api_auth_check` counted a *missing* `Authorization`
  header as a failed guess. One dashboard tab left open after its (RAM-only)
  session cookie died polled `/api/status` every 5 s with no credentials,
  arming the shared exponential lockout. A credential-less request carries
  no guess, so counting it is a self-DoS, not brute-force protection.
- **Fix:** `auth_logic::counts_toward_lockout` — NO_CREDENTIAL never counts;
  MALFORMED and WRONG_TOKEN still do.
- **Regression check:** `tests_host/test_auth_logic.cpp` (host).
- **Date learned:** 2026-07

### Self-test probes must not FAIL a subsystem that merely hasn't started
- **What happened:** The pre-flight health check showed "almost everything
  failing" on a healthy XIAO S3.
- **Root cause:** The Bluetooth probe hard-FAILed "NimBLE init failed"
  during the boot window before init ran and *permanently* in safe mode
  (where radio inits are skipped by design), and safe mode wasn't reported
  at all, so a recovering-but-fine device was indistinguishable from broken.
- **Fix:** `selftest_logic::bluetooth_status` returns SKIP for the
  not-yet-initialized and safe-mode states; `run_to_json` emits `safe_mode`
  and counts SKIP rows.
- **Regression check:** `tests_host/test_selftest_logic.cpp` +
  `selftest_ui.test.js`.
- **Date learned:** 2026-07

---

## GPS & Time

### Time coarsening is mandatory
- **Rule:** SecuraCV coarsens timestamps to 5-second buckets (minimum)
- **Why:** Privacy by design. Precise timestamps enable correlation attacks.
- **Watch for:** High-precision format strings (`%.6f`, `%.7f`) near GPS data

### First GPS fix takes 8+ minutes
- **What happened:** Thought GPS was broken, but it was cold-start TTFF
- **Reality:** L76K cold start = 25-35 seconds typical, but can take 8+
  minutes if almanac data is lost
- **UX:** Dashboard should show "Acquiring satellites..." not "GPS Error"

---

## Build System

### A new file under `canary-display/src/ui/` changes the committed emulator wasm — even one that compiles to nothing
- **What happened:** The First Light pair demo added
  `src/ui/pair_demo_ui.cpp`, whose entire translation unit sits behind
  `#if FEATURE_PAIR_DEMO` (off in the emulator build). CI's
  "firmware → wasm → boots in a browser" gate still went red: the committed
  `canary-local/emulator/dist/*.js` no longer matched a rebuild from the
  tree.
- **Root cause:** `canary-local/emulator/build.sh` globs the whole directory
  — `"$PROJ"/src/ui/*.cpp` — so the new TU joins the wasm link whether or
  not it emits code. An empty object file still moved the output by 4 bytes
  per display artifact (`canary-display-dash.js` 975983 → 975987,
  `canary-display-watch.js` 983326 → 983330), and the dist gate is a
  **byte** diff, not a behavior diff.
- **Fix:** dispatch **Actions → "Rebuild emulator dist (pinned emsdk)"** on
  your branch; it rebuilds with the pinned emsdk 6.0.3 and pushes the result.
  The rebuild is not optional for any `src/ui/` addition, and it cannot be
  done in most working environments — the gate's own fixer needs a toolchain
  that a network-restricted sandbox or an ordinary laptop generally lacks.
- **Two traps that cost a round each here:**
  - **Push before you dispatch.** The workflow rebuilds the ref it was
    dispatched on and pushes back; if you push anything while it runs, its
    push is rejected non-fast-forward and the job fails with nothing to show
    for the build minutes.
  - **Its push collects no checks.** GitHub's recursion guard means a
    `GITHUB_TOKEN` push does not retrigger workflows, so the refreshed head
    lands with **zero** check runs — a PR that looks unvalidated rather than
    green. It needs one ordinary push on top (this entry was that push).
    The workflow header says so; believe it.
- **Regression check:** the dist gate itself, which is why it exists. Nothing
  to add — just remember that `src/ui/` and byte-exact committed artifacts
  are the same decision.
- **Date learned:** 2026-08

### Whether `common/` needs an explicit build_src_filter depends on the env's LDF mode — and getting it wrong fails at LINK, in either direction
- **What happened:** `canary-display-nightstand7` was added with the same
  `build_src_filter` block its siblings carry:
  ```
  +<../../../common/color/color_engine.cpp>
  +<../../../common/color/look_engine.cpp>
  ```
  It failed to link with `multiple definition of
  canary::color::gamma8` — and of every other symbol in the pair. The build
  had compiled the color engine **twice**: once as `.pio/common/color/*.o`
  from that filter, and once as an ordinary LDF library
  (`.pio/build/canary-display-nightstand7/lib2ce/color/*`).
- **Why the siblings are different:** `nightstand-s3` and `touch169` extend
  `canary_display_base` on the core-2 platform, where the LDF runs `deep+`
  and does **not** map `common/color` to a library — those envs genuinely
  need the explicit sources, and the long warning above them (a
  library-manifest route that was tried twice and failed) is still correct
  *for them*. `nightstand7` extends the **dash**, which adds
  `lib_ldf_mode = chain` on top of the base's
  `lib_extra_dirs = ../../common`, and that combination *does* resolve the
  directory as a library. Same rule, different mechanism.
- **Both directions are link-time, not compile-time.** Too few sources gives
  undefined references; too many gives multiple definitions. Neither shows up
  while the files are compiling, which is the part everyone watches.
- **The rule:** before adding a `common/` entry to a new env, look at what it
  `extends` — specifically `lib_ldf_mode` and whether `lib_extra_dirs`
  reaches `firmware/common`. With both, the LDF already compiles it; add
  nothing. Without them, name the sources.
- **Both directions are linted now.** `scripts/lint_common_lib_manifests.py`
  already guarded the undefined-reference direction; it also flags the
  duplicate one, by resolving each env's *effective* `lib_ldf_mode` and
  `lib_extra_dirs` through the `extends` chain. It catches this in seconds —
  the build that found it took ~50 minutes and landed on main twice first.

### deep+ LDF only sees command-line defines — a feature flag that lives in config.h can't summon a framework library (2026-08, PR #1561)
- **What happened:** `canary-display-amoled241` (core-2 base, so `deep+`
  from common.ini) set `FEATURE_SD_STORAGE 1` in its flavor `config.h` and
  died at compile with `SD_MMC.h: No such file or directory` — in
  `fleet/sd_archive.cpp`, a TU that builds fine on `dash-sd`. The `deep+`
  LDF evaluates each TU's preprocessor conditionals to decide which
  libraries to wire in, but it evaluates them with **command-line defines
  only**: a flag defined in an `-I`'d header is invisible to it, so the
  `#include <SD_MMC.h>` sat in a branch the LDF scored false, the
  framework-bundled SD_MMC library never joined the build — and then the
  real compiler, which does read config.h, hit the include with no path to
  resolve it.
- **Why dash-sd never saw this:** its flag arrives as
  `-DFEATURE_SD_STORAGE=1` in build_flags (LDF-visible), and the dash
  family rides `chain` mode anyway, which follows includes without
  evaluating conditionals at all.
- **The rule:** a `FEATURE_*` flag that gates a `#include` of a
  framework/library header must ALSO be passed as `-D` in the env whenever
  the env's effective LDF mode is `deep+` — and the flavor config's own
  `#define` must be `#ifndef`-guarded so the two agree instead of fighting
  (the same guard dash-sd's comment demands for the opposite reason).
  Flags that only gate our own code can stay header-only.
  The guard's own fixtures live in
  `scripts/tests/test_lint_common_lib_manifests.py`, and they earned their
  keep immediately: the first version of the check followed
  `extends = env:<name>` to a key that does not exist, so it passed the tree,
  passed a direct hand-written case, and was blind to the real bug — the one
  where both halves are inherited and the offending section states neither.


### A flavor config's plain `#define` silently beats an env's `-D`
- **What happened:** The compile-verification envs that exist to prove
  bench-gated code actually builds were proving nothing. `canary-display-dash-vault`
  passes `-DFEATURE_TIME_MACHINE_PERSIST=1` to compile the LittleFS
  persistence body — but `configs/canary-display/dash/config.h` then said
  `#define FEATURE_TIME_MACHINE_PERSIST 0` with no guard. The file wins, so
  the env compiled the *inert no-op stubs* while its comment claimed it
  verified the real body. `-DFEATURE_BLE5_SCAN=1` had the same problem. Both
  had been green in CI for months, guarding nothing. Found while adding the
  SD-archive env (PR: hub self-setup / SD deep archive).
- **Root cause:** A `-D` on the command line is just a predefine; a later
  unguarded `#define` of the same name in a header replaces it (GCC emits a
  "redefined" warning, which is lost in a full-project build log). Only
  `FEATURE_CHIME` had the `#ifndef` guard, and its comment explains it was
  added for the emulator — the general rule was never generalized.
- **Fix:** Every flag any env sets with `-D` is now `#ifndef`-guarded in both
  flavor configs (`CHIRP_SCAN`, `BLE5_SCAN`, `FLEET_LINK`,
  `TIME_MACHINE_PERSIST`, `SD_STORAGE`), with a load-bearing comment saying
  why the guard is not stylistic.
- **Regression check:** `scripts/lint_build_matrix.py` already parses both the
  envs' `-D` flags and the configs' defines; the guard is what makes those two
  agree at compile time. The rule to apply when adding a flag: **if any env
  `-D`s it, the config must `#ifndef` it** — an unguarded define is a silent
  override, not a default.
- **Date learned:** 2026-08

### Gate on a HAS_* board capability only AFTER including pins.h
- **What happened:** `ambient_led.cpp` guarded its whole body with
  `#if defined(HAS_RGBLED) && HAS_RGBLED` — but `HAS_RGBLED` lives in the
  board's `pins.h`, which the file included *inside* the guard. At gate time
  the macro was undefined, the gate was always false, and the TU compiled
  EMPTY: the nightstand boards' WS2812 "primary ambient state channel" was
  dead code in every image that shipped it. The new Touch-1.69 CST816 gate
  copied the same pattern and would have shipped touch that ignored every
  tap. Caught in review (PR #1290), before either board's bench pass.
- **Root cause:** `HAS_*` capabilities come from the board header (`pins.h`
  via the env's `-I`), not from `<config.h>` (the flavor). A capability gate
  placed before the `pins.h` include silently evaluates the macro as
  undefined — and `defined(X) && X` makes that *look* deliberate.
- **Fix:** Include `pins.h` (pure `#define`s, safe unconditionally) BEFORE
  any `#if` that reads a `HAS_*` flag — the pattern `settings_ui.cpp`
  already documented for `HAS_ISOLATED_IO`. Fixed in `ambient_led.cpp` and
  `display_1in47.cpp`.
- **Regression check:** none automated yet; when touching a `HAS_*` gate,
  check the `pins.h` include is above it (a grep for `#if` lines that
  mention `HAS_` before the first `pins.h` include would automate this).
- **Date learned:** 2026-07

### Dual-build compatibility required
- **Rule:** Firmware must compile on BOTH Arduino IDE and PlatformIO
- **Why:** Different team members use different IDEs; CI tests both
- **Common pitfall:** PlatformIO auto-resolves includes via `lib_deps`;
  Arduino IDE requires libraries installed globally
- **Test:** CI runs both `arduino-cli compile` and `pio run`

### PlatformIO libraries can't see project `include/` by default
- **What happened:** CI build failed: `canary_config.h: No such file or directory`
  when compiling library components (`securacv_camera`, `securacv_crypto`, etc.)
- **Root cause:** PlatformIO builds libraries in isolation. The project's
  `include/` directory is only on the compiler search path for `src/`, not
  for `lib/` components. All 6 library modules `#include "canary_config.h"`
  but can't find it.
- **Fix:** Add `-I${PROJECT_DIR}/include` to `build_flags` in `[env]` section
  of `platformio.ini`. This makes the include path global across project
  source and library compilation.
- **Why not `lib_ldf_mode = deep+`?** That's slower and less explicit. A
  direct `-I` flag is clear about what's happening.
- **Date learned:** 2026-02

### Feature flags for everything
- **Rule:** Every major feature has a `#define FEATURE_X` flag (see
  `firmware/canary/include/canary_config.h` for defaults)
- **Why:** Allows building stripped-down firmware for testing, and enables
  compile-time security decisions (e.g., BLE off)
- **Pattern:**
  ```cpp
  #define FEATURE_CAMERA_PEEK  1
  #if FEATURE_CAMERA_PEEK
    #include "esp_camera.h"
    // camera code here
  #endif
  ```

### A shared `common/` module whose .cpp nothing compiles
- **What happened:** The nightstand look engine landed as
  `firmware/common/color/{color_engine,look_engine}.{h,cpp}`. Host tests passed,
  review passed, the headers resolved everywhere — and
  `canary-display-nightstand-s3` failed at LINK with an undefined reference to
  `canary::color::wash_stops`.
- **Root cause:** nothing compiled the two translation units. The
  `-I${PROJECT_DIR}/../../common` in the env still resolved the *headers*, which
  is exactly what hides the problem at compile time. **A header resolving is not
  evidence that its .cpp is built.**
- **Two wrong fixes first, and why they were wrong — this is the real lesson.**
  1. *Add a `library.json` so the LDF picks the library up.* Reasonable by
     analogy with `common/boot` and `common/fleet_link`, which have one. It did
     not change the error at all.
  2. *The manifest must be malformed, then.* The first one carried registry
     metadata including a `headers` list, which the LDF resolves relative to
     `includeDir` — so `headers: ["look_engine.h"]` beside `includeDir: ".."`
     pointed at a nonexistent `common/look_engine.h`. Plausible, checkable,
     also not it: reducing the manifest to `boot`'s exact shape produced the
     same link error a third time.
  The actual cause is upstream of the manifest. These projects inherit
  **`lib_ldf_mode = deep+`** from `common.ini`, and deep+ **evaluates
  preprocessor conditionals** while scanning for libraries. Both callers reach
  the engine from inside a guard — `portrait_ui.cpp` behind
  `#ifdef CD_FLAVOR_NIGHTSTAND`, `ambient_led.cpp` behind `FEATURE_AMBIENT_LED
  && HAS_RGBLED` — and those symbols are **not defined during the LDF scan**.
  The LDF therefore judges both includes inactive and never looks at
  `common/color` at all. **A manifest only matters for a library the LDF has
  already decided to look at.** `common/boot` works through the LDF purely
  because `boot/boot_banner.h` is included UNCONDITIONALLY from `main.cpp` —
  that difference, not the manifest, is why one linked and the other didn't.
- **Fix:** the repo's own established pattern, which `canary-sense.ini` already
  documented for exactly this reason ("our shared common/ modules … are not
  reliably mapped to LDF libraries — so compile them directly"): name the
  sources in the consuming env's `build_src_filter`. The nightstand-s3 env now
  lists `common/color/{color_engine,look_engine}.cpp`, and the manifest was
  removed so the TUs cannot be compiled twice.
- **The rule:** prefer `build_src_filter` — naming a file is deterministic.
  Reach for a `library.json` only when the header is included unconditionally.
  Never satisfy both routes for one file (duplicate symbols at link).
- **A third independent attempt made the same mistake.** #1229 landed on main
  while this was in flight and removed only the `headers` key, keeping the
  manifest — the same remedy as attempt 2 above. Main built again and failed
  again with the identical `wash_stops` undefined reference (run 30123437737 on
  `ff7cc04`). Two people reasoning carefully from `boot_banner` reached the same
  wrong conclusion, which is the strongest argument for writing the real
  distinction down: `boot_banner`'s include is UNCONDITIONAL, and that — not its
  manifest shape — is why it links.
- **Process lesson:** three CI rounds were spent because each attempt
  pattern-matched a precedent (`boot` has a manifest, so add a manifest) without
  first checking *why* the precedent works. The distinguishing fact —
  unconditional vs. conditional include under deep+ LDF — was one grep away and
  was already written down in `canary-sense.ini`. **Read the working example's
  reason, not just its shape.**
- **Regression check:** `firmware/scripts/check_common_build_reachability.py`
  (Regression Guards). Every non-test .cpp under `common/` must be reachable by
  a `build_src_filter` entry, a working manifest, or a staged Arduino copy —
  and a manifest must actually work (parse, and any declared `headers` must
  resolve, against an explicit `includeDir` or PlatformIO's `include/` →
  `srcDir` → root fallback, which is how `common/csi` resolves its `src/`-hosted
  headers). The guard's advice orders `build_src_filter` first and spells out
  the deep+ conditional trap, so the next person does not repeat the three
  rounds. It also surfaced a pre-existing orphan,
  `common/bluetooth/ble_debug_beacon.cpp`, waived with a written reason rather
  than silently ignored.
- **Date learned:** 2026-07

### A host test cannot pin the target's preprocessor
- **What happened:** `common/fleet_selfreport/fleet_selfreport.h` declared a
  local `static const char* HEX` for its `\u00XX` escape path. The host suite
  passed; the entire firmware CI matrix went red — both Arduino display builds
  on both cores, the Arduino WAP build, both PlatformIO builds, the CSI
  feature-disabled builds, and the DRAM symbol ranking.
- **Root cause:** Arduino's `cores/esp32/Print.h` does `#define HEX 16` before
  any sketch include is reached, so on device that line expands to
  `static const char* 16 = ...`. Plain g++ on the host has no such macro, so the
  test suite could not see the collision. Same shape as the `DEC`/`OCT`/`BIN`
  macros, and the reason board code avoids those names.
- **Fix:** Renamed to `fsr__hex`, matching the file's existing `fsr__` prefix
  convention for internal helpers.
- **Regression check:** `test_fleet_selfreport.cpp` now `#define`s Arduino's
  `HEX`/`DEC`/`OCT`/`BIN` immediately above the include, so the host build fails
  the same way the device build would. Verified both directions: it fails with
  the pre-fix header and passes with the fixed one. **When a shared header ships
  to Arduino, its host test should reproduce Arduino's macro namespace — a test
  that cannot see the target's preprocessor cannot pin the target's compile.**
- **Date learned:** 2026-07

---

## Security Architecture Principles

> These are non-negotiable. They override convenience, features, roadmap
> priorities, and business considerations. Any code change that violates
> a principle is a security defect. See `docs/security/THREAT_MODEL.md` for full details.

### Private keys must never leave the device
- **Principle:** The Ed25519 private key has no export, backup, or read interface
- **Why:** A key that can be exported can be compelled (court orders, coercion)
- **Rule:** No API endpoint, log statement, debug output, or export path may
  include private key material. There is no recovery mechanism by design.
- **Regression check:** Script greps for private key references in API/export context
- **Date established:** 2026-02

### Zero outbound network connections
- **Principle:** The device makes zero outbound connections — it is a server, never a client
- **Why:** Outbound connections reveal the device exists, create interceptable metadata,
  and enable server-side coercion (e.g., compelled malicious updates)
- **Rule:** No DNS lookups, no NTP, no telemetry, no update checks, no cloud sync.
  MQTT is off by default. OTA updates are user-initiated only.
- **Watch for:** Any `WiFi.begin()`, `HTTPClient`, `WiFiClient`, `mqtt.connect()` calls
- **Regression check:** Script greps for outbound connection patterns
- **Date established:** 2026-02

### No identifier leaks
- **Principle:** The device must not leak identifiers that enable tracking or correlation
- **Why:** WiFi probe responses, BLE beacons, mDNS, and raw MACs are all tracking vectors
- **Rule:** BLE off at compile time, MACs hashed before storage, GPS coarsened at
  capture, no manufacturer OUI in AP BSSID
- **Regression check:** Script checks for raw MAC storage, BLE enablement, high-precision GPS
- **Date established:** 2026-02

### Evidence must be self-verifying
- **Principle:** Exported evidence bundles verifiable without ERRERlabs involvement
- **Why:** If verification requires ERRERlabs, ERRERlabs can be shut down, compelled,
  or compromised — making all evidence unverifiable
- **Rule:** Every export includes: signed records, hash chain, public key, and
  an offline HTML+JS verification page
- **Date established:** 2026-02

### TLS required — no HTTP fallback
- **Principle:** All API traffic must be encrypted. No plaintext HTTP.
- **Why:** Even on a local AP, an attacker in WiFi range can sniff plaintext
- **Rule:** HTTP requests get 301 redirect to HTTPS. No `http://` endpoints.
  `DEFAULT_TLS_REQUIRED` must be 1 in `secure_defaults.h`.
- **Regression check:** Script checks for HTTP listener without TLS redirect
- **Date established:** 2026-02

### Fail secure, not fail open
- **Principle:** When something fails, the device fails toward MORE security
- **Why:** A device that falls back to HTTP on TLS failure, or stops recording
  when storage is full, betrays users at exactly the wrong moment
- **Rule:** SD full → witness to RAM. Auth fail → lockout. TLS fail → reject.
  Chain corrupt → tamper alert. Never degrade silently.
- **Date established:** 2026-02

### Secure defaults in secure_defaults.h
- **Principle:** All security-sensitive compile-time defaults are centralized
  in `firmware/canary/include/secure_defaults.h`
- **Why:** Scattered defaults are easy to misconfigure. Centralized defaults
  with static assertions prevent accidental weakening.
- **Rule:** Production builds should define `SECURACV_ENFORCE_SECURE_DEFAULTS=1`
  to trigger compile-time checks against insecure values.
- **Date established:** 2026-02

---

## Networking & Captive Portal

### Captive-portal probes are per-platform — answer them, don't blanket-redirect
- **What happened:** During first-boot setup, phones connected to the
  `SecuraCV-XXXX` AP but then dropped it (or routed everything over cellular,
  so `canary.local` stopped resolving) before the user could finish setup.
- **Root cause:** Every OS connectivity-check URL was answered with a 200 +
  HTML instruction page, including Android's `/generate_204`. Android only
  treats a network as "validated/online" when that probe returns **HTTP 204
  No Content**; any other response flips it to "Wi-Fi has no internet,"
  which de-prioritises the AP and eventually disassociates.
- **Fix:** Answer probes per-platform (the "hybrid" strategy):
  - **Apple** (`/hotspot-detect.html`, `/library/test/success.html`) → the
    instruction page. This pops the Captive Network Assistant sheet (which
    renders static HTML fine), and iOS/macOS keep the association while it's
    open, so they never disconnect — guidance *and* a live link.
  - **Android** (`/generate_204`, `/gen_204`) → **204 No Content**. No sheet,
    no cellular fallback, stays connected. User opens `canary.local` manually.
  - **Windows** (`/connecttest.txt`, `/ncsi.txt`) → exact NCSI bodies
    `Microsoft Connect Test` / `Microsoft NCSI`.
- **Gotcha:** Probes are always sent over **plain HTTP** — keep these handlers
  on the port-80 server, never behind the HTTP→HTTPS redirect, or detection
  breaks. `canary.local` itself resolves via mDNS on the AP netif plus the
  setup DNS hijack for non-`.local` lookups.
- **Date learned:** 2026-05

### iOS offers to *invent* a password for a Wi-Fi key field
- **What happened:** On the setup portal, tapping the Wi-Fi password field
  made iOS cover it with its "strong password" suggestion sheet — it read the
  form as account sign-up. A real user tapped the suggestion and "generated"
  a brand-new password instead of retrieving their home Wi-Fi key, and the
  join then failed with credentials no router had ever seen.
- **Root cause:** `<input type="password">` with no autocomplete annotation:
  iOS's heuristics treat any password field on an unfamiliar page as a
  new-account credential and push the generator.
- **Fix:** For password-shaped fields that are NOT account credentials (a
  Wi-Fi key, a broker secret), use `type="text"` masked with
  `-webkit-text-security: disc` via a `.pw-masked` class, plus
  `autocomplete="off" autocapitalize="none" autocorrect="off"
  spellcheck="false"`. The Show/Hide toggle flips the class, never the input
  type (flipping to `type="password"` re-summons the generator). Applied to
  the canary dashboard's Wi-Fi field and the first-boot wizard
  (`securacv_setup_page.cpp`); copy the pattern to any future field like it.
- **2026-07 sweep:** the pattern now also covers every surface *outside* the
  firmware pages — the desktop Flasher's provisioning, hub-wizard and MQTT
  fields (`desktop/src/index.html`, which shipped a Wi-Fi key marked
  `autocomplete="new-password"`), the browser flasher's Wi-Fi and broker
  fields (`canary-local/assets/flash.js`), the WAP portal simulator
  (`canary-local/assets/wap-ui.js`), and the one firmware page that missed
  the first migration (`csi_mqtt.cpp` `/mqtt`). Both flashers additionally
  help users *retrieve* the real key instead of typing blind: the browser has
  wifi-memory recall, and the desktop app's "Use saved" button reads the
  OS's own store (macOS Keychain / NetworkManager) behind the system consent
  prompt. Guarded by `canary-local/tests/desktop_parity.test.js` ("no surface
  ever summons the OS password generator").
- **Date learned:** 2026-07

### Captive DNS redirector must answer A queries only — NODATA for AAAA/HTTPS
- **What happened:** Even with the per-platform probes, `canary.local` and the
  redirect resolved slowly or not at all on Android Chrome.
- **Root cause:** The setup DNS responder appended an **A record to every
  query regardless of QTYPE**. Android Chrome fires `AAAA` (type 28) and
  `HTTPS`/SVCB (type 65) lookups in parallel with the `A` query; replying to
  those with an A-record answer is malformed, so the client waits/retries
  instead of falling back to IPv4.
- **Fix:** Parse the question's QTYPE. Answer the redirect A record only for
  `A` (type 1) queries; for everything else return **NOERROR with ANCOUNT=0
  (NODATA)** so the client immediately falls back to its A lookup. QNAMEs in
  queries aren't compressed, so a simple label walk finds the QTYPE.
- **Date learned:** 2026-05

### The captive DNS redirector runs for the AP's lifetime, not just first boot
- **What happened:** After provisioning, a phone joining the always-on
  management AP (e.g. because home WiFi dropped) hit the disconnect again —
  the per-platform probe handlers existed but were never reached.
- **Root cause:** The probe domains (`connectivitycheck.gstatic.com`, etc.)
  only resolve to the device when the captive DNS hijack is running, and it
  was gated on `setup_wizard::is_active()` — true only on first boot. With no
  hijack, the probe never reached the device's 204 handler, so the OS saw "no
  internet" and disconnected.
- **Fix:** Start the DNS redirector whenever the AP comes up and service
  `dns_process()` in `loop()` unconditionally (it self-guards on
  `s_dns_running`). The first-boot *wizard* (landing gate + 15-min timeout)
  stays separately gated on `is_active()`.
- **Why it's safe:** The softAP doesn't NAT, so AP clients have no upstream
  regardless; hijacking all A queries to the device is the intended captive
  behavior, not a regression. The device's own outbound DNS is unaffected (it
  uses the STA's resolver, never the local port-53 listener).
- **Date learned:** 2026-05

### BLE active scanning starves the SoftAP WPA2 join during provisioning
- **What happened:** Joining the device's setup Wi-Fi was intermittently
  failing — the phone's "enter password" sheet looped instead of associating.
  It worked sometimes, failed sometimes: a flaky loop.
- **Root cause:** BLE Discovery's Nearby scanner runs a 5-second, ~99%-duty
  **active** scan (window 99 / interval 100, `ble_config.h`) pinned to the
  shared WiFi/BLE core, and the first burst fires at boot — exactly when the
  operator is first joining the AP. On the single 2.4 GHz radio, a scan burst
  that overlaps the phone's WPA2 4-way handshake starves the handshake frames
  and the association times out. The firmware already rates AP+STA+BLE as
  unstable and drops the AP once STA is up to escape it, but the AP must stay
  up *during* provisioning — so BLE scanning was the thing to hold back.
- **Fix:** BLE Discovery still `init()`s at boot (stack + subsystems up), but
  its radio activity (Opera advertising, Nearby active scanning, boot chirp) is
  deferred out of the join window — brought up from `loop()` once the SoftAP is
  actually torn down (gate on AP-**down**, not mere `WL_CONNECTED`: the AP is
  held for a grace window after the STA gets an IP so the phone can read the
  success card, and the scan must stay out of that handoff too). AP-only mode
  (AP permanent) starts after a settle; a normal device whose home Wi-Fi never
  comes up starts after a 5-min max-hold fallback rather than staying disabled
  forever. BLE stays on by default; it just doesn't transmit mid-join.
- **Regression check:** `provisioning_logic::ble_discovery_start_due` is a pure
  wrap-safe predicate with host-test coverage in `test_provisioning_logic.cpp`
  (AP-down → due; AP-up non-AP-only → held until the max-hold fallback; AP-only
  → held until the settle window).
- **Date learned:** 2026-07

### A setup AP's SSID and password must change together, or not at all
- **What happened:** A 7" Dash could not be onboarded from an iPhone. The glass
  showed a correct join QR and a correct `SecuraCV-XXXX` / password caption;
  the phone answered **"Unable to join the network"** — with no password
  prompt, so there was nothing to retype. Power-cycling, re-flashing and
  re-scanning the QR all made it worse rather than better.
- **Root cause:** The display's onboarding AP paired a **stable** SSID (the
  salted device pseudonym, deliberately per-unit and sticky) with a
  **per-session** password, re-rolled inside every `provision_run()`. A phone
  keys its saved networks on the SSID: once it has joined `SecuraCV-XXXX` it
  auto-rejoins with the password it stored, that handshake is refused, and iOS
  reports a generic join failure instead of re-prompting — it does not believe
  it needs to ask. Every remedy an owner reaches for rolls another password, so
  the failure was self-reinforcing and invisible from the device side (a
  refused association leaves no trace on the AP).
- **Fix:** Mint the setup password **once** and persist it (`ap_pass` in the
  `securacv` namespace), so the SSID/password pair the QR promises stays true
  for the life of the unit. It is still random per unit and derivable from
  nothing published; rotation bought no secrecy anyway, because the password is
  printed on the glass the whole time the AP is up. Plus a bounded on-glass
  hint: 45 s with the AP up and nothing ever associated names the one move that
  clears a stale saved network ("forget this network, then scan again"), since
  units already in the field have phones carrying a password that no longer
  exists.
- **Rule:** An ephemeral credential may only sit behind an ephemeral
  identifier. If the name is stable, the secret behind it has to be too —
  otherwise the client's own cache becomes the thing that locks the user out.
- **Corollary, and the reason the fix checks its own write:** "persisted" has
  to be *verified*, not assumed. `Preferences::putString` reports a failed
  write by returning 0, not by refusing, so an unchecked store looks identical
  to a durable one and would re-open this bug on the next boot with nothing to
  see. The password is read back, and when it genuinely could not be kept the
  SSID takes a per-session suffix — the pair keeps one lifetime either way. Nor
  can the fallback be "derive the secret from the stable id": that id is
  printed in the SSID, so deriving from it would put the key on the screen for
  anyone who never looked at the password.
- **Date learned:** 2026-08

### Wi-Fi bring-up must not write flash while an RGB glass is scanning out
- **What happened:** The 7" dash-family glass garbled right as the onboarding
  SoftAP came up (end of the wizard's hello beat), then reboot-looped.
- **Root cause:** Arduino Wi-Fi persistence is ON by default, so every
  `WiFi.mode()` / `softAP()` / `begin()` commits esp_wifi config to NVS. A
  flash write suspends the cache; the RGB panel's bounce-refill ISR is not
  IRAM-resident, so the panel underruns and visibly garbles for the write
  window (the same PSRAM-contention mechanism the bounce buffers exist for —
  display_dash.cpp). The display never reads esp_wifi's own store: credentials
  live in the `securacv` namespace via runtime_config.
- **Fix:** `WiFi.persistent(false)` before the FIRST radio call in both
  `provision_run()` and `wifi_init_or_reboot()` (it was previously set only
  inside the portal's join handler — after the AP was already up).
- **Regression check:** review rule — any new radio bring-up path on a
  dash-family build starts with `WiFi.persistent(false)`; unavoidable NVS
  writes (set_wifi_credentials on join success) stay rare and deliberate.
- **Date learned:** 2026-08

### A seeded credential key is honored whichever NVS TYPE wrote it
- **What happened:** A Nightstand 7 flashed WITH Wi-Fi baked in still booted
  into its setup wizard — the seed was in flash, valid, and ignored.
- **Root cause:** the flashers pick the seed scheme per product (`wifi_nvs`:
  string for the getString readers, blob for wap/canary) — but a stale flasher
  frontend (a cached tab from before the per-product plumbing, or a missing
  catalog field falling back to blob) writes BLOBS under the same keys.
  `Preferences::isKey()` is type-blind and `getString()` on a blob returns ""
  — which the present-but-empty rule (see #1365) faithfully honored as "open
  network with empty SSID" → placeholder → wizard.
- **Fix:** `load_credential` (display + vision + sense, all copies) falls back
  to `getBytesLength`/`getBytes` when the key exists but reads as an empty
  string — a blob under the key is the same human intent.
- **Fix, inverse direction (2026-08):** the blob-side loaders got the mirror
  fallback: `ScvNetworkManager::loadCredentials` (main canary) and the wap's
  `wifi_load_credentials` read the key as a string when it exists but
  `getBytesLength` is 0 — a string-typed seed is the same human intent too.
  The canary-ota reference (`wifi_sta_connect_from_nvs`) additionally looks up
  the standard `securacv` namespace (`wifi_ssid`/`wifi_pass`, string or blob)
  when its own `wifi` namespace is empty, before the Kconfig defaults.
- **Regression check:** `desktop_parity.test.js` pins the blob fallback in all
  four string-scheme loaders alongside the existing isKey assertion, and the
  string fallback in both blob-scheme loaders.
- **Date learned:** 2026-08

---

## Sensing & Signal Processing

### A sensor that measures received frames is only as alive as its frame supply
- **What happened:** CSI/WiFi sensing "didn't detect much at all" in the
  field. The presence orb sat on "Sensing…" or flickered noise-driven
  states regardless of who was in the room.
- **Root cause (three independent breaks):** (1) the CSI active probe —
  the module that gives paired devices a deterministic frame supply —
  was never init/start/processed anywhere (dead code behind a ✅
  feature row); ambient supply was ~10 Hz at best (AP beacons) and ~0
  in AP-only installs. (2) The feature math measured physics, not
  people: pooled variance ≈ static multipath fingerprint, raw
  cross-product "Doppler" ≈ the ESP32's random per-frame CFO, and the
  in-window "breathing FFT" tried to resolve 0.2 Hz inside 1 s of
  data (impossible; all 8 Goertzel coefficients were ≈511 ≈ DC). (3)
  The CSI→rf_presence fusion hook was never registered.
- **Fix:** wire the probe (10 Hz ESP-NOW broadcast; peers sense each
  other); AGC-normalized true-magnitude per-subcarrier temporal
  variance; CFO-canceling relative band rotation
  (Im(C_b·conj(C_tot))/|C_tot|²); breathing on a cross-window envelope
  ring (~64 s); register the fusion hook; refuse to tick presence
  modules on <2-frame windows (no data ≠ empty room) and surface the
  supply on the dashboard.
- **Regression check:** `tests_host/test_csi_features.cpp` — synthetic
  physics: static channel + random CFO ⇒ all-zero features; ±30 % AGC
  flicker ⇒ zero; moving scatterer ⇒ detected; 0.25 Hz envelope ⇒
  breathing bin 3 dominant; reset_history wipes the ring.
- **Two lessons for future sensing features:** (a) a pipeline can be
  green end-to-end and still be measuring nothing — write a synthetic-
  physics test that FAILS when the estimator is replaced with noise;
  (b) grep for `init(` before marking a module row ✅ — the probe
  shipped, compiled, and was even pause/resumed by the channel-hop
  code, without ever being started.
- **Date learned:** 2026-07

### A feature that only exists in dev builds fails silently in the field
- **What happened:** A user pressed their smoke alarm's TEST button next to
  a production Canary; nothing happened — no event, no log, no error. The
  device looked healthy the whole time.
- **Root cause:** `FEATURE_ACOUSTIC_EVENTS` (and the rest of the Phase 2
  sensing suite) was defined only in `[env:dev]` / `[env:full]`; the
  published `release`/`release_ha` OTA images compiled the microphone code
  out entirely. Nothing at runtime can report the absence of code.
- **Fix:** the sensing suite ships in `[env:release]` (inherited by
  `release_ha`/`standalone`); the bench-test doc
  (`docs/hardware/acoustic_alarm_bench_test.md`) starts with a "does your
  build even have the feature" check.
- **Regression check:** FEATURES.md dashboard guard covers variant-level
  drift; the size guard keeps the grown release image inside the A/B slot.
- **Date learned:** 2026-07

### Envelope detectors need DC removal, and cadence timing needs a stream clock
- **What happened:** Even on builds WITH the mic code, bench cadences could
  fail to match: a PDM DC offset inflated the "loudness" the hysteresis saw
  (potentially pinning it ON forever), and beep/gap durations were stamped
  with `millis()` at *processing* time, so any main-loop stall (TLS, NVS,
  OTA check) compressed a burst of queued DMA frames into one instant.
  Amplitude-only matching also meant any 3-slams-and-quiet rhythm could
  read as a smoke alarm.
- **Root cause:** sum-of-squares RMS folds the DC bias in; wall-clock
  stamping measures when the CPU got around to the frame, not when the
  audio happened; and a purely temporal template has no spectral evidence.
- **Fix:** DC-removed RMS (`E[x²]−E[x]²`); a sample-stream clock
  (frames × frame_ms) for all envelope/cadence math; a stage-1 alarm-band
  tone gate (3.4 kHz biquad, the 3.0–4.0 kHz band UL sounders use) that
  T3/T4 beeps must pass; DMA ring deepened 4→8 buffers.
- **Regression check:** host tests `test_audio_cadence.cpp` — DC segment
  must produce zero transitions, an off-band (500 Hz) T3-timed cadence must
  NOT match, and T3 must still match with a frozen wall clock.
- **Date learned:** 2026-07

### A self-test must relax the SAME gate that fails in the field — and clean up when its window closes
- **What happened:** The dashboard's "Test with your alarm (30 s)" failed
  with "No alarm cadence heard (0 sound transitions seen)" while the badge
  said LISTENING. The user couldn't tell a dead mic from a quiet room from
  an alarm that was simply too far away — and neither could the firmware.
- **Root cause (three independent gaps):** (1) self-test relaxed the
  cadence *timing* tolerance and the tone-gate floor, but not the envelope
  ON/OFF thresholds — the very gate that produces "0 transitions". A UL
  sounder's TEST press at ~3 m lands near RMS ~600, under the default
  ON=800, so a perfectly working mic still failed the test it recommends.
  (2) Nothing distinguished "mic delivers exact zeros forever" (dead data
  line) from silence — a live PDM mic's noise floor never computes RMS==0
  for 30 s straight. (3) The one host test covering this pipeline existed
  but was never wired into CI, so none of this was ever exercised.
- **The leak nobody promised:** self-test suppresses the event callback
  *while active* — but a TEST press ending just before the window expired
  left its beeps in the transition ring, and the NORMAL matcher completed
  the match during the post-expiry pause and fired the callback into HA
  smoke automations. Suppressing a consumer is not enough; the *evidence*
  gathered under test conditions must not outlive the test.
- **Fix:** self-test halves the envelope thresholds (with floors) for its
  window; the window edge wipes the transition ring + envelope FSM;
  `peak_rms` in the self-test status + `zero_rms_streak` in stats let the
  UI say "heard nothing at all (hardware)" vs "heard faint sound, peak N,
  needs M — move closer" vs "sound but no alarm cadence"; CI now runs
  `test_audio_cadence` (17 tests, incl. one that FAILS if the window-edge
  wipe is removed).
- **Two lessons:** (a) when a self-test exists to diagnose a failure mode,
  relax/instrument the exact stage that produces that failure's symptom —
  a test that can't hear what it asks the user to play is worse than none;
  (b) "suppressed during the test" guarantees need a plan for state that
  *straddles* the test boundary.
- **Regression check:** `test_audio_cadence.cpp` — quiet (RMS 600) T3 must
  match under self-test but produce zero transitions in normal mode; a
  burst ending at the window edge must emit NO event; a flat signal must
  pin `zero_rms_streak` past `AUDIO_SILENT_STREAK_FRAMES` with peak_rms 0.
- **Date learned:** 2026-07

---

### An FSM's "no data" and "bad data" are different inputs — conflating them made the vitals lock unreachable
- **What happened:** The canary-sense breathing/heart lock (`VitalsFSM`)
  could never reach `Locked` against realistic radar traffic. The Sense Lab
  bench (`canary-local/senselab.html`), which streams the real frame mix
  through a line-for-line JS port of the FSM, showed `breathing_locked`
  stuck false forever at Seeed's own reference bedside geometry.
- **Root cause:** `tick()` computed `valid = single_target &&
  plausible(frame)` for EVERY frame, and `plausible()` requires
  `kind == Vitals`. But the MR60 wire interleaves presence/count/distance
  frames between 1 Hz vitals reports, and `loop()` also ticks the FSMs with
  an empty frame when nothing arrived — so `was_valid_` flipped false and
  the `valid_since_ms_` confirm window restarted on every interleave. The
  4 s lock-confirm could never elapse. The host test passed because it fed
  back-to-back vitals frames only — a stream real hardware never produces.
  The sibling `PresenceFSM` had the correct data guard all along
  ("a None/other frame is normal — leaves debounce timers running").
- **Fix:** `mr60_vitals.cpp` data-guards non-vitals frames after the
  deadline check: they no longer touch the valid-run bookkeeping. Loss
  stays deadline-driven (`lock_lost_ms`), multi-person suppression stays
  immediate (`bpm_valid` re-checks `single_target` on every tick, including
  non-vitals ticks).
- **Regression check:** `test_vitals_lock_survives_interleaved_presence`
  (host) and its JS twin in `canary-local/tests/senselab.test.js` both stream
  the realistic mix: 10 Hz presence + empty ticks + 1 Hz vitals. Rule of
  thumb pinned: when writing FSM integration tests for a multiplexed wire,
  feed the full frame mix, never a single-type stream.
- **Date learned:** 2026-07

---

## Portability

### A GPIO→peripheral-channel table is per-silicon, and only a second target proves it

- **What happened:** The first classic-ESP32 ports (`esp32cam`,
  `esp32-wroom`) failed to compile in `securacv_power.cpp`:
  `'ADC1_CHANNEL_8' was not declared in this scope`.
- **Root cause:** `gpio_to_adc1_channel()` was a flat table written against
  the XIAO — GPIO1-10 → ADC1_CHANNEL_0-9, which is the ESP32-S3 layout.
  ADC1 has ten channels on S3/S2, five on C3, and eight on the classic
  ESP32, where they live on GPIO32-39 in non-ascending order. The table had
  been correct-by-luck for three years because every board in the tree was
  S3 or C3, and the C3 boards never enable the battery monitor.
- **Fix:** the mapping is now `#if defined(CONFIG_IDF_TARGET_*)`-branched,
  with the S3 table untouched and classic-ESP32 / C3 tables beside it. The
  `default:` arm stays channel 0 on every target — that's the "board
  declared no usable VBAT pin" path, and it is safe because a reading
  outside the divider-detect window already reports USB-only rather than
  inventing a battery.
- **The general rule:** anything that maps a GPIO number to a peripheral
  channel (ADC, touch pad, RTC/LP GPIO, DAC) is silicon-specific data, not
  portable logic. When adding a target, grep for these tables before
  trusting a build — and prefer a compile error (which this was) to a
  silent wrong-channel read, which is what the `default:` arm would have
  given us if the channel had merely been out of range instead of
  undeclared.
- **Regression check:** the classic-ESP32 envs now build on every PR
  (`flavors.json` → `build-platformio`), so any new S3-shaped table fails
  CI on the target that disproves it.
- **Date learned:** 2026-08

### A feature flag no *built* env sets to 0 is an untested branch

- **What happened:** The `esp32-wroom` port failed to compile:
  `'storage_is_mounted' was not declared in this scope` in `main.cpp`'s
  serial `T` (run-all-tests) handler.
- **Root cause:** `securacv_storage.h` gates its whole API behind
  `#if FEATURE_SD_STORAGE`, and that call site wasn't guarded. The bug was
  years old and reachable the whole time — `[env:minimal]` sets
  `FEATURE_SD_STORAGE=0` — but `minimal` is not in `flavors.json`'s
  `build_envs`, so CI never compiled it. A board with no SD slot was the
  first env that both turned the flag off *and* got built.
- **Fix:** guard the call and print "storage not compiled in" on the
  else-branch, matching the `FEATURE_DIAGNOSTICS` pattern a few lines up.
- **The general rule:** a `#if FEATURE_X` header gate is only as good as
  the envs that exercise `FEATURE_X=0`. If no env in `flavors.json` builds
  a flag's off-state, its call sites are unverified by construction — which
  is an argument for the board ports carrying honest, *different* feature
  postures rather than all-on copies of the flagship.
- **Regression check:** the classic-ESP32 envs (SD off, camera off, mic
  off, touch off) now build on every PR, so the common off-states have a
  compiler watching them. A quick audit script for unguarded calls to
  gated symbols is in this PR's history if it's ever wanted as a lint.
- **Date learned:** 2026-08

---

## Memory Budget

### Internal-DRAM statics are the lever for the BLE budget — and nm lies about where they live

- **What happened:** The FULL/S3 build could never start Bluetooth: ~40 KB
  free internal RAM at the BLE gate against a ~96 KB need. Flash-side
  compression (the gz web assets) couldn't help — the ELF showed 158 KB of
  the 320 KB internal DRAM bank consumed by static globals before a single
  heap allocation, and that, not flash, is the scarce resource.
- **Root cause:** Multi-KB buffers (14 KB health-log ring, 11.5 KB
  daily-summary scratch, 10 KB CSI amplitude history, 2 x 2.5 KB fleet-scan
  buffers) were declared as file/function statics out of habit, parking
  them permanently in internal DRAM even though every one of them is only
  touched from task context and is PSRAM-safe. A second trap: auditing with
  `nm` section letters counts flash as RAM — ESP-IDF marks `.flash.rodata`
  writable, so `nm` reports flash-resident const tables (including the gz
  web assets) as 'D'. Filter by the S3's DRAM address window
  (0x3FC88000..0x3FD00000) instead.
- **Fix:** `csi_mem.h::csi_large_calloc()` — PSRAM-first, internal-heap
  fallback, NULL disables the owning feature (fail-safe; a C3 without
  PSRAM just recreates the old footprint). The wave-1 buffers above became
  pointers allocated at the top of `setup()` (before the first
  `log_health`) or lazily on the owning task, reclaiming ~41 KB of
  internal heap for the BLE budget. Rule of thumb going forward: any
  task-context-only buffer over ~1 KB gets `csi_large_calloc`, not a
  static array.
- **Regression check:** The RAM Audit workflow
  (`.github/workflows/ram_audit.yml`) builds the shipped FULL/S3 image,
  prints the address-filtered DRAM symbol ranking, and FAILS if any
  wave-1 buffer reappears in the internal-DRAM window at >= 1 KB.
- **Date learned:** 2026-07

### Phantom "/sd/" path prefix silently killed every write behind it
- **What happened:** The witness/chain/export data-management layer
  (`data_mgmt_api.h`) addressed `/sd/WITNESS`, `/sd/CHAIN/backup.bin`,
  `/sd/EXPORT`, … while the mount path provisioned root-level `/WITNESS`,
  `/CHAIN`, `/EXPORT`. The hourly HMAC'd chain backup, the export-bundle
  write, and the export rotation all targeted directories that never
  existed — every one failed (the backup loudly, hourly; the rest
  quietly).
- **Root cause:** The Arduino-ESP32 `SD` library already roots paths at
  the card (its VFS mount point is prepended internally), so `"/sd/X"`
  addresses a literal `sd/` SUBDIRECTORY on the card — and FAT `mkdir`
  cannot create intermediate directories, so `SD.mkdir("/sd/CHAIN")`
  failed on the missing parent. The `/sd/` spelling was copied from the
  ESP-IDF-style mount-point convention in the (stubbed, never-compiled)
  `sd_storage.h`.
- **Fix:** Root-level paths everywhere (`/WITNESS`, `/HEALTH`, `/CHAIN`,
  `/EXPORT`) matching the directories the mount path actually creates.
  Separately, `create_witness_record()`'s SD branch — which had only ever
  incremented a counter — now really appends each signed record to the
  append-only `/WITNESS/records.jsonl` (beacon-audit two-tier pattern),
  and boot reconciles the NVS chain-head cache against the SD tail
  (SD wins only when strictly ahead AND the tail signature verifies
  under this device's key).
- **Regression check:** `test_witness_store_logic.cpp` pins the jsonl
  line format, tail recovery, and the SD-wins decision in CI. Paths are
  now the same strings the mount path mkdirs, so a future drift shows up
  as a failed write in bench `[HEALTH]` output immediately.
- **Date learned:** 2026-07

### LVGL 9 group-opa fades composite through the LV_MEM pool — a full-screen fade is a ~22 KB-per-frame allocation
- **What happened:** The 7" glass (LVGL 9.5 dash family) panicked/halted right
  as the onboarding wizard's scenes changed — garble, then a reboot loop; the
  join QR never rendered. The LVGL 8.4 emulator twin could not reproduce it
  (provisioning is stubbed there, and v8 doesn't have the mechanism anyway).
- **Root cause:** in v9, animating style `opa` on a container composites the
  whole subtree through intermediate layer buffers drawn from the SAME
  `LV_MEM_SIZE` pool as every widget (`lv_refr.c` chunk layers,
  `LV_DRAW_LAYER_SIMPLE_BUF_SIZE` = 24 KB, ARGB8888 for a transparent-bg
  container). The wizard faded a full-screen container while TWO 800x480
  screens (bedside face + onboarding) plus the QR draw buffer already sat in
  the 64 KB pool. On exhaustion v9 either halts in `LV_ASSERT_MALLOC`'s
  default `while(1)` (silent with `LV_USE_LOG 0`), NULL-derefs in
  `lv_refr.c`'s unchecked `lv_draw_layer_create`, or livelocks in
  `draw_buf_flush` — all three end as a watchdog/panic reboot. v8 applies
  group opa per-draw with no compositing, so the same code was cheap there.
- **Fix:** the wizard's scene fade animates the labels' `text_opa` (per-part
  opa draws directly on both majors, no layers); the finish handoff cuts
  instead of screen-load-fading on v9. Rule of thumb: on a v9 800x480 build,
  style `opa` animations on containers are memory events, not style tweaks —
  fade per-part properties (`text_opa`, `bg_opa`, `arc_opa`, `image_opa`)
  or don't fade.
- **Regression check:** none automatable on host; grep the dash-family UI for
  `lv_anim` + plain-`opa` exec callbacks when touching scene transitions.
- **Date learned:** 2026-08

---

## Display: what the glass actually shows

### A QR widget can exist and still hold nothing — prove it rendered before you present it

- **What happened:** The Settings "get help" page created its Help Desk QR
  with the shared `mk_qrcode` + `lv_qrcode_update` recipe and showed a
  "scan" caption unconditionally. Under memory pressure on LVGL 9 that can
  present a blank white card with an inviting caption — on the exact page a
  person opened because something was already wrong. Caught in review on
  the page's first PR (#1567); `onboard_ui.cpp` had already learned the
  same lesson and encoded the full check.
- **Root cause:** two silent failure modes stack: the v9 `lv_qrcode_create`
  path can return a canvas whose draw-buffer allocation lost (widget
  exists, no pixels behind it), and `lv_qrcode_update` reports its own
  verdict that is easy to ignore because the v8 call "always worked".
- **Fix:** every QR presenter proves the render: null-check the widget, on
  v9 check `lv_canvas_get_draw_buf() != NULL`, and take the update result.
  On any failure, hide the card and degrade to the payload as plain text —
  for the help page, the URL itself with "type it into any browser".
- **Regression check:** none automatable on host (needs LVGL's allocator
  under pressure). The rule is mechanical in review: a call to
  `lv_qrcode_update` whose result is unused is the bug.

### The emulator's square canvas hides round-glass clipping — geometry must be an engine
- **What happened:** The watch face laid out on the full 240x240 canvas with
  hand-tuned offsets, and everything looked right in the emulator and in
  screenshots. On the physical round display the corners of that canvas do
  not exist: list rows near the top and bottom ran past the circle's chord
  and the glass cut them mid-character, and a settings back-line at y=16
  (where the chord is 98 px) lost a third of its text. A separate print-time
  cap (`%.24s`) then mangled "restricted zone" to "restricted zon" even on
  rows with room for the whole phrase.
- **Root cause:** no single owner for the circle. Each face respected the
  disc by local discipline — centered labels, "packs tighter" comments, an
  inscribed-square derivation in a comment — and every reviewer surface
  (emulator canvas, screenshots, square dev panels) renders the corners the
  product doesn't have, so nothing ever showed the loss.
- **Fix:** the Round Frame engine (`include/canary/ui/round_frame_core.h`,
  pure host-tested integer geometry; `round_frame.h/.cpp` the LVGL fit
  layer). A label near the rim gets the width its latitude honestly offers
  and ellipsizes instead of spilling; list pages ride equator-centered row
  stacks; QR budgets static_assert against the engine's inscribed square.
  Print-time caps were loosened to buffer bounds — visual truncation is the
  engine's job, and its ellipsis is honest.
- **Regression check:** `tests_host/test_round_frame_core.cpp` pins the
  exact chord/stack/square values the glass computes. The rule going
  forward: never hand a face a new magic number for the circle — extend the
  engine and its test instead.
- **Date learned:** 2026-08

### Re-addressing a panel is not repainting it — a rotation leaves the old frame behind

- **What happened:** A Nightlight on a bedside table showed thin yellow
  streaks beside the canary and white ones beside the clock digits, and they
  stayed there. Not tearing, not a camera artifact: leftovers of the previous
  orientation's image.
- **Root cause:** `display_set_rotation` wrote MADCTL and
  `lvgl_port_set_panel_rotation` reshaped LVGL's canvas, but nothing repainted
  the ST7789's frame memory. It still held the pre-rotation image, now read
  out along transposed axes, and LVGL only flushes regions it believes are
  dirty — so every pixel the rebuilt face didn't happen to cover survived.
  The bird's yellow and the digits' white are the brightest things on the old
  frame, so they are what a person actually sees.
- **Fix:** the HAL clears the frame on rotation (it is invalid by definition),
  and the port invalidates the whole screen instead of trusting a dirty list
  whose coordinates were measured on the other axis. The dash's
  software-rotation path had the same hazard against its scanned framebuffer
  and got the same full invalidate.
- **Regression check:** none automatable on host — it needs a panel. When a
  code path changes the display's coordinate space, clear and fully
  invalidate; a partial-render pipeline has no other way to know.
- **Date learned:** 2026-08

### LVGL draws a hollow box for any glyph outside the built-in font's range, silently

- **What happened:** Every date line on the glass read "Sunday [] Aug 9".
- **Root cause:** the faces used U+00B7 MIDDLE DOT as a separator. LVGL's
  built-in Montserrat is generated over `0x20-0x7F,0xB0,0x2022` (see the
  generator options in the header of `lv_font_montserrat_*.c`). U+00B7 is not
  in it. There is no fallback and no build error — LVGL renders the
  missing-glyph box. It sits one codepoint away from the degree sign that IS
  in range, which is why it looked safe, and the WASM emulator hid it because
  a browser has real fonts.
- **Fix:** U+2022 BULLET is in range and does the same job. The same sweep
  caught U+2026 in two "unknown value" placeholders and the em dashes the
  Canary Cards used for a null reading; those became plain ASCII.
- **Regression check:** `firmware/scripts/check_display_glyphs.py`, wired into
  `firmware.yml`. String literals in the faces and the code feeding them must
  stay inside the font's range. Serial-log and `#error` text is exempt — it is
  read where real fonts exist.
- **Date learned:** 2026-08

### A tool that interprets escape sequences turns "\xE2\x80\xA2" into mojibake on the way through

- **What happened:** three settings-sheet strings that were written with the
  BULLET spelled as the hex escapes `\xE2\x80\xA2` reached the file as the
  six-byte sequence for `â` + U+0080 + `¢` — mojibake that the glyph guard
  (correctly) failed three CI jobs on.
- **Root cause:** the edit was piped through a scripting language whose own
  string parser consumed the `\xNN` escapes as *its* escapes and wrote the
  decoded characters into the C++ source, instead of the twelve ASCII
  characters the C compiler was meant to see. Any templating or heredoc layer
  with C-style escape syntax has this failure mode; the diff even LOOKS right
  in viewers that render the decoded bytes.
- **Fix:** write the literal backslash sequences with the escape layer
  defused (raw strings, single-quoted heredocs, or editing tools that do no
  interpolation), then byte-check the result: the three faces' UI sources
  should contain no bytes above 0x7F inside string literals.
- **Regression check:** `firmware/scripts/check_display_glyphs.py` already
  catches it — U+00E2/U+0080/U+00A2 are outside the font range. Trust it
  locally before pushing: it is the same gate CI runs.
- **Date learned:** 2026-08

---

## Network API: what a LAN token does and does not prove

### A per-boot token any LAN host can read is not authentication for an egress switch
- **What happened:** The display's `POST /api/set` guarded every knob the same
  way: an Origin allowlist plus a per-boot CSRF token that `GET /api/settings`
  hands to the page. That stops a cross-site browser, which is what the guard
  was built for. It does not stop a host already on the home Wi-Fi, which can
  read the token from `/api/settings` and post it back — and among the knobs
  behind that guard were `wx_direct`, the glass's one opt-in outbound path,
  and `wx_loc`, the coarse location that fetch carries. Zero phone-home was a
  principle a neighbor on the same network could flip.
- **Root cause:** One write guard for two different classes of setting. Comfort
  knobs (brightness, look, night behavior, `/api/tz`) can live behind
  "the LAN is the trust boundary". A setting that opens an outbound connection
  or stores a location cannot, because the LAN *is* the party the guard has to
  keep out, and the glass mints no credential that would tell an owner's phone
  from any other host.
- **Fix:** A key-class table (`include/canary/net/settings_policy.h`, Arduino-free,
  host-tested) names the on-glass-only keys; `handle_settings_set` refuses them
  for every caller, token or not, with `403 {"ok":false,"err":"on_glass_only"}`
  before the Origin/CSRF gate is consulted. The switch stays where it already
  was: on the glass (settings → weather → fetch itself). `/api/settings` serves
  the class under `on_glass` so no client draws a control that would fail, and
  serves the location-derived facts only to same-site callers. Deliberately
  not a bearer token and not a button-confirm: the first would mint a
  credential the glass has no way to hand out, the second needs a UI change
  and the emulator-dist rebuild that comes with it.
- **Regression check:** `tests_host/test_settings_policy.cpp` pins the class
  (every egress and location key true, every comfort knob false, unknown
  false) and runs in `firmware.yml`. When adding a settings key, decide its
  class first: if it opens a connection or stores a place, it goes in the
  table, and the web page shows it read-only with the on-glass path.
- **Date learned:** 2026-09

## Build profiles: a configuration nobody compiles is a configuration that rots

### Every offered BUILD_PROFILE and hardware target needs its own CI leg
- **What happened:** `build_config.h` has offered `BUILD_PROFILE_MINIMAL`,
  `BUILD_PROFILE_DEV`, and `HARDWARE_XIAO_ESP32C3` as first-class choices
  since it was written — and every CI leg compiled `FULL` on the S3. A
  tamper-module include then landed inside a `#if FEATURE_VAULT_SNAPSHOT`
  block while its registration call sat outside any fence: `DEV`, `MINIMAL`,
  and C3 builds all broke, invisibly, because nothing ever compiled them.
  Adversarial review caught it; CI should have.
- **Root cause:** The feature matrix's claims and CI's coverage were two
  different sets. A profile-gated `#if` is only as tested as the profiles
  CI actually builds — the compiler cannot warn about a branch it never
  enters.
- **Fix:** `firmware.yml` grew compile-only legs for `BUILD_PROFILE_DEV`
  (S3) and `BUILD_PROFILE_FULL` on the claimed `XIAO_ESP32C3` FQBN — the
  combination `check_route_budget.py` already models. The DEV leg's first
  run immediately caught twelve mesh auth wrappers defined outside the
  `FEATURE_MESH_NETWORK` fence their handlers and registrations live in:
  years of invisible rot, found in the first hour of coverage.
- **Regression check:** The legs themselves ("Compile firmware (DEV profile
  gate)" / "(FULL profile, claimed C3 target)" in `firmware.yml`). When
  `build_config.h` gains a new profile or hardware target, add its leg in
  the same change — an unbuilt configuration is a false promise.
  `BUILD_PROFILE_MINIMAL` is still compiled by nobody and is a declared
  gap: the sketch fences the HTTP server's startup, not its handler
  bodies, so a no-WiFi/no-HTTP/no-SD build needs a fencing repair first.
- **Date learned:** 2026-09

## How to Add an Entry

When you encounter a bug, regression, or hard-won lesson:

1. Add it to the appropriate section above
2. Include: what happened, root cause, fix, and regression check (if any)
3. If it can be automated, add a check to `firmware/scripts/regression_check.sh`
4. Date it so we know when it was learned

**Format:**
```markdown
### Short description
- **What happened:** The symptom or failure
- **Root cause:** Why it happened
- **Fix:** What solved it
- **Regression check:** How we prevent it from recurring
- **Date learned:** YYYY-MM
```
