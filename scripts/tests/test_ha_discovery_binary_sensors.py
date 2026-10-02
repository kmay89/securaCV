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
field to a JSON boolean on that product's state row, and renders each
template with jinja2 the way Home Assistant does: an
ImmutableSandboxedEnvironment whose undefined logs as HA's LoggingUndefined
does, the row as value/value_json, the result stripped and compared with ==.
jinja2 is pinned in lint.yml's scripts/tests step at Home Assistant's own pin
(package_constraints.txt: Jinja2==3.1.6) and imported unconditionally: a
skipped render test reads as covered while catching nothing.

Run:  python3 -m unittest discover -s scripts/tests -p 'test_ha_discovery_binary_sensors.py' -v
CI:   .github/workflows/lint.yml (unittest discover -s scripts/tests)
"""

from __future__ import annotations

import json
import re
import unittest
from pathlib import Path

import jinja2
from jinja2.sandbox import ImmutableSandboxedEnvironment

REPO = Path(__file__).resolve().parents[2]
PRODUCTS = {
    "canary-vision": {"presence", "dwelling"},
    "canary-sense": {"presence", "radar_link", "breathing"},
    "canary-sentinel": {"presence", "anomaly", "channel_denied"},
}

_LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_string(body: str) -> str:
    """Concatenate the adjacent C string literals in `body` and decode them."""
    out = []
    for lit in _LITERAL.findall(body):
        out.append(re.sub(r"\\(.)", lambda m: {"n": "\n", "t": "\t"}.get(m.group(1), m.group(1)), lit))
    return "".join(out)


def snprintf_call(src: str, start: int) -> tuple[str, list[str]]:
    """The format (decoded) and the argument expressions of the snprintf call
    whose first literal follows `start`: literals up to the first token that
    is not one, then the comma-separated arguments up to the closing paren."""
    i = src.index("snprintf(", start)
    j = src.index(",", src.index(",", i) + 1) + 1  # past the buffer and its size
    k = j
    while True:
        m = re.compile(r'\s*(#[^\n]*\n\s*)*"').match(src, k)
        if not m:
            break
        lit = _LITERAL.match(src, m.end() - 1)
        k = lit.end()
    fmt = c_string(re.sub(r"^\s*#.*$", "", src[j:k], flags=re.M))
    depth, a, args = 0, k, []
    for n in range(k, len(src)):
        ch = src[n]
        if ch == "(":
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


def binary_sensors(product: str) -> dict[str, dict]:
    """object id -> the discovery JSON each binary_sensor announcement publishes."""
    src = (REPO / "firmware/projects" / product / "src/ha/ha_discovery.cpp").read_text(encoding="utf-8")
    dev_fmt, dev_args = snprintf_call(src, src.index("char devObj["))
    avail_fmt, avail_args = snprintf_call(src, src.index("char availObj["))
    ids = {"DEVICE_ID": "canary_test_001", "topics.state": "securacv/canary_test_001/state",
           "topics.status": "securacv/canary_test_001/status"}
    values = dict(ids)
    values["devObj"] = fill(dev_fmt, dev_args, ids)
    values["availObj"] = fill(avail_fmt, avail_args, ids)
    found = {}
    for m in re.finditer(r'topic_for\("binary_sensor", "([a-z_]+)", t, sizeof\(t\)\);', src):
        fmt, args = snprintf_call(src, m.end())
        found[m.group(1)] = json.loads(fill(fmt, args, values))
    return found


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


class HaLog:
    """Home Assistant's LoggingUndefined (helpers/template.py), recording."""

    def __init__(self):
        self.warnings: list[str] = []
        log = self

        class LoggingUndefined(jinja2.Undefined):
            def __str__(self):
                log.warnings.append(self._undefined_message)
                return super().__str__()

            def __iter__(self):
                log.warnings.append(self._undefined_message)
                return super().__iter__()

            def __bool__(self):
                log.warnings.append(self._undefined_message)
                return super().__bool__()

        self.env = ImmutableSandboxedEnvironment(undefined=LoggingUndefined)


def ha_state(entity: dict, payload: str) -> tuple[bool | None, str, list[str]]:
    """What HA's MQTT binary sensor makes of `payload` on the state topic:
    True (on), False (off) or None (no matching payload: left as it was)."""
    ha = HaLog()
    variables = {"value": payload}
    try:
        variables["value_json"] = json.loads(payload)
    except ValueError:
        pass
    rendered = ha.env.from_string(entity["value_template"]).render(**variables).strip()
    if rendered == entity["payload_on"]:
        return True, rendered, ha.warnings
    if rendered == entity["payload_off"]:
        return False, rendered, ha.warnings
    return None, rendered, ha.warnings


def field_of(entity: dict) -> str:
    fields = set(re.findall(r"value_json\.([a-z_]+)", entity["value_template"]))
    assert len(fields) == 1, entity["value_template"]
    return fields.pop()


class EveryBinarySensorTurnsOnAndOff(unittest.TestCase):
    def test_the_announcements_are_the_ones_expected(self):
        for product, want in PRODUCTS.items():
            with self.subTest(product=product):
                self.assertEqual(set(binary_sensors(product)), want)

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
                        want = (not value) if oid == "radar_link" else value  # a problem when NOT ok
                        self.assertIs(state, want)
                        self.assertEqual(warnings, [])

    def test_a_row_without_the_field_reads_off_quietly(self):
        for product in PRODUCTS:
            for oid, entity in binary_sensors(product).items():
                if "| default(false)" not in entity["value_template"]:
                    continue  # radar_link: `not value_json.radar_ok`, a problem if the row lacks it
                with self.subTest(product=product, sensor=oid):
                    state, rendered, warnings = ha_state(entity, '{"device_id":"canary_test_001"}')
                    self.assertIs(state, False, rendered)
                    self.assertEqual(warnings, [], "HA would log a template warning on every row")

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
