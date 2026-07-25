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
   that warden role.

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
- Blocklist updates are pull-only from public lists; nothing about the
  household leaves the Hub. This is Invariant IV (Local Ownership) applied
  to DNS.

This is not a limitation to apologize for — it is the differentiator.
"Witnessing without watching," applied to the network itself: the one DNS
shield that *cannot* become a log of your home life, even by us.

## 4. The honest hard part: devices that cheat

DNS filtering catches most telemetry, but modern TVs bypass the network's
resolver with hardcoded DNS (8.8.8.8) or DNS-over-HTTPS. Honesty-first copy
means two tiers, stated plainly:

- **Tier 1 (v1, works everywhere):** the Hub becomes the LAN's advertised
  resolver — via one guided router setting, or by the Hub taking over DHCP
  (blocky-adjacent tooling and HAOS both support this), which needs no
  router login at all in many homes. Catches the large majority of
  telemetry. Known-DoH-endpoint blocklists (HaGeZi ships one) raise the
  ceiling further.
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
| Self-manifest + LAN-auth rule (`firmware/common/attest/self_manifest.h`) | The warden face: periodic fleet attestation sweep, tamper/impostor flags as health claims |
| Card standards (`docs/standard/CANARY_CARDS.md`) | The per-device "who it talks to / mute" card |

**The proof moment** (the demo, the marketing, and the onboarding ending,
all at once): within the first hour, unprompted —

> *"14 devices found. Your TV called 47 tracking domains. Your Canaries
> called nobody."*

Then one tap to silence. No list URLs, no regex, no dashboard safari.
Curated plain-language toggles ("Block TV snooping", "Block ad trackers")
map to maintained HaGeZi/OISD lists underneath.

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
2. DHCP takeover vs router-setting walkthrough as the *default* path —
   which breaks fewer homes? (The wizard can try router auto-detect first
   and fall back.)
3. The aggregate schema: exactly which counters exist, at what time
   granularity, and where they live — must pass the invariants review
   before any storage code.
4. Warden sweep cadence and the impostor heuristic: what exactly flags a
   device advertising `_securacv._tcp` that fails authentication?
5. Product name clearance (§6) — attorney search before anything public.
6. Does the add-on ship in the default hub image or as a one-tap install?
   (Default-on witnessing with opt-in blocking is the current lean.)

## 8. What happens next (and what this doc is not)

This is a concept, staged like `canary-fence-guard`: no code, no add-on
skeleton, no store page. The sanctioned way to test demand is a concept
card on the website's ideas page. Concepts become candidates when people
ask for them — and when the open questions above close, this doc's status
line changes.
