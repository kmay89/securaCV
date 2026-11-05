// canary-local/tests/playground.test.js — Node tests for the Waveshare 4.3B
// peripheral Playground (repo convention: CI runs the exact shipped source).
//
// Two jobs:
//   1. HONESTY — devices/playground.json only names real terminals, its wiring
//      prose is verbatim from the firmware playground UI, and its bring-up code
//      carries the CH422G addresses/bits that actually live in the board pins.h.
//   2. BEHAVIOR — assets/playground-sim.js reproduces the firmware's PG1 driver
//      line-for-line (DI edges + doorbell→DO0 ding, DO pulse/latch auto-off,
//      ToF trip, cap-touch, census, SNAP, the RS485 probe's three answers and
//      the CAN station's sends, refusals and bus-off recovery), pinned against
//      the .cpp constants; every PG1 body the firmware station prints is one
//      the sim can print, and every stimulus/fault row in the data drives the
//      sim to the strings the row's station `expect`s.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, mkdtempSync, writeFileSync, rmSync } = require("node:fs");
const { join } = require("node:path");
const { tmpdir } = require("node:os");
const { spawn, spawnSync } = require("node:child_process");

const ROOT = join(__dirname, "..", "..");
const CL = join(__dirname, "..");
const DISPLAY = join(ROOT, "firmware/projects/canary-display");
const PLAYGROUND_CPP = join(DISPLAY, "arduino/canary_display/playground.cpp");
const PLAYGROUND_UI = join(DISPLAY, "arduino/canary_display/playground_ui.cpp");
const PLAYGROUND_H = join(DISPLAY, "include/canary/playground/playground.h");
// The canonical copies the Arduino mirror above is generated from (setup.sh
// regen; firmware/scripts/check_display_arduino_sync.sh gates the two). The
// pins below read both, so a line planted in either is seen before a regen.
const SRC_CPP = join(DISPLAY, "src/playground/playground.cpp");
const SRC_UI = join(DISPLAY, "src/playground/playground_ui.cpp");
const PINS_H = join(ROOT, "firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h");

function playground() {
  return JSON.parse(readFileSync(join(CL, "devices/playground.json"), "utf8"));
}
async function importSim() {
  return import("../assets/playground-sim.js");
}
// strip quotes/backslashes/whitespace so JSON prose can be found inside C
// string literals that escape quotes and split across lines.
function squash(s) {
  return s.replace(/[\\"'\s]/g, "");
}

// ── honesty: the data only speaks about real hardware ────────────────────

test("every station wire lands on a declared terminal", () => {
  const pg = playground();
  const terms = new Set(Object.keys(pg.terminals));
  for (const st of pg.stations) {
    for (const [term, net] of st.wires) {
      assert.ok(terms.has(term), `${st.id}: wire to unknown terminal ${term}`);
      assert.ok(pg.colors[net], `${st.id}: wire net ${net} has no color`);
    }
  }
});

test("every terminal net has a color and a position", () => {
  const pg = playground();
  for (const [id, t] of Object.entries(pg.terminals)) {
    assert.ok(pg.colors[t.net], `${id}: net ${t.net} missing from colors`);
    assert.ok(Array.isArray(t.pos) && t.pos.length === 3, `${id}: bad pos`);
  }
});

test("a station's port choices each resolve to a code variant", () => {
  const pg = playground();
  for (const st of pg.stations) {
    if (!st.port) continue;
    for (const choice of st.port.choices) {
      assert.ok(st.code[choice], `${st.id}: no code for port choice "${choice}"`);
    }
    assert.ok(st.port.choices.includes(st.port.default), `${st.id}: default not a choice`);
  }
});

test("station wiring instructions are verbatim from the firmware playground UI", () => {
  const pg = playground();
  const ui = squash(readFileSync(PLAYGROUND_UI, "utf8"));
  for (const st of pg.stations) {
    for (const line of st.instructions.split("\n")) {
      const needle = squash(line);
      if (needle.length < 8) continue; // skip trivial lines
      assert.ok(ui.includes(needle), `${st.id}: instruction not found in firmware UI: "${line}"`);
    }
  }
});

test("DI/DO bring-up code carries the CH422G addresses that pins.h defines", () => {
  const pg = playground();
  const pins = readFileSync(PINS_H, "utf8");
  const grab = (name) => {
    const m = pins.match(new RegExp(`#define\\s+${name}\\s+(\\S+)`));
    assert.ok(m, `${name} missing from pins.h`);
    return m[1];
  };
  const rdIo = grab("CH422G_ADDR_IN"); // 0x26
  const wrOc = grab("CH422G_ADDR_OC"); // 0x23
  assert.ok(pg.stations.find((s) => s.id === "doorbell").code.DI0.includes(rdIo),
    "doorbell DI0 code must read the CH422G RD_IO address from pins.h");
  assert.ok(pg.stations.find((s) => s.id === "chime").code.DO0.includes(wrOc),
    "chime DO0 code must write the CH422G WR_OC address from pins.h");
});

test("the board id + expander addresses match pins.h", () => {
  const pg = playground();
  const pins = readFileSync(PINS_H, "utf8");
  assert.ok(pins.includes(`"${pg.board.id}"`), "board id must match pins.h BOARD_ID");
  assert.ok(pins.includes(pg.board.expander.read), "expander.read must be a pins.h address");
  assert.ok(pins.includes(pg.board.expander.oc), "expander.oc must be a pins.h address");
});

// ── behavior: the sim is a faithful port, pinned to the .cpp constants ────

test("sim constants equal the firmware playground.cpp constants", async () => {
  const { PG, TOF_TRIPS, PAD_PRESETS } = await importSim();
  const cpp = readFileSync(PLAYGROUND_CPP, "utf8");
  const con = (name) => {
    const m = cpp.match(new RegExp(`${name}\\s*=\\s*(\\d+)`));
    assert.ok(m, `${name} not found in playground.cpp`);
    return Number(m[1]);
  };
  assert.strictEqual(PG.DO_PULSE_MS, con("DO_PULSE_MS"));
  assert.strictEqual(PG.DO_LATCH_MS, con("DO_LATCH_MS"));
  assert.strictEqual(PG.SNAP_EVERY_MS, con("SNAP_EVERY_MS"));
  assert.strictEqual(PG.CAN_DRAIN_MS, con("CAN_DRAIN_MS"));
  assert.strictEqual(PG.DI_ACTIVE_LEVEL, con("DI_ACTIVE_LEVEL"));
  // TOF_TRIPS[] = {50, 100, 200, 400}
  const trips = cpp.match(/TOF_TRIPS\[\]\s*=\s*\{([^}]*)\}/)[1]
    .split(",").map((s) => Number(s.trim()));
  assert.deepStrictEqual(TOF_TRIPS, trips);
  // PAD_PRESETS names appear in the .cpp
  for (const p of PAD_PRESETS) {
    assert.ok(cpp.includes(`"${p.name}"`), `preset ${p.name} missing from playground.cpp`);
  }
});

test("doorbell press: EVT + the DO0 ding link, then bounded auto-off", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  const press = s.setDI(0, true, 100);
  assert.deepStrictEqual(press, [
    "PG1 100 EVT doorbell state=active count=1",
    "PG1 100 EVT chime out=on",
  ]);
  assert.strictEqual(s.g.do0.sinking, true);
  assert.strictEqual(s.g.do0.auto_off_at_ms, 100 + 1500);
  // the pulse releases itself at the deadline
  assert.deepStrictEqual(s.tick(1600), ["PG1 1600 EVT chime out=off"]);
  // release reports how long it was held
  assert.deepStrictEqual(s.setDI(0, false, 2000), [
    "PG1 2000 EVT doorbell state=clear held_ms=1900",
  ]);
});

test("intrusion press does NOT ring DO0 (no ding link on DI1)", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.deepStrictEqual(s.setDI(1, true, 50), ["PG1 50 EVT intrusion state=active count=1"]);
  assert.strictEqual(s.g.do0.sinking, false);
});

test("DO latch holds then safety-releases at 30 s", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.deepStrictEqual(s.doLatchToggle(1, 5), ["PG1 5 EVT strobe out=on"]);
  assert.strictEqual(s.g.do1.auto_off_at_ms, 5 + 30000);
  assert.deepStrictEqual(s.tick(20000), []); // not yet
  assert.deepStrictEqual(s.tick(30005), ["PG1 30005 EVT strobe out=off"]);
});

test("ToF counts a falling edge under the trip threshold, once", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  s.attach("tof", 0); // default trip_mm = 100
  assert.deepStrictEqual(s.tofSample(80, 200), ["PG1 200 EVT tof trip=1 mm=80 count=1"]);
  assert.deepStrictEqual(s.tofSample(70, 300), []); // still under — no new edge
  assert.deepStrictEqual(s.tofSample(300, 400), []); // cleared
  assert.deepStrictEqual(s.tofSample(60, 500), ["PG1 500 EVT tof trip=1 mm=60 count=2"]);
  assert.deepStrictEqual(s.cycleTofTrip(600), ["PG1 600 EVT tof trip_mm=200"]);
});

test("cap-touch reports a new electrode + preset cycle", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  s.attach("pad", 0);
  assert.deepStrictEqual(s.padTouch(0x001, 10), ["PG1 10 EVT captouch pads=0x001 count=1"]);
  assert.deepStrictEqual(s.padTouch(0x001, 20), []); // held, not new
  assert.deepStrictEqual(s.cyclePadPreset(30), ["PG1 30 EVT captouch preset=2mm shell"]);
});

test("census attaches known sensors and reports the count", async () => {
  const { PlaygroundSim, i2cReserved } = await importSim();
  const s = new PlaygroundSim();
  const evts = s.census([0x10, 0x29, 0x5a, 0x5d], 0);
  assert.ok(evts.includes("PG1 0 EVT light attached=0x10"));
  assert.ok(evts.includes("PG1 0 EVT tof attached=0x29"));
  assert.ok(evts.includes("PG1 0 EVT captouch attached=0x5A"));
  assert.strictEqual(s.g.bus.count, 4);
  assert.ok(i2cReserved(0x5d), "GT911 0x5D is a reserved address");
  assert.ok(!i2cReserved(0x10), "VEML7700 0x10 is not reserved");
});

test("i2cName / i2cReserved port the firmware's i2c_device_name / i2c_addr_reserved", async () => {
  const { i2cName, i2cReserved, SENSORS } = await importSim();
  const cpp = readFileSync(PLAYGROUND_CPP, "utf8");
  const named = {
    ADDR_VEML7700: SENSORS.veml7700,
    ADDR_BH1750: SENSORS.bh1750,
    ADDR_VL53L0X: SENSORS.vl53l0x,
    ADDR_MPR121: SENSORS.mpr121,
  };
  const addr = (tok) => (tok in named ? named[tok] : parseInt(tok, 16));
  const nameFn = cpp.match(/const char\* i2c_device_name\(uint8_t addr\) \{([\s\S]*?)\n\}/);
  assert.ok(nameFn, "i2c_device_name() not found in playground.cpp");
  const want = new Map();
  for (const m of nameFn[1].matchAll(/((?:case [A-Za-z0-9_]+:\s*)+)return "([^"]*)";/g)) {
    for (const c of m[1].matchAll(/case ([A-Za-z0-9_]+):/g)) want.set(addr(c[1]), m[2]);
  }
  assert.ok(want.size >= 20, `parsed only ${want.size} names from i2c_device_name()`);
  for (let a = 0x08; a <= 0x77; a++) {
    assert.strictEqual(i2cName(a), want.get(a) || "", `i2cName(${a.toString(16)})`);
  }
  const resFn = cpp.match(/bool i2c_addr_reserved\(uint8_t addr\) \{([\s\S]*?)\n\}/);
  assert.ok(resFn, "i2c_addr_reserved() not found in playground.cpp");
  const reserved = new Set([...resFn[1].matchAll(/addr == (0x[0-9A-Fa-f]+)/g)].map((m) => parseInt(m[1], 16)));
  assert.ok(reserved.size >= 6, `parsed only ${reserved.size} reserved addresses`);
  for (let a = 0x08; a <= 0x77; a++) {
    assert.strictEqual(i2cReserved(a), reserved.has(a), `i2cReserved(${a.toString(16)})`);
  }
});

test("RS485 probe reads a register; a quiet bus reports no reply", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.deepStrictEqual(s.rs485Probe(100),
    ["PG1 100 EVT rs485 slave=1 reg=0 val=1000 crc=ok"]);
  assert.strictEqual(s.g.rs485.last_val, 1000);
  assert.deepStrictEqual(s.rs485Probe(200),
    ["PG1 200 EVT rs485 slave=1 reg=0 val=1010 crc=ok"]); // ramps per reply
  assert.deepStrictEqual(s.rs485ProbeQuiet(300),
    ["PG1 300 EVT rs485 slave=1 reply=none"]);
  assert.strictEqual(s.g.rs485.last_ok, false);
});

test("CAN send transmits; a node reply is logged via the frame formatter", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.deepStrictEqual(s.canSend(50), ["PG1 50 EVT can tx id=0x100 dlc=8 ok"]);
  assert.strictEqual(s.g.can.tx, 1);
  assert.deepStrictEqual(s.canReceive(60),
    ["PG1 60 EVT can rx id=0x101 ext=0 rtr=0 dlc=8 data=A0 A1 A2 A3 A4 A5 A6 A7"]);
  assert.strictEqual(s.g.can.rx, 1);
});

test("SNAP line matches the firmware's field order and formatting", async () => {
  const { PlaygroundSim, helloLine } = await importSim();
  const s = new PlaygroundSim();
  s.attach("veml", 0);
  s.lightSample(123.4);
  const snap = s.snapshot(1000);
  assert.strictEqual(
    snap,
    "PG1 1000 SNAP di0=0 di1=0 do0=off do1=off lux=123.4 tof_mm=0 tof_ok=0 " +
      "pads=0x000 preset=contact i2c=0"
  );
  assert.strictEqual(
    helloLine("waveshare-esp32s3-lcd43b", "2.2.0"),
    "PG1 HELLO board=waveshare-esp32s3-lcd43b fw=2.2.0"
  );
});

// ── RS485: a Modbus exception, and last_exc following the latest probe ────

test("RS485 exception is named and kept until the next probe clears it", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.strictEqual(s.g.rs485.last_exc, -1, "nothing named before a probe");
  assert.deepStrictEqual(s.rs485ProbeException(100),
    ["PG1 100 EVT rs485 slave=1 exception=2"]); // 2: illegal data address
  assert.strictEqual(s.g.rs485.last_exc, 2);
  assert.strictEqual(s.g.rs485.last_ok, false);
  assert.strictEqual(s.g.rs485.polls, 1);
  assert.strictEqual(s.g.rs485.replies, 0, "an exception is no register read");
  assert.deepStrictEqual(s.rs485Probe(200), ["PG1 200 EVT rs485 slave=1 reg=0 val=1000 crc=ok"]);
  assert.strictEqual(s.g.rs485.last_exc, -1, "a good reply clears it");
  assert.deepStrictEqual(s.rs485ProbeException(300, 4), ["PG1 300 EVT rs485 slave=1 exception=4"]);
  assert.strictEqual(s.g.rs485.last_exc, 4);
  assert.strictEqual(s.g.rs485.last_val, 1000, "the last good value stays");
  assert.deepStrictEqual(s.rs485ProbeQuiet(400), ["PG1 400 EVT rs485 slave=1 reply=none"]);
  assert.strictEqual(s.g.rs485.last_exc, -1, "no reply clears it");
  assert.deepStrictEqual(s.rs485ProbeException(500, 0), ["PG1 500 EVT rs485 slave=1 exception=0"]);
  assert.strictEqual(s.g.rs485.last_exc, 0, "0 is a code on the wire too, not 'none'");
});

test("rs485ProbeException refuses a code that is not one byte", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  for (const bad of [-1, 256, 2.5, "2", NaN]) {
    assert.throws(() => s.rs485ProbeException(0, bad), RangeError, `code ${String(bad)}`);
  }
  assert.strictEqual(s.g.rs485.polls, 0, "a refused call probes nothing");
  assert.deepStrictEqual(s.log, []);
});

// ── CAN: refusals say why, and a bus-off recovers from the drain ──────────

test("CAN bus-off: the tap says so, the next drain passes recover it, then a send goes out", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.deepStrictEqual(s.canSend(50), ["PG1 50 EVT can tx id=0x100 dlc=8 ok"]);
  assert.deepStrictEqual(s.canBusOff(60), ["PG1 60 EVT can tx=fail bus=off (recovering)"]);
  assert.strictEqual(s.g.can.tx, 1, "a refused tap sends nothing");
  // Out of service until the drain restarts it: sends are refused and say
  // why, a node's frame is not heard, and the tap itself takes no step.
  assert.deepStrictEqual(s.canSend(70), ["PG1 70 EVT can tx=fail bus=off (recovering)"]);
  assert.deepStrictEqual(s.canNoAck(75), ["PG1 75 EVT can tx=fail bus=off (recovering)"]);
  assert.deepStrictEqual(s.canReceive(80), []);
  assert.strictEqual(s.g.can.rx, 0);
  assert.deepStrictEqual(s.tick(90), [], "under CAN_DRAIN_MS since the last pass: no step");
  assert.deepStrictEqual(s.tick(100), ["PG1 100 EVT can bus=off recovery=started"]);
  assert.deepStrictEqual(s.canSend(150), ["PG1 150 EVT can tx=fail bus=off (recovering)"],
    "still recovering until the next pass");
  assert.deepStrictEqual(s.tick(199), []);
  assert.deepStrictEqual(s.tick(200), ["PG1 200 EVT can bus=on recovered"]);
  for (let t = 300; t <= 1000; t += 100) assert.deepStrictEqual(s.tick(t), [], "nothing more");
  assert.deepStrictEqual(s.canSend(1010), ["PG1 1010 EVT can tx id=0x100 dlc=8 ok"]);
  assert.strictEqual(s.g.can.tx, 2);
  assert.deepStrictEqual(s.canReceive(1020),
    ["PG1 1020 EVT can rx id=0x101 ext=0 rtr=0 dlc=8 data=A0 A1 A2 A3 A4 A5 A6 A7"]);
});

test("CAN: once per bus-off, a later bus-off recovers again, and a lone node is bus quiet", async () => {
  const { PlaygroundSim } = await importSim();
  const s = new PlaygroundSim();
  assert.deepStrictEqual(s.canNoAck(10), ["PG1 10 EVT can tx=fail (bus quiet? need another node/ack)"]);
  assert.strictEqual(s.g.can.ready, true, "the tap brought the controller up");
  assert.strictEqual(s.g.can.tx, 0);
  assert.deepStrictEqual(s.tick(100), [], "a refused ack is no bus-off: nothing to recover");
  s.canBusOff(110);
  s.canBusOff(120); // the same bus-off, tapped twice
  assert.deepStrictEqual(s.tick(200), ["PG1 200 EVT can bus=off recovery=started"]);
  s.canBusOff(210); // a tap while recovering arms nothing more
  assert.deepStrictEqual(s.tick(300), ["PG1 300 EVT can bus=on recovered"]);
  assert.deepStrictEqual(s.tick(400), []);
  s.canBusOff(410);
  assert.deepStrictEqual(s.tick(500), ["PG1 500 EVT can bus=off recovery=started"]);
  assert.deepStrictEqual(s.tick(600), ["PG1 600 EVT can bus=on recovered"]);
  const started = s.log.filter((l) => l.endsWith("bus=off recovery=started")).length;
  assert.strictEqual(started, 2, "one initiate per bus-off");
  // The DO auto-off still runs in the same pass, ahead of the CAN step.
  s.doPulse(0, 650);
  s.canBusOff(660);
  assert.deepStrictEqual(s.tick(2200),
    ["PG1 2200 EVT chime out=off", "PG1 2200 EVT can bus=off recovery=started"]);
  s.reset();
  assert.deepStrictEqual(s.canSend(0), ["PG1 0 EVT can tx id=0x100 dlc=8 ok"], "reset clears a recovery");
});

// ── The firmware station and the sim say the same things ─────────────────

// A C string literal's text (the formats here carry no escapes but \" and \\).
function cString(lit) {
  return lit.replace(/\\(["\\])/g, "$1");
}

function escapeRe(s) {
  return s.replace(/[.*+?^${}()|[\]\\/-]/g, "\\$&");
}

// A printf format as the pattern of what it can print: the literal text as
// itself, each %-conversion as a wildcard of its kind. A conversion the reader
// has no wildcard for is refused, not passed.
function formatPattern(fmt, where) {
  let out = "";
  let i = 0;
  const conv = /%([-+ #0]*)(\d*)(?:\.(\d+))?(hh|h|ll|l|z|j|t)?([a-zA-Z%])/y;
  while (i < fmt.length) {
    const at = fmt.indexOf("%", i);
    if (at < 0) {
      out += escapeRe(fmt.slice(i));
      break;
    }
    out += escapeRe(fmt.slice(i, at));
    conv.lastIndex = at;
    const m = conv.exec(fmt);
    assert.ok(m, `${where}: a %-conversion the pin cannot read in "${fmt}"`);
    const wild = { d: "-?\\d+", i: "-?\\d+", u: "\\d+", x: "[0-9a-f]+", X: "[0-9A-F]+",
                   o: "[0-7]+", c: ".", s: ".*", "%": "%" }[m[5]];
    assert.ok(wild, `${where}: no wildcard for %${m[5]} in "${fmt}"`);
    out += wild;
    i = conv.lastIndex;
  }
  return out;
}

// Split a call's argument text at its top-level commas.
function splitArgs(text) {
  const args = [];
  let depth = 0;
  let cur = "";
  let inStr = false;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (inStr) {
      cur += c;
      if (c === "\\") cur += text[++i];
      else if (c === '"') inStr = false;
      continue;
    }
    if (c === '"') inStr = true;
    if (c === "(") depth++;
    if (c === ")") {
      if (depth === 0) return { args: [...args, cur.trim()], end: i };
      depth--;
    }
    if (c === "," && depth === 0) {
      args.push(cur.trim());
      cur = "";
      continue;
    }
    cur += c;
  }
  return null;
}

// Every say_evt() call in a playground.cpp, as {where, stations, pattern}.
// The station is a literal, a ternary of two literals, or the parameter of
// the function the call sits in, resolved through that function's callers
// (di_update's `station`: "doorbell", "intrusion"). A call the reader cannot
// resolve is refused: an unread line is one the pin would pass unseen.
function sayEvtCalls(path) {
  const src = readFileSync(path, "utf8");
  const lineOf = (at) => src.slice(0, at).split("\n").length;
  const out = [];
  for (const m of src.matchAll(/\bsay_evt\s*\(/g)) {
    const where = `${path.slice(ROOT.length + 1)}:${lineOf(m.index)}`;
    const call = splitArgs(src.slice(m.index + m[0].length));
    assert.ok(call, `${where}: an unterminated say_evt( call`);
    const [st, fmt] = call.args;
    if (/^const char\*/.test(st)) continue; // the definition
    assert.ok(/^"(?:[^"\\]|\\.)*"$/.test(fmt || ""),
      `${where}: say_evt's format is not one string literal (${fmt}), so the pin cannot read it`);
    let stations;
    let lit;
    if ((lit = st.match(/^"([a-z0-9_]+)"$/))) stations = [lit[1]];
    else if ((lit = st.match(/^\w+\s*==\s*\w+\s*\?\s*"([a-z0-9_]+)"\s*:\s*"([a-z0-9_]+)"$/))) {
      stations = [lit[1], lit[2]];
    } else if (/^[A-Za-z_]\w*$/.test(st)) {
      // The enclosing function's parameter of that name, and its callers.
      const before = src.slice(0, m.index);
      const defs = [...before.matchAll(/^[A-Za-z_][\w:<>* ]*?\b([A-Za-z_]\w*)\(([^)]*)\)\s*\{/gm)];
      assert.ok(defs.length, `${where}: say_evt station ${st} has no enclosing function`);
      const [, fn, params] = defs[defs.length - 1];
      const idx = params.split(",").findIndex((p) => new RegExp(`\\b${st}$`).test(p.trim()));
      assert.ok(idx >= 0, `${where}: ${st} is not a parameter of ${fn}()`);
      stations = [];
      for (const c of src.matchAll(new RegExp(`\\b${fn}\\s*\\(`, "g"))) {
        if (c.index === defs[defs.length - 1].index + defs[defs.length - 1][0].indexOf(fn)) continue;
        const a = splitArgs(src.slice(c.index + c[0].length));
        assert.ok(a && /^"[a-z0-9_]+"$/.test(a.args[idx] || ""),
          `${where}: a call of ${fn}() at line ${lineOf(c.index)} passes no station literal`);
        stations.push(a.args[idx].slice(1, -1));
      }
      assert.ok(stations.length, `${where}: ${fn}() has no callers to name its station`);
    } else {
      assert.fail(`${where}: say_evt station ${st} is not a form the pin can read`);
    }
    const body = cString(fmt.slice(1, -1));
    out.push({ where, stations, body, pattern: formatPattern(body, where) });
  }
  return out;
}

// What the station's playground.cpp prints that the sim does not, each with
// its reason. Kept short on purpose: a line here is a line the website's
// /playground (the sim's one page, reached through scripts/carry_to_site.py's
// playground carry) cannot show.
const SIM_EXEMPT = new Map([
  ["can tx=fail driver",
    "the controller's install or start refused: the virtual controller always comes up"],
]);

// What each stimulus/fault action does to the sim, with the station's port
// choice where the page has one. The website's /playground dispatches the
// same calls in its _poke switch (js/playground.js there); the fault rows'
// cases (rs485:exception, can:busoff, can:noack) are to follow this table.
const POKE = {
  "di:on": (s, now, st, port) => s.setDI(diChannel(st, port), true, now),
  "di:off": (s, now, st, port) => s.setDI(diChannel(st, port), false, now),
  "do:pulse": (s, now, st, port) => s.doPulse(port === "DO1" ? 1 : 0, now),
  "do:latch": (s, now, st, port) => s.doLatchToggle(port === "DO1" ? 1 : 0, now),
  "light:bright": (s, now, st, port) => [...lightAttach(s, now, port), ...s.lightSample(820)],
  "light:dark": (s, now, st, port) => [...lightAttach(s, now, port), ...s.lightSample(3)],
  "tof:near": (s, now) => [...(s.g.tof.found ? [] : s.attach("tof", now)),
    ...s.tofSample(Math.max(10, s.g.tof.trip_mm - 40), now)],
  "tof:far": (s, now) => [...(s.g.tof.found ? [] : s.attach("tof", now)),
    ...s.tofSample(s.g.tof.trip_mm + 300, now)],
  "tof:cycle": (s, now) => s.cycleTofTrip(now),
  "pad:touch": (s, now) => [...(s.g.pad.found ? [] : s.attach("pad", now)), ...s.padTouch(0x001, now)],
  "pad:release": (s, now) => s.padTouch(0x000, now),
  "pad:preset": (s, now) => s.cyclePadPreset(now),
  "bus:scan": (s, now) => s.census([0x10, 0x29, 0x5a, 0x5d, 0x14], now),
  "rs485:probe": (s, now) => s.rs485Probe(now),
  "rs485:quiet": (s, now) => s.rs485ProbeQuiet(now),
  "rs485:exception": (s, now) => s.rs485ProbeException(now, 2),
  "can:send": (s, now) => s.canSend(now),
  "can:rx": (s, now) => s.canReceive(now),
  "can:busoff": (s, now) => s.canBusOff(now),
  "can:noack": (s, now) => s.canNoAck(now),
};
function diChannel(st, port) {
  return st.id === "intrusion" ? (port === "DI0" ? 0 : 1) : (port === "DI1" ? 1 : 0);
}
function lightAttach(s, now, port) {
  return s.g.light.found ? [] : s.attach(port.includes("BH1750") ? "bh" : "veml", now);
}

// Play a station as the page does: each of its rows' actions under each port
// choice, every one followed by two beats of the page's 1 Hz clock (a loop
// pass, then the SNAP), and return every line the sim printed.
function playStation(PlaygroundSim, st) {
  const lines = [];
  for (const port of st.port ? st.port.choices : [null]) {
    const s = new PlaygroundSim();
    let now = 0;
    for (const row of [...st.stimulus, ...(st.faults || [])]) {
      now += 10;
      lines.push(...POKE[row.action](s, now, st, port));
      for (let beat = 0; beat < 2; beat++) {
        now += 1000;
        lines.push(...s.tick(now), s.snapshot(now));
      }
    }
  }
  return lines;
}

test("every stimulus and fault row drives the sim to its station's expect strings", async () => {
  const { PlaygroundSim } = await importSim();
  const pg = playground();
  const used = new Set();
  for (const st of pg.stations) {
    assert.ok(Array.isArray(st.stimulus) && Array.isArray(st.faults), `${st.id}: stimulus/faults arrays`);
    for (const row of [...st.stimulus, ...st.faults]) {
      assert.ok(POKE[row.action], `${st.id}: no sim call for action "${row.action}"`);
      assert.ok(row.id && row.label, `${st.id}: ${row.action} needs an id and a label`);
      used.add(row.action);
    }
    const lines = playStation(PlaygroundSim, st);
    for (const want of st.expect) {
      assert.ok(lines.some((l) => l.includes(want)),
        `${st.id}: no line the sim printed for its rows carries "${want}"`);
    }
  }
  for (const action of Object.keys(POKE)) {
    assert.ok(used.has(action), `POKE["${action}"] is a row no station has`);
  }
});

test("every PG1 body the firmware station prints is one the sim can print", async () => {
  const { PlaygroundSim } = await importSim();
  const pg = playground();
  // Everything the sim prints when the page plays every station, plus the two
  // lines no row reaches: a sensor unplugged (the census's detach).
  const lines = pg.stations.flatMap((st) => playStation(PlaygroundSim, st));
  const s = new PlaygroundSim();
  for (const kind of ["veml", "tof", "pad"]) {
    s.attach(kind, 0);
    lines.push(...s.detach(kind, 1));
  }
  const evts = lines.map((l) => l.match(/^PG1 \d+ EVT (\S+) (.*)$/)).filter(Boolean);
  const exemptSeen = new Set();
  for (const file of [SRC_CPP, PLAYGROUND_CPP]) {
    const calls = sayEvtCalls(file);
    assert.ok(calls.length >= 20, `${file}: read only ${calls.length} say_evt() calls`);
    const missing = [];
    for (const c of calls) {
      for (const station of c.stations) {
        const printed = `${station} ${c.body}`;
        if (SIM_EXEMPT.has(printed)) {
          exemptSeen.add(printed);
          continue;
        }
        const re = new RegExp(`^${c.pattern}$`);
        if (!evts.some((e) => e[1] === station && re.test(e[2]))) {
          missing.push(`${c.where}: "${station} ${c.body}"`);
        }
      }
    }
    assert.deepStrictEqual(missing, [], `firmware PG1 lines the sim cannot print:\n  ${missing.join("\n  ")}`);
  }
  for (const line of SIM_EXEMPT.keys()) {
    assert.ok(exemptSeen.has(line), `SIM_EXEMPT names "${line}", which the firmware no longer prints`);
  }
});

// The sim's live model has the firmware's: PgState's members, each of its
// station structs' fields, and their defaults (playground.h's initializers).
test("the sim's state has PgState's fields and defaults, last_exc among them", async () => {
  const { PlaygroundSim } = await importSim();
  const g = new PlaygroundSim().g;
  const h = readFileSync(PLAYGROUND_H, "utf8");
  const structs = new Map();
  for (const m of h.matchAll(/^struct (\w+) \{\n([\s\S]*?)^\};/gm)) {
    const fields = new Map();
    for (const f of m[2].matchAll(/^\s+([\w:]+)\s+([\w, ]+?)(\[\d+\])?(?:\s*=\s*([^;]+))?;/gm)) {
      for (const name of f[2].split(",").map((n) => n.trim())) {
        fields.set(name, { type: f[1], array: !!f[3], init: f[4] });
      }
    }
    structs.set(m[1], fields);
  }
  const pgState = structs.get("PgState");
  assert.ok(pgState && pgState.size >= 11, "PgState not read from playground.h");
  assert.deepStrictEqual(Object.keys(g).sort(), [...pgState.keys()].sort(), "PgState members");
  const literal = (init) => {
    if (init === "false") return false;
    if (init === "true") return true;
    const n = Number(init.replace(/f$/, ""));
    assert.ok(!Number.isNaN(n), `initializer ${init} the pin cannot read`);
    return n;
  };
  for (const [member, { type }] of pgState) {
    const fields = structs.get(type);
    if (!fields) continue; // a plain member (display_ok: the sim's page always has a glass)
    assert.deepStrictEqual(Object.keys(g[member]).sort(), [...fields.keys()].sort(), `${member} (${type}) fields`);
    for (const [name, f] of fields) {
      if (f.array) {
        assert.ok(Array.isArray(g[member][name]), `${member}.${name} is an array in the sim`);
        continue;
      }
      assert.ok(f.init !== undefined, `${type}::${name} has no initializer to compare`);
      assert.strictEqual(g[member][name], literal(f.init.trim()), `${member}.${name} default`);
    }
  }
});

// The glass's RS485 words are playground.h's rs485_row_words() and
// rs485_tile_words(), which tests_host/test_fieldbus_glue.cpp holds to every
// probe outcome in three builds; tests_host/test_playground_glass.cpp
// compiles the canonical playground_ui.cpp and holds what it draws to them.
// Here both copies of playground_ui.cpp, the Arduino mirror too, must draw the
// row and the tile through them, with no word of their own.
test("the RS485 row and tile on the glass draw only playground.h's held words", () => {
  for (const file of [SRC_UI, PLAYGROUND_UI]) {
    const ui = readFileSync(file, "utf8");
    for (const [label, helper] of [["ROW_RS485", "rs485_row_words"], ["ST_RS485", "rs485_tile_words"]]) {
      const at = ui.indexOf(`case ${label}:`);
      assert.ok(at >= 0, `${file}: no case ${label}`);
      const block = ui.slice(at, ui.indexOf("break;", at));
      assert.ok(block.includes(`${helper}(g.rs485`), `${file}: case ${label} does not call ${helper}()`);
      assert.doesNotMatch(block, /"/, `${file}: case ${label} writes a word of its own`);
    }
  }
  for (const file of [PLAYGROUND_H, join(DISPLAY, "arduino/canary_display/playground.h")]) {
    const h = readFileSync(file, "utf8");
    assert.ok(h.includes("inline Rs485Look rs485_row_words(") && h.includes("inline Rs485Look rs485_tile_words("),
      `${file}: the RS485 word helpers are missing`);
  }
});

// The bring-up snippets the page shows are code a bench copies: each one
// compiles (g++ -fsyntax-only -Werror) against tests_host/fake_fieldbus, the
// host stand-ins for the Arduino core and ESP-IDF TWAI, with Arduino.h and
// the 4.3B pins.h in front of it as a sketch has them. The ESP32 toolchain
// itself is not run here.
//
// Without g++ the compile cannot run, and a gate that skips itself is a gate
// a runner without the tool passes without compiling anything. So, the shape
// of VISION_WASM32 in tests/native/wasm32.js: PLAYGROUND_SNIPPETS unset (or
// empty) skips and says so; PLAYGROUND_SNIPPETS=require fails and names g++;
// anything else is refused rather than read as one of the two. CI sets
// `require` on the step that runs this file (the test after this one holds
// it there).
function snippetsRequired() {
  const v = process.env.PLAYGROUND_SNIPPETS;
  if (v === undefined || v === "") return false;
  if (v === "require") return true;
  throw new Error(`PLAYGROUND_SNIPPETS=${v}: expected "require" (fail where g++ is missing) ` +
    "or nothing (skip there)");
}

test("every station's code snippet compiles against the host Arduino/TWAI fakes", async (t) => {
  const required = snippetsRequired();
  const probe = spawnSync("g++", ["--version"], { encoding: "utf8" });
  if (probe.error || probe.status !== 0) {
    const why = probe.error ? probe.error.message : `g++ --version exited ${probe.status}`;
    assert.ok(!required, `PLAYGROUND_SNIPPETS=require, and g++ is not found (${why}): ` +
      "the snippet compile cannot run, so the gate fails here instead of skipping");
    t.skip(`g++ not found (${why}): the snippet compile is not run; PLAYGROUND_SNIPPETS=require fails here instead`);
    return;
  }
  const pg = playground();
  const dir = mkdtempSync(join(tmpdir(), "pg-snippets-"));
  try {
    const jobs = [];
    const seen = new Set();
    for (const st of pg.stations) {
      for (const [variant, code] of Object.entries(st.code)) {
        if (seen.has(code)) continue;
        seen.add(code);
        const file = join(dir, `snippet_${jobs.length}.cpp`);
        writeFileSync(file, `#include <Arduino.h>\n#include "pins.h"\n${code}\n`);
        const args = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
          "-I", join(DISPLAY, "tests_host/fake_fieldbus"), "-I", join(DISPLAY, "include"),
          "-I", join(ROOT, "firmware/boards/waveshare-esp32s3-lcd43b/pins"), file];
        jobs.push(new Promise((resolve) => {
          const child = spawn("g++", args);
          let stderr = "";
          child.stderr.on("data", (d) => (stderr += d));
          child.on("close", (status) => resolve({ what: `${st.id} (${variant})`, status, stderr }));
        }));
      }
    }
    assert.ok(jobs.length >= 10, `found only ${jobs.length} snippets`);
    for (const r of await Promise.all(jobs)) {
      assert.strictEqual(r.status, 0, `${r.what} snippet does not compile:\n${r.stderr}`);
    }
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
});

// The require mode above is only a gate when CI turns it on. The variable
// takes "require" or nothing, never a look-alike, and the page logic job's
// step that runs this file sets it (read as the runner reads the step,
// tests/native/ci_steps.js: a comment saying so, or the variable on another
// step, fails here).
test("PLAYGROUND_SNIPPETS takes \"require\" or nothing, and CI sets it on the step that runs this file", () => {
  const saved = process.env.PLAYGROUND_SNIPPETS;
  try {
    for (const [v, want] of [[undefined, false], ["", false], ["require", true]]) {
      if (v === undefined) delete process.env.PLAYGROUND_SNIPPETS;
      else process.env.PLAYGROUND_SNIPPETS = v;
      assert.strictEqual(snippetsRequired(), want, String(v));
    }
    for (const v of ["1", "true", "required", "Require"]) {
      process.env.PLAYGROUND_SNIPPETS = v;
      assert.throws(() => snippetsRequired(), /expected "require" \(fail where g\+\+ is missing\)/, v);
    }
  } finally {
    if (saved === undefined) delete process.env.PLAYGROUND_SNIPPETS;
    else process.env.PLAYGROUND_SNIPPETS = saved;
  }

  const ci = require("./native/ci_steps.js");
  const { steps } = ci.job("logic-tests");
  const line = "node --test canary-local/tests/playground.test.js";
  const mine = steps.filter((st) => (st.run || "").split("\n").includes(line));
  assert.strictEqual(mine.length, 1, "one step of the page logic job runs this file");
  const [step] = mine;
  assert.strictEqual(step.env.PLAYGROUND_SNIPPETS, "require",
    `${step.name}: runs this file without PLAYGROUND_SNIPPETS=require, so a runner without g++ would pass the snippet gate by skipping it`);
  for (const [k, v] of Object.entries(ci.RECORDER)) {
    assert.strictEqual(step.env[k], v, `${step.name}: the path-filter recorder's ${k} line`);
  }
  assert.ok(!("continue-on-error" in step.keys), "its red stays red (no continue-on-error)");
});
