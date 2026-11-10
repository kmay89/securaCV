#!/usr/bin/env python3
"""The iPhone glue that turns two tested rules on (sweeps A88, A75, A92), held as source.

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
  * the stand-down is per window, never per app: `RootView` (one per
    scene) owns it as `@State`, hands the value to its `NearbyOfferOverlay`
    and the binding to its environment, and `FleetStore` (one per app)
    holds none. An app-wide flag let a "+" sheet in one iPad window hide
    the card in every window, and let closing one of two "+" sheets bring
    the card back behind the other;
  * `AddCanarySheet` (the Fleet tab's "+" sheet, `.medium` detent) must
    raise its window's stand-down on appear and clear it on disappear,
    through that environment binding, from its root (a modifier on a row
    inside its NavigationStack would clear it on a push while the sheet is
    still up), and nothing else may write it;
  * `NearbyOfferOverlay` must hand its `standingDown` to every `visible`
    and to its `latch`, and latch again when it changes (a card held
    behind the sheet comes back when the sheet closes).

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
STATE = "nearbyOfferStandsDown"   # RootView's @State, one per window
KEY = "nearbyOfferStandDown"      # the environment key that carries its binding


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


class TheStandDownIsPerWindow(unittest.TestCase):
    """A92: each scene's RootView owns the stand-down; the app's one store holds none."""

    def setUp(self):
        text = masked("ios/Sources/SecuraCV/App/SecuraCVApp.swift")
        root = struct_span(text, "RootView")
        self.root = flat(text[root.open:root.close + 1])

    def test_the_window_owns_it_as_state(self):
        self.assertIn(f"@State private var {STATE} = false", self.root)

    def test_the_window_hands_it_to_its_own_overlay_and_environment(self):
        self.assertIn(f"NearbyOfferOverlay(standingDown: {STATE})", self.root)
        self.assertIn(f".environment(\\.{KEY}, ${STATE})", self.root)

    def test_the_apps_one_store_holds_no_stand_down(self):
        store = masked("ios/Sources/SecuraCV/App/FleetStore.swift")
        self.assertNotRegex(store, r"(?i)stand_?s?_?down",
                            "FleetStore is one object for every window: a stand-down there is app-wide")

    def test_only_the_window_provides_the_binding(self):
        providers = []
        for root in ("ios/Sources", "ios/Shared"):
            for p in sorted((ROOT / root).rglob("*.swift")):
                text = L.mask(p.read_text(encoding="utf-8"))
                if re.search(r"\.environment\(\s*\\\.%s\b" % KEY, text):
                    providers.append(p.relative_to(ROOT).as_posix())
        self.assertEqual(providers, ["ios/Sources/SecuraCV/App/SecuraCVApp.swift"])


class ThePlusSheetStandsTheCardDown(unittest.TestCase):
    """A75 (2): the "+" sheet raises its window's stand-down from its root; nothing else writes it."""

    def setUp(self):
        self.text = masked("ios/Sources/SecuraCV/Views/FleetHiveView.swift")
        self.sheet = struct_span(self.text, "AddCanarySheet")
        self.src = self.text[self.sheet.open:self.sheet.close + 1]

    def _one(self, pattern: str) -> int:
        hits = [m.start() + self.sheet.open for m in re.finditer(pattern, self.src)]
        self.assertEqual(len(hits), 1, f"expected exactly one {pattern!r} in AddCanarySheet, found {len(hits)}")
        return hits[0]

    def test_it_reads_its_windows_binding_from_the_environment(self):
        self.assertIn(f"@Environment(\\.{KEY}) private var standDown", flat(self.src))

    def test_it_raises_the_flag_on_appear_and_clears_it_on_disappear(self):
        up = self._one(r"\.onAppear\s*\{\s*standDown\s*\.\s*wrappedValue\s*=\s*true\s*\}")
        down = self._one(r"\.onDisappear\s*\{\s*standDown\s*\.\s*wrappedValue\s*=\s*false\s*\}")
        for name, at in (("onAppear", up), ("onDisappear", down)):
            self.assertRegex(flat(innermost_header(self.text, self.sheet.open, at)), r"\bvar body: some View$",
                             f"the {name} must sit on the sheet's root, not inside its NavigationStack")

    def test_it_is_the_sheet_with_the_medium_detent(self):
        self.assertRegex(self.src, r"\.presentationDetents\(\s*\[\s*\.medium\b")

    def test_nothing_else_writes_the_flag(self):
        # The window's state is written only through the binding, and the
        # binding only by the sheet: its two writes, and no assignment to the
        # state itself beyond its declaration.
        binding_writes, state_writes = [], []
        for root in ("ios/Sources", "ios/Shared"):
            for p in sorted((ROOT / root).rglob("*.swift")):
                rel = p.relative_to(ROOT).as_posix()
                text = L.mask(p.read_text(encoding="utf-8"))
                for m in re.finditer(r"\bstandDown\s*\.\s*wrappedValue\s*=(?!=)", text):
                    binding_writes.append((rel, text.count("\n", 0, m.start()) + 1))
                for m in re.finditer(r"\b%s\s*=(?!=)" % STATE, text):
                    state_writes.append((rel, text.count("\n", 0, m.start()) + 1))
                self.assertNotRegex(text, r"\b%s\s*\.\s*wrappedValue\s*=(?!=)" % KEY,
                                    f"{rel}: the binding is written through the sheet's property only")
        self.assertEqual(sorted({w[0] for w in binding_writes}),
                         ["ios/Sources/SecuraCV/Views/FleetHiveView.swift"], binding_writes)
        self.assertEqual(len(binding_writes), 2, binding_writes)
        self.assertEqual([w[0] for w in state_writes], ["ios/Sources/SecuraCV/App/SecuraCVApp.swift"],
                         f"RootView only declares it: {state_writes}")


class TheOverlayReadsTheFlag(unittest.TestCase):
    """A75 (2): every draw and the latch read the window's stand-down; a change re-latches."""

    def setUp(self):
        text = masked("ios/Sources/SecuraCV/Views/Components/NearbyCanaryCard.swift")
        ov = struct_span(text, "NearbyOfferOverlay")
        self.src = flat(text[ov.open:ov.close + 1])

    def test_it_is_handed_the_value_not_a_store(self):
        self.assertIn("let standingDown: Bool", self.src)

    def test_every_draw_stands_down_with_the_flag(self):
        calls = re.findall(r"offer\.visible\([^)]*\)", self.src)
        self.assertGreaterEqual(len(calls), 1)
        self.assertEqual(calls, ["offer.visible(standingDown: standingDown)"] * len(calls))
        self.assertIn("if let c = offer.visible(standingDown: standingDown)", self.src,
                      "the card is drawn from the latch, through the flag")

    def test_the_latch_reads_the_flag_and_the_open_setup_sheet(self):
        calls = re.findall(r"offer\.latch\([^)]*\)", self.src)
        self.assertEqual(calls, ["offer.latch(store.nearbyOffer, setupOpen: target != nil, standingDown: standingDown)"])

    def test_a_change_of_the_flag_latches_again(self):
        self.assertIn(".onChange(of: standingDown) { _, _ in latchOffer() }", self.src,
                      "a card held behind the sheet must come back when it closes")


class TheSetupNetworkIsLetGoByTheClientNeverByIOS(unittest.TestCase):
    """The display's path and the no-Bluetooth fallback join SecuraCV-… from
    inside the app (SetupPortalClient). The client decides when the phone
    leaves that network, not `joinOnce`: a joinOnce network is dropped when
    the app leaves the foreground, and the one thing a person does in the
    middle of this step is leave for the Wi-Fi password. The pure halves
    (SetupPortal.HotspotJoinOutcome) have XCTests; this pins the glue."""

    def setUp(self):
        self.text = masked("ios/Sources/SecuraCV/Transport/SetupPortalClient.swift")
        spans = [s for s in L.type_spans(self.text) if s.kind == "class" and s.name == "SetupPortalClient"]
        self.assertEqual(len(spans), 1, "one SetupPortalClient")
        self.span = spans[0]

    def _body(self, name: str) -> str:
        return flat(func_body(self.text, self.span.open, self.span.close, name))

    def test_the_join_is_not_join_once(self):
        self.assertIn("config.joinOnce = false", self._body("connect"))
        self.assertNotIn("joinOnce = true", flat(self.text))

    def test_the_client_forgets_the_network_it_joined(self):
        finish = self._body("finish")
        self.assertIn("NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: ssid)", finish)
        connect = self._body("connect")
        # No page answered: the phone leaves the network before it says so.
        waited = connect.find("waitForPortal(")
        self.assertGreater(waited, 0)
        left = connect.find("finish()", waited)
        failed = connect.find("phase = .failed(", waited)
        self.assertGreater(left, waited, "a join with no page behind it is left")
        self.assertLess(left, failed, "left before the failure is shown")

    def test_iOSs_own_answer_is_folded_not_relayed_raw(self):
        connect = self._body("connect")
        self.assertIn("Self.joinOutcome(error)", connect)
        self.assertIn("outcome.advice(ssid: setupSSID)", connect)
        self.assertNotIn("error.localizedDescription", connect,
                         "the one place iOS's words are relayed is HotspotJoinOutcome.other")

    def test_a_join_finished_elsewhere_is_noticed_while_the_person_decides(self):
        connect = self._body("connect")
        self.assertIn("guard gen == generation else { return } networks = listed phase = .ready startReadyWatch()",
                      connect, "the watch starts the moment the list is up — and only for a live connect")
        for name in ("hand", "finish"):
            self.assertIn("stopReadyWatch()", self._body(name), f"{name} ends the watch")
        watch = self._body("watchWhileReady")
        self.assertIn("case .success?: phase = .done finish() return", watch)
        hand = self._body("hand")
        self.assertIn("handedSSID = homeSSID", hand, "only a network this phone posted is named")
        self.assertNotIn("handedSSID = homeSSID", watch)

    def test_the_watch_beats_the_firmwares_hold_and_names_a_vanished_portal(self):
        # onboarding_fsm.h takes the setup network down ap_hold_after_ack_ms
        # (1.6 s) after the FIRST client reads success — the page in iOS's
        # pop-up counts — so the watch polls every second and treats a portal
        # that stops answering as the end of the step, said in words.
        watch = self._body("watchWhileReady")
        self.assertIn("Task.sleep(for: .seconds(1))", watch)
        self.assertNotIn(".seconds(3)", watch)
        self.assertIn("case nil: silentPolls += 1 if silentPolls >= Self.silentPollsBeforeGone { finish() "
                      "phase = .failed(SetupPortal.portalGoneLine) return }", watch)
        self.assertIn("default: silentPolls = 0", watch, "one dropped answer is not the end")
        self.assertRegex(flat(self.text), r"static let silentPollsBeforeGone = [2-5]\b")

    def test_a_closed_screen_ends_every_step_in_flight(self):
        # connect() and hand() are unstructured work the view cannot cancel;
        # finish() moves the generation and each stops at its next await.
        finish = self._body("finish")
        self.assertTrue(finish.startswith("{ generation += 1 stopReadyWatch()"), finish[:60])
        connect = self._body("connect")
        self.assertIn("generation += 1 let gen = generation", connect)
        self.assertGreaterEqual(connect.count("guard gen == generation else {"), 3,
                                "after the iOS join, after the page answered, after the list")
        # A join iOS finished after the screen closed is let go, not kept.
        self.assertIn("guard gen == generation else { if outcome.isOn { "
                      "NEHotspotConfigurationManager.shared.removeConfiguration(forSSID: setupSSID) } return }", connect)
        self.assertIn("waitForPortal(seconds: 25, gen: gen)", connect)
        self.assertIn("scan(seconds: 10, gen: gen)", connect)
        hand = self._body("hand")
        self.assertIn("let gen = generation", hand)
        self.assertGreaterEqual(hand.count("guard gen == generation else { return }"), 3,
                                "after the post, after every status read, before the timeout verdict")
        for name in ("scan", "waitForPortal"):
            self.assertIn("while Date() < deadline, gen == generation {", self._body(name), name)

    def test_launch_forgets_what_an_earlier_run_left(self):
        sweep = self._body("forgetLingeringSetupNetworks")
        self.assertIn("getConfiguredSSIDs", sweep)
        self.assertIn("where SetupPortal.isSetupNetwork(ssid)", sweep, "only the setup-network shapes")
        self.assertIn("removeConfiguration(forSSID: ssid)", sweep)
        store = masked("ios/Sources/SecuraCV/App/FleetStore.swift")
        spans = [s for s in L.type_spans(store) if s.kind == "class" and s.name == "FleetStore"]
        self.assertEqual(len(spans), 1)
        on_appear = flat(func_body(store, spans[0].open, spans[0].close, "onAppear"))
        self.assertIn("SetupPortalClient.forgetLingeringSetupNetworks()", on_appear)

    def test_the_view_remembers_and_names_only_what_this_phone_handed_over(self):
        view = flat(masked("ios/Sources/SecuraCV/Views/CanarySetupView.swift"))
        self.assertIn("if let ssid = portal.handedSSID, HouseholdWiFiStore.shouldRemember(", view)
        self.assertNotIn("let ssid = effectiveSSID", view)
        self.assertIn("portal.handedSSID.map {", view)
        self.assertIn("?? SetupPortal.joinedElsewhereLine", view)
        self.assertIn("Text(SetupPortal.captivePopupNote)", view)


if __name__ == "__main__":
    unittest.main()
