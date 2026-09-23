// canary-local/tests/body_dims.test.js — the device sheet's Body row reads the CAD.
//
// registry.json used to carry a hand-typed `body_mm` per display (the Watch's
// drum, the Dash's case) and the sheet printed it. It was a third copy of
// numbers the fleet-figure ledger derives from the CAD, and it had drifted:
// the Dash row said 113.7 x 73.6 x 16.0 while the case had grown corner screw
// lobes, a thicker back and dock pads. These tests hold the replacement:
//   · no registry row carries a typed body size or stand angle again;
//   · every `stand_knob` names a real numeric Customizer knob of that
//     device's enclosure (enclosures.json, parsed from the .scad), and the
//     Watch's is the literal its manifest owns;
//   · the row is built from the device's figure (figures.json), with its
//     dims_source said beside the numbers — measured, board, or sketch.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const registry = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
const ledger = JSON.parse(readFileSync(join(ROOT, "devices/figures.json"), "utf8"));
const enclosures = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
const byId = new Map(registry.devices.map((d) => [d.id, d]));
const same = (mm) => String(mm);

test("no registry row types a body size or a stand angle any more", () => {
  for (const d of registry.devices) {
    assert.ok(!("body_mm" in d), `${d.id} carries body_mm — read the figure ledger instead`);
    assert.ok(!JSON.stringify(d).includes("stand_tilt_deg"),
      `${d.id} types a stand angle — name the case's knob in stand_knob instead`);
  }
});

test("every stand_knob is a real numeric knob of that device's enclosure", async () => {
  const { standTilt } = await import("../assets/body-dims.js");
  let n = 0;
  for (const d of registry.devices) {
    if (!d.stand_knob) continue;
    n++;
    assert.ok(d.enclosure, `${d.id} names a stand knob but no enclosure`);
    const tilt = standTilt(d, enclosures);
    assert.ok(Number.isFinite(tilt) && tilt > 0 && tilt < 90,
      `${d.id}: ${d.enclosure} has no numeric knob "${d.stand_knob}" (got ${tilt})`);
  }
  assert.ok(n >= 2, "the Watch and the Dash both show their stand");
  // the Watch's recline is manifest-owned: the literal the Lab shows is the
  // one gen_cad_params.py proves equal to devices/canary-display-watch
  const manifest = JSON.parse(readFileSync(join(REPO, "devices/canary-display-watch/device.json"), "utf8"));
  assert.strictEqual(standTilt(byId.get("canary-display-watch"), enclosures), manifest.cad.params.tilt);
});

test("the Body row is the figure's envelope, with its source said beside it", async () => {
  const { deviceFigure, bodyText, DIMS_SOURCE } = await import("../assets/body-dims.js");
  for (const src of new Set(ledger.figures.map((f) => f.dims_source))) {
    assert.ok(DIMS_SOURCE[src], `the Body row has words for dims_source "${src}"`);
  }
  const watch = byId.get("canary-display-watch");
  const wf = deviceFigure(ledger, watch.id);
  assert.strictEqual(wf.id, "device.canary-display-watch");
  assert.strictEqual(wf.dims_source, "assembled-cad");
  assert.strictEqual(bodyText(watch, wf, 25, same),
    `Ø ${wf.envelope_mm.w}  ×  ${wf.envelope_mm.d} deep · stand 25° · measured off the CAD · not print-validated`);

  const dash = byId.get("canary-display-dash");
  const df = deviceFigure(ledger, dash.id);
  assert.strictEqual(df.id, "device.canary-display-dash");
  assert.strictEqual(df.dims_source, "assembled-cad", "the Dash is its printed case, not the vendor board");
  const e = df.envelope_mm;
  assert.strictEqual(bodyText(dash, df, null, same),
    `${e.w}  ×  ${e.h}  ×  ${e.d} · measured off the CAD · not print-validated`);

  // a registry id with no `device.<id>` figure resolves to the device figure
  // drawn for it (the S3 nightstand's figure is device.canary-display-nightstand)
  const ns = deviceFigure(ledger, "canary-display-nightstand-s3");
  assert.strictEqual(ns?.id, "device.canary-display-nightstand");
  assert.match(bodyText(byId.get("canary-display-nightstand-s3"), ns, null, same), /sketch · an estimate$/);
  assert.strictEqual(deviceFigure(ledger, "no-such-device"), null);
  assert.strictEqual(deviceFigure(null, watch.id), null);
});
