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
  const { mqttProvisioningToNvs } = await import("../assets/flash-core.js");
  const { strings, u16 } = mqttProvisioningToNvs({
    deviceId: "d", mqttHost: "h", mqttPort: 1, mqttUser: "u", mqttPass: "p",
  });
  const browserKeys = new Set(["wifi_ssid", "wifi_pass", "wifi_en", "setup_ok",
    ...Object.keys(strings), ...Object.keys(u16)]);

  assert.deepStrictEqual([...browserKeys].sort(), [...nativeKeys].sort(),
    "browser vs native provisioning NVS key-sets diverged — reconcile " +
    "flash-core.js:mqttProvisioningToNvs/buildNvsSeedImage with " +
    "desktop/src-tauri/src/provisioning.rs:build_nvs");
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
