# SecuraCV

[![HACS Badge](https://img.shields.io/badge/HACS-Custom-41BDF5.svg)](https://github.com/hacs/integration)
[![Validate with HACS](https://github.com/kmay89/securaCV/actions/workflows/validate.yml/badge.svg)](https://github.com/kmay89/securaCV/actions/workflows/validate.yml)
[![Status](https://img.shields.io/badge/status-v1--rc-yellow.svg)](#status)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

![SecuraCV logo animation](docs/securacv_logo_animation-2.gif)

### A security camera with a 24-hour memory.

Clips stay on your hardware. They auto-delete. The only thing that persists is a
tamper-proof log that proves nobody — including you — altered the record.

- **No subscription.** It runs on your own hardware. There is no monthly fee, ever.
- **Private by design.** No faces, no plates, no precise timestamps — the witness kernel turns
  detections into semantic events ("a large object crossed the boundary") and never persists
  raw frames. The privacy rules are [enforced in code](spec/invariants.md), not promised in a
  policy.
- **Tamper-proof proof.** Every event is cryptographically signed and hash-chained. If anyone
  alters the record, the signature breaks and verification fails.

> **Why this exists:** [Witnessing is not watching](docs/why_witnessing_matters.md) — the case
> for tamper-evident perception that records *that something happened* without building a
> surveillance archive of *who*.

---

## New here? Start where you are

- **"I have Home Assistant and cameras."** You're the best-supported user today.
  [Install in 5 minutes](#install) — Frigate keeps recording as usual; SecuraCV adds the
  tamper-proof witness log and the privacy boundary.
- **"I like building little devices."** The [Canary firmware](#firmware) turns cheap ESP32
  boards into independent witnesses — camera, radar, and WiFi-sensing variants, plus a
  round "watch" and a wall "dash" display with a living canary mascot that shows system
  health through its mood. New canaries join by scanning a QR code off the display's screen.
- **"I just want to understand it."** Read [How it works (for normal people)](#how-it-works-for-normal-people)
  — five bullets, no jargon — then [why witnessing matters](docs/why_witnessing_matters.md).
- **Questions or ideas?** [Discussions](https://github.com/kmay89/securaCV/discussions) is the
  place to ask anything. The [open issues](https://github.com/kmay89/securaCV/issues) are
  mostly forward-looking trackers (labeled `v2`) — they're the future list, not a pile of
  broken things.

---

## Who it's for, and how it compares

Built for privacy-conscious Home Assistant / homelab users with RTSP cameras and a Pi
(*start here — best supported today*), for people who need records that hold up — tenants,
journalists, activists, abuse survivors — and for builders who want tamper-aware ESP32
"Canary" sensors (see [Firmware](#firmware)). Unlike cloud cameras there are no recurring
fees and footage never leaves your disk; unlike DIY NVRs the record is provably
tamper-evident. SecuraCV doesn't replace Frigate — it runs alongside it and adds a privacy
boundary enforced in code, plus a record you can *prove* nobody edited.
([Full market & cost analysis](docs/strategy/05-market-and-cost-comparison.md))

## Install

### Home Assistant (5 minutes)

**1.** Settings → Add-ons → Add-on Store: install **Mosquitto broker** (official
add-on), then ⋮ → Repositories → add both
`https://github.com/blakeblackshear/frigate-hass-addons` (install **Frigate**) and
`https://github.com/kmay89/securaCV` (install **Privacy Witness Kernel**).

**2.** Open the Privacy Witness Kernel add-on's Web UI and click through the setup
wizard. Device key: auto-generated. MQTT broker: auto-discovered from the
Mosquitto add-on (host, port, credentials — nothing to type). The wizard checks
that Mosquitto and Frigate are present and warns if not.

**3.** **Point Frigate at your cameras** — the wizard writes a ready-made config
template to `/config/frigate.yml`; copy its contents into Frigate's own config
(the Frigate add-on reads `/addon_configs/ccab4aaf_frigate/config.yml`, editable
from the Frigate Web UI's configuration editor or a file editor add-on), replace
the placeholder RTSP URLs with your cameras', then start Frigate
(Settings → Add-ons → Frigate → Start). Until this is done there are no
detections for SecuraCV to witness.

That's it. Frigate clips keep recording as usual; witness sensors, a daily-digest sensor,
a chain-integrity sensor, and a **Verify Now** button appear in Home Assistant
automatically, and the add-on panel can generate a ready-made dashboard from your live
zones. *(Alternative one-liner from the Terminal add-on:
`curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash`)*

### Docker, alongside an existing Frigate (5 minutes)

One compose file next to your existing Frigate: quickstart download, the end-to-end
`doctor` check, the bundled-broker variant, and API-token handling are all in
[`docs/frigate_integration.md`](docs/frigate_integration.md).

### What you need

A Raspberry Pi 4 (4 GB+) or x86 PC running Home Assistant OS, plus RTSP camera(s)
(Hikvision, Dahua, Reolink, Amcrest, Ubiquiti, …) — details in the
[hardware table](docs/homeassistant_setup.md#what-you-need).

## How it works (for normal people)

- Your cameras record clips locally via **Frigate** (an open-source NVR).
- Clips auto-delete after **24 hours** (configurable — you choose the retention).
- SecuraCV keeps a **cryptographic witness log** of every detected event.
- The log is tamper-evident: if anyone — including you — alters it, the signature breaks.
- If you ever need to prove what happened, **break the glass** — a multi-party authorization
  that requires your chosen trustees to approve access. Tampering isn't impossible — it's
  *evident*: alter the record and verification fails. (For what the trust boundary does and
  doesn't cover, see [the root paradox](docs/root_paradox.md).)
- **Daily digest & pattern alerts:** a morning push summarizes event counts per zone and
  confirms all witnesses are valid; unusual-hour activity or silent zones push automatically.

## How it works (for engineers)

SecuraCV wraps [Frigate](https://frigate.video/) (camera ingest, object detection, clip storage
and retention) with a **Privacy Witness Kernel** that:

1. Subscribes to Frigate's MQTT event stream.
2. Converts raw detections into privacy-preserving semantic claims (e.g.
   `BoundaryCrossingObjectLarge`) — no raw frames, no precise timestamps, no identity data, per
   the [invariants](spec/invariants.md).
3. Appends each claim to a **hash-chained, Ed25519-signed append-only log**.
4. Seals sensitive claims into encrypted vault envelopes (break-glass required to open).
5. Publishes verification status back to Home Assistant via MQTT Discovery.

```
Camera → Frigate (clips, detection) → MQTT → Privacy Witness Kernel
                                                    ↓
                               Hash-chained log + sealed vault
                                                    ↓
                               HA integration (sensors, verification)
                                                    ↓
                               Your phone (daily digest, alerts)
```

The result: a forensic-grade event log that proves what the cameras saw, while discarding
everything that could enable mass surveillance. Canary devices are *independent* witnesses
with their own on-device hash chains, verified against per-device pinned keys — not re-sealed
into the kernel's log ([two witness logs, two trust roots](docs/homeassistant_setup.md#two-witness-logs-two-trust-roots)).

### Canonical specifications

- [`spec/invariants.md`](spec/invariants.md) — seven non-negotiable privacy constraints (enforced in code)
- [`spec/event_contract.md`](spec/event_contract.md) — permissible event structure and forbidden claims
- [`spec/threat_model.md`](spec/threat_model.md) — threats in and out of scope
- [`kernel/architecture.md`](kernel/architecture.md) — component isolation, trust boundaries

## Home Assistant integration

SecuraCV integrates with Home Assistant via an **HTTP API** (required) and optional **MQTT**
(real-time Canary updates). It surfaces semantic witness events, hash-chain integrity, and
device health — never raw video or identity data.

**HACS install:** HACS → ⋮ → Custom repositories → add `https://github.com/kmay89/securaCV` as an
Integration → install "SecuraCV" → restart → Settings → Devices & Services → Add Integration →
SecuraCV. Requires Home Assistant 2024.4.1+.

A bundled Lovelace card ("SecuraCV Verified Timeline") shows events under a hash-chain
status header, with a **✓ Signature verified** badge that appears only when an event's
Ed25519 signature actually verified — see the [card guide](docs/lovelace_timeline.md).

The full entity catalog (kernel sensors, Canary MQTT sensors, per-tamper-type sensors,
multi-transport status, MQTT Discovery, Frigate mode), is in
[`docs/homeassistant_setup.md`](docs/homeassistant_setup.md) and
[`docs/frigate_integration.md`](docs/frigate_integration.md).

## Building from source (developers)

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt-get install build-essential libseccomp-dev pkg-config

# Run the demo, then verify the log
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
```

Real cameras (RTSP / V4L2 / ESP32), local ONNX detection, container deployment, and the
break-glass and event-export CLIs are in the **[Operator Guide](docs/operator_guide.md)**;
why exports work the way they do is in **[Why SecuraCV exports work this way](docs/why_secure.md)**.

## Firmware

Device firmware lives under [`firmware/`](firmware/), one project per device. Each Canary
is an *independent* witness with its own signed hash chain — they corroborate each other
instead of trusting a central box:

- **Canary Vision** — ESP32-C3 (XIAO ESP32-C3 or C3-DevKitM; XIAO ESP32-S3 variant) + Grove
  Vision AI V2: `firmware/projects/canary-vision/` — person detection runs on the camera
  module's own chip; only "someone is here" ever crosses the wire. No pixels leave the device.
- **Canary WAP** — XIAO ESP32-S3 Sense: `firmware/projects/canary-wap/` — senses presence
  through WiFi itself (how bodies disturb the radio field), no camera needed; device-to-device
  mesh, Bluetooth beacon tracking, and a built-in web dashboard.
- **Canary Sense** — XIAO ESP32-C6 + Seeed MR60BHA2 60 GHz mmWave radar:
  `firmware/projects/canary-sense/` — radar-native presence witness (no camera, no mic).
- **Canary Display** — XIAO ESP32-S3 + Seeed Round Display ("watch") or a Waveshare 4.3"
  touch panel ("dash"): `firmware/projects/canary-display/` — the family's face. A calm
  glass that sleeps at night, a **living canary mascot** whose mood honestly mirrors system
  health (worried when a sensor is late, searching for it at the edge of the screen, asleep
  when the house sleeps), on-glass settings, and QR onboarding: the display shows a code,
  a new canary looks at it, and it joins — no typing.
- **Canary OTA** — XIAO ESP32-S3 (ESP-IDF): `firmware/projects/canary-ota/` — OTA engine
  (manifest fetch, SHA256 verify, A/B rollback). Standalone today: not yet integrated into
  the Canary trees; Ed25519 manifest signing lands before it ships (post-v1).

**Hardware build plan & BOM:** to build a Canary from parts — audible chirp (buzzer), status LED, button/tamper/touch inputs, battery, and enclosure — see [`docs/hardware/`](docs/hardware/) ([build plan & BOM](docs/hardware/canary_peripheral_build_plan.md)).

## Project docs

- **[docs/README.md](docs/README.md)** — **the documentation map**: every guide, organized by
  what you're trying to do, CI-enforced so it can't rot. The same getting-started paths run
  interactively (pick your OS, copy commands with one tap, watch your progress fill) in the
  [Lab's Get Started guide](https://kmay89.github.io/securaCV/canary-local/start.html).
- **[docs/strategy/](docs/strategy/)** — codebase map, product strategy, market & cost analysis.
- Contribution rules: [`CONTRIBUTING.md`](CONTRIBUTING.md) · Security policy: [`SECURITY.md`](SECURITY.md)
  · Detection sandbox & engineering invariants: [`AGENTS.md`](AGENTS.md)
- Release notes: [`CHANGELOG.md`](CHANGELOG.md) · Host-compromise limits: [`docs/root_paradox.md`](docs/root_paradox.md)

## Status

**v1 release candidate.** The Frigate → MQTT → sealed-log pipeline is verified end-to-end
automatically in CI (including a real broker ingest test); on-device hardware validation and
the v1 tag are still pending. Track progress in [`docs/v1-roadmap.md`](docs/v1-roadmap.md) and
[`CHANGELOG.md`](CHANGELOG.md).

**Reading the issue tracker:** open issues are almost all *forward-looking* — `v2` feature
trackers (bigger meshes, learned room models, new radios) and hardware-bench checklists.
Each carries a current status comment mapping it against today's code, so you can tell at a
glance what's real, what's planned, and what's waiting on a soldering iron.

## Support the project

SecuraCV is independent, open source (Apache-2.0), and built in the open. If *witnessing
without watching* resonates with you: ⭐ star the repo, share it with the Home Assistant and
self-hosting communities, or help build it — [open an issue](https://github.com/kmay89/securaCV/issues),
join a [discussion](https://github.com/kmay89/securaCV/discussions), or see [`CONTRIBUTING.md`](CONTRIBUTING.md).

**SecuraCV** — a security camera with a 24-hour memory, and a tamper-proof log to prove it.
