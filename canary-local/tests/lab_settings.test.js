// Parity gate for the Lab's Settings panel (assets/lab-settings.js) — the
// app's "which build am I on / is there a newer one / what has it done"
// surface, matching the Flasher's About panel.
//
// Three ways this rots, all of them silent at runtime, all gated here:
//
//   1. The panel invokes a native command that isn't registered in the Lab's
//      invoke_handler. Tauri rejects the call at runtime with a string error
//      the panel shows as "—" or a red line, so the UI looks merely empty
//      rather than broken. Every `invoke("…")` below must be a command the
//      Lab actually exposes.
//   2. `app_info` reads env vars stamped by build.rs. Drop the stamping and
//      the crate fails to compile (env! is compile-time) — but drop the
//      *command* and the panel silently loses the build identity, so both
//      halves are asserted.
//   3. The panel leaks into the browser Lab, which has no updater and no app
//      data dir. Every native call must sit behind the IN_APP gate, and the
//      shell must only offer the sidebar entry in the app — otherwise the
//      website grows a Settings page that answers "—" to everything.
//
// Runs under "page logic tests" (.github/workflows/canary-local.yml). Reads
// source text only — no Rust toolchain, no browser.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const CANARY = join(__dirname, "..");
const ROOT = join(CANARY, "..");
const read = (p) => readFileSync(p, "utf8");

const settingsJs = read(join(CANARY, "assets/lab-settings.js"));
const shellJs = read(join(CANARY, "assets/lab-shell.js"));
const libRs = read(join(ROOT, "desktop-lab/src-tauri/src/lib.rs"));
const selfUpdateRs = read(join(ROOT, "desktop-lab/src-tauri/src/self_update.rs"));
const buildRs = read(join(ROOT, "desktop-lab/src-tauri/build.rs"));

// The command names inside the desktop invoke_handler![…] list, including the
// `self_update::`-qualified ones.
function registeredCommands() {
  const m = /invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/.exec(libRs);
  assert.ok(m, "couldn't find the Lab's invoke_handler in desktop-lab/src-tauri/src/lib.rs");
  return new Set(
    m[1].split(",").map((s) => s.trim().split("::").pop()).filter(Boolean)
  );
}

test("every command the Settings panel invokes is registered natively", () => {
  const registered = registeredCommands();
  const invoked = [...settingsJs.matchAll(/invoke\(\s*"([a-z_]+)"/g)].map((m) => m[1]);
  assert.ok(invoked.length >= 4,
    "expected the panel to invoke at least app_info, check_update, install_update and read_update_journal");
  for (const cmd of new Set(invoked)) {
    assert.ok(
      registered.has(cmd),
      `assets/lab-settings.js invokes "${cmd}", which the Lab does not register in ` +
      `desktop-lab/src-tauri/src/lib.rs. Tauri rejects an unregistered command at ` +
      `runtime, so the panel would show an error instead of the value.`
    );
  }
});

test("the commands the panel needs actually exist in the Rust", () => {
  const defined = new Set([
    ...[...libRs.matchAll(/fn\s+(\w+)\s*\(/g)].map((m) => m[1]),
    ...[...selfUpdateRs.matchAll(/fn\s+(\w+)\s*\(/g)].map((m) => m[1]),
  ]);
  for (const cmd of ["app_info", "check_update", "install_update", "read_update_journal", "update_journal_path"]) {
    assert.ok(defined.has(cmd), `desktop-lab is missing the ${cmd} command the Settings panel calls`);
  }
});

test("app_info's build stamp is actually stamped by build.rs", () => {
  // env! is compile-time: a missing stamp is a build failure, not a runtime
  // one — but only if lib.rs and build.rs agree on the names.
  for (const v of ["SECURACV_BUILD_REV", "SECURACV_BUILD_EPOCH"]) {
    assert.ok(libRs.includes(`env!("${v}")`), `desktop-lab lib.rs no longer reads ${v}`);
    assert.ok(
      buildRs.includes(`cargo:rustc-env=${v}=`),
      `desktop-lab/src-tauri/build.rs no longer stamps ${v}, which app_info reads with env!() — ` +
      `the crate will fail to compile`
    );
  }
});

test("the panel is gated on the self_update CAPABILITY, not on Tauri's presence", () => {
  // Three surfaces share this frontend and only one self-updates. On
  // iOS/iPadOS `window.__TAURI__` exists but lib.rs's `#[cfg(not(desktop))]`
  // handler registers none of the update commands, so a bare `__TAURI__`
  // check would light the entry up on an iPad and fail every invoke behind
  // it — an empty journal and a false "couldn't reach the release channel".
  assert.match(settingsJs, /invoke\("native_capabilities"\)/,
    "lab-settings.js must ask native_capabilities() whether this build self-updates");
  assert.match(settingsJs, /caps\.self_update/,
    "the gate must key off the self_update capability the Lab already reports");
  assert.ok(shellJs.includes('from "./lab-settings.js"'),
    "lab-shell.js must import the settings module it renders");
  assert.match(shellJs, /await probeSettings\(\)/,
    "lab-shell.js must await probeSettings() before its first render, or the sidebar " +
    "is built against a stale answer");
  // The sidebar entry and the route allowlist must BOTH be gated, or a
  // non-updating build shows a Settings page that can't answer anything.
  assert.match(shellJs, /SETTINGS_AVAILABLE\s*\?\s*navItem\("settings"/,
    "the Settings sidebar entry must be behind SETTINGS_AVAILABLE in lab-shell.js");
  assert.match(shellJs, /SETTINGS_AVAILABLE\s*\?\s*\["settings"\]\s*:\s*\[\]/,
    "\"settings\" must only join VALID_IDS where updates exist, so #settings elsewhere " +
    "falls back to the overview through the existing sanitizer");
});

test("the capability the gate reads is one the Lab reports everywhere", () => {
  // native_capabilities must be registered on BOTH handlers — the probe runs
  // on mobile too, and an unregistered command there would throw, which the
  // module treats as "no panel". Correct outcome, wrong reason; assert the
  // seam exists so the gate is answering, not failing.
  assert.match(libRs, /"self_update":\s*cfg!\(desktop\)/,
    "lib.rs must report self_update as a capability for the panel to gate on");
  const handlers = [...libRs.matchAll(/invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/g)];
  assert.strictEqual(handlers.length, 2, "expected a desktop and a non-desktop invoke_handler");
  for (const h of handlers) {
    assert.ok(h[1].includes("native_capabilities"),
      "every invoke_handler must register native_capabilities — the Settings gate probes it " +
      "on mobile as well as desktop");
  }
});

test("a manual check is journaled, or the panel's promise is a lie", () => {
  // The panel and the release notes both say "every check and install". The
  // routine pass journaled; the command the buttons call did not, so opening
  // Settings, pressing "Check now" or retrying offline left no trace — and
  // the offline failure is the single most useful line the journal carries.
  const body = /pub async fn check_update\([\s\S]*?\n}/.exec(selfUpdateRs);
  assert.ok(body, "couldn't find check_update in self_update.rs");
  // `journal(\n    &app,` — rustfmt splits the longer calls across lines.
  const journalCalls = (body[0].match(/journal\(\s*&app,/g) || []).length;
  assert.ok(
    journalCalls >= 3,
    `check_update journals ${journalCalls} of its outcomes; it must record all three ` +
    `(update ready, already current, check failed) or the Settings panel's "every check ` +
    `and install this app has done" is false for every user-initiated check.`
  );
});

test("the Settings panel reports the same build facts as the Flasher's About", () => {
  // Parity, not identity: both apps answer "which build am I on?" with the
  // same four facts. If the Flasher grows a fifth, this is where we notice.
  for (const label of ["Version", "Build", "Built", "Firmware train"]) {
    assert.ok(
      settingsJs.includes(`"${label}"`),
      `the Lab's Settings panel dropped the "${label}" row the Flasher's About panel shows`
    );
  }
});

test("release notes render as elements, never as markup", () => {
  // Notes come from the release body — text we publish, but still text that
  // reaches the DOM. The panel builds nodes; it must not innerHTML them.
  const notes = /function notesNode\([\s\S]*?\n}/.exec(settingsJs);
  assert.ok(notes, "lab-settings.js lost notesNode()");
  assert.ok(
    !/html:/.test(notes[0]) && !/innerHTML/.test(notes[0]),
    "notesNode() must build elements, not set innerHTML — release copy reaches the DOM here"
  );
});

test("the journal read is bounded", () => {
  // A Lab installed for years accumulates a journal; the panel must not ask
  // for an unbounded string.
  assert.match(selfUpdateRs, /MAX_LINES/,
    "read_update_journal must cap how much of the journal it hands the frontend");
  assert.ok(existsSync(join(ROOT, "desktop-lab/src-tauri/src/self_update.rs")));
});
