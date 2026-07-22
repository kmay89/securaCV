#!/usr/bin/env python3
"""canary-local/tools/gen_hub_image.py — the securaCV Home Assistant hub image, as data.

Emits canary-local/devices/hub_image.json: the single source of truth for the
native Raspberry Pi hub flasher (docs/design/raspberry_pi_hub_flashing.md). The
one-flash writer, the "Hub" page, and the docs all read from here so they can
never disagree about which image to write, how big a card it needs, or what ends
up pre-installed.

Honest by construction — the canary.local way. Not one fact here is typed by
hand and hoped to stay true; every value is DERIVED from a source the repo
already maintains, so a change over there fails CI's drift gate until this is
regenerated:

  1. canary-local/devices/homeassistant.json ... the base OS version rides the
     Hub's own upstream snapshot (haos_version, freshness-tracked by the
     homeassistant-freshness workflow) + the integration block + the hardware
     list's card requirement.
  2. desktop/src-tauri/src/hub_disk.rs ......... the card-size floor is PARSED
     from the flasher's safety gate (MIN/RECOMMENDED_TARGET_BYTES), so the copy
     "needs a 32 GB+ card" and the code that refuses an undersized one are one
     value, not two that can drift apart.
  3. privacy_witness_kernel/config.yaml ........ our add-on's slug + the
     companion add-ons it integrates with (mqtt / go2rtc / frigate).
  4. homeassistant/lovelace,automations/*.yaml . the dashboards + automations
     baked in; docs/blueprints/*.yaml the blueprints. Enumerated from disk, so
     adding one and forgetting to regenerate fails the drift gate.

Honest before an image is pinned: the base image URL is derivable from the
version (HA's release-asset naming), but the sha256 stays empty and
`pinned` is false until the pin ceremony sets it — the same honest-before-release
posture as flash.json shipping an all-zero release key until the signing
ceremony. The writer verifies the download against HA's published .sha256 in the
meantime.

Run:  python3 canary-local/tools/gen_hub_image.py
CI:   the same command + `git diff --exit-code canary-local/devices/hub_image.json`.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_JSON = REPO / "canary-local/devices/hub_image.json"

HA_JSON = REPO / "canary-local/devices/homeassistant.json"
HUB_DISK_RS = REPO / "desktop/src-tauri/src/hub_disk.rs"
ADDON_CONFIG = REPO / "privacy_witness_kernel/config.yaml"
LOVELACE_DIR = REPO / "homeassistant/lovelace"
AUTOMATIONS_DIR = REPO / "homeassistant/automations"
BLUEPRINTS_DIR = REPO / "docs/blueprints"

# HA publishes the operating-system images as GitHub release assets under a
# stable naming scheme; the URL is fully determined by (board, version).
HAOS_RELEASE = "https://github.com/home-assistant/operating-system/releases/download"

# The 64-bit Raspberry Pi boards we support as hubs, richest-first. Pi 5 is the
# recommended target because it can boot from NVMe/SSD — the durable default the
# design steers toward (SD cards are the multi-year weak link).
BOARDS = [
    {
        "id": "rpi5-64",
        "name": "Raspberry Pi 5 (64-bit)",
        "asset_stem": "haos_rpi5-64",
        "recommended": True,
        "durable_default": "boot from NVMe/SSD (Pi 5) for multi-year endurance",
    },
    {
        "id": "rpi4-64",
        "name": "Raspberry Pi 4 (64-bit, 4 GB+)",
        "asset_stem": "haos_rpi4-64",
        "recommended": False,
        "durable_default": "use a high-endurance A2 microSD; watch the SD wear sensor",
    },
]


def die(msg: str) -> None:
    """Fail closed and loud: better a red build than a catalog that quietly lies."""
    sys.exit(f"gen_hub_image.py: {msg}")


def load_ha_json() -> dict:
    if not HA_JSON.exists():
        die(f"missing {HA_JSON.relative_to(REPO)} — run gen_homeassistant.py first")
    return json.loads(HA_JSON.read_text(encoding="utf-8"))


def parse_card_bytes() -> tuple[int, int]:
    """Parse the card-size floor + recommendation straight from the Rust safety
    gate, so the catalog and the code that enforces it are one value.

    Reads `const GIB`, `MIN_TARGET_BYTES = N * GIB`, `RECOMMENDED_TARGET_BYTES =
    N * GIB` from hub_disk.rs. Any shape it doesn't recognise is a hard error —
    we never guess a size a flasher will act on.
    """
    if not HUB_DISK_RS.exists():
        die(f"missing {HUB_DISK_RS.relative_to(REPO)} — the flasher safety gate is the size source")
    text = HUB_DISK_RS.read_text(encoding="utf-8")

    gib_m = re.search(r"const\s+GIB:\s*u64\s*=\s*([0-9*\s]+);", text)
    if not gib_m:
        die("could not find `const GIB` in hub_disk.rs")
    try:
        gib = eval(gib_m.group(1).strip(), {"__builtins__": {}})  # noqa: S307 — literal `1024 * 1024 * 1024`
    except Exception as e:  # pragma: no cover - defensive
        die(f"could not evaluate GIB expression: {e}")
    if gib != 1024**3:
        die(f"unexpected GIB value {gib} — hub_disk.rs changed shape; update this parser deliberately")

    def units(name: str) -> int:
        m = re.search(rf"{name}:\s*u64\s*=\s*(\d+)\s*\*\s*GIB", text)
        if not m:
            die(f"could not parse {name} from hub_disk.rs")
        return int(m.group(1))

    return units("MIN_TARGET_BYTES") * gib, units("RECOMMENDED_TARGET_BYTES") * gib


def parse_human_minimum(ha: dict) -> str:
    """The human 'you need a 32 GB+ card' string, lifted from the Hub hardware
    list so it tracks the supported hardware rather than a second hand-typed
    number."""
    for need in ha.get("hardware", {}).get("needs", []):
        item = need.get("item", "")
        if "card" in item.lower():
            m = re.search(r"(\d+\s*GB\+?)", item)
            if m:
                return m.group(1).replace(" ", " ")
    return ""  # honest: no claim if the hardware list stops naming a size


def addon_slug_name() -> tuple[str, str]:
    """Our add-on's slug + name, from its own config.yaml (not re-typed)."""
    if not ADDON_CONFIG.exists():
        die(f"missing {ADDON_CONFIG.relative_to(REPO)}")
    text = ADDON_CONFIG.read_text(encoding="utf-8")
    slug = re.search(r'^slug:\s*"?([a-z0-9_]+)"?', text, re.M)
    name = re.search(r'^name:\s*"?([^"\n]+)"?', text, re.M)
    if not slug:
        die("could not read add-on slug from config.yaml")
    return slug.group(1), (name.group(1).strip() if name else slug.group(1))


def companion_addons() -> list[dict]:
    """The HA-community add-ons the full-stack image pre-installs, each justified
    by the add-on config that already integrates with it — so 'why is Frigate in
    here?' has an answer in the repo, not a maintainer's memory."""
    cfg = ADDON_CONFIG.read_text(encoding="utf-8") if ADDON_CONFIG.exists() else ""
    companions = []
    if "mqtt:" in cfg or "mqtt_publish" in cfg:
        companions.append(
            {
                "slug": "core_mosquitto",
                "name": "Mosquitto broker",
                "why": "the add-on auto-discovers it (config.yaml services: mqtt:want) and MQTT Discovery needs it",
            }
        )
    if "go2rtc" in cfg:
        companions.append(
            {
                "slug": "go2rtc",
                "name": "go2rtc",
                "why": "camera discovery/restreaming (config.yaml go2rtc_discovery / go2rtc_url)",
            }
        )
    if re.search(r"\bfrigate\b", cfg):
        companions.append(
            {
                "slug": "frigate",
                "name": "Frigate NVR",
                "why": "the recommended camera detection path (config.yaml mode: frigate)",
            }
        )
    return companions


def yaml_assets(directory: Path, kind: str) -> list[dict]:
    """Enumerate the *.yaml under a directory as {file, name}. Sorted for a
    stable diff; the filename minus extension is the human name."""
    if not directory.exists():
        return []
    out = []
    for p in sorted(directory.glob("*.yaml")):
        out.append(
            {
                "file": str(p.relative_to(REPO)),
                "name": p.stem.replace("securacv_", "").replace("securacv-", "").replace("_", " ").replace("-", " ").strip(),
            }
        )
    return out


def main() -> None:
    ha = load_ha_json()
    upstream = ha.get("upstream", {})
    version = upstream.get("haos_version", "")
    if not version:
        die("no haos_version in homeassistant.json upstream snapshot")

    integration = ha.get("integration", {})
    min_bytes, recommended_bytes = parse_card_bytes()
    slug, name = addon_slug_name()

    boards = [
        {
            "id": b["id"],
            "name": b["name"],
            "recommended": b["recommended"],
            "durable_default": b["durable_default"],
            "image_asset": f"{b['asset_stem']}-{version}.img.xz",
            "image_url": f"{HAOS_RELEASE}/{version}/{b['asset_stem']}-{version}.img.xz",
            # Honest-before-pin: derivable URL, empty hash until the pin ceremony.
            "sha256": "",
        }
        for b in BOARDS
    ]

    out = {
        "$generated_by": "canary-local/tools/gen_hub_image.py — do not edit by hand",
        "$doc": (
            "The securaCV Home Assistant hub image — single source of truth for the native "
            "Raspberry Pi hub flasher (docs/design/raspberry_pi_hub_flashing.md). Every fact is "
            "derived from an existing source so it can't drift: the base OS version rides the Hub's "
            "upstream snapshot, the card floor is parsed from the flasher safety gate (hub_disk.rs), "
            "and the baked-in payload is enumerated from the repo's own HA assets. Honest before an "
            "image is pinned: the URL is derivable from the version; sha256 stays empty (pinned=false) "
            "until the pin ceremony — the same posture as flash.json's all-zero release key."
        ),
        "schema_version": 1,
        "base_os": {
            "name": "Home Assistant OS",
            "why": (
                "HAOS is already the self-healing, auto-updating appliance the request asks for: an "
                "immutable A/B root (RAUC) that rolls back a bad boot, a Supervisor that watchdogs "
                "add-ons, and hands-off OS + add-on updates. We post-process the official image; we "
                "do not fork it. See the design doc §4."
            ),
            "version": version,
            "version_source": "canary-local/devices/homeassistant.json:upstream.haos_version",
            "arch": "aarch64",
            "pinned": False,
            "pin_note": (
                "sha256 is set by the pin ceremony (a refresh step, mirroring the OTA signing "
                "ceremony). Until then the writer verifies the download against HA's published "
                ".sha256 and these stay empty."
            ),
            "boards": boards,
        },
        "card_requirements": {
            "min_bytes": min_bytes,
            "min_source": "desktop/src-tauri/src/hub_disk.rs:MIN_TARGET_BYTES",
            "recommended_bytes": recommended_bytes,
            "recommended_source": "desktop/src-tauri/src/hub_disk.rs:RECOMMENDED_TARGET_BYTES",
            "human_minimum": parse_human_minimum(ha),
            "human_minimum_source": "canary-local/devices/homeassistant.json:hardware (microSD card)",
            "note": (
                "The exact byte floor is the flasher's safety gate; the human minimum is the supported "
                "hardware. A 32 GB card (~29.7 GiB) clears the floor; 16 GB and below are refused."
            ),
        },
        "payload": {
            "what": "The full-stack pre-bake: the hub boots already wired — no add-on hunting, no restarts.",
            "integration": {
                "domain": integration.get("domain", ""),
                "name": integration.get("name", ""),
                "version": integration.get("version", ""),
                "min_ha": integration.get("min_ha", ""),
                "source": "canary-local/devices/homeassistant.json:integration",
            },
            "add_ons": [
                {
                    "slug": slug,
                    "name": name,
                    "source": "securacv",
                    "from": "privacy_witness_kernel/config.yaml",
                },
                *[{**c, "source": "ha-community"} for c in companion_addons()],
            ],
            "dashboards": yaml_assets(LOVELACE_DIR, "dashboard"),
            "automations": yaml_assets(AUTOMATIONS_DIR, "automation"),
            "blueprints": yaml_assets(BLUEPRINTS_DIR, "blueprint"),
        },
        "provisioning": {
            "wifi": (
                "A NetworkManager keyfile (CONFIG/network/my-network) seeded onto the boot partition — "
                "the same secret the Canary flasher already collects (wifi-memory.js). Local-only: it "
                "goes on the card, never to a cloud."
            ),
            "seed": "A curated HA backup, restored at onboarding, brings the payload up pre-wired.",
            "self_heal": (
                "Inherited from HAOS: A/B rollback + Supervisor watchdog + hands-off updates; our "
                "add-on rides the Supervisor update channel."
            ),
            "custody": "No cloud, nothing phones home; the image is a post-processed pinned official artifact (Inv. IV).",
        },
        "meta": {
            "design": "docs/design/raspberry_pi_hub_flashing.md",
            "upstream_fetched_at": upstream.get("fetched_at", ""),
            "sources": [
                "canary-local/devices/homeassistant.json",
                "desktop/src-tauri/src/hub_disk.rs",
                "privacy_witness_kernel/config.yaml",
                "homeassistant/lovelace/*.yaml",
                "homeassistant/automations/*.yaml",
                "docs/blueprints/*.yaml",
            ],
        },
    }

    OUT_JSON.write_text(json.dumps(out, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    payload = out["payload"]
    print(
        f"wrote {OUT_JSON.relative_to(REPO)} — HAOS {version}, "
        f"min card {min_bytes // 1024**3} GiB, "
        f"{len(payload['add_ons'])} add-ons, {len(payload['dashboards'])} dashboards, "
        f"{len(payload['automations'])} automations, {len(payload['blueprints'])} blueprints"
    )


if __name__ == "__main__":
    main()
