#!/usr/bin/env python3
"""Is the OTA release signing key ready? Answer it without needing the key.

Signing a firmware release needs two halves that live in different places:

    the PUBLIC half   committed at firmware/common/ota/src/ota_release_key.h
    the PRIVATE half  the OTA_SIGNING_KEY_PEM GitHub Actions secret

If either is missing, `firmware-release.yml` fails ~20 seconds in at its key
guard and publishes nothing — which is correct, but it used to be discovered
*after* pressing a release button, in a different workflow run, with the
consequences ("the in-browser flasher stays dark, nothing can update") three
inferences away from the error text.

This reports the half that is readable from the tree, so a button can say
"firmware is not releasable, and here is the one-time ceremony" BEFORE it
dispatches anything. It deliberately cannot verify that the two halves MATCH —
that needs the private key, and it is the release workflow's job (it fails
closed if they have drifted).

An all-zero key is the shipped default and means OTA is HARD-DISABLED in
firmware: the pull-OTA engine and the BLE OTA path both refuse to install.
That is a safe default, not a bug — but it is also invisible unless something
says so out loud.

Usage:
    python3 firmware/scripts/ota_key_state.py            # human-readable
    python3 firmware/scripts/ota_key_state.py --json     # {"embedded":…, "key_id":…}

Exit status is 0 whether or not a key is embedded — this REPORTS, it does not
gate. Callers decide what to do with the answer. Exit 1 means the header could
not be read at all, which is a real problem with the tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HEADER = REPO / "firmware/common/ota/src/ota_release_key.h"
ROTATION_HEADER = REPO / "firmware/common/ota/src/ota_release_key_previous.h"

ARRAY = re.compile(
    r"SECURACV_OTA_RELEASE_PUBKEY\[32\]\s*=\s*\{(.*?)\};", re.DOTALL
)
BYTE = re.compile(r"0x([0-9a-fA-F]{2})")


def read_pubkey(path: Path) -> bytes | None:
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return None
    m = ARRAY.search(text)
    if not m:
        return None
    raw = bytes(int(b, 16) for b in BYTE.findall(m.group(1)))
    return raw if len(raw) == 32 else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    args = ap.parse_args()

    key = read_pubkey(HEADER)
    if key is None:
        msg = (
            f"could not read SECURACV_OTA_RELEASE_PUBKEY[32] out of "
            f"{HEADER.relative_to(REPO)} — the header is missing or malformed"
        )
        print(f"::error::{msg}" if not args.json else json.dumps({"error": msg}))
        return 1

    embedded = any(key)
    # Same shape setup_release_key.sh prints, so the two can be compared by eye.
    key_id = hashlib.sha256(key).hexdigest()[:16] if embedded else None
    rotating = ROTATION_HEADER.exists()

    if args.json:
        print(json.dumps({
            "embedded": embedded,
            "key_id": key_id,
            "rotation_window_open": rotating,
        }))
        return 0

    if embedded:
        print(f"OTA release public key: EMBEDDED (key id {key_id})")
        print("  Firmware built from this tree will accept OTA updates signed by")
        print("  the matching private key. CI cross-checks the match at release time.")
        if rotating:
            print("  ROTATION WINDOW OPEN: ota_release_key_previous.h is committed, so")
            print("  releases must still be signed with the PREVIOUS key until the")
            print("  fleet converges. Delete that file to close the window.")
    else:
        print("OTA release public key: ALL ZEROS — OTA is hard-disabled.")
        print("  No firmware release can be signed, and no device can install an")
        print("  update, until the one-time key ceremony is done:")
        print()
        print("    firmware/scripts/setup_release_key.sh --key ~/securacv-releaser.pem")
        print()
        print("  Run it on your OWN machine, never in CI or a cloud shell. Then add")
        print("  the private PEM as the OTA_SIGNING_KEY_PEM Actions secret and commit")
        print("  the public headers plus the regenerated canary-local/devices/flash.json.")
        print("  Full walkthrough: docs/RELEASE_BUTTONS.md § Firmware will not release")
    return 0


if __name__ == "__main__":
    sys.exit(main())
