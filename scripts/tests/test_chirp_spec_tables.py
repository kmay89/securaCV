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

  * Cooldown tiers (§2.5.4, §11.2; sweep F193). §11.2 says they are not
    stored, and §2.5.4 says they reset after 24 hours with no send, keep
    across a disable and clear at a reboot (test_chirp_commands_wap.cpp
    holds the code to that).

  * Memory budget (§12.3; sweeps F306, F311). Each row of the table is a
    `static_assert` in test_chirp_commands_wap.cpp whose message names the
    row ("§12.3 <row>") and whose last comparison is the row's figure
    (`== 32 * 52`, or `== 112`), so the compile holds the figure to sizeof
    and the count; here the spec's row says the same figure, its product is
    right, the rows sum to the asserted total, and the total reads as that
    many KB. A nearby row grew 4 bytes for its first-seen time (F311) and
    the table said 48 until this pin.

  * Channel and magic byte (§2.2, §12.1, §13.1; sweep F308). §12.1's
    `#define CHIRP_CHANNEL` is mesh_network.h's `CHIRP_CHANNEL` (0, the
    current radio channel, which chirp_channel.cpp registers its broadcast
    peer at), and §2.2 and §13.1 say the channel is shared; they said
    channel 6. Every `#define` in §12.1's block names a constant the
    firmware defines with that value, so `OPERA_MAGIC` (no firmware defines
    it: an Opera frame starts with its version byte, mesh_wire.h's
    `OPERA_VERSION`, or before membership with a pairing type,
    `PAIR_FIRST` to `PAIR_LAST`) is refused. The constants come from a strict reader
    that refuses a definition it cannot spell (an expression, a name, two
    definitions) rather than guessing.

  * Sybil cost (§2.5.3, §2.5.8, THREAT_MODEL.md; sweep D16). One device
    makes any number of session keys, so neither the spec nor the threat
    model's Chirp column prices a Sybil: §2.5.3 gives the threshold as
    mesh_network.h's CONFIRMATIONS_REQUIRED and CONFIRMATIONS_SAFETY signed
    ACKs from other keys and the ten minutes (PRESENCE_REQUIRED_MS) as a
    delay, and the old phrases are gone.

  * API examples, the receive mockup and the Replay cell (§8.2, §4.2, §5.3,
    §10.1, THREAT_MODEL.md; sweeps D19, F309). Each §8.2 example names a
    route chirp_api.h registers, and its keys are exactly the keys that
    route's handler writes (`doc["..."]`, `c["..."]`) or, for a send, reads
    (`input["..."]`); every category, urgency, status, state, refusal,
    template and detail it shows is one the firmware's own functions and
    tables can produce, and a template's text and category are that
    template's. §4.2's mockup shows a real template under its own category.
    The threat model's Chirp Replay cell gives the filter's size and hash
    count as chirp_channel.cpp defines them, says it takes only verified
    nonces (a relay with no origin signature among them: v0.2's soft
    accept), and keeps F310 open; its Chirp "Spoofed origination" cell is
    partial for as long as handle_witness() soft-accepts such a relay, and
    may not say so once it does not. §5.3 says a stale frame never counts
    against its key's hour, as handle_witness() orders it. The old text (an
    "activity" category, a `message`, "1024-entry dedup", "yes (Ed25519,
    v0.2)") fails each of these.

The self-tests at the bottom hold the readers to synthetic sources they
must refuse, so a reader that stops seeing a call fails here, not silently.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_chirp_spec_tables.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests), unfiltered
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SKETCH = REPO / "firmware/projects/canary-wap/arduino/canary_wap"
SPEC = REPO / "spec/chirp_channel_v0.md"
MESH_SPEC = REPO / "spec/canary_mesh_network_v0.md"
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


class ChirpSpecCooldownTiers(unittest.TestCase):
    """The cooldown tiers are RAM only (sweep F193), and the spec says so.

    An enable keeps them and a reboot clears them: storing them would put a
    record of when the owner chirped in flash. §11.2's table says they are
    not stored (the exact key list above already holds that no chirp key
    stores them), and §2.5.4 says what resets them.
    """

    def test_the_table_says_the_tiers_are_not_stored(self):
        section = spec_section(SPEC.read_text(encoding="utf-8"), "11.2 NVS Storage")
        block = re.search(r"```\n(.*?)```", section, re.S)
        self.assertIsNotNone(block, "§11.2 has no table block")
        not_stored = re.findall(r"^// (.+?) NOT stored\b", block.group(1), re.M)
        self.assertEqual(not_stored, ["Session identity", "Cooldown tiers"])

    def test_the_reset_counts_from_the_last_send(self):
        section = spec_section(SPEC.read_text(encoding="utf-8"), "2.5.4 Defense: Escalating Cooldowns")
        prose = " ".join(section.split())
        self.assertIn("Reset: 24 hours of no chirps", prose)
        self.assertIn("reset after 24 hours with no send, counted from the last send", prose)
        self.assertIn("A disable and an enable keep them", prose)
        self.assertIn("a reboot still clears them", prose)

    def test_the_count_per_day_is_the_one_the_harness_holds(self):
        # test_chirp_commands_wap.cpp's the_tiers_let_9_out_on_the_first_day_then_6
        # counts these from the COOLDOWN_TIER_* constants on the real channel.
        section = spec_section(SPEC.read_text(encoding="utf-8"), "2.5.4 Defense: Escalating Cooldowns")
        prose = " ".join(section.split())
        self.assertIn("sends 9 chirps in its first 24 hours and then one every 4 hours, 6 a day", prose)
        harness = (REPO / "firmware/projects/canary-wap/tests_host/test_chirp_commands_wap.cpp").read_text(encoding="utf-8")
        self.assertIn("CHECK(sends == (day == 0 ? 9u : 6u));", harness)


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


COMMANDS_TEST = REPO / "firmware/projects/canary-wap/tests_host/test_chirp_commands_wap.cpp"


def budget_rows(section: str) -> dict[str, tuple[int, ...]]:
    """§12.3's rows: {label: (figure...)}, where a row reads "N bytes"
    (N,) or "A * B = C bytes" (A, B, C), and the total "~K KB" (K,)."""
    rows: dict[str, tuple[int, ...]] = {}
    lines = [ln for ln in section.splitlines() if ln.startswith("|")]
    assert lines and lines[0].split("|")[1].strip() == "Component", "§12.3's header row moved"
    for ln in lines[2:]:
        cells = [c.strip() for c in ln.strip("|").split("|")]
        label, value = cells[0].strip("*"), cells[1]
        num = r"(\d{1,3}(?:,\d{3})*)"
        m = (re.fullmatch(num + r" \* " + num + " = " + num + " bytes", value)
             or re.fullmatch(num + " bytes", value)
             or re.fullmatch("~" + num + " KB", value))
        assert m, f"§12.3 row {label!r} reads {value!r}, which this reader cannot spell"
        rows[label] = tuple(int(g.replace(",", "")) for g in m.groups())
    return rows


def budget_asserts(src: str) -> dict[str, tuple[int, ...]]:
    """{row: figure} from each static_assert whose message is "§12.3 <row>":
    the figure is its last comparison's right side, `A * B` or `N`."""
    out: dict[str, tuple[int, ...]] = {}
    for m in re.finditer(r'static_assert\((.*?),\s*"§12\.3 ([^"]+)"\);', strip_comments(src), re.S):
        expr, label = " ".join(m.group(1).split()), m.group(2)
        tail = re.search(r"==\s*(\d+)(?:\s*\*\s*(\d+))?\s*$", expr)
        assert tail, f"the §12.3 assert for {label!r} does not end in == N or == A * B"
        out[label] = tuple(int(g) for g in tail.groups() if g is not None)
    return out


class ChirpSpecMemoryBudget(unittest.TestCase):
    def test_every_row_is_the_harness_figure(self):
        rows = budget_rows(spec_section(SPEC.read_text(encoding="utf-8"), "12.3 Memory Budget"))
        asserted = budget_asserts(COMMANDS_TEST.read_text(encoding="utf-8"))
        self.assertEqual(sorted(rows), sorted(asserted),
                         "every §12.3 row has a static_assert in test_chirp_commands_wap.cpp, and no other")
        total = asserted.pop("Total")
        spec_total = rows.pop("Total")
        added = 0
        for label, figure in rows.items():
            if len(figure) == 3:
                a, b, c = figure
                self.assertEqual(a * b, c, f"§12.3 {label}: {a} * {b} is not {c}")
                self.assertEqual(asserted[label], (a, b), f"§12.3 {label} against its static_assert")
                added += c
            else:
                self.assertEqual(asserted[label], figure, f"§12.3 {label} against its static_assert")
                added += figure[0]
        self.assertEqual(added, total[0], "§12.3's rows do not add up to the asserted total")
        self.assertEqual(round(total[0] / 1024), spec_total[0], "§12.3's total in KB")
        prose = " ".join(spec_section(SPEC.read_text(encoding="utf-8"), "12.3 Memory Budget").split())
        self.assertIn(f"The whole is {total[0]:,} bytes", prose)

    def test_the_nearby_and_recent_rows(self):
        # F311 and F306, as the items left them: a nearby row of 52 bytes, a
        # recent row still 964.
        asserted = budget_asserts(COMMANDS_TEST.read_text(encoding="utf-8"))
        self.assertEqual(asserted["Nearby (presence) table"], (32, 52))
        self.assertEqual(asserted["Recent chirps"], (16, 964))

    def test_the_reader_refuses_what_it_cannot_spell(self):
        with self.assertRaises(AssertionError):
            budget_rows("| Component | Max Size |\n|---|---|\n| X | about 1 KB |\n")
        with self.assertRaises(AssertionError):
            budget_asserts('static_assert(sizeof(T) >= 4, "§12.3 X");')
        self.assertEqual(budget_rows("| Component | Max Size |\n|---|---|\n| X | 2 * 1,024 = 2,048 bytes |\n"
                                     "| **Total** | ~2 KB |\n"),
                         {"X": (2, 1024, 2048), "Total": (2,)})
        self.assertEqual(budget_asserts('static_assert(a == 1 && b == 32 * 52, "§12.3 Y");'), {"Y": (32, 52)})


MESH_H = SKETCH / "mesh_network.h"
THREAT_MODEL = REPO / "docs/security/THREAT_MODEL.md"


def namespace_body(src: str, name: str) -> str:
    """The one `namespace <name> { ... } // namespace <name>` block of `src`
    (comments stripped), or Unreadable."""
    code = strip_comments(src)
    opens = [m.end() for m in re.finditer(r"\bnamespace\s+" + re.escape(name) + r"\s*\{", code)]
    if len(opens) != 1:
        raise Unreadable(f"namespace {name} opens {len(opens)} times; this reader reads one block")
    depth, i = 1, opens[0]
    while i < len(code) and depth:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    if depth:
        raise Unreadable(f"namespace {name} never closes")
    return code[opens[0]:i - 1]


def constant(src: str, name: str) -> int:
    """The value of the one `static const <int type> NAME = <literal>;` (or
    `constexpr`) in `src`. Anything else is refused: no definition, two, or
    one whose value is not a plain decimal or hex literal (an expression or
    another name would be a guess)."""
    code = strip_comments(src)
    found = re.findall(r"\b(?:static\s+const|static\s+constexpr|constexpr)\s+(?:u?int(?:8|16|32)_t|size_t)\s+"
                       + re.escape(name) + r"\s*=\s*([^;]*);", code)
    if len(found) != 1:
        raise Unreadable(f"{name} is defined {len(found)} times; this reader reads one definition")
    value = found[0].strip()
    m = re.fullmatch(r"(0[xX][0-9A-Fa-f]+|[0-9]+)[uU]?", value)
    if m is None:
        raise Unreadable(f"{name} = {value}: a value this reader cannot spell")
    return int(m.group(1), 0)


def spec_defines(section: str) -> dict[str, int]:
    """{name: value} of every `#define NAME VALUE` in the section's code
    block; a define whose value is not a literal is refused."""
    block = re.search(r"```c\n(.*?)```", section, re.S)
    assert block, "the section has no c block"
    out: dict[str, int] = {}
    for line in block.group(1).splitlines():
        code = line.split("//", 1)[0].strip()
        if not code.startswith("#define"):
            continue
        m = re.fullmatch(r"#define\s+(\w+)\s+(0[xX][0-9A-Fa-f]+|[0-9]+)", code)
        assert m, f"a define this reader cannot spell: {code!r}"
        assert m.group(1) not in out, f"{m.group(1)} is defined twice"
        out[m.group(1)] = int(m.group(2), 0)
    return out


def prose(text: str) -> str:
    return " ".join(text.split())


def threat_model_cell(text: str, threat: str, column: str) -> str:
    """The cell of the "Threats covered by mesh layers" table at row `threat`
    and column `column`."""
    lines = [ln for ln in text.splitlines() if ln.startswith("| Threat |")]
    assert len(lines) == 1, "THREAT_MODEL.md's mesh layers table header moved"
    header = [c.strip() for c in lines[0].strip().strip("|").split("|")]
    rows = [ln for ln in text.splitlines() if ln.startswith(f"| {threat} |")]
    assert len(rows) == 1, f"THREAT_MODEL.md has {len(rows)} {threat!r} rows"
    cells = [c.strip() for c in rows[0].strip().strip("|").split("|")]
    assert len(cells) == len(header), f"the {threat!r} row has {len(cells)} cells for {len(header)} columns"
    return cells[header.index(column)]


class ChirpSpecChannel(unittest.TestCase):
    """Chirp shares the radio's channel with Opera (sweep F308).

    The ESP32 has one radio, so a channel pinned for Chirp (6) retuned it
    away from the home Wi-Fi to send (docs/network_coexistence.md), and the
    pins were removed: chirp_channel.cpp registers its broadcast peer at
    CHIRP_CHANNEL, 0, which ESP-NOW reads as the current channel. The spec
    kept channel 6 in three places.
    """

    def setUp(self):
        self.mesh = MESH_H.read_text(encoding="utf-8")
        self.chirp = namespace_body(self.mesh, "chirp_channel")
        self.spec = SPEC.read_text(encoding="utf-8")

    def test_the_spec_defines_the_firmwares_channel(self):
        channel = constant(self.chirp, "CHIRP_CHANNEL")
        cpp = strip_comments((SKETCH / "chirp_channel.cpp").read_text(encoding="utf-8"))
        self.assertEqual(re.findall(r"\bpeer\.channel\s*=\s*([^;]+);", cpp), ["CHIRP_CHANNEL"],
                         "chirp_channel.cpp registers its broadcast peer at CHIRP_CHANNEL, and only there")
        defines = spec_defines(spec_section(self.spec, "12.1 ESP-NOW Configuration for Chirp"))
        self.assertEqual(defines.get("CHIRP_CHANNEL"), channel,
                         "spec §12.1's CHIRP_CHANNEL is not mesh_network.h's")

    def test_the_spec_says_the_channel_is_shared(self):
        # A pinned channel (anything but 0) would be a decision against
        # network_coexistence.md, and the wording below would be false.
        self.assertEqual(constant(self.chirp, "CHIRP_CHANNEL"), 0)
        self.assertEqual(constant(namespace_body(self.mesh, "mesh_network"), "ESPNOW_CHANNEL"), 0)
        fallback = constant((SKETCH / "mesh_channel_policy.h").read_text(encoding="utf-8"),
                            "MESH_FALLBACK_CHANNEL")
        transport = prose(spec_section(self.spec, "2.2 Transport"))
        self.assertIn("**Shared Channel**: Chirp rides the radio's current channel, as Opera does.", transport)
        self.assertIn("at channel 0, which ESP-NOW reads as the current channel", transport)
        self.assertIn(f"the STA's when it is connected, else the AP's, else {fallback}.", transport)
        self.assertIn("docs/network_coexistence.md", transport)
        self.assertNotIn("Separate Channel", transport)
        self.assertNotIn("Uses WiFi channel", transport)
        block = re.search(r"```c\n(.*?)```", spec_section(self.spec, "12.1 ESP-NOW Configuration for Chirp"),
                          re.S).group(1)
        self.assertIn("#define CHIRP_CHANNEL 0  // follow the current radio channel", block)
        self.assertNotIn("different channel", block)
        independence = prose(spec_section(self.spec, "13.1 Independence"))
        self.assertIn("- Same radio channel; Chirp frames are told apart by their magic byte (§12.1)",
                      independence)
        self.assertNotIn("Different channel", independence)

    def test_every_define_is_a_firmware_constant(self):
        section = spec_section(self.spec, "12.1 ESP-NOW Configuration for Chirp")
        defines = spec_defines(section)
        for name, value in defines.items():
            with self.subTest(name=name):
                try:
                    fw = constant(self.chirp, name)
                except Unreadable as e:
                    self.fail(f"spec §12.1 defines {name}, which mesh_network.h's chirp_channel does not ({e})")
                self.assertEqual(value, fw, f"spec §12.1's {name} is not mesh_network.h's")
        self.assertEqual(sorted(defines), ["CHIRP_CHANNEL", "CHIRP_MAGIC"])

    def test_an_opera_frame_starts_with_its_version_or_a_pairing_type(self):
        # A signed Opera frame starts with OPERA_VERSION; the five
        # pre-membership pairing frames start with their type, PAIR_FIRST to
        # PAIR_LAST (mesh_network.cpp classifies those before the version
        # check). The spec names both, and CHIRP_MAGIC is neither.
        wire = (SKETCH / "mesh_wire.h").read_text(encoding="utf-8")
        version = constant(wire, "OPERA_VERSION")
        first, last = constant(wire, "PAIR_FIRST"), constant(wire, "PAIR_LAST")
        section = prose(spec_section(self.spec, "12.1 ESP-NOW Configuration for Chirp"))
        self.assertIn(f"a Chirp frame starts with `CHIRP_MAGIC`, and an Opera frame starts with its "
                      f"protocol version (`OPERA_VERSION`, {version}) or, before membership, its pairing "
                      f"type ({first} to {last}, `PAIR_FIRST` to `PAIR_LAST`; "
                      "`spec/canary_mesh_network_v0.md` §4.5)", section)
        self.assertIn("There is no Opera magic byte", section)
        magic = constant(self.chirp, "CHIRP_MAGIC")
        self.assertNotEqual(magic, version, "the first byte must tell the two apart")
        self.assertFalse(first <= magic <= last, "CHIRP_MAGIC must not be a pairing type")
        pair = (SKETCH / "mesh_pair_frame.h").read_text(encoding="utf-8")
        self.assertIn("constexpr uint8_t TYPE_DISCOVER = mesh_wire::PAIR_DISCOVER;", pair)
        self.assertIn("constexpr uint8_t TYPE_COMPLETE = mesh_wire::PAIR_COMPLETE;", pair)
        self.assertEqual((constant(wire, "PAIR_DISCOVER"), constant(wire, "PAIR_COMPLETE")), (first, last))
        mesh_table = prose(spec_section(MESH_SPEC.read_text(encoding="utf-8"),
                                        "4.5 Wire Type Registry and Outer Frame — v0.4"))
        self.assertIn(f"| `{first}..{last}` | pre-membership pairing, unsigned |", mesh_table)
        cpp = strip_comments((SKETCH / "chirp_channel.cpp").read_text(encoding="utf-8"))
        self.assertIn("if (hdr->magic != CHIRP_MAGIC) return;", cpp)


class ChirpSpecSybilCost(unittest.TestCase):
    """One device makes any number of session keys (sweep D16).

    Each confirmation and vote needs its own signed key, but a key is a
    random seed, so the threshold prices nothing per identity. What a
    confirming key waits is ten minutes of presence (§3.4 rule 4, F311), and
    presence beacons are unsigned (F232), so one radio runs those minutes for
    many keys at once; a suppress vote needs no presence (F234).
    """

    def setUp(self):
        self.spec = SPEC.read_text(encoding="utf-8")
        self.chirp = namespace_body(MESH_H.read_text(encoding="utf-8"), "chirp_channel")

    def test_the_spec_gives_the_threshold_and_the_delay(self):
        needed = constant(self.chirp, "CONFIRMATIONS_REQUIRED")
        safety = constant(self.chirp, "CONFIRMATIONS_SAFETY")
        self.assertEqual(constant(self.chirp, "PRESENCE_REQUIRED_MS"), 10 * 60 * 1000)
        section = prose(spec_section(self.spec, "2.5.3 Defense: Witness Requirement (Sybil Resistance) — v0.2"))
        self.assertIn(f"The threshold needs {needed} signed ACKs ({safety} for safety templates) from session "
                      "keys other than the originator's, and one device can make any number of session keys",
                      section)
        self.assertIn("what a Sybil costs is the ten minutes of presence each confirming key must wait", section)
        self.assertIn("a delay, not a price per identity", section)
        # The ten minutes count from a key's first beacon (§3.4 rule 4), so
        # they run once per key and can run before the chirp they confirm.
        self.assertIn("They run once per key, from its first beacon (a refresh does not move it)", section)
        self.assertIn("one an attack can pay before it starts", section)
        self.assertNotIn("slows a device", section)
        # F232 and F234 are open: the bullet says so, and calls neither fixed.
        self.assertIn("(§3.2; sweep F232, open)", section)
        self.assertIn("(§2.5.6; sweep F234, open)", section)

    def test_no_page_prices_a_sybil(self):
        for heading in ("2.5.3 Defense: Witness Requirement (Sybil Resistance) — v0.2",
                        "2.5.8 Summary: Abuse Prevention Stack"):
            section = prose(spec_section(self.spec, heading))
            with self.subTest(section=heading):
                self.assertNotIn("Cost is real", section)
                self.assertNotIn("3+ devices", section)
                self.assertNotIn("3+ liars", section)
                self.assertNotIn("no solo broadcast", section)
        cell = threat_model_cell(THREAT_MODEL.read_text(encoding="utf-8"), "Sybil flood", "Chirp")
        self.assertTrue(cell.startswith("partial: "), f"the Chirp Sybil cell reads {cell!r}")
        self.assertNotIn("per-pubkey rate + unique-pubkey set", cell)
        self.assertIn("one device makes any number of session keys", cell)
        for item in ("F232", "F311", "F234"):
            self.assertIn(item, cell)
        # The rate limit holds per key, across uptime's wrap too (sweep
        # F416: pubkey_rate_check_and_record() gives up the row whose window
        # started longest ago, by unsigned elapsed time, and update() drops
        # a row whose hour has run out, so none sits long enough for that
        # age to wrap; it picked the smallest start time, and after the wrap
        # the rows written since it held the smallest, so two keys took
        # turns past the limit). What 32 rows cannot do is
        # test_chirp_security.cpp's c14_a_live_hour_goes_only_to_a_full_table.
        limit = constant(self.chirp, "MAX_WITNESSES_PER_PUBKEY_PER_HOUR")
        rows = constant(self.chirp, "MAX_PUBKEY_RATE_TRACK")
        self.assertIn(f"rate-limited ({limit} an hour; a full {rows}-key table gives up its oldest window first, "
                      f"across uptime's wrap too, so a key's count is lost only when {rows} other keys start an "
                      "hour within its own; F416)", cell)
        self.assertNotIn("F416, open", cell)
        self.assertNotIn("take turns past the limit", cell)
        self.assertIn("once per key and possibly before the chirp they confirm", cell)


API_H = SKETCH / "chirp_api.h"
CHIRP_CPP = SKETCH / "chirp_channel.cpp"


def function_bodies(src: str, head: str) -> list[str]:
    """The bodies of every function definition in `src` (comments stripped)
    whose text before its `{` matches the regex `head`, braces in string
    and character literals skipped."""
    code = strip_comments(src)
    out = []
    for m in re.finditer(head + r"\s*\{", code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            c = code[i]
            if c in "\"'":
                j = i + 1
                while j < len(code) and code[j] != c:
                    j += 2 if code[j] == "\\" else 1
                i = j
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        if depth:
            raise Unreadable(f"{head} never closes")
        out.append(code[m.end():i - 1])
    return out


def function_body(src: str, head: str) -> str:
    bodies = function_bodies(src, head)
    if len(bodies) != 1:
        raise Unreadable(f"{head} is defined {len(bodies)} times; this reader reads one definition")
    return bodies[0]


def case_names(src: str, fn: str) -> dict[str, str]:
    """{enumerator: name} for each `case X: return "name";` of `fn`; its
    default is not a name it gives a value."""
    body = function_body(src, r"const char\*\s+" + re.escape(fn) + r"\([^)]*\)")
    names = dict(re.findall(r'\bcase\s+(\w+)\s*:\s*return\s+"([^"\\]+)"\s*;', body))
    if not names:
        raise Unreadable(f"{fn}() has no case this reader can spell")
    return names


def returned_strings(src: str, fn: str) -> set[str]:
    """Every string literal a `return "..."` gives in each definition of `fn`."""
    bodies = function_bodies(src, r"const char\*\s+" + re.escape(fn) + r"\([^)]*\)")
    if not bodies:
        raise Unreadable(f"{fn}() is not defined")
    return {v for b in bodies for v in re.findall(r'\breturn\s+"([^"\\]+)"\s*;', b)}


def enum_values(mesh: str, prefix: str) -> dict[str, int]:
    return {name: int(v, 0) for name, v in re.findall(
        r"\b(" + prefix + r"\w+)\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)(?=\s*[,}])", strip_comments(mesh))}


def table_rows(cpp: str, table: str, row: str) -> list[tuple[str, ...]]:
    """The rows of `static const <T> <table>[] = { ... };`, each matched by
    `row`; a row the pattern cannot spell is refused."""
    m = re.search(r"\b" + re.escape(table) + r"\[\]\s*=\s*\{(.*?)\n\};", strip_comments(cpp), re.S)
    if m is None:
        raise Unreadable(f"{table} moved")
    rows = re.findall(row, m.group(1))
    if len(rows) != m.group(1).count("{"):
        raise Unreadable(f"{table} has a row this reader cannot spell")
    return rows


def templates(cpp: str, mesh: str) -> dict[int, tuple[str, str]]:
    """{template id: (text, ChirpCategory enumerator)} from TEMPLATE_TABLE."""
    ids = enum_values(mesh, "TPL_")
    out = {}
    for name, text, cat in table_rows(cpp, "TEMPLATE_TABLE",
                                      r'\{\s*(TPL_\w+)\s*,\s*"([^"\\]*)"\s*,\s*(CHIRP_CAT_\w+)\s*,'
                                      r"\s*(?:true|false)\s*\}"):
        if name not in ids:
            raise Unreadable(f"TEMPLATE_TABLE names {name}, which mesh_network.h does not give a value")
        out[ids[name]] = (text, cat)
    return out


def details(cpp: str, mesh: str) -> dict[int, str]:
    """{detail slot: text} from DETAIL_TABLE."""
    ids = enum_values(mesh, "DETAIL_")
    out = {}
    for name, text in table_rows(cpp, "DETAIL_TABLE", r'\{\s*(DETAIL_\w+)\s*,\s*"([^"\\]*)"\s*\}'):
        if name not in ids:
            raise Unreadable(f"DETAIL_TABLE names {name}, which mesh_network.h does not give a value")
        out[ids[name]] = text
    return out


def written_keys(body: str, obj: str) -> set[str]:
    """The keys `obj["..."]` names in a handler body; a key that is not a
    string literal is refused."""
    if re.search(r"\b" + obj + r"\[(?!\")", body):
        raise Unreadable(f'{obj}[...] with a key that is not a string literal')
    return set(re.findall(r"\b" + obj + r'\["([A-Za-z0-9_]+)"\]', body))


def route_handlers(api: str) -> dict[tuple[str, str], str]:
    """{(path, method): handler} from chirp_api.h's register_api_handler() calls."""
    return {(path, method): fn for path, method, fn in re.findall(
        r'register_api_handler\(server,\s*"([^"]+)",\s*HTTP_(GET|POST),\s*chirp_auth_gated<(\w+)>\);',
        strip_comments(api))}


def spec_api_examples(section: str) -> dict[tuple[str, str], object]:
    """{(path, method): parsed JSON} for each `// METHOD /path` example of
    the section's one json block; text this reader cannot place is refused."""
    blocks = re.findall(r"```json\n(.*?)```", section, re.S)
    assert len(blocks) == 1, f"the section has {len(blocks)} json blocks; this reader reads one"
    examples: dict[tuple[str, str], object] = {}
    current, lines = None, []

    def flush():
        if current is None:
            assert not "".join(lines).strip(), "json before the first // METHOD /path line"
            return
        assert current not in examples, f"{current} is shown twice"
        try:
            examples[current] = json.loads("\n".join(lines))
        except ValueError as e:
            raise AssertionError(f"the {current} example is not JSON this reader can parse: {e}")

    for line in blocks[0].splitlines():
        m = re.fullmatch(r"// (GET|POST) (/\S+)", line.strip())
        if m:
            flush()
            current, lines = (m.group(2), m.group(1)), []
        else:
            assert not line.strip().startswith("//"), f"a comment this reader cannot place: {line!r}"
            lines.append(line)
    flush()
    return examples


def api_example_problems(spec: str, api: str, cpp: str, mesh: str) -> list[str]:
    """What is wrong with §8.2's examples against chirp_api.h and the
    firmware's tables; empty when nothing is."""
    problems: list[str] = []
    examples = spec_api_examples(spec_section(spec, "8.2 Response Formats"))
    routes = route_handlers(api)
    shown = {("/api/chirp", "GET"), ("/api/chirp/recent", "GET"), ("/api/chirp/send", "POST")}
    if set(examples) != shown:
        problems.append(f"§8.2 shows {sorted(examples)}, not {sorted(shown)}")
    categories = case_names(cpp, "category_name")
    urgencies = set(case_names(cpp, "urgency_name").values())
    states = set(case_names(cpp, "state_name").values())
    statuses = returned_strings(cpp, "get_validation_status") - {"unknown"}
    reasons = returned_strings(cpp, "cannot_send_reason")
    tpl = templates(cpp, mesh)
    det = details(cpp, mesh)

    def same_keys(where: str, got, want: set[str]):
        if not isinstance(got, dict):
            problems.append(f"{where} is not an object")
            return False
        if set(got) != want:
            problems.append(f"{where}: keys {sorted(set(got) - want)} the handler does not have, "
                            f"{sorted(want - set(got))} it has and the example leaves out")
        return True

    def template_ok(where: str, tid, text=None, category=None):
        if tid not in tpl:
            problems.append(f"{where}: template_id {tid!r} is no template")
            return
        t_text, t_cat = tpl[tid]
        if text is not None and text != t_text:
            problems.append(f"{where}: template {tid}'s text is {t_text!r}, not {text!r}")
        if category is not None and category != categories.get(t_cat):
            problems.append(f"{where}: template {tid}'s category is {categories.get(t_cat)!r}, not {category!r}")

    for (path, method), body in sorted(examples.items()):
        where = f"§8.2 {method} {path}"
        handler = routes.get((path, method))
        if handler is None:
            problems.append(f"{where}: chirp_api.h registers no such route")
            continue
        hb = function_body(api, r"inline esp_err_t\s+" + re.escape(handler) + r"\(httpd_req_t\*\s*req\)")
        if path == "/api/chirp":
            if same_keys(where, body, written_keys(hb, "doc")):
                if body.get("state") not in states:
                    problems.append(f"{where}: state {body.get('state')!r} is not one state_name() gives")
                if "cannot_send_reason" in body and body["cannot_send_reason"] not in reasons:
                    problems.append(f"{where}: cannot_send_reason {body['cannot_send_reason']!r} is not one "
                                    "cannot_send_reason() gives")
        elif path == "/api/chirp/recent":
            if not same_keys(where, body, written_keys(hb, "doc")):
                continue
            chirps = body.get("chirps")
            if not isinstance(chirps, list) or not chirps:
                problems.append(f"{where}: no chirp shown")
                continue
            for c in chirps:
                if not same_keys(where + " chirp", c, written_keys(hb, "c")):
                    continue
                if c.get("category") not in categories.values():
                    problems.append(f"{where}: category {c.get('category')!r} is not one category_name() gives")
                if c.get("urgency") not in urgencies:
                    problems.append(f"{where}: urgency {c.get('urgency')!r} is not one urgency_name() gives")
                if c.get("status") not in statuses:
                    problems.append(f"{where}: status {c.get('status')!r} is not one get_validation_status() gives")
                if c.get("detail") not in det.values():
                    problems.append(f"{where}: detail {c.get('detail')!r} is no detail's text")
                template_ok(where, c.get("template_id"), c.get("template_text"), c.get("category"))
        elif path == "/api/chirp/send":
            if same_keys(where, body, written_keys(hb, "input")):
                sendable = set(re.findall(r'strcmp\(urgency_str,\s*"(\w+)"\)', hb))
                sendable |= set(re.findall(r'input\["urgency"\]\s*\|\s*"(\w+)"', hb))
                if body.get("urgency") not in sendable:
                    problems.append(f"{where}: urgency {body.get('urgency')!r} is not one the handler reads")
                if body.get("detail") not in det:
                    problems.append(f"{where}: detail {body.get('detail')!r} is no detail slot")
                template_ok(where, body.get("template_id"))
    return problems


def mockup_problems(spec: str, cpp: str, mesh: str) -> list[str]:
    """What is wrong with §4.2's receive mockup: its category and urgency
    line, and the quoted text under it, must be a template under its own
    category."""
    section = spec_section(spec, "4.2 Receiving a Chirp")
    block = re.search(r"```\n(.*?)```", section, re.S)
    assert block, "§4.2 has no mockup block"
    lines = [ln.strip().strip("\u2502").strip() for ln in block.group(1).splitlines()]
    heads = [i for i, ln in enumerate(lines) if "\u2022" in ln and not ln.startswith("\u23f1")]
    assert len(heads) == 1, f"§4.2's mockup has {len(heads)} category lines; this reader reads one"
    m = re.fullmatch(r"\S+\s+([A-Za-z][A-Za-z ]*?)\s+\u2022\s+([A-Za-z]+)", lines[heads[0]])
    assert m, f"§4.2's category line reads {lines[heads[0]]!r}, which this reader cannot spell"
    q = re.fullmatch(r'"([^"]+)"', lines[heads[0] + 1])
    assert q, f"§4.2's message line reads {lines[heads[0] + 1]!r}, not a quoted template text"
    category, urgency, text = m.group(1).lower().replace(" ", "_"), m.group(2).lower(), q.group(1)
    categories = case_names(cpp, "category_name")
    problems = []
    if category not in categories.values():
        problems.append(f"§4.2's category {m.group(1)!r} is not one category_name() gives")
    if urgency not in set(case_names(cpp, "urgency_name").values()):
        problems.append(f"§4.2's urgency {m.group(2)!r} is not one urgency_name() gives")
    rows = [cat for t, cat in templates(cpp, mesh).values() if t == text]
    if not rows:
        problems.append(f"§4.2's {text!r} is no template's text: a chirp has no free text")
    elif categories.get(rows[0]) != category:
        problems.append(f"§4.2 shows {text!r} under {m.group(1)!r}; its category is {categories.get(rows[0])!r}")
    return problems


def replay_cell_problems(threat: str, cpp: str) -> list[str]:
    """What is wrong with the threat model's Chirp Replay cell against the
    filter chirp_channel.cpp defines."""
    cell = threat_model_cell(threat, "Replay", "Chirp")
    code = strip_comments(cpp)
    bits, hashes = constant(code, "BLOOM_BITS"), constant(code, "BLOOM_HASHES")
    if not re.search(r"\bstatic const size_t BLOOM_BYTES = BLOOM_BITS / 8;", code):
        raise Unreadable("BLOOM_BYTES is no longer BLOOM_BITS / 8")
    if "csi_large_calloc(2 * BLOOM_BYTES)" not in code:
        raise Unreadable("the filter is no longer allocated as two halves of BLOOM_BYTES")
    problems = []
    if bits % (8 * 1024):
        problems.append(f"a half of {bits} bits is not a whole number of KB")
    want = f"a two-half Bloom filter ({bits // 8 // 1024} KB and {hashes} hashes each)"
    for phrase in ("partial: ", want,
                   "takes a nonce only once its frame verifies (F309; a relay with no origin signature counts as "
                   "verified, v0.2's soft accept)",
                   "the freshness window only while the wall clock is synced", "(sweep F310, open)"):
        if phrase not in cell:
            problems.append(f"the Chirp Replay cell does not say {phrase!r}")
    if not cell.startswith("partial: "):
        problems.append("the Chirp Replay cell does not start 'partial: ': F310 is open, replay is not closed")
    for stale in ("1024-entry", "yes ("):
        if stale in cell:
            problems.append(f"the Chirp Replay cell still says {stale!r}")
    return problems


# The line handle_witness() logs where it soft-accepts a relay that names an
# origin key and carries no origin signature (the v0.2 limitation).
SOFT_ACCEPT_LOG = "chirp: relayed witness lacks origin signature (v0.2 limitation)"


def origination_cell_problems(threat: str, cpp: str) -> list[str]:
    """What is wrong with the threat model's Chirp "Spoofed origination" cell
    against handle_witness()'s relay branch. While the code soft-accepts a
    relay that names an origin key and carries no origin signature (it logs
    SOFT_ACCEPT_LOG there), the cell is partial and says so, and the
    Outstanding list still names the origin signature; once the code refuses
    such a relay, the cell may not still say it is accepted. The cell read
    "yes (Ed25519, v0.2)" while a relay could put any template under any
    origin's key (review of sweep F309)."""
    cell = threat_model_cell(threat, "Spoofed origination", "Chirp")
    body = function_body(cpp, r"static void handle_witness\(const uint8_t\* data, size_t len, int8_t rssi\)")
    soft_accepts = f'"{SOFT_ACCEPT_LOG}"' in body
    says_accepted = "no origin signature is soft-accepted on its relayer's signature" in cell
    problems = []
    if soft_accepts:
        if not cell.startswith("partial: "):
            problems.append(f"the Chirp Spoofed origination cell reads {cell!r}, not 'partial: ', while "
                            "handle_witness() soft-accepts a relay without an origin signature")
        for phrase in ("no origin signature is soft-accepted on its relayer's signature",
                       "stored as that origin's chirp with content the origin never signed",
                       "(Outstanding: persistent origin signature across relays)"):
            if phrase not in cell:
                problems.append(f"the Chirp Spoofed origination cell does not say {phrase!r}")
        outstanding = threat.split("### Outstanding work (tracked)", 1)[1]
        if "Persistent origin signature across Chirp relays" not in outstanding:
            problems.append("the Outstanding list no longer names the origin signature across relays")
    elif says_accepted or cell.startswith("partial: "):
        problems.append("handle_witness() no longer soft-accepts a relay without an origin signature; "
                        "the Chirp Spoofed origination cell still says it does")
    return problems


class ChirpSpecApiExamples(unittest.TestCase):
    """§8.2's examples, §4.2's mockup and the Replay and Spoofed origination
    cells are the code (sweep D19; F309's review).

    The examples showed an "activity" category, a free-text `message` and
    an `acks` count, and the send example posted a category and a message:
    Chirp sends templates only, in six categories, and the handlers read
    and write other keys. The threat model's Replay cell gave a
    "1024-entry dedup", the v0.2 array v0.3 replaced with a Bloom filter
    (two halves since F270, verified nonces only since F309), and its
    Outstanding list still named the filter as work to do.
    """

    def setUp(self):
        self.spec = SPEC.read_text(encoding="utf-8")
        self.api = API_H.read_text(encoding="utf-8")
        self.cpp = CHIRP_CPP.read_text(encoding="utf-8")
        self.mesh = MESH_H.read_text(encoding="utf-8")
        self.threat = THREAT_MODEL.read_text(encoding="utf-8")

    def test_every_example_is_its_handler(self):
        self.assertEqual(api_example_problems(self.spec, self.api, self.cpp, self.mesh), [])

    def test_the_mockup_is_a_template(self):
        self.assertEqual(mockup_problems(self.spec, self.cpp, self.mesh), [])

    def test_the_categories_are_the_six(self):
        # §4.2's prose says six; category_name() gives each of them a name.
        self.assertEqual(sorted(case_names(self.cpp, "category_name").values()),
                         ["all_clear", "authority", "emergency", "infrastructure", "mutual_aid", "weather"])
        self.assertIn("six categories are `category_name()`'s", prose(spec_section(self.spec, "4.2 Receiving a Chirp")))

    def test_the_send_mockup_offers_templates(self):
        # §4.1: each picker line is a template's text, and there is no free text.
        section = spec_section(self.spec, "4.1 Sending a Chirp")
        offered = re.findall(r"○\s+(.*?)\s*│", section)
        texts = {text for text, _ in templates(self.cpp, self.mesh).values()}
        self.assertTrue(offered)
        self.assertEqual([o for o in offered if o not in texts], [])
        self.assertNotIn("Brief message", section)

    def test_the_rate_limits_are_the_codes(self):
        # §4.3's numbers are mesh_network.h's.
        chirp = namespace_body(self.mesh, "chirp_channel")
        limits = prose(spec_section(self.spec, "4.3 Rate Limiting"))
        tiers = [constant(chirp, f"COOLDOWN_TIER_{n}_MS") // 60000 for n in (1, 2, 3, 4)]
        self.assertEqual(tiers, [5, 15, 60, 240])
        self.assertIn("5 minutes, 15 minutes, 1 hour, then 4 hours", limits)
        self.assertIn(f"{constant(chirp, 'MAX_WITNESSES_PER_PUBKEY_PER_HOUR')} witnesses an hour", limits)
        self.assertIn(f"at most {constant(chirp, 'MAX_RELAYS_PER_MINUTE')} chirps a minute", limits)
        self.assertNotIn("Per-category", limits)

    def test_the_replay_cell_is_the_filter(self):
        self.assertEqual(replay_cell_problems(self.threat, self.cpp), [])
        outstanding = self.threat.split("### Outstanding work (tracked)", 1)[1]
        self.assertNotIn("Bloom-filter nonce dedup", outstanding)

    def test_the_origination_cell_is_the_soft_accept(self):
        self.assertEqual(origination_cell_problems(self.threat, self.cpp), [])

    def test_the_prose_says_what_a_zero_last_send_means(self):
        # handle_chirp_status() writes g_cooldown.last_chirp_ms, which
        # expire_stale_tiers() zeroes after COOLDOWN_RESET_MS with no send.
        hours = constant(namespace_body(self.mesh, "chirp_channel"), "COOLDOWN_RESET_MS") // 3600000
        self.assertIn(f"0 if it never sent or after {hours} hours with no send",
                      prose(spec_section(self.spec, "8.2 Response Formats")))

    def test_the_spec_says_only_verified_nonces_go_in(self):
        dedup = prose(spec_section(self.spec, "5.3 Deduplication"))
        self.assertIn("Only a verified frame's nonce enters the filter (sweep F309)", dedup)
        self.assertIn("a frame that verifies under any key, an attacker's own among them, takes the nonce it carries",
                      dedup)
        self.assertIn("A relay that names an origin key and carries no origin signature at all takes it too: v0.2 "
                      "still accepts one on its relayer's signature alone and stores it as that origin's chirp with "
                      "content the origin never signed", dedup)
        self.assertIn("Both are known limits of this version.", dedup)
        # handle_witness() runs the freshness window before the per-key
        # rate limit, and the spec says so.
        witness = function_body(self.cpp, r"static void handle_witness\([^)]*\)")
        self.assertLess(witness.index("outside freshness window"), witness.index("pubkey_rate_check_and_record("))
        self.assertIn("// template, freshness, per-key rate limit, store", spec_section(self.spec, "5.3 Deduplication"))
        self.assertIn("The freshness window runs before the rate limit, so a stale frame played back never counts "
                      "against its key's hour.", dedup)
        self.assertNotIn("use up its key's hour", dedup)
        mitigated = prose(spec_section(self.spec, "10.1 Threats Mitigated"))
        self.assertNotIn("5-minute TTL", mitigated)
        self.assertIn("(§5.3; sweep F310, open)", mitigated)


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

    def test_the_constant_reader_refuses_what_it_cannot_spell(self):
        self.assertEqual(constant("static const uint8_t A = 0;  // 0 = follow", "A"), 0)
        self.assertEqual(constant("static const uint8_t A = 0xC4;", "A"), 0xC4)
        self.assertEqual(constant("constexpr uint8_t A = 1;", "A"), 1)
        self.assertEqual(constant("static const uint32_t A = 600000u; /* static const uint8_t A = 6; */", "A"),
                         600000)
        for src in ("static const uint8_t A = 0 + 6;", "static const uint8_t A = B;",
                    "static const uint8_t A = 0;\nstatic const uint8_t A = 6;", "#define A 6",
                    "static const uint8_t AB = 6;"):
            with self.subTest(src=src), self.assertRaises(Unreadable):
                constant(src, "A")

    def test_the_namespace_reader_reads_one_block(self):
        src = "namespace a { int x; namespace b { int y; } }\nnamespace c { static const uint8_t A = 1; }"
        self.assertEqual(constant(namespace_body(src, "c"), "A"), 1)
        with self.assertRaises(Unreadable):
            constant(namespace_body(src, "a"), "A")
        with self.assertRaises(Unreadable):
            namespace_body(src + "\nnamespace c { }", "c")
        with self.assertRaises(Unreadable):
            namespace_body("namespace c { int x;", "c")

    def test_the_define_reader(self):
        self.assertEqual(spec_defines("```c\n#define A 0  // x\n// #define B 9\nint c;\n#define D 0xC4\n```\n"),
                         {"A": 0, "D": 0xC4})
        with self.assertRaises(AssertionError):
            spec_defines("```c\n#define A (1 + 2)\n```\n")
        with self.assertRaises(AssertionError):
            spec_defines("```c\n#define A 1\n#define A 2\n```\n")

    def test_the_old_spec_fails_the_channel_and_magic_pins(self):
        old = ("```c\n// Chirp uses different channel from Opera\n#define CHIRP_CHANNEL 6\n"
               "#define CHIRP_MAGIC 0xC4  // 'Ch' for Chirp\n#define OPERA_MAGIC 0x0A  // 'Op' for Opera\n```\n")
        chirp = "static const uint8_t CHIRP_MAGIC = 0xC4;\nstatic const uint8_t CHIRP_CHANNEL = 0;"
        defines = spec_defines(old)
        self.assertNotEqual(defines["CHIRP_CHANNEL"], constant(chirp, "CHIRP_CHANNEL"))
        with self.assertRaises(Unreadable):
            constant(chirp, "OPERA_MAGIC")

    def test_the_table_cell_reader(self):
        text = ("| Threat | Opera | Chirp | Beacon |\n|---|---|---|---|\n"
                "| Sybil flood | a | yes (x) | c |\n")
        self.assertEqual(threat_model_cell(text, "Sybil flood", "Chirp"), "yes (x)")
        with self.assertRaises(AssertionError):
            threat_model_cell(text + "| Sybil flood | a | b | c |\n", "Sybil flood", "Chirp")
        with self.assertRaises(AssertionError):
            threat_model_cell(text.replace("| c |", "| c | d |"), "Sybil flood", "Chirp")

    def test_the_api_examples_reader_refuses_a_wrong_example(self):
        # The lens on D19: a wrong key, a category category_name() cannot
        # give, a template's text under another category, the old free-text
        # send and a wrong filter size each fail.
        spec, api = SPEC.read_text(encoding="utf-8"), API_H.read_text(encoding="utf-8")
        cpp, mesh = CHIRP_CPP.read_text(encoding="utf-8"), MESH_H.read_text(encoding="utf-8")
        threat = THREAT_MODEL.read_text(encoding="utf-8")
        self.assertEqual(api_example_problems(spec, api, cpp, mesh), [])
        plants = [('"template_text": "power outage"', '"template_txt": "power outage"'),
                  ('"category": "infrastructure"', '"category": "activity"'),
                  ('"template_text": "power outage"', '"template_text": "water service disruption"'),
                  ('"category": "infrastructure"', '"category": "weather"'),
                  ('"urgency": "caution",\n  "detail": 0', '"urgency": "caution",\n  "message": "x"'),
                  ('"state": "cooldown"', '"state": "ACTIVE"'),
                  ('"status": "awaiting_confirmation"', '"status": "pending"')]
        for old, new in plants:
            with self.subTest(plant=new):
                self.assertEqual(spec.count(old), 1, old)
                self.assertNotEqual(api_example_problems(spec.replace(old, new), api, cpp, mesh), [])
        self.assertEqual(mockup_problems(spec, cpp, mesh), [])
        for old, new in (("Infrastructure \u2022", "Activity \u2022"), ('"power outage"   ', '"car doors"      '),
                         ("Infrastructure \u2022", "Weather \u2022")):
            with self.subTest(plant=new):
                self.assertNotEqual(mockup_problems(spec.replace(old, new, 1), cpp, mesh), [])
        self.assertEqual(replay_cell_problems(threat, cpp), [])
        for old, new in (("(4 KB and 4 hashes each)", "(8 KB and 4 hashes each)"),
                         ("(4 KB and 4 hashes each)", "(4 KB and 3 hashes each)"),
                         ("(sweep F310, open)", "(sweep F310)"),
                         ("| partial: a two-half Bloom filter", "| yes (1024-entry dedup + freshness); a two-half Bloom filter")):
            with self.subTest(plant=new):
                self.assertEqual(threat.count(old), 1, old)
                self.assertNotEqual(replay_cell_problems(threat.replace(old, new), cpp), [])
        # And a filter whose size changes in the code fails the cell as written.
        self.assertNotEqual(replay_cell_problems(threat, cpp.replace("BLOOM_BITS = 32768;", "BLOOM_BITS = 65536;")), [])
        # The Replay cell's F309 clause keeps its soft-accept qualification.
        qualified = "(F309; a relay with no origin signature counts as verified, v0.2's soft accept)"
        self.assertEqual(threat.count(qualified), 1)
        self.assertNotEqual(replay_cell_problems(threat.replace(qualified, "(F309)"), cpp), [])
        # The Spoofed origination cell: partial, and only while the code soft-accepts.
        self.assertEqual(origination_cell_problems(threat, cpp), [])
        for old, new in (("| partial: Ed25519 over a witness", "| yes (Ed25519, v0.2); Ed25519 over a witness"),
                         ("no origin signature is soft-accepted on its relayer's signature",
                          "no origin signature is refused"),
                         ("(Outstanding: persistent origin signature across relays)", "(closed)"),
                         ("- Persistent origin signature across Chirp relays:", "- Origin signatures across Chirp relays:")):
            with self.subTest(plant=new):
                self.assertEqual(threat.count(old), 1, old)
                self.assertNotEqual(origination_cell_problems(threat.replace(old, new), cpp), [])
        self.assertEqual(cpp.count(f'"{SOFT_ACCEPT_LOG}"'), 1)
        self.assertNotEqual(origination_cell_problems(
            threat, cpp.replace(f'"{SOFT_ACCEPT_LOG}"', '"chirp: rejected relayed witness"')), [])

    def test_the_old_api_examples_fail(self):
        old = ("### 8.2 Response Formats\n\n```json\n// GET /api/chirp/recent\n"
               '{"chirps": [{"emoji": "x", "category": "activity", "urgency": "caution",\n'
               '  "message": "someone checking car doors", "age_sec": 120, "hop_count": 1, "acks": 3}]}\n\n'
               "// POST /api/chirp/send\n"
               '{"category": "activity", "urgency": "caution", "message": "someone checking car doors",\n'
               '  "ttl_minutes": 15}\n```\n\n## 9.')
        api, cpp, mesh = (API_H.read_text(encoding="utf-8"), CHIRP_CPP.read_text(encoding="utf-8"),
                          MESH_H.read_text(encoding="utf-8"))
        problems = api_example_problems(old, api, cpp, mesh)
        self.assertTrue(any("'message'" in p for p in problems), problems)
        self.assertTrue(any("§8.2 shows" in p for p in problems), problems)

    def test_the_example_readers_refuse_what_they_cannot_spell(self):
        with self.assertRaises(AssertionError):
            spec_api_examples("```json\n// GET /a\n{\"a\": 1,}\n```\n")
        with self.assertRaises(AssertionError):
            spec_api_examples("```json\n// a note\n{}\n```\n")
        with self.assertRaises(AssertionError):
            spec_api_examples("```json\n{}\n// GET /a\n{}\n```\n")
        self.assertEqual(spec_api_examples("```json\n// GET /a\n{\"k\": 1}\n\n// POST /b\n[]\n```\n"),
                         {("/a", "GET"): {"k": 1}, ("/b", "POST"): []})
        with self.assertRaises(Unreadable):
            written_keys('doc[key] = 1; doc["a"] = 2;', "doc")
        self.assertEqual(written_keys('doc["a"] = 1; doc["b"].to<JsonArray>(); c["z"] = 0;', "doc"), {"a", "b"})
        self.assertEqual(function_body('int f() { const char* s = "}"; if (x) { y(); } return 1; }', r"int f\(\)"),
                         ' const char* s = "}"; if (x) { y(); } return 1; ')
        with self.assertRaises(Unreadable):
            function_body("int f() { } int f() { }", r"int f\(\)")
        with self.assertRaises(Unreadable):
            table_rows("static const E T[] = {\n  { A, \"x\" },\n  { B, y },\n};", "T", r'\{\s*(\w+)\s*,\s*"([^"]*)"\s*\}')

    def test_a_stale_or_missing_row_fails(self):
        rows = spec_nvs_rows("```\nNVS Key | Type | Description\n--------|------|---\n"
                             "chirp_relay | uint8 | x\nchirp_enabled | uint8 | y\n```\n")
        code = chirp_keys({"a.cpp": 'void f(){ nvs_set_u8("chirp_relay", 1); nvs_set_u8("chirp_filter", 0); }'})
        self.assertNotEqual(sorted(rows), sorted(code))


if __name__ == "__main__":
    unittest.main()
