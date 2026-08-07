// Drift-gate: keep the NATIVE Mac app (desktop/) in lock-step with the canonical
// product catalog the BROWSER Lab derives from, so the two flashers can't silently
// disagree. Both consume one catalog (devices/flash.json); the browser reads these
// values live, while the native app still HARDCODES a few. Where it does, this test
// fails the instant the hardcode drifts from the catalog — turning a silent runtime
// mismatch (a board the browser flashes but native rejects; a moved release host)
// into a loud CI failure that names the exact file to fix.
//
// This runs under the "page logic tests" check — canary-local.yml enumerates each
// test file, and this one is in that list; its `paths:` filters also include the
// native source (lib.rs / we2.rs) so a native-side edit triggers the gate too. It
// reads source text, not compiled Rust, so it needs no desktop toolchain. When a
// future change makes native DERIVE one of these from the
// embedded catalog instead of hardcoding it, update the matching assertion here —
// the drift risk is gone once there's a single source.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { pathToFileURL } = require("node:url");

const ROOT = join(__dirname, "..", "..");     // repo root
const CANARY = join(__dirname, "..");         // canary-local/
const read = (p) => readFileSync(p, "utf8");

const catalog = JSON.parse(read(join(CANARY, "devices/flash.json")));
const libRs = read(join(ROOT, "desktop/src-tauri/src/lib.rs"));
const we2Rs = read(join(ROOT, "desktop/src-tauri/src/we2.rs"));
const we2Core = read(join(CANARY, "assets/we2-core.js"));

// The same fold the browser (flash-core.js:normalizeChip) and the native guard use.
const normChip = (s) => String(s || "").toUpperCase().replace(/[\s\-_]+/g, "");

// The text of a top-level native fn (col-0 `fn`/`async fn`), from its signature
// to the next top-level item. Reads source, not compiled Rust — enough to assert
// which guard a command routes through without a desktop toolchain.
const nativeFnBody = (src, name) => {
  const sig = new RegExp(`\\n(?:pub\\s+)?(?:async\\s+)?fn\\s+${name}\\s*\\(`);
  const m = sig.exec(src);
  assert.ok(m, `couldn't find fn ${name} in desktop/src-tauri/src/lib.rs`);
  const after = src.slice(m.index + 1);
  // Stop at the next top-level item — its `fn`, its attribute (#[…]), or the doc
  // comment (///) that precedes it — so a body can't bleed into the next command.
  const next = after.search(/\n(?:pub\s+)?(?:async\s+)?fn\s+\w|\n\s*#\[|\n\s*\/\/\//);
  return next >= 0 ? after.slice(0, next) : after;
};

test("chip guard: native recognizes every ESP32 chip the catalog guards", () => {
  // native canonical_chip() maps espflash's raw string → canonical via a hardcoded
  // ("esp32s3","ESP32-S3")-style table. Pull the canonical spellings out of it.
  const recognized = new Set(
    [...libRs.matchAll(/\(\s*"esp32[a-z0-9]*"\s*,\s*"(ESP32[^"]*)"\s*\)/g)].map((m) => normChip(m[1]))
  );
  assert.ok(recognized.size >= 3,
    "couldn't parse native canonical_chip() table from desktop/src-tauri/src/lib.rs");

  // Every chip the catalog guards (the browser derives its picker from `chips`)
  // must be recognized, or native's detect_chip rejects a board the browser flashes.
  for (const chip of Object.keys(catalog.chips || {})) {
    assert.ok(
      recognized.has(normChip(chip)),
      `native can't recognize catalog chip "${chip}" — add it to ` +
      `desktop/src-tauri/src/lib.rs:canonical_chip so the Mac app flashes it too`
    );
  }
});

test("WE2 module USB id: native consts match the catalog's we2_module", () => {
  const we2 = catalog.we2_module || {};
  const vid = we2Rs.match(/USB_VID:\s*u16\s*=\s*(0x[0-9a-fA-F]+)/);
  const pid = we2Rs.match(/USB_PID:\s*u16\s*=\s*(0x[0-9a-fA-F]+)/);
  assert.ok(vid && pid, "couldn't parse USB_VID/USB_PID from desktop/src-tauri/src/we2.rs");
  assert.strictEqual(parseInt(vid[1], 16), parseInt(we2.usb_vid, 16),
    `we2.rs USB_VID (${vid[1]}) != catalog we2_module.usb_vid (${we2.usb_vid})`);
  assert.strictEqual(parseInt(pid[1], 16), parseInt(we2.usb_pid, 16),
    `we2.rs USB_PID (${pid[1]}) != catalog we2_module.usb_pid (${we2.usb_pid})`);
});

test("WE2 model slot: catalog, native, and browser all agree on the flash address", () => {
  const catalogAddr = parseInt(String((catalog.we2_module || {}).model_addr), 16);
  assert.ok(Number.isFinite(catalogAddr) && catalogAddr > 0, "catalog we2_module.model_addr missing");
  const nativeAddr = we2Rs.match(/MODEL_ADDR:\s*u32\s*=\s*(0x[0-9a-fA-F_]+)/);
  const browserAddr = we2Core.match(/MODEL_ADDR:\s*(0x[0-9a-fA-F]+)/);
  assert.ok(nativeAddr, "couldn't parse MODEL_ADDR from desktop/src-tauri/src/we2.rs");
  assert.ok(browserAddr, "couldn't parse WE2.MODEL_ADDR from canary-local/assets/we2-core.js");
  assert.strictEqual(parseInt(nativeAddr[1].replace(/_/g, ""), 16), catalogAddr,
    "we2.rs MODEL_ADDR != catalog we2_module.model_addr");
  assert.strictEqual(parseInt(browserAddr[1], 16), catalogAddr,
    "we2-core.js WE2.MODEL_ADDR != catalog we2_module.model_addr");
});

test("dev channel: browser, native backend, and native frontend pin the same fw-dev-latest URL", () => {
  // Three DEV_FLASH_MANIFEST_URL constants exist on purpose — each is a fixed
  // first-party address deliberately NOT routed through its side's URL guard,
  // so "dev channel" can only ever mean this one URL. That safety argument
  // collapses the instant any copy drifts, so pin all three to each other.
  const sources = [
    ["canary-local/assets/flash-core.js", read(join(CANARY, "assets/flash-core.js"))],
    ["desktop/src-tauri/src/lib.rs", libRs],
    ["desktop/src/app.js", read(join(ROOT, "desktop/src/app.js"))],
  ];
  const urls = sources.map(([label, src]) => {
    const m = src.match(/DEV_FLASH_MANIFEST_URL[^"]*"(https:\/\/[^"]+)"/);
    assert.ok(m, `couldn't parse DEV_FLASH_MANIFEST_URL from ${label}`);
    return [label, m[1]];
  });
  const [refLabel, ref] = urls[0];
  assert.match(ref, /\/releases\/download\/fw-dev-latest\//,
    `${refLabel} DEV_FLASH_MANIFEST_URL doesn't point at the fw-dev-latest tag`);
  for (const [label, url] of urls.slice(1)) {
    assert.strictEqual(url, ref,
      `${label} DEV_FLASH_MANIFEST_URL (${url}) != ${refLabel} (${ref}) — ` +
      `the two flashers would flash different dev releases`);
  }
});

test("release origin: native's download-host guard matches the catalog's manifest host", () => {
  // Native guards every download with a literal origin prefix; the browser derives
  // its host from the catalog's manifest_url. They must name the same release host,
  // else native refuses assets the browser fetches (or vice versa) after a move.
  const guards = [...libRs.matchAll(/"(https:\/\/[^"]+\/releases\/download\/)"/g)].map((m) => m[1]);
  assert.ok(guards.length >= 1,
    "couldn't find native's release-origin guard in desktop/src-tauri/src/lib.rs");
  const catalogHost = String(catalog.manifest_url).match(/^(https:\/\/.+\/releases\/download\/)/);
  assert.ok(catalogHost, "catalog manifest_url isn't a …/releases/download/ URL");
  for (const g of guards) {
    assert.ok(catalogHost[1].startsWith(g),
      `native release-origin guard "${g}" doesn't match catalog manifest host "${catalogHost[1]}"`);
  }
});

test("provisioning NVS: the browser writes the same key-set as native build_nvs", async () => {
  // Native build_nvs (provisioning.rs) IS the firmware contract for a provisioned
  // board — the exact NVS keys the runtime reads. The browser must write the same
  // set, or a board provisioned in the Lab differs from one provisioned natively.
  const provRs = read(join(ROOT, "desktop/src-tauri/src/provisioning.rs"));
  const nativeKeys = new Set(
    [...provRs.matchAll(/writer\.(?:string|u8|u16|u32|blob)\(\s*"([a-z0-9_]+)"/g)].map((m) => m[1])
  );
  assert.ok(nativeKeys.size >= 5,
    "couldn't parse native build_nvs keys from desktop/src-tauri/src/provisioning.rs");

  // The browser's provisioning: wifi in BOTH schemes native now writes too —
  // string (sense/vision/display) and blob + wifi_en (canary/wap, flash-core's
  // blob branch) — plus the usb-secrets id/broker map. Assert the browser
  // really has the blob-scheme enable key rather than hardcoding trust.
  const flashCoreSrc = read(join(CANARY, "assets/flash-core.js"));
  assert.match(flashCoreSrc, /writeInt\("wifi_en"/,
    "flash-core.js buildNvsSeedImage lost the blob-scheme wifi_en write (canary/wap)");
  assert.match(flashCoreSrc, /writeInt\("setup_ok"/,
    "flash-core.js buildNvsSeedImage lost the blob-scheme setup_ok latch — a seeded " +
    "canary/wap would boot into SETUP MODE despite joining");
  const { mqttProvisioningToNvs, apiTokenToNvs } = await import("../assets/flash-core.js");
  const { strings, u16 } = mqttProvisioningToNvs({
    deviceId: "d", mqttHost: "h", mqttPort: 1, mqttUser: "u", mqttPass: "p",
  });
  // The API-token blobs ride the same seed (blob-scheme boards); their keys
  // are part of the contract the two flashers share.
  const { blobs } = apiTokenToNvs("cv_" + "a".repeat(32));
  const browserKeys = new Set(["wifi_ssid", "wifi_pass", "wifi_en", "setup_ok",
    ...Object.keys(strings), ...Object.keys(u16), ...Object.keys(blobs)]);

  assert.deepStrictEqual([...browserKeys].sort(), [...nativeKeys].sort(),
    "browser vs native provisioning NVS key-sets diverged — reconcile " +
    "flash-core.js:mqttProvisioningToNvs/buildNvsSeedImage/apiTokenToNvs with " +
    "desktop/src-tauri/src/provisioning.rs:build_nvs");
});

test("parity wave 1: safety copy, error kinds, QR, token card, nursery, diagnostics ship on desktop too", () => {
  // The 2026-08 parity inventory's wave 1, gated so it can't regrow: each of
  // these was a browser-only capability the desktop user silently lacked.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const flashCore = read(join(CANARY, "assets/flash-core.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));

  // 1. The automatic pre-flash safety copy — reachable from the ONE-SHOT
  //    flow (onFlash), not just the Advanced rescue button, with the same
  //    failure tolerance the browser applies.
  assert.match(libRs, /fn auto_backup_path/,
    "desktop lost the auto-backup path helper");
  assert.match(appJs, /auto_backup_path/,
    "desktop one-shot flash no longer takes the safety copy first");
  assert.match(html, /id="skip-backup"/,
    "the skip-backup choice must exist (batch operators) — and default to COPYING");
  assert.ok(!/id="skip-backup"[^>]*checked/.test(html),
    "skip-backup must default to unchecked — the safety copy is the default");

  // 2. Structured failure classification: same kinds, same remedies, both
  //    frontends (the browser's classifyFlashError was the reference).
  assert.match(appJs, /function classifyFlashError/,
    "desktop lost classifyFlashError — raw String(e) is not a diagnosis");
  for (const kind of ["port-busy", "device-lost", "permission", "integrity",
                      "download", "not-in-download", "read-stall"]) {
    assert.ok(appJs.includes(`"${kind}"`), `desktop lost the ${kind} error kind`);
    assert.ok(flashCore.includes(`"${kind}"`), `browser lost the ${kind} error kind`);
  }

  // 3. USB bridge driver hints — same vendor table both sides.
  for (const vid of ["0x10c4", "0x1a86", "0x0403"]) {
    assert.ok(appJs.includes(vid), `desktop bridge table lost ${vid}`);
    assert.ok(flashCore.includes(vid), `browser bridge table lost ${vid}`);
  }

  // 4. Wi-Fi QR from the same escaped WIFI: payload.
  assert.match(appJs, /function wifiQrString/, "desktop lost the Wi-Fi QR payload builder");
  assert.match(html, /id="wifi-qr-btn"/, "desktop lost the Wi-Fi QR control");

  // 5. The minted API credential is SHOWN, not only banked in the keychain.
  assert.match(html, /id="token-once"/, "desktop hides the minted API token from its owner");
  assert.match(appJs, /renderTokenOnce/, "desktop token show-once is unwired");

  // 6. The session nursery strip — the batch operator's working memory.
  assert.match(html, /id="nursery-strip"/, "desktop lost the nursery strip");
  assert.match(appJs, /renderNurseryStrip/, "desktop nursery strip is unwired");

  // 7. One-click diagnostic report.
  assert.match(appJs, /function buildDiagnosticReport/, "desktop lost the diagnostic report builder");
  assert.match(html, /id="diag-report-btn"/, "desktop lost the diagnostic report button");

  // 8. The per-setting help layer reads the same generated registry —
  //    with desktop wording where the registry copy names browser mechanics
  //    (the address bar, the Downloads folder) that are false in the app.
  assert.match(appJs, /settings_help/, "desktop help dots no longer read settings_help");
  assert.match(appJs, /HELP_DESKTOP_OVERRIDES/,
    "desktop help lost its overrides — the registry's browser-only wording would show verbatim");

  // 9. Review hardening (Codex on #1502). The safety-copy dedup key is the
  //    MAC or nothing — a USB-port fallback let board B silently inherit
  //    board A's "already copied" through the port they shared.
  assert.ok(!appJs.includes("state.mac || state.port"),
    "desktop backup dedup must never key on the USB port");

  // 10. Clipboard-less diagnostic fallback: the FULL report in a selectable
  //     box, both frontends (the app log truncates; a log line is not a report).
  assert.match(html, /id="diag-report-out"/, "desktop lost the diagnostic fallback box");
  assert.match(appJs, /diag-report-out/, "desktop diagnostic fallback is unwired");
  assert.match(flashJs, /flash-report-ta/, "browser lost its diagnostic fallback textarea");

  // 11. A rendered Wi-Fi QR must not outlive the credentials it encodes:
  //     both frontends clear it when either field changes.
  assert.match(appJs, /wifiQrInvalidate/, "desktop no longer invalidates a stale Wi-Fi QR");
  assert.match(flashJs, /qrClear/, "browser no longer invalidates a stale Wi-Fi QR");
});

test("parity wave 2: the board passport, the install verdict, and 'we've met this board'", () => {
  // Wave 2 of the 2026-08 inventory. The browser has always read the board
  // before writing to it; the desktop app flashed blind — no idea what was
  // resident, so no way to say "this is a downgrade" or "you've flashed this
  // one before". These assertions keep that eyesight.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const healthRs = read(join(ROOT, "desktop/src-tauri/src/health.rs"));
  const flashCore = read(join(CANARY, "assets/flash-core.js"));

  // 1. A connect-time passport command, distinct from the deep health report.
  assert.match(libRs, /fn board_passport/, "desktop lost the board passport command");
  assert.match(appJs, /board_passport/, "desktop passport is never read");
  assert.match(html, /id="passport"/, "desktop has nowhere to show the passport");

  // 2. The booted slot is chosen from otadata, both sides. Reading the wrong
  //    slot silently turns a downgrade into an "update" — the one direction
  //    a user most needs named.
  assert.match(healthRs, /fn pick_booted_app_partition/,
    "desktop lost the booted-slot picker — it would judge by table order");
  assert.match(flashCore, /export function pickBootedAppPartition/,
    "browser lost the booted-slot picker");

  // 3. The install verdict: same kinds, same meaning, both frontends.
  for (const kind of ["fresh", "update", "downgrade", "same", "switch", "unknown"]) {
    assert.ok(appJs.includes(`"${kind}"`), `desktop lost the ${kind} install verdict`);
    assert.ok(flashCore.includes(`"${kind}"`), `browser lost the ${kind} install verdict`);
  }
  assert.match(appJs, /function installVerdict/, "desktop lost installVerdict");
  assert.match(appJs, /function compareVersions/,
    "desktop lost compareVersions — the verdict cannot tell up from down without it");
  assert.match(appJs, /function matchProjectToProduct/,
    "desktop cannot name the resident firmware without the project→product map");

  // 4. Passport history rows, same shape as the browser's.
  assert.match(appJs, /function passportRows/, "desktop lost the passport rows");
  assert.match(flashCore, /export function passportRows/, "browser lost the passport rows");

  // 5. A board the fleet book already knows is announced BEFORE the flash.
  assert.match(appJs, /You've flashed this exact board before/,
    "desktop no longer recognizes a board it has already written");

  // 6. An unreadable board must never render as a blank one — missing
  //    evidence is its own answer, not the cleanest verdict on the page.
  assert.match(appJs, /read failed — NOT a blank board/,
    "desktop must distinguish 'couldn't look' from 'nothing there'");

  // 7. Review hardening (Codex on #1505). The verdict row must not turn an
  //    unread board into "First install" — that contradicts the passport card
  //    directly above it and hides a downgrade.
  assert.match(appJs, /if \(r\.unknown\)/,
    "desktop verdict must answer 'can't tell yet' for an unread board");

  // 8. The flash button waits for the passport, so an install started during
  //    the connect-time read can't bypass the verdict entirely — with a
  //    bounded settle so an unreadable board is still flashable.
  assert.match(appJs, /passportPending/,
    "desktop lost the passport gate — a fast hand could flash with no verdict shown");
  assert.match(appJs, /setTimeout\(\(\) => \{\s*\n?\s*if \(state\.passportPending/,
    "the passport gate must be bounded — 'can't read' must never mean 'can't flash'");

  // 9. The crash row states what was read, not a lifetime claim: an empty
  //    dump region also follows a wipe or a fault that never persisted one.
  for (const [name, src] of [["desktop", appJs], ["browser", flashCore]]) {
    assert.ok(src.includes("no saved crash dump"),
      `${name} lost the honest crash-record wording`);
    assert.ok(!src.includes("never hard-crashed"),
      `${name} claims a crash-free lifetime the probe cannot establish`);
  }
});

test("parity wave 3a: the boot-log verdict and the self-healing baud ladder", async () => {
  // Two self-healing behaviors the browser has had and the desktop hasn't:
  // reading the boot log for signatures that have a specific fix, and walking
  // down the transfer speed when a cable/hub can't hold the fast one.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);

  // 1. Every boot signature the browser knows, the desktop knows — and both
  //    keep the power/firmware split, since telling someone to reinstall
  //    firmware when their USB port can't power the board loops forever.
  assert.match(appJs, /function diagnoseBootLog/, "desktop lost the boot-log diagnosis");
  assert.match(html, /id="boot-diagnosis"/, "desktop has nowhere to show the verdict");
  for (const sig of ["brownout", "panic", "no-app", "flash-error"]) {
    assert.ok(appJs.includes(`"${sig}"`), `desktop lost the ${sig} boot signature`);
  }
  assert.ok(appJs.includes('"power"') && appJs.includes('"clean-install"'),
    "desktop lost the action split — a brownout is not fixed by reflashing");

  // The ported matcher must agree with the browser's on real log text.
  const samples = {
    "Brownout detector was triggered": "brownout",
    "Guru Meditation Error: Core 0 panic'ed": "panic",
    "E (204) esp_image: invalid header: 0xffffffff": "no-app",
    "flash read err, 1000": "flash-error",
    "I (31) boot: ESP-IDF v5.1 2nd stage bootloader": null,
  };
  const desktopDiagnose = new Function(
    appJs.slice(appJs.indexOf("const BOOT_SIGNATURES"), appJs.indexOf("function clearBootDiagnosis")) +
    "return diagnoseBootLog;")();
  for (const [text, want] of Object.entries(samples)) {
    const mine = desktopDiagnose(text);
    const theirs = core.diagnoseBootLog(text);
    assert.strictEqual(mine && mine.signature, want, `desktop misreads: ${text}`);
    assert.strictEqual(theirs && theirs.signature, want, `browser misreads: ${text}`);
  }

  // 2. The ladder: same rungs both sides, and the desktop retries only the
  //    failures a slower speed can actually fix.
  assert.deepStrictEqual(core.FLASH_BAUDS, [921600, 460800, 230400, 115200],
    "the browser's ladder moved — the desktop's copy must move with it");
  for (const rung of core.FLASH_BAUDS) {
    assert.ok(appJs.includes(String(rung)), `desktop ladder lost the ${rung} rung`);
  }
  assert.match(appJs, /BAUD_RETRY_KINDS/,
    "desktop must not retry failures a slower speed cannot fix");
  for (const kind of ["not-in-download", "read-stall", "device-lost"]) {
    assert.ok(appJs.includes(`"${kind}"`), `desktop ladder lost the ${kind} trigger`);
  }
  // A ladder that can empty out would turn a cable fault into a dead end.
  assert.match(appJs, /rungs\.length \? rungs :/,
    "the baud ladder must never be empty — the slowest rung always survives");

  // Review hardening (Codex on #1507).
  // `unknown` must NOT be retried: espflash's exit code alone classifies as
  // unknown, so retrying it would walk a busy port or a denied permission
  // down the whole ladder, repeating the download and the full-chip erase.
  assert.ok(!/BAUD_RETRY_KINDS = new Set\(\[[^\]]*"unknown"/.test(appJs),
    "the baud ladder must not retry `unknown` — it would retry every sidecar failure");
  // …which is only safe because the backend keeps espflash's own words, so a
  // real transport fault can classify as itself.
  assert.match(libRs, /let mut tail: Vec<String>/,
    "espflash's output must survive into the error, or every failure is `unknown`");
  assert.match(libRs, /tail\.join\("\\n"\)/,
    "the espflash error must carry its tail for classification");
  // The ceiling is a per-board remedy. Left set across a swap it would hold a
  // healthy board at the slowest speed for the rest of the session.
  assert.match(appJs, /state\.baudCeiling = null;\s*\n\s*state\.usedBaud = null;/,
    "disconnect must clear the lowered baud ceiling");

  // The proof that the two fixes above actually compose: run the desktop's
  // real classifier over real espflash failures as the backend now reports
  // them, and require that only transport faults are retried. Asserting the
  // retry SET alone would not have caught the original bug — every one of
  // these used to arrive as `unknown`.
  const grabFn = (name) => {
    const i = appJs.indexOf("function " + name);
    let p = 0, j = appJs.indexOf("(", i);
    for (let k = j; k < appJs.length; k++) {
      if (appJs[k] === "(") p++;
      else if (appJs[k] === ")") { p--; if (!p) { j = k; break; } }
    }
    const b = appJs.indexOf("{", j);
    let d = 0;
    for (let k = b; k < appJs.length; k++) {
      if (appJs[k] === "{") d++;
      else if (appJs[k] === "}") { d--; if (!d) return appJs.slice(i, k + 1); }
    }
  };
  const classify = new Function(grabFn("classifyFlashError") + "\nreturn classifyFlashError;")();
  const RETRY = new Set(["not-in-download", "read-stall", "device-lost"]);
  const generic = "espflash exited with code 1. The board can't be bricked — " +
    "put it back in download mode and try again.\n";
  for (const [tail, kind, shouldRetry] of [
    ["Error: Failed to open serial port\nCaused by: Device or resource busy", "port-busy", false],
    ["Error: Permission denied (os error 13)", "permission", false],
    ["Error: Failed to connect to the device\nCaused by: No serial data received", "not-in-download", true],
    ["Error: Serial port disconnected\nCaused by: device not configured", "device-lost", true],
    ["", "unknown", false],
  ]) {
    const got = classify(new Error(generic + tail)).kind;
    assert.strictEqual(got, kind, `espflash tail misclassified: ${tail.slice(0, 40) || "(none)"}`);
    assert.strictEqual(RETRY.has(got), shouldRetry,
      `${kind} must ${shouldRetry ? "" : "NOT "}be retried down the baud ladder`);
  }
});

test("parity wave 3b: room presets are baked in as typed NVS ints", async () => {
  // Vision's detection dials and Sense's radar reflexes are NVS-backed, so
  // seeding them at flash time means a Canary is right for its room on first
  // boot instead of after a trip to Home Assistant.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const provRs = read(join(ROOT, "desktop/src-tauri/src/provisioning.rs"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);

  // 1. The NVS writer can express a u32 at all. Without it the dwell and
  //    every sns_* key would have to be written narrower, which the firmware
  //    reads back as missing — falling silently to its compiled default.
  assert.match(provRs, /fn u32\(&mut self/, "the NVS writer lost its u32 entry type");
  assert.match(provRs, /0x04/, "Preferences putULong is item type 0x04");
  assert.match(provRs, /fn dial_key_ok/,
    "dial keys must be validated — the writer truncates a long key into its neighbor");

  // 2. The UI and the payload exist on the desktop side.
  assert.match(html, /id="dials"/, "desktop has nowhere to show the room presets");
  assert.match(appJs, /function renderDials/, "desktop lost the preset picker");
  assert.match(appJs, /function dialsForFlash/, "desktop preset choice never reaches the flash");
  // "As it ships" must write nothing — seeding the shipped defaults would
  // freeze at flash time a value the maintainer can still change by release.
  assert.match(appJs, /preset\.id === "ships"/,
    "the shipped preset must write nothing at all");
  // Review hardening (Codex on #1508): renderProducts() re-runs when the
  // manifest or the passport lands, re-entering onProductChosen for the
  // selected row. An unconditional reset there discarded a preset picked
  // during those seconds — which is precisely when the user has time to pick
  // one, since Install is disabled until the passport lands.
  assert.match(appJs, /state\.dialChoice\.productId !== product\.id/,
    "a re-render must keep the preset chosen for the same product");
  assert.match(appJs, /\[data-preset="\$\{state\.dialChoice\.presetId\}"\]/,
    "a preserved preset must stay visibly selected, not silently held");

  // 3. The clamping agrees with the browser's, value for value. This is the
  //    part worth testing: an out-of-range dial is a setting the firmware
  //    rejects or misreads, so the two frontends must clamp identically.
  const grab = (name) => {
    const i = appJs.indexOf("function " + name);
    let p = 0, j = appJs.indexOf("(", i);
    for (let k = j; k < appJs.length; k++) {
      if (appJs[k] === "(") p++;
      else if (appJs[k] === ")") { p--; if (!p) { j = k; break; } }
    }
    const b = appJs.indexOf("{", j);
    let d = 0;
    for (let k = b; k < appJs.length; k++) {
      if (appJs[k] === "{") d++;
      else if (appJs[k] === "}") { d--; if (!d) return appJs.slice(i, k + 1); }
    }
  };
  const mod = new Function(grab("detectValuesToNvs") + "\n" + grab("reflexValuesToNvs") +
    "\nreturn {detectValuesToNvs, reflexValuesToNvs};")();

  const vision = catalog.products.find((p) => p.detect);
  assert.ok(vision, "the catalog still carries a product with detection dials");
  for (const preset of vision.detect.presets) {
    assert.deepStrictEqual(
      mod.detectValuesToNvs(preset.values, vision.detect),
      core.detectValuesToNvs(preset.values, vision.detect),
      `detect preset '${preset.id}' clamps differently on the two frontends`);
  }
  // Out-of-range input must be clamped, not passed through, on both sides.
  const wild = { target: 9999, score: -5, lost_ms: 1, dwell_ms: 99_999_999 };
  const mine = mod.detectValuesToNvs(wild, vision.detect);
  assert.deepStrictEqual(mine, core.detectValuesToNvs(wild, vision.detect),
    "out-of-range dials must clamp the same way on both frontends");
  const [scoreLo] = vision.detect.bounds.score;
  assert.strictEqual(mine.u8.det_score, scoreLo, "a below-range score must clamp to the floor");
  assert.ok(mine.u32.det_dwell <= vision.detect.bounds.dwell_ms[1],
    "an above-range dwell must clamp to the ceiling");

  const sense = catalog.products.find((p) => p.reflexes && p.reflexes.nvs);
  if (sense) {
    const values = Object.fromEntries((sense.reflexes.knobs || []).map((k) => [k.id, 999999]));
    assert.deepStrictEqual(
      mod.reflexValuesToNvs(values, sense.reflexes),
      core.reflexValuesToNvs(values, sense.reflexes),
      "reflex clamping differs between the frontends");
  }
});

test("parity wave 3c: the change map answers 'do my settings survive?'", () => {
  // The last wave-3 item. The safety copy already holds every byte on the
  // board and the image is verified before a write — so both sides of the
  // comparison exist for exactly one moment, and that is where this runs.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const cmRs = read(join(ROOT, "desktop/src-tauri/src/changemap.rs"));
  const libRsSrc = read(join(ROOT, "desktop/src-tauri/src/lib.rs"));
  const flashCore = read(join(CANARY, "assets/flash-core.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));

  assert.match(cmRs, /pub fn diff_install/, "desktop lost the install diff");
  assert.match(cmRs, /pub fn settings_verdict/, "desktop lost the settings verdict");
  assert.match(flashCore, /export function diffInstall/, "browser lost the install diff");
  assert.match(flashCore, /export function settingsVerdict/, "browser lost the settings verdict");

  // The four verdicts must all survive on both sides. Collapsing "wiped" into
  // "changed" would bury the one consequence a user needs — that the board
  // comes back up on its setup network.
  for (const v of ["untouched", "identical", "wiped", "changed"]) {
    assert.ok(cmRs.includes(`"${v}"`), `desktop change map lost the '${v}' verdict`);
    assert.ok(flashCore.includes(`"${v}"`), `browser change map lost the '${v}' verdict`);
  }

  // It runs where both sides exist, and never fails the install: a missing or
  // unreadable safety copy means no map, which is the honest answer.
  assert.match(libRsSrc, /flash:changemap/, "the change map is never emitted");
  assert.match(libRsSrc, /backup_path: Option<String>/,
    "flash must accept the safety copy to diff against");
  assert.match(appJs, /flash:changemap/, "desktop never listens for the change map");
  assert.match(appJs, /function renderChangeMap/, "desktop lost the change-map render");
  assert.match(html, /id="change-map"/, "desktop has nowhere to show the change map");
  // A listener per flash would stack across reflashes.
  assert.match(appJs, /unlistenMap\(\)/, "the change-map listener must be released");
  // A previous install's map describes bytes that are no longer true.
  assert.match(appJs, /renderChangeMap\(null\)/, "a new flash must clear the old map");

  // Review hardening (Codex on #1509).
  // 1. Baked Wi-Fi is REPLACED, not cleared. The flasher writes the user's
  //    network into the replacement NVS before the image is staged, so that
  //    region always differs — reading it as "your Wi-Fi is cleared, the
  //    board wants its setup network" is exactly backwards.
  assert.match(cmRs, /baked_wifi: bool/,
    "the settings verdict must know whether we baked the network in");
  assert.match(cmRs, /replaced with the ones you entered/,
    "a provisioned reflash must not claim the saved Wi-Fi was cleared");
  assert.match(libRsSrc, /let baked_wifi = provisioning/,
    "the baked-Wi-Fi fact must reach the verdict");
  // 2. A first-contact erase wipes what the image never reaches, so those
  //    regions must not be reported as surviving.
  assert.match(cmRs, /erase_all: bool/, "the diff must know about the full-chip erase");
  assert.match(libRsSrc, /let erase_all = erase_first/,
    "the erase mode must reach the diff");
  // 3. No map is a thing to SAY, not a panel that quietly disappears.
  assert.match(appJs, /function renderChangeMapUnavailable/,
    "the desktop must explain a missing change map");
  assert.match(appJs, /noMapReason/, "every no-map path must carry a reason");
  assert.match(flashJs, /No change map this time/,
    "the browser says why too — the wording is shared on purpose");
});

test("parity wave 4a: the counterfeit-capacity check the desktop got for free", () => {
  // The 2026-08 wave-4 re-scout's top finding. A relabeled flash part — a
  // 4 MB die sold as 16 MB — ACCEPTS writes past its real end and discards
  // them, so the install reports success and the board cannot boot, with no
  // error at any layer. The safety copy already holds the whole chip in
  // memory before the write, so this costs no serial time at all.
  const intakeRs = read(join(ROOT, "desktop/src-tauri/src/intake.rs"));
  const libRsSrc = read(join(ROOT, "desktop/src-tauri/src/lib.rs"));
  const appJsSrc = read(join(ROOT, "desktop/src/app.js"));
  const intakeJs = read(join(CANARY, "assets/intake.js"));

  assert.match(intakeRs, /pub fn flash_alias_verdict/, "desktop lost the capacity check");
  assert.match(intakeJs, /export function flashAliasVerdict/, "browser lost the capacity check");
  assert.match(intakeRs, /pub fn mac_checks/, "desktop lost the MAC sanity check");
  assert.match(intakeJs, /export function macChecks/, "browser lost the MAC sanity check");

  // Both sides probe the CAPACITIES, not the two ends. Comparing offset zero
  // against `declared - 4K` passes a counterfeit: that address wraps to the
  // top of the real part, whose bytes differ from the head.
  assert.match(intakeRs, /pub fn alias_candidates/,
    "the desktop must probe capacities — an end-to-end compare passes counterfeits");
  assert.match(intakeJs, /export function flashAliasCandidates/,
    "the browser must probe capacities too");

  // A blank chip is inconclusive, never clean: on a blank part a mirror and
  // an honest chip read identically, and missing evidence must not read as a
  // passed check.
  for (const [name, src] of [["desktop", intakeRs], ["browser", intakeJs]]) {
    assert.ok(src.includes("inconclusive"),
      `${name} must not call a blank chip's capacity confirmed`);
  }

  // And it runs where it can still stop the write.
  assert.match(libRsSrc, /intake::flash_alias_verdict/,
    "the capacity check must run before the image is written");
  assert.match(libRsSrc, /Nothing was written/,
    "a counterfeit part must abort the install, not warn after it");

  // Review hardening (Codex on #1510). Only a CLEAR result earns a tick: an
  // inconclusive check is missing evidence, and the module's whole point is
  // that missing evidence must not read as a passed check. The bug was in
  // the presentation, one layer above the careful verdict.
  assert.match(libRsSrc, /if f\.level == "clear"/,
    "only a clear capacity check may show a success marker");

  // The MAC check must actually be CALLED. Asserting a function exists
  // proves nothing if nothing invokes it — this one shipped unwired.
  assert.match(libRsSrc, /intake::mac_checks/,
    "the MAC intake check must run, not merely exist");
  assert.match(appJsSrc, /state\.macCheck/,
    "the MAC finding must reach the user, not stop at the backend");
});

test("the identity challenge is shown, never used as authorization", () => {
  // The fleet book asks each device to sign a fresh nonce and displays the
  // answer. It must NOT gate anything on it: the proof is relay-able (a peer
  // can forward a genuine device's answer), so treating it as permission
  // would authenticate the KEY while saying nothing about the SOCKET that
  // receives a bearer token — worse than the honest "unverified" it replaces.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const whoamiRs = read(join(ROOT, "desktop/src-tauri/src/whoami.rs"));
  const fleetRs = read(join(ROOT, "desktop/src-tauri/src/fleet.rs"));
  const libRsSrc = read(join(ROOT, "desktop/src-tauri/src/lib.rs"));

  // It exists, and it is actually CALLED and REGISTERED — the mac_checks
  // lesson from #1510: a tested function nothing invokes is decoration.
  assert.match(whoamiRs, /pub fn check_answer/, "the verifier is gone");
  assert.match(fleetRs, /pub async fn device_whoami/, "the challenge command is gone");
  assert.match(libRsSrc, /fleet::device_whoami/, "the command is never registered");
  assert.match(appJs, /device_whoami/, "the frontend never asks for a proof");
  assert.match(appJs, /probeIdentity/, "the challenge is never run");

  // The canonical and the nonce gate must match the firmware exactly, or the
  // verifier checks a string the device never signed.
  // The firmware COMPOSES the canonical from constants, so the joined
  // literal never appears in its source — check the parts and, crucially,
  // the field ORDER. device_id before nonce: swap them and every signature
  // still verifies against itself while agreeing with nothing real.
  const wapSig = read(join(ROOT,
    "firmware/projects/canary-wap/arduino/canary_wap/device_signature.cpp"));
  const wapHdr = read(join(ROOT,
    "firmware/projects/canary-wap/arduino/canary_wap/device_signature.h"));
  assert.ok(whoamiRs.includes("securacv-canary-sig|v1|whoami|{device_id}|{nonce}"),
    "desktop canonical drifted — it must be prefix|v1|whoami|<device_id>|<nonce>");
  assert.match(wapHdr, /SIG_PREFIX\s*=\s*"securacv-canary-sig"/,
    "firmware signing prefix moved");
  assert.match(wapHdr, /SCHEMA_V\s*=\s*1\b/, "firmware schema version moved");
  assert.match(wapSig, /"%s\|v%d\|whoami\|%s\|%s"/,
    "firmware whoami canonical shape moved");
  assert.match(wapSig, /whoami\|%s\|%s",\s*\n?\s*SIG_PREFIX, SCHEMA_V,\s*\n?\s*device_id/,
    "firmware must sign device_id BEFORE nonce, as the desktop verifier expects");

  // The fingerprint derivation includes the 0x00 domain separator. Dropping
  // it still compiles and still looks right, and fails against every real
  // board — so it is asserted rather than assumed.
  assert.match(whoamiRs, /update\(\[0x00u8\]\)/,
    "the fingerprint must hash domain || 0x00 || pubkey, as the firmware does");

  // A valid signature from an unexpected key is not proof.
  assert.match(whoamiRs, /WrongKey/, "a foreign key answering must be its own verdict");

  // THE WIRE CONTRACT. Both of these were wrong on the first attempt and
  // neither showed up in any unit test, because the pure verifier was fed
  // synthetic input that never touched the real route or field names. A
  // mismatch fails as "unavailable", which is indistinguishable from old
  // firmware — so it would have looked like a feature nobody had upgraded
  // to yet, forever. Pinned against the firmware's own source.
  const wapIno = read(join(ROOT,
    "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino"));
  const routeMatch = /\.uri\s*=\s*"([^"]+)",\s*\.method\s*=\s*HTTP_GET,\s*\n?\s*\.handler\s*=\s*device_identity_api::handle_enroll_json/
    .exec(wapIno);
  assert.ok(routeMatch, "couldn't find where the firmware registers the enroll JSON handler");
  assert.ok(fleetRs.includes(routeMatch[1]),
    `the desktop must call the route the firmware registers (${routeMatch[1]})`);

  // Every field the verifier reads must be one the firmware actually emits.
  for (const field of ["pubkey_hex", "sig_hex"]) {
    assert.ok(wapSig.includes(`\\"${field}\\":`) || wapSig.includes(`"${field}"`),
      `the firmware does not emit a field named ${field}`);
    assert.ok(fleetRs.includes(`"${field}"`),
      `the desktop must read the firmware's field name ${field}`);
  }

  // And the load-bearing negative: no gating. Identify/Update must not be
  // conditioned on the proof.
  assert.ok(!/proof\s*===\s*"answered"\s*&&[^\n]*canTalk/.test(appJs),
    "the identity proof must not gate device actions");
  assert.ok(!/canTalk\s*&&[^\n]*proof\s*===\s*"answered"/.test(appJs),
    "the identity proof must not gate device actions");
  assert.match(appJs, /SHOWN and never|never\s*\n?\s*\/\/ gating|and never gating/i,
    "the no-gating rule must be stated where the next reader will change it");
});

test("device API token: both flashers mint the same credential shape and seed the same keys", async () => {
  // The credential that makes the desktop fleet book (and any future browser
  // surface) able to talk to a board it flashed: "cv_" + 32 base62 chars,
  // minted with the firmware's own unbiased rejection sampling (reject bytes
  // >= 248, since 248 = 62 * 4 — securacv_crypto.cpp format_api_token_string)
  // and seeded as a BLOB under BOTH firmware key names ("api_token" for the
  // PIO canary, "api_tkn" for the wap). Both loaders check NVS before
  // deriving, so the seeded credential simply becomes the device's.
  const { mintApiToken, apiTokenToNvs, apiTokenShapeOk } = await import("../assets/flash-core.js");

  // The browser's mint: deterministic under crafted bytes, unbiased tail
  // rejected, exactly 32 chars after the prefix.
  const bytes = new Uint8Array(40);
  bytes.fill(255, 0, 8); // the biased tail — every one must be rejected
  for (let i = 8; i < 40; i++) bytes[i] = i - 8; // 0..31 → alphabet[0..31]
  const tok = mintApiToken(bytes);
  assert.match(tok, /^cv_[0-9A-Za-z]{32}$/);
  assert.strictEqual(tok, "cv_0123456789ABCDEFGHIJKLMNOPQRSTUV");
  assert.ok(apiTokenShapeOk(tok));
  assert.ok(!apiTokenShapeOk("cv_short") && !apiTokenShapeOk("xx_" + "a".repeat(32)));

  // Both variant keys, as blobs, same bytes.
  const { blobs } = apiTokenToNvs(tok);
  assert.deepStrictEqual(Object.keys(blobs).sort(), ["api_tkn", "api_token"]);
  assert.ok(blobs.api_token instanceof Uint8Array && blobs.api_token.length === 35);

  // Native writes the SAME two blob keys and validates the same shape.
  const provRs = read(join(ROOT, "desktop/src-tauri/src/provisioning.rs"));
  assert.match(provRs, /writer\.blob\(\s*"api_token"/,
    "provisioning.rs no longer seeds the PIO canary's api_token blob");
  assert.match(provRs, /writer\.blob\(\s*"api_tkn"/,
    "provisioning.rs no longer seeds the wap's api_tkn blob");
  assert.match(provRs, /fn api_token_shape_ok/,
    "provisioning.rs lost the token-shape gate — a malformed seed would be silently ignored by the firmware");

  // Both frontends mint with the same rejection constant — a biased token
  // generator is the kind of drift nobody notices until it matters.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const flashCoreSrc = read(join(CANARY, "assets/flash-core.js"));
  for (const [label, src] of [["desktop app.js", appJs], ["browser flash-core.js", flashCoreSrc]]) {
    assert.ok(/>=\s*248/.test(src),
      `${label} lost the unbiased base62 rejection (>= 248) in its token mint`);
  }
  // The desktop mints at flash time and keeps the credential in the secret
  // drawer; the browser seeds it and shows it once on the done card.
  assert.match(appJs, /apiToken\s*=/, "desktop app.js no longer mints the device API token");
  assert.match(appJs, /secret_set/, "desktop app.js never stores secrets in the OS drawer");
  const flashJs = read(join(CANARY, "assets/flash.js"));
  assert.match(flashJs, /mintApiToken/, "browser flasher no longer mints the device API token");
  assert.match(flashJs, /apiTokenToNvs/, "browser flasher no longer seeds the device API token");
});

test("dev channel: BOTH flashers give the user a control, not just a constant", () => {
  // RELEASE_LESSONS 2026-07-24: copy parity without CAPABILITY parity is worse
  // than divergence. The dev channel had the reverse problem — the browser
  // flasher owned the constant and the fetch, but its ONLY switch was
  // `?channel=dev` in the address bar. The Lab desktop app renders that exact
  // page in a webview with no address bar, so for every Lab user the dev
  // channel existed and could not be turned on. A reachable control on each
  // frontend is the capability; assert the control, not the string.
  const browser = read(join(CANARY, "assets/flash.js"));
  const nativeHtml = read(join(ROOT, "desktop/src/index.html"));

  assert.match(browser, /id\s*=\s*"flash-dev-channel"/,
    "the browser flasher has no dev-channel control — ?channel=dev is unreachable " +
    "inside the Lab app (no address bar). Add the Advanced toggle to canary-local/assets/flash.js");
  assert.match(browser, /function onDevChannelToggle/,
    "canary-local/assets/flash.js has a dev-channel checkbox with nothing behind it");
  assert.match(nativeHtml, /id="dev-channel"/,
    "the desktop Flasher has no dev-channel control — see desktop/src/index.html #adv-dev");

  // Both must re-resolve the manifest when the channel changes; a toggle that
  // leaves the previous channel's versions on screen is the silent-wrong case.
  assert.match(browser, /onDevChannelToggle[\s\S]{0,400}state\.manifest\s*=\s*null/,
    "the browser toggle doesn't drop the loaded manifest — the other channel's " +
    "versions and SHA-256s would stay on the picker rows");
  assert.match(read(join(ROOT, "desktop/src/app.js")), /onDevChannelToggle/,
    "desktop/src/app.js lost its dev-channel handler");
});

test("dev channel: the publishing workflow targets the tag the flashers read", () => {
  // The flashers' DEV_FLASH_MANIFEST_URL and flasher-release.yml's dev channel
  // are two independent spellings of one release. If they drift, the workflow
  // publishes to a release nothing reads and every product stays "unavailable"
  // with no error anywhere — the exact silent failure this file exists to stop.
  const wf = read(join(ROOT, ".github/workflows/flasher-release.yml"));
  const url = read(join(CANARY, "assets/flash-core.js"))
    .match(/DEV_FLASH_MANIFEST_URL[^"]*"(https:\/\/[^"]+)"/);
  assert.ok(url, "couldn't parse DEV_FLASH_MANIFEST_URL from flash-core.js");
  const tag = url[1].match(/\/releases\/download\/([^/]+)\//)[1];

  assert.match(wf, new RegExp(`tag=${tag}\\b`),
    `.github/workflows/flasher-release.yml doesn't publish its dev channel to "${tag}" — ` +
    `the tag the flashers' DEV_FLASH_MANIFEST_URL reads`);
  // The rolling pointer must never become releases/latest: that URL is what
  // every fielded Canary polls for OTA (see .github/actions/keep-firmware-latest).
  assert.match(wf, /prerelease:\s*true/,
    "the dev-channel publish step must mark the release a prerelease, or " +
    "releases/latest can drift off the firmware and the fleet stops seeing updates");
});

test("flasher publishing signs once a key is in force, and refuses to ship a refusable manifest", () => {
  // imageVerificationPolicy() is fail-closed by design: with a REAL pinned
  // release_pubkey, an official manifest carrying no signature returns
  // "require-signature" and both flashers refuse every image in it. So an
  // unsigned publish is correct only while the key is the all-zero
  // placeholder. If this workflow ever publishes unsigned after the ceremony
  // it doesn't degrade — it replaces a good manifest with an uninstallable
  // one, and on the dev channel it would do that on every bring-up run.
  const wf = read(join(ROOT, ".github/workflows/flasher-release.yml"));

  assert.match(wf, /ota_key_state\.py/,
    ".github/workflows/flasher-release.yml must ask whether a release key is " +
    "pinned before deciding to publish unsigned");
  assert.match(wf, /--signing-key/,
    "flasher-release.yml never passes --signing-key to build_flash_manifest.py, " +
    "so every manifest it publishes is unsigned — refusable the moment a real " +
    "key is pinned");
  assert.match(wf, /OTA_SIGNING_KEY_PEM/,
    "flasher-release.yml can't sign without reading the OTA_SIGNING_KEY_PEM secret");
  assert.match(wf, /pip install[^\n]*cryptography/,
    "signing needs `cryptography` (ota_release.py imports it) — install it, or " +
    "the sign path dies after the build instead of before it");

  // The script's own contract, which the above depends on: --signing-key is
  // optional and its absence means checksum-only, not a crash.
  const builder = read(join(ROOT, "firmware/scripts/build_flash_manifest.py"));
  assert.match(builder, /--signing-key/,
    "build_flash_manifest.py lost its --signing-key option");
});

test("Hatchery spec: browser and native draw the whimsy from the SAME hatch.json", () => {
  // The browser fetches devices/hatch.json; the native app embeds it at build
  // time (build.rs). Both must be the one committed canary-local/devices/hatch.json,
  // or the birthing moment (name + certificate) would differ between the surfaces.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  assert.match(flashJs, /fetch\(\s*["']devices\/hatch\.json["']/,
    "browser (flash.js) should fetch devices/hatch.json");
  const buildRs = read(join(ROOT, "desktop/src-tauri/build.rs"));
  assert.match(buildRs, /canary-local\/devices\/hatch\.json/,
    "native (build.rs) should embed canary-local/devices/hatch.json — the same spec the browser fetches");
});

test("offset-0 write guard: both flashers refuse an app-only image before writing 0x0", () => {
  // A merged factory image and an app-only PlatformIO build BOTH open with the
  // 0xE9 image magic, so byte 0 can't tell them apart — the only honest
  // discriminator is the partition table at 0x8000. Anything written from offset
  // 0 without that table lands on the bootloader and the board won't boot. This
  // is a shared SAFETY contract: both surfaces must gate their offset-0 local
  // writes on it, and EVERY native command that does such a write must route
  // through the one guard. (The native rescue bench's write_local_image shipped
  // without it once — a full-flash restore is safe, but "or any .bin" wasn't.)
  const flashCore = read(join(CANARY, "assets/flash-core.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));

  // Browser: the shape check exists, keys on 0x8000, and the local-file picker uses it.
  assert.match(flashCore, /function localImageShape\b/,
    "browser lost flash-core.js:localImageShape — the app-only-build refusal");
  assert.match(flashCore, /localImageShape[\s\S]{0,400}0x8000/,
    "browser localImageShape no longer checks the 0x8000 partition table");
  assert.match(flashJs, /localImageShape/,
    "the browser local-file path (flash.js:onLocalFile) no longer gates on core.localImageShape");

  // Native: the shape check exists and keys on the 0x8000 partition table…
  assert.match(libRs, /fn check_local_image\b/,
    "native lost lib.rs:check_local_image — the app-only-build refusal");
  assert.match(libRs, /check_local_image[\s\S]*?PARTITION_TABLE_OFFSET/,
    "native check_local_image no longer checks the partition table at 0x8000");

  // …and EVERY native command that writes a user-chosen file from offset 0 routes
  // through it. Pin both, so a future 0x0-write path can't skip the gate.
  for (const fn of ["flash_local_file", "write_local_image"]) {
    const body = nativeFnBody(libRs, fn);
    assert.match(body, /check_local_image\s*\(/,
      `native ${fn} writes a local image at 0x0 without calling check_local_image — ` +
      `an app-only .bin would overwrite the bootloader. Call check_local_image(&bytes)? ` +
      `before the write (desktop/src-tauri/src/lib.rs)`);
    // …and it must write the bytes it validated, not re-read the path: a file
    // swapped between the check and espflash's own read would slip unvalidated
    // bytes past the guard. Staging the validated bytes closes that TOCTOU.
    assert.match(body, /stage_firmware\s*\(/,
      `native ${fn} hands the on-disk path to espflash instead of staging the ` +
      `validated bytes — a file changed after the check would bypass the shape/size ` +
      `guards. Stage with stage_firmware(&bytes, …) and write the staged temp.`);
  }
});

test("rescue bench: both flashers can back up, restore, and erase — native wired end-to-end", () => {
  // CLAUDE.md: the two frontends share no UI code, so a rescue capability on one
  // must exist on the other or half the users lose it. The browser Lab has had
  // backup / restore / full-erase for a while; this asserts the native Mac app
  // reached parity AND that its controls are actually wired to the backend
  // commands (a button with nothing behind it is the silent-broken case).
  const html = read(join(ROOT, "desktop/src/index.html"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));

  for (const id of ["rescue-backup-btn", "rescue-restore-btn", "rescue-erase-btn"]) {
    assert.match(html, new RegExp(`id="${id}"`),
      `desktop/src/index.html is missing the rescue control #${id}`);
  }
  for (const fn of ["onRescueBackup", "onRescueRestore", "onRescueErase"]) {
    assert.match(appJs, new RegExp(`function ${fn}\\b`),
      `desktop/src/app.js is missing ${fn} — a rescue button with no handler`);
  }
  // Every rescue command the backend exposes must actually be invoked, or the
  // native bench is decorative. (write_local_image is also checked by the
  // offset-0 guard test above; here we assert it's reachable from the UI.)
  for (const cmd of ["backup_flash", "write_local_image", "erase_chip"]) {
    assert.match(appJs, new RegExp(`invoke\\(\\s*["']${cmd}["']`),
      `desktop/src/app.js never invokes ${cmd} — the native rescue bench is unreachable`);
    assert.match(libRs, new RegExp(`fn ${cmd}\\b`),
      `desktop/src-tauri/src/lib.rs is missing the ${cmd} command the UI calls`);
  }

  // The other direction: the browser must keep the same three capabilities.
  const browser = read(join(CANARY, "assets/flash.js"));
  assert.match(browser, /flash-erase-all/, "browser Lab lost its full-erase control");
  assert.match(browser, /isBackup:\s*true/, "browser Lab lost its restore-a-backup path");
  assert.match(browser, /onRestoreFile|takeBackup|standalone backup/i,
    "browser Lab lost its backup/restore entry points");
});

// ── customs: the unflashed-board posture, on BOTH flashers ──────────────────
// A board bought unflashed arrives running somebody else's firmware. Two
// controls answer that, and CLAUDE.md's "two flashers, two frontends" rule
// means a user-facing safety instruction on one is a bug on the other:
//
//   1. the cold-start gesture — hold BOOT while plugging in, so the resident
//      firmware never executes. It's the only instruction that has to land
//      BEFORE the cable goes in, and no app can substitute for it: the OS
//      finishes enumerating USB before either frontend hears about the device.
//   2. the forced full erase on first contact — a normal write only covers the
//      regions the image occupies, so anything a previous owner left in an
//      untouched partition survives unless the whole chip is erased.
//
// This gate fails the instant one frontend has them and the other doesn't.
test("cold-start guidance ships on both flashers", () => {
  const browser = read(join(CANARY, "assets/flash.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  // The gesture itself, spelled out where the user reads it before plugging in.
  assert.match(browser, /coldStartCard\s*\(/,
    "browser flasher lost the cold-start card (hold BOOT before plugging in)");
  // "the cable", not "the USB-C cable": the classic-ESP32 reach ports are
  // micro-USB (WROOM DevKit) or have no connector at all (ESP32-CAM).
  assert.match(browser, /Still holding it, plug the cable in/,
    "browser flasher's cold-start card no longer states the gesture");
  assert.match(html, /id="coldstart"/,
    "desktop flasher is missing the cold-start card — half the users lose the " +
    "one instruction that has to happen before the cable goes in");
  assert.match(html, /Still holding it, plug the cable in/,
    "desktop flasher's cold-start card no longer states the gesture");

  // The bridge-board escape hatch. A board with no BOOT button (or no USB
  // port) cannot follow the gesture above, and its threat model is different
  // besides — a UART bridge can only ever be a serial port. Both frontends
  // must say so, or half the users are told to hold a button that isn't there.
  for (const [what, src] of [["browser", browser], ["desktop", html]]) {
    assert.match(src, /jumper IO0 to GND before you apply power/,
      `${what} flasher omits the no-BOOT-button gesture for classic ESP32 boards`);
    assert.match(src, /cannot\s+pretend to be a keyboard/,
      `${what} flasher overstates the bridge-board threat`);
  }

  // Both must say plainly that an app cannot intercept USB enumeration —
  // this is the claim we must never let drift into "our app shields you".
  for (const [what, src] of [["browser", browser], ["desktop", html]]) {
    assert.match(src, /No (web page|app) can/i,
      `${what} flasher no longer admits that software can't intercept the plug-in`);
  }
});

test("first contact forces a full erase on both flashers", () => {
  const browser = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  // Browser: decided by reading the board (intake.isFirstContact), and the
  // Advanced checkbox must not be able to turn it back off.
  assert.match(browser, /forcedErase/,
    "browser flasher no longer forces the erase on a first-contact board");
  assert.match(browser, /const eraseOn = forcedErase \|\|/,
    "browser flasher's forced erase can be overridden by the Advanced toggle — " +
    "the force must win over the checkbox, not the other way round");

  // Desktop: espflash can't report resident firmware, so it asks — but it must
  // ask, must default to erasing, and must actually pass the answer through.
  assert.match(html, /id="first-contact"[^>]*checked/,
    "desktop flasher's first-contact erase must default to ON — the safe default " +
    "for a board of unknown provenance is to wipe it");
  assert.match(appJs, /eraseFirst:/,
    "desktop flasher never passes eraseFirst to the native flash command");
  assert.match(libRs, /erase_first/,
    "desktop/src-tauri/src/lib.rs ignores erase_first — the checkbox does nothing");
  assert.match(libRs, /erase_first\.unwrap_or\(false\)/,
    "erase_first must fail closed to 'no erase' only when explicitly absent");

  // The safe default has to be restored for EVERY board, not once per app
  // launch. Unticking it to reflash a known Canary must not carry over to the
  // next board plugged in — which is exactly the marketplace board that needs
  // the wipe.
  assert.match(appJs, /function resetSteps\b[\s\S]*?first-contact"\)\.checked = true/,
    "desktop resetSteps must re-arm the first-contact erase for each attached " +
    "board, or an untick leaks from one board to the next");
});

test("the board's own firmware claim never waives the first-contact erase", () => {
  // A board bought unflashed is untrusted, and its esp_app_desc_t project name
  // lives in writable flash — so an image that calls itself a known SecuraCV
  // product must not thereby skip the erase that would remove it. Only our own
  // session roster or an explicit human claim may waive it.
  const intakeJs = read(join(CANARY, "assets/intake.js"));
  const body = /export function isFirstContact\(([\s\S]*?)\n}/.exec(intakeJs);
  assert.ok(body, "isFirstContact vanished from canary-local/assets/intake.js");
  assert.ok(!/current|productName|projectName/.test(body[1]),
    "isFirstContact reads the board's own firmware claim again — that string is " +
    "attacker-controlled on an untrusted board, so it cannot gate the erase");
  assert.match(body[1], /rosterHit/, "our own session history must still waive it");
  assert.match(body[1], /ownerClaimed/, "an explicit human claim must still waive it");
});

test("the eFuse gap between the two flashers is stated, not hidden", () => {
  // The browser reads the chip's security fuses; espflash has no fuse-read
  // command, so the desktop app genuinely cannot. That's acceptable — silently
  // omitting it is not, because a missing check reads as a passed check.
  const browser = read(join(CANARY, "assets/flash.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  assert.match(browser, /efuseBlock0Addrs|readSecurityEfuses/,
    "browser flasher lost its security-fuse read");
  assert.match(html, /id="coldstart-efuse"/,
    "desktop flasher must say the fuse check is browser-only rather than leave " +
    "the user assuming it ran");
  assert.match(html, /browser-only/,
    "desktop flasher's fuse-gap note no longer names the gap");
});

test("health check: native parsers pin the browser's byte-magics, and the UI reaches the command", () => {
  // The native health parsers (health.rs) reimplement the browser's flash-core
  // parsers. Their magics/offsets/namespaces MUST match byte-for-byte, or the
  // same chip reads differently on each surface. Pin the shared constants.
  const healthRs = read(join(ROOT, "desktop/src-tauri/src/health.rs"));
  const flashCore = read(join(CANARY, "assets/flash-core.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  const hexOf = (src, re, what) => {
    const m = src.match(re);
    assert.ok(m, `couldn't read ${what} from source`);
    return parseInt(m[1].replace(/_/g, ""), 16); // Rust allows 0xabcd_5432
  };
  assert.strictEqual(
    hexOf(healthRs, /PARTITION_MAGIC:\s*u16\s*=\s*(0x[0-9a-fA-F_]+)/, "health.rs PARTITION_MAGIC"),
    hexOf(flashCore, /PARTITION_MAGIC\s*=\s*(0x[0-9a-fA-F]+)/, "flash-core PARTITION_MAGIC"),
    "partition-table magic drifted between health.rs and flash-core.js");
  assert.strictEqual(
    hexOf(healthRs, /APP_DESC_MAGIC:\s*u32\s*=\s*(0x[0-9a-fA-F_]+)/, "health.rs APP_DESC_MAGIC"),
    hexOf(flashCore, /APP_DESC_MAGIC\s*=\s*(0x[0-9a-fA-F]+)/, "flash-core APP_DESC_MAGIC"),
    "app-descriptor magic drifted");
  assert.strictEqual(
    hexOf(healthRs, /APP_DESC_OFFSET:\s*u32\s*=\s*(0x[0-9a-fA-F_]+)/, "health.rs APP_DESC_OFFSET"),
    hexOf(flashCore, /APP_DESC_OFFSET\s*=\s*(0x[0-9a-fA-F]+)/, "flash-core APP_DESC_OFFSET"),
    "app-descriptor offset drifted");
  for (const [reRs, reJs, what] of [
    [/WITNESS_NVS_NAMESPACE:\s*&str\s*=\s*"([^"]+)"/, /WITNESS_NVS_NAMESPACE\s*=\s*"([^"]+)"/, "witness namespace"],
    [/WITNESS_CHAIN_BLOB_KEY:\s*&str\s*=\s*"([^"]+)"/, /WITNESS_CHAIN_BLOB_KEY\s*=\s*"([^"]+)"/, "witness chain key"],
  ]) {
    const a = healthRs.match(reRs), b = flashCore.match(reJs);
    assert.ok(a && b, `couldn't read ${what} from both sources`);
    assert.strictEqual(a[1], b[1], `${what} drifted between health.rs and flash-core.js`);
  }

  // Both surfaces keep the health check + its parsers.
  assert.match(flashJs, /function runHealthCheck\b/, "browser Lab lost runHealthCheck");
  for (const fn of ["parsePartitionTable", "parseAppDescriptor", "parseOtaData", "parseCoredumpHeader", "witnessSummary"]) {
    assert.match(flashCore, new RegExp(`function ${fn}\\b`), `browser flash-core lost ${fn}`);
  }
  for (const fn of ["parse_partition_table", "parse_app_descriptor", "parse_ota_data", "parse_coredump_header", "witness_summary", "report_verdict"]) {
    assert.match(healthRs, new RegExp(`fn ${fn}\\b`), `native health.rs lost ${fn}`);
  }

  // The native command exists and the UI actually reaches it.
  assert.match(libRs, /fn health_check\b/, "native lib.rs lost the health_check command");
  assert.match(html, /id="health-check-btn"/, "index.html lost the Health check button");
  assert.match(appJs, /function onHealthCheck\b/, "app.js lost the onHealthCheck handler");
  assert.match(appJs, /invoke\(\s*["']health_check["']/,
    "app.js never invokes health_check — the health button is dead");
});

test("radar tuning suite: BOTH flashers wire every knob to the serial tuning console", () => {
  // CLAUDE.md's two-flashers rule, applied to the Sense tuning bench: a live
  // knob added to one frontend must exist on the other, or half the users
  // keep the read-only bench. Both frontends speak the SAME wire words
  // (`set <knob> <value>` / `reset` / `stream …` / `raw on|off`) from the
  // SAME catalog source (each knob's `console` name, gen_flash.py), and both
  // reconcile only to the firmware's `[cfg]` snapshot line — never to their
  // own optimistic slider state.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const flashCore = read(join(CANARY, "assets/flash-core.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  // The catalog carries a console token for every Sense knob — the one
  // vocabulary both frontends bind their sliders to.
  for (const p of catalog.products.filter((x) => x.role === "sense")) {
    for (const k of p.reflexes.knobs) {
      assert.ok(k.console && /^[a-z_]+$/.test(k.console),
        `${p.id}: knob ${k.id} has no console token — the tuning suites can't reach it`);
    }
  }
  const wellbeing = catalog.products.find((p) => p.id === "securacv-canary-sense-wellbeing");
  const tokens = wellbeing.reflexes.knobs.map((k) => k.console);
  for (const t of ["debounce", "clear", "stall", "near", "mid",
                   "vlock", "vlost", "breath_min", "breath_max", "heart_min", "heart_max"]) {
    assert.ok(tokens.includes(t),
      `wellbeing catalog lost console knob "${t}" — regen devices/flash.json (gen_flash.py)`);
  }

  // Browser: the bench builds sliders from the catalog and speaks the console.
  assert.match(flashJs, /sendCmd\(`set \$\{k\.console\} \$\{input\.value\}`\)/,
    "browser bench sliders no longer send `set <knob> <value>`");
  for (const cmd of ['sendCmd("reset")', '"raw on"', '"stream off"', 'sendCmd("cfg")']) {
    assert.ok(flashJs.includes(cmd), `browser bench lost the ${cmd} control`);
  }
  for (const fn of ["parseCfgLine", "parseTuneLine", "senseLineTone"]) {
    assert.match(flashCore, new RegExp(`export function ${fn}\\b`),
      `flash-core.js lost ${fn} — the bench can't read the tuning console`);
  }

  // Native: the monitor carries the same panel, same words, same [cfg] sync.
  assert.match(html, /id="sense-tune"/, "desktop index.html lost the radar tuning panel");
  assert.match(html, /id="sense-knobs"/, "desktop index.html lost the knob grid");
  assert.match(appJs, /function renderSenseTune\b/, "desktop app.js lost renderSenseTune");
  assert.match(appJs, /function parseSenseCfgLine\b/,
    "desktop app.js lost parseSenseCfgLine — the panel can't sync to [cfg]");
  assert.match(appJs, /sendTune\(`set \$\{k\.console\} \$\{input\.value\}`\)/,
    "desktop sliders no longer send `set <knob> <value>`");
  for (const cmd of ['sendTune("reset")', '"raw on"', '"stream off"', 'sendTune("cfg")']) {
    assert.ok(appJs.includes(cmd), `desktop tuning panel lost the ${cmd} control`);
  }
});

test("self-update: each app's updater endpoint names the pointer its workflow advances", () => {
  // A self-updating app and the workflow that feeds it name the same rolling
  // release in two different files — two chances to be wrong, and the failure
  // mode (RELEASE_LESSONS 2026-07-27/28) is an installed base that polls a URL
  // nothing serves, forever. So: the endpoint tag must be the exact tag the
  // publishing workflow re-points, for BOTH apps, and neither may ever be
  // `releases/latest` — that URL belongs to the firmware the fleet polls.
  const flasherConf = JSON.parse(read(join(ROOT, "desktop/src-tauri/tauri.conf.json")));
  const labConf = JSON.parse(read(join(ROOT, "desktop-lab/src-tauri/tauri.conf.json")));
  const flasherWf = read(join(ROOT, ".github/workflows/desktop-flasher-release.yml"));
  const labPointerWf = read(join(ROOT, ".github/workflows/desktop-lab-updater-pointer.yml"));

  const endpointTag = (conf, app) => {
    const eps = (((conf.plugins || {}).updater || {}).endpoints) || [];
    assert.strictEqual(eps.length, 1, `${app}: expected exactly one updater endpoint`);
    const m = eps[0].match(/\/releases\/download\/([^/]+)\/latest\.json$/);
    assert.ok(m, `${app}: endpoint isn't a <tag>/latest.json release download: ${eps[0]}`);
    assert.notStrictEqual(m[1], "latest",
      `${app}: updater endpoint rides releases/latest — that URL belongs to the firmware`);
    return m[1];
  };

  const flasherTag = endpointTag(flasherConf, "flasher");
  assert.strictEqual(flasherTag, "flasher-latest",
    "flasher updater endpoint moved off flasher-latest — stranded installs poll the old tag");
  for (const cmd of [`gh release view ${flasherTag}`, `gh release upload ${flasherTag}`]) {
    assert.ok(flasherWf.includes(cmd),
      `desktop-flasher-release.yml no longer advances the ${flasherTag} pointer (${cmd})`);
  }

  const labTag = endpointTag(labConf, "lab");
  assert.strictEqual(labTag, "lab-latest",
    "lab updater endpoint moved off lab-latest — stranded installs poll the old tag");
  for (const cmd of [`gh release view ${labTag}`, `gh release upload ${labTag}`]) {
    assert.ok(labPointerWf.includes(cmd),
      `desktop-lab-updater-pointer.yml no longer advances the ${labTag} pointer (${cmd})`);
  }

  // The two pointers must differ, or one app's publish would feed the other
  // app's updater a manifest for the wrong product.
  assert.notStrictEqual(flasherTag, labTag, "both apps poll the same updater pointer");

  // One repo signing secret (TAURI_SIGNING_PRIVATE_KEY) signs both apps'
  // updater artifacts, so both must embed the same public key — a divergent
  // pubkey means one app rejects every update it's offered.
  const pk = (c) => ((c.plugins || {}).updater || {}).pubkey;
  assert.ok(pk(flasherConf), "flasher lost its updater pubkey");
  assert.strictEqual(pk(flasherConf), pk(labConf),
    "the two apps embed different updater pubkeys — one of them can't verify updates");

  // And both must actually produce updater artifacts, or the pointer serves
  // a manifest with nothing behind it.
  assert.strictEqual(flasherConf.bundle.createUpdaterArtifacts, true,
    "flasher no longer builds updater artifacts");
  assert.strictEqual(labConf.bundle.createUpdaterArtifacts, true,
    "lab no longer builds updater artifacts");
});

test("hub first-boot watch: the escalation countdown is wired on both paths", () => {
  // The 25-minute troubleshooting escalation is visible-in-advance: a
  // countdown painted from the SAME deadline the watch checks, on the live
  // first-boot panel AND the resumed-after-relaunch banner. Losing either
  // wiring re-creates the tips-appear-from-nowhere surprise (or, on the
  // resumed path, the stranded-user case from the Codex review).
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  assert.match(appJs, /function hubCountdownStart\b/,
    "desktop/src/app.js lost hubCountdownStart");
  assert.match(appJs, /HUB_FB_ESCALATE_MS\s*-\s*Date\.now\(\)/,
    "the countdown no longer derives from HUB_FB_ESCALATE_MS — it can drift " +
    "from the deadline the escalation check actually uses");
  assert.match(appJs, /hubCountdownStart\(\$\("hub-fb-count"\),\s*t0\)/,
    "the live first-boot watch no longer starts the countdown");
  assert.match(appJs, /hubCountdownStart\(\$\("hub-resume-count"\),\s*rec\.at\)/,
    "the resumed watch no longer starts the countdown from the persisted " +
    "flash time (rec.at)");

  for (const id of ["hub-fb-count", "hub-resume-count"]) {
    assert.match(html, new RegExp(`id="${id}"`),
      `desktop/src/index.html is missing the countdown element #${id}`);
  }
});

test("board access notes: both flashers explain the radar's hidden flashing port", () => {
  // The Sense is two boards — a radar carrier with a XIAO seated in it — and
  // the port you can reach is NOT the one that flashes. Plug into the wrong
  // one and the device list stays empty, which reads as "dead board". The
  // truth lives once in the catalog (products[].access) and each frontend
  // renders it in its CONNECT step, before the cable goes in; this pins all
  // three so a future edit can't leave half the users with the vague version.
  const catalog = JSON.parse(read(join(CANARY, "devices/flash.json")));
  const sense = catalog.products.filter((p) => p.family === "sense");
  assert.ok(sense.length >= 1, "no sense products in the catalog");
  for (const p of sense) {
    assert.ok(p.access, `${p.id} lost its access block (gen_flash.py BOARD_ACCESS)`);
    for (const k of ["headline", "flash_port", "other_port", "other_effect", "steps"]) {
      assert.ok(p.access[k], `${p.id}.access is missing ${k}`);
    }
    assert.match(p.access.flash_port, /XIAO/,
      `${p.id}.access.flash_port must name the XIAO — the board that actually flashes`);
    assert.ok(Array.isArray(p.access.steps) && p.access.steps.length >= 2,
      `${p.id}.access.steps should walk through opening the case`);
  }

  const browser = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  assert.match(browser, /function accessCards\(\)/,
    "canary-local/assets/flash.js lost accessCards()");
  assert.match(browser, /accessCards\(\)[\s\S]{0,200}box\.append/,
    "the browser flasher no longer renders the access cards into the connect phase");
  assert.match(appJs, /function renderAccessNotes\(\)/,
    "desktop/src/app.js lost renderAccessNotes()");
  assert.match(appJs, /renderAccessNotes\(\)/g,
    "desktop/src/app.js never calls renderAccessNotes()");
  assert.match(html, /id="access-notes"/,
    "desktop/src/index.html is missing the #access-notes host in the connect step");

  // Both must read it from the catalog rather than hardcoding board copy —
  // that is what keeps them from drifting apart.
  for (const [label, src] of [["browser", browser], ["desktop", appJs]]) {
    assert.match(src, /\.access\b/, `${label} flasher doesn't read products[].access`);
    assert.ok(!/radar carrier's socket/.test(src),
      `${label} flasher hardcodes access copy that belongs in gen_flash.py's BOARD_ACCESS`);
  }
});

// ── broker credentials must reach every firmware that reads them ─────────────
//
// The displays could not be told which hub to talk to. Both flashers gated the
// MQTT fields on `provisioning === "usb-secrets"`, and every display is
// `on-glass` — yet canary-display's runtime_config.h carries mqtt_host / port /
// user / pass and mqtt_mgr.cpp reads them on every boot. The desktop app went
// further and hard-coded empty strings into the seed. So the fields were hidden
// in the app, absent from the on-glass portal, and blank in NVS: three surfaces
// agreeing on a value the user was never asked for.
//
// That is the dangerous shape — not a crash, just a board that quietly never
// reaches its broker. These pin the capability as a capability: read from each
// firmware's own source by gen_flash.py, never a hand-kept list, and never
// conflated with how identity happens to be provisioned.

// The catalog doesn't carry `project` (it stays internal to gen_flash.py), so
// map the family to the tree the firmware actually lives in.
const FAMILY_PROJECT = {
  canary: "firmware/canary",
  wap: "firmware/projects/canary-wap",
  vision: "firmware/projects/canary-vision",
  sense: "firmware/projects/canary-sense",
  display: "firmware/projects/canary-display",
};

test("every firmware whose runtime_config reads a broker is tagged broker_nvs", () => {
  for (const p of catalog.products) {
    const proj = FAMILY_PROJECT[p.family];
    assert.ok(proj, `${p.id}: unmapped family "${p.family}" — add it above`);
    const rc = join(ROOT, proj, "include/canary/runtime_config.h");
    let reads = false;
    try {
      const text = read(rc);
      reads = text.includes("mqtt_host") && text.includes("mqtt_user");
    } catch { reads = false; }
    assert.equal(
      p.broker_nvs === true, reads,
      `${p.id}: catalog says broker_nvs=${p.broker_nvs} but its runtime_config ` +
      `${reads ? "DOES" : "does not"} read mqtt_host/mqtt_user. Regenerate with ` +
      `canary-local/tools/gen_flash.py — never hand-edit devices/flash.json.`
    );
  }
});

test("displays can be given a broker — the case that was impossible", () => {
  const displays = catalog.products.filter((p) => p.id.includes("display"));
  assert.ok(displays.length >= 7, "expected the display line in the catalog");
  for (const d of displays) {
    assert.equal(d.provisioning, "on-glass", `${d.id} is on-glass`);
    assert.equal(d.broker_nvs, true,
      `${d.id} reads a broker from NVS, so both flashers must offer those fields`);
  }
});

test("neither flasher gates the broker fields on the provisioning mode", () => {
  // The exact regression: `prov === "usb-secrets"` deciding whether a broker
  // can be entered. Capability and provisioning are different questions.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  assert.ok(/broker_nvs/.test(appJs),
    "desktop app must gate broker fields on the broker_nvs capability");
  assert.ok(/broker_nvs/.test(flashJs),
    "browser flasher must gate broker fields on the broker_nvs capability");
  assert.ok(!/mqttHost:\s*usbSecrets\s*\?/.test(appJs),
    "desktop app must not blank mqttHost for non-usb-secrets boards");
});

test("no surface ever summons the OS password generator for an existing secret", () => {
  // The lesson (firmware/LESSONS_LEARNED.md "iOS offers to invent a password"):
  // a Wi-Fi key or broker secret is an EXISTING credential. `type="password"`
  // (worse, autocomplete="new-password") reads as sign-up to the OS, which then
  // offers to GENERATE a key no router has ever seen — and real users accepted
  // it. House pattern: type="text" + .pw-masked + autocomplete="off", with a
  // Show/Hide that flips the class, never the type. The ONLY legitimate
  // new-password fields are the hub's Home Assistant account creation pair
  // (hub-acct-pass / hub-acct-pass2) — those really are new accounts.
  const indexHtml = read(join(ROOT, "desktop/src/index.html"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const wapUi = read(join(CANARY, "assets/wap-ui.js"));

  const newPwCount = (indexHtml.match(/autocomplete="new-password"/g) || []).length;
  assert.equal(newPwCount, 2,
    'desktop/src/index.html: autocomplete="new-password" is allowed ONLY on the ' +
    "two hub account-creation fields — a Wi-Fi/broker secret must use the " +
    ".pw-masked text pattern instead");
  for (const id of ["wifi-pass", "mqtt-pass", "hub-pass"]) {
    const field = indexHtml.match(new RegExp(`<input id="${id}"[^>]*>`));
    assert.ok(field, `desktop/src/index.html: missing #${id}`);
    assert.ok(!/type="password"/.test(field[0]) && /pw-masked/.test(field[0]),
      `#${id} must be a masked text input (class="pw-masked"), never type="password"`);
    assert.ok(/autocomplete="off"/.test(field[0]), `#${id} must set autocomplete="off"`);
  }

  assert.ok(!/new-password/.test(flashJs),
    'browser flasher must never mark a field autocomplete="new-password"');
  assert.ok(/pass\.classList\.add\("pw-masked"\)/.test(flashJs),
    "browser flasher Wi-Fi key must use the .pw-masked text pattern");
  assert.ok(!/pass\.type\s*=\s*pass\.type\s*===\s*"password"/.test(flashJs),
    "browser flasher Show toggle must flip the masking class, never input.type");
  assert.ok(!/type\s*=\s*"password"/.test(wapUi),
    "the WAP portal simulator renders real DOM — its password field must be masked text");

  // Both frontends keep the class-flip rule in the toggle handlers.
  assert.ok(/classList\.toggle\("pw-masked"\)/.test(appJs),
    "desktop Show/Hide must flip the pw-masked class");
});

test("WE2 live bench: both flashers speak the same stream protocol, sliders read back", () => {
  // The bench is how a Vision module gets aimed and tuned without a reflash.
  // Its protocol is a shared contract: continuous `INVOKE=-1,0,0` starts the
  // frame stream, `BREAK` ends it, and the two on-module thresholds are set
  // AND read back (`TSCORE`/`TIOU` then `TSCORE?`/`TIOU?`) so a slider shows
  // what the module actually holds, never what the UI hoped. CLAUDE.md's
  // two-flashers rule: the browser has had this bench; the native app now has
  // it too, and neither side may drop or drift the protocol.
  const we2FlashJs = read(join(CANARY, "assets/we2-flash.js"));
  const benchRs = read(join(ROOT, "desktop/src-tauri/src/we2_bench.rs"));
  const sscmaRs = read(join(ROOT, "desktop/src-tauri/src/sscma.rs"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  // The protocol strings, pinned on BOTH surfaces.
  for (const [label, src] of [["browser we2-flash.js", we2FlashJs], ["native we2_bench.rs", benchRs]]) {
    assert.ok(src.includes("INVOKE=-1,0,0"),
      `${label} lost the continuous-invoke start (INVOKE=-1,0,0)`);
    assert.ok(src.includes("BREAK"), `${label} lost the BREAK stop`);
  }
  for (const [label, src] of [["browser we2-flash.js", we2FlashJs], ["native app.js", appJs]]) {
    for (const knob of ["TSCORE", "TIOU"]) {
      assert.ok(src.includes(knob), `${label} lost the ${knob} threshold`);
    }
  }
  // Read-back after a set — the "slider never lies" rule, on both sides.
  assert.match(we2FlashJs, /cmd\s*\+\s*"\?"/,
    "browser bench no longer reads a threshold back after setting it");
  assert.match(appJs, /body:\s*cmd\s*\+\s*"\?"/,
    "native bench no longer reads a threshold back after setting it");

  // Native wiring end-to-end: pure framing module → commands → UI.
  assert.match(sscmaRs, /pub struct FrameScanner\b/,
    "native lost the host-tested SSCMA frame scanner (sscma.rs)");
  for (const cmd of ["we2_bench_start", "we2_bench_cmd", "we2_bench_stop"]) {
    assert.match(benchRs, new RegExp(`pub (async )?fn ${cmd}\\b`),
      `desktop/src-tauri/src/we2_bench.rs lost the ${cmd} command`);
    assert.match(libRs, new RegExp(`we2_bench::${cmd}\\b`),
      `desktop/src-tauri/src/lib.rs no longer registers ${cmd}`);
    assert.match(appJs, new RegExp(`invoke\\(\\s*["']${cmd}["']`),
      `desktop/src/app.js never invokes ${cmd} — the bench UI is dead`);
  }
  for (const id of ["bench-start", "bench-stop", "bench-tscore", "bench-tiou", "bench-canvas"]) {
    assert.match(html, new RegExp(`id="${id}"`),
      `desktop/src/index.html is missing the bench control #${id}`);
  }

  // Label honesty on both surfaces: "person" only when the module's own model
  // card pins that class — an unknown model's boxes must read "object".
  assert.match(we2FlashJs, /classes\.includes\("person"\)/,
    "browser bench no longer checks the model card for the person class");
  assert.match(benchRs, /Some\("person"\)/,
    "native bench no longer checks the model card for the person class");
});

test("pre-configured Wi-Fi is honored: present-but-empty keys, and identity never gates the broker", () => {
  // The bring-up contract behind "bake Wi-Fi in and it just joins":
  //   1. FIRMWARE side — a seeded key that EXISTS is the answer, even empty.
  //      The flashers seed an empty wifi_pass for an open network; a loader
  //      that treats empty as absent substitutes the compiled ci-placeholder
  //      and the join fails with a password no router has ever seen. Every
  //      string-scheme loader must gate its fallback on Preferences::isKey.
  //   2. WRITER side — both flashers must WRITE that empty key (skipping it
  //      recreates the same bug through the other door), and neither may tie
  //      the broker to a device id: a display is told a broker with no
  //      identity at all (PR #1351's capability split, finished).
  const loaders = [
    "firmware/projects/canary-display/src/runtime_config.cpp",
    "firmware/projects/canary-display/arduino/canary_display/runtime_config.cpp",
    "firmware/projects/canary-vision/src/runtime_config.cpp",
    "firmware/projects/canary-sense/src/runtime_config.cpp",
  ];
  for (const path of loaders) {
    const src = read(join(ROOT, path));
    assert.match(src, /prefs\.isKey\(key\)/,
      `${path}: load_credential no longer distinguishes present-but-empty from absent — ` +
      "an open network's seeded empty password would be replaced by the compiled placeholder");
    // 1b. A blob under the same key is honored too: isKey() is type-blind
    //     and getString() on a blob reads "" — a blob-scheme seed (a stale
    //     flasher frontend from before the per-product scheme plumbing)
    //     otherwise re-raises onboarding over perfectly good credentials.
    assert.match(src, /prefs\.getBytesLength\(key\)/,
      `${path}: load_credential no longer falls back to a blob-scheme seed — ` +
      "a blob-seeded board would read empty credentials and boot into setup");
  }

  // Both writers write the wifi_pass key unconditionally inside the wifi block.
  const flashCoreSrc = read(join(CANARY, "assets/flash-core.js"));
  assert.match(flashCoreSrc, /writeString\("wifi_pass", passB\)/,
    "flash-core.js no longer writes the (possibly empty) wifi_pass string key");
  const provRs = read(join(ROOT, "desktop/src-tauri/src/provisioning.rs"));
  assert.match(provRs, /writer\.string\("wifi_pass", &config\.wifi_pass\)\?/,
    "provisioning.rs no longer writes the (possibly empty) wifi_pass string key");

  // Native: identity and broker validated/written independently — the old
  // wifi_only() coupling aborted a display's whole flash (Wi-Fi included)
  // over a device-id field its UI deliberately hides.
  assert.ok(!/fn wifi_only\(/.test(provRs),
    "provisioning.rs regrew the wifi_only() coupling of device id + broker");
  assert.match(provRs, /if !config\.device_id\.is_empty\(\)/,
    "provisioning.rs must validate/write dev_id only when present");
  assert.match(provRs, /if !config\.mqtt_host\.is_empty\(\)/,
    "provisioning.rs must validate/write the broker only when present");

  // Broker-only provisioning must build: a display kept on its on-glass
  // Wi-Fi setup can still be told its hub here. An unconditional SSID check
  // made that abort ("Wi-Fi name must be 1–32 bytes") — the Wi-Fi block must
  // be as optional as the others.
  assert.match(provRs, /if !config\.wifi_ssid\.is_empty\(\)/,
    "provisioning.rs must treat Wi-Fi itself as optional (broker-only flashes)");

  // BOTH flashers suggest a UNIQUE per-device id for every broker-capable
  // board — displays included. Their MQTT topics derive from dev_id, the
  // glass setup only ever asks for Wi-Fi, and with nothing seeded first boot
  // persists the flavor's SHARED compiled id (canary_dash_001) — so two
  // same-flavor displays collide on the same topics. The id must be visible
  // and clearable, and the family map must know displays.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  assert.match(flashJs,
    /(provisioning === "usb-secrets" \|\| product\.broker_nvs === true)[\s\S]{0,400}canary_display/,
    "flash.js no longer suggests a unique dev_id for broker-capable displays — " +
    "same-flavor displays would share the compiled id and collide on MQTT");
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  assert.match(appJs, /\(usbSecrets \|\| broker\) && !\$\("device-id"\)\.value/,
    "app.js no longer suggests a unique dev_id for broker-capable displays");
  assert.match(appJs, /includes\("display"\)[\s\S]{0,80}canary_display/,
    "app.js dev_id family map lost the display family");
  assert.match(appJs, /deviceId: usbSecrets \|\| broker \?/,
    "app.js readProvisioning no longer sends the display's device id");
});

test("witness wall: both apps discover the LAN fleet the same way, one emulator, one contract", () => {
  // The Fleet surface must be the SAME product in the Flasher and the Lab —
  // one vendored emulator, one witness_discover command, one postMessage
  // contract (tvos/EMBED_IN_APPS.md). If any of these drift, half the users
  // get a wall that behaves differently, with no compile error to say so.
  const labRs = read(join(ROOT, "desktop-lab/src-tauri/src/lib.rs"));
  for (const [name, src] of [["desktop", libRs], ["desktop-lab", labRs]]) {
    assert.match(src, /async fn witness_discover\(bases: Vec<String>\)/,
      `${name} lost the witness_discover command`);
    assert.match(src, /\/api\/fleet/,
      `${name}'s witness_discover no longer probes the /api/fleet contract`);
    assert.ok(src.split("witness_discover").length >= 3,
      `${name} defines witness_discover but never registers it in the invoke handler`);
  }
  // Both frontends drive the wall over the same wire: native discovery in,
  // witness:fleet out.
  const flasherHost = read(join(ROOT, "desktop/src/app.js"));
  const labHost = read(join(CANARY, "assets/witness-host.js"));
  for (const [name, src] of [["Flasher app.js", flasherHost], ["Lab witness-host.js", labHost]]) {
    assert.match(src, /invoke\("witness_discover"/, `${name} no longer invokes witness_discover`);
    assert.match(src, /witness:fleet/, `${name} no longer posts witness:fleet to the wall`);
    assert.match(src, /witness:ready/, `${name} no longer syncs on the wall's witness:ready handshake`);
  }
  // And the vendored emulator is the same bytes on both surfaces — the
  // byte-level guard is scripts/check_witness_emulator_sync.sh; this is the
  // fast in-gate echo of it for the file that carries the behavior.
  const a = read(join(ROOT, "desktop/src/witness/tv-emulator.js"));
  const b = read(join(CANARY, "witness/tv-emulator.js"));
  assert.strictEqual(a, b,
    "vendored tv-emulator.js drifted between the Flasher and the Lab — run scripts/vendor_witness_emulator.sh");
});

// ── The derived birth certificate: one bird, one name, three surfaces ─────
//
// The Mac app can't import canary-local, so it inlines the derivation. That is
// exactly the shape of drift this file exists to catch: an algorithm copied by
// hand goes wrong quietly, and the failure is a Canary that answers to one
// name in the Flasher and another on the phone. So run the native app's OWN
// inlined functions against the canonical module and demand identical output.
test("the native flasher derives the same certificate as the module", async () => {
  const src = read(join(ROOT, "desktop", "src", "app.js"));
  const hatch = JSON.parse(read(join(CANARY, "devices", "hatch.json")));
  const { deriveCertificate } = await import(
    pathToFileURL(join(CANARY, "tools", "hatchery", "derive.mjs")).href);

  // Lift the three inlined functions out of the bundle and evaluate them.
  const block = src.match(
    /function hatchFnv1a[\s\S]*?\nfunction deriveCertificate\([\s\S]*?\n}\n/);
  assert.ok(block, "desktop/src/app.js no longer carries the inlined derivation");
  const native = new Function(`${block[0]}; return deriveCertificate;`)();

  for (const fp of ["0000000000000000", "a3f7c1d2e4b58690", "deadbeefcafef00d",
                    "1234567890abcdef", "0f1e2d3c4b5a6978"]) {
    const mine = native(hatch, fp, { name: "Canary Vision" }, "");
    const theirs = deriveCertificate(hatch, { fingerprint: fp, product: { name: "Canary Vision" } });
    assert.equal(mine.name, theirs.name, `native/module name drift for ${fp}`);
    assert.equal(mine.lineage, theirs.lineage, `native/module lineage drift for ${fp}`);
    assert.equal(mine.motto, theirs.motto, `native/module motto drift for ${fp}`);
    assert.equal(mine.ringId, theirs.ringId, `native/module ring id drift for ${fp}`);
  }
});

// The gap this test exists for: the derivation can be perfectly correct in
// all three languages and still never run, because no CALLER passes a
// fingerprint. That is exactly what happened — both flashers kept rolling
// dice while the module, the vectors and the iOS tests all agreed with each
// other about an answer nobody asked for. So assert the wiring, not just the
// algorithm: each flasher must source the key from the board's own receipt.
test("both flashers feed the board's real key into the mint", () => {
  const desktop = read(join(ROOT, "desktop", "src", "app.js"));
  assert.match(desktop, /manifest\.pubkey_fp/,
    "desktop/src/app.js must take the fingerprint from the boot receipt manifest");
  assert.match(desktop, /mintCertificate\(product,\s*undefined,\s*bootFp\)/,
    "the desktop hatch must pass that fingerprint into mintCertificate");

  const browser = read(join(CANARY, "assets", "flash.js"));
  assert.match(browser, /identity\.pubkey_fp/,
    "flash.js must take the fingerprint from the serial self-manifest");
  assert.match(browser, /mintCertificate\(spec,\s*\{[\s\S]*?fingerprint[,\s]/,
    "the browser hatch must pass that fingerprint into mintCertificate");
});

test("a derived name offers no re-roll on either flasher", () => {
  // Re-rolling a derived name would make one app the only one calling the
  // bird by that name — the exact drift this whole change removes.
  assert.match(read(join(ROOT, "desktop", "src", "app.js")),
    /rerollBtn\.hidden = !!cert\.derived/, "desktop must hide the dice for a derived name");
  assert.match(read(join(CANARY, "assets", "flash.js")),
    /cert\.derived\s*\n?\s*\?\s*null/, "flash.js must not build a re-roll for a derived name");
});
