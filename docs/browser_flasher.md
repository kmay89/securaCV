# The browser flasher — flash a Canary from Chrome, over USB

Plug a Canary board into a Chromium browser and flash it from a web page: no
Arduino IDE, no PlatformIO, no terminal. The flasher lives in the Lab at
[`canary-local/flash.html`](../canary-local/flash.html) and is built around one
promise it can actually keep — **you cannot brick the board from here** — with
enough feedback that a first-timer can't get lost.

It is the **first-flash and recovery** channel. Routine updates still go
[over the air](firmware_ota.md), where the A/B partitions, anti-rollback floor,
and boot self-test give a safety net a full USB overwrite can't. Think of the
browser flasher as the one-click version of the recovery matrix's last row
("Absolute worst case → USB flash — always possible").

## Why it's safe to offer

Every Canary board is an ESP32-family chip (S3, C3, or C6). Their first-stage
bootloader is **mask ROM** — unerasable over USB. A wrong or interrupted write
just drops the chip back into download mode, and you flash again. The flasher
never issues an eFuse command, so the one truly irreversible operation is out
of reach. This is the same property [`firmware_ota.md`](firmware_ota.md)
already leans on.

On top of the silicon guarantee, the page adds:

- **A chip guard.** It detects the connected chip and only offers images built
  for that exact silicon — a C3 is never shown an S3 image, and the ROM would
  refuse it anyway. The catalog that drives this is *generated from the
  firmware tree* (see below), so it can't drift.
- **Verify before and after.** The downloaded image is checked against the
  release manifest's **SHA-256 before** a byte is written; after writing, the
  device's own **flash MD5** is read back and compared (esptool's
  `flashMd5sum`). Two independent checks, one each side of the write.
- **A one-click backup.** Before flashing, the user can read the whole chip to
  a `.bin` and download it — a literal undo button, restorable from the same
  page.
- **Reads what's already there.** The page parses the on-board partition table
  and `esp_app_desc_t` to show "looks like it's running canary-wap 2.1.0"
  before you change anything. Best-effort; never blocks flashing.
- **Recovery always in reach.** The BOOT/RESET download-mode gesture (the same
  one the WAP first-boot lesson teaches) is one panel away throughout, and a
  non-Chromium browser is met with the guided PlatformIO/Arduino path instead
  of a dead end.

## Browsers and cables

Web Serial is Chromium-only: Chrome, Edge, Brave, Opera, Arc on a **computer**,
and Chrome on Android. Not Safari, not Firefox, and nothing on iOS/iPadOS. The
page detects this and falls back to the guided flash.

Because the XIAO boards expose the ESP32's **native USB** (no CH340/CP210x
bridge), there is usually **no driver to install** — the single biggest
"it won't connect" support cause simply isn't present. A USB-C **data** cable
(not charge-only) is the one requirement.

## How it fits the release system

```
firmware-release.yml (fw-v* tag)
  ├─ (existing) app-only <variant>.bin + signed manifest-<variant>.json   → OTA
  └─ (flasher)  <variant>-<ver>-factory.bin  +  manifest-flash.json       → USB
                         │                              │
        make_factory.py merges bootloader +            │ fetched by flash.html at
        partition table + app into one image           │ releases/latest/download/
        flashed at offset 0                             ▼
                                          { schema, products: { <id>: {
                                            version, chipFamily, factory, sha256, size } } }
```

- **`canary-local/tools/gen_flash.py`** generates
  [`canary-local/devices/flash.json`](../canary-local/devices/flash.json): the
  product list and each product's chip, **re-derived from the PlatformIO board
  settings** and refused if they disagree. This is the chip guard's source of
  truth, drift-checked in CI exactly like `gen_start.py` / `gen_wap.py`.
- **`firmware/scripts/make_factory.py`** merges a build's `bootloader.bin` +
  `partitions.bin` + `firmware.bin` into a single factory image, reading the
  app offset straight from the compiled partition table so no offsets are
  hard-coded. `firmware-release.yml` runs it per variant and publishes the
  factory images plus `manifest-flash.json`.
- The flasher catalog (`flash.json`) supplies chips + human copy and is honest
  even before any release exists; the release manifest (`manifest-flash.json`)
  supplies the actual binaries. If no release is published yet, the page says
  so and points to **Advanced → flash a local file**.

### A factory flash re-initializes NVS

A factory image spans offset 0 to the end of the app, so flashing it clears the
NVS region (saved WiFi/settings) back to blank. That is intentional and matches
SecuraCV's provisioning model: an AP-based variant (canary, WAP) brings up its
own setup network afterwards; a sensor variant (vision, sense) takes its
credentials from the firmware and re-seeds NVS on first boot
([`firmware_ota.md` § generic release builds + NVS](firmware_ota.md)). Every
later OTA update inherits that identity. The "erase the entire chip first"
option is a belt-and-suspenders full erase for a misbehaving board.

## Trust model (read this before relying on it)

The **OTA** channel's authenticity rests on an Ed25519 chain the device
verifies against a pinned key. The **browser** channel is different: the user
is physically holding the board over USB, and the trust anchors are

1. **HTTPS + same-origin GitHub Releases** — the manifest and images are served
   from the project's own release assets over TLS, and
2. **SHA-256 integrity** — every image is checked against the manifest's digest
   before it's written.

The browser has no pinned-key ceremony, so `manifest-flash.json` is **not**
Ed25519-signed the way the OTA manifest is; binding the browser path to the
release key is possible future hardening but buys little against a physical-USB
attacker who could flash their own image anyway. For distributing the project's
own firmware to its own users, HTTPS + same-origin + SHA-256 is the same model
ESP Web Tools ships with, and it is stated here rather than implied.

## Going live (owner steps)

Everything is built; the official images light up when a release is cut:

1. Bump each variant's version and tag as usual (`git tag fw-v<x> && git push
   origin fw-v<x>`) — see [`firmware_ota.md` § Releasing firmware](firmware_ota.md).
2. `firmware-release.yml` now also builds `<variant>-<ver>-factory.bin` and
   `manifest-flash.json` and attaches them to the release. Factory-image
   failures are **warnings, not release blockers**: a variant that doesn't
   merge just shows as "unavailable" in the flasher until the next release.
3. Visit `canary-local/flash.html` (or the published Lab), plug in a board, and
   confirm it detects, reads, backs up, flashes, verifies, and boots.

Both release paths build the factory images through the *same*
`firmware/scripts/build_flash_manifest.py`, which reads the product list and
each board's chip from `canary-local/devices/flash.json` — so they can't drift,
and CI fails if a product exists without a build recipe.

### Rebuilding or publishing out of band (`flasher-release.yml`)

The flasher assets have a second, manual workflow — **Actions → Flasher Factory
Images → Run workflow**, with a `fw-v*` tag — for two cases:

- **Rebuild without a new version.** Tweaked `make_factory.py` or a board
  setting and want to regenerate the factory images for an existing tag? Run it
  with that tag; it rebuilds and re-attaches them to the release in place.
- **Publish before the OTA key ceremony.** The browser channel's integrity is
  SHA-256 + same-origin, **not** the Ed25519 OTA key (see Trust model above), so
  it doesn't need `OTA_SIGNING_KEY_PEM`. If you want the one-click flow live
  before setting up OTA signing — where `firmware-release.yml` hard-stops for
  the missing key — tag the release, then run `flasher-release.yml` by hand for
  that tag. It builds the images and **creates the release if one doesn't exist
  yet**, so the flasher lights up while OTA waits on the key.

On a normal signed release you don't touch this — `firmware-release.yml` already
produces the flasher assets in the same run. It's dispatch-only precisely so it
doesn't race that workflow.

Air-gapped / self-hosted: the page accepts `?manifest=<url>` to point at a
manifest you host yourself. To keep a crafted link from turning the public Lab
into a firmware-phishing vector, the override is honored **only** for a
same-origin manifest or one on a private/LAN/localhost host — the same hosts
the OTA engine trusts for plain-HTTP local update servers
([`firmware_ota.md` § transport policy](firmware_ota.md)); any other origin is
ignored and the flasher falls back to the signed release. **Advanced → flash a
local file** flashes any factory `.bin` with no manifest at all — the fully
offline posture the OTA engine also offers.

## Files

| Path | Role |
|---|---|
| `canary-local/flash.html` | the page shell (hero + `<main>` + module script) |
| `canary-local/assets/flash.js` | the renderer + esptool-js glue (the theatre) |
| `canary-local/assets/flash-core.js` | DOM-free core: chip guard, image parsers, manifest logic (tested) |
| `canary-local/assets/flash.css` | styles, on the Lab's design tokens |
| `canary-local/assets/vendor/esptool-js/` | vendored esptool-js (self-hosted, no CDN) |
| `canary-local/assets/vendor/md5/` | vendored MD5 (ESM) for read-back verify |
| `canary-local/tools/gen_flash.py` | generates the chip-guard catalog from firmware |
| `canary-local/devices/flash.json` | the generated catalog |
| `firmware/scripts/make_factory.py` | merges a build into one factory image |
| `firmware/scripts/build_flash_manifest.py` | builds every factory image + `manifest-flash.json` (shared by both release paths) |
| `.github/workflows/firmware-release.yml` | signed OTA release; also builds flasher assets in the same run |
| `.github/workflows/flasher-release.yml` | manual rebuild / pre-key publish of the flasher assets |

## Testing

- **Logic:** `node --test canary-local/tests/flash.test.js` — chip guard,
  partition-table + `esp_app_desc` parsers, manifest validation, and the
  byte↔binary-string glue (with high-byte round-trips).
- **Drift:** CI runs `gen_flash.py` and diffs `flash.json`; a board change in
  firmware that isn't regenerated is a red X.
- **Bench (real board):** plug into Chrome, flash a variant, pull the cable
  mid-write to prove it recovers, then reflash and watch it boot.
