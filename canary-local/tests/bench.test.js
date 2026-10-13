// canary-local/tests/bench.test.js — Node tests for the physical bench
// (repo convention: CI runs the exact shipped source).
//
// Pins the power-plane truth table the page teaches with:
//   rail up ⇔ USB ∨ (battery ∧ switch ∧ charge>0)
// — the switch gates ONLY the battery path, the straps are sampled only
// at reset, the charge/power LEDs answer to the rail and the charger
// (never to firmware), and each board's ROM banners are its own chip's,
// quoted from Espressif (sweep A59).
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
  // the twin's HAL clips where the board's does (sweep A61; the behavior is
  // held natively by emulator/test/backlight_cap_test.cpp, below), and the
  // note says so, and that an emulator built before it does not
  const emuHal = readFileSync(join(ROOT, "emulator/src/emu_hal_display.cpp"), "utf8");
  assert.ok(emuHal.includes("constexpr int kBacklightCapPct = CD_BL_MAX_PCT;"), "the twin's HAL takes the config's cap");
  assert.ok(cites("canary-local/emulator/src/emu_hal_display.cpp"), "the bench cites the twin's HAL for what it does");
  assert.match(b.backlight.note, /twin's display HAL clips it at the same register byte/, "the note says the twin clips");
  assert.match(b.backlight.note, /built before it set the level as asked/, "...and what an older emulator did");
  // ...and steps the night floor to the expander's byte as the board writes it
  // (sweep A79), citing the board's compression and the twin's step
  assert.match(b.backlight.note, /twin's HAL steps the night floor to that byte too/, "the note says the twin steps the night floor");
  assert.ok(b.sources.some((x) => x.file === "firmware/projects/canary-display/src/hal/display_1in47.cpp" &&
    x.quote === "uint8_t d8 = (uint8_t)(duty13 >> 5);"), "the bench cites the board's night byte");
  assert.ok(b.sources.some((x) => x.file === "canary-local/emulator/src/emu_hal_display.cpp" && x.quote === "return byte << 5;"),
    "...and the twin's step");
  // and the Lab shows it: the bench's backlight row reads the cap off the
  // profile and the twin's own cap off the emulator, and says which
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.match(app, /const blCap = profile\.backlight\?\.cap_pct;/);
  assert.match(app, /const twinCap = ctx\.emu\.c\.backlightCap \? await ctx\.emu\.c\.backlightCap\(\) : null;/);
  assert.match(app, /: `day ladder · \$\{level\}\/255`\) \+ backlightCapNote\(blCap, twinCap\);/);
  // lights: the board's files disagree (the case file names an RGB LED and a
  // charge LED; the board README and pins.h a board with neither), so the
  // bench lists none until a bench look settles it
  assert.strictEqual(b.leds, undefined, "no light is listed whose wiring no file settles");
});

// Sweep A61: the twin's display HAL clips the backlight where the board's
// does; sweep A79: on the expander board it also steps the night floor to
// the byte the board writes (duty >> 5, never dark while a glow is asked
// for). The behavior is held natively — emulator/test/backlight_cap_test.cpp,
// which glass_turn.sh builds against the real emu_hal_display.cpp in the
// nightlight's config and the dash's, holds every day level and every night
// duty to the C3-LCD-1.47's expander path — so this holds what that test
// leans on: its restatement of the board's path is the board's source, the
// script runs it in both configs, and the page says what the emulator in use
// does (backlightCapNote, run as app.js ships it).
test("A61, A79: the twin clips the Nightlight's backlight and steps its night floor where the board's HAL does, and the bench row says which", () => {
  const REPO = join(ROOT, "..");
  const board = readFileSync(join(REPO, "firmware/projects/canary-display/src/hal/display_1in47.cpp"), "utf8");
  const native = readFileSync(join(ROOT, "emulator/test/backlight_cap_test.cpp"), "utf8");
  const hal = readFileSync(join(ROOT, "emulator/src/emu_hal_display.cpp"), "utf8");
  // the board's expander path, line for line, and the test's restatement of it
  for (const line of ["#if defined(TFT_USE_EXIO) && TFT_USE_EXIO\n#define CD_1IN47_EXIO 1",
                      "#ifndef CD_BL_MAX_PCT\n#define CD_BL_MAX_PCT 100\n#endif",
                      "constexpr uint8_t kCap = (uint8_t)((255u * CD_BL_MAX_PCT) / 100u);",
                      "exio_write_reg(EXIO_REG_PWM, duty8 > kCap ? kCap : duty8);",
                      "if (duty13 > NIGHT_DUTY_MAX) duty13 = NIGHT_DUTY_MAX;",
                      "uint8_t d8 = (uint8_t)(duty13 >> 5);\n  if (d8 == 0 && duty13 > 0) d8 = 1;\n  exio_backlight(d8);",
                      "#ifdef CD_1IN47_EXIO\n  // Expander PWM: plain 0..255 duty; the CD_BL_MAX_PCT heat cap is applied\n  // inside exio_backlight, under every caller.\n  exio_backlight(level);"]) {
    assert.ok(board.includes(line), `display_1in47.cpp no longer says ${JSON.stringify(line)}: restate it in backlight_cap_test.cpp`);
  }
  for (const line of ["#if defined(TFT_USE_EXIO) && TFT_USE_EXIO", "constexpr unsigned kCap = (255u * kCapPct) / 100u;",
                      "unsigned exio(unsigned duty8) { return duty8 > kCap ? kCap : duty8; }",
                      "if (duty13 > 8191) duty13 = 8191;", "unsigned d8 = duty13 >> 5;", "if (d8 == 0 && duty13 > 0) d8 = 1;"]) {
    assert.ok(native.includes(line), `backlight_cap_test.cpp no longer restates ${JSON.stringify(line)}`);
  }
  // ...and it holds the twin's own night duty to the board's byte for it, on
  // the 13-bit scale (sweep A79), not only the byte the board would make of
  // it: a byte compare passes a twin that never clips (the board's clip,
  // applied to the twin's duty, hides it) and one that never steps (>> 5 of
  // an unstepped duty is the same byte). It counts the steps too.
  assert.ok(native.includes("unsigned twin_night(unsigned duty13) { return board_byte_night(duty13) << 5; }") &&
    native.includes("CHECK(got == (int)twin_night(duty),") && native.includes("CHECK((got & 31) == 0,") &&
    native.includes("CHECK(((unsigned)got >> 5) <= kCap,") &&
    native.includes("CHECK(steps == (int)kCap + 1 && lowest_glow == 32 && step_seen[kNightTop13] && night_stepped > 0,"),
  "backlight_cap_test.cpp holds the night duty the twin applies to the board's byte, step by step");
  // the twin's night step, as the board's backlight_night_set() makes the
  // byte, on the expander board alone
  const nightBody = /int capped_night\(uint16_t duty13\) \{\n([\s\S]*?)\n\}/.exec(hal)?.[1] || "";
  assert.strictEqual(nightBody, "#if defined(TFT_USE_EXIO) && TFT_USE_EXIO\n" +
    "  const int duty = duty13 > 8191 ? 8191 : duty13;  // NIGHT_DUTY_MAX\n" +
    "  int byte = duty >> 5;\n  if (byte == 0 && duty > 0) byte = 1;\n" +
    "  if (byte > kBacklightCap8) byte = kBacklightCap8;\n  return byte << 5;\n#else\n  return duty13;\n#endif",
  "the twin steps the night floor to the expander's byte on the expander board, and passes it elsewhere");
  // the twin's HAL: the board's gate, the board's byte, both sinks through it,
  // and the binding the page asks
  assert.ok(hal.includes("#if defined(TFT_USE_EXIO) && TFT_USE_EXIO\n#ifndef CD_BL_MAX_PCT\n#define CD_BL_MAX_PCT 100"));
  assert.ok(hal.includes("g_backlight_level = capped_level(level);") && hal.includes("js_backlight_apply(g_backlight_level, -1);"));
  assert.ok(hal.includes("g_night_duty13 = capped_night(duty13);") && hal.includes("js_backlight_apply(-1, g_night_duty13);"));
  assert.ok(hal.includes("EMSCRIPTEN_KEEPALIVE int emu_backlight_cap_pct(void) { return kBacklightCapPct; }"));
  const shell = readFileSync(join(ROOT, "emulator/web/emu-shell.js"), "utf8");
  assert.ok(shell.includes('backlightCap: M._emu_backlight_cap_pct ? M.cwrap("emu_backlight_cap_pct", "number", []) : null,'),
    "the shell asks the twin's cap only of a dist that has it");
  // CI's native glass step runs the test in both configs
  const gt = readFileSync(join(ROOT, "emulator/test/glass_turn.sh"), "utf8");
  assert.ok(gt.includes('"${CXX[@]}" -c "$HERE/backlight_cap_test.cpp" -o "$OUT/backlight_cap_test.o"') &&
    gt.includes('\n"$OUT/backlight_cap_test"\n'), "glass_turn.sh runs the backlight test in the dash config");
  assert.ok(gt.includes('"${NL_CXX[@]}" -c "$HERE/backlight_cap_test.cpp" -o "$OUT/nl_backlight_cap_test.o"') &&
    gt.includes('\n"$OUT/nl_backlight_cap_test"\n'), "...and in the nightlight's");
  // the bench row's word, as app.js ships it
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  const at = app.indexOf("function backlightCapNote(");
  assert.ok(at >= 0, "app.js has backlightCapNote()");
  let depth = 0, end = -1;
  for (let k = app.indexOf("{", app.indexOf(")", at)); k < app.length; k++) {
    if (app[k] === "{") depth++;
    else if (app[k] === "}" && --depth === 0) { end = k + 1; break; }
  }
  const note = new Function(`${app.slice(at, end)}\nreturn backlightCapNote;`)();
  assert.strictEqual(note(50, 50), " · capped at 50% duty, as the board's HAL caps it", "a twin that clips where the board does");
  assert.strictEqual(note(50, null), " · the board caps it at 50% duty, this twin does not", "a dist built before A61");
  assert.strictEqual(note(50, 100), " · the board caps it at 50% duty, this twin does not", "a twin that clips nothing");
  assert.strictEqual(note(50, 40), " · the board caps it at 50% duty, this twin at 40%", "a twin that clips elsewhere");
  assert.strictEqual(note(undefined, 100), "", "no cap on the board, none on the twin");
  assert.strictEqual(note(undefined, null), "");
  assert.strictEqual(note(undefined, 60), " · this twin caps it at 60% duty", "a twin cap the bench block does not state is said");
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
  // a hand at the battery chip: the board has no battery path, so nothing fits
  bench.setBattery(true);
  assert.strictEqual(bench.batteryFitted, false, "no battery can be fitted where the board has no battery path");
  assert.ok(events.some(([k, l]) => k === "log" && /no battery path/.test(l)), "the bench says why nothing fitted");
  bench.setUsb(false);
  assert.strictEqual(bench.mode, "off");
  assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(), ["power", false, "usb-out"]);
  assert.deepStrictEqual(bench.leds(), {}, "no lights listed, none lit");
});

// A bench block with no `power.battery` is a board with no battery path
// (the Nightstand stick, the Nightlight): its battery chip cannot fit one,
// so pulling USB drops the rail whatever was clicked first, and the page
// draws the chip inert and saying so. A board with a battery still takes one.
test("a board with no battery path takes no battery, so a USB pull always drops its rail", async () => {
  const { BenchPower } = await importBench();
  const none = registry().devices.filter((d) => d.bench && !d.bench.power?.battery).map((d) => d.id).sort();
  assert.deepStrictEqual(none, ["canary-display-nightstand-s3", "canary-nightlight"],
    "the benches with no battery path are the two USB-only boards");
  for (const id of none) {
    const { bench, events } = rig(benchProfile(id), BenchPower);
    assert.strictEqual(bench.batteryPath, false, `${id}: no battery path`);
    bench.setBattery(true);
    bench.setBattery(true);
    assert.strictEqual(bench.batteryFitted, false, `${id}: a click on the battery chip fits nothing`);
    bench.tick(60000);
    assert.strictEqual(bench.soc, 62, `${id}: no cell to charge`);
    bench.setUsb(false);
    assert.strictEqual(bench.mode, "off", `${id}: USB out, rail down`);
    assert.strictEqual(bench.source(), "none");
    assert.deepStrictEqual(events.filter(([k]) => k === "power").pop(), ["power", false, "usb-out"], id);
  }
  const { bench } = rig(benchProfile("canary-display-dash"), BenchPower);
  assert.strictEqual(bench.batteryPath, true);
  bench.setBattery(false);
  bench.setBattery(true);
  assert.strictEqual(bench.batteryFitted, true, "a board with a battery header still takes a battery");
  // and the page: an inert chip that says the board has none
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.match(app, /if \(!bench\.batteryPath\) \{\s*bat\.b\.disabled = true;/, "the battery chip is inert with no battery path");
  assert.match(app, /bat\.st\.textContent = !bench\.batteryPath \? "none on this board"/, "and says the board has none");
});

// The troubleshooter's steps stage the bench. A step that fits a battery
// says so (`battery: true`), and a board with no battery path is offered the
// flows without those steps, so no staged rail-down sits under words about
// riding a battery.
test("a bench with no battery path is offered no step that stages a battery", async () => {
  const { BENCH_FIXES, benchFixesFor } = await importGuides();
  const { BenchPower } = await importBench();
  const ctxFor = (bench) => ({
    bench,
    emu: { setWifi() {}, setBroker() {}, setTimeScale() {} },
    setHour() {},
    note() {},
  });
  // the marker is honest: exactly the steps whose stage fits a battery carry it
  for (const fix of BENCH_FIXES) {
    for (const step of fix.steps) {
      if (!step.stage) {
        assert.ok(!step.battery, `${fix.symptom}: ${step.title} stages nothing`);
        continue;
      }
      const { bench } = rig(benchProfile("canary-display-dash"), BenchPower);
      bench.setBattery(false);
      let fits = false;
      const real = bench.setBattery.bind(bench);
      bench.setBattery = (f) => { if (f) fits = true; real(f); };
      await step.stage(ctxFor(bench));
      assert.strictEqual(!!step.battery, fits, `${fix.symptom}: ${step.title} ${fits ? "fits a battery unmarked" : "is marked but fits none"}`);
    }
  }
  assert.ok(BENCH_FIXES.some((f) => f.steps.some((st) => st.battery)), "some step stages a battery");
  // a board with a battery is offered every step that stages one, as written
  const dashSteps = benchFixesFor(benchProfile("canary-display-dash")).flatMap((f) => f.steps);
  for (const f of BENCH_FIXES) {
    for (const st of f.steps) if (st.battery) assert.ok(dashSteps.includes(st), `a board with a battery is offered ${f.symptom}: ${st.title}`);
  }
  for (const id of ["canary-display-nightstand-s3", "canary-nightlight"]) {
    const offered = benchFixesFor(benchProfile(id));
    assert.ok(offered.length >= 3, `${id}: still a curriculum`);
    assert.ok(offered.every((f) => f.steps.length && f.steps.every((st) => !st.battery)), `${id}: no battery step offered`);
    assert.ok(!offered.some((f) => /unplugged the cable/.test(f.symptom)), `${id}: no ride-through flow`);
    const { bench, events } = rig(benchProfile(id), BenchPower);
    for (const f of offered) for (const st of f.steps) if (st.stage) await st.stage(ctxFor(bench));
    assert.ok(!events.some(([k, l]) => k === "log" && /no battery path/.test(l)), `${id}: no offered step asks for a battery`);
  }
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.match(app, /fixView\(guideCtx, benchFixesFor\(profile\), noteLine\)/, "the bench view offers the board's own flows");
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
  const s3 = benchProfile("canary-display-dash").rom;
  assert.match(romBanner("poweron", s3), /^ESP-ROM:esp32s3-20210327\n/);
  assert.match(romBanner("poweron", s3), /rst:0x1 \(POWERON\),boot:0x8 \(SPI_FAST_FLASH_BOOT\)/);
  assert.match(romBanner("swreset", s3), /rst:0x3 \(RTC_SW_SYS_RST\)/);
  assert.match(romBanner("download", s3), /boot:0x0 \(DOWNLOAD\(USB\/UART0\)\)/);
  assert.match(romBanner("download", s3), /waiting for download/);
  assert.ok(!/waiting for download/.test(romBanner("reset", s3)),
    "only a BOOT-low reset waits for a flasher");
});

// Sweep A59: romBanner() printed the ESP32-S3's banner and strap story for
// every card, so the Nightlight's twin (an ESP32-C3, BOOT on GPIO9) printed
// an S3 banner at power-on, reset and download mode. Each bench block now
// names its chip and BOOT strap (`rom`), the banner is that chip's, every
// banner line is a quote of the Espressif source bench.js cites, and a kind
// no source quotes prints a note saying so, never another chip's banner.
const CHIP_IDS = { "ESP32-S3": "esp32s3", "ESP32-C3": "esp32c3" };
// The cited sources' lines, as fetched for A59 (esp-idf tag v5.4), each with
// its file's four-space code indent stripped: what bench.js may print.
const QUOTED = {
  esp32s3: [ // docs/en/security/esp32s3_log.inc, lines 6-8 and 70-72
    "ESP-ROM:esp32s3-20210327", "Build:Mar 27 2021",
    "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)", "rst:0x3 (RTC_SW_SYS_RST),boot:0x8 (SPI_FAST_FLASH_BOOT)",
  ],
  esp32c3: [ // examples/security/security_features_app/README.md, lines 328-330
    "ESP-ROM:esp32c3-api1-20210207", "Build:Feb  7 2021", "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)",
  ],
};
// The S3's download lines predate A59 and are not in the fetched source; they
// are named here so a new unquoted line cannot hide among them.
const S3_UNSOURCED = ["rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))", "waiting for download"];

test("A59: every bench block names its chip's ROM and BOOT strap, and they are the board's", () => {
  const REPO = join(ROOT, "..");
  const manifests = new Map();
  for (const slug of require("node:fs").readdirSync(join(REPO, "devices"))) {
    let m;
    try { m = JSON.parse(readFileSync(join(REPO, "devices", slug, "device.json"), "utf8")); } catch { continue; }
    if (m.lab && m.lab.card) manifests.set(m.lab.card, m);
  }
  let benches = 0;
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    benches += 1;
    const rom = dev.bench.rom;
    assert.ok(rom && rom.chip && rom.boot_strap, `${dev.id}: the bench block names no rom { chip, boot_strap }`);
    // the chip: the one the registry's board names, and the claiming manifest's mcu
    const named = /ESP32-[A-Z]\d+/.exec(dev.board);
    assert.ok(named, `${dev.id}: the board "${dev.board}" names no ESP32 chip`);
    assert.strictEqual(rom.chip, CHIP_IDS[named[0]], `${dev.id}: rom.chip ${rom.chip} is not the board's ${named[0]}`);
    const m = manifests.get(dev.id);
    if (m) assert.strictEqual(rom.chip, CHIP_IDS[m.board.mcu], `${dev.id}: rom.chip is not its manifest's mcu ${m.board.mcu}`);
    // the strap: the pin the bench's own BOOT button note names
    const boot = dev.bench.buttons.find((b) => b.id === "boot");
    const pin = /^(GPIO\d+) strapping pin/.exec(boot.note);
    assert.ok(pin, `${dev.id}: the BOOT button's note names no strapping pin`);
    assert.strictEqual(rom.boot_strap, pin[1], `${dev.id}: rom.boot_strap ${rom.boot_strap} is not the BOOT button's ${pin[1]}`);
  }
  assert.strictEqual(benches, 7, "the bench blocks this test reads (six S3 boards, the C3 Nightlight)");
  // the Nightlight: an ESP32-C3 with BOOT on GPIO9, as its cited files say
  assert.deepStrictEqual(benchProfile("canary-nightlight").rom, { chip: "esp32c3", boot_strap: "GPIO9" });
  assert.ok(benchProfile("canary-nightlight").sources.some((x) => x.file === "devices/canary-display-nightlight-c3/device.json"),
    "the Nightlight's bench cites where its chip comes from");
});

test("A59: every bench block's ROM banners are its own chip's, quoted, and the C3's never an S3's", async () => {
  const { romBanner, ROM_VOICES } = await importBench();
  const KINDS = ["poweron", "reset", "swreset", "download"];
  assert.deepStrictEqual(Object.keys(ROM_VOICES).sort(), ["esp32c3", "esp32s3"]);
  // the voices say which kinds are not quoted (the ribbon reads it): only the
  // S3's download lines, and they are exactly the two named above
  assert.deepStrictEqual(ROM_VOICES.esp32s3.unsourced, ["download"]);
  assert.deepStrictEqual(ROM_VOICES.esp32c3.unsourced, []);
  assert.deepStrictEqual(ROM_VOICES.esp32s3.download.split("\n").filter(Boolean), S3_UNSOURCED);
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    const rom = dev.bench.rom;
    const others = Object.keys(ROM_VOICES).filter((c) => c !== rom.chip);
    for (const kind of KINDS) {
      const out = romBanner(kind, rom);
      assert.ok(out.length, `${dev.id}/${kind}: printed nothing`);
      for (const c of others) assert.ok(!out.includes(c), `${dev.id}/${kind}: prints ${c} on a ${rom.chip} board: ${JSON.stringify(out)}`);
      if (out.startsWith("ESP-ROM:")) {
        assert.ok(out.startsWith(`ESP-ROM:${rom.chip}`), `${dev.id}/${kind}: the banner is not ${rom.chip}'s`);
        // every line it prints is a quote of the cited source (the S3's two
        // download lines excepted, named above)
        for (const line of out.split("\n").filter(Boolean)) {
          const unsourced = (ROM_VOICES[rom.chip].unsourced || []).includes(kind);
          const ok = QUOTED[rom.chip].includes(line) || (unsourced && rom.chip === "esp32s3" && S3_UNSOURCED.includes(line));
          assert.ok(ok, `${dev.id}/${kind}: ${JSON.stringify(line)} is no line of the source bench.js cites for ${rom.chip}`);
        }
      } else {
        // no source quotes this one: a note in parentheses, no banner line
        assert.match(out, /^\(.*\)\n$/, `${dev.id}/${kind}: an unquoted banner must be a one-line note`);
        assert.ok(!/ESP-ROM:|rst:0x|boot:0x/.test(out), `${dev.id}/${kind}: a note must not pass for a banner`);
      }
    }
  }
  const c3 = benchProfile("canary-nightlight").rom;
  // the C3's quoted power-on banner, byte for byte, at power-on and RESET
  const c3On = "ESP-ROM:esp32c3-api1-20210207\nBuild:Feb  7 2021\nrst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\n";
  assert.strictEqual(romBanner("poweron", c3), c3On);
  assert.strictEqual(romBanner("reset", c3), c3On);
  // download mode: the C3's own strap story, and no banner nobody quoted
  assert.match(romBanner("download", c3), /^\(ESP32-C3 ROM: BOOT \(GPIO9\) was low at reset, so it waits for a flasher\./);
  assert.match(romBanner("swreset", c3), /^\(ESP32-C3 ROM: its banner for this reset is not reproduced/);
  for (const kind of KINDS) assert.ok(!/esp32s3|GPIO0/i.test(romBanner(kind, c3)), `the C3's ${kind} speaks of the S3`);
  // no chip named, no banner: never a default chip's
  for (const kind of KINDS) {
    assert.strictEqual(romBanner(kind), "", `romBanner(${kind}) with no rom must print nothing, not an S3 banner`);
    assert.strictEqual(romBanner(kind, {}), "");
    assert.ok(!/ESP-ROM:/.test(romBanner(kind, { chip: "esp32c6" })), "a chip with no quoted voice prints no banner");
  }
});

// Each call's argument text, parentheses balanced (a ternary or a nested call
// inside the arguments stays whole).
function callArgs(src, fn) {
  const out = [];
  for (let at = src.indexOf(`${fn}(`); at >= 0; at = src.indexOf(`${fn}(`, at + 1)) {
    if (/[\w.]/.test(src[at - 1] || "")) continue; // another name ending in fn
    let depth = 0, k = at + fn.length;
    for (; k < src.length; k++) {
      if (src[k] === "(") depth++;
      else if (src[k] === ")" && --depth === 0) break;
    }
    out.push(src.slice(at + fn.length + 1, k));
  }
  return out;
}

// Sweep A59 review: the bench's honesty ribbon was one fixed sentence on every
// card ("its text is verbatim ESP32-S3", "The PWR/CHG/DONE lights are
// hardwired"), so the Nightlight's ESP32-C3 card, with no lights, said the
// S3 story beside a C3 banner, and the S3 cards called their unsourced
// download lines verbatim. benchRibbon() now says it from the bench block.
test("A59: the bench's honesty ribbon names each board's own chip, sources and lights", async () => {
  const { benchRibbon, ROM_VOICES } = await importBench();
  assert.strictEqual(typeof benchRibbon, "function", "bench.js builds the ribbon per board");
  const HARDWIRED = new Set(["rail", "charger"]);
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    const r = benchRibbon(dev.bench);
    const voice = ROM_VOICES[dev.bench.rom.chip];
    assert.ok(r.startsWith("Honesty: "), `${dev.id}: ${r}`);
    assert.ok(r.includes(`this board's ${voice.name}:`), `${dev.id}: the ribbon does not name its ${voice.name}: ${r}`);
    for (const [chip, v] of Object.entries(ROM_VOICES)) {
      if (chip === dev.bench.rom.chip) continue;
      assert.ok(!r.includes(v.name) && !r.includes(chip), `${dev.id}: the ribbon names ${v.name} on a ${voice.name} board`);
    }
    assert.ok(!/verbatim/i.test(r), `${dev.id}: the ribbon calls the staged banner verbatim`);
    assert.strictEqual(/the bench's own, not checked against an Espressif source/.test(r), (voice.unsourced || []).length > 0,
      `${dev.id}: the ribbon and the voice disagree on unquoted lines`);
    assert.strictEqual(/prints a note instead/.test(r), ["poweron", "swreset", "download"].some((k) => !voice[k]),
      `${dev.id}: the ribbon and the voice disagree on where the bench prints a note`);
    // the lights: exactly the board's hardwired ones (a gpio LED, the watch's
    // USER, is the firmware's to drive and is not named)
    const leds = dev.bench.leds || [];
    for (const led of leds)
      assert.strictEqual(new RegExp(`\\b${led.label}\\b`).test(r), HARDWIRED.has(led.driver),
        `${dev.id}: the ribbon ${HARDWIRED.has(led.driver) ? "omits" : "names"} ${led.label} (${led.driver})`);
    for (const label of ["PWR", "CHG", "DONE", "USER"])
      if (!leds.some((l) => l.label === label)) assert.ok(!new RegExp(`\\b${label}\\b`).test(r), `${dev.id}: the ribbon names a ${label} light the board has not`);
    assert.strictEqual(/hardwired/.test(r), leds.some((l) => HARDWIRED.has(l.driver)), `${dev.id}: ${r}`);
  }
  // the Nightlight's card, whole: the C3's story, no S3 and no lights
  assert.strictEqual(benchRibbon(benchProfile("canary-nightlight")),
    "Honesty: the mask-ROM banner is staged by the bench for this board's ESP32-C3: its power-on banner is " +
    "quoted from Espressif's esp-idf (cited in bench.js); no Espressif source the Lab quotes prints its restart " +
    "or download-mode banner, so there the bench prints a note instead. Everything after the banner is the real firmware.");
  // an S3 card with all three lights: its download lines are not called quoted
  assert.strictEqual(benchRibbon(benchProfile("canary-display-dash")),
    "Honesty: the mask-ROM banner is staged by the bench for this board's ESP32-S3: its power-on and restart " +
    "banners are quoted from Espressif's esp-idf (cited in bench.js); its download-mode lines are the bench's own, " +
    "not checked against an Espressif source. Everything after the banner is the real firmware. The PWR, CHG and " +
    "DONE lights are hardwired to the power rail and the charge chip — no firmware here or on your desk can turn them off.");
  assert.match(benchRibbon(benchProfile("canary-display-watch")), / The CHG light is hardwired to the charge chip — no firmware here or on your desk can turn it off\.$/);
  // no chip named, none claimed
  assert.ok(!/ESP32/.test(benchRibbon({})) && !/ESP32/.test(benchRibbon(undefined)));
  assert.ok(!/ESP32-S3/.test(benchRibbon({ rom: { chip: "esp32c6" } })));
  // app.js draws the ribbon from the board's bench block and names no chip itself
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.match(app, /el\("p", "bench-ribbon", benchRibbon\(profile\)\)/, "app.js builds the bench ribbon per board");
  assert.ok(!/ESP32|esp32|verbatim/.test(app), "app.js names a chip, or calls a banner verbatim, itself");
});

test("A59: the pages stage each board's own ROM, and the bench and its flows name its BOOT strap", async () => {
  const { BenchPower } = await importBench();
  const { BENCH_FIXES, benchFixesFor } = await importGuides();
  // app.js: every banner it stages is the sheet's board's
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  assert.match(app, /async function buildDisplaySheet\(ctx, side, stage\) \{\n  const dev = ctx\.dev;\n[\s\S]{0,400}?\n  const rom = dev\.bench\?\.rom;\n/,
    "the display sheet reads its board's rom off the bench block");
  const calls = callArgs(app, "romBanner");
  assert.ok(calls.length >= 6, "app.js stages the ROM banners");
  for (const c of calls) assert.match(c, /^"(?:poweron|reset|swreset|download)", rom$/, `app.js calls romBanner(${c}) without the board's rom`);
  // the WAP page: an ESP32-S3 board, as its flash product says
  const wap = readFileSync(join(ROOT, "assets/wap-ui.js"), "utf8");
  assert.match(wap, /const WAP_ROM = \{ chip: "esp32s3", boot_strap: "GPIO0" \};/);
  const flash = JSON.parse(readFileSync(join(ROOT, "devices/flash.json"), "utf8"));
  assert.strictEqual(flash.products.find((p) => p.id === "securacv-canary-wap").chip, "ESP32-S3", "the WAP's chip moved: WAP_ROM must follow it");
  const wapCalls = callArgs(wap, "romBanner");
  assert.ok(wapCalls.length >= 2 && wapCalls.every((c) => /, WAP_ROM$/.test(c)), `wap-ui.js stages a banner without WAP_ROM: ${wapCalls.join(" | ")}`);
  // BenchPower: BOOT held through RESET names the board's strap
  for (const [id, pin] of [["canary-nightlight", "GPIO9"], ["canary-display-dash", "GPIO0"]]) {
    const { bench, events } = rig(benchProfile(id), BenchPower);
    bench.setBootHeld(true);
    bench.pressReset();
    assert.ok(events.some(([k, l]) => k === "log" && l === `RESET with BOOT (${pin}) low — the ROM boots to download mode instead of the app.`),
      `${id}: the download narration names ${pin}`);
  }
  // the troubleshooter: a step that names a GPIO as the BOOT strap is flagged
  // so, and each board's flows name only its own strap
  for (const f of BENCH_FIXES) {
    for (const st of f.steps) {
      const text = [st.title, st.body, st.onDevice].join(" ");
      const strapPin = /BOOT(?: is | \()GPIO\d+/.test(text);
      assert.strictEqual(!!st.strap, strapPin, `${f.symptom}: ${st.title} ${strapPin ? "names the BOOT strap unflagged" : "is flagged but names no strap"}`);
    }
  }
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    const words = JSON.stringify(benchFixesFor(dev.bench).map((f) => f.steps.filter((st) => st.strap).map((st) => [st.title, st.body, st.onDevice])));
    const pins = new Set(words.match(/GPIO\d+/g) || []);
    assert.deepStrictEqual([...pins], [dev.bench.rom.boot_strap], `${dev.id}: its flows name ${[...pins].join(", ")} as BOOT`);
  }
  // an S3 board with a battery is offered every flow, each step as written
  // (the BOOT steps are the board's own story, sweep A83, below)
  const dash = benchFixesFor(benchProfile("canary-display-dash"));
  assert.deepStrictEqual(dash.map((f) => f.symptom), BENCH_FIXES.map((f) => f.symptom), "an S3 board with a battery is offered every flow");
  for (const f of dash) for (const st of f.steps) assert.ok(BENCH_FIXES.some((g) => g.steps.includes(st)), `${f.symptom}: ${st.title} is offered as written`);
});

// Sweep A83: the bench told the S3 boards' story on every card. setBootHeld()
// logged "Running firmware ignores it" and the "Pressing BOOT does nothing"
// flow said the display never reads BOOT while it runs, on boards whose
// firmware does (main.cpp's BOOT grammar runs wherever the board's pins.h
// names a BOOT_BUTTON_PIN: the watch, the 1.47" stick, the Touch 1.69, the
// AMOLED 2.41 and the Nightlight; the 4.3" and 7" dashes name none, GPIO0
// being an RGB data line there), and three flows quoted ESP32-S3 ROM lines the
// Nightlight's ESP32-C3 card never prints. Each bench block's BOOT button now
// says whether the running firmware reads it (`read_while_running`), held
// here to its note's words and to the firmware; setBootHeld() narrates from
// it; and the flows read the board's ROM voice for the lines they quote.
const ROM_TOKENS = ["waiting for download", "RTC_SW_SYS_RST", "POWERON", "SPI_FAST_FLASH_BOOT", "DOWNLOAD(USB/UART0)", "ESP-ROM"];
function envFor(dev) {
  // the PlatformIO env a card's firmware is: its claiming manifest's, else
  // the env named after the card
  for (const slug of require("node:fs").readdirSync(join(ROOT, "..", "devices"))) {
    let m;
    try { m = JSON.parse(readFileSync(join(ROOT, "..", "devices", slug, "device.json"), "utf8")); } catch { continue; }
    if (m.lab?.card === dev.id) return m.board.envs[0];
  }
  return dev.id;
}
function envBlock(ini, env) {
  const at = ini.indexOf(`\n[env:${env}]\n`);
  assert.ok(at >= 0, `canary-display.ini has no [env:${env}]`);
  const rest = ini.slice(at + 1);
  const end = rest.slice(1).search(/\n\[/);
  return end < 0 ? rest : rest.slice(0, end + 1);
}

test("A83: each bench block's BOOT button says whether the running firmware reads it, as its note and the firmware do", () => {
  const REPO = join(ROOT, "..");
  const ini = readFileSync(join(REPO, "firmware/envs/platformio/canary-display.ini"), "utf8");
  const main = readFileSync(join(REPO, "firmware/projects/canary-display/src/main.cpp"), "utf8");
  // main.cpp reads BOOT while it runs wherever the pin map names one
  assert.match(main, /#if defined\(BOOT_BUTTON_PIN\) && \(BOOT_BUTTON_PIN >= 0\)\n  \/\/ BOOT-button grammar[\s\S]*?const bool pressed = digitalRead\(BOOT_BUTTON_PIN\) == BOOT_BUTTON_ACTIVE;/,
    "main.cpp's loop reads BOOT on every board whose pins.h names a BOOT_BUTTON_PIN");
  const seen = { read: [], ignored: [] };
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    const boot = dev.bench.buttons.find((b) => b.id === "boot");
    assert.strictEqual(typeof boot.read_while_running, "boolean", `${dev.id}: the BOOT button says whether the running firmware reads it`);
    // the note's words agree with the flag
    if (boot.read_while_running) {
      assert.match(boot.note, /, sampled at reset — this firmware also reads it while running: /, `${dev.id}: the note says the firmware reads it`);
    } else {
      assert.match(boot.note, /, sampled only at reset — this firmware never reads it while running$/, `${dev.id}: the note says it is never read`);
    }
    // ...and the firmware: the env's pin map names a BOOT_BUTTON_PIN or not
    const block = envBlock(ini, envFor(dev));
    const pinsDir = /boards\/([a-z0-9-]+)\/pins/.exec(block)?.[1];
    const cfgDir = /configs\/canary-display\/([a-z0-9-]+)/.exec(block)?.[1];
    assert.ok(pinsDir && cfgDir, `${dev.id}: its env names a pin map and a config`);
    const pins = readFileSync(join(REPO, "firmware/boards", pinsDir, "pins/pins.h"), "utf8");
    const pin = Number(/^#define BOOT_BUTTON_PIN\s+\(?(-?\d+)\)?/m.exec(pins)?.[1]);
    assert.ok(Number.isInteger(pin), `${dev.id}: ${pinsDir}'s pins.h names its BOOT_BUTTON_PIN`);
    assert.strictEqual(boot.read_while_running, pin >= 0,
      `${dev.id}: read_while_running is ${boot.read_while_running}, but ${pinsDir}'s BOOT_BUTTON_PIN is ${pin}`);
    // the presses it lists are the ones this firmware answers
    const cfg = readFileSync(join(REPO, "firmware/configs/canary-display", cfgDir, "config.h"), "utf8");
    if (boot.read_while_running) {
      assert.strictEqual(/a double press/.test(boot.note), /^#define FEATURE_LANTERN\s+1\b/m.test(cfg),
        `${dev.id}: the note's double press (the lamp) and ${cfgDir}'s FEATURE_LANTERN disagree`);
      assert.strictEqual(/a triple press/.test(boot.note), /^#define CD_NIGHTLIGHT\s+1\b/m.test(cfg),
        `${dev.id}: the note's triple press (the turn) and ${cfgDir}'s CD_NIGHTLIGHT disagree`);
      assert.match(boot.note, /a press wakes the glass/);
      assert.match(boot.note, /a hold acknowledges/);
    }
    seen[boot.read_while_running ? "read" : "ignored"].push(dev.id);
  }
  assert.deepStrictEqual(seen, {
    read: ["canary-display-watch", "canary-display-nightstand-s3", "canary-display-touch169", "canary-display-amoled241", "canary-nightlight"],
    ignored: ["canary-display-dash", "canary-display-dash7"],
  });
});

test("A83: holding BOOT narrates each board's own story, read off its bench block", async () => {
  const { BenchPower, bootHeldLine } = await importBench();
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    const { bench, events } = rig(dev.bench, BenchPower);
    bench.setBootHeld(true);
    const lines = events.filter(([k]) => k === "log").map(([, l]) => l);
    assert.deepStrictEqual(lines, [bootHeldLine(dev.bench)], `${dev.id}: one line, the board's own`);
    const [line] = lines;
    const reads = dev.bench.buttons.find((b) => b.id === "boot").read_while_running;
    assert.ok(line.startsWith(`BOOT (${dev.bench.rom.boot_strap}) held — a strapping pin`), `${dev.id}: names its strap: ${line}`);
    assert.strictEqual(/Running firmware ignores it\./.test(line), !reads, `${dev.id}: ${line}`);
    assert.strictEqual(/This board's firmware also reads it while running/.test(line), reads, `${dev.id}: ${line}`);
    if (reads) assert.match(line, /not wired to the twin's button, so the firmware sees no press\.$/, "and says the bench does not press it");
  }
  // a block that does not say gets neither story
  const unsaid = bootHeldLine({ rom: { boot_strap: "GPIO0" }, buttons: [{ id: "boot", label: "BOOT", note: "x" }] });
  assert.ok(!/ignores it|also reads it/.test(unsaid) && /does not say whether its running firmware reads it/.test(unsaid), unsaid);
  // held in download mode or off, the bench says nothing of the running firmware
  const { bench, events } = rig(benchProfile("canary-nightlight"), BenchPower);
  bench.setUsb(false);
  events.length = 0;
  bench.setBootHeld(true);
  assert.ok(!events.some(([k]) => k === "log"), "no running firmware, no story");
});

test("A83: the troubleshooter quotes only the ROM lines the board's chip prints, and tells its own BOOT story", async () => {
  const { BENCH_FIXES, benchFixesFor } = await importGuides();
  const { ROM_VOICES } = await importBench();
  const voiceText = (chip) => Object.entries(ROM_VOICES[chip] || {})
    .filter(([k, v]) => k !== "name" && typeof v === "string").map(([, v]) => v).join("\n");
  for (const t of ROM_TOKENS) assert.ok(voiceText("esp32s3").includes(t), `${t} is a line the S3's voice prints`);
  const textOf = (f, st) => [f.symptom, st.title, st.body, st.onDevice].filter(Boolean).join(" ");
  // the tags are honest: a step or flow that quotes a ROM line says which,
  // and what it says it quotes is in its text
  for (const f of BENCH_FIXES) {
    for (const q of f.rom || []) assert.ok(f.symptom.includes(q.quote), `${f.symptom} is tagged with ${q.quote} it does not say`);
    for (const st of f.steps) {
      const own = [st.title, st.body, st.onDevice].filter(Boolean).join(" ");
      const tagged = [...(f.rom || []), ...(st.rom || [])].map((q) => q.quote);
      for (const t of ROM_TOKENS) {
        if (own.includes(t)) assert.ok(tagged.includes(t), `${f.symptom}: ${st.title} quotes ${t} untagged`);
      }
      for (const q of st.rom || []) {
        assert.ok(own.includes(q.quote), `${f.symptom}: ${st.title} is tagged with ${q.quote} it does not say`);
        assert.ok((ROM_VOICES.esp32s3[q.kind] || "").includes(q.quote), `${q.quote} is the S3's ${q.kind} line`);
      }
      assert.ok(st.boot === undefined || st.boot === "read" || st.boot === "ignored", `${st.title}: boot is read or ignored`);
    }
  }
  // per board: no offered text quotes a ROM line its chip does not print,
  // and the BOOT steps are its own
  const offeredOn = {};
  for (const dev of registry().devices) {
    if (!dev.bench) continue;
    const voice = voiceText(dev.bench.rom.chip);
    const reads = dev.bench.buttons.find((b) => b.id === "boot").read_while_running;
    const offered = benchFixesFor(dev.bench);
    for (const f of offered) {
      for (const st of f.steps) {
        for (const t of ROM_TOKENS) {
          assert.ok(!textOf(f, st).includes(t) || voice.includes(t), `${dev.id}: "${f.symptom}" / ${st.title} quotes ${t}, which its ${dev.bench.rom.chip} never prints`);
        }
        if (st.boot) assert.strictEqual(st.boot === "read", reads, `${dev.id}: ${st.title} tells the other boards' BOOT story`);
      }
    }
    const boot = offered.find((f) => f.symptom === "Pressing BOOT does nothing");
    assert.ok(boot && boot.steps.length === 2, `${dev.id}: the BOOT flow is offered, its own story and the release`);
    assert.strictEqual(/never reads it/.test(JSON.stringify(boot.steps.map((st) => st.body))), !reads, `${dev.id}: the BOOT flow says never-read only where it is true`);
    offeredOn[dev.id] = offered.map((f) => `${f.symptom} (${f.steps.length})`);
  }
  // the Nightlight's C3: no download banner and no restart banner to quote,
  // so the download flow and both quoting steps are left out; the restore
  // step is not
  const nl = offeredOn["canary-nightlight"];
  assert.ok(!nl.some((s) => s.startsWith("Serial says 'waiting for download'")), "the C3 card offers no flow for a banner it never prints");
  assert.ok(nl.includes("It reboots by itself (2)"), `the C3 card's reboot flow keeps its countdown and its restore: ${nl}`);
  assert.ok(nl.includes("Screen dark, but the power light is on (1)"), `...and its screen-dark flow drops the download guess: ${nl}`);
  const s3 = offeredOn["canary-display-watch"];
  assert.ok(s3.includes("Serial says 'waiting for download' (2)") && s3.includes("It reboots by itself (3)") &&
    s3.includes("Screen dark, but the power light is on (2)"), `the S3 cards keep every ROM step: ${s3}`);
  // a block that says nothing of BOOT is offered neither BOOT story
  const bare = { ...benchProfile("canary-display-dash"), buttons: [{ id: "boot", label: "BOOT", note: "GPIO0 strapping pin" }, { id: "reset", label: "RESET" }] };
  assert.deepStrictEqual(benchFixesFor(bare).find((f) => f.symptom === "Pressing BOOT does nothing").steps.map((st) => st.title),
    ["Release it before the next reset"]);
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
