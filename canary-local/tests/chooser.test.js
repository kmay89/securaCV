// canary-local/tests/chooser.test.js — the needs-matcher's promises,
// pinned: privacy lines are absolute, outdoor demands sealed sets,
// statuses are never hidden, and the catalog data stays coherent with
// the enclosure library it was generated from.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
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

test("STL parser handles the real preview meshes", async () => {
  const { parseSTL } = await import("../assets/stl.js");
  const buf = readFileSync(join(ROOT, "enclosures/preview/canary_watch_station_drum.stl"));
  const { bbox, triangles } = parseSTL(
    buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
  assert.ok(triangles > 100, "real geometry");
  // the drum is Ø52 — the mesh must agree with the .scad within coarse-$fn slack
  assert.ok(Math.abs(bbox.size[0] - 52) < 1.5, `drum Ø ~52, got ${bbox.size[0]}`);
});
