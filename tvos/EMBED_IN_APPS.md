# Embedding the Witness Wall in the Flasher and the Lab

The emulator is deliberately a **single, self-contained web view** — one HTML
page + `js/tv-emulator.js` + `js/highlight.js` + `demo-fleet.json`, no build
step, no framework, CSP-safe (`script-src 'self'`). That makes it a drop-in for
both Tauri apps, which are already web-frontend shells.

> **Status: BOTH apps wired — Flasher and Lab — from one canonical emulator,
> with always-on native LAN discovery; web layer verified headless.**
> One command re-vendors every copy from the website checkout
> (`scripts/vendor_witness_emulator.sh`), CI keeps the two app copies
> byte-identical (`scripts/check_witness_emulator_sync.sh` +
> `desktop_parity.test.js`'s witness-wall gate), and both apps discover the
> LAN fleet through the same `witness_discover` Rust command — the Flasher on
> Fleet-tab open *and* post-flash, the Lab on its Witness Wall bench
> (`canary-local/witness-wall.html`, in the build-line manifest). Profiles are
> host-selectable (`?profile=home|business|apartment` / `witness:profile`), so
> each surface boots the wall into its use case and a discovered fleet updates
> that view in place. Verified headless: profile boot, no home-yank on a
> pinned view, live business zones + attention counts, mock-kernel connect,
> XSS discipline, and the vendored copy behaving identically. The remaining
> gate is a real **macOS/Linux Tauri build/run** of each app (per
> [`../CLAUDE.md`](../CLAUDE.md) / `RELEASE_LESSONS`) — that can't be done
> from CI or a browser (the Lab's Rust side does pass `cargo check`).

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

## Real LAN discovery (native, not simulated)

The Flasher is a **native app**, so — unlike the sandboxed browser page — it can
reach the LAN. Discovery runs in TWO modes, one controller
(`witnessDiscovery` in `desktop/src/app.js`): opening the **Your Fleet tab**
starts a continuous scan (every 10 s while the tab is open, with an honest
"scanning… / ● Live" status line above the wall), and a **successful flash**
triggers a fast 30 s burst with the new device highlighted — the Flasher just
provisioned the device's Wi-Fi, so it knows the Canary is about to join the
same network. The post-flash appear can therefore be **real**:

1. `onFlash` fires `announceToWitness()` (instant, simulated) so the wall reacts
   immediately, then `discoverAndPopulate()`.
2. `discoverAndPopulate()` polls the Rust command **`witness_discover(bases)`**
   for ~30s (the board takes a moment to boot + join Wi-Fi). Candidate bases are
   built from the provisioned host (the MQTT/HA host field) plus `canary.local`.
3. **`witness_discover`** (native, `reqwest`) GETs `{base}/api/fleet` on each
   candidate and returns the first that answers. No CSP applies (native), and
   `.local` resolves via the OS — **no mDNS crate**. It reuses the existing
   `reqwest` dependency and mirrors `fetch_manifest`.
4. The frontend posts `{type:'witness:fleet', fleet, highlight}` to the iframe;
   the emulator swaps the demo fleet for the **real** Canaries and highlights the
   one you just flashed.

Every step is wrapped so it can **never** affect the flash flow; if nothing
answers (older build, or nothing serving `/api/fleet` yet), the simulated
appearance stands.

> **Real devices now populate:** the firmware answers `GET /api/fleet` — both
> `canary-wap` (ESP-IDF `esp_http_server`) and `canary-display` (Arduino
> `WebServer`) serve the coarse presence/health contract from one shared,
> host-tested builder (the parity core in
> `firmware/common/fleet_selfreport/`; see
> [`discovery/DISCOVERY.md`](discovery/DISCOVERY.md) and
> [`../docs/FLEET_PARITY.md`](../docs/FLEET_PARITY.md)). So a just-flashed
> `canary-wap` at `canary.local/api/fleet` is exactly what `witness_discover`
> finds and populates the wall from. If a board on the LAN doesn't serve the
> route yet (older build), `witness_discover` simply finds nothing and the
> simulated appear stands. Verifying the native command still needs a
> macOS/Tauri build.

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

**Built.** The website emulator is canonical;
`scripts/vendor_witness_emulator.sh` stamps it into every app surface in one
pass (`desktop/src/witness/` for the Flasher, `canary-local/witness/` for the
Lab + browser Lab) with one deterministic transform (site chrome stripped,
script path flattened, root-relative links absolutized) and a PROVENANCE
recording the source sha256s. Two gates keep it honest:
`scripts/check_witness_emulator_sync.sh` (byte-compare, wired into
`canary-local.yml`) and the witness-wall test in
`canary-local/tests/desktop_parity.test.js` (same discovery command in both
Rust shells, same postMessage contract in both hosts, same emulator bytes).
Fix a bug once — on the website — re-run the script, and every app gets it.

## Verify before shipping

- Build each app locally (macOS + Tauri) and confirm the view loads, the focus
  engine + remote work with a keyboard, and skins/connect behave.
- Confirm the Flasher's post-flash registration reaches the kernel and the
  device appears in the embedded view.
- Only then add it to the app release workflows (read
  [`../.github/RELEASE_LESSONS.md`](../.github/RELEASE_LESSONS.md) first — the
  `cp -RL` rule applies to these bundled web assets too).
