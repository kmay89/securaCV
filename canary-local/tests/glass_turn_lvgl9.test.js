// canary-local/tests/glass_turn_lvgl9.test.js — the real LVGL 9.5 turned
// glass (F225), wired where it cannot be skipped and pinned to the release
// the dash builds ship.
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
const { readFileSync } = require("node:fs");
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

test("CI (F225): the LVGL 9.5 turned glass runs after its own cache, unconditionally, before anything reads the dist", () => {
  const wf = read(join(REPO, ".github/workflows/canary-local.yml"));
  const at = wf.indexOf("\n  wasm-build-and-boot:");
  assert.ok(at > 0, "the wasm job");
  const job = wf.slice(at);
  const run = job.indexOf(`run: bash ${SCRIPT}\n`);
  assert.ok(run > 0, "the wasm job runs glass_turn_lvgl9.sh");
  // Its own cache: the script's default checkout, keyed on the file that
  // names the release, apart from build.sh's third_party cache.
  const cache = /- name: Cache LVGL 9[^\n]*\n\s+uses: actions\/cache@v\d+\n\s+with:\n\s+path: (\S+)\n\s+key: ([^\n]+)\n/.exec(job);
  assert.ok(cache, "a cache step for the LVGL 9 checkout");
  assert.ok(job.indexOf(cache[0]) < run, "restored before the script runs");
  assert.strictEqual(cache[1], "canary-local/emulator/test/third_party",
    "the cached path is the script's default checkout's parent");
  assert.ok(read(join(ROOT, "emulator/test/glass_turn_lvgl9.sh")).includes('DEFAULT="$HERE/third_party/lvgl"'));
  assert.ok(cache[2].includes("hashFiles('firmware/projects/canary-display/arduino/canary_display/sketch.yaml')"),
    "keyed on sketch.yaml, where the pin lives");
  const tp = /- name: Cache third-party sources\n\s+uses: actions\/cache@v\d+\n\s+with:\n\s+path: (\S+)\n\s+key: ([^\n]+)\n/.exec(job);
  assert.ok(tp && tp[1] !== cache[1] && !cache[1].startsWith(`${tp[1]}/`) && tp[2] !== cache[2],
    "a path and key of its own, not inside build.sh's cached third_party");
  // Beside glass_turn.sh, before every step that reads the committed dist
  // (a stale dist turns those red and GitHub skips each later step).
  const gt = job.indexOf("run: bash canary-local/emulator/test/glass_turn.sh\n");
  assert.ok(gt > 0 && gt < run, "after glass_turn.sh");
  for (const later of ["node canary-local/tests/boot_probe.mjs", "node canary-local/tests/onboard_probe.mjs",
    "node canary-local/tests/csp_probe.mjs", "./build.sh all", "Dist drift check"]) {
    assert.ok(job.indexOf(later) > run, `glass_turn_lvgl9.sh runs before ${later}`);
  }
  const step = /- name: Turned glass — the real LVGL[\s\S]*?run: bash canary-local\/emulator\/test\/glass_turn_lvgl9\.sh\n/.exec(job)?.[0] || "";
  assert.ok(step && !/^\s+if:/m.test(step), "the step runs unconditionally");
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

test("check_lvgl9_quotes.py (F225): fails on each drift from the library and on a missing function", () => {
  const r = spawnSync("python3", [join(ROOT, "emulator/test/check_lvgl9_quotes.py"), "--self-test"], { encoding: "utf8" });
  assert.strictEqual(r.status, 0, `check_lvgl9_quotes.py --self-test failed:\n${r.stdout}${r.stderr}`);
  assert.match(r.stdout, /the fake's quotes pass, \d+ drifts and a missing function fail/);
  const bad = spawnSync("python3", [join(ROOT, "emulator/test/check_lvgl9_quotes.py"), join(ROOT, "emulator/test/no-such-lvgl")],
    { encoding: "utf8" });
  assert.strictEqual(bad.status, 2, "no checkout is a hard stop, not a pass");
});
