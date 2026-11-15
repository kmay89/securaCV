// Gates for the Lab app's own views (assets/lab-app.js): the Home, the fleet
// page, the status strip and the shortcuts the desktop app (desktop-lab/)
// adds on top of the shared shell.
//
// What can rot silently, and what each test holds:
//   1. the module only invokes commands desktop-lab registers — an
//      unregistered command rejects at runtime with nothing in CI to say so;
//   2. the app views are routed, listed and landed on ONLY where the native
//      shell has something to show (appViews(): a Tauri shell reporting ports
//      to list, an mDNS browse, or a kernel poll). The website and the iPad
//      shell must never get a Home that answers "—" to everything;
//   3. the words stay coarse and honest: presence is what the network said,
//      and "verified" is never claimed (it belongs to a chain walked against a
//      key pinned at pairing, which the Lab does not hold);
//   4. the kernel address and the well-known bases are the ONE list every
//      surface uses — the Witness Wall host, the menu bar companion, and now
//      the fleet page — so typing an address in one place steers all three;
//   5. the pure helpers answer the way the Flasher's fleet book does.
//
// Runs under "page logic tests" (.github/workflows/canary-local.yml). Reads
// source text, and imports the module under a stub window for the helpers.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { pathToFileURL } = require("node:url");

const CANARY = join(__dirname, "..");
const ROOT = join(CANARY, "..");
const read = (p) => readFileSync(p, "utf8");

const appJs = read(join(CANARY, "assets/lab-app.js"));
const shellJs = read(join(CANARY, "assets/lab-shell.js"));
const settingsJs = read(join(CANARY, "assets/lab-settings.js"));
const hostJs = read(join(CANARY, "assets/witness-host.js"));
const libRs = read(join(ROOT, "desktop-lab/src-tauri/src/lib.rs"));
const companionRs = read(join(ROOT, "desktop-lab/src-tauri/src/companion.rs"));
const stripComments = (src) => src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:"'`\w])\/\/.*$/gm, "$1");

function registeredCommands() {
  const m = /invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/.exec(libRs);
  assert.ok(m, "couldn't find the Lab's invoke_handler in desktop-lab/src-tauri/src/lib.rs");
  return new Set(m[1].split(",").map((s) => s.trim().split("::").pop()).filter(Boolean));
}

test("every command the app views invoke is registered by desktop-lab", () => {
  const registered = registeredCommands();
  const invoked = new Set([...stripComments(appJs).matchAll(/\binvoke\(\s*"([a-z_]+)"/g)].map((m) => m[1]));
  for (const must of ["native_capabilities", "list_ports", "fleet_scan", "witness_discover"]) {
    assert.ok(invoked.has(must), `lab-app.js no longer invokes ${must} — the Home lost its native read`);
  }
  for (const cmd of invoked) {
    assert.ok(registered.has(cmd), `assets/lab-app.js invokes "${cmd}", which desktop-lab never registers`);
  }
  // detect_chip resets the board. The Flash bench owns that call; the Home
  // must only ever enumerate ports, or two pollers would fight over one port.
  assert.ok(!invoked.has("detect_chip"), "lab-app.js must not read the chip — list_ports only");
  assert.ok(!invoked.has("flash"), "lab-app.js must never flash; it sends people to the Flash bench");
});

test("the app views exist only where the native shell has something to show", () => {
  assert.match(appJs, /export const appViews = \(\) => IN_APP && \(CAPS\.serial_list \|\| CAPS\.mdns \|\| CAPS\.discovery\)/,
    "appViews() must require the Tauri shell AND a capability the Home can read");
  // Routed, listed, and landed on — all three behind the same gate.
  assert.match(shellJs, /\.\.\.\(appViews\(\) \? \["dashboard", "devices"\] : \[\]\)/,
    "\"dashboard\" and \"devices\" must join VALID_IDS only behind appViews()");
  assert.match(shellJs, /appViews\(\) \? navItem\("dashboard"/, "the Home sidebar entry must be behind appViews()");
  assert.match(shellJs, /appViews\(\) \? navItem\("devices"/, "the fleet sidebar entry must be behind appViews()");
  assert.match(shellJs, /else if \(!raw && appViews\(\)\) navigate\("dashboard", false\);/,
    "the app lands on its Home only behind appViews(); the website keeps the Overview");
  assert.match(shellJs, /await probeApp\(\)/, "lab-shell.js must await probeApp() before VALID_IDS is built");
  // Every native read is behind its own capability: a build that lists no
  // ports or browses no mDNS must never invoke the command that would fail.
  const poller = appJs.slice(appJs.indexOf("async tick()"), appJs.indexOf("devices() {"));
  assert.match(poller, /if \(CAPS\.serial_list\) \{[\s\S]*?invoke\("list_ports"\)/, "list_ports must be behind serial_list");
  assert.match(poller, /if \(CAPS\.mdns\) \{[\s\S]*?invoke\("fleet_scan"/, "fleet_scan must be behind mdns");
  assert.match(poller, /if \(CAPS\.discovery\) \{[\s\S]*?invoke\("witness_discover"/, "witness_discover must be behind discovery");
});

test("the words stay coarse: presence as reported, and never a claim of verification", () => {
  // "verified" may appear only in its own denial (the header comment that
  // states the rule is prose, not a string the app shows).
  const code = stripComments(appJs);
  for (const m of code.matchAll(/verif\w*/gi)) {
    const around = code.slice(Math.max(0, m.index - 20), m.index + 10);
    assert.match(around, /Nothing here is verified/, `lab-app.js says "${m[0]}" outside the one honest sentence: …${around}…`);
  }
  for (const w of ["person", "occupant", "breathing", "sealed", "evidence vault", "recording"]) {
    assert.ok(!new RegExp(`"[^"]*\\b${w}\\b[^"]*"`, "i").test(appJs), `lab-app.js names "${w}" in a string — not a word the fleet report supports`);
  }
  // A kernel row's online field is read as a boolean or not at all (silence
  // is never "online"), and status words are the short lowercase set.
  assert.match(appJs, /typeof d\.online === "boolean" \? d\.online : null/, "online must be read as a boolean or null");
  assert.match(appJs, /\/\^\[a-z0-9_-\]\{1,16\}\$\//, "chain/hub words must be the short lowercase set, as companion.rs reads them");
});

test("one kernel address, one well-known list, every surface", () => {
  assert.match(appJs, /kernel: "scv-kernel"/, "lab-app.js must read the kernel address under the key witness-host.js uses");
  assert.match(hostJs, /localStorage\.getItem\("scv-kernel"\)/, "witness-host.js moved its kernel key — update lab-app.js to match");
  const defaults = ["http://canary.local:8099", "http://canary.local:8799", "http://canary.local"];
  const inApp = /b\.push\("([^"]+)", "([^"]+)", "([^"]+)"\);/.exec(appJs.slice(appJs.indexOf("function kernelBases")));
  assert.ok(inApp, "kernelBases() lost its well-known three");
  assert.deepStrictEqual(inApp.slice(1, 4), defaults, "lab-app.js's well-known bases drifted from the Apple TV's order");
  const rust = [...companionRs.slice(companionRs.indexOf("DEFAULT_BASES")).matchAll(/"(http:\/\/canary\.local[^"]*)"/g)].map((m) => m[1]).slice(0, 3);
  assert.deepStrictEqual(rust, defaults, "companion.rs DEFAULT_BASES drifted from the frontend's");
  // Saving an address hands it to the tray, so the companion polls the same hub.
  assert.match(appJs, /invoke\("companion_set_bases", \{ bases: kernelBases\(\) \}\)/, "saving the address must re-point the companion");
  // Settings points at the fleet page for the address instead of growing a second field.
  assert.match(settingsJs, /href: "#devices"/, "Settings must link to the fleet page for the kernel address");
});

test("the helpers answer like the Flasher's fleet book", async () => {
  globalThis.window = {};
  const app = await import(pathToFileURL(join(CANARY, "assets/lab-app.js")).href);
  assert.strictEqual(app.IN_APP, false, "no Tauri bridge under the stub → off-app");
  assert.strictEqual(app.appViews(), false);
  // fwBehind: dotted numbers only; a dev build is never nagged.
  assert.strictEqual(app.fwBehind("2.4.17", "2.4.19"), true);
  assert.strictEqual(app.fwBehind("2.4.19", "2.4.19"), false);
  assert.strictEqual(app.fwBehind("2.5.0", "2.4.19"), false);
  assert.strictEqual(app.fwBehind("2.4.18-dev.3", "2.4.19"), true);
  assert.strictEqual(app.fwBehind("dev", "2.4.19"), false);
  assert.strictEqual(app.fwBehind("", "2.4.19"), false);
  // deviceBase: a real port wins; the server-bearing families get :80; a
  // Sense or Vision (no server) gets no address.
  assert.strictEqual(app.deviceBase({ ip: "192.168.1.40", port: 8080, deviceType: "canary-sense" }), "http://192.168.1.40:8080");
  assert.strictEqual(app.deviceBase({ ip: "192.168.1.40", port: 1, deviceType: "canary-wap" }), "http://192.168.1.40");
  assert.strictEqual(app.deviceBase({ host: "canary-dash-3.local.", port: 1, deviceType: "canary-display" }), "http://canary-dash-3.local");
  assert.strictEqual(app.deviceBase({ ip: "192.168.1.52", port: 1, deviceType: "canary-sense" }), null);
  assert.strictEqual(app.deviceBase({ ip: "fe80::1", port: 1, deviceType: "canary-wap" }), null, "a bare IPv6 literal is not a base");
  // presenceWord: what was said, or that nothing was.
  assert.strictEqual(app.presenceWord({ online: true }), "online");
  assert.strictEqual(app.presenceWord({ online: false }), "offline");
  assert.strictEqual(app.presenceWord({ online: null, heardAt: 0 }), "hasn't said");
  assert.match(app.presenceWord({ online: null, heardAt: Date.now() - 5 * 60e3 }), /^last heard 5 min ago$/);
  // The well-known bases, with no typed address under the stub.
  assert.deepStrictEqual(app.kernelBases(), ["http://canary.local:8099", "http://canary.local:8799", "http://canary.local"]);
  // The journal read back as state, line by line as self_update.rs writes it.
  const settings = await import(pathToFileURL(join(CANARY, "assets/lab-settings.js")).href);
  const st = settings.updateState;
  assert.strictEqual(settings.noteJournalLine("2026-11-06 04:00:00Z v0.4.1 is ready (running v0.4.0)"), true);
  assert.deepStrictEqual({ v: st.update && st.update.version, c: st.checked, f: st.failed }, { v: "0.4.1", c: true, f: null });
  assert.strictEqual(settings.noteJournalLine("2026-11-06 04:00:05Z v0.4.1 offered — user chose later"), false, "a Later keeps the update ready");
  assert.strictEqual(st.update.version, "0.4.1");
  assert.strictEqual(settings.noteJournalLine("2026-11-06 10:00:00Z checked — v0.4.1 is current"), true);
  assert.deepStrictEqual({ v: st.update, c: st.checked }, { v: null, c: true });
  assert.strictEqual(settings.noteJournalLine("2026-11-06 16:00:00Z update check unavailable: offline"), true);
  assert.deepStrictEqual({ c: st.checked, f: st.failed }, { c: false, f: "update check unavailable: offline" });
  assert.strictEqual(settings.noteJournalLine("2026-11-06 16:00:01Z downloading v0.4.1…"), false);
});

test("the Home follows the native updater's journal, in the updater's own words", () => {
  // Codex on #1925: updateState had one writer, the Settings panel's check,
  // so a routine check that found an update with no panel open left the Home
  // saying "up to date". The journal is the updater's record; the frontend
  // reads it back in the exact vocabulary self_update.rs writes.
  const rs = read(join(ROOT, "desktop-lab/src-tauri/src/self_update.rs"));
  assert.match(rs, /"v\{version\} is ready \(running v\{\}\)"/, "self_update.rs changed its 'ready' line — update JOURNAL_READY");
  assert.match(rs, /"checked — v\{\} is current"/, "self_update.rs changed its 'current' line — update JOURNAL_CURRENT");
  assert.match(rs, /"update check unavailable: \{e\}"/, "self_update.rs changed its 'unavailable' line — update JOURNAL_FAILED");
  assert.match(settingsJs, /listen\("update:log", \(ev\) => \{ if \(typeof ev\.payload === "string"\) noteJournalLine\(ev\.payload\); \}\)/,
    "watchUpdates() must follow every journal line the updater emits");
  assert.match(settingsJs, /invoke\("read_update_journal"\)/, "watchUpdates() must seed from the journal on launch");
  assert.match(appJs, /watchUpdates\(\)\.catch/, "probeApp() must start the watch where self-update exists");
  assert.match(appJs, /updateState\.listeners\.add\(\(\) => fleet\.emit\(\)\)/, "a changed update state must redraw the Home and the strip");
});

test("the shortcuts reach the shell from inside a framed bench", async () => {
  // Focus is in the frame after any click on a bench, and a frame's keydowns
  // never reach the parent document — so without the forward, ⌘K and the
  // rest worked only until the first click. lab-nav.js forwards the chords
  // the shell owns; the shell takes them from the bench on screen only.
  const navJs = read(join(CANARY, "assets/lab-nav.js"));
  assert.match(navJs, /window\.parent\.postMessage\(\{ source: "scv-lab", type: "key", key, shiftKey: e\.shiftKey, typing \}, location\.origin\)/,
    "lab-nav.js must forward the chord — key, shift, typing — and nothing read from its document");
  assert.match(navJs, /forwardShortcuts\(\);/, "lab-nav.js must install the forward in embedded mode");
  // Codex on #1929: the website's shell installs no receiver, so a frame
  // that forwarded unconditionally canceled real browser shortcuts (⌘R,
  // ⌘K, ⌘1–6) for nothing. The frame forwards only once the shell says it
  // listens, and the shell says so only where it installed the handler.
  const fwd = navJs.slice(navJs.indexOf("function forwardShortcuts("));
  assert.match(fwd.slice(0, 400), /d\.source === "scv-lab-shell" && d\.type === "shortcuts"\) armed = true/,
    "the frame must arm forwarding only on the shell's own message");
  assert.match(fwd, /if \(!armed\) return;/, "an unarmed frame must leave every chord to the browser");
  assert.match(shellJs, /if \(SHORTCUTS_ON\) \{[\s\S]{0,200}type: "shortcuts" \}, location\.origin\)/,
    "the shell must arm a framed bench only where it installed the shortcuts");
  assert.match(shellJs, /installShortcuts\(APP_CTX\);\n\s+SHORTCUTS_ON = true;/, "SHORTCUTS_ON must follow installShortcuts()");
  const listener = appJs.slice(appJs.indexOf('d.type !== "key"') - 400);
  assert.match(listener.slice(0, 400), /e\.origin !== location\.origin/, "the shell must reject other origins before reading a forwarded key");
  assert.match(listener, /e\.source !== frame\.contentWindow/, "only the bench actually on screen may forward a key");
  // The chord set is the same on both sides: every key handleShortcut() acts
  // on is one the frame forwards, and the frame forwards nothing else.
  const chords = /const SHELL_CHORDS = \/\^\[(.+?)\]\$\/;/.exec(navJs);
  assert.ok(chords, "lab-nav.js lost SHELL_CHORDS");
  const forwarded = new RegExp("^[" + chords[1] + "]$");
  for (const k of ["k", "r", ",", "[", "]", "1", "6"]) assert.ok(forwarded.test(k), `the frame does not forward ⌘${k}`);
  assert.ok(!forwarded.test("h"), "⌘H alone must not be forwarded (only ⇧H is the shell's)");
  globalThis.window = {};
  const app = await import(pathToFileURL(join(CANARY, "assets/lab-app.js")).href);
  const calls = [];
  const ctx = { focusSearch: () => calls.push("search"), navigate: (v) => calls.push("nav:" + v), reloadBench: () => calls.push("reload"),
    stages: () => [{ id: "choose" }, { id: "build" }], firstOf: (s) => ({ slug: s.id + "-1" }), goBench: (s) => calls.push("bench:" + s), step: (d) => calls.push("step:" + d) };
  assert.strictEqual(app.handleShortcut(ctx, { key: "K" }), true);
  assert.strictEqual(app.handleShortcut(ctx, { key: "h", shiftKey: true }), true);
  assert.strictEqual(app.handleShortcut(ctx, { key: "h" }), false, "⌘H without shift is not a chord");
  assert.strictEqual(app.handleShortcut(ctx, { key: "2" }), true);
  assert.strictEqual(app.handleShortcut(ctx, { key: "2", typing: true }), false, "a digit while typing is text");
  assert.strictEqual(app.handleShortcut(ctx, { key: "]", typing: true }), false);
  assert.strictEqual(app.handleShortcut(ctx, { key: "]" }), true);
  assert.strictEqual(app.handleShortcut(ctx, { key: "r", typing: true }), true, "⌘R reloads the bench even from a field");
  assert.deepStrictEqual(calls, ["search", "nav:dashboard", "bench:build-1", "step:1", "reload"]);
  assert.match(settingsJs, /They work anywhere in the app, inside a bench too/, "Settings must stop saying the shortcuts fail inside a bench");
});

test("the Home's setup list reads only what the app can observe", () => {
  // The "flashed" step reads the Flash bench's own memory, under the key
  // flash.js writes when an install completed and its bytes matched.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const key = /const INSTALL_TIMES_KEY = "([^"]+)"/.exec(flashJs);
  assert.ok(key, "flash.js lost INSTALL_TIMES_KEY");
  const mine = /const FLASH_MEMORY_KEY = "([^"]+)"/.exec(appJs);
  assert.ok(mine, "lab-app.js lost FLASH_MEMORY_KEY");
  assert.strictEqual(mine[1], key[1], "lab-app.js reads the flasher's memory under a different key than flash.js writes");
  assert.doesNotMatch(appJs, /setItem\(FLASH_MEMORY_KEY/, "the Home must never write the flasher's memory");
  // Codex on #1929: the desktop app always mounts the native bench, which
  // never wrote the browser flasher's install times — so the step could not
  // be done where it was shown. The native bench records its own success,
  // under the key the Home reads, once the flash command has returned.
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const nativeKey = /const FLASHED_KEY = "([^"]+)"/.exec(nativeJs);
  const homeKey = /const FLASHED_KEY = "([^"]+)"/.exec(appJs);
  assert.ok(nativeKey && homeKey, "FLASHED_KEY must be declared in both flash-native.js and lab-app.js");
  assert.strictEqual(homeKey[1], nativeKey[1], "the Home reads the native flash record under a different key than flash-native.js writes");
  assert.match(nativeJs, /invoke\("flash", \{[\s\S]*?\}\), line\);\n\s+fill\.style\.width = "100%";\n\s+try \{ localStorage\.setItem\(FLASHED_KEY/,
    "flash-native.js must record the flash right after the flash command returned, before anything else can throw");
  assert.doesNotMatch(appJs, /setItem\(FLASHED_KEY/, "the Home must never write the native flash record");
  assert.match(appJs, /if \(localStorage\.getItem\(FLASHED_KEY\)\) return true;/, "flashedHere() must read the native record");
  // Every step is gated on the capability its observation needs.
  const steps = appJs.slice(appJs.indexOf("export function setupSteps"), appJs.indexOf("const setupHidden"));
  assert.match(steps, /id: "plug"[\s\S]*?show: CAPS\.serial_list/, "the USB step needs serial_list");
  assert.match(steps, /id: "flash"[\s\S]*?show: CAPS\.serial/, "the flash step needs serial");
  assert.match(steps, /id: "boot"[\s\S]*?show: CAPS\.mdns/, "the first-boot step is observed over mDNS");
  assert.match(steps, /id: "hub"[\s\S]*?show: CAPS\.discovery/, "the hub step needs discovery");
  // The step's words promise only what both benches do: the native bench
  // takes no safety copy (backupPath: ""), so no "backs up" here.
  assert.match(nativeJs, /backupPath: "",/, "flash-native.js now takes a backup — the setup step may say so again");
  assert.doesNotMatch(steps, /back(s|ed)? (it |the board )?up|safety copy/i, "the setup step must not promise a backup the native bench does not take");
  // Forgetting on-device memory also brings a hidden list back.
  assert.match(appJs, /KEYS\.benches, KEYS\.setupHidden\]/, "forgetEverything() must clear the setup list's keys");
});

test("a running flash holds the shell: no route change, step or reload orphans it unasked", () => {
  // The flash bench's write and boot monitor live in a framed page. The
  // write finishes in Rust whatever the page does, but replacing the frame
  // loses what only the page holds — the receipt, the boot log, the Home's
  // record of the flash — and the returning bench resets the board with a
  // board read. So the bench tells the shell when it is busy, and every way
  // off the bench asks first.
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const announce = nativeJs.slice(nativeJs.indexOf("const announceBusy ="), nativeJs.indexOf("const setMonitoring ="));
  assert.match(announce, /const busy = !!\(st\.busy \|\| st\.monitoring\)/, "busy is the write OR the monitor holding the port");
  assert.match(announce, /window === window\.top\) return;/, "a bench that is not framed posts nothing");
  assert.match(announce, /window\.parent\.postMessage\(\{ source: "scv-lab", type: "busy", busy \}, location\.origin\)/,
    "the busy word goes to the parent at this origin, and carries nothing else");
  assert.match(nativeJs, /st\.monitoring = on;\n\s+for \(const c of monitorControls\) c\.disabled = !on;\n\s+announceBusy\(\);/,
    "setMonitoring() must announce (the re-read, Stop watching and a stopped monitor all go through it)");
  assert.match(nativeJs, /st\.busy = true;\n\s+announceBusy\(\);/, "onFlash must announce as the write starts");
  assert.match(nativeJs, /st\.busy = false;\n\s+announceBusy\(\);/, "the finally must announce when the write is over");
  // The shell's listener: same origin, and only from the bench on screen.
  const listener = shellJs.slice(shellJs.indexOf('d.type !== "busy"') - 400);
  assert.match(listener.slice(0, 400), /e\.origin !== location\.origin/, "the busy listener must reject other origins first");
  assert.match(listener.slice(0, 700), /e\.source !== frame\.contentWindow/, "only the bench actually on screen may say it is busy");
  assert.match(listener.slice(0, 900), /benchBusy = d\.busy === true;/, "the flag is the message's boolean and nothing else");
  assert.match(listener.slice(0, 1000), /\.stepper\.reload"\)\) b\.disabled = benchBusy;/, "the bar's reload button follows the flag");
  // One guard, three doors: navigate(), step() and reloadBench().
  const guard = shellJs.slice(shellJs.indexOf("function leaveBusyBench("), shellJs.indexOf('window.addEventListener("message"', shellJs.indexOf("function leaveBusyBench(")));
  assert.match(guard, /if \(!benchBusy\) return true;/);
  assert.match(guard, /if \(!confirm\(BUSY_LEAVE\)\) return false;/, "leaving a busy bench must be a confirm()");
  assert.match(shellJs, /const BUSY_LEAVE = "A flash is still running on this bench\. Leave anyway\? The write finishes, but the receipt and boot log are lost\.";/);
  const nav = shellJs.slice(shellJs.indexOf("function navigate("), shellJs.indexOf("/* ---- sidebar"));
  assert.match(nav, /if \(!leaveBusyBench\(\)\) return;/, "navigate() must ask before it replaces the frame");
  const ctx = shellJs.slice(shellJs.indexOf("const APP_CTX = {"));
  assert.match(ctx, /reloadBench: \(\) => \{\n\s+const f = document\.querySelector\("\.bench-frame"\);\n\s+if \(!f \|\| !leaveBusyBench\(\)\) return;/,
    "reloadBench() (⌘R) must ask before it reloads the frame");
  assert.match(ctx, /step: \(dir\) => \{\n\s+if \(!leaveBusyBench\(\)\) return;/, "step() (⌘[ / ⌘]) must ask before it moves");
  assert.match(shellJs, /function renderContent\(view, entry\) \{[\s\S]{0,300}benchBusy = false;/, "replacing the frame clears the word");
  assert.match(shellJs, /class: "stepper out reload"[\s\S]{0,200}disabled: benchBusy/, "the reload button is built disabled while busy");
});

test("the status strip is quiet: no live region rebuilt per poll, and no redraw under the keyboard", () => {
  // The poller emits twice per tick. A footer that was aria-live re-announced
  // itself to a screen reader every 15–60 s for as long as the app was open,
  // and a Home rebuilt on every emit dropped a keyboard user's place.
  const strip = appJs.slice(appJs.indexOf("export function statusStrip"), appJs.indexOf("/* ---- keyboard shortcuts"));
  assert.doesNotMatch(strip, /aria-live/, "the strip must not be a live region");
  assert.doesNotMatch(strip, /replaceChildren/, "the chips are patched in place, never rebuilt");
  assert.strictEqual((strip.match(/role: "status"/g) || []).length, 1, "one role=status span carries the summary");
  assert.match(strip, /if \(c\.lastChild\.textContent !== text\) c\.lastChild\.textContent = text;/, "a chip's text is written only when it changed");
  assert.match(strip, /if \(now !== said\) \{ said = now; summary\.textContent = now; \}/, "the summary is written only when it changed");
  assert.doesNotMatch(strip, /const now = [^\n]*listening/, "the start-of-tick 'listening…' is never the summary's word");
  const home = appJs.slice(appJs.indexOf("export function dashboardView"), appJs.indexOf("function welcomeCard"));
  assert.match(home, /fleet\.subscribe\(\(\) => \{ if \(!root\.contains\(document\.activeElement\)\) render\(\); \}\)/,
    "the Home must not redraw while focus is inside it");
  const devices = appJs.slice(appJs.indexOf("export function devicesView"), appJs.indexOf("export function statusStrip"));
  assert.match(devices, /fleet\.subscribe\(\(\) => \{ if \(!root\.contains\(document\.activeElement\)\) render\(\); \}\)/,
    "the fleet page must not redraw while focus is inside it");
  assert.match(shellJs, /if \(side && side\.contains\(document\.activeElement\)\) return;/, "the sidebar must not rebuild while focus is inside it");
});

test("only an address on this network gets an Open link or is saved as the hub", async () => {
  // An mDNS announcement is unauthenticated LAN traffic, and the Open link
  // hands its address to the OS browser; the typed hub address is saved as
  // typed and announced as in use everywhere. Both pass the same gate the
  // native side applies before any socket opens — lib.rs host_is_local —
  // and the JS twin is pinned to that function's own accept/refuse lists.
  globalThis.window = {};
  const app = await import(pathToFileURL(join(CANARY, "assets/lab-app.js")).href);
  assert.strictEqual(app.deviceBase({ ip: "203.0.113.5", port: 1, deviceType: "canary-wap" }), null, "a public IP gets no Open link");
  assert.strictEqual(app.deviceBase({ host: "login.example.net", port: 8080, deviceType: "canary-wap" }), null, "a public name gets no Open link");
  assert.strictEqual(app.deviceBase({ ip: "192.168.1.40", port: 1, deviceType: "canary-wap" }), "http://192.168.1.40");
  assert.strictEqual(app.deviceBase({ host: "canary-dash-3.local.", port: 1, deviceType: "canary-display" }), "http://canary-dash-3.local");
  const fn = libRs.slice(libRs.indexOf("fn local_hosts_pass_and_public_hosts_are_refused"));
  const lists = [...fn.matchAll(/for h in \[([\s\S]*?)\]/g)].map((m) => [...m[1].matchAll(/"([^"]*)"/g)].map((x) => x[1]));
  assert.strictEqual(lists.length, 2, "lib.rs's host test lost its accept/refuse lists");
  assert.ok(lists[0].includes("canary.local") && lists[1].includes("example.com"), "the lists changed shape");
  for (const h of lists[0]) assert.strictEqual(app.hostIsLocal(h), true, `${h} is local to lib.rs and must be here`);
  for (const h of lists[1]) assert.strictEqual(app.hostIsLocal(h), false, `${h} is refused by lib.rs and must be here`);
  assert.strictEqual(app.hostIsLocal("[fe80::1]"), true, "brackets are stripped, as lib.rs does");
  assert.strictEqual(app.hostIsLocal("256.1.1.1"), false);
  // The fleet page's Save runs the gate before anything is written, and the
  // "Saved" sentence is reachable only past it.
  const save = appJs.slice(appJs.indexOf("const saveAddr = async"), appJs.indexOf("const note = h("));
  assert.match(save, /host = new URL\(v\)\.hostname/);
  assert.match(save, /if \(v && !hostIsLocal\(host\)\) \{\n\s+note\.textContent = "That address is not on this network \(private\/LAN addresses and \.local names only\), so the app will not use it\.";\n\s+return;/);
  assert.ok(save.indexOf("!hostIsLocal(host)") < save.indexOf('"Saved. The tray and the Witness Wall use it too."'), "the refusal must come before the save");
  assert.ok(save.indexOf("!hostIsLocal(host)") < save.indexOf("localStorage.setItem(KEYS.kernel"), "nothing is written before the gate");
  // What is remembered from an announcement is bounded.
  const tick = appJs.slice(appJs.indexOf("async tick()"), appJs.indexOf("devices() {"));
  assert.match(tick, /name: word\(s\.name, 40\), model: word\(s\.model, 40\), deviceType: word\(s\.deviceType, 40\)/);
  assert.match(tick, /role: word\(s\.role, 40\), fw: word\(s\.fw, 24\)/);
  assert.match(tick, /host: word\(s\.host, 40\)/);
  assert.match(tick, /if \(ids\.length > MAX_SEEN\)/, "the map is capped");
  assert.match(appJs, /const MAX_SEEN = 64;/);
  // The Wall host and the fleet page accept the same typed scheme.
  const key = (src, label) => {
    const m = /(\/\^https\?:\\\/\\\/\/i)\.test\(k\)/.exec(src);
    assert.ok(m, `${label} lost its kernel-address scheme test`);
    return m[1];
  };
  assert.strictEqual(key(hostJs, "witness-host.js"), key(appJs, "lab-app.js"), "witness-host.js and lab-app.js must accept the same scheme for the typed hub");
});

test("CI runs this gate", () => {
  const yml = read(join(ROOT, ".github/workflows/canary-local.yml"));
  assert.match(yml, /node --test canary-local\/tests\/lab_app\.test\.js/, "add this file to the page logic tests job");
});
