# Design: the Hub as network witness — CONCEPT

**Status:** concept — research complete, no code. Nothing in this doc ships;
it exists so the idea is argued honestly *before* the first line is written,
per the [`canary-fence-guard`](../../firmware/projects/canary-fence-guard/README.md)
precedent. · **Date:** 2026-07-25 · **Owner:** TBD

> *"Research what the Pi-hole does and see if we can borrow it and wrap it
> under ours, but with a magical setup that helps the user and makes it way
> prettier and understandable — something like that is needed for our brand
> as a way to stop TVs from calling home. … Maybe the idea could be it
> catches bad Canaries — and meanwhile happens to do this other amazing
> thing too."*

Short answer: **yes — as a Hub feature, not a Canary, and by embedding a
permissively-licensed engine rather than forking Pi-hole.** The experience
layer (the setup, the proof moment, the plain-language controls) is the part
worth building ourselves, and it is also the part our existing onboarding
canon already designed. The engine is a solved problem we should adopt.

One product, three faces:

1. **Witness the network.** Every smart device is somebody's canary — it
   sings, constantly, to whoever made it. The Hub listens for every device
   singing to a stranger and shows you, in plain words, who your gadgets
   talk to. Passive, zero-breakage, on by default.
2. **Silence the snitches.** One tap per device: "Mute this TV's snitching."
   DNS-level blocking as a per-device verb, not a network-wide list a user
   has to understand.
3. **Verify the fleet.** The Hub continuously authenticates its Canaries'
   self-manifests over the LAN — chain head, signature, tamper state — and
   flags a Canary that can't prove itself. This closes a promise the
   security model already makes: a self-manifest is trusted over USB
   (you hold the cable) but *must be authenticated before trust over LAN*
   ([`flasher_experience.md`](flasher_experience.md)). Today nothing plays
   that warden role — and, honestly stated, nothing *can* yet: the
   self-manifest (`j`) and the attest challenge are serial-console paths,
   Vision/Sense expose no HTTP API, and mDNS TXT data is unsigned. A
   signed LAN attestation protocol (challenge/response endpoint on every
   flavor) is a **firmware prerequisite** for this face, not a reusable
   asset (open question 4).

Faces 1–2 are the outward mirror of an existing internal virtue — the
codebase already praises devices that "never phone home"
(`firmware/projects/canary-display/src/main.cpp`, `mode_registry.h`).
Face 3 makes it a SecuraCV product instead of a commodity DNS box.

---

## 1. What Pi-hole actually is (research summary)

A DNS sinkhole: one daemon (`pihole-FTL`, a modified
[dnsmasq](https://docs.pi-hole.net/ftldns/)) answers all DNS queries on the
LAN, returns "blocked" for names on a compiled blocklist ("gravity",
SQLite), forwards the rest upstream, and (since
[v6](https://pi-hole.net/blog/2025/02/18/introducing-pi-hole-v6/)) serves
its own dashboard and REST API from the same binary. The magic ingredient
is not the code — it is the community blocklists
([HaGeZi](https://github.com/hagezi/dns-blocklists),
[OISD](https://oisd.nl/)), which any resolver may consume and which include
TV-vendor telemetry lists (Samsung, LG, Roku, TCL).

### Can we borrow it?

| Engine | License | Wrap/rebrand? | UI | Notes |
|---|---|---|---|---|
| Pi-hole (FTL) | EUPL 1.2 (copyleft) | Run **unmodified** + our own UI on its v6 REST API: fine. Fork-and-rebrand: copyleft applies, and the [trademark policy](https://pi-hole.net/trademark-rules-and-brand-guidelines/) forbids their name/logo. | Ships its own (we'd hide it) | Heaviest; drags dnsmasq + DHCP we don't need |
| AdGuard Home | GPLv3 | Same shape as Pi-hole | Ships its own | Commercial steward; HAOS add-on exists |
| **blocky** | **Apache 2.0** | **Embed, fork, brand freely** | **None — by design** | Go, single binary, YAML config, DoH/DoT upstreams built in |
| From scratch | — | — | — | `captive_dns.h` is a tested DNS packet layer (~80% of a sinkhole's hard part), but caching, TCP fallback, DNSSEC, and list compilation are months of solved edge cases |

**Recommendation: blocky.** The permissive license removes every legal
question, it deliberately ships no UI (the exact part we want to own), and
its upstream DoH/DoT support matters for the bypass problem in §4. Pi-hole
remains the fallback path (unmodified, behind our UI) if blocky's engine
disappoints in practice.

## 2. Why the Hub, not a Canary

The ESP32 fleet is the wrong host, structurally — not by a margin, by
category:

- **WiFi 2.4 GHz only, no Ethernet anywhere** in the board registry
  (`firmware/boards/boards.json`), and the softAP explicitly does not NAT
  (`docs/manual_test_plan_captive_portal.md`) — no client routes through a
  Canary today.
- The radio already timeshares STA + mesh + Chirp + CSI sensing under an
  airtime governor capped at ≤2% (`docs/network_coexistence.md`,
  `airtime_governor.*`). A home's resolver is a single point of failure for
  the whole LAN; putting it behind that contention makes Netflix look
  broken and gets the Canary blamed.
- The flagship build already sits near its flash ceiling
  (`firmware/projects/canary-wap/FLASH_MEMORY_ANALYSIS.md`,
  `firmware/PARTITIONS.md`); a 100k–1M-domain blocklist has nowhere to live
  on C3/C6 boards at all.

The **Raspberry Pi hub** ([`raspberry_pi_hub_flashing.md`](raspberry_pi_hub_flashing.md),
accepted, build in progress) is a full Linux appliance with Ethernet, GBs of
RAM, RAUC A/B rollback, and an add-on channel — the delivery mechanism this
wants. Concretely: **a Home Assistant OS add-on embedding blocky**, sitting
next to the Privacy Witness Kernel add-on that already ships.

## 3. The invariant boundary (what makes ours different, not just prettier)

Pi-hole keeps a full per-query log: who looked up what, when — a queryable
archive of household behavior. Our invariants forbid exactly that
([`spec/invariants.md`](../../spec/invariants.md): II No Identity Substrate,
III Metadata Minimization, VII Non-Queryability), and
[`08-product-strategy.md`](../strategy/08-product-strategy.md) §7 names the
test: *every "wouldn't it be convenient if…" that adds identity, query, or
retention is a competitor's product.*

So the wrapper's contract, stated up front:

- **Counters and coarse aggregates only.** Per-device tallies ("your TV
  tried 47 tracking domains today"), category rollups, and block/allow
  state. No per-query retention, no timestamps finer than the existing
  coarse-time discipline, no device-identity join across households, no
  search box over history.
- Blocklist updates are pull-only from public lists, and *we* add no
  egress: no telemetry, no log shipping, no cloud of ours. One boundary
  must be stated honestly, though: a forwarding resolver by nature
  discloses every **non-blocked** lookup to its configured upstream —
  DoH/DoT encrypt the transit, not the upstream's view. So the upstream is
  a user-visible, explicit choice (privacy-respecting defaults, QNAME
  minimization), and a **local recursive mode** (unbound-class, no
  third-party resolver at all) is an open question, not an assumed
  default. Claiming "nothing leaves the Hub" without that caveat would
  misstate the privacy boundary. This is Invariant IV (Local Ownership)
  applied to DNS, scoped truthfully.

This is not a limitation to apologize for — it is the differentiator.
"Witnessing without watching," applied to the network itself: the one DNS
shield that *cannot* become a log of your home life, even by us.

## 4. The honest hard part: devices that cheat

DNS filtering catches most telemetry, but modern TVs bypass the network's
resolver with hardcoded DNS (8.8.8.8) or DNS-over-HTTPS. Honesty-first copy
means two tiers, stated plainly:

- **Tier 1 (v1, works everywhere):** the Hub becomes the LAN's advertised
  resolver. The primary path is one guided router setting (the wizard's
  router auto-detect walkthrough). Hub-as-DHCP exists as a *fallback for
  routers that let you disable DHCP but not change DNS* — it is **not** a
  no-router-login shortcut: the router's own DHCP server must be disabled
  or scope-delegated first, or two authoritative DHCP servers race and
  clients get inconsistent leases and unfiltered DNS. Catches the large
  majority of telemetry. Known-DoH-endpoint blocklists (HaGeZi ships one)
  raise the ceiling further.
- **Tier 2 (future, full promise):** stubborn devices need port-53 redirect
  at the gateway — a Hub-as-gateway story we do not promise until it
  exists. Until then the copy stays present-tense about tier 1, per
  `BRAND.md`'s honesty rule.

## 5. The magical setup (reuse, don't rebuild)

The north star is already written —
[`flasher_experience.md`](flasher_experience.md) ("comes to life → proves
itself → one obvious next step") and
[`docs/onboarding_unified_wizard.md`](../onboarding_unified_wizard.md)
("end on proof, not a checkmark"). Applied here:

| Existing asset | Reused as |
|---|---|
| Flasher hub image writer + "type Wi-Fi once" seeding (`desktop/hub-core/`, `wifi-memory.js`) | The add-on arrives pre-installed in the hub image; zero extra setup steps |
| Unified wizard patterns (announce ≤2 s, Identify verb, end on proof) | The "make it your network's shield" step: router auto-detect → guided one-field change, or DHCP-takeover fallback |
| Fleet self-report contract (`fleet_selfreport.h`, `tvos/discovery/DISCOVERY.md`) | "Blocked today" counters surface on the tvOS Witness Wall, display fleet cards, iOS app — one new field, same contract, per [`FLEET_PARITY.md`](../FLEET_PARITY.md) |
| Self-manifest + LAN-auth rule (`firmware/common/attest/self_manifest.h`) | The warden face's *vocabulary* (manifest fields, attest challenge) — but the transport is new work: today these are serial-console paths, so the sweep needs a signed LAN endpoint added to every flavor first (open question 4) |
| Card standards (`docs/standard/CANARY_CARDS.md`) | The per-device "who it talks to / mute" card |

**The proof moment** (the demo, the marketing, and the onboarding ending,
all at once): within the first hour, unprompted —

> *"14 devices found. Your TV called 47 tracking domains. Your Canaries
> called nobody."*

Then one tap to silence. No list URLs, no regex, no dashboard safari.
Curated plain-language toggles ("Block TV snooping", "Block ad trackers")
map to maintained HaGeZi/OISD lists underneath.

## 5b. The experience bar: the phone-call test

The acceptance test for the whole setup arc, stated as a scene:

> **Someone who has never owned a Raspberry Pi finishes setup in a few
> minutes of their own effort, and calls a relative to say "this was
> amazing — look what I have."**

If the flow doesn't produce that phone call, it isn't done — same spirit
as the wizard canon's "end on proof, not a checkmark." The bar breaks down
into commitments the wizard must keep:

- **The timeline is truthful, down to the minute.** Before anything
  starts, the user sees the whole schedule — "Write card ~4 min · first
  boot ~12 min · your part: about 3 minutes total" — from *measured*
  medians (the validation runbook is where they get measured), not
  guesses. Long unattended waits (the HAOS first boot) get a live
  countdown and are used, not endured: that's when the wizard teaches the
  three faces, so the wait is a tour, not a spinner.
- **Rewarding at every step, encouraging on every stumble.** Each step
  ends with something visibly *alive* (the come-to-life rule from
  [`flasher_experience.md`](flasher_experience.md)), and no error screen
  exists without a next step written for a first-timer. The tone assumes
  success is normal: "almost there," never "operation failed."
- **Choices are present, but pre-decided.** Every decision ships with a
  bolded recommended default and is framed as an outcome ("Block TV
  snooping — recommended"), never as configuration (list URLs, ports,
  upstream IPs). One question per screen, maximum. Maker-depth escape
  hatches exist but are invisible until sought.
- **Anything automatable is one click — or zero.** Wi-Fi typed once and
  seeded onto the card (ships today), the add-on pre-installed in the hub
  image (open question 6 leans yes), router auto-detect attempted before
  the user is ever asked to look at their router. The ceiling to aim for:
  the only mandatory human inputs in the whole arc are the Wi-Fi password
  and physical acts (insert card, plug in).
- **It just works — and proves it.** The arc ends on the proof moment
  (§5's first-hour report), which is also the thing the relative gets
  shown on the phone call.

These commitments govern the DNS-witness onboarding specifically, but the
bar is the same one the one-flash hub design
([`raspberry_pi_hub_flashing.md`](raspberry_pi_hub_flashing.md)) is being
built to — one arc, one standard.

## 5c. The no-breakage rule, and the witness that teaches

Two standing laws for the product after setup day:

- **It must never block something useful. Zero complaints is the spec,
  not a stretch goal.** Overblocking is the classic reason DNS filters
  get unplugged — one broken smart-TV app and the whole product is
  blamed. So: *witnessing is default-on, blocking is by consent* — the
  product observes everything and blocks nothing until the user chooses,
  which means it structurally cannot break what it didn't promise to
  touch. Default lists are the conservative, liveability-first tier
  (OISD's stated philosophy); aggressive lists live behind the maker
  hatch. Every mute is one tap to undo, and when a device misbehaves
  after a mute, "undo the mute" is the first suggestion offered — the
  product apologizes with a working fix, never with a settings safari.
  A mute that keeps causing un-mutes should quietly suggest staying off.
- **It only adds value if it teaches.** The witness face is an education,
  not just a tally. Every observed or muted domain is explained in plain
  words — what it's for, who runs it, what typically flows ("this one
  reports what you watch, about once an hour") — so a user finishes their
  first week understanding what telemetry and tracking *are*, not just
  having them counted. Same register as every guide page: "an imaginative
  ten-year-old and a security-grade maker in the same breath"
  ([`BRAND.md`](../BRAND.md)). Curating those explanations for the
  common-offender domains is product work on par with the lists
  themselves — the lists are borrowed; the understanding is ours.

### Winning the existing Pi-hole owner

There's a third audience beyond first-timers and makers: people already
running a Pi-hole. They are won on *experience*, not features — they
already have the blocking. What they don't have: a box that updates
itself (HAOS-managed, RAUC rollback) instead of demanding SSH gardening,
per-device verbs instead of list management, explanations instead of a
query log, counters that can't become a household diary, and the fleet
integration (the same box that wardens their Canaries). The switching
cost must round to zero: accept a Pi-hole Teleporter backup during setup
and carry over their custom allow/deny decisions in one click — their
years of tuning arrive intact, minus the maintenance. The pitch is one
sentence: *everything your Pi-hole does, nothing it makes you do.*

## 6. Naming, trademark, and casing

- **The product name stays out of the Canary namespace.** Two reasons:
  the Hub is the *warden of* the Canaries, not a Canary; and
  [`legal-audit-2026-07.md`](../legal-audit-2026-07.md) H1 already flags
  "Canary" as colliding with a registered mark (CANARY / CANARY FLEX) in
  our own category — and Thinkst's "Canary" is a famous *network security
  appliance*, the closest possible neighbor to this product. Working
  candidates, all pending a real clearance search: **Warden**, **Sentry**,
  **Roost**. Punctuation variants ("Canary++", "/canary", "[canary]") are
  ruled out: trademark confusion is judged by ear, and punctuation cannot
  survive domains, bundle IDs, mDNS names, or store listings. Stylized
  marks belong to the design layer (the wordmark) only.
- **Consumer-facing composite:** whatever the name, it ships as
  "SecuraCV &lt;Name&gt;" — the house mark carries the distinctiveness,
  per the same logic as the legal audit's H1 mitigation.
- **Trademark clearance is an explicit open question** (below), not a
  launch-week discovery.

## 7. Open questions

1. Engine validation: does blocky's resolver hold up under a real
   household's query load on a Pi 4 alongside HAOS + PWK? (Bench it.)
2. Setup default: the router-setting walkthrough is primary; when (if
   ever) is Hub-as-DHCP offered, and how does the wizard verify the
   router's own DHCP is actually off before enabling it? Which failure
   mode breaks fewer homes?
3. The aggregate schema: exactly which counters exist, at what time
   granularity, and where they live — must pass the invariants review
   before any storage code.
4. The LAN attestation protocol — the warden face's firmware
   prerequisite: a signed challenge/response endpoint (reusing the
   existing attest challenge + self-manifest canonical) on every flavor,
   including Vision/Sense which expose no HTTP API today; then sweep
   cadence and the impostor heuristic (what exactly flags a device
   advertising `_securacv._tcp` that fails authentication?).
5. Product name clearance (§6) — attorney search before anything public.
6. Does the add-on ship in the default hub image or as a one-tap install?
   (Default-on witnessing with opt-in blocking is the current lean.)
7. Local recursive resolution (unbound-class) vs a chosen forwarding
   upstream: does the Pi carry full recursion alongside HAOS + PWK, and is
   removing the third-party resolver worth the cold-cache latency? (§3's
   disclosure caveat shrinks to zero only in recursive mode.)

## 8. What happens next (and what this doc is not)

This is a concept, staged like `canary-fence-guard`: no code, no add-on
skeleton, no store page. The sanctioned way to test demand is a concept
card on the website's ideas page. Concepts become candidates when people
ask for them — and when the open questions above close, this doc's status
line changes.
