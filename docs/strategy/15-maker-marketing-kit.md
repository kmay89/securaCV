# 15 — The Maker Marketing Kit: selling the Lab to the people who will love it

> Companion to [05-market-and-cost-comparison.md](05-market-and-cost-comparison.md) (the
> analysis) and [09-marketing-pitch-tco-and-friction.md](09-marketing-pitch-tco-and-friction.md)
> (the TCO math, June-2026 pricing). This doc is the *materials*: the head-to-head
> comparison in customer-facing form, the audience map for makers/builders/homelabbers,
> the channel playbook, and the launch sequence. Kit pricing and the fulfillment/flashing
> pipelines live in [16-kit-commerce-pricing-and-fulfillment.md](16-kit-commerce-pricing-and-fulfillment.md).

## 0. The one-paragraph pitch (maker voice)

> **A security camera with a 24-hour memory — and a canary that vouches for it.**
> Clips stay on your hardware and auto-delete. What survives is a signed, hash-chained
> log: tamper-evident, so any edit — by anyone, including you — breaks verification.
> It's $0/month forever,
> Apache-2.0 top to bottom, rides your existing Home Assistant + Frigate stack, and the
> sensors are ESP32 boards you can flash from a browser and case in enclosures you print
> yourself from parametric OpenSCAD. Before you buy or build anything, the firmware runs
> — for real, compiled to WebAssembly — in your browser at the Lab.

The Lab is the demo nobody else has: *"try the actual firmware before you own the
hardware"* is a hook that lands with exactly the audience we want.

## 1. The head-to-head — cost, quality, features, privacy

This is the customer-facing master table. Numbers are the June-2026 refresh from doc 09
(4 cameras, 5-year horizon — the design target). Keep the website version generated
in spirit from this doc; when doc 09's pricing refreshes, refresh here and on the site.

### 1.1 Cost

| | Upfront (4-cam) | Subscription | 5-yr fees | **5-yr total** |
|---|---|---|---|---|
| Ring (cloud) | ~$400 | Multi $9.99/mo · AI Pro $19.99/mo | $600–$1,200 | **~$1,000–$1,600** |
| Google Nest (cloud) | ~$400–$720 | Home Premium $10/mo · Advanced $20/mo | $600–$1,200 | **~$1,000–$1,900** |
| Eufy (local + optional cloud) | ~$630 | $0 · optional ~$3/mo/cam | $0–$720 | **~$630–$1,350** |
| Reolink (NVR) | ~$480 | $0 (optional cloud) | $0 | **~$480** |
| UniFi Protect (NVR) | ~$720–$900 | $0 | $0 | **~$720–$900** |
| Frigate (self-host) | ~$440–$520 | $0 (optional Frigate+ ~$50/yr) | $0–$250 | **~$440–$770** |
| **SecuraCV** (self-host) | **+$0 on an existing HA/Frigate stack** · ~$440–$520 greenfield | **$0, ever** | **$0** | **~$0 incremental** |
| **+ Canary witnesses** | **$27–$55/node DIY · kits from ~$69** | **$0, ever** | **$0** | **hardware only** |

Payback framing: a $200 mini-PC pays for itself vs. Ring Multi in ~20 months, vs. any
$20/mo plan in 10. Five years of Ring fees exceed the *entire* hardware cost of a
SecuraCV setup.

### 1.2 Features & quality

Honesty first — this is a **different category**, and the comparison must say so:

| | Live view / clips | Detection | Tamper-evident record | Works offline | Open source | Hackable hardware |
|---|---|---|---|---|---|---|
| Ring / Nest | Excellent apps, cloud clips | Cloud AI (faces, packages) | No | Degraded/no | No | No |
| Eufy / Reolink | Good apps, local clips | On-device AI | No | Mostly | No | No |
| UniFi Protect | Excellent local NVR | On-device AI | No | Yes | No | No |
| Frigate | Best-in-class local NVR | Local AI (Coral/GPU/OpenVINO) | No | Yes | Yes | BYO cameras |
| **SecuraCV** | **Via Frigate (unchanged)** | **Via Frigate + on-sensor (Canary)** | **Yes — signed, hash-chained, verifiable** | **Yes — LAN-only by design** | **Yes, Apache-2.0** | **Yes — ESP32 + printable enclosures** |

We do not out-app Ring and we do not replace Frigate — we ride alongside Frigate for
+$0 and +5 minutes and add the two things nobody in the row above has: a **privacy
boundary enforced in code** and a **tamper-evident record — any edit breaks
verification**. (Phrasing rule for all materials: "tamper-evident" and "verifiable",
never "tamper-proof" or "impossible to edit" — a root/admin on the host sits outside
the trust boundary, per [docs/root_paradox.md](../root_paradox.md), and doc 13's
Promise Card forbids overclaiming.)

Quality notes for the honest footnote: video quality is whatever your RTSP cameras and
Frigate deliver (typically better than cloud-cam compression); Canary Vision detection
is on-sensor (Grove Vision AI V2, Ethos-U55 NPU) — person detection without a single
pixel leaving the device; displays are for glanceable status, not video walls.

### 1.3 Privacy

| | Footage location | Faces / plates | Cloud dependency | Vendor can see footage | Deletion story |
|---|---|---|---|---|---|
| Ring / Nest | Vendor cloud | Yes — identification is the product | Total | Yes (policy-limited) | Policy |
| Eufy | Local base + optional cloud | Yes, on-device | Partial | Historically murky | Policy |
| Reolink / UniFi | Local NVR | Optional | None–partial | No | Manual |
| Frigate | Local | Via add-ons | None | No | Retention config |
| **SecuraCV** | **Local, auto-deletes in 24h** | **No — no code path exists** | **None — Invariant IV** | **No — nothing leaves the LAN** | **Enforced in code, not policy** |

The kicker line for materials: *"Ring's business model is retaining and analyzing your
footage. Ours is architecturally incapable of it — the privacy rules are
[invariants in code](../../spec/invariants.md), not promises in a policy."*

## 2. Who will love this — the audience map

Lead segment (doc 08): privacy-conscious prosumers / homelabbers. For marketing, split
them into four maker tribes with distinct hooks:

| Tribe | Where they live | The hook | The proof artifact |
|---|---|---|---|
| **Home Assistant people** | r/homeassistant, HA forum, HACS | "+$0, +5 min on your existing stack; sensors, digest, Verify Now button appear automatically" | 5-minute add-on install; the dashboard generator |
| **ESP32 / firmware tinkerers** | r/esp32, Hackaday, PlatformIO circles | "The emulator IS the firmware — same C++ in wasm. TOFU + Ed25519 on a $7 board. PRs welcome" | The Lab emulator; `firmware/` docs depth |
| **3D-printing makers** | Printables, MakerWorld, r/functionalprint | "Parametric OpenSCAD enclosures with print-validated STLs, GoPro hinges, TPU gaskets — free forever" | Enclosure showroom; the `.scad` customizer |
| **Homelab / self-host** | r/selfhosted, r/homelab, Self-Hosted pod | "$0/month forever, nothing phones home, Docker compose next to your Frigate" | TCO table; `doctor` check; SBOM |

Plus the "why we exist" story segment (tenants, journalists, survivors — doc 04): not
the lead *buyer* segment for kits, but the story that makes the maker audience care and
share. Lead with the maker hooks, close with the mission.

**Anti-personas** (don't spend on): people who want a Ring replacement with an app and
facial recognition; people who won't self-host anything. The materials should
disqualify them politely and early — it builds trust with everyone else.

## 3. Messaging pillars

1. **$0/month, forever.** The market is trained to resent subscriptions; say the number.
2. **Privacy by construction, not by policy.** No faces, no plates, no cloud — enforced
   in code you can read.
3. **Proof, not promises.** Ed25519-signed, hash-chained, court-aware. "Verified ✓" is
   a real signature check — even in the browser demo.
4. **Hackable all the way down.** Apache-2.0; PlatformIO firmware; OpenSCAD enclosures;
   BOMs with real part numbers; SBOM in CI. Nothing is a black box.
5. **Meet it before you buy it.** The Lab runs the shipping firmware in your browser.
   No signup, no email gate, nothing phones anywhere.

Tone rules (matches the repo's voice): honest to a fault (statuses like "in
development — bench pending" are *shown*, not hidden), playful where the bird lives,
precise where evidence lives. Never overclaim court admissibility — "designed for
FRE 901/902 workflows" not "court-proof".

## 4. Channel playbook

Ordered by expected yield per effort:

1. **Home Assistant Community forum + HACS** — the highest-intent audience on earth for
   this. Post a "Show off your project" thread with the 5-minute install and the
   dashboard generator GIF. Target HACS default-store inclusion as a milestone.
2. **Reddit** — r/homeassistant (install story), r/selfhosted (TCO story),
   r/esp32 (emulator-is-the-firmware story), r/functionalprint & r/3Dprinting
   (enclosure showroom story). One tailored post each; never cross-post identical text.
3. **Hackaday** — tip line with the wasm-emulator angle ("the demo page runs the
   shipping firmware; drift is structurally impossible"). Hackaday loves an
   architecture flex with schematics; doc 12's flight-rules angle is a second story.
4. **Show HN** — "Show HN: A security camera with a 24-hour memory (and its firmware
   runs in your browser)". The Lab link *is* the demo; HN converts to GitHub stars and
   contributors, not sales — that's fine, stars are the top of this funnel.
5. **Printables / MakerWorld** — publish every enclosure free with photo-rich listings
   linking back to the Lab. Makers who print the case buy the boards (see doc 16's
   parts-pack tier). This is a permanent, zero-cost acquisition loop.
6. **YouTube seeding** — 5–10 creators in the HA/self-host niche; offer kits + the
   angle "the camera that can't dox your neighbors". One good video outperforms
   everything above.
7. **Podcasts/newsletters** — Self-Hosted, Home Assistant podcast, Hackster newsletter.
8. **Crowd Supply launch** (when kits productize — doc 16 Phase 3): their audience is
   exactly this, and the campaign page doubles as durable marketing.

KPI funnel to watch: Lab visits → GitHub stars → HACS installs → Discussions joins →
kit waitlist signups → kit orders. Each channel post should link the Lab, not the repo
root — the Lab converts curiosity into "I've already touched it".

## 5. Launch sequence (repeatable)

- **T-2 wk**: kit waitlist live on the website (compare page), Printables listings up,
  press kit in repo (`docs/press/` — logo, mascot, screenshots, the one-paragraph pitch,
  the TCO table as an image).
- **T-0**: HA forum post + r/homeassistant same day (morning US). Lab front and center.
- **T+2 d**: Show HN (the wasm angle), then Hackaday tip with whatever HN surfaced.
- **T+1 wk**: r/selfhosted TCO post; r/esp32 emulator post; creator kits ship.
- **T+2–4 wk**: creator videos land; retro in Discussions; fold FAQs into the site.

Rule: every touchpoint ends with the same two CTAs — **"Open the Lab"** (no commitment)
and **"Join the kit waitlist"** (commitment).

## 6. The comparison one-liners (for social/ads/FAQ reuse)

- vs **Ring/Nest**: "Five years of their fees costs more than our entire hardware
  stack. And they can watch your footage; we built a system that can't."
- vs **Eufy/Reolink**: "Local is necessary, not sufficient. Local footage can still be
  edited without a trace. Ours comes with a tamper-evident chain — edit it and
  verification fails."
- vs **Frigate**: "We're not a Frigate alternative — we're the witness that rides
  alongside it. Keep Frigate. Add proof."
- vs **DIY from scratch**: "You could build this. We wrote it down — BOMs with part
  numbers, printable enclosures, firmware you can try in a browser. Skip to the fun part."
