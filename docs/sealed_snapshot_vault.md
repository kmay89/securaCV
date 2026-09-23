# Sealed Alarm Snapshots (canary-wap) — design & operations

Opt-in, event-triggered camera frames on the canary-wap device, encrypted so
the device **cannot read back what it wrote**. On a life-safety acoustic
trigger (T3 smoke / T4 CO / glass break — each individually opted in, ALL OFF
by default) the device captures one JPEG and seals it to the SD card against
an operator-held X25519 key. The private key never touches the device; review
happens off-device — with `tools/unseal_snapshot.py` on a computer, or in the
iPhone app's Keys tab, which makes the key on the phone and opens the frame
there (see [Operator workflow](#operator-workflow)).

This is the device-side analog of the witness kernel's break-glass evidence
vault (`src/bin/witnessd.rs:447-461`: `seal_latest_frame` — local, encrypted,
token-gated, off by default). The canary version keeps the same posture:
local-only storage, write-only escrow, nothing captured unless the operator
armed it, and only the *existence* of a sealed frame enters the witness chain.

## ⚠️ Threat-model tension — maintainer sign-off required

`docs/security/THREAT_MODEL.md:134` states:

> | Camera | Preview only (evidence is metadata, not video) |

This feature adds an **opt-in exception**: single alarm-triggered frames,
sealed on-device. The constitutional documents (`spec/invariants.md`,
`SECURITY_MODEL.md`, `THREAT_MODEL.md`, `spec/threat_model.md`) are
**deliberately not edited** by the PR that introduces this feature — changing
them is the maintainer's call, not an implementation detail. The PR requires
an explicit maintainer sign-off acknowledging this tension; if the sign-off is
withheld, the feature flag (`FEATURE_VAULT_SNAPSHOT`, `build_config.h`) can be
set to 0 and the entire subsystem compiles out.

Mitigations that keep the spirit of "preview only":

- **Everything defaults off.** No key registered → nothing is ever captured,
  not even the explicit dashboard Test capture.
- **Write-only escrow by construction.** The device stores only the
  operator's X25519 *public* key. Sealing derives a fresh key per snapshot
  via ephemeral ECDH; the device zeroizes the ephemeral secret, the shared
  secret, the symmetric key, and the plaintext staging buffer after sealing.
  There is no code path that decrypts a `.svlt` file on-device.
- **The chokepoint never sees image bytes.** The `media.vault` csi_event
  module declares exactly one event (`frame_sealed`) whose field allow-list
  is note (`"<trigger tag> <ciphertext SHA-256 prefix>"`) + time_bucket.
  The allow-list is structural: there is no field that could carry image
  data. The event is ANOMALY-category and stateless on purpose: it bypasses
  the quiet-hours hold (matching the acoustic module's classification of
  the very triggers that cause a seal — a night-time alarm is exactly when
  the hash matters) and bypasses the bundler (same-state bundling would
  fold a second seal into the first row and silently drop its hash).
- **Local only.** Files stay in `/VAULT` on the SD card, bounded to the
  newest 20 by the same tested `datamgmt::rotate_dir` used for `/EXPORT`.
  Nothing is pushed anywhere; download is an authenticated, operator-initiated
  HTTP GET.

## Crypto construction

Primitives are the ones already linked and used by `mesh_network.cpp`
(rweather Crypto's `Curve25519`/`ChaChaPoly` + mbedtls HKDF/SHA-256):

```
ephemeral e   ← esp_fill_random, X25519-clamped
E             = X25519(e, basepoint)
ss            = X25519(e, operator_pub)        # all-zero output rejected
key           = HKDF-SHA256(salt = E ‖ operator_pub,
                            ikm  = ss,
                            info = "securacv/vault/seal/v1", 32 bytes)
file          = header(64) ‖ ChaCha20-Poly1305(key, nonce,
                                               aad = header, jpeg) ‖ tag(16)
```

The 64-byte header is the AEAD associated data, so trigger, time bucket,
recipient key id, ephemeral key, nonce and length are all tamper-evident —
editing any of them fails the tag on unseal. The python side
(`tools/unseal_snapshot.py`) implements the inverse with the `cryptography`
library.

### `.svlt` header layout (little-endian, byte-exact)

| offset | size | field |
|---:|---:|---|
| 0 | 4 | magic `"SVLT"` |
| 4 | 1 | format version (1) |
| 5 | 1 | trigger (1 = T3 smoke, 2 = T4 CO, 3 = glass, 9 = test) |
| 6 | 1 | time bucket 0–143 (10-minute bucket — the **only** time info stored, matching the chokepoint's coarsening) |
| 7 | 1 | reserved (0) |
| 8 | 8 | recipient key id = first 8 B of SHA-256(operator pubkey) |
| 16 | 32 | ephemeral X25519 public key |
| 48 | 12 | ChaCha20-Poly1305 nonce |
| 60 | 4 | ciphertext length u32 (excludes the 16 B tag) |

Files are named `seal_<seq8>_<trigger>.svlt` (`vault_logic.h` builds and
parses both the header and the name; the name parser doubles as the
download/delete traversal gate). Writes go to a temp file and rename on
completion, so a power cut leaves either no file or a complete one.

## Fail-closed decision table

`vault_logic::capture_decision` — order is part of the contract (hard
preconditions before opt-in/cooldown, so a Test capture exercises the real
path):

| condition | decision |
|---|---|
| unknown trigger byte | `SKIP_BAD_TRIGGER` |
| no operator key registered | `SKIP_NO_KEY` (Test included) |
| SD unavailable | `SKIP_NO_SD` |
| camera not initialized | `SKIP_NO_CAMERA` |
| QR provisioning scan active (owns the sensor config) | `SKIP_QR_BUSY` |
| a seal already in flight (single worker) | `SKIP_WORKER_BUSY` |
| trigger == TEST | `CAPTURE` (bypasses only opt-in + cooldown) |
| trigger not opted in | `SKIP_DISABLED` (the default state) |
| within per-trigger cooldown (wrap-safe, default 60 s) | `SKIP_COOLDOWN` |
| otherwise | `CAPTURE` |

Every refusal except `SKIP_DISABLED` health-logs its reason. A raw frame is
never staged unless the decision is `CAPTURE`. Clearing the key also forces
every trigger off (a vault without a recipient must not stay armed), and the
config setter re-enforces it server-side.

## Threading (house rules)

- The audio event callback fires synchronously from `audio_process()` on the
  main loop, so `request_capture()` runs there: decision inline, cooldown
  stamped at request time, one-shot worker task (`vault_seal`, 8 KB internal
  stack, priority 1) spawned for the blocking capture + seal.
- The worker never touches the csi_event chokepoint. It writes a result
  struct and release-stores a done flag; `poll_completion()` in `loop()`
  adopts it — emits the `frame_sealed` witness event, health-logs, rotates
  `/VAULT`. Same worker/adopter pattern as the SD mount, MJPEG stream, fleet
  browse and BLE bring-up workers.
- The JPEG is copied to a PSRAM staging buffer and the camera framebuffer is
  returned immediately, so internal RAM and the camera queue are held for
  microseconds, not for the SD write.
- `POST /api/vault/test` runs on the HTTP task, so it only latches a pending
  flag; the loop drains it and runs the real (loop-only) request.

## Operator workflow

```
# once, on your own computer (pip install cryptography):
tools/unseal_snapshot.py gen-key --out vault_key
#   -> vault_key      (private; keep offline)
#   -> vault_key.pub  (public; paste the printed 64-hex key into the
#                      dashboard's Camera panel → Sealed Alarm Snapshots)

# after an alarm (or dashboard Test capture):
#   download seal_00000001_smoke.svlt from the dashboard
tools/unseal_snapshot.py inspect sealed.svlt      # header + ct hash; the
                                                  # 16-hex prefix should match
                                                  # the frame_sealed event note
tools/unseal_snapshot.py unseal sealed.svlt --key vault_key   # -> .jpg
```

On an iPhone (the SecuraCV app, Keys → Unseal a snapshot), the same loop
without a computer:

- **Create key** makes the X25519 key on the phone and keeps it in the
  Keychain, this device only (`ThisDeviceOnly` — it never rides iCloud
  Keychain), wrapped through the Secure Enclave where the phone has one and
  a passcode is set: opening a snapshot then asks for Face ID, Touch ID or
  the passcode. (The Enclave holds only P-256 keys, so it wraps the X25519
  key rather than holding it; the key is in app memory for the length of an
  unseal. Without an Enclave the same envelope uses a software key, and the
  Keys tab says so.) The private half has no export path; forgetting it is a
  destructive, confirmed step, because every frame sealed to it is then
  unreadable for good — and removing the phone's passcode disables an
  Enclave-wrapped key the same way.
- **Register this phone's key** sends only the public half to a paired
  canary-wap (`POST /api/vault/key`), and the screen shows, per Canary,
  whether the key it holds is this phone's (the 16-hex key id is compared).
  Arming the triggers stays on the Canary's own dashboard.
- **Unseal** pulls a listed file (`GET /api/vault/download`) and opens it on
  the phone; a `.svlt` from Files, Mail or AirDrop opens the same way
  (the app declares the `com.securacv.svlt` document type). The frame is
  shown once, full screen, with its trigger and ten-minute bucket, and is
  discarded on Done or when the app leaves the foreground — never written
  to disk, Photos, the pasteboard or iCloud, and there is no share button.

A sealed snapshot is **not** the kernel's break-glass evidence vault: that
opens only by an n-of-m trustee quorum (Invariant V), and the phone has no
path into it. A sealed snapshot is single-recipient by construction — one
frame, sealed to one key — so the key's holder opens it alone.

HTTP surface (all auth-gated; download also accepts `?token=` for browser
downloads, like the peek stream): `GET /api/vault/status`,
`POST /api/vault/config`, `POST`/`DELETE /api/vault/key`,
`GET /api/vault/list`, `GET /api/vault/download?name=`,
`DELETE /api/vault/item?name=`, `POST /api/vault/test`.

## Testing

- `tests_host/test_vault_logic.cpp` (CI: firmware.yml) — full decision
  matrix incl. millis wrap and the "no key → not even Test" invariant,
  malformed-header rejects, filename build/parse, and a **golden 64-byte
  header hex fixture**.
- `tools/test_unseal_snapshot.py` (CI: firmware.yml) — python
  seal→unseal round-trip, wrong-key / tampered-ciphertext / tampered-tag /
  tampered-header(AAD, including the reserved byte the parser never checks) /
  truncation negatives, and the **same golden header fixture verbatim**,
  pinning the byte layout across both languages.
- `tools/fixtures/vault/svlt_parity.json` (generated by
  `tools/gen_svlt_parity.py` from the python reference and fixed,
  test-only keys; `--check` in firmware.yml) — one whole sealed file, byte
  for byte. The python test asserts it; the iPhone app's
  `SnapshotVaultTests` (CI: ios-selfheal.yml) opens it with CryptoKit and
  re-seals it to the identical bytes, and holds the golden header as the
  third language.

**Honest residual gap:** CI does not bit-compare the firmware's ChaChaPoly
output against python — the rweather Crypto library is not host-linkable in
this repo's CI. (Python and the iPhone's CryptoKit port ARE bit-compared, by
the parity fixture above.) Both sides implement standard RFC 7748/5869/8439 primitives,
the shared golden fixture pins the container format, and the hardware
verification step (seal on-device → unseal off-device) closes the loop
end-to-end. If the firmware's crypto ever diverged, every real unseal would
fail loudly at the tag check — it cannot silently "succeed wrong".

## Storage & wear bounds

- Ring of 20 files (`vault_logic::KEEP_FILES`), rotated only after a new
  seal lands (oldest is the casualty, never the newest evidence).
- Per-trigger cooldown (10–3600 s, default 60) bounds writes while an alarm
  cadence re-fires continuously.
- Single-ciphertext cap 512 KB (an XGA JPEG is ~100–300 KB); oversized frames
  are refused before any write.
- `/VAULT` is never touched by log rotation or the export sweep — it joins
  `/WITNESS` and `/CHAIN` under Invariant IV's "sealed evidence is not
  regenerable" rule; only its own ring bound deletes files, plus explicit
  operator deletes.
