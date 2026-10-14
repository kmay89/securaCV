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

Run:  python3 -m unittest discover -s scripts/tests -p 'test_wap_bom_flags.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BOM = REPO / "docs" / "hardware" / "bom_canary_wap.csv"
BUILD_CONFIG = (REPO / "firmware" / "projects" / "canary-wap" / "arduino"
                / "canary_wap" / "build_config.h")

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


if __name__ == "__main__":
    unittest.main()
