#!/usr/bin/env python3
"""canary-local/tools/gen_start.py — the Get Started guide, as data.

Emits canary-local/devices/start.json for the gamified getting-started
page (canary-local/start.html): pick your starting point (mission), pick
your computer (macOS / Windows / Linux), and every step renders with
one-tap-copy commands and honest expected output — the same replay-bench
voice as The Hub, without a fact written twice.

Sources of truth (never re-stated here):

  1. canary-local/devices/homeassistant.json — versions ({{haos}}, {{ha}},
     {{integration}}, {{min_ha}}), the upstream snapshot date, and the
     Linux flash/boot chapters, imported VERBATIM. The Hub's drift gate is
     therefore this page's drift gate too.
  2. README.md — the two app (add-on) repository URLs and the curl one-liner
     are EXTRACTED from the README's install section. Change the README
     and this guide re-lines; delete them and this generator fails loudly
     rather than teach a dead path.
  3. canary-local/devices/registry.json — the firmware train.
  4. docs/frigate_integration.md — the Docker quickstart's real command
     lines (compose filename and service name), extracted so the
     interactive path can never teach a compose file that doesn't exist.

Authored-in-generator content (mission copy, macOS/Windows imager paths,
Docker steps) lives HERE as constants — the repo's pattern for curated
copy that still travels through the drift gate (see gen_homeassistant.py).

Run:  python3 canary-local/tools/gen_start.py
CI:   regenerates and diffs (drift gate) alongside the other canary-local
      generators.
"""
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_JSON = REPO / "canary-local/devices/start.json"
HUB_JSON = REPO / "canary-local/devices/homeassistant.json"
REGISTRY = REPO / "canary-local/devices/registry.json"
README = REPO / "README.md"


def fail(msg):
    print(f"gen_start.py: {msg}", file=sys.stderr)
    sys.exit(1)


def extract_readme_facts(text):
    """The install section's load-bearing strings, or die loudly."""
    facts = {}
    m = re.search(r"https://github\.com/blakeblackshear/frigate-hass-addons", text)
    if not m:
        fail("README no longer names the Frigate app repository URL")
    facts["frigate_addons_repo"] = m.group(0)

    m = re.search(r"`(https://github\.com/kmay89/securaCV)`", text)
    if not m:
        fail("README no longer names the SecuraCV app repository URL")
    facts["securacv_repo"] = m.group(1)

    m = re.search(r"`(curl -fsSL https://raw\.githubusercontent\.com/kmay89/securaCV/[^`]+install\.sh \| bash)`", text)
    if not m:
        fail("README no longer carries the install one-liner")
    facts["curl_line"] = m.group(1)

    if "Requires Home Assistant" not in text:
        fail("README no longer states the minimum Home Assistant version")
    return facts


def extract_docker_cmds(text):
    """The Docker quickstart's real command lines, from the guide itself.

    The guide (docs/frigate_integration.md) is the source of truth for the
    compose filename and service name — hand-restating them here is how the
    interactive path once taught a compose file that didn't exist.
    """
    cmds = []
    for pattern, why in [
        (r"^curl -fsSLO \S+quickstart\.compose\.yml$", "fetch the quickstart compose file"),
        (r"^docker compose -f quickstart\.compose\.yml up -d$", "bring the sidecar up"),
        (r"^docker compose -f quickstart\.compose\.yml run --rm \S+ doctor$", "run the doctor check"),
    ]:
        m = re.search(pattern, text, re.MULTILINE)
        if not m:
            fail(f"docs/frigate_integration.md no longer carries the command to {why}")
        cmds.append(m.group(0))
    return cmds


FRIGATE_DOC = REPO / "docs/frigate_integration.md"


def main():
    hub = json.loads(HUB_JSON.read_text())
    registry = json.loads(REGISTRY.read_text())
    readme = extract_readme_facts(README.read_text())
    docker_cmds = extract_docker_cmds(FRIGATE_DOC.read_text())

    # The Hub's flash + boot chapters, verbatim — the Linux path of the
    # "spare Pi" mission. Imported, not copied: if gen_homeassistant.py
    # re-lines them, this guide re-lines on the next drift-gated run.
    hub_chapters = {c["id"]: c for c in hub["terminal"]["chapters"]}
    for needed in ("flash", "boot"):
        if needed not in hub_chapters:
            fail(f"homeassistant.json lost its '{needed}' terminal chapter")

    OSES = [
        {"id": "mac", "label": "macOS", "glyph": "🍎"},
        {"id": "win", "label": "Windows", "glyph": "🪟"},
        {"id": "linux", "label": "Linux", "glyph": "🐧"},
    ]

    IMAGER = {
        "mac": {
            "bullets": [
                "Download Raspberry Pi Imager for macOS and open the .dmg.",
                "Choose device → Raspberry Pi 4 (or 5). Choose OS → Other specific-purpose OS → Home assistants and home automation → Home Assistant OS {{haos}}.",
                "Choose storage → your SD card → Write. Imager verifies the write for you.",
            ],
            "links": [{"label": "Raspberry Pi Imager (macOS)", "href": "https://www.raspberrypi.com/software/"}],
        },
        "win": {
            "bullets": [
                "Download Raspberry Pi Imager for Windows and run the installer.",
                "Choose device → Raspberry Pi 4 (or 5). Choose OS → Other specific-purpose OS → Home assistants and home automation → Home Assistant OS {{haos}}.",
                "Choose storage → your SD card → Write. Imager verifies the write for you.",
            ],
            "links": [{"label": "Raspberry Pi Imager (Windows)", "href": "https://www.raspberrypi.com/software/"}],
        },
    }

    # ── the missions ─────────────────────────────────────────────────────
    MISSIONS = [
        {
            "id": "ha",
            "glyph": "🏠",
            "title": "I already run Home Assistant",
            "line": "Best-supported start — witness log, verified timeline, and a daily digest on the stack you have.",
            "time": "~5 minutes",
            "difficulty": "easy",
            "chapters": [
                {
                    "id": "addons",
                    "title": "1 · Install the apps",
                    "intro": "Everything happens in the Home Assistant UI — no terminal on this path. (Apps are what older Home Assistant called “add-ons.”)",
                    "steps": [
                        {
                            "title": "Install the Mosquitto broker",
                            "variants": {"all": {"bullets": [
                                "Settings → Apps → App Store.",
                                "Install the official Mosquitto broker app and start it.",
                            ]}},
                            "note": "MQTT is the wire your witnesses speak on.",
                        },
                        {
                            "title": "Add both app repositories",
                            "variants": {"all": {
                                "bullets": [
                                    "App Store → ⋮ (top right) → Repositories.",
                                    "Paste each URL below, one at a time, then Add.",
                                ],
                                "copies": [
                                    {"label": "Frigate app repo", "text": readme["frigate_addons_repo"]},
                                    {"label": "SecuraCV app repo", "text": readme["securacv_repo"]},
                                ],
                            }},
                            "note": "These strings are lifted straight from the README — if they change there, this page re-lines itself.",
                        },
                        {
                            "title": "Install Frigate and the Privacy Witness Kernel",
                            "variants": {"all": {"bullets": [
                                "From the store, install Frigate (records clips, detects objects).",
                                "Install Privacy Witness Kernel (the tamper-evident witness log).",
                            ]}},
                        },
                    ],
                },
                {
                    "id": "wizard",
                    "title": "2 · Run the wizard",
                    "intro": "The app's Web UI walks you through keys and MQTT — nothing to type.",
                    "steps": [
                        {
                            "title": "Click through the setup wizard",
                            "variants": {"all": {"bullets": [
                                "Open the Privacy Witness Kernel app → Open Web UI.",
                                "Device key: auto-generated. MQTT: auto-discovered from Mosquitto.",
                                "The wizard checks Mosquitto and Frigate are present and warns if not.",
                            ]}},
                        },
                        {
                            "title": "Point Frigate at your cameras",
                            "doc": "docs/frigate_integration.md",
                            "variants": {"all": {"bullets": [
                                "The wizard writes a ready-made config template to /config/frigate.yml.",
                                "Copy its contents into Frigate's own config (Frigate Web UI → configuration editor), replace the placeholder RTSP URLs with your cameras', and start Frigate.",
                            ]}},
                            "note": "Until Frigate sees cameras there are no detections to witness.",
                        },
                    ],
                },
                {
                    "id": "verify",
                    "title": "3 · See it verified",
                    "intro": "The payoff — the thing no other camera can say.",
                    "steps": [
                        {
                            "title": "Watch the entities appear",
                            "doc": "docs/homeassistant_setup.md",
                            "variants": {"all": {"bullets": [
                                "Witness sensors, a daily-digest sensor, a chain-integrity sensor, and a Verify Now button appear automatically.",
                                "Press Verify Now: every Ed25519 signature in the chain is re-checked on the spot.",
                            ]}},
                            "note": "Tamper with the log — even as root — and this is where it shows.",
                        },
                    ],
                },
            ],
        },
        {
            "id": "hub",
            "glyph": "🫐",
            "title": "I have a spare Raspberry Pi",
            "line": "Build the hub first: Home Assistant OS on your Pi, then the mission above.",
            "time": "~45 minutes (20 of them unattended)",
            "difficulty": "medium",
            "chapters": [
                {
                    "id": "flash",
                    "title": "1 · Flash the card",
                    "intro": "Get Home Assistant OS {{haos}} onto an SD card (32 GB+).",
                    "steps": [
                        {
                            "title": "Write the image",
                            "variants": {
                                "mac": IMAGER["mac"],
                                "win": IMAGER["win"],
                                "linux": {
                                    "bullets": [
                                        "Raspberry Pi Imager works on Linux too — or do it by hand, the bench way:",
                                    ],
                                    "cmds": [
                                        {"cmd": s["cmd"], "out": s.get("out", []), "note": s.get("note", "")}
                                        for s in hub_chapters["flash"]["steps"]
                                    ],
                                    "danger": "dd erases the target device completely — triple-check the device name.",
                                },
                            },
                            "note": "The Linux commands here are the Hub bench's own chapter, imported verbatim.",
                        },
                    ],
                },
                {
                    "id": "boot",
                    "title": "2 · First boot",
                    "intro": "Card in, ethernet in, power last. First boot installs — up to 20 minutes.",
                    "steps": [
                        {
                            "title": "Find it on your network",
                            "variants": {
                                "mac": {"cmds": [{"cmd": "ping -c 3 homeassistant.local", "out": []}],
                                        "bullets": ["Or just open the URL below in Safari and wait for onboarding."]},
                                "win": {"cmds": [{"cmd": "ping homeassistant.local", "out": []}],
                                        "bullets": ["Run it in PowerShell — or just open the URL below in your browser."]},
                                "linux": {"cmds": [
                                    {"cmd": s["cmd"], "out": s.get("out", []), "note": s.get("note", "")}
                                    for s in hub_chapters["boot"]["steps"]
                                ]},
                            },
                            "copies_all": [{"label": "Home Assistant onboarding", "text": "http://homeassistant.local:8123"}],
                        },
                        {
                            "title": "Create your account, then continue above",
                            "variants": {"all": {"bullets": [
                                "Walk the onboarding (name, location, account).",
                                "Then switch to the “I already run Home Assistant” mission — you run it now.",
                            ]}},
                            "next_mission": "ha",
                        },
                    ],
                },
            ],
        },
        {
            "id": "docker",
            "glyph": "🐳",
            "title": "I run Docker (Frigate already, or homelab)",
            "line": "One compose file next to your existing stack — the kernel rides alongside Frigate.",
            "time": "~10 minutes",
            "difficulty": "medium",
            "chapters": [
                {
                    "id": "compose",
                    "title": "1 · Compose it",
                    "intro": "The full quickstart (compose file, bundled-broker variant, API tokens) is in the written guide — these are the beats.",
                    "steps": [
                        {
                            "title": "Install Docker",
                            "variants": {
                                "mac": {"bullets": ["Install Docker Desktop for Mac, then use its terminal — commands below are identical."],
                                        "links": [{"label": "Docker Desktop (Mac)", "href": "https://www.docker.com/products/docker-desktop/"}]},
                                "win": {"bullets": ["Install Docker Desktop for Windows (WSL2 backend), then use PowerShell or the WSL shell."],
                                        "links": [{"label": "Docker Desktop (Windows)", "href": "https://www.docker.com/products/docker-desktop/"}]},
                                "linux": {"cmds": [{"cmd": "curl -fsSL https://get.docker.com | sh", "out": []}]},
                            },
                        },
                        {
                            "title": "Fetch the quickstart and bring it up",
                            "doc": "docs/frigate_integration.md",
                            "variants": {"all": {"cmds": [
                                {"cmd": docker_cmds[0], "out": [], "note": "Grabs the quickstart compose file — these lines are the guide's own, extracted at build time."},
                                {"cmd": docker_cmds[1], "out": []},
                                {"cmd": docker_cmds[2], "out": [], "note": "The end-to-end check: broker, Frigate events, chain, signatures."},
                            ]}},
                        },
                    ],
                },
            ],
        },
        {
            "id": "builder",
            "glyph": "🐤",
            "title": "I like building little devices",
            "line": "ESP32 Canaries: independent witnesses you flash from your browser — no toolchain, no OS difference.",
            "time": "an afternoon you'll enjoy",
            "difficulty": "fun",
            "chapters": [
                {
                    "id": "lab",
                    "title": "1 · Start in the Lab",
                    "intro": "Your browser does the flashing over WebSerial — macOS, Windows, and Linux are the same three clicks.",
                    "steps": [
                        {
                            "title": "Meet the family, pick your bird",
                            "variants": {"all": {"links": [
                                {"label": "Find your Canary (4 questions)", "href": "choose.html"},
                                {"label": "Meet the Canaries (live firmware)", "href": "index.html"},
                                {"label": "The Board Room (wiring)", "href": "boards.html"},
                            ]}},
                        },
                        {
                            "title": "Build and flash it",
                            "doc": "docs/getting_started_canary.md",
                            "variants": {"all": {"bullets": [
                                "The Workshop specs your exact build (parts, pins, enclosure).",
                                "Kits and BOMs: see Build It in the Lab — plans are free forever.",
                            ], "links": [{"label": "The Workshop", "href": "workshop.html"}]}},
                        },
                    ],
                },
            ],
        },
    ]

    out = {
        "$note": "GENERATED by canary-local/tools/gen_start.py — do not edit by hand. "
                 "Versions ride devices/homeassistant.json's drift-gated snapshot; install "
                 "strings are extracted from README.md.",
        "fw_train": registry["fw_train"],
        "upstream": hub["upstream"],
        "integration": {"version": hub["integration"]["version"], "min_ha": hub["integration"]["min_ha"]},
        "readme": readme,
        "oses": OSES,
        "missions": MISSIONS,
        "docs": {"setup": "docs/homeassistant_setup.md", "frigate": "docs/frigate_integration.md",
                 "canary": "docs/getting_started_canary.md", "index": "docs/README.md"},
    }

    OUT_JSON.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
    print(f"wrote {OUT_JSON.relative_to(REPO)}")


if __name__ == "__main__":
    main()
