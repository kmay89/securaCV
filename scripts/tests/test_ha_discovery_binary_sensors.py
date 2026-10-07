#!/usr/bin/env python3
"""The binary sensors canary-vision, canary-sense and canary-sentinel announce
over MQTT discovery render the payload Home Assistant compares.

Each product's src/ha/ha_discovery.cpp announces binary sensors (Presence on
all three; Dwelling on the Vision; Breathing confirmed and Radar link problem
on the Sense; Anomaly and Channel blinded on the Sentinel) with
payload_on "true" and payload_off "false", over the retained state row,
where the field each reads is a JSON boolean (publish_state_retained in the
product's src/net/mqtt_mgr.cpp writes it with %s from `? "true" : "false"`).
Their templates used to be `{{ value_json.<field> | default(false) }}`,
which renders a Python bool: "True" or "False". Home Assistant's MQTT binary
sensor (homeassistant/components/mqtt/binary_sensor.py, 2025.4.4) renders the
template through MqttValueTemplate into Template.async_render_with_possible_
json_value (helpers/template.py: value_json = json_loads(payload), the result
`.strip()`-ed and, with parse_result False, returned as text), then compares
it with plain equality: `payload == payload_on`, `payload == payload_off`.
"True" matches neither, so HA logged "No matching payload found" and the
entity stayed unknown: Presence and Dwelling could never turn on (sweep HA25).

The templates now render the payload strings themselves,
`{{ 'true' if value_json.<field> | default(false) else 'false' }}`. That was
chosen over setting payload_on/payload_off to "True"/"False" because it does
not hang on how the template engine prints a Python bool (a number or a
string field would print differently again), it keeps the payloads in the
JSON spelling the rows use, and with the `| default(false)` inside the test a
row without the field renders "false" without touching an undefined value,
so Home Assistant logs no template warning and a strict environment would not
raise. (The Sense's Radar link problem sensor already rendered strings.)

This test extracts every binary_sensor announcement from the three C++ files
(the snprintf format, decoded and parsed as the JSON it publishes), holds each
to the field it is named for and that field to a JSON boolean on that
product's state row, and renders each template with jinja2 the way Home
Assistant does (_ha_jinja.environment(): HA's sandbox, its LoggingUndefined
and its filters), the row as value/value_json, the result stripped and
compared with ==. Every sensor but the Sense's Radar link problem must read
a row without its field as off with no template warning: that is the reason
the default sits inside the test, so it is held by object id, never by
searching the template for the text that provides it. jinja2 is pinned in
lint.yml's scripts/tests step at Home Assistant's own pin
(package_constraints.txt: Jinja2==3.1.6) and imported unconditionally: a
skipped render test reads as covered while catching nothing.

The longer template made each payload longer, so the announcements are also
built for the longest device id the firmware accepts (runtime_config.h:
device_id[48], so 47 characters) with each product's real manufacturer,
model (every flavor's) and firmware version, devObj and availObj cut at
their own declared sizes as snprintf cuts them, and each must fit the p[]
buffer it is formatted into: at 768 bytes the Vision's Presence JSON was
cut off mid-object from a 40-character id. And devObj and availObj
themselves, formatted whole with their real format strings for that id and
every flavor's model, must fit the buffers they are declared with (sweep
F181): at 256 bytes the Vision's and the Sense's device object was cut
mid-JSON from a 46- and a 44-character id (39 on the Sense wellbeing build),
which made every entity's config invalid.

The Vision's and the Sense's number entities and the Vision's watch profile
select take their fields from tables (NumberEnt numbers[] inside
publish_discovery, WATCH_PROFILES[] in detect_profiles.h), and the payloads
built from them check their own snprintf result and are skipped, with only a
serial log line, when they do not fit: an entity that outgrew its buffer
would simply be missing from Home Assistant. The test reads those tables
(their struct's member order, each row's literals, topics and named integer
constants, and a flavor's `#if` rows only where that flavor builds them),
formats every row for the 47-character id and every flavor's model, and holds
it, and the buffers it is assembled in (the Vision's unitField and options),
to its declared size, which a buffer one byte too small fails (sweep F203).
What the reader cannot follow (an expression it cannot evaluate, another
preprocessor form, an argument it has no value for) it refuses by name.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_ha_discovery_binary_sensors.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

import _ha_jinja

REPO = Path(__file__).resolve().parents[2]
FIRMWARE = REPO / "firmware"
# product -> object id -> the state row field that sensor is named for
PRODUCTS = {
    "canary-vision": {"presence": "presence", "dwelling": "dwelling"},
    "canary-sense": {"presence": "presence", "radar_link": "radar_ok", "breathing": "breathing_locked"},
    "canary-sentinel": {"presence": "presence", "anomaly": "anomaly_active",
                        "channel_denied": "channel_denied"},
}
# The one sensor that is ON when its field is false or missing: a problem
# sensor over `radar_ok` (a missing row field reads as a link problem).
INVERTED = {("canary-sense", "radar_link")}

_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_string(body: str) -> str:
    """Concatenate the adjacent C string literals in `body` and decode them."""
    out = []
    for lit in _LITERAL.findall(body):
        out.append(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), lit))
    return "".join(out)


def named_format(src: str, name: str) -> str:
    """The decoded literal of `static constexpr char <name>[] = "..." "...";`."""
    m = re.search(r"\bconstexpr char %s\[\] =" % re.escape(name), src)
    assert m, name
    return c_string(src[m.end():src.index(";", m.end())])


def snprintf_call(src: str, start: int) -> tuple[str, list[str]]:
    """The format (decoded) and the argument expressions of the snprintf call
    whose first literal follows `start`: literals up to the first token that
    is not one, then the comma-separated arguments up to the closing paren.
    A format named by a constant (the device object's, sweep F181) is that
    constant's literal."""
    i = src.index("snprintf(", start)
    j = src.index(",", src.index(",", i) + 1) + 1  # past the buffer and its size
    named = re.compile(r"\s*(k[A-Z]\w*)\s*,").match(src, j)
    if named:
        _, args = snprintf_call(src[:j] + '"",' + src[named.end():], start)
        return named_format(src, named.group(1)), args
    k = j
    while True:
        m = re.compile(r'\s*(#[^\n]*\n\s*)*"').match(src, k)
        if not m:
            break
        lit = _LITERAL.match(src, m.end() - 1)
        k = lit.end()
    fmt = c_string(re.sub(r"^\s*#.*$", "", src[j:k], flags=re.M))
    depth, a, args, n = 0, k, [], k - 1
    while n + 1 < len(src):
        n += 1
        ch = src[n]
        if ch == '"':  # an argument's string literal: its commas and parens are text
            n = _LITERAL.match(src, n).end() - 1
        elif ch == "(":
            depth += 1
        elif ch == ")":
            if depth == 0:
                args.append(src[a:n])
                break
            depth -= 1
        elif ch == "," and depth == 0:
            args.append(src[a:n])
            a = n + 1
        elif ch == "\n" and src[a:n].strip().startswith("#"):
            a = n + 1
    args = [re.sub(r"^\s*#.*$", "", x, flags=re.M).strip() for x in args]
    return fmt, [x for x in args if x]


_CONV = re.compile(r"%(?:\.\d+)?l*[sdufx]")


def fill(fmt: str, args: list[str], values: dict[str, str]) -> str:
    assert len(_CONV.findall(fmt)) == len(args), (fmt, args)
    it = iter(args)
    return _CONV.sub(lambda m: values.get(next(it), "x"), fmt)


def discovery_source(product: str) -> str:
    return (FIRMWARE / "projects" / product / "src/ha/ha_discovery.cpp").read_text(encoding="utf-8")


def device_objects(product: str, ids: dict[str, str]) -> dict[str, tuple[str, int]]:
    """devObj and availObj, each formatted whole with its real format string
    and arguments, and the size of the buffer it is declared with."""
    src = discovery_source(product)
    out = {}
    for obj in ("devObj", "availObj"):
        decl = re.search(r"char %s\[(\d+)\];" % obj, src)
        fmt, args = snprintf_call(src, decl.start())
        out[obj] = (fill(fmt, args, ids), int(decl.group(1)))
    return out


def announcements(product: str, ids: dict[str, str]) -> dict[str, tuple[str, int]]:
    """object id -> (the bytes each binary_sensor announcement formats, as
    text, and the size of the p[] buffer it is formatted into), with devObj
    and availObj cut at their declared sizes the way snprintf cuts them."""
    src = discovery_source(product)
    values = dict(ids)
    for obj, (text, size) in device_objects(product, ids).items():
        values[obj] = text.encode("utf-8")[:size - 1].decode("utf-8", errors="ignore")
    found = {}
    for m in re.finditer(r'char t\[\d+\], p\[(\d+)\];\s*'
                         r'topic_for\("binary_sensor", "([a-z_]+)", t, sizeof\(t\)\);', src):
        fmt, args = snprintf_call(src, m.end())
        found[m.group(2)] = (fill(fmt, args, values), int(m.group(1)))
    # every binary_sensor announcement was matched with its buffer declaration
    assert len(found) == src.count('topic_for("binary_sensor",'), product
    return found


def topic_values(product: str, device_id: str) -> dict[str, str]:
    """`topics.<name>` -> the topic build_topics (topics.h) writes for `device_id`."""
    src = (FIRMWARE / "projects" / product / "include/canary/topics.h").read_text(encoding="utf-8")
    found = re.findall(r'snprintf\(t\.(\w+),\s*sizeof\(t\.\1\),\s*"([^"]*)",\s*device_id\);', src)
    assert found and all(fmt.count("%s") == 1 for _, fmt in found), product
    return {f"topics.{name}": fmt.replace("%s", device_id) for name, fmt in found}


def every_announcement(product: str, ids: dict[str, str]) -> tuple[dict[str, tuple[str, int]], list[str]]:
    """Every announcement whose arguments are all known (the device id, a
    topic, availObj, devObj or a literal): "<component>/<object id>" -> (the
    payload, the p[] size), with devObj and availObj whole; and the
    announcements left out, whose arguments come from a table (the number
    entities, the Vision's watch profile select): table_announcements()
    formats those (sweep F203)."""
    src = discovery_source(product)
    values = dict(ids)
    for obj, (text, _) in device_objects(product, ids).items():
        values[obj] = text
    found, table = {}, []
    # one line per repetition, so a run of blank lines has one way to match
    for m in re.finditer(r'char t\[\d+\], p\[(\d+)\][^;]*;(?:[^\n]*\n)*?[ \t]*'
                         r'topic_for\("([a-z_]+)", ("[a-z_]+"|[\w.]+), t, sizeof\(t\)\);', src):
        component, oid = m.group(2), m.group(3).strip('"')
        fmt, args = snprintf_call(src, m.end())
        if not m.group(3).startswith('"') or any(a not in values and not re.fullmatch(r'"[^"]*"', a)
                                                 for a in args):
            table.append(f"{component}/{oid}")
            continue
        found[f"{component}/{oid}"] = (fill(fmt, args, values), int(m.group(1)))
    # every announcement was matched with its buffer declaration
    assert len(found) + len(table) == src.count("topic_for(\"") , product
    return found, table


# --------------------------------------------------------------------------- #
# The table-driven announcements (sweep F203): the Vision's and the Sense's
# number entities and the Vision's watch profile select take their fields
# from tables inside publish_discovery (NumberEnt numbers[]) and in
# detect_profiles.h (WATCH_PROFILES[]). Each such payload checks its own
# snprintf result and is skipped, with a serial log line, when it does not
# fit, so an entity that outgrew its buffer would simply be missing from Home
# Assistant. These read the tables the compiler reads, refusing by name what
# they cannot follow rather than guessing at it, and format every row.
# --------------------------------------------------------------------------- #

def strip_comments(text: str) -> str:
    """`text` without its // and /* */ comments; string literals kept whole."""
    out, i, n = [], 0, len(text)
    while i < n:
        if text.startswith("//", i):
            i = text.find("\n", i)
            i = n if i < 0 else i
        elif text.startswith("/*", i):
            i = text.index("*/", i) + 2
            out.append(" ")
        elif text[i] == '"':
            lit = _LITERAL.match(text, i)
            assert lit, text[i:i + 40]
            out.append(lit.group(0))
            i = lit.end()
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def struct_members(src: str, struct: str) -> list[str]:
    """The member names of `struct <struct> { ... };`, in declaration order."""
    m = re.search(r"\bstruct %s\s*\{(.*?)\};" % re.escape(struct), src, re.S)
    assert m, struct
    names = []
    for decl in strip_comments(m.group(1)).split(";"):
        decl = decl.strip()
        if decl:
            parts = [p.strip() for p in decl.split(",")]
            names.append(re.search(r"(\w+)$", parts[0]).group(1))
            names += parts[1:]
    assert names and all(re.fullmatch(r"[A-Za-z_]\w*", x) for x in names), (struct, names)
    return names


def preprocessor_holds(line: str, defines: dict[str, str]) -> bool:
    """`#if defined(X) && X` or `#if X`, against a flavor's #defines; any other
    condition is refused, never guessed."""
    m = re.fullmatch(r"#if\s+defined\((\w+)\)\s*&&\s*(\w+)", line) or re.fullmatch(r"#if\s+(\w+)()", line)
    assert m and (not m.group(2) or m.group(1) == m.group(2)), f"cannot follow {line!r}"
    value = defines.get(m.group(1))
    return value is not None and int(value, 0) != 0


def table_rows(src: str, array: str, defines: dict[str, str]) -> list[list[str]]:
    """The rows of `<array>[] = { {...}, ... };` as lists of field expressions,
    with `#if`/`#endif` blocks kept or dropped for the flavor's #defines."""
    m = re.search(r"\b%s\[\]\s*=\s*\{" % re.escape(array), src)
    assert m, array
    body = strip_comments(src[m.end():])
    rows, row, field, depth, keep, i = [], None, [], 0, [True], 0
    while True:
        ch = body[i]
        if ch == "#" and body[:i].rstrip(" \t").endswith("\n"):
            end = body.index("\n", i)
            line = body[i:end].strip()
            if line.startswith("#if"):
                keep.append(keep[-1] and preprocessor_holds(line, defines))
            elif line == "#endif" and len(keep) > 1:
                keep.pop()
            else:
                raise AssertionError(f"{array}: cannot follow {line!r}")
            i = end
            continue
        if ch == '"':
            lit = _LITERAL.match(body, i)
            field.append(lit.group(0))
            i = lit.end()
            continue
        if ch == "{":
            depth += 1
            if depth == 1:
                row, field = [], []
            else:
                field.append(ch)
        elif ch == "}":
            if depth == 0:
                break  # the array's own closing brace
            depth -= 1
            if depth == 0:
                row.append("".join(field).strip())
                if keep[-1]:
                    rows.append(row)
                row, field = None, []
            else:
                field.append(ch)
        elif ch == "," and depth == 1:
            row.append("".join(field).strip())
            field = []
        elif depth >= 1:
            field.append(ch)
        i += 1
    assert len(keep) == 1, f"{array}: an #if without its #endif"
    return rows


def int_constant(product: str, name: str) -> int:
    """The value of `constexpr <type> <name> = <integer>;` in the product's
    headers (canary/*.h)."""
    for header in sorted((FIRMWARE / "projects" / product / "include/canary").glob("*.h")):
        m = re.search(r"\bconstexpr\s+[\w:]+\s+%s\s*=\s*([^;]+);" % re.escape(name),
                      header.read_text(encoding="utf-8"))
        if m:
            return field_value(product, m.group(1).strip(), {})
    raise AssertionError(f"{product}: no integer constant {name}")


def field_value(product: str, expr: str, topics: dict[str, str]):
    """A table field's value: a string literal, nullptr, `topics.<name>`, an
    integer literal or a named integer constant (through a (long) cast or a
    canary::cfg:: qualifier). Anything else is refused by name."""
    if expr.startswith('"'):
        assert re.fullmatch(r'("(?:[^"\\]|\\.)*"\s*)+', expr), expr
        return c_string(expr)
    if expr == "nullptr":
        return None
    if expr.startswith("topics."):
        assert expr in topics, f"{product}: no topic {expr}"
        return topics[expr]
    m = re.fullmatch(r"(?:\((?:long|int|uint32_t|uint8_t)\)\s*)?(?:canary::cfg::)?(\w+)", expr)
    assert m, f"{product}: cannot evaluate table field {expr!r}"
    token = m.group(1)
    lit = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", token)
    if lit:
        return int(lit.group(1), 0)
    return int_constant(product, token)


def flavor_defines(path: Path) -> dict[str, str]:
    """A flavor config.h's #defines, in order: a quoted #include of another
    flavor's header is read in its place, and #undef drops a name (the
    Sentinel's flavors build on its door flavor this way)."""
    defines: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        inc = re.match(r'\s*#include\s+"([^"]+)"', line)
        if inc:
            defines.update(flavor_defines((path.parent / inc.group(1)).resolve()))
            continue
        undef = re.match(r"\s*#undef\s+(\w+)", line)
        if undef:
            defines.pop(undef.group(1), None)
            continue
        d = re.match(r'\s*#define\s+(\w+)\s+("(?:[^"\\]|\\.)*"|[^\s/]+)', line)
        if d:
            defines[d.group(1)] = d.group(2)
    return defines


def flavors(product: str) -> list[dict]:
    """Each flavor of the product: its name, its config.h #defines, and the
    MANUFACTURER and MODEL it builds with (the project config.h's literal, or
    the flavor macro that constant is set from)."""
    cfg = (FIRMWARE / "projects" / product / "include/canary/config.h").read_text(encoding="utf-8")
    out = []
    for flavor in sorted((FIRMWARE / "configs" / product).glob("*/config.h")):
        defines = flavor_defines(flavor)
        found = {"flavor": flavor.parent.name, "defines": defines}
        for name in ("MANUFACTURER", "MODEL"):
            m = re.search(r"\b%s\s*=\s*(\"[^\"]*\"|[A-Z_]+);" % name, cfg)
            assert m, (product, name)
            if m.group(1).startswith('"'):
                found[name] = m.group(1)[1:-1]
            else:
                value = defines.get(m.group(1), "")
                assert value.startswith('"'), (product, flavor, m.group(1))
                found[name] = c_string(value)
        out.append(found)
    assert out, product
    return out


def watch_profiles(product: str = "canary-vision", defines: dict[str, str] | None = None) -> list[dict]:
    """The product's WATCH_PROFILES rows (detect_profiles.h), by member."""
    profiles = (FIRMWARE / "projects" / product / "include/canary/detect_profiles.h").read_text(encoding="utf-8")
    members = struct_members(profiles, "WatchProfilePreset")
    return [{m: field_value(product, e, {}) for m, e in zip(members, row)}
            for row in table_rows(profiles, "WATCH_PROFILES", defines or {})]


def table_announcements(product: str, ids: dict[str, str], defines: dict[str, str],
                        src: str | None = None) -> dict[str, tuple[str, int]]:
    """Every table-driven announcement of the product, formatted for `ids`
    with the flavor's #defines: "<component>/<object id>" -> (the payload,
    the p[] size). The buffers each payload is assembled from (the Vision's
    unitField and options) are held to their own sizes as "<name>#<buffer>".
    `src` replaces the product's ha_discovery.cpp (the self-test below)."""
    src = discovery_source(product) if src is None else src
    values = dict(ids)
    for obj, (text, _) in device_objects(product, ids).items():
        values[obj] = text
    topics = {k: v for k, v in ids.items() if k.startswith("topics.")}
    out: dict[str, tuple[str, int]] = {}
    # The number entities: NumberEnt numbers[] and the loop that formats it.
    if "NumberEnt numbers[]" in src:
        members = struct_members(src, "NumberEnt")
        loop = src.index("for (const auto& n : numbers) {")
        decl = re.compile(r"char t\[\d+\], p\[(\d+)\](?:, unitField\[(\d+)\] = \"\")?;").search(src, loop)
        assert decl, product
        body_end = src.index("publish_cfg(mqtt, t, p);", loop)
        unit_call = src.find("snprintf(unitField", loop, body_end)
        main_call = src.index("const int written = snprintf(p, sizeof(p),", loop)
        for row in table_rows(src, "numbers", defines):
            assert len(row) == len(members), (product, row)
            ent = {m: field_value(product, e, topics) for m, e in zip(members, row)}
            vals = dict(values)
            vals.update({f"n.{m}": ("" if v is None else str(v)) for m, v in ent.items()})
            if decl.group(2):
                # unitField: "" for a unitless row, else its own snprintf
                assert src[loop:unit_call].rstrip().endswith("if (n.unit) {"), product
                ufmt, uargs = snprintf_call(src, unit_call - 1)
                assert uargs == ["n.unit"], uargs
                unit = "" if ent["unit"] is None else fill(ufmt, uargs, vals)
                out[f"number/{ent['objectId']}#unitField"] = (unit, int(decl.group(2)))
                vals["unitField"] = unit
            else:
                # %s straight from n.unit: a nullptr there would print "(null)" or crash
                assert ent["unit"] is not None, f"{product}: {ent['objectId']} has no unit"
            fmt, args = snprintf_call(src, main_call)
            unknown = [a for a in args if a not in vals]
            assert not unknown, f"{product}: cannot fill {unknown}"
            out[f"number/{ent['objectId']}"] = (fill(fmt, args, vals), int(decl.group(1)))
    # The watch profile select: options[] joined from WATCH_PROFILES' labels.
    if 'topic_for("select", "watch_profile"' in src:
        rows = watch_profiles(product, defines)
        osize = int(re.search(r"char options\[(\d+)\] = \"\";", src).group(1))
        ofmt, oargs = snprintf_call(src, src.index("snprintf(options + off, sizeof(options) - off,") - 1)
        assert oargs == ['i ? "," : ""', "canary::cfg::WATCH_PROFILES[i].label"], oargs
        options = "".join(fill(ofmt, oargs, {oargs[0]: "," if i else "", oargs[1]: r["label"]})
                          for i, r in enumerate(rows))
        out["select/watch_profile#options"] = (options, osize)
        at = src.index('topic_for("select", "watch_profile"')
        size = int(re.findall(r"char t\[\d+\], p\[(\d+)\];", src[:at])[-1])
        fmt, args = snprintf_call(src, at)
        vals = dict(values, options=options)
        unknown = [a for a in args if a not in vals]
        assert not unknown, f"{product}: cannot fill {unknown}"
        out["select/watch_profile"] = (fill(fmt, args, vals), size)
    return out


def longest_ids(product: str, flavor: dict) -> dict[str, str]:
    did = longest_device_id(product)
    return {"DEVICE_ID": did, **topic_values(product, did), "MANUFACTURER": flavor["MANUFACTURER"],
            "MODEL": flavor["MODEL"], "CANARY_FW_VERSION": firmware_version(product)}


def overflows(product: str, src: str | None = None) -> list[tuple[str, str, int, int]]:
    """Every table-driven payload (or the buffer it is built in) that does
    not fit for the longest device id, in any flavor: (flavor, name, bytes
    needed with the terminator, buffer size)."""
    out = []
    for flavor in flavors(product):
        for name, (text, size) in table_announcements(product, longest_ids(product, flavor),
                                                      flavor["defines"], src).items():
            need = len(text.encode("utf-8")) + 1
            if need > size:
                out.append((flavor["flavor"], name, need, size))
    return out


def binary_sensors(product: str) -> dict[str, dict]:
    """object id -> the discovery JSON each binary_sensor announcement publishes."""
    ids = {"DEVICE_ID": "canary_test_001", "topics.state": "securacv/canary_test_001/state",
           "topics.status": "securacv/canary_test_001/status"}
    return {oid: json.loads(text) for oid, (text, _) in announcements(product, ids).items()}


def string_constants(product: str, name: str) -> set[str]:
    """Every value the firmware can give the string constant `name`: its
    literal in the project's config.h, or, when it is set from a flavor macro,
    that macro's value in every flavor's config.h (firmware/configs/<product>)."""
    cfg = (FIRMWARE / "projects" / product / "include/canary/config.h").read_text(encoding="utf-8")
    m = re.search(r"\b%s\s*=\s*(\"[^\"]*\"|[A-Z_]+);" % name, cfg)
    assert m, (product, name)
    if m.group(1).startswith('"'):
        return {m.group(1)[1:-1]}
    found = set()
    for flavor in sorted((FIRMWARE / "configs" / product).glob("*/config.h")):
        found |= set(re.findall(r'#define\s+%s\s+"([^"]*)"' % m.group(1), flavor.read_text(encoding="utf-8")))
    assert found, (product, name, m.group(1))
    return found


def longest_device_id(product: str) -> str:
    rc = (FIRMWARE / "projects" / product / "include/canary/runtime_config.h").read_text(encoding="utf-8")
    size = int(re.search(r"char device_id\[(\d+)\];", rc).group(1))
    return "d" * (size - 1)


def state_row_booleans(product: str) -> set[str]:
    """The keys publish_state_retained writes as a JSON true/false."""
    src = (REPO / "firmware/projects" / product / "src/net/mqtt_mgr.cpp").read_text(encoding="utf-8")
    start = src.index("void publish_state_retained(")
    body = src[start:src.index("\n}\n", start)]
    fmt, args = snprintf_call(body, body.index("(msg, sizeof(msg),") - len("snprintf"))
    convs = _CONV.findall(fmt)
    assert len(convs) == len(args), (product, len(convs), len(args))
    by_conv = []
    pos = 0
    for c in convs:  # pair each conversion with its key (when it has one)
        at = fmt.index(c, pos)
        km = re.search(r'"([a-z_]+)":$', fmt[:at])
        by_conv.append(km.group(1) if km else None)
        pos = at + len(c)
    return {k for k, c, a in zip(by_conv, convs, args)
            if k and c == "%s" and re.fullmatch(r'.+\?\s*"true"\s*:\s*"false"', a)}


def ha_state(entity: dict, payload: str) -> tuple[bool | None, str, list[str]]:
    """What HA's MQTT binary sensor makes of `payload` on the state topic:
    True (on), False (off) or None (no matching payload: left as it was),
    with the template warnings HA would log rendering it."""
    warnings: list[str] = []
    variables = {"value": payload}
    try:
        variables["value_json"] = json.loads(payload)
    except ValueError:
        pass
    env = _ha_jinja.environment(warnings)
    rendered = env.from_string(entity["value_template"]).render(**variables).strip()
    if rendered == entity["payload_on"]:
        return True, rendered, warnings
    if rendered == entity["payload_off"]:
        return False, rendered, warnings
    return None, rendered, warnings


def firmware_version(product: str) -> str:
    ver = (FIRMWARE / "projects" / product / "include/canary/version.h").read_text(encoding="utf-8")
    return re.search(r'#define CANARY_FW_VERSION "([^"]+)"', ver).group(1)


def field_of(entity: dict) -> str:
    fields = set(re.findall(r"value_json\.([a-z_]+)", entity["value_template"]))
    assert len(fields) == 1, entity["value_template"]
    return fields.pop()


class EveryBinarySensorTurnsOnAndOff(unittest.TestCase):
    def test_the_announcements_are_the_ones_expected(self):
        for product, want in PRODUCTS.items():
            with self.subTest(product=product):
                self.assertEqual(set(binary_sensors(product)), set(want))

    def test_each_reads_the_field_it_is_named_for(self):
        for product, want in PRODUCTS.items():
            for oid, entity in binary_sensors(product).items():
                with self.subTest(product=product, sensor=oid):
                    self.assertEqual(field_of(entity), want[oid])

    def test_each_reads_a_json_boolean_off_its_state_row(self):
        for product in PRODUCTS:
            bools = state_row_booleans(product)
            self.assertIn("presence", bools, product)
            for oid, entity in binary_sensors(product).items():
                with self.subTest(product=product, sensor=oid):
                    self.assertEqual(entity["state_topic"], "securacv/canary_test_001/state")
                    self.assertIn(field_of(entity), bools)
                    self.assertEqual((entity["payload_on"], entity["payload_off"]), ("true", "false"))

    def test_the_rows_the_device_sends_turn_each_sensor_on_and_off(self):
        for product in PRODUCTS:
            for oid, entity in binary_sensors(product).items():
                field = field_of(entity)
                for value in (True, False):
                    with self.subTest(product=product, sensor=oid, value=value):
                        # the row as the device writes it: the key and a JSON literal
                        row = '{"device_id":"canary_test_001","%s":%s,"uptime_s":41}' % (
                            field, "true" if value else "false")
                        state, rendered, warnings = ha_state(entity, row)
                        self.assertIsNotNone(state, f"{product} {oid}: HA finds no matching payload "
                                                    f"for {rendered!r} ({entity['value_template']})")
                        want = (not value) if (product, oid) in INVERTED else value  # a problem when NOT ok
                        self.assertIs(state, want)
                        self.assertEqual(warnings, [])

    def test_a_row_without_the_field_reads_off_quietly(self):
        # Held by object id, not by looking for `| default(false)` in the
        # template: a template that drops the default (HA logs a warning on
        # every row without the field) or flips it (`| default(true)` reads
        # such a row as on) must fail here, not be skipped.
        checked = 0
        for product in PRODUCTS:
            for oid, entity in binary_sensors(product).items():
                if (product, oid) in INVERTED:
                    continue  # `not value_json.radar_ok`: a row without it is a link problem
                with self.subTest(product=product, sensor=oid):
                    state, rendered, warnings = ha_state(entity, '{"device_id":"canary_test_001"}')
                    self.assertIs(state, False, rendered)
                    self.assertEqual(warnings, [], "HA would log a template warning on every row")
                checked += 1
        self.assertEqual(checked, sum(len(v) for v in PRODUCTS.values()) - len(INVERTED))

    def test_each_fits_its_buffer_for_the_longest_device_id(self):
        for product in PRODUCTS:
            did = longest_device_id(product)
            self.assertEqual(len(did), 47, product)
            for manufacturer in string_constants(product, "MANUFACTURER"):
                for model in string_constants(product, "MODEL"):
                    ids = {"DEVICE_ID": did, "topics.state": f"securacv/{did}/state",
                           "topics.status": f"securacv/{did}/status", "MANUFACTURER": manufacturer,
                           "MODEL": model, "CANARY_FW_VERSION": firmware_version(product)}
                    for oid, (text, size) in announcements(product, ids).items():
                        with self.subTest(product=product, model=model, sensor=oid):
                            self.assertLess(len(text.encode("utf-8")), size,
                                            f"snprintf cuts the {oid} discovery JSON at {size - 1} bytes")

    def test_the_device_object_fits_for_the_longest_device_id(self):
        # Sweep F181: devObj (and availObj) formatted whole, with the real
        # format strings, the longest id runtime_config.h accepts and each
        # product's manufacturer, every flavor's model and its firmware
        # version, must fit the buffer it is declared with. At 256 bytes the
        # Vision's was cut from a 46-character id and the Sense's from 44 (39
        # on the wellbeing build): snprintf ends it mid-JSON, so every entity
        # that embeds it publishes invalid JSON. The announcements above cut
        # it at its size the way snprintf does; this holds that nothing is cut.
        checked = set()
        for product in PRODUCTS:
            did = longest_device_id(product)
            for manufacturer in string_constants(product, "MANUFACTURER"):
                for model in string_constants(product, "MODEL"):
                    ids = {"DEVICE_ID": did, "topics.status": f"securacv/{did}/status",
                           "MANUFACTURER": manufacturer, "MODEL": model,
                           "CANARY_FW_VERSION": firmware_version(product)}
                    for obj, (text, size) in device_objects(product, ids).items():
                        with self.subTest(product=product, model=model, object=obj):
                            json.loads("{%s}" % text)  # whole, it is the JSON members it should be
                            self.assertIn(did, text)
                            self.assertLess(len(text.encode("utf-8")), size,
                                            f"snprintf cuts {obj} at {size - 1} bytes "
                                            f"({len(text.encode('utf-8'))} needed)")
                        checked.add((product, model, obj))
        self.assertEqual(len(checked), 2 * sum(len(string_constants(p, "MODEL")) for p in PRODUCTS))

    def test_every_announcement_fits_its_buffer_for_the_longest_device_id(self):
        # With the device object whole (sweep F181), each payload carries all
        # of it: every announcement built from the device id, topics and the
        # two objects must still fit its p[] for the longest id and every
        # flavor's model, and parse as JSON. The table-driven ones are named
        # here and formatted by the test after it (sweep F203).
        for product in PRODUCTS:
            did = longest_device_id(product)
            for manufacturer in string_constants(product, "MANUFACTURER"):
                for model in string_constants(product, "MODEL"):
                    ids = {"DEVICE_ID": did, **topic_values(product, did), "MANUFACTURER": manufacturer,
                           "MODEL": model, "CANARY_FW_VERSION": firmware_version(product)}
                    found, table = every_announcement(product, ids)
                    self.assertTrue(all(t.startswith(("number/", "select/")) for t in table), table)
                    self.assertGreaterEqual(len(found), 12, product)
                    for name, (text, size) in found.items():
                        with self.subTest(product=product, model=model, announcement=name):
                            entity = json.loads(text)
                            self.assertEqual(entity["device"]["model"], model)
                            self.assertLess(len(text.encode("utf-8")), size,
                                            f"snprintf cuts the {name} discovery JSON at {size - 1} bytes")

    def test_every_table_driven_announcement_fits_its_buffer_for_the_longest_device_id(self):
        # Sweep F203: the Vision's and the Sense's number entities and the
        # Vision's watch profile select take their fields from tables, so the
        # test above could not format them. Each row is formatted here from
        # the table the compiler reads (a flavor's #if rows only where that
        # flavor builds them), for the longest id and every flavor's model,
        # parsed, and held to its p[] and to the buffers it is assembled in
        # (the Vision's unitField and options). A payload that does not fit
        # is skipped by the firmware with only a serial log line, so the
        # entity would just be missing from Home Assistant.
        labels = [r["label"] for r in watch_profiles()]
        for product in PRODUCTS:
            for flavor in flavors(product):
                ids = longest_ids(product, flavor)
                did = ids["DEVICE_ID"]
                anns = table_announcements(product, ids, flavor["defines"])
                _, table = every_announcement(product, ids)
                # every site the test above leaves out is formatted here
                for site in table:
                    component, oid = site.split("/")
                    if oid.startswith("n."):
                        self.assertTrue(any(k.startswith(component + "/") and "#" not in k for k in anns), site)
                    else:
                        self.assertIn(site, anns)
                numbers = sorted(k[len("number/"):] for k in anns if k.startswith("number/") and "#" not in k)
                vitals = {"cfg_vlock", "cfg_vlost", "cfg_bmin", "cfg_bmax", "cfg_hmin", "cfg_hmax"}
                if product == "canary-vision":
                    self.assertEqual(numbers, ["cfg_dwell", "cfg_lost", "cfg_score", "cfg_target"])
                    self.assertIn("select/watch_profile", anns)
                elif product == "canary-sense":
                    vitals_build = flavor["defines"].get("FEATURE_VITALS", "0") != "0"
                    self.assertEqual(vitals <= set(numbers), vitals_build, (flavor["flavor"], numbers))
                    self.assertEqual(len(numbers), 11 if vitals_build else 5)
                else:
                    self.assertEqual(anns, {}, product)
                for name, (text, size) in anns.items():
                    with self.subTest(product=product, flavor=flavor["flavor"], payload=name):
                        self.assertLess(len(text.encode("utf-8")), size,
                                        f"snprintf cuts {name} at {size - 1} bytes "
                                        f"({len(text.encode('utf-8'))} needed): the entity would be skipped")
                        if "#" in name:
                            continue
                        entity = json.loads(text)
                        self.assertEqual(entity["device"]["model"], flavor["MODEL"])
                        self.assertEqual(entity["unique_id"], f"{did}_{name.split('/')[1]}")
                        if name.startswith("number/"):
                            self.assertLessEqual(entity["min"], entity["max"])
                            self.assertGreater(entity["step"], 0)
                        else:
                            self.assertEqual(entity["options"], labels)

    def test_a_table_driven_buffer_one_byte_too_small_fails(self):
        # The check above reads each buffer's size from the source: shrink a
        # buffer to one byte under what its longest payload needs (the bytes
        # plus the terminator) and overflows() must name that payload; at
        # exactly what it needs, nothing. So a tight buffer cannot pass.
        cases = [
            ("canary-vision", "number/", r"(for \(const auto& n : numbers\) \{\s*char t\[\d+\], p\[)(\d+)(\])"),
            ("canary-vision", "#unitField", r"(unitField\[)(\d+)(\] = \"\";)"),
            ("canary-vision", "select/watch_profile", r"(char t\[\d+\], p\[)(\d+)(\];\s*topic_for\(\"select\", \"watch_profile\")"),
            ("canary-vision", "#options", r"(char options\[)(\d+)(\] = \"\";)"),
            ("canary-sense", "number/", r"(for \(const auto& n : numbers\) \{\s*char t\[\d+\], p\[)(\d+)(\])"),
        ]
        for product, kind, decl in cases:
            src = discovery_source(product)
            need = 0
            for flavor in flavors(product):
                for name, (text, _) in table_announcements(product, longest_ids(product, flavor),
                                                           flavor["defines"]).items():
                    if (name.endswith(kind) if kind.startswith("#") else
                            (name.startswith(kind) and "#" not in name)):
                        need = max(need, len(text.encode("utf-8")) + 1)
            self.assertGreater(need, 1, (product, kind))
            for size, fits in ((need - 1, False), (need, True)):
                with self.subTest(product=product, buffer=kind, size=size):
                    mutated, n = re.subn(decl, lambda m: m.group(1) + str(size) + m.group(3), src)
                    self.assertEqual(n, 1, decl)
                    found = overflows(product, mutated)
                    if fits:
                        self.assertEqual(found, [])
                    else:
                        self.assertTrue(found and all(o[2] == need and o[3] == need - 1 for o in found), found)

    def test_a_table_the_reader_cannot_follow_is_refused(self):
        # The reader formats what the compiler would, or stops and says why:
        # a field it cannot evaluate, a preprocessor line it cannot follow,
        # an argument it has no value for. Each edit below is refused by name
        # rather than formatted with a guess (or left out, reading as fit).
        src = discovery_source("canary-sense")
        flavor = flavors("canary-sense")[-1]
        ids = longest_ids("canary-sense", flavor)
        edits = [
            ("(long)canary::cfg::SENSE_CLEAR_MS_HI, 100}",
             "(long)canary::cfg::SENSE_CLEAR_MS_HI * 2, 100}", "cannot evaluate"),
            ("#if defined(FEATURE_VITALS) && FEATURE_VITALS", "#ifdef FEATURE_VITALS", "cannot follow"),
            ("#endif\n    };", "#else\n#endif\n    };", "cannot follow"),
            ("n.unit, n.icon, availObj, devObj);", "n.unit, n.icon, availObj, devObj, n.extra);", "cannot fill"),
            ('"cm", "mdi:map-marker-radius-outline"', 'nullptr, "mdi:map-marker-radius-outline"', "has no unit"),
        ]
        for old, new, why in edits:
            with self.subTest(edit=new):
                self.assertIn(old, src)
                with self.assertRaisesRegex(AssertionError, why):
                    table_announcements("canary-sense", ids, flavor["defines"], src.replace(old, new, 1))

    def test_the_old_template_never_matched(self):
        # what the three products announced before HA25, rendered the same way
        old = {"value_template": "{{ value_json.presence | default(false) }}",
               "payload_on": "true", "payload_off": "false"}
        for value in ("true", "false"):
            state, rendered, _ = ha_state(old, '{"presence":%s}' % value)
            self.assertIsNone(state)
            self.assertEqual(rendered, value.capitalize())


if __name__ == "__main__":
    unittest.main()
