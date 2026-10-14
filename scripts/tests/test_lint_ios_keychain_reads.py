#!/usr/bin/env python3
"""Tests for scripts/lint_ios_keychain_reads.py — the guard needs a guard.

The lint decides whether a Keychain read in a SwiftUI view is on the render
path by masking comments and strings and walking braces with regexes. That is
a heuristic, and a heuristic nobody tests reads as covered while catching
nothing, so the verdicts listed here are pinned on small Swift fixtures (a
verdict not listed is not pinned):

  * refused: a `@State` (or any stored) initial value — the A75 shape
    `SetupView` had, also split over two lines — `body`, a `some View`
    property or function, a container's `footer:` or a `Button`'s `label:`
    closure, the label of `Button(action:)`, a view's `init`, a view
    modifier's content (`.overlay`, `.sheet`, `.toolbar` and its items,
    `.background`, `.safeAreaInset`, `.contextMenu`), a `Task` made in
    `body`, and a view whose `View` conformance is not its first;
  * allowed: `.task`, `.onAppear` (closure or `perform:`), `.onDisappear`,
    `.onChange`, a `Button`'s action (trailing or `action:`),
    `.fileImporter`, a `Task` made inside any of those or inside a `func`,
    any `func` that does not return a view, and every read outside a
    `View` struct (a store, a type nested in a view);
  * masking: a brace or a reader name inside a string or a comment is not
    code, while a call inside a string interpolation is.

`header_of` stopping at `;` is not pinned: no declaration Swift accepts in
a view can sit before a `;` and change the verdict of the brace after it.

The readers are derived, so the derivation is pinned too: a primitive read
(`Keychain.get` or a bare `SecItemCopyMatching`), a reader that calls a
reader through `Self.` or by its bare name, and a protocol requirement with
no body that must not borrow the next declaration's braces. `main()` fails
on a finding and on a derivation that found nothing past `Keychain.get`.
Then the real tree: the lint reads every Swift directory the app target
compiles (ios/project.yml), finds nothing, and derives the readers the setup
screens use.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("lint_ios_keychain_reads",
                                               ROOT / "scripts" / "lint_ios_keychain_reads.py")
L = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = L  # its dataclasses look their module up by name
_spec.loader.exec_module(L)

STORE = '''
enum Keychain {
    static func get(account: String, service: String) -> Data? {
        var out: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &out) == errSecSuccess else { return nil }
        return out as? Data
    }
}
enum WiFiStore {
    static func load() -> WiFi? {
        guard let data = Keychain.get(account: "a", service: "s") else { return nil }
        return try? JSONDecoder().decode(WiFi.self, from: data)
    }
    static func forget() { Keychain.delete(account: "a", service: "s") }
}
enum LoginStore {
    static func login() -> Login? { Self.raw().flatMap(Login.init) }
    private static func raw() -> Data? { Keychain.get(account: "u", service: "h") }
}
'''


def findings(view_src: str) -> list:
    files = {"Store.swift": L.mask(STORE), "View.swift": L.mask(view_src)}
    readers = L.derive_readers(files)
    return L.check_text("View.swift", files["View.swift"], readers)


def lines(view_src: str) -> list[int]:
    return [f.line for f in findings(view_src)]


class Derivation(unittest.TestCase):
    def test_primitive_and_transitive_readers(self):
        readers = L.derive_readers({"Store.swift": L.mask(STORE)})
        self.assertIn(("Keychain", "get"), readers)
        self.assertIn(("WiFiStore", "load"), readers, "a static func that calls Keychain.get")
        self.assertIn(("LoginStore", "raw"), readers)
        self.assertIn(("LoginStore", "login"), readers, "a reader through Self. is a reader")
        self.assertNotIn(("WiFiStore", "forget"), readers, "a delete is not a read")

    def test_a_reader_called_by_its_bare_name_makes_a_reader(self):
        src = '''
enum HubLogin {
    static func login() -> Login? { raw().flatMap(Login.init) }
    private static func raw() -> Data? { Keychain.get(account: "u", service: "h") }
}
'''
        readers = L.derive_readers({"S.swift": L.mask(src)})
        self.assertIn(("HubLogin", "raw"), readers)
        self.assertIn(("HubLogin", "login"), readers, "a reader through a bare call is a reader")

    def test_a_bare_sec_item_copy_matching_is_a_read(self):
        src = '''
enum RawSlots {
    static func read(_ q: [String: Any]) -> Data? {
        var out: CFTypeRef?
        return SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess ? out as? Data : nil
    }
}
'''
        readers = L.derive_readers({"S.swift": L.mask(src)})
        self.assertIn(("RawSlots", "read"), readers)

    def test_a_requirement_with_no_body_borrows_nothing(self):
        src = '''
protocol Slots {
    static func make() -> Self
}
enum Other {
    static func read() -> Data? { Keychain.get(account: "a", service: "s") }
}
'''
        readers = L.derive_readers({"S.swift": L.mask(src)})
        self.assertNotIn(("Slots", "make"), readers)
        self.assertIn(("Other", "read"), readers)

    def test_a_nested_requirement_borrows_nothing_either(self):
        # A protocol nested in a type is not a type span, so its requirement
        # is owned by the enum; only the no-body guard keeps it from taking
        # `read()`'s braces (and its Keychain read) as its own.
        src = '''
enum Slots {
    protocol Maker {
        static func make() -> Self
    }
    static func read() -> Data? { Keychain.get(account: "a", service: "s") }
}
'''
        readers = L.derive_readers({"S.swift": L.mask(src)})
        self.assertNotIn(("Slots", "make"), readers)
        self.assertIn(("Slots", "read"), readers)


class RenderPath(unittest.TestCase):
    def test_the_shape_setup_view_had_before_a75_is_refused(self):
        src = '''
struct SetupView: View {
    @State private var hubRecord = HubStore.load()
    @State private var householdWiFi = WiFiStore.load()
    var body: some View {
        List {
            if let login = LoginStore.login() {
                Text(login.name)
            }
        }
        .onAppear {
            householdWiFi = WiFiStore.load()
        }
    }
}
'''
        got = findings(src)
        self.assertEqual([f.line for f in got], [4, 7])
        self.assertIn("initial value", got[0].reason)
        self.assertIn("render path", got[1].reason)

    def test_the_same_reads_moved_to_on_appear_pass(self):
        src = '''
struct SetupView: View {
    @State private var householdWiFi: WiFi?
    @State private var login: Login?
    var body: some View {
        List {
            if let login { Text(login.name) }
        }
        .onAppear {
            householdWiFi = WiFiStore.load()
            login = LoginStore.login()
        }
    }
}
'''
        self.assertEqual(lines(src), [])

    def test_every_render_place_is_refused(self):
        src = '''
struct Sheet: View {
    let stored = WiFiStore.load()
    init() { _ = LoginStore.login() }
    var body: some View {
        Section {
            Text("x")
        } footer: {
            if let l = LoginStore.login() { Text(l.name) }
        }
        Button {
            go()
        } label: {
            Text(WiFiStore.load()?.ssid ?? "")
        }
        Button(action: go) {
            Text(WiFiStore.load()?.ssid ?? "")
        }
    }
    @ViewBuilder private var appear: some View {
        if let l = LoginStore.login() { Text(l.name) }
    }
    private var caption: String { WiFiStore.load()?.ssid ?? "" }
    private func row(_ s: String) -> some View {
        Text(LoginStore.login()?.name ?? s)
    }
    private func go() {}
}
'''
        self.assertEqual(lines(src), [3, 4, 9, 14, 17, 21, 23, 25])

    def test_view_modifier_content_is_render_path(self):
        src = '''
struct Sheet: View {
    @State private var on = false
    var body: some View {
        List { Text("x") }
            .overlay { Text(WiFiStore.load()?.ssid ?? "") }
            .sheet(isPresented: $on) { Text(WiFiStore.load()?.ssid ?? "") }
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Text(LoginStore.login()?.name ?? "") }
            }
            .background { Text(WiFiStore.load()?.ssid ?? "") }
            .safeAreaInset(edge: .bottom) { Text(WiFiStore.load()?.ssid ?? "") }
            .contextMenu { Text(LoginStore.login()?.name ?? "") }
    }
}
'''
        self.assertEqual(lines(src), [6, 7, 9, 11, 12, 13])

    def test_a_task_made_in_body_is_made_on_every_render(self):
        src = '''
struct Sheet: View {
    @State private var w: WiFi?
    var body: some View {
        let _ = Task { w = WiFiStore.load() }
        Text(w?.ssid ?? "")
    }
}
'''
        got = findings(src)
        self.assertEqual([f.line for f in got], [5])
        self.assertIn("render path", got[0].reason)

    def test_a_read_split_over_lines_and_a_later_view_conformance_are_seen(self):
        src = '''
struct Row: Identifiable, View {
    let id = 1
    @State private var w = WiFiStore
        .load()
    var body: some View { Text(w?.ssid ?? "") }
}
'''
        self.assertEqual(lines(src), [4])

    def test_every_effect_place_is_allowed(self):
        src = '''
struct Sheet: View {
    @State private var w: WiFi?
    var body: some View {
        List {
            Button("Forget", role: .destructive) { w = WiFiStore.load() }
            Button { w = WiFiStore.load() } label: { Text("x") }
            Button(action: { w = WiFiStore.load() }) { Text("y") }
        }
        .task { await begin() }
        .task(id: w?.ssid) { w = WiFiStore.load() }
        .onAppear(perform: { w = WiFiStore.load() })
        .onAppear {
            Task { w = WiFiStore.load() }
        }
        .onDisappear { w = WiFiStore.load() }
        .onChange(of: w?.ssid) { _, now in
            if now == nil { w = WiFiStore.load() }
        }
        .fileImporter(isPresented: .constant(false), allowedContentTypes: [.json]) { result in
            switch result {
            case .success: w = WiFiStore.load()
            case .failure: break
            }
        }
    }
    private func begin() async {
        w = WiFiStore.load()
        Task { @MainActor in
            _ = LoginStore.login()
        }
    }
    private func prefill(
        typed: String,
        remembered: WiFi?
    ) {
        if typed.isEmpty { w = WiFiStore.load() }
    }
}
'''
        self.assertEqual(lines(src), [])

    def test_only_view_structs_are_checked(self):
        src = '''
final class Store: ObservableObject {
    var cached = WiFiStore.load()
    var body: String { LoginStore.login()?.name ?? "" }
}
struct Plain {
    let w = WiFiStore.load()
}
struct Outer: View {
    enum Stage { static let first = WiFiStore.load() }
    var body: some View { Text("x") }
}
'''
        self.assertEqual(lines(src), [])


class Masking(unittest.TestCase):
    def test_strings_and_comments_are_not_code(self):
        src = '''
struct V: View {
    // let x = WiFiStore.load()
    /* var body: some View { LoginStore.login() } */
    var body: some View {
        TextField("{ \\"device_id\\": WiFiStore.load() }", text: .constant(""))
        Text(#"} raw "quote" { LoginStore.login() }"#)
        Text("""
            { WiFiStore.load() }
            """)
    }
    private func go() { _ = WiFiStore.load() }
}
'''
        self.assertEqual(lines(src), [], "a brace or a reader in a string or comment is not code")

    def test_an_interpolation_is_code(self):
        src = '''
struct V: View {
    var body: some View {
        Text("Wi-Fi: \\(WiFiStore.load()?.ssid ?? "none") and \\(f("(", 1))")
        Text(#"raw \\#(LoginStore.login()?.name ?? "")"#)
    }
}
'''
        self.assertEqual(lines(src), [4, 5])

    def test_mask_keeps_offsets_and_lines(self):
        src = 'let a = "x{y}" // c\n/* d\n e */ let b = 1\n'
        masked = L.mask(src)
        self.assertEqual(len(masked), len(src))
        self.assertEqual(masked.count("\n"), src.count("\n"))
        self.assertNotIn("{", masked)
        self.assertIn("let b = 1", masked)


class Main(unittest.TestCase):
    def _main(self, readers, found):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(L, "run", return_value=(readers, found)), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = L.main()
        return code, out.getvalue(), err.getvalue()

    def test_a_derivation_that_found_nothing_fails(self):
        code, _, err = self._main({("Keychain", "get")}, [])
        self.assertEqual(code, 1, "no reader past the seed means nothing was checked")
        self.assertIn("derivation broke", err)

    def test_a_finding_fails_and_a_clean_run_names_its_readers(self):
        readers = {("Keychain", "get"), ("WiFiStore", "load")}
        f = L.Finding("V.swift", 3, "WiFiStore.load", "in a property's initial value")
        code, _, err = self._main(readers, [f])
        self.assertEqual(code, 1)
        self.assertIn("V.swift:3: WiFiStore.load()", err)
        code, out, _ = self._main(readers, [])
        self.assertEqual(code, 0)
        self.assertIn("readers: Keychain.get, WiFiStore.load", out)


class RealTree(unittest.TestCase):
    def test_the_lint_reads_every_swift_directory_the_app_compiles(self):
        # ios/project.yml's SecuraCV target: a source directory the app
        # compiles is one whose views render and whose readers read.
        yml = (ROOT / "ios" / "project.yml").read_text(encoding="utf-8").splitlines()
        paths: list[str] = []
        inside = False
        for line in yml:
            if line.rstrip() == "  SecuraCV:":
                inside = True
            elif inside and line.startswith("  ") and not line.startswith("   "):
                break
            elif inside and line.strip().startswith("- path:"):
                paths.append(line.split(":", 1)[1].split("#")[0].strip())
        swift_dirs = [f"ios/{p}" for p in paths
                      if (ROOT / "ios" / p).is_dir() and any((ROOT / "ios" / p).rglob("*.swift"))]
        self.assertIn("ios/Sources/SecuraCV", swift_dirs)
        self.assertIn("ios/Shared", swift_dirs)
        for d in swift_dirs:
            for roots, what in ((L.VIEW_ROOTS, "VIEW_ROOTS"), (L.READER_ROOTS, "READER_ROOTS")):
                self.assertTrue(any(d == r or d.startswith(r + "/") for r in roots), f"{what} misses {d}")

    def test_no_keychain_read_on_a_render_path(self):
        readers, found = L.run()
        for need in [("HouseholdWiFiStore", "load"), ("HubSecretStore", "brokerLogin"),
                     ("TokenStore", "token"), ("PinnedKeyStore", "key")]:
            self.assertIn(need, readers, f"the derivation lost {need[0]}.{need[1]}")
        self.assertEqual([str(f) for f in found], [])


if __name__ == "__main__":
    unittest.main()
