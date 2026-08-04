#!/usr/bin/env python3
"""scripts/lint_board_design.py — a carrier board cannot drift from its firmware.

The whole cost case for `boards/canary-witness-s3` rests on one claim: the
carrier is pin-for-pin identical to the module it replaces, so the existing
*verified* firmware runs on it unmodified and the existing bench runbook is
its acceptance test. That claim is worth exactly as much as its enforcement.
A GPIO that moves in `sd_storage.h` and not in `board.json` turns a drop-in
carrier into a respin nobody noticed — discovered, at best, on a populated
board.

So four things are checked, and all of them read the *real* sources:

  1. PIN MAP MATCHES FIRMWARE — every entry in board.json's `pin_map` names a
     source file and a token. We re-read that file, parse the token's value,
     and fail if it disagrees. Not a snapshot of the firmware: the firmware.

  2. NETS RESOLVE — every node references a declared part (or a documented
     virtual designator), every `signal` names a row in the pin map, and the
     GPIO in a net's node (U1.IO42) agrees with the pin map's number for that
     signal. A net that says IO42 for a signal the firmware puts on GPIO 41 is
     the exact defect this gate exists for.

  3. NO ORPHAN NETS — a net with fewer than two nodes connects nothing. In a
     hand-authored netlist that is a typo, every time.

  4. HONESTY FIELDS PRESENT — anything without distributor-verified pricing
     carries a `basis` string, and the board carries a `confidence` from the
     same ladder the fleet figures use. An estimate that does not say it is an
     estimate is the failure mode this whole pipeline is built against.

Run:  python3 scripts/lint_board_design.py
CI:   .github/workflows/lint.yml (Repo Lints)
"""
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BOARDS = REPO / "boards"

LADDER = {"idea", "prototype", "confirmed", "shipping"}

# Virtual designators: real nodes on the board that are not standalone parts.
# Kept explicit so a typo'd refdes can't hide behind a permissive rule.
VIRTUAL_REFS = {"R-DIV"}


def token_value(path: Path, token: str) -> int | None:
    """Read an integer constant out of a C/C++ header or source file.

    Handles both spellings the firmware actually uses:
        #define CAM_PIN_XCLK    10
        static const int SD_CS_PIN = 21;
        static constexpr uint8_t ADC_GPIO = 1;
    """
    if not path.exists():
        return None
    text = path.read_text(encoding="utf-8", errors="replace")
    patterns = (
        rf"#\s*define\s+{re.escape(token)}\s+(-?\d+)",
        rf"\b{re.escape(token)}\s*=\s*(-?\d+)",
    )
    for pat in patterns:
        m = re.search(pat, text)
        if m:
            return int(m.group(1))
    return None


def lint_board(design_path: Path, failures: list) -> None:
    rel = design_path.relative_to(REPO)
    design = json.loads(design_path.read_text(encoding="utf-8"))
    bid = design.get("id", str(rel))

    def fail(msg: str) -> None:
        failures.append(f"{bid}: {msg}")

    # ── 4. Honesty fields ───────────────────────────────────────────────────
    if design.get("confidence") not in LADDER:
        fail(
            f"confidence {design.get('confidence')!r} is not on the ladder "
            f"({'/'.join(sorted(LADDER))}) — see canary-local/devices/figures.json"
        )
    for part in design.get("parts", []):
        if part.get("est_unit_usd") is not None and not part.get("basis"):
            fail(
                f"part {part['ref']} ({part['mpn']}) carries an estimated price "
                "with no `basis` — say where the number came from"
            )

    # ── 1. Pin map matches the firmware it claims to match ──────────────────
    pin_map = design.get("pin_map", {})
    for signal, spec in pin_map.items():
        if signal.startswith("_"):
            continue
        src = REPO / spec["source"]
        token = spec["token"]
        actual = token_value(src, token)
        if actual is None:
            fail(
                f"pin_map[{signal}] cites {spec['source']}:{token}, but that "
                "token is not defined there (renamed? file moved?)"
            )
        elif actual != spec["gpio"]:
            fail(
                f"pin_map[{signal}] says GPIO {spec['gpio']}, but "
                f"{spec['source']} defines {token} = {actual}. The firmware "
                "moved and the board did not — this carrier is no longer "
                "drop-in."
            )

    # ── 2/3. Net coherence ──────────────────────────────────────────────────
    declared = {p["ref"] for p in design.get("parts", [])} | VIRTUAL_REFS
    # Connector-ish refs appear in nets with pin names; that is fine. What is
    # not fine is a ref nobody declared.
    all_pins = {**pin_map, **design.get("pin_map_unenforced", {})}
    seen_nets = set()

    for group in ("power", "signals"):
        for net in design.get("nets", {}).get(group, []):
            name = net["net"]
            if name in seen_nets:
                fail(f"net {name} is declared twice")
            seen_nets.add(name)

            nodes = net.get("nodes", [])
            if len(nodes) < 2:
                fail(
                    f"net {name} has {len(nodes)} node(s) — a net that connects "
                    "fewer than two things connects nothing"
                )

            for node in nodes:
                ref, dot, pin = node.partition(".")
                if not dot:
                    fail(f"net {name} node {node!r} is not in REF.PIN form")
                    continue
                if ref not in declared:
                    fail(
                        f"net {name} references part {ref!r}, which is not in "
                        "`parts` (and is not a documented virtual designator)"
                    )

            sig = net.get("signal")
            if sig:
                if sig not in all_pins:
                    fail(
                        f"net {name} names signal {sig!r}, which is in neither "
                        "pin_map nor pin_map_unenforced"
                    )
                    continue
                gpio = all_pins[sig]["gpio"]
                # The MCU node must name the same GPIO the pin map fixes.
                mcu = [n for n in nodes if n.startswith("U1.IO")]
                for node in mcu:
                    got = int(node.split(".IO", 1)[1])
                    if got != gpio:
                        fail(
                            f"net {name} wires {node} but signal {sig!r} is "
                            f"GPIO {gpio} — the netlist and the pin map "
                            "disagree"
                        )

    # Every enforced signal should actually be routed somewhere.
    routed = {
        n.get("signal")
        for group in ("power", "signals")
        for n in design.get("nets", {}).get(group, [])
        if n.get("signal")
    }
    for signal in pin_map:
        if signal.startswith("_"):
            continue
        if signal not in routed:
            fail(
                f"pin_map[{signal}] is enforced against the firmware but no net "
                "routes it — the board would not implement it"
            )


def main() -> int:
    designs = sorted(BOARDS.glob("*/board.json"))
    if not designs:
        print("lint_board_design.py: no boards/*/board.json — nothing to check")
        return 0

    failures: list = []
    for path in designs:
        lint_board(path, failures)

    if failures:
        print("lint_board_design.py: board design does not match its sources\n")
        for f in failures:
            print(f"  {f}")
        print(
            f"\n{len(failures)} problem(s). A carrier board's whole cost case is "
            "that the firmware does not change; these are the ways that stops "
            "being true."
        )
        return 1

    print(f"lint_board_design.py: {len(designs)} board design(s) OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
