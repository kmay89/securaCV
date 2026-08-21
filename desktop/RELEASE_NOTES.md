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

## 0.11.5 — 2026-08-20

**Minimal mode — for the fortieth board of the day.**

A new feather button in the rail (next to day/night) folds the explainer
prose away: the page ledes, the product taglines, the settled support-tier
lines and the provisioning note step aside, the cards tighten up, and the
flash console collapses the progress bar's redraw spam into one live line.
Every control, safety check, receipt and warning stays exactly where it was
— including the first-contact wipe choice and the "never booted" tier note —
and one click brings the full story back. The choice is remembered. The
browser flasher gains the same mode in the same release.

## 0.11.4 — 2026-08-15

**Flashes firmware 2.4.11 — the bedside fixes.**

2.4.11 is about the display on your nightstand. Its clock defaults to US
Eastern instead of UTC, so it no longer reads 3 a.m. at 11 a.m. — which also
kept the face stuck in night mode through the morning and left the on-screen
settings corner dark and unresponsive in daylight. Night now keeps a dim glow
by default instead of going fully dark, the clock's night red is a red you can
actually read at a glance, and the settings corner opens day or night.

Nothing changed in the Flasher itself. This release exists because the app
bakes the firmware catalog in when it is built, so an older Flasher keeps
installing the older firmware no matter what has shipped since.

## 0.11.3 — 2026-08-14

**Flashes firmware 2.4.10, and it tells you the truth about what it wrote.**

2.4.10 is the release that fixes the display setup network. A 7" glass
handed your phone a network name that never changed and a password that
changed on every boot — so once your phone had saved it, it kept trying
the old password, the display kept refusing, and the phone just said
"Unable to join the network" without ever asking you for the new one.
Re-flashing made it worse. The password is now minted once and kept, so
what is printed on the glass stays true. A phone that already saved an
old one needs one Forget This Network, and the display now says so on
its own screen if nothing has managed to join for a while.

**The install log stopped overstating itself.** It used to tick off
"Wi-Fi + MQTT settings sealed" whenever anything at all was written — and
for a display, the hub address is filled in for you, so an install with an
EMPTY network field still printed the Wi-Fi claim. You would read the tick,
then watch the board come up asking for Wi-Fi on its own screen, with no
way to tell which had happened. The log now names what actually went in,
and says plainly when no network did.

The network field is still optional for displays — they can be set up on
the glass — so this is about the receipt being honest, not about forcing
you to fill it in.

## 0.11.2 — 2026-08-13

**This Flasher flashes firmware 2.4.9 — the first signed release — and
older Flashers can't.**

The catalog inside the app is baked in when the app is built, and every
Flasher before this one was built before 2.4.9 existed: flashing from an
older Flasher installs 2.4.8 even though newer firmware is out. Update,
then flash each Canary once over USB — 2.4.9 is the release that teaches
your devices to verify update signatures, and a device on 2.4.8 refuses
all over-the-air updates until that one USB flash (that refusal is the
signature protection working, not a fault). After it, updates arrive over
the air.

What 2.4.9 itself brings to a display: touch settings on the 7", the
missing-glyph boxes gone, screen brightness you can set from the phone,
your fleet drawn as its actual hardware, and Wi-Fi auto-join — enter your
network here in the Flasher before flashing and the Canary joins on first
boot with no on-screen login.

## 0.11.1 — 2026-08-07

**Your Canary gets the same name here as it does on your phone.**

A Canary's name isn't assigned, it's derived from the device's own key — so
every SecuraCV app should arrive at the same name for the same bird, with
nothing stored and nothing synced. The Flasher was rolling a name at random
instead: the derived path existed but never received the board's key, so a
Canary you flashed here introduced itself by one name and the iPhone app
called it another. Both are now derived from the fingerprint the board
reports in its own boot receipt.

If you flashed a Canary with 0.11.0 and its name doesn't match what your
phone shows, the phone is right — the board's key never changed, only what
this app did with it. The certificate here now matches.

The "roll again" button is hidden for a derived certificate, on purpose: a
name that comes from the key isn't a name you can re-roll. It stays for
boards that haven't produced a key yet.

## 0.11.0 — 2026-08-07

**Your Canaries can now answer "prove it" — and the app is careful about
what that proves.**

Newer canary-wap firmware can sign a fresh, random challenge with the
identity key it generated on its very first boot. The fleet book asks, and
shows the answer:

- **"identity answered"** — a device holding the key recorded at flash time
  was reachable and signed a challenge it could not have prepared in
  advance.
- **"different key answered"** — something answered for this device's name
  using a *different* identity. That one is worth stopping for.
- Nothing at all, for firmware that predates the feature. Silence here means
  "not asked yet", never "failed".

**What it deliberately does not do is unlock anything.** A neighbor on your
network could forward your challenge to the real Canary and pass its answer
back as their own, so a good answer tells you which key replied — not who
is on the other end of the connection. Treating it as a password would be
worse than not having it, because it would look like proof. So it is shown
and never used to decide anything; the honest limits of local-network trust
are unchanged and written down in the fleet-book documentation.

The first time a device proves itself, its fingerprint is remembered — so if
that ever changes later, you see it.

## 0.10.0 — 2026-08-07

**It now catches a fake flash chip before writing to it.**

Counterfeit and relabeled ESP32 boards are common — a 4 MB chip sold as
16 MB. The nasty part is how quietly they fail: the chip *accepts*
everything written past its real end and throws it away, so the install
reports success and the board simply never boots, with no error anywhere to
explain it.

The app now catches that. During the safety copy it already takes, it checks
whether the chip really holds what it claims, and **refuses the install**
if it doesn't — naming the real size it found. Nothing is written, so
there's nothing to undo.

This costs no extra time at all: the whole chip is already in memory at that
moment, so the check is a comparison rather than another read. It's also
done in the one way that actually works — comparing the start of the chip
against each candidate capacity, not against the far end. (The far end of a
fake part holds *different* data, so the obvious check passes counterfeits.)

On a blank chip the answer is "can't confirm yet" rather than "fine" — a
blank mirror and an honest blank chip look identical, and a check that
couldn't run must never read as a check that passed.

## 0.9.0 — 2026-08-07

**Before it writes anything, the app now tells you what will change.**

The safety copy it already takes holds every byte that's on your board, and
the firmware is verified before a byte is written — so for one moment both
halves exist, and that's where this happens.

- **"Do my settings survive?"** — answered in a sentence, first, because for
  most people it's the only line that matters. Either your saved Wi-Fi,
  device identity and witness-chain counters stay exactly as they are, or
  the board comes back up on its setup network like the first day. No more
  finding out afterward.
- **A region-by-region map** of what's rewritten, what's already identical,
  what's reset to factory-fresh, and what the image doesn't reach at all —
  with the firmware version being replaced named next to the slot it's in.
- **A changed layout is called out**, because rearranged partitions are a
  different kind of install from rewritten ones.

If you skipped the safety copy, or the board wouldn't be read, there's no
map — and the app says nothing rather than guessing.

## 0.8.0 — 2026-08-07

**Tune it for the room before it ever boots.**

- **Room presets, baked in with the firmware.** Camera Canaries (Vision) and
  radar Canaries (Sense) read their detection dials out of the chip's
  settings, so the app now offers the same room presets the web Lab does —
  Entryway, Living room, Hallway, Garage/workshop, Home office and the rest
  — written in the same pass as the firmware. The board comes up already
  behaving right for where it's going, instead of needing a trip through
  Home Assistant first.
- **Nothing is locked in.** These are the very numbers Home Assistant
  retunes live afterward; a preset is a starting point. And **"as it ships"
  writes nothing at all** — the firmware's own defaults stay the answer, so
  a value the maintainer improves in a later release isn't frozen into your
  board by today's flash.

Under the hood this needed a new settings type (a full-width integer): a
dwell of ten minutes doesn't fit in the narrow one, and written narrower the
firmware reads it back as absent and quietly uses its built-in default. Both
flashers now clamp every dial to the same published range, checked
value-for-value against each other.

## 0.7.0 — 2026-08-07

**Two things that used to need a person who knows ESP32s.**

- **A slow cable no longer means "it won't flash."** If the transfer fails
  partway — the board stops answering, a read stalls, the port drops — the
  app now retries at a gentler speed on its own, and again slower after
  that, telling you in the log what it's doing. Long USB runs, unpowered
  hubs and tired cables sync fine at low speed and choke at high speed, and
  that was previously an unexplained failure. It only retries the failures a
  slower speed can actually fix: a bad image or a busy port fails the same
  way at every speed, so those still stop immediately instead of making you
  wait through four attempts for the same answer.
- **The boot log gets read for you.** After a flash, the monitor watches for
  the four failures that have a specific fix and says what happened in plain
  language — a brownout, a startup crash, no bootable app, or a flash read
  error — with the fix beside it. A brownout is called out as a **power**
  problem, not a firmware one: it isn't fixed by reinstalling, and being
  told otherwise sends you around the same loop.

## 0.6.0 — 2026-08-07

**The app now reads your board before it writes to it.**

- **A passport, the moment you plug in.** The connect step says what the
  board is actually running — the firmware, its version, when it was built,
  and which slot it boots from — instead of showing you a chip name and a
  list of images. It reads the slot the bootloader will really run, so a
  board that has updated itself over the air is judged by the firmware it
  runs, not the older copy still sitting in the other slot.
- **Every image now says what installing it would do.** Update, downgrade,
  reinstall, or a change of role, named on the row before you choose it. A
  downgrade is still allowed — that is what the automatic safety copy is
  for — but it is never silent, and switching a board from one product to
  another warns you that settings from the old role don't carry over.
- **"You've flashed this exact board before."** If your fleet book already
  knows the board's MAC, the app says so, with its name and when it was
  last written. Reflashing the wrong one of two identical boards on the
  bench used to be invisible until afterward.
- **What it has lived through.** Lifetime boots, updates seen, witness
  records, whether a crash dump is stored, and whether the tamper flag is
  raised — read-only, from the board itself.

A board whose passport can't be read is still perfectly flashable: the app
says it couldn't look, which is deliberately not the same as saying the
board is blank — and the image rows say "can't tell yet" rather than
claiming a first install. The Install button waits for that read to finish
so a verdict is never skipped, and gives up waiting after a few seconds so
a board that won't be read is never a board that can't be flashed.

## 0.5.0 — 2026-08-07

The browser flasher grew a lot of magic the desktop app never got. This
release closes the first wave of that gap — six capabilities, each one
something the web Lab did for you that this app silently didn't:

- **A safety copy before every flash, unasked.** The whole chip is read to
  a timestamped file in the app's backups folder before anything is
  written — the undo button reflashing never had here. Once per board per
  session; skippable under Advanced for big batches; a copy that can't be
  read warns and continues (a board that won't read is often the one that
  needs rescuing). Restore any copy from Rescue, as always.
- **Failures now name themselves — and their fix.** A busy port says which
  program is probably holding it; a vanished board says "reseat the
  cable"; a bootloader timeout coaches the BOOT/RESET ritual; an
  integrity failure says plainly that nothing was written. Same
  classifications, same remedies as the web Lab.
- **Known USB-serial bridge, missing driver? It says so** — CP210x,
  CH340/CH343, and FTDI boards that won't open now point at the exact
  driver instead of a mystifying nothing.
- **Your Wi-Fi as a QR code** — one click next to the network fields, for
  camera Canaries and phones helping with setup. Rendered locally.
- **The board's API key is shown once after minting** — with a Copy
  button. It's still kept in your Keychain for the fleet book; now you can
  bank it in a password manager too, and a locked Keychain no longer
  means the credential is simply gone.
- **This session's nursery** — a numbered strip of every board hatched
  since the app opened (name, firmware, MAC tail, Wi-Fi-baked and
  key-kept flags), right where the next board gets plugged in. The batch
  flasher's working memory, distinct from the durable fleet book.
- Plus: a one-click **diagnostic report** to your clipboard (nothing is
  sent anywhere), and **help dots** on key controls that read the same
  generated help registry as the web Lab — so the explanation can't drift
  from what the firmware actually does.

## 0.4.1 — 2026-08-07

- **The fleet book now finds your displays.** Three fixes in one: newer
  display firmware announces its real web port instead of the "nothing
  listens here" formality; the app probes displays already in the
  field (which keep the old announcement until updated) and recognizes
  their glass page anyway; and network presence is now sticky — a device
  that misses one scan window on busy Wi-Fi no longer flickers offline.
  Every web-capable device's row also gains an **Open** button straight to
  its own page: the display's live mirror, the wap's dashboard.
- **The Flash-a-Canary page calmed down.** Steps you haven't reached yet
  show as a heading, not a wall of text — each one reveals itself when it
  becomes real. The new-board safety brief folds to one line (every word
  still inside, one click away, and the wipe-on-first-contact default is
  unchanged), and the serial monitor's explainer tucked itself away too.
  Nothing was removed — it just stopped arriving all at once.

## 0.4.0 — 2026-08-06

- **Type your home's setup once — every later flash fills itself in.** The
  Canary form and the hub form now share one setup profile: with the
  "Remember" box ticked (it is, by default), your Wi-Fi and broker passwords
  are kept in your Mac's Keychain (Windows: Credential Manager) and every
  other field in the app's local memory, so a reflash — or the next board —
  is plug in, pick firmware, flash. The consent note under the form names
  exactly where each value goes, and "Reset the app's memory" on the About
  page sweeps the Keychain entries too.
- **The hub can finish its own setup even after you quit the app.** The
  Home Assistant login you type at flash time now survives a relaunch (in
  the same Keychain, only with "Remember these" ticked) — so the app that
  reopens mid-first-boot still creates your account and checks the login
  works, instead of apologizing that the password died with the window.
- **Your Fleet grew a book.** Every Canary you flash is recorded — its
  name, certificate, firmware, and identity — and the Fleet tab now finds
  the real devices on your network by their own mDNS announcements: who's
  online, who hasn't been seen, and who's running behind the app's firmware
  train. Devices someone else flashed show up too, one click to add.
- **One-click over-the-air updates, end to end.** Flashing a canary/wap
  board now mints that board's local-API key, seeds it into the board's own
  settings, and keeps it in your Keychain — so when the book shows "update
  available", one click asks the board to update ITSELF: the device
  downloads the signed release, verifies it against its pinned key, and
  A/B-swaps with automatic rollback. The app never pushes bytes — it rings
  a bell only your computer holds the key to. (The browser flasher seeds
  the same key and shows it once on its done card, so both flashers
  provision identical boards.)
- **No more twin names.** After a successful flash the Canary-name field
  clears and the next board gets a fresh suggestion — two boards can no
  longer inherit the same identity (and the same MQTT topics) from a
  remembered field. The name you gave lives on in the fleet book and on
  the certificate.

## 0.3.12 — 2026-08-05

- **The Canary Nightlight is in the picker.** The pocket ESP32-C3-LCD-1.47
  board's kid-facing firmware — a 7-segment bedside clock over a soft lamp
  (warm orange, Rainbow, Moonbeam white), with the living canary keeping the
  household's rhythm — flashes right from the app: pick it, plug in the
  board, done. The lamp never encodes "safe," and the panel caps its own
  backlight power at 50% in firmware (a heat budget for the closed pocket
  case) — the app's Nightlight card says so too.
- **The catalog now rides the 2.4.6 firmware train.** The stable channel
  pins the release that actually carries the Nightlight images; the dev
  channel toggle keeps reading the rolling prerelease as before.

## 0.3.11 — 2026-08-05

- **The serial monitor now connects by itself after a flash — no more
  unplugging and replugging the board.** Three things conspired against it.
  A freshly flashed board comes back as a *different* USB device (new port
  path, sometimes a new identity), and the monitor kept waiting for the old
  one; it now recognizes the board in whatever form it returns, and if two
  boards are plugged in it says so instead of guessing — in this app and in
  the browser Lab alike. The flasher's own end-of-flash reset also doesn't
  always take on boards wired straight to the chip's USB — the board would
  sit silently in its bootloader looking dead — so the monitor now reboots
  the board itself, once, right after a flash, and you watch it start from
  the very first line. And if the board still says nothing, the console now
  tells you what's worth trying instead of showing an empty pane.
- **The monitor explains what it is.** A line under the heading says it
  plainly: the board's own voice, live over the USB cable — watch it boot,
  see why something isn't working, press h for the firmware's menu. It's
  read-only unless you type, and closing it changes nothing on the board.
- Fixed a subtle hazard where pressing the board's physical RESET while the
  monitor was attached could land a native-USB board in its bootloader
  instead of starting your firmware.
- **The live preview now starts right after a module flash — no unplugging.**
  Starting the Vision module's live bench right after "Flash & prove" (or
  right after plugging in) often failed with "The module didn't answer AT",
  and the fix was to unplug and replug the cable. The cause: opening the
  module's port jiggles the same signal line the flasher uses as the module's
  reset, so the module was still booting when the app asked its one question —
  and the app only asked once. It now asks patiently while the module comes
  up, and if the module stays quiet, the app pulses the reset line itself —
  the "power-cycle it" advice, automated — before trying again. The error
  only appears when the module truly isn't answering, and its advice now
  starts with checking that the cable is in the module's own USB-C port.
- The browser Lab's bench and its post-flash proof got the same patience, so
  both flashers behave alike.

## 0.3.10 — 2026-08-02

- **"Wait for my Pi" now works on Intel Macs.** Flashing a Raspberry Pi over
  USB-C — the no-card-reader path — failed on every Intel Mac the moment you
  opened the panel, with `could not start rpiboot: Bad CPU type in executable`.
  The download was advertised as universal and the app itself was, but one
  bundled helper (`rpiboot`, Raspberry Pi's official USB-boot tool) had been
  built only for Apple Silicon, so Intel Macs couldn't start it. It is now
  built for both architectures and the release build refuses to publish if
  either one is missing. Apple Silicon Macs are unaffected — nothing about
  that path changes. If you hit this, update and the panel will simply work;
  no need to re-flash or redo anything.
- **A helper that can't run now says so in English.** If any bundled helper
  ever can't start on your machine, the app explains what happened and what to
  do — update, or use a USB card reader in the meantime — instead of showing
  the operating system's raw error number.

## 0.3.9 — 2026-08-01

- **The hub can now set itself up — no monitor, ever.** A new opt-in on Build a
  Hub, "Let this app finish hub setup by itself," puts two more things on the
  card next to your Wi-Fi: the securaCV **self-setup bundle** (the same narrated,
  never-does-a-step-twice installer the guide documents) and a **maintenance
  key** minted on your computer. After first boot, the app's own "watching for
  your hub" step doesn't stop at "It's alive!" — it connects to the hub's
  service console over your network and installs the whole stack itself: the
  Mosquitto broker, the MQTT connection Home Assistant needs to actually *use*
  the broker, Frigate, and the securaCV witness kernel, each step narrated in
  the console so you can watch your house being configured and know why.
  Experimental until it's been proven on a real first boot — everything is
  verified up to the card (the bundle rides the same read-back verification as
  the Wi-Fi), and the fallback is exactly the by-hand setup the guide already
  walks; a stumble is a note, never a failed flash.
- **Pi-hole, offered honestly.** Self-setup can also install Pi-hole — the
  widely-used open-source DNS service — as a recommended companion, on by
  default in the panel and skippable with one untick. The pitch is deliberately
  not "ad blocking": securaCV's promise is devices that don't talk out, and
  Pi-hole's local query log is how you *check* that promise instead of taking
  our word — every domain every device (Canaries included) asks for, on one
  page that never leaves your house. The panel is equally plain about the
  trade: to say *which* device asked, Pi-hole logs client IP + domain + time
  (never page contents), that log stays on the hub, and retention is yours to
  shorten or switch off. It installs idle and does nothing until you point
  your router's DNS at the hub; the app says exactly that.
- **Quit mid-first-boot and nothing is lost.** The resume banner now remembers
  when a hub still needs its self-setup run, and picks it up the moment the hub
  answers — with a "Run self-setup again" button if a run stops partway (safe:
  it never repeats a finished step).
- **Honest about the key.** The maintenance key's private half stays in the
  app's data folder and never leaves your computer; only the public half goes
  on the card, and deleting `authorized_keys` from the card's boot partition
  revokes it. And if the hub's identity ever differs from the one seen last
  time, the app **stops and tells you** rather than trusting the new one: after
  re-flashing that's expected, but the same signal appears if something else on
  your network answered to that address, so re-pairing is your call, not a
  silent default.

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
  - **An interrupted update.** Installing an update moves the app itself, so
    being stopped part-way can leave a copy that won't open at all. That one
    only a reinstall can fix — the repair would have to run from the copy that
    moved — so the Flasher now recognizes it and tells you, instead of leaving
    you guessing. It only says so when the update genuinely didn't land: if
    you're already running the version that was being installed, it stays
    quiet. And quitting during an install is now refused outright, whether you
    close the window or press ⌘Q.
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
