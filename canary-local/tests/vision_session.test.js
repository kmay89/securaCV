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
