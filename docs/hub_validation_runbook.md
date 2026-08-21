# Hub hardware-validation runbook

One session, one Raspberry Pi 5, one sacrificial card — this checklist is the
gate between "every line is host-tested" and "a tagged flasher release may
claim the hub path." It validates the four things no CI runner can: the raw
write on a real block device, HAOS accepting our first-boot seeds, the
Pi-over-USB-C gadget flow, and the zero-touch account restore mechanism.
Design: [the one-flash Pi hub](design/raspberry_pi_hub_flashing.md) (§7, §9).

**Kit:** Raspberry Pi 5 · a 32 GB+ microSD you can destroy (64 GB A2
preferred) · USB-C↔USB-C *data* cable · a Mac and a Linux machine — **both a
an Apple-silicon and an Intel Mac if you can get one**, since the USB-C path is
the one flow that has broken on Intel alone (RELEASE_LESSONS 2026-08-02) ·
optionally an NVMe in the Pi and a USB card reader for the
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
- ☐ Linux: same flow. **Finding (2026-07-26): udev IS needed** — without a rule
  granting access to the Pi's `0a5c:` boot device, rpiboot can't open it, so the
  Pi never appears as a disk (looks like "nothing happened"). Fixed: the rule now
  ships in the `.deb` (`/usr/lib/udev/rules.d/60-rpiboot.rules`, from
  `desktop/src-tauri/packaging/rpiboot.rules`) and INSTALL.md documents the manual
  add for AppImage; the app now emits a udev hint when rpiboot reports an
  access/open failure. **Re-validate:** with the rule installed, "Wait for my Pi"
  serves the gadget and the Pi shows up in the picker (and without it, confirm the
  hint appears).
- ☐ Full flash + Wi-Fi seed THROUGH the Pi, then boot it — same result as
  the reader path.
- ☐ If an NVMe is fitted: it appears as a second LUN and is flashable.
- ☐ **Pin the ref:** record the usbboot commit SHA the sidecar build logged,
  set `USBBOOT_REF` in `desktop-flasher-release.yml` to the validated tag,
  and commit — the honest-before-pin window closes here.

## 4 · Account pre-seed ground truth + restore mechanism

**The shipping promise no longer hangs on this section.** Since 2026-08-05 the
app finishes the account over Home Assistant's own onboarding API the moment
the hub answers (`hub_io::onboarding`, design §7 step 5) — it creates the
owner, completes the wizard's remaining pages, and verifies the login, from
whatever state first boot actually produced. Validate that first:

- ☐ Flash with an account typed (seed on or off), keep the app open, boot the
  hub: the first-boot watch must report the account created and the login
  verified, and `homeassistant.local:8123` must accept those credentials with
  no wizard shown.
- ☐ Complete onboarding by hand in a browser first, then let the watch run:
  it must detect the foreign owner and say so plainly — never claim success.

The card-seed work below is still worth doing (a working seed collapses the
window where the hub sits onboarding-less, and it is the only story when the
app is closed during first boot), but it is a head start now, not the promise.

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

Note: the app's account panel writes the minted `.storage` under the boot
partition's `CONFIG/` (Mechanism B) — so a single opt-in flash is the fastest
way to run the Mechanism B experiment above. To see what the SEED did (as
opposed to the API companion), boot with the app closed and check whether
`homeassistant.local:8123` shows a login page (B works) or the setup wizard
(B doesn't → the companion, or A/C, carries the story).

## 5 · The UX behaviors (worth a look while you're here)

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

### Opt-in extras (self-setup's `--with` flags)

The plan's optional features carry the same honest-status caveat as self-setup
itself: host-tested end to end, validated the first time a real run watches
them land. Check each on the session's hub:

- ☐ **Pi-hole** (`--with pihole` / the Flasher's tick): the companion run
  registers the repository, installs and starts the add-on; its page loads
  from the hub; nothing on the network changes until the router's DNS points
  at the hub.
- ☐ **Hub display** (`--with display` / "Also install the hub display"):
  flash with the tick on and an HDMI touchscreen attached — the reference
  panel is a 7" 1024x600 IPS with USB touch. The companion run must install
  HAOSKiosk **without starting it** and say so ("you finish" line names the
  Configuration tab and steers to a dedicated non-admin user). Then, on the
  hub: make the `screen` user (Administrator off), enter its login in
  Settings → Apps → HAOS Kiosk Display → Configuration, press Start — the
  dashboard appears on the panel, touch registers where it taps, and both
  survive a hub reboot. Confirm the non-admin login is enough to view the
  shipped dashboards. Record the zoom/rotation values that suit the 7" panel
  and fold them into [`full_stack_setup.md`](full_stack_setup.md).
- ☐ **Headless unchanged:** a flash with neither tick shows no trace of
  either add-on — the check that opt-in still means zero footprint.

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
- ☐ On the published macOS build, confirm the sidecar is genuinely universal:
  `lipo -archs "SecuraCV Flasher.app/Contents/MacOS/rpiboot"` lists **both**
  `arm64` and `x86_64`, and the same for
  `Contents/Frameworks/libusb-1.0.0.dylib`. CI asserts this, but this is the
  check that would have caught 0.3.9 shipping an arm64-only "universal"
  helper. Best done by running the panel once on an Intel Mac.
