# Brand & positioning — the Canary

Canonical positioning for SecuraCV. If a name, tagline, or claim anywhere —
site copy, device strings, README, a pitch — disagrees with this file, this
file wins. Keep it short; keep it true.

---

## The mission (one word)

**Trust.** Not "trust us" — the engineered kind: **you don't have to.**
Every promise is structural, *can't* rather than *won't*: there is no
identity code to switch on, the log can't be edited without the edit
showing, there is no account and no telemetry to hand over, and sealed
evidence takes more than one person to open — us included. Where a
guarantee can't be structural yet, we say so out loud (the
"what still has to be true" section below exists for exactly that).
The test for any claim, feature, or sentence of copy: does it *show*
proof, or does it *ask for* faith? Anything that asks for faith is
off-mission, full stop.

## The one thing to remember

**The name on the door is SecuraCV, and what it is, is a *witness*.** The
thing you can point at is a **Canary**.

- **SecuraCV** — the witness: open-source software for the cameras and
  sensors you already own, and the platform a Canary runs.
- **Canary** — the device, and the name people say out loud for one.
- **Errer Labs** — who makes it. The legal entity behind the marks.

Say "a Canary," "your fleet of Canaries," "Works with SecuraCV." Lead with
what it does, and don't make a newcomer learn "witness kernel" before they
understand what they're getting.

Until 2026-11 this section said the hero noun was the Canary. Three things
moved it; [strategy doc 32](strategy/32-one-story-every-surface.md) is the
full account.

1. **What ships today is software.** The Home Assistant app and integration
   are released, they are how the project reaches people first, and they
   involve no Canary at all. A front page about a device nobody can buy yet
   hid the product somebody can install tonight.
2. **The word is not ours to lead with.** Others hold trademark rights in
   "CANARY" for security cameras, and [`TRADEMARK.md`](../TRADEMARK.md)
   already says we use it only in combination, as "SecuraCV Canary." The
   open item (W15 in the 2026-09 sweep) offered three ways out; this is the
   one that needs no rename: build the public hierarchy around *witness*,
   and keep *Canary* as the product's own name. The clearance question
   itself is still the owner's to settle.
3. **A witness is not always a camera.** The WAP senses a Wi-Fi field, the
   Sense is radar, and beside Frigate the kernel keeps no video at all.

## The promise (plain words, for anyone)

One ladder, used on every surface — the site, the READMEs, the store
listings, a pitch. A surface picks the rung that fits its room; it does not
write a sixth.

| Rung | Words | Where it goes |
| --- | --- | --- |
| Signature | **Witnessing without watching.** | under the name, anywhere |
| Hook | **Records what happened. Never who.** | the home page headline, the share card |
| The sentence | SecuraCV is an open-source witness for the cameras and sensors you already own: it keeps a signed, tamper-evident record of what happened — never a face, a plate or a cloud archive. | the first paragraph, anywhere |
| For Home Assistant | **Keep Frigate. Add proof.** | the Home Assistant page, the store listings |
| The explanation | Three things, one witness: it sees, it proves, it protects. | the long form |

The earlier lead, "A security camera that remembers what happened — and
can't rat you out," is retired. It called every witness a camera, it led
with a promise a sentence can't show, and it never reached the home page:
the site, this file and the launch kit each carried a different one. (This
table used to say "five taglines = zero" about other companies.)

The honest edge, when someone asks "how do I know it wasn't tampered with?":

> The promise is not that tampering is impossible. It's that tampering
> becomes **visible**.

## Who it's for (all four, deliberately, in the order they arrive)

Every guide page already speaks "to an imaginative ten-year-old and a
security-grade maker in the same breath." Hold that.

1. **People who already run cameras** — a Home Assistant and Frigate
   household. They install software, today, and need no hardware from us.
   This is who the project reaches first, and until 2026-11 no page was
   theirs.
2. **Makers** — print it, wire it, flash it. The browser Lab and the build
   path are their front door.
3. **Buyers** — power it on, point at a QR code, done. Pre-flashed kits,
   when the store may sell a radio.
4. **The people who can't build their own** — elders, renters, the
   non-technical. The mission is getting witnesses into *their* homes too.

The site's header is that order: Lab, How It Works, Home Assistant, Build,
Store, Help.

## The magic we under-sell

The single most differentiating asset is **the browser Lab**: the *real
shipping firmware, compiled to WebAssembly*, flashing a blank chip and
showing its cryptographic "birth certificate" — live, in a browser, with no
hardware. Nobody else in home security can show that.

Treat it like Figure treats its demo videos: a 60-second "blank chip →
verifiable witness → check its birth certificate" clip is the centerpiece of
every launch, not a buried `/lab` link.

## Naming rules (non-negotiable)

- **The name is "SecuraCV" — capital S, capital CV, exactly that casing.**
  It's an initialism: **Secure Computer Vision**. Prose, UI strings, store
  listings, and legal filings all write `SecuraCV`. The code layer stays
  lowercase `securacv` (domains, package names, bundle IDs, MQTT topics,
  repo slugs) — that's convention, not a third style. Mixed forms like
  `securaCV` in copy are typos to fix on sight; existing paths and repo
  names are grandfathered code-layer, not precedent for prose.
- **Never "flock."** A group of Canaries is a **fleet**. The word is banned
  in all user-facing copy, device strings, product names, identifiers, and
  comments (a company called Flock soured it). The *only* exception is the
  Unix `flock(2)` syscall. Full rule in [`CLAUDE.md`](../CLAUDE.md).
- **Never overclaim trust.** "Verified" means an Ed25519 signature checked
  against a pinned key — nothing looser. The firmware's `fleet_model.h`
  already enforces this in code; copy must match it.
- Product line: **Canary Vision · Canary WAP · Canary Sense · Canary
  Display · Canary OTA**. The platform is **SecuraCV**. The badge anyone may
  earn is **"Works with SecuraCV."**

## Who we learn from (and why they, not the giants)

The instinct to compare against Diebold Nixdorf or Figure AI is understandable
but off-target. Our real role models are open-hardware companies that grew a
community *into* a company without betraying it:

| Peer | What we steal from them |
| --- | --- |
| **Prusa / Bambu** | A content flywheel — build-of-the-week, a public cadence that pulls makers in. (Our `/gallery` is still seed builds.) |
| **Framework** | One crisp promise a normal person repeats. (Five taglines = zero.) This doc fixes that. |
| **Meshtastic / ESPHome** | Our exact trademark model — plus governance that lets strangers contribute safely. |
| **Home Assistant / Nabu Casa** | Our closest business twin: open core, hosted convenience tier, a maintainer hierarchy so no one person is the bottleneck. |
| **Adafruit / SparkFun** | Education *as* the top of the funnel — Learn guides and a contributor spotlight. |

And the cautionary tale: **MakerBot** was born from the maker movement, closed
its source, and the makers defected to Prusa. Our Apache-2.0 + "Works with
SecuraCV" badge is the anti-MakerBot. Never walk it back. That decision is
worth more than any feature.

## What still has to be true before the promise is fully earned

Say these out loud; the brand is honesty-first.

- **Bench validation.** Much of the firmware is proven in CI, not yet on a
  bench. v1 is held until it is. A witness product has to be proven on the
  device.
- **Certification.** Selling the radios legally means FCC/CE. A published
  third-party security audit (Signal/Meshtastic-style) is worth more than any
  feature for a *trust* product.

Until then, the copy stays in the present tense about what's real and the
future tense about what's coming — the fact-tests in CI keep it that way.
