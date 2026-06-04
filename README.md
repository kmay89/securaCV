# SecuraCV

[![HACS Badge](https://img.shields.io/badge/HACS-Custom-41BDF5.svg)](https://github.com/hacs/integration)
[![Validate with HACS](https://github.com/kmay89/securaCV/actions/workflows/validate.yml/badge.svg)](https://github.com/kmay89/securaCV/actions/workflows/validate.yml)
[![Status](https://img.shields.io/badge/status-v1--rc%20(CI%20gates%20green%2C%20on--device%20validation%20pending)-yellow.svg)](CHANGELOG.md)

![SecuraCV logo animation](docs/securacv_logo_animation-2.gif)

### A security camera with a 24-hour memory.

Clips stay on your hardware. They auto-delete. The only thing that persists is a
tamper-proof log that proves nobody — including you — altered the record.

- **No subscription.** It runs on your own hardware. There is no monthly fee, ever.
- **Private by design.** No faces, no plates, no precise timestamps — the kernel turns camera
  detections into semantic events and never persists raw frames, with the privacy rules written
  in code and spec rather than promised in a policy. (Detection runs inside a forked,
  seccomp-restricted sandbox that blocks disk, network, and key-exfil syscalls — see
  [`AGENTS.md`](AGENTS.md).)
- **Tamper-proof proof.** Every event is cryptographically signed and hash-chained. If anyone
  alters the record, the signature breaks and verification fails.

> **Why this exists:** [Witnessing is not watching](docs/why_witnessing_matters.md) — the case
> for tamper-evident perception that records *that something happened* without building a
> surveillance archive of *who*.

> **The payoff — a verified-✓ timeline in your dashboard.** SecuraCV ships a Home Assistant
> Lovelace card that turns the events into a newest-first timeline with a hash-chain status
> header. Each row carries an *honest* verification badge whose **label** is the source of
> truth: a **✓ "Signature verified"** appears only when the event's Ed25519 signature actually
> verified — weaker states (signed-but-unverified, logged, verification-failed) read distinctly,
> so the **"Signature verified"** badge never overclaims (signed-but-unverified reuses the ✓
> glyph but is labelled "Signed (unverified)" and themed apart). Add it from **Add Card → "SecuraCV Verified Timeline"**; see the
> [card guide](docs/lovelace_timeline.md).

---

## Who it's for

- **Privacy-conscious Home Assistant / homelab users** who want a private, no-subscription
  camera and already run RTSP cameras and a Pi. *(Start here — best supported today.)*
- **People who need records that hold up** — tenants, journalists, activists, abuse survivors —
  who need proof that can't be quietly altered.
- **Builders** who want tamper-aware ESP32 "Canary" sensors. See [Firmware](#firmware).

---

## Install (3 steps)

**1.** Install [Home Assistant OS](https://www.home-assistant.io/installation/) on a Raspberry Pi 4/5 or PC.

**2.** Open the Terminal add-on (or SSH) and run:

```bash
curl -fsSL https://raw.githubusercontent.com/kmay89/securaCV/main/scripts/install.sh | bash
```

**3.** Follow the setup wizard — open the Privacy Witness Kernel add-on from
Settings → Add-ons → Privacy Witness Kernel → Open Web UI.

That's it. Your Frigate clips keep recording as usual, and each detected event shows up in
Home Assistant with a **verified ✓** witness status (hash-chain + signature checked).

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
  *evident*: within a trusted host boundary, any alteration breaks the signature and fails
  verification. (A host's root/admin operator runs outside the kernel's control; see
  [the root paradox](docs/root_paradox.md) for what that boundary does and doesn't cover.)

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
**[Operator Guide](docs/operator_guide.md)**.

---

## Firmware

Device firmware lives under [`firmware/`](firmware/):

- **Canary Vision** (ESP32-C3 + Grove Vision AI V2): `firmware/projects/canary-vision/` —
  publishes privacy-preserving semantic events and HA MQTT discovery.
- **Canary WAP** (ESP32-S3 / XIAO ESP32-S3 Sense): `firmware/projects/canary-wap/` — WiFi CSI
  sensing, mesh networking (Opera Protocol), BLE-Scout integration.
- **Canary OTA**: `firmware/projects/canary-ota/` — over-the-air updates for deployed devices.

**Hardware build plan & BOM:** to build a Canary from parts — audible chirp (buzzer), status LED, button/tamper/touch inputs, battery, and enclosure — see [`docs/hardware/`](docs/hardware/) ([build plan & BOM](docs/hardware/canary_peripheral_build_plan.md)).

---

## Project docs

- **[docs/strategy/](docs/strategy/)** — codebase map, product strategy, market & cost analysis.
- Contribution rules: [`CONTRIBUTING.md`](CONTRIBUTING.md) · Security policy: [`SECURITY.md`](SECURITY.md)
- Release notes: [`CHANGELOG.md`](CHANGELOG.md) · Host-compromise limits: [`docs/root_paradox.md`](docs/root_paradox.md)

## Release gate (v1 tagging)

The SecuraCV-owned Frigate → MQTT pipeline is now gated **automatically in CI**:
`cargo test --test frigate_mqtt_e2e` (a `frigate/events` payload → sealed log → real
`log_verify` against the encrypted DB) plus the `frigate-mqtt-e2e` job, which runs the real
`frigate_bridge` binary ingesting from a live mosquitto broker
(`integrations/ha_frigate_mqtt/ci_smoke.sh`).

Before tagging a v1 release, two manual steps remain: run the operator smoke check against a
live 4-container stack — `integrations/ha_frigate_mqtt/verify_pipeline.sh` (see
`docs/integrations/home-assistant-frigate-mqtt.md`) — and validate on real device hardware.
**The CI gates are green; on-device validation and the v1 tag are still pending.**

---

**SecuraCV** is a privacy-preserving computer-vision witness system.
