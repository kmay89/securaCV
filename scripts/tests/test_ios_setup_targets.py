#!/usr/bin/env python3
"""Every Set up destination a link can name is one the app can open.

`securacv://setup?what=<target>` (ios/Shared/AppRoute.swift) lands on a
`SetupTarget` (ios/Sources/SecuraCV/Views/SetupView.swift), and SetupView's
`navigationDestination` switch opens it. Three files, one list, no Swift
toolchain on the Linux runners, so this reads the sources and holds them to
each other:

  * every word the route parser accepts is a `SetupTarget` case;
  * every `SetupTarget` case has a destination in SetupView;
  * the display target opens the display walkthrough itself, and Today's
    first-run card offers it (a display never answers the Bluetooth listen
    that card starts, so its owner must not have to dig for the way in).

It says what the source says, not that it compiles; `ios-selfheal.yml`
compiles it and AppRouteTests runs the round trips.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ROUTE = ROOT / "ios" / "Shared" / "AppRoute.swift"
SETUP = ROOT / "ios" / "Sources" / "SecuraCV" / "Views" / "SetupView.swift"
TODAY = ROOT / "ios" / "Sources" / "SecuraCV" / "Views" / "TodayView.swift"


class SetupTargetsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.route = ROUTE.read_text(encoding="utf-8")
        cls.setup = SETUP.read_text(encoding="utf-8")
        cls.today = TODAY.read_text(encoding="utf-8")
        m = re.search(r"enum SetupTarget: String[^{]*\{(.*?)\n\}", cls.setup, re.S)
        assert m, "SetupView.swift declares SetupTarget"
        cases = re.search(r"^\s*case ([a-z, ]+)$", m.group(1), re.M)
        assert cases, "SetupTarget lists its cases on one line"
        cls.targets = {c.strip() for c in cases.group(1).split(",")}

    def test_every_routed_word_is_a_setup_target(self) -> None:
        m = re.search(r"self = \.setup\(what: \((.*?)\) \? what : nil\)", self.route)
        self.assertIsNotNone(m, "AppRoute's setup parser names its accepted words")
        words = set(re.findall(r'what == "([a-z]+)"', m.group(1)))
        self.assertTrue(words, "it accepts at least one word")
        self.assertEqual(words, self.targets, "the link and the targets name the same walkthroughs")

    def test_every_target_has_a_destination(self) -> None:
        dest = re.search(r"\.navigationDestination\(for: SetupTarget\.self\) \{ target in(.*?)\n            \}",
                         self.setup, re.S)
        self.assertIsNotNone(dest)
        for t in self.targets:
            self.assertIn(f"case .{t}:", dest.group(1), f"{t} opens something")
        self.assertIn("case .display: CanaryFamilySetupView(family: .display)", dest.group(1))

    def test_todays_first_run_card_offers_the_display_path(self) -> None:
        self.assertGreaterEqual(self.today.count('open(.display)'), 2,
                                "both the ask and the listening card offer it")


if __name__ == "__main__":
    unittest.main()
