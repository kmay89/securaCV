# 28 — Seeed Studio: partnership, SenseCraft teardown, and the open device commons

**Question this doc answers:** should we work with / partner with / launch through
Seeed Studio; what does their SenseCraft platform get right that we should absorb;
and how do we grow from "runs great on Seeed boards" to the thing 3D printing built —
one open, organized commons where *every* vendor's hardware is welcome, managed, and
clean, with a real community around it because the project is truly open.

**Verdict up front:**

1. **Seeed is already our de facto hardware partner — formalize it, cheaply and
   without exclusivity.** Seven of our fourteen registered boards are Seeed XIAO
   family — and all three `verified`-tier boards are Seeed; the Grove Vision AI V2 is a
   first-class Canary sensor (doc 10); Canary Vision Pro/Lite are designed around
   their reCamera; `suppliers.json` and doc 25 already name Seeed Co-Create for
   kitting. The partnership ladder in §6 starts at zero-commitment (a wiki PR and a
   Bazaar listing) and never requires giving up neutrality.
2. **SenseCraft is the right teardown target: copy the *feel*, refuse the
   *architecture*.** Their zero-to-working-in-minutes flow, public model library,
   and live tune-while-you-watch preview are genuinely excellent and we should match
   or beat each one (§4). Their cloud-account spine, metered inference, and
   Seeed-only device list are the exact weaknesses our invariants already forbid —
   doc 10's SenseCraft-path rejection stands unchanged (§5).
3. **The growth move is the slicer move, not the console move.** Marlin/Klipper
   gave us the support-tier model we already run (`docs/marlin_klipper_learnings.md`,
   `firmware/HARDWARE.md`). The next borrow is OrcaSlicer's: *vendor profiles as
   data, contributed by PR, verified for free, never sold*. Our `boards.json` +
   HAL + Sensor Adapter Contract are that registry in embryo — we should name it,
   document the on-ramp per vendor class, and recruit a non-Seeed second vendor to
   prove neutrality (§7).
4. **Community scaffolding is built but unstaffed — seed it with Seeed-style
   programs.** Their Ranger/Contributor/Co-Create programs are the best-run
   community engine in open hardware; our maker tiers, Maker Corps, vendors page,
   and builds gallery are the same shapes, currently empty. §8 maps each of their
   programs onto scaffolding we already shipped.

Everything below is sourced either from this repo (cited by path) or from public
Seeed/third-party material (linked in §11). Prices and plan limits are
**point-in-time (mid-2026)** and must be re-verified before anything customer-facing
quotes them.

---

## 1. Who Seeed is, and how deep we already are

Seeed Studio (Shenzhen, founded 2008, ~50M units shipped, self-described "The AI
Hardware Partner") is the largest open-hardware vendor in the AIoT space: XIAO
MCU boards, the Grove connector ecosystem (300+ modules), SenseCAP industrial
sensors, reTerminal HMIs, and the reCamera open AI camera line. They publish CAD,
schematics, and wiki source openly, and run manufacturing services (Fusion PCBA,
ODM/OEM, a licensing program) alongside the catalog.

We did not plan a Seeed dependency; we grew one because their hardware fits our
constraints (cheap, open, documented, castellated modules with published CAD).
The current touchpoints, all already in-repo:

| Touchpoint | Where | Depth |
|---|---|---|
| Canary WAP flagship board | XIAO ESP32-S3 Sense — `firmware/projects/canary-wap/` | `verified` tier |
| Canary Vision sensor | Grove Vision AI V2 (Himax WiseEye2) — doc 10, `docs/hardware/grove_vision_ai_v2_guide.md` | first-class program |
| Canary Sense radar | MR60BHA2 60 GHz + XIAO ESP32-C6 — `firmware/projects/canary-sense/` | shipping design |
| Canary Display puck | XIAO ESP32-S3 + Seeed Round Display | compile-tested |
| Canary Vision Pro / Lite | Seeed reCamera / reCamera Pro — `docs/hardware/canary_vision_pro_recamera.md`, doc 26 | design + B2B channel concept |
| Fence Guard concept | XIAO ESP32-S3 + Wio-SX1262 LoRa | concept |
| Vendor CAD in Board Room | `boards/vendor/` (XIAO, Grove Vision AI V2, Round Display STEPs) | shipped, with takedown clause |
| Kitting/assembly plan | `suppliers.json` ("a kitter like Seeed who makes the XIAO"), docs 19/22/25 (Co-Create royalties, module kitting) | planned |
| Board registry | 7 of 14 entries in `firmware/boards/boards.json` are Seeed (6 Waveshare, 1 Espressif) — and all 3 `verified`-tier boards are Seeed | structural |

Two conclusions fall out immediately. First: a Seeed partnership is not a new bet,
it is *recognizing sunk reality and getting paid for it* (co-marketing, supply
priority, kitting terms). Second: this concentration is itself a risk — §7 and the
tripwires in §10 are the counterweight, and we have prior art for the failure mode
(`firmware/HARDWARE.md` already documents Seeed silently revising the Round
Display and demoting a board's tier).

---

## 2. SenseCraft teardown — what they built

SenseCraft is Seeed's umbrella software suite. It is genuinely several products
under one brand, which is both its power (one name from hobby demo to industrial
dashboard) and its weakness (fragmented portals, uneven docs).

| Component | What it is | Openness | Runs where |
|---|---|---|---|
| **SenseCraft AI** | No-code edge-AI console: pick from a 400+ public model library → connect a Seeed board over WebUSB/WebSerial (Chrome-family only) → deploy → live preview with confidence/IoU sliders. Cloud training from uploaded images. | Platform closed; browse without account, **account required to deploy or share** | Browser + cloud; inference on-device |
| **SenseCraft HMI** | Cloud drag-and-drop dashboard designer for their screens/ePaper, AI-generates UIs from text, live data feeds | Closed, beta, free with daily AI-generation caps | Cloud designer → device render |
| **SenseCraft Data** | SenseCAP device management + telemetry store | Closed, built on Azure | Cloud |
| **ModelAssistant (SSCMA)** | Training/porting toolkit for embedded models; the AT protocol + Web toolkit around it | **Open source (Apache-2.0, GitHub)** | Local |
| **Watcher agent stack** | SenseCAP Watcher (~$79–99): on-device tinyML *trigger* (Himax) + cloud LLM *interpretation*; Home Assistant / Node-RED hooks | Hardware open (OSHW repo); service closed | Split local/cloud |

The Watcher economics are the part to study hardest, because they show where the
architecture leads. The free tier meters cognition: **200 LLM prompts/month, 3,000
image analyses/month, a 15-minute minimum gap between image analyses**; a $6.90
premium plan lifts you into usage-based billing. Fully-local operation exists but
as a premium *appliance* — the "Private LLM Pack" bundles a Watcher with an NVIDIA
Jetson AGX box running SenseCraft on-prem. Privacy is real in their lineup, but it
is an **upsell**, not the baseline.

And the device list is closed: SenseCraft AI's deploy targets are a hardcoded
per-board workspace list of Seeed products. Their own SenseCraft-Wio README *hopes*
for M5Stack and community ports; none shipped. SenseCraft is a moat around Seeed
hardware, not a commons.

We already litigated whether to build *on* this: **doc 10 §5 rejected the
SenseCraft production path** (vendor account in the loop, unsigned generic MQTT,
closed adapter firmware, raw-ish streaming) and kept exactly one job for it — the
web flasher that loads models onto the Himax module, offline, one-time, attended.
Since then the Lab grew its own WebSerial/XMODEM module flasher with a pinned,
SHA-256-verified model (`canary-local/devices/vision.json`, live on flash.html),
demoting SenseCraft from "required vendor step" to "documented fallback." That
direction — absorb the convenience, remove the account — is the template for
everything in §4.

---

## 3. Design comparison — SenseCraft vs. the SecuraCV surface

Same maker-facing promise ("hardware does something smart in minutes, no
toolchain"), opposite spines:

| Axis | SenseCraft | SecuraCV |
|---|---|---|
| Spine | Cloud account; device enrolls to platform | Local-first; no account exists to enroll in (`spec/invariants.md`, LICENSING.md) |
| Output | Raw-ish inference results / frames to app or cloud | Closed vocabulary of semantic claims, Ed25519-signed, hash-chained; no ordinary raw export by construction — the only path to raw bytes is the quorum-gated break-glass vault flow (`export_for_vault()` + `BreakGlassToken`, Invariant I) |
| Business model | Hardware + metered cognition (prompts/analyses per month) | Hardware, attestation, support; **never** bits or privacy (doc 21) |
| Trust story | "Trust Seeed + Azure" | Standalone verifier, TOFU-pinned keys, SLSA provenance, no phone-home |
| Device support | Seeed boards only, hardcoded list | `boards.json` registry, tiered, any vendor via data PR (`firmware/PORTING.md`) |
| Model story | 400+ public library, one-click deploy, cloud training | One pinned person-detect model, repo-verified; custom-model path documented but manual (doc 10 Phase 3) |
| First-five-minutes | Excellent: browser → board → live preview with sliders | Good and improving: Lab flasher, QR onboarding — but multi-surface and developer-flavored in places |
| Coherence | Fragmented (AI/HMI/Data/app portals) | One repo, one release button, one fleet dashboard; apps mirror one `fleet_model.h` |
| Community | Ranger/Contributor/Co-Create programs, staffed | Maker tiers/Corps/gallery/vendors scaffolding, unstaffed |

Read across the rows: **they beat us today on onboarding delight and model
abundance; we beat them structurally on trust, ownership, coherence, and
vendor-neutrality.** Their advantages are engineering effort we can copy. Ours are
architecture they cannot copy without abandoning their revenue model. That
asymmetry is the whole strategic opportunity.

---

## 4. The steal list — their good pieces, made ours (and better)

Each item: what SenseCraft does → the SecuraCV version that keeps our invariants.

1. **Zero-to-working in the browser.** Their three-step deploy is the best
   onboarding in embedded AI. Ours must equal it *without a login*: the Lab +
   web flasher already flash firmware and the vision model over WebSerial with
   hash verification. Gap to close: one continuous golden path — plug in → flash →
   provision (QR) → see a live *claim* (not a frame) — measured in minutes, on one
   page, for the WAP and Vision kits. Their metric is "model running"; ours is
   stronger: **"first signed claim verified on your own machine."** Make that the
   demo.
2. **Live preview with sliders.** SenseCraft lets you drag confidence/IoU and watch
   detections move — a tight, addictive tuning loop. Our privacy-correct
   equivalent already half-exists: the Aim card (boxes-only over local MQTT, off by
   default, auto-off) beats their USB-tethered preview *operationally* (couch vs.
   laptop-on-a-ladder — `vision.json` `why_not_sensecraft`). Close the loop by
   putting the runtime thresholds (`target`, confidence) next to the Aim card so
   tune-while-you-watch works end-to-end, locally.
3. **A model library as a commons.** 400+ browsable, one-click-deployable models is
   real leverage. Ours should be a **claims-first recipe library, not a model
   zoo**: each entry = pinned model + class map + claim vocabulary entry + bench
   validation checklist (exactly doc 10 Phase 3's shape, and doc 26's Recipe
   Library is the B2B twin). Every entry passes the invariant filter (no
   identity-capable outputs), is signed and manifest-pinned like the current
   vision model, and is contributed by PR like everything else. Smaller than 400,
   but every one of ours is *deployable unattended in a privacy-critical setting* —
   a bar none of theirs meets.
4. **Hardware modularity as product language.** reCamera's sensor-board/base-board
   split and the Grove connector make configuration legible to non-engineers. Our
   equivalents (Canary styles on one firmware layer cake, the enclosure
   configurator + fit coupon, Grove as our sensor bus already) should be *marketed*
   with the same clarity — the Sentinel tier table (Lite/Standard/Heavy) is
   already this; say it everywhere.
5. **Structured, funded community programs.** Not vibes — programs with names,
   ladders, and deliverables (Ranger 3.0's certified local partners; Contributor
   Program's 37 contributors → 82 shipped tasks in 2024; Co-Create's
   revenue-shared community products). §8 maps these onto our empty scaffolding.
6. **Open wiki culture.** wiki.seeedstudio.com is a public Docusaurus repo taking
   PRs. We are already stricter (repo-canonical, CI-checked, `builds.json`-as-
   database) — keep it, but adopt their *invitation posture*: label
   good-first-doc-tasks, credit every merged contributor on the site (the gallery
   lineage system already does credit; extend it to docs).

What we explicitly do **not** steal: the account spine, cloud training as default,
metered inference, HMI-in-the-cloud for local screens (Canary Display renders from
local fleet state by design), and AI-generated dashboards phoning home.

---

## 5. What stays non-negotiable (why we don't "integrate with SenseCraft")

Doc 10 §5's rejection table remains the controlling decision; nothing found in
this research weakens it — the Watcher plan limits strengthen it. The line we hold:

- **Seeed hardware: yes, enthusiastically.** Open, cheap, documented, and they
  keep shipping exactly the sensors our modalities need.
- **SenseCraft as a runtime dependency: never.** No vendor account, no cloud
  broker, no unsigned event path, no metered anything between a Canary and its
  fleet. The only sanctioned uses are offline/bench: their model flasher as a
  documented fallback, and SSCMA/ModelAssistant (genuinely open) as a training
  toolchain feeding our pinned-model pipeline.

This also defines the *pitch* to Seeed, which is stronger than it sounds: we are
the answer to the customer their cloud story loses. "reCamera/XIAO/Grove are
lovely; SenseCraft's account requirement is a dealbreaker for my
privacy/compliance case" is a real segment (their own forums show it — it's why
they built the Private LLM Pack). SecuraCV turns that objection into a hardware
sale for them at zero cloud cost to serve. We are not a competitor to their
platform; we are a second software brain for their catalog — the ESPHome to their
vendor app.

---

## 6. The partnership ladder with Seeed

Ordered by commitment; each rung is independently worthwhile, none requires
exclusivity, and every rung keeps §5 intact. (Counterpart programs verified
mid-2026: Fusion Co-Create, ODM/OEM/Licensing, IIoT Partner Program, Ranger 3.0.)

- **Rung 0 — Be visible in their ecosystem (now, free).**
  PR a "SecuraCV / Canary Vision" recipe into their open wiki (they take
  community pages; our Grove Vision AI V2 + XIAO guide is better than most vendor
  content). List finished Canary kits on the Bazaar marketplace when the store
  exits preview. Pitch their blog's case-story series — "an open-source privacy
  witness built on XIAO/Grove" is exactly what that series runs.
- **Rung 1 — Supply & kitting (doc 25's plan, activated).**
  Fusion/Co-Create quote for module kitting of the Vision kit (Grove Vision AI
  V2 + XIAO + harness, bagged) with our signed self-test as acceptance. This is
  the "committed batch" lane doc 25 already designed; Seeed is simply the named
  overseas source, with the US box-build hedge unchanged.
- **Rung 2 — Co-Create launch SKU.**
  Submit a Canary (WAP or Vision) as a Fusion Co-Create product: they
  manufacture, list, and sell an open design and pay a royalty (precedent:
  OpenUC2 microscope, XIAO PowerBread — open projects, revenue-shared). This is
  the lowest-capital "launch with Seeed" move: their store traffic + our design,
  no inventory risk on our side, Apache-2.0 and the trademark rules
  (TRADEMARK.md) already define what they may call it. Doc 19 already counts
  Co-Create royalties among the passive commission rails.
- **Rung 3 — Licensing-program co-brand.**
  Their licensing program ships proven Seeed hardware under a partner's brand and
  firmware. Inverted, it's our "sold by partners" model (ecosystem.html) with
  Seeed as the first listed vendor: a "Canary Vision — built by Seeed, verified
  SecuraCV" unit, pre-flashed with our open firmware, self-test-signed, on our
  empty `vendors.json` page under the existing listing rules (re-flashable to
  stock, open shipped configs, no ranking).
- **Rung 4 — reCamera Pro program.**
  Doc 26's industrial channel already rides reCamera Pro (RV1126B, 3 TOPS, 4K,
  PoE — shipping 2026). A joint reference design — "reCamera Pro running the
  SecuraCV witness stack, on-device inference, coarse claims only" — is the
  co-marketing rung, and it lands on the Sensor Adapter Contract, not on firmware
  forks (§7). This is also the rung where their Ranger network matters: certified
  local partners doing on-site installs is precisely the physical-work outlet
  doc 26 refuses to do ourselves.

What we ask for at each rung is boring and cheap for them: stock visibility/EOL
notice on the boards we verify (their silent-revision habit is our top supply
risk), a contact for the takedown-clause CAD we mirror, and co-marketing. What
they get: the privacy-segment sale their platform can't close, a flagship
open-source case study, and pull-through on XIAO/Grove/reCamera.

---

## 7. The 3D-printing move: one commons, every vendor, kept clean

The user-visible ambition: *support all of Seeed's stuff AND other companies',
while keeping it managed, organized, and clean — the way 3D printing did.*

3D printing's playbook, concretely:

- **Open interchange formats** (STL/3MF, G-code) let any printer join.
- **Firmware as commons** — Marlin, then Klipper, supported hundreds of printers
  from one codebase via per-machine *data* (`printer.cfg`), not forks.
- **Slicer vendor profiles** — Cura/PrusaSlicer/OrcaSlicer ship per-printer JSON
  profiles; vendors and community contribute by PR; OrcaSlicer's Partner Program
  verifies vendors and ships official profiles **free of charge — "status is
  granted on review and is never sold."**
- **A model commons** (Printables et al.) plus remix lineage made contribution the
  default behavior.

The result: a new printer vendor launches "works with OrcaSlicer" on day one, and
the neutral hub — not any one vendor — owns the ecosystem's center. That center is
the position SecuraCV should take for privacy-first physical sensing.

We already own every primitive; doc 17 argued the history, and
`docs/marlin_klipper_learnings.md` + `firmware/HARDWARE.md` already imported the
tier system. What's missing is *naming the registry, opening the on-ramps per
vendor class, and proving neutrality with a second vendor.*

**The three on-ramps (all existing surfaces, no new architecture):**

1. **MCU boards → the board registry.** `boards.json` + `flavors.json` + HAL +
   `PORTING.md`'s contract ("a new board is a data contribution, not a code
   fork") *is* the slicer-profile model: pins as data, CI builds every board on
   every PR, tiers with evidence, promotion via the Hardware Test Report template.
   Waveshare (6 boards) and the generic Espressif DevKit already prove non-Seeed
   entries work. Treat `boards.json` as our `vendor/*.json` profile directory and
   document "add your board in one PR" as a vendor-facing page, not just a
   contributor doc.
2. **Smart sensors/modules → the capability seam.** Grove Vision AI V2 shows the
   shape: the module does perception, our firmware owns the chokepoint (FSM,
   coarsening, signing). Any vendor's person-detect module that can speak
   boxes-over-I2C/serial can slot behind the same `HAS_*` capability. M5Stack,
   LILYGO, Adafruit, SparkFun modules are all candidates the moment someone files
   a Hardware Test Report.
3. **Linux-class devices & third-party systems → the Sensor Adapter Contract.**
   `spec/sensor_adapter_contract_v0.md` is explicitly vendor-neutral, with six
   adapters already shipping as Cargo features (Frigate, MQTT sensor, webhook,
   BLE presence, Meshtastic, CAN bus — plus auxiliary TLS and sandbox flags). reCamera lands here — and so can *competitors'* cameras
   (ONVIF/Frigate paths), which is exactly the point: the commons is credible
   only if a Seeed rival's device is welcome. The narrow `Claim` type keeps every
   adapter inside the invariants no matter whose silicon feeds it.

**Keeping it clean (the part 3D printing half-failed at):** Thingiverse rotted and
early Cura profile sprawl was chaos; our defenses are already designed —
CI-enforced registry checks (`check_board_registry.py`), tier evidence with
demotion, the coplanar/normals-style "tests enforce the lesson" culture,
`vendors.json` listing criteria (re-flashable, open configs, no ranking, no paid
placement), and TRADEMARK.md's free-by-grant "Works with SecuraCV" badge — our
OrcaSlicer Partner Program equivalent, likewise never sold. The one new artifact
worth writing is a short **conformance checklist per on-ramp** (board / module /
adapter): what to test, what evidence to attach, what the badge means. That's the
whole "managed, organized, clean" story: state in git, checks in CI, badges not
gatekeeping (ecosystem.html's words, already published).

**Neutrality proof:** recruit and land one non-Seeed vendor visibly — the
realistic first candidates are Waveshare (already six compile-tested boards; ask
them for a Hardware Test Report + listing) or an M5Stack camera/PIR unit via
on-ramp 2. The Seeed rungs in §6 and this section are deliberately compatible:
Seeed gets first-partner prominence, never exclusivity — same as Prusa's
relationship to PrusaSlicer forks.

---

## 8. Community: their engine, our scaffolding

Seeed runs the best-structured community programs in open hardware. Ours are
designed (arguably better — two-sided, no backend, credit-forever) but empty.
The mapping writes its own to-do list:

| Seeed program | What it does | Our counterpart (built) | Activation step |
|---|---|---|---|
| Contributor Program | Scoped tasks (docs, code, ports) for credit/reward; 37 contributors → 82 tasks in one year | Gallery lineage credit, maker tiers (Apprentice→Steward), CONTRIBUTING.md | Publish a labeled task board: good-first Hardware Test Reports, recipe-library entries, doc gaps. Credit merged work on-site automatically (extend `builds.json` pattern to contributions) |
| Ranger 3.0 | Certified local technical partners run events, installs, feedback loops | Maker Corps + Certified Maker tier ("earned, not bought") | Certify the first 3–5 makers through the existing tier ladder; hand them the doc 26 install/physical-work channel |
| Fusion Co-Create | Community designs manufactured & sold, revenue-shared | Ecosystem page's "kits built and sold by partners", empty `vendors.json`, Print-It-Forward plan (doc 19) | Rung 2 of §6 makes *us* the first Co-Create case; then invite community Canary variants (Sentinel tiers are natural candidates) |
| Open wiki (PR-able Docusaurus) | Low-friction doc contribution, vendor-hosted | Repo-canonical docs + CI, stricter and more durable | Adopt the invitation posture: "edit this page" links, doc-task labels, contributor credits page |
| Bazaar marketplace | Distribution for community/partner hardware | `/vendors` page rules (WLED-style, neutral, alphabetical) | Seed it: first entry per §6 rung 3 or the first Certified Maker selling kits |

The structural advantage to press: Seeed's community ultimately feeds a closed
platform and one vendor's catalog. Ours feeds a commons where the contributor's
board/recipe/kit works *everywhere*, is credited *forever* (remix lineage), and
can be *sold by them* under the badge rules with money that never touches us.
"Truly open" is not the license line — Seeed's hardware licenses are fine — it's
that no account, no cloud, and no vendor sits between a contributor's work and a
user's fleet. That is the community pitch, verbatim.

---

## 9. Launch play (sequenced, mostly free)

1. **Now (docs/PRs, $0):** Seeed wiki recipe page; Hackster/blog case-story pitch;
   "add your board" vendor page carved out of PORTING.md; conformance checklists;
   contributor task board.
2. **Store-preview exit:** Bazaar listing of Vision/WAP kits; first `vendors.json`
   entry; Co-Create submission (§6 rung 2) so the launch has their channel behind
   it.
3. **First committed batch (doc 25 trigger):** Fusion kitting quote + US hedge;
   ask for stock/EOL visibility on `verified` boards as part of the PO.
4. **reCamera Pro reference build (doc 26 pilot):** joint writeup, adapter-contract
   integration, Ranger-network install partner for the pilot customer.
5. **Neutrality milestone (before any co-marketing goes loud):** one non-Seeed
   vendor landed in the registry with a Hardware Test Report, so the story is
   "open device commons, Seeed first among equals" — never "a Seeed accessory."

---

## 10. Tripwires and risks

- **Single-vendor concentration.** 7/14 boards including every verified tier, one supplier
  country. Mitigations: §7's second-vendor milestone; keep the Espressif-generic
  env verified-track; doc 25's dual-source rule. *Tripwire: if a XIAO EOL/revision
  would strand a shipping kit, the registry must already hold a compile-tested
  substitute.*
- **Silent hardware revisions** (already bitten once — Round Display). Ask for
  revision notice in any Rung-1+ agreement; keep the demotion policy ruthless.
- **SenseCraft scope creep.** Any future convenience that reintroduces their cloud
  into a runtime path fails doc 10 §5 by default; the burden of proof is on the
  integration, permanently. The Lab's own module flasher must stay ahead of the
  fallback path so the account-free story never regresses.
- **Brand capture.** Co-marketing must follow TRADEMARK.md: they may say "works
  with SecuraCV" under the grant; we never appear as an official SenseCraft
  anything. The neutrality milestone (§9.5) gates loud co-marketing.
- **Program overreach.** Seeed staffs its community programs; we are one person +
  systems (doc 20). Activate programs only in the self-running shapes already
  designed (PR-editable pages, CI checks, badges) — no program that requires an
  operator in the loop.
- **Stale numbers.** Watcher plan limits, reCamera specs/prices, and program terms
  are mid-2026 snapshots; re-verify before quoting anywhere customer-facing.

---

## 11. Sources

**In-repo:** docs 10, 16, 17, 19, 20, 21, 25, 26; `firmware/HARDWARE.md`,
`firmware/PORTING.md`, `firmware/boards/boards.json`,
`docs/marlin_klipper_learnings.md`, `spec/invariants.md`,
`spec/sensor_adapter_contract_v0.md`, `canary-local/devices/vision.json`,
`docs/hardware/canary_vision_pro_recamera.md`; website repo: `ecosystem.html`,
`vendors.json`, `suppliers.json`, TRADEMARK.md, LICENSING.md.

**Seeed (public, mid-2026):**
- SenseCraft platform & pricing: <https://sensecraft.seeed.cc/en>,
  <https://sensecraft.seeed.cc/pricing/>, <https://sensecraft.seeed.cc/ai/model/>,
  <https://wiki.seeedstudio.com/sensecraft_ai_overview/>,
  <https://wiki.seeedstudio.com/sensecraft_hmi_overview/>,
  <https://wiki.seeedstudio.com/sensecraft-fee/sensecraft-cloud-fee/>,
  <https://wiki.seeedstudio.com/watcher_price/>
- Watcher: <https://www.seeed.cc/product/sensecap-watcher-the-physical-ai-agent-for-smarter-spaces>,
  <https://github.com/Seeed-Studio/OSHW-SenseCAP-Watcher>,
  <https://www.hackster.io/news/seeed-studio-s-sensecap-watcher-combines-local-tinyml-with-cloud-llms-to-deliver-smart-monitoring-85f81423e2ad>,
  <https://www.cnx-software.com/2024/09/19/the-sensecap-watcher-voice-controlled-physical-ai-agent-for-llm-based-space-monitoring/>
- reCamera: <https://github.com/Seeed-Studio/OSHW-reCamera-Series>,
  <https://wiki.seeedstudio.com/recamera_hardware_and_specs/>,
  <https://www.cnx-software.com/2026/07/17/recamera-pro-open-ai-camera-supports-computer-vision-llm-vlm-stt-and-tts-workloads/>,
  <https://petapixel.com/2024/08/26/the-50-modular-recamera-aims-to-be-the-most-advanced-ai-camera/>
- Programs: <https://www.seeedstudio.com/blog/2025/05/19/odm-vs-oem-vs-licensing-program-services-you-should-know-from-seeed-studios-customization-offerings/>,
  <https://www.seeedstudio.com/co-create.html>,
  <https://www.seeed.cc/customization>,
  <https://solution.seeedstudio.com/partner-program/>,
  <https://www.seeedstudio.com/blog/2026/01/19/ranger-program-3-0-building-the-on-ground-infrastructure-for-open-aiot/>
- Open-source repos: <https://github.com/Seeed-Studio/ModelAssistant>,
  <https://github.com/Seeed-Studio/SenseCraft-Wio>,
  <https://github.com/Seeed-Studio/wiki-documents>

**3D-printing model:**
- OrcaSlicer Partner Program (free verification, profiles never sold):
  <https://www.orcaslicer.com/partner-program/>
- Profile-contribution model: <https://github.com/OrcaSlicer/OrcaSlicer/wiki/How-to-create-profiles>
