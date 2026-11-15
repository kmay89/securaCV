// Host test for firmware/canary's dashboard unlock banner and the BOOT tap
// (canary/lib/securacv_webui/src/securacv_webui.cpp) — repo sweep F354.
//
// The flagship's BOOT tap goes to ONE asker since F354: its receipt and its
// home-LAN page loads ask provisioning_gate.h's tap_arbitrate(), so a tap
// is served only to the sole address that asked from 10 s before the press
// onward, and a second address in that window closes it (409
// tap_contended). The banner used to say "Tap the BOOT button on the Canary
// and reload within 30 seconds": the press first, the ask after, the one
// order the arbiter cannot protect (a host watching for the tap asks first).
// It now asks first:
//   - while it is shown after the page loads, it polls GET
//     /api/provisioning-receipt every 2 s, from before the tap, and says
//     "keep this page open, then tap BOOT", with "don't press yet" until the
//     Canary has answered an ask;
//   - a 200 sets CV_TOKEN from the receipt's token, with no reload, hides
//     the banner and stops the poll;
//   - a 409 says another device asked, and stops the poll;
//   - the poll stops when its one-minute window closes and on pagehide
//     (clearing its line and offering its button), and a page brought back
//     from the back/forward cache asks again; it keeps asking while the tab
//     is hidden, within that window, since a page that stopped while its
//     line said "press" would leave a host polling the receipt the one
//     asker (a draft that paused while hidden handed a press made 12 s
//     into the hidden period to the host); an ask that never answers is given up after
//     10 s, and the line goes back to "don't press yet"; it never runs two
//     timers or two requests at once, however often its button is pressed
//     or the tab is hidden and shown again;
//   - Save recovery kit, on a page with no token, starts the same poll and
//     says to press when the banner says to, never "tap, then try again";
//     its credential-less ask that the poll's tap overtakes asks again with
//     the token, instead of saying the Canary refused a token it never
//     sent.
// The two-asker cases run the dashboard's own poll against the firmware's
// own tap step: tap_step_oracle.cpp, which compiles provisioning_gate_take()
// cut verbatim from securacv_network.cpp over the real provisioning_gate.h
// (the Makefile builds it and passes TAP_STEP_ORACLE; run alone, this test
// builds it). The owner's page (A) and a host curling the receipt in a loop
// (B) never get two receipts, and B never gets one. The 409 body the test
// answers is the one securacv_network.cpp sends, and the 403 the one its
// builder makes.
//
// The page's functions are lifted out of the C++ raw string by literal
// markers that stand in the page before and after F354 (so the old page
// runs here and fails on what it does, not on a missing marker), and run in
// a vm context with a stub DOM, a fake clock that owns every timer, and a
// fetch that answers from the device model.
//
// Run: node --test firmware/tests_host/test_canary_provisioning_banner.test.js
// (the Makefile's `run` target does, after the C++ suites).

"use strict";

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync, mkdtempSync } = require("node:fs");
const { join } = require("node:path");
const { tmpdir } = require("node:os");
const { execFileSync, spawnSync } = require("node:child_process");
const vm = require("node:vm");

const FW = join(__dirname, "..");
const UI = process.env.CANARY_WEBUI || join(FW, "canary", "lib", "securacv_webui", "src", "securacv_webui.cpp");
const NET = process.env.CANARY_NETWORK_CPP ||
  join(FW, "canary", "lib", "securacv_network", "src", "securacv_network.cpp");
const src = readFileSync(UI, "utf8");

function slice(from, to) {
  const a = src.indexOf(from);
  const b = a < 0 ? -1 : src.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return src.slice(a, b);
}

// The token helpers through the recovery kit (and, since F354, the banner's
// poll), and the page's initialize block up to the live-sensing poll.
const helpers = slice("    function tokenIsMissing()", "    // API calls");
const init = slice("    refreshLockBanner();\n    refreshStatus();", "    /* Live sensing updates only matter");

// ── The firmware's tap step, as a process ────────────────────────────────

let oracleBin = null;
function oracle() {
  if (oracleBin) return oracleBin;
  if (process.env.TAP_STEP_ORACLE && existsSync(process.env.TAP_STEP_ORACLE)) {
    oracleBin = process.env.TAP_STEP_ORACLE;
    return oracleBin;
  }
  // Run outside the Makefile: build it (the Makefile's recipe, same flags).
  const dir = mkdtempSync(join(tmpdir(), "tap-step-oracle-"));
  const inc = join(dir, "tap_step.inc");
  const cut = execFileSync("awk", ["-v", "sigs=static bool provisioning_gate_take(httpd_req_t* req,",
                                   "-f", join(__dirname, "cut_functions.awk"), NET], { encoding: "utf8" });
  require("node:fs").writeFileSync(inc, cut);
  oracleBin = join(dir, "tap_step_oracle");
  execFileSync(process.env.CXX || "g++",
    ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", join(FW, "canary", "include"),
     "-I", join(FW, "common"), `-DTAP_STEP_INC="${inc}"`, join(__dirname, "tap_step_oracle.cpp"),
     "-o", oracleBin], { stdio: "inherit" });
  return oracleBin;
}

function runOracle(lines) {
  const res = spawnSync(oracle(), { input: lines.join("\n") + "\n", encoding: "utf8" });
  assert.strictEqual(res.status, 0, `tap_step_oracle failed: ${res.stderr}`);
  const out = res.stdout.trim().split("\n");
  assert.strictEqual(out.length, lines.length, "one answer per command");
  return out;
}

// The flagship's 409 body, as handle_provisioning_receipt() sends it (the
// one C string literal that starts {"error":"tap_contended",), and its 403,
// as build_gate_refusal_json() makes it at the build's TTL.
function contendedBody() {
  const net = readFileSync(NET, "utf8");
  const open = '"{\\"error\\":\\"tap_contended\\",';
  const at = net.indexOf(open);
  assert.ok(at >= 0 && net.indexOf(open, at + 1) < 0, "securacv_network.cpp sends one 409 tap_contended body");
  let out = "";
  for (let i = at + 1; i < net.length && net[i] !== '"'; i++) {
    if (net[i] === "\\") i++;
    out += net[i];
  }
  return out;
}
const BODY_409 = contendedBody();
const BODY_403 = runOracle(["body403"])[0];
const TOKEN = "cv_RECEIPTtoken0123456789abcdefghj";
const RECEIPT = JSON.stringify({ device_id: "canary-test", base_url: "http://192.168.1.50", token: TOKEN,
                                 ap_ssid: "SecuraCV-canary-test", ap_password: "cv-test",
                                 tls_cert_fp: "", provisioned_at: "boot:1" });

// The device: every credential-less ask goes through the tap step (the
// oracle replays the whole history each time, so its state is the
// firmware's after exactly these asks and presses).
function device() {
  const events = [];
  const log = [];
  return {
    log,
    press(at) { events.push(`tap ${at}`); },
    ask(peer, at, who) {
      events.push(`ask ${peer} ${at}`);
      const out = runOracle(events);
      const v = out[out.length - 1];
      log.push({ who, at, v });
      return v;
    },
    answer(v) {
      if (v === "TAKE") return { status: 200, body: RECEIPT };
      if (v === "CONTENDED") return { status: 409, body: BODY_409 };
      return { status: 403, body: BODY_403 };
    },
  };
}

// ── A page on a fake clock ───────────────────────────────────────────────

const A_PEER = "192.168.1.20";
const B_PEER = "192.168.1.21";

const flush = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };

// One dashboard tab served WITHOUT the token (the home-LAN page), its page
// load an ask at 0 ms (page_token_inject asks the same step), then its
// initialize block run. `answer(url, opts, now)` (default: the device for
// A's peer) answers each fetch; `latency` delays every answer; `hang(n)`
// true leaves the n-th fetch (1-based) unanswered until its signal aborts.
function page({ dev = device(), peer = () => A_PEER, latency = 0, answer = null, hang = null } = {}) {
  const els = {};
  const el = (id) => els[id] || (els[id] = {
    id, style: { display: id === "lanLockBanner" ? "none" : "" }, textContent: "", value: "",
    click() {}, remove() {},
  });
  const timers = new Map();    // the page's own (its setTimeout), id → { at, fn }
  const harness = [];          // the test's own events (a press, B's curl loop, an answer due)
  let nextId = 1;
  let now = 0;
  const fetches = [];
  let inFlight = 0;
  let maxInFlight = 0;
  let maxTimers = 0;
  const alerts = [];
  const listeners = { document: {}, window: {} };
  const calls = [];
  let reloads = 0;
  const doc = {
    hidden: false,
    getElementById: el,
    createElement: () => el("download"),
    body: { appendChild() {} },
    addEventListener: (type, fn) => { (listeners.document[type] = listeners.document[type] || []).push(fn); },
  };
  const ctx = {
    API_BASE: "", CV_TOKEN: "", operaServed: true, chirpServed: true, btServed: true,
    console, document: doc, alert: (m) => alerts.push(String(m)),
    location: { reload: () => { reloads++; } },
    window: { addEventListener: (type, fn) => { (listeners.window[type] = listeners.window[type] || []).push(fn); } },
    AbortController,
    URL: { createObjectURL: () => "blob:0", revokeObjectURL() {} },
    Blob: function Blob() {},
    setInterval: () => 0, clearInterval() {},
    setTimeout: (fn, ms) => {
      const id = nextId++;
      timers.set(id, { at: now + (ms || 0), fn });
      maxTimers = Math.max(maxTimers, timers.size);
      return id;
    },
    clearTimeout: (id) => { timers.delete(id); },
    fetch: (url, opts = {}) => {
      const auth = (opts.headers || {}).Authorization || null;
      fetches.push({ url, at: now, auth });
      inFlight++;
      maxInFlight = Math.max(maxInFlight, inFlight);
      const signal = opts.signal || null;
      if (hang && hang(fetches.length)) {
        // The request never reaches the device's handler.
        return new Promise((resolve, reject) => {
          if (signal) signal.addEventListener("abort", () => { inFlight--; reject(new Error("aborted")); });
        });
      }
      const res = answer ? answer(url, opts, now)
        : url === "/api/provisioning-receipt" && !auth ? dev.answer(dev.ask(peer(), now, "A"))
          : { status: 200, body: RECEIPT };
      const reply = {
        status: res.status, ok: res.status >= 200 && res.status < 300,
        text: async () => res.body, json: async () => JSON.parse(res.body),
      };
      if (!latency) { inFlight--; return Promise.resolve(reply); }
      return new Promise((resolve, reject) => {
        let settled = false;
        const due = { at: now + latency, fn: () => { if (!settled) { settled = true; inFlight--; resolve(reply); } } };
        harness.push(due);
        if (signal) {
          signal.addEventListener("abort", () => {
            if (settled) return;
            settled = true;
            inFlight--;
            reject(new Error("aborted"));
          });
        }
      });
    },
  };
  for (const stub of ["refreshStatus", "refreshLiveSensing", "loadChain", "loadWifiStatus", "refreshOpera",
                      "refreshChirpStatus", "refreshBtStatus", "updateResolutionUI"]) {
    ctx[stub] = () => calls.push(stub);
  }
  vm.createContext(ctx);
  // The page load itself, then the script: helpers, then initialize.
  dev.ask(peer(), 0, "A page load");
  vm.runInContext(helpers + "\n;globalThis.__t = { saveRecoveryKit, tokenIsMissing };\n", ctx);
  const p = {
    ctx, el, dev, alerts, calls, fetches, timers, harness, listeners,
    get now() { return now; },
    get reloads() { return reloads; },
    get maxInFlight() { return maxInFlight; },
    get maxTimers() { return maxTimers; },
    get inFlight() { return inFlight; },
    receiptAsks: () => fetches.filter((f) => f.url === "/api/provisioning-receipt" && !f.auth),
    line: () => el("lanTapStatus").textContent,
    again: () => el("lanTapAgain").style.display !== "none",
    start: async () => { vm.runInContext(init, ctx); await flush(); },
    // Run every page timer and harness event due by `until`, in time order
    // (a harness event first at a tie), flushing the page's promises after
    // each.
    async advance(ms) {
      const until = now + ms;
      for (;;) {
        let best = null;
        for (const h of harness) if (h.at <= until && (!best || h.at < best.at)) best = h;
        let tid = null;
        for (const [id, t] of timers) {
          if (t.at <= until && (!best || t.at < best.at) && (tid === null || t.at < timers.get(tid).at)) tid = id;
        }
        if (tid !== null) {
          const t = timers.get(tid);
          timers.delete(tid);
          now = t.at;
          t.fn();
        } else if (best) {
          harness.splice(harness.indexOf(best), 1);
          now = best.at;
          best.fn();
        } else {
          break;
        }
        await flush();
      }
      now = until;
    },
    at(t, fn) { harness.push({ at: t, fn }); },
    fire(target, type, ev = {}) { for (const fn of (listeners[target][type] || [])) fn(ev); },
    async setHidden(hidden) { doc.hidden = hidden; p.fire("document", "visibilitychange"); await flush(); },
  };
  return p;
}

// B: a host on the network curling the receipt every `every` ms from `b0`.
function curlLoop(p, b0, every = 700, until = 90000) {
  assert.ok(b0 > 0, "B starts after the page load");
  for (let t = b0; t <= until; t += every) {
    p.at(t, () => { p.dev.ask(B_PEER, p.now, "B"); });
  }
}

const unlocked = (p) => p.ctx.CV_TOKEN === TOKEN && p.el("lanLockBanner").style.display === "none";

// ── The banner's words ───────────────────────────────────────────────────

test("the banner asks first: keep this page open, then tap BOOT (never press, then reload)", () => {
  const banner = src.slice(src.indexOf('id="lanLockBanner"'), src.indexOf("<!-- Status Panel -->"));
  const flat = banner.replace(/\s+/g, " ");
  assert.match(flat, /Keep this page open, then tap BOOT/);
  assert.doesNotMatch(flat, /reload within 30 seconds/);
  assert.ok(banner.includes('id="lanTapStatus"') && banner.includes('id="lanTapAgain"'),
            "the banner has its status line and its ask button");
  assert.doesNotMatch(src, /then try again within 30 seconds/, "no press-first alert is left");
});

// ── The poll, against the firmware's own step ────────────────────────────

test("the banner polls before the tap, and a 200 sets CV_TOKEN from the receipt without a reload", async () => {
  const p = page();
  await p.start();
  assert.strictEqual(p.el("lanLockBanner").style.display, "", "the page loaded without the token shows the banner");
  assert.strictEqual(p.receiptAsks().length, 1, "the banner asks at once, before any tap");
  assert.match(p.line(), /Now short-tap BOOT/, "once the Canary has answered the first ask, the line says to press");
  await p.advance(2000);
  assert.strictEqual(p.receiptAsks().length, 2, "and asks again 2 s later");
  p.at(5100, () => p.dev.press(5100));   // the owner presses between two polls
  await p.advance(4000);
  assert.ok(unlocked(p), `the receipt's token unlocks the page where it stands (CV_TOKEN ${p.ctx.CV_TOKEN})`);
  assert.strictEqual(p.reloads, 0, "no reload");
  assert.deepStrictEqual(p.receiptAsks().map((f) => f.at), [0, 2000, 4000, 6000]);
  assert.strictEqual(p.dev.log.filter((e) => e.v === "TAKE").length, 1, "one receipt");
  assert.strictEqual(p.timers.size, 0, "the poll stopped: no timer left");
  assert.strictEqual(p.line(), "", "its line is cleared");
  assert.ok(p.again(), "and its button left on offer, should the banner show again");
  assert.ok(p.calls.includes("refreshStatus"), "the page reloads its data with the token, not itself");
  await p.advance(60000);
  assert.strictEqual(p.receiptAsks().length, 4, "nothing is asked after the 200");
});

test("a token the Canary later refuses brings the banner back with its button", async () => {
  const p = page();
  p.at(1000, () => p.dev.press(1000));
  await p.start();
  await p.advance(2000);
  assert.ok(unlocked(p));
  vm.runInContext(`dropRefusedToken(${JSON.stringify(TOKEN)}, 403, '{"error":"forbidden"}')`, p.ctx);
  assert.strictEqual(p.el("lanLockBanner").style.display, "", "the banner shows again");
  assert.ok(p.again(), "with its button, so a new tap window is one click away");
  const asks = p.receiptAsks().length;
  vm.runInContext("startTapPoll()", p.ctx);
  await flush();
  assert.strictEqual(p.receiptAsks().length, asks + 1, "the click asks first");
});

test("the line says not to press until the Canary has answered an ask", async () => {
  const p = page({ latency: 1500 });
  await p.start();
  assert.match(p.line(), /Don't press BOOT yet/, "before any answer");
  await p.advance(1500);
  assert.match(p.line(), /Now short-tap BOOT on the Canary.*30s/, `after the 403: ${p.line()}`);
});

test("a 409 says another device asked, and the poll stops", async () => {
  const p = page();
  curlLoop(p, 300);
  p.at(5100, () => p.dev.press(5100));
  await p.start();
  await p.advance(8000);
  assert.ok(!unlocked(p) && p.ctx.__t.tokenIsMissing(), "the page holds no token");
  assert.match(p.line(), /Another device on your network asked for this Canary's token during the same BOOT tap/);
  assert.match(p.line(), /handed out nothing/);
  assert.ok(p.again(), "the button to ask again is offered");
  assert.strictEqual(p.timers.size, 0, "no timer left");
  const asks = p.receiptAsks().length;
  await p.advance(60000);
  assert.strictEqual(p.receiptAsks().length, asks, "nothing is asked after the 409");
  assert.strictEqual(p.dev.log.filter((e) => e.v === "TAKE").length, 0, "nothing was handed out");
  assert.ok(p.dev.log.some((e) => e.who === "B" && e.v === "CONTENDED"), "B heard 409 tap_contended");
});

// The lens's two LAN clients: A loads the page and its banner polls, B
// curls the receipt every 700 ms from `b0`, the press lands at `T` between
// A's polls. Swept over T and B's start. In every run at most one receipt
// goes out and B never gets one. B asking from 10 s before the press until
// A's first poll after it: nothing is handed out, B hears 409, and A's
// banner says another device asked. Otherwise: A's banner takes the one
// receipt, without a reload, and every ask of B's after the press is 403.
test("two LAN clients around one tap: one receipt or none, never B's", async () => {
  let both = 0, alone = 0;
  for (const T of [41100, 41900, 42500]) {   // inside A's one-minute window, B's loop from 11 s on
    for (const off of [-30000, -10000, -9999, -1, 0, 1, 700, 2500, 9000]) {
      const b0 = T + off;
      const p = page();
      curlLoop(p, b0);
      p.at(T, () => p.dev.press(T));
      await p.start();
      await p.advance(90000);
      const takes = p.dev.log.filter((e) => e.v === "TAKE");
      const bTakes = takes.filter((e) => e.who === "B");
      const what = `T=${T} B from ${b0}`;
      assert.ok(takes.length <= 1 && bTakes.length === 0, `${what}: ${takes.length} receipt(s), ${bTakes.length} to B`);
      const a1 = 2000 * Math.ceil(T / 2000);   // A's first poll at or after the press
      let bInWindow = false;
      for (let t = b0; t < a1; t += 700) if (t >= T - 10000) bInWindow = true;
      if (bInWindow) {
        both++;
        assert.strictEqual(takes.length, 0, `${what}: B in the window, nothing handed out`);
        assert.ok(p.dev.log.some((e) => e.who === "B" && e.v === "CONTENDED"), `${what}: B heard 409`);
        assert.match(p.line(), /Another device on your network asked/, `${what}: A's banner says so`);
      } else {
        alone++;
        assert.ok(unlocked(p), `${what}: A alone takes the one receipt`);
        assert.strictEqual(p.reloads, 0);
        assert.ok(p.dev.log.filter((e) => e.who === "B" && e.at >= T).every((e) => e.v === "NO_TAP"),
                  `${what}: every ask of B's after the press is the 403`);
      }
      assert.strictEqual(p.maxInFlight, 1, `${what}: one request at a time`);
      assert.ok(p.maxTimers <= 1, `${what}: one timer at a time`);
    }
  }
  assert.ok(both > 0 && alone > 0, `the sweep reached both outcomes (${both} with B in the window, ${alone} alone)`);
});

test("one owner on two addresses hears the 409, never a second grant", async () => {
  // The page loaded over IPv4; after the press its poll leaves from an IPv6
  // temporary address (a second tab on another interface reads the same).
  let addr = A_PEER;
  const p = page({ peer: () => addr });
  p.at(5100, () => { p.dev.press(5100); addr = "20010db8000000005c319a027e11400b"; });
  await p.start();
  await p.advance(8000);
  assert.ok(!unlocked(p));
  assert.match(p.line(), /or this computer did, from a second address/);
  assert.strictEqual(p.dev.log.filter((e) => e.v === "TAKE").length, 0, "no grant at all");
});

// ── The poll on a small httpd: when it stops, and that it never stacks ───

test("the window closes after one minute of asks, and the button opens a new one", async () => {
  const p = page();
  await p.start();
  await p.advance(120000);
  assert.strictEqual(p.receiptAsks().length, 30, "30 asks, 2 s apart");
  assert.strictEqual(p.timers.size, 0, "no timer left");
  assert.match(p.line(), /Didn't see a tap/);
  assert.ok(p.again());
  await p.advance(600000);
  assert.strictEqual(p.receiptAsks().length, 30, "nothing more without a click");
  vm.runInContext("startTapPoll()", p.ctx);
  await flush();
  assert.strictEqual(p.receiptAsks().length, 31, "the button asks again at once");
  assert.ok(!p.again(), "and hides itself while it asks");
});

test("pagehide stops the poll, clears its line and offers its button", async () => {
  const p = page();
  await p.start();
  await p.advance(4000);
  assert.match(p.line(), /Now short-tap BOOT/, "the line said to press while the page was asking");
  const asks = p.receiptAsks().length;
  p.fire("window", "pagehide", { persisted: true });
  assert.strictEqual(p.timers.size, 0, "no timer left after pagehide");
  assert.strictEqual(p.line(), "", "the line no longer says to press");
  assert.ok(p.again(), "and the button to ask again is on offer");
  await p.advance(120000);
  assert.strictEqual(p.receiptAsks().length, asks, "nothing asked after pagehide");
  // Brought back from the back/forward cache, the page opens a new window,
  // as a page load does; a first show (not from the cache) asks nothing more.
  p.fire("window", "pageshow", { persisted: false });
  await flush();
  assert.strictEqual(p.receiptAsks().length, asks, "a first show starts nothing");
  p.fire("window", "pageshow", { persisted: true });
  await flush();
  assert.strictEqual(p.receiptAsks().length, asks + 1, "a page restored from the cache asks at once");
  assert.match(p.line(), /Now short-tap BOOT/, "and says to press once the Canary has answered");
  await p.advance(120000);
  assert.strictEqual(p.receiptAsks().length, asks + 30, "one more window of 30 asks, then it stops");
  assert.strictEqual(p.timers.size, 0);
});

test("a hidden tab keeps asking for the rest of its window, then stops", async () => {
  const p = page();
  await p.start();
  await p.advance(5000);   // asks at 0, 2000, 4000
  await p.setHidden(true);
  assert.strictEqual(p.timers.size, 1, "hiding the tab keeps the next ask");
  await p.advance(600000);
  const at = p.receiptAsks().map((f) => f.at);
  assert.strictEqual(at.length, 30, "ten hidden minutes: the window's 30 asks and no more");
  assert.deepStrictEqual(at, Array.from({ length: 30 }, (_, i) => 2000 * i), "every 2 s while hidden");
  assert.strictEqual(p.timers.size, 0, "no timer left once the window closed");
  assert.match(p.line(), /Didn't see a tap/);
  await p.setHidden(false);
  await flush();
  assert.strictEqual(p.receiptAsks().length, 30, "shown again: nothing until the button is clicked");
});

// The owner's dashboard sits in a background tab while the owner presses
// BOOT, and a host curls the receipt every 700 ms. A banner that stopped
// asking while hidden (a draft of this one did) left the host the sole
// asker, and the host took the receipt while the hidden line still said
// "Now short-tap BOOT". The hidden page stays on record: nothing is handed
// out, the host hears 409, and the banner says another device asked.
test("a hidden tab stays on record: a host curling across the press never gets the receipt", async () => {
  for (const { hideAt, b0, T } of [
    { hideAt: 5000, b0: 300, T: 20000 },     // B from the start, the press 15 s into the hidden tab
    { hideAt: 7000, b0: 8000, T: 18100 },    // B from 8 s, the press 11 s after the tab hid
    { hideAt: 3000, b0: 25000, T: 26100 },   // B starts 1.1 s before the press, 23 s into the hidden tab
  ]) {
    const p = page();
    curlLoop(p, b0);
    p.at(hideAt, () => p.setHidden(true));
    p.at(T, () => p.dev.press(T));
    await p.start();
    await p.advance(90000);
    const what = `hidden at ${hideAt}, B from ${b0}, press at ${T}`;
    const takes = p.dev.log.filter((e) => e.v === "TAKE");
    assert.deepStrictEqual(takes, [], `${what}: nothing is handed out (TAKEs ${JSON.stringify(takes)})`);
    assert.ok(p.dev.log.some((e) => e.who === "B" && e.v === "CONTENDED"), `${what}: B heard 409`);
    const aAsks = p.receiptAsks().filter((f) => f.at >= hideAt && f.at <= T).length;
    assert.ok(aAsks >= Math.floor((T - hideAt) / 2000), `${what}: the hidden page asked every 2 s (${aAsks})`);
    assert.match(p.line(), /Another device on your network asked/, `${what}: A's banner says so`);
  }
});

test("an ask that never answers is given up after 10 s, and the line says not to press", async () => {
  // The second ask never settles (a request lost on a slow link); the rest answer.
  const p = page({ hang: (n) => n === 2 });
  await p.start();
  await p.advance(2000);   // the second ask goes out and hangs
  assert.match(p.line(), /Now short-tap BOOT/, "the first ask's 403 is still the last answer");
  assert.strictEqual(p.timers.size, 1, "the hung ask's deadline is the one timer");
  await p.advance(10000);  // its deadline passes: given up, counted as no answer
  assert.strictEqual(p.inFlight, 0, "the hung request was aborted");
  assert.match(p.line(), /Don't press BOOT yet/, `the page may not be on record: ${p.line()}`);
  assert.deepStrictEqual(p.receiptAsks().map((f) => f.at), [0, 2000], "nothing asked while it hung");
  await p.advance(2000);
  assert.strictEqual(p.receiptAsks().length, 3, "the window goes on 2 s after the deadline");
  assert.match(p.line(), /Now short-tap BOOT/, "and says to press again once the Canary answers");
  await p.advance(600000);
  assert.strictEqual(p.receiptAsks().length, 30, "the window still ends after 30 asks");
  assert.strictEqual(p.timers.size, 0);
  assert.ok(p.maxTimers <= 1, "one timer at a time, the deadline included");
});

test("it never runs two timers or two requests, however it is pushed", async () => {
  const p = page({ latency: 1500 });
  await p.start();
  for (let i = 0; i < 40; i++) {
    vm.runInContext("startTapPoll()", p.ctx);            // the button, pressed again and again
    if (i % 3 === 0) await p.setHidden(true);            // the tab hidden and shown mid-request
    if (i % 3 === 1) await p.setHidden(false);
    await p.advance(250);
    assert.ok(p.timers.size <= 1, `${p.timers.size} timers at ${p.now}`);
    assert.ok(p.inFlight <= 1, `${p.inFlight} requests at ${p.now}`);
  }
  await p.setHidden(false);
  await p.advance(200000);
  assert.strictEqual(p.maxInFlight, 1, "one request at a time");
  assert.ok(p.maxTimers <= 1, "one timer at a time");
  const at = p.receiptAsks().map((f) => f.at);
  for (let i = 1; i < at.length; i++) {
    assert.ok(at[i] - at[i - 1] >= 1500, `asks ${at[i - 1]} and ${at[i]} overlap a request`);
  }
});

test("a receipt answered after a new window began still unlocks the page", async () => {
  // The tap is spent on the answer, so whichever window asked, it is taken.
  let n = 0;
  const p = page({
    latency: 1000,
    answer: () => (++n === 1 ? { status: 403, body: BODY_403 } : { status: 200, body: RECEIPT }),
  });
  await p.start();
  await p.advance(2500);            // the first answer, then the second ask is out
  vm.runInContext("stopTapPoll(); startTapPoll();", p.ctx);
  await p.advance(1000);
  assert.ok(unlocked(p), "the answer to the older window unlocks the page");
  assert.strictEqual(p.timers.size, 0);
});

test("a Host the Canary cannot be named by stops the poll and says how to open the page", async () => {
  const p = page({ answer: () => ({ status: 403, body: '{"error":"host"}' }) });
  await p.start();
  assert.match(p.line(), /under the name this page was opened with/);
  assert.strictEqual(p.timers.size, 0);
  await p.advance(60000);
  assert.strictEqual(p.receiptAsks().length, 1);
});

test("Save recovery kit on a page with no token asks first, then says to press", async () => {
  const p = page({ answer: (url, opts, now) => p.dev.answer(p.dev.ask(A_PEER, now, "A")) });
  vm.runInContext("tapPollState = 'idle';", p.ctx);
  await p.ctx.__t.saveRecoveryKit();
  await flush();
  assert.strictEqual(p.alerts.length, 1);
  assert.match(p.alerts[0], /Keep it open, then short-tap BOOT on the Canary when the line in the banner/);
  assert.doesNotMatch(p.alerts[0], /then try again within 30 seconds/);
  assert.ok(p.receiptAsks().length >= 2, "the banner's poll started: the page is asking before the press");
  assert.match(p.line(), /short-tap BOOT|Don't press BOOT yet/);

  const q = page({ answer: () => ({ status: 409, body: BODY_409 }) });
  await q.ctx.__t.saveRecoveryKit();
  assert.match(q.alerts[0], /Another device on your network asked/);
  assert.match(q.alerts[0], /when the line in the banner at the top says to/);
});

// A credential-less ask of the kit's goes out while the banner's poll is
// asking; the poll's ask takes the tap and unlocks the page; the kit's ask
// comes back 403 (the tap is spent). It sent no token, so it asks again
// with the page's, and the kit downloads. A 403 branch that read the page's
// token at answer time (a draft of this one did) alerted that "the Canary
// did not take this page's token", which no request had sent.
test("Save recovery kit overtaken by the banner's tap asks again with the token", async () => {
  const p = page({ latency: 1500 });
  let downloads = 0;
  p.el("download").click = () => { downloads++; };
  p.at(3000, () => p.dev.press(3000));
  p.at(3600, () => { p.ctx.__t.saveRecoveryKit(); });
  await p.start();
  await p.advance(10000);
  assert.ok(unlocked(p), "the poll's ask took the tap and unlocked the page");
  assert.deepStrictEqual(p.dev.log.filter((e) => e.who === "A").map((e) => e.v).slice(-2), ["TAKE", "NO_TAP"],
                         "the poll's ask took the tap; the kit's credential-less ask then heard the 403");
  const kit = p.fetches.filter((f) => f.at >= 3600 && f.url === "/api/provisioning-receipt");
  assert.strictEqual(kit[0].auth, null, "the kit's first ask carried no token");
  assert.ok(kit.some((f) => f.auth === "Bearer " + TOKEN), "its retry carried the page's token");
  assert.deepStrictEqual(p.alerts, [], `no alert (${JSON.stringify(p.alerts)})`);
  assert.strictEqual(downloads, 1, "the kit downloaded");
});
