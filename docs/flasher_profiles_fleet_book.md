# Setup profiles, the secret drawer, and the fleet book

The desktop Flasher's answer to three adjacent frustrations: retyping the
same Wi-Fi and broker settings on every board (and fat-fingering one of
them), losing track of which Canaries exist and what they run, and having no
way to update a deployed board without a USB cable. Shipped in Flasher 0.4.0;
the moving parts live in `desktop/src/app.js` (`secretStore`, `fleetBook`),
`desktop/src-tauri/src/secret_store.rs`, `desktop/src-tauri/src/fleet.rs`,
and `desktop/src-tauri/src/provisioning.rs`.

## The setup profile — type it once

The Canary form and the hub form share one memory:

- **Non-secrets** (device name, Wi-Fi name, broker host/port/user, hub board
  choice) persist live in the app's local prefs, as they already did.
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
HTTP-less variants (vision/sense advertise port 1 and are status-only here;
their OTA rides MQTT). Devices on the network that aren't in the book show
as one-click "Add to book" rows; the hub gets a row of its own probed via
`hub_probe_hub`.

## Over-the-air updates — the app only rings the bell

When a book row's firmware is behind the app's pinned train, "Update over
the air" runs the device's **own** signed pull pipeline
(`docs/firmware_ota.md`): `POST /api/ota/check`, then `POST
/api/ota/install`, then narrate `GET /api/ota/status` (state → progress →
reboot) until the row reads "up to date". The device downloads the manifest
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
  elsewhere → reflash here mints a fresh one), never retried blindly.

## Honest limits

- Vision/sense rows are presence + version only (no HTTP server on-device).
- A board flashed by something other than these flashers has no stored
  token; its row says exactly that and what one reflash fixes.
- `POST /api/ota/config` is deliberately **not** exposed from the app — the
  manifest URL and auto-update policy stay the device owner's settings.
- **mDNS is unauthenticated by nature.** A hostile device already admitted
  to your network can announce a known Canary's `device_id` and, when you
  click Identify or Update, receive that board's bearer token at its own
  address. Nothing the client can check today changes that: the TXT record,
  hostname, and IP are all attacker-chosen, and pinning any of them (or a
  TXT-advertised fingerprint) would authenticate nothing. This is the local
  API's existing trust model — the wap's pairing-QR flow hands the same
  `{base_url, token}` to any client on the LAN, and the token travels in
  cleartext HTTP headers to the real board anyway — so the fleet book
  inherits, rather than widens, "LAN admission is the boundary." What a
  stolen token yields is bounded: Bearer-gated reads (status, diagnostics,
  chain heads) and a signed-only, anti-rollback update trigger — never
  firmware the pinned release key didn't sign, and never a raw frame. The
  structural fix is firmware-side authenticated discovery — an
  unauthenticated challenge endpoint the device answers by signing a nonce
  with its Ed25519 identity key, verified against the `pubkey_fp` the book
  already stores from the serial receipt — and belongs in a firmware PR,
  tracked as follow-up.
