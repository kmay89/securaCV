#!/usr/bin/env python3
"""lint_ios_keychain_reads.py — no Keychain read on a SwiftUI view's render path.

A SwiftUI view struct is rebuilt far more often than it appears. Its `@State`
initial values are evaluated on every init (a parent's re-render inits it
again; SwiftUI then throws the new value away), and its `body`, and every
`some View` property or function `body` reaches, runs on every change the
objects it observes publish. The setup screens observe `FleetStore`, which
republishes on every Bluetooth sighting while a scan runs, and a Keychain
read (`SecItemCopyMatching`) is a synchronous trip to securityd on the main
thread. So a Keychain read there is a stall per advert, for a value that does
not change while the screen is up.

The 2026-10 review moved one such read (`CanarySetupView`'s remembered Wi-Fi)
into the screen's on-appear prefill and left the same read in a `@State`
initial value in `SetupView`, plus the hub's broker login read in the `body`
of three setup screens (sweep A75). This lint is what keeps them out.

THE RULE. Inside a `struct` that conforms to `View`, a call to a Keychain
reader may sit only

  * inside a `func` that does not return `some View` (an action, a prefill,
    a `begin()` the view's `.task` awaits), or
  * inside an effect closure: `.task`, `.onAppear`, `.onDisappear`,
    `.onChange`, `.onReceive`, `.refreshable`, `.onSubmit`, `.onOpenURL`,
    `.onTapGesture`, `.onLongPressGesture`, `.fileImporter`,
    `.fileExporter`, or a `Button`'s action closure (not its `label:`, and
    not the trailing closure of `Button(action:)`, which is the label).

A `Task { }` is neither: it runs where it is made, so the brace around it
decides (one made in a `.task` or an action is fine; one made in `body` is
made on every render). Anywhere else in a view — a `@State` or stored
property's initial value, an `init`, `body`, a computed property, a
`some View` function, a container's, a view modifier's (`.overlay`,
`.sheet`, `.toolbar`, ...) or a `label:`/`footer:` closure — it is on the
render path and is refused.

THE READERS are derived, not listed: every `static func` in `ios/Sources`
or `ios/Shared` whose body calls `Keychain.get(` or `SecItemCopyMatching(`,
plus every `static func` that calls one of those (`Self.f(`, `Type.f(` or a
bare `f(` of its own type), to a fixed point. A clean run prints the set it
derived; the test pins that the setup screens' readers are in it, and the run
fails when it derives nothing beyond `Keychain.get` (a derivation that broke
would otherwise check nothing and pass).

WHAT IT DOES NOT SEE (a heuristic over masked source, not a Swift parser):
a reader reached through an instance (`KeychainSlots().get`), through a
helper `func` the render path calls (a `func` is presumed to be an action
unless it returns `some View`), or from a view declared in an `extension`.
Comments and string literals are masked before anything is matched, so a
brace or a call inside either is not code; string interpolations are code.

Run: python3 scripts/lint_ios_keychain_reads.py   (exit 0 = clean, 1 = findings)
Tested by scripts/tests/test_lint_ios_keychain_reads.py.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Where the readers are derived from, and where views are checked.
READER_ROOTS = ("ios/Sources", "ios/Shared")
VIEW_ROOTS = ("ios/Sources", "ios/Shared")

# The primitive reads: anything that calls one of these reads the Keychain.
PRIMITIVES = (re.compile(r"\bKeychain\s*\.\s*get\s*\("), re.compile(r"\bSecItemCopyMatching\s*\("))

# Effect closures: a `{` whose header (the code on its line before it) ends
# in one of these runs on an event, not on a render.
_EFFECT_MODIFIERS = (
    "task", "onAppear", "onDisappear", "onChange", "onReceive", "refreshable",
    "onSubmit", "onOpenURL", "onTapGesture", "onLongPressGesture",
    "fileImporter", "fileExporter",
)
# `Task` is not here: a Task runs where it is made, so the brace around it
# decides (classify() walks past it to the next one out).
EFFECT_HEADER = re.compile(
    r"(?:\.(?:%s)\b(?:\s*\(.*\))?|\bButton\b(?:\s*\(.*\))?)"
    r"\s*(?:\{\s*)?$" % "|".join(_EFFECT_MODIFIERS)
)
CLOSURE_SIGNATURE = re.compile(r"^\s*(?:@\w+\s+)?(?:\[[^\]]*\]\s*)?(?:[\w\s,()]*\bin\b)?")

TYPE_DECL = re.compile(r"\b(struct|enum|class|actor|extension)\s+(\w+)\b([^{;]*)\{")
STATIC_FUNC = re.compile(r"\bstatic\s+func\s+(\w+)\b")
CALL = re.compile(r"\b(\w+)\s*\.\s*(\w+)\s*\(")
BARE_CALL = re.compile(r"(?<![.\w])(\w+)\s*\(")
FUNC_HEADER = re.compile(r"\bfunc\s+\w+")
INIT_DECL = re.compile(
    r"^\s*(?:@\w+\s+)*(?:(?:public|private|fileprivate|internal|convenience|required|override|nonisolated)\s+)*"
    r"init\s*[?!]?\s*[(<]", re.M)
SOME_VIEW = re.compile(r"->\s*some\s+\w+")


def mask(src: str) -> str:
    """Return src with comments and string-literal text blanked to spaces
    (newlines kept, so offsets and line numbers survive). Code inside a
    string interpolation `\\( … )` is kept: it is code."""
    out = list(src)
    n = len(src)

    def blank(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    def scan_string(j: int, delim: str, hashes: int) -> tuple[str, int]:
        """Scan string text from j. Returns ("end", index after the closing
        delimiter) or ("interp", index after the opening `\\(`)."""
        close = delim + "#" * hashes
        escape = "\\" + "#" * hashes
        start = j
        while j < n:
            if src.startswith(close, j):
                blank(start, j)
                return "end", j + len(close)
            if src.startswith(escape, j):
                k = j + len(escape)
                if k < n and src[k] == "(":
                    blank(start, k)
                    return "interp", k + 1
                j = k + 1
                continue
            if delim == '"' and src[j] == "\n":
                blank(start, j)  # an unterminated one-line string ends at the line
                return "end", j
            j += 1
        blank(start, n)
        return "end", n

    stack: list[list] = []  # [delim, hashes, paren depth] per open interpolation
    i = 0
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            blank(i, j)
            i = j
            continue
        if src.startswith("/*", i):
            depth, j = 0, i
            while j < n:
                if src.startswith("/*", j):
                    depth += 1
                    j += 2
                elif src.startswith("*/", j):
                    depth -= 1
                    j += 2
                    if depth == 0:
                        break
                else:
                    j += 1
            blank(i, j)
            i = j
            continue
        c = src[i]
        if stack and c == "(":
            stack[-1][2] += 1
        elif stack and c == ")":
            if stack[-1][2] == 0:
                delim, hashes, _ = stack.pop()
                state, i = scan_string(i + 1, delim, hashes)
                if state == "interp":
                    stack.append([delim, hashes, 0])
                continue
            stack[-1][2] -= 1
        m = re.match(r'(#*)("""|")', src[i:i + 8])
        if m and (m.group(1) or c == '"'):
            hashes = len(m.group(1))
            delim = m.group(2)
            state, i = scan_string(i + m.end(), delim, hashes)
            if state == "interp":
                stack.append([delim, hashes, 0])
            continue
        i += 1
    return "".join(out)


def match_brace(text: str, open_at: int) -> int:
    """Index of the `}` closing the `{` at open_at (len(text) if none)."""
    depth = 0
    for k in range(open_at, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return k
    return len(text)


@dataclass(frozen=True)
class Span:
    kind: str
    name: str
    conforms: str
    open: int
    close: int


def type_spans(text: str) -> list[Span]:
    spans = []
    for m in TYPE_DECL.finditer(text):
        o = m.end() - 1
        spans.append(Span(m.group(1), m.group(2), m.group(3), o, match_brace(text, o)))
    return spans


def enclosing_type(spans: list[Span], at: int) -> Span | None:
    best = None
    for s in spans:
        if s.open < at < s.close and (best is None or s.open > best.open):
            best = s
    return best


def derive_readers(files: dict[str, str]) -> set[tuple[str, str]]:
    """(Type, func) pairs whose static func reads the Keychain, to a fixed point."""
    bodies: list[tuple[str, str, str]] = []  # (type, func, masked body)
    for text in files.values():
        spans = type_spans(text)
        for m in STATIC_FUNC.finditer(text):
            o = text.find("{", m.end())
            # A requirement with no body (a protocol's) must not borrow the
            # next declaration's braces.
            if o < 0 or "}" in text[m.end():o] or re.search(r"\b(?:func|var|let|case)\b", text[m.end():o]):
                continue
            owner = enclosing_type(spans, m.start())
            if owner is None:
                continue
            bodies.append((owner.name, m.group(1), text[o:match_brace(text, o) + 1]))
    readers: set[tuple[str, str]] = {("Keychain", "get")}
    changed = True
    while changed:
        changed = False
        for owner, func, body in bodies:
            if (owner, func) in readers:
                continue
            hit = any(p.search(body) for p in PRIMITIVES)
            if not hit:
                for c in CALL.finditer(body):
                    t = owner if c.group(1) == "Self" else c.group(1)
                    if (t, c.group(2)) in readers:
                        hit = True
                        break
            if not hit:
                for c in BARE_CALL.finditer(body):
                    if (owner, c.group(1)) in readers:
                        hit = True
                        break
            if hit:
                readers.add((owner, func))
                changed = True
    return readers


def header_of(text: str, brace: int) -> str:
    """The code that opens a brace: everything back to the previous brace (or
    `;`), so a signature split over lines, or a modifier after a chain of
    others, is read whole."""
    k = brace - 1
    while k >= 0 and text[k] not in "{};":
        k -= 1
    return text[k + 1:brace].strip()


def classify(text: str, view: Span, at: int) -> str | None:
    """None when the call at `at` is off the render path, else the reason."""
    stack: list[int] = []
    k = view.open + 1
    while k < at:
        ch = text[k]
        if ch == "{":
            stack.append(k)
        elif ch == "}" and stack:
            stack.pop()
        k += 1
    for brace in reversed(stack):  # innermost first
        head = header_of(text, brace)
        if FUNC_HEADER.search(head):
            if SOME_VIEW.search(head):
                return "inside a function that returns a view"
            return None
        if INIT_DECL.search(head):
            return "inside the view's init"
        if re.search(r"\b(?:action|perform)\s*:\s*$", head):
            return None  # the closure IS the action: Button(action: { … }), .onAppear(perform: { … })
        if re.search(r"\b(?:action|intent)\s*:", head):
            continue  # Button(action: f) { … } — that trailing closure is the label
        if EFFECT_HEADER.search(head):
            return None
    if not stack:
        return "in a property's initial value"
    return "on the render path (body, a computed property or a view closure)"


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    call: str
    reason: str

    def __str__(self) -> str:
        return (f"{self.path}:{self.line}: {self.call}() reads the Keychain {self.reason}; "
                "read it in the view's onAppear/.task (or a func it calls) into a @State")


def check_text(path: str, text: str, readers: set[tuple[str, str]]) -> list[Finding]:
    findings = []
    spans = type_spans(text)
    views = [s for s in spans if s.kind == "struct" and re.search(r"\bView\b", s.conforms)]
    for c in CALL.finditer(text):
        if (c.group(1), c.group(2)) not in readers:
            continue
        view = enclosing_type(views, c.start())
        if view is None:
            continue
        # The innermost type must be the view itself, not a type nested in it.
        inner = enclosing_type(spans, c.start())
        if inner is not None and inner != view:
            continue
        reason = classify(text, view, c.start())
        if reason:
            findings.append(Finding(path, text.count("\n", 0, c.start()) + 1,
                                    f"{c.group(1)}.{c.group(2)}", reason))
    return findings


def swift_files(roots: tuple[str, ...], base: Path = ROOT) -> dict[str, str]:
    files = {}
    for r in roots:
        for p in sorted((base / r).rglob("*.swift")):
            files[p.relative_to(base).as_posix()] = mask(p.read_text(encoding="utf-8"))
    return files


def run(base: Path = ROOT) -> tuple[set[tuple[str, str]], list[Finding]]:
    readers = derive_readers(swift_files(READER_ROOTS, base))
    findings: list[Finding] = []
    for path, text in swift_files(VIEW_ROOTS, base).items():
        findings.extend(check_text(path, text, readers))
    return readers, findings


def main() -> int:
    readers, findings = run()
    if not readers - {("Keychain", "get")}:
        print("lint_ios_keychain_reads: derived no Keychain reader beyond Keychain.get — "
              "the derivation broke, so nothing would be checked", file=sys.stderr)
        return 1
    for f in findings:
        print(f, file=sys.stderr)
    if findings:
        return 1
    names = ", ".join(sorted(f"{t}.{f}" for t, f in readers))
    print(f"lint_ios_keychain_reads: no Keychain read on a view's render path (readers: {names})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
