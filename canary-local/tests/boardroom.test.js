// canary-local/tests/boardroom.test.js — the Board Room honesty gate.
//
// Two contracts, both validated against the committed data (CI, node only):
//
// 1. Pin anchors are claims about the MESH: every `anchor`/`anchors` on a
//    pinout row and every entry in a board's `pads` map must sit inside the
//    committed GLB's bounding box (re-parsed by the page's own loader). An
//    anchor outside the mesh means someone eyeballed a coordinate — exactly
//    what tools/pin_anchors.mjs exists to prevent.
//
// 2. Wiring is claims about the CATALOG: every wiring.json build must
//    reference a real board, resolve every `to` pin through the same resolver
//    the page uses, wire only declared peripherals/pins/colors, keep steps in
//    range, and mirror the pinout's own live/planned status — a wire may not
//    promise more than the firmware config does.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const boards = JSON.parse(readFileSync(join(ROOT, "devices/boards.json"), "utf8"));
const wiring = JSON.parse(readFileSync(join(ROOT, "devices/wiring.json"), "utf8"));

const room = () => import("../assets/board-room.js");
const glb = () => import("../assets/glb.js");

const anchorsOf = (b) => {
  const out = [];
  for (const row of b.pinout || []) {
    if (row.anchor) out.push({ where: `pinout "${row.label}"`, p: row.anchor });
    for (const a of row.anchors || []) out.push({ where: `pinout "${row.label}"`, p: a });
  }
  for (const [k, v] of Object.entries(b.pads || {})) {
    if (!k.startsWith("_")) out.push({ where: `pad ${k}`, p: v });
  }
  return out;
};

for (const [bid, b] of Object.entries(boards.boards)) {
  test(`${bid}: every pin anchor sits inside the committed mesh`, async () => {
    const list = anchorsOf(b);
    if (!list.length) return; // no anchors authored yet — table-only board
    const { parseGLB } = await glb();
    const { bbox } = parseGLB(readFileSync(join(ROOT, b.glb)));
    const M = 1.0; // mm slack: pads may be authored a hair off the face
    for (const { where, p } of list) {
      assert.ok(Array.isArray(p) && p.length === 3 && p.every((v) => typeof v === "number"),
        `${where}: anchor is not an [x,y,z] triple`);
      p.forEach((v, i) => {
        assert.ok(v >= bbox.min[i] - M && v <= bbox.max[i] + M,
          `${where}: anchor[${i}]=${v} outside mesh bbox [${bbox.min[i].toFixed(2)}, ${bbox.max[i].toFixed(2)}]`);
      });
    }
  });
}

test("flagSpecs pairs multi-pin rows with their anchors token by token", async () => {
  const { flagSpecs } = await room();
  const xiao = boards.boards.seeed_xiao_esp32s3_sense;
  const texts = flagSpecs(xiao).map((f) => f.text);
  for (const t of ["D1", "D6", "D7", "5V", "3V3", "GND", "BAT+", "BAT-"]) {
    assert.ok(texts.includes(t), `expected a "${t}" flag, got ${texts.join(", ")}`);
  }
});

test("wirePoints starts on the pad and ends on the peripheral pin", async () => {
  const { wirePoints } = await room();
  const pts = wirePoints([0, 0, 0], [10, 2, -4], 6);
  assert.deepStrictEqual(pts[0], [0, 0, 0]);
  const end = pts[pts.length - 1];
  end.forEach((v, i) => assert.ok(Math.abs(v - [10, 2, -4][i]) < 1e-9, "end drifted"));
  assert.ok(pts.some((p) => p[1] > 2), "wire never lifted above its endpoints");
});

test("wiring schema: colors and peripheral catalog are well-formed", async () => {
  const { PERIPHERAL_PARTS } = await room();
  assert.strictEqual(wiring.schema, 1);
  for (const [id, c] of Object.entries(wiring.colors)) {
    assert.ok(Array.isArray(c) && c.length === 3, `color ${id} is not [r,g,b]`);
  }
  for (const [id, p] of Object.entries(wiring.peripherals)) {
    assert.ok(p.name, `peripheral ${id} has no name`);
    assert.ok(PERIPHERAL_PARTS[p.part], `peripheral ${id} names unknown body "${p.part}"`);
    assert.ok(p.pins && Object.keys(p.pins).length, `peripheral ${id} has no pins`);
    for (const [pin, at] of Object.entries(p.pins)) {
      assert.ok(Array.isArray(at) && at.length === 3, `peripheral ${id}.${pin} pin is not [x,y,z]`);
    }
  }
});

for (const build of wiring.builds) {
  test(`build ${build.id}: every reference is real`, async () => {
    const { resolvePin } = await room();
    const b = boards.boards[build.board];
    assert.ok(b, `build names unknown board ${build.board}`);
    assert.ok(boards.device_board[build.device] === build.board,
      `build device ${build.device} does not map to ${build.board} in device_board`);

    const periIds = new Set();
    for (const pp of build.peripherals) {
      assert.ok(!periIds.has(pp.id), `duplicate peripheral id ${pp.id}`);
      periIds.add(pp.id);
      assert.ok(wiring.peripherals[pp.ref], `${pp.id} references unknown peripheral ${pp.ref}`);
      assert.ok(Array.isArray(pp.pos) && pp.pos.length === 3, `${pp.id} has no placement`);
      assert.ok((pp.step ?? 0) < build.steps.length, `${pp.id} step out of range`);
    }
    for (const conn of build.connections) {
      const [pid, pin] = conn.from;
      assert.ok(periIds.has(pid), `connection from unknown peripheral ${pid}`);
      const cat = wiring.peripherals[build.peripherals.find((p) => p.id === pid).ref];
      assert.ok(cat.pins[pin], `${pid} has no pin "${pin}"`);
      assert.ok(resolvePin(b, conn.to), `"${conn.to}" resolves to no pad/pinout anchor on ${build.board}`);
      assert.ok(wiring.colors[conn.color], `unknown wire color ${conn.color}`);
      assert.ok((conn.step ?? 0) < build.steps.length, `connection ${pid}.${pin} step out of range`);
    }
  });

  test(`build ${build.id}: wires never promise more than the firmware config`, () => {
    const b = boards.boards[build.board];
    for (const conn of build.connections) {
      if (!conn.signal) continue; // power/return wires carry no firmware claim
      const row = (b.pinout || []).find((r) =>
        String(r.pin || "").split("/").map((s) => s.trim()).includes(conn.to));
      if (!row) continue; // pad-only target (e.g. BAT+) — no status to mirror
      const rowPlanned = row.status === "planned";
      const connPlanned = conn.status === "planned";
      assert.strictEqual(connPlanned, rowPlanned,
        `${conn.to} (${conn.signal}): wiring says ${conn.status || "live"} but the pinout row says ${row.status || "live"}`);
    }
  });

  test(`build ${build.id}: every step wires something`, () => {
    build.steps.forEach((s, i) => {
      const n = build.connections.filter((c) => (c.step ?? 0) === i).length;
      assert.ok(n > 0, `step ${i + 1} "${s.title}" has no connections`);
    });
  });
}
