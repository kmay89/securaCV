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
# Version-agnostic "latest signed release" asset URLs, mirroring the OTA
# engine's convention (docs/firmware_ota.md). The flasher fetches the
# manifest; the manifest names the versioned factory binaries.
RELEASE_LATEST = f"https://github.com/{REPO_SLUG}/releases/latest/download"
MANIFEST_URL = f"{RELEASE_LATEST}/manifest-flash.json"

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
# .github/workflows/firmware-release.yml (canary-display is not released over
# this channel yet, so it is intentionally absent). `env` + `board` are
# re-verified against the firmware tree below; `asset_stem` is the release
# binary name minus version and extension.
PRODUCTS = [
    {
        "id": "securacv-canary",
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
        "name": "Canary Sense · Wellbeing",
        "tagline": "The mmWave witness with breathing/heartbeat sensing — a distinct privacy surface.",
        "asset_stem": "canary-sense-wellbeing",
        "project": "firmware/projects/canary-sense",
        "env": "canary-sense-wellbeing",
        "board": "seeed_xiao_esp32c6",
        "provisioning": "usb-secrets",
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
    "usb-secrets": "This sensor learns your WiFi from the firmware you flash. The signed "
                   "release carries safe placeholders; to bake in your own network, build "
                   "it once with your secrets (the guide shows how) — every later update "
                   "inherits it from the device’s memory.",
}


def die(msg: str) -> None:
    print(f"gen_flash.py: {msg}", file=sys.stderr)
    raise SystemExit(1)


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
    # Arduino-cli variants name their board via FQBN in the release workflow.
    if env.startswith("arduino:"):
        fqbn_board = env.split(":", 1)[1]
        wf = read(REPO / ".github/workflows/firmware-release.yml")
        if f":{fqbn_board}:" not in wf and f":{fqbn_board}\n" not in wf:
            die(f"{project}: FQBN board '{fqbn_board}' not found in firmware-release.yml")
        # Map the FQBN board to a PlatformIO board id for the chip lookup.
        fqbn_to_pio = {"XIAO_ESP32S3": "seeed_xiao_esp32s3"}
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


def main() -> None:
    registry = json.loads(read(CANARY_LOCAL / "devices/registry.json"))
    fw_train = registry.get("fw_train")
    if not fw_train:
        die("registry.json has no fw_train")

    flavors = {f["name"]: f for f in json.loads(read(REPO / "firmware/flavors.json"))}

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
        products_out.append({
            "id": p["id"],
            "name": p["name"],
            "tagline": p["tagline"],
            "chip": chip,
            "board": p["board"],
            "asset_stem": p["asset_stem"],
            "provisioning": p["provisioning"],
            "provisioning_note": PROVISIONING[p["provisioning"]],
        })

    doc = {
        "$generated_by": "canary-local/tools/gen_flash.py — do not edit by hand",
        "$doc": "Browser flasher catalog. Live binaries come from manifest_url "
                "(the signed release); this file supplies the chip guard and the "
                "human copy, and is honest even before a release exists.",
        "fw_train": fw_train,
        "repo": REPO_SLUG,
        "release_latest": RELEASE_LATEST,
        "manifest_url": MANIFEST_URL,
        # The pinned Ed25519 release public key (from the firmware header) so
        # the flasher verifies image signatures against the same key the device
        # does. All-zero until the signing ceremony → flasher falls back to
        # checksum-only and says so.
        "release_pubkey": read_release_pubkey(),
        "flash_baud": 921600,
        "console_baud": 115200,
        "chips": {c: CHIP_INFO[c] for c in sorted(chips_used)},
        "products": products_out,
        # The Vision's camera module — a different chip (Himax HX6538 behind a
        # CH343 bridge), a different engine (ROM bootloader + XMODEM, mirrored
        # from Seeed's open-source flasher), the same posture: pinned asset,
        # SHA-256 before a byte is written, and you can't brick it (the burn
        # menu lives in ROM). Facts drift-gated against the device guide.
        "we2_module": we2_module_block(),
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


def we2_module_block() -> dict:
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
        "manifest_url": f"{RELEASE_LATEST}/manifest-vision-model.json",
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
