// Host tests for assets/wifi-memory.js — "remember the home Wi-Fi so a batch of
// boards provisions without re-typing." Proves the two tiers behave: the
// session copy never reaches disk, the opt-in persist survives a "reload"
// (a fresh instance over the same storage), opting out clears disk but keeps
// the session, Forget wipes both, and an absent/blocked storage never throws.

const { test } = require("node:test");
const assert = require("node:assert");

const mod = () => import("../assets/wifi-memory.js");

// A localStorage-shaped mock so the pure behavior is testable without a browser.
function mockStorage() {
  const m = new Map();
  return {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
    _size: () => m.size,
  };
}

test("recall is null until something is remembered", async () => {
  const { makeWifiMemory } = await mod();
  const mem = makeWifiMemory(mockStorage());
  assert.strictEqual(mem.recall(), null);
  assert.strictEqual(mem.isPersisted(), false);
});

test("session tier: auto-fills across boards but never touches disk", async () => {
  const { makeWifiMemory } = await mod();
  const store = mockStorage();
  const mem = makeWifiMemory(store);
  mem.remember({ ssid: "Home", pass: "hunter2" }, /*persist=*/false);

  // The next board pre-fills from the same session instance…
  assert.deepStrictEqual(mem.recall(), { ssid: "Home", pass: "hunter2" });
  assert.strictEqual(mem.isPersisted(), false);
  // …but nothing was written to disk: a fresh instance (a "reload") sees nothing.
  assert.strictEqual(store._size(), 0);
  assert.strictEqual(makeWifiMemory(store).recall(), null);
});

test("persist tier: survives a reload (fresh instance over the same storage)", async () => {
  const { makeWifiMemory } = await mod();
  const store = mockStorage();
  makeWifiMemory(store).remember({ ssid: "Home", pass: "hunter2" }, /*persist=*/true);

  const reopened = makeWifiMemory(store); // simulate closing + reopening the tab
  assert.deepStrictEqual(reopened.recall(), { ssid: "Home", pass: "hunter2" });
  assert.strictEqual(reopened.isPersisted(), true);
});

test("opting out of persist clears the disk copy but keeps the session", async () => {
  const { makeWifiMemory } = await mod();
  const store = mockStorage();
  const mem = makeWifiMemory(store);
  mem.remember({ ssid: "Home", pass: "pw" }, true);
  assert.strictEqual(mem.isPersisted(), true);

  mem.remember({ ssid: "Home", pass: "pw" }, false); // untick "remember on this computer"
  assert.strictEqual(mem.isPersisted(), false);            // gone from disk
  assert.deepStrictEqual(mem.recall(), { ssid: "Home", pass: "pw" }); // still this session
  assert.strictEqual(makeWifiMemory(store).recall(), null);           // truly off disk
});

test("Forget wipes both session and disk", async () => {
  const { makeWifiMemory } = await mod();
  const store = mockStorage();
  const mem = makeWifiMemory(store);
  mem.remember({ ssid: "Home", pass: "pw" }, true);
  mem.forget();
  assert.strictEqual(mem.recall(), null);
  assert.strictEqual(mem.isPersisted(), false);
  assert.strictEqual(makeWifiMemory(store).recall(), null);
});

test("an empty SSID forgets (nothing worth remembering) and trims whitespace", async () => {
  const { makeWifiMemory } = await mod();
  const store = mockStorage();
  const mem = makeWifiMemory(store);
  mem.remember({ ssid: "Home", pass: "pw" }, true);
  mem.remember({ ssid: "   ", pass: "pw" }, true); // blank SSID
  assert.strictEqual(mem.recall(), null);
  assert.strictEqual(makeWifiMemory(store).recall(), null);

  mem.remember({ ssid: "  Padded  ", pass: "pw" }, false);
  assert.strictEqual(mem.recall().ssid, "Padded"); // trimmed
});

test("absent / blocked storage (private mode) never throws — session still works", async () => {
  const { makeWifiMemory } = await mod();
  const mem = makeWifiMemory(null);
  mem.remember({ ssid: "Home", pass: "pw" }, true); // persist requested but no storage
  assert.deepStrictEqual(mem.recall(), { ssid: "Home", pass: "pw" }); // session tier holds
  assert.strictEqual(mem.isPersisted(), false);                        // nothing on disk
  mem.forget();
  assert.strictEqual(mem.recall(), null);
});

test("the default singleton is usable and starts empty in Node (no localStorage)", async () => {
  const { wifiMemory } = await mod();
  assert.strictEqual(typeof wifiMemory.recall, "function");
  assert.strictEqual(wifiMemory.recall(), null);
});
