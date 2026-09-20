# Firmware variant audit — what each build actually carries

Per-variant truth for capabilities that are easy to claim fleet-wide and easy
to ship on one product only. Every cell names its test tier honestly:
**compile-tested** means CI's PlatformIO / Arduino CLI build compiles the
path and the pure decision logic behind it is host-tested; nothing here has
been bench-tested against a broker on hardware unless the cell says so.

Where to look for the other truths: `firmware/build_matrix.json` is the
generated record of which feature flags each env builds with, and
[`firmware/FEATURES.md`](../firmware/FEATURES.md) is the narrative dashboard.
This file is narrower on purpose — one capability, every variant, and the
gap written down until it closes.

## MQTT broker transport (TLS)

**Premise, corrected (2026-09-08).** Roadmap item 12 said the headless
variants had no TLS option *while canary-wap did*. The check found no
verified TLS path anywhere: canary-wap's `tls` bool built an `mqtts://` URI
with **no CA** — an unverified handshake, which looks secured and is not —
and canary-display / -sense / -vision had no option at all. So the broker
username and password crossed the LAN in the clear on every product, and on
the wap they crossed an encrypted socket that anyone on the LAN could have
been the other end of.

**What holds now.** The decision lives once, in
`firmware/common/network/mqtt_transport_logic.h` (pure, host-tested by
`firmware/tests_host/test_mqtt_transport_logic.cpp`), and the five clients
(four products plus the `firmware/canary` tree's `securacv_mqtt` library)
apply it rather than deciding for themselves:

| Provisioned mode (byte) | Socket | Needs |
|---|---|---|
| `0` plain (default — every already-flashed unit) | `mqtt://`, unchanged | — |
| `1` CA-verified | `mqtts://`, broker chain must verify against the CA | the CA, PEM |
| `2` fingerprint-pinned | `mqtts://`, broker certificate's **SHA-256** must equal the pin | the pin (64 hex digits) |
| `3` lab | `mqtts://`, **no** peer verification | nothing — and a warning is logged on **every** connect |
| anything incomplete or unknown | **refused** — no socket, reason in the log | reprovisioning |

Fail-closed is the rule: mode `1` with no CA, mode `2` with no pin, a
malformed PEM, or a mode byte outside the table refuses to connect and says
why. Nothing ever downgrades to plain or to unverified on its own; the
unverified socket exists only as a mode chosen by name, and only on the
transports that can honor it (display / sense / vision and the
`firmware/canary` tree — not the WAP).

| Variant | MQTT stack | Plain | TLS, CA-verified | TLS, SHA-256-pinned | Lab opt-in (unverified, warns every connect) | Provisioned through | Test tier |
|---|---|---|---|---|---|---|---|
| canary-display (every flavor except nightstand-c6) | PubSubClient over `network/mqtt_transport.h` (WiFiClientSecure) | ✅ default | ✅ `mqtt_tls=1` + `mqtt_ca` | ✅ `mqtt_tls=2` + `mqtt_fp` | ✅ `mqtt_tls=3` | NVS namespace `securacv`, seeded by both flashers' broker block — the *Broker encryption* select with the CA box or fingerprint field (browser `renderWifiFields` → `mqttProvisioningToNvs`, desktop `readProvisioning` → `build_nvs`; the catalog's `broker_tls` gates the modes and `canary-local/tests/desktop_parity.test.js` pins the two forms equal). The on-glass onboarding provisions Wi-Fi only. | compile-tested (PlatformIO envs + the generated Arduino parity sketch); decision host-tested; the browser emulator compiles the plain path unchanged (`EMU_BUILD_FLAVOR` guard) |
| canary-display nightstand-c6 | plain `WiFiClient` — built with `CANARY_MQTT_PLAIN_ONLY` (`canary-display.ini`): the TLS transport put the image 1,568 bytes over its 0x1F0000 OTA slot | ✅ default | ❌ refused: a non-zero `mqtt_tls` in NVS is answered with a refusal and the reason on the log, never a plaintext socket | ❌ same | ❌ same | same NVS row — both flashers disable the TLS modes for it (catalog `broker_tls=false`, derived by `gen_flash.py` from the env's `-DCANARY_MQTT_PLAIN_ONLY`) and quote this refusal under the select, so neither seeds a mode the board will not connect with | compile-tested; returns with the next size cut or a grown slot |
| canary-sense | same shared transport | ✅ default | ✅ | ✅ | ✅ | same NVS row; the shared setup portal provisions Wi-Fi only | compile-tested; decision host-tested |
| canary-vision | same shared transport | ✅ default | ✅ | ✅ | ✅ | same NVS row; the shared setup portal provisions Wi-Fi only | compile-tested; decision host-tested |
| canary-wap | esp_mqtt (ESP-IDF) applying the same decision (staged copy of the header, drift-gated by `check_mqtt_transport_sync.sh`) | ✅ default | ✅ `mqtt.tlsmode=1` + `mqtt.ca` — the device's `/mqtt` page or `POST /api/mqtt/config` | ❌ refused at save time and at connect: esp_mqtt has no fingerprint hook — use the CA mode | ❌ refused at save time and at connect: the pinned Arduino core builds esp-tls without `CONFIG_ESP_TLS_INSECURE` (every chip's sdkconfig in framework-arduinoespressif32-libs 3.3.8), so a session with no verification option fails with `ESP_ERR_INVALID_STATE` — the mode is not offered on the `/mqtt` page | the device's own `/mqtt` page / API (NVS namespace `csi`) | compile-tested (Arduino CLI); decision host-tested |
| `firmware/canary` (PIO tree, `securacv_mqtt` lib) | same shared transport | ✅ default | ✅ `mqtt_tls=1` + `mqtt_ca` | ✅ `mqtt_tls=2` + `mqtt_fp` | ✅ `mqtt_tls=3` | same NVS row, written by the device itself — the flashers do not seed these rows (`broker_nvs=false`: this tree stores its credentials as blobs/u32, not the fleet's strings). The setup wizard's hub step carries the same *Encryption* select (four modes), CA box, fingerprint field and 8883 suggestion as the flashers; the API is `POST /api/mqtt/config` with optional `tls` (0-3) and `fp`, plus `POST` / `DELETE /api/mqtt/ca` for the PEM (raw body, bounded to the firmware's 3071 bytes), all behind the same bearer gate and rate limit as the other mutating handlers. A save is judged at save time with the shared decision (`mqtt_tls_fields.h`) and refused with the header's own reason when the connect would refuse; `GET /api/mqtt/status` (`tls`, `transport`, `tls_reason`, `ca_set` / `fp_set` presence only) and the serial `m` menu report the transport and the refusal, never the CA, pin or credentials. A reprovision reconnects without a reboot. | compile-tested by CI's `release_ha` leg only — the one env that compiles `securacv_mqtt`; decision and the API's field judgment host-tested (`test_mqtt_transport_logic`, `test_mqtt_tls_fields`); **no bench pass against a TLS broker** |

Behavior worth knowing before you flip a mode on:

- **canary-wap rows that predate the mode byte** carry only the old
  `mqtt.tls` bool. `true` now reads as mode `1` (CA) and is **refused until a
  CA is uploaded** — a unit that was quietly on the unverified `mqtts://`
  stops connecting after this update, with the reason on its serial log and
  on the `/mqtt` page, until the operator uploads the broker's CA (the WAP
  has no lab mode — see its row). That is the intended behavior, not a regression to
  paper over.
- **The port is yours.** No firmware rewrites `mqtt_port` when a TLS mode is
  set; a TLS broker normally listens on 8883. A plain listener on a
  TLS-configured port fails as *"the broker did not speak TLS on this port"*.
- **Both flashers' forms carry the three keys** (the browser flasher's
  broker block and the desktop Flasher's provisioning form share no code;
  `canary-local/tests/desktop_parity.test.js` pins them equal): a *Broker
  encryption* select — plain / CA-verified / SHA-256 fingerprint pin / lab,
  the last labeled encrypted-but-NOT-verified and never preselected — with a
  CA box shown for the CA mode and a fingerprint field shown for the pin
  mode; only the field the chosen mode uses is written. A TLS mode with the
  port still at 1883 gets an 8883 *suggestion* with a button, never a
  rewrite. Fingerprints are taken in every spelling the firmware accepts
  (any run of `:` or spaces between pairs, either case) and folded to 64 hex
  before the builders' narrower check. The catalog's `broker_tls`
  (`gen_flash.py`, from the env's `-DCANARY_MQTT_PLAIN_ONLY`) disables the
  TLS modes for the nightstand-c6 with the firmware's own reason. Host-tested
  (source-read parity plus the builder tests); the forms have not been
  exercised against a TLS broker on hardware.
- **Display broker gossip / referrals** rebind host and port only; the
  provisioned TLS mode persists. A gossiped plain `1883` endpoint on a
  TLS-mode display fails closed with that same message rather than
  downgrading.
- **Errors name the TLS reason, never the secret.** The PubSubClient products
  log `<transport>: <reason> (mbedtls -0xNNNN)` — wrong or missing CA,
  expired certificate, pin mismatch, plaintext listener. canary-wap logs the
  same from esp-tls's stack error and X.509 verify flags, and shows it on
  `/mqtt` and in `POST /api/mqtt/test`. Every text is a constant; the CA, the
  pin and the credentials are never formatted into a log line.
- **`firmware/canary` bounds each connect stage to its 8 s task watchdog.**
  That tree arms an 8 s task watchdog on the loop that connects, and the
  core's WiFiClientSecure defaults (30 s connect, 120 s handshake) or the
  shared transport's 15 s handshake would panic-reset it on a black-holed
  broker address. So `securacv_mqtt` brings the socket up itself with a 3 s
  connect and a 4 s handshake budget, feeds the watchdog, then lets
  PubSubClient send CONNECT with a 5 s socket timeout. Those are
  watchdog-derived numbers, not bench-measured ones: a legitimate handshake
  that needs more than 4 s on an S3 fails there with *TLS handshake failed*
  on the log — the recoverable side of that trade — and the first hardware
  pass should measure it.
- **The Hub's Mosquitto add-on** (installed by the one-command hub plan)
  listens on plain `1883` by default; TLS on the broker side is an add-on
  configuration the plan does not perform. See
  [Home Assistant setup](homeassistant_setup.md).

Gaps still open after this pass:

- The shared setup portal and the display's on-glass onboarding carry no
  MQTT fields at all (pre-existing); the `firmware/canary` tree's own setup
  wizard is the exception, above.
- `firmware/canary`'s adoption is compile-tested by the `release_ha` leg
  only and has not run against a TLS broker; its 4 s handshake budget is a
  watchdog constraint awaiting a bench number.
- No bench pass: nothing above has connected to a real TLS broker on
  hardware. The claim is *compile-tested*.
