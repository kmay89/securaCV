// canary-local/tests/catalog_lab.test.js — the enclosure lab's catalog-driven
// configurator logic, pinned. The lab (enclosure-lab.js) now renders live,
// constraint-aware controls from catalog.json instead of read-only param text;
// these tests hold the pure data helpers behind that UI to the manifest:
//   · every committed set maps to a catalog product + variant (so the lab can
//     always find a configurator for what a pill shows)
//   · the OpenSCAD export vector carries the user axes + user options and omits
//     engineering knobs (the audience split, honored on the way out)
//   · variant matching resolves an axis choice back to the committed STL that
//     ships it, or null for a combination we don't ship
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const cat = JSON.parse(readFileSync(join(ROOT, "devices/catalog.json"), "utf8"));
const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));

// The module is ESM; import() it once for the whole suite.
let LAB;
test("load the lab module", async () => {
  LAB = await import("../assets/enclosure-lab.js");
  assert.ok(LAB.catalogVariantIndex && LAB.scadParamVector && LAB.matchVariant);
});

test("every committed set maps to a catalog product + variant", async () => {
  const idx = LAB.catalogVariantIndex(cat);
  for (const s of enc.sets) {
    const hit = idx.get(s.id);
    assert.ok(hit, `set ${s.id} has a catalog product`);
    assert.strictEqual(hit.variant.id, s.id);
    assert.ok(hit.product.scad, `${s.id}: product carries a scad`);
  }
});

test("configurable siblings are the other pinned-selector variants", async () => {
  const idx = LAB.catalogVariantIndex(cat);
  const weather = idx.get("vision-xiao-weather");
  const sibIds = weather.siblings.map((v) => v.id);
  assert.ok(sibIds.includes("vision-xiao-indoor"), "indoor is a sibling flavor");
  assert.ok(sibIds.includes("vision-devkit-indoor"), "devkit is a sibling flavor");
  // vision-mount-kit has empty selects (an addon, not a flavor) → never a sibling
  assert.ok(!sibIds.includes("vision-mount-kit"), "the mount kit is not a flavor");
  assert.ok(!sibIds.includes("vision-xiao-weather"), "not its own sibling");
});

test("the export vector carries user axes + user options, drops engineering", async () => {
  const vision = cat.products.find((p) => p.id === "vision_enclosure");
  const axisVals = { host: "xiao", preset: "vision_weather" };
  const optVals = {};
  for (const o of vision.options) optVals[o.id] = !!o.default;
  const vec = LAB.scadParamVector(vision, axisVals, optVals);
  assert.strictEqual(vec.preset, "vision_weather", "user axis is in the vector");
  assert.strictEqual(vec.host, "xiao", "second user axis is in the vector");
  assert.ok("opt_led" in vec, "a user option is in the vector");
  assert.ok(!("lid_ribs" in vec), "an engineering option is NOT in the vector");
});

test("options seed by type: enum keeps its value, bool coerces to boolean", async () => {
  // mount_style is an enum option on the vision/sense/wap enclosures — its
  // default must survive as a string, not collapse to a boolean.
  const vision = cat.products.find((p) => p.id === "vision_enclosure");
  const mount = vision.options.find((o) => o.id === "mount_style");
  assert.ok(mount && mount.type === "enum", "mount_style is an enum option");
  const seeded = LAB.seedOptionValues(vision.options);
  assert.strictEqual(seeded.mount_style, mount.default,
    "enum option seeds to its string default, not true/false");
  assert.strictEqual(typeof seeded.opt_led, "boolean", "a bool option seeds boolean");
  // engineering options are not part of the user surface
  assert.ok(!("lid_ribs" in seeded), "engineering options are not seeded");
});

test("editing an option flips a named preset to custom (SCAD override safety)", async () => {
  const wap = cat.products.find((p) => p.id === "wap_enclosure");
  // WAP has a preset axis whose enum includes "custom"
  assert.strictEqual(LAB.presetAfterEdit(wap, "battery_full"), "custom",
    "a named preset falls back to custom when an option is edited");
  assert.strictEqual(LAB.presetAfterEdit(wap, "custom"), "custom",
    "already-custom stays custom");
  // a product without a custom-capable preset axis leaves the value alone
  const dash = cat.products.find((p) => p.id === "dash_display");
  assert.strictEqual(LAB.presetAfterEdit(dash, "whatever"), "whatever",
    "no preset axis → nothing to override, value unchanged");
});

test("matchVariant resolves an axis choice to its committed STL, else null", async () => {
  const idx = LAB.catalogVariantIndex(cat);
  const self = idx.get("vision-xiao-weather").variant;
  const sibs = [self, ...idx.get("vision-xiao-weather").siblings];
  assert.strictEqual(
    LAB.matchVariant(sibs, { host: "xiao", preset: "vision_weather" }),
    "vision-xiao-weather", "the exact flavor resolves to its set");
  assert.strictEqual(
    LAB.matchVariant(sibs, { host: "devkit", preset: "vision_indoor" }),
    "vision-devkit-indoor", "another flavor resolves to its set");
  assert.strictEqual(
    LAB.matchVariant(sibs, { host: "devkit", preset: "vision_weather" }),
    null, "a combination we don't ship a mesh for is a custom (null) match");
});
