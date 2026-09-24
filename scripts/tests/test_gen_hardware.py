#!/usr/bin/env python3
"""Tests for docs/hardware/enclosure/gen_hardware.py — the hardware ledger and its BOM join.

No OpenSCAD here: the echo parser, the join and the DESIGN_RULES table check
are pure Python, fed fixtures (including the doorbell's real screw_insert
echo, whose seven inserts are the drift the gate was built to surface). The
render half runs in enclosure.yml (`gen_hardware.py --check`).

  • parse_line reads every item of a real HARDWARE echo into (qty, kind,
    spec, text), and refuses an item it cannot count rather than dropping it;
  • the join bills each echoed fastener to the row its description names,
    reports a short row and an unbilled fastener, honors a set's join
    filter, and never reads a non-fastener;
  • the committed ledger agrees with itself: its drift is exactly
    KNOWN_DRIFT, its eight rib sets are the "Lid rib proportions" table
    DESIGN_RULES.md prints, and a table that disagrees is caught.

Discovered by lint.yml's `unittest discover -s scripts/tests`.
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[2]
ENC = REPO / "docs" / "hardware" / "enclosure"
sys.path.insert(0, str(ENC))
spec = importlib.util.spec_from_file_location("gen_hardware", ENC / "gen_hardware.py")
gh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gh)  # type: ignore[union-attr]

# The doorbell's own echo with screw_insert = true, verbatim from OpenSCAD.
DOORBELL_INSERTS = (
    '"HARDWARE — Vision doorbell: 6x M2 pan x 10 machine (into the inserts) · '
    '4x M2 pan x 6 self-tap (OV5647 to the face posts) · '
    '1x M2 x 10 security screw, Torx pin/tri-wing, machine thread (into the boss insert) '
    '(plate foot into the body boss) · '
    '7x M2 heat-set insert 3.5 OD x 4 — the +1 seats in the security boss · '
    '1x Ø12 illuminated momentary button + panel nut (16.2 AC) · '
    '1x TPU gasket (print part="gasket") · 1x Ø12 adhesive ePTFE/GORE vent patch · '
    '1x Ø14 x 1 clear disc (neutral-cure silicone) · 2x #6 pan wall screw (plate)"'
)


class ParsesTheEcho(unittest.TestCase):
    def test_the_doorbell_insert_line(self):
        case, items = gh.parse_line(DOORBELL_INSERTS)
        self.assertEqual(case, "Vision doorbell")
        self.assertEqual([i["qty"] for i in items], [6, 4, 1, 7, 1, 1, 1, 1, 2])
        self.assertEqual([i["kind"] for i in items],
                         ["screw", "screw", "security-screw", "insert", "button", "gasket",
                          "vent", "window", "wall-screw"])
        ins = items[3]
        self.assertEqual(ins["spec"], "M2 heat-set insert 3.5 OD x 4")
        self.assertIn("security boss", ins["text"])
        self.assertEqual(items[0]["spec"], "M2 pan x 10 machine (into the inserts)")
        self.assertEqual(items[1]["spec"], "M2 pan x 6 self-tap")
        self.assertEqual(items[2]["spec"], "M2 x 10 security screw")

    def test_every_kind_is_named(self):
        for text, kind in (("M2 flat x 8 self-tap", "screw"),
                           ("M3 heat-set insert 4.5 OD x 5", "insert"),
                           ("O-ring 2 ID x 1 CS (under the head)", "o-ring"),
                           ("M5 x 25 bolt + nut (hinge; knob part=\"knob\")", "bolt"),
                           ("Ø3 light pipe", "light-pipe"),
                           ("Ø6 x 2.2 disc magnet (press + glue)", "magnet"),
                           ("hood (print part=\"hood\"; bond into the front's groove)", "printed-part"),
                           ("SMA bulkhead jack", "other")):
            self.assertEqual(gh.classify(text)[0], kind, text)

    def test_an_item_it_cannot_count_is_refused_not_dropped(self):
        with self.assertRaises(ValueError):
            gh.parse_line('"HARDWARE — WAP: 4x M2 flat x 8 self-tap · some M2 screws"')
        with self.assertRaises(ValueError):
            gh.parse_line('"not a hardware line"')


def _set(sid, device, line):
    _, items = gh.parse_line(line)
    return {sid: {"device": device, "items": items}}


class JoinsTheBom(unittest.TestCase):
    def test_short_row_and_unbilled_fastener(self):
        sets = _set("doorbell+inserts", "canary-vision", DOORBELL_INSERTS)
        boms = {"bom_canary_vision.csv": {"INS1": 5, "SCR5": 8, "SCR8": 1}}
        table, drift = gh.join(sets, boms)
        # the +inserts set joins inserts only: INS1 is the option's billed line
        self.assertEqual(table["doorbell+inserts"], {"INS1": {"bom": 5, "echo": 7}})
        self.assertEqual([(d["set"], d["what"]) for d in drift], [("doorbell+inserts", "short INS1")])
        boms = {"bom_canary_vision.csv": {"INS1": 7}}
        _, drift = gh.join(sets, boms)
        self.assertEqual(drift, [])

    def test_a_row_joins_only_the_head_its_description_names(self):
        line = '"HARDWARE — Sense: 4x M2 pan x 10 self-tap · 1x Ø3 light pipe"'
        sets = _set("sense", "canary-sense", line)
        _, drift = gh.join(sets, {"bom_canary_sense.csv": {"SCR3": 4}})
        self.assertEqual([d["what"] for d in drift], ["unbilled M2 pan x 10 self-tap"])
        # the Sense row names PAN x 16 (driven from the back): only that joins
        line = '"HARDWARE — Sense: 4x M2 pan x 16 self-tap from the back · 1x Ø3 light pipe"'
        table, drift = gh.join(_set("sense", "canary-sense", line),
                               {"bom_canary_sense.csv": {"SCR3": 4}})
        self.assertEqual(drift, [])
        self.assertEqual(table["sense"], {"SCR3": {"bom": 4, "echo": 4}})   # the pipe is not joined

    def test_the_shield_set_joins_only_its_replacement_screws(self):
        line = ('"HARDWARE — WAP: 4x M2 flat x 8 self-tap · '
                '4x M2 flat x 16 self-tap — REPLACES the lid screws when the shield is fitted"')
        table, drift = gh.join(_set("wap.battery_weather.shield", "canary-wap", line),
                               {"bom_canary_wap.csv": {"SCR3": 4}})
        # SCR3 names flat heads 8-16 mm (16 is the battery build's from-the-back
        # length), so it bills the shield's replacement screws too — and only
        # them: the set's own 8 mm lid screws are the ones the shield replaces
        self.assertEqual(table["wap.battery_weather.shield"], {"SCR3": {"bom": 4, "echo": 4}})
        self.assertEqual(drift, [])

    def test_a_missing_row_is_short(self):
        line = '"HARDWARE — WAP: 4x M2 flat x 8 self-tap"'
        _, drift = gh.join(_set("wap.compact_plain", "canary-wap", line), {"bom_canary_wap.csv": {}})
        self.assertEqual([d["what"] for d in drift], ["short SCR3"])


class TheCommittedLedgerAgreesWithItself(unittest.TestCase):
    def setUp(self):
        self.led = json.loads((ENC / "hardware.json").read_text(encoding="utf-8"))

    def test_its_drift_is_exactly_the_known_list(self):
        keys = {f"{d['set']}|{d['what']}" for d in self.led["bom_drift"]}
        self.assertEqual(keys, gh.KNOWN_DRIFT)
        # the drift the gate was built to surface: 7 inserts against INS1 = 5
        self.assertIn("doorbell+inserts|short INS1", keys)
        self.assertEqual(self.led["sets"]["doorbell+inserts"]["bom"]["INS1"], {"bom": 5, "echo": 7})

    def test_the_join_reproduces_from_the_committed_items_and_csvs(self):
        sets = {sid: {"device": r["device"], "items": r["items"]} for sid, r in self.led["sets"].items()}
        table, drift = gh.join(sets)
        self.assertEqual(drift, self.led["bom_drift"])
        for sid, rows in table.items():
            self.assertEqual(rows, self.led["sets"][sid]["bom"], sid)

    def test_the_eight_rib_sets_are_the_design_rules_table(self):
        ribs = sorted(sid for sid, r in self.led["sets"].items() if "lid_rib" in r)
        self.assertEqual(len(ribs), 8)
        self.assertEqual(gh.check_design_rules(self.led["sets"]), [])

    def test_a_table_that_disagrees_is_caught(self):
        text = (ENC / "DESIGN_RULES.md").read_text(encoding="utf-8")
        wrong = text.replace("| `vision.xiao_weather` | 1.00 | 4.58 |", "| `vision.xiao_weather` | 1.00 | 4.00 |")
        self.assertNotEqual(wrong, text)
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "DESIGN_RULES.md"
            p.write_text(wrong, encoding="utf-8")
            with mock.patch.object(gh, "DESIGN_RULES", p):
                bad = gh.check_design_rules(self.led["sets"])
        self.assertEqual(len(bad), 1)
        self.assertIn("vision.xiao_weather", bad[0])
        self.assertNotIn("§", bad[0])  # cited by title: a renumbered section cannot retarget it


class TheStaleMessageNamesTheSourceThatMoved(unittest.TestCase):
    """A BOM CSV edit must not be reported as the CAD's hardware moving."""

    def setUp(self):
        self.led = json.loads((ENC / "hardware.json").read_text(encoding="utf-8"))

    def _moved(self, edit) -> str:
        fresh = json.loads(json.dumps(self.led))
        edit(fresh)
        return gh.stale_message(self.led, fresh)

    def test_a_bom_quantity_is_named_as_the_bom(self):
        def edit(f):
            f["sets"]["wap.compact_plain+inserts"]["bom"]["INS1"]["bom"] = 3
            f["bom_drift"].append({"set": "wap.compact_plain+inserts", "csv": "bom_canary_wap.csv",
                                   "what": "short INS1", "detail": "x"})
        msg = self._moved(edit)
        self.assertIn("a BOM quantity or row moved (wap.compact_plain+inserts)", msg)
        self.assertNotIn("CAD", msg)

    def test_an_echoed_count_is_named_as_the_cad(self):
        def edit(f):
            f["sets"]["wap.compact_plain+inserts"]["items"][1]["qty"] = 5
        msg = self._moved(edit)
        self.assertIn("the CAD's hardware moved (wap.compact_plain+inserts)", msg)
        self.assertNotIn("BOM", msg)


if __name__ == "__main__":
    unittest.main()
