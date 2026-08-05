# Glossary — every name this project uses, defined once

SecuraCV has a lot of proper nouns: a platform, a device, a daemon, five
firmware products, three radio channels, a dozen surfaces. This page is the
**one place each of them is defined**, so a newcomer — or an AI assistant
answering someone's question — never has to guess from context.

**How to use it:** each entry is one sentence of "what it is," then where the
real detail lives. Where a name is *often gotten wrong*, the entry says so
explicitly. If this page disagrees with the doc it points at, the doc wins and
this page is the bug.

Related: [the docs map](README.md) · [FAQ](FAQ.md) ·
[brand & positioning](BRAND.md) (canonical naming) ·
[the tree map](CONSOLIDATION.md) (which directory is what) ·
[`spec/witness_dictionary.json`](../spec/witness_dictionary.json) (the
machine-readable vocabulary CI checks)

---

## The three names people mix up

These are the ones that cause the most confusion. Get these right and the rest
follows.

| Name | What it is | Not |
|---|---|---|
| **SecuraCV** | The **platform** — the witness kernel plus the signed evidence log. The "how." | Not the device, not the company |
| **Canary** | The **device**, and the brand people say out loud. The hero noun. | Not the software |
| **Errer Labs** | The **company** that makes it and holds the trademarks. | Not the product |

Say "a Canary," "your fleet of Canaries," "SecuraCV runs on it." Don't make
someone learn "witness kernel" before they understand what they're buying.
Canonical rules: [`docs/BRAND.md`](BRAND.md).

**Casing.** The platform is written **`SecuraCV`** in all prose, UI, store
listings, and legal text — capital S, capital CV. It is an initialism for
**Secure Computer Vision**. The code layer stays lowercase `securacv` (domains,
package names, bundle IDs, MQTT topics). The repository slug `securaCV` is
grandfathered code-layer history, *not* a third style to copy into prose.

---

## Core concepts

**Witness kernel** — The Rust daemon and library at [`src/`](../src) that turns
camera or sensor input into signed, hash-chained events. Ships as the
`witnessd` binary. This is "the product" in code terms; everything else in the
tree is a wrapper, a device, or a surface.
→ [`kernel/architecture.md`](../kernel/architecture.md),
[the whitepaper](securaCV_whitepaper.md)

**Witnessing (vs. watching)** — The project's central distinction. A watcher
keeps footage and can be asked who was in it. A witness keeps a *record of what
happened* that can be checked later, and structurally cannot answer "who."
→ [why witnessing matters](why_witnessing_matters.md)

**Event** — The unit of output: a short semantic statement like "large object
crossed boundary" or "presence in restricted zone," with a coarse timestamp and
a zone, and no identity. The full vocabulary is enumerated in
[`spec/witness_dictionary.json`](../spec/witness_dictionary.json) and specified
in [`spec/event_contract.md`](../spec/event_contract.md).

**Sealed event / sealed log** — Each event carries a hash of the previous one
and an Ed25519 signature, so the log is *append-only in evidence terms*: you can
add to it, but you cannot silently edit or delete history without the chain
failing verification. Tampering is not prevented — it is made **visible**.
→ [log verification](log_verify.md), [logging](logging.md)

**Evidence envelope** — The versioned, self-verifying container an export ships
in, with a dual Rust↔JS verifier so a recipient can check it without our tools.
→ [`spec/evidence_envelope.md`](../spec/evidence_envelope.md),
[evidence lifecycle](evidence_lifecycle.md)

**Break-glass** — The only path to raw footage: a time-bounded, audited unseal
that requires **n-of-m trustee approvals**. No single person — including the
owner, including us — can open a sealed envelope alone.
→ [`spec/break_glass.md`](../spec/break_glass.md),
[the operator guide](operator_guide.md),
[why exports work this way](why_secure.md)

**Trustee / quorum** — The people who hold approval keys, and the `n-of-m`
policy saying how many of them must sign for a break-glass request to
authorize. Configured once, then rehearsed — see the Operator's Bench in
the Lab.

**The Vault** — The sealed store of raw snapshots that break-glass opens.
→ [sealed snapshot vault](sealed_snapshot_vault.md)

**Coarse timestamps** — Event times are bucketed (10-minute windows) on
purpose, so the log can prove *that* something happened without becoming a
minute-by-minute behavioral diary. Precision is treated as a privacy cost, not
a feature.
→ [timestamping](timestamping.md)

**Attestation tier** — How much the system is willing to vouch for where an
event came from: `device` (the device attested it), `adapter` (an adapter host
did), `ha-bridged` (it arrived via Home Assistant). Enumerated in the witness
dictionary; the tier is shown to the user rather than flattened away.

**Verified** — A reserved word. It means *an Ed25519 signature checked against
a pinned key* — nothing looser. Never use it to mean "probably fine," "seen," or
"heard nearby." (See **presence** below for the deliberately weaker word.)

**Presence** — The weaker claim: "I heard this device recently," self-reported
and **unsigned**. Used for the nearby-device roster. Never a trust boundary.

**Randomart / trust card** — A drunken-bishop ASCII rendering of a device's key
fingerprint, so a human can compare a device's identity at a glance instead of
reading hex. Printed by the device and shown in the Lab.
→ [themed serial console](design/serial_console_theming.md)

**Self-manifest** — A device's own machine-readable report of what it is,
what firmware it runs, and what it can do — the basis for "plug it in and it
proves itself" and for the fleet self-report API.
→ [self-\* roadmap](design/self_star_roadmap.md),
[parity by architecture](FLEET_PARITY.md)

**Fleet** — **The word for a group of Canaries.** Not "flock" — that word is
off-limits in all copy, identifiers, and comments (a company called Flock
soured it); the only exception is the Unix `flock(2)` syscall. Rule:
[`CLAUDE.md`](../CLAUDE.md), [`AGENTS.md`](../AGENTS.md).

---

## The privacy invariants

Seven structural rules the system is built around. They are `can't`, not
`won't` — the surveillance code was never written, so there is no setting to
turn off. Canonical text: [`spec/invariants.md`](../spec/invariants.md);
enforcement story: [governance & invariants](governance_and_invariants.md).

| # | Name | In one line |
|---|---|---|
| I | No raw export | Raw frame bytes have no public accessor; the only way out is break-glass |
| II | No identity substrate | No face embeddings, plate strings, re-ID vectors, or demographic estimates exist |
| III | Metadata minimization | Coarse timestamps, local-only zone IDs, single-use correlation tokens |
| IV | Local ownership | Logs stay local; no remote indexing, no telemetry |
| V | Break-glass by quorum | Raw media needs n-of-m trustee approval |
| VI | No retroactive expansion | A new ruleset cannot be applied to already-recorded data |
| VII | Non-queryable | No bulk search, no identity selectors |

**The root paradox** — The honest limit: every guarantee above assumes an
uncompromised host. What host compromise does and does not break is written
down rather than glossed over.
→ [the root paradox](root_paradox.md)

---

## Pipeline types (what an agent will meet in the Rust)

**`RawFrame`** — Pixels. `data` is private, with no public getter, no `Clone`,
no `AsRef<[u8]>`. The single escape hatch is `export_for_vault()` behind a
`BreakGlassToken`. → [`src/frame.rs`](../src/frame.rs)

**`InferenceView`** — The restricted view a detector gets: enough to detect,
never a handle it can copy pixels out of.

**`DetectorBackend`** — The trait a detection engine implements. It is an
**audit boundary**, and in the production pipeline it runs inside a forked,
seccomp-restricted child process that denies filesystem, network, `execve`,
`ptrace`, and `process_vm_*`. It is a denylist — defense in depth, not an
airtight allowlist. → [`src/detect/backend.rs`](../src/detect/backend.rs),
[inference backends](inference_backends.md)

**`DetectionResult` / `Detection`** — What a backend may return: classes and
boxes, never identity. `ObjectClass` is `Person | Vehicle | Animal | Package`
— deliberately *not* `Face` or `LicensePlate`.

**`CandidateEvent` → `SealedEvent`** — A detection becomes a candidate event,
then is sealed into the hash chain.

**`ReprocessGuard`** — The mechanism enforcing Invariant VI: it checks the
ruleset hash so new rules cannot reprocess old data.

**`FailureType`** — The system-trace vocabulary for honest failure
(`StorageFull`, `ClockSkew`, `PowerLoss`, `GapMissingData`, …). Gaps are
recorded, not hidden. → [failure semantics](failure_semantics.md)

---

## The device line

Firmware products live under [`firmware/`](../firmware). The canonical product
list is [`firmware/flavors.json`](../firmware/flavors.json); per-build feature
truth is [`firmware/build_matrix.json`](../firmware/build_matrix.json).

| Name | What it is |
|---|---|
| **Canary WAP** | The recommended first build — captive-portal setup, SD storage, mesh. XIAO ESP32-S3 Sense. |
| **Canary Vision** | Camera + on-device person detection, reports to Home Assistant. |
| **Canary Sense** | Presence and breathing radar (60 GHz MR60BHA2) — care and wellbeing without a camera to point. |
| **Canary Pool** | *Design-stage* — an outdoor pool/spa water-chemistry node (pH · ORP · water temp · TDS) that publishes to the fleet; the Dash already renders its cards. ESP32 + Atlas EZO or industrial differential probes. See [pool water-monitor research](research/pool_water_monitor.md). |
| **Canary Sentinel** | Multi-sensor fusion guardian: PIR + radar + WiFi CSI + WiFi/BLE + light, scored for corroboration across physically independent channels. Lite / Standard / Heavy tiers. |
| **Canary Display** | The wall displays and dashes — the ambient surface a household actually looks at. |
| **Canary OTA** | The signed pull-update path, with rollback. |
| **Canary Fence Guard** | Boundary/perimeter variant. |
| **The Tin Can** | Design-stage kids' wrist Canary on the AMOLED watch board: two kids "tie a string" and knock at each other, with **no voice, no text, no location, no cloud**. The refusal list *is* the design. |
| **The Night Watch** | Phase-0 bedside clock on the same AMOLED watch board. Ships `GoDark` — genuinely off, which only AMOLED allows — under one rule that outranks the owner's preference: *silence is never rendered as safety*, so fleet trouble or a clock unsure of the time breaks blackout every time. |
| **The Nightlight** | The kid-facing Canary Display flavor on the Waveshare ESP32-C3-LCD-1.47 pocket board (`canary-display-nightlight-c3`): a 7-segment clock over a lamp wash (warm Lantern orange · Rainbow · Moonbeam white), with the living canary visiting on the household's rhythm. The lamp is decor and never a status — this flavor never renders safety as light — and backlight duty is HAL-capped at 50% for heat (a `can't`, not a `won't`). Not **The Night Watch** (the AMOLED watch-board clock); this one lives in the [C3 pocket case](hardware/enclosure/README.md). |
| **The Pocket Canary** | Phase-0 virtual pet: Tamagotchi's charm without its guilt loop. It cannot die, has no bell, has no random reward, and stops paying growth once the day's care is done. See **the Weather / the Bond**. |
| **The Weather / the Bond** | The two channels the Pocket Canary keeps separate. **The Weather** is real fleet health and owns the bird's *posture* (it is diagnostic and never faked). **The Bond** is the child's own care history and owns the bird's *growth* (it is openly a game). Real household hygiene feeds the Bond as a bonus — and there is deliberately no penalty direction, because a child must never be made to feel responsible for a red fleet. |

**Board tier** — Every board is labeled `verified` (proven on real hardware)
or `compile-tested` (builds in CI, not yet bench-proven). The distinction is
published rather than blurred; see [`firmware/boards/boards.json`](../firmware/boards/boards.json)
and [what still has to be true](BRAND.md) in the brand doc.

---

## Radios & channels

Three separate channels, deliberately different trust models. Mixing them up is
the most common misreading of this project.

**Opera** — The household mesh protocol (ESP-NOW/BLE): how a household's own
Canaries stay in sync. Requires WiFi association; `opera_secret` provisioning
refuses to run on a device without flash encryption.
→ [`spec/canary_mesh_network_v0.md`](../spec/canary_mesh_network_v0.md),
[BLE mesh + Opera tandem](BLE_MESH_OPERA_TANDEM.md)

**Chirp** — The community witness channel: neighbors corroborating an event,
with ephemeral session keys (never persisted — that's the privacy firewall
between Chirp and Opera) and confirmation counted as unique signing pubkeys.
→ [`spec/chirp_channel_v0.md`](../spec/chirp_channel_v0.md)

**Beacon** — The higher-trust, narrow life-safety advisory channel, held to
smoke-detector-grade reliability. Every alert needs **two distinct device
signatures**; no automatic origination by sensors; no PII on the wire; never
impersonates an official alert (CI-linted). It is explicitly *not* a
neighborhood-watch or suspicious-person reporting system.
→ [`spec/beacon_channel_v0.md`](../spec/beacon_channel_v0.md), and the Beacon
invariants in [`AGENTS.md`](../AGENTS.md)

**WiFi CSI** — Channel State Information: sensing motion from how a room
disturbs existing WiFi, with no camera involved.
→ [CSI quickstart](csi_quickstart.md), [CSI modules](csi_modules.md)

**mmWave radar (MR60BHA2)** — The 60 GHz presence-and-breathing sensor behind
Canary Sense. → [MR60BHA2 design](canary_sense_mr60bha2_design.md)

**Meshtastic** — Third-party LoRa mesh the project integrates with for
long-range witnesses. → [Meshtastic integration](meshtastic_integration.md)

---

## Surfaces (the things a person actually touches)

**The Lab** — [`canary-local/`](../canary-local) — the offline, in-browser guide
where **the real shipping firmware, compiled to WebAssembly**, runs in the page:
flash a blank chip, watch it boot, check its cryptographic birth certificate,
all with no hardware. The project's most differentiating asset.
→ [`canary-local/README.md`](../canary-local/README.md)

**The Flasher** — The desktop app ([`desktop/`](../desktop)) that flashes and
tends devices. **Note for contributors:** there are *two* flashers — the
in-browser one (`canary-local/assets/`) and this desktop app (`desktop/src/`) —
and they share no UI code. A user-facing diagnostic added to one must be added
to the other. → [browser flasher](browser_flasher.md)

**The Lab desktop app** — [`desktop-lab/`](../desktop-lab) — a Tauri shell
around the browser Lab for Mac/Linux. A different app from the Flasher.

**The Hub** — Home Assistant on a Raspberry Pi, running the kernel as an add-on
([`privacy_witness_kernel/`](../privacy_witness_kernel)) plus the integration
([`custom_components/securacv/`](../custom_components)).
→ [the full stack, end to end](full_stack_setup.md),
[Home Assistant setup](homeassistant_setup.md)

**The Witness Wall** — The Apple TV surface: the *verified record* on the shared
screen for homes and venues, not a wall of live feeds.
→ [tvOS docs](tvos/README.md)

**The Verified Timeline card** — The Home Assistant Lovelace card that shows
✓-badged events on a dashboard. → [Lovelace timeline](lovelace_timeline.md)

**The Doctor / checkup** — The symptom-first fix-it flow ("mine needs help") on
the website and in the Lab.

**The Builder / Showroom** — Browser tools for fitting an enclosure to your
hardware (dropdowns in, STL out) and turning every case over in 3D.

**Maker Corps** — The four-tier maker community: how someone goes from printing
one case to selling kits. Paired with the free **"Works with SecuraCV"** badge
anyone may earn under the trademark policy.
→ [`TRADEMARK.md`](../TRADEMARK.md), [trademark grants](trademark-grants.md)

---

## Process & engineering vocabulary

**Flight rules** — The engineering constitution: rules earned from real
failures, each written down once so it is never re-learned.
→ [`docs/FLIGHT_RULES.md`](FLIGHT_RULES.md)

**The witness dictionary** — [`spec/witness_dictionary.json`](../spec/witness_dictionary.json):
one machine-readable source of truth for every vocabulary that is otherwise
duplicated as constants across Rust, Python, JavaScript, and firmware C++.
`scripts/lint_dictionary_sync.py` parses the real source in each language and
fails CI if any copy drifts. To change a vocabulary, edit the dictionary first.

**Parity by architecture** — The rule that a fleet-wide capability lives in one
host-tested `common/` core so a single edit reaches every board — never a
per-board copy-paste. → [`docs/FLEET_PARITY.md`](FLEET_PARITY.md)

**Release buttons** — The operator's index of every release action, when to
press it and when not to. The default is **Actions → "Update everything (only
what needs it)"**. → [`docs/RELEASE_BUTTONS.md`](RELEASE_BUTTONS.md)

**Maturity labels** — Specs declare themselves 🟢 Stable / 🟡 Draft / ⚪
Spec-only, and the reader is told to check before treating any document as a
contract. → [`spec/README.md`](../spec/README.md)

---

## Words we deliberately do not use

| Never | Instead | Why |
|---|---|---|
| "flock" (of Canaries) | **fleet** | A company called Flock soured the word. Only exception: the Unix `flock(2)` syscall. |
| "verified" loosely | "heard," "reported," "presence" | "Verified" is reserved for a checked Ed25519 signature against a pinned key. |
| face recognition, plate reading, gait, re-ID, demographics | — | Not disabled — **absent**. Invariant II; never implement. |
| "secure" as a bare adjective | the specific property | Say what can't happen and what still has to be true. |
| performance claims without benchmarks | "varies by hardware; benchmark first" | Claims discipline; CI fact-tests enforce it on the site. |

---

## Where to go next

- **"What is this project?"** → [why witnessing matters](why_witnessing_matters.md)
  then [the whitepaper](securaCV_whitepaper.md)
- **"Common questions, plainly answered"** → [FAQ](FAQ.md)
- **"I want to build one"** → [getting started with Canaries](getting_started_canary.md)
- **"I'm an AI agent working in this repo"** → [`AGENTS.md`](../AGENTS.md)
- **"Which directory is which?"** → [the tree map](CONSOLIDATION.md)
