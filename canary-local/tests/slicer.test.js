// canary-local/tests/slicer.test.js — the real-slicer bridge's safety net.
//
// The Kiri:Moto bridge (slicer.js) upgrades ONLY the print-time number, and
// only when an engine is vendored — otherwise the honest estimate stands. This
// test pins the two properties that keep that safe:
//   1. It fails closed — with no vendored engine, loadEngine()/sliceSeconds()
//      resolve to null (never throw), so the UI always has the estimate.
//   2. It doesn't rot — the device/process mapping uses Kiri:Moto's real FDM
//      field names, and the g-code time parse matches Kiri's actual output
//      format (`; --- print time: Ns ---`) and rejects anything malformed.

const { test } = require("node:test");
const assert = require("node:assert");

test("kiriDevice maps our rig onto Kiri's real FDM device fields", async () => {
  const { kiriDevice } = await import("../assets/slicer.js");
  const { MACHINE } = await import("../assets/print-guide.js");
  const dev = kiriDevice(MACHINE);
  assert.strictEqual(dev.bedWidth, 220);
  assert.strictEqual(dev.bedDepth, 220);
  assert.strictEqual(dev.maxHeight, 250);
  assert.strictEqual(dev.extruders[0].extNozzle, 0.4);
  assert.strictEqual(dev.extruders[0].extFilament, 1.75);
});

test("kiriProcess maps settings onto Kiri's real FDM process fields", async () => {
  const { kiriProcess } = await import("../assets/slicer.js");
  const p = kiriProcess({ layer_height_mm: 0.2, walls: 3, infill_pct: 25, top_bottom_layers: 5 }, "PETG");
  assert.strictEqual(p.sliceHeight, 0.2);
  assert.strictEqual(p.sliceShells, 3);
  assert.strictEqual(p.sliceFillSparse, 0.25);
  assert.strictEqual(p.sliceTopLayers, 5);
  assert.strictEqual(p.sliceBottomLayers, 5);
  assert.ok(p.outputFeedrate > 0 && p.outputTemp > 0);
});

test("kiriProcess forces TPU to 100% infill", async () => {
  const { kiriProcess } = await import("../assets/slicer.js");
  assert.strictEqual(kiriProcess({ infill_pct: 25 }, "TPU").sliceFillSparse, 1);
});

test("parseGcodeTime reads Kiri:Moto's actual FDM time comment", async () => {
  const { parseGcodeTime } = await import("../assets/slicer.js");
  // exactly the line src/kiri/mode/fdm/work/export.js writes
  assert.strictEqual(parseGcodeTime("G1 X0\n; --- print time: 4835s ---\nG1 Y0"), 4835);
  assert.strictEqual(parseGcodeTime("; --- print time: 90.5s ---"), 90.5);
});

test("parseGcodeTime fails closed on missing / malformed output", async () => {
  const { parseGcodeTime } = await import("../assets/slicer.js");
  assert.strictEqual(parseGcodeTime("G1 X0 Y0\n; no time here"), null);
  assert.strictEqual(parseGcodeTime(""), null);
  assert.strictEqual(parseGcodeTime(null), null);
  assert.strictEqual(parseGcodeTime("; --- print time: 0s ---"), null); // zero is not sane
});

test("validSliceSeconds rejects junk", async () => {
  const { validSliceSeconds } = await import("../assets/slicer.js");
  assert.ok(validSliceSeconds(120));
  assert.ok(!validSliceSeconds(0));
  assert.ok(!validSliceSeconds(-5));
  assert.ok(!validSliceSeconds(NaN));
  assert.ok(!validSliceSeconds(Infinity));
});

test("no vendored engine ⇒ loadEngine/sliceSeconds resolve null, never throw", async () => {
  const { loadEngine, sliceSeconds, slicerAvailable } = await import("../assets/slicer.js");
  assert.strictEqual(await loadEngine(), null, "engine absent in the test tree");
  assert.strictEqual(await slicerAvailable(), false);
  const bytes = new Uint8Array(84).buffer; // an empty binary-STL header; never reached
  assert.strictEqual(await sliceSeconds(bytes, {}), null, "falls back, no throw");
});
