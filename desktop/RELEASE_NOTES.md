# SecuraCV Flasher — release notes

What changed **for the person using the app**, one section per released
version, newest first. This file is load-bearing, not decoration:

- The release workflows publish the newest section as the GitHub release
  body, so the download page says what the download changes.
- The self-updater's manifest (`latest.json`) carries the same section as
  its `notes`, so the in-app "an update is ready" banner can say **what's
  changing** instead of just a version number.
- `desktop/scripts/release_notes.py check` (run by lint and by both app
  release workflows) fails the build if the newest section here doesn't
  match the version in `src-tauri/tauri.conf.json` — bumping the version
  and writing the notes are one act, like the three version files.

Write for the user, not the diff: what they can do now, what got fixed,
and what to expect after updating. Heading grammar is
`## <version> — <YYYY-MM-DD>`.

## 0.3.8 — 2026-07-31

- **Force quitting the Flasher can no longer stop it from opening again.** If
  the app was killed rather than closed, the next launch could get stuck
  bouncing in the Dock with no window and no way in except another force quit —
  the same way every time, with nothing on screen to explain it. The Flasher now
  leaves a breadcrumb as it starts and reads the last one on the way up, before
  it builds a window, so a launch that never arrived gets repaired instead of
  repeated. Three things a force quit used to leave behind are handled:
  - **A flashing tool still holding your board.** `espflash` and `rpiboot` are
    separate programs the Flasher starts; killing the app never killed them, and
    `rpiboot` waits for a Raspberry Pi indefinitely. Their IDs are now recorded
    while they run and cleaned up at the next launch — so "no board found" right
    after a force quit is one less thing that can happen.
  - **A saved session left half-written.** Cleared automatically, once. You'll
    re-enter the Wi-Fi network name and device names you last used; no password
    was ever stored there, so nothing secret is lost.
  - **An interrupted update.** Installing an update replaces the app itself, so
    being killed mid-write can leave a copy your Mac won't finish opening. That
    one only a reinstall can fix — so the Flasher now recognises it and tells
    you, instead of leaving you guessing. It also won't let you quit while an
    update is being written.
- **If it still can't open, it now says why.** Every launch appends a line to
  `launch.log` beside the app's settings (`~/Library/Application
  Support/com.securacv.flasher/` on macOS), and a window that hasn't finished
  loading after 20 seconds offers to clear its saved session and reopen rather
  than leaving you with a bouncing icon.
- Nothing about this affects a device you've flashed. A Canary keeps the
  firmware it already has, and an interrupted flash is always re-flashable.

## 0.3.7 — 2026-07-31

- **Your Wi-Fi is now put into the image before the card is written, so there
  is no re-mount to fail.** Previously the settings were added afterwards, by
  asking your computer to mount the card it had just written — a step that
  failed outright on macOS for at least one person, retry and reseat included,
  leaving a hub that never joined a network. The keyfile now travels inside
  the Home Assistant image itself, so it is covered by the same byte-for-byte
  read-back that already verifies the card, and a setting that can't be placed
  stops the flash in under a second with your card untouched instead of after
  a multi-gigabyte write.

## 0.3.6 — 2026-07-30

- **Your Wi-Fi now has to actually make it onto the hub card.** Saving it was
  treated as a convenience: if the card couldn't be re-mounted, the flash still
  said "Done — written, read back, and verified" and you found out at a Pi that
  never appeared on your network. It is now checked — written, flushed to the
  card, and compared against what we meant to write — and a Wi-Fi you asked for
  that didn't land fails the flash, on the spot, with the reason and what to do
  next.
- **Typing Wi-Fi while "wired ethernet" is selected is no longer resolved
  silently.** It used to drop the Wi-Fi. Now it says which one would win.
- Under the hood: the app's own release build is checked by CI for the first
  time, so a change to the hub writer can't reach you uncompiled.

## 0.3.5 — 2026-07-28

- **The hub really is headless now — and the app says so.** The Wi-Fi you
  type at flash time lands on the card the way Home Assistant's own docs
  specify (a stable connection identity, and name-resolution turned on at
  the OS level), so a Pi 5 joins your network and answers
  `homeassistant.local` with no monitor or keyboard ever attached.
- **The first-boot watch never leaves you guessing.** A visible countdown
  shows exactly when patience stops being the plan; if the hub hasn't
  answered by then, the watch swaps in a concrete checklist — try it from
  a phone, read your router's device list, or fall back to ethernet or a
  re-flash. Quitting and reopening the app mid-boot keeps the same
  countdown and the same checklist.
- **The update banner now says what's changing.** When a newer version is
  ready, the app shows that release's own notes — these very sections —
  not just a version number, so you decide with the facts in front of you.
- **Update checks are now routine, not just at launch.** The app re-checks
  every six hours while it stays open, and again when you come back to a
  window that sat idle — so a bench machine that never relaunches still
  hears about updates. Nothing installs without your click, and every
  update stays signed and verified before it runs.
- Checks and installs are recorded in the About page's activity log, kept
  on this computer only.

## 0.3.4 — 2026-07-28

- The macOS installer window (the DMG) draws its background at the right
  size again — no more giant, cropped artwork with the instructions pushed
  out of view.
- Flashers 0.3.0 and older lost self-update when the update address moved;
  the release pipeline now serves a valid manifest at the old address too,
  so those installs see updates again and heal forward on their own.
