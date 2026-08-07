# SecuraCV

[![HACS Badge](https://img.shields.io/badge/HACS-Custom-41BDF5.svg)](https://github.com/hacs/integration)
[![Validate with HACS](https://github.com/kmay89/securaCV/actions/workflows/validate.yml/badge.svg)](https://github.com/kmay89/securaCV/actions/workflows/validate.yml)
[![Status](https://img.shields.io/badge/status-v1--rc-yellow.svg)](#status)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)

![SecuraCV logo animation](docs/securacv_logo_animation-2.gif)

## Witnessing without watching.

SecuraCV is a calm, local-first witness layer for homes, studios, labs, and small sites. It helps you understand **what happened** without turning your cameras and sensors into an identity archive.

Clips stay on your hardware. Events become privacy-preserving claims. The durable record is a signed, hash-chained log you can verify offline.

### The promise

| Principle | What it means in practice |
|---|---|
| **Local by default** | No subscription, remote indexing, or telemetry. Your cameras, your logs, your disk. |
| **Minimal by design** | The kernel accepts semantic events such as `motion in zone A`, not face embeddings, plates, demographics, or person re-identification. |
| **Verifiable after the fact** | Events are Ed25519-signed and hash-chained, so edits show up during verification. |
| **Comfortable to live with** | Home Assistant integration, a verified timeline, gentle Canary devices, and display surfaces designed to be readable instead of alarming. |
| **Honest about limits** | Specs, threat model, docs linting, and supply-chain provenance are part of the product, not fine print. |

> Start with the short essay: [Witnessing is not watching](docs/why_witnessing_matters.md). For the precise rules, read the [privacy invariants](spec/invariants.md).

---

## Choose your path

| If you are... | Start here | Outcome |
|---|---|---|
| Running Home Assistant with cameras | [Home Assistant setup](docs/homeassistant_setup.md) → [Frigate integration](docs/frigate_integration.md) | Frigate keeps recording; SecuraCV adds verified witness events and chain health. |
| Running Docker beside an existing Frigate | [Docker quickstart](docs/frigate_integration.md#quick-start-docker-no-home-assistant) | One sidecar container wraps the witness API, bridge, and doctor checks. |
| Building hardware | [Getting started with Canaries](docs/getting_started_canary.md) → [hardware guides](docs/hardware/README.md) | Small independent witnesses for radar, camera-module presence, WiFi sensing, and displays. |
| Evaluating trust and safety | [Why exports work this way](docs/why_secure.md) → [threat model](docs/security/THREAT_MODEL.md) | A clear view of what is enforced, what is audited, and what remains out of scope. |
| Developing | [Operator guide](docs/operator_guide.md) → [documentation map](docs/README.md) | Specs, commands, testing expectations, and day-2 operations in one map. |
| Just trying to understand a word | [The glossary](docs/GLOSSARY.md) | Every proper noun defined once — SecuraCV vs the Canary vs the kernel, Opera/Chirp/Beacon, break-glass, the device line. |
| Asking a common question | [The FAQ](docs/FAQ.md) | "Does it do face recognition?", "how do I get footage after a break-in?", "is this production-ready?" — answered plainly. |
| An AI coding assistant | [AGENTS.md](AGENTS.md) | The canonical brief: invariants, naming rules, repo map, CI gates. Every tool's own entrypoint file is generated from it. |

Prefer to explore first? The [SecuraCV Lab](https://kmay89.github.io/securaCV/canary-local/start.html) turns the same paths into an interactive guide with browser demos and one-tap commands.

---

## Install

### Home Assistant app

Requires Home Assistant 2024.4.1 or newer. (Home Assistant now calls these
**apps**; older versions call them add-ons, and the steps are the same either way.)

1. In Home Assistant, install the official **Mosquitto broker** app.
2. Add these repositories in **Settings → Apps → App Store → ⋮ → Repositories**:
   - `https://github.com/blakeblackshear/frigate-hass-addons`
   - `https://github.com/kmay89/securaCV`

   Or add the SecuraCV repository in one click:
   [add to my Home Assistant](https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2Fkmay89%2FsecuraCV).
3. Install **Frigate** and **Privacy Witness Kernel**.
4. Open the Privacy Witness Kernel Web UI and follow the setup wizard.
5. Point Frigate at your cameras. The wizard writes a starter config template to `/config/frigate.yml`; copy it into Frigate's config, replace the placeholder RTSP URLs, then start Frigate.

After setup, SecuraCV publishes witness sensors, chain-integrity status, daily digest state, and a **Verify Now** action into Home Assistant. Frigate keeps handling clips and retention; SecuraCV adds the privacy boundary and verifiable event log.

The happiest path today is Home Assistant + Frigate + Mosquitto on a Raspberry Pi 4 or x86 host with RTSP cameras.

### Docker beside Frigate

Use the compose quickstart, doctor check, bundled-broker option, and API-token setup in [Frigate integration](docs/frigate_integration.md).

### From source

On a supported Linux host, the repository installer is `curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash`.

```bash
# Ubuntu/Debian prerequisites
sudo apt-get install build-essential libseccomp-dev pkg-config

# Run a local demo, then verify the generated witness log
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
```

Real camera ingest, break-glass exports, service operation, and day-2 commands live in the [Operator Guide](docs/operator_guide.md).

---

## How it works

```text
Camera / sensor
      ↓
Frigate, RTSP, file, V4L2, ESP32, or sensor adapter
      ↓
Privacy Witness Kernel
      ↓
Event contract + metadata minimization
      ↓
Ed25519-signed hash-chain log + optional sealed vault envelope
      ↓
Home Assistant, CLI verification, or export workflow
```

SecuraCV separates **private media** from **verifiable claims**:

1. Cameras and sensors observe locally.
2. Detection output is reduced to an allowed event vocabulary.
3. Precise timestamps are coarsened, zones stay local, and identity selectors are forbidden.
4. Each accepted event is appended to a signed hash chain.
5. Verification proves whether the chain still matches what was written.
6. Raw media access, when enabled, goes through the break-glass vault flow instead of casual browsing.

The important promise is not that tampering is impossible. It is that tampering becomes visible.

---

## What is ready to explore

- **Verified Home Assistant timeline.** A Lovelace card shows events under chain status and displays the verified badge only after signature verification succeeds. See [Verified Timeline](docs/lovelace_timeline.md).
- **Frigate bridge with CI coverage.** The Frigate → MQTT → sealed-log path is exercised by automated tests and a live-broker CI smoke path. See [Frigate integration](docs/frigate_integration.md).
- **Real ingest paths.** File, RTSP, V4L2, ESP32 HTTP, and sensor-adapter paths are documented; the ffmpeg RTSP path has an end-to-end CI roundtrip. See [v1 roadmap](docs/v1-roadmap.md).
- **Detection backends.** The default motion path is dependency-light; Tract/ONNX is feature-gated for local object detection experiments. See [inference backends](docs/inference_backends.md).
- **Canary hardware family.** Vision, WAP, Sense, Display, and OTA projects share a growing firmware platform under [`firmware/`](firmware/).
- **Canary Cards.** Radar and witness state can render as standardized, privacy-classed cards across Lab, display, and Home Assistant surfaces. See [Canary Cards](docs/standard/CANARY_CARDS.md).
- **SecuraCV Lab.** Browser demos model real firmware behavior, display surfaces, onboarding, power/bench behavior, and the MR60BHA2 radar placement pipeline. Try [the Lab](https://kmay89.github.io/securaCV/canary-local/).
- **USB onboarding design.** Experimental USB-OTG builds provide a read-only drive with `START-HERE.html`, consented one-tap help launch, and recovery/unseal flows. It is off in stock profiles. See [USB onboarding](docs/design/usb_onboard.md).
- **Supply-chain transparency.** Published firmware artifacts and browser-flasher factory images carry SLSA provenance attestations recorded in Rekor, complementing checksums and device-side OTA verification. See [supply-chain transparency](docs/supply_chain_transparency.md).

---

## Canary devices

Canaries are small, local witnesses that corroborate the kernel instead of trusting one central box.

| Device | What it does | Where |
|---|---|---|
| **Canary Vision** | Presence from a Grove Vision AI module; no pixels leave the device. | [`firmware/projects/canary-vision/`](firmware/projects/canary-vision/) |
| **Canary WAP** | WiFi-sensing presence, mesh experiments, beacon/chirp work, local dashboard. | [`firmware/projects/canary-wap/`](firmware/projects/canary-wap/) |
| **Canary Sense** | MR60BHA2 60 GHz mmWave radar witness for presence, coarse occupancy, range bands, and wellbeing builds. | [`firmware/projects/canary-sense/`](firmware/projects/canary-sense/) |
| **Canary Display** | Watch and wall display surfaces with calm status, QR onboarding concepts, Lab parity, and living-canary health cues. | [`firmware/projects/canary-display/`](firmware/projects/canary-display/) |
| **Canary OTA** | Standalone signed-update engine work: manifest fetch, SHA-256 verify, A/B rollback building blocks. | [`firmware/projects/canary-ota/`](firmware/projects/canary-ota/) |

Build plans, BOMs, enclosure notes, and bench procedures are collected in [hardware docs](docs/hardware/README.md). Firmware capability status is tracked in [firmware features](firmware/FEATURES.md).

---

## Trust, safety, and limits

SecuraCV should feel useful, not stressful. It is intentionally conservative about its claims:

- The [invariants](spec/invariants.md) define what the kernel must not collect or expose.
- The [event contract](spec/event_contract.md) defines which claims are allowed.
- The [threat model](docs/security/THREAT_MODEL.md) separates enforceable security boundaries from audited trust boundaries.
- The [root paradox](docs/root_paradox.md) explains what host compromise can and cannot preserve.
- The [Beacon and Chirp specs](spec/beacon_channel_v0.md) keep life-safety advisories distinct from emergency-broadcast impersonation and forbid PII on the wire.

A few bright lines keep the system calm:

- No face recognition, plate OCR, demographic estimates, or bulk identity search.
- No cloud dependency for verification.
- No life-safety shortcut that auto-originates Beacon alerts from sensors alone.
- No claim that outruns the implementation: if a feature is marked design, RFC, pending bench validation, or post-v1, treat it as exactly that.

The project values honest edges more than glossy overclaiming.

---

## Documentation

The best entry point is the [documentation map](docs/README.md). It is checked in CI so new docs cannot become unreachable.

High-signal references:

- [Home Assistant setup](docs/homeassistant_setup.md)
- [Frigate integration](docs/frigate_integration.md)
- [Operator guide](docs/operator_guide.md)
- [Security docs](docs/security/README.md)
- [Supply-chain transparency](docs/supply_chain_transparency.md)
- [v1 roadmap](docs/v1-roadmap.md)
- [Changelog](CHANGELOG.md)

---

## Status

SecuraCV is a **v1 release candidate**.

Current audited docs status:

- The root README now points at current setup paths, hardware families, Lab surfaces, and supply-chain verification.
- The docs map is CI-enforced, so Markdown under `docs/` must remain reachable.
- The core Frigate/MQTT/witness-log path is verified end-to-end in CI.
- The RTSP ffmpeg path has a real roundtrip test.
- The v1 tag is still held for on-device hardware validation across the kernel/bridge path and representative ESP32 firmware.

Track the exact gate in [v1 roadmap](docs/v1-roadmap.md) and recent work in [CHANGELOG](CHANGELOG.md).

---

## Contributing

SecuraCV is independent, open source, and Apache-2.0 licensed. Helpful contributions include:

- testing a real Home Assistant or Docker install,
- bench-validating Canary hardware,
- improving docs and examples,
- reviewing privacy/security boundaries,
- building small, calm, local-first user experiences.

Read [CONTRIBUTING](CONTRIBUTING.md), the [security policy](SECURITY.md), and the repo engineering guide in [AGENTS.md](AGENTS.md) before opening a PR.

---

## License & usage

The software is **Apache-2.0** ([LICENSE](LICENSE)) — free to run, anywhere, at
any scale, forever. Two short, plain-language companions cover the rest:

- **[LICENSING.md](LICENSING.md)** — who pays, who doesn't, and why. Free for
  schools, universities, nonprofits, groups, businesses, and governments to
  self-host at any scale; the short list of things that cost money (bought
  hardware, court-grade attestation, support SLAs) each has a free path; and the
  survival promises (no phone-home, no kill switch, standalone verifier) that
  make it safe to standardize on.
- **[TRADEMARK.md](TRADEMARK.md)** — using the name, logo, and mascot: a free
  "Works with SecuraCV" badge for anyone, primary marks by public grant.
- **[TERMS.md](TERMS.md)** — a plain-language template for paid-services terms
  (advisory-only, liability capped at fees, not a safety device). A scaffold for
  your attorney to finish — not legal advice, and it covers services only, never
  the Apache-2.0 software.

**SecuraCV** — calm local proof, without the surveillance archive.
