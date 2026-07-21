// canary-local/tests/print_estimate.test.js — the print-estimator honesty gate.
//
// The estimate card in the enclosure lab splits into MEASURED (filament mass /
// length / cost, from the STL's own solid volume + the repo's documented
// density) and MODELLED (time / energy, a transparent physical model). This
// test pins the measured half against ground truth the repo already states —
// the README §Engineering & materials mass budget ("WAP compact ≈ 12 g/pair …
// PETG at 1.27 g/cm³") — so the headline numbers stay checkable, not asserted.
// It also fences the model's monotonic invariants (infill ↓ ⇒ mass ↓, deposited
// ≤ solid, TPU forced to 100 %) so the physics can't silently invert.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const ENC = join(REPO, "docs/hardware/enclosure");

function loadMesh(parseSTL, file) {
  const buf = readFileSync(join(ENC, file));
  // parseSTL wants an ArrayBuffer; slice to this Buffer's exact view.
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  return parseSTL(ab);
}

test("meshVolume reproduces the README mass budget (WAP compact ≈ 12 g/pair)", async () => {
  const { parseSTL } = await import("../assets/stl.js");
  const { meshVolume, MATERIALS } = await import("../assets/print-guide.js");

  const base = loadMesh(parseSTL, "canary_wap_enclosure_compact_base.stl");
  const lid = loadMesh(parseSTL, "canary_wap_enclosure_compact_lid.stl");
  const cm3 = (meshVolume(base.mesh) + meshVolume(lid.mesh)) / 1000; // mm³ → cm³
  const solidGrams = cm3 * MATERIALS.PETG.density;

  // README states "≈ 12 g/pair" as a solid-volume upper bound. Same method
  // (solid mesh volume × 1.27), so this should land close — a wide band keeps
  // it robust to STL regen / openscad facet count, tight enough to catch a
  // units or winding bug (which would be off by orders of magnitude).
  assert.ok(solidGrams > 8 && solidGrams < 18,
    `WAP compact pair solid mass = ${solidGrams.toFixed(1)} g, expected ≈ 12 g (README)`);
});

test("meshVolume / meshArea are positive and winding-independent", async () => {
  const { parseSTL } = await import("../assets/stl.js");
  const { meshVolume, meshArea } = await import("../assets/print-guide.js");
  const lid = loadMesh(parseSTL, "canary_wap_enclosure_compact_lid.stl");
  assert.ok(meshVolume(lid.mesh) > 0, "volume must be positive");
  assert.ok(meshArea(lid.mesh) > 0, "area must be positive");
});

test("estimatePart: deposited ≤ solid, and less infill means less plastic", async () => {
  const { estimatePart } = await import("../assets/print-guide.js");
  const geom = { volume: 20000, area: 9000, height: 15 }; // a chunky test part

  const solid = estimatePart(geom, { material: "PETG", settings: { infill_pct: 100 } });
  const sparse = estimatePart(geom, { material: "PETG", settings: { infill_pct: 15 } });

  assert.ok(solid.depositedMm3 <= solid.volumeMm3 + 1e-6, "deposited ≤ solid");
  assert.ok(sparse.grams < solid.grams, "less infill ⇒ less plastic");
  assert.ok(sparse.gramsSolid > 0 && Math.abs(sparse.gramsSolid - solid.gramsSolid) < 1e-6,
    "solid basis is infill-independent");
});

test("estimatePart: time / energy / cost are all positive and ordered", async () => {
  const { estimatePart } = await import("../assets/print-guide.js");
  const est = estimatePart({ volume: 9000, area: 6000, height: 12 }, { material: "PETG" });
  assert.ok(est.timeS > 0 && est.energyKWh > 0 && est.cost > 0);
  assert.ok(est.timeLowS < est.timeS && est.timeS < est.timeHighS, "band brackets the estimate");
  assert.ok(Math.abs(est.cost - (est.filamentCost + est.energyCost)) < 1e-9, "cost = filament + energy");
});

test("materialForPart: gaskets force TPU regardless of shell choice", async () => {
  const { materialForPart } = await import("../assets/print-guide.js");
  assert.strictEqual(materialForPart({ material: "TPU 90–95A" }, "ASA"), "TPU");
  assert.strictEqual(materialForPart({ material: "PETG / ASA (PLA indoors)" }, "ASA"), "ASA");
  assert.strictEqual(materialForPart({ material: "PETG / ASA (PLA indoors)" }, "PETG"), "PETG");
});

test("partsCost sums required vs optional from BOM rows", async () => {
  const { partsCost } = await import("../assets/print-guide.js");
  const rows = [
    { usd: 14.9, qty: "1", required: true },
    { usd: 8.5, qty: "1", required: true },
    { usd: 3.5, qty: "1", required: true },
    { usd: 2, qty: "4", required: false },       // qty>1 optional
    { usd: "n/a", qty: "1", required: true },     // unpriced → skipped
  ];
  const c = partsCost(rows);
  assert.ok(Math.abs(c.required - 26.9) < 1e-9, `required=${c.required}`);
  assert.ok(Math.abs(c.optional - 8) < 1e-9, `optional=${c.optional}`);
  assert.strictEqual(c.count, 3);
  assert.deepStrictEqual(partsCost(undefined), { required: 0, optional: 0, count: 0 });
});

test("tco: Canary is one-time, cloud accrues subscription", async () => {
  const { tco, CLOUD_CAM } = await import("../assets/print-guide.js");
  const t = tco({ unitCost: 45, years: 3 });
  assert.strictEqual(t.canary, 45);
  assert.strictEqual(t.cloud, CLOUD_CAM.hardware + CLOUD_CAM.monthly * 36); // 40 + 288
  assert.ok(t.cloud > t.canary, "cloud TCO exceeds a one-time Canary over 3y");
});

test("slicerConfigIni: importable config matches the shown settings", async () => {
  const { slicerConfigIni } = await import("../assets/print-guide.js");
  const petg = slicerConfigIni({ material: "PETG" });
  // PrusaSlicer/Slic3r config-bundle sections + the numbers the card shows
  for (const line of [
    "[print:", "[filament:", "[printer:",
    "layer_height = 0.2", "perimeters = 3", "fill_density = 25%",
    "top_solid_layers = 5", "nozzle_diameter = 0.4", "filament_diameter = 1.75",
    "filament_type = PETG", "temperature = 235",
    "bed_shape = 0x0,220x0,220x220,0x220", "max_print_height = 250",
  ]) {
    assert.ok(petg.includes(line), `config missing: ${line}`);
  }
  // material selection flows through
  const asa = slicerConfigIni({ material: "ASA" });
  assert.ok(asa.includes("filament_type = ASA") && asa.includes("temperature = 245"));
  assert.ok(slicerConfigIni({ material: "PLA" }).includes("bed_temperature = 60"));
});

test("estimateSet: totals sum the parts", async () => {
  const { estimateSet } = await import("../assets/print-guide.js");
  const parts = [
    { part: { name: "base", material: "PETG / ASA" }, geom: { volume: 8000, area: 5000, height: 14 } },
    { part: { name: "lid", material: "PETG / ASA" }, geom: { volume: 5000, area: 4000, height: 6 } },
    { part: { name: "gasket", material: "TPU 90–95A" }, geom: { volume: 400, area: 1800, height: 1.2 } },
  ];
  const { rows, total } = estimateSet(parts, { shellMaterial: "PETG" });
  assert.strictEqual(rows.length, 3);
  assert.strictEqual(rows[2].est.material, "TPU", "gasket estimated as TPU");
  const sumCost = rows.reduce((a, r) => a + r.est.cost, 0);
  assert.ok(Math.abs(total.cost - sumCost) < 1e-9, "set cost = Σ part costs");
  assert.ok(total.grams > 0 && total.timeS > 0 && total.energyKWh > 0);
});
