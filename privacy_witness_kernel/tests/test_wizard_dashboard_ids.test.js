// Host test for the kernel wizard's generated Lovelace dashboard
// (privacy_witness_kernel/wizard/index.html: generateDashboard) — repo
// sweep HA16.
//
// The dashboard says it is "Built from your actual zones and entity IDs".
// Its zones are the keys of the kernel's /digest `per_zone`, which are raw
// zone ids ("zone:front_door", src/api/mod.rs), and the page used to make
// each id by replacing every non-alphanumeric with "_": `zone:front_door`
// became sensor.pwk_zone_front_door_events. The kernel's MQTT bridge
// (src/bin/event_mqtt_bridge.rs) drops the "zone:" prefix (extract_zone_name)
// and asks Home Assistant for `sensor.pwk_front_door_events`, its object id
// slugged the way Home Assistant slugs an ASCII name (ha_slug: lowercase,
// each run of anything else one "_", none at either end). So every live zone
// named an entity nobody creates. What this pins:
//   - each zone card names the two ids the bridge asks for, for prefixed
//     zones, dashes, doubled and trailing separators, and a non-ASCII name;
//   - the zone cases of the bridge's own Rust test
//     (zone_entity_ids_are_slugged_like_home_assistant) give the same ids
//     here, read from that file, so the two cannot quietly part;
//   - the four kernel-wide entities stay as the bridge names them, and a
//     page with no zones yet falls back to front_door.
// The section of the page's script from downloadEvents() to copyDashboard()
// is lifted out by literal markers and run against a stub DOM, the way
// test_wizard_mesh_pairing.test.js runs the pairing screens.
//
// Run: node --test privacy_witness_kernel/tests/test_wizard_dashboard_ids.test.js
// (CI: pwk-wizard-tests.yml.)

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const PAGE = process.env.WIZARD_PAGE || join(__dirname, "..", "wizard", "index.html");
const BRIDGE = join(__dirname, "..", "..", "src", "bin", "event_mqtt_bridge.rs");
const src = readFileSync(PAGE, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

const code =
  "let statusZones = globalThis.__zones;\n" +
  slice("function downloadEvents() {", "function copyDashboard() {") +
  "\n;globalThis.__t = { generateDashboard };\n";

// The YAML the page puts in its textarea for these digest zone keys.
function dashboard(zones) {
  const els = {};
  const el = (id) => {
    if (!els[id]) {
      els[id] = { id, value: "", classList: { add() {}, remove() {} } };
    }
    return els[id];
  };
  const ctx = { document: { getElementById: el }, __zones: zones };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  ctx.__t.generateDashboard();
  return el("dashboard-yaml").value;
}

// Every entity id the YAML names, in order.
const entities = (yaml) =>
  [...yaml.matchAll(/^\s*- entity: (\S+)$/gm)].map((m) => m[1]);

// The two ids one zone's card names, read after its title line.
function zoneCard(yaml, zone) {
  const lines = yaml.split("\n");
  const at = lines.indexOf(`        title: ${zone}`);
  assert.ok(at >= 0, `no card titled ${zone}`);
  return lines
    .slice(at + 1, at + 6)
    .map((l) => /^\s*- entity: (\S+)$/.exec(l))
    .filter(Boolean)
    .map((m) => m[1]);
}

const KERNEL_WIDE = [
  "binary_sensor.pwk_chain_problem",
  "button.pwk_verify_now",
  "sensor.pwk_daily_digest",
  "sensor.pwk_last_event",
];

test("each zone card names the ids the bridge asks for", () => {
  const cases = [
    ["zone:front_door", "pwk_front_door"],
    ["zone:back-gate", "pwk_back_gate"],
    ["zone:lot__a-", "pwk_lot_a"],
    ["zone:Garage 2", "pwk_garage_2"],
    ["driveway", "pwk_driveway"],
    ["zone:-porch-", "pwk_porch"],
    // Kept as the bridge keeps it; Home Assistant transliterates it itself.
    ["zone:Café", "pwk_café"],
  ];
  const yaml = dashboard(cases.map(([z]) => z));
  for (const [zone, slug] of cases) {
    assert.deepStrictEqual(
      zoneCard(yaml, zone),
      [`sensor.${slug}_events`, `binary_sensor.${slug}_motion`],
      zone,
    );
  }
  // Nothing the bridge never asks for: no "zone_" left from the prefix, no
  // doubled or edge "_" in an object id.
  for (const id of entities(yaml)) {
    const obj = id.slice(id.indexOf(".") + 1);
    assert.ok(!obj.startsWith("pwk_zone_"), id);
    assert.ok(!/__|^_|_$/.test(obj), id);
  }
});

test("the bridge's own zone cases give the same ids here", () => {
  // The cases table of zone_entity_ids_are_slugged_like_home_assistant:
  // ("Front Door", "Front_Door", "pwk_front_door"), ... The bridge takes the
  // zone name after extract_zone_name, so the digest key carries "zone:".
  const rust = readFileSync(BRIDGE, "utf8");
  const fn = rust.indexOf("fn zone_entity_ids_are_slugged_like_home_assistant");
  assert.ok(fn >= 0, "bridge test not found");
  const body = rust.slice(fn, rust.indexOf("for (zone, clean, slug) in cases", fn));
  const cases = [...body.matchAll(/\("([^"]*)",\s*"[^"]*",\s*"([^"]*)"\)/g)].map((m) => [m[1], m[2]]);
  assert.ok(cases.length >= 4, `read ${cases.length} bridge cases`);
  const yaml = dashboard(cases.map(([z]) => `zone:${z}`));
  for (const [zone, slug] of cases) {
    assert.deepStrictEqual(
      zoneCard(yaml, `zone:${zone}`),
      [`sensor.${slug}_events`, `binary_sensor.${slug}_motion`],
      zone,
    );
  }
});

test("the kernel-wide entities and the no-zone fallback", () => {
  const yaml = dashboard([]);
  assert.deepStrictEqual(entities(yaml), [
    ...KERNEL_WIDE,
    "sensor.pwk_front_door_events",
    "binary_sensor.pwk_front_door_motion",
  ]);
  assert.deepStrictEqual(entities(dashboard(["zone:a"])).slice(0, 4), KERNEL_WIDE);
});
