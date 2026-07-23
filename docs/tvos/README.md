# SecuraCV on Apple TV — the Witness Wall

> Status: **design, greenfield.** No tvOS code exists yet. This is the plan —
> what it does, why it's the anti-surveillance-wall, and how it self-heals,
> self-publishes, and self-updates so it "never rots, works for years"
> (`docs/design/raspberry_pi_hub_flashing.md`).

Witnessing without watching — on the biggest screen in the house, or behind
the bar.

---

## 1. What everyone else built, and why we don't copy it

Open any security-camera app on Apple TV today — Ring, Nest (via the Home
app), Eufy, Wyze, HomeKit Secure Video — and you get the same object: **a grid
of live video feeds**, with motion alerts that pop over whatever you were
watching. The Apple TV is treated as a free NVR monitor. The mental model is
*always be looking*.

That model is exactly what SecuraCV exists to refuse. Raw media never leaves
the hardware (`spec/invariants.md`, Invariant I — "No Raw Export by Design").
A wall of faces on the living-room TV is the surveillance archive we tell
people we won't build. So the Apple TV app can't be a video wall. It has to be
the opposite.

## 2. Our angle: the Witness Wall

The Apple TV is the one screen in the home that is **always on, ambient, shared,
and large** — the same qualities the README already asks of our display
surfaces: *"readable instead of alarming."* We lean all the way into that.

The Witness Wall shows **the verified record, not the footage**:

- **Your fleet, breathing.** Each Canary is a calm tile — online, healthy,
  chain intact — not a video pane. Presence and trust, at a glance, from
  across the room. (Never a "flock." Always the *fleet*.)
- **The verified timeline.** `motion in Zone A · 3:14pm · sealed & verified ✓`.
  Semantic events from the Privacy Witness Kernel, rendered as plain language.
  The Ed25519 hash-chain is checked as it scrolls; a tampered or missing link
  shows up *here*, on the shared screen, not buried in a log.
- **Chain health as ambient calm.** A quiet, ever-present "verified through
  4:02 pm" heartbeat. The reassurance the whole product is built to give,
  finally sitting where the household can see it.
- **Ambient mode (the magic).** When idle, the Apple TV becomes a witness
  screensaver — the fleet slowly breathing, the chain growing link by link,
  *all is well* rendered as something you'd actually leave on. Apple's aerials,
  but it's your home telling you it's safe. Security that feels like calm
  instead of anxiety. It never paints stale data as fresh: if it can't verify
  right now, it says so, calmly, with a timestamp.
- **Break-glass, in the open.** Unsealing evidence needs a quorum
  (`spec/invariants.md`). On a phone that's a hidden tap. On the shared TV it
  becomes what it should be: a request that appears where the household sees
  it, and members approve together, on the record. The most serious action in
  the system, done in the most accountable place.
- **A Doctor card, not a dead pixel.** An offline Canary or a chain gap turns
  into one gentle, actionable card — mirroring the browser flasher's
  "never get stuck" self-healing (`docs/browser_flasher.md`).

The Apple TV is also, conveniently, a device that is *already in the home and
already always-on* — so it can double as the local, no-extra-hardware surface
for a household that doesn't want a wall-mounted display.

## 3. Where it earns its keep — homes, and the venues that already have cameras

A home is the gentle case. The sharper one is a **bar, a restaurant, a shop, a
gym, a clinic** — anywhere that already runs cameras and already has an Apple TV
on the wall for signage, sports, or music. For them the pain was never "watch
the feed." It's **prove what happened**, and **don't get sued for how you
proved it**. That is exactly what a tamper-evident witness layer is for, and
nobody is giving it to them for free.

The Witness Wall grows a **Venue mode** — same app, same core, tuned for the
back-of-house screen:

- **Works with the cameras they already bought.** SecuraCV ingests RTSP / ONVIF
  / Frigate, which is to say *basically any IP camera or NVR* already on the
  wall — Hikvision, Reolink, Lorex, Ubiquiti, Amcrest, the lot. No
  rip-and-replace, no per-camera cloud subscription. The magic is a witness
  layer laid **over the cameras they already own**, not a new camera to buy.
- **A dispute-proof record, for free.** Chargebacks, slip-and-fall claims,
  "your bartender overserved me," a walk-out, a fight at last call — each is a
  semantic event, Ed25519-signed and hash-chained. When the dispute or the
  insurer or the officer arrives, there is a **court-ready timeline that edits
  can't touch**. That's the evidentiary value venues pay four figures a month
  for — with no monthly bill.
- **A liability shield, not a face archive.** The record is *events* —
  "motion at register 2 · 11:47 pm · sealed" — never a biometric archive of
  every customer's face. In a BIPA / GDPR / CCPA world that is a genuine
  exposure reducer: a venue can honestly say *we witness, we don't surveil our
  customers* (`spec/invariants.md`, Invariant I).
- **The calm board behind the bar.** Not a stressful grid of feeds staff learn
  to ignore — every camera's *health* and the verified timeline, ambient and
  readable, plus a **close-of-night sealed digest** ("here's what the night
  witnessed") a manager can glance at on the way out.
- **One screen for a group.** A small chain sees every location's health and
  chain integrity on a single multi-site board — all green, or exactly which
  site needs a look.

None of this asks the venue to trust a cloud, feed a subscription, or become the
thing they'd be liable for. It runs local, on hardware they mostly already have.

## 4. How it's built — reuse the core, don't rebuild it

This is the same discipline as the hub doc's *"inherit it, don't rebuild it."*

- **Native SwiftUI**, because tvOS is a native, focus-engine, remote-driven
  platform — a webview wrapped in Tauri (how desktop/iOS ship) would fight the
  remote and the ambient aesthetic. This is the one platform where native is
  the honest choice.
- **The exact same Rust witness core** that verifies the sealed log on desktop,
  in firmware, and in the kernel — compiled for `aarch64-apple-tvos` and called
  from Swift over a thin FFI. The chain math is *byte-identical* everywhere, so
  there is no "the TV disagrees with the kernel" failure mode by construction.
  `tvos/witness-core/` wraps the workspace crate; `tvos/scripts/build-witness-core.sh`
  builds it.
- **Local discovery over Bonjour/mDNS.** The TV finds the kernel on the LAN,
  no account, no cloud, no pairing codes typed on a remote. Reconnects with
  backoff on any network blip (the watchdog posture, borrowed from firmware).
- **Read-only by design.** The app renders the record and can *request*
  break-glass; it never holds authoritative state and never touches raw media.
  Nothing to corrupt, nothing to rot — every launch re-derives everything from
  the signed source and re-verifies it.

## 5. The autopipeline

The magic the Mac and firmware pipelines already have, brought to Apple TV:
**a git tag is the only control surface**, CI does the rest, and the whole thing
is built so it can't quietly rot. Full mapping in
[`AUTOPIPELINE.md`](AUTOPIPELINE.md). The pipeline itself lives in
[`.github/workflows/tvos-release.yml`](../../.github/workflows/tvos-release.yml),
gated to a friendly no-op until you enable it — see [`../../tvos/README.md`](../../tvos/README.md).

---

Related reading: [`spec/invariants.md`](../../spec/invariants.md) ·
[`docs/RELEASE_PROCESS.md`](../RELEASE_PROCESS.md) ·
[`docs/firmware_ota.md`](../firmware_ota.md) ·
[`desktop-lab/MOBILE.md`](../../desktop-lab/MOBILE.md)
