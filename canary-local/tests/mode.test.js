// canary-local/tests/mode.test.js — drift-lock for the Mode system sim.
//
// Pins assets/mode-sim.js to the canary-display mode firmware the same way
// playground.test.js pins the bench sim: by reading the firmware's own
// source (the committed Arduino mirror, regenerated from src/ by setup.sh)
// and comparing constants, tables, and semantics. A change on either side
// that isn't mirrored breaks CI here.
//
//   registry tokens + policy   <- arduino/canary_display/mode_registry.h
//   demo cast + beats + loop   <- arduino/canary_display/demo_script.h
//
// Sections: "honesty" (the sim carries exactly what the firmware says) and
// "behavior" (boot resolution, latch semantics, loop stepping).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..", "..");
const REGISTRY_H = join(
  ROOT,
  "firmware/projects/canary-display/arduino/canary_display/mode_registry.h"
);
const DEMO_H = join(
  ROOT,
  "firmware/projects/canary-display/arduino/canary_display/demo_script.h"
);

async function importSim() {
  return import("../assets/mode-sim.js");
}

// ── honesty ─────────────────────────────────────────────────────────────────

test("sim tokens equal the firmware token set", async () => {
  const { MODE_TOKENS } = await importSim();
  const h = readFileSync(REGISTRY_H, "utf8");
  const fw = [...h.matchAll(/return "(\w+)";/g)].map((m) => m[1]);
  for (const t of MODE_TOKENS) {
    assert.ok(fw.includes(t), `token ${t} missing from mode_registry.h`);
  }
  // And every strcmp'd token in mode_from_token is one the sim knows.
  for (const m of h.matchAll(/strcmp\(s, "(\w+)"\)/g)) {
    assert.ok(MODE_TOKENS.includes(m[1]),
      `firmware parses token ${m[1]} the sim doesn't carry`);
  }
});

test("sim policy rows equal the firmware mode_policy table", async () => {
  const { MODES } = await importSim();
  const h = readFileSync(REGISTRY_H, "utf8");
  const rows = [...h.matchAll(
    /case Mode::(\w+):\s*return \{(\w+),\s*(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\};/g
  )];
  assert.strictEqual(rows.length, 5, "five policy rows in the firmware");
  for (const r of rows) {
    const token = r[1].toLowerCase();
    const fw = {
      network: r[2] === "true",
      ota: r[3] === "true",
      watchdog: r[4] === "true",
      touchExit: r[5] === "true",
      banner: r[6] === "true",
    };
    assert.deepStrictEqual(MODES[token], fw, `policy row for ${token}`);
  }
});

test("sim demo cast + beats + loop equal the firmware storyline", async () => {
  const { DEMO } = await importSim();
  const h = readFileSync(DEMO_H, "utf8");

  const loop = h.match(/DEMO_LOOP_S = (\d+)/);
  assert.ok(loop, "DEMO_LOOP_S in demo_script.h");
  assert.strictEqual(DEMO.LOOP_S, Number(loop[1]));

  const cast = [...h.matchAll(
    /\{"(demo-[\w-]+)",\s*"([^"]+)",\s*"(\w+)",\s*(\d+)\}/g
  )];
  assert.strictEqual(cast.length, DEMO.CAST.length, "cast size");
  cast.forEach((m, i) => {
    assert.deepStrictEqual(
      DEMO.CAST[i],
      { id: m[1], name: m[2], room: m[3], battery: Number(m[4]) },
      `cast member ${i}`
    );
  });

  const beats = [...h.matchAll(
    /\{(\d+),\s*(\d+),\s*"([\w]+)",\s*fleet::Sev::(\w+)\}/g
  )];
  assert.strictEqual(beats.length, DEMO.BEATS.length, "beat count");
  beats.forEach((m, i) => {
    assert.deepStrictEqual(
      DEMO.BEATS[i],
      {
        atS: Number(m[1]),
        witness: Number(m[2]),
        event: m[3],
        intent: m[4].toLowerCase(),
      },
      `beat ${i}`
    );
  });
});

test("no bird-group word anywhere in the sim", async () => {
  const src = readFileSync(join(__dirname, "..", "assets/mode-sim.js"), "utf8");
  assert.ok(!/flock/i.test(src), "the word is fleet");
});

// ── behavior ────────────────────────────────────────────────────────────────

test("boot resolution: bench env wins, tokens gate on caps, fleet fail-safe",
  async () => {
    const { resolveBootMode } = await importSim();
    assert.strictEqual(
      resolveBootMode({ dedicatedBench: true }, "demo", true), "bench",
      "dedicated bench env ignores every latch");
    const caps = { hasDevmode: true, hasDemo: true };
    assert.strictEqual(resolveBootMode(caps, "demo", false), "demo");
    assert.strictEqual(resolveBootMode(caps, "", false), "fleet");
    assert.strictEqual(resolveBootMode(caps, "arcade", false), "fleet",
      "uncarried gear fails safe to the product face");
    assert.strictEqual(resolveBootMode({}, "garbage#", false), "fleet",
      "corrupt token fails safe");
  });

test("legacy devmode bool migrates to bench; a written token outranks it",
  async () => {
    const { resolveBootMode } = await importSim();
    const caps = { hasDevmode: true, hasDemo: true };
    assert.strictEqual(resolveBootMode(caps, "", true), "bench");
    assert.strictEqual(resolveBootMode(caps, "demo", true), "demo");
    assert.strictEqual(resolveBootMode(caps, "fleet", true), "fleet");
    assert.strictEqual(resolveBootMode({}, "", true), "fleet",
      "legacy bool without the bench compiled in -> fleet");
  });

test("policy invariants: only fleet updates; every gear has an exit + banner",
  async () => {
    const { MODES, MODE_TOKENS } = await importSim();
    for (const t of MODE_TOKENS) {
      const p = MODES[t];
      if (t === "fleet") {
        assert.ok(p.ota && p.network && p.watchdog && !p.banner && !p.touchExit);
      } else {
        assert.ok(!p.ota, `${t} never takes an update`);
        assert.ok(!p.watchdog, `${t} never arms the watchdog`);
        assert.ok(p.touchExit && p.banner, `${t} has the exit and the banner`);
      }
    }
    assert.ok(MODES.debug.network && !MODES.demo.network && !MODES.bench.network,
      "debug is the one non-fleet gear with the network up");
  });

test("latch semantics: enter writes, exit clears, bench keeps the legacy bool",
  async () => {
    const { ModeSim } = await importSim();
    const sim = new ModeSim({ hasDevmode: true, hasDemo: true });
    assert.strictEqual(sim.boot(), "fleet");
    assert.strictEqual(sim.enter("demo"), "demo");
    assert.strictEqual(sim.nvs.mode, "demo");
    assert.strictEqual(sim.enter("bench"), "bench");
    assert.strictEqual(sim.nvs.devmode, true,
      "bench also writes the legacy bool (downgrade-safe)");
    assert.strictEqual(sim.exitToFleet(), "fleet");
    assert.deepStrictEqual(sim.nvs, {}, "exit clears both latches");
    assert.strictEqual(sim.enter("arcade"), "fleet",
      "a gear this build lacks cannot be entered");
  });

test("loop stepping: half-open windows, wrap-safe, once per lap", async () => {
  const { beatsBetween, DEMO } = await importSim();
  assert.deepStrictEqual(beatsBetween(0, 5), [0], "boundary beat fires once");
  assert.deepStrictEqual(beatsBetween(5, 6), [], "no double-fire");
  assert.deepStrictEqual(beatsBetween(45, 45), [], "no time, no beats");
  assert.deepStrictEqual(beatsBetween(140, 5), [0], "wrap fires the head");
  assert.deepStrictEqual(beatsBetween(120, 10), [0, 8], "wrap catches tail+head");
  // A full lap in odd steps fires each beat exactly once.
  const fired = new Array(DEMO.BEATS.length).fill(0);
  let prev = 0;
  for (let clock = 7; ; clock += 7) {
    const now = clock >= DEMO.LOOP_S ? 0 : clock;
    for (const i of beatsBetween(prev, now)) fired[i]++;
    prev = now;
    if (now === 0) break;
  }
  fired.forEach((n, i) => assert.strictEqual(n, 1, `beat ${i} once per lap`));
});

test("the story resolves before it wraps", async () => {
  const { DEMO } = await importSim();
  const last = DEMO.BEATS[DEMO.BEATS.length - 1];
  assert.strictEqual(last.intent, "ok", "no standing alarm across the seam");
  const tiers = new Set(DEMO.BEATS.map((b) => b.intent));
  for (const t of ["ok", "notice", "warn", "alert", "tamper"]) {
    assert.ok(tiers.has(t), `story shows ${t}`);
  }
});
