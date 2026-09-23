// canary-local/tests/chooser.test.js — the needs-matcher's promises,
// pinned: privacy lines are absolute, outdoor demands sealed sets,
// statuses are never hidden, and the catalog data stays coherent with
// the enclosure library it was generated from.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync, readdirSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");

async function chooser() {
  return import("../assets/chooser-data.js");
}

test("'no cameras' removes every camera device, regardless of score", async () => {
  const { score } = await chooser();
  const r = score({ place: "door", want: ["see"], privacy: ["nocam"], power: "outlet" });
  assert.ok(r.every((c) => c.device !== "canary-vision"),
    "no canary-vision recommendation under a nocam line");
});

test("outdoor placement only ever recommends sealed sets", async () => {
  const { score, CANDIDATES } = await chooser();
  const r = score({ place: "outdoor", want: ["feel"], power: "battery" });
  assert.ok(r.length > 0);
  for (const c of r) {
    const cand = CANDIDATES.find((x) => x.id === c.id);
    assert.ok(cand.tags.place.includes("outdoor"), `${c.id} is outdoor-rated`);
  }
});

test("bedside wellbeing surfaces the radar stand, honestly marked", async () => {
  const { score } = await chooser();
  const r = score({ place: "bedside", want: ["breathe"], privacy: ["nocam", "nomic"], power: "outlet" });
  assert.strictEqual(r[0].id, "sense-bedside");
  assert.strictEqual(r[0].status, "in-development");
});

test("every result carries a status; released gets the tie-break bonus", async () => {
  const { score } = await chooser();
  const r = score({ place: "indoor", want: ["feel"], power: "outlet" });
  assert.ok(r.length >= 2);
  for (const c of r) {
    assert.ok(["released", "in-development"].includes(c.status), c.id);
  }
  // the +0.5 released bonus is visible in the scores…
  assert.ok(r.filter((c) => c.status === "released").every((c) => c.score % 1 === 0.5));
  // …and the top indoor presence match is shipping hardware, not a promise
  assert.strictEqual(r[0].status, "released");
});

test("display recommendation appears for 'show' directly", async () => {
  const { score } = await chooser();
  const r = score({ place: "indoor", want: ["show"], power: "outlet" });
  assert.ok(r.some((c) => c.device.startsWith("canary-display")));
});

test("chooser candidates reference real enclosure sets and devices", async () => {
  const { CANDIDATES } = await chooser();
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  const encIds = new Set(enc.sets.map((s) => s.id));
  const devIds = new Set(reg.devices.map((d) => d.id));
  for (const c of CANDIDATES) {
    assert.ok(devIds.has(c.device), `${c.id}: device ${c.device} exists`);
    assert.ok(encIds.has(c.enclosure), `${c.id}: enclosure set ${c.enclosure} exists`);
  }
});

// ── device ↔ enclosure pairing: the MANIFESTS are the source of truth ─────
// Every devices/<slug>/device.json lists the sets its hardware takes in
// cad.enclosure_sets; gen_enclosures.py inverts that into each set's
// `device` — the first claimant (slug order), on its family's device when the
// family is itself a manifest — and `devices`, every claimant, whenever the
// device alone does not name them. A set no manifest claims is universal.
// scripts/lint_device_manifests.py refuses any other homing.
function manifestOwners() {
  const dir = join(ROOT, "../devices");
  const owners = new Map();
  const family = new Map();
  for (const slug of readdirSync(dir).filter((d) => existsSync(join(dir, d, "device.json"))).sort()) {
    const m = JSON.parse(readFileSync(join(dir, slug, "device.json"), "utf8"));
    family.set(slug, m.family);
    for (const s of m.cad?.enclosure_sets || []) owners.set(s, [...(owners.get(s) || []), slug]);
  }
  const home = (slug) => (family.has(family.get(slug)) ? family.get(slug) : slug);
  return { owners, home };
}

test("enclosures.json homes every set on the manifests that claim it", () => {
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  const { owners, home } = manifestOwners();
  for (const s of enc.sets) {
    const own = owners.get(s.id);
    if (!own) {
      assert.strictEqual(s.device, null, `${s.id}: no manifest claims it, so it is universal`);
      continue;
    }
    const dev = home(own[0]);
    assert.strictEqual(s.device, dev, `${s.id}: device is the first claimant's home`);
    assert.deepStrictEqual(s.devices, own.length === 1 && own[0] === dev ? undefined : own,
      `${s.id}: devices = every claimant, whenever the device alone does not say them`);
  }
  // the DevKit's case is claimed by the DevKit manifest and stays on the Vision page
  const devkit = enc.sets.find((s) => s.id === "vision-devkit-indoor");
  assert.strictEqual(devkit.device, "canary-vision");
  assert.deepStrictEqual(devkit.devices, ["canary-vision-devkit"]);
  // the five display cards the old name regex mis-homed now sit on their own devices
  const by = Object.fromEntries(enc.sets.map((s) => [s.id, s]));
  assert.strictEqual(by["c6-display-pocket-case"].device, "canary-display-nightstand-c6");
  assert.strictEqual(by["1-69-touch-watch-display-puck"].device, "canary-display-touch169");
  assert.strictEqual(by["s3-hallway-stick-case"].device, "canary-display-nightstand-s3");
  assert.strictEqual(by["c3-pocket-display-case"].device, "canary-display-nightlight-c3");
  assert.deepStrictEqual(by["7-touch-dashboard-case"].devices,
    ["canary-display-dash7", "canary-display-nightstand7"]);
});

test("device pages list what the manifests give them, and the chooser agrees", async () => {
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  const { setServes } = await import("../assets/enclosure-sets.js");
  const page = (dev) => enc.sets.filter((s) => setServes(s, dev)).map((s) => s.id);
  // the Watch and Dash pages carry their own cases — no other board's
  assert.deepStrictEqual(page("canary-display-watch"), ["watch-station"]);
  assert.deepStrictEqual(page("canary-display-dash"), ["dashboard-display-case"]);
  // a claimant beyond the first still gets the case…
  assert.ok(page("canary-display-nightstand7").includes("7-touch-dashboard-case"));
  // …and a family's own page lists its host variants' cases (the DevKit's)
  const vision = page("canary-vision");
  for (const id of ["vision-xiao-indoor", "vision-devkit-indoor", "vision-doorbell", "combo-witness"]) {
    assert.ok(vision.includes(id), `canary-vision page lists ${id}`);
  }
  // every chooser pairing is one the manifests make — or a universal case
  const { CANDIDATES } = await chooser();
  for (const c of CANDIDATES) {
    const set = enc.sets.find((s) => s.id === c.enclosure);
    assert.ok(set.device == null || setServes(set, c.device),
      `${c.id}: ${c.enclosure} is not a ${c.device} case per the manifests`);
  }
});

test("enclosures.json: released parts exist on disk; previews rendered", () => {
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  assert.ok(enc.sets.length >= 25, "catalog parsed");
  for (const s of enc.sets) {
    for (const p of s.parts) {
      const path = p.preview_mesh
        ? join(ROOT, "enclosures/preview", p.file.replace(/^preview\//, ""))
        : join(ROOT, "../docs/hardware/enclosure", p.file);
      assert.ok(existsSync(path), `${s.id}: ${p.file}`);
    }
    if (s.status === "released") assert.ok(s.parts.length > 0, `${s.id} has parts`);
    assert.ok(s.scad, `${s.id} has a configurator`);
  }
  // customizer parsing produced real parameter maps
  const watch = enc.scads["canary_watch_station.scad"];
  assert.ok(watch.groups.length >= 5);
  const part = watch.groups[0].params.find((p) => p.name === "part");
  assert.deepStrictEqual(part.enum, ["drum", "bezel", "stand", "all"]);
});

test("mesh slicing: a unit cube's cross-section is its square perimeter", async () => {
  const { sliceSegments } = await import("../assets/print-guide.js");
  // 10 mm cube, 2 triangles per side face (z from 0 to 10)
  const pos = [];
  const idx = [];
  const quad = (a, b, c, d) => {
    const base = pos.length / 3;
    pos.push(...a, ...b, ...c, ...d);
    idx.push(base, base + 1, base + 2, base, base + 2, base + 3);
  };
  quad([0, 0, 0], [10, 0, 0], [10, 0, 10], [0, 0, 10]);     // front
  quad([10, 0, 0], [10, 10, 0], [10, 10, 10], [10, 0, 10]); // right
  quad([10, 10, 0], [0, 10, 0], [0, 10, 10], [10, 10, 10]); // back
  quad([0, 10, 0], [0, 0, 0], [0, 0, 10], [0, 10, 10]);     // left
  const segs = sliceSegments({ pos, idx }, 5);
  assert.strictEqual(segs.length / 6, 8, "8 crossing triangles → 8 segments");
  for (let i = 0; i < segs.length; i += 3) {
    assert.strictEqual(segs[i + 2], 5, "every point at slice height");
    const onPerimeter =
      segs[i] === 0 || segs[i] === 10 || segs[i + 1] === 0 || segs[i + 1] === 10;
    assert.ok(onPerimeter, "points lie on the square perimeter");
  }
});

test("mesh slicing: a vertex exactly on the plane yields one clean segment", async () => {
  const { sliceSegments } = await import("../assets/print-guide.js");
  // Triangle with vertex C exactly at the slice height — both of C's
  // edges intersect AT C; without dedup the duplicate ate the real
  // segment and left contour gaps (review catch).
  const pos = [0, 0, 0, 10, 0, 10, 0, 0, 5];
  const idx = [0, 1, 2];
  const segs = sliceSegments({ pos, idx }, 5);
  assert.strictEqual(segs.length / 6, 1, "exactly one segment");
  const [x1, y1, , x2, y2] = [segs[0], segs[1], segs[2], segs[3], segs[4]];
  const len = Math.hypot(x2 - x1, y2 - y1);
  assert.ok(len > 1, `segment is non-degenerate (len=${len})`);
});

test("display BOMs split by flavor: watch never carries the dash panel", () => {
  const b = JSON.parse(readFileSync(join(ROOT, "devices/build.json"), "utf8"));
  const watch = b.devices["canary-display-watch"].bom;
  const dash = b.devices["canary-display-dash"].bom;
  assert.ok(watch.rows.every((r) => !r.ref.startsWith("D-")), "no D-* rows in watch");
  assert.ok(dash.rows.every((r) => !r.ref.startsWith("W-")), "no W-* rows in dash");
  assert.ok(dash.rows.some((r) => /Waveshare/.test(r.desc) && r.required), "dash owns its panel");
  assert.ok(watch.rows.some((r) => r.ref === "PSU1"), "shared rows serve watch");
  assert.ok(dash.rows.some((r) => r.ref === "PSU1"), "shared rows serve dash");
});

test("print guidance rides the catalog: notes + materials per part", () => {
  const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));
  assert.ok(enc.print_settings.layer_height_mm === 0.2);
  assert.match(enc.print_settings.material, /PETG/);
  for (const s of enc.sets) {
    for (const p of s.parts) {
      assert.ok(p.print_note, `${s.id}/${p.name} has a print note`);
      assert.ok(p.material, `${s.id}/${p.name} has a material`);
    }
  }
  const weather = enc.sets.find((s) => s.id === "wap-weather");
  const gasket = weather.parts.find((p) => p.file.includes("gasket"));
  assert.match(gasket.material, /TPU/);
  const lid = weather.parts.find((p) => p.file.includes("lid"));
  assert.match(lid.print_note, /face-down/);
});

test("STL parser handles the real preview meshes", async () => {
  const { parseSTL } = await import("../assets/stl.js");
  const buf = readFileSync(join(ROOT, "enclosures/preview/canary_watch_station_drum.stl"));
  const { bbox, triangles } = parseSTL(
    buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
  assert.ok(triangles > 100, "real geometry");
  // the v0.2 drum is Ø49 (bore Ø44.4 over the measured Ø43.0 disc + walls) —
  // the mesh must agree with the .scad within coarse-$fn slack
  assert.ok(Math.abs(bbox.size[0] - 49) < 1.5, `drum Ø ~49, got ${bbox.size[0]}`);
});
