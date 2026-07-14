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

> **The payoff — a verified-✓ timeline in your dashboard.** SecuraCV ships a Home Assistant
> Lovelace card: events newest-first under a hash-chain status header, where the
> **✓ "Signature verified"** badge appears only when an event's Ed25519 signature actually
> verified — weaker states are labelled distinctly, so the badge never overclaims. Add it from
> **Add Card → "SecuraCV Verified Timeline"**; see the [card guide](docs/lovelace_timeline.md).

---

## Who it's for

- **Privacy-conscious Home Assistant / homelab users** who want a private, no-subscription
  camera and already run RTSP cameras and a Pi. *(Start here — best supported today.)*
- **People who need records that hold up** — tenants, journalists, activists, abuse survivors —
  who need proof that can't be quietly altered.
- **Builders** who want tamper-aware ESP32 "Canary" sensors. See [Firmware](#firmware).

---

## How it compares

| What matters | Cloud cameras (Ring, Nest) | DIY NVR (Frigate, UniFi) | **SecuraCV** |
|---|---|---|---|
| Recurring fees | $5–20/month per setup | None | **None** |
| Footage lives | Their cloud | Your disk | **Your disk, auto-deleting** |
| Surveillance archive | Grows forever | Grows until disk fills | **24-hour memory** |
| Proof the record wasn't altered | Trust the vendor | None | **Ed25519-signed hash chain** |

SecuraCV doesn't replace Frigate — it runs alongside it and adds the two things no NVR gives
you: a privacy boundary enforced in code, and a record you can *prove* nobody edited.
([Full market & cost analysis](docs/strategy/05-market-and-cost-comparison.md))

---

## Install

### Home Assistant (5 minutes)

**1.** Settings → Add-ons → Add-on Store → ⋮ → Repositories → add
`https://github.com/kmay89/securaCV` → install **Privacy Witness Kernel**.

**2.** Open the add-on's Web UI and click through the setup wizard.
Device key: auto-generated. MQTT broker: auto-discovered from the
Mosquitto add-on (host, port, credentials — nothing to type).

**3.** **Point Frigate at your cameras** — edit `/config/frigate.yml`
(the wizard generates it) and replace the placeholder RTSP URLs with your
cameras', then start Frigate (Settings → Add-ons → Frigate → Start).
Until this is done there are no detections for SecuraCV to witness.

That's it. Your Frigate clips keep recording as usual; witness sensors, a
daily-digest sensor, a chain-integrity sensor, and a **Verify Now** button
appear in Home Assistant automatically. The add-on panel can generate a
ready-made dashboard from your live zones.

*(Alternative one-liner from the Terminal add-on:
`curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash`)*

### Docker, alongside an existing Frigate (5 minutes)

```bash
curl -fsSLO https://raw.githubusercontent.com/kmay89/securaCV/main/docker/sidecar/quickstart.compose.yml
# edit one line: FRIGATE_MQTT_HOST → the broker Frigate publishes to
docker compose -f quickstart.compose.yml up -d
docker compose -f quickstart.compose.yml run --rm securacv doctor   # checks everything end-to-end
```

The device key is auto-generated into the data volume (back it up). See
[`docs/frigate_integration.md`](docs/frigate_integration.md) for details
and the bundled-broker variant.

> **Where's the API token?** If you connect the integration in "Witness Kernel via
> HTTP API" mode, keep the default token-file path (`/config/api_token`) — the kernel
> rotates the token every 10 minutes and the integration follows the rotation
> automatically. Only paste a static token for a kernel on another host.

### What you need

| Item | Notes |
|------|-------|
| Raspberry Pi 4 (4 GB+) or x86 PC | Pi 5 works great; ~3 cameras at 10 fps |
| Home Assistant OS | Installed from the official image |
| IP camera(s) with RTSP | Hikvision, Dahua, Reolink, Amcrest, Ubiquiti, etc. |
| HA Companion App (optional) | For push notifications to your phone |

---

## How it works (for normal people)

- Your cameras record clips locally via **Frigate** (an open-source NVR).
- Clips auto-delete after **24 hours** (configurable — you choose the retention).
- SecuraCV keeps a **cryptographic witness log** of every detected event.
- The log is tamper-evident: if anyone — including you — alters it, the signature breaks.
- If you ever need to prove what happened, **break the glass** — a multi-party authorization
  that requires your chosen trustees to approve access. Tampering isn't impossible — it's
  *evident*: alter the record and verification fails. (For what the trust boundary does and
  doesn't cover, see [the root paradox](docs/root_paradox.md).)

**Daily digest:** every morning, a push notification summarizes event counts per zone and
confirms all witnesses are valid. **Pattern alerts:** unusual-hour activity or unexpectedly
silent zones trigger a high-priority push automatically.

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
everything that could enable mass surveillance.

> **Two witness logs, two trust roots.** The kernel's sealed log covers the Frigate
> pipeline above. Canary devices are independent witnesses: each keeps its **own**
> Ed25519-signed hash chain on-device and publishes signed events over MQTT, which the
> Home Assistant integration verifies against that device's pinned key. Canary events are
> *not* re-sealed into the kernel's log — a "fleet" today is N independently-signed
> canaries converging in your dashboard, each verifiable on its own.

### Canonical specifications

- [`spec/invariants.md`](spec/invariants.md) — seven non-negotiable privacy constraints (enforced in code)
- [`spec/event_contract.md`](spec/event_contract.md) — permissible event structure and forbidden claims
- [`spec/threat_model.md`](spec/threat_model.md) — threats in and out of scope
- [`kernel/architecture.md`](kernel/architecture.md) — component isolation, trust boundaries

---

## Home Assistant integration

SecuraCV integrates with Home Assistant via an **HTTP API** (required — event queries, vault
storage, management) and optional **MQTT** (real-time updates from Canary devices, multi-transport
resilience). It surfaces semantic witness events, hash-chain integrity, and device health —
never raw video or identity data.

**HACS install:** HACS → ⋮ → Custom repositories → add `https://github.com/kmay89/securaCV` as an
Integration → install "SecuraCV" → restart → Settings → Devices & Services → Add Integration →
SecuraCV. Requires Home Assistant 2024.4.1+.

The full entity catalog (kernel sensors, Canary MQTT sensors, per-tamper-type sensors,
multi-transport status, MQTT Discovery, Frigate mode), is in
[`docs/homeassistant_setup.md`](docs/homeassistant_setup.md) and
[`docs/frigate_integration.md`](docs/frigate_integration.md).

---

## Building from source (developers)

```bash
# Prerequisites (Ubuntu/Debian)
sudo apt-get install build-essential libseccomp-dev pkg-config

# Run the demo, then verify the log
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
```

For real cameras (RTSP / V4L2 / ESP32), local ONNX detection, container deployment, the
break-glass and event-export CLIs, and the full Home Assistant entity reference, see the
**[Operator Guide](docs/operator_guide.md)**. For a plain-language explanation of why exports
and verification work the way they do (vs. "download the clip"), see
**[Why securaCV exports work this way](docs/why_secure.md)**.

---

## Firmware

Device firmware lives under [`firmware/`](firmware/):

- **Canary Vision** (ESP32-C3 + Grove Vision AI V2): `firmware/projects/canary-vision/` —
  publishes privacy-preserving semantic events and HA MQTT discovery.
- **Canary WAP** (ESP32-S3 / XIAO ESP32-S3 Sense): `firmware/projects/canary-wap/` — WiFi CSI
  sensing, mesh networking (Opera Protocol), BLE-Scout integration.
- **Canary OTA**: `firmware/projects/canary-ota/` — over-the-air update engine (manifest
  fetch, SHA256 verify, A/B rollback). Standalone today: not yet integrated into the
  Canary firmware trees, and Ed25519 manifest signing lands before it ships (post-v1).

**Hardware build plan & BOM:** to build a Canary from parts — audible chirp (buzzer), status LED, button/tamper/touch inputs, battery, and enclosure — see [`docs/hardware/`](docs/hardware/) ([build plan & BOM](docs/hardware/canary_peripheral_build_plan.md)).

---

## Project docs

- **[docs/strategy/](docs/strategy/)** — codebase map, product strategy, market & cost analysis.
- Contribution rules: [`CONTRIBUTING.md`](CONTRIBUTING.md) · Security policy: [`SECURITY.md`](SECURITY.md)
  · Detection sandbox & engineering invariants: [`AGENTS.md`](AGENTS.md)
- Release notes: [`CHANGELOG.md`](CHANGELOG.md) · Host-compromise limits: [`docs/root_paradox.md`](docs/root_paradox.md)

## Status

**v1 release candidate.** The Frigate → MQTT → sealed-log pipeline is verified end-to-end
automatically in CI (including a real broker ingest test); on-device hardware validation and
the v1 tag are still pending. Track progress in [`docs/v1-roadmap.md`](docs/v1-roadmap.md) and
[`CHANGELOG.md`](CHANGELOG.md).

---

## Support the project

SecuraCV is independent, open source (Apache-2.0), and built in the open. If *witnessing
without watching* resonates with you:

- ⭐ **Star the repo** — it's the main way new people find the project.
- 🗣️ **Share it** with the Home Assistant and self-hosting communities — or anyone still
  paying a monthly camera subscription.
- 🛠️ **Help build it** — [open an issue](https://github.com/kmay89/securaCV/issues), join a
  [discussion](https://github.com/kmay89/securaCV/discussions), or see
  [`CONTRIBUTING.md`](CONTRIBUTING.md).

---

**SecuraCV** — a security camera with a 24-hour memory, and a tamper-proof log to prove it.
