# Launch & Promotion Kit

Ready-to-post copy for announcing SecuraCV to its target communities. Each post is written
for that community's norms — adjust details, don't pad. **Fire these when v1 tags**, not
before: every post below links the install page, and first impressions don't repeat.

> **Rewritten 2026-11.** The first version of this kit was built on a hook — "a security
> camera with a 24-hour memory" — and on the line "clips auto-delete after 24 hours."
> Neither is something the software does: beside Frigate the witness kernel records no
> video at all, Frigate's own retention governs the clips, and the signed log keeps 7 days
> by default (the setup wizard offers 7 days, 30 days or a year). The claims audit retired
> both lines before anything was posted. The posts below use the message ladder in
> [`BRAND.md`](../BRAND.md); the reasoning is
> [strategy doc 32](../strategy/32-one-story-every-surface.md).

General rules that hold across all channels:

- **Lead with the hook** ("Keep Frigate. Add proof." to people who run Frigate; "Records
  what happened. Never who." to everyone else), not the architecture.
- **Never overclaim.** The honest-limits docs ([root paradox](../root_paradox.md),
  [threat model](../../spec/threat_model.md)) are a *selling point* to this audience — link
  them proactively when questions get sharp. No retention promise the code does not make,
  no performance number without a benchmark behind it, and "verified" only for a signature
  checked against a pinned key.
- **Say "app."** Home Assistant renamed add-ons to apps; say so once for readers on an
  older release.
- **Reply fast on day one.** The first six hours of comments decide how a post ranks.
- One channel per day, not all at once — each post should link the repo, and a sudden
  cluster of cross-posts reads as spam to moderators.

---

## 1. Home Assistant Community forum (primary audience)

**Category:** Share your Projects! · **Tags:** camera, frigate, mqtt, privacy

**Title:** SecuraCV — keep Frigate, add proof (a signed, tamper-evident event log, no
subscription)

> I built an app (what used to be called an add-on) that sits beside Frigate and keeps a
> record you can check.
>
> **The idea:** Frigate keeps doing what it does — your clips, your retention. SecuraCV
> turns each detection into a coarse claim — which camera, what kind of thing, which
> ten-minute window — and seals it into a cryptographically signed, hash-chained log. No
> faces, no plates, no identities: that code was never written, so there is no setting to
> turn off. If anyone alters the log — including me, including you — verification fails.
> Proof without a searchable archive of who walked past.
>
> **What it is in practice:**
> - An app that runs **beside Frigate** (it records no video of its own)
> - Witness sensors, a chain-integrity sensor, and a **Verify now** button
> - A timeline card where "Verified ✓" means the integration walked the chain and checked
>   every signature against the key it pinned when you connected
> - One sealed event per camera, kind of thing and ten-minute window — the coarsening is
>   the privacy guarantee, not a bug
> - No cloud, no account, no subscription — Apache-2.0
>
> **Install** is two buttons from Home Assistant, nothing to type; there is a one-command
> path and a Docker path too: https://securacv.com/home-assistant
>
> **What it's not:** it doesn't replace Frigate, and it can't make tampering *impossible* —
> only *evident*. The integration is a custom HACS repository today, not in the default
> catalog. The threat model and its limits are in the repo, and I'd genuinely rather you
> read those and poke holes than take the pitch at face value.
>
> Needs Home Assistant 2026.5.0 or newer, an MQTT broker, and Frigate 0.14 or newer.
> Feedback, bug reports, and skepticism all welcome.

---

## 2. r/selfhosted

**Title:** I built a self-hosted witness for the cameras you already run — a signed,
tamper-evident log of what happened, with no identity archive

> Every camera product I looked at kept footage *forever* (cloud subscription) or kept it
> *until the disk filled* (NVR). Both build an archive of everyone who walks past — family
> included — and neither can show that the record wasn't edited. I wanted the opposite
> trade: say less, prove more.
>
> SecuraCV runs alongside Frigate. Every detection becomes a semantic event ("large object
> crossed the boundary") in an Ed25519-signed, hash-chained append-only log — no faces, no
> plates, no precise timestamps, enforced in code rather than promised in a privacy
> policy. Alter the log and verification fails. Opening sealed raw evidence takes a
> multi-party "break glass", so no one person can casually browse it.
>
> - A Docker sidecar next to Frigate, or a Home Assistant app
> - Hardware: a Pi or any x86 box, plus the RTSP cameras you already own
> - No cloud, no account, no telemetry, no subscription. Apache-2.0.
> - Honest limits documented: a root operator on the host is outside the trust boundary
>   (the repo calls this "the root paradox" and explains exactly what is and isn't covered)
>
> Install: https://securacv.com/home-assistant · Repo: https://github.com/kmay89/securaCV
>
> Target user is anyone who wants cameras without becoming a tiny surveillance company —
> and especially people who may someday need a record someone else can check: tenants,
> journalists, anyone whose word might be doubted.

---

## 3. Hacker News (Show HN)

**Title:** Show HN: SecuraCV – a witness for your cameras: a signed log of what happened,
never who

**URL:** https://github.com/kmay89/securaCV

**First comment (post immediately after submitting):**

> Author here. The premise: surveillance archives are a liability, but the *ability to
> prove what happened* is genuinely valuable. SecuraCV separates the two.
>
> It sits beside the cameras you already run (Frigate does ingest and detection) and
> keeps an append-only log of semantic claims ("BoundaryCrossingObjectLarge", bucketed
> timestamps) — each entry Ed25519-signed and hash-chained, so any alteration breaks
> verification. Seven privacy invariants (no raw export by design, no identity substrate,
> metadata minimization, ...) are enforced in the kernel code, with the spec in the repo.
> Opening sealed raw evidence takes a quorum of trustees the owner names.
>
> It's a Rust kernel, a Home Assistant integration, and optional ESP32 "Canary" sensor
> firmware, each device keeping its own signed chain checked against a pinned key. The
> firmware also runs in the browser, compiled to WebAssembly: https://securacv.com/lab
>
> Known limits, documented in the repo: the record is tamper-*evident*, and nothing here
> prevents tampering; a root operator on the host is outside the boundary; on-device
> hardware validation is the last gate before v1. Happy to answer anything about the
> invariant design, the break-glass ceremony, or why timestamps are deliberately
> imprecise.

**HN survival notes:** submit on a weekday morning US time; never edit the title to add
hype; answer the inevitable "why not just chain-of-custody a normal NVR?" with the
invariants doc rather than adjectives.

---

## 4. Short-form (Mastodon / X / Discord)

> Your cameras don't need to remember everyone forever to protect you.
>
> SecuraCV: keep Frigate, add proof. A signed, hash-chained log of what happened — never
> who — that shows if anyone edited it. No cloud. No subscription.
> Frigate + Home Assistant + Rust. Apache-2.0.
>
> https://securacv.com/home-assistant

(Fits with room for one screenshot — use the timeline card showing "Verified ✓"; until
one is captured from a real install, the [social preview image](../social_preview.png).)

---

## 5. Directory listings (one-time, compounding)

- **HACS default repository** — submit once stable; HACS users discover integrations
  in-app far more often than via GitHub search. (The integration already ships its
  own icon in `custom_components/securacv/brand/`, so no brands-repo PR gates this;
  [`brands/home-assistant/README.md`](../../brands/home-assistant/README.md) has the status.)
- **awesome-selfhosted** (PR under *Video Surveillance*), **awesome-home-assistant** — one
  PR each, evergreen traffic.
- **Frigate community discussions** — a "works with Frigate" post; Frigate's user base is
  exactly the "Start here" segment in the README.

---

## After every launch post

- Pin a "Welcome — start here" discussion in the repo so arrivals land somewhere warm.
- Watch the issue tracker: first-day installers finding bugs is the *good* outcome —
  every fast, friendly fix is visible marketing to everyone who reads the thread after.
