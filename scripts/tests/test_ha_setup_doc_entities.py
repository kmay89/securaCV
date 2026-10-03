"""docs/homeassistant_setup.md names the entities the kernel's bridge makes.

Sweep HA30: the guide's Template Sensor example read
`sensor.pwk_boundary_crossing_count`, which no publisher creates, and its slug
example named a zone the kernel refuses (`Front Door`). The 2026-10 cleanup
rewrote the example as a REST sensor on the app's `/digest`; this holds the
guide to the sources instead of to a reader's memory of them:

  - every `sensor.` / `binary_sensor.` / `button.` id under `pwk_` the guide
    names is one src/bin/event_mqtt_bridge.rs asks Home Assistant for
    (its `ha_entity_id(...)` calls: the fixed ids, and the per-zone
    `pwk_<zone>_<kind>` ones with `<zone>` a slug);
  - every `zone:` id the guide names passes the kernel's own pattern, read
    from validate_zone_id() in src/lib.rs (lowercased first, as it does), and
    the guide's slug example is what the bridge's slug makes of its zone;
  - the REST Sensor example asks `/digest`, and its value template renders,
    in Home Assistant's template environment (_ha_jinja), to the sum of the
    digest's boundary-crossing types, and to 0 when the digest counted none;
    the event-type keys it reads are the ones the kernel's digest writes
    (`format!("{:?}", ...)` of an EventType variant), every boundary-crossing
    variant and no other.

Read from source; nothing here talks to a running Home Assistant.
"""

from __future__ import annotations

import pathlib
import re
import unittest

import yaml

import _ha_jinja

REPO = pathlib.Path(__file__).resolve().parents[2]
DOC = REPO / "docs" / "homeassistant_setup.md"
BRIDGE = REPO / "src" / "bin" / "event_mqtt_bridge.rs"
LIB = REPO / "src" / "lib.rs"
API = REPO / "src" / "api" / "mod.rs"


def ha_slug(s: str) -> str:
    """event_mqtt_bridge.rs ha_slug(), for ASCII: lowercase, every run of
    anything not a letter or digit one `_`, none at either end."""
    out = []
    gap = False
    for c in s:
        if c.isascii() and not c.isalnum():
            gap = bool(out)
            continue
        if gap:
            out.append("_")
            gap = False
        out.append(c.lower())
    return "".join(out)


def bridge_ids() -> tuple[set[tuple[str, str]], set[tuple[str, str]]]:
    src = BRIDGE.read_text(encoding="utf-8")
    fixed = set(re.findall(r'ha_entity_id\(\s*"(\w+)"\s*,\s*"(pwk_\w+)"\s*\)', src))
    zoned = set(re.findall(r'ha_entity_id\(\s*"(\w+)"\s*,\s*&format!\(\s*"pwk_\{\}_(\w+)"', src))
    return fixed, zoned


def zone_pattern() -> re.Pattern:
    src = LIB.read_text(encoding="utf-8")
    body = src[src.index("pub fn validate_zone_id("):]
    body = body[: body.index("\n}\n")]
    m = re.search(r'Regex::new\(r"([^"]+)"\)', body)
    assert m, "validate_zone_id() builds its pattern with Regex::new(r\"...\")"
    assert "to_lowercase()" in body, "validate_zone_id() lowercases before matching"
    return re.compile(m.group(1))


def doc_section(heading: str) -> str:
    text = DOC.read_text(encoding="utf-8")
    a = text.index(heading)
    b = text.find("\n### ", a + len(heading))
    return text[a: b if b > 0 else len(text)]


class GuideEntityIds(unittest.TestCase):
    def test_bridge_ids_were_read(self):
        fixed, zoned = bridge_ids()
        self.assertIn(("sensor", "pwk_daily_digest"), fixed)
        self.assertIn(("sensor", "pwk_last_event"), fixed)
        self.assertEqual(zoned, {("sensor", "events"), ("binary_sensor", "motion")})

    def test_every_pwk_id_the_guide_names_is_one_the_bridge_makes(self):
        fixed, zoned = bridge_ids()
        text = DOC.read_text(encoding="utf-8")
        named = set(re.findall(r"\b(sensor|binary_sensor|button)\.(pwk_[a-z0-9_<>]+)", text))
        self.assertGreater(len(named), 4, "the guide's ids were read")
        wrong = []
        for domain, obj in sorted(named):
            if (domain, obj) in fixed:
                continue
            m = re.fullmatch(r"pwk_(<zone>|[a-z0-9_]+?)_([a-z]+)", obj)
            ok = bool(m) and (domain, m.group(2)) in zoned and (
                m.group(1) == "<zone>" or ha_slug(m.group(1)) == m.group(1))
            if not ok:
                wrong.append(f"{domain}.{obj}")
        self.assertEqual(wrong, [], "ids the guide names that the bridge never asks for")

    def test_every_zone_id_the_guide_names_is_one_the_kernel_accepts(self):
        pattern = zone_pattern()
        text = DOC.read_text(encoding="utf-8")
        zones = set(re.findall(r"\bzone:[A-Za-z0-9_-][^\s`\"')\],]*", text))
        self.assertTrue(zones, "the guide's zone ids were read")
        refused = sorted(z for z in zones if not pattern.match(z.lower()))
        self.assertEqual(refused, [])

    def test_the_slug_example_is_what_the_bridge_makes(self):
        section = doc_section("### Before the integration claims the kernel")
        m = re.search(r"`zone:([a-z0-9_-]+)` becomes\s+`([a-z0-9_]+)`", section)
        self.assertIsNotNone(m, "the discovery section gives a zone id and its slug")
        zone, slug = m.groups()
        self.assertTrue(zone_pattern().match(f"zone:{zone}"), "the example zone is one the kernel accepts")
        self.assertEqual(ha_slug(zone), slug)


class RestSensorExample(unittest.TestCase):
    def setUp(self):
        section = doc_section("### REST Sensor (Event Count)")
        block = re.search(r"```yaml\n(.*?)```", section, re.S)
        self.assertIsNotNone(block, "the REST Sensor section has a YAML block")
        self.rest = yaml.safe_load(block.group(1))["sensor"][0]

    def test_rest_sensor_asks_the_digest(self):
        self.assertEqual(self.rest["platform"], "rest")
        self.assertTrue(self.rest["resource"].endswith("/digest"))
        self.assertIn("digest", API.read_text(encoding="utf-8"))

    def test_digest_keys_are_event_type_names(self):
        self.assertIn('format!("{:?}", event.event_type)', API.read_text(encoding="utf-8"))
        lib = LIB.read_text(encoding="utf-8")
        enum = lib[lib.index("pub enum EventType {"):]
        enum = enum[: enum.index("\n}")]
        variants = re.findall(r"^\s{4}([A-Z]\w+),", enum, re.M)
        crossing = sorted(v for v in variants if v.startswith("BoundaryCrossing"))
        self.assertEqual(crossing, ["BoundaryCrossingObjectLarge", "BoundaryCrossingObjectSmall"])
        read = sorted(set(re.findall(r"value_json\.event_types\.(\w+)", self.rest["value_template"])))
        self.assertEqual(read, crossing, "the template reads every boundary-crossing type and no other")

    def render(self, value_json):
        warnings: list[str] = []
        env = _ha_jinja.environment(warnings)
        out = env.from_string(self.rest["value_template"]).render(value_json=value_json)
        return out.strip(), warnings

    def test_template_sums_the_boundary_crossing_types(self):
        out, warnings = self.render({"total_events": 13, "event_types": {
            "BoundaryCrossingObjectLarge": 3, "BoundaryCrossingObjectSmall": 2,
            "TamperDetected": 7, "ContactStateChange": 1}})
        self.assertEqual(out, "5")
        self.assertEqual(warnings, [])

    def test_template_reads_zero_when_the_digest_counted_none(self):
        self.assertEqual(self.render({"total_events": 0, "event_types": {}})[0], "0")
        self.assertEqual(self.render({"total_events": 1, "event_types": {"TamperDetected": 1}})[0], "0")


if __name__ == "__main__":
    unittest.main()
