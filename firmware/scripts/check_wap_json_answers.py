#!/usr/bin/env python3
"""Every canary-wap REST answer fits the buffer it is serialized into (sweep F196),
the fleet scan's cache keeps the adverts that fit (F211), the answers spelled
with snprintf() are measured and escaped (F212), every other JSON text spelled
with printf is held or recorded (F227), and the device name holds no byte
ArduinoJson would write raw (F226).

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

The same answer files also spelled answers with `snprintf()`, which
terminates but cuts an answer that does not fit (unparseable), and wrote
their strings with `%s` unescaped: GET /api/device-info wrote the device name
a person typed, so a `"` in it broke the answer (F212).

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
    (host-tested: the shortest adverts that fit, whole, in browse order, and
    a complete document at every step). The task begins a
    `fleet_scan_cache::Cache` over the staging buffer it allocated with
    `FLEET_SCAN_CACHE_SIZE` bytes, with that size; reads the browse's count
    once (`const int n = MDNS.queryService(...)`, never assigned again) and
    hands that count, unchanged, to `fleet_scan_cache::fill()` once, which
    offers every result (the loop and its bound are the header's, where the
    host test holds them); calls no `fleet_scan_cache::add()` of its own; and
    copies the buffer into the cache with that size, in that order: begun,
    filled, copied, freed (F227). Nothing else writes the staging buffer. No
    site is exempt from J1-J3: the one that was, the cut cache's
    `serializeJson()`, is gone.

J5. An answer spelled with `snprintf()` (F212): a call whose format is a JSON
    object or array (its first byte `{` or `[`), into a buffer the same
    function sends, and every call that continues it at `buf + pos`; and
    every call into a buffer the function sends piece by piece with
    `httpd_resp_send_chunk()` or `httpd_resp_sendstr_chunk()` whose format
    spells JSON (JSONISH: a key, a `"%s"`, or an opening `{` or `[`), so a
    chunked list's `,"%s"` row is a piece of the answer (F227 review). A send
    is `httpd_resp_sendstr()`, `httpd_resp_send()`, their chunked forms, or
    any helper of the sketch shaped `(httpd_req_t* req, const char* text)`
    (`const char* const` too) that hands `text` to a send (`http_send_json()`,
    `send_json()`, `send_json_response()`, found to a fixpoint, so an answer
    sent through a helper is an answer: F227), with or without a
    `(const char*)` cast at the send. A format may be one of the sketch's
    named string constants. No answer file calls an unsized `sprintf()` or
    `vsprintf()` (F227). Each answer is measured, one of:
    - guarded: the statement after it tests the length it returned against
      the size it was given (`n >= (int)sizeof(buf)` beside `n <= 0` or
      `n < 0`, or `(size_t)n >= sizeof(buf)`) and its body returns;
    - composed: it is followed by `ok = (n > 0 && (size_t)n < left);`, the
      buffer's `left` starts at `sizeof(buf)` and its `pos` at 0, both move
      only together by `n` inside `if (ok)`, and the send is right behind an
      `if (!ok)` whose body returns (a refused piece is written over, never
      sent);
    - fits: a `char buf[N]` given `sizeof(buf)`, whose longest output (the
      format's own bytes and each conversion's widest spelling: `%s` by the
      longest string it can be given, an integer by its type's widest, a
      float, a `*` width or a precision unbounded) is under N.
    And each `%s` is escaped by construction: inside a JSON string every
    string it can be given (a literal, a ternary of them, a function that
    returns only literals, a `const char*` given only those, a parameter
    given only those at every call) holds no byte JSON escapes (`"`, `\\`,
    a control byte); outside one, each is `true`, `false`, `null` or a
    number. A string the check cannot read (a char array, a person-typed
    name) fails: such an answer goes through `wap_json_writer.h`.
J6. The two answers that carry strings the check cannot read (F212), GET
    /api/device-info (`handle_device_info()`, the device name a person
    typed) and the provisioning receipt (`send_provisioning_receipt()`), are
    built by `identity_json.h` (host-tested: escaped, the old bytes for
    ordinary input) into a heap buffer of the length it measured. Every
    statement that names the buffer stands in this order and there is no
    other (F227): `need = identity_json::<answer>(in, nullptr, 0) + 1`,
    `malloc(need)`, its failure refused, built with `need` in an `if (... >=
    need)` whose body frees the buffer (the receipt wipes it with
    `secure_zero()` first) and returns (F230: a refused build answers 503),
    sent, then freed (the receipt wiped first); and neither spells JSON with
    `snprintf()`.
J7. Every other JSON text the sketch spells with printf (F227: the
    non-answer texts in `canary_wap.ino`, the builders outside the answer
    files) is held or recorded. A printf is the family (`snprintf()`,
    `sprintf()`, their `v` forms) and every function or function-like macro
    of the sketch that hands its own format to one unchanged, found to a
    fixpoint (`health_logging::logf()`, `set_detail()`, `boot_linef()`,
    `CSI_LOG_WARNF()`), each judged at the format it is given. A format is a
    literal, a ternary of them, or one of the sketch's named string
    constants; any other format fails, unless it is a forwarding function's
    own parameter, whose calls are judged instead (F227 review). A format is
    JSON when it spells a key (`"name":`, `"%s":`), a string by a conversion
    (`"%s"`, as a value or a list's `,"%s"` element) or opens with `{` or
    `[` (F227 review: a text with a `%s` key, or a chunked row, was
    skipped). Each `%s` value the check can read is held as J5 holds one,
    and the values it cannot read are recorded in `RECORDED` by file and
    function, spelled as the call spells them, with why each is safe, or
    that it is not (a known gap with its open item), or that the text is
    not JSON after all (a strstr() needle, a header). A `%s` the record does
    not name, a record the code no longer matches and a recorded function
    with nothing left to record all fail. Their lengths are not computed:
    where a cut matters, the record says what the builder does with one.
J8. The device name holds no control byte (F226): ArduinoJson 7.4.1 writes
    one raw into POST /api/device-name's answer and GET /api/fleet/scan's
    self row, and JSON.parse refuses it. `setup_wizard::set_device_name()`
    opens with `if (!device_name_rule::storable(name)) return false;`,
    `init()` scrubs the stored name right after reading it
    (`device_name_rule::scrub_stored(s_device_name);`), nothing but those
    two and `get_device_name()` names `s_device_name`, and the setter is
    called once in each rename route (`handle_wifi_connect()`,
    `handle_device_name()`), after `device_name_rule::usable()` of the same
    argument, unchanged in between (host-tested in
    tests_host/test_device_name.cpp).

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
    strings: dict[str, list[bytes | None]] = field(default_factory=dict)      # `const char NAME[] = "...";`


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


# A named string constant, which a printf may be handed as its format (F227 review).
STR_CONST = re.compile(r'\b(?:static\s+)?(?:constexpr|const)\s+char\s+(\w+)\s*\[\s*\w*\s*\]\s*(?:PROGMEM\s*)?=\s*'
                       r'((?:"(?:[^"\\\n]|\\.)*"\s*)+);')


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
    for m in STR_CONST.finditer(blank_comments_only(src)):
        idx.strings.setdefault(m.group(1), []).append(c_literal_bytes(m.group(2)))
    return idx


def build_index(files: dict[str, str]) -> Index:
    idx = Index(files)
    for name, src in files.items():
        part = index_file(name, src)
        for mine, theirs in ((idx.fns, part.fns), (idx.structs, part.structs), (idx.enums, part.enums),
                             (idx.consts, part.consts), (idx.strings, part.strings)):
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
        # The browse's count, read once and handed to fill() unchanged: the
        # loop over the results, and its bound, are the header's.
        counts = re.findall(r"\bconst\s+int\s+(\w+)\s*=\s*MDNS\s*\.\s*queryService\s*\(\s*\"securacv\"\s*,"
                            r"\s*\"tcp\"\s*\)\s*;", kept[fn.body[0]:fn.body[1]])
        fills = re.findall(r"\bfleet_scan_cache\s*::\s*fill\s*\(\s*([^;]*?)\s*\)\s*;", body)
        if len(counts) != 1:
            errors.append(f"{where}: the browse's count is not read once as `const int n = "
                          f"MDNS.queryService(\"securacv\", \"tcp\");` (J4)")
        else:
            # Every write of the count after its declaration (an assignment,
            # a compound assignment, ++ or --) caps the browse.
            rest = re.sub(r"\bconst\s+int\s+" + counts[0] + r"\s*=\s*MDNS\s*\.\s*queryService\s*\([^;]*;", "", body)
            if re.search(r"(?<![\w.>])" + counts[0] + r"\s*(?:[-+*/%&|^]?=(?!=)|\+\+|--)", rest) or \
                    re.search(r"(?:\+\+|--)\s*" + counts[0] + r"\b", rest):
                errors.append(f"{where}: the browse's count {counts[0]} is changed after it is read; fill() "
                              f"offers every result (J4)")
        if len(fills) != 1:
            errors.append(f"{where}: fleet_scan_cache::fill() {len(fills)} times; the browse is handed to it once (J4)")
        else:
            args = [a.strip() for a in fills[0].split(",")]
            if len(args) != 3 or args[0] != cache or not counts or args[1] != counts[0] \
                    or not re.fullmatch(r"\w+", args[2]):
                errors.append(f"{where}: fleet_scan_cache::fill({fills[0]}) is not ({cache}, <the browse's "
                              f"count, unchanged>, <reader>): every result is offered (J4)")
    # In that order: begun, filled, copied, freed (F227: J4 held the statements, not their order).
    steps = [("fleet_scan_cache::begin()", r"\bfleet_scan_cache\s*::\s*begin\s*\("),
             ("fleet_scan_cache::fill()", r"\bfleet_scan_cache\s*::\s*fill\s*\("),
             (f"the copy of {buf}", r"\bmemcpy\s*\(\s*g_fleet_scan_cache\s*,\s*" + re.escape(buf) + r"\b"),
             (f"free({buf})", r"\bfree\s*\(\s*" + re.escape(buf) + r"\s*\)")]
    at = [[m.start() for m in re.finditer(pat, body)] for _, pat in steps]
    if all(len(a) >= 1 for a in at):
        for k in range(1, len(steps)):
            if min(at[k]) < max(at[k - 1]):
                errors.append(f"{where}: {steps[k][0]} stands before {steps[k - 1][0]}; the task begins the "
                              "cache, fills it, copies it and frees it, in that order (J4)")
                break
    elif not at[3]:
        errors.append(f"{where}: {buf} is never freed (J4)")
    if re.search(r"\bfleet_scan_cache\s*::\s*(?:add|pick_\w+)\s*\(", code):
        errors.append(f"{INO}: calls fleet_scan_cache::add() or a pick_*() step itself; fill() is the browse, "
                      f"whole (J4)")
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

# ── J5: snprintf() answers ───────────────────────────────────────────────

BASE_SINKS = ("http_send_json", "httpd_resp_sendstr", "httpd_resp_sendstr_chunk")   # (req, text)
CAST = r"(?:\(\s*const\s+char\s*\*\s*\)\s*)?"     # a send of `(const char*)buf` sends buf
TEXT_PARAM = re.compile(r"const\s+char\s*\*\s*(?:const\s+)?(\w+)")


def send_re(sinks: frozenset[str]) -> re.Pattern:
    """(req, text) sends, and httpd_resp_send(_chunk), which take a length too."""
    names = "|".join(sorted(re.escape(n) for n in sinks))
    return re.compile(r"\b(?:" + names + r")\s*\(\s*\w+\s*,\s*" + CAST + r"(\w+)\s*\)"
                      r"|\bhttpd_resp_send(?:_chunk)?\s*\(\s*\w+\s*,\s*" + CAST + r"(\w+)\s*,")


# The sends that hand the client one piece of a streamed answer: whatever a piece's first byte, a
# piece that spells JSON is a piece of the answer (F227 review).
CHUNK_SEND = re.compile(r"\bhttpd_resp_send(?:str)?_chunk\s*\(\s*\w+\s*,\s*" + CAST + r"(\w+)\s*[,)]")
# A JSON text, or a piece of one: a key (`"name":`, `"%s":`), a string spelled by a conversion
# (`"%s"`, the value or array element `,"%s"` a chunked list sends), or an object or an array opened
# at the start. A format with none of these (a log line, an HTML page, an SVG) is not judged as JSON.
STR_CONV = rb"%[-+ #0]*\d*s"
JSONISH = re.compile(rb'"[^"\n]*"\s*:|"' + STR_CONV + rb'"|^\s*[\[{]')
NO_TEXT = {"nullptr", "NULL"}


def formats_of(idx: Index, arg: str) -> list[bytes] | None:
    """Every format a printf's format argument can be: a literal, a ternary of them, one of the
    sketch's named string constants (`const char NAME[] = "...";`), or none (nullptr). None when the
    check cannot read it (F227 review: a named format was skipped as if it were no JSON)."""
    e = strip_parens(arg)
    data = c_literal_bytes(e)
    if data is not None:
        return [data]
    if e in NO_TEXT:
        return []
    tern = top_ternary(e)
    if tern is not None:
        a, b = formats_of(idx, tern[0]), formats_of(idx, tern[1])
        return None if a is None or b is None else a + b
    if re.fullmatch(r"[A-Za-z_]\w*", e):
        vals = idx.strings.get(e) or []
        if len(set(vals)) == 1 and vals[0] is not None:
            return [vals[0]]
    return None


def one_format(idx: Index, arg: str) -> bytes | None:
    fmts = formats_of(idx, arg)
    return fmts[0] if fmts is not None and len(fmts) == 1 else None


# The printf family, by the index of its format argument.
PRINTF_FORMAT_AT = {"snprintf": 2, "vsnprintf": 2, "sprintf": 1, "vsprintf": 1}
DEFINE_FN = re.compile(r"^[ \t]*#\s*define\s+(\w+)\(([^)]*)\)((?:[^\n]*\\\n)*[^\n]*)$", re.M)
FORMAT_DECL = re.compile(r"(?:const\s+)?char\s*\*\s*(?:const\s+)?\w*")    # a declaration's `const char* fmt`


@dataclass
class Callees:
    at: dict[str, int]                       # every printf-like name -> its format argument's index
    wrappers: set[tuple[str, str]]           # (function, file): hands its own format parameter on
    macros: set[str]                         # function-like macros that hand theirs on


def callee_re(at: dict[str, int]) -> re.Pattern:
    names = "|".join(sorted(map(re.escape, at)))
    return re.compile(r"(?<![\w.>:])((?:[A-Za-z_]\w*\s*::\s*)*)(" + names + r")\s*\(")


def own_format_param(fn: Fn, arg: str) -> bool:
    return any((t := TEXT_PARAM.fullmatch(p.strip())) is not None and t.group(1) == arg.strip()
               for p in split_top(fn.params))


def printf_callees(idx: Index) -> Callees:
    """The printf family, and every function and function-like macro of the sketch that hands its own
    format, unchanged, to one of them as the format, to a fixpoint: a function taking `...` or a
    va_list whose `const char*` parameter is that format (health_logging::logf(), set_detail(),
    boot_linef()), and a macro whose parameter or __VA_ARGS__ is (CSI_LOG_WARNF()). Their calls are
    printf calls, judged at the format they are given (F227 review)."""
    cs = Callees(dict(PRINTF_FORMAT_AT), set(), set())
    changed = True
    while changed:
        changed = False
        at_items = tuple(sorted(cs.at.items()))
        for name, src in idx.files.items():
            fns, macros = forwarders_in(name, src, at_items)
            for fname, i in fns:
                if (fname, name) not in cs.wrappers:
                    cs.wrappers.add((fname, name))
                    cs.at.setdefault(fname, i)
                    changed = True
            for mname, i in macros:
                if mname not in cs.macros:
                    cs.macros.add(mname)
                    cs.at.setdefault(mname, i)
                    changed = True
    return cs


@functools.lru_cache(maxsize=1024)
def forwarders_in(name: str, src: str, at_items: tuple[tuple[str, int], ...]) -> tuple[tuple, tuple]:
    """One file's functions and macros that hand their own format to a callee in `at_items` (cached:
    a mutation re-reads only the file it changed)."""
    at = dict(at_items)
    rx = callee_re(at)
    code = blank_comments_and_strings(src)
    kept = blank_comments_only(src)
    fns: list[tuple[str, int]] = []
    for fname, defs in index_file(name, src).fns.items():
        for f in defs:
            if not f.body:
                continue
            params = [p.strip() for p in split_top(f.params)]
            if not params or not (params[-1] == "..." or re.fullmatch(r"va_list\s+\w+", params[-1])):
                continue
            for m in rx.finditer(code, f.body[0], f.body[1]):
                open_at = m.end() - 1
                args = split_top(kept[open_at + 1:matching_paren(code, open_at)])
                fi = at[m.group(2)]
                hit = [i for i, p in enumerate(params) if fi < len(args)
                       and (t := TEXT_PARAM.fullmatch(p)) is not None and t.group(1) == args[fi].strip()]
                if hit:
                    fns.append((fname, hit[0]))
                    break
    macros: list[tuple[str, int]] = []
    for d in DEFINE_FN.finditer(code):
        mparams = [p.strip() for p in d.group(2).split(",")]
        body = d.group(3)
        for m in rx.finditer(body):
            open_at = m.end() - 1
            args = split_top(body[open_at + 1:matching_paren(body, open_at)])
            fi = at[m.group(2)]
            a = args[fi].strip() if fi < len(args) else ""
            if a == "__VA_ARGS__" and mparams[-1] == "...":
                macros.append((d.group(1), len(mparams) - 1))
            elif a in mparams:
                macros.append((d.group(1), mparams.index(a)))
            else:
                continue
            break
    return tuple(fns), tuple(macros)


def printf_calls(files: dict[str, str], cs: Callees):
    """Each call of a printf-like name: (file, offset, name, args, format index). The family is
    matched unqualified, as before; a forwarding function may be qualified (health_logging::logf).
    A declaration, and a call inside a macro that hands on the macro's own format, are not calls."""
    at_items = tuple(sorted(cs.at.items()))
    for name, src in files.items():
        for at, callee, args, fi in calls_in(name, src, at_items):
            yield name, at, callee, list(args), fi


@functools.lru_cache(maxsize=1024)
def calls_in(name: str, src: str, at_items: tuple[tuple[str, int], ...]) -> tuple:
    at = dict(at_items)
    rx = callee_re(at)
    code = blank_comments_and_strings(src)
    kept = blank_comments_only(src)
    defines = [(d.start(), d.end(), [p.strip() for p in d.group(2).split(",")])
               for d in DEFINE_FN.finditer(code)]
    out = []
    for m in rx.finditer(code):
        callee = m.group(2)
        if callee in PRINTF_FORMAT_AT and m.group(1):
            continue
        open_at = m.end() - 1
        args = split_top(kept[open_at + 1:matching_paren(code, open_at)])
        fi = at[callee]
        if fi >= len(args) or FORMAT_DECL.fullmatch(args[fi].strip()):
            continue                        # another function of the name (logf(x)), or a declaration
        inside = next((d for d in defines if d[0] <= m.start() < d[1]), None)
        if inside is not None and args[fi].strip() in set(inside[2]) | {"__VA_ARGS__"}:
            continue                        # the macro's own calls are judged
        out.append((m.start(), callee, tuple(args), fi))
    return tuple(out)
def sinks_of(idx: Index) -> frozenset[str]:
    """Every function that sends its text: the httpd sends, and each helper of the sketch shaped
    `(httpd_req_t* req, const char* text)` that hands `text` to one of them (send_json() and friends),
    to a fixpoint. An answer sent through a helper is an answer (F227)."""
    sinks = set(BASE_SINKS)
    changed = True
    while changed:
        changed = False
        rx = send_re(frozenset(sinks))
        for name, fns in idx.fns.items():
            if name in sinks:
                continue
            for f in fns:
                params = split_top(f.params) if f.body else []
                if len(params) != 2 or not re.fullmatch(r"(?:const\s+)?httpd_req_t\s*\*\s*\w+", params[0].strip()):
                    continue
                text = TEXT_PARAM.fullmatch(params[1].strip())
                if text is None:
                    continue
                body = blank_comments_and_strings(idx.files[f.file])[f.body[0]:f.body[1]]
                if any((m.group(1) or m.group(2)) == text.group(1) for m in rx.finditer(body)):
                    sinks.add(name)
                    changed = True
                    break
    return frozenset(sinks)


SEND = send_re(frozenset(BASE_SINKS))   # check_files() widens it to every sink the sketch defines
CONV = re.compile(rb"%(?P<flags>[-+ #0]*)(?P<width>\d+|\*)?(?:\.(?P<prec>\d+|\*))?(?P<len>hh|h|ll|l|z|j|t|L)?"
                  rb"(?P<conv>[diouxXcsfFeEgGaAp%n])")
INT_SPELL = {  # the widest spelling of an integer conversion, by length modifier
    "d": {"hh": 4, "h": 6, "": 11, "l": 20, "ll": 20, "z": 20, "j": 20, "t": 20},
    "u": {"hh": 3, "h": 5, "": 10, "l": 20, "ll": 20, "z": 20, "j": 20, "t": 20},
    "x": {"hh": 2, "h": 4, "": 8, "l": 16, "ll": 16, "z": 16, "j": 16, "t": 16},
    "o": {"hh": 3, "h": 6, "": 11, "l": 22, "ll": 22, "z": 22, "j": 22, "t": 22},
}
# What a %s outside a JSON string may write: a token, or the separator a
# list's pieces put between them (`i ? "," : ""`).
JSON_TOKEN = re.compile(rb"true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][-+]?\d+)?|,|")


def literal_values(ctx: Ctx, expr: str, pos: int) -> list[bytes]:
    """Every string `expr` can be at file offset `pos`, when the check can see them all."""
    if ctx.depth > 12:
        raise Unbounded(expr)
    e = strip_parens(expr)
    data = c_literal_bytes(e)
    if data is not None:
        return [data.split(b"\0")[0]]
    tern = top_ternary(e)
    if tern is not None:
        return literal_values(ctx, tern[0], pos) + literal_values(ctx, tern[1], pos)
    cast = re.match(r"^\(\s*const\s+char\s*\*\s*\)\s*(.+)$", e, re.S)
    if cast is not None:
        return literal_values(ctx, cast.group(1), pos)
    call = re.match(r"^([A-Za-z_][\w:]*)\s*\(", e)
    if call is not None and matching_paren(e, call.end() - 1) == len(e) - 1:
        parts = call.group(1).split("::")
        fname, qual = parts[-1], tuple(p for p in parts[:-1] if p)
        defs = [c for c in pick(ctx.idx.fns.get(fname, []), ctx.fn.ns, qual) if c.body is not None]
        if not defs or any(c.ret.replace(" ", "") not in ("constchar*", "char*") for c in defs):
            raise Unbounded(f"{call.group(1)}(): not a function whose strings the check can read")
        out = []
        for d in defs:
            kept = blank_comments_only(ctx.idx.files[d.file])
            code = blank_comments_and_strings(ctx.idx.files[d.file])
            sub = Ctx(ctx.idx, d.file, d, kept, code, ctx.depth + 1)
            rets = list(re.finditer(r"\breturn\b", code[d.body[0]:d.body[1]]))
            if not rets:
                raise Unbounded(f"{d.name}() returns nothing the check can read")
            for m in rets:
                start = d.body[0] + m.end()
                out += literal_values(sub, kept[start:statement_end(kept, start)], start)
        return out
    if re.fullmatch(r"[A-Za-z_]\w*", e):
        return name_values(ctx, e, pos)
    raise Unbounded(e)


def name_values(ctx: Ctx, name: str, pos: int) -> list[bytes]:
    """Every string a `const char*` local or parameter holds at `pos`."""
    t = decl_type(ctx, name, pos)
    if t is None:
        raise Unbounded(f"{name}: no declaration found")
    if t[0].replace(" ", "") != "constchar*" or t[1]:
        raise Unbounded(f"{name}: a {t[0]}{'[]' * len(t[1])} whose bytes the check cannot read")
    lo = ctx.fn.body[0]
    region = ctx.code[lo:pos]
    esc = re.escape(name)
    other = re.search(r"(?<![\w.>:&])&\s*" + esc + r"\b|(?<![\w.>:])" + esc + r"\s*(?:\[[^\]]*\]\s*)?"
                      r"(?:[-+*/%&|^]|<<|>>)=|(?:\+\+|--)\s*" + esc + r"\b|(?<![\w.>:])" + esc + r"\s*(?:\+\+|--)"
                      r"|(?<![\w.>:])" + esc + r"\s*\[[^\]]*\]\s*=(?!=)", region)
    if other:
        raise Unbounded(f"{name}: changed in a way the check does not follow ({other.group(0).strip()})")
    out = []
    for a in re.finditer(r"(?<![\w.>:])" + esc + r"\s*=(?!=)", region):
        start = lo + a.end()
        out += literal_values(ctx, ctx.kept[start:statement_end(ctx.kept, start)], start)
    if local_decl(ctx, name, pos) is not None:
        if not out:
            raise Unbounded(f"{name}: a local given no value the check can see")
        return out
    # A parameter: what every call in the file passes it.
    params = split_top(ctx.fn.params)
    at = [i for i, p in enumerate(params) if re.search(r"\b" + esc + r"\b", p)]
    if len(at) != 1 or "=" in params[at[0]]:
        raise Unbounded(f"{name}: not a parameter of {ctx.fn.name}() the check can follow")
    callers = 0
    for m in re.finditer(r"(?<![\w:.>])" + re.escape(ctx.fn.name) + r"\s*\(", ctx.code):
        if ctx.fn.body and ctx.fn.body[0] - 400 <= m.start() <= ctx.fn.body[0]:
            continue                    # the definition's own header
        caller = next((f for fs in ctx.idx.fns.values() for f in fs
                       if f.file == ctx.file and f.body and f.body[0] <= m.start() < f.body[1]), None)
        if caller is None:
            continue
        open_at = m.end() - 1
        args = split_top(ctx.kept[open_at + 1:matching_paren(ctx.code, open_at)])
        if at[0] >= len(args):
            raise Unbounded(f"{ctx.fn.name}(): a call without its {name}")
        callers += 1
        out += literal_values(Ctx(ctx.idx, ctx.file, caller, ctx.kept, ctx.code, ctx.depth + 1), args[at[0]],
                              open_at + 1)
    if not callers:
        raise Unbounded(f"{ctx.fn.name}(): no call in its file to read {name} at")
    return out


@dataclass
class Printf:
    file: str
    fn: str
    line: int
    target: str
    verdict: str          # "guarded", "composed", "fits", or an error
    bound: int | None = None
    size: int | None = None


def in_json_string(fmt: bytes, upto: int) -> bool:
    inside, i = False, 0
    while i < upto:
        c = fmt[i:i + 1]
        if inside and c == b"\\":
            i += 2
            continue
        if c == b'"':
            inside = not inside
        i += 1
    return inside


def printf_conversions(ctx: Ctx, fmt: bytes, vals: list[str], pos: int) -> tuple[int | None, list[str], str]:
    """The format's longest output (None when the check cannot bound it, with why), and what is wrong
    with its %s values: a string whose bytes the check cannot read, or one that needs escaping."""
    width, problems, arg = 0, [], 0
    unbounded = ""
    last = 0
    for m in CONV.finditer(fmt):
        width += m.start() - last
        last = m.end()
        conv = m.group("conv").decode()
        if conv == "%":
            width += 1
            continue
        if arg >= len(vals):
            problems.append("more conversions than values")
            break
        val = vals[arg]
        arg += 1
        if m.group("width") == b"*" or m.group("prec") == b"*":
            problems.append("a `*` width or precision")
            continue
        mod = (m.group("len") or b"").decode()
        if conv == "s":
            if m.group("prec") is not None:
                problems.append("a %s with a precision")
                continue
            try:
                strings = literal_values(ctx, val, pos)
            except Unbounded as why:
                problems.append(f"%s value {val}: a string whose bytes the check cannot read ({why})")
                continue
            spell = max(len(v) for v in strings)
            if in_json_string(fmt, m.start()):
                bad = [v for v in strings if any(b in b'"\\' or b < 0x20 for b in v)]
                if bad:
                    problems.append(f"%s value {bad[0]!r} holds a byte JSON escapes, inside a JSON string")
            else:
                bad = [v for v in strings if not JSON_TOKEN.fullmatch(v)]
                if bad:
                    problems.append(f"%s value {bad[0]!r} is not a JSON token or a separator, outside a JSON "
                                    "string")
        elif conv in "di":
            spell = INT_SPELL["d"][mod]
        elif conv in "uxXo":
            spell = INT_SPELL["x" if conv in "xX" else conv][mod]
        elif conv == "c":
            spell = 1
        else:
            unbounded = unbounded or f"a %{conv} conversion"
            continue
        if m.group("width"):
            spell = max(spell, int(m.group("width")))
        width += spell
    width += len(fmt) - last
    if arg != len(vals) and not problems:
        problems.append("more values than conversions")
    return (None if unbounded else width), problems, unbounded


def next_statement(code: str, after: int) -> int:
    i = after
    while i < len(code) and code[i].isspace():
        i += 1
    return i


def guard_after(ctx: Ctx, stmt_end: int, var: str, size: str, target: str) -> str | None:
    """'guarded' or 'composed:<flag>' when the statement after the snprintf tests its length."""
    code, kept = ctx.code, ctx.kept
    i = next_statement(code, stmt_end + 1)
    v, sz = re.escape(var), re.escape(size.replace(" ", ""))
    if re.match(r"if\s*\(", code[i:]):
        parsed = if_statement(code, i)
        if parsed is None or not body_always_returns(parsed[1]):
            return None
        disj = [d.replace(" ", "") for d in split_top(kept[code.find("(", i) + 1:matching_paren(code, code.find("(", i))], "|")]
        disj = [d.strip("|") for d in disj if d.strip("|")]
        sizes = {sz, re.escape("sizeof(" + target + ")")}
        unsigned = any(re.fullmatch(r"\(size_t\)" + v + r">=(?:" + "|".join(sizes) + r")", d) for d in disj)
        signed = any(re.fullmatch(v + r">=\(int\)(?:" + "|".join(sizes) + r")", d) for d in disj)
        negative = any(re.fullmatch(v + r"(?:<=0|<0)", d) for d in disj)
        if unsigned or (signed and negative):
            return "guarded"
        return None
    flag = re.match(r"(?:bool\s+)?(\w+)\s*=\s*\(\s*" + v + r"\s*>\s*0\s*&&\s*\(\s*size_t\s*\)\s*" + v + r"\s*<\s*"
                    + re.escape(size.strip()) + r"\s*\)\s*;", code[i:])
    if flag:
        return "composed:" + flag.group(1)
    return None


def composed_ok(ctx: Ctx, target: str, size: str, flag: str, offset: str | None, var: str) -> str | None:
    """What breaks the composed form in ctx's function, or None."""
    lo, hi = ctx.fn.body
    body = ctx.code[lo:hi]
    t, r, f = re.escape(target), re.escape(size.strip()), re.escape(flag)
    if not re.search(r"\bsize_t\s+" + r + r"\s*=\s*sizeof\s*\(\s*" + t + r"\s*\)\s*;", body):
        return f"{size} does not start at sizeof({target})"
    pos_names = set(re.findall(r"\bsize_t\s+(\w+)\s*=\s*0\s*;", body))
    if offset is not None and offset not in pos_names:
        return f"{offset} does not start at 0"
    moves = re.findall(r"if\s*\(\s*" + f + r"\s*\)\s*\{\s*(\w+)\s*\+=\s*\(\s*size_t\s*\)\s*(\w+)\s*;\s*" + r
                       + r"\s*-=\s*\(\s*size_t\s*\)\s*(\w+)\s*;\s*\}", body)
    writes = len(re.findall(r"(?<![\w.>])" + r + r"\s*(?:[-+*/]?=)(?!=)", body)) - 1     # less the declaration
    if writes != len(moves) or any(m[1] != m[2] for m in moves):
        return f"{size} moves outside `if ({flag}) {{ pos += (size_t)n; {size} -= (size_t)n; }}`"
    for p in pos_names & {m[0] for m in moves}:
        if len(re.findall(r"(?<![\w.>])" + re.escape(p) + r"\s*(?:[-+*/]?=)(?!=)", body)) - 1 != len(moves):
            return f"{p} moves outside `if ({flag})`"
    send = None
    for m in SEND.finditer(body):
        if (m.group(1) or m.group(2)) == target:
            send = m
    if send is None:
        return f"{target} is never sent"
    stmt_start = max(body.rfind(c, 0, send.start()) for c in ";{}") + 1
    before = body[:stmt_start].rstrip()
    guard = re.search(r"if\s*\(\s*!\s*" + f + r"\s*\)\s*(\{[^{}]*\}|[^;{}]*;)\s*$", before)
    if guard is None or not body_always_returns(guard.group(1).strip("{}")):
        return f"the send of {target} is not right behind `if (!{flag})` refusing"
    return None


def printf_args(ctx: Ctx, call_at: int) -> list[str]:
    open_at = ctx.code.find("(", call_at)
    return split_top(ctx.kept[open_at + 1:matching_paren(ctx.code, open_at)])


def json_answer_starts(ctx: Ctx, calls: list[int], sent: set[str], chunked: set[str]) -> dict[str, bool]:
    """The sent buffers a snprintf() starts with a JSON object or array (False), and the buffers sent
    piece by piece with a chunk send that a snprintf() spells JSON into (True: each JSON piece is
    judged, whatever its first byte: a chunked list's `,"%s"` row is a piece of the answer)."""
    out: dict[str, bool] = {}
    for at in calls:
        args = printf_args(ctx, at)
        if len(args) < 3:
            continue
        fmt = one_format(ctx.idx, args[2])
        if args[0] in sent and (fmt is None or fmt.lstrip()[:1] in (b"{", b"[")):
            out.setdefault(args[0], False)
        if args[0] in chunked and (fmt is None or JSONISH.search(fmt)):
            out[args[0]] = True
    return out


def judge_printf(ctx: Ctx, call_at: int, answers: dict[str, bool]) -> Printf | None:
    kept, code = ctx.kept, ctx.code
    args = printf_args(ctx, call_at)
    if len(args) < 3:
        return None
    tgt = re.fullmatch(r"(\w+)(?:\s*\+\s*(\w+))?", args[0])
    if tgt is None or tgt.group(1) not in answers:
        return None
    target, offset = tgt.group(1), tgt.group(2)
    fmt = one_format(ctx.idx, args[2])
    if fmt is not None and offset is None:
        if answers[target] and not JSONISH.search(fmt):
            return None                 # a chunked piece that spells no JSON (an SVG, a part header)
        if not answers[target] and fmt.lstrip()[:1] not in (b"{", b"["):
            return None                 # a text into the same buffer that is not the JSON answer
    line = kept.count("\n", 0, call_at) + 1
    pf = Printf(ctx.file, ctx.fn.name, line, args[0], "")
    if fmt is None:
        pf.verdict = f"snprintf({args[0]}, …): a format the check cannot read (J5)"
        return pf
    bound, problems, unbounded = printf_conversions(ctx, fmt, args[3:], call_at)
    if problems:
        pf.verdict = f"{args[0]}: {problems[0]}; spell it with wap_json_writer.h (J5)"
        return pf
    stmt_end = statement_end(code, call_at)
    head = kept[max(kept.rfind(";", 0, call_at), kept.rfind("{", 0, call_at), kept.rfind("}", 0, call_at)) + 1:call_at]
    var = re.search(r"(?:\b(?:const\s+)?(?:int|size_t)\s+)?(\w+)\s*=\s*$", head)
    guard = guard_after(ctx, stmt_end, var.group(1), args[1], target) if var else None
    if guard == "guarded":
        pf.verdict = "guarded"
        return pf
    if guard is not None and guard.startswith("composed:"):
        why = composed_ok(ctx, target, args[1], guard.split(":", 1)[1], offset, var.group(1))
        pf.verdict = "composed" if why is None else f"{args[0]}: {why} (J5)"
        return pf
    if offset is not None:
        pf.verdict = f"{args[0]}: a piece written at an offset with no length test after it (J5)"
        return pf
    t = decl_type(ctx, target, call_at)
    if t is None or re.sub(r"\bconst\b", "", t[0]).replace(" ", "") != "char" or len(t[1]) != 1:
        pf.verdict = f"{target}: a buffer the check cannot size, and no length test after the snprintf (J5)"
        return pf
    size = const_value(ctx.idx, t[1][0], ctx.fn.ns)
    if args[1].replace(" ", "") != f"sizeof({target})" and const_value(ctx.idx, args[1], ctx.fn.ns) != size:
        pf.verdict = f"{target}: written with a size ({args[1]}) that is not the buffer's (J5)"
        return pf
    if size is None or bound is None:
        pf.verdict = (f"{target}: cannot bound the answer ({unbounded or 'its size'}) and no length test after "
                      "the snprintf (J5)")
        return pf
    pf.bound, pf.size = bound, size
    if bound >= size:
        pf.verdict = (f"{target}: its longest answer is {bound} bytes against {size}, so it is cut; test "
                      "snprintf()'s length or size the buffer past it (J5)")
        return pf
    pf.verdict = "fits"
    return pf


def check_printf_answers(files: dict[str, str], idx: Index) -> tuple[list[str], list[Printf]]:
    errors: list[str] = []
    found: list[Printf] = []
    for name in scope_of(files):
        code = blank_comments_and_strings(files[name])
        kept = blank_comments_only(files[name])
        for fs in idx.fns.values():
            for fn in fs:
                if fn.file != name or not fn.body:
                    continue
                body = code[fn.body[0]:fn.body[1]]
                sent = {m.group(1) or m.group(2) for m in SEND.finditer(body)}
                chunked = {m.group(1) for m in CHUNK_SEND.finditer(body)}
                if not sent or "snprintf" not in body:
                    continue
                ctx = Ctx(idx, name, fn, kept, code)
                calls = [fn.body[0] + m.start() for m in re.finditer(r"(?<![\w:.>])snprintf\s*\(", body)
                         if fn_containing(idx, name, fn.body[0] + m.start()) is fn]
                answers = json_answer_starts(ctx, calls, sent, chunked)
                for at in calls:
                    pf = judge_printf(ctx, at, answers)
                    if pf is None:
                        continue
                    found.append(pf)
                    if pf.verdict not in ("guarded", "composed", "fits"):
                        errors.append(f"{name}:{pf.line}: {fn.name}(): {pf.verdict}")
    return errors, found


# ── J6: the identity answers ─────────────────────────────────────────────

IDENTITY = {"handle_device_info": "device_info", "send_provisioning_receipt": "provisioning_receipt"}
# The identity answers that carry a secret (the API token, the AP password): wiped before every free.
IDENTITY_WIPED = {"send_provisioning_receipt"}


def statements_naming(code: str, lo: int, hi: int, name: str) -> list[tuple[int, str]]:
    """Each statement of code[lo:hi] that names `name`, in order: (offset, squashed text). An `if (...)`
    or `else if (...)` statement runs to its condition's close."""
    out: list[tuple[int, str]] = []
    seen: set[int] = set()
    for m in re.finditer(r"(?<![\w.>])" + re.escape(name) + r"\b", code[lo:hi]):
        at = lo + m.start()
        start = max(code.rfind(c, lo, at) for c in ";{}") + 1
        if start in seen:
            continue
        seen.add(start)
        head = code[start:at]
        cond = re.match(r"\s*(?:else\s+)?(?:if|while)\s*\(", head)
        if cond is not None:
            end = matching_paren(code, start + cond.end() - 1) + 1
        else:
            end = statement_end(code, start) + 1
        out.append((start, re.sub(r"\s+", "", code[start:end])))
    return out


def check_identity_answers(files: dict[str, str], idx: Index) -> list[str]:
    """J6: each identity answer is measured, allocated at that length, refused when the allocation
    fails, built at it with a refused build freed (and wiped) and refused, sent, then (wiped and)
    freed, in that order, and nothing else names the buffer (F212, F227, F230)."""
    errors = []
    code = blank_comments_and_strings(files[INO])
    for fname, builder in IDENTITY.items():
        fns = [f for f in idx.fns.get(fname, []) if f.file == INO and f.body]
        where = f"{INO}: {fname}()"
        if len(fns) != 1:
            errors.append(f"{where}: {len(fns)} definitions; the identity answer cannot be read (J6)")
            continue
        lo, hi = fns[0].body
        body = code[lo:hi]
        b = re.escape(builder)
        m = re.search(r"\bconst\s+size_t\s+(\w+)\s*=\s*identity_json\s*::\s*" + b
                      + r"\s*\(\s*(\w+)\s*,\s*nullptr\s*,\s*0\s*\)\s*\+\s*1\s*;", body)
        if m is None:
            errors.append(f"{where}: the answer is not measured first (`need = identity_json::{builder}(in, "
                          "nullptr, 0) + 1`) (J6)")
            continue
        need, inp = m.group(1), m.group(2)
        a = re.search(r"\bchar\s*\*\s*(\w+)\s*=\s*\(\s*char\s*\*\s*\)\s*malloc\s*\(\s*" + re.escape(need)
                      + r"\s*\)\s*;", body)
        if a is None:
            errors.append(f"{where}: the answer is not written into malloc({need}) (J6)")
            continue
        buf = a.group(1)
        wiped = fname in IDENTITY_WIPED
        wipe = f"secure_zero({buf},{need});"
        free = f"free({buf});"
        # Every statement that names the buffer, in the order they must stand.
        want = [f"char*{buf}=(char*)malloc({need});",
                f"if(!{buf})",
                f"if(identity_json::{builder}({inp},{buf},{need})>={need})"]
        want += [wipe] if wiped else []
        want += [free, "<send>"]
        want += [wipe] if wiped else []
        want += [free]
        got = statements_naming(code, lo, hi, buf)
        texts = [t for _, t in got]
        send_at = [k for k, t in enumerate(texts)
                   if re.fullmatch(r"(?:constesp_err_t\w+=|return)?(?:httpd_resp_sendstr|http_send_json)\(\w+," + re.escape(buf)
                                   + r"\);", t)]
        if len(send_at) == 1:
            texts[send_at[0]] = "<send>"
        if texts != want:
            first = next((k for k in range(max(len(texts), len(want)))
                          if k >= len(texts) or k >= len(want) or texts[k] != want[k]), 0)
            saw = texts[first] if first < len(texts) else "(nothing)"
            wanted = want[first] if first < len(want) else "(nothing)"
            errors.append(f"{where}: the statements naming {buf} run out of order or carry another write: "
                          f"statement {first + 1} is `{saw}` where `{wanted}` stands (measured, malloc'd, "
                          f"refused, built with a refused build {'wiped, ' if wiped else ''}freed and refused, "
                          f"sent, {'wiped, ' if wiped else ''}freed) (J6)")
            continue
        # The two refusals return.
        for k, t in ((1, "a failed malloc"), (2, "a refused build")):
            start = got[k][0]
            stmt = if_statement(code, start)
            if stmt is None or not body_always_returns(stmt[1]):
                errors.append(f"{where}: {t} is not refused (its `if` must return) (J6)")
        if re.search(r"(?<![\w:.>])snprintf\s*\(", body):
            errors.append(f"{where}: spells JSON with snprintf() again; build it with identity_json.h (J6)")
    return errors



# ── J7: the JSON texts outside J5's answers ─────────────────────────────

# Every JSON text the sketch spells with printf (the family, and each function
# or macro that forwards its format to it) that J5 does not judge as an
# answer piece (F227): non-answer texts (a QR payload, BLE characteristic
# values, card lines, MQTT bodies) and answers built outside the answer
# files. Each %s value the check can read is held as J5 holds one (inside a
# JSON string no byte JSON escapes; outside one a token). The values it
# cannot read are recorded here by file and function, spelled as the call
# spells them, with why each one is safe, or, where it is not, the gap and
# its open item, or why the text is not JSON (a needle, a header); a %s the
# record does not name, a record the code no longer matches, and a function
# with none to record fail. The lengths are not computed here: where a cut
# matters the reason says what the builder does with one.
RECORDED: dict[tuple[str, str], tuple[tuple[str, ...], str]] = {
    ("api_auth.h", "api_auth_check"): (
        ("retry_str",),
        "the 429 answer: retry_str is snprintf(\"%lu\") of the lockout's seconds, a number outside a "
        "string; body[128] holds the longest answer (63 bytes)"),
    ("beacon_channel.cpp", "sd_append_audit_entry"): (
        ("cfp_hex", "head_hex", "ofp_hex", "prev_hex", "ref_hex", "sigc_hex", "sigo_hex"),
        "the audit line on the card: every value is audit_hex() of fixed-size binary (hex digits); a cut "
        "line is refused by the length test after it"),
    ("ble_opera.h", "updateDeviceInfoCharacteristic"): (
        ('g_deviceIdHash ? g_deviceIdHash : "unknown"', 'g_firmwareVersion ? g_firmwareVersion : "0.0.0"'),
        "a BLE characteristic: the device id hash ble_manager.h hands init() (hex) and FIRMWARE_VERSION"),
    ("ble_opera.h", "updateWitnessCharacteristic"): (
        ("hashHex",),
        "a BLE characteristic: hashHex is snprintf(\"%02x\") of the chain head's first 8 bytes"),
    ("canary_wap.ino", "ble_witness_get_head_json"): (
        ("head_hex", "pub_hex"),
        "a BLE witness value: hex_to_str() of the 32-byte chain head and public key"),
    ("canary_wap.ino", "ble_witness_get_record_json"): (
        ("ch", "ph", "prev", "sig"),
        "a BLE witness value: hex_to_str() of the record's three hashes and its signature"),
    ("canary_wap.ino", "handle_pairing_qr"): (
        ("base_host", "g_device.api_token_str", "g_device.device_id", "g_tls_cert_fp_hex"),
        "the pairing QR payload: the device id (prefix and the unambiguous base-32 suffix), the API token "
        "(\"cv_\" and base62), the certificate pin (hex), base_host the sanitized mDNS label ([a-z0-9-]) "
        "with \".local\" or the AP's dotted address; a cut payload is refused by the length test after it"),
    ("canary_wap.ino", "handle_vault_download"): (
        ("name",),
        "not JSON: the Content-Disposition header's filename, which the route takes only after "
        "vault_snapshot::validate_name() reads it as a vault file name (vault_logic::filename_parse(): "
        "seal_, eight digits, _, a trigger tag from its table, .svlt)"),
    ("canary_wap.ino", "loop"): (
        ('event_fresh ? g_audio_mqtt_event : "none"',),
        "the MQTT sensing body: g_audio_mqtt_event holds audio_event_name()'s literal names or \"none\""),
    ("csi_event_log_line.h", "marshal"): (
        ("rec->module_id", "rec->type_name",
         'rec->values.confidence[0] ? rec->values.confidence : ""',
         'rec->values.dominant_signal[0] ? rec->values.dominant_signal : ""',
         'rec->values.state_name[0] ? rec->values.state_name : ""'),
        "an events-log line on the card, not an answer: a live record's names are the CSI modules' own "
        "labels; a record read back from the card (a restored ring row a dismissal rewrites) passed "
        "csi_event_log_line::parse(), which refuses a line whose string field holds a backslash or a byte "
        "outside printable ASCII (sweep F264; json_str() stops at a quote), so every name it spells needs "
        "no escape; marshal() returns 0 on a cut line"),
    ("csi_event_wire.h", "build_event_body"): (
        ('module_id ? module_id : ""', "sig_b64", "sig_kv", 'signer.alg ? signer.alg : ""',
         "signer.fingerprint_hex", "state_s", 'type_name ? type_name : ""',
         'values->confidence[0] ? values->confidence : "tentative"'),
        "the MQTT events body (host-tested in firmware/tests_host): a live record's module and state names "
        "are the modules' labels, the signature base64url, the fingerprint hex, the algorithm a constant; "
        "sig_kv is the piece this function spells itself; a cut body returns 0. A record the backfill "
        "reads back from the card (csi_event_backfill.h, through csi_mqtt::publish_event_row()) passed "
        "csi_event_log_line::parse(), which refuses a line whose string field holds a backslash or a byte "
        "outside printable ASCII (sweep F264): the walk skips and counts such a line and sends the rows "
        "behind it"),
    ("csi_event_wire.h", "build_tamper_bridge_body"): (
        ("values->state_name",),
        "the tamper bridge body: only for system.integrity's tamper events, whose state names are "
        "tamper_events_module's literal kinds"),
    ("csi_integration.cpp", "handle_events_today"): (
        ("active", "r->module_id", "r->type_name", "r->values.confidence", "r->values.state_name"),
        "GET /api/events/today, chunked: rows from the event ring (the modules' labels; a row the "
        "boot load restored from the card passed csi_event_log_line::parse(), which refuses a backslash or "
        "a byte outside printable ASCII in its strings, sweep F264) and active, "
        "tamper_events_active_kind()'s literal kinds; each row is length-tested and a cut one skipped"),
    ("csi_integration.cpp", "handle_pair_token"): (
        ("hex",),
        "the pairing token answer: pair_token_issue() writes hex digits"),
    ("csi_integration.cpp", "handle_settings_get"): (
        ("tz", "tz_iana"),
        "the settings answer: tz is kept only when tz_rule::posix_valid() parses it (letters, digits, "
        "<>+-:,./), tz_iana only when it names a zone in tz_rule's table"),
    ("csi_integration.cpp", "handle_stream"): (
        ("s->module_id", "s->type_name", "s->values.confidence", "s->values.state_name", "supply"),
        "the stream answer: the modules' labels, and supply, the JSON piece this function spells itself "
        "from numbers and true/false"),
    ("csi_integration.cpp", "handle_tune_get_preset"): (
        ("c.full_key",),
        "GET /api/tune/preset, chunked: each row's key is the coefficient table's literal full key "
        "(TUNE_COEFFS, as handle_tune_get_coefficients' record says), the value a number; the longest "
        "row (a 36-byte key, an 11-digit long) is far under buf[160]"),
    ("csi_integration.cpp", "handle_tune_get_coefficients"): (
        ("c.full_key", "c.group", "c.label"),
        "the Tuning Lab list: the coefficient table's literal keys, groups and labels"),
    ("csi_mqtt.cpp", "handle_test"): (
        ("s_last_error", "transport_name()"),
        "POST /api/mqtt/test: s_last_error holds one of mqtt_transport_logic.h's constant messages (its "
        "host suite holds them free of quotes and backslashes), transport_name() a literal"),
    ("csi_mqtt.cpp", "publish_beacon_state"): (
        ('active_template ? active_template : ""', 'state_name ? state_name : "unknown"'),
        "an MQTT body: Beacon's literal state names and template ids"),
    ("csi_mqtt.cpp", "publish_chain"): (
        ("device_signature::ALG_NAME", "device_signature::fingerprint_hex()", "hash_hex", "sig_b64"),
        "an MQTT body: the algorithm constant, the fingerprint and chain hash in hex, the signature in "
        "base64url"),
    ("csi_mqtt.cpp", "publish_chirp_state"): (
        ('state_name ? state_name : "unknown"',),
        "an MQTT body: Chirp's literal state names"),
    ("csi_mqtt.cpp", "publish_counts"): (
        ("device_signature::ALG_NAME", "device_signature::fingerprint_hex()", "sig_b64"),
        "an MQTT body: the algorithm constant, the fingerprint in hex, the signature in base64url"),
    ("csi_mqtt.cpp", "publish_egress"): (
        ("object", "s_firmware_version"),
        "an MQTT body: object is csi_event_egress::stats_json()'s JSON object (numbers only), written "
        "outside a string on purpose; the firmware version a constant"),
    ("csi_mqtt.cpp", "publish_health"): (
        ('battery->charge_state ? battery->charge_state : "unknown"', "s_firmware_version", "s_public_key_hex"),
        "an MQTT body: power_monitor's literal charge-state names, the firmware version, the public key in "
        "hex"),
    ("csi_mqtt.cpp", "publish_mic_discovery"): (
        ("prefix", "s_device_id", "s_firmware_version", "slug"),
        "an MQTT discovery body: the topic prefix (the config POST keeps only its safe character set), the "
        "device id and its slug, the firmware version"),
    ("csi_mqtt.cpp", "publish_one_discovery"): (
        ("dev_class_kv", "e.component", "e.dev_class", "e.icon", "e.name", "e.object_id", "e.state_class",
         "e.state_topic", "e.unit", "e.val_tpl", "icon_kv", "prefix", "s_device_id", "s_firmware_version",
         "slug", "state_class_kv", "unit_kv"),
        "an MQTT discovery body: the entity table's literals, the *_kv pieces this function spells from "
        "them, the topic prefix (its safe character set), the device id and its slug, the firmware version"),
    ("csi_mqtt.cpp", "publish_one_trigger"): (
        ("prefix", "s_device_id", "s_firmware_version", "t.payload", "t.state_topic", "t.subtype", "t.type",
         "t.val_tpl"),
        "an MQTT discovery body: the trigger table's literals, the topic prefix (its safe character set), "
        "the device id, the firmware version"),
    ("csi_mqtt.cpp", "publish_update_discovery"): (
        ("prefix", "s_device_id", "s_firmware_version", "slug"),
        "an MQTT discovery body: the topic prefix (its safe character set), the device id and its slug, "
        "the firmware version"),
    ("csi_tune_lab.cpp", "tune_post"): (
        ("c.full_key",),
        "not a text: the strstr() needle a Tuning Lab POST body is searched for, the coefficient table's "
        "literal full key between quotes; a needle that does not fit is skipped"),
    ("device_signature.cpp", "handle_enroll_json"): (
        ("nonce", "sig_hex"),
        "the enrollment answer: the nonce is accepted only as hex, the signature written as hex"),
    ("device_signature.cpp", "render_enroll_json"): (
        ("device_signature::ALG_NAME", 'did ? did : ""', 'fp ? fp : ""', 'pk ? pk : ""'),
        "the enrollment answer: the device id, the public key and fingerprint in hex, the algorithm "
        "constant"),
    ("health_store_logic.h", "line_build"): (
        ('category_name ? category_name : "?"', "det_esc", 'level_name ? level_name : "?"', "msg_esc"),
        "a health line on the card: message and detail through json_escape() (quotes, backslashes and "
        "control bytes escaped, host-tested), level and category literal names"),
    ("sys_monitor.h", "get_json"): (
        ("ESP.getSdkVersion()", "g_sys_metrics.chip_model", "hw_token", "uptime_str"),
        "GET /api/system: the chip model ESP.getChipModel() names, the IDF version string, the hardware "
        "pseudonym in hex, the uptime format_uptime() spells from digits"),
    ("wap_diagnostics.h", "build_json"): (
        ('in.degrade_name != nullptr ? in.degrade_name : "UNKNOWN"', "object"),
        "the diagnostics body (host-tested): the degrade level's literal name, and object, a JSON piece "
        "written outside a string on purpose"),
    ("wap_server.cpp", "send_json_error"): (
        ('error_code ? error_code : "error"', 'message ? message : "Unknown error"'),
        "never called: wap_server.cpp's helper has no caller in the sketch"),
    ("witness_page.h", "header_build"): (
        ("CHAIN_FORMAT", "SCHEMA", "id"),
        "the witness page header (host-tested): the schema and chain-format constants, the device id"),
    ("witness_page.h", "record_build"): (
        ("hash", "iso", "ph", "prev", "sig", "when"),
        "the witness page record (host-tested): hashes and signature in hex, iso an ISO-8601 stamp, when "
        "the piece this function spells around it"),
    ("witness_store.h", "line_build"): (
        ("ch", "ph", "prev", "sig"),
        "a witness line on the card (host-tested): hashes and signature in hex"),
}


def check_recorded_json_texts(files: dict[str, str], idx: Index, judged: set[tuple[str, int]]) -> list[str]:
    """J7: every printf-spelled JSON text outside J5's answers is held or recorded: every call of the
    family and of each function or macro that forwards its format to it, at every format the call can
    be given; a format the check cannot read fails."""
    global J7_STATS
    errors: list[str] = []
    seen: dict[tuple[str, str], set[str]] = {}
    sites = 0
    cs = printf_callees(idx)
    for name, at, callee, args, fi in printf_calls(files, cs):
        code = blank_comments_and_strings(files[name])
        kept = blank_comments_only(files[name])
        line = code.count("\n", 0, at) + 1
        where = f"{name}:{line}"
        fmts = formats_of(idx, args[fi])
        if not fmts or not any(JSONISH.search(f) for f in fmts):
            if fmts is not None:
                continue                    # no JSON: a log line, a page, a path
        fn = fn_containing(idx, name, at)
        if fmts is None:
            if fn is not None and (fn.name, name) in cs.wrappers and own_format_param(fn, args[fi]):
                continue                    # a forwarding function's own format: its calls are judged
            errors.append(f"{where}: {callee}() is handed a format the check cannot read "
                          f"({' '.join(args[fi].split())}): spell it as a literal or a named string constant, "
                          "so the check can tell whether it spells JSON (J7)")
            continue
        for fmt in fmts:
            if not JSONISH.search(fmt) or (name, line) in judged:
                continue
            if fn is None:
                errors.append(f"{where}: a JSON text spelled outside a function (J7)")
                continue
            key = (name.rsplit("/", 1)[-1], fn.name)
            unread = seen.setdefault(key, set())
            sites += 1
            ctx = Ctx(idx, name, fn, kept, code)
            vals = args[fi + 1:]
            ai = 0
            for cm in CONV.finditer(fmt):
                conv = cm.group("conv").decode()
                if conv == "%":
                    continue
                if ai >= len(vals):
                    break
                val = vals[ai]
                ai += 1
                if conv != "s":
                    continue
                try:
                    strings = literal_values(ctx, val, at)
                except Unbounded:
                    unread.add(" ".join(val.split()))
                    continue
                if in_json_string(fmt, cm.start()):
                    bad = [v for v in strings if any(b in b'"\\' or b < 0x20 for b in v)]
                    if bad:
                        errors.append(f"{where}: {fn.name}(): %s value {bad[0]!r} holds a byte JSON escapes, "
                                      "inside a JSON string (J7)")
                else:
                    bad = [v for v in strings if not JSON_TOKEN.fullmatch(v)]
                    if bad:
                        errors.append(f"{where}: {fn.name}(): %s value {bad[0]!r} is not a JSON token, outside "
                                      "a JSON string (J7)")
    for key, unread in sorted(seen.items()):
        rec = RECORDED.get(key)
        if not unread:
            if rec is not None:
                errors.append(f"{key[0]}: {key[1]}(): recorded, but every %s it writes is one the check reads; "
                              "drop the record (J7)")
            continue
        if rec is None:
            errors.append(f"{key[0]}: {key[1]}(): a JSON text with %s values the check cannot read "
                          f"({', '.join(sorted(unread))}); record why each is safe, or spell it with "
                          "wap_json_writer.h (J7)")
        elif set(rec[0]) != unread:
            new = sorted(unread - set(rec[0]))
            gone = sorted(set(rec[0]) - unread)
            errors.append(f"{key[0]}: {key[1]}(): the record does not match the code (not recorded: {new}; "
                          f"recorded but gone: {gone}) (J7)")
        elif not rec[1].strip():
            errors.append(f"{key[0]}: {key[1]}(): recorded with no reason (J7)")
    for key in RECORDED:
        if key not in seen:
            errors.append(f"{key[0]}: {key[1]}(): recorded, but it spells no JSON text any more (J7)")
    J7_STATS = (sites, len(seen), sum(1 for k in seen if k in RECORDED))
    return errors


J7_STATS = (0, 0, 0)


# ── J8: the device name holds no control byte ────────────────────────────

WIZARD = f"{SKETCH}/setup_wizard.h"
NAME_ROUTES = ("handle_wifi_connect", "handle_device_name")


def check_device_name(files: dict[str, str], idx: Index) -> list[str]:
    """J8: setup_wizard::set_device_name() refuses a control byte, init() scrubs a stored name, nothing
    else writes s_device_name, and every rename decides with device_name_rule::usable() first."""
    errors: list[str] = []
    if WIZARD not in files:
        return [f"{WIZARD}: missing; the device name's setter cannot be read (J8)"]
    code = blank_comments_and_strings(files[WIZARD])
    kept = blank_comments_only(files[WIZARD])

    def one(name: str) -> Fn | None:
        fns = [f for f in idx.fns.get(name, []) if f.file == WIZARD and f.body and f.ns == ("setup_wizard",)]
        if len(fns) != 1:
            errors.append(f"{WIZARD}: setup_wizard::{name}() {len(fns)} times; it cannot be read (J8)")
            return None
        return fns[0]

    setter, init, getter = one("set_device_name"), one("init"), one("get_device_name")
    if setter is not None:
        body = code[setter.body[0]:setter.body[1]]
        params = split_top(setter.params)
        arg = re.search(r"(\w+)\s*$", params[0]).group(1) if len(params) == 1 else None
        if arg is None or not re.match(r"\s*if\s*\(\s*!\s*device_name_rule\s*::\s*storable\s*\(\s*" + arg
                                       + r"\s*\)\s*\)\s*return\s+false\s*;", body):
            errors.append(f"{WIZARD}: set_device_name() does not open with `if (!device_name_rule::storable(name)) "
                          "return false;`: a name holding a control byte would be stored (J8)")
    if init is not None:
        body = kept[init.body[0]:init.body[1]]
        if not re.search(r"prefs\s*\.\s*getString\s*\(\s*\"dev_name\"\s*,\s*s_device_name\s*,\s*sizeof\s*\(\s*"
                         r"s_device_name\s*\)\s*\)\s*;\s*device_name_rule\s*::\s*scrub_stored\s*\(\s*s_device_name\s*\)"
                         r"\s*;", body):
            errors.append(f"{WIZARD}: init() does not scrub the stored name right after reading it "
                          "(`device_name_rule::scrub_stored(s_device_name);`): a name stored before F226 would "
                          "reach the answers with its control bytes (J8)")
    # Nothing but the declaration, init(), get_device_name() and set_device_name() names s_device_name.
    allowed = [f.body for f in (setter, init, getter) if f is not None]
    for name, src in files.items():
        c = blank_comments_and_strings(src)
        for m in re.finditer(r"\bs_device_name\b", c):
            if name == WIZARD and (any(b[0] <= m.start() < b[1] for b in allowed)
                                   or re.match(r"static\s+char\s+s_device_name\s*\[", c[c.rfind("\n", 0, m.start()) + 1:].lstrip())):
                continue
            line = c.count("\n", 0, m.start()) + 1
            errors.append(f"{name}:{line}: names s_device_name outside setup_wizard.h's init(), "
                          "set_device_name() and get_device_name(): the name's one writer is the setter (J8)")
    # Every rename decides with usable() on the argument it then stores.
    calls = 0
    for name, src in files.items():
        c = blank_comments_and_strings(src)
        k = blank_comments_only(src)
        for m in re.finditer(r"\bsetup_wizard\s*::\s*set_device_name\s*\(", c):
            calls += 1
            open_at = m.end() - 1
            args = split_top(k[open_at + 1:matching_paren(c, open_at)])
            fn = fn_containing(idx, name, m.start())
            line = c.count("\n", 0, m.start()) + 1
            where = f"{name}:{line}: {fn.name if fn else '?'}()"
            if fn is None or len(args) != 1 or not re.fullmatch(r"\w+", args[0]):
                errors.append(f"{where}: set_device_name() with an argument the check cannot follow (J8)")
                continue
            if fn.name not in NAME_ROUTES:
                errors.append(f"{where}: sets the device name outside the two rename routes {NAME_ROUTES} (J8)")
            before = c[fn.body[0]:m.start()]
            if not re.search(r"\bdevice_name_rule\s*::\s*usable\s*\(\s*" + re.escape(args[0]) + r"\s*\)", before):
                errors.append(f"{where}: stores {args[0]} without deciding with device_name_rule::usable({args[0]}) "
                              "first (J8)")
            elif re.search(r"(?<![\w.>])" + re.escape(args[0]) + r"\s*=(?!=)", before[before.rfind("device_name_rule"):]):
                errors.append(f"{where}: {args[0]} is reassigned between usable() and the store (J8)")
    if calls != len(NAME_ROUTES):
        errors.append(f"{INO}: setup_wizard::set_device_name() is called {calls} times, not once in each of "
                      f"{NAME_ROUTES} (J8)")
    return errors


def check_unsized_printf(files: dict[str, str]) -> list[str]:
    """J5: no answer file spells text with an unsized sprintf() (or vsprintf()): it writes whatever
    length its arguments come to, past any buffer (F227)."""
    errors = []
    for name in scope_of(files):
        code = blank_comments_and_strings(files[name])
        for m in re.finditer(r"(?<![\w:.>])v?sprintf\s*\(", code):
            line = code.count("\n", 0, m.start()) + 1
            errors.append(f"{name}:{line}: an unsized {m.group(0).rstrip('( ')}() in an answer file: give it the "
                          "buffer's size (snprintf()) or spell the text with wap_json_writer.h (J5)")
    return errors


def check_files(files: dict[str, str]) -> tuple[list[str], list[Site]]:
    global SEND
    idx = build_index(files)
    SEND = send_re(sinks_of(idx))
    errors: list[str] = []
    sites: list[Site] = []
    errors += check_unsized_printf(files)
    for name in EXCLUDED:
        if name not in files:
            errors.append(f"{name}: EXCLUDED names a file the sketch no longer has")
    errors += check_fleet_cache(files, idx)
    errors += check_identity_answers(files, idx)
    errors += check_device_name(files, idx)
    printf_errors, printfs = check_printf_answers(files, idx)
    errors += printf_errors
    errors += check_recorded_json_texts(files, idx, {(pf.file, pf.line) for pf in printfs})
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
     raw_in(INO, "      fleet_scan_cache::fill(cache, n, read_advert);",
            "      fleet_scan_cache::fill(cache, n, read_advert);\n      strcat(staging, name.c_str());")),
    ("the task hands fill() only the first eight results (the review's G2)",
     raw_in(INO, "      fleet_scan_cache::fill(cache, n, read_advert);",
            "      fleet_scan_cache::fill(cache, n < 8 ? n : 8, read_advert);")),
    ("the task puts back its own loop, bounded at eight",
     raw_in(INO, "      fleet_scan_cache::fill(cache, n, read_advert);",
            "      for (int i = 0; i < n && i < 8; i++) fleet_scan_cache::add(cache, read_advert(i));")),
    ("the browse's count is capped where it is read",
     raw_in(INO, '    const int n = MDNS.queryService("securacv", "tcp");',
            '    const int n = std::min(MDNS.queryService("securacv", "tcp"), 8);')),
    ("the browse's count is cut after it is read",
     raw_in(INO, '    const int n = MDNS.queryService("securacv", "tcp");',
            '    int n = MDNS.queryService("securacv", "tcp");\n    if (n > 8) n = 8;')),
    ("the browse is handed to fill() twice",
     raw_in(INO, "      fleet_scan_cache::fill(cache, n, read_advert);",
            "      fleet_scan_cache::fill(cache, n, read_advert);\n      fleet_scan_cache::fill(cache, n, read_advert);")),
    ("the fleet scan's cache is copied short",
     raw_in(INO, "memcpy(g_fleet_scan_cache, staging, FLEET_SCAN_CACHE_SIZE);",
            "memcpy(g_fleet_scan_cache, staging, strlen(staging));")),
    # J5: a snprintf() answer is measured, and every %s in it escaped by construction.
    ("http_send_error() goes down to a 40-byte buffer (its longest answer is 44)",
     raw_in(INO, "  char response[128];\n  snprintf(response, sizeof(response), \"{\\\"ok\\\":false,\\\"error\\\":\\\"%s\\\"}\", error_code);",
            "  char response[40];\n  snprintf(response, sizeof(response), \"{\\\"ok\\\":false,\\\"error\\\":\\\"%s\\\"}\", error_code);")),
    ("a route answers the request's own bytes through http_send_error()",
     raw_in(INO, 'return http_send_error(req, 400, "unknown_action");', "return http_send_error(req, 400, action);")),
    ("a peek refusal's reason holds a quote",
     raw_in(f"{SKETCH}/camera_gate_logic.h", '"Device is too hot. The preview stays off until it cools."',
            '"Device is \\"too hot\\". The preview stays off until it cools."')),
    ("the mic mute answer writes yes/no where a token goes",
     raw_in(INO, 'muted ? "true" : "false", persisted ? "true" : "false");',
            'muted ? "yes" : "no", persisted ? "true" : "false");')),
    ("the receipt gate's answer gets 128 bytes and an off-by-one length test",
     both(raw_in(INO, "    char body[256];\n    int n = snprintf(body, sizeof(body),",
                 "    char body[128];\n    int n = snprintf(body, sizeof(body),"),
          raw_in(INO, "if (n < 0 || (size_t)n >= sizeof(body)) {", "if (n < 0 || (size_t)n > sizeof(body)) {"))),
    ("the receipt gate's answer gets 128 bytes and a length test that does not refuse",
     both(raw_in(INO, "    char body[256];\n    int n = snprintf(body, sizeof(body),",
                 "    char body[128];\n    int n = snprintf(body, sizeof(body),"),
          raw_in(INO, "      return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,\n"
                      "                                 \"Failed to build provisioning gate response\");",
                 "      (void)httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,\n"
                 "                                 \"Failed to build provisioning gate response\");"))),
    ("the transitions answer drops its final refusal",
     raw_in(INO, "  if (!fits) {\n    return http_send_json(req, \"{\\\"ok\\\":false,\\\"error\\\":\\\"buffer overflow\\\"}\");\n  }\n"
                 "  return http_send_json(req, buf);", "  return http_send_json(req, buf);")),
    ("a transitions piece is written with the whole buffer's size",
     raw_in(INO, "    w = snprintf(buf + pos, remaining,\n", "    w = snprintf(buf + pos, sizeof(buf),\n")),
    ("the transitions answer's room stops shrinking",
     raw_in(INO, "  bool fits = (w > 0 && (size_t)w < remaining);\n  if (fits) { pos += (size_t)w; remaining -= (size_t)w; }",
            "  bool fits = (w > 0 && (size_t)w < remaining);\n  if (fits) { pos += (size_t)w; }")),
    ("a new route answers a float it cannot bound, untested",
     add_file(f"{SKETCH}/zz_float_api.h",
              "namespace zz_api {\ninline esp_err_t handle_zz(httpd_req_t* req, float t) {\n  char buf[32];\n"
              "  snprintf(buf, sizeof(buf), \"{\\\"t\\\":%f}\", t);\n  return httpd_resp_sendstr(req, buf);\n}\n}\n")),
    ("a new route answers a char array, length-tested but unescaped",
     add_file(f"{SKETCH}/zz_name_api.h",
              "namespace zz_api {\ninline esp_err_t handle_zz(httpd_req_t* req) {\n  char name[33] = {0};\n  char buf[96];\n"
              "  int n = snprintf(buf, sizeof(buf), \"{\\\"name\\\":\\\"%s\\\"}\", name);\n"
              "  if (n < 0 || (size_t)n >= sizeof(buf)) return ESP_FAIL;\n  return httpd_resp_sendstr(req, buf);\n}\n}\n")),
    # J6: the identity answers are built by identity_json.h at their measured length.
    ("GET /api/device-info goes back to snprintf() with the raw name",
     raw_in(INO, "  const size_t need = identity_json::device_info(in, nullptr, 0) + 1;\n  char* json = (char*)malloc(need);\n"
                 "  if (!json) return http_send_error(req, 500, \"out_of_memory\");\n"
                 "  if (identity_json::device_info(in, json, need) >= need) {\n    free(json);\n"
                 "    return http_send_error(req, 503, \"answer_changed\");\n  }\n",
            "  char json[768];\n  snprintf(json, sizeof(json), \"{\\\"device_name\\\":\\\"%s\\\"}\", in.device_name);\n")),
    ("the receipt is built into a fixed 1024 bytes",
     raw_in(INO, "  const size_t need = identity_json::provisioning_receipt(in, nullptr, 0) + 1;",
            "  const size_t need = 1024;")),
    ("GET /api/device-info is built with a size that is not the measured one",
     raw_in(INO, "  if (identity_json::device_info(in, json, need) >= need) {", "  if (identity_json::device_info(in, json, 512) >= need) {")),
    ("GET /api/device-info sends a refused build (the F230 refusal dropped)",
     raw_in(INO, "  if (identity_json::device_info(in, json, need) >= need) {\n    free(json);\n"
                 "    return http_send_error(req, 503, \"answer_changed\");\n  }\n",
            "  identity_json::device_info(in, json, need);\n")),
    ("a failed malloc for the receipt is not refused",
     raw_in(INO, "  if (!json) return http_send_error(req, 500, \"out_of_memory\");\n  if (identity_json::provisioning_receipt(in, json, need) >= need) {",
            "  if (identity_json::provisioning_receipt(in, json, need) >= need) {")),
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
    # F227: what J4 to J6 did not read.
    ("a route spells its hex with an unsized sprintf() again",
     raw_in(BEACON, '    snprintf(out + i * 2, 3, "%02x", fp[i]);', '    sprintf(out + i * 2, "%02x", fp[i]);')),
    ("an answer file formats a text with vsprintf()",
     add_file(f"{SKETCH}/zz_vs_api.h",
              "#include <stdarg.h>\nnamespace zz_api {\ninline void fmt(char* out, const char* f, ...) {\n"
              "  va_list ap; va_start(ap, f); vsprintf(out, f, ap); va_end(ap);\n}\n}\n")),
    ("a new route answers a raw name through its own send_json() helper",
     add_file(f"{SKETCH}/zz_helper_api.h",
              "namespace zz_api {\nstatic inline esp_err_t send_json(httpd_req_t* req, const char* json) {\n"
              "  httpd_resp_set_type(req, \"application/json\");\n  return httpd_resp_sendstr(req, json);\n}\n"
              "inline esp_err_t handle_zz(httpd_req_t* req) {\n  char name[33] = {0};\n  char buf[96];\n"
              "  int n = snprintf(buf, sizeof(buf), \"{\\\"name\\\":\\\"%s\\\"}\", name);\n"
              "  if (n < 0 || (size_t)n >= sizeof(buf)) return ESP_FAIL;\n  return send_json(req, buf);\n}\n}\n")),
    ("a new route answers through a helper that hands its text to another helper",
     add_file(f"{SKETCH}/zz_helper2_api.h",
              "namespace zz_api {\nstatic inline esp_err_t send_raw(httpd_req_t* req, const char* t) {\n"
              "  return httpd_resp_send(req, t, HTTPD_RESP_USE_STRLEN);\n}\n"
              "static inline esp_err_t reply(httpd_req_t* r, const char* body) { return send_raw(r, body); }\n"
              "inline esp_err_t handle_zz(httpd_req_t* req, float t) {\n  char buf[16];\n"
              "  snprintf(buf, sizeof(buf), \"{\\\"t\\\":%f}\", t);\n  return reply(req, buf);\n}\n}\n")),
    ("a new route answers a JSON array of a raw name",
     add_file(f"{SKETCH}/zz_array_api.h",
              "namespace zz_api {\ninline esp_err_t handle_zz(httpd_req_t* req) {\n  char name[33] = {0};\n"
              "  char buf[96];\n  int n = snprintf(buf, sizeof(buf), \"[\\\"%s\\\"]\", name);\n"
              "  if (n < 0 || (size_t)n >= sizeof(buf)) return ESP_FAIL;\n  return httpd_resp_sendstr(req, buf);\n}\n}\n")),
    ("GET /api/device-info frees its answer before sending it",
     raw_in(INO, "  const esp_err_t ret = httpd_resp_sendstr(req, json);\n  free(json);\n  return ret;\n}\n\n"
                 "// ════════════════════════════════════════════════════════════════════════════\n"
                 "// API: PROVISIONING RECEIPT",
            "  free(json);\n  const esp_err_t ret = httpd_resp_sendstr(req, json);\n  return ret;\n}\n\n"
            "// ════════════════════════════════════════════════════════════════════════════\n"
            "// API: PROVISIONING RECEIPT")),
    ("the receipt is freed without its wipe",
     raw_in(INO, "  const esp_err_t ret = httpd_resp_sendstr(req, json);\n  secure_zero(json, need);\n  free(json);\n",
            "  const esp_err_t ret = httpd_resp_sendstr(req, json);\n  free(json);\n")),
    ("the receipt is wiped after it is freed",
     raw_in(INO, "  const esp_err_t ret = httpd_resp_sendstr(req, json);\n  secure_zero(json, need);\n  free(json);\n",
            "  const esp_err_t ret = httpd_resp_sendstr(req, json);\n  free(json);\n  secure_zero(json, need);\n")),
    ("the receipt's refused build is freed unwiped",
     raw_in(INO, "    secure_zero(json, need);   // a refused write pass leaves its whole pieces behind\n", "")),
    ("a strcat() onto the receipt after its build",
     raw_in(INO, "    return http_send_error(req, 503, \"answer_changed\");\n  }\n\n  httpd_resp_set_type(req, \"application/json\");\n"
                 "  httpd_resp_set_hdr(req, \"Cache-Control\", \"no-store\");\n  const esp_err_t ret = httpd_resp_sendstr(req, json);\n"
                 "  secure_zero(json, need);",
            "    return http_send_error(req, 503, \"answer_changed\");\n  }\n  strcat(json, \"x\");\n\n"
            "  httpd_resp_set_type(req, \"application/json\");\n"
            "  httpd_resp_set_hdr(req, \"Cache-Control\", \"no-store\");\n  const esp_err_t ret = httpd_resp_sendstr(req, json);\n"
            "  secure_zero(json, need);")),
    ("GET /api/device-info's refused build falls through to the send",
     raw_in(INO, "  if (identity_json::device_info(in, json, need) >= need) {\n    free(json);\n"
                 "    return http_send_error(req, 503, \"answer_changed\");\n  }\n",
            "  if (identity_json::device_info(in, json, need) >= need) {\n    free(json);\n  }\n")),
    ("the fleet scan's cache is copied out before the browse fills it",
     raw_in(INO, "    fleet_scan_cache::begin(cache, staging, FLEET_SCAN_CACHE_SIZE);\n",
            "    fleet_scan_cache::begin(cache, staging, FLEET_SCAN_CACHE_SIZE);\n"
            "    memcpy(g_fleet_scan_cache, staging, FLEET_SCAN_CACHE_SIZE);\n")),
    # J7: every JSON text spelled with printf outside the answers, held or recorded.
    ("the pairing QR carries the device name in place of the token",
     raw_in(INO, "g_device.device_id, base_host, g_device.api_token_str, g_tls_cert_fp_hex);",
            "g_device.device_id, base_host, setup_wizard::get_device_name(), g_tls_cert_fp_hex);")),
    ("the hardware state writes yes/no where a token goes",
     raw_in(f"{SKETCH}/hardware_state.h", 'g_hw.safe_mode ? "true" : "false",', 'g_hw.safe_mode ? "yes" : "no",')),
    ("a recorded builder is renamed, leaving its record behind",
     raw_in(f"{SKETCH}/wap_server.cpp", "esp_err_t send_json_error(httpd_req_t* req, int status_code,",
            "esp_err_t send_json_error2(httpd_req_t* req, int status_code,")),
    ("a new card line writes a raw char array",
     add_file(f"{SKETCH}/zz_card_line.h",
              "inline int zz_line(char* out, size_t cap, const char label[24]) {\n"
              "  return snprintf(out, cap, \"{\\\"label\\\":\\\"%s\\\"}\\n\", label);\n}\n")),
    # J5 and J7 (F227 review): what spells JSON without a literal key or an opening brace.
    ("a new route sends a JSON list of raw names, one chunked row per name",
     add_file(f"{SKETCH}/zz_chunk_api.h",
              "namespace zz_api {\nextern char g_names[4][33];\n"
              "inline esp_err_t handle_zz(httpd_req_t* req) {\n  httpd_resp_send_chunk(req, \"[\", 1);\n"
              "  for (int i = 0; i < 4; ++i) {\n    char row[48];\n"
              "    int n = snprintf(row, sizeof(row), \"%s\\\"%s\\\"\", i ? \",\" : \"\", g_names[i]);\n"
              "    if (n > 0 && (size_t)n < sizeof(row)) httpd_resp_send_chunk(req, row, n);\n  }\n"
              "  httpd_resp_send_chunk(req, \"]\", 1);\n  return httpd_resp_send_chunk(req, nullptr, 0);\n}\n}\n")),
    ("a JSON piece takes a typed label for its key",
     add_file(f"{SKETCH}/zz_keys.cpp",
              "extern char g_label[33];\nint zz_piece(char* out, size_t cap, bool first, long v) {\n"
              "  return snprintf(out, cap, \"%s\\\"%s\\\":%ld\", first ? \"\" : \",\", g_label, v);\n}\n")),
    ("the tuning preset's rows take a typed label for their key",
     raw_in(f"{SKETCH}/csi_integration.cpp", 'first ? "" : ",", c.full_key, (long)v);',
            'first ? "" : ",", g_typed_label, (long)v);')),
    ("a JSON list continues with a typed string",
     add_file(f"{SKETCH}/zz_arr.cpp",
              "extern char g_label[33];\nint zz_el(char* out, size_t cap) {\n"
              "  return snprintf(out, cap, \",\\\"%s\\\"\", g_label);\n}\n")),
    ("a JSON text's format is a named constant",
     add_file(f"{SKETCH}/zz_named.cpp",
              "static const char kZzFmt[] = \"{\\\"name\\\":\\\"%s\\\"}\";\nextern char g_label[33];\n"
              "int zz_named(char* out, size_t cap) {\n  return snprintf(out, cap, kZzFmt, g_label);\n}\n")),
    ("a printf is handed a format the check cannot read",
     add_file(f"{SKETCH}/zz_unread.cpp",
              "const char* zz_pick_format(int k);\n"
              "int zz_unread(char* out, size_t cap, int k, int v) {\n"
              "  return snprintf(out, cap, zz_pick_format(k), v);\n}\n")),
    ("a JSON text goes through the health log's printf wrapper",
     add_file(f"{SKETCH}/zz_log.cpp",
              "extern char g_label[33];\nvoid zz_log() {\n"
              "  health_logging::logf(health_logging::LEVEL_INFO, health_logging::CAT_RF, "
              "\"{\\\"name\\\":\\\"%s\\\"}\", g_label);\n}\n")),
    ("a JSON text goes through a macro that forwards its format",
     add_file(f"{SKETCH}/zz_macro.cpp",
              "extern char g_label[33];\nvoid zz_warn() {\n"
              "  CSI_LOG_WARNF(\"{\\\"name\\\":\\\"%s\\\"}\", g_label);\n}\n")),
    ("a route answers through a send_json() whose text is a `const char* const`, and its answer is cut",
     add_file(f"{SKETCH}/zz_cc_api.h",
              "namespace zz_api {\nstatic inline esp_err_t send_json2(httpd_req_t* req, const char* const json) {\n"
              "  return httpd_resp_sendstr(req, json);\n}\n"
              "inline esp_err_t handle_zz(httpd_req_t* req) {\n  char buf[8];\n"
              "  snprintf(buf, sizeof(buf), \"{\\\"state\\\":\\\"%s\\\"}\", \"connected\");\n"
              "  return send_json2(req, buf);\n}\n}\n")),
    ("a route sends its answer through a cast, and the answer is cut",
     add_file(f"{SKETCH}/zz_cast_api.h",
              "namespace zz_api {\ninline esp_err_t handle_zz(httpd_req_t* req) {\n  char buf[8];\n"
              "  snprintf(buf, sizeof(buf), \"{\\\"state\\\":\\\"%s\\\"}\", \"connected\");\n"
              "  return httpd_resp_sendstr(req, (const char*)buf);\n}\n}\n")),
    # J8: the device name holds no control byte (F226).
    ("the name's setter goes back to bounding only its length",
     raw_in(WIZARD, "  if (!device_name_rule::storable(name)) return false;\n",
            "  if (!name || strlen(name) == 0 || strlen(name) > DEVICE_NAME_MAX) return false;\n")),
    ("init() reads a stored name without scrubbing it",
     raw_in(WIZARD, "    device_name_rule::scrub_stored(s_device_name);\n", "")),
    ("init() scrubs before it reads the stored name",
     raw_in(WIZARD, "    prefs.getString(\"dev_name\", s_device_name, sizeof(s_device_name));\n"
                    "    device_name_rule::scrub_stored(s_device_name);\n",
            "    device_name_rule::scrub_stored(s_device_name);\n"
            "    prefs.getString(\"dev_name\", s_device_name, sizeof(s_device_name));\n")),
    ("POST /api/device-name decides by length again, not usable()",
     raw_in(INO, "    if (!device_name_rule::usable(name)) {\n",
            "    if (!name || strlen(name) == 0 || strlen(name) > setup_wizard::DEVICE_NAME_MAX) {\n")),
    ("the Wi-Fi join stores the name before it tests it",
     raw_in(INO, "  if (device_name_rule::usable(device_name) && setup_wizard::set_device_name(device_name)) {",
            "  if (setup_wizard::set_device_name(device_name) && device_name_rule::usable(device_name)) {")),
    ("the Wi-Fi join tests one name and stores another",
     raw_in(INO, "  if (device_name_rule::usable(device_name) && setup_wizard::set_device_name(device_name)) {",
            "  if (device_name_rule::usable(ssid) && setup_wizard::set_device_name(device_name)) {")),
    ("a name is copied into s_device_name around the setter",
     raw_in(INO, "  const char* device_name = body[\"device_name\"] | \"\";\n",
            "  const char* device_name = body[\"device_name\"] | \"\";\n"
            "  strncpy(setup_wizard::s_device_name, device_name, setup_wizard::DEVICE_NAME_MAX);\n")),
    ("a third route sets the name, tested",
     add_file(f"{SKETCH}/zz_name_route.h",
              "static void zz_ble_rename(const char* n) {\n"
              "  if (device_name_rule::usable(n)) setup_wizard::set_device_name(n);\n}\n")),
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
    j7_stats = J7_STATS                 # before the self-test re-runs the check on its mutations
    printfs = check_printf_answers(files, build_index(files))[1]
    if "--list" in argv:
        for s in sites:
            extra = f" (longest {s.bound} of {s.size})" if s.bound is not None else ""
            print(f"{s.file.rsplit('/', 1)[-1]}:{s.line} {s.fn}() -> {s.target}: {s.verdict}{extra}")
        for p in printfs:
            extra = f" (longest {p.bound} of {p.size})" if p.bound is not None else ""
            print(f"{p.file.rsplit('/', 1)[-1]}:{p.line} {p.fn}() -> snprintf {p.target}: {p.verdict}{extra}")
    for err in errors:
        print(f"::error::{err}")
    problems = [] if "--no-self-test" in argv else self_test(files)
    for p in problems:
        print(f"::error::{p}")
    if errors or problems:
        return 1
    fits = [s for s in sites if s.verdict == "fits"]
    kinds = {k: sum(1 for p in printfs if p.verdict == k) for k in ("fits", "guarded", "composed")}
    j7_sites, j7_fns, j7_recorded = j7_stats
    print(f"canary-wap REST answers fit their buffers: {len(sites)} serializeJson() calls in "
          f"{len(scope_of(files))} files, {len(fits)} fixed buffers measured against their longest "
          f"answer, the rest grow or are sized by measureJson(); {len(printfs)} snprintf() answer pieces "
          f"({kinds['fits']} measured to fit, {kinds['guarded']} length-tested, {kinds['composed']} composed), "
          f"every %s escaped by construction; the identity answers measured by identity_json.h; "
          f"{j7_sites} other JSON texts spelled with printf in {j7_fns} functions, {j7_recorded} of them "
          f"recorded by the values the check cannot read; the device name's setter, loader and routes "
          f"({len(MUTATIONS)} mutations refused).")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
