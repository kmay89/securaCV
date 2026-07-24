# Embedding the Witness Wall in the Flasher and the Lab

The emulator is deliberately a **single, self-contained web view** — one HTML
page + `js/tv-emulator.js` + `js/highlight.js` + `demo-fleet.json`, no build
step, no framework, CSP-safe (`script-src 'self'`). That makes it a drop-in for
both Tauri apps, which are already web-frontend shells.

> **Status: plan, not yet wired.** This touches the app frontends and their
> build/release flows, so per [`../CLAUDE.md`](../CLAUDE.md) it needs a real
> **macOS + Tauri** build to verify before shipping. Everything below is the
> exact integration; give the go-ahead and it gets wired and built.

## The Flasher — the magic moment

**Goal:** after the Flasher writes firmware to a Canary, that Canary *appears*
on the Witness Wall view inside the app — the "flash it and watch it show up"
payoff. The emulator already has the receiving half:

- It polls a connected kernel's `GET /api/fleet` and **animates a newly-seen
  device in** (`appearDevice()` + the `.tile.just` join animation), and exposes
  `simulateFlash()` for a no-hardware demo.

The Flasher wires the sending half:

1. Bundle the emulator page as a view/route in `desktop/` (the Flasher
   frontend) — e.g. a "Fleet" tab that loads `witness-wall.html`.
2. The Flasher already knows the device it just flashed (port, product,
   chosen name). On a successful flash it **registers that device with the
   local kernel/hub** (or, hub-less, has the device advertise `canary.local`
   with the `/api/fleet` route from [`discovery/DISCOVERY.md`](discovery/DISCOVERY.md)).
3. The embedded emulator — pointed at that kernel (or the device) — picks the
   new Canary up on its next poll and plays the join animation. Because the app
   is native (not a sandboxed browser tab), it can reach the LAN device
   directly and even do the mDNS discovery the public web page can't.

Fallback with no kernel yet: the Flasher can call the emulator's
`simulateFlash()`-style hook with the real device name so the appearance still
plays immediately off the flash result.

## The Lab

The Lab (`desktop-lab/`, frontend = `canary-local`) is the "explore the whole
line" app. Add the Witness Wall as another surface next to the existing
benches — the same page, reached from the Lab's nav — so the Lab gains the
big-screen witness view and the live fleet connect. Because the Lab reuses one
frontend across web/desktop/mobile, adding it here also carries it everywhere
the Lab ships.

## What's shared, so it can't drift

Keep **one** copy of the emulator assets and have both apps reference it (a
shared folder or a build step that copies `witness-wall.html` + the two JS
modules + `demo-fleet.json` into each app's frontend), mirroring the Lab's
"one frontend, N platforms" rule. Fix a bug once, both apps get it.

## Verify before shipping

- Build each app locally (macOS + Tauri) and confirm the view loads, the focus
  engine + remote work with a keyboard, and skins/connect behave.
- Confirm the Flasher's post-flash registration reaches the kernel and the
  device appears in the embedded view.
- Only then add it to the app release workflows (read
  [`../.github/RELEASE_LESSONS.md`](../.github/RELEASE_LESSONS.md) first — the
  `cp -RL` rule applies to these bundled web assets too).
