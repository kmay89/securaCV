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
