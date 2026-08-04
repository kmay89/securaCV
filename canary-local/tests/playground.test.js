// canary-local/tests/playground.test.js — Node tests for the Waveshare 4.3B
// peripheral Playground (repo convention: CI runs the exact shipped source).
//
// Two jobs:
//   1. HONESTY — devices/playground.json only names real terminals, its wiring
//      prose is verbatim from the firmware playground UI, and its bring-up code
//      carries the CH422G addresses/bits that actually live in the board pins.h.
//   2. BEHAVIOR — assets/playground-sim.js reproduces the firmware's PG1 driver
//      line-for-line (DI edges + doorbell→DO0 ding, DO pulse/latch auto-off,
//      ToF trip, cap-touch, census, SNAP), pinned against the .cpp constants.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..", "..");
const CL = join(__dirname, "..");
const PLAYGROUND_CPP = join(
  ROOT,
  "firmware/projects/canary-display/arduino/canary_display/playground.cpp"
);
const PLAYGROUND_UI = join(
  ROOT,
  "firmware/projects/canary-display/arduino/canary_display/playground_ui.cpp"
);
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
