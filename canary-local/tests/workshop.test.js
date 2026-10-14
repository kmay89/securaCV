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

test("every workshop option is tagged user-audience (drives the audience filter, not opt_)", () => {
  for (const [dev, d] of Object.entries(ws.devices)) {
    for (const o of d.options || []) {
      assert.strictEqual(o.audience, "user",
        `${dev}/${o.id}: workshop options are user-facing (opt_* + mount_style)`);
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

// The WAP's flags are its sketch's build_config.h — the BUILD_PROFILE_FULL
// block for the default XIAO ESP32-S3, one "full" flavor (sweep F258). They
// used to come from firmware/configs/canary-wap/, which no build compiled and
// whose FEATURE_GNSS / FEATURE_CHIRP the sketch never read.
const WAP_SKETCH = join(ROOT, "../firmware/projects/canary-wap/arduino/canary_wap");
const wapFullBlock = () => {
  // The body of each `#elif defined(BUILD_PROFILE_FULL)`, to the #elif /
  // #else / #endif at its own depth (the block nests #ifndef defaults).
  const lines = readFileSync(join(WAP_SKETCH, "build_config.h"), "utf8").split("\n");
  const blocks = [];
  lines.forEach((l, i) => {
    if (!/^\s*#\s*elif\s+defined\(BUILD_PROFILE_FULL\)\s*$/.test(l)) return;
    let depth = 0;
    const body = [];
    for (const m of lines.slice(i + 1)) {
      const d = /^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b/.exec(m)?.[1];
      if (d && d.startsWith("if")) depth++;
      else if (d === "endif" && depth) depth--;
      else if (d && depth === 0) break;
      body.push(m);
    }
    blocks.push(body.join("\n"));
  });
  const setting = blocks.filter((b) => /#\s*define\s+FEATURE_/.test(b));
  assert.strictEqual(setting.length, 1, "build_config.h has one FULL block that sets FEATURE_*");
  return setting[0];
};

test("the WAP's firmware flavor is build_config.h's FULL profile, flag for flag", () => {
  const fw = ws.devices["canary-wap"].firmware;
  assert.strictEqual(fw.source, "firmware/projects/canary-wap/arduino/canary_wap/build_config.h");
  assert.ok(existsSync(join(ROOT, "..", fw.source)), `${fw.source} exists`);
  assert.deepStrictEqual(Object.keys(fw.flavors), ["full"], "one flavor: what a stock WAP compiles");
  // Every FEATURE_* the FULL block sets is on the page, in the block's order,
  // and nothing else is (canary-local/tools/tests/test_wap_profile.py holds
  // each value to the C preprocessor).
  const inBlock = [...new Set([...wapFullBlock().matchAll(/#\s*define\s+(FEATURE_\w+)/g)].map((m) => m[1]))];
  assert.deepStrictEqual(Object.keys(fw.flavors.full), inBlock);
  assert.ok(!existsSync(join(ROOT, "../firmware/configs/canary-wap")),
    "firmware/configs/canary-wap/ is gone: no build compiled it, and nothing reads it");
});

test("buzzer option: the piezo's flag is the one the sketch gates it on", () => {
  const buzzer = ws.devices["canary-wap"].options.find((o) => o.id === "opt_buzzer");
  assert.ok(buzzer, "opt_buzzer parsed from the scad");
  assert.deepStrictEqual(buzzer.fw, ["FEATURE_AUDIBLE_CHIRP"]);
  assert.strictEqual(ws.devices["canary-wap"].firmware.flavors.full.FEATURE_AUDIBLE_CHIRP.on, true);
  // audible_chirp.h is the buzzer driver (CHIRP_GPIO, LEDC tones), and it
  // compiles only under that flag.
  const driver = readFileSync(join(WAP_SKETCH, "audible_chirp.h"), "utf8");
  assert.match(driver, /^#if FEATURE_AUDIBLE_CHIRP$/m);
  assert.match(driver, /#define CHIRP_GPIO/);
});

test("gps option: BOM link intact, and no firmware flag, because GNSS has none", () => {
  const gps = ws.devices["canary-wap"].options.find((o) => o.id === "opt_gps");
  assert.ok(gps, "opt_gps parsed from the scad");
  assert.match(gps.label, /L76K|GPS/i);
  assert.deepStrictEqual(gps.bom, ["M1"]);
  assert.strictEqual(gps.fw, undefined, "the GPS option links no flag: the sketch has none to flip");
  const m1 = build.devices["canary-wap"].bom.rows.find((r) => r.ref === "M1");
  assert.match(m1.desc, /L76K/);
  // GNSS is unconditional in the sketch: setup() opens the receiver's UART
  // outside every #if, and no profile carries a GNSS bit.
  for (const fl of Object.values(ws.devices["canary-wap"].firmware.flavors)) {
    assert.ok(!Object.keys(fl).some((f) => /GNSS|GPS/.test(f)), "no GNSS flag in the WAP's flavors");
  }
  const ino = readFileSync(join(WAP_SKETCH, "canary_wap.ino"), "utf8");
  const at = ino.indexOf("Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_GPIO, GPS_TX_GPIO);");
  assert.ok(at > 0, "canary_wap.ino opens the GNSS UART");
  let depth = 0;
  for (const line of ino.slice(0, at).split("\n")) {
    if (/^\s*#\s*if/.test(line)) depth++;
    else if (/^\s*#\s*endif/.test(line)) depth--;
  }
  assert.strictEqual(depth, 0, "...outside every #if");
});

test("the flash stage names the file a flag lives in", async () => {
  const { flagsHome } = await import("../assets/workshop.js");
  const wap = flagsHome(ws.devices["canary-wap"].firmware);
  assert.strictEqual(wap.edit, "build_config.h");
  assert.strictEqual(wap.href, "firmware/projects/canary-wap/arduino/canary_wap/build_config.h");
  assert.match(wap.line, /BUILD_PROFILE_FULL/);
  const sense = flagsHome(ws.devices["canary-sense"].firmware);
  assert.strictEqual(sense.edit, "config.h");
  assert.strictEqual(sense.href, "firmware/configs/canary-sense/");
  assert.match(sense.line, /one folder per flavor/);
});

test("the WAP's flag notes are for a builder: the tamper caveat stays, no other target's note", async () => {
  // Review of F258: a FULL-block line's comment is written for the firmware's
  // developers, so the Workshop's notes come through WAP_NOTES
  // (gen_enclosures.py), each held to the header. The tamper input is off in
  // every shipped profile until its pin is bench-validated, and the page says
  // so wherever it shows the flag; nothing on the S3 flavor's page describes
  // the ESP32-C3.
  const fw = ws.devices["canary-wap"].firmware;
  const full = fw.flavors.full;
  assert.match(full.FEATURE_TAMPER_GPIO.note, /off in every shipped profile until the pin is bench-validated/);
  assert.strictEqual(full.FEATURE_TAMPER_GPIO.on, false);
  const header = readFileSync(join(ROOT, "..", fw.source), "utf8");
  assert.match(header, /OFF in every shipped profile until the pin is\s*(?:\/\/\s*)?bench-validated/,
    "the caveat is the header's own");
  for (const [name, v] of Object.entries(full)) {
    assert.ok(!/\bC3\b/.test(v.note || ""), `${name}'s note describes another target: ${v.note}`);
  }
  // The words the flash stage renders: the tamper option's line keeps the
  // caveat ahead of the how-to, and names the header to edit, not config.h.
  const { impliedFlagText, flagValueText } = await import("../assets/workshop.js");
  const tamper = impliedFlagText({ flag: "FEATURE_TAMPER_GPIO", ...full.FEATURE_TAMPER_GPIO }, "full", fw);
  assert.match(tamper, /^ — OFF in full \(Enclosure tamper contact .*bench-validated\): set it to 1 in build_config\.h when you build$/);
  const camera = impliedFlagText({ flag: "FEATURE_CAMERA_PEEK", ...full.FEATURE_CAMERA_PEEK }, "full", fw);
  assert.strictEqual(camera, " — ON in the full flavor");
  assert.strictEqual(flagValueText(full.FEATURE_CAMERA_PEEK), " 1 (HW_HAS_CAMERA)");
  assert.strictEqual(flagValueText(full.FEATURE_TAMPER_GPIO),
    ` 0 — ${full.FEATURE_TAMPER_GPIO.note}`);
  // ...and the stage renders through those helpers, not copy of its own
  // (workshop_probe.mjs holds the rendered page in Chromium).
  const src = readFileSync(join(ROOT, "assets/workshop.js"), "utf8");
  const body = src.slice(src.indexOf("function renderFlash(root) {"), src.indexOf("// ── stage: BUILD CARD"));
  assert.ok(body.length > 200, "renderFlash found");
  for (const call of ["impliedFlagText(f, fl.flavor, fw)", "flagValueText(v)", "const home = flagsHome(fw);",
    'el("a", null, home.label)', 'src.append("Flags live in ", a, home.line);']) {
    assert.ok(body.includes(call), `renderFlash renders through ${call}`);
  }
  assert.ok(!/config\.h|one folder per flavor/.test(body), "renderFlash names no flag home of its own");
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
