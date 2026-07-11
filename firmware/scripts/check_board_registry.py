#!/usr/bin/env python3
"""Board registry guard — keeps firmware/boards/boards.json honest.

firmware/boards/boards.json is the single source of truth for every board
the firmware tree supports (the board-level counterpart of flavors.json).
This guard enforces the contract between the registry, the board
directories, the pin headers, the build environments, and the flavor
manifest, so the "supported boards" story can never silently drift the way
a hand-maintained README table does.

Checks:
  1. Registry entries are well-formed: required fields, unique ids, a
     valid support tier, and tier evidence for any tier above
     compile-tested.
  2. Registry <-> firmware/boards/ directories match one-to-one.
  3. Every board dir ships README.md and pins/pins.h.
  4. pins/pins.h defines BOARD_ID matching the directory name, defines
     the baseline capability flags, and contains ONLY preprocessor
     directives (pins are data, not code).
  5. HAS_PSRAM in pins.h agrees with psram_mb in the registry.
  6. Every used_by flavor exists in firmware/flavors.json.
  7. Every boards/<id>/pins include path referenced from
     firmware/envs/platformio/*.ini points at a registered board.
  8. firmware/boards/README.md mentions every registered board id.

Run from the repo root (CI) or anywhere inside the repo:
    python3 firmware/scripts/check_board_registry.py
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BOARDS_DIR = REPO_ROOT / "firmware" / "boards"
REGISTRY = BOARDS_DIR / "boards.json"
FLAVORS = REPO_ROOT / "firmware" / "flavors.json"
ENVS_DIR = REPO_ROOT / "firmware" / "envs" / "platformio"
BOARDS_README = BOARDS_DIR / "README.md"

VALID_TIERS = ("verified", "community", "compile-tested")
REQUIRED_FIELDS = ("id", "name", "vendor", "mcu", "flash_mb", "psram_mb",
                   "pio_board", "tier", "tier_evidence", "used_by", "notes")
# Every board must take a position on these, even if the answer is 0 —
# feature code gates on them, and an undefined flag is an accidental 0.
BASELINE_CAPS = ("HAS_CAMERA", "HAS_MICROPHONE", "HAS_SD_CARD", "HAS_PSRAM",
                 "HAS_USB_CDC", "HAS_WIFI", "HAS_BLE")

errors = []


def err(msg: str) -> None:
    errors.append(msg)


def strip_comments(src: str) -> str:
    # Block comments become an equal number of newlines so line numbers in
    # the stripped text still match the original file in error reports.
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src,
                 flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def check_registry_shape(entries):
    seen = set()
    for e in entries:
        bid = e.get("id", "<missing id>")
        for field in REQUIRED_FIELDS:
            if field not in e:
                err(f"{bid}: registry entry missing field '{field}'")
        if bid in seen:
            err(f"{bid}: duplicate registry entry")
        seen.add(bid)
        if e.get("tier") not in VALID_TIERS:
            err(f"{bid}: tier '{e.get('tier')}' not one of {VALID_TIERS}")
        if e.get("tier") in ("verified", "community"):
            evidence = e.get("tier_evidence", "")
            if not evidence:
                err(f"{bid}: tier '{e.get('tier')}' requires tier_evidence "
                    "(a repo path or issue URL documenting the hardware test)")
            elif "://" not in evidence and not (REPO_ROOT / evidence).exists():
                err(f"{bid}: tier_evidence '{evidence}' does not exist in the repo")


def check_dirs_match(entries):
    registry_ids = {e["id"] for e in entries if "id" in e}
    dir_ids = {d.name for d in BOARDS_DIR.iterdir() if d.is_dir()}
    for missing in sorted(dir_ids - registry_ids):
        err(f"{missing}: board directory has no boards.json entry — "
            "register it (see firmware/PORTING.md)")
    for ghost in sorted(registry_ids - dir_ids):
        err(f"{ghost}: registered in boards.json but firmware/boards/{ghost}/ "
            "does not exist")
    return registry_ids & dir_ids


def check_board_dir(entry):
    bid = entry["id"]
    board_dir = BOARDS_DIR / bid
    readme = board_dir / "README.md"
    pins = board_dir / "pins" / "pins.h"
    if not readme.is_file():
        err(f"{bid}: missing README.md (board metadata and constraints)")
    if not pins.is_file():
        err(f"{bid}: missing pins/pins.h")
        return

    src = pins.read_text(encoding="utf-8", errors="replace")
    bare = strip_comments(src)

    # Pins are data: only preprocessor directives survive comment-stripping.
    for lineno, line in enumerate(bare.splitlines(), 1):
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            err(f"{bid}: pins/pins.h:{lineno} is not a preprocessor "
                f"directive: '{stripped[:60]}' — pin files must contain "
                "only #define/#pragma/#if directives")

    m = re.search(r'#define\s+BOARD_ID\s+"([^"]+)"', bare)
    if not m:
        err(f"{bid}: pins/pins.h does not define BOARD_ID")
    elif m.group(1) != bid:
        err(f"{bid}: BOARD_ID is \"{m.group(1)}\" but the directory (and "
            f"registry id) is \"{bid}\"")

    caps = dict(re.findall(r"#define\s+(HAS_\w+)\s+(\d+)", bare))
    for cap in BASELINE_CAPS:
        if cap not in caps:
            err(f"{bid}: pins/pins.h missing baseline capability flag {cap} "
                "(define it 0 or 1 — an undefined flag is an accidental 0)")

    psram_mb = entry.get("psram_mb")
    if "HAS_PSRAM" in caps and isinstance(psram_mb, int):
        has = caps["HAS_PSRAM"] != "0"
        if has != (psram_mb > 0):
            err(f"{bid}: HAS_PSRAM={caps['HAS_PSRAM']} in pins.h but "
                f"psram_mb={psram_mb} in boards.json")


def check_flavor_refs(entries):
    if not FLAVORS.is_file():
        err("firmware/flavors.json not found")
        return
    flavor_names = {f["name"] for f in json.loads(FLAVORS.read_text())}
    for e in entries:
        for flavor in e.get("used_by", []):
            if flavor not in flavor_names:
                err(f"{e['id']}: used_by flavor '{flavor}' not in "
                    "firmware/flavors.json")


def check_env_refs(registry_ids):
    pattern = re.compile(r"boards/([A-Za-z0-9_-]+)/pins")
    for ini in sorted(ENVS_DIR.glob("*.ini")):
        for lineno, line in enumerate(ini.read_text().splitlines(), 1):
            for ref in pattern.findall(line):
                if ref not in registry_ids:
                    err(f"{ini.relative_to(REPO_ROOT)}:{lineno}: references "
                        f"unregistered board '{ref}'")


def check_readme_mentions(registry_ids):
    if not BOARDS_README.is_file():
        err("firmware/boards/README.md not found")
        return
    text = BOARDS_README.read_text()
    for bid in sorted(registry_ids):
        if bid not in text:
            err(f"firmware/boards/README.md does not mention '{bid}' — "
                "keep the board index in sync with boards.json")


def main() -> int:
    try:
        entries = json.loads(REGISTRY.read_text())
    except (OSError, json.JSONDecodeError) as e:
        print(f"::error::cannot load {REGISTRY}: {e}")
        return 1
    if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries):
        print(f"::error::{REGISTRY} must be a JSON array of board objects "
              "(same shape as flavors.json)")
        return 1

    check_registry_shape(entries)
    registry_ids = check_dirs_match(entries)
    for entry in entries:
        if entry.get("id") in registry_ids:
            check_board_dir(entry)
    check_flavor_refs(entries)
    check_env_refs(registry_ids)
    check_readme_mentions(registry_ids)

    if errors:
        for e in errors:
            print(f"::error::board registry: {e}")
        print(f"\n{len(errors)} board registry error(s). "
              "See firmware/PORTING.md for the board contract.")
        return 1

    print(f"Board registry OK: {len(registry_ids)} boards registered, "
          "directories, pins, envs, and flavors all consistent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
