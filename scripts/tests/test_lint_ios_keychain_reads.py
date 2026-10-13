#!/usr/bin/env python3
"""Tests for scripts/lint_ios_keychain_reads.py — the guard needs a guard.

The lint decides whether a Keychain read in a SwiftUI view is on the render
path by masking comments and strings and walking braces with regexes. That is
a heuristic, and a heuristic nobody tests reads as covered while catching
nothing, so each verdict it gives is pinned here on a small Swift fixture:

  * refused: a `@State` (or any stored) initial value — the A75 shape
    `SetupView` had — `body`, a `some View` property or function, a
    container's `footer:` or a `Button`'s `label:` closure, the label of
    `Button(action:)`, and a view's `init`;
  * allowed: `.task`, `.onAppear` (closure or `perform:`), `.onChange`,
    `Task { }`, a `Button`'s action (trailing or `action:`), `.fileImporter`
    and any `func` that does not return a view, and every read outside a
    `View` struct (a store, a type nested in a view);
  * masking: a brace or a reader name inside a string or a comment is not
    code, while a call inside a string interpolation is.

The readers are derived, so the derivation is pinned too: a primitive read,
a reader that calls a reader through `Self.`, and a protocol requirement with
no body that must not borrow the next declaration's braces. Then the real
tree: no finding, and the readers the setup screens use are in the set.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_*.py'
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

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


class RealTree(unittest.TestCase):
    def test_no_keychain_read_on_a_render_path(self):
        readers, found = L.run()
        for need in [("HouseholdWiFiStore", "load"), ("HubSecretStore", "brokerLogin"),
                     ("TokenStore", "token"), ("PinnedKeyStore", "key")]:
            self.assertIn(need, readers, f"the derivation lost {need[0]}.{need[1]}")
        self.assertEqual([str(f) for f in found], [])


if __name__ == "__main__":
    unittest.main()
