# Launch & Promotion Kit

Ready-to-post copy for announcing SecuraCV to its target communities. Each post is written
for that community's norms — adjust details, don't pad. **Fire these when v1 tags**, not
before: every post below links the 5-minute install, and first impressions don't repeat.

General rules that hold across all channels:

- **Lead with the hook** ("a security camera with a 24-hour memory"), not the architecture.
- **Never overclaim.** The honest-limits docs ([root paradox](../root_paradox.md),
  [threat model](../../spec/threat_model.md)) are a *selling point* to this audience — link
  them proactively when questions get sharp.
- **Reply fast on day one.** The first six hours of comments decide how a post ranks.
- One channel per day, not all at once — each post should link the repo, and a sudden
  cluster of cross-posts reads as spam to moderators.

---

## 1. Home Assistant Community forum (primary audience)

**Category:** Share your Projects! · **Tags:** camera, frigate, mqtt, privacy

**Title:** SecuraCV — a security camera with a 24-hour memory (Frigate + tamper-evident
event log, no subscription)

> I built an add-on that gives your cameras a deliberately short memory.
>
> **The idea:** clips stay on your hardware and auto-delete after 24 hours (you choose the
> retention). The only thing that persists is a cryptographically signed, hash-chained log
> of *what happened* — "a large object crossed the boundary at night" — with no faces, no
> plates, no precise timestamps. If anyone alters that log — including me, including you —
> verification fails. Proof without a surveillance archive.
>
> **What it is in practice:**
> - An add-on that wraps **Frigate** (your clips keep recording as usual)
> - Witness sensors, a chain-integrity sensor, and a **Verify Now** button via MQTT Discovery
> - A Lovelace timeline card where ✓ means an Ed25519 signature actually verified
> - A daily-digest push every morning; pattern alerts for unusual-hour activity
> - No cloud, no account, no subscription — Apache-2.0
>
> **Install** is 3 steps from the add-on store (wizard auto-discovers your Mosquitto broker):
> https://github.com/kmay89/securaCV
>
> **What it's not:** it doesn't replace Frigate, and it can't make tampering *impossible* —
> only *evident*. The threat model and its limits are documented in the repo, and I'd
> genuinely rather you read those and poke holes than take the pitch at face value.
>
> Runs on a Pi 4 (4 GB) with ~3 cameras at 10 fps. Feedback, bug reports, and skepticism
> all welcome.

---

## 2. r/selfhosted

**Title:** I built a self-hosted security camera system that deletes its own footage —
what persists is a tamper-evident log that proves nobody edited the record

> Every camera product I looked at kept footage *forever* (cloud subscription) or kept it
> *until the disk filled* (NVR). Both build a surveillance archive of everyone who walks
> past — family included. I wanted the opposite: short memory, strong proof.
>
> SecuraCV runs alongside Frigate. Clips auto-delete after 24 hours. Every detection becomes
> a semantic event ("large object crossed the boundary") in an Ed25519-signed, hash-chained
> append-only log — no faces, no plates, no precise timestamps, enforced in code rather than
> promised in a privacy policy. Alter the log and verification fails. Sensitive events are
> sealed behind a multi-party "break glass" so even *you* can't casually browse them.
>
> - Docker compose quickstart (one env var to edit) or Home Assistant add-on
> - Hardware: Pi 4 / any x86 box + RTSP cameras you already own
> - No cloud, no account, no telemetry, no subscription. Apache-2.0.
> - Honest limits documented: a root operator on the host is outside the trust boundary
>   (the repo calls this "the root paradox" and explains exactly what is and isn't covered)
>
> Repo: https://github.com/kmay89/securaCV
>
> Target user is anyone who wants cameras without becoming a tiny surveillance company —
> and especially people who may someday need a record that holds up: tenants, journalists,
> anyone whose word might be doubted.

---

## 3. Hacker News (Show HN)

**Title:** Show HN: A security camera with a 24-hour memory and a tamper-evident log

**URL:** https://github.com/kmay89/securaCV

**First comment (post immediately after submitting):**

> Author here. The premise: surveillance archives are a liability, but the *ability to
> prove what happened* is genuinely valuable. SecuraCV separates the two.
>
> Camera clips stay local and auto-delete after 24h. What persists is an append-only log
> of semantic claims ("BoundaryCrossingObjectLarge", bucketed timestamps) — each entry
> Ed25519-signed and hash-chained, so any alteration breaks verification. Seven privacy
> invariants (no faces, no plates, no raw frames in the log, ...) are enforced in the
> kernel code, with the spec in the repo. Sensitive events are sealed into encrypted
> envelopes that need multi-party authorization to open.
>
> It's a Rust kernel that wraps Frigate for ingest/detection, plus a Home Assistant
> integration and optional ESP32 "Canary" sensor firmware, each device keeping its own
> signed chain verified against a pinned key.
>
> Known limits, documented in the repo: tamper-*evidence* not tamper-*proofing*; a root
> operator on the host is outside the boundary; on-device hardware validation is the last
> gate before v1. Happy to answer anything about the invariant design, the break-glass
> ceremony, or why timestamps are deliberately imprecise.

**HN survival notes:** submit on a weekday morning US time; never edit the title to add
hype; answer the inevitable "why not just chain-of-custody a normal NVR?" with the
invariants doc rather than adjectives.

---

## 4. Short-form (Mastodon / X / Discord)

> Your security camera doesn't need to remember everyone forever to protect you.
>
> SecuraCV: clips auto-delete in 24h. What persists is a signed, hash-chained log that
> proves what happened — and proves nobody edited it. No cloud. No subscription.
> Frigate + Home Assistant + Rust. Apache-2.0.
>
> https://github.com/kmay89/securaCV

(Fits with room for one screenshot — use the verified-✓ timeline once it exists; until
then, the social preview image in `docs/social_preview.png`.)

---

## 5. Directory listings (one-time, compounding)

- **HACS default repository** — submit once stable; HACS users discover integrations
  in-app far more often than via GitHub search. (`brands/submission/` is already prepared.)
- **awesome-selfhosted** (PR under *Video Surveillance*), **awesome-home-assistant** — one
  PR each, evergreen traffic.
- **Frigate community discussions** — a "works with Frigate" post; Frigate's user base is
  exactly the "Start here" segment in the README.

---

## After every launch post

- Pin a "Welcome — start here" discussion in the repo so arrivals land somewhere warm.
- Watch the issue tracker: first-day installers finding bugs is the *good* outcome —
  every fast, friendly fix is visible marketing to everyone who reads the thread after.
