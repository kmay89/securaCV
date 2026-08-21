# SecuraCV on Apple TV — the Witness Wall

> Status: **built and continuously tested.** The app lives in
> [`tvos/`](../../tvos/) (SwiftUI Wall + a Rust witness core, built and
> tested on every PR); the App Store upload waits on an Apple Developer
> account — see [`tvos/README.md`](../../tvos/README.md). This doc remains
> the design brief — what it does, why it's the anti-surveillance-wall, and
> how it self-heals, self-publishes, and self-updates so it "never rots,
> works for years" (`docs/design/raspberry_pi_hub_flashing.md`).

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

## 3. Two editions — the Witness Wall (Home) and the Witness Board (Business)

One app, two editions. The home is the gentle case; the sharper one is a
**bar, restaurant, shop, gym, or clinic** — anywhere that already runs cameras
and often already has an Apple TV on the wall for signage, sports, or music.
Their pain was never "watch the feed." It's **prove what happened**, and
**don't get sued for how you proved it** — exactly what a tamper-evident
witness layer is for, and nobody gives it to them free.

The app picks its edition from what the kernel reports (a household vs a venue
profile), the same way the Lab's nav adapts per platform. Same binary, same
core, same tag-only pipeline — so the second edition adds **zero** release
surface to rot.

| | **Home — the Witness Wall** | **Business — the Witness Board** |
|---|---|---|
| The screen's job | Calm ambient reassurance | Prove what happened, defensibly |
| Signature move | "All is well" witness screensaver | **One-tap Incident capture** + **dispute-pack export** |
| Zones | Rooms & doors | Registers, entrances, patio, kitchen line, walk-in |
| Quorum for break-glass | Household members | Managers / owners |
| The daily glance | A quiet digest | A **close-of-night sealed digest** |
| Scale | Your Canaries | A **multi-site health board** for a group |
| Hours | Always calm | **Open-hours vs after-hours** witness postures |

Both editions share the non-negotiables: **local, $0/mo, no cloud**; an
Ed25519-signed, hash-chained record; the *same* Rust verifier as the kernel;
and — the hook that makes Business land — **it works with the cameras they
already own.**

### What the Business edition gives a venue (all free)

- **Works with their existing cameras.** SecuraCV ingests RTSP / ONVIF /
  Frigate — *basically any IP camera or NVR* already on the wall (Hikvision,
  Reolink, Lorex, Ubiquiti, Amcrest, the lot), auto-discovered over ONVIF on
  the LAN. A witness layer laid **over the cameras they already bought** — no
  rip-and-replace, no per-camera cloud subscription.
- **Dispute-proof record.** Chargebacks, slip-and-fall claims, "your bartender
  overserved me," a walk-out, a fight at last call — each a semantic event,
  signed and hash-chained. A **court-ready timeline edits can't touch** — the
  evidentiary value venues pay four figures a month for, with no monthly bill.
- **One-tap Incident capture.** Something just happened at table six: a single
  press seals a bookmarked evidence window around that moment for later
  quorum-gated break-glass. The staff action is one button; the accountability
  is automatic.
- **Dispute-pack export.** Generate a verifiable bundle — the signed timeline
  plus the sealed clip (via break-glass) — to hand an insurer, a card
  processor, or an officer. It verifies offline, on their machine, without
  trusting us.
- **A liability shield, not a face archive.** The record is *events* — "motion
  at register 2 · 11:47 pm · sealed" — never a biometric archive of every
  customer. In a BIPA / GDPR / CCPA world that is a real exposure reducer, and
  the app carries a plain "what we do and don't record" screen you can show an
  inspector or paste into a privacy notice: *we witness, we don't surveil our
  customers* (`spec/invariants.md`, Invariant I).
- **The calm board behind the bar.** Not a grid staff learn to ignore — every
  camera's *health* and the verified timeline, ambient and readable, with a
  **close-of-night sealed digest** a manager glances at on the way out.
- **One screen for a group.** A small chain sees every location's health and
  chain integrity on a single multi-site board — all green, or exactly which
  site needs a look.

### What the Home edition keeps gentle

- The **ambient witness screensaver** — the fleet breathing, the chain growing,
  *all is well* as something you'd leave on.
- **Household break-glass** — the family approves an unseal together, in the
  open, on the shared screen.
- **Gentle Doctor cards** and a quiet daily digest — "your Canaries," personal
  and calm, never alarming.

Neither edition asks anyone to trust a cloud, feed a subscription, or become the
thing they'd be liable for. Both run local, on hardware most people already have.

## 4. How it's built — reuse the core, don't rebuild it

This is the same discipline as the hub doc's *"inherit it, don't rebuild it."*

- **Native SwiftUI**, because tvOS is a native, focus-engine, remote-driven
  platform — a webview wrapped in Tauri (how desktop/iOS ship) would fight the
  remote and the ambient aesthetic. This is the one platform where native is
  the honest choice.
- **The same chain math, proven against the kernel.** The kernel's own Rust
  verifier cannot compile for Apple TV (it is welded to rusqlite/sqlcipher and
  GStreamer), so `tvos/witness-core/` is a small standalone crate that
  re-implements the kernel's *pinned bytes* — the same precedent as the
  offline JavaScript verifier — compiled for `aarch64-apple-tvos` and called
  from Swift over a thin FFI. CI proves it against the kernel's own
  domain-separation fixtures on every PR and again on the release path
  (`tvos/witness-core/tests/vectors.rs`), so "the TV disagrees with the
  kernel" is a failure CI catches before it ships, not one prevented by
  sharing a binary. `tvos/scripts/build-witness-core.sh` builds it.
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

Want to try it on hardware? The fast, low-headache runbook — AirPlay demo,
on-device Xcode build with a free Apple ID, then TestFlight — is
[`tvos/RUN_ON_APPLE_TV.md`](../../tvos/RUN_ON_APPLE_TV.md).

---

Related reading: [`spec/invariants.md`](../../spec/invariants.md) ·
[`docs/RELEASE_PROCESS.md`](../RELEASE_PROCESS.md) ·
[`docs/firmware_ota.md`](../firmware_ota.md) ·
[`desktop-lab/MOBILE.md`](../../desktop-lab/MOBILE.md)

## Trademarks

Apple, Apple TV, tvOS, Siri, Mac, iPhone, iPad, HomeKit, AirPlay, Xcode, and
TestFlight are trademarks of Apple Inc., registered in the U.S. and other
countries and regions. App Store and App Store Connect are service marks of
Apple Inc. SecuraCV is an independent project by Errer Labs and is **not
affiliated with, endorsed, sponsored, or certified by Apple Inc.** References to
Apple products are nominative — for identification and interoperability only.
