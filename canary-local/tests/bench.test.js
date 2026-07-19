// canary-local/tests/bench.test.js — Node tests for the physical bench
// (repo convention: CI runs the exact shipped source).
//
// Pins the power-plane truth table the page teaches with:
//   rail up ⇔ USB ∨ (battery ∧ switch ∧ charge>0)
// — the switch gates ONLY the battery path, the straps are sampled only
// at reset, the charge/power LEDs answer to the rail and the charger
// (never to firmware), and the ROM banners say what a real ESP32-S3 says.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");

async function importBench() {
  return import("../emulator/web/bench.js");
}
async function importGuides() {
  return import("../assets/guides.js");
}

function registry() {
  return JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
}
function benchProfile(id) {
  const dev = registry().devices.find((d) => d.id === id);
  assert.ok(dev, id);
  return dev.bench;
}

// Recording harness: capture every callback the plane fires.
function rig(profile, BenchPower) {
  const events = [];
  const bench = new BenchPower(profile, {
    onPower: (up, cause) => events.push(["power", up, cause]),
    onReset: (kind) => events.push(["reset", kind]),
    onLog: (line) => events.push(["log", line]),
  });
  return { bench, events };
}

// ── registry: the bench blocks are complete and honest ──────────────────

test("every display entry carries a bench block; witnesses carry none", () => {
  for (const dev of registry().devices) {
    if (dev.emulator) {
      assert.ok(dev.bench, `${dev.id} has a bench block`);
      assert.ok(dev.bench.power?.usb, `${dev.id} names its USB source`);
      assert.strictEqual(dev.bench.power.switch.controls, "battery",
        `${dev.id}: the slide switch gates the battery path only`);
      const ids = (dev.bench.buttons || []).map((b) => b.id).sort();
      assert.deepStrictEqual(ids, ["boot", "reset"], `${dev.id} buttons`);
      for (const led of dev.bench.leds || []) {
        assert.ok(["rail", "charger", "gpio"].includes(led.driver),
          `${dev.id}/${led.label}: driver names a real wire`);
        assert.ok(led.note, `${dev.id}/${led.label}: honesty note present`);
      }
    } else {
      assert.strictEqual(dev.bench, undefined, `${dev.id}: no glass, no bench`);
    }
  }
});

test("dash wears PWR/CHG/DONE; watch wears CHG (flickers batteryless) + USER", () => {
  const dash = benchProfile("canary-display-dash");
  assert.deepStrictEqual(dash.leds.map((l) => l.id), ["pwr", "chg", "done"]);
  assert.strictEqual(dash.leds[0].driver, "rail");
  const watch = benchProfile("canary-display-watch");
  const chg = watch.leds.find((l) => l.id === "chg");
  assert.strictEqual(chg.flicker_without_battery, true,
    "the XIAO's documented no-battery flicker is modeled");
  const user = watch.leds.find((l) => l.id === "user");
  assert.strictEqual(user.driver, "gpio",
    "the USER LED is the one firmware-drivable light");
});

// ── the power truth table ───────────────────────────────────────────────

test("the switch gates only the battery: USB keeps the board up", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-dash"), BenchPower);
  bench.setSwitch(false);
  assert.ok(bench.powered(), "switch OFF + USB in → still powered");
  assert.ok(!events.some(([k, up]) => k === "power" && up === false));
  bench.setUsb(false);
  assert.ok(!bench.powered(), "…until USB leaves too");
  assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(),
    ["power", false, "usb-out"]);
});

test("a healthy switched-on battery rides through a USB pull, silently", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-watch"), BenchPower);
  bench.setUsb(false);
  assert.ok(bench.powered(), "battery carries the rail");
  assert.strictEqual(bench.source(), "battery");
  assert.ok(!events.some(([k]) => k === "power"),
    "no rail transition — the firmware never notices");
});

test("no battery fitted: pulling USB kills the rail instantly", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-dash"), BenchPower);
  bench.setBattery(false);
  bench.setUsb(false);
  assert.strictEqual(bench.mode, "off");
  assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(),
    ["power", false, "usb-out"]);
});

// ── straps and buttons ──────────────────────────────────────────────────

test("BOOT held through RESET parks the ROM; a plain RESET recovers", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-dash"), BenchPower);
  bench.setBootHeld(true);
  bench.pressReset();
  assert.strictEqual(bench.mode, "download");
  assert.ok(!bench.canBoot(), "download mode never boots the app");
  assert.deepStrictEqual(events.filter(([k]) => k === "reset").pop(),
    ["reset", "download"]);
  bench.setBootHeld(false);
  bench.pressReset();
  assert.strictEqual(bench.mode, "run");
  assert.deepStrictEqual(events.filter(([k]) => k === "reset").pop(),
    ["reset", "reset"]);
});

test("RESET with no power does nothing at all", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-dash"), BenchPower);
  bench.setBattery(false);
  bench.setUsb(false);
  events.length = 0;
  bench.pressReset();
  assert.ok(!events.some(([k]) => k === "reset"));
  assert.strictEqual(bench.mode, "off");
});

test("power arriving re-samples the straps — BOOT low means download", async () => {
  const { BenchPower } = await importBench();
  const { bench } = rig(benchProfile("canary-display-watch"), BenchPower);
  bench.setBattery(false);
  bench.setUsb(false);
  bench.setBootHeld(true);
  bench.setUsb(true);
  assert.strictEqual(bench.mode, "download");
});

test("even a software reset (ESP.restart) re-samples the straps", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-dash"), BenchPower);
  bench.setBootHeld(true);
  bench.firmwareRestart();
  assert.strictEqual(bench.mode, "download");
  assert.deepStrictEqual(events.filter(([k]) => k === "reset").pop(),
    ["reset", "download"]);
});

// ── the lights: wired past the chip ─────────────────────────────────────

test("dash LEDs follow the rail and the charger, never the firmware", async () => {
  const { BenchPower } = await importBench();
  const { bench } = rig(benchProfile("canary-display-dash"), BenchPower);
  // USB + part-charged battery: PWR lit, CHG filling, DONE dark.
  assert.deepStrictEqual(bench.leds(), { pwr: "on", chg: "on", done: "off" });
  // Full: CHG hands over to DONE.
  bench.soc = 100;
  assert.deepStrictEqual(bench.leds(), { pwr: "on", chg: "off", done: "on" });
  // Battery only: rail up, charger idle (it runs off USB).
  bench.setUsb(false);
  assert.deepStrictEqual(bench.leds(), { pwr: "on", chg: "off", done: "off" });
  // Rail down: everything dark.
  bench.setSwitch(false);
  assert.deepStrictEqual(bench.leds(), { pwr: "off", chg: "off", done: "off" });
});

test("watch CHG flickers with no battery; USER stays dark by design", async () => {
  const { BenchPower } = await importBench();
  const { bench } = rig(benchProfile("canary-display-watch"), BenchPower);
  bench.setBattery(false);
  assert.strictEqual(bench.leds().chg, "flicker", "charger hunting for a cell");
  assert.strictEqual(bench.leds().user, "off",
    "the one firmware-drivable LED — this firmware leaves it dark");
  bench.setBattery(true);
  assert.strictEqual(bench.leds().chg, "on", "steady while filling");
});

// ── the battery model ───────────────────────────────────────────────────

test("charging: SOC climbs on USB, tops out, and DONE takes over", async () => {
  const { BenchPower, CHARGE_PCT_PER_SEC } = await importBench();
  const { bench } = rig(benchProfile("canary-display-dash"), BenchPower);
  const before = bench.soc;
  bench.tick(1000);
  assert.ok(bench.soc > before, "SOC climbs on USB");
  assert.ok(Math.abs(bench.soc - before - CHARGE_PCT_PER_SEC) < 1e-9);
  bench.soc = 99.9;
  bench.tick(60000);
  assert.strictEqual(bench.soc, 100);
  assert.strictEqual(bench.leds().done, "on");
});

test("the charger runs even with the switch OFF — it gates output, not charging", async () => {
  const { BenchPower } = await importBench();
  const { bench } = rig(benchProfile("canary-display-dash"), BenchPower);
  bench.setSwitch(false);
  const before = bench.soc;
  bench.tick(1000);
  assert.ok(bench.soc > before);
  assert.strictEqual(bench.leds().chg, "on");
});

test("a draining battery browns out at 0 and USB revives the board", async () => {
  const { BenchPower } = await importBench();
  const { bench, events } = rig(benchProfile("canary-display-watch"), BenchPower);
  bench.setUsb(false);
  bench.soc = 0.01;
  bench.tick(60000);
  assert.strictEqual(bench.soc, 0);
  assert.strictEqual(bench.mode, "off");
  assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(),
    ["power", false, "battery-empty"]);
  assert.ok(events.some(([k, line]) => k === "log" && /Brownout/.test(line)));
  bench.setUsb(true);
  assert.strictEqual(bench.mode, "run");
  assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(),
    ["power", true, "usb-in"]);
});

test("the bench fast-forward scales the battery, not virtual time", async () => {
  const { BenchPower, DRAIN_PCT_PER_SEC } = await importBench();
  const { bench } = rig(benchProfile("canary-display-watch"), BenchPower);
  bench.setUsb(false);
  bench.rate = 60;
  const before = bench.soc;
  bench.tick(1000);
  assert.ok(Math.abs(before - bench.soc - DRAIN_PCT_PER_SEC * 60) < 1e-9);
});

// ── the ROM's voice ─────────────────────────────────────────────────────

test("ROM banners say what a real ESP32-S3 says", async () => {
  const { romBanner } = await importBench();
  assert.match(romBanner("poweron"), /^ESP-ROM:esp32s3-20210327\n/);
  assert.match(romBanner("poweron"), /rst:0x1 \(POWERON\),boot:0x8 \(SPI_FAST_FLASH_BOOT\)/);
  assert.match(romBanner("swreset"), /rst:0x3 \(RTC_SW_SYS_RST\)/);
  assert.match(romBanner("download"), /boot:0x0 \(DOWNLOAD\(USB\/UART0\)\)/);
  assert.match(romBanner("download"), /waiting for download/);
  assert.ok(!/waiting for download/.test(romBanner("reset")),
    "only a BOOT-low reset waits for a flasher");
});

// ── the debug curriculum stages cleanly ─────────────────────────────────

test("every BENCH_FIXES flow is complete and its stages run", async () => {
  const { BENCH_FIXES } = await importGuides();
  const { BenchPower } = await importBench();
  assert.ok(BENCH_FIXES.length >= 6, "a real curriculum, not a stub");
  const { bench } = rig(benchProfile("canary-display-dash"), BenchPower);
  const ctx = {
    bench,
    emu: { setWifi() {}, setBroker() {}, setTimeScale() {} },
    setHour() {},
    note() {},
  };
  for (const fix of BENCH_FIXES) {
    assert.ok(fix.symptom, "symptom named");
    assert.ok(fix.steps.length >= 1, fix.symptom);
    for (const step of fix.steps) {
      assert.ok(step.title && step.body, `${fix.symptom}: ${step.title}`);
      if (step.stage) await step.stage(ctx); // must not throw
    }
  }
});
