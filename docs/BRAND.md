# Brand & positioning — the Canary

Canonical positioning for SecuraCV. If a name, tagline, or claim anywhere —
site copy, device strings, README, a pitch — disagrees with this file, this
file wins. Keep it short; keep it true.

---

## The one thing to remember

**The hero noun is the *Canary*.** A person can't repeat a system, but they
can point at a thing. The Canary is the thing.

- **Canary** — the device, and the brand people say out loud.
- **SecuraCV** — the platform the Canary runs on (the witness kernel + the
  signed log). It's the "how," not the "what."
- **Errer Labs** — who makes it. The legal entity behind the marks.

Say "a Canary," "your fleet of Canaries," "Works with SecuraCV." Don't make a
newcomer learn "witness kernel" before they understand what they're buying.

## The promise (plain words, for anyone)

> **A security camera that remembers what happened — and can't rat you out.**

That is the homepage sentence. It tells a normal buyer *why they'd want one*
in language they'll repeat to a friend. Everything else is proof of it.

Poetic subtitle (keep it — it's earned):

> **Witnessing without watching.**

The honest edge, when someone asks "how do I know it wasn't tampered with?":

> The promise is not that tampering is impossible. It's that tampering
> becomes **visible**.

## Who it's for (all three, deliberately)

Every guide page already speaks "to an imaginative ten-year-old and a
security-grade maker in the same breath." Hold that.

1. **Makers** — print it, wire it, flash it. The browser Lab and Maker Corps
   are their front door.
2. **Buyers** — power it on, point at a QR code, done. Pre-flashed kits.
3. **The people who can't build their own** — elders, renters, the
   non-technical. The mission is getting witnesses into *their* homes too.

## The magic we under-sell

The single most differentiating asset is **the browser Lab**: the *real
shipping firmware, compiled to WebAssembly*, flashing a blank chip and
showing its cryptographic "birth certificate" — live, in a browser, with no
hardware. Nobody else in home security can show that.

Treat it like Figure treats its demo videos: a 60-second "blank chip →
verifiable witness → check its birth certificate" clip is the centerpiece of
every launch, not a buried `/lab` link.

## Naming rules (non-negotiable)

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

- **Bench validation.** Much of the firmware is CI-verified, not yet
  hardware-verified. v1 is held until it is. A witness product has to be
  proven on the device.
- **Certification.** Selling the radios legally means FCC/CE. A published
  third-party security audit (Signal/Meshtastic-style) is worth more than any
  feature for a *trust* product.

Until then, the copy stays in the present tense about what's real and the
future tense about what's coming — the fact-tests in CI keep it that way.
