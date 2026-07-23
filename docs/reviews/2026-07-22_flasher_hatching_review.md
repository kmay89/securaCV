# Flasher hatching review — Raspberry Pi hub, Home Assistant, and Canaries

**Date:** 2026-07-22  
**Scope:** desktop flasher, browser/Lab flasher, Raspberry Pi Home Assistant hub writer, and the post-flash first-use moment for each Canary class.

## Where the recent PRs left off

The recent branch history shows a clear handoff from Claude/recent PR work: the destructive Raspberry Pi path has been kept in pure, tested pieces, while the Canary serial flasher already ships the happy path.

| Area | Recent PR evidence | State now |
|---|---|---|
| Canary Vision completion | `canary-local: two-port Vision completion checklist — insist on BOTH ports (#1121)` | The browser/Lab side now treats Vision as two required jobs: ESP32 firmware plus Grove Vision AI V2 model. |
| Native hub safety | `desktop(hub-core): the write-authorization gate — safety ordering as types (#1122)` | The raw write cannot be reached without a verified image, eligible target, and explicit confirmation proof. |
| Hub Wi-Fi seed | `desktop(hub-core): the Wi-Fi boot-seed keyfile generator (Step 5 core) (#1120)` | The NetworkManager keyfile generation exists and is host-tested; real boot-partition writing is still pending. |
| Hub image trust | `desktop(hub-core): the image-download trust decision, pure and tested (#1118)` | The resolver/verification decision exists; actual TLS download, SHA-256 calculation, `.sha256` fetch, and `.xz` decompress remain. |
| Hub disk enumeration | `desktop: extract hub-core crate so the disk-writer safety logic is CI-tested (#1115)` and `desktop(flasher): hub disk enumeration core — pick the right disk, safely (Step 2) (#1113)` | Linux read-only enumeration logic exists in core form; Tauri command/UI plus macOS/Windows enumerators remain. |
| Default detection honesty | `fix(flasher): don't label or celebrate a custom Vision model as "person" (#1114)` | Keep post-flash copy privacy-safe: “presence” / “motion,” never identity claims. |

## What is already functional

- **Canary ESP32 flashing:** the native desktop app can detect ESP serial ports, identify the chip, show only matching catalog products, fetch the signed release manifest, download the correct factory image, verify SHA-256, and flash through bundled `espflash`.
- **Browser/Lab flashing:** the web catalog and release train already define Canary products, chip guards, recovery guidance, and the Vision WE2 module model metadata.
- **Home Assistant integration:** MQTT discovery, the custom integration, Lovelace assets, add-on architecture, and HA setup docs already exist; the current user pain is orchestration, not a missing protocol.
- **Raspberry Pi hub core decisions:** image target eligibility, image trust decisions, guarded write authorization, and Wi-Fi seed generation are in `desktop/hub-core` so the dangerous path is tested outside Tauri.

## What remains to make the hub fully functional

1. **Expose the hub writer in the native desktop app.** Add Tauri commands for `list_hub_targets`, board/image selection, and a guided target confirmation screen.
2. **Finish image acquisition.** Implement TLS download, streaming SHA-256, Home Assistant `.sha256` fallback fetch, and `.xz` decompression around the existing pure verification decision.
3. **Implement the hardware write path.** Write the verified HAOS image to the authorized block device, show progress, then read back enough data to prove the target contains what was written. This must be hardware-validated before merge.
4. **Seed the boot partition.** Mount or reopen the written boot/CONFIG partition, write the Wi-Fi keyfile, and add the curated SecuraCV full-stack seed: Mosquitto, PWK add-on, Frigate/go2rtc, dashboards, blueprints, and integration wiring.
5. **First-boot proof.** After the Pi boots, the desktop app should discover Home Assistant on the LAN, open the SecuraCV panel, and run a “hub heartbeat” check: MQTT broker up, PWK add-on healthy, integration loaded, and at least one adoption path ready.
6. **Release-train convergence.** Fold the hub image/catalog into the signed release train with drift checks so the HAOS version, add-on version, and docs cannot silently diverge.

## The required post-flash magical moment

Every target should do one satisfying, privacy-safe thing immediately after flashing. The moment should be physical, visible in the app or Home Assistant, and honest about what is being detected.

| Device | Moment after flashing | User action | Proof shown |
|---|---|---|---|
| **Canary / Canary WAP** | “A Canary is hatched”: it creates its setup network, then Identify blinks/chirps the exact board. | Join `SecuraCV-XXXX`, open `canary.local`, tap Identify, then knock once or run the acoustic self-test. | Dashboard shows live device status; Home Assistant self-test does not fire real alarm automations. |
| **Canary Vision** | Presence-only vision witness wakes up. | Flash both ports, place it at a doorway, walk through once. | Home Assistant presence entity flips detected/clear; copy must not claim face/person identity or raw video storage. |
| **Canary Sense** | Radar presence flips with a walk-by. | Power it in the room, wait for HA discovery, walk past it. | Home Assistant presence changes without a camera or mic. |
| **Canary Sense · Wellbeing** | Radar presence first, wellbeing second. | Confirm normal presence, then sit still and breathe after the card stabilizes. | HA shows stable presence/wellbeing telemetry with privacy-surface language. |
| **Raspberry Pi Home Assistant hub** | “The house has a wall”: the Pi boots into HA with SecuraCV already present. | Insert the flashed disk, power the Pi, wait for discovery, open the SecuraCV panel. | Hub checklist shows MQTT up, PWK add-on healthy, integration loaded, dashboards installed, and ready-to-adopt Canaries. |

## UX rule added in this pass

The generated flasher catalog now carries each product's post-flash **hatch** moment, and the desktop flasher renders that catalog metadata after a successful flash. That keeps browser/native copy from drifting and gives the user a concrete first action based on the selected product family: AP/dashboard + Identify for Canary/WAP, two-port + walk-through for Vision, radar walk-by for Sense, and a separate settling path for Sense · Wellbeing. The copy is intentionally local and privacy-safe: it promises motion/presence, not identity recognition.

## Definition of done for the “hatched” experience

- The flasher never ends at “done” alone; it always shows the next physical action.
- Each first action produces an observable state change within about one minute on healthy hardware.
- The proof is visible where the user already is: the desktop app, `canary.local`, or Home Assistant.
- Test/self-test paths cannot accidentally trigger emergency or Home Assistant alarm automations.
- Vision completion requires both ports before celebration.
- Hub completion requires a booted HA panel and service health, not merely a successful disk write.
