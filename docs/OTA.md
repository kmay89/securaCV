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
   (`securacv_ota_health_tick(now, healthy)`, 60 s on every tree that runs
   the engine) — a crash loop, a watchdog reset, a power cut or a wedged
   radio inside the dwell reboots into the previous image instead of
   confirming the new one. A build that sets no dwell confirms at `init()`.

What "healthy" means is each board's call, and none of them needs the
broker — a node with no MQTT must still be able to confirm an update it
installed from its own page:

| Product | Healthy while | Also confirms |
|---|---|---|
| `firmware/canary` (flagship) | the station link is up (the probes themselves run at its healthy gate, 30 s into `loop()`) | a deliberate restart (Home Assistant or HTTP reboot, serial `x`) once the probes passed — the code chose to stop, so it is not a crash inside the dwell |
| canary-wap | the station link is up, **or** it runs standalone ("Use without home WiFi") with its own access point up — it can install over Bluetooth with no station network at all | — |
| canary-display (every flavor) | Wi-Fi is up; the broker is not required | — |
| canary-vision, canary-sense, canary-sentinel | Wi-Fi is up; the broker is not required | — |

Until the image is confirmed, the update is not over: the one-shot install
marker the engine wrote when the boot partition flipped stays where it is
(`securacv_ota_take_pending_version()` returns false while the running image
is `PENDING_VERIFY`). Each tree records the outcome from one function, called
at boot after the probes and again on the pass the health tick confirms: the
confirming pass records "applied"; a revert inside the dwell boots the old
image, which finds the marker and records the rollback. Engines before 2.4.17
consumed the marker on the new image's first boot, so a revert inside the
dwell went unrecorded while the record said "applied".

The order of the two calls cannot change the answer. Every board runs the
probe (`securacv_ota_boot_self_test`) before `securacv_ota_init`, because the
probes must run before the network comes up — so at probe time the dwell is
not known yet. The probe therefore never confirms on its own: it arms a pure
dwell state machine (`securacv_ota_dwell_*`, host-tested in
`test_ota_logic.cpp` in both call orders), `init()` resolves it once the
dwell is known (a dwell → the ticks decide; none → confirm now), and the
ticks confirm after the dwell has held. The otadata partition is read only
while a dwell is armed. A build that can install an image but runs no
engine (the flagship's `secure` envs) says so with
`securacv_ota_confirm_now()` and confirms at the probe as it always did; the
flagship's deliberate-restart path calls the same function once the probes
passed.

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
| every field fits its buffer whole — refused, never cut to fit (`securacv_ota_manifest_from_fields`); `ota_release.py verify` holds a release to the same sizes (`DEVICE_FIELD_BYTES`) before it is published | engine + CI | `MANIFEST_INVALID` |
| manifest signature over every field | engine | `MANIFEST_SIG` |
| `product` equals the compiled product id | engine | `MANIFEST_INVALID` |
| `board` / `chip` equal the compiled board (`CANARY_FIGURE_HARDWARE`) and IDF target — **manifest_version 2** only; a v1 manifest carries no hardware fields and a v1 manifest that does is refused by the signer | engine | `BOARD_MISMATCH` |
| `version` newer than max(running, NVS floor) | engine | `VERSION_ROLLBACK` |
| running version ≥ `min_version` — **enforced now**, not logged: a device below the floor is told to install the intermediate release first | engine | `MIN_VERSION` |
| `size` fits the inactive slot | engine + CI (`flavors.json` `size_guards`, `check_slot_budget.py`) | `SIZE_MISMATCH` |
| URL is https, or http to a private host the owner opted into | engine (`securacv_ota_url_allowed`) | `URL_POLICY` |

After writing: sha256 of the written partition against the manifest, then
the image signature. Then the pending-verify boot above.

Two version boundaries the release workflow respects:

- **manifest_version 2 (board / chip) needs 2.4.17 or newer on the device.**
  Older engines rebuild a v1 message for a v2 manifest and refuse it, so the
  release workflow keeps signing v1 (no `--board` / `--chip`) until the
  fielded units have moved past 2.4.16.
- **Product ids of 32 bytes or more verify only on 2.4.17 or newer.** Older
  engines kept the id in `char product[32]` and cut it to fit, so seven
  Canary Display products (`dash-modes`, `nightstand7`, `nightstand-s3`,
  `touch169`, `amoled241`, `nightstand-c6`, `nightlight-c3`) never verified a
  manifest over the air. 2.4.17 holds 47 bytes (`SECURACV_OTA_PRODUCT_MAX`,
  host-tested against every id the build files declare); a unit on 2.4.16
  needs one install with the browser flasher or the Flasher app to reach
  2.4.17, and updates over the air work from then on. `ota_release.py verify`
  prints a note for such a manifest.

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
| signatures, floors, board/chip, min_version | `common/ota/src/securacv_ota.cpp` | `firmware/common/ota/test_ota_logic.cpp`, `firmware/scripts/test_ota_release.py` |
| manifest v2 message | `ota_release.py` ↔ `securacv_ota_build_manifest_message()` | the shared fixture in both tests |
| fields whole or refused; the device's buffer sizes | `securacv_ota_manifest_from_fields()` ↔ `ota_release.DEVICE_FIELD_BYTES` | `test_ota_logic.cpp` (every declared product id), `test_ota_release.py` (`TestDeviceBuffers` reads the header) |
| confirm dwell | `securacv_ota_health_tick()` | `test_ota_logic.cpp` |
| the install marker waits for confirmation | `securacv_ota_take_marker()` behind `securacv_ota_take_pending_version()` | `test_ota_logic.cpp` |
| staged rollout | `custom_components/securacv/rollout.py` | `custom_components/securacv/tests/test_staged_update.py` |
| the signer wrapper's in-repo refusal | `tools/sign-firmware` | `firmware/scripts/test_ota_release.py` (`TestSignFirmwareWrapper`) |
