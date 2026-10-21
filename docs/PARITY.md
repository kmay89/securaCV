# Parity — every firmware target against the onboarding, discovery and OTA brief

**Status as of 2026-10-18**, branch `feat/firmware-parity-onboarding-ota`
(the before-picture is [`FIRMWARE_MAP.md`](FIRMWARE_MAP.md), read from `main`
at `2e715eb` before any code changed). Every cell below is one of:

- ✅ — the row's firmware does what the column says, through the shared core
  named in the column key, and a test or a build on this branch shows it;
- ⚠️ *(reason)* — it does part of it, or does it its own way, and the reason
  says exactly which part is missing or different;
- ❌ *(reason)* — it does not, and the reason says why (a design choice, a
  board without the hardware, or work left for the next program).

A cell never rounds up. Where the column names a shared module, "✅" means that
module is compiled into the target (directly from `firmware/common/` or as a
byte-checked staged copy), not that the same idea was written again by hand.
The vocabulary is the project's ([`GLOSSARY.md`](GLOSSARY.md)): a group of
Canaries is a **fleet**; **the hub** is Home Assistant plus the kernel; **the
Witness Wall** is the Apple TV app that polls `GET /api/fleet`.

---

## 1. The matrix

### Column key

| # | Column | The shared rule it refers to |
|---|---|---|
| C1 | SoftAP + captive portal | `common/network/setup_portal.{h,cpp}` + `setup_portal_logic.h` page and routes, or a board's own page that keeps every rule of `onboarding_fsm.h` |
| C2 | BLE setup door (Improv) | `common/network/improv_core.h` + `improv_ble.cpp` |
| C3 | Typed SSID + QR at boot | `onboarding_fsm.h` `ap_ssid()` → `SecuraCV-<type>-XXXX`; `announce_line()` and `qr_line()` on serial (`[SETUP] QR WIFI:T:WPA;S:…;P:…;;`) |
| C4 | Wi-Fi password semantics | `wifi_credential_policy.h`: the picked network's password, no prefill, show/hide, no minimum, 0–63 bytes, open networks allowed (§1 of the program) |
| C5 | RSSI-sorted scan + hidden SSID | the portal's `/scan` sorted strongest first with "Join another network…" |
| C6 | Live join reasons | `wifi_join_policy.h` labels (Network not found / Wrong password / No address from the router / Couldn't connect) shown while the join runs, retry without a re-scan |
| C7 | AP hold after join | `onboarding_fsm.h` `Timing`: 120 s after the join, 1.6 s after the phone acknowledges |
| C8 | Saved-network ring + backoff | `wifi_store.h` (4 networks, last-good first, rotate on Network-not-found) under the shared retry policy |
| C9 | Credentials encrypted at rest | NVS encryption on the NVS that holds the credential blobs (flash encryption alone does not cover NVS) |
| C10 | Recovery re-raise without wipe | `onboarding_fsm.h` `recovery_reraise_due()`: 10 min offline after ever being online → SoftAP (and the BLE door where there is one) come back, nothing forgotten |
| C11 | Hold-button reset | `common/io/reset_hold.h`: warn at 3 s, reset at 10 s (credentials and settings; identity kept), restart on release |
| C12 | `GET /api/status` | `common/network/node_status.h` body: node, fw, uptime, ip, rssi, onboarding, hub, peers_seen, wall, peer, coord, ota{…}, creds_at_rest, clock — no MAC, no key, no broker address |
| C13 | mDNS advertise | `_securacv._tcp` on port 80 with `common/network/mdns_txt.h`'s TXT (`device_id name host fw model dt role caps coord broker bport`) |
| C14 | Peer roster | a table of the other nodes on the LAN, kept without a hub |
| C15 | Peer protocol | `common/network/peer_protocol.h` SCVP frames (multicast heartbeat every 10 s, unicast events) tagged with the household fleet key (`fleet_key.h`) |
| C16 | Coordinator | `peer_protocol.h` `elect()`: oldest uptime wins, a hub anywhere wins; `coord` word in `/api/status` |
| C17 | Hub loss and reattach | broker reconnect with backoff; `hub` word `none / down / ok`; nothing stops sensing |
| C18 | Witness Wall | `GET /api/fleet` served directly (TXT `host` so the Wall finds it) and the signed `chain` published to the broker for the hub's roll-call |
| C19 | OTA signed, bound, floored | `common/ota/securacv_ota.cpp`: Ed25519 image + manifest signatures, manifest v2 `board`/`chip` binding, `min_version` enforced, size/sha256/product checks |
| C20 | OTA A/B, delivery, visibility | pending-verify slot confirmed only after a 60 s healthy dwell (`securacv_ota_health_tick`), the three delivery paths (hub install, manifest pull, manual upload), `ota{state,target}` in `/api/status` and the roll-call |

### Products

| Target (CI envs) | C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8 | C9 | C10 |
|---|---|---|---|---|---|---|---|---|---|---|
| **canary-sense** (`default`, `wellbeing`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ¹ | ✅ |
| **canary-vision** (`default`, `xiao-c3`, `xiao-s3`, `c3-super-mini`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ¹ | ✅ |
| **canary-sentinel** (`door`, `lite`) | ✅ | ❌ ² | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ¹ | ✅ (portal only) |
| **canary-wap** (`default`, `usbdrive`) | ⚠️ ³ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ¹ | ⚠️ ⁴ |
| **canary (PIO)** (`dev release release_ha full esp32cam esp32-wroom freenove-s3`) | ⚠️ ³ | ❌ ⁵ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ¹ | ✅ ⁶ |
| **canary-display** (21 envs, 7 released) | ⚠️ ³ | ❌ ⁷ | ✅ (and on the glass) | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ¹ | ✅ |

| Target | C11 | C12 | C13 | C14 | C15 | C16 | C17 | C18 | C19 | C20 |
|---|---|---|---|---|---|---|---|---|---|---|
| **canary-sense** | ✅ | ✅ open on :80 | ✅ | ✅ ⁸ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ⁹ |
| **canary-vision** | ✅ | ✅ open on :80 | ✅ | ✅ ⁸ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ⁹ |
| **canary-sentinel** | ✅ | ✅ open on :80 | ✅ | ✅ ⁸ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ ⁹ |
| **canary-wap** | ✅ | ✅ bearer-gated ¹⁰ | ⚠️ ¹¹ | ⚠️ ¹² | ❌ ¹³ | ❌ ¹³ | ✅ | ✅ | ✅ | ✅ |
| **canary (PIO)** | ✅ (30 s wipe opt-in) | ✅ bearer-gated ¹⁰ | ✅ | ⚠️ ¹² | ❌ ¹³ | ❌ ¹³ | ✅ | ✅ | ✅ | ⚠️ ⁹ |
| **canary-display** | ⚠️ ¹⁴ | ✅ open on :80 | ⚠️ ¹¹ | ✅ | ⚠️ ¹⁵ | ❌ ¹⁵ | ✅ | ✅ (direct) ¹⁶ | ✅ | ⚠️ ⁹ |

### Not products

| Target | Verdict |
|---|---|
| canary-ota (`firmware/projects/canary-ota/`) | ❌ every column — an ESP-IDF teaching harness with the older SHA-256-only engine; no CI job compiles it and it has no onboarding. Left as it was, by design (`firmware/VARIANT_POLICY.md`). |
| canary-companion, canary-tincan | not applicable — Phase-0 pure cores with no runtime, no network stack and no env; their host tests still pass (`make -C firmware/projects/<p>/tests_host`). |
| canary-fence-guard | not applicable — a concept whose `main_stub.cpp` refuses to compile. |
| the emulator (`canary-local/emulator/`) | inherits the display row: it compiles `provision.cpp`, `runtime_config.cpp` and `main.cpp`, which all changed, so the committed `dist/` was rebuilt from this tree by Actions → "Rebuild emulator dist (pinned emsdk)" before the merge (`CLAUDE.md`, "Generated files"); the dist drift check and the page logic tests hold it to the sources. |

### Footnotes — every ⚠️ and ❌, with its reason

1. **Plaintext NVS, reported honestly.** No shipped build encrypts its
   credentials: NVS encryption is not configurable on either pinned Arduino
   core as built (the precompiled `sdkconfig` leaves it off; see
   `firmware/common/identity/key_at_rest.h` and `FIRMWARE_MAP.md` §3). Rather
   than fake it, every `/api/status` reports `creds_at_rest: "plaintext-nvs"`,
   and the path is NVS encryption, which needs the arduino-as-IDF-component
   migration (`firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` item 9): the opt-in
   secure kit (`firmware/provisioning/`) does not encrypt NVS, since flash
   encryption does not cover it and the kit's NVS-encryption setting is
   inert under `framework = arduino`. This is the one brief item the
   program could not close in firmware, and it says so on every node.
2. **No BLE in the sentinel build**, by design: it is a door/window/hallway
   sensor on a C6 whose radio budget goes to the fleet beacon; the shared
   portal is its only door. `improv_ble.cpp` compiles its no-op stub so the
   status body can say `ble_door: 0`.
3. **Own page, shared rules.** The WAP (`setup_wizard.h` + `companion_pwa.h`),
   the PIO canary (`lib/securacv_setup`) and the display (`src/net/provision.cpp`)
   keep their own captive pages — each is a product surface with its own
   look — but every rule they apply is `onboarding_fsm.h`'s (typed SSID,
   announce and QR lines, the 120 s / 1.6 s hold, the join words, the reset
   hold, the saved-network ring), consumed through staged byte-checked copies
   (`check_onboarding_sync.sh`) or the display's `setup.sh regen`. One
   template is the goal; one rule set is what is shipped and tested.
4. **The WAP re-raises its SoftAP, not its BLE door.** On a lost network the
   management AP comes back as before; the Improv door stays on its first-boot
   window / BOOT-tap rule (`canary_wap.ino`). Re-opening the door on recovery is
   a WAP-specific change left for its own review (the door hands out claim
   tickets, so its window is a trust decision, `TRUST_MODEL.md` §3).
5. **The flagship builds with Bluetooth off** (`CONFIG_BT_ENABLED=n`); it
   onboards over its SoftAP, USB console and the flashers. Unchanged.
6. **The PIO canary's AP is always-on AP+STA**: it re-raises on STA loss at
   once (there is no 10-minute wait to re-raise something that never went
   away); nothing is forgotten. The shared 120 s / 1.6 s hold now governs when
   it is dropped after a join.
7. **A screen beats a radio**: the display shows a join QR on the glass and
   has never carried a BLE door. Unchanged, by design.
8. **The roster is the heartbeat, not an mDNS query.** The headless boards
   advertise over mDNS and keep their peer table from SCVP heartbeats on the
   multicast group; they do not run an mDNS *browse* (Arduino's `MDNS.queryService`
   blocks the loop for seconds). The Wall and the displays, which do browse,
   find them through the advert.
9. **Two of the three delivery paths.** Manifest pull and hub-driven install
   (the HA `update` entity, and `securacv.staged_update` for a fleet) work on
   every product; a **manual signed upload** route exists only on the WAP (BLE
   OTA and the USB drop-file). The headless boards and the display have no
   file-upload route (the PIO canary's `POST /api/ota` is the dev-only,
   unsigned one). `OTA.md` §4 names the URL-install alternative a signed
   manifest on any HTTP server gives you.
10. **Gated, with the same body.** The WAP and the PIO canary answer
    `/api/status` behind their bearer token (they also serve a rich, gated
    status and carry more than the coarse body); the shared `node_status`
    body rides inside it. The three headless boards and the display answer it
    open on port 80, coarse by construction.
11. **Own TXT, same essentials.** The WAP (`device_id fw host name model dt
    role broker bport`) and the display (`… role dt hw broker bport`) keep the
    TXT they already advertised — the `host`, `role` and `fw` keys the Wall
    and the displays read are the shared schema's — but do not yet emit `caps`
    and `coord`. Moving both onto `mdns_txt.h` is a small follow-up left for
    the next pass over their sketches.
12. **On demand, not a roster**: the PIO canary browses `_securacv._tcp` only
    when `GET /api/v1/peers` is called; the WAP on `GET /api/fleet/scan`.
13. **No SCVP on the WAP or the flagship this round.** Both carry the Opera
    mesh in their FULL / `[env:full]` profiles, which no shipped image enables;
    the fleet-key peer protocol was wired into the three headless boards first
    (they had nothing). The flagship's `release` image is at 82.6 % of its
    slot; adding `peer_link.cpp` there is a budget decision for its own PR.
    Both still *appear* to peers: they advertise over mDNS and publish signed
    `chain` to the broker, so a hub's roll-call lists them.
14. **Boards without a BOOT pin have no hold.** The reset hold runs on every
    display flavor with a button (watch, nightstands, 1.47", touch, AMOLED);
    the 7" dash family has no button, so its Settings → Reset stays the path.
15. **The display is a viewer.** It listens to the fleet beacon on all three
    transports and shows what it hears, but does not transmit SCVP frames and
    never coordinates: it has no witness role to coordinate for.
16. **Direct only.** The display's `/api/fleet` lists itself and the peers it
    heard; it holds no device key, so it publishes no signed `chain` and the
    hub's roll-call does not list it as a witness. By design (`TRUST_MODEL.md` §1).

---

## 2. Host-native unit tests

Every rule in the matrix that can be stated without a radio is a pure header
under `firmware/common/`, compiled and run on the host with `g++ -Wall -Wextra
-Werror` by `make -C firmware/tests_host` (the whole suite ran green on this
branch; the suites this program added or changed are listed first).

| Suite (`firmware/tests_host/`) | Pins |
|---|---|
| `test_onboarding_fsm.cpp` | the five state words; `derive()` from the facts; the 120 s / 1.6 s AP teardown; the 10 min recovery re-raise; `ap_ssid()` for every node type at the 32-byte cap; the announce and QR lines byte for byte |
| `test_wifi_store.cpp` | the four-slot ring, last-good-first `pick()`, rotation on Network-not-found, legacy single-slot import, the 412-byte CRC-checked blob |
| `test_reset_hold.cpp` | warn at 3 s, reset at 10 s, opt-in wipe, the Released edge and `released_stage` |
| `test_node_status.cpp` | the `/api/status` body: every key, omitted-when-unknown integers, no MAC and no key in any field |
| `test_clock_sync.cpp` | the believable-epoch rule, the two SNTP pools by name with the hub as the third server, the 30 s sync grace, the source words |
| `test_mdns_txt.cpp` | the TXT schema: key order, `caps` bit string round trip, `coord` words, host-label validity |
| `test_fleet_key.cpp` | HKDF-SHA256 of the Wi-Fi credentials with the fixed info string, refusal on an open network, HMAC tag and constant-time compare, the key id |
| `test_peer_protocol.cpp` | SCVP frame encode/decode, tag verification, replay by sequence and uptime, table expiry at 35 s, the election (oldest uptime wins, hub anywhere wins) |
| `test_canary_onboarding_glue.cpp` | the PIO canary's reset scopes and join-reason mapping onto the shared words |
| `test_wifi_credential_policy.cpp`, `test_wifi_join_policy.cpp`, `test_setup_portal_logic.cpp`, `test_improv_core.cpp`, `test_hub_handoff.cpp`, `test_provisioning_gate.cpp` | the §1 password rules, the join labels and backoff, the portal's request parsing, the BLE door wire format, the hub hand-off, the claim gate (pre-existing; all still green), and since F342 the WAP's one-asker BOOT tap (`tap_arbitrate()`; the flagship's tap still goes to the first asker, F354) |
| `firmware/common/ota/test_ota_logic.cpp` | manifest v2 (`board`/`chip` in the signed message, a v1 manifest carrying them refused), `min_version` enforced, board mismatch refused, the 60 s confirm dwell, the lowercase state words, the friendly strings (366 checks) |
| `firmware/projects/canary-display/tests_host/test_onboard_layout.cpp` | the glass QR (version 4, 33 modules) composed in portrait and landscape for the typed SSID |
| `firmware/projects/canary-wap/tests_host/` | the WAP's provisioning and BLE OTA policy suites (pre-existing; green) |

Beside the C++ suites:

- `python3 -m pytest firmware/scripts/test_ota_release.py` — the signer: v1
  and v2 manifests, `--board/--chip`, version-aware `verify` (45 tests).
- `python3 -m pytest custom_components/securacv/tests` — the hub side,
  including `test_staged_update.py` (plan, one node at a time, abort on a
  failed or refused install, timeout) and the vocabulary pins (853 tests).
- `python3 -m unittest discover -s scripts/tests` — among them
  `test_lint_wifi_join_policy.py` (shims and the shared supervisor) and
  `test_wap_name_examples.py` (the typed SSID example in every doc).
- `node --test canary-local/tests/*.test.js` — 59 suites, all green on this
  branch: `onboard`, `flash`, `flash_epic`, `desktop_parity` (the two
  flashers parse the same `[SETUP]` lines), `fingerprint_examples` (every
  SSID a generated page shows is the test key's, format strings excluded),
  `wap`, `sense`, `vision`, `homeassistant`, `canary_local`.

Run them all with:

```sh
make -C firmware/tests_host
make -C firmware/projects/canary-wap/tests_host
make -C firmware/projects/canary-display/tests_host
python3 -m pytest firmware/scripts/test_ota_release.py custom_components/securacv/tests -q
python3 -m unittest discover -s scripts/tests
for f in canary-local/tests/*.test.js; do node --test "$f"; done
```

---

## 3. Builds — every target, size and slot fit

Built in this container with the repo's own `platformio.ini` files, from an
offline mirror of the pinned platforms (pioarduino core 3.3.8 for the C6
boards and the WAP; `espressif32@6.9.0` / Arduino 2.0.17 for the rest); CI's
`firmware.yml` builds the same env list on the pushed branch and applies the
`flavors.json` slot guards after each. "Slot" is the app partition the env's
partition table gives each A/B half.

<!-- BUILD_TABLE:begin -->
| Product | Env | Result here | Flash used / slot | RAM used / total |
|---|---|---|---|---|
| firmware/canary | `dev` | ✅ built | 1,821,837 / 1,966,080 (92.7 %) | 101,064 / 327,680 (30.8 %) |
| firmware/canary | `release` | ✅ built | 1,623,417 / 1,966,080 (82.6 %) | 100,808 / 327,680 (30.8 %) |
| firmware/canary | `release_ha` | ✅ built | 1,735,101 / 1,966,080 (88.3 %) | 116,076 / 327,680 (35.4 %) |
| firmware/canary | `full` | ❌ not buildable here — the pioarduino platform wants openocd from GitHub and the proxy refuses its TLS | — | — |
| firmware/canary | `esp32cam` | ✅ built | 1,648,789 / 1,966,080 (83.9 %) | 100,472 / 327,680 (30.7 %) |
| firmware/canary | `esp32-wroom` | ✅ built | 1,479,125 / 1,966,080 (75.2 %) | 91,088 / 327,680 (27.8 %) |
| firmware/canary | `freenove-s3` | ✅ built | 1,534,277 / 1,966,080 (78.0 %) | 96,108 / 327,680 (29.3 %) |
| firmware/projects/canary-wap | `canary-wap-default` | ✅ built | 2,348,406 / 3,342,336 (70.3 %) | 123,184 / 327,680 (37.6 %) |
| firmware/projects/canary-wap | `canary-wap-usbdrive` | ✅ built | 2,417,946 / 3,342,336 (72.3 %) | 152,656 / 327,680 (46.6 %) |
| firmware/projects/canary-vision | `canary-vision-default` | ✅ built | 1,372,714 / 1,966,080 (69.8 %) | 70,988 / 327,680 (21.7 %) |
| firmware/projects/canary-vision | `canary-vision-xiao-c3` | ✅ built | 1,381,938 / 1,966,080 (70.3 %) | 71,316 / 327,680 (21.8 %) |
| firmware/projects/canary-vision | `canary-vision-xiao-s3` | ✅ built | 1,331,905 / 3,342,336 (39.8 %) | 77,656 / 327,680 (23.7 %) |
| firmware/projects/canary-vision | `canary-vision-c3-super-mini` | ✅ built | 1,372,698 / 1,966,080 (69.8 %) | 70,988 / 327,680 (21.7 %) |
| firmware/projects/canary-display | `canary-display-dash` | ✅ built | 2,319,359 / 6,553,600 (35.4 %) | 155,044 / 327,680 (47.3 %) |
| firmware/projects/canary-display | `canary-display-playground` | ✅ built | 2,365,871 / 6,553,600 (36.1 %) | 155,692 / 327,680 (47.5 %) |
| firmware/projects/canary-display | `canary-display-dash-b` | ✅ built | 2,367,107 / 6,553,600 (36.1 %) | 155,692 / 327,680 (47.5 %) |
| firmware/projects/canary-display | `canary-display-dash-rs485` | ✅ built | 2,321,883 / 6,553,600 (35.4 %) | 155,116 / 327,680 (47.3 %) |
| firmware/projects/canary-display | `canary-display-dash-can` | ✅ built | 2,321,947 / 6,553,600 (35.4 %) | 155,116 / 327,680 (47.3 %) |
| firmware/projects/canary-display | `canary-display-dash-vault` | ✅ built | 2,359,751 / 6,553,600 (36.0 %) | 155,268 / 327,680 (47.4 %) |
| firmware/projects/canary-display | `canary-display-dash-sd` | ✅ built | 2,378,423 / 6,553,600 (36.3 %) | 155,396 / 327,680 (47.4 %) |
| firmware/projects/canary-display | `canary-display-dash-rtc` | ✅ built | 2,323,679 / 6,553,600 (35.5 %) | 155,124 / 327,680 (47.3 %) |
| firmware/projects/canary-display | `canary-display-dash-espnow` | ✅ built | 2,324,675 / 6,553,600 (35.5 %) | 155,708 / 327,680 (47.5 %) |
| firmware/projects/canary-display | `canary-display-dash-ble5` | ✅ built | 2,319,671 / 6,553,600 (35.4 %) | 155,044 / 327,680 (47.3 %) |
| firmware/projects/canary-display | `canary-display-dash-modes` | ✅ built | 2,380,043 / 6,553,600 (36.3 %) | 156,836 / 327,680 (47.9 %) |
| firmware/projects/canary-display | `canary-display-dash-mic` | ✅ built | 2,338,939 / 6,553,600 (35.7 %) | 155,868 / 327,680 (47.6 %) |
| firmware/projects/canary-display | `canary-display-dash7` | ✅ built | 2,345,367 / 6,553,600 (35.8 %) | 156,116 / 327,680 (47.6 %) |
| firmware/projects/canary-display | `canary-display-nightstand7` | ✅ built | 2,345,711 / 6,553,600 (35.8 %) | 155,380 / 327,680 (47.4 %) |
| firmware/projects/canary-display | `canary-display-watch` | ✅ built | 1,850,461 / 3,342,336 (55.4 %) | 214,124 / 327,680 (65.3 %) |
| firmware/projects/canary-display | `canary-display-watch-modes` | ✅ built | 1,859,809 / 3,342,336 (55.6 %) | 215,108 / 327,680 (65.6 %) |
| firmware/projects/canary-display | `canary-display-nightstand-s3` | ✅ built | 1,807,141 / 6,553,600 (27.6 %) | 205,024 / 327,680 (62.6 %) |
| firmware/projects/canary-display | `canary-display-touch169` | ✅ built | 1,848,729 / 6,553,600 (28.2 %) | 213,812 / 327,680 (65.3 %) |
| firmware/projects/canary-display | `canary-display-amoled241` | ❌ not buildable here — the offline library mirror resolves no FS for SD_MMC (CI builds it) | — | — |
| firmware/projects/canary-display | `canary-display-nightstand-c6` | ✅ built | 1,915,680 / 2,031,616 (94.3 %) | 128,772 / 327,680 (39.3 %) |
| firmware/projects/canary-display | `canary-display-nightlight-c3` | ✅ built | 1,537,310 / 2,031,616 (75.7 %) | 194,132 / 327,680 (59.2 %) |
| firmware/projects/canary-sense | `canary-sense-default` | ✅ built | 1,802,308 / 1,966,080 (91.7 %) | 74,772 / 327,680 (22.8 %) |
| firmware/projects/canary-sense | `canary-sense-wellbeing` | ✅ built | 1,805,644 / 1,966,080 (91.8 %) | 74,812 / 327,680 (22.8 %) |
| firmware/projects/canary-sentinel | `canary-sentinel-door` | ✅ built | 1,401,448 / 1,966,080 (71.3 %) | 63,384 / 327,680 (19.3 %) |
| firmware/projects/canary-sentinel | `canary-sentinel-lite` | ✅ built | 1,111,166 / 1,310,720 (84.8 %) | 58,628 / 327,680 (17.9 %) |

36 of 38 CI environments built in this container; 2 could not be built here (toolchain or library the offline mirror does not carry — the reason is in the row); 0 were not attempted here. `firmware.yml` builds all of them on the pushed branch and applies the slot guards.
<!-- BUILD_TABLE:end -->

Three sizes to keep an eye on: **canary-sense** sits at 91.7 % of its
0x1E0000 slot (the C6's 4 MB flash with two app halves) — the shared
supervisor, the HTTP status server and the peer link fit, but the next
feature on the Sense pays for itself in flash; **canary (PIO) `release`** is
at 82.6 %, the reason the peer link is not on the flagship yet (footnote
13); and the **C6 nightstand** (`canary-display-nightstand-c6`) had 2,624
bytes of its 0x1F0000 slot left on `main`, so this program's additions put
its `.bin` 8,912 bytes over and CI's slot guard refused it. The cut its
env's budget notes had scoped landed: the lean build also drops the LVGL
widget classes no face creates (`lv_conf.h` under `CD_LEAN_BUILD`),
measured at −35,920 bytes of `.bin`, which leaves about 27 KB spare. The
next cut (no C++ unwind tables on the env's own sources, −65 KB) is
measured and written in the env's notes, not taken. The table's C6 row is
the lean image after the cut.

---

## 4. End-to-end test plan (hardware)

What this container cannot do is put a radio in a room. The bench plan is
written so one person with one of each board can run it in an afternoon;
`GET /api/status` is the oracle throughout.

**Onboarding and recovery — `manual_test_plan_captive_portal.md` section G,
G1–G10.** Factory-fresh boot with the typed SSID and the QR line (G1); the
join through `provisioning` and `joining` (G2); the two-minute hold and the
1.6 s close after the phone acknowledges (G3, G4); the ten-minute recovery
re-raise with nothing forgotten (G5); two saved networks and rotation on
Network-not-found (G6); the 3 s / 10 s hold and the identity surviving it
(G7); the status body on every node (G8); `/api/fleet` and `wall: "direct"`
(G9); two nodes electing a coordinator, yielding to a hub, and the honest
`peer: off` on an open network (G10).

**Discovery and the Wall.**

| # | Step | Expect |
|---|---|---|
| D1 | Open the Witness Wall with no hub paired, a Sense, a Vision and a PIO canary on the LAN | each appears from its `_securacv._tcp` advert (TXT `host`, port 80) within one browse cycle; the Wall's rows read their `/api/fleet` directly |
| D2 | Pair the Wall with a hub | the hub's roll-call replaces the direct set (the Wall's own "hub replaces direct" rule, unchanged by this program) and lists every node that publishes signed `chain` — the display is absent, by design |
| D3 | Pull the hub's network cable for 3 minutes | every node's `hub` word goes `ok → down`; sensing, the peer heartbeat and the local status keep running; `coord` moves from `hub` to the oldest node; plug it back: `hub: ok` and `coord: hub` within one heartbeat |
| D4 | `dns-sd -B _securacv._tcp` (or `avahi-browse -r`) on a laptop | the trio and the PIO canary carry the full TXT (`role caps coord`); the WAP and the display carry theirs (footnote 11) |

**OTA — `OTA.md` §2–§5.**

| # | Step | Expect |
|---|---|---|
| O1 | Sign a build with `tools/sign-firmware` for the right `--board/--chip`, publish the manifest, press Install in HA | the node downloads, verifies, reboots into the new slot, and `/api/status` reads `ota.pending_verify: true` until 60 s of healthy uptime, then the key disappears (confirmed) and `fw` is the new version; the roll-call row carries the same `fw` |
| O2 | Sign a build for the **wrong** board (`--board xiao-c3` for a Sense) | refused before download: `ota.state: error`, and `/api/ota/status` (or the HA entity) says "Manifest is for another board or chip"; nothing written |
| O3 | Publish a manifest whose `min_version` is above the node's version | refused the same way with "Running version below the manifest's min_version" |
| O4 | Flash an image whose boot self-test cannot pass (a build with the Wi-Fi join deliberately broken) | the 60 s dwell never completes, the slot is marked invalid and the node rolls back: the previous `fw` is running again on the next status read, `ota.pending_verify` is gone, and the hub's update entity still offers the new version |
| O5 | `securacv.staged_update` on three nodes, one of which is powered off mid-install | nodes update one at a time; the rollout stops at the dead one; the service raises `rollout_aborted` naming it; the remaining node is untouched |
| O6 | On the WAP, drop a signed `.bin` on the USB drive | installs as before (the manual path, footnote 9) |

**Degradation words (no radio needed, but worth a glance on hardware).**
On an open Wi-Fi network: `peer: off`, serial says "no fleet key on an open
network". With no hub: `hub: none`, `coord` is a node. With a stale Wall
poll: `wall: none` after 60 s.

---

## 5. Decisions made without asking (and where they are written down)

The brief said to make the call when the codebase implied it and note it.
These are the calls.

- **The SSID suffix is not the MAC.** The brief's `SecuraCV-<nodetype>-<last4mac>`
  would put a stable radio identifier on the air; AGENTS.md Invariant II
  forbids exactly that. The suffix is the node's existing pseudonymous
  4-character suffix (fingerprint- or pseudonym-derived), so the SSID reads
  `SecuraCV-sense-4dC2`. The type word is in the name as asked.
- **A 10 s hold resets credentials and settings, not identity.** Wiping the
  witness key would orphan the device from its hub, its displays and the Wall;
  only the PIO canary offers a 30 s opt-in wipe, announced first (`reset_hold.h`,
  `FAQ.md`).
- **SNTP on the headless boards, hub third.** Sense, Vision and the Sentinel
  gained a wall clock through the shared supervisor (the brief's "NTP with hub
  fallback"); the two public pools are first and the hub third
  (`clock_sync.h`), the destinations are named at the call site so the
  phone-home scan matches them, and `docs/security/SECURITY_MODEL.md`
  discloses the new path. Their records still stamp `ts_ms` from uptime.
- **`/api/status` is open and coarse on nodes that had no HTTP server**, and
  rides inside the bearer-gated status on the two that already had one. The
  body never carries a MAC, a key or the broker address, so it is safe to be
  open; the roll-call the Wall polls was already open on the same terms.
- **The fleet key is derived, not provisioned.** HKDF of the Wi-Fi credentials
  with a fixed info string — every node that joins the household network can
  derive it, nothing new is typed, and an open network honestly has none
  (`TRUST_MODEL.md` §3). Rotation is hub-side and deliberately **not** a Home
  Assistant service (the integration's rule: services never mutate trust).
- **Manifest v2 ships beside v1.** Devices accept v1 manifests without
  `board`/`chip` until every product's release pipeline passes them; a v1
  manifest that *carries* them is refused, so nobody can smuggle a binding
  into an unsigned field (`OTA.md` §1).
- **Staged rollout lives in the HA integration**, driving the existing
  `update` entities one node at a time, because that is where the fleet view
  and the install command already were (`OTA.md` §5).

---

## 6. What still needs hardware

- G1–G10, D1–D4, O1–O6 above, on one of each board. The SCVP election and the
  60 s OTA dwell are the two behaviors with no software stand-in at all.
- The emulator needs none: its dist was rebuilt from this tree by Actions →
  "Rebuild emulator dist (pinned emsdk)" before the merge, so the Lab's
  onboarding preview already shows the shared portal.
- Swift: the iOS `NearbyCanaries.suffix(ofSetupSSID:)` and
  `SetupPortal.isSetupNetwork` changes and the tvOS Wall were checked by
  reading and by their unit tests' expectations; there is no Xcode here, so
  the macOS CI legs are their build.

What changed per target, in one line each, is in the pull request's summary
and in [`FIRMWARE_MAP.md`](FIRMWARE_MAP.md) §10 ("after").
