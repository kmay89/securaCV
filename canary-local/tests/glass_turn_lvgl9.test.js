// canary-local/tests/glass_turn_lvgl9.test.js — the real LVGL 9.5 turned
// glass (F225), wired where no other step's red can skip it and pinned to
// the release the dash builds ship.
//
// emulator/test/glass_turn_lvgl9.sh builds the real ui/lvgl_port.cpp (dash
// config, LVGL 9 branch) against the LVGL 9.x release sketch.yaml's
// dash-core3 profile pins and renders a scene through it at every quarter
// turn; check_lvgl9_quotes.py holds tests_host/fake_lvgl9's quotes of that
// release to the fetched source. Neither needs the emulator's dist, and both
// need a network fetch and a compiler, so they run in canary-local.yml's
// wasm job; this file holds what the logic job can hold without them: the
// step's place and cache, the pin the script reads, the release the fake
// says it quotes, and the quote checker failing on drift (its --self-test
// builds a stand-in checkout from the fake's own quotes; no LVGL needed).
const { test } = require("node:test");
const assert = require("node:assert");
const { existsSync, readFileSync } = require("node:fs");
const { join } = require("node:path");
const { spawnSync } = require("node:child_process");

const ROOT = join(__dirname, "..");      // canary-local/
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");
const SCRIPT = "canary-local/emulator/test/glass_turn_lvgl9.sh";

// sketch.yaml's pin: the lvgl release one profile names.
function profileLvgl(sketch, profile) {
  const body = new RegExp(`\\n  ${profile}:\\n([\\s\\S]*?)(?=\\n  [A-Za-z0-9_-]+:\\n|$)`).exec(sketch)?.[1] || "";
  return /^\s*- lvgl \(([0-9.]+)\)\s*$/m.exec(body)?.[1] || null;
}

// The wasm job's steps, each from its "      - " line to the next.
function jobSteps(wf, job) {
  const at = wf.indexOf(`\n  ${job}:\n`);
  if (at < 0) return [];
  const rest = wf.slice(at + 1);
  const end = rest.slice(1).search(/\n {2}[A-Za-z0-9_-]+:\n/);
  const body = end < 0 ? rest : rest.slice(0, end + 1);
  return body.split(/\n(?= {6}- )/).slice(1).map((st) => `${st.replace(/\n+$/, "")}\n`);
}

test("CI (F225): the LVGL 9.5 turned glass closes the wasm job after its own cache, run whether the steps before passed or failed", () => {
  const wf = read(join(REPO, ".github/workflows/canary-local.yml"));
  const steps = jobSteps(wf, "wasm-build-and-boot");
  assert.ok(steps.length > 10, "the wasm job's steps");
  const runAt = steps.findIndex((st) => st.includes(`run: bash ${SCRIPT}\n`));
  assert.ok(runAt > 0, "the wasm job runs glass_turn_lvgl9.sh");
  const cacheAt = steps.findIndex((st) => /^ {6}- name: Cache LVGL 9/.test(st));
  assert.ok(cacheAt >= 0, "a cache step for the LVGL 9 checkout");
  // The job's last two steps, the cache restored first: no step follows,
  // so a red here (a failed cold-cache clone included) skips nothing.
  assert.strictEqual(runAt, steps.length - 1, "glass_turn_lvgl9.sh is the wasm job's last step");
  assert.strictEqual(cacheAt, steps.length - 2, "its cache is restored right before it");
  // Both run whether the steps before passed or failed (anything but a
  // canceled run): glass_turn.sh (the same lvgl_port.cpp against 8.4) or a
  // probe on a stale dist going red earlier cannot hide the LVGL 9.5
  // result. A step with no if: runs only when every step before it passed;
  // the vision/eyes/audio native steps spell the same condition this way.
  for (const at of [cacheAt, runAt]) {
    const ifs = steps[at].match(/^ {8}if: .*$/gm) || [];
    assert.deepStrictEqual(ifs, ["        if: ${{ success() || failure() }}"],
      `${at === runAt ? "the run step" : "the cache step"} runs after a red step too`);
  }
  const gtAt = steps.findIndex((st) => st.includes("run: bash canary-local/emulator/test/glass_turn.sh\n"));
  assert.ok(gtAt > 0 && gtAt < cacheAt, "after glass_turn.sh");
  // Its own cache: the script's default checkout, keyed on the file that
  // names the release, apart from build.sh's third_party cache.
  const cacheRe = /\n\s+uses: actions\/cache@v\d+\n\s+with:\n\s+path: (\S+)\n\s+key: ([^\n]+)\n/;
  const cache = cacheRe.exec(steps[cacheAt]);
  assert.ok(cache, "the LVGL 9 cache step restores a path under a key");
  assert.strictEqual(cache[1], "canary-local/emulator/test/third_party",
    "the cached path is the script's default checkout's parent");
  assert.ok(read(join(ROOT, "emulator/test/glass_turn_lvgl9.sh")).includes('DEFAULT="$HERE/third_party/lvgl"'));
  assert.ok(cache[2].includes("hashFiles('firmware/projects/canary-display/arduino/canary_display/sketch.yaml')"),
    "keyed on sketch.yaml, where the pin lives");
  const tpStep = steps.find((st) => /^ {6}- name: Cache third-party sources\n/.test(st)) || "";
  const tp = cacheRe.exec(tpStep);
  assert.ok(tp && tp[1] !== cache[1] && !cache[1].startsWith(`${tp[1]}/`) && tp[2] !== cache[2],
    "a path and key of its own, not inside build.sh's cached third_party");
  // The workflow runs on a change to any input: the port, the pin, the env,
  // the fake, the script.
  const prPaths = wf.slice(wf.indexOf("\n  pull_request:"), wf.indexOf("\njobs:"));
  for (const glob of ["firmware/projects/canary-display/**", "canary-local/**", "firmware/envs/**",
    "firmware/configs/**", "firmware/boards/waveshare-esp32s3-lcd43/**"]) {
    assert.ok(prPaths.includes(`- "${glob}"`), `pull_request paths include ${glob}`);
  }
});

test("the pin (F225): the script reads sketch.yaml's dash-core3 LVGL, fetches that tag, and builds the real port", () => {
  const sh = read(join(ROOT, "emulator/test/glass_turn_lvgl9.sh"));
  assert.ok(sh.includes('pin="$(profile_lvgl dash-core3)"') && sh.includes('tag="v$pin"'));
  assert.ok(sh.includes('git clone --depth 1 --branch "$tag" https://github.com/lvgl/lvgl.git "$DEFAULT"'),
    "fetched the way glass_turn.sh fetches build.sh's 8.4");
  assert.ok(sh.includes('python3 "$HERE/check_lvgl9_quotes.py" --self-test') &&
    sh.includes('python3 "$HERE/check_lvgl9_quotes.py" "$LVGL"'), "the quote checker runs, self-test first");
  assert.ok(sh.includes('"${CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp"'), "the REAL lvgl_port.cpp");
  assert.ok(sh.includes('-I "$PROJ/include"') && sh.includes('CFG="$FW/configs/canary-display/dash"'),
    "the display's lv_conf.h, the dash config");
  // The pin itself, and every place that must agree with it.
  const sketch = read(join(REPO, "firmware/projects/canary-display/arduino/canary_display/sketch.yaml"));
  const pin = profileLvgl(sketch, "dash-core3");
  assert.match(pin || "", /^9\.\d+\.\d+$/, "dash-core3 pins an LVGL 9 release");
  const nines = [...new Set([...sketch.matchAll(/^\s*- lvgl \((9\.[0-9.]+)\)\s*$/gm)].map((m) => m[1]))];
  assert.deepStrictEqual(nines, [pin], "every profile that names an LVGL 9 names the same one");
  const ini = read(join(REPO, "firmware/envs/platformio/canary-display.ini"));
  const dash = /\n\[env:canary-display-dash\]\n([\s\S]*?)(?=\n\[)/.exec(ini)?.[1] || "";
  assert.ok(dash.includes(`lvgl/lvgl@^${pin}`) && dash.includes("-DLV_CONF_INCLUDE_SIMPLE") &&
    dash.includes("-I${PROJECT_DIR}/include") && !dash.includes("CD_LEAN_BUILD"),
  "the PlatformIO dash env builds that release with include/lv_conf.h, not lean");
  // The fake quotes that release (a pin bump makes its quotes be re-read).
  const fake = read(join(REPO, "firmware/projects/canary-display/tests_host/fake_lvgl9/lvgl.h"));
  const fv = ["MAJOR", "MINOR", "PATCH"].map((p) => new RegExp(`#define LVGL_VERSION_${p} (\\d+)`).exec(fake)?.[1]).join(".");
  assert.strictEqual(fv, pin, "fake_lvgl9/lvgl.h quotes the pinned release");
  // The checkout is ignored by git and skipped by every tree walker (a
  // directory named third_party).
  assert.match(read(join(ROOT, "emulator/.gitignore")), /^third_party\/$/m);
});

test("nightstand-c6 (F248): the script builds the same test in the lean C6 config, against LVGL built lean, and every suite runs", () => {
  const sh = read(join(ROOT, "emulator/test/glass_turn_lvgl9.sh"));
  // The config its env compiles: the nightstand on the C6 pin map, lean.
  assert.ok(sh.includes('C6_CFG="$FW/configs/canary-display/nightstand"') &&
    sh.includes('C6_PINS="$FW/boards/waveshare-esp32c6-lcd147/pins"') &&
    /C6_DEFS=\(-DCD_LEAN_BUILD=1 /.test(sh) && !sh.includes("CONFIG_NIGHTSTAND"), "the nightstand-c6 config, lean");
  assert.ok(sh.includes('build_lvgl "$OUT/lvgl_lean" -DCD_LEAN_BUILD=1') &&
    sh.includes('"$OUT"/lvgl_lean/*.o'), "its LVGL objects are compiled with the lean lv_conf");
  assert.ok(sh.includes('"${C6_CXX[@]}" -c "$PROJ/src/ui/lvgl_port.cpp"') &&
    sh.includes('"${C6_CXX[@]}" -c "$HERE/glass_turn_lvgl9_test.cpp"'), "the REAL port and the same test");
  // The env still is that config (the script refuses to run otherwise).
  const ini = read(join(REPO, "firmware/envs/platformio/canary-display.ini"));
  const c6 = /\n\[env:canary-display-nightstand-c6\]\n([\s\S]*?)(?=\n\[)/.exec(ini)?.[1] || "";
  for (const flag of ["-DCD_LEAN_BUILD=1", "-DLV_CONF_INCLUDE_SIMPLE", "-I${PROJECT_DIR}/include",
    "-I${PROJECT_DIR}/../../configs/canary-display/nightstand", "-I${PROJECT_DIR}/../../boards/waveshare-esp32c6-lcd147/pins"]) {
    assert.ok(c6.split("\n").includes(`    ${flag}`), `the nightstand-c6 env carries ${flag}`);
    assert.ok(sh.includes(`'${flag}'`), `the script checks ${flag}`);
  }
  // A red suite does not stop the next: each runs, and the script fails at the end.
  const runs = [...sh.matchAll(/^run_suite "([^"]+)" "\$OUT\/([a-z0-9_]+)"$/gm)].map((m) => m[2]);
  assert.ok(runs.includes("glass_turn_lvgl9_test") && runs.includes("c6_glass_turn_lvgl9_test"), `suites run: ${runs}`);
  assert.ok(sh.includes('"$2" || failed+=("$1")') && /if \(\(\$\{#failed\[@\]\}\)\); then\n[^\n]*\n\s+exit 1\nfi\n?$/.test(sh),
    "every suite runs, and a failed one fails the script at its end");
  // The C6 board's pins reach the workflow's path filter.
  const wf = read(join(REPO, ".github/workflows/canary-local.yml"));
  const prPaths = wf.slice(wf.indexOf("\n  pull_request:"), wf.indexOf("\njobs:"));
  assert.ok(prPaths.includes('- "firmware/boards/*/pins/pins.h"'), "a C6 pin map edit runs the workflow");
});

test("the splash's tail (F246): the script links the REAL splash.cpp and canary_mark.cpp into a test it runs in both configs", () => {
  const sh = read(join(ROOT, "emulator/test/glass_turn_lvgl9.sh"));
  assert.ok(sh.includes("SPLASH_SRC=(splash canary_mark theme character)") &&
    sh.includes('"$@" -c "$PROJ/src/ui/$f.cpp"'), "the real splash.cpp and canary_mark.cpp, with the theme and Character they read");
  assert.ok(sh.includes('"$@" -c "$HERE/glass_splash_lvgl9_test.cpp"'), "the splash tail test");
  assert.ok(sh.includes('build_splash "" "$OUT/lvgl_port.o" "$OUT/lvgl" "${CXX[@]}"') &&
    sh.includes('build_splash c6_ "$OUT/c6_lvgl_port.o" "$OUT/lvgl_lean" "${C6_CXX[@]}"'),
  "built in the dash config and in nightstand-c6's, each with its own port and LVGL objects");
  const runs = [...sh.matchAll(/^run_suite "([^"]+)" "\$OUT\/([a-z0-9_]+)"$/gm)].map((m) => m[2]);
  assert.ok(runs.includes("glass_splash_lvgl9_test") && runs.includes("c6_glass_splash_lvgl9_test"), `suites run: ${runs}`);
  // The stand-ins it needs beside the port's, and the pseudonym's length held
  // to the real header before anything builds.
  for (const f of ["Preferences.h", "esp_random.h", "identity/device_pseudonym.h"]) {
    assert.ok(existsSync(join(ROOT, "emulator/test/native9", f)), `native9/${f}`);
  }
  assert.ok(sh.includes('PSEUDO="$FW/common/identity/device_pseudonym.h"'), "the pseudonym stand-in is held to the real header");
  const real = read(join(REPO, "firmware/common/identity/device_pseudonym.h"));
  assert.match(real, /^constexpr size_t TOKEN_BYTES = 8;/m);
  assert.match(read(join(ROOT, "emulator/test/native9/identity/device_pseudonym.h")), /constexpr size_t TOKEN_BYTES = 8;/);
  // The test reads the tail off the frame and holds it to the box and styles:
  // its own ink read with the bubble hidden too, so an F185 option filled with
  // the bubble's color is read whole, and every option held to the same checks.
  const t = read(join(ROOT, "emulator/test/glass_splash_lvgl9_test.cpp"));
  assert.ok(t.includes("canary::ui::splash_play(1700);") && t.includes("Ink tail_ink(lv_obj_t* tail, lv_obj_t* under") &&
    t.includes("Box styled_extent(const lv_obj_t* tail)"), "the splash played, its tail read off the glass and held to its styles");
  assert.ok(t.includes("r.own = tail_ink(tail, bub);") && t.includes("HOLD(alt.drawn() && alt.lands(), W.option_fails,"),
    "check 4 reads the tail's own ink with the bubble hidden; each of F185's options is held to checks 3 and 4");
  // The whole first meeting: the lines typed are the script's, derived from it.
  assert.ok(t.includes('#include "story/story_scripts.h"') && t.includes("const canary::story::Scene& sc = canary::story::kHello;") &&
    t.includes("CHECK(same == want.size() && W.typed.size() == want.size(),"),
  "every line of story::kHello typed whole, in order, counted from the script");
});

test("check_lvgl9_quotes.py (F225): fails on each drift from the library and on a missing function", () => {
  const r = spawnSync("python3", [join(ROOT, "emulator/test/check_lvgl9_quotes.py"), "--self-test"], { encoding: "utf8" });
  assert.strictEqual(r.status, 0, `check_lvgl9_quotes.py --self-test failed:\n${r.stdout}${r.stderr}`);
  assert.match(r.stdout, /the fake's quotes pass, \d+ drifts and a missing function fail/);
  const bad = spawnSync("python3", [join(ROOT, "emulator/test/check_lvgl9_quotes.py"), join(ROOT, "emulator/test/no-such-lvgl")],
    { encoding: "utf8" });
  assert.strictEqual(bad.status, 2, "no checkout is a hard stop, not a pass");
});
