# Firmware map — every target, every shared layer, the hub and the Witness Wall

**Snapshot:** `main` at `2e715eb` (2026-10-15), read on 2026-10-18 before any
code in the parity program changed. **The program has since run**: the
per-target state *after* it is [`PARITY.md`](PARITY.md), and §10 below ends
with what each row became. The sections between are kept as they were read —
they are the evidence the matrix was built against, and the file:line
references still point at the files that were changed. Every claim below names the file (and
usually the line) it was read from; where a reader did not open a file the
row says so. This is the §0 "discover before you assume" deliverable of the
firmware parity, onboarding, discovery and safe-OTA program; the matrix it
feeds is [`PARITY.md`](PARITY.md), the trust design it feeds is
[`TRUST_MODEL.md`](TRUST_MODEL.md), the update design is [`OTA.md`](OTA.md).

Vocabulary is the project's ([`GLOSSARY.md`](GLOSSARY.md)): a group of
Canaries is a **fleet**; **the hub** is Home Assistant plus the Rust kernel;
**the Witness Wall** is the Apple TV app in [`tvos/`](../tvos/) that polls
`GET /api/fleet`. "Verified" means an Ed25519 signature checked against a
pinned key and nothing looser.

---

## 1. The targets

Nine directories under `firmware/projects/` plus the flagship tree
`firmware/canary/`. Six of them are products with a network stack; the other
four are a teaching harness, two Phase-0 cores and a concept.

| Target | Lifecycle (`firmware/VARIANT_POLICY.md`) | MCU / boards | Build system (platform pin, `firmware/envs/platformio/platforms.ini`) | CI envs (`firmware/flavors.json` `build_envs`) | A/B app slots | OTA product |
|---|---|---|---|---|---|---|
| **canary (PIO)** `firmware/canary/` | ACTIVE flagship | ESP32-S3 XIAO Sense; reach ports ESP32-CAM, ESP32-WROOM DevKit, Freenove S3 | PlatformIO, `espressif32@6.9.0` (Arduino core 2.0.17 / IDF 4.4.7) for every env but `full`, which is the pioarduino core-3 zip (Arduino 3.3.8 / IDF 5.5.4) | `dev release release_ha full esp32cam esp32-wroom freenove-s3` (+ compile-only `secure`, `secure_ha`, `dev_ha`, `minimal`, `usb-onboard`) | `partitions_ota.csv` 2 × 0x1E0000; `full` uses `default_8MB.csv` | `securacv-canary` (release_ha is the published image); the three reach ports carry product ids with no published manifest |
| **canary-wap** `firmware/projects/canary-wap/` | COMPATIBILITY | ESP32-S3 XIAO Sense (C3 is a compile-only CI target) | ONE Arduino sketch (`arduino/canary_wap/`, `canary_wap.ino` 13.5k lines) built by arduino-cli (latest core 3.x) AND PlatformIO via `src_dir` on the pioarduino core-3 zip | `canary-wap-default canary-wap-usbdrive` | `default_8MB.csv` 2 × 0x330000 | `securacv-canary-wap` (version `2.4.16-wap`) |
| **canary-vision** `firmware/projects/canary-vision/` | SPECIALIZED | ESP32-C3 DevKit / XIAO C3 / C3 Super Mini / XIAO S3 + Grove Vision AI V2 | PlatformIO, `espressif32@6.9.0` | `canary-vision-default canary-vision-xiao-c3 canary-vision-xiao-s3 canary-vision-c3-super-mini` | `min_spiffs.csv` 2 × 0x1E0000 (S3: `default_8MB.csv`) | `securacv-canary-vision`, `-xiao-c3`, `-xiao-s3` published; `-c3-super-mini` polled, unpublished |
| **canary-sense** `firmware/projects/canary-sense/` | SPECIALIZED | ESP32-C6 XIAO + MR60BHA2 radar | PlatformIO, pioarduino core-3 zip (the official platform has no C6 Arduino) | `canary-sense-default canary-sense-wellbeing` | `min_spiffs.csv` 2 × 0x1E0000 | `securacv-canary-sense`, `-wellbeing` |
| **canary-sentinel** `firmware/projects/canary-sentinel/` | SPECIALIZED Phase 1a, **unreleased** | ESP32-C6 XIAO (door/window/hallway/demo-head), XIAO C3 (lite) | PlatformIO, pioarduino core-3 (C6) and `espressif32@6.9.0` (lite) | `canary-sentinel-door canary-sentinel-lite` | `min_spiffs.csv` / `partitions.csv` | one product per preset, all unpublished (pull-OTA 404s by design) |
| **canary-display** `firmware/projects/canary-display/` | SPECIALIZED (the glass line) | ESP32-S3: XIAO round watch, Waveshare 4.3/4.3B/4.3C dash, 7" dash7/nightstand7, 1.47" nightstand, Touch 1.69, AMOLED 2.41; ESP32-C6 1.47" nightstand; ESP32-C3 1.47" nightlight | PlatformIO: `espressif32@6.9.0` for the SPI panels (watch, nightstand-s3, touch169, amoled241, nightlight-c3), pioarduino core-3 for the RGB dash family and the C6; plus a generated Arduino sketch (`arduino/canary_display/`, `setup.sh regen`) built by arduino-cli on cores 2.0.17 and 3.3.10 | 21 envs, sharded five ways in CI; `release_envs` = dash7, nightstand7, nightstand-s3, touch169, amoled241, nightstand-c6, nightlight-c3 | `default_8MB.csv` (watch), `default_16MB.csv` (S3 boards), `partitions_display_4mb.csv` 2 × 0x1F0000 (C6, C3) | one product per flavor via `-DSECURACV_OTA_PRODUCT` |
| canary-ota `firmware/projects/canary-ota/` | SPECIALIZED teaching harness | ESP32-S3 | ESP-IDF via `espressif32@6.5.0`; **no CI job compiles it** | — | `partitions.csv` | none (SHA-256-only engine, predates the shared one) |
| canary-companion, canary-tincan | Phase 0 (pure cores host-tested, no runtime, no env) | Waveshare AMOLED 2.06 wrist board | `make -C firmware/projects/<p>/tests_host` only | — | — | — |
| canary-fence-guard | CONCEPT (`src/main_stub.cpp` refuses to compile) | — | — | — | — | — |

The emulator (`canary-local/emulator/`) is a tenth build: the display tree
compiled to WebAssembly with emsdk 6.0.3, committed under
`canary-local/emulator/dist/`. It is not a device target, but every edit to a
file `build.sh` compiles (`src/net/provision.cpp`, `mqtt_mgr.cpp`, `main.cpp`,
`ui/`, `fleet/`, `trust.cpp`, `runtime_config.cpp`, the shared
`wifi_join_policy.h`) moves `dist/` and trips the "Dist drift check"
(`CLAUDE.md` explains the rebuild button).

### What this container can and cannot build

The egress proxy here allows `github.com` release assets, git clones and
PyPI, and refuses `registry.platformio.org`, `dl.platformio.org`,
`codeload.github.com`, `espressif.github.io`, `dl.espressif.com` and
`downloads.arduino.cc`. So:

- **Host tests, node tests, Python and Rust tests, every lint and every
  `gen_*.py --check`** run here unchanged (baseline recorded green on
  `main` before any edit: `make -C firmware/tests_host`,
  `make -C firmware/projects/{canary-wap,canary-display}/tests_host`, the
  onboarding node suites).
- **PlatformIO builds** run only through a local mirror: the pioarduino
  platform zip, its toolchains and the Arduino core 3.3.8 come from GitHub
  release assets; the official core 2.0.17 comes from arduino-cli's board
  index (itself a GitHub release asset) and is bridged into an isolated
  `PLATFORMIO_CORE_DIR` as `symlink://` packages; SCons comes from the PyPI
  wheel; the seven library dependencies are cloned from their GitHub tags.
  The recipe is `/root/pio-tools/pio_offline_libs.sh` in this session (not
  committed — it is a workaround for this sandbox, not a repo convention).
  Anything this mirror cannot reproduce is built by the repo's own
  `firmware.yml` on the pushed branch and read back from the job logs.
- **The emulator dist** cannot be rebuilt here (emsdk 6.0.3). It is rebuilt
  by Actions → "Rebuild emulator dist (pinned emsdk)" on the branch.
- **Swift** (iOS, tvOS) has no toolchain here; those changes are checked by
  reading and by CI on macOS runners.

---

## 2. The shared layers and how sharing actually works

`firmware/common/` is the one place a fleet-wide behavior may live
([`FLEET_PARITY.md`](FLEET_PARITY.md) is the doctrine; `common/README.md` the
module map). Three routes compile it:

1. **PlatformIO trees include it directly** (`-I ../../common`,
   `build_src_filter` for the `.cpp`, or a `library.json` for an
   unconditionally included library). `firmware/scripts/check_common_build_reachability.py`
   fails a shared `.cpp` that no env compiles.
2. **The Arduino sketches carry byte-identical staged copies**, pinned by
   sixteen `firmware/scripts/check_*_sync.sh` guards (csi, power, boot,
   fleet_beacon, improv, sentinel_net, fleet_selfreport, birth_day, ota,
   audio, ble_scan, mesh, signature, display_arduino, mqtt_transport,
   ap_security). The display's sketch is regenerated whole by `setup.sh regen`
   and diffed by `check_display_arduino_sync.sh`.
3. **A host test pins the contract** in `firmware/tests_host/` (g++ `-Wall
   -Wextra -Werror`, 75+ suites) so the pure half cannot regress.

What is shared today, by area (consumer lists are from `grep -rl`):

| Shared module | Consumers | Not consumers |
|---|---|---|
| `network/setup_portal.{h,cpp}` + `setup_portal_logic.h` (SoftAP + A-only captive DNS + scan/join/status wizard, non-blocking) | canary-sense, canary-vision, canary-sentinel (`wifi_mgr.cpp`, `main.cpp`) | **canary (PIO)** (own `securacv_setup` + `securacv_network`, 5.8k lines), **canary-wap** (own `setup_wizard.h`, `captive_dns.h`, `captive_probe.h`, `companion_pwa.h`), **canary-display** (own blocking `src/net/provision.cpp`, 1.1k lines) |
| `network/improv_core.h` + `improv_ble.{h,cpp}` (Improv Wi-Fi over BLE, the "setup door") | canary-sense, canary-vision via `setup_portal_submit_join`; canary-wap through a byte-synced copy of the core with its own `ble_improv.cpp` glue | canary (PIO) (`CONFIG_BT_ENABLED=n`), canary-display (glass QR instead), canary-sentinel (portal only) |
| `network/hub_handoff.h` (the phone hands a Sense/Vision its broker + login over the door) | sense, vision, sentinel runtime_config | WAP (claim ticket instead), canary (PIO), display |
| `network/wifi_join_policy.h` (JoinFailure vocabulary, backoff, `wifi_should_open_setup`) | sense, vision, sentinel, display `wifi_mgr.cpp`, the emulator's `emu_net.cpp` (`scripts/lint_wifi_join_policy.py` requires it) | WAP (inline strings, flat 30 s retry), canary (PIO) (own backoff) |
| `network/provision_core.h` (DNS builder, probe classifier, JSON/QR escaping, password alphabet) | shared portal, display portal | WAP (`captive_dns.h`, `captive_probe.h` are older copies of the same logic) |
| `network/claim_ticket.h`, `provisioning_gate.h`, `auth_backoff.h`, `host_guard.h`, `ap_security_policy.h`, `ap_password.h`, `tls_policy.h`, `mqtt_transport*.h` | canary (PIO) and/or WAP as named in each header | the headless trio and the display, except `mqtt_transport` |
| `fleet_selfreport/fleet_selfreport.h` (the `GET /api/fleet` body) | canary-wap (synced copy), canary-display (regen copy) | canary (PIO) serves **no** `/api/fleet`; sense/vision/sentinel run no HTTP server after provisioning |
| `fleet_link/` (BLE presence beacon wire format, ESP-NOW and UDP-multicast transports, roster) | senders: sense, vision, WAP; receivers: display (all three transports) | canary (PIO) in `[env:full]` only |
| `link/` (Canary Link: AEAD frame, node table, relay, replay, session) | pure, host-tested; no runtime transmit side yet (Tin Can design) | — |
| `ota/src/securacv_ota.{h,cpp}` + `ota_release_key.h` (signed pull-OTA engine) | canary (PIO) via `build_src_filter`; WAP and display via synced copies; vision/sense/sentinel via `lib_extra_dirs`/`build_src_filter` with `src/net/ota_mgr.cpp` glue | canary-ota harness (older SHA-256-only engine) |
| `identity/` (device key, pseudonym, `device_signature` canonical bytes, `key_at_rest.h`, `birth_day.h`) | every witnessing tree; the display has **no** device key | — |
| `witness/`, `csi/`, `power/`, `boot/`, `health/`, `attest/`, `time/`, `ui/`, `color/`, `story/`, `sensors/`, `fusion/`, `doorbell/`, `usb/`, `console/` | per module map in `common/README.md` | — |

The pattern that matters for this program: a capability is "shared" in this
repo when it is one header or library under `common/`, host-tested, and either
compiled directly or carried as a byte-checked copy. "Parity by convention"
(the same idea written three times) is exactly what the readers found for the
captive portal, the AP-password mint, the `wl_status_t → JoinFailure` switch
(five copies), the captive-probe classifier (three) and the AP linger rule
(three), and is what §5 of this program replaces.

---

## 3. Onboarding today, per target

> **After the program:** one rule set, `common/network/onboarding_fsm.h`
> (+ `wifi_store.h`, `io/reset_hold.h`, `node_status.h`), on all six
> products; the headless trio's `wifi_mgr.cpp` files are shims over
> `common/network/wifi_supervisor.cpp`; the SSID is `SecuraCV-<type>-XXXX`
> everywhere; the AP hold is 120 s / 1.6 s; the saved-network ring holds four;
> a 10 s hold resets credentials and keeps identity. Cells: `PARITY.md` C1–C12.

Four portal implementations exist. All four raise a device-unique
`SecuraCV-XXXX` WPA2 network, hijack DNS for A queries only, answer the
Apple/Android/Windows connectivity probes, list nearby networks strongest
first, and persist credentials only after a join actually worked — because
each was ported from the WAP's lessons (`firmware/LESSONS_LEARNED.md`
§Networking) by hand.

| | canary (PIO) | canary-wap | canary-vision / -sense / -sentinel | canary-display |
|---|---|---|---|---|
| Portal code | `lib/securacv_setup` + `lib/securacv_network` (`/setup`, `/api/wifi/{scan,connect,status,disconnect}`) | `setup_wizard.h` + `companion_pwa.h` wizard over `esp_http_server` `/api/wifi/*`, one-shot pair token | `common/network/setup_portal.cpp` (`/`, `/scan`, `/join`, `/status`) | `src/net/provision.cpp` (`/`, `/scan`, `/join`+tz, `/status`), blocking, the glass shows a join QR |
| AP SSID / key | `SecuraCV-<4 chars of pubkey fp>`, key `cv-`+12 from HMAC(privkey) (`common/network/ap_password.h`, since F350), printed on serial, never persisted | `SecuraCV-<4>`, key `cv-`+12 from HMAC(privkey), printed on serial, never persisted | `SecuraCV-<4 of salted pseudonym>`, 8-char key minted once, persisted in NVS `ap_pass` | same algorithm as the shared portal, separately implemented (`provision.cpp:651-703`), shown on the glass |
| AP lifetime | always-on AP+STA; dropped 8 s after STA holds, re-raised on STA loss (`raiseAp`) | always-on management AP; dropped 120 s after STA holds (`AP_DROP_GRACE_MS`), re-raised on loss | raised only while unprovisioned or recovering; lingers ≤ 25 s after success (1.6 s after the phone's ack) | same 25 s / 1.6 s linger, then the glass takes over |
| BLE door (Improv) | none | FULL profile: 30-min first-boot window, 60 s BOOT tap, claim ticket → `GET /api/provisioning-receipt?claim=` | sense, vision: same door, same join path as the portal, plus the hub hand-off; sentinel: none | none by design ("a screen beats a radio") |
| Password field | `/setup`: label "Wi-Fi password", placeholder "Your existing Wi-Fi password", hint "Nothing new to invent" (`securacv_setup_page.cpp:106-111`); dashboard SPA: "WiFi password" | companion wizard "Network password"; legacy dashboard "WiFi password"; fleet QR form "Password" | placeholder **"Password"** under the picked SSID (`setup_portal.cpp:150`) | placeholder **"Password"** (`provision.cpp:231`) |
| Server-side rule | ssid non-empty, password strncpy'd into `char[65]` (silent truncation), no minimum | ssid 1..32, pass ≤ 64, empty = open | ssid 1..32, pass 0..64 (`:314`) | ssid 1..32, pass ≤ 64 (`:485`) |
| Join verdicts | `/api/wifi/status` | inline strings ("Wrong password or auth rejected"), flat 30 s retry | `wifi_join_policy.h` labels: Network not found / Wrong password / No address from the router / Couldn't connect | same shared labels, 30 s STA timeout |
| Saved networks | one (`wifi_ssid`/`wifi_pass`/`wifi_en` blobs) | one (same keys; `wifi_ap_only` too) | one (`wifi_ssid`/`wifi_pass` strings) | one (strings) + `ap_pass` |
| Recovery without wipe | implicit: AP always up, wizard served on probes while STA is down | SoftAP re-raised on loss, STA retried every 30 s; Improv door does **not** reopen for a failing saved network | `wifi_should_open_setup`: never-online + NotFound/BadPassword + 3 attempts → portal, saved network retried every 60 s underneath; a unit that was **ever** online never re-raises | same rule; blocking wizard re-run from `loop()`; no background retry |
| Factory reset | BOOT held ≥ 5 s at boot or runtime → `NvsManager.clear()` wipes the whole `securacv` namespace, identity key included (`main.cpp:855-872`) | **none** ("the long-hold is RESERVED — no factory-reset path exists", `canary_wap.ino:500`); `POST /api/wifi/forget` clears Wi-Fi only | **none**; BOOT tap only opens the door for 60 s | Settings → "Forget Wi-Fi Network" (Wi-Fi only) and "Reset Settings"; no button hold |
| Credentials at rest | plaintext NVS (`key_at_rest = plaintext-nvs` reported) | plaintext NVS; MQTT in the `csi` namespace | plaintext NVS strings | plaintext NVS strings |
| First-boot timeout | 15 min then `setup_ok=true` + reboot even with no credentials (`securacv_setup.cpp:174-192`) | wizard idle restart every 15 min | none (sensing continues under the portal) | none (glass IS the product) |

Two facts the whole program hangs on:

- **No firmware anywhere requires a minimum Wi-Fi password length**, and every
  firmware accepts an empty password for an open network. The 8-character
  WPA2 floor in `ap_security_policy.h` governs only the device's **own**
  SoftAP key. The minimum-length rules that do exist are client-side:
  `ios/.../SetupPortal.swift:164-169` (hint), `canary-local/assets/flash-core.js:887`
  and `desktop/src/app.js:3991` plus `desktop/flash-engine/src/provisioning.rs:171`
  (hard 8–63 reject on the NVS seed), and the hub card's
  `desktop/hub-core/src/hub_seed.rs:110-127` (8–63 printable ASCII or 64-hex,
  no open-network option). The firmware's own bound is 64 bytes.
- **No field says "create a password."** Every firmware page follows the
  masked-text pattern (`type="text"` + `.pw-masked`, never `type="password"`,
  `LESSONS_LEARNED.md` "iOS offers to invent a password"), but the shared
  portal and the display label the field only "Password", and the three iOS
  Wi-Fi `SecureField`s carry no `.textContentType`, which is the one
  unguarded surface left for the OS to misread as sign-up.

Credential storage, encryption and the platform limit are in
`scratchpad` reader detail; the summary: every shipped build keeps Wi-Fi,
MQTT, bearer token and (where present) the Ed25519 private key as plaintext
NVS entries under namespace `securacv`. No shipped partition table has an
`encrypted` flag or `nvs_keys` partition; `firmware/provisioning/` holds the
opt-in secure kit (flash encryption + Secure Boot v2) that no CI leg builds.
*(Amended 2026-10-21: `firmware.yml` compiles the kit's two envs,
compile-only, since #1744; the kit stages flash encryption and Secure Boot,
commented out, rather than enabling them, and neither would encrypt NVS.)*
NVS encryption is not configurable on either pinned Arduino core as built
(`platformio_secure.ini:63-76`, `key_at_rest.h:16-33`): the precompiled
`sdkconfig` leaves it off and `-DSDKCONFIG_DEFAULTS` is inert. Only the PIO
canary admits its posture on the wire (`key_at_rest`).

The flashers seed NVS at install time with a hand-built NVS page carrying
`wifi_pass`, `mqtt_pass` and the API token in cleartext
(`canary-local/assets/flash-core.js` `buildNvsSeedImage`,
`desktop/flash-engine/src/provisioning.rs` `build_nvs`, byte-identical by
test). Per-product `wifi_nvs` scheme (`blob` for canary/WAP, `string` for the
rest) is derived by `gen_flash.py` from the firmware source.

---

## 4. Discovery and peers today

> **After the program:** the trio and the PIO canary advertise
> `common/network/mdns_txt.h`'s TXT on port 80 and the trio serve
> `/api/status` + `/api/fleet` (`node_http.cpp`); the trio run the SCVP peer
> protocol (`peer_protocol.h`, `peer_link.cpp`) with the household fleet key
> (`fleet_key.h`) and elect a coordinator that yields to a hub; the PIO canary
> serves `/api/fleet`. The WAP and the display keep their own TXT and the
> flagship and the WAP are not on SCVP yet. Cells: `PARITY.md` C13–C18.

| | canary (PIO) | canary-wap | vision / sense / sentinel | canary-display |
|---|---|---|---|---|
| mDNS advertise | `_http._tcp` + `_securacv._tcp` on port 80; hostname **constant `canary`** for every unit; TXT `device_id fw model tls secure_port`, `model` hard-coded "XIAO ESP32S3" for the three board ports; **no `host`, no `name`** (`securacv_network.cpp:312-368`) | `_http._tcp` + `_securacv._tcp`; hostname `canary-<name>.local` / `canary-XXXX.local` + a staggered `canary.local` catch-all; TXT `device_id fw host name model dt=canary-wap role=witness broker bport` | `_securacv._tcp` on **dummy port 1** with the canonical TXT `device_id name host fw model dt role=witness broker bport` (`mdns_mgr.cpp:73-107`, byte-identical in all three); advertised although **no HTTP server answers**, so a Wall that follows the advert gets a refused connection | `_securacv._tcp` port 80, TXT `device_id name host fw model role=display dt hw` + `broker/bport` gossip while connected (`discovery.cpp:91-147`) |
| mDNS browse | `browsePeers()` queries `_securacv._tcp` into an 8-entry cache, **only when `GET /api/v1/peers` is requested** (`:767-833`, `:2512`) | `GET /api/fleet/scan` browses on a worker task (10 s cache) | none | browses `_securacv` then `_mqtt` for a broker referral (refused once `mqtt_user` is set) and browses witnesses every 20/60 s into the fleet model (`discovery.cpp:149-260`) |
| Hub (broker) discovery | compiled/configured host; portal hub step `/api/mqtt/config` | user-typed broker; publishes broker gossip for displays | NVS seed, or the phone's hub hand-off over the BLE door (sense, vision); never browses; **sense and sentinel dial the compiled placeholder `127.0.0.1:1883` forever** when nothing is provisioned, vision skips with a log line | referral from mDNS gossip or `_mqtt._tcp`; rebind after 120 s dark |
| Peer protocol | Opera mesh over ESP-NOW (Ed25519 + ChaCha20-Poly1305, X25519 pairing, 6-digit code) in `[env:full]` **only**; dev/release ship none; 30 s ± 3 s member heartbeats, liveness from verified frames (F324) | Opera mesh (FULL profile, which **no CI job compiles**): 30 s heartbeats, 90 s stale / 5 min offline, HKDF pairing; **does not interoperate with the PIO mesh** (`mesh_pair_crypto.h:32-39`) | fleet-link presence **beacon only**, unsigned: vision transmits v2 (ALERT flag + class + score) on BLE, ESP-NOW ch 6 and UDP multicast 239.83.67.86:5867; sense transmits v1 on BLE only; sentinel transmits nothing; both run a passive BLE roster scan whose result is never surfaced | receive-only: BLE chirp scan, ESP-NOW, UDP multicast, GATT pull → `FleetModel::on_beacon`; never transmits, never signs |
| Coordinator / election | `HubMonitor` + lowest-fingerprint rule exist; the canary persists a received `HUB_ELECTED`, **never acts on it and sends no hub heartbeat** (`csi_modules_integration.cpp:245-248`) | lowest-fingerprint coordinator broadcasts `HUB_ELECTED`; it governs **channel-hop proposals only**; `HubMonitor` type staged but never instantiated | none | none (presence works without a broker; ack-sync and escalation need MQTT) |
| Hub loss | MQTT reconnect 1–30 s backoff, bounded offline queue + SD backfill on reconnect | `esp_mqtt` reconnects on its own; `hub` word in `/api/fleet` is `none/down/ok` | mqtt reconnect with backoff (per `mqtt_mgr`) | broker dark 120 s → rediscover and rebind |
| `GET /api/fleet` (what the Wall reads) | **absent** (`securacv_network.cpp:1774`) | present, unauthenticated, CORS `*`, self row | **absent** (no HTTP server after provisioning) | present; self row (`chain: unknown`) + one row per heard peer |
| `GET /api/status` | bearer-gated, rich (`:1820`) | bearer/cookie-gated, rich | none | none; status spread over `/api/device`, `/api/glass`, `/api/ota/status` |

The three fleet-link transports (BLE, ESP-NOW, UDP multicast) carry the same
11/13-byte unsigned beacon and resolve through one host-tested parser
(`fleet_beacon.h`, `fleet_beacon_udp.h`): "trust never rises above presence."
Canary Link (`common/link/`) is the only authenticated datagram design in the
tree with a session, replay window and relay rules, but it is pure code with
no transmit side and a design-only key ceremony.

---

## 5. The hub and the Witness Wall

**The hub** is Home Assistant on a Pi running the kernel as an add-on
(`privacy_witness_kernel/`) plus `custom_components/securacv/`, or the Docker
sidecar. The pieces that touch Canaries:

- **Mosquitto**: one shared `canary` login minted by the installer
  (`canary-local/tools/hub_seed_apply.py`), plain 1883 by default, optional
  CA or fingerprint-pinned TLS; **no ACLs, no per-device credentials**
  anywhere in the repo.
- **`event_mqtt_bridge --fleet-peers-path`** (`src/fleet_peers.rs`)
  subscribes `securacv/<id>/{availability,status,health,chain,state,meta}`
  — **never** `events`, `sensing`, `counts`, `tamper` — pins each device's
  key from the first unsigned `health` publish (TOFU), verifies the Ed25519
  `chain` signature against that pin, and writes `fleet_peers.json`.
- **The kernel** (`src/api/mod.rs`) projects that file into `GET /api/fleet`
  (open, rate-limited, browser CORS allow-list) and serves `GET /api/sealed-log`
  to a viewer token minted by `witness_api mint-viewer-token`.
- **The HA integration** verifies per-publish Ed25519 on `chain/events/counts`
  against its own TOFU or manual pin (`signature.py`, `device_trust.py`),
  has **no `update` platform** (the firmware's own MQTT discovery creates
  HA's generic update entity; "Install" publishes `install` to
  `securacv/<id>/update/cmd`), and no fleet OTA orchestration.
- **The kernel never speaks MQTT and holds no OTA code.** Canary events
  reach Home Assistant; they never reach the kernel's sealed log.

**The Witness Wall** (`tvos/WitnessWall`, Rust core `tvos/witness-core`) is
a **pure poller**: every 10 s it GETs `/api/fleet` from one hub or from each
`_securacv._tcp` source it browsed (TXT `host`, private-host-validated,
30-day expiry), and `/api/sealed-log` from a single paired hub. It receives no
events on either path and the kernel has no push route. "Hub preferred,
direct fallback" is **not** implemented: a found or typed hub replaces the
discovered set, hub loss degrades to a stale snapshot, and hub + direct
sources cannot coexist with a verdict. Only canary-wap and canary-display can
be read directly; canary (PIO) serves no `/api/fleet` and the headless trio
serve nothing. A kernel install is not discoverable over Bonjour (no advert
from the add-on or sidecar; the 8799 host port ships disabled).

So in this repo "push events to the Witness Wall" has one concrete meaning:
**serve the roll-call contract the Wall polls** (`/api/fleet`, with the
Bonjour `host` TXT so the Wall can find the node) and **publish signed state
to the broker** so the hub's roll-call lists the node. There is no Wall
inbound endpoint to push to, and `tvos/discovery/DISCOVERY.md` is the
contract both halves must keep.

---

## 6. OTA today

> **After the program:** manifest v2 binds `board`/`chip` in the signed
> message, `min_version` is enforced, every target confirms the new slot only
> after a 60 s healthy dwell (`securacv_ota_health_tick`), `ota{state,target,
> pending_verify}` rides `/api/status`, the hub runs `securacv.staged_update`
> one node at a time, and `tools/sign-firmware` wraps the signer. Design and
> rules: [`OTA.md`](OTA.md); cells: `PARITY.md` C19–C20.

One engine, `firmware/common/ota/src/securacv_ota.{h,cpp}` (1,888 lines),
linked by canary (PIO), canary-wap, canary-display (byte-synced copies),
canary-vision, canary-sense and canary-sentinel. Facts:

- **Signing**: Ed25519 with one release key. Image signature over
  `uint32_le(size) || sha256(image)`; manifest signature over
  `"scv-manifest-v1\0"` + NUL-separated fields; BLE v2 header over
  `"scv-ble-ota-v2\0"` + fields. Public key in `ota_release_key.h`
  (key id `532429078dc47c04`), private key offline (`releaser.pem` →
  `OTA_SIGNING_KEY_PEM` secret), signer `firmware/scripts/ota_release.py`
  (keygen / sign / manifest / index / verify / ble-header) with the
  `setup_release_key.sh` ceremony. All-zero key fail-closes.
- **Manifest** (`manifest_version 1`, one per product at
  `releases/latest/download/manifest-<product>.json`): `product`, `version`,
  `min_version` (**parsed, logged, not enforced**), `url`, `sha256`, `size`,
  `signature`, `manifest_signature`, `ble_signature`, `signing_key_id`,
  `release_notes`, `release_url`. No board/chip field (board binding is by
  product id per env), no cohort or rollout field.
- **A/B + rollback**: the engine overrides `verifyRollbackLater()` so the new
  image stays `PENDING_VERIFY` until the product's boot self-test confirms;
  a required probe failure calls `esp_ota_mark_app_invalid_rollback_and_reboot`;
  an NVS version floor rises only after confirmation. **"Healthy" differs
  per target**: canary = identity + diagnostics ≥ 50 after 30 s of `loop()`
  plus a 4-boot crash-loop safe mode; WAP = "device identity" at the end of
  `setup()`; vision/sense/sentinel/display = "wifi connectivity" right after
  the join, before the broker, no dwell. Rollback is compile-checked, **not
  bench-verified** (`V1_BENCH_TEST_RUNBOOK.md` Track E unchecked).
- **Delivery**: daily jittered manifest pull (first check ~2 min after boot)
  on every linked target; HA update entity + Auto Update switch via
  device-published discovery; bearer-gated `/api/ota/{status,check,install,config}`
  on canary and WAP; LAN-open `/api/ota/{status,check,install}` on the
  display; BLE OTA and USB drop-file (both signed) on the WAP only; the dev-only
  unsigned `POST /api/ota` on canary dev builds. **No signed manual web
  upload on any release build; no hub push; no staged rollout; no OTA state
  in `/api/fleet` or on the Wall.**
- **Anti-brick on device**: size (when the manifest carries one), sha256 of
  the written partition, product-id match, https-or-opted-in-private-http.
  CI adds the slot budgets (`flavors.json` `size_guards`,
  `check_slot_budget.py` on the released bytes).

---

## 7. Trust today

> **After the program:** [`TRUST_MODEL.md`](TRUST_MODEL.md) — the household
> fleet key (HKDF of the Wi-Fi credentials) tags every SCVP frame, an open
> network honestly has no mesh, coordination is explicitly not trust, and the
> rotation of the key is hub-side and not a Home Assistant service.

- **Identity**: one Ed25519 keypair per witnessing Canary, generated on first
  boot from the hardware RNG, kept in plaintext NVS (`key_at_rest.h` Tier 0).
  The display line has no key. Fingerprint =
  `SHA256("securacv:pubkey:fingerprint" || pubkey)[0:8]`.
- **What is signed**: `chain`, `events`, `counts` (+ `sense`, `sentinel`)
  MQTT publishes, over the locked canonical
  `securacv-canary-sig|v1|<kind>|<device_id>|...` (`device_signature.h`,
  mirrored in `signature.py` and the display's `trust.cpp`). `health` (which
  carries the pubkey), `status`, `availability`, `state`, every command
  topic, the presence beacon and mDNS are **unsigned**.
- **Who pins**: three independent TOFU stores (HA, the display's `cvtrust`
  NVS, the bridge's `fleet_peers.json`) that never exchange pins; only HA has
  manual out-of-band pinning.
- **What an attacker can do** (from the readers' evidence): with the shared
  broker login (or by sniffing it off plain 1883) pre-pin a key for any
  device id, replay a signed publish within one window, invent ids, trigger
  OTA install / identify / mic commands on every tree (bounded only by the
  release key and version floor); with nothing but LAN access read every
  `/api/fleet`, appear in displays' fleet models via mDNS, rebind an
  uncredentialed display's broker by referral, POST `/api/ota` on a display;
  in radio range spoof the presence beacon for any fingerprint suffix, and
  claim an unowned device during its 30-minute first-boot window.
- **The only pairing-grade trust** is the WAP's Opera mesh (owner-confirmed
  code, per-frame Ed25519), which has a documented grindable MITM
  (`spec/canary_mesh_network_v0.md` §11.1), has never paired on a radio, and
  does not interoperate with the PIO tree's mesh.

Constraints every design in this program inherits (`AGENTS.md`): Invariants
II/III (no identity substrate, no stable ids or MACs on the wire, ObjectClass
vocabulary only), "Verified" = Ed25519 vs a pinned key, dictionary-first
vocabulary, the Beacon/Chirp invariants, `identity_transport.md` ("capability
tokens are not identity"), and `key_at_rest` rule 1 (the default Canary never
refuses to boot).

---

## 8. The provisioning clients

- **iOS** (`ios/`): provisions over BLE Improv (`NearbySetupSheet`: scan →
  pick → `ImprovWire.wifiSettings` → verdict; then the WAP's claim or the
  Sense/Vision hub hand-off) and over the SoftAP portal (`CanaryFamilySetupView`:
  `WIFI:` QR or typed key → `NEHotspotConfiguration` → `/scan` → `/join` →
  `/status`). Three Wi-Fi password fields (`CanarySetupView.swift:345`,
  `NearbyCanaryCard.swift:528`, `FleetWiFiSheet.swift:82`), all `SecureField`
  with an eye toggle and **no `.textContentType`**; `SetupPortal.passwordHint`
  adds a non-blocking "at least 8 characters" hint. The hub account creation
  (`HubSetupView.swift`) is the one legitimate create-a-password form.
- **Desktop Flasher** (`desktop/`): masked-text Wi-Fi/MQTT fields, "Use saved"
  from the OS store, "open network (no password)" checkbox, hard 8–63-byte
  reject on the NVS seed (`app.js:3991`, `provisioning.rs:171`), `maxlength=63`
  where the firmware takes 64; the hub card's Wi-Fi has no open-network path.
- **Browser flasher / Lab** (`canary-local/assets/`): same pattern, same
  `WIFI_PASS_REQUIRED` words pinned byte-equal across both flashers
  (`desktop_parity.test.js:4469-4482`), hard 8–63 reject in
  `flash-core.js:887`; the Lab's phone sheet mirrors the display portal's
  "Password" placeholder.

---

## 9. CI gates a change in this program trips

The CI map (`.github/workflows/`) in one paragraph each; the exact local
commands are in the parity program's test plan (`PARITY.md`).

- **firmware.yml**: PlatformIO build of every `flavors.json` env with the
  OTA-slot size guard right after each env; `pio check`; four arduino-cli legs
  (WAP on the latest core; display on 2.0.17 and 3.3.10); the sixteen
  `check_*_sync.sh` staged-copy guards; `make -C firmware/tests_host` and the
  four project `tests_host` Makefiles; the inline mesh/BLE/CSI g++ suites;
  Regression Guards (`regression_check.sh`, `check_common_build_reachability.py`,
  `check_ota_channels.py`, `pin_budget.py`, `check_board_registry.py`);
  route security and route budget for canary and WAP; `pytest` for
  `test_ota_release.py`; the microcopy lint over the WAP's embedded HTML.
- **canary-local.yml**: 60+ `node --test` page suites; the generator drift
  gates (`gen_display_portal.py` → `gen_csp.py`, `gen_wap.py` →
  `gen_homeassistant.py`, `gen_figures.mjs` → `gen_flash.py`…); the
  Chromium probes; the WASM build + "Dist drift check" (emsdk 6.0.3).
- **lint.yml** (unfiltered, every PR): `lint_spelling.py` (US spellings;
  the banned list lives only in its regex), `lint_fleet_word.py`,
  `lint_docs_index.py` (a new doc must be linked from `docs/README.md`),
  `lint_md_links.py`, `lint_dictionary_sync.py`, `lint_build_matrix.py`,
  `lint_platform_pins.py`, `flavor_envs.py --check-workflows`,
  `gen_firmware_sbom.py --check --validate`, `lint_wifi_join_policy.py`,
  `lint_common_lib_manifests.py`, `gen_agent_entrypoints.py --check`,
  `unittest discover -s scripts/tests`, and ~30 more.
- **features-dashboard-guard.yml**: a `FEATURES.md` dashboard cell may not
  move ✅ → ⚠️/❌ without an issue reference in the PR body.
- **python.yml** (HA integration pytest + mypy), **rust.yml** (kernel),
  **tvos.yml** (witness-core cargo + simulator tests), **desktop-hub-core.yml**
  (cargo in `desktop/{hub-core,hub-io,flash-engine,src-tauri}`),
  **viewer.yml** (the WAP's three sketch-dir node suites among others).
  iOS has no PR-time test run (nightly `ios-selfheal.yml` only).

Change-shape rules that cost real time if forgotten: a `provision.cpp` or
`wifi_join_policy.h` edit needs `gen_display_portal.py` **then** `gen_csp.py`
**then** the emulator dist rebuild; a `README.md` edit regenerates
`canary-local/devices/start.json`; a new `common/` `.cpp` needs exactly one
compile route; a version bump touches seven version strings, the SBOM, the
dist and `gen_flash.py` in that order; an `AGENTS.md` brief edit regenerates
six vendor files; any embedded WAP HTML edit regenerates `web_assets_gz.h`.

---

## 10. Gap summary against the program brief

What the brief asks for, where the repo stands, and where each gap is closed
in this program (the matrix with per-target cells is [`PARITY.md`](PARITY.md)).

| Brief item | Today | Closure |
|---|---|---|
| Wi-Fi password = "the password of the network you picked", no min length, ≤ 63/64, open allowed, show/hide, labeled | firmware already has these rules but labels the field "Password" on the shared and display portals; three client-side minimum-length rules; iOS fields unannotated | one shared credential policy header + its JS and Swift twins, labels fixed on every surface, the three minimums removed, iOS fields annotated, tests pinning each |
| SSID scan sorted by RSSI with hidden-SSID manual entry | present on every portal (manual "Join another network…") | kept; shared portal gains an open-network affordance |
| Live join validation with reasons and retry without re-scan | present (shared labels on four trees; WAP has inline strings) | WAP adopts `wifi_join_policy.h` |
| One onboarding state machine for every firmware | four portals; the shared one on three products | a pure `onboarding_fsm.h` core every portal consumes (timers, AP hold, recovery, factory-reset hold), adopted by the shared portal and the display, with the WAP and canary (PIO) consuming the pure rules through staged copies |
| SoftAP kept alive 2 min after join | 25 s on the shared portal and display; 120 s AP grace on the WAP | 120 s in the shared core |
| Multiple saved networks + auto-reconnect backoff | one slot everywhere; backoff shared on four trees | a `wifi_store.h` ring of saved networks (pure, host-tested) behind the shared portal |
| Encrypted credentials at rest | plaintext NVS everywhere; NVS encryption not configurable on the pinned cores | honest `⚠️`: posture reported on every node, the secure kit documented as the path *(amended 2026-10-21: the path is NVS encryption, after the arduino-as-IDF-component migration, roadmap item 9; the secure kit's flash encryption does not cover NVS)*; no fake encryption |
| Recovery re-raise without wipe after N minutes | present on five trees with differing rules; a once-online unit never re-raises on three | the shared core adds the "saved network gone for N minutes" raise |
| Hold-button factory reset | canary (PIO) only (5 s, wipes identity too) | 10 s hold on every board with a BOOT pin, credentials-only by default, shared pure classifier |
| Captive portal one shared page | three page templates | the shared portal's page becomes the template; the display's is reconciled to it |
| QR for the SoftAP | display glass QR; serial `[SETUP]`/`[WIFI]` lines the flashers parse | the shared `provision_core` QR grammar printed at boot on every portal |
| `GET /api/status` on every node | gated on canary and WAP; absent elsewhere | one shared status body (`node_status.h`) served unauthenticated-coarse on every node with an HTTP server, folded into `/api/fleet` where that is the public surface |
| mDNS advertise + browse everywhere | advertise on all but the headless trio's TXT is thin; browse on WAP/display/canary-on-demand | one TXT schema (`role`, `fw`, `host`, `caps`, `coord`), browse into a shared roster on every target |
| UDP multicast heartbeat + unicast events, shared schema | unsigned presence beacon only; events only via MQTT | `peer_protocol.h`: authenticated heartbeat + event frames over the existing multicast group, with a fleet key |
| Deterministic coordinator with hub hand-off | partial, [env:full]/FULL only, never acted on | oldest-uptime-wins coordinator in the shared roster, yields when a hub is seen |
| Survive hub loss, re-attach | MQTT backoff everywhere; display rebinds after 120 s | kept; coordinator covers the gap |
| Witness Wall direct and via hub | direct: WAP + display; via hub: everything that publishes signed chain | `/api/fleet` + TXT `host` on canary (PIO) and the headless trio; roll-call carries fw/OTA state |
| Pairing so only paired nodes accept each other | none below Opera | a household fleet key derived at onboarding (HKDF of the Wi-Fi credentials, rotatable by the hub), MAC on every peer frame, documented in `TRUST_MODEL.md` |
| Signed-only OTA, A/B rollback, anti-brick checks | present; `min_version` not enforced; healthy criteria inconsistent | `min_version` enforced; one shared health rule (Wi-Fi + hub-or-wall-or-selftest within a window); board/chip field in the manifest |
| Hub push, manifest pull, manual upload | pull everywhere; hub trigger via MQTT; manual only on WAP (BLE/USB) | hub-triggered install documented as the push path; signed manual upload route on nodes with a server |
| Staged rollout | none | hub-side one-at-a-time rollout service in the HA integration, abort on failure |
| OTA visibility on node / hub / Wall | `/api/ota/status`, HA entity | `ota` in the shared status body and the roll-call |
| `docs/OTA.md`, `tools/sign-firmware`, `docs/TRUST_MODEL.md`, `docs/PARITY.md` | `docs/firmware_ota.md`, `ota_release.py` | written; `tools/sign-firmware` wraps `ota_release.py` |

### After the program

Every "Closure" above landed on the branch except where [`PARITY.md`](PARITY.md)
marks a cell ⚠️ or ❌ with a footnote. The open items, so nobody has to
re-derive them from the matrix:

- **Credentials at rest** stay plaintext NVS on every shipped build; the
  posture is reported as `creds_at_rest` on every node, and NVS encryption,
  after the arduino-as-IDF-component migration, is the documented path; the
  secure kit is not, since flash encryption does not cover NVS (PARITY
  footnote 1).
- **The WAP and the display keep their own mDNS TXT** (`host`, `role`, `fw`
  are the shared keys; `caps` and `coord` are not emitted yet) and **are not
  on the SCVP peer protocol** — neither is the flagship, whose `release` slot
  is at 82.6 % (footnotes 11, 13).
- **Manual signed upload** exists only on the WAP (BLE OTA, USB drop-file);
  the other products take the hub's install or the manifest pull (footnote 9).
- **The headless trio do not mDNS-browse**; their roster is the SCVP
  heartbeat (footnote 8). **The WAP's BLE door does not reopen on recovery**,
  only its SoftAP does (footnote 4). **The display's portal takes no name**;
  naming stays on the glass and in Home Assistant.
- **The emulator dist** was rebuilt from this tree (Actions → "Rebuild emulator
  dist (pinned emsdk)") before the merge; the dist drift check and the page
  logic tests hold it to the sources.
- **Hardware**: G1–G10, D1–D4 and O1–O6 in `PARITY.md` §4 have not run on a
  bench yet; the election and the OTA dwell have no software stand-in.

The readers' full evidence (12 JSON maps, ~600 KB, every row with file:line)
was produced in this session's scratch space and distilled here; the rows
above that name a line number are the ones to re-check first if a file has
moved.
