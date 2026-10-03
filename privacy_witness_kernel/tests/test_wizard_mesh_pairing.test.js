// Host test for the kernel wizard's "Add another Canary" pairing
// (privacy_witness_kernel/wizard/index.html: meshStartPairing,
// meshPollForCodes, meshConfirm) — repo sweep F163.
//
// After both confirms, meshConfirm() waited up to 60 s for both Canaries'
// GET /api/mesh `state` to read ACTIVE and otherwise said "Pairing did not
// complete". A PlatformIO Canary reads ACTIVE only once it has heard a
// member this boot, and a new member sends nothing until it has an opera
// frame to send (F162), so a finished pairing read as not completed; and a
// failed one (a timeout, a refusal at the seal) was only ever "did not
// complete" a minute later. Since F133 a PlatformIO Canary's pair/start and
// pair/join answer the pairing number they started (pairing_seq), and
// GET /api/mesh reports that pairing's outcome (pairing_seq, pairing_result,
// pairing_fail_reason). What this pins:
//   - a pairing both Canaries report paired is a success, whatever their
//     state reads, and the done screen says they may not have heard from
//     each other yet;
//   - a pairing either reports failed stops the wait at once, names that
//     Canary and the reason in words, and cancels only a side still running;
//   - a body about another pairing (another number, or 0 after a restart)
//     stops the wait without claiming either;
//   - a Canary whose answers carry no number (from before F133, or a
//     canary-wap) is read the old way, done once it reads ACTIVE, also beside
//     a Canary that reports its outcome;
//   - the code-wait poll ends early the same way.
// The mesh section of the page's script is lifted out by literal markers
// and run against a stub DOM, a scripted api() (the add-on proxy's routes,
// told apart by the device address in the body) and an immediate delay(),
// the way firmware/tests_host/test_canary_mesh_pairing_poll.test.js runs the
// PlatformIO page's poll.
//
// Run: node --test privacy_witness_kernel/tests/test_wizard_mesh_pairing.test.js
// (CI: pwk-wizard-tests.yml.)

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const PAGE = process.env.WIZARD_PAGE || join(__dirname, "..", "wizard", "index.html");
const src = readFileSync(PAGE, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

const code =
  slice("let meshInit = null;",
        "// ---------------------------------------------------------------------------\n// Utilities") +
  "\n;globalThis.__t = { meshStartPairing, meshConfirm };\n";

const INIT = "10.0.0.11";
const JOIN = "10.0.0.22";

// A wizard page: stub DOM, api() answering from `script`, delay() at once.
//   script.start / script.join: the pair/start and pair/join answers;
//   script.codes / script.confirm: { init: [bodies], join: [bodies] } for
//   GET /api/mesh in each phase, the last body repeated once a queue runs
//   out; script.confirmAnswer: { init, join } (default {ok: true}).
// The page's elements that start hidden (class="hidden" in its markup).
const STARTS_HIDDEN = ["mesh-progress", "mesh-codes", "mesh-done", "mesh-done-note", "mesh-error"];

function page(script) {
  const els = {};
  const el = (id) => {
    if (!els[id]) {
      const cls = new Set(STARTS_HIDDEN.includes(id) ? ["hidden"] : []);
      els[id] = {
        id, value: "", textContent: "", innerHTML: "", disabled: false,
        classList: {
          add: (c) => cls.add(c),
          remove: (c) => cls.delete(c),
          toggle: (c, on) => { if (on === undefined ? !cls.has(c) : on) cls.add(c); else cls.delete(c); },
          contains: (c) => cls.has(c),
        },
      };
    }
    return els[id];
  };
  const calls = [];
  let phase = "codes";
  const polls = { codes: { init: 0, join: 0 }, confirm: { init: 0, join: 0 } };
  const ctx = {
    document: { getElementById: el },
    currentStep: 1,
    delay: async () => {},
    console,
    api: async (route, dev) => {
      const who = dev.address === INIT ? "init" : dev.address === JOIN ? "join" : "?";
      calls.push(`${route} ${who}`);
      if (route === "api/mesh/pair/start") return script.start;
      if (route === "api/mesh/pair/join") return script.join;
      if (route === "api/mesh/pair/cancel") return { ok: true };
      if (route === "api/mesh/pair/confirm") {
        phase = "confirm";
        return (script.confirmAnswer && script.confirmAnswer[who]) || { ok: true };
      }
      if (route === "api/mesh/status") {
        const q = script[phase][who];
        const n = polls[phase][who]++;
        return q[Math.min(n, q.length - 1)];
      }
      throw new Error(`unscripted route ${route}`);
    },
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  el("mesh-init-addr").value = INIT;
  el("mesh-join-addr").value = JOIN;
  return { t: ctx.__t, el, calls, polls };
}

const settle = () => new Promise((r) => setImmediate(r));
const visible = (p, id) => !p.el(id).classList.contains("hidden");

// Bodies a PlatformIO Canary since F133 answers.
const piom = (state, seq, result, reason = "none", extra = {}) =>
  Object.assign({ ok: true, state, pairing_seq: seq, pairing_result: result,
                  pairing_fail_reason: reason }, extra);
const codeBody = (seq) => piom("PAIRING_CONFIRM", seq, "running", "none", { pairing_code: 42 });
// A Canary from before F133 (or a canary-wap): no pairing number anywhere.
const legacy = (state, extra = {}) => Object.assign({ ok: true, state }, extra);

const F133 = {
  start: { ok: true, created: false, state: "PAIRING_INIT", pairing_seq: 3 },
  join: { ok: true, state: "PAIRING_JOIN", pairing_seq: 1 },
  codes: { init: [codeBody(3)], join: [codeBody(1)] },
};

async function toCodes(script) {
  const p = page(script);
  await p.t.meshStartPairing();
  await settle();
  assert.ok(visible(p, "mesh-codes"), "the codes are shown");
  return p;
}

test("a pairing both Canaries report paired is a success though neither reads ACTIVE", async () => {
  const p = await toCodes(Object.assign({}, F133, {
    confirm: {
      init: [piom("PAIRING_CONFIRM", 3, "running"), piom("CONNECTING", 3, "running"),
             piom("CONNECTING", 3, "paired")],
      join: [piom("CONNECTING", 1, "paired")],
    },
  }));
  await p.t.meshConfirm();
  assert.ok(visible(p, "mesh-done"), "the done screen is shown");
  assert.ok(!visible(p, "mesh-error"), p.el("mesh-error").textContent);
  assert.ok(visible(p, "mesh-done-note"));
  assert.match(p.el("mesh-done-note").textContent, /report the pairing finished/);
  assert.strictEqual(p.polls.confirm.init, 3, "it stops at the first poll both report paired");
  assert.ok(!p.calls.some((c) => c.startsWith("api/mesh/pair/cancel")));
});

test("a refusal at the seal stops the wait at once, names the Canary and the reason", async () => {
  const p = await toCodes(Object.assign({}, F133, {
    confirm: {
      init: [piom("CONNECTING", 3, "failed", "partner_refused")],
      join: [piom("PAIRING_JOIN", 1, "running")],
    },
  }));
  await p.t.meshConfirm();
  assert.ok(!visible(p, "mesh-done"));
  assert.ok(visible(p, "mesh-error"));
  const msg = p.el("mesh-error").textContent;
  assert.match(msg, /^Pairing failed on the existing Canary: that Canary cannot take the other/);
  assert.strictEqual(p.polls.confirm.init, 1, "no minute-long wait");
  // The new Canary's pairing is still running: it is canceled; the existing
  // Canary's has ended and is left alone.
  assert.ok(p.calls.includes("api/mesh/pair/cancel join"));
  assert.ok(!p.calls.includes("api/mesh/pair/cancel init"));
  assert.ok(visible(p, "mesh-form"), "back to the form");
});

test("a failure on the new Canary names it and its reason", async () => {
  const p = await toCodes(Object.assign({}, F133, {
    confirm: {
      init: [piom("CONNECTING", 3, "paired")],
      join: [piom("CONNECTING", 1, "failed", "bad_complete")],
    },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent,
    /^Pairing failed on the new Canary: the opera key from the other Canary could not be opened/);
  assert.ok(!p.calls.some((c) => c.startsWith("api/mesh/pair/cancel")),
    "a side that finished (paired or failed) is not canceled");
  assert.ok(!visible(p, "mesh-done"));
});

test("each reason is said in words; an unknown one is named", async () => {
  for (const [reason, words] of [
    ["timeout", /timed out before both Canaries confirmed the code/],
    ["canceled", /it was canceled\./],
    ["bad_confirm", /confirmation did not match/],
    ["crypto", /a pairing key could not be made/],
    ["mystery", /it ended \(mystery\)\./],
  ]) {
    const p = await toCodes(Object.assign({}, F133, {
      confirm: { init: [piom("CONNECTING", 3, "failed", reason)], join: [piom("PAIRING_JOIN", 1, "running")] },
    }));
    await p.t.meshConfirm();
    assert.match(p.el("mesh-error").textContent, words, reason);
  }
});

test("a body about another pairing stops the wait and claims neither", async () => {
  for (const other of [piom("CONNECTING", 4, "paired"),       // a later pairing
                       piom("NO_OPERA", 0, "none")]) {        // a restart
    const p = await toCodes(Object.assign({}, F133, {
      confirm: { init: [piom("CONNECTING", 3, "paired")], join: [other] },
    }));
    await p.t.meshConfirm();
    assert.ok(!visible(p, "mesh-done"));
    assert.match(p.el("mesh-error").textContent,
      /^The new Canary restarted or started another pairing, so this pairing's outcome is unknown/);
    assert.strictEqual(p.polls.confirm.join, 1);
  }
});

test("Canaries from before F133 are read the old way: done once both read ACTIVE", async () => {
  const old = {
    start: { ok: true, state: "PAIRING_INIT" },
    join: { ok: true, state: "PAIRING_JOIN" },
    codes: { init: [legacy("PAIRING_CONFIRM", { pairing_code: 7 })],
             join: [legacy("PAIRING_CONFIRM", { pairing_code: 7 })] },
  };
  let p = await toCodes(Object.assign({}, old, {
    confirm: { init: [legacy("CONNECTING"), legacy("ACTIVE")], join: [legacy("ACTIVE")] },
  }));
  await p.t.meshConfirm();
  assert.ok(visible(p, "mesh-done"));
  assert.ok(!visible(p, "mesh-done-note"), "an ACTIVE success needs no note");
  assert.strictEqual(p.polls.confirm.init, 2);

  // Never ACTIVE: the old minute-long wait and the old message.
  p = await toCodes(Object.assign({}, old, {
    confirm: { init: [legacy("CONNECTING")], join: [legacy("CONNECTING")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.polls.confirm.init, 30);
  assert.strictEqual(p.el("mesh-error").textContent,
    "Pairing did not complete. Check both Canaries and retry.");

  // A body carrying the fields when the answer carried no number (another
  // client's pairing, say) is still read the old way.
  p = await toCodes(Object.assign({}, old, {
    confirm: { init: [piom("CONNECTING", 9, "paired")], join: [legacy("ACTIVE")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.polls.confirm.init, 30, "not done: CONNECTING, and no number of its own");
});

test("a Canary that reports its outcome beside one that does not", async () => {
  const p = await toCodes({
    start: F133.start,
    join: { ok: true, state: "PAIRING_JOIN" },                // a canary-wap joiner
    codes: { init: [codeBody(3)], join: [legacy("PAIRING_CONFIRM", { pairing_code: 42 })] },
    confirm: {
      init: [piom("CONNECTING", 3, "paired")],
      join: [legacy("CONNECTING"), legacy("CONNECTING"), legacy("ACTIVE")],
    },
  });
  await p.t.meshConfirm();
  assert.ok(visible(p, "mesh-done"));
  assert.strictEqual(p.polls.confirm.join, 3, "the canary-wap side is done at ACTIVE");
});

test("the code wait ends early on a failure or on a pairing finished elsewhere", async () => {
  // A timeout before the codes ever showed on both.
  let p = page(Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running"), piom("CONNECTING", 3, "failed", "timeout")],
             join: [piom("PAIRING_JOIN", 1, "running")] },
  }));
  await p.t.meshStartPairing();
  await settle();
  assert.match(p.el("mesh-error").textContent,
    /^Pairing failed on the existing Canary: it timed out before both Canaries confirmed/);
  assert.strictEqual(p.polls.codes.init, 2, "not the 60-poll wait for a code");
  assert.ok(p.calls.includes("api/mesh/pair/cancel join"));

  // Both confirmed on the Canaries themselves: paired, never ACTIVE.
  p = page(Object.assign({}, F133, {
    codes: { init: [piom("CONNECTING", 3, "paired")], join: [piom("CONNECTING", 1, "paired")] },
  }));
  await p.t.meshStartPairing();
  await settle();
  assert.ok(visible(p, "mesh-done"));
  assert.ok(!visible(p, "mesh-error"));
});

test("a 0 is no pairing number: that Canary is read the old way", async () => {
  // pair/start never answers 0 for a pairing it started; a 0 there (or a
  // missing number) means the outcome cannot be told apart from an earlier
  // pairing's, so ACTIVE decides, as before F133.
  const p = await toCodes({
    start: { ok: true, state: "PAIRING_INIT", pairing_seq: 0 },
    join: { ok: true, state: "PAIRING_JOIN" },
    codes: { init: [piom("PAIRING_CONFIRM", 0, "none", "none", { pairing_code: 5 })],
             join: [legacy("PAIRING_CONFIRM", { pairing_code: 5 })] },
    confirm: { init: [piom("ACTIVE", 0, "none")], join: [legacy("ACTIVE")] },
  });
  await p.t.meshConfirm();
  assert.ok(visible(p, "mesh-done"), p.el("mesh-error").textContent);
  assert.strictEqual(p.polls.confirm.init, 1);
});
