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
    [...provRs.matchAll(/writer\.(?:string|u8|u16|u32)\(\s*"([a-z0-9_]+)"/g)].map((m) => m[1])
  );
  assert.ok(nativeKeys.size >= 5,
    "couldn't parse native build_nvs keys from desktop/src-tauri/src/provisioning.rs");

  // The browser's usb-secrets provisioning: wifi (string scheme) + the id/broker map.
  const { mqttProvisioningToNvs } = await import("../assets/flash-core.js");
  const { strings, u16 } = mqttProvisioningToNvs({
    deviceId: "d", mqttHost: "h", mqttPort: 1, mqttUser: "u", mqttPass: "p",
  });
  const browserKeys = new Set(["wifi_ssid", "wifi_pass", ...Object.keys(strings), ...Object.keys(u16)]);

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
    assert.match(nativeFnBody(libRs, fn), /check_local_image\s*\(/,
      `native ${fn} writes a local image at 0x0 without calling check_local_image — ` +
      `an app-only .bin would overwrite the bootloader. Call check_local_image(&bytes)? ` +
      `before the write (desktop/src-tauri/src/lib.rs)`);
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
