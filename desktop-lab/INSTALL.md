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

Two formats on the release page — take your pick:

- **AppImage** (runs anywhere):

  ```sh
  chmod +x "SecuraCV Lab_"*.AppImage
  ./"SecuraCV Lab_"*.AppImage
  ```

- **.deb** (Debian/Ubuntu):

  ```sh
  sudo apt install ./"SecuraCV Lab_"*_amd64.deb
  ```

---

## Updating

Grab the next release from the
[releases page](https://github.com/kmay89/securaCV/releases) and reinstall —
same one-liner on macOS. (`.deb` users can also update through `apt`.)

---

## Uninstalling

- **macOS:** drag **SecuraCV Lab** from Applications to the Trash.
- **Linux AppImage:** delete the file.
- **Linux .deb:** `sudo apt remove securacv-lab`.

Nothing else is left behind — the Lab keeps its state locally and talks only to
your own devices.
