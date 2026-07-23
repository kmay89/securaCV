#!/usr/bin/env python3
"""Generate canary-local/devices/flash.json — the browser flasher's catalog.

Honest by construction, the canary.local way: the *facts* the flasher shows
(which products exist, and which ESP32 chip each one runs on) are not typed
here by hand and hoped to stay true. This generator carries an explicit
product table that mirrors the release workflow, then RE-DERIVES each
product's chip from the firmware's own PlatformIO board settings and refuses
to write if they disagree. Change a board in firmware and forget to
regenerate, and CI's drift gate fails — exactly like gen_start.py /
gen_wap.py.

Sources of truth this reads:
  - firmware/flavors.json .............. the variant list (dir per variant)
  - firmware/**/platformio.ini + envs .. the `board =` per build env
  - .github/workflows/firmware-release.yml the published product ids + assets
  - canary-local/devices/registry.json . the firmware train (fw_train)

What the flasher does at runtime with this file:
  - chip guard: only products whose `chip` matches the physically-detected
    chip are offered — a board can never be handed another board's image.
  - live binaries come from the signed release manifest (manifest_url); this
    catalog supplies labels, chips, and the human copy so the page is
    honest and useful even before the first release exists.

Run:  python3 canary-local/tools/gen_flash.py
CI:   the same command + `git diff --exit-code devices/flash.json`.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CANARY_LOCAL = HERE.parent
REPO = CANARY_LOCAL.parent

REPO_SLUG = "kmay89/securaCV"
# Firmware ships in its OWN tagged release, fw-v<train>, and the flasher pins its
# manifest to that tag — deliberately NOT /releases/latest/. This repo also
# publishes the native desktop app + flasher (app-v*, flasher-v*), and GitHub's
# "latest" is the newest release of ANY kind: a native-app release silently
# shadows the firmware manifest at /latest/, and the flasher then shows "no
# release yet" though the firmware release is sitting right there. The train is
# the committed single source of truth (registry.json), drift-gated in CI, so
# the pinned URL is exact, reproducible, and can't be shadowed. The manifest it
# points to still names the versioned factory binaries (as absolute URLs).
def release_download_base(fw_train: str) -> str:
    return f"https://github.com/{REPO_SLUG}/releases/download/fw-v{fw_train}"

# The Ed25519 release public key the device pins, single-sourced so the browser
# flasher verifies signatures against the SAME key (docs/firmware_ota.md).
OTA_KEY_HEADER = REPO / "firmware/common/ota/src/ota_release_key.h"


def read_release_pubkey() -> str:
    """Extract SECURACV_OTA_RELEASE_PUBKEY[32] from the firmware header as hex.

    Returns 64 hex chars (all-zero if the signing ceremony hasn't happened —
    the flasher treats that as unprovisioned and verifies by checksum only).
    """
    text = OTA_KEY_HEADER.read_text(encoding="utf-8") if OTA_KEY_HEADER.exists() else ""
    m = re.search(r"SECURACV_OTA_RELEASE_PUBKEY\[32\]\s*=\s*\{(.*?)\}", text, re.S)
    if not m:
        return "00" * 32
    bytes_ = re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))
    if len(bytes_) != 32:
        return "00" * 32
    return "".join(b.lower() for b in bytes_)

# esptool's chip identity strings (ESPLoader.chip.CHIP_NAME), keyed by the
# PlatformIO board id. This is the ONE place board→silicon is spelled out.
BOARD_CHIP = {
    "seeed_xiao_esp32s3": "ESP32-S3",
    "seeed_xiao_esp32c3": "ESP32-C3",
    "seeed_xiao_esp32c6": "ESP32-C6",
    "esp32-c3-devkitm-1": "ESP32-C3",
    "waveshare_esp32s3_lcd43": "ESP32-S3",  # the Dash's 4.3B host (generic S3 FQBN)
}

# Flash silicon per board, in MB — the second half of board identification
# (the chip guard is the first). The flasher reads the real flash size off
# the connected board, so chip + size often names the module in the user's
# hand without asking (XIAO-class S3 = 8 MB; the Waveshare panel module the
# dash envs target overrides the generic devkit profile to its 16 MB part).
# Same honesty rule as BOARD_CHIP: one place, verified nowhere else — if a
# future product reuses a board id with different silicon, split the id.
BOARD_FLASH_MB = {
    "seeed_xiao_esp32s3": 8,
    "seeed_xiao_esp32c3": 4,
    "seeed_xiao_esp32c6": 4,
    "esp32-c3-devkitm-1": 4,
    "waveshare_esp32s3_lcd43": 16,  # dash profiles: FlashSize=16M / huge_app
}

# Per-chip human copy. Every Canary board is native-USB (the ESP32 chip's own
# USB, no CH340/CP210x bridge) so the download-mode gesture is uniform; we
# keep it per-chip anyway so a future bridge board can differ.
CHIP_INFO = {
    "ESP32-S3": {
        "label": "ESP32-S3",
        "native_usb": True,
        "download_mode": "Hold the BOOT (B) button, tap RESET (R), then let go of BOOT.",
    },
    "ESP32-C3": {
        "label": "ESP32-C3",
        "native_usb": True,
        "download_mode": "Hold the BOOT (B) button, tap RESET (R), then let go of BOOT.",
    },
    "ESP32-C6": {
        "label": "ESP32-C6",
        "native_usb": True,
        "download_mode": "Hold the BOOT (B) button, tap RESET (R), then let go of BOOT.",
    },
}

# The published, flashable product line. Mirrors the release assets in
# .github/workflows/firmware-release.yml — including the display flavors
# (watch / dash / dash-modes), built there from the sketch's committed
# profiles. `env` + `board` are re-verified against the firmware tree below;
# `asset_stem` is the release binary name minus version and extension.
# Order matters for the picker's recommendation: the sensing flagship stays
# first per chip, so the display cards never outrank a witness by accident.
PRODUCTS = [
    {
        "id": "securacv-canary",
        "family": "canary",
        "board_label": "Seeed XIAO ESP32-S3",
        "name": "Canary",
        "tagline": "The all-rounder witness — full sensing plus the Home Assistant bridge.",
        "asset_stem": "canary",
        "project": "firmware/canary",
        "env": "release_ha",
        "board": "seeed_xiao_esp32s3",
        "provisioning": "ap",
    },
    {
        "id": "securacv-canary-wap",
        "family": "wap",
        "board_label": "Seeed XIAO ESP32-S3",
        "name": "Canary WAP",
        "tagline": "Feels presence through the WiFi field itself — no camera. Sets itself up from a phone.",
        "asset_stem": "canary-wap",
        "project": "firmware/projects/canary-wap",
        "env": "arduino:XIAO_ESP32S3",
        "board": "seeed_xiao_esp32s3",
        "provisioning": "ap",
    },
    {
        "id": "securacv-canary-vision",
        "family": "vision",
        "board_label": "ESP32-C3 DevKit",
        "pick_label": "ESP32-C3 DevKitM-1",
        "name": "Canary Vision",
        "tagline": "Person detection on the camera module itself — only “someone is here” ever leaves the board.",
        "asset_stem": "canary-vision",
        "project": "firmware/projects/canary-vision",
        "env": "canary-vision-default",
        "board": "esp32-c3-devkitm-1",
        "provisioning": "usb-secrets",
    },
    {
        "id": "securacv-canary-vision-xiao-c3",
        "family": "vision",
        "board_label": "Seeed XIAO ESP32-C3",
        "pick_label": "XIAO ESP32-C3",
        "name": "Canary Vision · XIAO C3",
        "tagline": "The Vision witness on a Seeed XIAO ESP32-C3.",
        "asset_stem": "canary-vision-xiao-c3",
        "project": "firmware/projects/canary-vision",
        "env": "canary-vision-xiao-c3",
        "board": "seeed_xiao_esp32c3",
        "provisioning": "usb-secrets",
    },
    {
        "id": "securacv-canary-vision-xiao-s3",
        "family": "vision",
        "board_label": "Seeed XIAO ESP32-S3",
        "pick_label": "XIAO ESP32-S3",
        "name": "Canary Vision · XIAO S3",
        "tagline": "The Vision witness on a Seeed XIAO ESP32-S3.",
        "asset_stem": "canary-vision-xiao-s3",
        "project": "firmware/projects/canary-vision",
        "env": "canary-vision-xiao-s3",
        "board": "seeed_xiao_esp32s3",
        "provisioning": "usb-secrets",
    },
    {
        "id": "securacv-canary-sense",
        "family": "sense",
        "board_label": "Seeed XIAO ESP32-C6",
        "pick_label": "presence only",
        "name": "Canary Sense",
        "tagline": "Radar-native presence on 60 GHz mmWave — no camera, no mic.",
        "asset_stem": "canary-sense",
        "project": "firmware/projects/canary-sense",
        "env": "canary-sense-default",
        "board": "seeed_xiao_esp32c6",
        "provisioning": "usb-secrets",
    },
    {
        "id": "securacv-canary-sense-wellbeing",
        "family": "sense",
        "board_label": "Seeed XIAO ESP32-C6",
        "pick_label": "with wellbeing (breathing/heartbeat)",
        "name": "Canary Sense · Wellbeing",
        "tagline": "The mmWave witness with breathing/heartbeat sensing — a distinct privacy surface.",
        "asset_stem": "canary-sense-wellbeing",
        "project": "firmware/projects/canary-sense",
        "env": "canary-sense-wellbeing",
        "board": "seeed_xiao_esp32c6",
        "provisioning": "usb-secrets",
    },
    {
        "id": "securacv-canary-display-watch",
        "family": "display",
        "board_label": "XIAO ESP32-S3 + Seeed Round Display",
        "pick_label": "Watch Station — the round puck",
        "name": "Canary Watch Station",
        "tagline": "The bedside glance — your whole fleet on one calm round glass.",
        "asset_stem": "canary-display-watch",
        "project": "firmware/projects/canary-display",
        "env": "profile:watch",
        "board": "seeed_xiao_esp32s3",
        "provisioning": "on-glass",
    },
    {
        "id": "securacv-canary-display-dash",
        "family": "display",
        "board_label": "Waveshare 4.3 panel module",
        "pick_label": "Dash — the plain 4.3 wall panel",
        "name": "Canary Dash",
        "tagline": "The wall glass — quiet 4.3″ 800×480 truth for the whole house.",
        "asset_stem": "canary-display-dash",
        "project": "firmware/projects/canary-display",
        "env": "profile:dash",
        "board": "waveshare_esp32s3_lcd43",
        "provisioning": "on-glass",
    },
    {
        "id": "securacv-canary-display-dash-modes",
        "family": "display",
        "board_label": "Waveshare 4.3B panel module",
        "pick_label": "Dash · Modes — the 4.3B multi-tool",
        "name": "Canary Dash · Modes",
        "tagline": "The 4.3B multi-tool — the fleet face plus the bench, demo, debug and arcade gears.",
        "asset_stem": "canary-display-dash-modes",
        "project": "firmware/projects/canary-display",
        "env": "profile:modes",
        "board": "waveshare_esp32s3_lcd43",
        "provisioning": "on-glass",
    },
]

# The family layer — how a ten-product line stays un-intimidating (the
# Arduino-IDE lesson in reverse: no 400-item board menu, no FQBN options).
# The picker shows FAMILIES (five, each a plain sentence); a family with
# more than one product asks ONE plain-language question (`pick`) and each
# product answers it with its `pick_label`. Detection does the rest — the
# chip guard plus the read flash size usually name the exact board before
# the user chooses anything. Validation below: every product belongs to
# exactly one listed family; a multi-product family must carry its question.
FAMILIES = [
    {
        "id": "canary",
        "name": "Canary",
        "pitch": "The all-rounder witness — full sensing plus the Home Assistant bridge.",
        "pick": None,
    },
    {
        "id": "wap",
        "name": "Canary WAP",
        "pitch": "Feels presence through the WiFi field itself — no camera.",
        "pick": None,
    },
    {
        "id": "vision",
        "name": "Canary Vision",
        "pitch": "Person detection on the camera module itself — events, never video.",
        "pick": "Which board is yours? (It's printed on the silkscreen.)",
    },
    {
        "id": "sense",
        "name": "Canary Sense",
        "pitch": "Radar-native presence on 60 GHz mmWave — no camera, no mic.",
        "pick": "Which sensing? Wellbeing adds breathing/heartbeat — a distinct privacy surface.",
    },
    {
        "id": "display",
        "name": "Canary Display",
        "pitch": "The fleet's face — it shows, it never watches.",
        "pick": "Which glass is in your hands?",
    },
]

# Provisioning copy — what happens after the flash, per scheme. This is the
# "what do I do now" the user needs, and it is true to the firmware: AP-based
# variants bring up their own WiFi to be set up from; the sensor variants
# seed WiFi/MQTT into NVS from the build's secrets and inherit it thereafter
# (docs/firmware_ota.md § canary-vision: generic release builds + NVS).
PROVISIONING = {
    "ap": "When it boots it hands you a WiFi network of its own — join it and a "
          "setup page opens automatically. Nothing to type here; no secrets ever "
          "touch the browser.",
    "usb-secrets": "Fill in WiFi during the install and it’s written straight into the "
                   "chip’s settings region — the signed generic release then joins YOUR "
                   "network on first boot, no custom build needed. (Leave it empty and "
                   "the firmware falls back to its compiled defaults, exactly as before.)",
    "on-glass": "It sets itself up on its own glass: the screen walks you through WiFi "
                "and the fleet on first boot. Bake WiFi here during the install and it "
                "skips straight to meeting your Canaries.",
}


def wifi_scheme(project: str) -> str:
    """How this firmware reads its WiFi out of NVS — derived from the
    project's own source, never guessed. sense/vision runtime_config use
    Preferences getString (NVS string entries); the wap/canary family reads
    blobs. The flasher's seed writer must match or the firmware reads ''."""
    candidates = [REPO / project / "src/runtime_config.cpp",
                  *(REPO / project).glob("arduino/*/runtime_config.cpp")]
    for rc in candidates:
        if rc.exists():
            text = rc.read_text(encoding="utf-8")
            # The credential loader reads via Preferences getString (nvs
            # string entries); "wifi_ssid" is the key it loads.
            if ".getString(" in text and '"wifi_ssid"' in text:
                return "string"
    return "blob"

# Post-flash "hatching" copy. This lives in the generated catalog instead of
# desktop/web UI branches so every flasher surface can share the same first-use
# promise, and CI's catalog drift gate catches missing metadata for new products.
HATCH_MOMENTS = {
    "ap": {
        "kicker": "Canary hatched",
        "title": "Your Canary is on its perch.",
        "body": "The magical first proof is local and physical: join its setup network, open the dashboard, then make one harmless signal it can witness.",
        "steps": [
            "Join the SecuraCV-XXXX Wi-Fi network it creates and open canary.local.",
            "Tap Identify so the bird blinks and chirps — you know this is the board in your hand.",
            "Knock once near it or use the acoustic self-test card; Home Assistant automations are not fired by the self-test.",
        ],
    },
    "vision": {
        "kicker": "Vision Canary hatched",
        "title": "Your Vision Canary is waking up.",
        "body": "Give it one visible, privacy-safe thing to notice immediately: presence only, no faces and no saved frames.",
        "steps": [
            "If you have not flashed the Grove Vision AI V2 module yet, move the USB-C cable to the module port and flash the pinned model.",
            "Put the board where it can see a doorway, then walk through once.",
            "Open Home Assistant and watch the presence entity flip to detected, then clear.",
        ],
    },
    "sense": {
        "kicker": "Sense Canary hatched",
        "title": "Your Sense Canary is listening with radar.",
        "body": "The first satisfying test is motion in empty air: no camera, no mic, just the mmWave witness waking up.",
        "steps": [
            "Power it from the room where it will live and wait for Home Assistant discovery.",
            "Stand still for a breath, then walk past it at normal speed.",
            "Watch presence flip in Home Assistant.",
        ],
    },
    "display": {
        "kicker": "Display Canary hatched",
        "title": "Watch the glass — it comes alive.",
        "body": "This Canary SHOWS. The proof isn’t in a console: after the splash, its face comes up and walks you through first flight on the screen itself.",
        "steps": [
            "Keep it powered and watch the splash give way to the on-glass welcome.",
            "Follow the wizard on the screen — WiFi (unless you baked it here), then meeting the fleet.",
            "Tap the glass: it feels touch. The same face is running in the emulator on the fleet page, pixel for pixel.",
        ],
    },
    "sense-wellbeing": {
        "kicker": "Wellbeing Canary hatched",
        "title": "Your Sense Wellbeing Canary is listening with radar.",
        "body": "Prove ordinary presence first, then let the gentler wellbeing signal settle before trusting breathing/heartbeat tiles.",
        "steps": [
            "Power it from the room where it will live and wait for Home Assistant discovery.",
            "Walk past it once and watch presence flip in Home Assistant.",
            "Sit still after the presence card is stable; then watch the wellbeing tile settle into its first breathing/heartbeat reading.",
        ],
    },
    "display": {
        "kicker": "Display hatched",
        "title": "Your glass is waking up.",
        "body": "First light is the whole setup: the display guides you from its own screen — no app, no account, and it never reboot-loops on a fresh network.",
        "steps": [
            "Watch the splash land, then the hello: a fresh display shows a join QR on its own glass (a device-unique setup network).",
            "Scan the QR with your phone — the setup page opens by itself; pick your home Wi-Fi and you're done typing.",
            "The fleet face fades in and every Canary on your broker appears on its own — retained topics, no pairing, ever.",
        ],
    },
    "display-modes": {
        "kicker": "Display hatched — with gears",
        "title": "Your glass is waking up, and it's a multi-tool.",
        "body": "Same first light as any display — plus four extra gears a reboot away, each with a 3-second hold to come home.",
        "steps": [
            "Watch the splash land, then the hello: a fresh display shows a join QR on its own glass (a device-unique setup network).",
            "Scan the QR with your phone — the setup page opens by itself; pick your home Wi-Fi and the fleet face fades in.",
            "Open Settings → modes to find the gears: the peripheral bench, the scripted demo, on-glass diagnostics, and the arcade. Hold the glass 3 s in any of them to return to the fleet face.",
        ],
    },
}


def hatch_kind(product_id: str, provisioning: str) -> str:
    if "display" in product_id:
        return "display-modes" if "modes" in product_id else "display"
    if "sense-wellbeing" in product_id:
        return "sense-wellbeing"
    if "sense" in product_id:
        return "sense"
    if "vision" in product_id:
        return "vision"
    return provisioning


def die(msg: str) -> None:
    print(f"gen_flash.py: {msg}", file=sys.stderr)
    raise SystemExit(1)


# ── parity: every Canary proves itself TWO ways ─────────────────────────────
# One real proof (live, over the cable or on the glass) and one emulated twin
# (a lab page that shows the same behavior with no hardware in hand) — per
# ROLE, within each board's honest abilities. The done card renders this
# uniformly, and a test enforces that no product ships without both. Twin
# pages are checked to exist at generation time, so a moved page fails the
# build instead of shipping a dead link.
PROVE = {
    "canary": {
        "real": {"kind": "monitor",
                 "label": "Watch it boot & prove itself →",
                 "how": "The live monitor: boot log, the signed self-manifest (j), "
                        "health score and temperature — from the board's own mouth."},
        "emulated": {"href": "homeassistant.html",
                     "label": "🧪 the emulated twin — its Home Assistant life",
                     "how": "The Hub lab: the exact entities, cards and automations this "
                            "Canary raises, simulated from the same generated data."},
    },
    "wap": {
        "real": {"kind": "bench-field",
                 "label": "🌊 Feel the field — the live WiFi bench →",
                 "how": "The field bench: RF events, device counts, and the CSI stir "
                        "meter, live off the USB console."},
        "emulated": {"href": "wap.html",
                     "label": "🧪 the emulated twin — the guided WAP walkthrough",
                     "how": "The WAP lab: the same sensing pipeline and first-boot "
                            "story, simulated from the firmware's own facts."},
    },
    "vision": {
        "real": {"kind": "bench-camera",
                 "label": "👁 See through it — the camera bench →",
                 "how": "The module bench (the camera's OWN USB-C port): live frames, "
                        "boxes, the confidence meter and the two on-module dials."},
        "emulated": {"href": "vision.html",
                     "label": "🧪 the emulated twin — firmware-backed detection",
                     "how": "The Vision lab: the real detection math compiled for the "
                            "browser, running on a staged scene."},
    },
    "sense": {
        "real": {"kind": "bench-radar",
                 "label": "👋 Feel it sense — the live radar bench →",
                 "how": "The radar bench: presence, bands, count — and on Wellbeing, "
                        "live breathing and heart rate — off the USB console."},
        "emulated": {"href": "senselab.html",
                     "label": "🧪 the emulated twin — the Sense Lab bench",
                     "how": "The Sense Lab: the real presence/vitals FSMs ported to the "
                            "browser, with every reflex on a slider."},
    },
    "display": {
        "real": {"kind": "glass",
                 "label": "Watch the glass — it comes alive",
                 "how": "The proof is the screen itself: splash, then the face, then "
                        "the on-glass wizard. Touch works."},
        "emulated": {"href": "fleet.html",
                     "label": "🧪 the 1:1 twin — the same firmware, in the browser",
                     "how": "The fleet page boots the REAL display firmware compiled to "
                            "WASM — the pixels here are the pixels the glass shows."},
    },
}


def prove_block(role: str, product_id: str) -> dict:
    p = {k: dict(v) for k, v in PROVE[role].items()}  # deep-ish copy per product
    if role == "display":
        # Deep-link straight to THIS display's sheet (registry ids drop the
        # securacv- prefix: canary-display-watch / canary-display-dash).
        p["emulated"]["href"] = "fleet.html#" + product_id.replace("securacv-", "")
    page = p["emulated"]["href"].split("#")[0]
    if not (CANARY_LOCAL / page).exists():
        die(f"prove twin page missing: {page} (role {role})")
    return p


# ── waiting is learning: the coach's lesson deck ────────────────────────────
# Shown (optionally, dismissibly) while the long operations run — a safety
# copy takes a minute, a full erase takes a while, and that minute can leave
# the user knowing more than they arrived with. `stage` tags are matched
# against the words the live progress stages actually use ("saving a safety
# copy…", "downloading", "erasing", "writing firmware"); "any" fills the
# gaps. Every fact below is real and grounded in this repo — the coach
# teaches, it never invents.
LESSONS = [
    {"id": "backup-undo", "stage": "safety copy",
     "title": "This copy is your undo button",
     "body": "Right now every byte on the chip — firmware, settings, identity key, "
             "witness diary — is being read into one file in your downloads. Restore "
             "it any time from Advanced and the board rewinds to this exact moment."},
    {"id": "backup-harmless", "stage": "safety copy",
     "title": "Reading can’t hurt anything",
     "body": "A backup only reads. The witness chain, your settings, the firmware — "
             "all untouched. That’s why the Nursery does it automatically: all upside."},
    {"id": "dl-layers", "stage": "download",
     "title": "One file, three layers",
     "body": "A factory image is really three things fused together: a tiny second-stage "
             "bootloader, a partition table (the chip’s floor plan), and the firmware "
             "itself. The release pipeline merges them so a blank chip boots from zero."},
    {"id": "dl-pinned", "stage": "download",
     "title": "Pinned, never “latest”",
     "body": "The flasher fetches from the exact release tag for this firmware train — "
             "never from “latest”, which any newer release of anything could shadow. "
             "Reproducible today, reproducible in five years."},
    {"id": "verify-sha", "stage": "authentic",
     "title": "The checksum handshake",
     "body": "Your browser just computed the download’s SHA-256 and compared it against "
             "the fingerprint published in the release — before writing a single byte. "
             "After the write, the chip computes its own MD5 over what actually landed."},
    {"id": "verify-sig", "stage": "authentic",
     "title": "A signature that can’t be stripped",
     "body": "Once a release key is in force, an official image without a valid Ed25519 "
             "signature is refused outright — because a tampered manifest could always "
             "strip a signature. Failing closed is the whole point."},
    {"id": "erase-what", "stage": "erase",
     "title": "What a full erase really is",
     "body": "Every flash cell returns to its blank state (all ones). Settings, WiFi, "
             "witness history — gone by design. On first boot the firmware mints a "
             "fresh identity key and starts a brand-new diary."},
    {"id": "write-nobrick", "stage": "writ",
     "title": "Why you can’t brick it",
     "body": "The chip’s first-stage bootloader lives in mask ROM — etched at the "
             "factory, physically unwritable. Whatever happens to the flash, the ROM "
             "can always drop back into download mode and take a fresh image."},
    {"id": "write-nvs", "stage": "writ",
     "title": "NVS — the chip’s settings drawer",
     "body": "One small region holds key-value settings: WiFi, the detection dials, "
             "the radar reflexes. It’s exactly where your choices from the confirm "
             "card are being baked right now, in the same pass as the firmware."},
    {"id": "write-ab", "stage": "writ",
     "title": "Two slots, zero drama",
     "body": "The floor plan keeps two firmware slots. Over-the-air updates write the "
             "spare slot and only flip when it verifies — a failed update just boots "
             "the old one. The update counter you saw at hello counts those flips."},
    {"id": "write-witness", "stage": "writ",
     "title": "The diary and the deep install",
     "body": "This Canary keeps a tamper-evident hash diary. Over-the-air updates "
             "never touch it — they swap firmware slots and nothing else. A USB "
             "factory install like this one is deeper: on some boards the diary’s "
             "chain state rides the settings region and starts fresh here. Visible "
             "by design — a new chain reads as a new chain, never as silently "
             "missing evidence."},
    {"id": "any-chip-board", "stage": "any",
     "title": "Chip vs. board",
     "body": "The CHIP is the silicon (ESP32-S3, -C3, -C6); the BOARD is the little "
             "PCB it sits on (a XIAO, a Waveshare). The chip guard reads the silicon "
             "itself, so a board can never be handed another chip’s firmware."},
    {"id": "any-baud", "stage": "any",
     "title": "Why the speed varies",
     "body": "Bytes move at the fastest rate your cable can carry — the Nursery starts "
             "quick and steps down by itself if the link stumbles. Speed never costs "
             "correctness: every byte is verified after, at any speed."},
    {"id": "any-manifest", "stage": "any",
     "title": "The board can prove itself",
     "body": "After the install, press the monitor’s “self-manifest” key (j): the "
             "running firmware answers with a signed statement of what it is — "
             "version, identity, health score, even its temperature."},
]


# ── the epic layer: roles, dials, reflexes, displays, per-setting help ──────
# Same honesty rule as everything above: every number below is PARSED out of
# the firmware tree (config.h / detect_config.h / registry.json / the lab's
# own drift-gated vision.json), never typed here and hoped true.

def read_const(path: Path, name: str) -> int:
    """Read `#define NAME value` or `constexpr T NAME = value;` as an int."""
    text = read(path)
    m = re.search(rf"(?:#define\s+{name}\s+|constexpr\s+\w+\s+{name}\s*=\s*)(\d+)", text)
    if not m:
        die(f"constant {name} not found in {path.relative_to(REPO)}")
    return int(m.group(1))


def product_role(pid: str) -> str:
    """Mirror of flash-core.js productRole — one word per board family."""
    if re.search(r"display|watch|dash", pid):
        return "display"
    if "sense" in pid:
        return "sense"
    if "vision" in pid:
        return "vision"
    if "wap" in pid:
        return "wap"
    return "canary"


VISION_CFG = REPO / "firmware/configs/canary-vision/default/config.h"
VISION_BOUNDS = REPO / "firmware/projects/canary-vision/include/canary/detect_config.h"
VISION_LAB = CANARY_LOCAL / "devices/vision.json"


def vision_detect_block() -> dict:
    """The Vision's four runtime dials: defaults from the firmware config,
    bounds from detect_config.h (the clamps the setters enforce), and the
    room presets from the lab's own drift-gated vision.json — the exact
    values the Home Assistant numbers write, now bakeable at flash time
    (NVS namespace "securacv": det_target u8, det_score u8, det_lost u32,
    det_dwell u32 — detect_config.cpp)."""
    defaults = {
        "target": read_const(VISION_CFG, "CONFIG_PERSON_TARGET"),
        "score": read_const(VISION_CFG, "CONFIG_SCORE_MIN"),
        "lost_ms": read_const(VISION_CFG, "CONFIG_LOST_TIMEOUT_MS"),
        "dwell_ms": read_const(VISION_CFG, "CONFIG_DWELL_START_MS"),
    }
    bounds = {
        "score": [read_const(VISION_BOUNDS, "DETECT_SCORE_MIN_LO"),
                  read_const(VISION_BOUNDS, "DETECT_SCORE_MIN_HI")],
        "target": [0, 255],
        "lost_ms": [read_const(VISION_BOUNDS, "DETECT_LOST_MS_LO"),
                    read_const(VISION_BOUNDS, "DETECT_LOST_MS_HI")],
        "dwell_ms": [read_const(VISION_BOUNDS, "DETECT_DWELL_MS_LO"),
                     read_const(VISION_BOUNDS, "DETECT_DWELL_MS_HI")],
    }
    lab = json.loads(read(VISION_LAB))
    presets = [{
        "id": "ships",
        "icon": "🐤",
        "title": "As it ships",
        "blurb": "The firmware’s own defaults — balanced for a first bring-up. "
                 "Pick nothing and this is what you get.",
        "values": dict(defaults),
    }]
    for uc in lab["placement"]["use_cases"]:
        presets.append({
            "id": uc["id"],
            "icon": uc.get("icon", "·"),
            "title": uc["title"],
            "blurb": uc["blurb"],
            "values": {
                "score": uc["preset"]["score"],
                "lost_ms": uc["preset"]["lost_ms"],
                "dwell_ms": uc["preset"]["dwell_ms"],
            },
        })
    return {
        "note": "These are the SAME four numbers Home Assistant tunes live later "
                "(cfg/*/set → NVS). Baking a preset here just means the Canary is "
                "already dialed for its room on first boot — nothing is locked in.",
        "nvs": {"namespace": "securacv",
                "keys": {"target": "det_target", "score": "det_score",
                         "lost_ms": "det_lost", "dwell_ms": "det_dwell"}},
        "defaults": defaults,
        "bounds": bounds,
        "presets": presets,
    }


SENSE_CFG = {
    "default": REPO / "firmware/configs/canary-sense/default/config.h",
    "wellbeing": REPO / "firmware/configs/canary-sense/wellbeing/config.h",
}

SENSE_CONFIG_H = REPO / "firmware/projects/canary-sense/include/canary/sense_config.h"

SENSE_KNOBS = [
    # (id, macro, nvs key, bounds macros stem, unit, flavor)
    # flavor None = both builds carry it. The NVS keys and bounds come from
    # sense_config.{h,cpp} — the runtime twin of vision's detect_config.
    ("present_debounce_ms", "CS_PRESENT_DEBOUNCE_MS", "sns_debounce", "SENSE_DEBOUNCE_MS", "ms", None),
    ("clear_timeout_ms", "CS_CLEAR_TIMEOUT_MS", "sns_clear", "SENSE_CLEAR_MS", "ms", None),
    ("stall_timeout_ms", "CS_RADAR_STALL_MS", "sns_stall", "SENSE_STALL_MS", "ms", None),
    ("range_near_cm", "CS_RANGE_NEAR_CM", "sns_near", "SENSE_NEAR_CM", "cm", None),
    ("range_mid_cm", "CS_RANGE_MID_CM", "sns_mid", "SENSE_MID_CM", "cm", None),
    ("vitals_lock_ms", "CS_VITALS_LOCK_MS", "sns_vlock", "SENSE_VLOCK_MS", "ms", "wellbeing"),
    ("vitals_lost_ms", "CS_VITALS_LOST_MS", "sns_vlost", "SENSE_VLOST_MS", "ms", "wellbeing"),
]

# Room presets for the radar — authored here (like the hatch moments), values
# guided by the Sense pages' raise-it-when/lower-it-when playbook and bounded
# by sense_config.h (asserted below, so an out-of-range preset can't ship).
# "As it ships" is prepended at build time and writes nothing.
SENSE_PRESETS = [
    {
        "id": "bedside", "icon": "🛏",
        "title": "Bedside / sleep watch",
        "blurb": "Slow to alarm, steady through stillness. Longer debounce and clear "
                 "ride out turning-over; tighter bands match a bed two steps away; "
                 "vitals lock waits for a genuinely settled signal.",
        "values": {"present_debounce_ms": 500, "clear_timeout_ms": 5000,
                   "range_near_cm": 100, "range_mid_cm": 250,
                   "vitals_lock_ms": 6000, "vitals_lost_ms": 8000},
    },
    {
        "id": "living", "icon": "🛋",
        "title": "Living room / shared space",
        "blurb": "Occupancy over motion: a long clear timeout rides through reading "
                 "and TV stillness; wider bands cover the whole room.",
        "values": {"present_debounce_ms": 300, "clear_timeout_ms": 4000,
                   "range_near_cm": 200, "range_mid_cm": 450},
    },
    {
        "id": "hall", "icon": "🚶",
        "title": "Hallway / corridor",
        "blurb": "People transit fast — a short debounce catches them, a snappy "
                 "clear frees the room the moment they’re gone.",
        "values": {"present_debounce_ms": 200, "clear_timeout_ms": 800,
                   "range_near_cm": 150, "range_mid_cm": 350},
    },
    {
        "id": "workshop", "icon": "🧰",
        "title": "Garage / workshop",
        "blurb": "Machinery shakes the air: a longer debounce shrugs off vibration "
                 "phantoms; a far mid band covers the long room.",
        "values": {"present_debounce_ms": 600, "clear_timeout_ms": 2000,
                   "range_near_cm": 150, "range_mid_cm": 500},
    },
]


def sense_reflexes_block(flavor: str) -> dict:
    """The radar build's reflexes — now RUNTIME dials. Compiled CS_* values
    seed the first boot; after that the seven numbers live in NVS
    (sense_config.cpp), are HA-tunable over cfg/*/set, and the flasher can
    bake a room preset at install time — the exact posture the Vision's
    detect dials established. Defaults parsed from the flavor config,
    bounds from sense_config.h, so no surface can drift from the firmware."""
    cfg = SENSE_CFG[flavor]
    knobs = []
    for kid, macro, nvs_key, bstem, unit, only in SENSE_KNOBS:
        if only and only != flavor:
            continue
        knobs.append({
            "id": kid, "macro": macro, "unit": unit,
            "value": read_const(cfg, macro),
            "nvs": nvs_key,
            "bounds": [read_const(SENSE_CONFIG_H, bstem + "_LO"),
                       read_const(SENSE_CONFIG_H, bstem + "_HI")],
        })
    by_id = {k["id"]: k for k in knobs}
    presets = [{
        "id": "ships", "icon": "🐤",
        "title": "As it ships",
        "blurb": "The firmware’s own reflexes — balanced for a first bring-up. "
                 "Pick nothing and this is what you get.",
        "values": {k["id"]: k["value"] for k in knobs},
    }]
    for pr in SENSE_PRESETS:
        values = {kid: v for kid, v in pr["values"].items() if kid in by_id}
        for kid, v in values.items():
            lo, hi = by_id[kid]["bounds"]
            if not (lo <= v <= hi):
                die(f"sense preset {pr['id']}: {kid}={v} outside [{lo},{hi}]")
        # A preset only overrides what it names; unnamed knobs keep defaults.
        presets.append({**{k: pr[k] for k in ("id", "icon", "title", "blurb")},
                        "values": {**presets[0]["values"], **values}})
    return {
        "applies": "runtime",
        "nvs": {"namespace": "securacv",
                "keys": {k["id"]: k["nvs"] for k in knobs}},
        "note": "These are live numbers now: they persist in the chip’s settings "
                "region and Home Assistant can retune every one of them later "
                "(cfg/*/set). Baking a preset here just means the Canary is already "
                "dialed for its room on first boot — nothing is locked in.",
        "lab": "senselab.html",
        "knobs": knobs,
        "presets": presets,
    }


REGISTRY = CANARY_LOCAL / "devices/registry.json"


def displays_block() -> list:
    """The boards that SHOW. Since the mode-system wave they are ALSO
    flashable products (see PRODUCTS — watch/dash/dash-modes ride the
    signed release train); this block remains the emulator-preview layer:
    the flasher names a display when it reads one off the wire, and offers
    the same 1:1 WASM firmware emulator fleet.html boots as the honest
    preview of the glass. Facts from registry.json + the committed
    emulator build's own meta."""
    reg = json.loads(read(REGISTRY))
    out = []
    for d in reg["devices"]:
        if d.get("kind") != "display" or not d.get("emulator"):
            continue
        meta_path = CANARY_LOCAL / (d["emulator"]["module"].replace(".js", ".meta.json"))
        meta = json.loads(read(meta_path))
        g = d.get("glass", {})
        out.append({
            "id": d["id"],
            "name": d["name"],
            "tagline": d.get("tagline", ""),
            "board": d.get("board", ""),
            "match": "display",  # app-descriptor project names carry it
            "panel": f'{g.get("panel", "?")} · {g.get("w")}×{g.get("h")}'
                     + (" round" if g.get("round") else ""),
            "glass": {"w": g.get("w"), "h": g.get("h"), "round": bool(g.get("round"))},
            "shows": d.get("shows", []),
            "emulator": {"module": d["emulator"]["module"],
                         "factory": d["emulator"]["factory"],
                         "fw_version": meta.get("fw_version"),
                         "lvgl": meta.get("lvgl")},
            "build_note": "Flashable right here — the display flavors ship in the "
                          "firmware release train now. The emulator is the SAME "
                          "firmware compiled for the browser: try the glass before "
                          "(or after) you flash it.",
            # Cross-link to the flashable product card (same id, securacv- prefix).
            "flash_product": f"securacv-{d['id']}",
        })
    if not out:
        die("no display devices found in registry.json — displays block would lie")
    return out


def settings_help_block(vision: dict, sense_default: dict, sense_wellbeing: dict) -> dict:
    """Every dial and toggle the flasher shows, explained once — what it is,
    when to touch it, what the default means. Short, calm, never scolding;
    numeric defaults are interpolated from the parsed firmware values so the
    help can't drift from the code. flash-core.js helpTopic() looks these up."""
    vd = vision["defaults"]
    sk = {k["id"]: k["value"] for k in sense_default["knobs"]}
    wk = {k["id"]: k["value"] for k in sense_wellbeing["knobs"]}
    return {
        # ── the install itself ──
        "erase_all": {
            "label": "Erase the entire chip first",
            "what": "Wipes every byte — firmware, settings, stored WiFi, witness history — "
                    "then writes fresh. A factory reset and an install in one pass.",
            "when": "A board that misbehaves in ways an ordinary reinstall doesn’t fix, "
                    "or one you’re handing to someone else.",
            "default": "Off — a normal install only touches the regions the new firmware needs.",
        },
        "skip_backup": {
            "label": "Skip the automatic safety copy",
            "what": "Normally every byte on the board is saved to your downloads before "
                    "anything is written — that file is your undo button.",
            "when": "Only when you already backed this exact board up this session and "
                    "want to save the minute.",
            "default": "Off — the copy happens by itself.",
        },
        "wifi_bake": {
            "label": "WiFi at install time",
            "what": "Typed here, your network is written into the chip’s settings region "
                    "in the same pass as the firmware — the Canary joins it on first boot. "
                    "It never leaves this page except over the USB cable.",
            "when": "Always worth it if you know the network; leave it empty and the "
                    "board raises its own setup WiFi instead.",
            "default": "Empty — the setup network is the fallback either way.",
        },
        "local_file": {
            "label": "Install a local file",
            "what": "Flashes a .bin you built or downloaded yourself. Signatures can’t be "
                    "checked for a personal file, but the write is still verified against "
                    "the chip and the board still can’t be bricked.",
            "when": "Your own builds, air-gapped setups, or restoring someone’s shared image.",
        },
        "restore_backup": {
            "label": "Restore a backup",
            "what": "Rewinds the board to the exact moment a backup file was taken — "
                    "firmware, settings, identity, everything.",
            "when": "After an experiment, a downgrade gone odd, or to clone a known-good state "
                    "back onto the same board.",
        },
        "dev_channel": {
            "label": "Dev channel",
            "what": "?channel=dev in the address bar switches to the rolling prerelease — "
                    "signed with the same key, newer, less soaked.",
            "when": "You’re testing a fix the maintainer just cut. Remove the parameter to "
                    "return to stable.",
            "default": "Stable release.",
        },
        "flash_speed": {
            "label": "Flash speed",
            "what": "How fast bytes move over USB (921600 baud first). The flasher steps "
                    "down by itself when a cable can’t keep up — speed never costs "
                    "correctness, every byte is verified after.",
            "default": "Automatic — no setting to get wrong.",
        },
        # ── Vision: the four live dials ──
        "det_score": {
            "label": "Confidence floor",
            "what": "How sure the camera must be before “someone is here” counts (0–100). "
                    "Higher shrugs off shadows and screen-people; lower catches more at "
                    "the cost of false alarms.",
            "when": "Raise it in rooms with monitors or glare; lower it if real people "
                    "go unnoticed.",
            "default": f"{vd['score']} — the shipped balance.",
        },
        "det_lost": {
            "label": "Lost timeout",
            "what": "How long with no person in frame before presence flips back to "
                    "“clear”. Short feels snappy; long rides through someone sitting still.",
            "when": "Short for doorways and hallways, long for sofas and desks.",
            "default": f"{vd['lost_ms']} ms.",
        },
        "det_dwell": {
            "label": "Dwell threshold",
            "what": "How long someone must stay before it counts as dwelling — the "
                    "difference between passing through and being there.",
            "when": "Short where any stop matters (an entryway), long where lingering "
                    "is the signal (a hallway at night).",
            "default": f"{vd['dwell_ms']} ms.",
        },
        "det_target": {
            "label": "What it looks for",
            "what": "The model class treated as “person”. With the pinned person-detection "
                    "model this stays 0 — it exists so a future model swap can’t strand "
                    "the firmware.",
            "when": "Only with a custom model whose person class sits elsewhere.",
            "default": f"{vd['target']} — person, for the pinned model.",
        },
        "tscore": {
            "label": "Module confidence (TSCORE)",
            "what": "The camera module’s own reporting floor — below it, the module "
                    "doesn’t even mention a box. Lives on the module, set over the "
                    "bench wire, separate from the firmware’s confidence floor.",
            "default": "50 — the model zoo default.",
        },
        "tiou": {
            "label": "Module overlap (TIOU)",
            "what": "How much two candidate boxes may overlap before the module merges "
                    "them into one person instead of reporting two.",
            "default": "45 — the model zoo default.",
        },
        # ── Sense: the radar reflexes (compile-time, shown honestly) ──
        "sense_flavor": {
            "label": "Which Sense build",
            "what": "Canary Sense watches presence only. Sense · Wellbeing adds breathing "
                    "and heart-rate sensing — a genuinely different privacy surface, which "
                    "is why it’s a separate firmware, not a toggle.",
            "when": "Wellbeing only where vitals are wanted (a bedside); presence-only "
                    "everywhere else.",
        },
        "present_debounce_ms": {
            "label": "Presence debounce",
            "what": "How long the radar must keep seeing a target before “someone is "
                    "here” is announced — the guard against blinks and noise.",
            "default": f"{sk['present_debounce_ms']} ms in this build.",
        },
        "clear_timeout_ms": {
            "label": "Clear timeout",
            "what": "How long with no target before the room reads empty again.",
            "default": f"{sk['clear_timeout_ms']} ms in this build.",
        },
        "stall_timeout_ms": {
            "label": "Radar stall alarm",
            "what": "No frames at all from the radar for this long means the sensor link "
                    "itself is in trouble — state becomes “unknown”, never a silent guess.",
            "default": f"{sk['stall_timeout_ms']} ms in this build.",
        },
        "range_near_cm": {
            "label": "Near band",
            "what": "Inside this distance counts as “near”. Only the coarse band ever "
                    "leaves the device — raw centimetres stay on the board.",
            "default": f"{sk['range_near_cm']} cm in this build.",
        },
        "range_mid_cm": {
            "label": "Mid band",
            "what": "Between near and this is “mid”; beyond it, “far”. Three honest "
                    "buckets instead of a tracking dot.",
            "default": f"{sk['range_mid_cm']} cm in this build.",
        },
        "vitals_lock_ms": {
            "label": "Vitals lock",
            "what": "Breathing must hold steady this long before the Wellbeing build "
                    "trusts it — and vitals are suppressed entirely unless exactly one "
                    "person is in view.",
            "default": f"{wk['vitals_lock_ms']} ms in the Wellbeing build.",
        },
        "vitals_lost_ms": {
            "label": "Vitals lost",
            "what": "How long without a plausible breathing signal before the lock is "
                    "dropped rather than guessed at.",
            "default": f"{wk['vitals_lost_ms']} ms in the Wellbeing build.",
        },
        # ── the passport & facts (what the flasher reads off the board) ──
        "chip": {
            "label": "Chip",
            "what": "The exact silicon on this board, read from its mask ROM — the one "
                    "fact the chip guard trusts. Only firmware built for this chip is "
                    "ever offered.",
        },
        "mac": {
            "label": "ID (MAC)",
            "what": "The chip's factory hardware address — how the Nursery tells boards "
                    "apart on a bench. It stays on this page; the firmware itself never "
                    "broadcasts it (devices go by salted pseudonyms on the network).",
        },
        "flash_size": {
            "label": "Flash",
            "what": "How much storage the chip carries. The safety copy reads all of it; "
                    "the install only touches the regions the new firmware needs.",
        },
        "updates_seen": {
            "label": "Updates seen",
            "what": "How many firmware updates this board has taken in its life, read "
                    "from the chip's own update ledger (otadata). A brand-new board says "
                    "nothing at all.",
        },
        "boots": {
            "label": "Lifetime boots",
            "what": "How many times this board has ever powered up, from its witness "
                    "counters. A high number is a life well lived, not a problem.",
        },
        "witness_records": {
            "label": "Witness records",
            "what": "How many entries this Canary's tamper-evident hash chain holds — "
                    "its sealed diary. The chain rides through reflashes untouched.",
        },
        "tamper_flag": {
            "label": "Tamper flag",
            "what": "The firmware's own 'something opened me' marker. Expected if you've "
                    "had the enclosure apart; worth a look if you haven't.",
        },
        "crash_record": {
            "label": "Crash record",
            "what": "Whether the chip holds a saved crash dump — proof of a past hard "
                    "fault. One dump isn't alarming; repeated ones are worth a Rescue.",
        },
        "self_check": {
            "label": "Self-check score",
            "what": "The running firmware's own 0-100 health verdict, straight from its "
                    "mouth over the cable — sensors, storage, radio, chain, all rolled "
                    "into one honest number.",
        },
        "temperature": {
            "label": "Temperature",
            "what": "The chip's internal die temperature. Warm is normal (they run "
                    "30-60 °C); past ~70 °C give it airflow or shade.",
        },
        # ── the tools ──
        "health_check": {
            "label": "Health check",
            "what": "A deep, read-only tour of the board's story: partition map, both "
                    "firmware slots, update history, crash dumps, witness counters. "
                    "Changes nothing — ever.",
        },
        "serial_monitor": {
            "label": "Serial monitor",
            "what": "The board's live voice over USB: watch it think, send its "
                    "single-key commands. Nothing typed here can break it.",
        },
        "rescue": {
            "label": "Rescue",
            "what": "The back-to-known-good lever: safety copy (attempted), full wipe, "
                    "newest signed firmware. For a board that's misbehaving in ways a "
                    "plain reinstall doesn't cure.",
        },
        # ── the verdicts & the session ──
        "verdict": {
            "label": "The install verdict",
            "what": "What this install IS for this board: an update (newer), a "
                    "downgrade (older — allowed, never silent), a reinstall (same "
                    "version, byte-for-byte fresh), or a role switch (a different kind "
                    "of Canary).",
        },
        "journey": {
            "label": "The journey bar",
            "what": "Where you are on the path: Connect → Meet it → Choose → Install → "
                    "First flight. Tools and side-quests never move it — only the main "
                    "road does.",
        },
        "roster": {
            "label": "The session roster",
            "what": "Every board hatched in this tab, in order — so a batch never loses "
                    "its place. Public facts only (which firmware, which dials, a MAC "
                    "tail), kept only until the tab closes.",
        },
        "chirps": {
            "label": "Chirps",
            "what": "Tiny synthesized birdsong at the big moments — hello, hatch, first "
                    "sight. Off until you invite it; the choice sticks to this browser.",
            "default": "Off — sound is invited, never sprung.",
        },
        # ── the benches ──
        "radar_bench": {
            "label": "The radar bench",
            "what": "The Sense's live senses off the USB cable: presence, the near/mid/"
                    "far band, the 0/1/2+ count — the same coarse truths it publishes, "
                    "never raw centimetres.",
        },
        "field_bench": {
            "label": "The field bench",
            "what": "The WAP's live verdict on the room: RF events, a device count "
                    "(never an identity), a confidence word, and the CSI stir score — "
                    "how much a moving body bends the radio.",
        },
        # ── displays ──
        "display_emulator": {
            "label": "The 1:1 emulator",
            "what": "The real display firmware — same C++, same LVGL — compiled to run "
                    "in the browser. The pixels you see are the pixels the glass will "
                    "show, framebuffer out, touch in. It boots on the fleet page; the "
                    "flasher itself runs a stricter security policy on purpose.",
            "when": "Before building a Watch Station or Dash: try the face, the touch, "
                    "the night floor, without hardware.",
        },
    }


def read(path: Path) -> str:
    if not path.exists():
        die(f"missing source: {path.relative_to(REPO)}")
    return path.read_text(encoding="utf-8")


def board_for_env(project: str, env: str) -> str:
    """Re-derive a build env's PlatformIO board from the firmware tree.

    Fails loud if the env or its board can't be found, or if it disagrees
    with the product table — that disagreement is exactly the drift the CI
    gate exists to catch.
    """
    # Profile builds (canary-display): the FQBN — and therefore the board —
    # lives in the sketch's committed sketch.yaml, the same file the release
    # workflow compiles with (arduino-cli --profile). Read it from there so
    # the chip guard can't drift from what actually gets built.
    if env.startswith("profile:"):
        prof = env.split(":", 1)[1]
        candidates = list((REPO / project).glob("arduino/*/sketch.yaml"))
        if not candidates:
            die(f"{project}: profile env '{env}' but no arduino/*/sketch.yaml")
        text = read(candidates[0])
        m = re.search(rf"^  {re.escape(prof)}:\n(?:(?!^  \S).*\n)*?\s*fqbn:\s*esp32:esp32:([A-Za-z0-9_]+)",
                      text, re.M)
        if not m:
            die(f"{project}: profile '{prof}' (or its fqbn) not found in {candidates[0].name}")
        fqbn_board = m.group(1)
        fqbn_to_pio = {"XIAO_ESP32S3": "seeed_xiao_esp32s3",
                       "esp32s3": "waveshare_esp32s3_lcd43",
                       "waveshare_esp32_s3_touch_lcd_43b": "waveshare_esp32s3_lcd43"}
        if fqbn_board not in fqbn_to_pio:
            die(f"unknown profile FQBN board '{fqbn_board}' — extend fqbn_to_pio")
        return fqbn_to_pio[fqbn_board]

    # Arduino-cli variants name their board via FQBN in the release workflow.
    if env.startswith("arduino:"):
        fqbn_board = env.split(":", 1)[1]
        wf = read(REPO / ".github/workflows/firmware-release.yml")
        if f":{fqbn_board}:" not in wf and f":{fqbn_board}\n" not in wf:
            die(f"{project}: FQBN board '{fqbn_board}' not found in firmware-release.yml")
        # Map the FQBN board to a PlatformIO board id for the chip lookup.
        fqbn_to_pio = {"XIAO_ESP32S3": "seeed_xiao_esp32s3",
                       "esp32s3": "waveshare_esp32s3_lcd43",
                       "waveshare_esp32_s3_touch_lcd_43b": "waveshare_esp32s3_lcd43"}
        if fqbn_board not in fqbn_to_pio:
            die(f"unknown FQBN board '{fqbn_board}' — extend fqbn_to_pio")
        return fqbn_to_pio[fqbn_board]

    # PlatformIO envs: scan the project's platformio.ini plus any files it
    # pulls in through extra_configs for the [env:<env>] board, or fall back
    # to the base [env] board (the canary project sets board once there).
    proj_dir = REPO / project
    inis = [proj_dir / "platformio.ini"]
    base_ini = read(inis[0])
    # extra_configs / extends is a PlatformIO multiline list: the `key =` line
    # is followed by indented continuation lines, one path each. Gather both
    # the inline value and every indented line that follows.
    lines = base_ini.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^\s*(?:extra_configs|extends)\s*=\s*(.*)$", line)
        if not m:
            continue
        vals = [m.group(1).strip()]
        for cont in lines[i + 1:]:
            if cont.strip() and (cont[0] in " \t"):
                vals.append(cont.strip())
            elif not cont.strip():
                continue
            else:
                break
        for frag in vals:
            frag = frag.strip()
            if frag.endswith(".ini"):
                inis.append((proj_dir / frag).resolve())

    # Parse every [section] across the project's inis into a name→body map,
    # so we can follow PlatformIO `extends =` inheritance (e.g. a -wellbeing
    # env that inherits its board from -default).
    sections: dict[str, str] = {}
    for ini in inis:
        if not ini.exists():
            continue
        text = ini.read_text(encoding="utf-8")
        for chunk in re.split(r"^\[", text, flags=re.M)[1:]:
            name, _, body = chunk.partition("]")
            sections.setdefault(name.strip(), body)

    def board_of(body: str) -> str | None:
        mm = re.search(r"^\s*board\s*=\s*(\S+)", body, re.M)
        return mm.group(1) if mm else None

    def resolve(name: str, seen: set[str]) -> str | None:
        if name in seen or name not in sections:
            return None
        seen.add(name)
        body = sections[name]
        b = board_of(body)
        if b:
            return b
        m2 = re.search(r"^\s*extends\s*=\s*(\S+)", body, re.M)
        if m2:
            return resolve(m2.group(1).strip(), seen)
        return None

    found = resolve(f"env:{env}", set())
    if not found:
        found = board_of(sections.get("env", ""))  # base [env] (canary pattern)
    if not found:
        die(f"{project}: could not find a board for env '{env}'")
    return found


def supports_serial_receipt(project: str) -> bool:
    """Derive the native flasher's post-write receipt gate from firmware.

    A product supports the live receipt only when its own compiled sources wire
    the shared self-manifest builder to the public ``j`` serial command.  This
    deliberately is not another product capability table: adding or removing
    the command in firmware changes flash.json on the next generator run, and
    the existing catalog drift check makes that change visible in CI.
    """
    project_dir = REPO / project
    source = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in sorted(project_dir.rglob("*"))
        if path.is_file()
        and not {".pio", "build", "dist"}.intersection(path.relative_to(project_dir).parts)
        and path.suffix.lower() in {".c", ".cc", ".cpp", ".h", ".hpp", ".ino"}
    )
    has_builder = 'attest/self_manifest.h' in source and "emit_self_manifest" in source
    has_command = bool(re.search(
        r"case\s+'j'|\{\s*'j'\s*,\s*\"self_manifest\"",
        source,
    ))
    return has_builder and has_command


def main() -> None:
    registry = json.loads(read(CANARY_LOCAL / "devices/registry.json"))
    fw_train = registry.get("fw_train")
    if not fw_train:
        die("registry.json has no fw_train")

    # Pin every release asset URL to this train's tag (fw-v<train>) — see
    # release_download_base(): /latest/ is unsafe here because native-app
    # releases share the repo and become GitHub's "latest".
    release_base = release_download_base(fw_train)
    manifest_url = f"{release_base}/manifest-flash.json"

    flavors = {f["name"]: f for f in json.loads(read(REPO / "firmware/flavors.json"))}

    vision_detect = vision_detect_block()
    sense_reflexes = {f: sense_reflexes_block(f) for f in ("default", "wellbeing")}

    products_out = []
    chips_used = set()
    for p in PRODUCTS:
        derived = board_for_env(p["project"], p["env"])
        if derived != p["board"]:
            die(
                f"board drift for {p['id']}: table says '{p['board']}' but "
                f"{p['project']} env '{p['env']}' builds for '{derived}'. "
                f"Update PRODUCTS in gen_flash.py (and the chip guard follows)."
            )
        chip = BOARD_CHIP.get(p["board"])
        if not chip:
            die(f"no chip mapping for board '{p['board']}' — extend BOARD_CHIP")
        # Sanity: the variant's firmware dir should exist (flavors or disk).
        fam = p["asset_stem"].split("-xiao-")[0].split("-wellbeing")[0]
        if fam not in flavors and not (REPO / p["project"]).exists():
            die(f"{p['id']}: neither flavors.json nor {p['project']} knows this variant")
        chips_used.add(chip)
        flash_mb = BOARD_FLASH_MB.get(p["board"])
        if not flash_mb:
            die(f"no flash-size mapping for board '{p['board']}' — extend BOARD_FLASH_MB")
        fam_ids = [f["id"] for f in FAMILIES]
        if p.get("family") not in fam_ids:
            die(f"{p['id']}: family '{p.get('family')}' is not in FAMILIES")
        role = product_role(p["id"])
        hatch = HATCH_MOMENTS[hatch_kind(p["id"], p["provisioning"])]
        entry = {
            "id": p["id"],
            "name": p["name"],
            "tagline": p["tagline"],
            "family": p["family"],
            "board_label": p["board_label"],
            "chip": chip,
            "flash_mb": flash_mb,
            "board": p["board"],
            "asset_stem": p["asset_stem"],
            "provisioning": p["provisioning"],
            "provisioning_note": PROVISIONING[p["provisioning"]],
            "wifi_nvs": wifi_scheme(p["project"]),
            "hatch": hatch,
            "serial_receipt": supports_serial_receipt(p["project"]),
            "role": role,
            "prove": prove_block(role, p["id"]),
        }
        if p.get("pick_label"):
            entry["pick_label"] = p["pick_label"]
        # The dials that genuinely apply to this product — Vision's four NVS
        # numbers are flash-bakeable; Sense's reflexes are compile-time and
        # say so. Nothing here is decorative.
        if role == "vision":
            entry["detect"] = vision_detect
        elif role == "sense":
            entry["reflexes"] = sense_reflexes[
                "wellbeing" if "wellbeing" in p["id"] else "default"]
        products_out.append(entry)

    # Family coverage: every family has products; a family with a variant
    # choice carries its question and every member answers it (pick_label);
    # single-product families must NOT carry stray pick labels.
    for fam in FAMILIES:
        members = [q for q in products_out if q["family"] == fam["id"]]
        if not members:
            die(f"family '{fam['id']}' has no products — drop it or add one")
        if len(members) > 1:
            if not fam.get("pick"):
                die(f"family '{fam['id']}' has {len(members)} products but no pick question")
            for q in members:
                if not q.get("pick_label"):
                    die(f"{q['id']}: family '{fam['id']}' asks a question but this product has no pick_label")

    doc = {
        "$generated_by": "canary-local/tools/gen_flash.py — do not edit by hand",
        "$doc": "Browser flasher catalog. Live binaries come from manifest_url "
                "(the signed release); this file supplies the chip guard and the "
                "human copy, and is honest even before a release exists.",
        "fw_train": fw_train,
        "repo": REPO_SLUG,
        "release_download": release_base,
        "manifest_url": manifest_url,
        # The pinned Ed25519 release public key (from the firmware header) so
        # the flasher verifies image signatures against the same key the device
        # does. All-zero until the signing ceremony → flasher falls back to
        # checksum-only and says so.
        "release_pubkey": read_release_pubkey(),
        "flash_baud": 921600,
        "console_baud": 115200,
        "chips": {c: CHIP_INFO[c] for c in sorted(chips_used)},
        # The family layer (see FAMILIES): the picker's browse structure and
        # the variant questions. Emitted only after the coverage validation
        # below — a family with no products, or a multi-product family with
        # no question, refuses to generate.
        "families": FAMILIES,
        "products": products_out,
        # The boards that SHOW — known and named by the flasher, previewed by
        # the real firmware compiled to WASM, honest about not being in the
        # release train yet. See displays_block().
        "displays": displays_block(),
        # Every dial and toggle, explained once (flash-core.js helpTopic).
        "settings_help": settings_help_block(
            vision_detect, sense_reflexes["default"], sense_reflexes["wellbeing"]),
        # The coach's lesson deck — waiting is optional learning (pickLesson).
        "lessons": LESSONS,
        # The Vision's camera module — a different chip (Himax HX6538 behind a
        # CH343 bridge), a different engine (ROM bootloader + XMODEM, mirrored
        # from Seeed's open-source flasher), the same posture: pinned asset,
        # SHA-256 before a byte is written, and you can't brick it (the burn
        # menu lives in ROM). Facts drift-gated against the device guide.
        "we2_module": we2_module_block(release_base),
        # The promise the whole tool is built to keep, shown in the UI and
        # grounded in docs/firmware_ota.md § the no-brick guarantees.
        "no_brick": {
            "headline": "You cannot brick your Canary from here.",
            "why": "The ESP32’s first-stage bootloader lives in mask ROM — it can’t "
                   "be erased or overwritten over USB. If a flash is interrupted or an "
                   "image is wrong, the board just drops back into download mode and you "
                   "flash again. Nothing you click here is one-way.",
            "points": [
                "Unplug mid-flash? No harm — reconnect and start over.",
                "Wrong image for the chip? The flasher won’t offer it, and the ROM "
                "would refuse it anyway.",
                "Want to go back? Take a one-click backup first and restore it any time.",
            ],
        },
        # Recovery ladder if the board won't connect — the same BOOT/RESET
        # gesture the flashing lesson teaches, surfaced right where it's needed.
        "recovery": [
            {
                "when": "The board doesn’t show up when you click Connect",
                "do": "Unplug and replug the USB-C cable — use a data cable, not a "
                      "charge-only one. Then click Connect again.",
            },
            {
                "when": "It connects but won’t sync / detect",
                "do": "Put it in download mode by hand: hold BOOT (B), tap RESET (R), "
                      "release BOOT — then click Connect.",
            },
            {
                "when": "You’re not on Chrome, Edge, or another Chromium browser",
                "do": "Web Serial only exists there. Use the guided PlatformIO / Arduino "
                      "path instead — same result, a few more steps.",
            },
        ],
    }

    validate_we2_guide()

    out = CANARY_LOCAL / "devices/flash.json"
    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {out.relative_to(REPO)} — {len(products_out)} products, "
          f"chips: {', '.join(sorted(chips_used))}")


GUIDE = REPO / "docs/hardware/grove_vision_ai_v2_guide.md"


WE2_CORE = CANARY_LOCAL / "assets/we2-core.js"


def we2_engine_fact(src: str, name: str) -> str:
    """Read a WE2 constant straight out of the engine so the catalog can't
    disagree with the code that does the burning. we2-core.js is the ONE
    place these live; the offline test (tests/we2.test.js) re-ties the two."""
    m = re.search(rf"\b{name}\s*:\s*(0x[0-9a-fA-F]+|\d+)", src)
    if not m:
        die(f"ERROR: WE2.{name} not found in {WE2_CORE.name} "
            "— the flasher engine moved; the catalog can't be built without it.")
    return m.group(1)


def we2_module_block(release_base: str) -> dict:
    # The burn address is the engine's, verbatim — never a second literal that
    # could drift from what we2-core.js actually writes to. Read once; read()
    # dies cleanly if the engine file is gone. int(..., 0) accepts a hex baud
    # literal too, so a future 0x… value in we2-core.js still parses.
    src = read(WE2_CORE)
    model_addr = we2_engine_fact(src, "MODEL_ADDR")
    baud = int(we2_engine_fact(src, "BAUD"), 0)
    usb_vid = we2_engine_fact(src, "USB_VID")
    usb_pid = we2_engine_fact(src, "USB_PID")
    return {
        "name": "Grove Vision AI V2 — the Vision’s camera module",
        "chip": "Himax HX6538 (WiseEye2) · Ethos-U55 NPU · CH343 USB-serial",
        "usb_vid": usb_vid,
        "usb_pid": usb_pid,
        "baud": baud,
        "model_addr": model_addr,
        "manifest_url": f"{release_base}/manifest-vision-model.json",
        "model": {
            "name": "Person Detection",
            "arch": "Swift-YOLO (tiny) · 192×192×3 RGB · compiled for the Ethos-U55",
            "license": "MIT — Seeed SSCMA model zoo, redistributable with attribution",
            "why_pinned": "One model, chosen and tested with the canary-vision firmware "
                          "train. No catalog to scroll, no wrong pick to make — and the "
                          "firmware’s runtime class-index setting absorbs any future swap.",
        },
        "port_note": "the MODULE’s USB-C port (the big carrier-PCB one, next to the Grove "
                     "connector) — not the XIAO’s. The XIAO port cannot reach the Himax flash.",
        "persistence": "The model lives in the module’s own 16 MB flash and persists across "
                       "power cycles and every future host reflash.",
        "no_brick": "The burn menu lives in the HX6538’s ROM bootloader — an interrupted "
                    "transfer just means reset and flash again. And a bricked module "
                    "bootloader is still recoverable through the host over I2C "
                    "(we2_iic_bootloader_recover — device guide §7).",
        "engine": "SecuraCV WE2 engine (assets/we2-core.js) — XMODEM/CRC-16 at 921600 over "
                  "WebSerial, the same wire protocol Seeed’s open-source flasher speaks, "
                  "clean-room implemented and pinned by tests/we2.test.js.",
        "docs": "docs/hardware/grove_vision_ai_v2_guide.md",
        # The live bench: how to get the camera preview working, start to
        # finish, plus the fixes for every way it usually goes sideways.
        # Threshold defaults come from the drift-gated vision lab data (the
        # SSCMA model-zoo YOLO defaults), never typed here.
        "bench": bench_block(),
    }


VISION_LAB_WIRE_KEYS = ("tscore_default", "tiou_default")


def bench_block() -> dict:
    wire = json.loads(read(VISION_LAB))["model_load"]["wire"]
    for k in VISION_LAB_WIRE_KEYS:
        if not isinstance(wire.get(k), int):
            die(f"vision.json model_load.wire.{k} missing — bench defaults would lie")
    return {
        "defaults": {"tscore": wire["tscore_default"], "tiou": wire["tiou_default"]},
        "steps": [
            "Plug the MODULE’s own USB-C port into this computer — the big port on "
            "the camera carrier board, next to the Grove connector. The XIAO’s port "
            "can’t reach the camera.",
            "Click Connect and pick “USB Single Serial” (the CH343). If the model "
            "isn’t on the module yet, burn it first — one click, verified.",
            "Press “Start live preview”. Frames appear within a second or two, with "
            "a box and a confidence score on everything the model finds.",
            "Aim and light it like the real spot: face the camera, two to four "
            "meters, light on you rather than behind you. Watch the meter climb.",
            "Tune if needed: Confidence (TSCORE) is the module’s reporting floor — "
            "raise it to shrug off weak phantoms, lower it to catch more. IoU (TIOU) "
            "merges overlapping boxes of the same object.",
            "Press Stop when done. Day-to-day aiming stays boxes-only over MQTT — "
            "the video stream exists only on this attended bench.",
        ],
        "troubleshooting": [
            {"when": "No port shows up in the picker",
             "fix": "Wrong port (the XIAO’s instead of the module’s), a charge-only "
                    "cable, or Linux missing the one udev rule — device guide §7. "
                    "Unplug, replug into the module’s port, use a data cable."},
            {"when": "Connected, but Start shows no frames",
             "fix": "The module may still be in its bootloader — power-cycle it "
                    "(unplug/replug), reconnect, start again. If it persists, the "
                    "module might run non-SSCMA firmware; reflash the model here."},
            {"when": "Frames, but never a box",
             "fix": "Check the model is burned (the header above says so), then step "
                    "back — Swift-YOLO wants the whole person in frame, not a face "
                    "filling it. Try more light, or lower Confidence a notch."},
            {"when": "The image is black or very dark",
             "fix": "Peel the lens film if it’s still on, add light in front of the "
                    "camera, and give the sensor a second to auto-expose."},
            {"when": "Boxes flicker or split in two",
             "fix": "Raise IoU (TIOU) slightly so overlapping candidates merge, or "
                    "raise Confidence so marginal duplicates drop."},
            {"when": "Scores feel low",
             "fix": "Confidence is a reporting floor, not a grade — a steady 60-80% "
                    "on a well-lit person is normal and plenty. Chase framing and "
                    "light before chasing 99%."},
        ],
    }


def validate_we2_guide() -> None:
    """Drift-gate the module facts against the device guide (the doc of record)."""
    try:
        guide = GUIDE.read_text(encoding="utf-8")
    except OSError:
        sys.exit("gen_flash.py: ERROR: device guide missing: " + str(GUIDE))
    for needle, label in [
        ('ATTRS{idVendor}=="1a86"', "CH343 USB vendor id"),
        ('ATTRS{idProduct}=="55d3"', "CH343 USB product id"),
        ("921600", "module serial baud"),
        ("HX6538", "module chip name"),
        ("16 MB", "module flash size"),
        ("we2_iic_bootloader_recover", "I2C bootloader recovery"),
        ("never the module's", "port rule"),
    ]:
        if needle not in guide:
            sys.exit(f"gen_flash.py: ERROR: we2_module fact drifted — {label} "
                     f"({needle!r}) not found in {GUIDE.name}")


if __name__ == "__main__":
    main()
