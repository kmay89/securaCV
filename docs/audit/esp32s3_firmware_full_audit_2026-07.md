# ESP32-S3 Firmware — Full Audit & RF-Sensing Capability Study

**Date:** 2026-07-19
**Scope:** The ESP32-S3 firmware in this repo (primarily `firmware/projects/canary-wap/`
plus the shared `firmware/common/csi/` and `firmware/canary/lib/` libraries), its WiFi /
Bluetooth / WAP / mesh / CSI / RF-sensing subsystems, and what the Seeed XIAO ESP32-S3 (Sense)
hardware can and cannot do.
**Method:** Static read of the firmware, cross-checked against Espressif primary sources
(ESP-IDF, SoC capability headers, datasheets), the Seeed wiki, and the WiFi-sensing / BLE-tracking
literature. Every load-bearing "can we / can't we" fact was adversarially verified against primary
sources; the top three high-impact firmware findings were re-confirmed line-by-line before
inclusion. Hardware behaviour was **not** bench-validated (no board in the loop) — claims tagged
*bench-unverified* rest on code reading, not observation.

> Companion: the board-definition corrections from the Seeed documentation comparison shipped
> separately in PR #905 (LED polarity, GPIO26–37 PSRAM reservations, OV3660 camera, 50 mA charge
> current, camera/SD bus constraints). This document is the firmware + capability audit.

---

## 1. TL;DR

**The headline surprise: almost everything you asked about is already built.** This is not a
"we need to add BLE item-finding and CSI sensing" situation — the `canary-wap` ESP32-S3 firmware
already contains a WiFi-CSI motion/breathing/presence pipeline, a 50 Hz CSI active probe, a BLE
"Scout" paired-beacon room-presence tracker, an ESP-NOW "Opera" mesh with channel-hop + hub
election, RF/WiFi device presence, empty-room auto-calibration, and a layered self-healing/OTA
stack. The engineering quality of the *primitives* is high — well above typical hobbyist ESP32 code.

**The real problem is the gap between what is coded and what actually runs.** Several flagship
features are **coded, unit-tested at the primitive level, and then never wired to a caller**, so
they are inert in shipping firmware:

| Feature you asked about | Status | Reality |
|---|---|---|
| Locate a Bluetooth item you paired | **Inert** | `ble_scout_pair()` has no production caller (only tests); `FEATURE_BLE_SCAN` defaults off. The registry is always empty, so the scout emits nothing. |
| Clever always-self-healing mesh | **Cannot form on hardware** | The Opera pairing ECDH feeds Ed25519 keys into X25519 → both sides derive different session keys → no opera can pair on a real device. And there is no active reconnection path once paired. |
| CSI multi-node fusion (the real ceiling-raiser) | **Dormant** | `core_multilink_fusion` is fully written but nothing transmits/receives `CSI_FEATURES` between nodes, so its only event can never fire. |
| Self-healing home WiFi | **Split** | The WAP does it well (AP fallback, retry forever). The `sense`/`vision` builds "self-heal" by **rebooting** on outage — an anti-pattern for a local-first witness. |
| Bad-OTA auto-rollback | **Likely inert** | The bootloader rollback path is `#if`-gated on `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, which is set nowhere in the repo; the stock Arduino build compiles it out. |

**The hardware truth (all verified against Espressif primary sources):** the ESP32-S3 is a strong
CSI room-sensor and a fine BLE-proximity node, but it has hard ceilings you cannot firmware your
way past — **no BLE 5.1 direction finding (AoA/AoD), no 802.15.4, no UWB, a single time-shared
2.4 GHz radio, and Wi-Fi-4 (not Wi-Fi-6) CSI.** "Point me to my keys with an arrow" and "sub-meter
locate" are **not** ESP32-S3 firmware features — they need added silicon (UWB / an AoA array) or a
different SoC. Everything achievable on the S3 alone, this firmware already attempts; the value is
in **finishing and verifying** it, not adding more.

---

## 2. Hardware ground truth (verified capability matrix)

Every row was adversarially verified against Espressif's own datasheets, SoC capability headers,
and ESP-IDF docs. Verdicts: **CONFIRMED** = the limitation/capability is real; **REFUTED** = the
common belief (or a repo claim) is wrong.

| # | Claim | Verdict | What it means for this product |
|---|---|---|---|
| 1 | S3 supports WiFi CSI via `esp_wifi_set_csi_rx_cb()` in stock ESP-IDF | **CONFIRMED** | The whole CSI room-sensing stack is sound on the reference board; no custom driver or chip swap needed. The Arduino prebuilt libs ship `CONFIG_ESP_WIFI_CSI_ENABLED=y`. |
| 2 | S3 does **not** support BLE 5.1 Direction Finding (AoA/AoD / CTE) | **CONFIRMED** | No bearing/angle to a BLE item is possible on the S3 — RSSI proximity only. Espressif's SoC header has no `SOC_BLE_CTE_SUPPORTED`; the ESP-FAQ says plainly the S3/C3/C6 don't do AoA/AoD. |
| 3 | S3 has no 802.15.4 (no Thread/Zigbee) and no UWB | **CONFIRMED** | Sub-meter tag ranging needs external hardware (Qorvo DW3000 UWB) or dense multi-node RSSI (still ~1–3 m). Thread/Zigbee would need a C6/H2 co-processor. |
| 4 | Single 2.4 GHz radio shared by WiFi+BLE; heavy STA+SoftAP+BLE+CSI can't all run at full duty | **CONFIRMED** | The design must budget airtime. CSI is not a separate consumer — it *is* WiFi RX, so CSI frame rate degrades exactly when BLE/AP steal slices. |
| 5 | 802.11bf sounding is exposed on the C6 but not the S3 (repo `CSI_CAP_SOUNDING_11BF` C6-only) | **REFUTED** | **No ESP32, C6 included, exposes IEEE 802.11bf in ESP-IDF.** The C6's *potential* edge is 802.11ax HE-LTF CSI (richer than HT-LTF). ESP-IDF v5.5+ *does* expose C6 HE-LTF CSI acquisition (`acquire_csi_su`/`mu`/`dcm`/`beamformed`), but **this firmware** configures the legacy CSI fields and caps ingest at `CSI_MAX_SUBCARRIERS`=128, so the ~242-tone path is not available here — the gap is this repo's HAL, not ESP-IDF. The repo bit is never set by `get_caps()` on any target. *(README corrected.)* |
| 6 | S3 can be an Apple Find My *tag* but not a *finder*, and gets no UWB precision | **CONFIRMED** | OpenHaystack-style firmware makes it findable; it can never locate arbitrary AirTags (rotating keys + Apple-only finder role + private-key-encrypted reports). It *can* passively detect nearby trackers (anti-stalking). |
| 7 | ESP-NOW requires all peers on the same WiFi channel; the home AP dictates that channel | **CONFIRMED** | The mesh doesn't pick its own channel when joined to home WiFi. If the router auto-hops channels, links break until re-sync. The repo already handles this correctly with `channel=0` peers + `mesh_channel_policy`. |

Primary sources for the above: ESP32-S3 datasheet; ESP-IDF `soc_caps.h` (esp32s3 / esp32h2);
ESP-IDF Wi-Fi CSI, coexistence, ESP-NOW, and BLE feature-support docs; ESP-FAQ BLE AoA/AoD note;
IEEE 802.11bf-2025 (published 2025-09-26); OpenHaystack; Apple Find My security guide. Full URL
list in §10.

---

## 3. Subsystem audit

Each subsystem is rated **bench-verified** / **host-tested** / **compile-tested** / **scaffold**,
with the load-bearing findings. File paths are under
`firmware/projects/canary-wap/arduino/canary_wap/` unless noted.

### 3.1 Home-WiFi STA + self-healing + provisioning — *mixed*

The `canary-wap` sketch is a **bench-grade** provisioner: device-unique WPA2 SoftAP, captive-portal
DNS with per-OS probe handling, non-blocking STA join, and — the clever part — it **drops the
SoftAP once the STA link is stable** (to run the Espressif-rated-stable STA+BLE combo on the single
radio) and **auto-re-raises it on STA loss**. It never reboots for WiFi; it retries forever while
staying reachable at `canary.local`.

The modular `sense`/`vision`/`display` trees share a much simpler polled supervisor
(`src/net/wifi_mgr.cpp`): exponential backoff 2→30 s, then **`ESP.restart()` after 5 min of outage**,
and a blocking boot-connect that **reboots** if it can't associate within 30 s — *before* the
sensing loop starts.

- **[HIGH] Reboot-as-recovery defeats the local-first witness.** `wifi_mgr.cpp` reboots on
  sustained outage and makes WiFi a hard boot dependency. A router down at power-up → endless
  30 s-connect→reboot loop where the device *never senses*. The code explicitly says MQTT-down must
  not block sensing, yet WiFi-down does. The WAP's graceful degradation is the model to copy.
- **[MEDIUM] Two uncoordinated reconnect engines in the WAP** (`WiFi.setAutoReconnect(true)` +
  a manual 30 s re-`begin()` that can interrupt the SDK's in-flight reconnect).
- **[MEDIUM] `canary-ota` `wifi_sta.c` gives up permanently after 5 back-to-back retries** (no
  backoff, no re-arm) — a transient AP reboot during the OTA window kills the link for good.

Best-practice gaps (all feasible on the S3): consume `WIFI_EVENT_STA_DISCONNECTED` reason codes
instead of polling; store **multiple known APs** and pick best-RSSI (`WiFiMulti`); treat
`IP_EVENT_STA_LOST_IP` as a first-class health signal (a stale-DHCP-lease-after-router-reboot
currently reads as "connected"); add backoff **jitter** so a fleet doesn't reconnect in lockstep;
optionally enable 802.11k/v/r roaming (ESP-IDF sdkconfig, opt-in for large homes).

### 3.2 WAP / SoftAP / web server / captive portal — *mixed (bench-informed)*

Genuinely well-engineered: WPA2 SoftAP with a device-unique password HMAC'd from the **private**
key (fixed an older publicly-recomputable scheme), Bearer-token auth with constant-time compare +
exponential backoff, an `HttpOnly`/`SameSite=Strict` session cookie, a CI gate
(`check_route_security.py`) that fails the build if any route is un-gated, graceful camera-peek
degradation, and TLS that persists to NVS and falls back rather than bricking.

- **[MEDIUM] Trust-on-LAN: any host on the same L2 can mint a full session with no device secret.**
  `GET /` embeds a fresh pair token; `GET /?cv_pair=<token>` sets a full-auth session cookie
  (camera preview, witness listing, config, reboot, WiFi cred change). On the SoftAP this is
  WPA2-gated, but once on home WiFi the same handler is served on the STA interface — a compromised
  IoT gadget or roommate on the LAN can pair. Deliberate posture, but a passwordless live-camera
  path to every LAN host is a real residual risk for a custody-first device.
- **[MEDIUM] Port-80→HTTPS redirect hardcodes `192.168.4.1`** (`handle_https_redirect`), so once
  the AP is dropped (normal home-WiFi operation, exactly when TLS is on), typing `canary.local`
  301-redirects to a non-routable address and the dashboard hangs. Build the `Location` from the
  request `Host` header.
- **[LOW] Self-signed cert has no SubjectAltName** and CN matches neither `canary.local` nor the
  IP → a cert warning on every visit (trains users to click through TLS warnings). Move to ECDSA
  P-256 + proper SANs (also drops the 30–60 s RSA keygen at first boot).
- **[LOW] `cv_session` cookie lacks `Secure`**; **wildcard `Access-Control-Allow-Origin: *`** on all
  JSON; **global (not per-client) auth lockout** any LAN host can trip to 429 the operator.

`wap_server.cpp` is a **linker stub** — the real 499 KB server is inline in the `.ino`, and the
stub's header comment advertises properties ("TLS on all connections", rate limiting) that aren't
what's enforced. Full-potential gaps: single httpd worker (could pin to core 1 / add workers);
8 MB PSRAM barely used by the web layer; WebSockets available on the S3 but the dashboard polls.

### 3.3 BLE core stack (NimBLE) — *mixed (bench-informed)*

Substantial and careful: NimBLE 2.x, the device is both peripheral (GATT server: signed OTA,
offline console, WiFi provisioning, health/witness export, SIG DIS/Battery) and central. Pairing is
**LE Secure Connections with Numeric Comparison** (MITM-resistant, human-confirmed); nearly every
characteristic is `READ_ENC`/`WRITE_AUTHEN`. A **fail-closed heap guard** refuses to bring up the
controller unless ~48 KB contiguous internal DMA RAM is free (the controller *panics*, not returns,
on OOM) — leaving the dashboard alive instead of boot-looping.

- **[HIGH] Persistent MAC + name storage + trusted/blocked list**, in tension with the hard privacy
  invariants (`spec/canary_free_signals_v0.md` Invariant A "no MAC storage / no persistent
  identifiers", Invariant F "no known-vs-unknown distinction"). This is the *owner's* bonded phone
  (NimBLE persists bond keys anyway), but the code keeps a **redundant** app-level identity blob in
  NVS *and* writes the peer MAC to the durable `/HEALTH` SD log. Recommend: drop the app-level blob
  (use the NimBLE bond store), stop logging the MAC, remove trusted/blocked flags.
- **[MEDIUM] Cross-task double-free risk** on `g_pending_pair_info` (NimBLE host task vs HTTP/loop
  task, no lock) — narrow human-driven window but memory-unsafe.
- **[MEDIUM] BLE OTA characteristics are unauthenticated** (only `WRITE`, no `WRITE_ENC`) unlike
  every other service — currently masked because the release pubkey is all-zeros, but an unbonded
  peer can `ABORT` a legitimate update. Add a bonded-link requirement.
- **[MEDIUM] No OTA session teardown on disconnect** → leaked `esp_ota_handle` until the next BEGIN.

Full-potential gaps: **no BLE 5.0 extended/periodic advertising** anywhere (pinned to 31-byte legacy
adv — this is what forces the scan-results notify truncation); Coded PHY long-range is off by default
with no adaptive PHY selection; OTA/export stream over GATT chunking instead of an **L2CAP CoC**
(much higher throughput, supported on the S3); NimBLE host could allocate from PSRAM to relieve the
internal-RAM pressure the heap guard fights.

### 3.4 BLE "Scout" — locate a Bluetooth item you paired — *scaffold (inert)*

**This is the direct answer to "locate bluetooth items you maybe pair with it."** The `ble_scout`
module is exactly that feature, and its primitives are good: passive-only scan (never advertises),
per-device-keyed MAC hashing (raw MAC never leaves the function), a bounded 16-slot paired-beacon
registry, a **1-D Kalman RSSI smoother**, and a per-beacon presence FSM (AWAY→PROVISIONAL→PRESENT at
≥ −75 dBm held 5 s; →AWAY after 30 s silence) that emits `arrived`/`departed` with the user's room
label. All host-unit-tested.

**But it does not function end-to-end.** `ble_scout_pair()` has **no production caller** — only test
files call it — and `FEATURE_BLE_SCAN` defaults to `0` (on only in the FULL build). With an empty
registry every advert is dropped, so the shipping scout emits only `scout_initialized`. The
"locate your paired item" capability is coded and unit-tested but has **no user-reachable pairing
path**; the never-landed "PR 5c" pairing UI is the missing piece.

Beyond the wiring gap, there are real design issues to resolve before shipping it:

- **[HIGH] It contradicts the product's own privacy invariants.** Emitting per-paired-device
  `arrived`/`departed` events with a label *is* the known-vs-unknown distinction Invariant F forbids,
  and the persistent per-device key (never rotated) is exactly the "persistent identifier / cross-day
  correlation" Invariant A forbids. It stays compliant today only by being inert. **This is a
  product decision, not a bug:** either the invariants get a carve-out for owner-paired beacons, or
  the feature shouldn't ship. Worth an explicit ADR.
- **[MEDIUM] Unsynchronized data races** on the registry/tracker (NimBLE core-0 task vs loop core-1),
  where the sibling `ble_nearby` correctly uses a mutex and the scout does not.
- **[LOW] Modern-phone blind spot:** the scout hashes whatever address arrives, so a phone/watch/
  AirTag using RPA privacy rotation (~every 15 min) changes `hashed_id` each rotation and is lost.
  The scout can only reliably track **static-address** beacons (cheap tags, some fitness bands). The
  live `household` pipeline solves this with IRK resolution; the scout does not — so "locate items
  you pair" silently excludes phones.
- **[LOW] No liveness watchdog:** if the shared NimBLE scanner stalls (radio contention, or another
  module grabbing the single `getScan()` singleton), every PRESENT beacon falsely goes DEPARTED and
  nothing restarts it — unlike the CSI path, which has a watchdog.

**Feasibility verdict for "locate my paired items" on the S3** (see also §4): room-level presence
(which room is it in) is feasible and already coded; **distance/direction is not** — RSSI gives
"warmer/colder" proximity to a few meters, meter-scale in a real home, and *no bearing*. There is no
AoA on the S3 (verified). Multi-node RSSI over the mesh gives coarse room/zone (loudest-anchor-wins),
still ~1–3 m. Sub-meter + an arrow needs a UWB add-on (DW3000) on *both* tag and locator.

### 3.5 Self-healing mesh ("Opera") — *mixed (does not bootstrap on hardware)*

The "clever always-self-healing network" is a **single-transport (ESP-NOW only)** design. The wire
format and signing (Ed25519 over header+payload, per-peer monotonic counter freshness, cross-opera
rejection) are sound and host-tested, and several **self-healing primitives are individually well
built**: coordinated channel-hop with hysteresis, deterministic hub election (lowest fingerprint
wins, no voting), replay counters that survive reboot, a storm limiter, an airtime governor
(≤2 %/10 s), and a fail-closed beacon-audit chain-recovery. The single-radio channel policy
(follow the STA/AP channel, `channel=0` peers) is exactly right and directly solves the coexistence
constraint from claim #7.

**But the two load-bearing pieces do not work on hardware** (both re-confirmed against the code):

- **[HIGH] The pairing ECDH is broken: Ed25519 public keys are fed into X25519.**
  `mesh_network.cpp:955/990` generate the ephemeral keypair with `Ed25519::derivePublicKey()` (an
  Edwards-curve point), then `derive_session_key()` passes that pubkey to `Curve25519::eval()`,
  which expects a Montgomery u-coordinate. The comment claims "the Crypto library's Curve25519 does
  this conversion internally" — **it does not.** So `eval(privA, pubB) ≠ eval(privB, pubA)`, the two
  sides derive **different** session keys, the 6-digit confirmation codes never match, and the
  AEAD-wrapped `opera_secret` fails to decrypt. **No opera can be formed on a real device, in either
  tree.** The host test-shim uses a self-consistent fake-key model, so CI passes while the device
  path is non-functional. Fix: derive a proper X25519 keypair for the ECDH (or convert the Ed25519
  public key to Montgomery form), and add a host test that exercises the *real* curve path.
- **[HIGH] No working (re)connection or steady state.** The AUTH handshake is never initiated
  (`MSG_AUTH_CHALLENGE` has a handler but no sender); heartbeats only run in `MESH_ACTIVE`, and a
  peer only reaches `CONNECTED` on *receiving* a valid frame — so freshly-paired and rebooted peers
  never receive one and two devices deadlock in `MESH_CONNECTING` forever. This directly defeats
  "always self-healing": there is no active neighbor discovery/heartbeat that reaches a not-yet-
  connected peer.
- **[HIGH] Secret rotation on peer removal silently partitions the opera** (it only re-keys
  `session_established` peers, which — per the above — is none, so it commits a new secret locally
  and every survivor keeps the old `opera_id`).

Spec-vs-reality: the WiFi-AP bridge and BLE fallback transports are **spec-only**; gossip replication
has **zero implementation**. So mesh resilience today rests on ESP-NOW alone — and that path can't
bootstrap. **The "always self-healing" claim is aspirational, not operational.**

Full-potential gaps: the S3's idle **BLE radio** is the obvious missing transport (a BLE-advertising
last-gasp / rediscovery path would give the channel diversity that makes jamming survivable); no
active neighbor discovery; PSRAM/second-core unused (single global RX buffer drops bursty frames);
software Ed25519 per frame with the S3's crypto accel unused.

### 3.6 WiFi CSI core pipeline — *mixed (crown jewel; near single-antenna ceiling)*

**This is the strongest part of the codebase and the answer to "inspect CSI / RF sensing."** It is a
genuinely sophisticated, privacy-first, single-antenna CSI stack, well above typical hobbyist ESP32
CSI code. It enables CSI via the standard `esp_wifi_set_csi_config/_rx_cb/_set` path (with deferred
retry until WiFi is up), and the driver callback **structurally scrubs identifiers at the ISR
boundary** — it copies only RSSI/channel/bandwidth + the raw I/Q, never `info->mac`/`hdr`/`payload`.
Frames land in a 16-slot lock-free SPSC ring drained by the loop into a 32-dim `int8` feature vector
per 1 s window.

The DSP is the crown jewel and uses the *right* techniques for a single-antenna part:
- True `sqrt(I²+Q²)` magnitude via integer `isqrt` (not the L1 shortcut),
- Per-packet AGC/gain normalization,
- **CFO-corrected relative-band Doppler** — it estimates the frame-pair common phase rotation from
  the aggregate and scores each band relative to it, so the ESP32's per-frame PLL/CFO offset cancels
  and a static channel reads ~0. This is the correct substitute for the two-antenna conjugate trick
  the S3 cannot do,
- Per-subcarrier **temporal** variance for motion (not pooled variance, which would just measure
  static multipath),
- A cross-window envelope ring + **8-bin Goertzel** breathing bank at the physically-correct ~1 Hz
  timescale.

Host unit tests validate AGC-flicker rejection and breathing-bin selection against synthetic
channels. **For a single 2.4 GHz single-antenna S3 node this sits near the practical ceiling.**

Findings:
- **[MEDIUM] Watchdog escalation is dead code.** `process()` emits a near-empty window every ~1 s
  regardless of frame supply, and the callback resets the consecutive-silence counter on *every*
  window *before* the frames-in-window honesty gate — so it can never reach `WATCHDOG_ESCALATE_AFTER
  (3)`, and the designed hard `csi_hal::stop()/start()` recovery never runs. Only the gentle CSI
  toggle recovers. Fix: reset the counter only when `frames_in_window >= 2`.
- **[MEDIUM] HT40 is advertised but never engaged.** `cfg.bandwidth_mhz` is stored but never applied
  (`esp_wifi_set_bandwidth()` is never called; the channel lock always uses `WIFI_SECOND_CHAN_NONE`),
  yet `get_caps()` reports `CSI_CAP_HT40` on the S3. CSI is HT20-only in practice; ~2× subcarriers
  are left unused, and the caps mismatch can mislead capability-gated fusion.
- **[MEDIUM] Multi-link fusion is dormant** — `core_multilink_fusion_ingest_peer_features()` has no
  production caller and nothing sends/receives `CSI_FEATURES` between nodes, so `motion_confirmed`
  can never fire. **This is the one path that would actually beat single-antenna limits** (spatial
  diversity from cooperating nodes), and it's unwired.
- **[LOW] No subcarrier cleaning** (null/guard/DC/pilot subcarriers dilute band features — a standard
  step in published ESP32 CSI methods); breathing envelope sampled at only 1 Hz (0.45 Hz bin near
  Nyquist) with coarse log2 scoring → ~3 BPM resolution; Doppler *direction* is oversold (the sign
  self-cancels for anything but slow drift).

### 3.7 CSI sensing modules — *mixed (good detectors, dead scaffolds)*

Thin state machines over the feature vector: `core.presence` (empty/subtle/quiet/active/together with
hysteresis + multipath-shimmer reject), `core.breathing` (locks the dominant Goertzel bin),
`anomaly.baseline` (60 s rolling-average spike gate), `activity_ribbon` (96×15-min leaky-max ring).
The extractor + presence/breathing/anomaly are well-designed and validated by a **real
synthetic-physics host test**.

- **[MEDIUM] Breathing consumers systematically disagree.** `core.breathing` correctly uses the
  *peak* Goertzel bin, but every scalar consumer (`core.presence`, `anomaly`, integration) reduces
  the 8-bin breathing vector by **averaging** — a real signal is ~40 in one bin and ~0–2 elsewhere,
  so the average (~5) is far below the thresholds (30, 50). So `presence` breathing-quiet and
  `anomaly` unusual-breathing are effectively unreachable while `core.breathing` fires normally.
  Fix: take the peak (max), not the mean.
- **[MEDIUM] Three registered modules are never fed their inputs** and so cannot function on-device:
  `multilink_fusion` (no peer feature exchange), `empty_room_baseline` (never started, never
  subtracted — so the advertised static-clutter rejection **does not exist at runtime**), and
  `daily_summary` (its clock is never set, so the 23:55 emit never fires).
- **[LOW] The 8 canary-wap module files are byte-identical copies** of `firmware/common/csi/src/` —
  a source-duplication divergence risk for exactly the safety-relevant detectors (a fix to the common
  tree silently doesn't reach the sketch build unless re-copied).

Biggest accuracy win available with no new hardware: **per-subcarrier breathing extraction** (run
the Goertzel bank per subcarrier / SNR-weight the best few) instead of the whole-band mean amplitude,
which largely self-cancels chest modulation. And **actually subscribe the empty-room baseline** so
motion/breathing run on `v − baseline`.

### 3.8 Non-CSI RF / WiFi / device presence — *mixed (careful privacy, much dormant)*

A 5-state FSM (EMPTY/IMPULSE/PRESENCE/DWELLING/DEPARTING) driven by BLE adverts, each MAC crossing a
privacy barrier (secret-keyed, session-epoch-rotated ephemeral token for counting only), fused with a
CSI motion score. Around it: a `household` IRK recognizer (resolves the owner's bonded phones to
suppress self-alerts), a `familiar` recognizer (rotating salted Bloom filters, 1 % DP bit-flip), and
a quiet-by-default notify policy. The privacy engineering is careful and deliberate.

- **[HIGH] `household` persists 16-byte IRKs to NVS** — a *stronger*, longer-lived identifier than a
  MAC (it resolves all of a device's rotating RPAs across days), in direct tension with Invariant A,
  and the suppression path affects event *generation* (Invariant F). Same reconcile-with-spec
  decision as the Scout.
- **[HIGH] Cross-task race** on the token map (the wired BLE producer on the NimBLE task vs the loop
  consumer, no locking — the parallel `wifi_presence` module correctly uses a queue + critical
  sections; this one doesn't).
- **[HIGH] Household enrollment is unwired** — `add_irk()` has one caller: a test. No NimBLE bonding
  callback extracts the IRK, so resolution always fails, owner auto-context never activates, and the
  device pins `CTX_AWAY` forever.
- **[MEDIUM] Time-of-day comes from `millis()` uptime, not a wall clock**, so "unusual hour" is keyed
  to boot time and every reboot shifts the familiar-filter phase; **WiFi never fuses into the FSM**
  (`feed_wifi_probe()` has no caller — the separate `wifi_presence` promiscuous module counts probes
  but doesn't bridge in); the spec's 64-entry observation ring and temp/power "free signals" are
  plumbed but **dead** (no callers).

Note `wifi_presence` itself is a neat promiscuous-mode probe-request counter (ISR→queue→hash,
counts-only, no identifiers) — but single-channel (only the AP's channel; phones probe 1/6/11, so
counts undercount 2–3×) and its salt is the predictable `millis()` bucket start.

### 3.9 60 GHz mmWave radar (canary-sense) — *host-tested logic, bench-unverified protocol*

**Important architecture fact:** this runs on the **ESP32-C6**, not the S3, on a *separate board*.
The MR60BHA2 FMCW radar decoder + presence/vitals FSMs are genuinely host-tested (build clean under
`-Werror`, 13 tests pass), but the wire-format itself is `[BENCH]`-marked (extracted from the ESPHome
reference, not confirmed against real hardware), and the whole UART/hardware path is unverified.

- **[MEDIUM] The occupant-count + vitals gate hinges on an unverified `TARGET_COUNT` frame** — if the
  real module doesn't emit it, the device reports "present / 0 occupants" and the breathing lock can
  never engage.
- **No multi-modal fusion is actually implemented.** Radar (C6) and CSI+BLE (S3) are separate
  firmwares on separate boards sharing only MQTT. `rf_presence` even reserves a breathing hook for "a
  future paired mmWave module" with `CSI_BREATHING_FUSION_ENABLED=false` — the hook exists but has no
  data path to the radar. Radar-confirmed presence cross-validating CSI motion (each cancels the
  other's false positives) is the single most valuable unrealized fusion, but it would require the two
  modalities to share a board or a working mesh link — neither exists.

### 3.10 Device-level resilience infrastructure — *mixed (strong, one likely-inert safety net)*

A genuinely layered story: an 8 s panic task-watchdog (fed per boot-phase and once per loop, with all
blocking work — SD mount, BLE init, MJPEG, OTA download — offloaded to worker tasks so nothing
starves the budget); an NVS rapid-crash "safe mode" (disable optional peripherals after 3 resets,
then bounded auto-recovery); crash-safe witness sequencing (NVS every 10 records + reconcile-from-SD
on boot); a heap-degradation ladder; the airtime governor; and brownout/thermal counters. The
pure-logic pieces are host-tested.

- **[HIGH] The marquee "bad OTA → automatic revert" is very likely inert on the shipped build.** The
  bootloader-rollback path (`verifyRollbackLater`, the boot self-test/`PENDING_VERIFY` flow) is
  `#if defined(CONFIG_APP_ROLLBACK_ENABLE) || defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)` — and
  **neither is set anywhere in the repo** (re-confirmed by grep). The stock arduino-esp32 build
  compiles it out, so `esp_ota_set_boot_partition` leaves the state UNDEFINED (not `PENDING_VERIFY`),
  the boot self-test takes the "nothing to validate" branch, and a crash-looping OTA image is **not**
  reverted — app-level safe mode stays on the same broken image and can't re-flash. An OTA that
  crashes before WiFi/BLE come up bricks the field device until USB recovery. Their own docs list
  "pull power mid-flash to prove rollback" as an unchecked manual TODO. **Fix: ship a custom
  partition table + sdkconfig with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` (and ideally eFuse
  `secure_version` anti-rollback), and bench-verify it.**
- **[MEDIUM] OTA-reboot and `<3 % SoC` deep-sleep paths skip the witness-chain flush** the manual/
  safe-mode reboots perform, so on card-less (RAM-buffered) devices they can lose up to 10 records
  and **reuse sequence numbers**, weakening the tamper-evidence chain.

Full-potential gaps: Secure Boot v2 + Flash Encryption exist as a reference (`partitions_secure.csv`)
but aren't wired into the FULL build — identity keys and the chain sit in plaintext flash today; a
PSRAM-backed deferred-write ring would stop record loss while the SD card is absent (the mechanism
already exists for other rings); the ULP-RISC-V / RTC domain is used only for a crash breadcrumb.

---

## 4. Direct answer: "locate Bluetooth items you pair with it"

**What the ESP32-S3 can do (and this firmware already codes):** room-level presence of a *paired,
static-address* BLE beacon — "your tag is in the kitchen" — via passive scan + Kalman-filtered RSSI +
a dwell/lost FSM (`ble_scout`). Coarse and privacy-preserving. **Today it's inert** (no pairing UI),
tracks phones poorly (RPA rotation), and conflicts with the product's own no-known-device invariant.

**What it cannot do on the S3, ever, in firmware** (all verified):
- **Direction / bearing** ("point me to it") — no BLE 5.1 AoA/AoD (no CTE hardware). Needs an external
  AoA antenna-array + a 5.1 SoC (Nordic nRF52811 / SiLabs xG22) as a co-processor.
- **Sub-meter distance** — RSSI is meter-scale in a real home; multi-node RSSI trilateration is still
  ~1–3 m (the repo's own mesh design *correctly rejects trilateration in homes as unreliable*).
  Sub-meter + an arrow needs **UWB** (Qorvo DW3000) on *both* the tag and the locator — a new BOM,
  e.g. the Makerfabs `MaUWB_ESP32S3` (ESP32-S3 + DW3000), not a firmware toggle.
- **Locate someone else's AirTag/Tile** — cryptographically impossible (rotating keys, reports
  encrypted to the owner's private key, finder role is Apple/Google-first-party-only). The S3 *can*
  passively **detect** nearby trackers → a privacy-aligned **anti-stalking "unknown tracker with you"**
  alert is the one third-party-tracker feature that fits this product.

**Recommended framing:** scope "find my item" to *your own beacons located by your own anchors*, keep
the UI honest ("getting closer" bar, not a compass arrow), and gate any sub-meter/directional
ambition behind an explicit UWB hardware add-on variant. And decide the `ble_scout` invariant
question in an ADR before wiring the pairing UI.

---

## 5. Direct answer: "always self-healing WiFi / WAP / home-WiFi in a clever way"

**The clever design largely exists** and is genuinely good where it runs:
- **WAP self-healing** (§3.1/3.2): drop-AP-when-STA-stable / re-raise-on-loss, retry home WiFi
  forever without rebooting, race-hardened `canary.local` claim — this is real resilience engineering
  and the model the whole fleet should follow.
- **Mesh self-healing primitives** (§3.5): channel-hop, deterministic hub election, replay
  persistence, airtime governor, chain-recovery — all individually well built.

**But the "always" part is not true today:**
- The `sense`/`vision` builds "self-heal" by **rebooting** on outage and make WiFi a hard boot
  dependency (§3.1 HIGH) — the opposite of resilient for a local-first witness.
- The mesh **cannot even pair on hardware** (ECDH bug) and **has no reconnection path** once paired
  (§3.5 HIGH×2), so the multi-device mutual-protection story doesn't operate.
- Bad-OTA auto-recovery is **likely compiled out** (§3.10 HIGH).

**The single cleverest available upgrade, using hardware you already have:** the S3's **idle BLE
radio** is the missing mesh transport. A BLE-advertising last-gasp/rediscovery channel (spec'd but
unbuilt) would give the channel diversity that makes ESP-NOW jamming/link-loss actually survivable —
turning "self-healing" from aspiration into a real dual-transport property. Combined with fixing the
ECDH bug, wiring active neighbor heartbeats, and copying the WAP's graceful-degradation pattern into
the sense/vision builds, "always self-healing" becomes achievable on the current silicon.

---

## 6. Direct answer: CSI & RF sensing — where you sit vs the state of the art

**You are close to the single-antenna ESP32-S3 ceiling on the parts that are wired, and the frontier
gains are about *finishing multi-node fusion* and *per-subcarrier breathing*, not exotic hardware.**

What the S3 can credibly do (and mostly does): presence, motion, short-range (≤ ~5 m) breathing,
activity level, anomaly. Demonstrated in the literature at these levels on commodity ESP32 CSI; your
DSP (CFO-corrected relative-band Doppler, Goertzel breathing, AGC normalization) is the right toolkit.

The hard single-antenna limits (physics, not code):
- **Two-antenna CSI-ratio / conjugate-multiplication** (FarSense/WiRM — the technique that pushes
  respiration to ~8 m and through walls) **is not reachable on the S3**: its "antenna diversity" is a
  GPIO-switched *single* RF chain, not two synchronized complex streams from one clock. That needs a
  multi-antenna NIC (Intel 5300 / Atheros / Pi+Nexmon) — a different platform.
- **Gesture recognition (Widar-class), multi-person separation, trilateration, AoA** — all need
  antenna diversity or dense geometry the single 1-antenna S3 lacks. Don't market them.
- **802.11bf sounding** — no ESP32 exposes it (§2 claim #5, REFUTED). The C6's *potential* edge is
  802.11ax HE-LTF CSI, richer than HT-LTF. ESP-IDF v5.5+ exposes C6 HE-LTF CSI acquisition
  (`acquire_csi_su`/`mu`/`dcm`/`beamformed`), but **this firmware** configures the legacy CSI fields
  and caps ingest at 128 subcarriers, so the ~242-tone path isn't available here today — the gap is
  this repo's HAL, not ESP-IDF. A C6 is a reasonable *future board* to evaluate, but the CSI path
  and HAL work to actually exploit HE-LTF would have to be built and verified first; unrelated to 11bf.

What IS cutting-edge **and** feasible here:
- **Multi-node bistatic fusion** — the correct substitute for two-antenna diversity on the S3. Your
  `core_multilink_fusion` + ESP-NOW active probe is exactly this design; it's just **unwired**
  (§3.6/3.7). Finishing it (broadcast local `CSI_FEATURES`, route received envelopes into
  `ingest_peer_features`) is the highest-leverage sensing upgrade you have, and it needs no new
  hardware — only a working mesh (§3.5) and the peer feature exchange.
- **Amplitude-only TinyML breathing/HR** — PulseFi (2025) shows *clinical-grade* breathing/HR from a
  **single-antenna** ESP32 using a ~46 k-param LSTM (~0.15 s inference on the S3, use the 8 MB PSRAM),
  no two-antenna trick required. This is the biggest single-radio robustness win available and would
  replace/backstop the current rule-based breathing — kept inside the existing privacy chokepoint.
- **Per-subcarrier breathing extraction** (§3.7) — sharpen the current whole-band-mean envelope; best
  accuracy win with zero new hardware.
- **WiFi FTM ranging** — the S3 *does* support FTM initiator+responder (verified), giving ~1–2 m LOS
  absolute range between cooperating Canaries (no site survey, unlike CSI fingerprinting). Not in the
  repo today; a natural addition for mesh auto-topology / relocation detection. Don't depend on the
  customer's router (few implement FTM) — do it Canary-to-Canary.

What needs **added hardware** (scope as optional variants, not firmware toggles):
- **UWB (DW3000)** for cm-range + cryptographically-secure ranging (802.15.4z STS) — genuinely
  valuable for a "witness" device that wants to *prove* a tag/person is really within X cm and resist
  relay/distance-reduction spoofing. Separate radio; the S3 is only the SPI host.
- **60 GHz mmWave** (you already have the MR60BHA2 path on the C6) as a vitals ground-truth + range
  channel, and a **sub-dollar PIR** on a GPIO as a zero-power wake/trigger that gates the heavier
  CSI/camera paths and cross-checks them to cut false alarms.

---

## 7. "Full potential" — prioritized roadmap

Grouped by what it costs. Nothing here is speculative — each maps to a specific finding above.

**A. Fix what's already built but broken/inert (highest value, no new hardware):**
1. **Mesh ECDH key-type bug** (§3.5) — the mesh literally cannot pair on hardware. Fix the X25519
   key derivation + add a real-curve host test. *Blocks the entire self-healing-mesh story.*
2. **Wire active mesh neighbor discovery/heartbeat + reconnection** (§3.5) — so paired/rebooted peers
   actually re-associate.
3. **Wire CSI multi-link fusion end-to-end** (§3.6/3.7) — broadcast/receive `CSI_FEATURES`. The one
   path that beats single-antenna limits.
4. **Wire the BLE Scout pairing UI** *(after an ADR on the privacy-invariant conflict, §3.4)* — or
   deliberately decide not to ship it.
5. **Enable bootloader OTA rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + custom
   partitions/sdkconfig) and bench-verify (§3.10) — the current safety net is dead code on target.
6. **Fix the CSI watchdog escalation** and the **breathing peak-vs-mean** consumer bug (§3.6/3.7).
7. **Copy the WAP's graceful-degradation into sense/vision** so an AP outage doesn't reboot-loop a
   witness (§3.1); flush the witness chain on OTA/deep-sleep reboots (§3.10).

**B. Cheap on-S3 upgrades (small work, no new hardware):**
8. `WiFiMulti` multi-AP + best-RSSI failover; event-driven reconnect + `IP_EVENT_STA_LOST_IP`;
   backoff jitter (§3.1).
9. Per-subcarrier breathing extraction + subscribe the empty-room baseline (§3.7).
10. PulseFi-style amplitude-only TinyML breathing/HR in PSRAM, inside the privacy chokepoint (§6).
11. BLE 5.0 extended advertising for chirp/provisioning payloads; adaptive PHY selection (§3.3).
12. WiFi FTM Canary-to-Canary ranging for mesh topology / relocation detection (§6).
13. Add BLE as a second mesh transport (the idle radio) for real channel diversity (§3.5/§5).

**C. Needs added hardware (scope as optional variants):**
14. UWB (DW3000) for cm-range + secure ranging; PIR wake-trigger; a shared-board radar+CSI node for
    real multi-modal fusion (§3.9/§6).

**D. Not feasible on the S3 — don't build/market (verified):**
15. BLE direction-finding / "point me to it" arrow (no AoA); two-antenna CSI-ratio respiration;
    Widar-class gesture; sub-meter locate without UWB; Thread/Zigbee without a co-processor;
    802.11bf sounding (no ESP32 has it).

**E. Cross-cutting privacy debt to reconcile with `spec/canary_free_signals_v0.md`:**
16. The BLE Scout, `household` IRK store, and `familiar` recognizer all keep persistent per-device
    identity and/or a known-vs-unknown distinction that the hard invariants forbid. These are
    deliberate design tensions, not accidents — resolve them in an ADR (carve-out for owner-paired
    devices, or drop the features) before enabling them.

---

## 8. Highest-confidence bugs (re-verified against code)

| Sev | Subsystem | File | Bug |
|---|---|---|---|
| HIGH | Mesh | `mesh_network.cpp:955/990,296` | Ed25519 pubkeys fed to `Curve25519::eval` (X25519) → different session keys → opera can't pair on hardware |
| HIGH | Mesh | `mesh_network.cpp` update/heartbeat/add_peer | No AUTH-handshake initiator + `CONNECTED`-only send gates → peers deadlock in `MESH_CONNECTING` |
| HIGH | Resilience | `securacv_ota.cpp:436` | Bootloader rollback `#if`-gated on a config set nowhere in repo → crash-loop OTA not auto-reverted on target |
| HIGH | WiFi | `sense/vision/src/net/wifi_mgr.cpp` | Reboot-on-outage + WiFi hard boot dependency → router-down = reboot loop, never senses |
| HIGH | BLE core | `bluetooth_channel.cpp` | Persistent MAC/name + trusted/blocked list + MAC to `/HEALTH` — violates no-MAC-storage invariant |
| MED | CSI | `csi_modules_integration.cpp:519` | Watchdog escalation counter reset every window → hard-recovery `stop()/start()` never runs |
| MED | CSI | `csi_hal.cpp:321` | `bandwidth_mhz` never applied; HT40 advertised in caps but never engaged |
| MED | CSI modules | `core_presence.cpp:205` etc. | Breathing consumers average 8 bins (should take peak) → presence/anomaly breathing paths unreachable |
| MED | WAP | `canary_wap.ino:7081` | Port-80→HTTPS redirect hardcodes `192.168.4.1` → dashboard hangs once on home WiFi |
| MED | RF presence | `rf_presence.cpp:1055` | Cross-task race on token map (NimBLE task vs loop, no lock) |
| MED | BLE core | `ble_ota.cpp` | OTA GATT chars unauthenticated; no session teardown on disconnect (leaked `esp_ota_handle`) |

(Full detail, plus ~20 LOW/NOTE findings, in the per-subsystem sections above.)

---

## 9. What this audit did **not** cover / caveats

- **No hardware in the loop.** Runtime behaviour (WDT recovery, A/B rollback, brownout reaction, real
  CSI frame rates, mesh on-air) is inferred from code, not observed. The repo's own open item #610
  (on-device CSI/mesh verification) and the OTA rollback bench test remain the gating validations.
- **Non-S3 subsystems** (canary-sense mmWave on the C6) were audited only for the fusion story; the
  radar wire format is `[BENCH]`-unverified upstream.
- Findings on **privacy-invariant tensions** are judgment calls against the repo's own normative specs,
  not universal claims — they need a product decision, not a mechanical fix.

---

## 10. Sources

**Espressif primary:** ESP32-S3 datasheet; ESP-IDF `esp_wifi.h`, Wi-Fi CSI / vendor-features guide,
`coexist` guide, `esp_now.rst`, BLE feature-support-status + ESP-FAQ (BLE AoA/AoD), `soc_caps.h`
(esp32s3 & esp32h2), FTM example/docs, bootloader rollback + anti-rollback docs; esp32-arduino-libs
`sdkconfig` (CSI enabled); esp-csi / esp-radar; espressif/esp-idf#15839 (C6 beamforming feedback).
**Standards:** IEEE 802.11bf-2025 (published 2025-09-26); 802.11-2016 FTM/11mc.
**BLE tracking / Find My:** OpenHaystack (seemoo-lab); Apple Find My security guide; Apple Secure
Enclave / attestation; Google Find My Device network security post; adamcatley.com AirTag teardown;
Makerfabs ESP32-UWB-DW3000 / MaUWB_ESP32S3; Qorvo DW3000 / IEEE 802.15.4z.
**CSI / RF sensing literature:** ESP32-CSI-Tool (Hernandez & Bulut 2020); Wi-ESP (JCDE 2020);
FarSense CSI-ratio (arXiv:1907.03994); WiRM (arXiv:2507.23419); Widar3.0 (TPAMI); PulseFi
(arXiv:2510.24744); STAR on-device CSI (arXiv:2510.26148); MDPI Sensors FTM positioning (2020);
RSSI-vs-UWB indoor positioning surveys.
**Mesh:** ESP-NOW / ESP-WIFI-MESH / esp-mesh-lite / painlessMesh docs.
**Seeed:** XIAO ESP32-S3 wiki (getting-started, camera, filesystem, mic, power) — full reconciliation
in PR #905.

*(Per-agent research bundles with full URL lists are archived in the audit working notes.)*
