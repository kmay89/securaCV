# Setup profiles, the secret drawer, and the fleet book

The desktop Flasher's answer to three adjacent frustrations: retyping the
same Wi-Fi and broker settings on every board (and fat-fingering one of
them), losing track of which Canaries exist and what they run, and having no
way to update a deployed board without a USB cable. Shipped in Flasher 0.4.0;
the moving parts live in `desktop/src/app.js` (`secretStore`, `fleetBook`),
`desktop/src-tauri/src/secret_store.rs`, `desktop/src-tauri/src/fleet.rs`,
and `desktop/flash-engine/src/provisioning.rs`.

## The setup profile — type it once

The Canary form and the hub form share one memory:

- **Non-secrets** (device name, Wi-Fi name, broker host/port/user and the
  hub board choice) persist live in the app's local prefs, as they already
  did; the broker encryption mode with its CA certificate or SHA-256
  fingerprint — a public certificate and its hash — joins them. Choosing a
  board that refuses TLS (the plain-only nightstand-c6) shows Plain for that
  flash without overwriting the remembered mode, and a board with no broker
  block leaves it alone. The one value not carried forward is the lab
  (unverified) TLS mode: a per-flash choice, never preselected. After a
  flash, the installation receipt names the broker encryption mode that was
  sealed — for the CA mode the certificate's byte count, for the pin mode
  the SHA-256 fingerprint itself (public data: the broker presents that
  certificate to every client on the LAN) — never the PEM or a password, and
  never a claim that the board connected; the browser flasher's done card
  says the same line (the flash engine's `broker_receipt.rs` and
  `flash-core.js`, held equal by `desktop_parity.test.js`).
- **Secrets** now persist too, with consent: the "Remember" checkbox under
  each form routes the Wi-Fi password (keyed by SSID, so two homes don't
  overwrite each other), the broker password (keyed by host + user), and the
  hub's Home Assistant login into the **OS credential store** — the macOS
  Keychain or the Windows Credential Manager (`secret_store.rs`, `keyring`
  crate, platform-gated). On Linux there is no packaged backend, so the app
  falls back to its prefs file **and the consent note says so** — the same
  trade the browser flasher's opt-in Wi-Fi memory makes.
- Autofill never overwrites something typed, and announces itself ("Filled
  from your setup profile…") instead of silently materializing a secret.
- "Reset the app's memory" (About page) sweeps every stored secret from the
  OS store via the tracked key list, plus the book and prefs.

Because the hub login survives a relaunch (opt-in), the first-boot
companion's promise is finally unconditional: an app reopened mid-first-boot
restores the login from the drawer and still finishes Home Assistant's
onboarding itself (`hubMaybeResume`), instead of apologizing that the
password died with the window.

A board with no hub yet keeps the `homeassistant.local` default: a hub
built later answers at exactly that name, so Canaries flashed today find it
by themselves the day it exists.

## The device API token — minted at flash time

Both blob-scheme firmwares load their local-API bearer credential from NVS
**before** deriving one from the device key (`securacv_auth.cpp
auth_load_or_derive`; `canary_wap.ino nvs_load_token`). The flashers exploit
that deliberately: at flash time they mint the credential themselves —
`"cv_"` + 32 base62 chars, same unbiased rejection sampling as the
firmware's `format_api_token_string` (reject bytes ≥ 248 = 62 × 4) — and
seed it as a **blob** under both variant key names, `api_token` (PIO canary,
`NVS_KEY_TOKEN`) and `api_tkn` (wap, `NVS_KEY_API_TKN`), in the `securacv`
namespace. The device simply adopts it; an unseeded board keeps deriving its
own exactly as before.

- The desktop app keeps the token in the secret drawer under
  `canary:<mac>:token` — this is what makes the fleet book able to talk to
  the board with no pairing dance.
- The browser flasher seeds the same keys and shows the token **once** on
  its done card (the Nursery roster stays public-facts-only by design).
- The shape and key set are drift-gated in
  `canary-local/tests/desktop_parity.test.js`; the NVS layout round-trips in
  `canary-local/tests/flash.test.js` and `provisioning.rs`'s unit tests.

## The fleet book — every board you flashed, live

Every successful flash records the board (MAC-keyed, enriched by the serial
boot receipt with `device_id` / `pubkey_fp` / firmware, and by the Hatchery
certificate with its name). The Fleet tab renders the book above the
Witness Wall and, while open, browses `_securacv._tcp` for real (`fleet.rs`,
`mdns-sd`) — the TXT schema every networked Canary advertises (device_id,
name, fw, model, dt, role; `docs/onboarding_unified_wizard.md`) gives
presence *and* version without opening a single connection, and reaches the
HTTP-less variants (vision/sense advertise port 1 and are status-only here —
like every networked flavor they run the same signed daily HTTPS pull
themselves; MQTT is only their Home Assistant trigger/status surface). Devices on the network that aren't in the book show
as one-click "Add to book" rows; the hub gets a row of its own probed via
`hub_probe_hub`.

## Over-the-air updates — the app only rings the bell

When a book row's firmware is behind the app's pinned train, "Update over
the air" runs the device's **own** signed pull pipeline
(`docs/firmware_ota.md`): `POST /api/ota/check`, then `POST
/api/ota/install`, then narrate `GET /api/ota/status` (state → progress →
reboot) until the row reads "up to date". The displays expose the same
`/api/ota/*` contract as the flagship canary (since 2026-08), so their rows
get the same one-click update. The device downloads the manifest
and image itself, verifies Ed25519 against its pinned release key plus
SHA-256 and the anti-rollback floor, and A/B-swaps with automatic rollback.
The app never serves firmware bytes.

Security posture of the bridge (`fleet.rs`, unit-tested):

- device calls are a **closed verb set** (`ota-status` / `ota-check` /
  `ota-install` / `status` / `identify`) — never a path or URL proxy;
- targets must be private/local hosts (`.local`/`.lan`/`.internal`/
  `.home.arpa`, single-label names, RFC1918/loopback/link-local literals) —
  mirroring the firmware's own local-transport policy;
- a 401 is surfaced as "the board refused the stored key" (reflashed
  elsewhere, or an all-rounder Canary reset with its 10 s BOOT hold, which
  removes a seeded token and derives one from the key on the next boot →
  reflash here mints a fresh one), never retried blindly;
- **the key never travels in the clear.** Before a token goes to a
  `(device_id, address)` pair the app agrees a session key with the device:
  `GET /api/device/seal?nonce=&client_pub=` carries a fresh X25519 public
  key, the device answers with its own ephemeral key and an Ed25519
  signature over the whole transcript (`securacv-canary-sig|v1|seal|<device_id>|<nonce>|<client_pub>|<device_pub>`),
  the app checks that signature against the fingerprint the book recorded
  at flash time, and both ends derive `HKDF-SHA256(salt = SHA-256(transcript),
  ikm = X25519 shared secret, info = "securacv:api-seal:v1")`. Every
  authenticated call then carries `Authorization: Sealed <sid>.<seq>.<hex>`:
  the token under ChaCha20-Poly1305 with that key, bound to the method, the
  path and a strictly increasing counter (`desktop/src-tauri/src/seal.rs`;
  the firmware's `api_seal.h`, opened by `api_auth.h`'s gates under the
  same per-address lockout as a plain bearer). The three suites —
  `tests_host/test_api_seal.cpp`, `seal.rs`'s tests and
  `desktop_parity.test.js` — pin the same reference vectors, computed by a
  third implementation. There is **no plain-bearer fallback**: a board whose
  firmware predates the handshake is told to update once over USB (the same
  line as for firmware before one-click updates), because a fallback is
  exactly the downgrade a relay would ask for.

## Honest limits

- Vision/sense rows are presence + version only (no always-on HTTP API
  on-device — their shared setup portal serves HTTP only while the board is
  provisioning or recovering; updates arrive by their own daily signed pull,
  or on demand from Home Assistant).
- A board flashed by something other than these flashers has no stored
  token; its row says exactly that and what one reflash fixes.
- `POST /api/ota/config` is deliberately **not** exposed from the app — the
  manifest URL and auto-update policy stay the device owner's settings. (The
  flash-time "Keep this Canary updated automatically" toggle is the one place
  the flashers set that policy, over USB with the owner's hands on the board —
  never over the network.)
- **mDNS is unauthenticated by nature, and the fleet book no longer leans
  on it.** A hostile device already admitted to your network can announce a
  known Canary's `device_id` at its own address; nothing a client can check
  changes that (the TXT record, hostname and IP are all attacker-chosen),
  and the signed challenge `GET /api/device/enroll?nonce=` answers does not
  settle it either — a peer that can reach the real board relays the nonce
  and hands the signature back as its own. So the book sends nothing such a
  host can use: the key goes only sealed to the identity recorded at flash
  time (the bullet above). What a host at a spoofed address gets is a
  ciphertext; what a relay gets is one request it can forward but not read,
  replay or point at another route.

  What the seal does **not** do, so nobody over-reads it: the request body
  and the answer are not sealed (a relay can read a status answer, or
  change identify's blink duration — the bodies on this verb set carry
  nothing worth more), and the device still accepts a plain `Bearer`
  header from every other client, so the wap's pairing-QR flow, the phone
  app on a plain-http device and Home Assistant still hand the token to
  whatever answers on the LAN. For those clients "LAN admission is the
  boundary" stands as written; sealing their calls is the same protocol
  applied to each of them, and is open. What a stolen token yields is
  bounded as before: Bearer-gated reads and a signed-only, anti-rollback
  update trigger — never firmware the pinned release key didn't sign, and
  never a raw frame.

  Why this and not https: the firmware advertises a `tls_cert_fp` and the
  iPhone app pins it, but no stock build stores a certificate (sweep F15),
  so there is nothing to pin on a fielded board; a browser-flashed board
  could not pin a self-signed certificate at all; and a device that fell
  back to plain HTTP (no certificate, a core without `esp_https_server`)
  would have reopened the relay path exactly where it matters. Sealing to
  the identity key the book already holds works on every build over the
  transport every board has.
