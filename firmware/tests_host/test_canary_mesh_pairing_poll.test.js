// Host test for firmware/canary's Opera pairing screen
// (canary/lib/securacv_webui/src/securacv_webui.cpp: startPairing,
// pairingVerdict, pairingFailText, startPairingPolling, confirmPairing) —
// repo sweep F133.
//
// The pairing poll read only `state` from GET /api/mesh and called ACTIVE or
// CONNECTING "Pairing complete" (with "Successfully joined opera!" on
// ACTIVE). A Canary already in an opera returns to exactly those states after
// a failed pairing: a timeout, a refusal at the seal (F118), a cancel. Since
// F133 the body carries pairing_seq, pairing_result and pairing_fail_reason
// (mesh_api::build_mesh_status_json, pinned in test_mesh_session), and the
// POST pair/start and pair/join answers carry the pairing_seq they started.
// What this pins:
//   - a failed pairing is reported as failed, with its reason in words, and
//     never as complete, whatever the state reads;
//   - a finished one is reported once, in the words of the side this Canary
//     played;
//   - a body about another pairing (another number, or 0 after a restart)
//     ends the poll without claiming either;
//   - 000000 is shown like any other code (the old test was truthiness);
//   - after this owner's confirm, the poll no longer offers the confirm
//     button again, and the next pairing in the same page starts
//     unconfirmed; a confirm refused with partner_refused says so once;
//   - a body without the F133 fields (older firmware) claims neither a
//     success nor a failure.
// The functions are lifted out of the C++ raw string by literal markers and
// run against a stub DOM, a scripted api(), a captured setInterval and a
// recording alert(), the way test_canary_mesh_alerts.test.js does it.
//
// Run: node --test firmware/tests_host/test_canary_mesh_pairing_poll.test.js
// (CI: firmware.yml's "Build + run all mesh + ble_scan host tests" step, with
// the mesh C++ suites whose JSON it reads).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const UI = join(__dirname, "..", "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const src = readFileSync(UI, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

// The page's pairing state (the `let`s after `let operaState`), then
// startPairing through confirmPairing.
const code =
  slice("    let pairingPollingInterval = null;", "\n    async function refreshOpera() {") +
  slice("    async function startPairing(mode) {", "    async function cancelPairing() {") +
  "\n;globalThis.__t = { startPairing, pairingVerdict, pairingFailText, confirmPairing," +
  " stopPairingPolling, state: () => ({ pairingSeq, pairingMode, pairingConfirmed," +
  " polling: pairingPollingInterval }) };\n";

// A page: stub DOM, api() answering from `routes` (a function per endpoint
// or a queue of GET /api/mesh bodies), captured interval, recorded alerts.
function page(routes) {
  const els = {};
  const el = (id) => els[id] || (els[id] = { id, style: {}, textContent: "", innerHTML: "" });
  const alerts = [];
  const calls = [];
  let tick = null;
  let refreshed = 0;
  const meshBodies = routes["/api/mesh"] || [];
  const ctx = {
    document: { getElementById: el },
    api: async (url, method) => {
      calls.push(`${method || "GET"} ${url}`);
      if (url === "/api/mesh") {
        assert.ok(meshBodies.length > 0, "the poll asked for more bodies than the test scripted");
        return meshBodies.shift();
      }
      return routes[url]();
    },
    setInterval: (fn) => { tick = fn; return 42; },
    clearInterval: (id) => { assert.strictEqual(id, 42); tick = null; },
    alert: (msg) => alerts.push(String(msg)),
    refreshOpera: () => { refreshed++; },
    console,
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  return {
    t: ctx.__t, el, alerts, calls,
    polling: () => tick !== null,
    poll: async () => { assert.ok(tick, "not polling"); await tick(); },
    refreshed: () => refreshed,
  };
}

// The success words, old ("Successfully joined opera!", the "Pairing complete"
// branch) and new.
const SUCCESS = /Pairing complete|Successfully|Joined the opera|joined opera/;

// What a Canary already in an opera answers after its pairing number 7 ended.
function after(seq, result, reason, state = "ACTIVE") {
  return { ok: true, state, opera_id: "a0", opera_name: "Home", has_opera: true, enabled: true,
           peers_total: 2, peers_online: 1, alerts_received: 0,
           pairing_seq: seq, pairing_result: result, pairing_fail_reason: reason };
}

test("a failed pairing is never called complete, and says why", async () => {
  for (const [reason, words] of [
    ["timeout", /timed out/], ["canceled", /canceled/],
    ["partner_refused", /cannot take that device/], ["bad_confirm", /did not match/],
  ]) {
    for (const state of ["ACTIVE", "CONNECTING"]) {
      const p = page({
        "/api/mesh/pair/start": () => ({ ok: true, created: false, state: "PAIRING_INIT", pairing_seq: 7 }),
        "/api/mesh": [after(7, "running", "none", "PAIRING_INIT"), after(7, "failed", reason, state)],
      });
      await p.t.startPairing("init");
      assert.strictEqual(p.t.state().pairingSeq, 7);
      await p.poll();
      assert.ok(p.polling() && p.alerts.length === 0);
      await p.poll();
      assert.ok(!p.polling(), "the poll stops");
      assert.strictEqual(p.refreshed(), 1);
      assert.strictEqual(p.alerts.length, 1, `${reason}/${state}`);
      assert.match(p.alerts[0], /did not complete/);
      assert.match(p.alerts[0], words);
      assert.doesNotMatch(p.alerts[0], SUCCESS);
    }
  }
});

test("a finished pairing is reported once, in the words of this side", async () => {
  for (const [mode, start, words] of [
    ["init", "/api/mesh/pair/start", /Pairing complete on this Canary/],
    ["join", "/api/mesh/pair/join", /^Joined the opera\.$/],
  ]) {
    const p = page({
      [start]: () => ({ ok: true, state: mode === "init" ? "PAIRING_INIT" : "PAIRING_JOIN", pairing_seq: 3 }),
      "/api/mesh": [after(3, "paired", "none", mode === "init" ? "ACTIVE" : "CONNECTING")],
    });
    await p.t.startPairing(mode);
    await p.poll();
    assert.ok(!p.polling());
    assert.strictEqual(p.alerts.length, 1);
    assert.match(p.alerts[0], words);
    assert.deepStrictEqual(p.calls, [`POST ${start}`, "GET /api/mesh"]);
  }
});

test("a body about another pairing ends the poll and claims nothing", async () => {
  // Another pairing started (8), or the Canary restarted (0, none).
  for (const body of [after(8, "paired", "none"), after(8, "failed", "timeout"), after(0, "none", "none")]) {
    const p = page({
      "/api/mesh/pair/start": () => ({ ok: true, state: "PAIRING_INIT", pairing_seq: 7 }),
      "/api/mesh": [body],
    });
    await p.t.startPairing("init");
    await p.poll();
    assert.ok(!p.polling());
    assert.strictEqual(p.alerts.length, 1);
    assert.match(p.alerts[0], /This pairing ended/);
    assert.doesNotMatch(p.alerts[0], SUCCESS);
    assert.doesNotMatch(p.alerts[0], /did not complete/);
  }
});

test("the code shows, 000000 included, and the confirm is offered until this owner confirms", async () => {
  const confirm = { ...after(5, "running", "none", "PAIRING_CONFIRM"), pairing_code: 0 };
  const p = page({
    "/api/mesh/pair/join": () => ({ ok: true, state: "PAIRING_JOIN", pairing_seq: 5 }),
    "/api/mesh/pair/confirm": () => ({ ok: true }),
    "/api/mesh": [confirm, confirm, { ...confirm, pairing_code: 42 }],
  });
  await p.t.startPairing("join");
  await p.poll();
  assert.strictEqual(p.el("pairingCodeValue").textContent, "000000");
  assert.strictEqual(p.el("pairingCode").style.display, "block");
  assert.strictEqual(p.el("pairingConfirmBtn").style.display, "inline-flex");
  await p.t.confirmPairing();
  assert.strictEqual(p.el("pairingConfirmBtn").style.display, "none");
  // The state reads PAIRING_CONFIRM until the other owner confirms: the
  // button is not offered again.
  await p.poll();
  assert.strictEqual(p.el("pairingConfirmBtn").style.display, "none");
  assert.match(p.el("pairingStatus").textContent, /Waiting for the other device/);
  await p.poll();
  assert.strictEqual(p.el("pairingCodeValue").textContent, "000042");
  assert.strictEqual(p.el("pairingConfirmBtn").style.display, "none");
  assert.ok(p.polling() && p.alerts.length === 0);
});

test("a second pairing in the same page offers its own confirm", async () => {
  // startPairing() starts each pairing unconfirmed: an owner who confirmed
  // one pairing and presses Add Device again sees the next code with the
  // confirm button, not "Confirmed here" (that pairing could only time out).
  let seq = 0;
  const p = page({
    "/api/mesh/pair/start": () => ({ ok: true, state: "PAIRING_INIT", pairing_seq: ++seq }),
    "/api/mesh/pair/confirm": () => ({ ok: true }),
    "/api/mesh": [
      { ...after(1, "running", "none", "PAIRING_CONFIRM"), pairing_code: 111111 },
      after(1, "paired", "none"),
      { ...after(2, "running", "none", "PAIRING_CONFIRM"), pairing_code: 222222 },
    ],
  });
  await p.t.startPairing("init");
  await p.poll();
  await p.t.confirmPairing();
  assert.strictEqual(p.t.state().pairingConfirmed, true);
  await p.poll();
  assert.ok(!p.polling() && p.alerts.length === 1);
  assert.match(p.alerts[0], /Pairing complete on this Canary/);
  await p.t.startPairing("init");
  assert.strictEqual(p.t.state().pairingSeq, 2);
  assert.strictEqual(p.t.state().pairingConfirmed, false);
  await p.poll();
  assert.strictEqual(p.el("pairingCodeValue").textContent, "222222");
  assert.strictEqual(p.el("pairingConfirmBtn").style.display, "inline-flex");
  assert.match(p.el("pairingStatus").textContent, /Verify the code/);
  assert.ok(p.polling() && p.alerts.length === 1);
});

test("a confirm refused with partner_refused says so once and stops the poll", async () => {
  const p = page({
    "/api/mesh/pair/start": () => ({ ok: true, state: "PAIRING_INIT", pairing_seq: 9 }),
    "/api/mesh/pair/confirm": () => ({ ok: false, error: "partner_refused" }),
    "/api/mesh": [{ ...after(9, "running", "none", "PAIRING_CONFIRM"), pairing_code: 123456 }],
  });
  await p.t.startPairing("init");
  await p.poll();
  await p.t.confirmPairing();
  assert.ok(!p.polling());
  assert.strictEqual(p.alerts.length, 1);
  assert.match(p.alerts[0], /did not complete: this Canary cannot take that device/);
  assert.strictEqual(p.t.state().pairingConfirmed, false);
});

test("another confirm refusal keeps the poll and the button", async () => {
  const p = page({
    "/api/mesh/pair/start": () => ({ ok: true, state: "PAIRING_INIT", pairing_seq: 9 }),
    "/api/mesh/pair/confirm": () => ({ ok: false, error: "mesh_busy" }),
    "/api/mesh": [{ ...after(9, "running", "none", "PAIRING_CONFIRM"), pairing_code: 123456 }],
  });
  await p.t.startPairing("init");
  await p.poll();
  await p.t.confirmPairing();
  assert.ok(p.polling());
  assert.strictEqual(p.el("pairingConfirmBtn").style.display, "inline-flex");
  assert.match(p.alerts[0], /confirmation failed: mesh_busy/);
});

test("a body without the F133 fields claims neither a success nor a failure", async () => {
  const old = (state, extra = {}) => ({ ok: true, state, has_opera: true, enabled: true,
                                        peers_total: 1, peers_online: 1, ...extra });
  const p = page({
    "/api/mesh/pair/start": () => ({ ok: true, state: "PAIRING_INIT" }),
    "/api/mesh": [old("PAIRING_CONFIRM", { pairing_code: 0 }), old("ACTIVE")],
  });
  await p.t.startPairing("init");
  assert.strictEqual(p.t.state().pairingSeq, null);
  await p.poll();
  assert.strictEqual(p.el("pairingCodeValue").textContent, "000000");
  await p.poll();
  assert.ok(!p.polling());
  assert.strictEqual(p.alerts.length, 0);
  assert.strictEqual(p.refreshed(), 1);
});

test("pairingVerdict reads the fields, not the state", () => {
  const p = page({});
  const v = (body, seq) => p.t.pairingVerdict(body, seq);
  assert.strictEqual(v(after(7, "failed", "timeout", "ACTIVE"), 7), "failed");
  assert.strictEqual(v(after(7, "paired", "none", "ACTIVE"), 7), "paired");
  assert.strictEqual(v(after(7, "running", "none", "PAIRING_INIT"), 7), "waiting");
  assert.strictEqual(v({ ...after(7, "running", "none", "PAIRING_CONFIRM"), pairing_code: 0 }, 7), "code");
  // An initiator whose COMPLETE is out but whose member is not registered
  // yet reads running with a steady state: still waiting, not complete.
  assert.strictEqual(v(after(7, "running", "none", "CONNECTING"), 7), "waiting");
  assert.strictEqual(v(after(6, "paired", "none"), 7), "ended");
  assert.strictEqual(v(after(7, "none", "none"), 7), "ended");
  assert.strictEqual(v({ ok: true, state: "ACTIVE" }, null), "ended");
  assert.match(p.t.pairingFailText("crypto"), /key could not be made/);
  assert.match(p.t.pairingFailText(undefined), /no reason given/);
});
