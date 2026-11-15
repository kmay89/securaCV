#!/usr/bin/env python3
"""The WAP's BOM names only build flags the WAP sketch has.

docs/hardware/bom_canary_wap.csv is read by gen_enclosures.py into
canary-local/devices/build.json, whose rows the Lab's build pages show. Its
GNSS row (M1) ended its note with `FEATURE_GNSS`, a flag of the canary
(PlatformIO) tree: the WAP sketch opens its GNSS UART at every boot and has
no such flag (sweep F258 moved the WAP's flags to the sketch's
build_config.h; sweep C19 reworded the note). Nothing gated the note, so a
builder reading it went looking for a switch that does not exist.

This test holds every `FEATURE_*` token anywhere in the WAP's BOM (part rows
and recipe comment rows alike) to the flags the sketch's build_config.h
names. A flag the BOM wants to mention has to exist where the WAP is built.

The board viewer carried the same phantom (sweep C20): the XIAO ESP32-S3
Sense's GNSS UART row in boards/boards.config.json, copied by gen_boards.py
into canary-local/devices/boards.json for the Lab's board pages, read
`L76K TX/RX @ 9600 (FEATURE_GNSS)`, and that board's only device is the WAP.
So the same rule binds every string of every board whose only device is
canary-wap, in the config and in the generated file alike. A board of any
other device may not name a flag at all until this test is taught where that
device's flags live: a row naming one would otherwise go unchecked.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_wap_bom_flags.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BOM = REPO / "docs" / "hardware" / "bom_canary_wap.csv"
BUILD_CONFIG = (REPO / "firmware" / "projects" / "canary-wap" / "arduino"
                / "canary_wap" / "build_config.h")
BOARDS_CONFIG = REPO / "boards" / "boards.config.json"
BOARDS_JSON = REPO / "canary-local" / "devices" / "boards.json"

FLAG = re.compile(r"\bFEATURE_[A-Z0-9_]+\b")


def sketch_flags(text: str) -> set[str]:
    return set(FLAG.findall(text))


def phantom_flags(bom_text: str, flags: set[str]) -> list[tuple[int, str]]:
    """(line, flag) for each FEATURE_* token in the BOM the sketch lacks."""
    out = []
    for n, line in enumerate(bom_text.splitlines(), 1):
        for tok in FLAG.findall(line):
            if tok not in flags:
                out.append((n, tok))
    return out


def strings(node, path: str):
    """(path, text) for every string under `node`, keys included."""
    if isinstance(node, str):
        yield path, node
    elif isinstance(node, dict):
        for k, v in node.items():
            yield f"{path}.{k}", k
            yield from strings(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from strings(v, f"{path}[{i}]")


def board_records(doc: dict) -> list[tuple[str, dict]]:
    """(id, record) for each board, from the config's list or boards.json's
    map: the generator writes the one from the other."""
    boards = doc["boards"]
    if isinstance(boards, dict):
        return list(boards.items())
    return [(b["id"], b) for b in boards]


def wap_only(record: dict) -> bool:
    return record.get("devices") == ["canary-wap"]


def board_phantoms(doc: dict, flags: set[str]) -> list[tuple[str, str]]:
    """(path, flag) for each FEATURE_* token a board record names that its
    devices' firmware cannot be shown to have: on a canary-wap-only board, a
    flag the sketch lacks; on any other board, any flag at all."""
    out = []
    for bid, rec in board_records(doc):
        for path, text in strings(rec, bid):
            for tok in FLAG.findall(text):
                if not wap_only(rec) or tok not in flags:
                    out.append((path, tok))
    return out


class WapBomFlags(unittest.TestCase):
    def setUp(self):
        self.flags = sketch_flags(BUILD_CONFIG.read_text(encoding="utf-8"))

    def test_the_sketch_flag_set_was_read(self):
        # A moved or emptied build_config.h would make every BOM token a
        # phantom; a parser that found nothing would make none of them one.
        self.assertGreater(len(self.flags), 10, sorted(self.flags))
        self.assertIn("FEATURE_TAMPER_GPIO", self.flags)

    def test_the_bom_names_no_flag_the_sketch_lacks(self):
        bad = phantom_flags(BOM.read_text(encoding="utf-8"), self.flags)
        self.assertEqual(
            bad, [],
            "docs/hardware/bom_canary_wap.csv names build flags the WAP "
            "sketch's build_config.h does not have: "
            + ", ".join(f"line {n}: {t}" for n, t in bad))

    def test_a_phantom_flag_is_reported(self):
        # The guard's guard: the shape the old M1 note had.
        bom = 'M1,"UART 9600 on D6/D7; FEATURE_GNSS"\nBZ1,"FEATURE_TAMPER_GPIO"\n'
        self.assertEqual(phantom_flags(bom, self.flags), [(1, "FEATURE_GNSS")])


class WapBoardFlags(unittest.TestCase):
    """C20: the board viewer's rows, held to the same flag set."""

    def setUp(self):
        self.flags = sketch_flags(BUILD_CONFIG.read_text(encoding="utf-8"))
        self.docs = {
            "boards/boards.config.json":
                json.loads(BOARDS_CONFIG.read_text(encoding="utf-8")),
            "canary-local/devices/boards.json":
                json.loads(BOARDS_JSON.read_text(encoding="utf-8")),
        }

    def test_the_wap_boards_were_found(self):
        # A renamed device or a reshaped file would leave no WAP-only board
        # and nothing to check; both boards the WAP is built from are named.
        for name, doc in self.docs.items():
            ids = {bid for bid, rec in board_records(doc) if wap_only(rec)}
            with self.subTest(file=name):
                self.assertEqual(
                    ids, {"seeed_xiao_esp32s3_sense", "quectel_l76k_gnss"})

    def test_no_board_row_names_a_flag_its_firmware_lacks(self):
        for name, doc in self.docs.items():
            bad = board_phantoms(doc, self.flags)
            with self.subTest(file=name):
                self.assertEqual(
                    bad, [],
                    f"{name} names build flags the board's firmware cannot be "
                    "shown to have (a canary-wap-only board may name only the "
                    "WAP sketch's build_config.h flags; any other board, none "
                    "until this test reads its device's flags): "
                    + ", ".join(f"{p}: {t}" for p, t in bad))

    def test_a_phantom_board_flag_is_reported(self):
        # The guard's guard: the shape the old GNSS UART row had, a real
        # flag beside it, and a flag on a board of another device.
        doc = {"boards": [
            {"id": "s3", "devices": ["canary-wap"], "pinout": [
                {"label": "GNSS UART", "use": "L76K TX/RX @ 9600 (FEATURE_GNSS)"},
                {"label": "Tamper", "use": "FEATURE_TAMPER_GPIO=1"}]},
            {"id": "disp", "devices": ["canary-display-watch"],
             "blurb": "FEATURE_TAMPER_GPIO"},
        ]}
        self.assertEqual(board_phantoms(doc, self.flags), [
            ("s3.pinout[0].use", "FEATURE_GNSS"),
            ("disp.blurb", "FEATURE_TAMPER_GPIO"),
        ])


if __name__ == "__main__":
    unittest.main()
