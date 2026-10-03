#!/usr/bin/env python3
"""Every canary-wap REST answer fits the buffer it is serialized into (sweep F196).

ArduinoJson's `serializeJson(doc, char_array)` and its sized form
`serializeJson(doc, buf, n)` write at most the buffer's size and add the
terminating NUL only when the answer is shorter than that (7.4.1's
`serialize()`: `if (n < bufferSize) buffer[n] = 0;`): an answer that fills
the buffer leaves it cut and unterminated, and the `httpd_resp_sendstr()`
that follows sends it and whatever memory follows it, up to the next zero
byte. The Chirp confirm and mute refusals went out that way from the stack
(F174), and so did `GET /api/chirp/nearby` once its neighbors' presence
beacons carried emoji of bytes JSON escapes, and `GET /api/chirp/recent`,
from the heap, once its list was full: sixteen ordinary chirps are about
4.8 KB against its 4096-byte buffer (F196, an ArduinoJson 7.4.1 scratch
harness of the real handlers).

This check measures every one of them, in the canary-wap's REST answer files
(`SCOPE`: every `*_api.h` of the sketch but the ones `EXCLUDED` names, and
`canary_wap.ino`):

J1. Each `serializeJson(` serializes into a `String` or a `File` (they grow),
    into a heap buffer of `measureJson(doc) + 1` bytes (allocated from that
    length, serialized with it), or into a fixed buffer behind a
    `measureJson(doc) >= sizeof(buf)` refusal (the answer is refused whole,
    never cut). The measure must be of the answer that goes out: between a
    `needed = measureJson(doc) + 1;` and its serialize stand only the one
    allocation from `needed`, constants, and `if`s on `needed` or on the
    allocation whose bodies return; a refusal's body must return, with
    nothing between it and the serialize. A key written after the measure
    would fill the measured buffer, unterminated. Anything else is a fixed
    buffer, J2's.
J2. A fixed buffer (`char buf[N]`, the declaration in scope at the
    serialize, or a constant-size allocation with the sized form) is held
    to the longest answer this check computes from the document's own
    statements: every `x["key"] = value;` into the document and the objects
    and arrays it hands out, each value bounded by what it is (a literal by
    its escaped bytes, a lookup function by the longest string it returns,
    a `const char*` by every value assigned to it before the answer, a char
    array by twice its length, since ArduinoJson escapes `"`, `\\`, `\\b`,
    `\\f`, `\\n`, `\\r` and `\\t` with two bytes and writes every other byte
    as it is, a number by its type's widest spelling, a float by 26 bytes).
    N must exceed it. The bound is computed, never typed: a key or a longer
    message added to an answer moves it.
J3. What the check cannot bound fails: an array built in a loop, a value
    whose type it cannot find, a `const char*` whose strings it cannot see
    (one handed out by address, stepped, or written through), a `char*`
    anything may write, a document it cannot follow. Such an answer
    serializes to `measureJson()`'s length (J1), which needs no bound. A
    helper whose document carries a `const char*` parameter
    (`send_err(req, msg)`) is measured at every call of it in its file, and
    at every value the helper assigns it.
J4. The fleet scan's cache is not an answer but the handler answers from it
    (F211): `fleet_scan_task()` writes it only through `fleet_scan_cache.h`
    (host-tested: the adverts that fit, whole, and a complete document at
    every step). The task begins a `fleet_scan_cache::Cache` over the
    staging buffer it allocated with `FLEET_SCAN_CACHE_SIZE` bytes, with that
    size, adds the adverts with `fleet_scan_cache::add()`, and copies the
    buffer into the cache with that size; nothing else writes the staging
    buffer. No site is exempt from J1-J3: the one that was, the cut cache's
    `serializeJson()`, is gone.

The value bounds follow ArduinoJson 7.4.1's TextFormatter (the version the
sketch builds against, `arduino-libs.txt`). The check runs each mutation in
`MUTATIONS` and must refuse every one.

Run locally:  python3 firmware/scripts/check_wap_json_answers.py   (repo root)
CI:           firmware.yml "Regression guard", via regression_check.sh
"""

from __future__ import annotations

import functools
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_event_egress_order import (  # noqa: E402  (shared C++ scanning helpers)
    AnchorMissing,
    blank_comments_and_strings as _blank,
    matching_paren,
)

blank_comments_and_strings = functools.lru_cache(maxsize=256)(_blank)

REPO = Path(__file__).resolve().parents[2]
SKETCH = "firmware/projects/canary-wap/arduino/canary_wap"
INO = f"{SKETCH}/canary_wap.ino"
# Answer files left out, with why. A file named here is not measured.
EXCLUDED: dict[str, str] = {}
# J4: the fleet scan's cache, its writer and its size.
FLEET_TASK = "fleet_scan_task"
FLEET_SIZE = "FLEET_SCAN_CACHE_SIZE"

# ArduinoJson 7.4.1 writes an integer in its type's widest decimal spelling at
# most; a float with up to 9 decimals and an exponent (JsonFloat is double).
INT_WIDTHS = {
    "bool": 5, "uint8_t": 3, "int8_t": 4, "uint16_t": 5, "int16_t": 6, "uint32_t": 10,
    "int32_t": 11, "uint64_t": 20, "int64_t": 20, "int": 11, "unsigned": 10,
    "unsignedint": 10, "long": 11, "unsignedlong": 10, "longlong": 20, "unsignedlonglong": 20,
    "size_t": 10, "ssize_t": 11, "time_t": 20, "short": 6, "unsignedshort": 5,
    "esp_err_t": 11, "uint_fast8_t": 10, "int_fast8_t": 11,
}
FLOAT_TYPES = {"float", "double"}
FLOAT_WIDTH = 26
# Functions the sketch calls from the Arduino core and ESP-IDF: their widths.
BUILTIN_WIDTHS = {"millis": 10, "micros": 10, "esp_get_free_heap_size": 10,
                  "esp_get_minimum_free_heap_size": 10, "heap_caps_get_free_size": 10,
                  "heap_caps_get_largest_free_block": 10, "esp_timer_get_time": 20}
KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "defined", "alignof",
            "decltype", "static_assert", "else", "do", "new", "delete", "case", "goto", "throw"}
CHAR_ESCAPE = 2          # bytes ArduinoJson spends at most on one byte of a char array


def sketch_files() -> dict[str, str]:
    out = {}
    for pattern in ("*.h", "*.cpp", "*.ino"):
        for path in sorted((REPO / SKETCH).glob(pattern)):
            out[path.relative_to(REPO).as_posix()] = path.read_text(encoding="utf-8", errors="replace")
    return out


def scope_of(files: dict[str, str]) -> list[str]:
    names = sorted(n for n in files if re.search(r"/[^/]*_api\.h$", n) and n not in EXCLUDED)
    return names + [INO]


@functools.lru_cache(maxsize=256)
def blank_comments_only(src: str) -> str:
    """Comments become spaces, string literals stay (offsets kept)."""
    code = blank_comments_and_strings(src)
    out = list(code)
    # Copy every literal's inside back from the source.
    i, n = 0, len(code)
    while i < n:
        c = code[i]
        if c in "\"'" and (i == 0 or code[i - 1] != "\\"):
            j = i + 1
            while j < n and code[j] != c and code[j] != "\n":
                j += 1
            out[i:j + 1] = src[i:j + 1]
            i = j + 1
        else:
            i += 1
    return "".join(out)


def close_brace(code: str, open_at: int) -> int:
    depth = 0
    for j in range(open_at, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return j
    return -1


def split_top(text: str, sep: str = ",") -> list[str]:
    """Split on `sep` outside (), [], {}, <> of templates and string literals."""
    parts, depth, cur, i, quote = [], 0, [], 0, None
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == "\\" and i + 1 < len(text):
                cur.append(text[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "([{":
            depth += 1
            cur.append(c)
        elif c in ")]}":
            depth -= 1
            cur.append(c)
        elif c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    parts.append("".join(cur))
    return [p.strip() for p in parts]


def statement_end(text: str, start: int) -> int:
    """The `;` that ends the statement starting at `start` (outside literals and brackets)."""
    depth, i, quote = 0, start, None
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\":
                i += 2
                continue
            if c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ";" and depth == 0:
            return i
        i += 1
    return -1


# ── C string literals and their JSON length ──────────────────────────────

C_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f", "v": "\v", "a": "\a",
             "\\": "\\", "\"": "\"", "'": "'", "?": "?", "0": "\0"}


def c_literal_bytes(lit: str) -> bytes | None:
    """The bytes of one or more adjacent C string literals ("a" "b"), or None."""
    pieces = re.findall(r'"((?:[^"\\\n]|\\.)*)"', lit)
    if not pieces or re.sub(r'"(?:[^"\\\n]|\\.)*"', "", lit).strip():
        return None
    out = bytearray()
    for p in pieces:
        i = 0
        while i < len(p):
            c = p[i]
            if c != "\\":
                out += c.encode("utf-8")
                i += 1
                continue
            nxt = p[i + 1]
            if nxt == "x":
                m = re.match(r"[0-9a-fA-F]+", p[i + 2:])
                out.append(int(m.group(0), 16) & 0xFF)
                i += 2 + len(m.group(0))
            elif nxt in "01234567":
                m = re.match(r"[0-7]{1,3}", p[i + 1:])
                out.append(int(m.group(0), 8) & 0xFF)
                i += 1 + len(m.group(0))
            else:
                out += C_ESCAPES.get(nxt, nxt).encode("utf-8")
                i += 2
    return bytes(out)


def json_string_len(data: bytes) -> int:
    """What ArduinoJson 7.4.1's TextFormatter::writeString spends on `data`, quotes included."""
    n = 2
    for b in data:
        if b == 0:
            break                       # a const char* ends at its NUL
        n += 2 if chr(b) in "\"\\\b\f\n\r\t" else 1
    return n


# ── The sketch's declarations ────────────────────────────────────────────

@dataclass
class Fn:
    name: str
    ns: tuple[str, ...]
    ret: str
    params: str
    file: str
    body: tuple[int, int] | None      # offsets of the body in the file, braces excluded


@dataclass
class Struct:
    name: str
    ns: tuple[str, ...]
    fields: dict[str, tuple[str, list[str]]] = field(default_factory=dict)


@dataclass
class Index:
    files: dict[str, str]
    fns: dict[str, list[Fn]] = field(default_factory=dict)
    structs: dict[str, list[Struct]] = field(default_factory=dict)
    enums: dict[str, list[tuple[tuple[str, ...], str]]] = field(default_factory=dict)
    consts: dict[str, list[tuple[tuple[str, ...], str]]] = field(default_factory=dict)
    table_fields: dict[tuple[str, str], int] = field(default_factory=dict)   # bound_table_field's


def namespace_spans(code: str) -> list[tuple[int, int, str]]:
    spans = []
    for m in re.finditer(r"\bnamespace\s+(\w+)?\s*\{", code):
        end = close_brace(code, m.end() - 1)
        if end > 0:
            spans.append((m.end(), end, m.group(1) or ""))
    return spans


def ns_at(spans: list[tuple[int, int, str]], pos: int) -> tuple[str, ...]:
    return tuple(name for s, e, name in spans if s <= pos < e and name)


FN_DEF = re.compile(
    r"(?:^|(?<=[;{}\n]))[ \t]*(?:template\s*<[^>]*>\s*)?"
    r"(?P<quals>(?:(?:static|inline|constexpr|extern|virtual|IRAM_ATTR|\[\[maybe_unused\]\])\s+)*)"
    r"(?P<ret>(?:(?:const|unsigned|signed|struct|enum)\s+)*[A-Za-z_][\w:]*(?:\s*<[^<>;(){}]*>)?"
    r"(?:\s+(?:long|int|short|char))*)(?:\s*(?P<ptr>[*&]+)\s*|\s+)(?P<name>[A-Za-z_]\w*)\s*\(",
    re.M)


@functools.lru_cache(maxsize=256)
def index_file(name: str, src: str) -> Index:
    """One file's declarations (cached: each mutation of the self-test re-reads only the file it
    changed)."""
    idx = Index({})
    code = blank_comments_and_strings(src)
    spans = namespace_spans(code)
    # Functions: a return type, a name, a parameter list, then a body or `;`.
    for m in FN_DEF.finditer(code):
        fname = m.group("name")
        ret = (m.group("ret") + (m.group("ptr") or "")).strip()
        if fname in KEYWORDS or ret.split()[-1] in KEYWORDS | {"else", "return"}:
            continue
        open_at = m.end() - 1
        close = matching_paren(code, open_at)
        if close < 0:
            continue
        rest = code[close + 1:close + 120]
        tail = re.match(r"\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?([{;])", rest)
        if not tail:
            continue
        body = None
        if tail.group(1) == "{":
            b_open = close + 1 + tail.end() - 1
            b_close = close_brace(code, b_open)
            if b_close < 0:
                continue
            body = (b_open + 1, b_close)
        fn = Fn(fname, ns_at(spans, m.start()), re.sub(r"\s+", " ", ret), code[open_at + 1:close],
                name, body)
        idx.fns.setdefault(fname, []).append(fn)
    # Structs and classes, and typedef'd anonymous ones.
    for m in re.finditer(r"\b(?:struct|class)\s+(\w+)\s*(?::[^{;]*)?\{", code):
        end = close_brace(code, m.end() - 1)
        if end > 0:
            st = Struct(m.group(1), ns_at(spans, m.start()))
            parse_fields(code[m.end():end], st)
            idx.structs.setdefault(st.name, []).append(st)
    for m in re.finditer(r"\btypedef\s+struct\s*\w*\s*\{", code):
        end = close_brace(code, m.end() - 1)
        after = re.match(r"\s*(\w+)\s*;", code[end + 1:]) if end > 0 else None
        if after:
            st = Struct(after.group(1), ns_at(spans, m.start()))
            parse_fields(code[m.end():end], st)
            idx.structs.setdefault(st.name, []).append(st)
    for m in re.finditer(r"\benum\s+(?:class\s+)?(\w+)\s*(?::\s*([\w:]+))?\s*\{", code):
        idx.enums.setdefault(m.group(1), []).append((ns_at(spans, m.start()), m.group(2) or "int"))
    for m in re.finditer(r"\b(?:static\s+)?(?:constexpr|const)\s+(?:unsigned\s+)?[\w:]+\s+(\w+)\s*=\s*([^;{}]+);",
                         code):
        idx.consts.setdefault(m.group(1), []).append((ns_at(spans, m.start()), m.group(2).strip()))
    for m in re.finditer(r"^[ \t]*#\s*define\s+(\w+)[ \t]+([^\n]+)$", code, re.M):
        idx.consts.setdefault(m.group(1), []).append(((), m.group(2).strip()))
    return idx


def build_index(files: dict[str, str]) -> Index:
    idx = Index(files)
    for name, src in files.items():
        part = index_file(name, src)
        for mine, theirs in ((idx.fns, part.fns), (idx.structs, part.structs), (idx.enums, part.enums),
                             (idx.consts, part.consts)):
            for key, vals in theirs.items():
                mine.setdefault(key, []).extend(vals)
    return idx


def parse_fields(body: str, st: Struct) -> None:
    depth, cur = 0, []
    for c in body:
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            cur = []
            continue
        if depth:
            continue
        if c == ";":
            decl = "".join(cur).strip()
            cur = []
            m = re.match(r"^(?:static\s+|mutable\s+|volatile\s+)*(?P<type>(?:const\s+)?(?:unsigned\s+|signed\s+)?"
                         r"[A-Za-z_][\w:]*(?:\s+(?:long|int|short|char))*(?:\s*<[^;]*>)?\s*[*&]*)\s*"
                         r"(?P<name>[A-Za-z_]\w*)\s*(?P<dims>(?:\[[^\]]*\]\s*)*)\s*(?::\s*\d+)?\s*(?:=.*)?$",
                         decl, re.S)
            if m and "(" not in decl:
                st.fields[m.group("name")] = (re.sub(r"\s+", " ", m.group("type")).strip(),
                                              re.findall(r"\[([^\]]*)\]", m.group("dims")))
        else:
            cur.append(c)


def pick(cands: list, ns: tuple[str, ...], qual: tuple[str, ...]) -> list:
    """Candidates whose namespace ends with `qual`; among them, those nearest `ns`."""
    out = [c for c in cands if tuple(c.ns if hasattr(c, "ns") else c[0])[-len(qual):] == qual] if qual else list(cands)
    if not qual and len(out) > 1:
        for k in range(len(ns), -1, -1):
            near = [c for c in out if tuple(c.ns if hasattr(c, "ns") else c[0]) == ns[:k]]
            if near:
                return near
    return out


def const_value(idx: Index, expr: str, ns: tuple[str, ...], depth: int = 0) -> int | None:
    """The integer value of a constant expression of literals and named constants."""
    if depth > 8:
        return None
    expr = expr.strip()

    def name_value(m: re.Match) -> str:
        parts = m.group(0).split("::")
        name, qual = parts[-1], tuple(p for p in parts[:-1] if p)
        cands = pick(idx.consts.get(name, []), ns, qual)
        vals = {const_value(idx, e, c_ns, depth + 1) for c_ns, e in cands}
        if len(vals) != 1 or None in vals:
            raise ValueError(name)
        return str(vals.pop())
    try:
        text = re.sub(r"\(\s*(?:size_t|int|uint32_t|uint16_t|uint8_t|unsigned)\s*\)", "", expr)
        text = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b", lambda m: str(int(m.group(1), 0)), text)
        text = re.sub(r"\b[A-Za-z_][\w]*(?:::[A-Za-z_]\w*)*\b", name_value, text)
        if not re.fullmatch(r"[\d\s+\-*/()%]+", text):
            return None
        return int(eval(text.replace("/", "//"), {"__builtins__": {}}))  # noqa: S307 (digits and operators only)
    except (ValueError, SyntaxError, ZeroDivisionError):
        return None


# ── Bounding a value ─────────────────────────────────────────────────────

@dataclass
class Ctx:
    idx: Index
    file: str
    fn: Fn
    kept: str          # the file, comments blanked, literals kept
    code: str          # the file, comments and literals blanked
    depth: int = 0

    @property
    def body(self) -> str:
        return self.kept[self.fn.body[0]:self.fn.body[1]]


class Unbounded(Exception):
    """A value whose longest JSON spelling the check cannot find."""


def type_width(idx: Index, t: str, ns: tuple[str, ...], dims: list[str] | None = None) -> int:
    """The longest JSON spelling of a value of C type `t` (dims: a char array's)."""
    t = re.sub(r"\bconst\b|\bvolatile\b|\bstatic\b", "", t).strip()
    base = t.replace(" ", "")
    if dims:
        if base in ("char", "unsignedchar", "uint8_t") and len(dims) == 1:
            n = const_value(idx, dims[0], ns)
            if n is None:
                raise Unbounded(f"char[{dims[0]}]")
            return 2 + CHAR_ESCAPE * max(0, n - 1)
        raise Unbounded(f"an array of {t}")
    if base.endswith("*") or base.endswith("&"):
        raise Unbounded(f"a {t}")
    if base in INT_WIDTHS:
        return INT_WIDTHS[base]
    if base in FLOAT_TYPES:
        return FLOAT_WIDTH
    name = base.split("::")[-1]
    qual = tuple(p for p in base.split("::")[:-1] if p)
    enums = pick(idx.enums.get(name, []), ns, qual)
    if enums:
        widths = {INT_WIDTHS.get(u.replace(" ", "").split("::")[-1], 11) for _ns, u in enums}
        return max(widths)
    raise Unbounded(f"a value of type {t}")


def strip_parens(e: str) -> str:
    e = e.strip()
    while e.startswith("(") and matching_paren(e, 0) == len(e) - 1:
        e = e[1:-1].strip()
    return e


def top_ternary(e: str) -> tuple[str, str] | None:
    depth, q = 0, -1
    quote = None
    for i, c in enumerate(e):
        if quote:
            if c == quote and e[i - 1] != "\\":
                quote = None
            continue
        if c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "?" and depth == 0 and q < 0:
            q = i
        elif c == ":" and depth == 0 and q >= 0 and e[i - 1] != ":" and (i + 1 >= len(e) or e[i + 1] != ":"):
            return e[q + 1:i], e[i + 1:]
    return None


def top_binary(e: str) -> list[str] | None:
    """The operands of a top-level arithmetic, comparison or logical expression."""
    depth, quote, cuts = 0, None, []
    i = 0
    while i < len(e):
        c = e[i]
        if quote:
            if c == quote and e[i - 1] != "\\":
                quote = None
        elif c in "\"'":
            quote = c
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and i > 0:
            two = e[i:i + 2]
            if two in ("&&", "||", "==", "!=", "<=", ">=", "<<", ">>"):
                cuts.append((i, 2))
                i += 2
                continue
            if c in "+-*/%|^<>" and e[i - 1] not in "-<>=!&|" and e[i + 1:i + 2] not in ("=", ">"):
                prev = e[:i].rstrip()
                if prev and prev[-1] not in "(*/%+-<>=!&|^?:,":
                    cuts.append((i, 1))
        i += 1
    if not cuts:
        return None
    out, at = [], 0
    for pos, ln in cuts:
        out.append(e[at:pos])
        at = pos + ln
    out.append(e[at:])
    return out


def bound_value(ctx: Ctx, expr: str, pos: int) -> int:
    """The longest JSON spelling of `expr`, written at file offset `pos` of ctx's function."""
    if ctx.depth > 12:
        raise Unbounded(expr)
    e = strip_parens(expr)
    if not e:
        raise Unbounded("an empty value")
    data = c_literal_bytes(e)
    if data is not None:
        return json_string_len(data)
    if e in ("true", "false"):
        return 5
    if e in ("nullptr", "NULL"):
        return 4
    if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*", e):
        return len(str(int(e.rstrip("uUlL"), 0)))
    if re.fullmatch(r"-?\d+\.\d*(?:[eE][-+]?\d+)?[fF]?", e):
        return FLOAT_WIDTH
    tern = top_ternary(e)
    if tern is not None:
        return max(bound_value(ctx, tern[0], pos), bound_value(ctx, tern[1], pos))
    ops = top_binary(e)
    if ops is not None:
        if re.search(r"&&|\|\||==|!=|<=|>=|(?<![<>-])[<>](?![<>=])", e) and not re.search(r"<<|>>", e):
            return 5                                    # a comparison or a logical: a bool
        widths = [bound_number(ctx, op, pos) for op in ops]
        return max([11] + widths)
    if e.startswith("!"):
        return 5
    if e.startswith("-"):
        return max(11, bound_number(ctx, e[1:], pos))
    cast = re.match(r"^\(\s*((?:const\s+)?(?:unsigned\s+)?[A-Za-z_][\w:]*(?:\s+(?:long|int|short|char))*\s*\**)\s*\)\s*(.+)$",
                    e, re.S)
    if cast is None:
        cast = re.match(r"^static_cast\s*<\s*([^>]+)>\s*\((.*)\)$", e, re.S)
    if cast is not None:
        t = cast.group(1).replace(" ", "")
        if t in ("constchar*", "char*"):
            return bound_value(ctx, cast.group(2), pos)
        if t.rstrip("*") != t:
            raise Unbounded(f"a cast to {cast.group(1)}")
        return type_width(ctx.idx, cast.group(1), ctx.fn.ns)
    call = re.match(r"^([A-Za-z_][\w:]*)\s*\((.*)\)$", e, re.S)
    if call is not None and matching_paren(e, call.end(1) + (len(e[call.end(1):]) - len(e[call.end(1):].lstrip()))) == len(e) - 1:
        return bound_call(ctx, call.group(1), call.group(2), pos)
    member = re.match(r"^([A-Za-z_]\w*)\s*(\[[^\]]*\])?\s*(\.|->)\s*([A-Za-z_]\w*)$", e)
    if member is not None:
        st = struct_of_var(ctx, member.group(1), pos)
        fname = member.group(4)
        if fname not in st.fields:
            raise Unbounded(f"{e}: no field {fname} in {st.name}")
        t, dims = st.fields[fname]
        if t.replace(" ", "") in ("constchar*", "char*") and not dims:
            return bound_table_field(ctx.idx, st, fname)
        return type_width(ctx.idx, t, st.ns, dims)
    index = re.match(r"^([A-Za-z_]\w*)\s*\[[^\]]*\]$", e)
    if index is not None:
        return bound_literal_table(ctx, index.group(1), pos)
    if re.fullmatch(r"[A-Za-z_]\w*", e):
        return bound_name(ctx, e, pos)
    if re.fullmatch(r"[A-Za-z_]\w*\s*\[\s*\"\w+\"\s*\]\s*\.as<bool>\(\)", e):
        return 5
    raise Unbounded(e)


def bound_number(ctx: Ctx, expr: str, pos: int) -> int:
    """An operand of arithmetic: its width, which must be a number's."""
    e = strip_parens(expr)
    if c_literal_bytes(e) is not None:
        raise Unbounded(f"a string in arithmetic: {e}")
    if e in ("nullptr", "NULL"):
        raise Unbounded(e)
    w = bound_value(ctx, e, pos)
    var = re.fullmatch(r"[A-Za-z_]\w*", e)
    if var is not None:
        t = decl_type(ctx, e, pos)
        if t is not None and (t[0].rstrip().endswith("*") or t[1]):
            raise Unbounded(f"a pointer or array in arithmetic: {e}")
    return w


def bound_call(ctx: Ctx, name: str, args: str, pos: int) -> int:
    parts = name.split("::")
    fname, qual = parts[-1], tuple(p for p in parts[:-1] if p)
    if not qual and fname in BUILTIN_WIDTHS:
        return BUILTIN_WIDTHS[fname]
    cands = pick(ctx.idx.fns.get(fname, []), ctx.fn.ns, qual)
    if not cands:
        raise Unbounded(f"{name}(): no declaration found")
    rets = {c.ret.replace(" ", "") for c in cands}
    if all(r in ("constchar*", "char*") for r in rets):
        defs = [c for c in cands if c.body is not None]
        if not defs:
            raise Unbounded(f"{name}(): returns a string and has no body to read")
        return max(bound_returns(ctx, d) for d in defs)
    widths = []
    for c in cands:
        widths.append(type_width(ctx.idx, c.ret, c.ns))
    return max(widths)


def bound_returns(ctx: Ctx, fn: Fn) -> int:
    """The longest string a `const char*` function returns."""
    kept = blank_comments_only(ctx.idx.files[fn.file])
    code = blank_comments_and_strings(ctx.idx.files[fn.file])
    sub = Ctx(ctx.idx, fn.file, fn, kept, code, ctx.depth + 1)
    best = None
    for m in re.finditer(r"\breturn\b", code[fn.body[0]:fn.body[1]]):
        start = fn.body[0] + m.end()
        end = statement_end(kept, start)
        w = bound_value(sub, kept[start:end], start)
        best = w if best is None else max(best, w)
    if best is None:
        raise Unbounded(f"{fn.name}() returns nothing the check can read")
    return best


def block_end(code: str, lo: int, hi: int, p: int) -> int:
    """The `}` closing the innermost block around offset `p` of a function body [lo, hi), or hi."""
    depth = 0
    for j in range(p - 1, lo - 1, -1):
        c = code[j]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                end = close_brace(code, j)
                return end if 0 <= end < hi else hi
            depth -= 1
    return hi


def local_decl(ctx: Ctx, name: str, pos: int) -> tuple[str, list[str]] | None:
    """The declaration of `name` in ctx's function that is in scope at `pos` (the innermost)."""
    lo, hi = ctx.fn.body
    region = ctx.code[lo:pos]
    pat = re.compile(r"(?:^|[;{}(,])\s*(?:static\s+)?(?P<const>const\s+)?(?P<type>(?:unsigned\s+)?[A-Za-z_][\w:]*"
                     r"(?:\s*<[^;(){}]*?>)?)\s*(?P<ptr>[*&]*)\s*\b" + re.escape(name) +
                     r"\b\s*(?P<dims>(?:\[[^\]]*\]\s*)*)\s*(?:=|;|\{|\)|,)", re.M)
    found = None
    for m in pat.finditer(region):
        t = m.group("type")
        if t in KEYWORDS or t in ("return", "else"):
            continue
        # A declaration in a block that closed before `pos` is another
        # variable of the same name, out of scope here (`{ char buf[768]; }`
        # beside the 64-byte buf the answer is written into).
        if block_end(ctx.code, lo, hi, lo + m.start("type")) < pos:
            continue
        found = (("const " if m.group("const") else "") + t + m.group("ptr"),
                 re.findall(r"\[([^\]]*)\]", ctx.kept[lo + m.start("dims"):lo + m.end("dims")]))
    return found


def decl_type(ctx: Ctx, name: str, pos: int) -> tuple[str, list[str]] | None:
    """The declared type (and array dims) of `name` in scope at `pos` of ctx's function, or its parameter."""
    found = local_decl(ctx, name, pos)
    if found is not None:
        return found
    for p in split_top(ctx.fn.params):
        m = re.match(r"^(?P<const>const\s+)?(?P<type>(?:unsigned\s+)?[A-Za-z_][\w:]*(?:\s*<[^>]*>)?)\s*(?P<ptr>[*&]*)\s*"
                     + re.escape(name) + r"\s*(?P<dims>(?:\[[^\]]*\])*)\s*(?:=.*)?$", p.strip(), re.S)
        if m:
            return (("const " if m.group("const") else "") + m.group("type") + m.group("ptr"),
                    re.findall(r"\[([^\]]*)\]", m.group("dims")))
    return None


def struct_of_var(ctx: Ctx, var: str, pos: int) -> Struct:
    t = decl_type(ctx, var, pos)
    if t is None:
        raise Unbounded(f"{var}: no declaration found")
    base = re.sub(r"\bconst\b|[*&]|\bstruct\b", "", t[0]).strip()
    parts = base.split("::")
    name, qual = parts[-1], tuple(p for p in parts[:-1] if p)
    cands = pick(ctx.idx.structs.get(name, []), ctx.fn.ns, qual)
    if len(cands) != 1:
        raise Unbounded(f"{var}: {len(cands)} definitions of {base}")
    return cands[0]


def table_field_bound(idx: Index, struct_name: str, fields: tuple[str, ...], fname: str) -> int:
    i = fields.index(fname)
    lens = []
    for file, src in idx.files.items():
        code = blank_comments_and_strings(src)
        if not re.search(r"\b" + re.escape(struct_name) + r"\b", code):
            continue                    # a file that cannot name the struct cannot fill or write it
        kept = blank_comments_only(src)
        if re.search(r"(?:\.|->)\s*" + re.escape(fname) + r"\s*=(?!=)", code):
            raise Unbounded(f"{struct_name}::{fname}: written outside a table in {file.rsplit('/', 1)[-1]}")
        for m in re.finditer(r"\b" + re.escape(struct_name) + r"\s+\w+\s*\[[^\]]*\]\s*=\s*\{", code):
            start = m.end() - 1
            end = close_brace(code, start)
            for row in split_top(kept[start + 1:end]):
                if not row:
                    continue
                if not (row.startswith("{") and row.endswith("}")) or re.search(r"(?:^|[{,])\s*\.", row):
                    raise Unbounded(f"{struct_name} table row {row[:40]}: not a positional initializer")
                items = split_top(row[1:-1])
                data = c_literal_bytes(items[i]) if i < len(items) else None
                if data is None:
                    raise Unbounded(f"{struct_name}::{fname}: {items[i] if i < len(items) else '(none)'} "
                                    "is not a string literal")
                lens.append(json_string_len(data))
    if not lens:
        raise Unbounded(f"{struct_name}::{fname}: a const char* no table of literals fills")
    return max(lens)


def bound_table_field(idx: Index, st: Struct, fname: str) -> int:
    """A `const char*` field every value of which a static table of positional literals gives
    (computed once per index: each mutation of the self-test builds its own)."""
    key = (st.name, fname)
    if key not in idx.table_fields:
        idx.table_fields[key] = table_field_bound(idx, st.name, tuple(st.fields), fname)
    return idx.table_fields[key]


def bound_literal_table(ctx: Ctx, name: str, pos: int) -> int:
    """`names[i]` of a local `const char* names[] = {"a", "b"};`: its longest string."""
    lo = ctx.fn.body[0]
    m = None
    for m in re.finditer(r"\bconst\s+char\s*\*\s*(?:const\s+)?" + re.escape(name) + r"\s*\[\s*\d*\s*\]\s*=\s*\{",
                         ctx.code[lo:pos]):
        pass
    if m is None:
        raise Unbounded(f"{name}[...]: not a table of string literals")
    start = lo + m.end() - 1
    end = close_brace(ctx.code, start)
    items = split_top(ctx.kept[start + 1:end])
    lens = []
    for it in items:
        if not it:
            continue
        data = c_literal_bytes(it)
        if data is None:
            raise Unbounded(f"{name}[...]: {it} is not a string literal")
        lens.append(json_string_len(data))
    return max(lens)


def bound_name(ctx: Ctx, name: str, pos: int) -> int:
    t = decl_type(ctx, name, pos)
    if t is None:
        raise Unbounded(f"{name}: no declaration found")
    typ, dims = t
    base = typ.replace(" ", "")
    if base == "char*" and not dims:
        raise Unbounded(f"{name}: a char* whose bytes anything may write")
    if base == "constchar*" and not dims:
        # Every value it holds before `pos`: each assignment in the function
        # (its initializer, one in an if, an else or a case alike), and a
        # parameter's value at every call too.
        lo = ctx.fn.body[0]
        region = ctx.code[lo:pos]
        esc = re.escape(name)
        other = re.search(r"(?<![\w.>:&])&\s*" + esc + r"\b|(?<![\w.>:])" + esc + r"\s*(?:\[[^\]]*\]\s*)?"
                          r"(?:[-+*/%&|^]|<<|>>)=|(?:\+\+|--)\s*" + esc + r"\b|(?<![\w.>:])" + esc + r"\s*(?:\+\+|--)"
                          r"|(?<![\w.>:])" + esc + r"\s*\[[^\]]*\]\s*=(?!=)", region)
        if other:
            raise Unbounded(f"{name}: changed in a way the check does not follow ({other.group(0).strip()})")
        vals = []
        for a in re.finditer(r"(?<![\w.>:])" + esc + r"\s*=(?!=)", region):
            start = lo + a.end()
            vals.append(bound_value(ctx, ctx.kept[start:statement_end(ctx.kept, start)], start))
        if local_decl(ctx, name, pos) is None:
            vals.append(bound_param(ctx, name))
        elif not vals:
            raise Unbounded(f"{name}: a local given no value the check can see")
        return max(vals)
    return type_width(ctx.idx, typ, ctx.fn.ns, dims)


def bound_param(ctx: Ctx, name: str) -> int:
    """A `const char*` parameter of a helper: the longest it is passed, at every call in its file."""
    params = split_top(ctx.fn.params)
    at = [i for i, p in enumerate(params) if re.search(r"\b" + re.escape(name) + r"\b", p)]
    if len(at) != 1:
        raise Unbounded(f"{name}: not a parameter of {ctx.fn.name}()")
    i = at[0]
    default = re.search(r"=\s*(.+)$", params[i])
    widths = []
    if default:
        widths.append(4 if default.group(1).strip() in ("nullptr", "NULL") else None)
    callers = 0
    for m in re.finditer(r"(?<![\w:.>])" + re.escape(ctx.fn.name) + r"\s*\(", ctx.code):
        if ctx.fn.body and ctx.fn.body[0] - 400 <= m.start() <= ctx.fn.body[0]:
            continue                    # the definition's own header
        open_at = m.end() - 1
        close = matching_paren(ctx.code, open_at)
        args = split_top(ctx.kept[open_at + 1:close])
        caller = next((f for fs in ctx.idx.fns.values() for f in fs
                       if f.file == ctx.file and f.body and f.body[0] <= m.start() < f.body[1]), None)
        if caller is None:
            continue
        callers += 1
        sub = Ctx(ctx.idx, ctx.file, caller, ctx.kept, ctx.code, ctx.depth + 1)
        if i < len(args):
            widths.append(bound_value(sub, args[i], open_at + 1))
        elif default:
            widths.append(4)
        else:
            raise Unbounded(f"{ctx.fn.name}(): a call without its {name}")
    if None in widths:
        raise Unbounded(f"{name}: a default the check cannot read")
    if not callers:
        raise Unbounded(f"{ctx.fn.name}(): no call in {ctx.file} to measure {name} at")
    return max(widths)


# ── Bounding a document ──────────────────────────────────────────────────

def loop_spans(code: str, lo: int, hi: int) -> list[tuple[int, int]]:
    spans = []
    for m in re.finditer(r"\b(?:for|while)\s*\(", code[lo:hi]):
        p_open = lo + m.end() - 1
        p_close = matching_paren(code, p_open)
        nxt = re.match(r"\s*\{", code[p_close + 1:])
        if nxt:
            b = p_close + 1 + nxt.end() - 1
            spans.append((p_close, close_brace(code, b)))
        else:
            spans.append((p_close, statement_end(code, p_close + 1)))
    for m in re.finditer(r"\bdo\s*\{", code[lo:hi]):
        b = lo + m.end() - 1
        spans.append((b, close_brace(code, b)))
    return spans


def bound_document(ctx: Ctx, doc: str, upto: int) -> int:
    """The longest answer `doc` (a local JsonDocument) serializes to, from its statements before `upto`."""
    lo, hi = ctx.fn.body[0], upto
    code, kept = ctx.code, ctx.kept
    if not re.search(r"\b(?:JsonDocument|DynamicJsonDocument|StaticJsonDocument\s*<[^>]*>)\s+" + re.escape(doc)
                     + r"\b", code[lo:hi]):
        raise Unbounded(f"{doc}: not a JsonDocument this function declares")
    loops = loop_spans(code, lo, hi)

    def in_loop(p: int) -> bool:
        return any(s <= p <= e for s, e in loops)

    # What each name stands for in the document: a path of keys, and its kind.
    nodes: dict[str, tuple[tuple[str, ...], str]] = {doc: ((), "object")}
    handed = []
    for m in re.finditer(r"\bJson(Object|Array)\s+(\w+)\s*=\s*(\w+)\s*\[\s*\"(\w+)\"\s*\]\s*\.\s*to\s*<\s*Json(Object|Array)\s*>\s*\(\s*\)\s*;",
                         kept[lo:hi]):
        parent = m.group(3)
        if parent not in nodes:
            raise Unbounded(f"{m.group(2)}: from {parent}, which the check does not follow")
        if m.group(1) != m.group(5):
            raise Unbounded(f"{m.group(2)}: a Json{m.group(1)} from to<Json{m.group(5)}>()")
        nodes[m.group(2)] = (nodes[parent][0] + (m.group(4),), m.group(1).lower())
        handed.append(m)
    objects: dict[tuple[str, ...], dict[str, list[int]]] = {(): {}}
    arrays: dict[tuple[str, ...], list[int]] = {}
    for name, (path, kind) in nodes.items():
        if kind == "object":
            objects.setdefault(path, {})
        else:
            arrays.setdefault(path, [])
        if path:
            objects.setdefault(path[:-1], {}).setdefault(path[-1], [])
    names = "|".join(re.escape(n) for n in nodes)
    # Every other use of a name that stands for part of the document: only
    # its declaration, a key or a method, and the document's own measure.
    for m in re.finditer(r"(?<![\w.>:])(" + names + r")\b", code[lo:hi]):
        p = lo + m.start()
        after = code[lo + m.end():lo + m.end() + 2].lstrip()[:1]
        if after in ("[", "."):
            continue
        decl = re.search(r"\b(?:JsonDocument|DynamicJsonDocument|StaticJsonDocument\s*<[^>]*>|JsonObject|JsonArray)"
                         r"\s+$", code[max(lo, p - 40):p])
        if decl:
            continue
        if re.search(r"\b(?:measureJson|serializeJson)\s*\(\s*$", code[max(lo, p - 20):p]):
            continue                                # measured, or written out (a doc of the same name)
        raise Unbounded(f"{m.group(1)}: handed to something the check does not follow")
    for m in re.finditer(r"(?<![\w.>])(" + names + r")\b\s*(\[|\.)", code[lo:hi]):
        p = lo + m.start()
        name = m.group(1)
        path, kind = nodes[name]
        after = lo + m.end(2) - 1
        if m.group(2) == "[":
            key = re.match(r"\[\s*\"(\w+)\"\s*\]\s*", kept[after:])
            if key is None:
                raise Unbounded(f"{name}[...]: a key that is not a literal")
            rest = after + key.end()
            if re.match(r"\.\s*to\s*<", kept[rest:]):
                if not any(lo + a.start() <= p < lo + a.end() for a in handed):
                    raise Unbounded(f"{name}[\"{key.group(1)}\"].to<>(): an alias the check does not follow")
                continue                                # handed out above
            if re.match(r"\.\s*(?:as|is)\s*<", kept[rest:]):
                continue                                # a read
            assign = re.match(r"=(?!=)", kept[rest:])
            if assign is None:
                raise Unbounded(f"{name}[\"{key.group(1)}\"]: a use the check does not follow")
            if kind != "object":
                raise Unbounded(f"{name}: an array indexed by a key")
            if in_loop(p):
                raise Unbounded(f"{name}[\"{key.group(1)}\"]: set in a loop")
            start = rest + assign.end()
            end = statement_end(kept, start)
            objects[path].setdefault(key.group(1), []).append(bound_value(ctx, kept[start:end], start))
        else:
            meth = re.match(r"\.\s*(\w+)", kept[after:])
            if meth is None:
                raise Unbounded(f"{name}.: a use the check does not follow")
            if meth.group(1) == "add" and kind == "array":
                if in_loop(p):
                    raise Unbounded(f"{name}.add(): an array built in a loop")
                open_at = kept.find("(", after)
                close = matching_paren(kept, open_at)
                arrays[path].append(bound_value(ctx, kept[open_at + 1:close], open_at + 1))
            else:
                raise Unbounded(f"{name}.{meth.group(1)}(): a use the check does not follow")

    def size_of(path: tuple[str, ...], kind: str) -> int:
        if kind == "array":
            items = arrays.get(path, [])
            return 2 + sum(items) + max(0, len(items) - 1)
        keys = objects.get(path, {})
        total = 2 + max(0, len(keys) - 1)
        for key, vals in keys.items():
            sub = path + (key,)
            if sub in objects and not vals:
                width = size_of(sub, "object")
            elif sub in arrays and not vals:
                width = size_of(sub, "array")
            elif vals:
                width = max(vals)
            else:
                raise Unbounded(f"{doc}: key {key} with no value")
            total += json_string_len(key.encode()) + 1 + width
        return total
    return size_of((), "object")


# ── J1-J4 ────────────────────────────────────────────────────────────────

@dataclass
class Site:
    file: str
    fn: str
    line: int
    target: str
    verdict: str          # "grows", "measured", "guarded", "fits", or an error
    bound: int | None = None
    size: int | None = None


def fn_containing(idx: Index, file: str, pos: int) -> Fn | None:
    best = None
    for fs in idx.fns.values():
        for f in fs:
            if f.file == file and f.body and f.body[0] <= pos < f.body[1]:
                if best is None or f.body[0] > best.body[0]:
                    best = f
    return best


def body_always_returns(body: str) -> bool:
    """An if-body (braces stripped, or one statement) whose last statement is a `return` it always
    reaches: nothing in it jumps out another way."""
    if re.search(r"\b(?:break|continue|goto)\b", body):
        return False
    text = body.strip()
    if not text.endswith(";"):
        return False
    depth, last = 0, 0
    for i, c in enumerate(text[:-1]):
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0 and c == "}":
                last = i + 1
        elif c == ";" and depth == 0:
            last = i + 1
    return re.match(r"\s*return\b", text[last:]) is not None


def if_statement(code: str, at: int) -> tuple[str, str, int] | None:
    """`if (cond) body` at `at`: (cond, body, offset past it), or None when it has an `else`."""
    p_open = code.find("(", at)
    p_close = matching_paren(code, p_open)
    j = p_close + 1
    while j < len(code) and code[j].isspace():
        j += 1
    if code[j] == "{":
        end = close_brace(code, j)
        body, nxt = code[j + 1:end], end + 1
    else:
        end = statement_end(code, j)
        body, nxt = code[j:end + 1], end + 1
    if re.match(r"\s*else\b", code[nxt:]):
        return None
    return code[p_open + 1:p_close], body, nxt


def nothing_but_alloc(code: str, kept: str, start: int, end: int, target: str, size: str) -> str | None:
    """What stands between a `size = measureJson(doc) + 1;` and its serializeJson(): only the
    allocation of `target` from `size` (once), constants, and `if`s on the size or the
    allocation whose bodies return. Anything else may grow the document (or change the size)
    after it was measured: the answer would fill its measured buffer, unterminated. Returns
    what does not belong (from `kept`, the same text with its literals), or None."""
    t, n = re.escape(target), re.escape(size)
    alloc = re.compile(r"(?:char\s*\*\s*)?" + t + r"\s*=\s*(?:\(\s*char\s*\*\s*\)\s*)?"
                       r"(?:malloc\s*\(\s*" + n + r"\s*\)|calloc\s*\(\s*1\s*,\s*" + n + r"\s*\))\s*$")
    const = re.compile(r"(?:static\s+)?const(?:expr)?\s+(?:size_t|int|unsigned|uint32_t)\s+\w+\s*=\s*\d+[uUlL]*\s*$")
    cond = re.compile(r"\s*(?:!\s*" + t + r"|" + t + r"\s*==\s*(?:nullptr|NULL)|(?:nullptr|NULL)\s*==\s*" + t
                      + r"|" + n + r"\s*>=?\s*(?:[A-Za-z_]\w*|\d+[uUlL]*))\s*$")
    allocs, i = 0, start
    while True:
        while i < end and code[i].isspace():
            i += 1
        if i >= end:
            break
        if re.match(r"if\s*\(", code[i:end]):
            parsed = if_statement(code, i)
            if parsed is None or parsed[2] > end:
                return kept[i:end].strip()[:60]
            c, body, nxt = parsed
            if not cond.match(c) or not body_always_returns(body):
                return kept[i:nxt].strip()[:60]
            i = nxt
            continue
        e = statement_end(code, i)
        if e < 0 or e >= end:
            return kept[i:end].strip()[:60]
        stmt = code[i:e]
        if alloc.match(stmt):
            allocs += 1
        elif not const.match(stmt):
            return kept[i:e].strip()[:60]
        i = e + 1
    return None if allocs == 1 else f"{allocs} allocations of {target} from {size}"


def judge_site(ctx: Ctx, call_at: int) -> Site:
    kept, code = ctx.kept, ctx.code
    open_at = code.find("(", call_at)
    close = matching_paren(code, open_at)
    args = split_top(kept[open_at + 1:close])
    line = kept.count("\n", 0, call_at) + 1
    site = Site(ctx.file, ctx.fn.name, line, args[1] if len(args) > 1 else "?", "")
    if len(args) not in (2, 3) or not re.fullmatch(r"\w+", args[0]) or not re.fullmatch(r"\w+", args[1]):
        site.verdict = f"serializeJson({', '.join(args)}): a form the check does not follow (J3)"
        return site
    doc, target = args[0], args[1]
    t = decl_type(ctx, target, call_at)
    if t is None:
        site.verdict = f"{target}: no declaration found (J3)"
        return site
    typ, dims = t
    base = re.sub(r"\bconst\b", "", typ).replace(" ", "")
    if base in ("String", "std::string", "File", "fs::File") and len(args) == 2:
        site.verdict = "grows"
        return site
    lo = ctx.fn.body[0]
    before = kept[lo:call_at]
    size_expr = None
    if base == "char" and len(dims) == 1:
        size_expr = dims[0]
        guards = list(re.finditer(r"if\s*\(\s*measureJson\s*\(\s*" + re.escape(doc) + r"\s*\)\s*>=\s*sizeof\s*\(\s*"
                                  + re.escape(target) + r"\s*\)\s*\)", before))
        if guards and len(args) == 3 and re.fullmatch(r"sizeof\s*\(\s*" + re.escape(target) + r"\s*\)", args[2]):
            # The refusal must refuse (its body returns) and be the last
            # thing before the serialize: a document written after it is
            # an answer it never measured.
            parsed = if_statement(code, lo + guards[-1].start())
            if parsed is None or not body_always_returns(parsed[1]):
                site.verdict = (f"{target}: the measureJson() refusal before it does not return (an else, or a "
                                "body that falls through to the serialize) (J1)")
            elif code[parsed[2]:call_at].strip():
                site.verdict = (f"{target}: something stands between the measureJson() refusal and the "
                                f"serialize ({kept[parsed[2]:call_at].strip()[:60]}) (J1)")
            else:
                site.verdict = "guarded"
            return site
    elif base == "char*":
        alloc = list(re.finditer(r"\b" + re.escape(target) + r"\s*=\s*(?:\(\s*char\s*\*\s*\)\s*)?"
                                 r"(?:malloc\s*\(\s*([^()]+?)\s*\)|calloc\s*\(\s*1\s*,\s*([^()]+?)\s*\))", before))
        if not alloc:
            site.verdict = f"{target}: a char* the check cannot size (J3)"
            return site
        size_expr = (alloc[-1].group(1) or alloc[-1].group(2)).strip()
        measured = list(re.finditer(r"\b" + re.escape(size_expr) + r"\s*=\s*measureJson\s*\(\s*" + re.escape(doc)
                                    + r"\s*\)\s*\+\s*1\s*;", before)) if re.fullmatch(r"\w+", size_expr) else []
        if measured and len(args) == 3 and args[2] == size_expr:
            stray = nothing_but_alloc(code, kept, lo + measured[-1].end(), call_at, target, size_expr)
            site.verdict = "measured" if stray is None else (
                f"{target}: between its measureJson() and the serialize stands {stray!r}; only the allocation "
                "from the measured size, a constant, and an if on the size or the allocation that returns "
                "may (J1)")
            return site
        if len(args) != 3:
            site.verdict = f"{target}: a heap buffer serialized without its size (J3)"
            return site
    else:
        site.verdict = f"{target}: a {typ} the check cannot size (J3)"
        return site
    size = const_value(ctx.idx, size_expr, ctx.fn.ns)
    if len(args) == 3:
        third = const_value(ctx.idx, args[2], ctx.fn.ns)
        if args[2].replace(" ", "") != f"sizeof({target})" and third != size:
            site.verdict = f"{target}: serialized with a size ({args[2]}) that is not the buffer's (J2)"
            return site
    if size is None:
        site.verdict = f"{target}: a buffer of {size_expr} bytes, a size the check cannot read (J3)"
        return site
    site.size = size
    try:
        site.bound = bound_document(ctx, doc, call_at)
    except Unbounded as why:
        site.verdict = (f"{doc} into the {size}-byte {target}: cannot bound the answer ({why}) — serialize "
                        "it to measureJson()'s length (J3)")
        return site
    if site.bound >= size:
        site.verdict = (f"{doc} into the {size}-byte {target}: its longest answer is {site.bound} bytes, so it "
                        "goes out cut and unterminated, with whatever memory follows the buffer; size the "
                        f"buffer past {site.bound} or serialize to measureJson()'s length (J2)")
        return site
    site.verdict = "fits"
    return site


def check_fleet_cache(files: dict[str, str], idx: Index) -> list[str]:
    """J4: fleet_scan_task() writes its staging buffer only through fleet_scan_cache.h, at the cache's
    size, and copies all of it into the cache."""
    where = f"{INO}: {FLEET_TASK}()"
    fns = [f for f in idx.fns.get(FLEET_TASK, []) if f.file == INO and f.body]
    if len(fns) != 1:
        return [f"{where}: {len(fns)} definitions; the fleet scan's cache writer cannot be read (J4)"]
    fn = fns[0]
    code = blank_comments_and_strings(files[INO])
    kept = blank_comments_only(files[INO])
    body = code[fn.body[0]:fn.body[1]]
    errors = []
    alloc = re.findall(r"\bchar\s*\*\s*(\w+)\s*=\s*\(\s*char\s*\*\s*\)\s*(?:calloc\s*\(\s*1\s*,|malloc\s*\()\s*"
                       + FLEET_SIZE + r"\s*\)\s*;", body)
    if len(alloc) != 1:
        return [f"{where}: no one staging buffer of {FLEET_SIZE} bytes (J4)"]
    buf = alloc[0]
    begins = re.findall(r"\bfleet_scan_cache\s*::\s*begin\s*\(\s*(\w+)\s*,\s*(\w+)\s*,\s*([^;]*?)\s*\)\s*;", body)
    if len(begins) != 1:
        errors.append(f"{where}: fleet_scan_cache::begin() {len(begins)} times; the cache is begun once (J4)")
    else:
        cache, target, size = begins[0]
        if target != buf or size != FLEET_SIZE:
            errors.append(f"{where}: fleet_scan_cache::begin({cache}, {target}, {size}) is not over {buf} with "
                          f"{FLEET_SIZE} bytes (J4)")
        adds = re.findall(r"\bfleet_scan_cache\s*::\s*add\s*\(\s*(\w+)\s*,", body)
        if not adds or any(a != cache for a in adds):
            errors.append(f"{where}: the adverts are not added to {cache} with fleet_scan_cache::add() (J4)")
    if not re.search(r"\bmemcpy\s*\(\s*g_fleet_scan_cache\s*,\s*" + re.escape(buf) + r"\s*,\s*" + FLEET_SIZE
                     + r"\s*\)", body):
        errors.append(f"{where}: {buf} is not copied whole ({FLEET_SIZE} bytes) into g_fleet_scan_cache (J4)")
    # Every use of the staging buffer: the allocation, the test of it, the
    # writer's begin, the copy and the free. Anything else writes around the
    # builder (a serializeJson(), a hand-placed NUL, a strcpy()).
    allowed = [
        r"char\s*\*\s*" + buf + r"\s*=",
        r"if\s*\(\s*" + buf + r"\s*&&",
        r"fleet_scan_cache\s*::\s*begin\s*\(\s*\w+\s*,\s*" + buf + r"\s*,",
        r"memcpy\s*\(\s*g_fleet_scan_cache\s*,\s*" + buf + r"\s*,",
        r"free\s*\(\s*" + buf + r"\s*\)",
    ]
    for m in re.finditer(r"(?<![\w.>])" + re.escape(buf) + r"\b", body):
        at = m.start()
        start = max(body.rfind(c, 0, at) for c in ";{}") + 1     # the statement around this use
        end = statement_end(body, at)
        stmt = body[start:end if end >= 0 else len(body)]
        if any(a.start() <= at - start < a.end() for pat in allowed for a in re.finditer(pat, stmt)):
            continue
        line = code.count("\n", 0, fn.body[0] + at) + 1
        text = kept[fn.body[0] + start:fn.body[0] + at + len(buf) + 30].strip()
        errors.append(f"{INO}:{line}: {FLEET_TASK}(): {buf} is written around fleet_scan_cache.h "
                      f"({text[:70]!r}); the cache holds the adverts that fit, never a cut list (J4)")
    return errors


def check_files(files: dict[str, str]) -> tuple[list[str], list[Site]]:
    idx = build_index(files)
    errors: list[str] = []
    sites: list[Site] = []
    for name in EXCLUDED:
        if name not in files:
            errors.append(f"{name}: EXCLUDED names a file the sketch no longer has")
    errors += check_fleet_cache(files, idx)
    for name in scope_of(files):
        src = files[name]
        code = blank_comments_and_strings(src)
        kept = blank_comments_only(src)
        for m in re.finditer(r"(?<![\w:.>])serializeJson\s*\(", code):
            fn = fn_containing(idx, name, m.start())
            if fn is None:
                errors.append(f"{name}:{code.count(chr(10), 0, m.start()) + 1}: serializeJson( outside a function")
                continue
            site = judge_site(Ctx(idx, name, fn, kept, code), m.start())
            sites.append(site)
            if site.verdict not in ("grows", "measured", "guarded", "fits"):
                errors.append(f"{name}:{site.line}: {fn.name}(): {site.verdict}")
    return errors, sites


# ── Self-test: mutations the check must refuse ───────────────────────────

Mutation = Callable[[dict[str, str]], dict[str, str]]


def raw_in(path: str, old: str, new: str) -> Mutation:
    def mutate(files: dict[str, str]) -> dict[str, str]:
        if files[path].count(old) != 1:
            raise AnchorMissing(old)
        out = dict(files)
        out[path] = files[path].replace(old, new, 1)
        return out
    return mutate


CHIRP_API = f"{SKETCH}/chirp_api.h"
MESH_H = f"{SKETCH}/mesh_network.h"
HOUSEHOLD = f"{SKETCH}/household_api.h"
RF = f"{SKETCH}/rf_presence_api.h"
BEACON = f"{SKETCH}/beacon_api.h"

def both(first: Mutation, second: Mutation) -> Mutation:
    def mutate(files: dict[str, str]) -> dict[str, str]:
        return second(first(files))
    return mutate


def add_file(path: str, text: str) -> Mutation:
    def mutate(files: dict[str, str]) -> dict[str, str]:
        if path in files:
            raise AnchorMissing(path)
        out = dict(files)
        out[path] = text
        return out
    return mutate


LONG = "x" * 150

MUTATIONS: list[tuple[str, Mutation]] = [
    # J2: a fixed buffer under its longest answer.
    ("the Chirp not-run answer goes back to a 64-byte buffer",
     raw_in(CHIRP_API, "  char buffer[160];\n  serializeJson(doc, buffer);\n  httpd_resp_set_type(req, \"application/json\");\n"
            "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n  return httpd_resp_sendstr(req, buffer);\n}\n\n"
            "// A confirm the loop task ran",
            "  char buffer[64];\n  serializeJson(doc, buffer);\n  httpd_resp_set_type(req, \"application/json\");\n"
            "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n  return httpd_resp_sendstr(req, buffer);\n}\n\n"
            "// A confirm the loop task ran")),
    ("the mute answer's buffer holds its longest answer and no terminator (100 of 100)",
     raw_in(CHIRP_API, "    doc[\"message\"] = chirp_channel::mute_refusal_message(r.mute_refusal);\n  }\n  char buffer[160];",
            "    doc[\"message\"] = chirp_channel::mute_refusal_message(r.mute_refusal);\n  }\n  char buffer[100];")),
    ("the enable answer goes back to its 128-byte buffer",
     raw_in(CHIRP_API, "  char buffer[160];\n  serializeJson(doc, buffer);\n\n  httpd_resp_set_type(req, \"application/json\");\n"
            "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n  return httpd_resp_sendstr(req, buffer);\n}\n\n"
            "// POST /api/chirp/disable",
            "  char buffer[128];\n  serializeJson(doc, buffer);\n\n  httpd_resp_set_type(req, \"application/json\");\n"
            "  httpd_resp_set_hdr(req, \"Access-Control-Allow-Origin\", \"*\");\n  return httpd_resp_sendstr(req, buffer);\n}\n\n"
            "// POST /api/chirp/disable")),
    ("the presence status answer gets a 256-byte buffer",
     raw_in(HOUSEHOLD, "  char buf[640];", "  char buf[256];")),
    # J2: the bound is computed: a longer message, a new key, a request's bytes.
    ("a dismiss whose vote stayed home says 150 bytes more",
     raw_in(MESH_H, 'return "Dismissed on this device only: the suppress vote could not be signed";',
            f'return "Dismissed on this device only: the suppress vote could not be signed {LONG}";')),
    ("the RF settings answer gains a key past its buffer",
     raw_in(RF, '  doc["emit_narrative_hints"] = settings.emit_narrative_hints;',
            '  doc["emit_narrative_hints"] = settings.emit_narrative_hints;\n'
            f'  doc["note"] = "{LONG}";')),
    ("a household error answers bytes of the request",
     raw_in(HOUSEHOLD, '    return send_err(req, "slot empty or role invalid");', "    return send_err(req, body);")),
    # J3: what the check cannot bound fails.
    ("the nearby route goes back to its 3072-byte stack buffer",
     raw_in(CHIRP_API, "  const size_t needed = measureJson(doc) + 1;\n  char* buffer = (char*)malloc(needed);\n  if (!buffer) {\n"
            "    free(t);\n    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, \"Memory allocation failed\");\n"
            "    return ESP_FAIL;\n  }\n  serializeJson(doc, buffer, needed);\n  free(t);\n\n",
            "  char buffer[3072];\n  serializeJson(doc, buffer);\n  free(t);\n\n")),
    ("the recent route serializes into a 4096-byte heap buffer again",
     raw_in(CHIRP_API, "  char* buffer = (char*)malloc(needed);\n  if (!buffer) {\n    free(t);\n"
            "    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, \"Memory allocation failed\");\n"
            "    return ESP_FAIL;\n  }\n\n  serializeJson(doc, buffer, needed);",
            "  char* buffer = (char*)malloc(4096);\n  if (!buffer) {\n    free(t);\n"
            "    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, \"Memory allocation failed\");\n"
            "    return ESP_FAIL;\n  }\n\n  serializeJson(doc, buffer, 4096);")),
    ("the templates route serializes with a size that is not the measured one",
     raw_in(CHIRP_API, "    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, \"Memory allocation failed\");\n"
            "    return ESP_FAIL;\n  }\n\n  serializeJson(doc, buffer, needed);\n  httpd_resp_set_type",
            "    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, \"Memory allocation failed\");\n"
            "    return ESP_FAIL;\n  }\n\n  serializeJson(doc, buffer, 4096);\n  httpd_resp_set_type")),
    ("the RF status answer goes back to a 512-byte buffer (two const char* fields)",
     raw_in(RF, "  const size_t needed = measureJson(doc) + 1;\n  char* buffer = (char*)malloc(needed);\n"
            "  if (!buffer) return send_error(req, \"out of memory\");\n  serializeJson(doc, buffer, needed);\n"
            "  esp_err_t ret = send_json_response(req, buffer);\n  free(buffer);\n  return ret;",
            "  char buffer[512];\n  serializeJson(doc, buffer);\n  return send_json_response(req, buffer);")),
    ("a heap buffer sized from another document",
     raw_in(RF, "  const size_t needed = measureJson(doc) + 1;\n  char* buffer = (char*)malloc(needed);\n"
            "  if (!buffer) return send_error(req, \"out of memory\");",
            "  JsonDocument other;\n  const size_t needed = measureJson(other) + 1;\n  char* buffer = (char*)malloc(needed);\n"
            "  if (!buffer) return send_error(req, \"out of memory\");")),
    ("the beacon reply drops its measureJson() refusal",
     raw_in(BEACON, "  if (measureJson(doc) >= sizeof(buf)) {\n    return send_json(req, \"{\\\"success\\\":false,\\\"error\\\":\\\"reply_too_long\\\"}\");\n  }\n",
            "")),
    ("the beacon status builds its reasons in a loop",
     raw_in(BEACON, '    reasons.add("time_unsynced");',
            '    for (int k = 0; k < 2; k++) reasons.add("time_unsynced");')),
    ("the presence status hands its document to a helper first",
     raw_in(HOUSEHOLD, "  char buf[640];", "  fill_more(doc);\n  char buf[640];")),
    ("a sketch answer goes into a fixed buffer",
     raw_in(INO, '  doc["message"] = "Rebooting...";\n  \n  String response;',
            '  doc["message"] = "Rebooting...";\n  \n  char response[16];')),
    ("a new route file answers a long message from a short buffer",
     add_file(f"{SKETCH}/zz_new_api.h",
              "namespace zz_api {\ninline esp_err_t handle_zz(httpd_req_t* req) {\n  JsonDocument doc;\n"
              f'  doc["message"] = "{LONG}";\n  char buffer[64];\n  serializeJson(doc, buffer);\n'
              "  return httpd_resp_sendstr(req, buffer);\n}\n}\n")),
    # J4: the fleet scan's cache is written by fleet_scan_cache.h, whole.
    ("the fleet scan's cache goes back to a cut serializeJson()",
     raw_in(INO, "    fleet_scan_cache::begin(cache, staging, FLEET_SCAN_CACHE_SIZE);\n",
            "    fleet_scan_cache::begin(cache, staging, FLEET_SCAN_CACHE_SIZE);\n"
            "    JsonDocument doc;\n    if (serializeJson(doc, staging, FLEET_SCAN_CACHE_SIZE) >= FLEET_SCAN_CACHE_SIZE) {\n"
            "      staging[FLEET_SCAN_CACHE_SIZE - 1] = '\\0';\n    }\n")),
    ("the fleet scan's cache is terminated by hand",
     raw_in(INO, "    portENTER_CRITICAL(&g_fleet_scan_mux);\n    memcpy(g_fleet_scan_cache, staging, FLEET_SCAN_CACHE_SIZE);",
            "    staging[FLEET_SCAN_CACHE_SIZE - 1] = '\\0';\n"
            "    portENTER_CRITICAL(&g_fleet_scan_mux);\n    memcpy(g_fleet_scan_cache, staging, FLEET_SCAN_CACHE_SIZE);")),
    ("the fleet scan's cache is begun past its buffer",
     raw_in(INO, "fleet_scan_cache::begin(cache, staging, FLEET_SCAN_CACHE_SIZE);",
            "fleet_scan_cache::begin(cache, staging, FLEET_SCAN_CACHE_SIZE + 512);")),
    ("an advert is copied into the cache around the builder",
     raw_in(INO, "      fleet_scan_cache::add(cache, advert);",
            "      strcat(staging, name.c_str());")),
    ("the fleet scan's cache is copied short",
     raw_in(INO, "memcpy(g_fleet_scan_cache, staging, FLEET_SCAN_CACHE_SIZE);",
            "memcpy(g_fleet_scan_cache, staging, strlen(staging));")),
    # J2: every value a const char* holds before the answer, wherever it is
    # assigned (the review's P1 and P3: an assignment after `)` or `case 3:`).
    ("the status reason is reassigned on an if's own line",
     raw_in(CHIRP_API, "  const char* why = chirp_channel::cannot_send_reason(v);\n",
            "  const char* why = chirp_channel::cannot_send_reason(v);\n"
            f'  if (v.muted) why = "{LONG * 3}";\n')),
    ("the status reason is reassigned in a one-line case",
     raw_in(CHIRP_API, "  const char* why = chirp_channel::cannot_send_reason(v);\n",
            "  const char* why = chirp_channel::cannot_send_reason(v);\n"
            f'  switch (v.cooldown_tier) {{ case 3: why = "{LONG * 3}"; break; default: break; }}\n')),
    ("the status reason is handed out by address",
     raw_in(CHIRP_API, "  const char* why = chirp_channel::cannot_send_reason(v);\n",
            "  const char* why = chirp_channel::cannot_send_reason(v);\n  pick_reason(v, &why);\n")),
    ("a household error from a local reassigned on an if's line (the review's P6)",
     raw_in(HOUSEHOLD, '    return send_err(req, "slot empty or role invalid");',
            f'    {{ const char* m = "slot empty or role invalid"; if (len) m = "{LONG}"; return send_err(req, m); }}')),
    ("a helper that assigns its own parameter is still measured at its callers",
     both(raw_in(HOUSEHOLD, '  JsonDocument d; d["success"] = false; d["error"] = msg;',
                 '  if (!msg)\n    msg = "none";\n  JsonDocument d; d["success"] = false; d["error"] = msg;'),
          raw_in(HOUSEHOLD, '    return send_err(req, "slot empty or role invalid");',
                 f'    return send_err(req, "{LONG}");'))),
    # J2: the buffer in scope at the serialize, not one of the same name in
    # a block that closed (the review's P7).
    ("the status answer's buffer is shadowed by a closed block's 768 bytes",
     raw_in(CHIRP_API, "  char buffer[768];\n  serializeJson(doc, buffer);\n",
            "  char buffer[64];\n  { char buffer[768]; (void)buffer; }\n  serializeJson(doc, buffer);\n")),
    # J1: nothing writes the document after it was measured (the review's P4
    # and its recent-route twin), and a refusal refuses (P5).
    ("the nearby list gains a key after its measureJson()",
     raw_in(CHIRP_API, "  // (the copy's emoji) by pointer until then (rule CV7).\n  const size_t needed = measureJson(doc) + 1;\n",
            "  // (the copy's emoji) by pointer until then (rule CV7).\n  const size_t needed = measureJson(doc) + 1;\n"
            '  doc["late"] = 1;\n')),
    ("the recent list gains a key after its allocation",
     raw_in(CHIRP_API, "  }\n\n  serializeJson(doc, buffer, needed);\n  free(t);   // after the serialize",
            '  }\n  doc["extra"] = "0123456789";\n  serializeJson(doc, buffer, needed);\n  free(t);   // after the serialize')),
    ("the RF status grows between its measure and its allocation",
     raw_in(RF, "  const size_t needed = measureJson(doc) + 1;\n  char* buffer = (char*)malloc(needed);\n",
            '  const size_t needed = measureJson(doc) + 1;\n  doc["late"] = 1;\n  char* buffer = (char*)malloc(needed);\n')),
    ("the beacon reply's refusal only logs",
     raw_in(BEACON, "  if (measureJson(doc) >= sizeof(buf)) {\n    return send_json(req, \"{\\\"success\\\":false,\\\"error\\\":\\\"reply_too_long\\\"}\");\n  }\n",
            "  if (measureJson(doc) >= sizeof(buf)) {\n    (void)0;\n  }\n")),
    ("the beacon reply is written after its refusal",
     raw_in(BEACON, "  }\n  serializeJson(doc, buf, sizeof(buf));\n  return send_json(req, buf);",
            '  }\n  doc["late"] = 1;\n  serializeJson(doc, buf, sizeof(buf));\n  return send_json(req, buf);')),
]


def self_test(files: dict[str, str]) -> list[str]:
    problems = []
    for name, mutate in MUTATIONS:
        try:
            m = mutate(files)
        except AnchorMissing as missing:
            problems.append(f"self-test: mutation '{name}' no longer applies (anchor {missing}) — the "
                            "source changed shape; update this guard's mutations with it")
            continue
        if m == files:
            problems.append(f"self-test: mutation '{name}' changed nothing")
        elif not check_files(m)[0]:
            problems.append(f"self-test: the check did not bite on mutation '{name}'")
    return problems


def main(argv: list[str]) -> int:
    files = sketch_files()
    errors, sites = check_files(files)
    if "--list" in argv:
        for s in sites:
            extra = f" (longest {s.bound} of {s.size})" if s.bound is not None else ""
            print(f"{s.file.rsplit('/', 1)[-1]}:{s.line} {s.fn}() -> {s.target}: {s.verdict}{extra}")
    for err in errors:
        print(f"::error::{err}")
    problems = [] if "--no-self-test" in argv else self_test(files)
    for p in problems:
        print(f"::error::{p}")
    if errors or problems:
        return 1
    fits = [s for s in sites if s.verdict == "fits"]
    print(f"canary-wap REST answers fit their buffers: {len(sites)} serializeJson() calls in "
          f"{len(scope_of(files))} files, {len(fits)} fixed buffers measured against their longest "
          f"answer, the rest grow or are sized by measureJson() ({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
