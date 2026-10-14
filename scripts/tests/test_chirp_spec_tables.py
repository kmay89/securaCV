#!/usr/bin/env python3
"""Chirp spec tables say what the canary-wap stores and sends.

spec/chirp_channel_v0.md §11.2 listed NVS keys nothing stores
(`chirp_enabled`, `chirp_notify_sound`, `chirp_urgency_filter`) and left out
two the canary-wap does (`chirp_filter`, `st_chirp_at`) (sweep F236). And
§3.2 gave a presence beacon's emoji as 3 to 12 bytes while the sketch sends a
five-emoji display of 15 to 30 bytes in a 31-byte field (sweep F231). Both
were prose with nothing holding them to the code, so this test does:

  * NVS (§11.2). Every key the sketch reads or writes through nvs_store.h's
    chirp-namespace calls (`nvs_get_u8`, `nvs_set_u8`, `nvs_get_u32`,
    `nvs_set_u32`, `nvs_has_key`, `nvs_remove`, each opening
    `NVS_CHIRP_NS`, which is "chirp") is a row of the table, with its type,
    and every row is such a key. A call this reader cannot spell (a key that
    is not a string literal) or an `NvsSession` opened outside nvs_store.h
    fails the test rather than passing it: the reader would not see what
    that code stores. IDF's own `nvs_get_u8(handle, key, &v)` (three
    arguments) is another API and is not read.
  * Emoji (§3.2). The beacon's `emoji` field is `tstr .size (min..max)` where
    min and max are five times the shortest and longest entry of
    chirp_channel.cpp's EMOJI_SET in UTF-8 bytes, max is
    EMOJI_DISPLAY_SIZE - 1, and the spec names that field's size.

The self-tests at the bottom hold the reader to synthetic sources it must
refuse, so a reader that stops seeing a call fails here, not silently.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_chirp_spec_tables.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SKETCH = REPO / "firmware/projects/canary-wap/arduino/canary_wap"
SPEC = REPO / "spec/chirp_channel_v0.md"
NVS_STORE = SKETCH / "nvs_store.h"

CALLS = ("nvs_get_u8", "nvs_set_u8", "nvs_get_u32", "nvs_set_u32", "nvs_has_key", "nvs_remove")
TYPE_OF = {"nvs_get_u8": "uint8", "nvs_set_u8": "uint8", "nvs_get_u32": "uint32",
           "nvs_set_u32": "uint32"}


def strip_comments(src: str) -> str:
    """C/C++ comments out, string literals kept (a // inside one survives)."""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            out.append(" " if j < 0 else "\n" * src.count("\n", i, j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def call_args(src: str, open_paren: int) -> list[str] | None:
    """The top-level arguments of the call whose '(' is at open_paren."""
    depth, i, start, args = 0, open_paren, open_paren + 1, []
    while i < len(src):
        c = src[i]
        if c == '"':
            i += 1
            while i < len(src) and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append(src[start:i].strip())
                return args
        elif c == "," and depth == 1:
            args.append(src[start:i].strip())
            start = i + 1
        i += 1
    return None


class Unreadable(Exception):
    pass


def chirp_keys(sources: dict[str, str]) -> dict[str, set[str]]:
    """{key: {types}} for every chirp-namespace call in `sources` ({name: text})."""
    keys: dict[str, set[str]] = {}
    call = re.compile(r"(?<![\w:.>])(" + "|".join(CALLS) + r")\s*\(")
    for name, text in sources.items():
        src = strip_comments(text)
        if re.search(r"\bNvsSession\b", src):
            raise Unreadable(f"{name}: opens an NvsSession, whose keys this reader does not follow; "
                             "store through nvs_store.h's chirp calls or teach the reader")
        for m in call.finditer(src):
            fn = m.group(1)
            args = call_args(src, m.end() - 1)
            if args is None:
                raise Unreadable(f"{name}: {fn}( never closes")
            line = src.count("\n", 0, m.start()) + 1
            # A declaration or definition of the call itself is not a use.
            before = src[max(0, m.start() - 40):m.start()]
            if re.search(r"\b(bool|inline|esp_err_t)\s+$", before):
                continue
            want = 1 if fn in ("nvs_has_key", "nvs_remove") else 2
            if len(args) != want:
                if fn in TYPE_OF and len(args) == 3:
                    continue   # IDF's nvs_get_u8(handle, key, &v): another API, another namespace
                raise Unreadable(f"{name}:{line}: {fn} with {len(args)} arguments")
            key = re.fullmatch(r'"([^"\\]+)"', args[0])
            if key is None:
                raise Unreadable(f"{name}:{line}: {fn}({args[0]}, ...): a key this reader cannot spell")
            keys.setdefault(key.group(1), set())
            if fn in TYPE_OF:
                keys[key.group(1)].add(TYPE_OF[fn])
    return keys


def spec_section(spec: str, heading: str) -> str:
    m = re.search(r"^### " + re.escape(heading) + r"\s*$(.*?)(?=^##)", spec, re.M | re.S)
    assert m, f"spec section {heading!r} moved"
    return m.group(1)


def spec_nvs_rows(section: str) -> dict[str, str]:
    block = re.search(r"```\n(.*?)```", section, re.S)
    assert block, "§11.2 has no table block"
    rows: dict[str, str] = {}
    lines = [ln for ln in block.group(1).splitlines() if "|" in ln]
    assert lines and lines[0].split("|")[0].strip() == "NVS Key", "§11.2's header row moved"
    for ln in lines[1:]:
        cells = [c.strip() for c in ln.split("|")]
        if set(cells[0]) <= {"-"}:
            continue
        rows[cells[0]] = cells[1]
    return rows


def sketch_sources() -> dict[str, str]:
    out = {}
    for p in sorted(SKETCH.iterdir()):
        if p.suffix in (".cpp", ".h", ".ino") and p.name != "nvs_store.h":
            out[p.name] = p.read_text(encoding="utf-8", errors="replace")
    return out


def emoji_set_lengths(chirp_cpp: str) -> list[int]:
    m = re.search(r"EMOJI_SET\[16\]\s*=\s*\{(.*?)\};", chirp_cpp, re.S)
    assert m, "chirp_channel.cpp's EMOJI_SET moved"
    entries = re.findall(r'"((?:\\x[0-9A-Fa-f]{2})+)"', strip_comments(m.group(1)))
    assert len(entries) == 16, f"EMOJI_SET has {len(entries)} hex-escaped entries, not 16"
    return [len(e) // 4 for e in entries]


class ChirpSpecNvsTable(unittest.TestCase):
    def test_the_namespace_is_chirp(self):
        store = strip_comments(NVS_STORE.read_text(encoding="utf-8"))
        self.assertRegex(store, r'NVS_CHIRP_NS\s*=\s*"chirp"\s*;')
        for fn in CALLS:
            body = re.search(r"inline bool " + fn + r"\([^)]*\)\s*\{(.*?)\n\}", store, re.S)
            self.assertIsNotNone(body, f"nvs_store.h's {fn} moved")
            self.assertRegex(body.group(1), r"NvsSession nvs\(NVS_CHIRP_NS,",
                             f"{fn} no longer opens the chirp namespace")
        section = spec_section(SPEC.read_text(encoding="utf-8"), "11.2 NVS Storage")
        self.assertIn("namespace `chirp`", section)

    def test_the_table_is_the_keys_the_sketch_stores(self):
        code = chirp_keys(sketch_sources())
        rows = spec_nvs_rows(spec_section(SPEC.read_text(encoding="utf-8"), "11.2 NVS Storage"))
        self.assertEqual(sorted(rows), sorted(code),
                         "spec §11.2's keys are not the keys the canary-wap stores in namespace chirp")
        for key, types in code.items():
            self.assertEqual(len(types), 1, f"{key} is read or written as {sorted(types)}")
            self.assertEqual(rows[key], next(iter(types)), f"§11.2 gives {key} as {rows[key]}")
        # The keys F236 named, as the item found them.
        self.assertEqual(sorted(code), ["chirp_filter", "chirp_relay", "st_chirp_at"])


class ChirpSpecEmojiField(unittest.TestCase):
    def test_the_beacon_field_is_five_emoji_in_31_bytes(self):
        chirp = (SKETCH / "chirp_channel.cpp").read_text(encoding="utf-8")
        mesh = (SKETCH / "mesh_network.h").read_text(encoding="utf-8")
        lens = emoji_set_lengths(chirp)
        count = re.search(r"const int EMOJIS_TO_RENDER = (\d+);", chirp)
        size = re.search(r"static const size_t EMOJI_DISPLAY_SIZE = (\d+);", mesh)
        self.assertTrue(count and size, "EMOJIS_TO_RENDER or EMOJI_DISPLAY_SIZE moved")
        n, field = int(count.group(1)), int(size.group(1))
        lo, hi = n * min(lens), n * max(lens)
        self.assertEqual((n, lo, hi, field), (5, 15, 30, 31))
        self.assertEqual(hi, field - 1, "the longest display must fit the field with its NUL")
        section = spec_section(SPEC.read_text(encoding="utf-8"), "3.2 CHIRP_PRESENCE (Discovery)")
        m = re.search(r"emoji:\s*tstr \.size \((\d+)\.\.(\d+)\),[^\n]*NUL-padded to (\d+) bytes", section)
        self.assertIsNotNone(m, "§3.2's emoji field no longer reads tstr .size (lo..hi) ... NUL-padded to N bytes")
        self.assertEqual(tuple(map(int, m.groups())), (lo, hi, field))
        self.assertIn("five-emoji", m.group(0))


class ReaderSelfTest(unittest.TestCase):
    """The reader refuses what it cannot read, and reads what it must."""

    def test_reads_literal_keys_and_types(self):
        src = {"a.cpp": 'void f(){ uint8_t v; nvs_get_u8("k1", &v); nvs_set_u32("k2", 7u);\n'
                        '  nvs_has_key("k3"); /* nvs_set_u8("gone", 1); */ // nvs_remove("gone2")\n'
                        '  err = nvs_set_u8(handle, key, value); }'}
        self.assertEqual(chirp_keys(src), {"k1": {"uint8"}, "k2": {"uint32"}, "k3": set()})

    def test_refuses_a_key_it_cannot_spell(self):
        with self.assertRaises(Unreadable):
            chirp_keys({"a.cpp": "void f(const char* k){ nvs_set_u8(k, 1); }"})
        with self.assertRaises(Unreadable):
            chirp_keys({"a.cpp": 'void f(){ nvs_set_u8(PREFIX "x", 1); }'})

    def test_refuses_a_session_it_does_not_follow(self):
        with self.assertRaises(Unreadable):
            chirp_keys({"a.cpp": "void f(){ NvsSession nvs(NVS_CHIRP_NS, false); nvs.setU8(\"x\", 1); }"})

    def test_refuses_a_call_with_the_wrong_shape(self):
        with self.assertRaises(Unreadable):
            chirp_keys({"a.cpp": 'void f(){ nvs_has_key("a", "b"); }'})

    def test_a_stale_or_missing_row_fails(self):
        rows = spec_nvs_rows("```\nNVS Key | Type | Description\n--------|------|---\n"
                             "chirp_relay | uint8 | x\nchirp_enabled | uint8 | y\n```\n")
        code = chirp_keys({"a.cpp": 'void f(){ nvs_set_u8("chirp_relay", 1); nvs_set_u8("chirp_filter", 0); }'})
        self.assertNotEqual(sorted(rows), sorted(code))


if __name__ == "__main__":
    unittest.main()
