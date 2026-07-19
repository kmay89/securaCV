// canary-local/tests/workshop.test.js — the workshop's promises, pinned.
// The configurator claims "what you see is what you get"; these tests hold
// the generated data to it: every package resolves to a real variant set,
// every part file exists on disk, every option's BOM ref exists in the
// parsed BOM, every firmware flag exists in the parsed configs, and the
// preset option vectors match what the README says is on each preset.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const ENC = join(ROOT, "../docs/hardware/enclosure");

const ws = JSON.parse(readFileSync(join(ROOT, "devices/workshop.json"), "utf8"));
const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
const build = JSON.parse(readFileSync(join(ROOT, "devices/build.json"), "utf8"));
const setIds = new Set(enc.sets.map((s) => s.id));

test("every package resolves to a real enclosure set with parts on disk", () => {
  for (const [dev, d] of Object.entries(ws.devices)) {
    for (const p of d.packages || []) {
      assert.ok(setIds.has(p.set), `${dev}/${p.id}: set ${p.set} exists`);
      for (const part of p.parts || []) {
        if (part.preview_mesh) continue; // in-dev preview meshes live in canary-local
        assert.ok(existsSync(join(ENC, part.file)), `${dev}/${p.id}: ${part.file}`);
      }
    }
    for (const a of d.addons || []) {
      assert.ok(setIds.has(a.set), `${dev}/addon ${a.id}: set ${a.set} exists`);
    }
  }
});

test("option links are live: BOM refs exist, firmware flags exist", () => {
  for (const [dev, d] of Object.entries(ws.devices)) {
    const refs = new Set((build.devices?.[dev]?.bom?.rows || []).map((r) => r.ref));
    const flags = new Set();
    for (const fl of Object.values(d.firmware.flavors)) {
      for (const f of Object.keys(fl)) flags.add(f);
    }
    for (const o of d.options || []) {
      for (const ref of o.bom || []) {
        assert.ok(refs.has(ref), `${dev}/${o.id}: BOM ref ${ref} exists`);
      }
      for (const f of o.fw || []) {
        assert.ok(flags.has(f), `${dev}/${o.id}: flag ${f} exists in configs`);
      }
    }
  }
});

test("wap presets carry the README's own option story", () => {
  const wap = ws.devices["canary-wap"];
  const by = Object.fromEntries(wap.packages.map((p) => [p.id, p.options]));
  assert.strictEqual(wap.packages.length, 3);
  // compact: plain board — no camera, no battery, no gps
  assert.strictEqual(by.compact_plain.opt_camera, false);
  assert.strictEqual(by.compact_plain.opt_battery, false);
  assert.strictEqual(by.compact_plain.opt_buzzer, true);
  // battery_full: the loaded indoor build
  for (const o of ["opt_camera", "opt_battery", "opt_gps", "opt_tamper"]) {
    assert.strictEqual(by.battery_full[o], true, `battery_full ${o}`);
  }
  assert.strictEqual(by.battery_full.opt_seal, false);
  // battery_weather inherits battery_full and ADDS seal + mount
  for (const o of ["opt_camera", "opt_battery", "opt_gps", "opt_seal", "opt_mount"]) {
    assert.strictEqual(by.battery_weather[o], true, `battery_weather ${o}`);
  }
  // rendered dims quoted from the README, mm triplets
  for (const p of wap.packages) assert.match(p.dims_mm, /×.*×.*mm/);
});

test("gps option: the flagship link chain is intact end to end", () => {
  const gps = ws.devices["canary-wap"].options.find((o) => o.id === "opt_gps");
  assert.ok(gps, "opt_gps parsed from the scad");
  assert.match(gps.label, /L76K|GPS/i);
  assert.deepStrictEqual(gps.bom, ["M1"]);
  assert.deepStrictEqual(gps.fw, ["FEATURE_GNSS"]);
  const m1 = build.devices["canary-wap"].bom.rows.find((r) => r.ref === "M1");
  assert.match(m1.desc, /L76K/);
  assert.strictEqual(
    ws.devices["canary-wap"].firmware.flavors.default.FEATURE_GNSS.on, true);
});

test("drill templates exist and every device reference is a real device", () => {
  for (const [file, t] of Object.entries(ws.templates)) {
    assert.ok(existsSync(join(ENC, file)), file);
    for (const dev of t.devices) assert.ok(ws.devices[dev], `${file} → ${dev}`);
  }
  assert.match(ws.calibration_note, /20 mm|100%/);
});

test("recipes quote the BOM's own summary rows, prices included", () => {
  const r = ws.devices["canary-wap"].recipes;
  assert.ok(r.length >= 3, "wap recipes parsed");
  const weather = r.find((x) => /weather kit/i.test(x.label));
  assert.ok(weather, "weather kit recipe");
  assert.match(weather.formula, /FIL1/);
  assert.ok(weather.usd > 0, "indicative price rides along");
});

test("parameter-set names read at a glance in a crowded Downloads folder", async () => {
  const { specName } = await import("../assets/workshop.js");
  const pkg = { id: "battery_weather" };
  assert.strictEqual(
    specName("canary-wap", { exact: true, pkg }, {}),
    "canary-wap_battery-weather_openscad-params");
  assert.strictEqual(
    specName("canary-wap", { exact: false, pkg },
      { opt_gps: true, opt_battery: true, opt_seal: true, opt_camera: false }),
    "canary-wap_custom-gps-batt-seal_openscad-params");
  assert.strictEqual(
    specName("canary-wap", { exact: false, pkg: null }, {}),
    "canary-wap_custom-bare_openscad-params");
});

test("mesh volume: a closed 10 mm cube measures exactly 1 cm³", async () => {
  const { meshVolumeCm3 } = await import("../assets/workshop.js");
  const pos = [];
  const idx = [];
  const v = (x, y, z) => { pos.push(x, y, z); return pos.length / 3 - 1; };
  const corners = [];
  for (const z of [0, 10]) for (const y of [0, 10]) for (const x of [0, 10]) {
    corners.push(v(x, y, z));
  }
  // 12 triangles, outward-wound (volume is |signed|, winding just must be consistent)
  const quads = [
    [0, 1, 3, 2], [4, 6, 7, 5], [0, 2, 6, 4], [1, 5, 7, 3], [0, 4, 5, 1], [2, 3, 7, 6],
  ];
  for (const [a, b, c, d] of quads) idx.push(a, b, c, a, c, d);
  const vol = meshVolumeCm3({ pos, idx });
  assert.ok(Math.abs(vol - 1) < 1e-9, `1 cm³, got ${vol}`);
});

test("real STL: the wap compact base has a sane volume for its dims", async () => {
  const { meshVolumeCm3 } = await import("../assets/workshop.js");
  const { parseSTL } = await import("../assets/stl.js");
  const buf = readFileSync(join(ENC, "canary_wap_enclosure_compact_base.stl"));
  const { mesh, bbox } = parseSTL(
    buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
  const vol = meshVolumeCm3(mesh);
  const bboxVol = (bbox.size[0] * bbox.size[1] * bbox.size[2]) / 1000;
  assert.ok(vol > 1 && vol < bboxVol,
    `walls-and-floor volume (${vol.toFixed(1)} cm³) sits inside its bbox (${bboxVol.toFixed(1)} cm³)`);
});
