# Canary Firmware Updates (Signed Pull-OTA)

How a Canary that's screwed to a ceiling gets new firmware without anyone
touching it — and why it can't be bricked or fed a forged image along the way.

**Covered variants:** all of them —

- **canary** (PIO, `firmware/canary/`)
- **canary-wap** (`firmware/projects/canary-wap/` — one sketch built by both
  arduino-cli and PlatformIO, so both toolchains ship the same OTA)
- **canary-vision** (PIO, `firmware/projects/canary-vision/` — the Grove
  Vision AI V2 host; `default`, `xiao-c3` and `xiao-s3` are distinct OTA
  products so a board can never install another board's image. Note: only
  the ESP32 host updates over the air — the Grove Vision AI V2 module's own
  firmware/model loads once over its USB-C port via SenseCraft and is not
  host-flashable.)
- **canary-sense** (PIO, `firmware/projects/canary-sense/` — the MR60BHA2
  mmWave breathing/heartbeat device on XIAO ESP32-C6; `default` and
  `wellbeing` are distinct OTA products)

Every variant uses the same engine, manifest schema, signature scheme,
release key, MQTT topic layout, and HA update-entity UX. Future variants
follow the checklist at the end of this document.

## The user experience

1. A firmware release is published (GitHub Release on a `fw-v*` tag).
2. Every Canary checks for updates once a day (jittered) — a single HTTPS
   GET of a small JSON manifest, no telemetry, no identifiers.
3. Home Assistant shows the device's **Firmware** update entity:
   "Update available — 2.2.0", release notes, and an **Install** button.
   The device's own dashboard (Settings → Software Update) shows the same.
4. The user presses Install (or has the **Auto Update** switch on, in which
   case the daily check installs by itself).
5. The device downloads the image to its **inactive** A/B partition with a
   live progress bar, verifies the SHA-256, verifies the Ed25519 release
   signature, reboots into the new firmware, runs its boot self-tests, and
   only then marks the image good.
6. If anything fails — power loss mid-download, a corrupt image, a
   self-test failure on first boot — the device automatically returns to
   the previous firmware and tells the user why in plain language.

Every step is signed into the witness chain (`fw_update_started`,
`fw_update_applied`, `fw_update_rolled_back`), so the audit trail proves
when the firmware changed.

## Architecture

```
GitHub Release (fw-v2.2.0)                 ┌─ Home Assistant ─────────────┐
  canary-2.2.0.bin            MQTT         │  update.canary_xxx_firmware  │
  manifest-canary.json   ┌────discovery───▶│  "Install" → update/cmd      │
  manifest-canary-wap.…  │                 └──────────────────────────────┘
  manifest-index.json    │
  sha256sums.txt         │
        ▲                │
        │ HTTPS GET      │
        │ (daily,        │
        │  jittered)     │
┌───────┴────────────────┴───────────────────────────────────────────────┐
│ Canary device — securacv_ota engine (firmware/common/ota/)             │
│  manifest check → version + anti-rollback floor → can_install gate     │
│  → download to inactive slot → SHA-256 → Ed25519 → reboot → self-test  │
│  → mark valid  (any failure → automatic rollback)                      │
└─────────────────────────────────────────────────────────────────────────┘
```

The engine is canonical at `firmware/common/ota/` and consumed by:

- **canary (PIO):** `-I` + `build_src_filter` in `firmware/canary/platformio.ini`
  (gated by `FEATURE_OTA_PULL`).
- **canary-wap (both toolchains):** committed copies next to the sketch, kept
  identical by `firmware/scripts/check_ota_sync.sh` (CI-enforced). The
  PlatformIO env builds the same sketch directory as arduino-cli, so one
  codebase serves both.
- **canary-vision (PIO):** as a PlatformIO library via `lib_extra_dirs =
  ../../common` (glue in `src/net/ota_mgr.cpp`).
- **canary-sense (PIO):** via `build_src_filter` (the pioarduino C6 core
  forces chain-mode LDF, so `lib_extra_dirs` alone doesn't link the
  engine); same `src/net/ota_mgr.cpp` glue as canary-vision.

### Unified versioning

One `fw-v*` tag versions **every** variant: `fw-v2.2.0` publishes canary
`2.2.0`, canary-wap `2.2.0-wap`, and canary-vision `2.2.0`. Each variant's
compiled version string must be bumped before tagging (the workflow fails
loudly if a binary is missing the tagged version). One tag, one changelog,
one release page — nothing to cross-reference.

### canary-vision: generic release builds + NVS identity

Vision historically compiled its WiFi/MQTT credentials and device id into
the binary — which would make an OTA-installed generic image forget the
unit's setup. `canary/runtime_config` fixes this:

- **Identity is sticky:** `device_id` lives in NVS; the compiled value only
  seeds the very first boot. HA entities survive OTA updates and reflashes.
- **Credentials follow the most recent real configuration:** a user-compiled
  USB flash (real `secrets/secrets.h`) wins and is persisted to NVS; the
  placeholder secrets baked into generic release builds defer to NVS.

So vision provisioning is: first flash over USB with your own secrets
(seeds NVS) → every OTA release afterwards inherits the unit's identity and
credentials.

## Manifest schema (v1)

One flat JSON per variant, published as a GitHub Release asset and reachable
at a stable URL (`releases/latest/download/manifest-<variant>.json`):

```json
{
  "manifest_version": 1,
  "product": "securacv-canary",
  "version": "2.2.0",
  "min_version": "2.1.0",
  "url": "https://github.com/.../releases/download/fw-v2.2.0/canary-2.2.0.bin",
  "sha256": "<64 hex>",
  "size": 1048576,
  "signature": "<128 hex>",
  "manifest_signature": "<128 hex>",
  "ble_signature": "<128 hex>",
  "signing_key_id": "<16 hex>",
  "release_notes": "Improved detection accuracy",
  "release_url": "https://github.com/.../releases/tag/fw-v2.2.0"
}
```

`ble_signature` (added with BLE OTA protocol v2) is the same key's
signature over the BLE header's canonical message — see [Signature
scheme](#signature-scheme). The pull engine ignores it. It is present only
for products whose id and version fit the header's 31-byte slots
(`ota_release.ble_header_fits()`); the longer Canary Display ids have no
Bluetooth OTA path and carry none, and `ota_release.py verify` asks them for
none. For a product that fits, manifests from earlier releases lack it; a
BLE client then has only the legacy v1 header, which a current device
refuses unless break-glass is armed, and `verify` reports such a manifest as
incomplete.

`manifest-index.json` maps products to their manifest URLs for tooling and
future variants. Devices fetch only their own flat manifest.

`firmware/scripts/ota_release.py` is the single source of truth for this
format (`keygen` / `sign` / `manifest` / `index` / `verify` /
`ble-header`), reused by the release workflow and the local mock server.

## Signature scheme

```
message   = image_size as uint32 little-endian (4 bytes)
          || sha256(firmware.bin)              (32 bytes)
signature = Ed25519.sign(message)              (64 bytes)
```

This is the pull path's image signature (and what the legacy BLE OTA v1
header carried). The device verifies against the public key compiled into
`firmware/common/ota/src/ota_release_key.h` — and critically, it verifies
over the digest it computed from the **actual flash contents**, not the
manifest's claim. **One release key signs every update channel**: the BLE
OTA v2 header below is signed by the same key, over a message built with
the same convention as the manifest signature.

**The manifest itself is signed too** (`manifest_signature`, same key) over
a canonical NUL-separated field string:

```
"scv-manifest-v1\0" product "\0" version "\0" min_version "\0" url "\0"
sha256hex "\0" size-as-decimal "\0" release_notes "\0" release_url "\0"
```

The device rejects any manifest whose metadata doesn't verify — so a
hostile mirror cannot forge the release notes users read in Home
Assistant, the version offered, or the download URL. The byte layout is
pinned by a cross-language fixture (`test_ota_release.py` ⇄
`test_ota_logic.cpp`): if the Python signer and the C verifier ever
drift, both test suites fail.

### BLE OTA protocol v2

The WAP's GATT push path (`ble_ota.cpp` under
`firmware/projects/canary-wap/arduino/canary_wap/`, policy in
`ble_ota_policy.h` beside it) opens a session with a `BEGIN_V2` write:
command byte `0x03` followed by a 168-byte header —

| offset | size | field |
|---|---|---|
| 0 | 2 | magic `"SC"` (`0x53 0x43`) |
| 2 | 1 | header version `0x02` |
| 3 | 1 | reserved, must be `0x00` |
| 4 | 32 | product id, NUL-terminated, zero-padded |
| 36 | 32 | semantic version, NUL-terminated, zero-padded |
| 68 | 4 | image size, uint32 little-endian |
| 72 | 32 | SHA-256 of the image |
| 104 | 64 | Ed25519 signature over the canonical message |

The signature covers a domain-separated canonical message with the
manifest signature's NUL-separated convention, which the device rebuilds
from the **parsed** fields (never the raw wire bytes):

```
"scv-ble-ota-v2\0" product "\0" version "\0" size-as-decimal "\0"
sha256hex-lowercase "\0"
```

`ota_release.py` emits that signature into every manifest as
`ble_signature`; the companion PWA (and `ota_release.py ble-header`) builds
the header from the manifest's product / version / size / sha256 plus that
field. The device then, before it accepts a single image byte:

1. verifies the signature against the release key (an all-zero key refuses,
   as everywhere else);
2. requires `product` to equal the running product (never bypassed — a
   signed image for another product is a brick, not a rescue);
3. runs `version` through the pull engine's own decision,
   `securacv_ota_update_decision()`, against `max(running version,
   NVS floor)`. At or above the floor is accepted (re-flashing the running
   version is a repair and cannot move the floor); below it is a rollback.

Field hygiene is strict: 1–31 printable ASCII bytes, no spaces, zero
padding after the NUL, and a numeric `MAJOR.MINOR` prefix on the version,
so an unparseable string can never rank "equal to the floor". Anything else
is `header malformed`. The layout is pinned by `static_assert` and by a
cross-language fixture (`test_ota_release.py` ⇄
`tests_host/test_ble_ota_policy.cpp`).

**Break-glass (the rescue case).** A legacy v1 header (`BEGIN`, `0x01`, the
132-byte header signed over `size || sha256` only, no product, unsigned
version) or a v2 header below the floor is refused by default with a
distinct reason (`v1 header refused: protocol v2 required (not armed)` /
`version below anti-rollback floor (not armed)`). It is accepted only when
the owner has **armed break-glass through the existing physical-presence
surface**: a short tap of the BOOT button opens the same single-use, 30 s
provisioning gate that reveals the provisioning receipt, and the one BEGIN
it admits closes it. Every such acceptance writes a health-log line naming
what was bypassed and against which floor (`OTA break-glass: anti-rollback
floor bypassed`), and `/api/bluetooth/ota` reports `break_glass: true` for
that session. A bad signature or a product mismatch is never bypassed — the
gate relaxes the floor, not the key.

The floor is raised the same way for both channels: the install records
`securacv_ota_mark_pending_install()`, and the new image raises the floor
to its own compiled version only once its boot self-test confirms it.
Raising it at install time would, after a rollback, block ever re-offering
that version.

An **all-zero public key fail-closes**: both pull-OTA installs and BLE OTA
refuse every image until a real key is provisioned.

### Key ceremony

```bash
# On an offline machine:
python firmware/scripts/ota_release.py keygen --private-key releaser.pem

# Embed the public half in firmware (commit this):
python firmware/scripts/ota_release.py pubkey-header \
  --private-key releaser.pem --out firmware/common/ota/src/ota_release_key.h
cp firmware/common/ota/src/ota_release_key.h \
   firmware/projects/canary-wap/arduino/canary_wap/ota_release_key.h

# Store the private key as the OTA_SIGNING_KEY_PEM GitHub Actions secret.
# Keep the PEM itself offline — treat it like a code-signing certificate.
```

The release workflow refuses to publish if the secret's public half doesn't
match the committed header. Rotate by shipping a release (signed with the
old key) whose firmware carries the new public key.

## Releasing firmware

1. Bump every variant's version to the release number (unified versioning):
   - `FIRMWARE_VERSION` in `firmware/canary/include/canary_config.h` (`2.2.0`)
   - `FIRMWARE_VERSION` in `canary_wap.ino` (`2.2.0-wap` — the `-wap` suffix
     is part of the string and must match what the workflow writes into the
     WAP manifest)
   - `CANARY_FW_VERSION` in
     `firmware/projects/canary-vision/include/canary/version.h` (`2.2.0`)
2. Add a `## [2.2.0]` section to `CHANGELOG.md` — it becomes the release
   notes users read in Home Assistant.
3. Tag and push: `git tag fw-v2.2.0 && git push origin fw-v2.2.0`.
4. `.github/workflows/firmware-release.yml` builds every variant, signs
   them, verifies every signature against the committed public key, checks
   the binaries actually contain the new version string, and publishes the
   GitHub Release.

## Anti-rollback

The engine persists a **monotonic minimum-version floor** in NVS. A validly
signed but older image — e.g. replayed by a hostile mirror — is rejected
even after a physical downgrade, because the floor only ever rises. The
decision logic is host-tested (`firmware/common/ota/test_ota_logic.cpp`).

BLE OTA enforces the **same floor through the same function** since
protocol v2 (`securacv_ota_update_decision()` against `max(running, NVS
floor)`, read through `securacv_ota_get_min_version()`), plus the product
binding the pull path already had. The one exception is the owner-armed
break-glass rescue described under [BLE OTA protocol
v2](#ble-ota-protocol-v2): a below-floor image or a legacy v1 header needs
a BOOT-button tap within 30 s, is admitted once, and is logged as a bypass.

## Transport policy & local hosting

- **Public URLs require HTTPS** (certificate bundle; GitHub works out of
  the box).
- **`http://` is allowed only** when the user enables the local-update-server
  option (Settings, NVS-persisted) **and** the host is private:
  10/8, 172.16/12, 192.168/16, 127/8, 169.254/16, `localhost`, or a name
  under `.local` / `.lan` / `.internal` / `.home.arpa`.

Image integrity never depends on the transport — the Ed25519 signature,
the manifest signature, and the anti-rollback floor do that work — so
air-gapped installs don't need a certificate authority. With manifests
signed, the worst a hostile mirror can do is **withhold updates** (serve a
stale-but-genuine manifest or nothing) — indistinguishable from having no
network, and recoverable the moment the device reaches a honest server.
BLE OTA follows the same rules since protocol v2 — product and version are
inside the signed header, the product must match, and the version must be
at or above the floor. The one deliberate exception is the **break-glass
rescue** (a legacy v1 header or a below-floor image, admitted only after a
BOOT-button tap and logged as a bypass); see [BLE OTA protocol
v2](#ble-ota-protocol-v2). A hostile party holding a manifest can still only
withhold: stripping `ble_signature` leaves a BLE client with the v1 header,
which the device refuses without the owner's hands on the button.

### Hosting updates locally (air-gapped / privacy-strict)

```bash
cd firmware/projects/canary-ota/tools
python mock_ota_server.py generate canary-2.2.0.bin 2.2.0 \
  --product securacv-canary -o manifest-canary.json
python mock_ota_server.py serve            # https://<host>:8443
```

Point the device at it: Settings → Software Update → Advanced → update
server address (or `POST /api/ota/config {"manifest_url": "..."}`). For a
plain-HTTP LAN server, also enable the local-server option. The mock server
auto-creates a development signing key on first use and prints the command
that embeds its public half into a dev build.

## Device API

All endpoints are bearer-token gated on canary and canary-wap. Since 2026-08
canary-display serves `status`/`check`/`install` under the same contract —
same routes, statuses, and body shapes, so the desktop Flasher's one-click
update works on displays too — but LAN-open like the rest of the glass web
mirror (the glass mints no bearer credential), and without `/api/ota/config`.
canary-vision and canary-sense have no always-on local HTTP server — they are
HA/MQTT-managed (their setup portal serves HTTP only while provisioning or
recovering):

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/api/ota/status` | versions, state + plain-language text, progress, error, settings |
| POST | `/api/ota/check` | fetch manifest, compare versions (no install) |
| POST | `/api/ota/install` | full signed install pipeline |
| POST | `/api/ota/config` | `{manifest_url?, auto_update?, local_http_allowed?}` |

MQTT (Home Assistant discovers these automatically):

| Topic | Direction | Payload |
|---|---|---|
| `securacv/<id>/update/state` | device → HA | update-entity JSON (retained) |
| `securacv/<id>/update/cmd` | HA → device | `install` |
| `securacv/<id>/update/auto` (WAP) / `…/auto/state` (canary) | device → HA | `ON`/`OFF` (retained) |
| `securacv/<id>/update/auto/cmd` | HA → device | `ON`/`OFF` |

The dev-only raw push endpoint (`POST /api/ota`, used by
`firmware/canary/scripts/ota_deploy.py`) is compiled out of release builds.

## Safety properties (the no-brick guarantees)

The design goal is stronger than "hard to brick": **there is no sequence of
user actions through the update system that leaves a device unrecoverable.**

- **A/B partitions:** updates are written only to the inactive slot; the
  running firmware is never touched. Power loss at any point during the
  download or flash lands on a bootable slot.
- **The application owns rollback confirmation.** The engine overrides the
  Arduino core's `verifyRollbackLater()` hook (which would otherwise
  auto-confirm a new image microseconds into its first boot, before
  `setup()` runs — silently disabling rollback). A freshly installed image
  stays `PENDING_VERIFY` until the variant's boot self-test confirms it.
  Consequences:
  - **Crash-loop protection:** if the new firmware crashes, hits a watchdog
    reset, or loses power at ANY point before confirmation, the bootloader
    boots the previous firmware on the very next start. One bad boot,
    automatic recovery — no ladder.
  - **Health-gated commit:** a required self-test failure triggers
    `esp_ota_mark_app_invalid_rollback_and_reboot()` — back to the previous
    firmware, with the outcome recorded.
  - **Integrator contract:** every variant (and every install channel,
    including the dev push endpoint and BLE OTA) must reach
    `securacv_ota_boot_self_test()` on boot, or fresh installs revert on
    their second start. Canary's validate block therefore compiles whenever
    ANY install channel exists; vision validates immediately after WiFi,
    BEFORE its blocking MQTT connect, so a broker outage can't cause a
    spurious revert.
- **Expected (safe) edge:** power-cycling the device during the first
  minute after an update — before it confirms itself — reverts it to the
  previous version. Nothing is lost; the update is simply offered again.
- **Outcome bookkeeping survives every path:** the engine records the
  install target the moment the boot partition flips (not at reboot), so
  deferred reboots, battery-gated installs, and BLE-pushed images all get
  correct applied/rolled-back records.
- **Anti-rollback floor rises only after confirmation**, so a rolled-back
  version can always be retried.
- **Install gating:** the device defers installs while on a low battery
  (canary) or while a BLE OTA session is active (canary-wap); the two OTA
  channels strictly exclude each other.
- **Pre-reboot flush:** witness-chain state is persisted before the install
  reboot, mirroring the manual-reboot path.
- **Never reboots unattended without consent:** the engine's stored
  auto-update flag defaults to off. The flashers ask at flash time — "Keep
  this Canary updated automatically", on by default, untick to keep updates
  manual — and seed the choice into the device's settings; the HA **Auto
  Update** switch (or the dashboard, where there is one) flips it any time.
- **OTA can never touch the bootloader or partition table** — it writes app
  slots only. The ESP32's first-stage bootloader is mask ROM. Even in an
  unforeseeable worst case, a USB flash always recovers the device.

## If something goes wrong (recovery matrix)

| What happened | What the device does | What the user does |
|---|---|---|
| Power/WiFi lost mid-download | Old firmware keeps running; partial download discarded | Nothing — press Install again whenever |
| Update file corrupted or forged | Refused before install (SHA-256 + Ed25519 + format checks) | Nothing — error shown in plain language |
| New firmware crashes or hangs on first boot | Bootloader restores previous firmware on the next start | Nothing — rollback is recorded; update re-offered |
| New firmware boots but fails its health check | Restores previous firmware automatically | Nothing — reason shown in HA / dashboard |
| Power cycled in the first minute after an update | Returns to previous firmware (unconfirmed images don't stick) | Press Install again |
| Wrong update server address saved | Checks fail with a clear message; firmware untouched | Clear the field (Settings) to return to the official server |
| Wrong variant's manifest configured | Product check refuses the image | Fix the address; nothing was installed |
| WiFi password changed at the router | canary/WAP: own AP + dashboard still up — reconfigure there. vision/sense: after a few failed joins for a fixable reason, the board raises its own `SecuraCV-XXXX` setup network (the shared setup portal); sensing continues underneath | Reconnect via dashboard (canary/WAP), or join the setup network from a phone and enter the new password (vision/sense) — a USB reflash always works too |
| Absolute worst case | Device still enters ROM download mode | USB flash — always possible, OTA cannot break it |

## Adding a new variant (the recipe)

Every future firmware flavor gets the same update process with five steps:

1. **Consume the engine** from `firmware/common/ota/`: PlatformIO projects
   add it via `lib_extra_dirs`/`build_src_filter` + the shared
   `ota_release_key.h`; Arduino-IDE-style sketches take committed copies and
   extend `check_ota_sync.sh`'s file list.
2. **Wire the glue** (copy `canary-vision/src/net/ota_mgr.cpp` as the
   template): boot self-test with a required is-it-doing-its-job probe,
   `securacv_ota_init()` with a unique `product` id (`securacv-<variant>`)
   and default manifest URL
   (`releases/latest/download/manifest-<variant>.json`), the daily jittered
   scheduler, and the `ota_target` applied/rolled-back record.
3. **Expose the HA update entity** over the variant's MQTT layer:
   discovery for `update` + auto-update `switch`, retained state on
   `securacv/<id>/update/state`, commands on `…/update/cmd` and
   `…/update/auto/cmd`, reconnect republish.
4. **Add the variant to `.github/workflows/firmware-release.yml`**: build
   step, signed manifest, index entry, signature verification, version-string
   guard, release asset. For PR CI, add the variant's entry — including its
   OTA-slot `size_guards` budget — to `firmware/flavors.json`; the
   `firmware.yml` matrix picks it up with no new jobs (see
   `firmware/ARCHITECTURE.md` § CI Flavor Manifest).
5. **Keep identity out of the binary**: anything per-unit (device id,
   credentials) must live in NVS so generic release images inherit it
   (see canary-vision's `runtime_config` for the pattern).

The signing key, manifest schema, transport policy, and anti-rollback floor
come with the engine — don't reimplement them.

## Testing

- Host: `pytest firmware/scripts/test_ota_release.py`, the
  `test_ota_logic.cpp` host build, and the BLE OTA header policy suite
  (`tests_host/test_ble_ota_policy.cpp`, run by
  `make -C firmware/projects/canary-wap/tests_host`; real Ed25519 via
  OpenSSL — the accept / refuse / break-glass matrix and the canonical bytes
  shared with the Python signer). All run in `firmware.yml`.
- Bench (real device): build with a dev key embedded, serve a
  newer-versioned image from `mock_ota_server.py`
  (`SECURACV_OTA_SKIP_CERT_VERIFY=1` dev builds accept the self-signed
  cert), press Install in HA or the dashboard, watch the progress bar,
  confirm the version flips. Pull power mid-flash to prove rollback; check
  the witness chain for the `fw_update_*` records.

## First-release runbook (turning the key)

Everything above is built and CI-verified, but a device can only install
what has actually been released. Two owner-held steps make the whole
system live — until both are done every firmware **fails gracefully and
honestly**: with the all-zero placeholder key, installs are hard-refused
with "Release public key not provisioned"; with no `fw-v*` release, the
daily check reports "Failed to fetch manifest" and the device keeps
running its current image.

1. **Generate the release keypair** (once, on the release engineer's
   machine — the private key must never enter the repository or any
   device):

   ```
   python firmware/scripts/ota_release.py keygen --private-key releaser.pem
   python firmware/scripts/ota_release.py pubkey-header \
     --private-key releaser.pem --out firmware/common/ota/src/ota_release_key.h
   ```

   Commit the regenerated `ota_release_key.h` (public half only; the
   canary-wap sketch copy must be re-synced — `check_ota_sync.sh` guards
   it), and store the full `releaser.pem` in the `OTA_SIGNING_KEY_PEM`
   GitHub Actions secret. The release workflow refuses to publish if the
   secret's public half doesn't match the committed header.

2. **Cut the first release** once the key commit is on main:

   ```
   git tag fw-v<version> && git push origin fw-v<version>
   ```

   The tag version must match every variant's compiled-in version string
   (the workflow greps each binary and fails otherwise). A first release
   at the currently-flashed version is a safe baseline: devices check,
   verify the signed manifest, and report "up to date". The next version
   bump then exercises a real over-the-air install end to end.

3. **Verify on-device:** each firmware's daily check (or a manual check —
   canary-wap: dashboard/`POST /api/ota/check`; canary-vision and
   canary-sense: the Home Assistant update entity) should flip from
   "Failed to fetch manifest" to a verified answer. After the first
   version bump, press Install and watch the A/B swap + boot self-test
   confirm the new image, with automatic rollback if it doesn't.

### Key rotation (read before rotating — orphaning risk)

The engine trusts a single key, and devices poll one static
`releases/latest/...` URL. A naive rotation orphans stragglers: once a
NEW-key release becomes `latest`, any device still running OLD-key
firmware fails signature verification on every future check, forever.
Rotate in phases:

1. Ship a **transition release** signed with the OLD key whose firmware
   carries the NEW public key. Concretely: regenerate `ota_release_key.h`
   from the NEW key, commit the OLD public key alongside it as
   `firmware/common/ota/src/ota_release_key_previous.h` (same header
   shape), and keep `OTA_SIGNING_KEY_PEM` on the OLD key. The release
   workflow's key guard accepts the previous-key file as the signing
   match during this window (with a loud warning) — without it, an
   old-key-signed release with a new-key header would be refused.
2. **Wait for fleet convergence** — devices check daily; with auto-update
   enabled they converge within days. Verify (HA update entities /
   dashboard) that every device you care about is on the transition
   version before proceeding.
3. Switch `OTA_SIGNING_KEY_PEM` to the new key, **delete
   `ota_release_key_previous.h`** (closing the rotation window — the
   guard returns to strict single-key), and release normally.

A device that slept through the window is NOT bricked, but it can no
longer follow `latest`. Recovery without re-tethering: point it at the
transition release's **versioned** URL via the manifest-URL override
(`releases/download/fw-v<transition>/manifest-<variant>.json` — canary-wap:
`POST /api/ota/config`; all variants: the NVS override) — it installs the
OLD-key-signed transition image, learns the new key, then switch the URL
back to `latest`. Engine-level co-signing (a manifest carrying signatures
from both keys) would remove the convergence window entirely and is the
designated future hardening if rotation becomes routine.
