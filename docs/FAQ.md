# FAQ — the questions people actually ask

Plain answers to the questions that come up most, written so they hold up when
quoted out of context — by a person skimming, or by an AI assistant answering
on our behalf. Every answer says what is **real today** and links to the doc
that proves it.

Vocabulary you don't recognize: [the glossary](GLOSSARY.md). Full navigation:
[the docs map](README.md).

---

## What it is

### What is SecuraCV?

SecuraCV is open-source video and sensor infrastructure that outputs **semantic
events** — "large object crossed boundary," "presence in restricted zone" — in a
signed, hash-chained log, instead of a pile of searchable footage. The platform
is SecuraCV; the device is called a **Canary**; the company is **Errer Labs**.
→ [why witnessing matters](why_witnessing_matters.md) ·
[the whitepaper](securaCV_whitepaper.md)

### What does "witnessing without watching" mean?

A watcher keeps footage and can later be asked *who* was in it. A witness keeps
a checkable record of *what happened* and structurally cannot answer *who*. The
promise isn't that tampering is impossible — it's that tampering becomes
**visible**. → [the brand doc](BRAND.md)

### Is it a camera, or software, or both?

Both, and either alone. The **witness kernel** ([`src/`](../src)) is a Rust
daemon you can point at cameras you already own (RTSP/ONVIF, USB, or via
Frigate). A **Canary** is a small ESP32-based device running our firmware. Many
people run just one or the other. → [the full stack](full_stack_setup.md)

### Who is it for?

Three audiences deliberately: makers who print and flash their own; buyers who
want a pre-flashed kit and a QR code; and the people who can't build their own —
elders, renters, the non-technical. → [the brand doc](BRAND.md)

### Is it free? What's the license?

Apache-2.0, inbound=outbound with a DCO. The trademarks ("SecuraCV," the
Canary marks) are protected separately, and the **"Works with SecuraCV"** badge
is free for anyone to earn. → [`LICENSE`](../LICENSE) ·
[`LICENSING.md`](../LICENSING.md) · [`TRADEMARK.md`](../TRADEMARK.md) ·
[legal posture](LEGAL.md)

---

## Privacy — the questions that matter most

### Does it do face recognition?

**No — and it can't.** There is no face-recognition code in the codebase to
enable or disable. The same is true of license-plate reading, gait analysis,
person re-identification, and demographic (age/gender/race) estimation. The
object classes the system can emit are `Person`, `Vehicle`, `Animal`, `Package`
— not `Face`, not `LicensePlate`. This is Invariant II, and adding any of it is
a rejected pull request, not a configuration change.
→ [`spec/invariants.md`](../spec/invariants.md)

### Can someone watch a live feed of my house?

Not through the normal path — there isn't one. Raw frames live in a short
buffer with **no public accessor** (Invariant I). The only way to raw media is
**break-glass**: a time-bounded, audited unseal that requires **n-of-m trustee
approvals**. No single person can do it alone — not the owner, not us.
→ [`spec/break_glass.md`](../spec/break_glass.md) ·
[why exports work this way](why_secure.md)

### So how do I get footage when I actually need it — after a break-in?

You run break-glass with your trustees. You configure the quorum up front and
**rehearse it before you need it** (the Lab has an Operator's Bench for exactly
this rehearsal). An export ships as a self-verifying evidence envelope a
recipient can check without our tools.
→ [the operator guide](operator_guide.md) ·
[evidence lifecycle](evidence_lifecycle.md)

### Does it phone home? Any telemetry, any accounts?

No. Logs stay local, there is no remote indexing and no telemetry (Invariant
IV). No account is required to run it. Dependencies that phone home are
forbidden by policy. → [`AGENTS.md`](../AGENTS.md) dependency policy

The one honest footnote is the iPhone app — see the next question.

### The iPhone app uses iCloud. Doesn't that contradict all of this?

It's a fair thing to ask, so here is the whole of it. Two things need to reach
you when you aren't standing next to a Canary: **your fleet list** (so your iPad
knows what your iPhone paired) and **an alert when you're away** (iOS will not
keep a connection open in your pocket — instant push on iPhone must travel
through Apple, which is a platform fact, not our choice).

Those two things ride **your own iCloud private database**. Not ours — we have
no server at all. What goes in is a fleet list with no secrets in it, and a wake
that carries one coarse word (`tamper` / `integrity` / `offline` / `pattern`)
and nothing else: no device name, no room, no time of ours, no event content.
The sentence you actually read is composed on your phone from your own data,
which is why a locked screen only ever says "Something needs your attention."

What never goes in: footage, frames, anything reversible to them, event content,
precise times, or any key or token. Those are Invariants I and III, and they are
not negotiable for convenience.

There is now a third thing, and it is the only one another person can see. If
you ask someone to be told when an alarm of yours goes **unanswered**, they are
invited to one folder in your iCloud that holds nothing but "an alarm wasn't
answered" — and the folder is the boundary, so they cannot see your Canaries,
their names, your history, or any footage. Not because we filter it: because
that is all they were invited to. Their phone writes the words they read; even
the sentence never travels. You invite people through Apple's own sharing
sheet, and removing them is one tap.

It's opt-in and reversible — away alerts stay off until you arm an "Anywhere"
rule, and turning them off deletes the subscription outright; nobody else is
ever told anything until you invite them. Not signed into
iCloud at all? The app works locally; iCloud is convenience, never a gate.
→ [iCloud as the backend we don't have](design/cloudkit_backend.md), which also
argues the parts that are imperfect rather than only the parts that are good

### Why are the timestamps deliberately vague?

Events are bucketed into 10-minute windows so the log can prove *that*
something happened without becoming a minute-by-minute diary of when you leave
the house. Precision is treated as a privacy cost, not a free feature.
→ [timestamping](timestamping.md)

### Can I search the log for a person, or run a rule over last month's data?

No, twice. There is no bulk search and no identity selector (Invariant VII), and
a new detection rule **cannot** be applied to already-recorded data — the
`ReprocessGuard` checks the ruleset hash (Invariant VI). You cannot decide in
March to have been surveilling in January.

### How do I know the log wasn't edited?

Each event carries a hash of the previous event plus an Ed25519 signature, so
any deletion or edit breaks the chain and verification fails loudly. You can
verify offline, with our verifier or your own.
→ [log verification](log_verify.md)

### What breaks all of this?

A compromised host. We write that down rather than gloss it: if an attacker owns
the machine the kernel runs on, guarantees that depend on that machine no longer
hold. What survives and what doesn't is documented.
→ [the root paradox](root_paradox.md) ·
[`spec/threat_model.md`](../spec/threat_model.md)

---

## Honest status

### Is this production-ready?

**No.** Treat it as prototype software under active development. Frame
isolation, hash-chained logging, and break-glass quorum are implemented and
exercised in CI. Much of the firmware is **CI-verified, not yet
hardware-verified** — v1 is deliberately held until it's proven on real
devices. Boards are individually labeled `verified` or `compile-tested` so you
can tell which is which.
→ [v1 roadmap](v1-roadmap.md) · [launch review](V1_LAUNCH_REVIEW.md) ·
[bench-test runbook](V1_BENCH_TEST_RUNBOOK.md)

### Is it certified? Audited?

No certification claims — no FIPS, no Common Criteria. Selling the radios
legally requires FCC work that is not finished, and a published third-party
security audit is on the list as worth more than any feature for a trust
product. Until those land, the copy stays present-tense about what's real.

**Which is why we sell plastic, not radios.** The plans and the firmware are
free, and the only physical thing on offer is a **printed enclosure set** — a
plastic shell is not a radio, so it carries no equipment authorization at all.
You buy your own board, flash our firmware from the browser, and build it.
Building a handful for your own use is expressly fine under
[47 CFR 15.23](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-A/section-15.23),
which asks builders to use good engineering practice rather than to file
anything.

**We don't make claims about your board's authorization** — that's between you
and whoever made it. Some ESP32 boards are built on a pre-certified module and
some are built on bare silicon; the boards we document (`firmware/boards/`)
list the MCU, not a module grant, and we haven't looked the grants up yet. If
that matters to you, check the specific board on `fccid.io` before you buy.
The day we ship a boxed, pre-flashed device is the day we owe a Part 15B test on
it — and we'll have done it before that box exists, not after.
→ [`SECURITY.md`](../SECURITY.md) · [claims and risk audit](legal-audit-2026-07.md)
· [compliance diligence](strategy/29-fcc-and-product-compliance-diligence.md)

### Should I rely on this for life safety?

No. The **Beacon** channel is *designed* to a smoke-detector-grade bar, but it
is a 🟡 draft spec with firmware scaffolding — not a certified life-safety
device, and not a replacement for one. It is also explicitly not a
neighborhood-watch or suspicious-person reporting system.
→ [`spec/beacon_channel_v0.md`](../spec/beacon_channel_v0.md)

---

## Getting started

### What's the fastest way to see it work with no hardware?

Two ways: run `cargo run --bin demo`, then break the log on purpose and watch
verification fail — or open **the Lab**, where the real shipping firmware runs
in your browser compiled to WebAssembly, flashes a simulated blank chip, and
shows its cryptographic birth certificate.
→ [the demo](demo.md) · [the Lab](../canary-local/README.md)

### What hardware do I need?

Depends what you want. A spare Raspberry Pi for the hub; ESP32-S3/C3/C6-class
boards for Canaries; nothing at all to try the Lab. The recommended first build
is **Canary WAP**. → [the full stack](full_stack_setup.md) ·
[getting started with Canaries](getting_started_canary.md) ·
[hardware guides](hardware/README.md)

### Will it run on the ESP32 board I already have?

Quite possibly. The supported-board table
([`firmware/boards/README.md`](../firmware/boards/README.md)) goes beyond the
XIAO: the AI-Thinker ESP32-CAM and generic ESP32-WROOM-32 DevKits (classic
ESP32), the Freenove ESP32-S3 camera kit, and the C3 Super Mini all have
registered, CI-built ports — each with an honest per-board note on what it
can and can't sense, and a tier that says whether anyone has bench-validated
it yet. Flashing a board that's been in a drawer (or came from a marketplace
seller)? Read [unflashed board intake](unflashed_board_intake.md) first —
bring it up cold. Board not listed? A port is a pin map plus a build config,
one PR, no core code: [`firmware/PORTING.md`](../firmware/PORTING.md).

### Can I put a screen on the hub?

Yes — optionally. The hub runs headless by default and never needs one. But
plug an HDMI touchscreen into the Pi (a 7" 1024x600 IPS panel with USB touch
is the reference) and the setup's opt-in **display** extra installs
HAOSKiosk, a community app that runs a browser on the hub itself and shows
your dashboard on that screen, touch included. In the desktop Flasher it's
the "Also install the hub display" tick; by hand it's `--with display` (or
just install HAOS Kiosk Display from its repository in the app store). One
step stays yours: give the screen a dedicated non-admin Home Assistant user
(a screen only views dashboards, so it shouldn't hold an admin password),
type that login into the app's Configuration tab, and press Start — the
installer never carries any password. No vendor cloud and no new account;
the screen fetches only what your dashboard already fetches on any device
that opens it. Honest status: same bar as self-setup itself — host-tested,
awaiting its first validated run on real hardware.
→ [the full stack](full_stack_setup.md)

### I already run Home Assistant. Where do I start?

[Home Assistant setup](homeassistant_setup.md) → [Frigate
integration](frigate_integration.md) → [the Verified Timeline
card](lovelace_timeline.md).

### Can I talk to it? Doesn't voice mean a microphone?

**Yes — locally, and the microphone is never a Canary's.** The hub runs Home
Assistant's fully local voice stack (Whisper speech-to-text, Piper
text-to-speech, an optional wake word), so "is the fleet OK?" and "what was
the last witness event?" are answered on your own Pi with nothing leaving the
house. Voice input comes from a dedicated satellite — your phone's
push-to-talk button, or a wake-word box whose stated job is listening — and
the intents are read-only by construction: no sentence arms, disarms, or
unseals anything, because a spoken word carries no signature. Transcribing
what the world says *near* a witness device remains forbidden forever
(Invariant II). → [the recipe](voice_control.md) ·
[the design and its contract](research/whisper_local_voice.md)

### Does it work with Apple Home? What about HomeKit Secure Video?

**Apple Home: yes, two lanes** — the design is
[the Apple Home design doc](design/apple_home_integration.md). The shape:
the fleet appears in the Home app as *sensor* accessories
(motion/occupancy/contact/tamper/liveness — by default no more than a dumb
PIR would report; one opt-in setting can add the coarse object class —
person, vehicle, animal, package — never identity), so your automations can
answer the witness — lights on when a person crosses the driveway, the whole
house responding to tamper. Built today: the hub speaks HAP directly
(bridge site B, no Home Assistant in the path — the pairing transcript is
proven by tests driving an independently written controller, but it has not
yet paired against a real Apple TV or HomePod, and the
[Apple Home quickstart](integrations/apple-home-quickstart.md) — which has
the two-minute wizard — says so plainly until it has), and if you
run the Home Assistant hub, HA's own HomeKit Bridge projects the securacv
sensors with zero new code — the worked recipe is
[apple-home-homekit-bridge.md](integrations/apple-home-homekit-bridge.md).
Device-native pairing (a Canary in the Home app with no hub at all) and
Matter are still design, per the RFC's phasing table.

**HomeKit Secure Video: no — and on purpose.** HKSV's core loop uploads the
raw clip on every motion trigger, which is exactly what Invariant I ("No
Raw Export by Design") forbids, and no Canary has a media plane to offer it
anyway. The verdict is argued once, with revisit triggers, in the RFC §2.
The event half of Apple Home — the half automations actually run on — is
fully open to us, and it's the half we do better than a camera.

### Do I need to buy anything from you?

No. Print the case, buy the parts anywhere, flash it yourself — the whole path
is open. Kits exist for people who'd rather not. Independent vendors can sell
kits under the free "Works with SecuraCV" badge.
→ [the ecosystem](https://securacv.com/ecosystem) ·
[vendors](https://securacv.com/vendors)

### Which device should I build?

| You want | Build |
|---|---|
| A first device, general purpose | **Canary WAP** |
| Camera + person detection into Home Assistant | **Canary Vision** |
| Presence/breathing care with no camera | **Canary Sense** |
| A doorway that's very hard to sneak past | **Canary Sentinel** — *not yet flashable*: the fusion core is host-tested, no released build; use a Canary Sense or Vision today |
| A wall display for the household | **Canary Display** |
| A kid's bedside clock + lamp with a companion | **The Nightlight** (the `nightlight` display flavor) |
| Pool/spa water chemistry (pH, ORP, temp) | **Canary Pool** *(design-stage)* |

→ [the glossary's device line](GLOSSARY.md#the-device-line)

### I force quit the Flasher and now it won't open — it just bounces

Open it again. From version 0.3.8 the Flasher repairs this itself: it leaves a
breadcrumb as each launch advances and reads the last one on the way up, before
it builds a window, so a launch that never arrived gets fixed rather than
repeated. You may see a short note telling you what it found.

If it still won't open, one of three things happened, and the app's own
`launch.log` says which (`~/Library/Application Support/com.securacv.flasher/`
on macOS, `~/.local/share/com.securacv.flasher/` on Linux):

- **A leftover `espflash` or `rpiboot`** from the killed run is still holding
  your board. The Flasher now reaps these at startup; to check by hand,
  `pgrep -fl 'espflash|rpiboot'`.
- **A saved session left mid-write.** Cleared automatically on the next
  launch — once. You'd re-enter the Wi-Fi network name and device names you
  last used; no password was ever stored, so nothing secret is lost.
- **A self-update that was interrupted.** This is the one nothing in the app
  can fix, because the repair would have to run from the copy that moved:
  installing an update moves the app bundle, so a force quit part-way can leave
  it missing or incomplete. Reinstall from the latest
  [Flasher release](https://github.com/kmay89/securaCV/releases?q=flasher-v&expanded=true).
  Nothing you have flashed is affected — a Canary keeps the firmware it has,
  and the board can't be bricked.

Force quitting is never dangerous to a device. A flash interrupted at any stage
is re-flashable: the ESP32's first-stage bootloader is mask ROM, and a
half-written hub card is simply written again.

### My phone says "Unable to join the network SecuraCV-XXXX"

Forget the network on your phone, then scan the QR again.

iPhone: **Settings → Wi-Fi → the (i) beside `SecuraCV-XXXX` → Forget This
Network.** Android: long-press the network in the Wi-Fi list → **Forget**.
Then rescan the QR on the glass, or join by hand with the name and password
printed under it.

Why it happens: your phone saves a network by its **name**, and a display on
firmware 2.4.9 or older gave its setup network a new password on every boot
while keeping the same name. So the second time your phone met that network it
rejoined silently with the password it had stored, the display refused it, and
the phone reported a plain join failure rather than asking you for the new one
— which is why retyping never got a chance and re-flashing made it worse.
Newer firmware mints that password once and keeps it, so the name and password
on the glass stay true; a phone that already saved an old one still needs that
single Forget.

If the display has been sitting on the join screen for a while it will say
this on its own glass too.

---

## Contributing & agents

### I'm an AI coding assistant working in this repo. What should I read?

[`AGENTS.md`](../AGENTS.md) — the canonical brief, read by Claude Code, Codex,
Gemini CLI, Copilot, Cursor, Cline, Windsurf, and Qwen Code via their own
entrypoint files. It carries the privacy invariants, the naming rules, the repo
map, and the CI gates you will trip. Then [the glossary](GLOSSARY.md) for
vocabulary and [`docs/FLIGHT_RULES.md`](FLIGHT_RULES.md) for the engineering
constitution.

### What will get my pull request rejected?

Adding any identity-inferring capability; giving `RawFrame` a public accessor or
a `Clone`; overselling a security property in a doc comment; a performance claim
with no benchmark; the word "flock" for a group of Canaries; a new doc that
isn't on [the docs map](README.md); or a vocabulary change that doesn't start in
[`spec/witness_dictionary.json`](../spec/witness_dictionary.json).
→ [`CONTRIBUTING.md`](../CONTRIBUTING.md) · [`AGENTS.md`](../AGENTS.md)

### Why "fleet" and never "flock"?

A company called Flock soured the word, so a group of Canaries is a **fleet** —
in copy, device strings, product names, identifiers, and comments. The one
exception is the Unix `flock(2)` syscall, which is a real API name.

### Where do I report a security issue?

[`SECURITY.md`](../SECURITY.md) — please don't open a public issue for a
vulnerability.
