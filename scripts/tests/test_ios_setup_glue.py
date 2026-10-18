#!/usr/bin/env python3
"""The iPhone glue that turns two tested rules on (sweeps A88, A75), held as source.

Both rules are pure values with XCTest cases in CI's simulator job:
`ProvisioningReceipt.rebased(onto:discoveredAs:)` (DeviceAPITests) decides
which recovery kits move onto the host a Ready-to-pair row was discovered
at, and `NearbyOfferLatch` (NearbyCanaryTests) decides when the card that
slides up by itself draws and latches. Neither test sees the views that
feed them, and the behavior lives in those few lines:

  * `PairView.pair(from:)` must pass the ROW's id and host — the kit's own
    id would make every kit "this device", so WAP A's kit opened from WAP
    B's row would be moved onto B with A's token — and must pair only the
    receipt that came back, behind the `isPrivate` gate and the https pin
    check;
  * `AddCanarySheet` (the Fleet tab's "+" sheet, `.medium` detent) must
    raise `FleetStore.nearbyOfferStandsDown` on appear and clear it on
    disappear, from its root (a modifier on a row inside its
    NavigationStack would clear it on a push while the sheet is still up),
    and nothing else may write it;
  * `NearbyOfferOverlay` must hand that flag to every `visible` and to its
    `latch`, and latch again when it changes (a card held behind the sheet
    comes back when the sheet closes).

There is no Swift toolchain on the Linux CI runners, and SwiftUI glue has no
unit seam in this project, so this is a source pin: the files are masked by
`scripts/lint_ios_keychain_reads.py`'s own `mask()` (comments and string text
blanked), so a mention in a comment never satisfies it. It says what the code
says, not that it compiles; `ios-selfheal.yml` compiles it.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("lint_ios_keychain_reads",
                                               ROOT / "scripts" / "lint_ios_keychain_reads.py")
L = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = L  # its dataclasses look their module up by name
_spec.loader.exec_module(L)

VIEWS = ROOT / "ios" / "Sources" / "SecuraCV" / "Views"
FLAG = "nearbyOfferStandsDown"


def masked(rel: str) -> str:
    return L.mask((ROOT / rel).read_text(encoding="utf-8"))


def flat(s: str) -> str:
    """Whitespace collapsed, so a call wrapped over lines reads as one."""
    return re.sub(r"\s+", " ", s).replace("( ", "(").replace(" )", ")")


def struct_span(text: str, name: str):
    spans = [s for s in L.type_spans(text) if s.kind == "struct" and s.name == name]
    if len(spans) != 1:
        raise AssertionError(f"expected one struct {name}, found {len(spans)}")
    return spans[0]


def func_body(text: str, start: int, end: int, name: str) -> str:
    m = re.compile(r"\bfunc\s+%s\b" % re.escape(name)).search(text, start, end)
    if m is None:
        raise AssertionError(f"no func {name}")
    o = text.find("{", m.end())
    return text[o:L.match_brace(text, o) + 1]


def innermost_header(text: str, open_at: int, at: int) -> str:
    """The header of the innermost brace still open at `at`, scanning from
    the brace at `open_at` (a type's)."""
    stack: list[int] = []
    for k in range(open_at + 1, at):
        if text[k] == "{":
            stack.append(k)
        elif text[k] == "}" and stack:
            stack.pop()
    return L.header_of(text, stack[-1]) if stack else ""


class PairViewRebasesUnderTheRowsId(unittest.TestCase):
    """A88: the kit is moved under the row's id and host, then gated."""

    def setUp(self):
        text = masked("ios/Sources/SecuraCV/Views/PairView.swift")
        view = struct_span(text, "PairView")
        self.body = flat(func_body(text, view.open, view.close, "pair"))

    def test_the_kit_is_rebased_onto_the_rows_host_under_the_rows_id(self):
        want = ("let receipt = decoded.rebased(onto: canary.host.flatMap(DeviceAPI.url(forDiscoveredHost:)), "
                "discoveredAs: canary.deviceID)")
        self.assertIn(want, self.body,
                      "pair(from:) must rebase onto the row's discovered host under the row's device id "
                      "(the kit's own id makes every kit 'this device')")

    def test_only_the_rebased_receipt_is_paired(self):
        uses = len(re.findall(r"\bdecoded\b", self.body))
        self.assertEqual(uses, 2, "the kit as decoded is bound and rebased, and read nowhere else")
        rebase = self.body.find("decoded.rebased(")
        private = self.body.find("guard DeviceAPI.isPrivate(receipt.baseURL)")
        pin = self.body.find("DeviceAPI.isTLS(receipt.baseURL) && receipt.tlsCertFingerprint == nil")
        add = self.body.find("store.devices.add(ref, token: receipt.token)")
        for name, at in (("the isPrivate gate", private), ("the https pin check", pin), ("the add", add)):
            self.assertGreater(at, rebase, f"{name} must follow the rebase and read the rebased receipt")
        self.assertIn("baseURL: receipt.baseURL", self.body)
        self.assertIn("tlsCertFingerprint: receipt.tlsCertFingerprint", self.body)


class ThePlusSheetStandsTheCardDown(unittest.TestCase):
    """A75 (2): the "+" sheet raises the flag from its root; nothing else writes it."""

    def setUp(self):
        self.text = masked("ios/Sources/SecuraCV/Views/FleetHiveView.swift")
        self.sheet = struct_span(self.text, "AddCanarySheet")
        self.src = self.text[self.sheet.open:self.sheet.close + 1]

    def _one(self, pattern: str) -> int:
        hits = [m.start() + self.sheet.open for m in re.finditer(pattern, self.src)]
        self.assertEqual(len(hits), 1, f"expected exactly one {pattern!r} in AddCanarySheet, found {len(hits)}")
        return hits[0]

    def test_it_raises_the_flag_on_appear_and_clears_it_on_disappear(self):
        up = self._one(r"\.onAppear\s*\{\s*store\s*\.\s*%s\s*=\s*true\s*\}" % FLAG)
        down = self._one(r"\.onDisappear\s*\{\s*store\s*\.\s*%s\s*=\s*false\s*\}" % FLAG)
        for name, at in (("onAppear", up), ("onDisappear", down)):
            self.assertRegex(flat(innermost_header(self.text, self.sheet.open, at)), r"\bvar body: some View$",
                             f"the {name} must sit on the sheet's root, not inside its NavigationStack")

    def test_it_is_the_sheet_with_the_medium_detent(self):
        self.assertRegex(self.src, r"\.presentationDetents\(\s*\[\s*\.medium\b")

    def test_nothing_else_writes_the_flag(self):
        writers = []
        for root in ("ios/Sources", "ios/Shared"):
            for p in sorted((ROOT / root).rglob("*.swift")):
                rel = p.relative_to(ROOT).as_posix()
                text = L.mask(p.read_text(encoding="utf-8"))
                for m in re.finditer(r"\b%s\s*=(?!=)" % FLAG, text):
                    writers.append((rel, text.count("\n", 0, m.start()) + 1, flat(text[m.start():m.start() + 40])))
        where = sorted({w[0] for w in writers})
        self.assertEqual(where, ["ios/Sources/SecuraCV/App/FleetStore.swift",
                                 "ios/Sources/SecuraCV/Views/FleetHiveView.swift"], writers)
        decl = [w for w in writers if w[0].endswith("FleetStore.swift")]
        self.assertEqual(len(decl), 1, f"FleetStore only declares it: {decl}")
        self.assertEqual(len(writers), 3, writers)


class TheOverlayReadsTheFlag(unittest.TestCase):
    """A75 (2): every draw and the latch read the flag; a change re-latches."""

    def setUp(self):
        text = masked("ios/Sources/SecuraCV/Views/Components/NearbyCanaryCard.swift")
        ov = struct_span(text, "NearbyOfferOverlay")
        self.src = flat(text[ov.open:ov.close + 1])

    def test_every_draw_stands_down_with_the_flag(self):
        calls = re.findall(r"offer\.visible\([^)]*\)", self.src)
        self.assertGreaterEqual(len(calls), 1)
        self.assertEqual(calls, [f"offer.visible(standingDown: store.{FLAG})"] * len(calls))
        self.assertIn(f"if let c = offer.visible(standingDown: store.{FLAG})", self.src,
                      "the card is drawn from the latch, through the flag")

    def test_the_latch_reads_the_flag_and_the_open_setup_sheet(self):
        calls = re.findall(r"offer\.latch\([^)]*\)", self.src)
        self.assertEqual(calls, [f"offer.latch(store.nearbyOffer, setupOpen: target != nil, standingDown: store.{FLAG})"])

    def test_a_change_of_the_flag_latches_again(self):
        self.assertIn(f".onChange(of: store.{FLAG}) {{ _, _ in latchOffer() }}", self.src,
                      "a card held behind the sheet must come back when it closes")


if __name__ == "__main__":
    unittest.main()
