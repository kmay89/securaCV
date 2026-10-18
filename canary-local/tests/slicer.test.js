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

// C8, closed as intended: the engine is deliberately not vendored, so no
// shipped copy of the Lab can slice. The card used to offer "⚡ slice for
// exact time" anyway, and tell the reader to press it, when the only possible
// answer was "this copy doesn't include it". Now the button and the sentence
// that points at it join the card only when slicerAvailable() says so, and the
// how-to-vendor pointer goes to the console, never to the person printing.
test("no engine: the card offers no slice button; the vendor pointer is console-only", async () => {
  const { SLICER_ABSENT_CONSOLE } = await import("../assets/enclosure-lab.js");
  assert.match(SLICER_ABSENT_CONSOLE, /tools\/vendor_kiri\.sh/);
  assert.match(SLICER_ABSENT_CONSOLE, /vendor\/kiri\/README\.md/);
  assert.match(SLICER_ABSENT_CONSOLE, /modeled estimate stands/);

  const src = require("node:fs").readFileSync(require("node:path").join(__dirname, "../assets/enclosure-lab.js"), "utf8");
  // The row is built, never appended unconditionally: it is placed only in the
  // slicerAvailable() continuation, and so is the sentence telling people to hit it.
  assert.doesNotMatch(src, /card\.append\(sliceRow\)/, "the slice row must not join the card before the engine is known");
  const gate = src.slice(src.indexOf("slicerAvailable().then("));
  assert.ok(gate.length < src.length, "enclosure-lab.js must gate the slice row on slicerAvailable()");
  const body = gate.slice(0, gate.indexOf("\n    });"));
  assert.match(body, /if \(!ok\)[\s\S]*return;[\s\S]*totals\.after\(sliceRow\)/,
    "the row goes in after the totals only when the engine is present");
  assert.match(body, /hit ⚡ slice for exact time/,
    "the provenance's pointer at the button lives inside the same gate");
  const outside = src.replace(body, "");
  assert.doesNotMatch(outside, /hit ⚡ slice for exact time/,
    "no copy outside the gate may tell the reader to press a button that is not there");

  const { readFileSync } = require("node:fs");
  const { join } = require("node:path");
  const readme = readFileSync(join(__dirname, "../assets/vendor/kiri/README.md"), "utf8");
  const head = readme.split("\n## ")[0];
  assert.match(head, /Status \(\d{4}-\d{2}\): deliberately not vendored/,
    "the decision is stated at the top of the vendor README");
  assert.match(head, /SharedArrayBuffer/, "…with its blocker named");
});
