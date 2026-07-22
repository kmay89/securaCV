// Host tests for assets/vision-session.js — "remember which of the two Vision
// ports you've flashed so the flasher can insist on both." Proves: each port is
// recorded, the pair survives a "reload" (a fresh instance over the same
// storage, like sessionStorage), finishing a pair and flashing again starts a
// FRESH pair (a batch just works), unknown parts are ignored, reset clears, and
// an absent/blocked storage degrades to in-memory without ever throwing.

const { test } = require("node:test");
const assert = require("node:assert");

const mod = () => import("../assets/vision-session.js");

// A sessionStorage-shaped mock so the behaviour is testable without a browser.
function mockStorage() {
  const m = new Map();
  return {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
    _size: () => m.size,
  };
}

test("starts empty — neither port flashed", async () => {
  const { makeVisionSession } = await mod();
  const vs = makeVisionSession(mockStorage());
  assert.deepStrictEqual(vs.parts(), { esp32: false, we2: false });
});

test("records each port; both makes a complete pair", async () => {
  const { makeVisionSession } = await mod();
  const vs = makeVisionSession(mockStorage());
  assert.deepStrictEqual(vs.markDone("esp32"), { esp32: true, we2: false });
  assert.deepStrictEqual(vs.parts(), { esp32: true, we2: false });
  assert.deepStrictEqual(vs.markDone("we2"), { esp32: true, we2: true });
});

test("survives a reload — a fresh instance over the same storage sees the pair", async () => {
  const { makeVisionSession } = await mod();
  const store = mockStorage();
  makeVisionSession(store).markDone("esp32");
  // Simulate an accidental reload mid-pair: a new instance, same storage.
  assert.deepStrictEqual(makeVisionSession(store).parts(), { esp32: true, we2: false });
});

test("finishing a pair, then flashing again, starts a FRESH pair (a batch just works)", async () => {
  const { makeVisionSession } = await mod();
  const vs = makeVisionSession(mockStorage());
  vs.markDone("esp32");
  vs.markDone("we2");                                  // board 1 complete
  assert.deepStrictEqual(vs.parts(), { esp32: true, we2: true });
  // Board 2: the first flash of the next board must NOT read as "2 of 2".
  assert.deepStrictEqual(vs.markDone("esp32"), { esp32: true, we2: false });
});

test("an abandoned half-pair goes stale — can't combine with a later unrelated flash", async () => {
  const { makeVisionSession } = await mod();
  let t = 1000;
  const vs = makeVisionSession(mockStorage(), () => t);
  vs.markDone("esp32");                                  // half a pair at t=1000
  assert.deepStrictEqual(vs.parts(), { esp32: true, we2: false });
  t += 31 * 60 * 1000;                                   // 31 min later — walked away
  // The stale half reads as empty, so it can't falsely complete…
  assert.deepStrictEqual(vs.parts(), { esp32: false, we2: false });
  // …and flashing the OTHER part now starts a FRESH pair, not "both done".
  assert.deepStrictEqual(vs.markDone("we2"), { esp32: false, we2: true });
});

test("within one bring-up, the pair stays live across the wait for the second port", async () => {
  const { makeVisionSession } = await mod();
  let t = 1000;
  const vs = makeVisionSession(mockStorage(), () => t);
  vs.markDone("esp32");
  t += 10 * 60 * 1000;                                   // 10 min to find + plug in the module
  vs.markDone("we2");
  assert.deepStrictEqual(vs.parts(), { esp32: true, we2: true });   // both — the celebration
});

test("a COMPLETE pair never goes stale (only the next flash opens a new pair)", async () => {
  const { makeVisionSession } = await mod();
  let t = 1000;
  const vs = makeVisionSession(mockStorage(), () => t);
  vs.markDone("esp32");
  vs.markDone("we2");
  t += 24 * 60 * 60 * 1000;                              // a day later
  assert.deepStrictEqual(vs.parts(), { esp32: true, we2: true });   // still shown done
});

test("unknown parts are ignored, never throw", async () => {
  const { makeVisionSession } = await mod();
  const vs = makeVisionSession(mockStorage());
  assert.deepStrictEqual(vs.markDone("nope"), { esp32: false, we2: false });
  assert.deepStrictEqual(vs.parts(), { esp32: false, we2: false });
});

test("reset clears the pair", async () => {
  const { makeVisionSession } = await mod();
  const vs = makeVisionSession(mockStorage());
  vs.markDone("esp32");
  vs.reset();
  assert.deepStrictEqual(vs.parts(), { esp32: false, we2: false });
});

test("absent storage degrades to in-memory and never throws", async () => {
  const { makeVisionSession } = await mod();
  const vs = makeVisionSession(null);
  assert.deepStrictEqual(vs.markDone("esp32"), { esp32: true, we2: false });
  assert.deepStrictEqual(vs.parts(), { esp32: true, we2: false });
  // A separate instance has its own memory (no shared disk) — the honest result
  // when there's no storage to share.
  assert.deepStrictEqual(makeVisionSession(null).parts(), { esp32: false, we2: false });
});

test("a corrupt stored value reads as an empty pair, never throws", async () => {
  const { makeVisionSession } = await mod();
  const store = mockStorage();
  store.setItem("scv-vision", "{not json");
  assert.deepStrictEqual(makeVisionSession(store).parts(), { esp32: false, we2: false });
});
