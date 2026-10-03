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

// Sweep A54: the Nightlight's twin carries a bench, and every fact in it is
// one a file in this repository states. A bench block may cite its facts
// (`sources`: fact, file, quote); each cited file must exist and still say
// the quoted words, so a fact whose source changes fails here, not on a desk.
test("bench facts cite files that still say them; the Nightlight's says what its board is", async () => {
  const REPO = join(ROOT, "..");
  for (const dev of registry().devices) {
    for (const s of dev.bench?.sources || []) {
      assert.ok(s.fact && s.file && s.quote, `${dev.id}: a source names its fact, file and quote`);
      const text = readFileSync(join(REPO, s.file), "utf8");
      assert.ok(text.includes(s.quote), `${dev.id}: ${s.file} no longer says ${JSON.stringify(s.quote)} (${s.fact})`);
    }
  }
  const nl = registry().devices.find((d) => d.id === "canary-nightlight");
  assert.deepStrictEqual(nl.emulator, { module: "emulator/dist/canary-display-nightlight.js", factory: "createCanaryEmuNightlight" },
    "the Lab boots the nightlight flavor CI built (F204)");
  const b = nl.bench;
  assert.ok(b.sources.length >= 5, "the Nightlight's bench cites where its facts come from");
  const cites = (file) => b.sources.some((s) => s.file === file);
  // power: USB-C only — no battery, no charger (pins.h says so; the board
  // README says it too, but this job may read only the pin maps of boards/)
  const pins = readFileSync(join(REPO, "firmware/boards/waveshare-esp32c3-lcd147/pins/pins.h"), "utf8");
  assert.match(pins, /#define HAS_BATTERY\s+0\b/, "pins.h: the C3-LCD-1.47 has no battery");
  assert.strictEqual(b.power.battery, undefined, "no battery on the bench the board does not have");
  assert.match(b.power.usb, /^USB-C/);
  assert.ok(cites("firmware/boards/waveshare-esp32c3-lcd147/pins/pins.h"));
  // the panel's backlight cap: the nightlight config's, enforced in the board's HAL only
  const cfg = readFileSync(join(REPO, "firmware/configs/canary-display/nightlight/config.h"), "utf8");
  const cap = Number(/#define CD_BL_MAX_PCT\s+(\d+)/.exec(cfg)?.[1]);
  assert.ok(cap > 0 && cap < 100, "the nightlight config caps the backlight");
  assert.strictEqual(b.backlight.cap_pct, cap, "the bench states the config's cap");
  assert.ok(cites("firmware/configs/canary-display/nightlight/config.h"));
  const emuHal = readFileSync(join(ROOT, "emulator/src/emu_hal_display.cpp"), "utf8");
  assert.ok(!emuHal.includes("CD_BL_MAX_PCT"), "the twin's HAL does not model the cap, as the bench says");
  assert.match(b.backlight.note, /twin/, "the note says the twin does not clip");
  // and the Lab shows it: the bench's backlight row reads the cap off the profile
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.match(app, /const blCap = profile\.backlight\?\.cap_pct;/);
  assert.match(app, /blCap \? ` · the board caps it at \$\{blCap\}% duty, this twin does not` : ""/);
  // lights: the board's files disagree (the case file names an RGB LED and a
  // charge LED; the board README and pins.h a board with neither), so the
  // bench lists none until a bench look settles it
  assert.strictEqual(b.leds, undefined, "no light is listed whose wiring no file settles");
});

test("the Nightlight's bench: USB is its only power, so pulling it kills the rail", async () => {
  const { BenchPower } = await importBench();
  const profile = benchProfile("canary-nightlight");
  // its own block, not the plane's defaults (which fit no battery either)
  assert.match(profile?.power?.usb || "", /^USB-C/, "the Nightlight's card carries its own bench (A54)");
  assert.strictEqual(profile.power.switch?.controls, "battery", "its switch row names the battery path it would gate");
  const { bench, events } = rig(profile, BenchPower);
  assert.strictEqual(bench.batteryFitted, false, "no battery fitted where the board has none");
  bench.setSwitch(false);
  assert.ok(bench.powered(), "the listed-as-none switch gates nothing: USB keeps the board up");
  bench.setSwitch(true);
  bench.setUsb(false);
  assert.strictEqual(bench.mode, "off");
  assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(), ["power", false, "usb-out"]);
  assert.deepStrictEqual(bench.leds(), {}, "no lights listed, none lit");
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
