// Host test for the kernel wizard's "Add another Canary" pairing
// (privacy_witness_kernel/wizard/index.html: meshStartPairing,
// meshPollForCodes, meshConfirm) — repo sweeps F163 and F200.
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
//     stops the wait without claiming either, and cancels a side still
//     running this wizard's pairing, so a retry is not refused until that
//     Canary's own 5-minute timeout;
//   - giving up at either wait's timeout cancels what the last poll saw
//     still running this wizard's pairing, and nothing on a poll that did
//     not read both, nor a Canary that reports no number;
//   - a Canary whose answers carry no number (from before F133, or a
//     canary-wap) is read the old way, done once it reads ACTIVE, also beside
//     a Canary that reports its outcome;
//   - the code-wait poll ends early the same way;
//   - (F200) a confirmation either Canary rejects, a confirm that fails on
//     the network, and five unreadable polls in either wait each read both
//     Canaries' status once and cancel a side that reports this wizard's
//     pairing still running, and nothing else: a Canary starts a pairing
//     only when none runs, so left alone it refuses the retry those
//     messages invite until its own 5-minute timeout. Each cancel goes out
//     before the message (while Start is still disabled);
//   - (F200's review) beside a side that reports this pairing paired nothing
//     is canceled: the other is completing (F134); a confirm whose answer was
//     lost then waits for the outcome as for any other answer;
//   - work an attempt began stops at its next await once the user cancels
//     or starts again: a cleanup still reading, or a wait asleep, sends
//     nothing more and leaves the screen to the new attempt;
//   - a retry finds Start ready (after a failure, a success or Cancel), the
//     wait's line reset, and at its codes a confirm button that is ready.
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
  "\n;globalThis.__t = { meshStartPairing, meshConfirm, meshCancel, openMeshWizard };\n";

const INIT = "10.0.0.11";
const JOIN = "10.0.0.22";

// A wizard page: stub DOM, api() answering from `script`, delay() at once.
//   script.start / script.join: the pair/start and pair/join answers;
//   script.codes / script.confirm: { init: [bodies], join: [bodies] } for
//   GET /api/mesh in each phase, the last body repeated once a queue runs
//   out; script.confirmAnswer: { init, join } (default {ok: true}). THROW in
//   place of a body or an answer makes that request fail on the network
//   (api() rejects, as fetch() does for an unreachable Canary).
//   script.hold(route, who): when given, each request awaits what it
//   returns before answering (a test holds one read in flight that way);
//   script.delay(): when given, delay() awaits it.
// Each pair/cancel also records what the page showed when it was sent
// (p.cancelSeen): whether Start was disabled, whether a message was shown,
// and the status line.
// The page's elements that start hidden (class="hidden" in its markup).
const STARTS_HIDDEN = ["mesh-progress", "mesh-codes", "mesh-done", "mesh-done-note", "mesh-error"];
const THROW = Symbol("network error");
const answer = (a) => { if (a === THROW) throw new TypeError("Failed to fetch"); return a; };

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
  const cancelSeen = [];
  let phase = "codes";
  const polls = { codes: { init: 0, join: 0 }, confirm: { init: 0, join: 0 } };
  const ctx = {
    document: { getElementById: el },
    currentStep: 1,
    delay: async () => { if (script.delay) await script.delay(); },
    console,
    api: async (route, dev) => {
      const who = dev.address === INIT ? "init" : dev.address === JOIN ? "join" : "?";
      calls.push(`${route} ${who}`);
      if (route === "api/mesh/pair/cancel") {
        cancelSeen.push({ who, startDisabled: el("mesh-start-btn").disabled,
                          messageShown: !el("mesh-error").classList.contains("hidden"),
                          line: el("mesh-status-line").innerHTML });
      }
      if (script.hold) await script.hold(route, who);
      if (route === "api/mesh/pair/start") { phase = "codes"; return script.start; }
      if (route === "api/mesh/pair/join") return script.join;
      if (route === "api/mesh/pair/cancel") return { ok: true };
      if (route === "api/mesh/pair/confirm") {
        phase = "confirm";
        return answer((script.confirmAnswer && script.confirmAnswer[who]) || { ok: true });
      }
      if (route === "api/mesh/status") {
        const q = script[phase][who];
        const n = polls[phase][who]++;
        return answer(q[Math.min(n, q.length - 1)]);
      }
      throw new Error(`unscripted route ${route}`);
    },
  };
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  el("mesh-init-addr").value = INIT;
  el("mesh-join-addr").value = JOIN;
  return { t: ctx.__t, el, calls, polls, cancelSeen };
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
    assert.ok(!p.calls.some((c) => c.startsWith("api/mesh/pair/cancel")),
      "the existing Canary finished its pairing; the new one's is not ours to cancel");
  }
});

test("a body about another pairing cancels the side still running this one", async () => {
  // The new Canary restarted mid-pairing while the existing one still runs
  // pairing #3: left alone, the existing Canary refuses the retry's
  // pair/start until #3 times out (5 minutes). Its body carries #3, and a
  // Canary runs one pairing at a time, so the cancel ends only that one.
  let p = await toCodes(Object.assign({}, F133, {
    confirm: { init: [piom("PAIRING_CONFIRM", 3, "running")], join: [piom("NO_OPERA", 0, "none")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent,
    /^The new Canary restarted or started another pairing/);
  assert.ok(p.calls.includes("api/mesh/pair/cancel init"), p.calls.join(", "));
  assert.ok(!p.calls.includes("api/mesh/pair/cancel join"), "the restarted side runs nothing of ours");
  assert.ok(visible(p, "mesh-form"), "back to the form for the retry");

  // The other way round: the existing Canary is on a later pairing.
  p = await toCodes(Object.assign({}, F133, {
    confirm: { init: [piom("PAIRING_INIT", 4, "running")], join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent,
    /^The existing Canary restarted or started another pairing/);
  assert.ok(p.calls.includes("api/mesh/pair/cancel join"));
  assert.ok(!p.calls.includes("api/mesh/pair/cancel init"), "pairing #4 is someone else's");
});

test("giving up cancels what the last poll saw still running this pairing", async () => {
  // The confirm wait: the existing Canary never finishes; the new one has.
  let p = await toCodes(Object.assign({}, F133, {
    confirm: { init: [piom("PAIRING_CONFIRM", 3, "running")], join: [piom("CONNECTING", 1, "paired")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.polls.confirm.init, 30);
  assert.strictEqual(p.el("mesh-error").textContent,
    "Pairing did not complete. Check both Canaries and retry.");
  assert.ok(p.calls.includes("api/mesh/pair/cancel init"), p.calls.join(", "));
  assert.ok(!p.calls.includes("api/mesh/pair/cancel join"), "a finished side is left as it finished");

  // The code wait: neither ever shows a code.
  p = page(Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running")], join: [piom("PAIRING_JOIN", 1, "running")] },
  }));
  await p.t.meshStartPairing();
  await settle();
  assert.strictEqual(p.polls.codes.init, 60);
  assert.strictEqual(p.el("mesh-error").textContent,
    "Timed out waiting for the pairing code. Try again.");
  assert.ok(p.calls.includes("api/mesh/pair/cancel init"));
  assert.ok(p.calls.includes("api/mesh/pair/cancel join"));

  // A last poll that did not read both cancels nothing: an older read may
  // be about a pairing that has since ended.
  const running = Array(29).fill(piom("PAIRING_CONFIRM", 3, "running"));
  p = await toCodes(Object.assign({}, F133, {
    confirm: { init: running.concat([{ ok: false, error: "unreachable" }]),
               join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.polls.confirm.init, 30);
  assert.strictEqual(p.el("mesh-error").textContent,
    "Pairing did not complete. Check both Canaries and retry.");
  assert.ok(!p.calls.some((c) => c.startsWith("api/mesh/pair/cancel")), p.calls.join(", "));

  // The same in the code wait.
  p = page(Object.assign({}, F133, {
    codes: { init: Array(59).fill(piom("PAIRING_INIT", 3, "running")).concat([{ ok: false }]),
             join: [piom("PAIRING_JOIN", 1, "running")] },
  }));
  await p.t.meshStartPairing();
  await settle();
  assert.strictEqual(p.polls.codes.init, 60);
  assert.strictEqual(p.el("mesh-error").textContent,
    "Timed out waiting for the pairing code. Try again.");
  assert.ok(!p.calls.some((c) => c.startsWith("api/mesh/pair/cancel")), p.calls.join(", "));
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
  assert.ok(!p.calls.some((c) => c.startsWith("api/mesh/pair/cancel")),
    "a Canary that reports no number is not canceled at the timeout, as before");

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

// ── F200: the paths that end the pairing without a read of both sides ────

const cancels = (p) => p.calls.filter((c) => c.startsWith("api/mesh/pair/cancel"));

// The order on those paths: each cancel goes out while the attempt still
// holds the screen (Start disabled, no message yet, the status line saying
// both Canaries are being checked). The message re-enables Start, and a
// retry begun before the last cancel was sent could have its own pairing
// read and canceled (F200's review).
function cancelsBeforeTheMessage(p) {
  assert.ok(p.cancelSeen.length > 0, "a cancel was sent");
  for (const c of p.cancelSeen) {
    assert.ok(c.startDisabled, `the ${c.who} cancel went out with Start ready`);
    assert.ok(!c.messageShown, `the ${c.who} cancel went out after the message`);
    assert.match(c.line, /Checking both Canaries…/, `the ${c.who} cancel's status line`);
  }
}

test("a confirmation the existing Canary rejects cancels the new one still running", async () => {
  // The existing Canary refused at the confirm (409 partner_refused): its
  // pairing has failed; the new one confirmed and waits on it for 5 minutes.
  const p = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { init: { ok: false, error: "partner_refused" } },
    confirm: { init: [piom("CONNECTING", 3, "failed", "partner_refused")],
               join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.el("mesh-error").textContent,
    "The existing Canary rejected the confirmation: partner_refused");
  assert.deepStrictEqual(p.polls.confirm, { init: 1, join: 1 }, "each side is read once");
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel join"]);
  cancelsBeforeTheMessage(p);
  assert.ok(visible(p, "mesh-form"), "back to the form for the retry");
  assert.ok(!p.el("mesh-start-btn").disabled);

  // A refusal that ended neither pairing (an error body while both still
  // run): both are canceled, the second also before the message.
  const q = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { init: { ok: false, error: "busy" } },
    confirm: { init: [piom("PAIRING_CONFIRM", 3, "running")],
               join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await q.t.meshConfirm();
  assert.strictEqual(q.el("mesh-error").textContent, "The existing Canary rejected the confirmation: busy");
  assert.deepStrictEqual(cancels(q), ["api/mesh/pair/cancel init", "api/mesh/pair/cancel join"]);
  cancelsBeforeTheMessage(q);
});

test("a confirmation the new Canary rejects cancels the existing one still running", async () => {
  // The new Canary restarted after the codes showed (a power pull): its
  // confirm answers not_pairing and its body is about no pairing of ours.
  let p = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { join: { ok: false, error: "not_pairing" } },
    confirm: { init: [piom("PAIRING_CONFIRM", 3, "running")],
               join: [piom("NO_OPERA", 0, "none")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.el("mesh-error").textContent,
    "The new Canary rejected the confirmation: not_pairing");
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel init"]);
  cancelsBeforeTheMessage(p);

  // Both finished as they finished (one paired, one failed): nothing to end.
  p = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { join: { ok: false, error: "bad_confirm" } },
    confirm: { init: [piom("CONNECTING", 3, "paired")],
               join: [piom("CONNECTING", 1, "failed", "bad_confirm")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent, /^The new Canary rejected the confirmation/);
  assert.deepStrictEqual(cancels(p), [], "a side that finished is left as it finished");
});

test("a confirm that fails on the network reads each side once and cancels the one running", async () => {
  // The existing Canary lost power after the codes showed: its confirm never
  // answers, the new one's is never sent, and the new one still runs #1.
  const p = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { init: THROW },
    confirm: { init: [THROW], join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent, /^Network error during confirm: Failed to fetch/);
  assert.ok(!p.calls.includes("api/mesh/pair/confirm join"), "the second confirm was never sent");
  assert.deepStrictEqual(p.polls.confirm, { init: 1, join: 1 },
    "the unreachable side does not stop the other side's read");
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel join"]);
  cancelsBeforeTheMessage(p);
  assert.ok(visible(p, "mesh-form"));
});

test("five unreadable polls in the code wait cancel the side still running", async () => {
  // Unreachable (fetch rejects): the existing Canary never answers again.
  let p = page(Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running"), THROW],
             join: [piom("PAIRING_JOIN", 1, "running")] },
  }));
  await p.t.meshStartPairing();
  await settle();
  assert.strictEqual(p.el("mesh-error").textContent,
    "A Canary became unreachable. Check the network and try again.");
  assert.strictEqual(p.polls.codes.init, 7, "one good poll, five failed, then the one read");
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel join"]);
  cancelsBeforeTheMessage(p);

  // Answering, but not ok (the add-on proxy's error body): the same.
  p = page(Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running")],
             join: [piom("PAIRING_JOIN", 1, "running"), { ok: false, error: "timeout" }] },
  }));
  await p.t.meshStartPairing();
  await settle();
  assert.strictEqual(p.el("mesh-error").textContent, "A Canary stopped responding. Try again.");
  assert.strictEqual(p.polls.codes.join, 7);
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel init"]);
  cancelsBeforeTheMessage(p);
});

test("five unreadable polls in the confirm wait cancel the side still running", async () => {
  const p = await toCodes(Object.assign({}, F133, {
    confirm: { init: [THROW], join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.el("mesh-error").textContent,
    "A Canary became unreachable while completing. Try again.");
  assert.strictEqual(p.polls.confirm.init, 6, "five failed polls, then the one read");
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel join"]);
  cancelsBeforeTheMessage(p);
});

test("on those paths a Canary that reports no number, or another pairing, is not canceled", async () => {
  // Canaries from before F133 (or canary-wap): no number to tell this
  // pairing by, so nothing is canceled, as before F200.
  const old = {
    start: { ok: true, state: "PAIRING_INIT" },
    join: { ok: true, state: "PAIRING_JOIN" },
    codes: { init: [legacy("PAIRING_CONFIRM", { pairing_code: 7 })],
             join: [legacy("PAIRING_CONFIRM", { pairing_code: 7 })] },
  };
  let p = await toCodes(Object.assign({}, old, {
    confirmAnswer: { init: { ok: false, error: "nope" } },
    confirm: { init: [legacy("PAIRING_CONFIRM")], join: [legacy("PAIRING_CONFIRM")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent, /^The existing Canary rejected the confirmation: nope/);
  assert.deepStrictEqual(cancels(p), []);

  // A Canary on a later pairing (someone else's) is left alone.
  p = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { init: THROW },
    confirm: { init: [piom("PAIRING_INIT", 4, "running")], join: [piom("CONNECTING", 1, "paired")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent, /^Network error during confirm/);
  assert.deepStrictEqual(cancels(p), [], "pairing #4 is not this wizard's; #1 finished");
});

test("a confirm answer lost while a side reports this pairing paired cancels nothing and waits", async () => {
  // The new Canary's confirm landed, but its answer did not: a reply lost on
  // the way back, or an error body after the Canary acted (the proxy's
  // timeout, mesh_timeout). The existing Canary reports #3 paired (it got
  // there only on the new one's confirm) and re-sends the opera key until the
  // new one has it (F134); the new one still runs #1. Canceling it would leave
  // the existing Canary holding a member that never joined.
  for (const lost of [THROW,
                      { ok: false, error: "Device HTTP error 503: {\"error\":\"mesh_timeout\"}" }]) {
    const lines = [];  // the status line at each read of the new Canary after the confirms
    let seen = null;   // the page, once its codes show
    const p = await toCodes(Object.assign({}, F133, {
      confirmAnswer: { join: lost },
      confirm: { init: [piom("CONNECTING", 3, "paired")],
                 join: [piom("PAIRING_CONFIRM", 1, "running"), piom("PAIRING_CONFIRM", 1, "running"),
                        piom("CONNECTING", 1, "paired")] },
      hold: async (route, who) => {
        if (seen && route === "api/mesh/status" && who === "join") {
          lines.push(seen.el("mesh-status-line").innerHTML);
        }
      },
    }));
    seen = p;
    await p.t.meshConfirm();
    assert.deepStrictEqual(cancels(p), [], String(lost.error || "network"));
    assert.ok(visible(p, "mesh-done"), p.el("mesh-error").textContent);
    assert.ok(!visible(p, "mesh-error"));
    assert.strictEqual(p.polls.confirm.join, 3, "the one read, then the wait until it reports paired");
    assert.match(lines[0], /Checking both Canaries…/);
    assert.match(lines[1], /Completing pairing…/, "the wait says it is completing again");
  }

  // The wait's verdicts still decide: the new Canary then reports it failed.
  const p = await toCodes(Object.assign({}, F133, {
    confirmAnswer: { join: THROW },
    confirm: { init: [piom("CONNECTING", 3, "paired")],
               join: [piom("PAIRING_CONFIRM", 1, "running"), piom("CONNECTING", 1, "failed", "bad_complete")] },
  }));
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent,
    /^Pairing failed on the new Canary: the opera key from the other Canary could not be opened/);
  assert.deepStrictEqual(cancels(p), []);
});

test("five unreadable polls in the confirm wait cancel nothing beside a side that reports paired", async () => {
  const p = await toCodes(Object.assign({}, F133, {
    confirm: { init: [THROW, THROW, THROW, THROW, THROW, piom("CONNECTING", 3, "paired")],
               join: [piom("PAIRING_CONFIRM", 1, "running")] },
  }));
  await p.t.meshConfirm();
  assert.strictEqual(p.el("mesh-error").textContent,
    "A Canary became unreachable while completing. Try again.");
  assert.strictEqual(p.polls.confirm.init, 6);
  assert.deepStrictEqual(cancels(p), [], "the new Canary is completing, not stuck");
});

// A gate the test opens by hand: wait() parks until open() is called.
function gate() {
  let waiters = [];
  return {
    wait: () => new Promise((r) => waiters.push(r)),
    open: () => { const w = waiters; waiters = []; w.forEach((r) => r()); },
    get parked() { return waiters.length; },
  };
}
const ticks = async (n = 20) => { for (let i = 0; i < n; i++) await settle(); };

test("a cleanup still reading when the user cancels and starts again leaves the new attempt alone", async () => {
  // Five unreadable polls; the cleanup's first read (of the existing Canary)
  // hangs, as the add-on proxy's does for up to 10 s. Meanwhile the user
  // presses Cancel, reopens the wizard and starts again (pairings #8 and #4),
  // and then the old read answers, about the new pairing.
  const script = Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running")], join: [THROW] },
  });
  const read = gate();
  let held = false;
  script.hold = async (route, who) => {
    if (route === "api/mesh/status" && who === "init" && !held && p.polls.codes.init === 5) {
      held = true;
      await read.wait();
    }
  };
  const p = page(script);
  await p.t.meshStartPairing();
  await ticks();
  assert.strictEqual(read.parked, 1, "the cleanup's first read is in flight");
  assert.ok(visible(p, "mesh-progress"));
  assert.match(p.el("mesh-status-line").innerHTML, /Checking both Canaries…/);

  await p.t.meshCancel();
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel init", "api/mesh/pair/cancel join"]);
  p.t.openMeshWizard();
  assert.ok(!p.el("mesh-start-btn").disabled, "Start is ready in the reopened wizard");
  assert.strictEqual(p.el("mesh-start-btn").innerHTML, "Start Pairing");

  const poll = gate();
  Object.assign(script, {
    start: { ok: true, state: "PAIRING_INIT", pairing_seq: 8 },
    join: { ok: true, state: "PAIRING_JOIN", pairing_seq: 4 },
    codes: { init: [piom("PAIRING_INIT", 8, "running")], join: [piom("PAIRING_JOIN", 4, "running")] },
    delay: () => poll.wait(),
  });
  await p.t.meshStartPairing();
  await ticks();
  assert.strictEqual(poll.parked, 1, "the new attempt's wait is running");
  const before = p.calls.length;

  read.open();  // the old read answers, about pairing #8
  await ticks();
  assert.deepStrictEqual(p.calls.slice(before), [],
    "the old cleanup sends nothing more: no read of the new Canary, no cancel");
  assert.ok(!visible(p, "mesh-error"), p.el("mesh-error").textContent);
  assert.ok(visible(p, "mesh-progress"), "the new attempt keeps the screen");
  assert.ok(!visible(p, "mesh-form"));

  // The new attempt's wait goes on and reaches its codes.
  script.codes = { init: [codeBody(8)], join: [codeBody(4)] };
  poll.open();
  await ticks();
  assert.ok(visible(p, "mesh-codes"), "the new attempt reaches its codes");
  assert.deepStrictEqual(cancels(p), ["api/mesh/pair/cancel init", "api/mesh/pair/cancel join"],
    "only the user's own Cancel was ever sent");
});

test("a wait from a canceled attempt stops at its next await", async () => {
  // The user cancels while the code wait sleeps between polls, and starts
  // again before it wakes: only the new attempt's wait polls.
  const poll = gate();
  const script = Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running")], join: [piom("PAIRING_JOIN", 1, "running")] },
    delay: () => poll.wait(),
  });
  const p = page(script);
  await p.t.meshStartPairing();
  await ticks();
  assert.strictEqual(poll.parked, 1);
  await p.t.meshCancel();
  p.t.openMeshWizard();
  Object.assign(script, {
    start: { ok: true, state: "PAIRING_INIT", pairing_seq: 4 },
    join: { ok: true, state: "PAIRING_JOIN", pairing_seq: 2 },
    codes: { init: [piom("PAIRING_INIT", 4, "running")], join: [piom("PAIRING_JOIN", 2, "running")] },
  });
  await p.t.meshStartPairing();
  await ticks();
  assert.strictEqual(poll.parked, 2, "both waits sleep");
  poll.open();
  await ticks();
  assert.deepStrictEqual(p.polls.codes, { init: 1, join: 1 }, "one wait polled, not two");
  assert.strictEqual(poll.parked, 1, "and only that one sleeps again");
});

test("a retry offers a ready Start and, at its codes, a ready confirm", async () => {
  // A confirmation the new Canary rejects (it restarted): the message
  // invites a retry, and the retry reaches the codes again.
  const script = Object.assign({}, F133, {
    confirmAnswer: { join: { ok: false, error: "not_pairing" } },
    confirm: { init: [piom("PAIRING_CONFIRM", 3, "running")], join: [piom("NO_OPERA", 0, "none")] },
  });
  const lines = [];
  script.hold = async (route) => {
    if (route === "api/mesh/status") lines.push(p.el("mesh-status-line").innerHTML);
  };
  const p = page(script);
  await p.t.meshStartPairing();
  await settle();
  await p.t.meshConfirm();
  assert.match(p.el("mesh-error").textContent, /^The new Canary rejected the confirmation/);
  assert.ok(!p.el("mesh-start-btn").disabled);

  Object.assign(script, {
    start: { ok: true, state: "PAIRING_INIT", pairing_seq: 4 },
    join: { ok: true, state: "PAIRING_JOIN", pairing_seq: 2 },
    codes: { init: [codeBody(4)], join: [codeBody(2)] },
    confirmAnswer: {},
    confirm: { init: [piom("CONNECTING", 4, "paired")], join: [piom("CONNECTING", 2, "paired")] },
  });
  lines.length = 0;
  await p.t.meshStartPairing();
  await settle();
  assert.match(lines[0], /Pairing in progress…/, "the retry's wait does not show the last attempt's line");
  assert.ok(visible(p, "mesh-codes"), "the retry reaches the codes");
  assert.ok(!p.el("mesh-confirm-btn").disabled, "confirm is ready, not left \"Confirming…\"");
  assert.strictEqual(p.el("mesh-confirm-btn").innerHTML, "Codes match — confirm");

  // And once it succeeds, Start is ready for the next Canary.
  await p.t.meshConfirm();
  assert.ok(visible(p, "mesh-done"), p.el("mesh-error").textContent);
  assert.ok(!p.el("mesh-start-btn").disabled, "Start is ready for the next Canary");
  assert.strictEqual(p.el("mesh-start-btn").innerHTML, "Start Pairing");
});

test("Cancel during a cleanup's read ends it: nothing more is sent or shown", async () => {
  const script = Object.assign({}, F133, {
    codes: { init: [piom("PAIRING_INIT", 3, "running")], join: [THROW] },
  });
  const read = gate();
  let held = false;
  script.hold = async (route, who) => {
    if (route === "api/mesh/status" && who === "init" && !held && p.polls.codes.init === 5) {
      held = true;
      await read.wait();
    }
  };
  const p = page(script);
  await p.t.meshStartPairing();
  await ticks();
  assert.strictEqual(read.parked, 1);
  await p.t.meshCancel();
  p.t.openMeshWizard();
  const before = p.calls.length;
  read.open();
  await ticks();
  assert.deepStrictEqual(p.calls.slice(before), [], "the user's Cancel already ended both sides");
  assert.ok(!visible(p, "mesh-error"), "no message from the canceled attempt on the reopened form");
  assert.ok(!p.el("mesh-start-btn").disabled);
});
