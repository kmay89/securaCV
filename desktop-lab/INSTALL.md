# Installing the SecuraCV Lab

No App Store, no account, no browser. Download it, run one line the first time
on macOS, and it's yours.

Every macOS build is **universal**: the same download runs natively on Apple
Silicon (M1/M2/M3/M4) *and* Intel Macs. You don't pick a version.

---

## macOS — getting past the safety warning (one time)

Because this app doesn't come from the App Store, macOS **quarantines** it on
first download and refuses to open it — it does this to *everything* from the
web, signed or not. You'll see one of these the first time:

> *"SecuraCV Lab" can't be opened because Apple cannot check it for malicious
> software.*
>
> — or, on some Macs —
>
> *"SecuraCV Lab" is damaged and can't be opened.*

Neither means anything is wrong with the app — it's just the missing Apple
notarization. Clear it **once** and it launches normally forever after. The
`.dmg` window shows this same command; here it is in full.

### The one-liner (works on every macOS version — including Sequoia)

1. Open the `.dmg` and drag **SecuraCV Lab** into **Applications**.
2. Open **Terminal** (⌘-Space, type "Terminal", press return).
3. Paste this line and press return:

   ```sh
   xattr -dr com.apple.quarantine "/Applications/SecuraCV Lab.app"
   ```

4. Launch it from Applications or Spotlight. Done — no more warnings, ever.

That command isn't granting anything risky: it removes the "downloaded from the
internet" tag Apple stamps on the file. Identical on Apple Silicon and Intel.

> **On recent macOS, use the command above — not right-click → Open.** Apple
> removed the old right-click bypass in macOS 15 (Sequoia), and the "damaged"
> variant of the warning has no clickable override at all. The one-liner is the
> reliable path on every version.

### If you'd rather not use Terminal (macOS 14 Sonoma and earlier only)

1. **Double-click** the app once; on the *"cannot check it"* warning, click
   **Done** (never "Move to Trash").
2. Open **System Settings → Privacy & Security**, scroll to **Security**. You'll
   see *""SecuraCV Lab" was blocked…"* with an **Open Anyway** button — click it,
   confirm, and authenticate.

> This path does **not** clear the *"is damaged"* variant — for that, use the
> one-liner.

> **Why the extra step at all?** The zero-step experience means paying Apple's
> $99/yr Developer Program and notarizing every release. The app is fully open
> source, so we ship it directly. To turn on signing later, add the Apple
> signing secrets and set the `ENABLE_MACOS_SIGNING` repo variable to `true` —
> see [`README.md`](README.md).

---

## Linux

Two formats on the release page — take your pick. (GitHub stores release
asset names with dots where the app's name has a space, so the downloads are
named `SecuraCV.Lab_…`.)

- **AppImage** (self-updating, runs anywhere):

  ```sh
  chmod +x SecuraCV.Lab_*_amd64.AppImage
  ./SecuraCV.Lab_*_amd64.AppImage
  ```

- **.deb** (Debian/Ubuntu):

  ```sh
  sudo apt install ./SecuraCV.Lab_*_amd64.deb
  ```

### Serial access for USB flashing (one time)

Flashing a **Canary** from the Lab's Flash page needs two things from Linux
that macOS grants for free: permission to open the USB serial device, and
**ModemManager keeping its hands off it**. ModemManager probes a just-plugged
board (or one re-enumerating after the post-flash reset) as if it were a
modem — it can hold the port for ~30 s and its probing can reset the board
mid-boot, so the flash verifies but the live boot receipt never arrives.
This is the same rule the SecuraCV Flasher ships.

- **`.deb` users:** the rule ships with the package
  (`/usr/lib/udev/rules.d/61-securacv-lab.rules` — the Flasher's `.deb`
  installs the identical rule as `61-securacv-canary.rules`, and having both
  is harmless) and fixes both — just **replug the board** after installing.
- **AppImage / other users:** add it once (skip this if you already added it
  for the Flasher — it is the same file):

  ```sh
  sudo tee /etc/udev/rules.d/61-securacv-canary.rules >/dev/null <<'EOF'
  ATTRS{idVendor}=="303a", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1", MODE="0666", TAG+="uaccess"
  ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="55d3", ENV{ID_MM_DEVICE_IGNORE}="1", ENV{ID_MM_PORT_IGNORE}="1", MODE="0666", TAG+="uaccess"
  EOF
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```

  Then replug the board. (`303a` is the Canary's own ESP32 USB port;
  `1a86:55d3` is the Grove Vision AI V2 camera module.)

- **Fallback for the permission half only:** add yourself to the `dialout`
  group and log back in — but note this does *not* stop ModemManager:

  ```sh
  sudo usermod -aG dialout "$USER"   # then log out and back in
  ```

---

## The menu bar companion

While the Lab runs it keeps a small fleet icon in the menu bar (macOS) or
the system tray (Linux): how many of your Canaries are online, one line per
device, **Open the Lab**, **Pause notifications** and **Quit**. Every 30
seconds it listens for Canaries announcing themselves on your network and
asks your kernel for its fleet report, and it posts a desktop notification
when that report says a device went offline, came back, lost its hub or has
a chain problem. Only those coarse words — never recordings, never anything
from the sealed log, and never "verified" (the Lab doesn't pair with a
kernel yet, so it repeats the fleet's own report and says so). A device
that simply goes quiet reads "not heard lately" and raises no alarm: a
missed announcement is not an outage.

- **macOS:** closing the window keeps the Lab running in the menu bar (the
  first time, a notification says so); **Quit** is in the menu bar icon, or
  press ⌘Q. Notifications come from the installed app, not from a
  development build.
- **Linux:** the tray needs an AppIndicator host — KDE Plasma, Xfce,
  Cinnamon, MATE and Ubuntu's own GNOME show it; stock GNOME needs the
  "AppIndicator and KStatusNotifierItem Support" extension. The `.deb`
  installs the library it loads (`libayatana-appindicator3-1`); if the
  AppImage shows no icon, install that package. Because a Linux desktop may
  have no tray at all, closing the window quits the Lab there, as it always
  has — notifications arrive while it is open.

---

## Updating

You don't. The Lab checks the project's GitHub releases when it starts (and
every six hours while it stays open) and offers a one-click
**Update & relaunch** when a newer signed build is out. (Self-update covers
the macOS `.app` and the Linux **AppImage**; `.deb` users update through
`apt` or by grabbing the next `.deb` from the
[releases page](https://github.com/kmay89/securaCV/releases).)

---

## Uninstalling

- **macOS:** drag **SecuraCV Lab** from Applications to the Trash.
- **Linux AppImage:** delete the file.
- **Linux .deb:** `sudo apt remove securacv-lab`.

A few small things stay behind (no secrets in any of them) — remove them too for a
complete uninstall:

- **The app data folder** — the self-updater's journal
  (`update-journal.log`) and its declined-version marker (`update-declined`):
  - macOS: `~/Library/Application Support/com.securacv.lab/`
  - Linux: `~/.local/share/com.securacv.lab/` (or under `$XDG_DATA_HOME`)
- **Webview data** (bench preferences kept in the page's local storage):
  - macOS: `~/Library/WebKit/com.securacv.lab`,
    `~/Library/Caches/com.securacv.lab`
  - Linux: inside the app data folder above, plus
    `~/.cache/com.securacv.lab`
- **The Linux udev rule**, only if you added it by hand for the AppImage and
  no longer run the Flasher either: `/etc/udev/rules.d/61-securacv-canary.rules`.
  (The `.deb`'s copy under `/usr/lib/udev/rules.d/` is removed with the
  package.)

The Lab keeps all of its state locally and talks to your own devices. It
reaches the internet for two things only: its update manifest — the one thing
it ever fetches on its own — and, when you press Flash, the signed firmware
release you chose, from the project's GitHub releases.
