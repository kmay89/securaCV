# 30 — Supplier breadth: who we're missing, how the mix reads from outside, and the customer hardware roadmap

**Questions this doc answers:** (a) beyond Raspberry Pi, Seeed, Waveshare, and
Espressif, which board vendors and brands are worth adding — and which are
non-goals we should stop re-litigating; (b) to an outsider, does the current
hardware mix read as vendor capture ("they're a Seeed shop") or as the natural
starting lineup; (c) what is the phased, customer-facing roadmap that turns
board support into "more people can run SecuraCV on hardware they already own."

**Companion docs:** [`docs/board_market_research.md`](../board_market_research.md)
(the installed-base numbers this doc builds on),
[doc 28](28-seeed-studio-partnership-and-open-device-commons.md) (the Seeed
relationship and the open device commons),
[doc 25](25-supplier-and-volume-production.md) (the supplier/volume path),
[`firmware/HARDWARE.md`](../../firmware/HARDWARE.md) (support tiers and scope),
[`docs/hardware/flagship_board_program.md`](../hardware/flagship_board_program.md)
(the custom carrier).

**Verdict up front:**

1. **The chip bet stays. The brand mix widens on top of it.** Espressif is the
   only silicon family that covers all three witness pillars — camera, Wi-Fi
   CSI, cheap mesh radio — at Canary prices
   ([board research](../board_market_research.md)). Adding a second *chip*
   vendor would buy a feature-stripped build chasing a smaller installed base.
   Adding second and third *board brands on the same chip* is nearly free, and
   that is where every recommendation below lives.
2. **The biggest miss is not a new vendor — it is the classic ESP32 port.**
   The ESP32-CAM and WROOM-32 DevKit are the largest owned base of
   camera-capable dev boards on Earth, and one compile-tested port unlocks
   *every* brand that ships them (AI-Thinker, Freenove, DFRobot, and dozens of
   white-label makers) in a single move. This was already the top
   recommendation of the board research; this doc makes it Phase 0 of the
   customer roadmap.
3. **On perception: natural to makers today, one press cycle away from "a
   Seeed accessory."** By *count* the registry is already majority non-Seeed
   (7 of 17 boards are Seeed, 9 are Waveshare, 1 generic Espressif) — but by
   *evidence* it is 100% Seeed: all three `verified`-tier boards, the flagship
   WAP, the radar, the vision module, the display puck, and the planned pro
   camera are one vendor. Insiders read that as "the community-default
   hardware, sensibly chosen." A reviewer writing a headline reads it as
   capture. The fix is already specified (doc 28 §9.5) and cheap: promote one
   Waveshare board on real hardware evidence *before* any loud co-marketing,
   and keep the generic Espressif environment alive.
4. **"Stick with this for now" is the right answer at every layer except
   reach.** No new chip family, no new SBC ports, no new sensor-vendor
   commitments are needed. What is needed is reach (classic ESP32, pin maps
   for the boards people already own), one visible neutrality proof, and one
   genuine capability gap filled (wired Ethernet/PoE for installs where Wi-Fi
   is the wrong transport).
5. **The customer roadmap is milestone-gated, not date-gated.** Each phase
   below is named by what a customer can *newly do*, and each promotion runs
   on the existing evidence machinery (`tier_evidence`, Hardware Test Reports,
   the figures.json confidence ladder) — the honesty rules don't bend for
   marketing.

Point-in-time note: registry counts, prices, and vendor lineups below are an
early-August-2026 snapshot of this repo and public catalogs; re-verify before
quoting anywhere customer-facing.

---

## 1. Where we actually are (the audit an outsider would run)

What a diligent outsider finds in an afternoon, all public in this repo:

| Layer | Fact on disk | Where |
|---|---|---|
| Chip | Every firmware target is Espressif (S3/C3/C6); classic ESP32 named as the gap | [`firmware/boards/boards.json`](../../firmware/boards/boards.json), [board research](../board_market_research.md) |
| Boards | 17 registered: 7 Seeed XIAO, 9 Waveshare, 1 generic Espressif DevKit | same registry |
| Tiers | All 3 `verified` boards are Seeed; every Waveshare board is `compile-tested` | same registry |
| Devices | Shipping devices (WAP, Vision, Sense, Doorbell) are built on Seeed modules end-to-end: XIAO, Grove Vision AI V2, MR60BHA2, Round Display | [`canary-local/devices/figures.json`](../../canary-local/devices/figures.json) |
| Hub | Raspberry Pi 4/5 official HAOS images, or any Linux box via Docker | [`canary-local/devices/hub_image.json`](../../canary-local/devices/hub_image.json), `scripts/install.sh` |
| Adapters | Six vendor-neutral lanes shipping (Frigate, MQTT, webhook, BLE presence, Meshtastic, CAN) | [`spec/sensor_adapter_contract_v0.md`](../../spec/sensor_adapter_contract_v0.md) |
| Exit ramp | A custom S3 carrier (`idea` stage) on the pre-certified WROOM-1 module, pin-identical to the XIAO | [`boards/canary-witness-s3/`](../../boards/canary-witness-s3/), [flagship program](../hardware/flagship_board_program.md) |
| Strategy | A published plan to formalize a Seeed partnership — and its own tripwires | [doc 28](28-seeed-studio-partnership-and-open-device-commons.md) |

Note the drift worth recording: doc 28 was written against a 14-board registry
("7 of 14, 6 Waveshare"). The registry has since grown to 17, and Waveshare now
*outnumbers* Seeed by board count. The concentration story is no longer "mostly
Seeed boards"; it is "only Seeed boards have hardware evidence." That is a
narrower, cheaper problem — one Hardware Test Report on one Waveshare board
changes the sentence an outsider can write.

---

## 2. How the mix reads, audience by audience

**To the maker/ESPHome crowd (our lead segment, doc 08): natural.** The XIAO
ESP32-S3 Sense is the community-default camera board; Waveshare is the
community-default display line; the generic DevKit entries prove the tree isn't
locked. Community guides independently rank S3/C3/C6 exactly as our registry
does. Nobody in this audience asks "why Seeed" — they ask "does it run on the
board I have," which is a *reach* question (§3), not a neutrality question.

**To a reviewer or journalist: one sentence from capture.** "Every board they've
actually verified is from the same Shenzhen vendor they're planning a
partnership with" is a true sentence today, and it is the sentence a skeptical
writeup leads with. The counterweights exist (HARDWARE.md's vendor-neutral
tiers, the adapter contract, doc 28's tripwires) but they live in strategy docs
an outsider won't read. The defense that actually lands is a *fact*, not an
argument: a non-Seeed board at `community` or `verified` tier, with the
evidence linked from the registry. Doc 28 §9.5 already gates loud co-marketing
on exactly this; this doc adds only urgency and a concrete first candidate
(§4, Phase 1).

**To a supply-chain skeptic: the real single point is Espressif, and we should
say so plainly.** Seeed concentration is a board-brand question; every radio we
ship is one chip vendor regardless of whose logo is on the carrier. That is an
honest, defensible position — it is the industry default for this class of
device, it is the only family meeting the witness pillars, and the mitigations
are structural rather than cosmetic: the pre-certified WROOM module strategy
(doc 29), the dual-source supplier rule (doc 25), the carrier board that makes
the module the swappable seam (flagship program), and the Linux-class adapter
contract that runs on anyone's silicon. The move is disclosure, not
diversification theater: [`docs/supply_chain_transparency.md`](../supply_chain_transparency.md)
is the right home, and the "why these boards" rationale
([board research](../board_market_research.md)) should be linked from
user-facing docs, not just strategy.

**Bottom line on the user's question:** we did not get taken; we grew a
dependency for defensible reasons (cheap, open CAD, documented, castellated
modules — doc 28 §1 says the same). But "defensible" and "self-evidently
neutral" are different states, and the gap between them closes with one
promoted board and one published rationale — both already on the books.

---

## 3. Who we're missing — ranked, with the honest "no"s

The scope rule from [`firmware/HARDWARE.md`](../../firmware/HARDWARE.md)
governs: a board belongs in the tree when it serves a witness role, is
obtainable, and holds the privacy invariants. Ranked by customer reach per
unit of maintainer effort:

| # | Addition | What it buys | On-ramp | Effort | Verdict |
|---|---|---|---|---|---|
| 1 | **Classic ESP32 port** (`esp32cam-ai-thinker`, `esp32-wroom-devkit`) | The largest owned camera-board base in existence; covers AI-Thinker, Freenove, DFRobot, and every white-label ESP32-CAM at once; keeps camera + CSI pillars | Board registry, `compile-tested` | One port, well-trodden gotchas (UART flashing, pin overlap) already documented in the board research | **Do — Phase 0** |
| 2 | **ESP32-C3 Super Mini pin map** | The most-owned sub-$3 board in the hobby; we already build `xiao-esp32c3` | Board registry | Nearly free | **Do — Phase 0** |
| 3 | **Freenove ESP32-S3-WROOM CAM pin map** | The default Amazon ESP32-S3 camera kit — the board a US newcomer actually receives in two days | Board registry | Small (S3 + OV2640, near-sibling of existing S3 targets) | **Do — Phase 0** |
| 4 | **Waveshare promotion** (one display board to `community`/`verified`) | The neutrality proof; also honesty for a line we already sell enclosures for | Hardware Test Report on an existing entry | One bench session + one registry edit | **Do — Phase 1, before co-marketing** |
| 5 | **M5Stack** (CamS3-class unit, or a PoE unit) | Second-vendor camera hardware behind the capability seam; huge installed base; doc 28 already names them a commons candidate | Module seam (on-ramp 2) or board registry | Medium | **Invite — Phase 2**; ideal community contribution |
| 6 | **Olimex ESP32-POE-ISO** (or M5Stack PoE) | A genuinely missing *capability*: wired Ethernet/PoE for install-grade witnesses where Wi-Fi is wrong (basements, metal buildings, code-required wiring); OSHW-certified, EU-sourced — a nice neutrality data point too | Board registry (transport/display roles; no camera on these boards) | Medium (Ethernet PHY is a new HAL surface) | **Do when an install customer asks — Phase 2** |
| 7 | **LILYGO** (T-Camera S3 class) | Budget AliExpress reach | Board registry | Small port, but silent-revision risk is highest in class — demotion policy must be ruthless | **Accept by community PR; don't lead** |
| 8 | **Adafruit / SparkFun** | US-sourcing hedge, education-channel credibility; already in BOMs as breakout parts (`ADA-*` classes) | Board registry (Feather ESP32-S3 class — no camera variants) | Small | **Accept by community PR**; value is channel, not silicon |
| 9 | **DFRobot FireBeetle 2 ESP32-S3** | Camera-header S3 board from a credible third vendor | Board registry | Small | **Accept by community PR** |

And the honest "no"s, so we stop re-deciding them:

- **Other Linux SBCs (Radxa, Orange Pi, Banana Pi, …):** not board ports —
  they are already served by the generic Linux/Docker hub lane. The right
  artifact is one sentence in the hub docs ("any aarch64/x86 Linux box works;
  Pi 4/5 gets the official image"), not per-board support. Pi keeps the
  flashed-image path because it has the installed base (~75M) to justify it.
- **RP2040/RP2350, Arduino AVR/R4, STM32, nRF52 as firmware targets:** each
  fails at least two of camera/CSI/Wi-Fi; unchanged from the board research.
  (nRF52 stays exactly where it is: idea-stage LoRa/wearable research.)
- **ESP32-P4:** re-evaluate only if on-device vision outgrows the Grove
  module's NPU ([roadmap review](../review/02-roadmap.md)); not before.
- **A second chip family "for optics":** the most expensive way to buy a
  neutrality story that one Hardware Test Report buys for free.

---

## 4. The customer hardware roadmap

Phases are gated by evidence, not dates, and named by the new customer
sentence each one makes true. Every promotion runs through the existing
machinery: `boards.json` + `tier_evidence`, Hardware Test Report issues,
the figures.json confidence ladder, and the two flashers' catalogs
([`canary-local/devices/flash.json`](../../canary-local/devices/flash.json)).

### Phase 0 — "The board in your drawer probably runs SecuraCV."

The reach phase. All compile-tested entries; zero hardware obligation on
maintainers; the flasher catalogs grow as ports land.

- Port classic ESP32: `esp32cam-ai-thinker` and `esp32-wroom-devkit`
  (items 1 above; scope and friction already scoped in the
  [board research](../board_market_research.md) — no native USB means the
  browser flasher's chip guard needs the UART story told honestly, or these
  ship desktop-Flasher-first).
- Add the C3 Super Mini and Freenove S3 CAM pin maps (items 2–3).
- Wire the "I have some random board" intake
  ([`docs/unflashed_board_intake.md`](../unflashed_board_intake.md)) into the
  getting-started path so an unlisted board routes to PORTING.md instead of a
  dead end.

### Phase 1 — "Nobody's brand gates your trust in this."

The neutrality phase — sequenced *before* any loud Seeed co-marketing
(doc 28 §9.5).

- Bench one Waveshare display board through its runbook; file the Hardware
  Test Report; promote to `community` tier (item 4).
- Publish the vendor-facing "add your board in one PR" page carved out of
  [`firmware/PORTING.md`](../../firmware/PORTING.md), plus the per-on-ramp
  conformance checklists doc 28 §7 calls for.
- Link the board rationale and
  [supply-chain transparency](../supply_chain_transparency.md) from
  user-facing docs — the disclosure move from §2.
- Seed the website's `vendors.json` with its first entry.

### Phase 2 — "It installs where Wi-Fi and USB power can't go."

The capability phase — additions chosen for what they let a customer *deploy*,
not for logo count.

- Wired Ethernet/PoE lane (item 6) when the first install-grade customer
  materializes — likely alongside the doc 26 Pro channel.
- First second-vendor sensing module (item 5, M5Stack-class) behind the
  capability seam, proving the module on-ramp with silicon Seeed doesn't make.
- LILYGO/Adafruit/SparkFun/DFRobot entries land here as community PRs — the
  posture is "welcome and CI-built," not "maintainer-owned."

### Phase 3 — "Your existing cameras can join the witness."

The adapter phase — no new boards at all; this is telling the story of lanes
that already ship.

- Document Frigate/ONVIF-path onboarding as a first-class "bring your own
  camera" journey (competitors' hardware included — that is the commons
  being credible, doc 28 §7).
- The hub-on-anything story (Pi image, generic Linux, Docker, Jetson/Coral
  acceleration) presented as one decision page instead of scattered setup
  docs.
- reCamera Pro/Lite stay honest: `idea` on the ladder until hardware exists.

### Phase 4 — "The module is the seam, and it's dual-sourced."

The volume phase, triggered by committed batches (doc 25), not by calendar.

- The Witness S3 carrier ([flagship program](../hardware/flagship_board_program.md))
  moves off `idea` only through its own gates; at that point the pre-certified
  WROOM-1 module — not any board brand — becomes the supply seam, and doc
  25's dual-source rule applies to kitting.
- SDoC timing per doc 29: certify the board we intend to keep, not the board
  we intend to leave.

---

## 5. Tripwires

- **Reach without honesty.** A flood of compile-tested entries must never
  blur the tier line — the badge machinery
  ([`check_board_registry.py`](../../firmware/scripts/check_board_registry.py))
  already enforces this; keep it ruthless as the registry grows.
- **The neutrality milestone slips behind the partnership.** If any Seeed
  co-marketing goes loud before a non-Seeed board holds tier evidence, §2's
  bad headline writes itself. Doc 28 §9.5 is the gate; treat it as hard.
- **Capability additions without a customer.** The PoE lane is pulled by an
  install customer, not pushed — a port nobody deploys is maintenance debt
  with a logo on it.
- **Stale counts.** The 7/9/1 registry split and every vendor lineup here
  will drift exactly like doc 28's "7 of 14" did. Anything quoting these
  numbers customer-facing must re-derive them from `boards.json`, not from
  this doc.
