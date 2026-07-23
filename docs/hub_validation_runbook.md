# Hub hardware-validation runbook

One session, one Raspberry Pi 5, one sacrificial card — this checklist is the
gate between "every line is host-tested" and "a tagged flasher release may
claim the hub path." It validates the four things no CI runner can: the raw
write on a real block device, HAOS accepting our first-boot seeds, the
Pi-over-USB-C gadget flow, and the zero-touch account restore mechanism.
Design: [the one-flash Pi hub](design/raspberry_pi_hub_flashing.md) (§7, §9).

**Kit:** Raspberry Pi 5 · a 32 GB+ microSD you can destroy (64 GB A2
preferred) · USB-C↔USB-C *data* cable · a Mac (Apple silicon) and a Linux
machine · optionally an NVMe in the Pi and a USB card reader for the
reader-path comparison. Build the flasher from this branch with the sidecars
populated (run the release workflow with `dry_run=true` and grab the
artifacts, or build locally per `desktop/README.md`).

Record everything in a copy of the checklist; every ☐ is a release blocker
until checked or consciously waived in writing.

## 1 · Write + read-back (the footgun)

Both OSes, reader path first (it isolates the writer from the gadget):

- ☐ Linux: eligible card appears in the picker with the right size/model;
  the system disk and internal disks appear under "hidden — here's why".
- ☐ Linux: full flash completes — download → verify → unpack → write →
  read-back all green; note wall-clock per stage.
- ☐ macOS: same two checks; confirm the `authopen` authorization prompt
  appears (app not running as root) and the write proceeds after approval.
- ☐ Pull the card mid-write once (yes, on purpose): the error is clear, the
  app stays alive, and a re-flash of the same card succeeds.
- ☐ Read-back honesty spot-check: after a green flash, `head -c 4M` of the
  device hashes to the same prefix on a second machine/reader.

## 2 · First boot + Wi-Fi seed

- ☐ Boot the flashed card in the Pi (wired power, no peripherals). Record
  time to `homeassistant.local:8123` responding.
- ☐ **Wi-Fi seed accepted:** flash again with Wi-Fi typed in the app, boot
  with NO ethernet — the Pi must appear on the Wi-Fi network on its own.
  This is the `CONFIG/network/<id>` keyfile acceptance check
  (`hub-core::hub_seed` note). If HAOS ignores it, capture
  `journalctl -u hassos-config` from the console and file the finding.
- ☐ Hidden-SSID variant once if convenient.

## 3 · The Pi as its own card reader (USB-C)

- ☐ macOS: "Wait for my Pi" → hold power button, connect USB-C, release —
  rpiboot narrates, exits 0, and the card inside the Pi appears in the
  picker badged "your Pi, over USB-C".
- ☐ Linux: same flow (note whether udev needed a rule for `0a5c:2712`; if
  so, add it to the deb/AppImage and this runbook).
- ☐ Full flash + Wi-Fi seed THROUGH the Pi, then boot it — same result as
  the reader path.
- ☐ If an NVMe is fitted: it appears as a second LUN and is flashable.
- ☐ **Pin the ref:** record the usbboot commit SHA the sidecar build logged,
  set `USBBOOT_REF` in `desktop-flasher-release.yml` to the validated tag,
  and commit — the honest-before-pin window closes here.

## 4 · Account pre-seed ground truth + restore mechanism

First capture reality, then pick the mechanism (design §7 step 5):

- ☐ On a stock-flashed hub, complete onboarding by hand (any throwaway
  account), then from the HAOS console copy
  `/mnt/data/supervisor/homeassistant/.storage/{auth,auth_provider.homeassistant,onboarding}`
  and diff their SHAPE against `hub_io::account::mint_owner_store` output.
  Fix the module to match reality; the committed schema is a template until
  this diff is clean.
- ☐ **Mechanism A — data-partition injection (preferred):** from Linux,
  place the minted `.storage` files into the image's data partition before
  writing; boot; first visit must be a login page and the minted
  credentials must work.
- ☐ **Mechanism B — CONFIG import:** if A fails or for the macOS story,
  test whether current HAOS's `hassos-config` can carry the files in from
  the FAT boot partition.
- ☐ **Mechanism C — onboarding restore:** confirm the onboarding screen's
  restore-backup path accepts a curated backup as the no-typing fallback.
- ☐ Record the winner in the design doc §8 as a dated decision.

Note: the app's account panel is labelled **experimental** and writes the
minted `.storage` under the boot partition's `CONFIG/` (Mechanism B) — so a
single opt-in flash is now the fastest way to run the Mechanism B experiment
above. After booting, report whether `homeassistant.local:8123` showed a
login page (win) or the setup wizard (B doesn't work → fall back to A or C).

## 5 · The UX behaviours (worth a look while you're here)

These aren't safety gates but they're the "feels like magic" surface — sanity
check each once on real hardware:

- ☐ **ETA** shows a sensible "about Xm left" during download and write, and
  never a lying full bar on the indeterminate stages.
- ☐ **Cancel** mid-write stops within a second, the card is simply re-flashable,
  and the message is calm ("Stopped, no harm done").
- ☐ **Image cache**: a second flash of the same board reuses the local copy
  ("reused your verified local copy — no re-download") and starts near-instantly.
- ☐ **First-boot companion** flips from "watching…" to "It's alive!" when the
  hub answers, fires the OS notification + chime, and the QR opens the hub
  from a phone.
- ☐ **Remember**: SSID / name / username come back pre-filled next launch;
  passwords never do.
- ☐ **Preflight** warns if the staging disk is low; the macOS permission
  heads-up appears before the write.

### Recovery (the "always recovers" bar — try to break it)

- ☐ **Pull the card mid-write** → clear "the card wandered off… plug it back
  in and type ERASE again" message; the app stays alive and re-arms; a
  re-flash of the same card succeeds.
- ☐ **Quit the app mid-flash** → the "Quit while flashing?" dialog appears;
  "Keep flashing" resumes, "Quit" cancels cleanly and exits.
- ☐ **Force-kill the app (or pull power) mid-download** → relaunch: the
  cache holds no half-file (only a `.partial`, which the startup sweep
  removes); a fresh flash re-downloads cleanly.
- ☐ **Force-kill mid-write** → relaunch: no stale multi-GB image is left in
  the temp dir (startup sweep reclaimed it — check free space).
- ☐ **Reopen soon after a completed flash** → the resume banner appears and
  watches for the hub; when it boots, it flips to "up 🐤" with the notification.
- ☐ **Disk-full mid-run** (fill the staging disk) → specific "ran low on
  room… clear space and try again" copy; the card is untouched.

## 6 · Wrap-up

- ☐ Re-run the freshness workflow (`workflow_dispatch`) so the HAOS version
  validated here is the one pinned in `hub_image_pins.json`.
- ☐ Update the design doc ledger: flip the OUTSTANDING markers this session
  cleared, with the date and the hardware used.
- ☐ Tag `flasher-v*`; verify the published macOS build's rpiboot runs on a
  clean machine (no Homebrew) — the bundled-libusb check.
