// canary-local/tests/house.test.js — the Canary House's promises, pinned:
// every perch is a real chooser candidate (titles/statuses can't drift),
// every deep link into the chooser uses real question/option ids, rooms
// don't overlap, perches sit in their rooms, and the flock tally adds up.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");

async function house() {
  return import("../assets/house-data.js");
}
async function chooser() {
  return import("../assets/chooser-data.js");
}

test("every perch resolves to a real chooser candidate with honest status", async () => {
  const { PLACEMENTS, placementInfo } = await house();
  const { CANDIDATES } = await chooser();
  assert.ok(PLACEMENTS.length >= 8, "the house shows a real flock");
  for (const p of PLACEMENTS) {
    const c = CANDIDATES.find((x) => x.id === p.candidate);
    assert.ok(c, `${p.id} references chooser candidate ${p.candidate}`);
    const info = placementInfo(p);
    assert.ok(info.title && info.device, `${p.id} has catalog title/device`);
    assert.ok(["released", "in-development"].includes(info.status),
      `${p.id} carries a status it can say to your face`);
    assert.ok(info.sense, `${p.id} sense '${p.sense}' has modality copy`);
  }
});

test("every modality explains how it senses AND what leaves the device", async () => {
  const { SENSE_COPY, PLACEMENTS } = await house();
  for (const p of PLACEMENTS) {
    const s = SENSE_COPY[p.sense];
    assert.ok(s?.label && s?.how && s?.emits, `copy for ${p.sense}`);
  }
  // the point of the page: no modality's emit line mentions media leaving
  for (const [k, s] of Object.entries(SENSE_COPY)) {
    assert.ok(!/uploads|cloud copy/i.test(s.emits), `${k} emits stays honest`);
  }
});

test("perches sit inside their rooms (wall mounts get a small tolerance)", async () => {
  const { PLACEMENTS, ROOMS } = await house();
  const TOL = 0.35;
  for (const p of PLACEMENTS) {
    const r = ROOMS.find((x) => x.id === p.room);
    assert.ok(r, `${p.id} lives in a real room (${p.room})`);
    assert.ok(
      p.at.x >= r.x0 - TOL && p.at.x <= r.x1 + TOL &&
      p.at.y >= r.y0 - TOL && p.at.y <= r.y1 + TOL,
      `${p.id} at (${p.at.x},${p.at.y}) is inside ${r.id}`);
  }
});

test("interior rooms never overlap within a floor", async () => {
  const { ROOMS } = await house();
  const interior = ROOMS.filter((r) => !r.outside);
  for (const a of interior) {
    for (const b of interior) {
      if (a === b || a.floor !== b.floor) continue;
      const overlap = a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
      assert.ok(!overlap, `${a.id} does not overlap ${b.id}`);
    }
  }
});

test("chooser deep links only use real question and option ids", async () => {
  const { PLACEMENTS, chooserHash } = await house();
  const { QUESTIONS } = await chooser();
  for (const p of PLACEMENTS) {
    for (const [qid, v] of Object.entries(p.answers || {})) {
      const q = QUESTIONS.find((x) => x.id === qid);
      assert.ok(q, `${p.id} answer question '${qid}' exists`);
      const vals = Array.isArray(v) ? v : [v];
      for (const id of vals) {
        assert.ok(q.options.some((o) => o.id === id),
          `${p.id} option '${qid}=${id}' exists`);
      }
      // multi questions get arrays, single questions get scalars
      assert.strictEqual(Array.isArray(v), !!q.multi,
        `${p.id} '${qid}' matches the question's multi-ness`);
    }
    const hash = chooserHash(p.answers);
    assert.match(hash, /^#[a-z]+=/, `${p.id} produces a chooser hash`);
  }
});

test("a house deep link actually scores in the chooser", async () => {
  const { PLACEMENTS } = await house();
  const { score } = await chooser();
  for (const p of PLACEMENTS) {
    const ranked = score(p.answers);
    assert.ok(ranked.length > 0, `${p.id} prefill finds at least one match`);
    assert.ok(ranked.some((c) => c.id === p.candidate),
      `${p.id} prefill surfaces its own candidate ${p.candidate}`);
  }
});

test("the flock tally adds up and reacts to toggles", async () => {
  const { PLACEMENTS, flockSummary } = await house();
  const all = flockSummary(PLACEMENTS.map((p) => p.id));
  assert.strictEqual(all.total, PLACEMENTS.length);
  assert.strictEqual(all.witnesses + all.displays + all.infra, all.total);
  assert.strictEqual(all.released + all.indev, all.total);
  assert.ok(all.witnesses >= 5, "the house is mostly witnesses");
  const none = flockSummary([]);
  assert.strictEqual(none.total, 0);
  const one = flockSummary([PLACEMENTS[0].id]);
  assert.strictEqual(one.total, 1);
});

test("the visitor's walk stays on the ground and inside the yard", async () => {
  const { WALK, ROOMS } = await house();
  const yard = ROOMS.find((r) => r.id === "yard");
  assert.ok(WALK.length >= 4, "the walk visits several rooms");
  for (const w of WALK) {
    assert.ok(w.x >= yard.x0 - 0.5 && w.x <= yard.x1 + 0.5 &&
      w.y >= yard.y0 - 0.5 && w.y <= yard.y1 + 1.5,
      `waypoint (${w.x},${w.y}) is on the property`);
  }
});

test("house.html ships and wires the renderer; the chooser accepts prefill", () => {
  const page = readFileSync(join(ROOT, "house.html"), "utf8");
  assert.match(page, /assets\/house\.js/);
  assert.match(page, /id="stage"/);
  assert.match(page, /id="panel"/);
  const chooserJs = readFileSync(join(ROOT, "assets", "chooser.js"), "utf8");
  assert.match(chooserJs, /prefill/, "chooser.js keeps its deep-link prefill");
});
