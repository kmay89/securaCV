# Installing the SecuraCV Flasher

No App Store, no account, no Chrome. Download it, run one line the first time,
and it's yours — and it updates itself from then on.

Every build is **universal**: the same download runs natively on Apple Silicon
(M1/M2/M3/M4) *and* Intel Macs. You don't pick a version.

---

## macOS — getting past the safety warning (one time)

Because this app doesn't come from the App Store, macOS **quarantines** it on
first download and shows a warning — it does this to *everything* from the web,
signed or not. You'll see one of these the first time you open it:

> *"SecuraCV Flasher" can't be opened because Apple cannot check it for
> malicious software.*
>
> — or, on some Macs —
>
> *"SecuraCV Flasher" is damaged and can't be opened.*

Neither means anything is wrong with the app — it's just the missing Apple
notarization. Clear it **once** and it launches normally forever after. When
you open the `.dmg`, the window shows these same steps; here they are in full.

### Option A — the one-liner (works on every macOS version)

1. Open the `.dmg` and drag **SecuraCV Flasher** into **Applications**.
2. Open **Terminal** (⌘-Space, type "Terminal"), paste this line, press return:

   ```sh
   xattr -dr com.apple.quarantine "/Applications/SecuraCV Flasher.app"
   ```

3. Launch it from Applications or Spotlight. Done — no more warnings, ever.

That command isn't granting anything risky: it removes the "downloaded from the
internet" tag Apple stamps on the file. Identical on Apple Silicon and Intel.

### Option B — no Terminal

**macOS 14 (Sonoma) and earlier:**
1. In **Applications**, **right-click** (or Control-click) **SecuraCV Flasher**.
2. Choose **Open**, then **Open** again in the dialog.
3. It's remembered — double-click normally from now on.

**macOS 15 (Sequoia) and later** — Apple removed the right-click shortcut, so:
1. **Double-click** the app once. You'll get the "cannot check it" warning —
   click **Done** (do *not* click "Move to Trash").
2. Open  **System Settings → Privacy & Security**, scroll to the **Security**
   section. You'll see *""SecuraCV Flasher" was blocked…"* with an
   **Open Anyway** button — click it.
3. Confirm with **Open Anyway** and authenticate (Touch ID / password).
4. It's remembered from then on.

> If you ever see *"is damaged and can't be opened"*, that's the same
> quarantine flag being strict — **Option A** clears it directly.

> **Why the extra step at all?** Skipping it would mean paying Apple's $99/yr
> Developer Program and notarizing every release. The app is fully open source
> and self-updating, so we ship it directly instead. If you later want the
> zero-step experience, add an Apple Developer signing identity to the release
> workflow — see `README.md`.

---

## Linux

Two formats on the release page — take your pick. (GitHub stores release
asset names with dots where the app's name has spaces, so the downloads are
named `SecuraCV.Flasher_…`.)

- **AppImage** (self-updating, runs anywhere):

  ```sh
  chmod +x SecuraCV.Flasher_*_amd64.AppImage
  ./SecuraCV.Flasher_*_amd64.AppImage
  ```

- **.deb** (Debian/Ubuntu):

  ```sh
  sudo apt install ./SecuraCV.Flasher_*_amd64.deb
  ```

### Serial access (one time)

Flashing a **Canary** needs two things from Linux that macOS grants for free:
permission to open the USB serial device, and **ModemManager keeping its hands
off it**. ModemManager probes a just-plugged board (or one re-enumerating
after the post-flash reset) as if it were a modem — it can hold the port for
~30 s and its probing can reset the board mid-boot, so the flash verifies but
the live boot receipt never arrives.

- **`.deb` users:** the rule ships with the package
  (`/usr/lib/udev/rules.d/61-securacv-canary.rules`) and fixes both — just
  **replug the board** after installing.
- **AppImage / other users:** add it once:

  ```sh
  sudo tee /etc/udev/rules.d/61-securacv-canary.rules >/dev/null <<'EOF'
  ATTRS{idVendor}=="303a", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1", MODE="0666", TAG+="uaccess"
  ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="55d3", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1", MODE="0666", TAG+="uaccess"
  EOF
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```

  Then replug the board. (`303a` is the Canary's own ESP32-S3 USB port;
  `1a86:55d3` is the Grove Vision AI V2 camera module.)

- **Fallback for the permission half only:** add yourself to the `dialout`
  group and log back in — but note this does *not* stop ModemManager:

  ```sh
  sudo usermod -aG dialout "$USER"   # then log out and back in
  ```

### Flash a Pi over USB-C (Linux) — one-time udev rule

Flashing a **Raspberry Pi through its own USB-C** (the "Wait for my Pi" button —
no card reader) uses `rpiboot`, which must open the Pi's boot-ROM USB device.
Linux blocks that for a normal user unless a udev rule grants access — without
it, "Wait for my Pi" waits forever and the Pi never appears as a disk. (macOS
needs nothing here, which is why it just works there.)

- **`.deb` users:** the rule ships with the package
  (`/usr/lib/udev/rules.d/60-rpiboot.rules`) — just **replug the Pi** after
  installing.
- **AppImage / other users:** add it once:

  ```sh
  sudo tee /etc/udev/rules.d/60-rpiboot.rules >/dev/null <<'EOF'
  SUBSYSTEM=="usb", ATTRS{idVendor}=="0a5c", ATTRS{idProduct}=="2763", MODE="0666", TAG+="uaccess"
  SUBSYSTEM=="usb", ATTRS{idVendor}=="0a5c", ATTRS{idProduct}=="2764", MODE="0666", TAG+="uaccess"
  SUBSYSTEM=="usb", ATTRS{idVendor}=="0a5c", ATTRS{idProduct}=="2711", MODE="0666", TAG+="uaccess"
  SUBSYSTEM=="usb", ATTRS{idVendor}=="0a5c", ATTRS{idProduct}=="2712", MODE="0666", TAG+="uaccess"
  EOF
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```

  Then replug the Pi. `2712` is Pi 5, `2711` is Pi 4; if `lsusb` shows a
  different `0a5c:` id while the Pi is in boot mode, add that one too.

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

That removes the app itself, but the Flasher keeps working data on your
machine that removing the app does **not** delete — and some of it is
sensitive, so here is the complete list:

- **The app data folder** — remove it to erase everything in this list's
  first three bullets at once:
  - macOS: `~/Library/Application Support/com.securacv.flasher/`
  - Linux: `~/.local/share/com.securacv.flasher/` (or under `$XDG_DATA_HOME`)

  It holds:
  - `backups/` — the automatic pre-flash **full-chip safety copies**. Each is
    everything that was on a board, including its settings partition — which
    can carry the board's saved Wi-Fi credentials and its device identity
    key. Treat these files like the board itself.
  - `hub-ssh/` — the **maintenance SSH keypair** minted for hub self-setup.
    The private key grants root console access (port 22222) to any hub whose
    card was seeded with its public half. Deleting the folder removes the key
    from this computer; to revoke access on the hub side, remove the matching
    line from the hub's `authorized_keys` (or reflash the card).
  - `launch.log` / `launch-state.json` — the launch journal (no secrets).
- **Saved passwords and device tokens** live in the OS credential store, not
  in a file: on macOS open **Keychain Access** and delete the entries whose
  service is **"SecuraCV Flasher"** (saved Wi-Fi/broker passwords and
  per-Canary API tokens). They survive an app reinstall by design.
- **Webview data** (window preferences, no secrets):
  - macOS: `~/Library/WebKit/com.securacv.flasher`,
    `~/Library/Caches/com.securacv.flasher`,
    `~/Library/HTTPStorages/com.securacv.flasher`
  - Linux: inside the app data folder above, plus
    `~/.cache/com.securacv.flasher`
- **Linux udev rules**, only if you added them by hand for the AppImage:
  `/etc/udev/rules.d/60-rpiboot.rules` and
  `/etc/udev/rules.d/61-securacv-canary.rules`. (The `.deb`'s copies under
  `/usr/lib/udev/rules.d/` are removed with the package.)
