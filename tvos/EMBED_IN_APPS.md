# Embedding the Witness Wall in the Flasher and the Lab

The emulator is deliberately a **single, self-contained web view** — one HTML
page + `js/tv-emulator.js` + `js/highlight.js` + `demo-fleet.json`, no build
step, no framework, CSP-safe (`script-src 'self'`). That makes it a drop-in for
both Tauri apps, which are already web-frontend shells.

> **Status: Flasher — wired; web layer verified. Lab — same pattern, pending.**
> The Flasher integration below is implemented and the web layer is verified in
> a headless browser (the `witness:appear` postMessage makes a just-flashed
> Canary show up on the wall; skins, connect, and the highlighted JSON all
> work inside the app-origin iframe). The remaining gate is a real **macOS +
> Tauri build/run** of the Flasher (per [`../CLAUDE.md`](../CLAUDE.md) /
> `RELEASE_LESSONS`) — that can't be done from CI or a browser. The Lab is the
> identical pattern; wire it the same way once the Flasher build is confirmed.

## As wired in the Flasher (this repo)

- `desktop/src/witness/` — the vendored emulator (`witness.html` +
  `tv-emulator.js` + `highlight.js` + `demo-fleet.json`), self-contained and
  loaded in an isolated `<iframe>`. See `witness/PROVENANCE.txt` for the sync.
- `desktop/src/index.html` — a **Your Fleet** tab (`data-nav="fleet"`) and a
  `#fleet-view` holding `<iframe id="witness-frame" src="witness/witness.html">`.
- `desktop/src/app.js` — `VIEWS` includes `fleet`; `announceToWitness()` posts
  `{type:'witness:appear', name}` to the iframe right after a successful flash
  (`onFlash`), wrapped in try/catch so it can **never** break the flash flow;
  a "new" badge draws attention until you open the tab.
- Works within the app's existing CSP (`default-src 'self'`) — same-origin
  iframe + `demo-fleet.json` fetch; no CSP change. A real LAN kernel needs
  `connect-src` widened (a deliberate, separate change).

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
