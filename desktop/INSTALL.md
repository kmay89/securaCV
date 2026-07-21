# Installing the SecuraCV Flasher

No App Store, no account, no Chrome. Download it, run one line the first time,
and it's yours — and it updates itself from then on.

Every build is **universal**: the same download runs natively on Apple Silicon
(M1/M2/M3/M4) *and* Intel Macs. You don't pick a version.

---

## macOS — the one-time "let me in" step

Because this app isn't distributed through the App Store, macOS quarantines it
on first download (it does this to *everything* downloaded from the web). Clear
the quarantine flag once and it launches like any other app forever after.

### The magic one-liner (recommended)

1. Open the `.dmg` and drag **SecuraCV Flasher** into **Applications**.
2. Open **Terminal** and paste this, then press return:

   ```sh
   xattr -dr com.apple.quarantine "/Applications/SecuraCV Flasher.app"
   ```

3. Launch it from Applications (or Spotlight). Done — it never asks again.

That command doesn't grant anything scary: it removes the "downloaded from the
internet" tag Apple stamps on the file. It works identically on Apple Silicon
and Intel.

### No-Terminal alternative

If you'd rather not touch Terminal: **right-click** (or Control-click) the app
in Applications → **Open** → **Open** again in the dialog. macOS remembers the
exception after that first time. On macOS 15 (Sequoia) you may instead need
**System Settings → Privacy & Security → Open Anyway** right after the first
launch attempt.

> **Why the extra step at all?** Skipping it would mean paying Apple's $99/yr
> Developer Program and notarizing every release. The app is fully open source
> and self-updating, so we ship it directly instead. If you later want the
> zero-step experience, add an Apple Developer signing identity to the release
> workflow — see `README.md`.

---

## Linux

Two formats on the release page — take your pick:

- **AppImage** (self-updating, runs anywhere):

  ```sh
  chmod +x SecuraCV-Flasher_*.AppImage
  ./SecuraCV-Flasher_*.AppImage
  ```

- **.deb** (Debian/Ubuntu):

  ```sh
  sudo apt install ./SecuraCV-Flasher_*_amd64.deb
  ```

### Serial permission (one time)

Flashing needs access to the USB serial device. On most distros, add yourself
to the `dialout` group and log back in:

```sh
sudo usermod -aG dialout "$USER"   # then log out and back in
```

---

## Updating

You don't. The app checks the project's GitHub releases on launch and offers a
one-click **Update & relaunch** when a newer signed build is out. (Self-update
covers the macOS `.app` and the Linux **AppImage**; `.deb` users update through
`apt` or by grabbing the next `.deb`.)

---

## Uninstalling

- **macOS:** drag **SecuraCV Flasher** from Applications to the Trash.
- **Linux AppImage:** delete the file.
- **Linux .deb:** `sudo apt remove securacv-flasher`.

Nothing else is left behind on your machine.
