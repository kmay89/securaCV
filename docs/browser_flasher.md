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
  and the booted slot's `esp_app_desc_t` to show "looks like it's running
  Canary WAP 2.4.18-wap" before you change anything, and the install verdict
  (update, reinstall, downgrade, switch) compares that with the release.
  Best-effort; never blocks flashing. The name and version are there because
  the release stamps them into each app after the build
  (`firmware/scripts/stamp_app_desc.py`: `project_name` is the product's
  `asset_stem`, `version` the firmware's own version string). Images up to
  fw-v2.4.17, and any build made outside the release workflows (`pio run -t
  upload`, `dev_flash.sh`, an OTA push), carry the Arduino core's descriptor
  instead (`arduino-lib-builder`, `esp-idf: v4.4.7 …`), so a board running
  one reads as an Arduino sketch and its verdict as "Version unknown". The
  descriptor's date is not shown beside the version: on an Arduino build it
  is when the Arduino core was built, so only the health report shows it,
  labeled "descriptor dated".
- **Recovery always in reach.** The BOOT/RESET download-mode gesture (the same
  one the WAP first-boot lesson teaches) is one panel away throughout, and a
  non-Chromium browser is met with the guided PlatformIO/Arduino path instead
  of a dead end.
- **Customs, for a board it has never met.** Before a byte is written, the page
  reads the chip's security eFuses (is it locked to a previous owner's key?),
  checks the flash really is the size its id claims, and names what the board
  arrived running — then forces a full chip erase on first contact, because a
  normal install only overwrites the regions it needs. See
  [unflashed_board_intake.md](unflashed_board_intake.md), which also explains
  the one control that has to happen *before* the cable goes in and why no web
  page can substitute for it.

## Browsers and cables

Web Serial is Chromium-only: Chrome, Edge, Brave, Opera, Arc on a **computer**,
and Chrome on Android. Not Safari, not Firefox, and nothing on iOS/iPadOS. The
page detects this and falls back to the guided flash.

Because the XIAO boards expose the ESP32's **native USB** (no CH340/CP210x
bridge), there is usually **no driver to install** — the single biggest
"it won't connect" support cause simply isn't present. A USB-C **data** cable
(not charge-only) is the one requirement.

### What the Canary is called over USB

A common wish is for the board to show up as "SecuraCV Canary" instead of a
generic serial/modem device. The honest picture, because it's silicon-deep:

- **Flashing always talks to the ROM.** To flash, the S3 is in download mode,
  where its **USB-Serial-JTAG** peripheral presents a descriptor fixed in
  Espressif's mask ROM ("USB JTAG/serial debug unit", VID `0x303A`). No firmware
  runs, so nothing we ship can rebrand the name in the flash chooser — on any
  board, in any USB mode. That's a property of the recovery channel, not a bug.
- **At runtime it depends on the USB mode.** The stock profile boots
  **hwcdc** (`ARDUINO_USB_MODE=1`), which reuses that same fixed USB-Serial-JTAG
  descriptor — so the running board also reads generic. Only a build in
  **USB-OTG / TinyUSB** mode (`ARDUINO_USB_MODE=0`) has a programmable
  descriptor. There the name is set by **build flags** — `-DUSB_MANUFACTURER` +
  `-DUSB_PRODUCT` (env `usb-onboard`) — which is what actually takes: with
  CDC-on-boot the core enumerates USB *before* `setup()`, so a runtime
  `USB.productName()` lands too late. With the flags, the **Web Serial chooser,
  Windows, and USB device info** read **`SecuraCV-Canary`** (hyphenated to match
  the BLE GAP name — one identity across both radios). Even then, **macOS keeps
  the port node `/dev/cu.usbmodem…`** — a macOS CDC-ACM convention, not the
  product string, so it never changes.

**Why it isn't the global default.** Two hard reasons, not laziness:

1. **C3/C6 have no USB-OTG hardware.** `canary-vision` (C3) and `canary-sense`
   (C6) expose *only* USB-Serial-JTAG — there is no TinyUSB path to a
   programmable descriptor, so they can never be rebranded. "Every Canary reads
   SecuraCV" is impossible on those units; it's an **S3-only** capability.
2. **Flipping the S3 default touches flashing.** OTG mode changes how the board
   enumerates for flashing (the native-USB DFU path), and the firmware's own
   notes gate it behind on-device (Phase-2) validation and flag it as new USB
   attack surface. Making it the shipping default is a **bench-validated**
   change (does the one-click flasher still auto-enter download mode? does the
   serial console still come up every boot?), not a blind flag flip — otherwise
   it trades a cosmetic name for a "sometimes won't connect," the opposite of
   the flasher's promise.

Net: the branded USB name is a **build-flag** property of an **OTG/TinyUSB,
S3-only** build, shown in the picker label (never the macOS device path), and
promoting it to default is gated on a real-board test. The BLE console (below)
is a cleaner over-the-air "it's a *branded* Canary" signal, but on a WAP only:
`ble_console.h` is the one firmware that serves it, so the flagship and the
C3/C6 boards offer no console to find.

### Showing up on an iPhone over USB-C (the Files app)

A flashed Canary does **not** appear in an iPhone's Files app today, and the
reason is concrete, not mysterious. For a device to show up there it must
enumerate as a **USB Mass Storage** drive — that's the *evidence drive* feature
(`usb_evidence_drive`, `docs/design/usb_evidence_drive.md`). Four things all
have to be true, and today none of the shipped images clear the first:

1. **MSC has to be in the flashed image.** `FEATURE_USB_EVIDENCE_DRIVE` (and the
   `usb-onboard` MSC) are **opt-in build flags, never in a release image** — the
   stock firmware is hwcdc and presents only a serial port, no drive. So a
   normally-flashed Canary offers the iPhone nothing to mount. *This is why "we
   thought we had it" didn't work.* The feature's own header says Phase-2
   on-device validation is still pending — it "has not yet enumerated on real
   hosts."
2. **OTG mode, so S3-only.** MSC needs TinyUSB (`ARDUINO_USB_MODE=0`). The C3/C6
   boards have no USB-OTG at all, so `canary-vision` / `canary-sense` can **never**
   present a drive to a phone — this is an ESP32-S3-only capability.
3. **A filesystem iOS mounts.** iOS Files mounts **exFAT / FAT32 / HFS+ / APFS**
   — *not FAT16*. The evidence LUN serves the **raw SD sectors**, so the SD card
   itself must be **FAT32/exFAT** to appear. ⚠️ The PSRAM *update* drop-zone is
   currently formatted **FAT16** (`fat16_format`), which iOS will not mount — so
   the update-over-USB workflow is PC/Mac-only until that LUN is FAT32/exFAT.
4. **Power.** Over USB-C↔USB-C the **iPhone is the host** and supplies limited
   power; a Canary running WiFi/camera can trip iOS's "accessory needs too much
   power." Give the Canary its **own power** (self-powered, or a powered hub with
   data to the phone).

So the honest path to "browse a Canary's witness files on an iPhone" is: an
**opt-in OTG + evidence-drive S3 image**, a **FAT32/exFAT SD**, **self-powered**,
and a **bench test on a real iPhone** (the Phase-2 step that hasn't run). That
build now exists as the **`canary-wap-usbdrive`** env (it also carries the
branded USB name, so one flash validates both) — flash it and run the checklist
in [`docs/hardware/usb_drive_bench.md`](hardware/usb_drive_bench.md). It is
compile-verified in CI; what remains is the on-device/iPhone confirmation.

## Self-healing (never get stuck)

The flasher tries to fix the common failures before asking the user to:

- **Baud ladder** — connecting tries the fast transfer speed, then steps down
  (921600 → 460800 → 230400 → 115200) automatically. Flaky cables, unpowered
  hubs, and long USB runs sync fine but choke the high-speed transfer; stepping
  down heals it silently instead of dead-ending. (`FLASH_BAUDS`.)
- **Boot-log diagnosis** — the serial monitor watches the boot log for fatal
  signatures (brownout, `Guru Meditation`, no bootable app, flash-read errors)
  and turns each into a plain-language cause + fix, rather than raw panic text.
  (`diagnoseBootLog`.)
- **Failure escalation** — a failed install offers the next self-heal step
  inline: **Clean install (full erase)** reconnects straight into the rescue
  flow (which also restores a safety copy if one was taken).
- **Bridge-chip driver hints** — for the non-native-USB variant, the flasher
  reads the port's USB VID/PID and links the exact driver (CP210x / CH340 /
  FTDI) *only* when the board actually uses that bridge. (`usbBridgeInfo`.)
- **"Copy a diagnostic report"** — every stuck screen offers one click to copy
  a paste-able report (browser, OS, chip, MAC, baud, error, log tail) for
  Discussions. Public-only by construction — never WiFi credentials or keys.
  (`buildDiagnosticReport`.)

These sit on top of what was already there: automatic pre-flash backup,
auto-reconnect across native-USB re-enumeration, chunked reads with per-chunk
retry, and a manual rescue flow.

The baud ladder heals *write-time* failures too, not just connect: a flaky
cable can sync at 921600 but time out mid-transfer, so a failed install lowers
the baud ceiling a rung and the retry writes at the gentler speed (reset for a
fresh board). And the "Clean install" escalation carries the product you were
installing into the rescue, so it can't default to the wrong firmware.

## Liveness (nothing may look stuck)

Real flashes carry real waits — a full-chip erase the silicon runs in
silence (up to ~two minutes on 16 MB parts), a download on whatever network
is at hand, minutes of writing, an on-chip checksum at the end — and a
frozen screen is indistinguishable from a hung one. The rule, enforced by
`desktop_parity.test.js` on both flashers: **every wait shows something that
moves**, plus honest words about what any silence means.

- Every progress card carries an **elapsed clock** ticking once a second;
  after 8 s with no other signal it adds "still working" out loud.
- Waits with no honest number — the erase, the chip recomputing its MD5 —
  **sweep the bar** (`pulse()`) instead of freezing it, and the stage names
  the pause with a size-shaped estimate (`eraseEstimateText`, unit-tested).
- The **image download streams** with live bytes/rate/ETA against the
  release's published size; the write bar counts the engine's own
  (compressed) units per file, so it genuinely reaches 100 %.
- The connect-time reads narrate step by step (current firmware, passport,
  customs — including each flash-capacity probe), clicking Install lands on
  a live card *before* the port re-sync instead of after it, and the serial
  "listening…" line explains itself after five quiet seconds.
- The desktop Flasher mirrors all of it with a progress strip above each
  working console (stage + elapsed + bar), fed by the engine's own progress
  frames, structured download events, and a narrated `board_passport`.

## Minimal mode (the quiet dress)

The Nursery narrates by default — right for a first flash, a lot of reading
for the fortieth. The **🪶 minimal** toggle in the journey bar (also under
⚙ settings) folds the teaching away and keeps everything that does the
work: the journey bar, every control, the live status line, the progress
bar with bytes and time left, the verdicts (customs, install, self-check)
and every error card. Mechanically it is hide-not-remove: one root class on
`#flash` (`flash-minimal`), with `flash.css` owning the exact fold list;
each screen carries a "show the full story for this step" bridge that
un-folds everything for that phase without touching the preference. The
layers tour and the coach are skipped rather than hidden — they run timers.
The choice is sticky per browser (`nursery.minimal` — `minimal.js` owns the
preference), and off by default: quiet is opted into, never sprung.

Error guidance and safety surfaces never fold, in either mode — an error
that folds away is an error that never happened, and the fold list exempts
anything that explains a destructive choice (the forced-erase note) or
discloses a credential-bearing artifact (the done card's safety-copy line).
The desktop Flasher carries the same mode (the feather in the rail,
`prefs.minimal`, `html[data-density="minimal"]`), where it also collapses
espflash's progress-bar redraw frames into one live console line;
`desktop_parity.test.js` holds the two flashers together.

## Post-flash proof

"Watch it boot & prove itself →" doesn't just stream the log — it asks the
running firmware for its **signed self-manifest** (`j`, schema
`securacv.canary.manifest/v1`) and shows a verified-identity card: board,
firmware version, **key fingerprint**, health, and boot count, read straight
from the board over the cable. It's the flash proven from the device's own
mouth — the same self-verify [`securacv.com/canary`](design/self_star_roadmap.md) does
— and it never leaves the page. Variants without a serial console simply don't
show the card (the boot log still streams). (`parseSelfManifest`.)

### The Bluetooth check — proof over the air, not the cable

The self-manifest above proves the flash *over USB*. Its companion proves the
board is alive *over the air*: a Canary's WAP firmware keeps a **read-only BLE
console** running — one GATT service, one snapshot characteristic (READ +
NOTIFY) carrying a ≤220-byte UTF-8 JSON blob of live state — the same fields the
SPA shows over WiFi, on the path that keeps working when WiFi is down
([`firmware/.../ble_console.h`](../firmware/projects/canary-wap/arduino/canary_wap/ble_console.h);
the iOS app's `BLEConsole.swift` knows the same UUIDs but never reaches a WAP's
console: it dials only a peripheral advertising the console service, which a
WAP does not, ledger A65). The flasher's
**Bluetooth check** (reachable any time from the reassurance strip, and offered
after flashing a WAP, the one board that serves the console) answers three plain questions with **Web
Bluetooth** — is Bluetooth on (`navigator.bluetooth.getAvailability()`), can this
browser reach a Canary (`requestDevice` matched to the board's **advertised**
identity — its branded GAP name `SecuraCV-Canary` and its pairing service UUID,
since a BLE advert only fits one 128-bit UUID and the console service isn't the
one advertised; the console service is then reached over the open connection via
`optionalServices`), and what is it reporting (read + subscribe the snapshot,
rendered as a live identity card that names the board). It writes **nothing** to
the board — it's a connectivity test, not a flash channel (you can't flash an
ESP32 over BLE with esptool). Web
Bluetooth is Chromium-only just like Web Serial (desktop or Android; never
Safari/Firefox/iOS), and the console is **bonded-peers-only** by design, so an
unpaired browser is told to pair first rather than dead-ending. A "can't find
it" failure also names the **external antenna** first: on the XIAO ESP32-S3 the
u.FL WiFi/BT antenna must be seated or — per Seeed's own Bluetooth guide — *BLE
may not work at all*, so it's the first thing to check before range or power.
The UUIDs, the availability gate, and the snapshot parser live in
`flash-core.js` (`BLE_CONSOLE`, `bleRequestOptions`, `bleSupport`, `parseBleSnapshot`,
`bleSnapshotRows`), pinned by `tests/flash.test.js`; `flash.js` owns the GATT
dance.

## How it fits the release system

```
firmware-release.yml (fw-v* tag)
  ├─ (existing) app-only <variant>.bin + signed manifest-<variant>.json   → OTA
  └─ (flasher)  <variant>-<ver>-factory.bin  +  manifest-flash.json       → USB
                         │                              │
        make_factory.py merges bootloader +            │ pinned by flash.json at
        partition table + app into one image           │ releases/download/fw-v<x>/;
        flashed at offset 0                             │ pages.yml copies it into the
                                                        │ site (canary-local/firmware/
                                                        │ fw-v<x>/), where flash.html
                                                        ▼ reads it, same-origin
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
  hard-coded. The bootloader goes where the chip's ROM loads it, taken from
  `--chip`: 0x1000 on a classic ESP32 (the image's first 4 KB are erased
  flash), 0x0 on the S3/C3/C6. The bootloader's own header must name the same
  chip, or nothing is merged. `firmware-release.yml` runs it per variant and
  publishes the factory images plus `manifest-flash.json`.
- The flasher catalog (`flash.json`) supplies chips + human copy and is honest
  even before any release exists; the release manifest (`manifest-flash.json`)
  supplies the actual binaries. If no release is published yet, the page says
  this site's copy of it is not available (it cannot tell an uncut release
  from a copy not deployed yet) and points to **Advanced → flash a local
  file**.

### Where the page reads the release from (the same-origin mirror)

A browser **cannot** fetch a GitHub release asset. `github.com` answers
`/releases/download/<tag>/<file>` with a redirect that carries no
`Access-Control-Allow-Origin`, and the asset host it redirects to sends none
either, so the browser refuses the answer whatever the page's CSP allows
(`api.github.com` sends the header, but its asset downloads redirect to the
same host). Until 2026-10-27 the hosted page fetched the release straight from
there, and every product read "unavailable" behind a "connection problem"
([`RELEASE_LESSONS.md`](../.github/RELEASE_LESSONS.md), 2026-10-27).

So the site carries its own copy of the releases it pins:

- **What.** `pages.yml` runs
  [`canary-local/tools/mirror_release.py`](../canary-local/tools/mirror_release.py)
  after it assembles the site. It reads three pins — `flash.json`
  `manifest_url` (the stable channel, an exact `fw-v*` tag), `we2_module.manifest_url`
  (the Vision module's model) and `flash-core.js` `DEV_FLASH_MANIFEST_URL` (the
  dev channel's rolling `fw-dev-latest`) — and copies each manifest **byte for
  byte**, plus every file it lists, to `canary-local/firmware/<tag>/<file>`.
  The tag and file come from each URL itself, so a dev manifest that names its
  images under the real `fw-v*-dev.*` tag is mirrored under that tag.
- **Checked before it is published.** Every image is held to its manifest's
  SHA-256 and size, and a channel lands whole or not at all. A release that
  does not exist yet (the push deploy that merges a version bump runs before
  `firmware-release.yml` cuts the tag), or one mid-upload, is skipped with a
  `::warning::` and the deploy goes on: the page then says *this site's copy*
  of the pinned release is not available, which is true, and never that the
  release was not cut (it cannot see the release itself). A transport failure
  that outlasts the retries, a body cut off mid-read included, fails the
  deploy, which leaves the last good site, and its copy, live.
- **Kept current.** Every deploy mirrors afresh, and because a release made
  with the GITHUB_TOKEN fires no event, `firmware-release.yml`,
  `flasher-release.yml` and `vision-model-release.yml` dispatch `pages.yml` on
  the default branch once they publish (a refused dispatch is a warning and a
  run-summary line, never a failed release). What a deploy carries is what
  the default branch's `flash.json` pins, plus `fw-dev-latest`: an older tag
  rebuilt with **Flasher Factory Images**, or a Vision model repaired for one,
  is not copied at all.
- **How the page reads it.** `flash-core.js` `fetchReleaseAsset` maps a
  release URL to `firmware/<tag>/<file>` beside the page, by the URL's own tag
  and file name; the URLs in `flash.json` and inside the signed manifests are
  never rewritten. On any http(s) page, public, loopback or LAN, it reads
  **only** that copy. There is no fallback to the release URL: no browser can
  read it from any origin, so a fallback would only turn the copy's honest
  "HTTP 404" into a false "connection problem", and the page's CSP names no
  GitHub host. A page that is not on http(s) and a URL that is not the
  project's release (a `?manifest=` override) are fetched exactly as given.
  `flash.js` (manifest and factory image) and `we2-flash.js` (model manifest
  and model) read every release asset through it. When the copy answers an
  HTTP error, the page says *this site's copy* of that release (or of its
  Vision model) is not available and that a release published moments ago
  appears once the site redeploys; when a `?manifest=` override's server
  answers one, the page names that server. It never says a release was not
  cut: the release host answers no browser, so no answer the page gets can
  mean that.
- **Trust is unchanged.** The SHA-256 check, and the Ed25519 check once the
  key is in force, run on the bytes that arrive, from wherever they were
  served, against the manifest as published.
- **From a checkout.** `python3 -m http.server` over the repo serves no copy,
  so the page reports this site's copy of the release as not available. To
  try the hosted path locally, mirror into a scratch copy of the site rather
  than the checkout (the page-logic tests walk `canary-local/`), starting
  from an empty directory (with `/tmp/site` missing, `cp` would copy
  `canary-local`'s contents into it rather than the directory itself), and
  with a token (one run is about fifty API requests; unauthenticated, a
  second run within the hour hits GitHub's limit of sixty):

  ```sh
  rm -rf /tmp/site && mkdir -p /tmp/site && cp -rL canary-local /tmp/site/ &&
    GH_TOKEN=$(gh auth token) python3 canary-local/tools/mirror_release.py --site /tmp/site/canary-local &&
    python3 -m http.server -d /tmp/site 8000
  ```

  then open <http://localhost:8000/canary-local/flash.html>. Or use
  **Advanced → flash a local file**.
- **The desktop apps are untouched.** The Lab's native bench and the Flasher
  fetch the release natively (`flash-native.js` → Rust), where no CORS rule
  applies, and never read the copy.

### A factory flash re-initializes NVS

A factory image spans offset 0 to the end of the app, so flashing it clears the
NVS region (saved WiFi/settings) back to blank. That is intentional and matches
SecuraCV's provisioning model: an AP-based variant (canary, WAP) brings up its
own setup network afterwards; a sensor variant (vision, sense) re-seeds NVS
from the firmware's compiled credentials on first boot
([`firmware_ota.md` § generic release builds + NVS](firmware_ota.md)) — and if
those leave it without a real network, it raises its own `SecuraCV-<kind>-XXXX` setup
network too (the shared setup portal), so a factory flash never strands it.
Every later OTA update inherits that identity. The "erase the entire chip first"
option is a belt-and-suspenders full erase for a misbehaving board.

## Trust model (read this before relying on it)

The flasher verifies an image against the **same pinned Ed25519 release key the
device does** — so a swapped-but-checksummed image (a compromised release or
host) is refused on the USB channel too, not just over the air. Three anchors:

1. **Ed25519 release signature** — when the release is signed and the key is
   provisioned, the browser verifies the signature over `uint32_le(size) ||
   sha256(image)` (the same scheme as `ota_release.py` and the device
   verifier) against the public key pinned into the page. The key is
   single-sourced from `firmware/common/ota/src/ota_release_key.h` into
   `flash.json` by `gen_flash.py`, so it can't drift from what the device
   trusts. A **failed** signature check refuses the flash before any byte is
   written.
2. **SHA-256 integrity** — every image is checked against the manifest digest,
   always, before writing.
3. **HTTPS + the project's own release assets** — the manifest and images are
   the project's GitHub release assets, over TLS; on the hosted Lab they are
   served from the site's own copy of the release (see *Where the page reads
   the release from* above), which the deploy checks against each manifest's
   SHA-256 before publishing. Anchors 1 and 2 are what bind the bytes, wherever
   they were served from.

**Honest fallbacks.** Before the signing-key ceremony the pinned key was
all-zero (`flash.json` pins a real key now), and the out-of-band
`flasher-release.yml` (the pre-key path) published unsigned images; in both
cases the flasher checks **SHA-256 only** and says so plainly (the receipts
show "checksum only, unsigned build"). With a real key pinned, an official
manifest with no signature is refused, not downgraded. A
**local file** (Advanced) is checksum-fingerprinted but, by definition, not
signature-checked. What it never does is *silently* accept an unverified image
when it claimed it would verify one.

The verifier is the vendored, self-hosted [`@noble/ed25519`](https://github.com/paulmillr/noble-ed25519)
(`assets/vendor/ed25519/`); the Python-signer ↔ browser-verifier interop is
pinned by a test. Note this raises the bar to OTA parity but doesn't add
per-device anti-replay: the USB path can still install any *validly signed*
release (including an older one) — that's deliberate, it's the recovery
channel.

### Page & engine integrity (CSP + Subresource-Integrity)

The three anchors above authenticate the *firmware*. They assume the flasher's
own code is honest — so a second layer hardens the page itself, the way a
world-class installer guards its own supply chain.

- **A strict Content-Security-Policy** (a `<meta http-equiv>` in `flash.html`,
  since GitHub Pages can't set response headers). The line is **generated** by
  [`canary-local/tools/gen_csp.py`](../canary-local/tools/gen_csp.py) from the
  one policy table every Lab page shares — the flasher's additions (loopback,
  the import-map hash) are rows there with a reason each, and
  CI regenerates and diffs it (`canary-local/README.md` § 9). `default-src 'none'` denies
  everything by default; `script-src 'self'` / `style-src 'self'` allow only
  this site's own code with **no inline and no `eval`** — the vendored engines
  use neither (verified in CI), so the policy needs no `'unsafe-*'` escape.
  `base-uri`, `object-src`, and `form-action` are `'none'`: no `<base>` rewrite
  of the relative asset paths, no plugins, no form posts. `connect-src` is
  narrowed to `'self'` (the catalog, and the site's copy of the release) +
  **loopback** (`localhost` / `127.0.0.1`) for a same-machine manifest server,
  and names no GitHub host (no browser can read a release asset there), so not even a
  first-party bug could beam your backup or MAC to a third party. That loopback
  set is exactly what the `?manifest=` override accepts (see below), so the code
  guard and the CSP agree — no accepted host is silently blocked at the browser
  layer, and loopback works even from the hosted HTTPS Lab (it's "potentially
  trustworthy", so not mixed-content-blocked).
- **Subresource-Integrity on the vendored modules.** An inline import map pins
  the SHA-384 of each vendored third-party module — esptool-js, md5, ed25519,
  qrcode — so a tampered engine simply won't load: the browser refuses a hash
  mismatch (proven by a Chromium probe that flips a byte and watches the module
  get rejected). First-party app code is same-origin, already constrained by
  `script-src 'self'`, and changes too often to hash by hand, so it's left
  unpinned by design. Enforced on Chromium ≥ 127 — which the flasher already
  requires for Web Serial; older engines ignore the hashes and load the same
  same-origin modules, so nothing breaks.
- **Drift-gated, like the rest.** `tests/flash.test.js` recomputes every SRI
  hash from the real vendored bytes, and the CSP's import-map hash from the
  map's own text, and walks the flasher's module graph so a *new, unpinned*
  vendored import fails CI. The hashes can't silently rot or be skipped.

## How the picker chooses (the Arduino lesson)

Arduino IDE's board menu and PlatformIO's env matrix are the two honest
ways to support many boards — and both intimidate, because they make the
human answer questions the tooling could answer itself (four hundred board
entries; `PSRAM=opi`). The flasher inverts that: **everything it can READ
narrows the choice, and the human only answers what silicon can't.**

The selection ladder (`smartPick` in `flash-core.js`, pure + tested in
`tests/flash_select.test.js`), highest first — every rung states its
evidence in plain words on the page:

1. **What you asked for** — `?product=…` (arriving from /checkup or a doc
   link) wins outright: "Showing X — the firmware you picked."
2. **What the board already runs** — the app descriptor read off the wire
   names the product: "This board already runs X, so it is preselected. An
   install starts its settings over (a new witness key, where it keeps one);
   an over-the-air update keeps them."
3. **What the silicon says** — the chip guard plus the *measured flash
   size* (catalog `flash_mb`, derived from the firmware's own board
   settings). An ESP32-S3 with 16 MB flash can only be the Waveshare panel
   module; with 8 MB it's the XIAO class: "Your board reads as an ESP32-S3
   with 16 MB flash — that looks like a Waveshare 4.3 panel module."
   Where size genuinely can't distinguish (both Vision C3 boards are
   4 MB), the picker says nothing rather than guessing — honesty over
   cleverness.
4. **The authored default** — the catalog is flagship-first per chip, so
   the fallback is still a sensible single card.

One card leads; the full line hides behind "show all N for this chip
(developer)" — and that browse is grouped by **family** (`families` in
`flash.json`), five stories instead of a wall of SKUs, each multi-variant
family asking one plain-language question ("Which glass is in your
hands?") that its products answer with `pick_label`s. Adding a board or
flavor is one `PRODUCTS` entry + one family membership in `gen_flash.py` —
the generator refuses to emit a family with no products, a variant family
with no question, or a product with no answer.

### The size gate (why "show all" still can't brick your boot)

The chip guard says "right silicon"; the size gate (`flashFitVerdict` in
`flash-core.js`) says "enough of it." The one mismatch the write itself
never catches is an image built for a **bigger** flash than the chip in
hand — an ESP32-S3 image for a 16 MB Freenove written to an 8 MB XIAO
passes the chip guard, flashes cleanly, verifies byte-for-byte… and then
boot-loops before the app prints a single line. The tell in the serial
log is misdirection incarnate:

```
E (303) esp_core_dump_flash: Core dump flash config is corrupted! CRC=0x… instead of 0x0
Rebooting...
rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)
```

— a core-dump complaint and a reboot at ~300 ms, when the firmware's own
`setup()` doesn't speak until later. Nothing in that log says "wrong
board," so the flasher has to say it instead: a product whose `flash_mb`
exceeds the measured flash renders unpickable ("needs a 16 MB board —
this chip has 8 MB"), the install click refuses with the same sentence,
and the rescue path filters such images out of its candidates entirely
(a "rescue" into the boot loop is not a rescue). A smaller image on a
bigger chip stays allowed — that's headroom — and a chip whose size
couldn't be measured is never judged. The desktop Flasher enforces the
identical gate with the identical words (`flashFitVerdict` in
`desktop/src/app.js`).

## The display family (watch / dash / dash-modes)

Since the mode-system wave the flasher's product line includes the three
**canary-display** images: `securacv-canary-display-watch` (the round
bedside puck), `-dash` (the plain Waveshare 4.3 wall panel), and
`-dash-modes` — the 4.3B multi-tool that boots the fleet face and carries
the bench / demo / debug / arcade gears behind Settings → modes
(`docs/hardware/display_modes.md`). Three deliberate properties:

- **Three distinct OTA products.** A modes unit must never cross-grade to
  the plain-dash image (it would silently strip the gears *and* swap the
  4.3B pin map for the plain 4.3's) — same rule that keeps a watch image
  off a dash.
- **Provisioning is the glass itself** (`provisioning: "ap"`): a fresh
  display shows a join QR on its own screen and walks the user through —
  the release builds carry the same NVS-backed placeholder scheme as
  vision/sense.
- **The catalog can run ahead of the release.** Display cards appear in
  the picker as soon as `flash.json` carries them; the flash button lights
  up only when a release's `manifest-flash.json` actually offers the image
  (`manifestEntry` returns nothing until then). The emulator preview on the
  same page is live either way.

## Going live (owner steps)

Everything is built; the official images light up when a release is cut:

1. Bump each variant's version and tag as usual (`git tag fw-v<x> && git push
   origin fw-v<x>`) — see [`firmware_ota.md` § Releasing firmware](firmware_ota.md).
2. `firmware-release.yml` now also builds `<variant>-<ver>-factory.bin` and
   `manifest-flash.json` and attaches them to the release. Factory-image
   failures are **warnings, not release blockers**: a variant that doesn't
   merge just shows as "unavailable" in the flasher until the next release.
3. The release run ends by dispatching **pages**, which copies the new release
   into the site (*Where the page reads the release from*, above). Then visit
   `canary-local/flash.html` (or the published Lab), plug in a board, and
   confirm it detects, reads, backs up, flashes, verifies, and boots.

Both release paths build the factory images through the *same*
`firmware/scripts/build_flash_manifest.py`, which reads the product list and
each board's chip from `canary-local/devices/flash.json` — so they can't drift,
and CI fails if a product exists without a build recipe.

### Rebuilding or publishing out of band (`flasher-release.yml`)

The flasher assets have a second, manual workflow — **Actions → Flasher Factory
Images → Run workflow**. It has a `channel` input, and the two channels answer
different questions.

**`channel: release`** — a `fw-v*` tag that already exists:

- **Rebuild without a new version.** Fixed `make_factory.py` or the packaging
  and want to regenerate the factory images for an existing tag? Run it with
  that tag — it compiles the *tagged* firmware but with **today's** packaging
  tooling (it overlays `make_factory.py`, `build_flash_manifest.py`, and the
  `flash.json` catalog from the dispatch ref), so the fix reaches already-cut
  tags, and re-attaches the images in place. (A firmware *source* change still
  needs a new tag — the overlay is packaging only.) It replaces every image on
  the tag, the good ones too: for a tag people already install from, read
  `docs/RELEASE_BUTTONS.md` § Flasher Factory Images, "Know what it replaces on
  a published tag", first.
- **Publish before the OTA key ceremony (no longer).** Before the ceremony
  (it has happened: `flash.json` pins a real key), the browser channel's
  integrity rested on SHA-256 + same-origin, so this path could publish
  without `OTA_SIGNING_KEY_PEM` while `firmware-release.yml` hard-stopped for
  the missing key: tag the release, run `flasher-release.yml` by hand for that
  tag, and it built the images and **created the release if one didn't exist
  yet**. Now it signs like `firmware-release.yml` and refuses to run without
  the secret, since both flashers refuse an unsigned image once a key is
  pinned (see Trust model above).

This channel checks the tag *out*, so the tag has to exist; the workflow now
says so by name (and points at `channel: dev`) instead of dying in
`actions/checkout`.

**`channel: dev`** — no tag at all. It builds the dispatch ref's HEAD and
publishes to the rolling **`fw-dev-latest`** prerelease, which is the one fixed
address the flasher's dev channel reads. No version bump and no tag. It is
signed with the release key, the same as a release, now that one is pinned,
so it needs `OTA_SIGNING_KEY_PEM` and refuses to run without it:

```
Actions → Flasher Factory Images → Run workflow
  branch:  <the branch you want built>
  channel: dev
  tag:     (leave blank)
```

Then in either flasher: **Advanced → dev channel**. Every product the build
produced — displays included — becomes installable, checked like a release's
(its SHA-256, and its Ed25519 signature against the pinned key). Use it when
hardware is on the bench and you need a real image today; use a release for
anything a stranger will install.

Two consequences worth knowing:

- The images were **unsigned only while `release_pubkey` was the all-zero
  placeholder**, when `imageVerificationPolicy()` resolved to `checksum-only`
  and both the banner and the receipt said so. That was the honest state, not
  a downgrade. Since the key ceremony this workflow signs too: it reads
  `ota_key_state.py`, and with the real key pinned it passes `--signing-key` to
  `build_flash_manifest.py` — or **stops** if `OTA_SIGNING_KEY_PEM` is missing.
  Publishing unsigned after the ceremony wouldn't degrade gracefully; the
  policy becomes `require-signature`, both flashers refuse every image in the
  manifest, and a bring-up run would have replaced a working rolling release
  with an uninstallable one.
- **Its images carry the train's version.** The stamp writes the version the
  sources compile in, `registry.json`'s `fw_train` (the last release's number
  until the next bump), which is also what the board reports over the air. So
  a board running a `fw-dev-latest` image reads in either flasher as that
  release: on the stable channel, "already on 2.4.17" and up to date, though
  it runs newer code. Nothing is blocked; the verdict just cannot tell the two
  builds apart until a dev build carries a version of its own.
- `firmware-release.yml` mirrors only *manifests* to `fw-dev-latest` (its
  binaries live at the real `fw-v*` tag: a dev release's, or a stable one's
  when the dev channel carries nothing newer, `.github/scripts/dev_channel.py`);
  this path has no such tag, so it uploads binaries there too and points the
  manifest at them. Each is self-consistent. This path refuses a branch whose
  `fw_train` is older than `releases/latest`, and a firmware release (or
  **Refresh the dev channel**) replaces its `manifest-flash.json` unless it
  carries a newer `fw_train`: press the button before a bring-up at stable's
  version, never after one. Both flashers' dev banners say how the dev
  catalog's `fw_train` compares with the release the flasher is pinned to:
  newer, the same version, or older.

On a normal signed release you don't touch this — `firmware-release.yml` already
produces the flasher assets in the same run. It's dispatch-only precisely so it
doesn't race that workflow.

Air-gapped / self-hosted: the page accepts `?manifest=<url>` to point at a
manifest you host yourself. To keep a crafted link from turning the public Lab
into a firmware-phishing vector, the override is honored **only** for a
same-origin manifest or one on a **loopback** host (`localhost` / `127.0.0.1`) —
a manifest server on the same machine. That set is deliberately the exact set
the page's CSP (`connect-src`) can pin, so the guard and the browser policy
never disagree. For a manifest on another box on the LAN (a private IP or
`.local` name), CSP has no way to allowlist arbitrary private ranges, so either
serve the manifest same-origin alongside the page, or use **Advanced → flash a
local file**, which flashes any factory `.bin` laid out for the connected chip
(its bootloader where that chip's ROM loads it), with no manifest at all — the
fully offline posture the OTA engine also offers.

## Files

| Path | Role |
|---|---|
| `canary-local/flash.html` | the page shell (hero + `<main>` + module script); carries the SRI import map and the generated CSP line |
| `canary-local/tools/gen_csp.py` | writes every Lab page's CSP `<meta>` from one policy table (drift-gated) |
| `canary-local/assets/flash.js` | the renderer + esptool-js glue (the theater) |
| `canary-local/assets/flash-core.js` | DOM-free core: chip guard, image parsers, manifest logic, BLE console contract + snapshot parser (tested) |
| `canary-local/assets/flash.css` | styles, on the Lab's design tokens |
| `canary-local/assets/vendor/esptool-js/` | vendored esptool-js (self-hosted, no CDN) |
| `canary-local/assets/vendor/md5/` | vendored MD5 (ESM) for read-back verify |
| `canary-local/tools/gen_flash.py` | generates the chip-guard catalog from firmware |
| `canary-local/devices/flash.json` | the generated catalog |
| `firmware/scripts/make_factory.py` | merges a build into one factory image |
| `firmware/scripts/build_flash_manifest.py` | builds every factory image + `manifest-flash.json` (shared by both release paths) |
| `.github/workflows/firmware-release.yml` | signed OTA release; also builds flasher assets in the same run, then dispatches `pages.yml` |
| `.github/workflows/flasher-release.yml` | manual rebuild of a tag's flasher assets, or a branch onto `fw-dev-latest` (signed), then dispatches `pages.yml` |
| `.github/workflows/pages.yml` | deploys the Lab, with the same-origin copy of the pinned releases the page reads |
| `canary-local/tools/mirror_release.py` | writes that copy (`firmware/<tag>/<file>`), each image held to its manifest's SHA-256 |

## Testing

- **Logic:** `node --test canary-local/tests/flash.test.js` — chip guard,
  partition-table + `esp_app_desc` parsers, manifest validation, the
  byte↔binary-string glue (with high-byte round-trips), and the CSP + SRI
  drift gate (recomputes every vendored hash + the import-map hash, and fails
  if a vendored import isn't pinned).
- **Drift:** CI runs `gen_flash.py` and diffs `flash.json`; a board change in
  firmware that isn't regenerated is a red X.
- **Where the release is read from:** `flash.test.js` pins the mapping
  (`releaseAssetParts`, `releaseFetchUrl`, `fetchReleaseAsset`), that
  `flash.js` / `we2-flash.js` read no release asset by a bare `fetch`, and the
  banner's two readings of a 404 (this site's copy missing, or a `?manifest=`
  override's server answering an error), neither of which says a release was
  not cut; `canary-local/tests/flash_mirror_probe.mjs` serves the page and a
  fixture copy from one origin in Chromium and fails on any request that
  leaves for github.com, from a public host or from loopback;
  `scripts/tests/test_pages_release_mirror.py`
  holds the deploy's writer to the page's reader (through node) and the
  workflows' dispatches to their order.
- **Policy:** `node --test canary-local/tests/csp.test.js` pins the page's CSP
  to the generated table (and proves it is no weaker than the hand-written
  policy it replaced); `canary-local/tests/csp_probe.mjs` loads it in Chromium
  and fails on any `securitypolicyviolation`.
- **Bench (real board):** plug into Chrome, flash a variant, pull the cable
  mid-write to prove it recovers, then reflash and watch it boot.
- **Bluetooth check:** the UUID contract, availability gate, and snapshot
  parser are pinned in `flash.test.js` (`BLE_CONSOLE`, `bleSupport`,
  `parseBleSnapshot`, `bleSnapshotRows`, `formatUptime`). On a real board:
  open the Bluetooth check, confirm it reports Bluetooth on, pick a running
  Canary, and watch its live snapshot card fill in; a non-Chromium browser
  should get the guided fallback instead of a dead end.
