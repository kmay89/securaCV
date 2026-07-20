# USB Evidence Drive + Drop-File Updates + Release Channels

Design document. Status: **draft — implementation in progress**.
Companion PR implements Phase 1 (logic + scaffolding); flags stay off by
default until hardware validation.

## Why

Three user-facing promises, one USB port:

1. **Plug the Canary into any computer and the evidence is just… there.**
   A read-only USB drive with the witness logs — no app, no driver, no
   account. Works on anything that can read a flash drive, including an
   iPhone/iPad via the Files app (the same port that cannot flash the board
   CAN read a drive from it).
2. **Drop one file on it to update.** A signed `canary-update.bin` dropped
   onto the drive installs on reboot — the same Ed25519-signed artifact and
   verification path as the network OTA, so trust is identical; only the
   transport differs.
3. **A dev channel that is invisible to everyone who didn't opt in.**
   Maintainers test builds with every production feature enabled, while
   release-channel devices (and their Home Assistant update entities) never
   hear about them.

## Part 1 — the evidence drive (USB MSC, read-only)

### Architecture

```
                 ┌────────────────────────────────────────┐
   USB-C ──────► │ TinyUSB composite device (ESP32-S3 OTG)│
                 │  ├─ CDC: the serial console (unchanged)│
                 │  └─ MSC: one volume at a time (v1),    │
                 │     cycled with the console's 'u' key: │
                 │      ├─ "CANARY-EVIDENCE"              │
                 │      │   SD card sectors, READ-ONLY    │
                 │      └─ "CANARY-UPDATE"                │
                 │          PSRAM FAT volume, writable    │
                 └────────────────────────────────────────┘
```

(v1 presents the two volumes as explicit modes rather than simultaneous
LUNs — arduino-esp32's `USBMSC` wrapper is single-LUN, and one-drive-at-a-
time is also the easier mental model to narrate. Moving to true dual-LUN
raw TinyUSB is a contained Phase-2+ upgrade if wanted.)

- **LUN0 — evidence.** Raw SD sectors served via the MSC read callback;
  `is_writable = false` at the USB level, so no host can alter, reorder, or
  delete a witness record through this path. Tamper-evidence is preserved by
  construction, not by politeness.
- **LUN1 — update drop-zone.** A small FAT16 volume in PSRAM (default 4 MB,
  fits every canary-wap OTA slot image). Writable. The host sees an empty
  drive named `CANARY-UPDATE` with a `README.TXT` explaining what to drop.
  **Updates never touch the SD card**: the staging volume is RAM, so a
  half-copied or malicious file evaporates on unplug, and the evidence
  medium is never host-writable.

### Single-writer discipline (why LUN0 is safe)

A FAT filesystem cannot have two writers, and a host caches what it mounts.
The rules, enforced by a small state machine (`evidence_drive_logic`, host-
tested):

- `NORMAL` — firmware owns the SD. MSC not exposed (or LUN0 reports
  not-ready).
- `SHARED` — entered when USB share mode is activated (serial command `u`,
  or web UI toggle). Firmware **flushes and closes** all SD files, then
  stops SD writes entirely; witness records continue — signing and chaining
  live in NVS/RAM — and buffer in a bounded RAM ring; the SD is presented
  read-only to the host. A host writing? Impossible at the USB layer.
- back to `NORMAL` — on host eject / share-mode off / USB unplug: buffered
  records append to SD, life resumes. Ring overflow forces an early
  auto-exit from `SHARED` (evidence durability outranks host convenience —
  documented behavior, surfaced on the console).

### Why not expose the SD writable and "let the update land there"?

Rejected: it would make the evidence medium host-writable (tamper surface),
couple update integrity to FAT-cache coherency, and leave failed copies as
litter on the witness card. The PSRAM staging volume kills all three
problems and is simpler to reason about. (This is the "custom" design; the
stock alternative — arduino-esp32's `USBMSC` straight over the whole SD,
writable, single LUN — is what every example ships and is exactly what we
don't want for an evidence device.)

## Part 2 — drop-file updates (LUN1 → signed OTA)

The engine's trust anchors (see `firmware/common/ota/src/securacv_ota.cpp`)
are: an Ed25519-signed **manifest** (signature over the canonical field
string, `securacv_ota_build_manifest_message`), an Ed25519 **image
signature** over `size_LE32 || sha256`, the recomputed SHA-256, the exact
size match, the `product` id match, and the monotonic NVS anti-rollback
floor. A bare `.bin` is NOT self-verifying — its signature lives in the
per-variant manifest. So the drop is **the two files every release already
publishes**, no new format, no CI change:

1. Host copies `canary-wap-<ver>.bin` **and** `manifest-canary-wap.json`
   (both straight off the GitHub release page) onto `CANARY-UPDATE`.
2. On host eject (or quiesce timeout after the last write), firmware scans
   the FAT root (pure parser, host-tested) for exactly one `.bin` + exactly
   one `manifest-*.json`.
3. Verification is the network path with the download replaced by PSRAM
   reads: parse manifest → verify `manifest_signature` → check `product` →
   compare the staged image's size + SHA-256 to the manifest → verify the
   image signature over `size||sha` → run `securacv_ota_update_decision`
   against the running version and the NVS floor. Any failure → nothing
   written, reason written back as `RESULT.TXT` on the staging volume.
4. Success → image streams from PSRAM into the inactive OTA slot
   (`esp_ota_*`), `securacv_ota_mark_pending_install()` records the target,
   pending-verify + boot self-test + rollback semantics run **unchanged**,
   including the `fw_update_applied` witness record.

No new trust, no new parser surface beyond one FAT root directory we
formatted ourselves. (A future convenience: CI could emit a single
`*-update.scv` container = length-prefixed manifest + image; deferred —
two files from the release page is honest and zero-cost today.)

## Part 3 — release channels (dev / release)

### The question ("tags and show all, or gate?")

**Gate at the manifest, not in the UI.** A release-channel device polls a
manifest that simply never lists dev builds — there is nothing to hide
because nothing arrives. UI gating (fetch everything, filter locally) leaks
dev URLs to every device, invites "show hidden updates" bugs, and makes the
privacy of the dev channel depend on client code. Manifest gating makes it
structural.

### Design (grounded in what exists)

The engine (`firmware/common/ota`) has no channel concept — it polls one
manifest URL, defaulting to `releases/latest/download/manifest-<product>.json`,
with an **NVS `manifest_url` override that already exists**
(`securacv_ota_set_manifest_url`). GitHub's `releases/latest` **never points
at prereleases**. Those two facts make the whole channel design fall out:

- **Tags are the source of truth** (existing `fw-v*` convention):
  - `fw-vX.Y.Z` → stable release → `releases/latest` moves → every default
    device sees it. Exactly today's behavior, untouched.
  - `fw-vX.Y.Z-dev.N` / `fw-vX.Y.Z-rc.N` → GitHub **prerelease** →
    `releases/latest` does NOT move (release-channel devices structurally
    cannot see it) → CI additionally mirrors its assets onto a rolling
    **`fw-dev-latest`** release, giving the dev channel a stable URL:
    `releases/download/fw-dev-latest/manifest-<product>.json`.
- **Identical artifacts.** A dev build is a full production build — same
  features, same signing key, same workflow — differing only in its version
  suffix. ("Test as if it's release" is the requirement; a stripped or
  debug-only dev build would test the wrong thing.)
- **Device-side channel = the manifest URL override that already ships.**
  `channel dev` ≡ `securacv_ota_set_manifest_url(<fw-dev-latest URL>)`;
  `channel release` ≡ clearing the override. HA update entities announce
  only what the device's own manifest offers — release users never see a
  dev version string anywhere, by construction.
- **Version ordering.** `securacv_version_compare` reads only `%d.%d.%d`,
  so `2.3.1-dev.2` and `2.3.1` tie today. This PR teaches the comparator
  prerelease ranking (stable > rc.N > dev.N at an equal triple, numerically
  within a band) so promotion offers the stable build to dev-channel
  devices and anti-rollback floors behave. The variant suffix (`-wap`) is
  not a prerelease marker and stays neutral.
- **Promotion** is retagging the same commit with the stable tag; CI
  rebuilds from the tag with the pinned toolchain and republishes. (Bit-
  identical artifact promotion is a possible future hardening; rebuild-from-
  pinned-tag is the honest current guarantee and is documented as such.)
- **Flasher.** `?channel=dev` loads the `fw-dev-latest` flash manifest (a
  fixed, first-party constant — NOT the arbitrary-URL path, which stays
  same-origin/LAN-guarded) with a visible banner. The default page remains
  release-only.

### Repeatability

One command: `git tag v2.3.0 && git push --tags`. CI does the rest —
build every product, sign, generate the channel's manifest, run the drift
gates that already protect `flash.json`, attach artifacts. The full
sequence, preflight checklist, and rollback ("re-point the manifest at the
previous release", never delete artifacts) live in `docs/RELEASE_PROCESS.md`.

## Feature flags & rollout

- `FEATURE_USB_EVIDENCE_DRIVE` — default **0** everywhere (new USB attack
  surface must be opt-in per the threat model). A CI build variant compiles
  with the flag on so the code can never rot unbuilt.
- Requires `ARDUINO_USB_MODE=0` (USB-OTG/TinyUSB) on the S3; the share-mode
  console command is a no-op on builds without the flag.
- Phase 1 (this PR): host-tested logic (state machine, FAT parser, staging
  volume math), firmware module scaffolding, CI compile gate, channels
  pipeline, docs.
- Phase 2 (hardware in hand): enable on a dev-channel build, validate MSC
  enumeration on macOS/Windows/Linux/iPadOS, then default-on discussion.

## Security review notes

- Evidence LUN read-only at the USB protocol level; share mode pauses the
  only writer. No new write path to witness data exists.
- Update path adds no verifier, no key, no format — it feeds the existing
  signed-OTA pipeline from a RAM volume.
- Channel gating cannot weaken updates: both manifests are generated and
  signed by the same CI; a dev manifest never reaches a device that didn't
  opt in via a local, user-set NVS flag.
- New attack surface (USB MSC parsing lives host-side; device only serves
  sectors and parses one FAT root directory it formatted itself) — bounded
  and host-tested.
