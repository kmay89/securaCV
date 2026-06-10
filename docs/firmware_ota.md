# Canary Firmware Updates (Signed Pull-OTA)

How a Canary that's screwed to a ceiling gets new firmware without anyone
touching it — and why it can't be bricked or fed a forged image along the way.

**Covered variants:** all of them —

- **canary** (PIO, `firmware/canary/`)
- **canary-wap** (`firmware/projects/canary-wap/` — one sketch built by both
  arduino-cli and PlatformIO, so both toolchains ship the same OTA)
- **canary-vision** (PIO, `firmware/projects/canary-vision/`)

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
  "signing_key_id": "<16 hex>",
  "release_notes": "Improved detection accuracy",
  "release_url": "https://github.com/.../releases/tag/fw-v2.2.0"
}
```

`manifest-index.json` maps products to their manifest URLs for tooling and
future variants. Devices fetch only their own flat manifest.

`firmware/scripts/ota_release.py` is the single source of truth for this
format (`keygen` / `sign` / `manifest` / `index` / `verify`), reused by the
release workflow and the local mock server.

## Signature scheme

```
message   = image_size as uint32 little-endian (4 bytes)
          || sha256(firmware.bin)              (32 bytes)
signature = Ed25519.sign(message)              (64 bytes)
```

Identical to the BLE OTA path (`ble_ota.cpp`), so **one release key signs
every update channel**. The device verifies against the public key compiled
into `firmware/common/ota/src/ota_release_key.h` — and critically, it
verifies over the digest it computed from the **actual flash contents**,
not the manifest's claim.

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

## Transport policy & local hosting

- **Public URLs require HTTPS** (certificate bundle; GitHub works out of
  the box).
- **`http://` is allowed only** when the user enables the local-update-server
  option (Settings, NVS-persisted) **and** the host is private:
  10/8, 172.16/12, 192.168/16, 127/8, 169.254/16, `localhost`, or a name
  under `.local` / `.lan` / `.internal` / `.home.arpa`.

Image integrity never depends on the transport — the Ed25519 signature and
the anti-rollback floor do that work — so air-gapped installs don't need a
certificate authority. The known residual risk: the **manifest itself is
unsigned**, so a hostile local mirror can offer any *previously signed*
release newer than the device's floor. Signing the manifest is the named
follow-up that closes this.

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

All endpoints are bearer-token gated, on canary and canary-wap
(canary-vision has no local HTTP server — it is HA/MQTT-managed only):

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

## Safety properties

- **A/B partitions:** the running firmware is never touched; power loss at
  any point lands on a bootable slot.
- **Health-gated commit:** the new image is only confirmed after the boot
  self-test suite passes; a required failure triggers
  `esp_ota_mark_app_invalid_rollback_and_reboot()`.
- **Install gating:** the device defers installs while on a low battery
  (canary) or while a BLE OTA session is active (canary-wap); the two OTA
  channels strictly exclude each other.
- **Pre-reboot flush:** witness-chain state is persisted before the install
  reboot, mirroring the manual-reboot path.
- **Never reboots unattended by default:** auto-update is an explicit
  per-device opt-in.

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
   OTA-slot `size_guard` budget — to `firmware/flavors.json`; the
   `firmware.yml` matrix picks it up with no new jobs (see
   `firmware/ARCHITECTURE.md` § CI Flavor Manifest).
5. **Keep identity out of the binary**: anything per-unit (device id,
   credentials) must live in NVS so generic release images inherit it
   (see canary-vision's `runtime_config` for the pattern).

The signing key, manifest schema, transport policy, and anti-rollback floor
come with the engine — don't reimplement them.

## Testing

- Host: `pytest firmware/scripts/test_ota_release.py` and the
  `test_ota_logic.cpp` host build (both run in `firmware.yml`).
- Bench (real device): build with a dev key embedded, serve a
  newer-versioned image from `mock_ota_server.py`
  (`SECURACV_OTA_SKIP_CERT_VERIFY=1` dev builds accept the self-signed
  cert), press Install in HA or the dashboard, watch the progress bar,
  confirm the version flips. Pull power mid-flash to prove rollback; check
  the witness chain for the `fw_update_*` records.
