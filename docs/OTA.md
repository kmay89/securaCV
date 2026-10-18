# OTA — how a Canary updates itself, and why it cannot brick

**Scope:** every tree that links the shared engine `firmware/common/ota/src/securacv_ota.{h,cpp}`
— `firmware/canary`, canary-wap, canary-vision, canary-sense, canary-sentinel,
canary-display — the hub side in `custom_components/securacv`, and the
signer `firmware/scripts/ota_release.py` behind [`tools/sign-firmware`](../tools/sign-firmware).
This is the program's design page; the operator's deep dive (manifest
schema, key ceremony, recovery matrix, first-release runbook) stays in
[`firmware_ota.md`](firmware_ota.md) and [`RELEASE_BUTTONS.md`](RELEASE_BUTTONS.md),
and the per-target cells are in [`PARITY.md`](PARITY.md).

## 1. Signed, or nothing

Every image carries an Ed25519 signature over `uint32_le(size) ‖ sha256(image)`
and every manifest a second signature over its own fields, both under ONE
release key whose public half is compiled in (`ota_release_key.h`, key id
`532429078dc47c04`). An all-zero public key fails closed. The private key is
never in the repository: the CI release workflow reads it from the
`OTA_SIGNING_KEY_PEM` secret, a human keeps it on the ceremony machine, and
`tools/sign-firmware` **refuses** a key path inside any source checkout
before it will sign. There is no unsigned path on a release build: the
dev-only `POST /api/ota` on `firmware/canary` is compiled out of `release`
images, and the WAP's BLE and USB drop-file paths check the same signatures.

The signer:

```
tools/sign-firmware keygen   --key /secure/releaser.pem
tools/sign-firmware manifest --key /secure/releaser.pem \
    --product securacv-canary-sense --version 2.5.0 \
    --url https://github.com/kmay89/securaCV/releases/download/v2.5.0/canary-sense.bin \
    --board xiao-esp32c6-mr60 --chip esp32c6 --min-version 2.4.0 \
    --out manifest-canary-sense.json firmware.bin        # verifies what it wrote
tools/sign-firmware verify --pubkey firmware/common/ota/src/ota_release_key.h \
    --manifest manifest-canary-sense.json firmware.bin
```

## 2. A/B with a health-gated rollback

Every tree boots from one of two app slots (`partitions_*.csv`, 2 × 0x1E0000
on the small flashes, larger on the 8/16 MB boards). A new image is written
to the inactive slot, verified (size, sha256 of the written bytes, image
signature) and marked `PENDING_VERIFY`; the bootloader runs it once. The
engine confirms it (`esp_ota_mark_app_valid_cancel_rollback`) only when:

1. every **required probe** the board registered passes
   (`securacv_ota_register_selftest` + `securacv_ota_boot_self_test`) — the
   fleet rule is "Wi-Fi associated"; `firmware/canary` adds its diagnostics
   score; and
2. the board has reported itself healthy for the **confirm dwell**
   (`securacv_ota_health_tick(now, healthy)`, default 60 s, set by the board's
   config) — a crash loop, a watchdog reset or a wedged radio inside the
   dwell reboots into the previous image instead of confirming the new one.
   A board that sets no dwell confirms at the probe, as before.

A failed required probe calls `esp_ota_mark_app_invalid_rollback_and_reboot`
on the spot. The anti-rollback floor in NVS rises **only after** confirmation,
so a rolled-back version can be offered again.

What this is not: bench-verified. The rollback path is compile-checked and
host-tested; `V1_BENCH_TEST_RUNBOOK.md` Track E (pull the plug mid-install,
flash a self-failing image) is still unchecked, and `PARITY.md`'s E2E plan
lists the exact steps.

## 3. Anti-brick checks, on the device

Before a byte is written:

| Check | Where | Refusal |
|---|---|---|
| manifest signature over every field | engine | `MANIFEST_SIG` |
| `product` equals the compiled product id | engine | `MANIFEST_INVALID` |
| `board` / `chip` equal the compiled board (`CANARY_FIGURE_HARDWARE`) and IDF target — **manifest_version 2** only; a v1 manifest carries no hardware fields and a v1 manifest that does is refused by the signer | engine | `BOARD_MISMATCH` |
| `version` newer than max(running, NVS floor) | engine | `VERSION_ROLLBACK` |
| running version ≥ `min_version` — **enforced now**, not logged: a device below the floor is told to install the intermediate release first | engine | `MIN_VERSION` |
| `size` fits the inactive slot | engine + CI (`flavors.json` `size_guards`, `check_slot_budget.py`) | `SIZE_MISMATCH` |
| URL is https, or http to a private host the owner opted into | engine (`securacv_ota_url_allowed`) | `URL_POLICY` |

After writing: sha256 of the written partition against the manifest, then
the image signature. Then the pending-verify boot above.

## 4. Three ways an update arrives

| Path | How | Where it is on |
|---|---|---|
| **Manifest pull on a schedule** | daily, jittered, first check ~2 min after boot; `GET` of the product's manifest from the compiled URL, or an owner-set override on a private server (`securacv_ota_set_manifest_url`, `local_http_allowed`) | every tree that links the engine |
| **Hub push** | Home Assistant's update entity (MQTT discovery, `device_class: firmware`) — "Install" publishes `install` to `securacv/<id>/update/cmd`; the Auto Update switch opts a node into unattended installs. The **staged rollout** below drives several of these one at a time | every MQTT-publishing tree |
| **Manual** | the WAP's signed BLE OTA (`scv-ble-ota-v2` header) and USB drop-file; on every node with an HTTP surface, `POST /api/ota/check` / `/api/ota/install` against the manifest the node already trusts — there is no unsigned upload route anywhere, so "manual" means "point the node at a signed manifest you host" | WAP (BLE/USB); canary, WAP, display (routes); the headless trio through the hub |

## 5. Staged rollout (hub side)

`securacv.staged_update` in the Home Assistant integration
(`custom_components/securacv/rollout.py`): it takes the fleet's `update`
entities (every Canary's firmware entity, or the ones you name), installs on
**one node at a time**, waits for that node to report the new version through
its update entity (up to `node_timeout`, default 10 minutes), and **aborts the
rollout on the first node that fails or times out**, returning what was
installed, what was skipped and what failed. A node whose update entity says
it is already current is skipped, never re-pushed. Hub-less fleets have no
rollout driver in this program — the coordinator in `peer_protocol.h` is the
intended seat for one, and `PARITY.md` marks it ⚠️.

## 6. Visibility

- **On the node:** `GET /api/status` → `ota.state` (idle / checking /
  downloading / verifying / flashing / rebooting / error), `ota.target` (the
  version available or installing) and `ota.pending_verify` (this boot is the
  unconfirmed image). `GET /api/ota/status` keeps the engine's full record
  where the tree serves it.
- **On the hub:** the update entity (installed / latest / progress / release
  notes), the Auto Update switch, and the rollout service's response.
- **On the Witness Wall:** the roll-call rows carry `fw` and, while an
  install is in flight, `ota` (`fleet_selfreport.h`), so a Wall shows which
  node is mid-update; the display aggregates the peers it hears.

## 7. Where each rule lives

| Rule | Code | Test |
|---|---|---|
| signatures, floors, board/chip, min_version | `common/ota/src/securacv_ota.cpp` | `firmware/tests_host/test_ota_logic.cpp`, `firmware/scripts/test_ota_release.py` |
| manifest v2 message | `ota_release.py` ↔ `securacv_ota_build_manifest_message()` | the shared fixture in both tests |
| confirm dwell | `securacv_ota_health_tick()` | `test_ota_logic.cpp` |
| staged rollout | `custom_components/securacv/rollout.py` | `custom_components/securacv/tests/test_staged_update.py` |
| the signer wrapper's in-repo refusal | `tools/sign-firmware` | `firmware/scripts/test_ota_release.py` (`TestSignFirmwareWrapper`) |
