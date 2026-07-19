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

test("every real perch resolves to a real chooser candidate with honest status", async () => {
  const { PLACEMENTS, placementInfo } = await house();
  const { CANDIDATES } = await chooser();
  assert.ok(PLACEMENTS.filter((p) => !p.teaser).length >= 8, "the house shows a real flock");
  for (const p of PLACEMENTS.filter((p) => !p.teaser)) {
    const c = CANDIDATES.find((x) => x.id === p.candidate);
    assert.ok(c, `${p.id} references chooser candidate ${p.candidate}`);
    const info = placementInfo(p);
    assert.ok(info.title && info.device, `${p.id} has catalog title/device`);
    assert.ok(["released", "in-development"].includes(info.status),
      `${p.id} carries a status it can say to your face`);
    assert.ok(info.sense, `${p.id} sense '${p.sense}' has modality copy`);
  }
});

test("teaser perches are honesty-fenced: no catalog claims, no chooser doors", async () => {
  const { PLACEMENTS, placementInfo } = await house();
  const teasers = PLACEMENTS.filter((p) => p.teaser);
  assert.ok(teasers.length >= 1, "the fence guard teaser is perched");
  for (const p of teasers) {
    assert.ok(!p.candidate, `${p.id} claims no catalog candidate`);
    assert.ok(!p.answers, `${p.id} offers no chooser pre-fill — you can't shop a concept`);
    const info = placementInfo(p);
    assert.strictEqual(info.status, "coming-soon", `${p.id} says coming-soon to your face`);
    assert.strictEqual(info.teaser, true);
    assert.ok(info.sense?.how && info.sense?.emits, `${p.id} still explains itself honestly`);
    assert.match(info.sense.emits, /concept/i,
      `${p.id}'s emits line admits it is a concept`);
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
  for (const p of PLACEMENTS.filter((p) => !p.teaser)) {
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
  for (const p of PLACEMENTS.filter((p) => !p.teaser)) {
    const ranked = score(p.answers);
    assert.ok(ranked.length > 0, `${p.id} prefill finds at least one match`);
    assert.ok(ranked.some((c) => c.id === p.candidate),
      `${p.id} prefill surfaces its own candidate ${p.candidate}`);
  }
});

test("the flock tally adds up, reacts to toggles, and never counts concepts", async () => {
  const { PLACEMENTS, flockSummary } = await house();
  const real = PLACEMENTS.filter((p) => !p.teaser);
  const teasers = PLACEMENTS.filter((p) => p.teaser);
  const all = flockSummary(PLACEMENTS.map((p) => p.id));
  assert.strictEqual(all.total, real.length, "teasers never inflate the real total");
  assert.strictEqual(all.soon, teasers.length, "…but they are counted as coming-soon");
  assert.strictEqual(all.witnesses + all.displays + all.infra, all.total);
  assert.strictEqual(all.released + all.indev, all.total);
  assert.ok(all.witnesses >= 5, "the house is mostly witnesses");
  const none = flockSummary([]);
  assert.strictEqual(none.total, 0);
  assert.strictEqual(none.soon, 0);
  const one = flockSummary([real[0].id]);
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

test("house.html ships and wires the renderer; the chooser accepts prefill", async () => {
  const page = readFileSync(join(ROOT, "house.html"), "utf8");
  // if any teaser is perched, the page copy must own up to concepts —
  // "every marker is real" would be a lie with a concept on the fence
  const { PLACEMENTS } = await house();
  if (PLACEMENTS.some((p) => p.teaser)) {
    assert.match(page, /concept/i, "hero/footer copy admits concept perches exist");
    assert.ok(!/Every marker is a real device/.test(page),
      "the all-markers-are-real claim is gone while a teaser is perched");
  }
  assert.match(page, /assets\/house\.js/);
  assert.match(page, /assets\/house\.css\?v=/,
    "house styles ship in their own versioned sheet — cached shared CSS must not undress the page");
  assert.match(page, /id="stage"/);
  assert.match(page, /id="panel"/);
  const chooserJs = readFileSync(join(ROOT, "assets", "chooser.js"), "utf8");
  assert.match(chooserJs, /prefill/, "chooser.js keeps its deep-link prefill");
});
