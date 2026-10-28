// Drift-gate: keep the NATIVE Mac app (desktop/) in lock-step with the canonical
// product catalog the BROWSER Lab derives from, so the two flashers can't silently
// disagree. Both consume one catalog (devices/flash.json). The chip table, the
// release-origin guard and the WE2 USB identity now DERIVE from the embedded
// catalog on the native side too, so for those this test guards the derivation
// itself (the reader still reads the catalog; no retyped copy crept back in) and
// that the catalog fields it reads stay well-formed — native fails closed on a
// malformed field, so a catalog typo would otherwise break it silently. The
// values native still HARDCODES (MODEL_ADDR, the dev-channel URL) are diffed
// against the catalog/browser as before — turning a silent runtime mismatch
// (a board the browser flashes but native rejects; a moved release host)
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
const { mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } = require("node:fs");
const { tmpdir } = require("node:os");
const { spawnSync } = require("node:child_process");
const { join } = require("node:path");
const { pathToFileURL } = require("node:url");
const vm = require("node:vm");

const ROOT = join(__dirname, "..", "..");     // repo root
const CANARY = join(__dirname, "..");         // canary-local/
const read = (p) => readFileSync(p, "utf8");

const catalog = JSON.parse(read(join(CANARY, "devices/flash.json")));
const libRs = read(join(ROOT, "desktop/src-tauri/src/lib.rs"));
// The ESP32 flash engine both desktop apps share (desktop/flash-engine): what
// used to live in the Flasher's lib.rs and its pure modules — the catalog
// guards, release verification, provisioning, the change map, intake, the
// flash pipeline and the serial monitor. The Flasher's lib.rs keeps the Tauri
// command wrappers over it, so an assertion about "the native backend" reads
// both: `nativeRs` is the Flasher's commands plus the engine they call.
const ENGINE = join(ROOT, "desktop/flash-engine/src");
const engineRs = (name) => read(join(ENGINE, `${name}.rs`));
const engineAllRs = readdirSync(ENGINE).filter((f) => f.endsWith(".rs")).sort()
  .map((f) => read(join(ENGINE, f))).join("\n");
const nativeRs = libRs + "\n" + engineAllRs;
const we2Rs = read(join(ROOT, "desktop/src-tauri/src/we2.rs"));
const we2Core = read(join(CANARY, "assets/we2-core.js"));

// The same fold the browser (flash-core.js:normalizeChip) and the native guard use.
const normChip = (s) => String(s || "").toUpperCase().replace(/[\s\-_]+/g, "");

// The text of a top-level native fn (col-0 `fn`/`async fn`), from its signature
// to the next top-level item. Reads source, not compiled Rust — enough to assert
// which guard a command routes through without a desktop toolchain.
const nativeFnBody = (src, name) => {
  const sig = new RegExp(`\\n(?:pub\\s+)?(?:async\\s+)?fn\\s+${name}\\s*(?:<[^>]*>)?\\s*\\(`);
  const m = sig.exec(src);
  assert.ok(m, `couldn't find fn ${name} in the native source (desktop/src-tauri or desktop/flash-engine)`);
  const after = src.slice(m.index + 1);
  // Stop at the next top-level item — its `fn`, its attribute (#[…]), or the doc
  // comment (///) that precedes it — so a body can't bleed into the next command.
  const next = after.search(/\n(?:pub\s+)?(?:async\s+)?fn\s+\w|\n\s*#\[|\n\s*\/\/\//);
  return next >= 0 ? after.slice(0, next) : after;
};

test("chip guard: native derives its chip table from the catalog, not a copy", () => {
  // native chip_table() builds canonical_chip()'s lookup from the embedded
  // catalog's `chips` keys — the drift risk this test used to diff away is
  // gone as long as the derivation stays and no hardcoded table creeps back.
  assert.match(nativeFnBody(engineRs("catalog"), "chip_table"), /get\("chips"\)/,
    "flash-engine catalog.rs chip_table() no longer reads the catalog's `chips` keys — " +
    "if the table went back to being hardcoded, restore the old diff here");
  assert.strictEqual(
    [...nativeRs.matchAll(/\(\s*"esp32[a-z0-9]*"\s*,\s*"(ESP32[^"]*)"\s*\)/g)].length, 0,
    "a hardcoded (\"esp32…\",\"ESP32-…\") chip pair is back in the native source — " +
    "the table derives from the catalog now; delete the retyped copy");

  // The catalog's spelling wins for chips it ships (an ESP32-family chip the
  // catalog doesn't ship is still named, algorithmically, so rescue stays
  // catalog-independent), so the keys native folds into lookup tokens must
  // stay canonically spelled or the flash chip-guard compares wrong names.
  const chips = Object.keys(catalog.chips || {});
  assert.ok(chips.length >= 3, "catalog `chips` table is empty/tiny — the " +
    "browser's picker and native's catalog spellings both derive from it");
  for (const chip of chips) {
    assert.match(chip, /^ESP32/,
      `catalog chip key "${chip}" isn't a canonical ESP32-… spelling — ` +
      "native folds these keys into its lookup tokens");
  }
});

test("WE2 module USB id: native derives it from the catalog's we2_module", () => {
  // native usb_ids() reads we2_module.usb_vid/usb_pid out of the embedded
  // catalog — assert the derivation stays and the retyped consts stay gone.
  const usbIdsBody = nativeFnBody(we2Rs, "usb_ids");
  for (const key of ["we2_module", "usb_vid", "usb_pid"]) {
    assert.ok(usbIdsBody.includes(`"${key}"`),
      `we2.rs usb_ids() no longer reads catalog key "${key}" — ` +
      "if the IDs went back to consts, restore the old diff here");
  }
  assert.ok(!/USB_[VP]ID:\s*u16\s*=/.test(we2Rs),
    "a hardcoded USB_VID/USB_PID const is back in we2.rs — the IDs derive " +
    "from the catalog now; delete the retyped copy");

  // Native fails closed on a malformed entry (is_module_usb matches nothing),
  // so the catalog fields it parses must stay well-formed hex.
  const we2 = catalog.we2_module || {};
  for (const key of ["usb_vid", "usb_pid"]) {
    const parsed = parseInt(String(we2[key]).replace(/^0x/i, ""), 16);
    assert.ok(Number.isFinite(parsed) && parsed > 0 && parsed <= 0xffff,
      `catalog we2_module.${key} (${we2[key]}) isn't a u16 hex string — ` +
      "native's derived port matcher would silently match no ports");
  }
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
    ["desktop/flash-engine/src/catalog.rs", engineRs("catalog")],
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

test("release origin: native derives its download-host guard from the catalog", () => {
  // native release_origin() cuts the origin prefix out of the catalog's own
  // manifest_url (everything through /releases/download/), the same value the
  // browser derives live — assert the derivation stays and no literal origin
  // guard crept back (the dev-channel URL is a full manifest URL, not an
  // origin prefix, so it never matched this pattern).
  const originBody = nativeFnBody(engineRs("catalog"), "release_origin");
  assert.ok(originBody.includes('"manifest_url"') && originBody.includes("/releases/download/"),
    "flash-engine catalog.rs release_origin() no longer derives from the catalog's manifest_url — " +
    "if the guard went back to a literal, restore the old diff here");
  assert.strictEqual(
    [...nativeRs.matchAll(/"(https:\/\/[^"]+\/releases\/download\/)"/g)].length, 0,
    "a literal release-origin prefix is back in the native source — the guard derives " +
    "from the catalog now; delete the retyped copy");

  // Native fails closed on a manifest_url without the marker (nothing
  // downloads), so the catalog's URL shape is what keeps flashing alive.
  assert.match(String(catalog.manifest_url), /^https:\/\/.+\/releases\/download\/.+/,
    "catalog manifest_url isn't a …/releases/download/… URL — native's derived " +
    "origin guard would refuse every download");
});

test("provisioning NVS: the browser writes the same key-set as native build_nvs", async () => {
  // Native build_nvs (provisioning.rs) IS the firmware contract for a provisioned
  // board — the exact NVS keys the runtime reads. The browser must write the same
  // set, or a board provisioned in the Lab differs from one provisioned natively.
  const provRs = engineRs("provisioning");
  const nativeKeys = new Set(
    [...provRs.matchAll(/writer\.(?:string|u8|u16|u32|blob)\(\s*"([a-z0-9_]+)"/g)].map((m) => m[1])
  );
  assert.ok(nativeKeys.size >= 5,
    "couldn't parse native build_nvs keys from desktop/flash-engine/src/provisioning.rs");

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
  assert.match(flashCoreSrc, /writeInt\("auto_upd"/,
    "flash-core.js buildNvsSeedImage lost the OTA auto-update seed — a browser-flashed " +
    "board would never keep itself updated");
  const { mqttProvisioningToNvs, apiTokenToNvs } = await import("../assets/flash-core.js");
  // Broker TLS fields ride the same row: a CA-mode sample with a pin present
  // exercises every key the builder can emit (mqtt_tls / mqtt_ca / mqtt_fp).
  const { strings, u16, u8 } = mqttProvisioningToNvs({
    deviceId: "d", mqttHost: "h", mqttPort: 1, mqttUser: "u", mqttPass: "p",
    mqttTls: 1, mqttCa: "-----BEGIN CERTIFICATE-----\nx\n-----END CERTIFICATE-----",
    mqttFp: "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9",
  });
  // The API-token blobs ride the same seed (blob-scheme boards); their keys
  // are part of the contract the two flashers share.
  const { blobs } = apiTokenToNvs("cv_" + "a".repeat(32));
  const browserKeys = new Set(["wifi_ssid", "wifi_pass", "wifi_en", "setup_ok", "auto_upd",
    ...Object.keys(strings), ...Object.keys(u16), ...Object.keys(u8), ...Object.keys(blobs)]);

  assert.deepStrictEqual([...browserKeys].sort(), [...nativeKeys].sort(),
    "browser vs native provisioning NVS key-sets diverged — reconcile " +
    "flash-core.js:mqttProvisioningToNvs/buildNvsSeedImage/apiTokenToNvs with " +
    "desktop/flash-engine/src/provisioning.rs:build_nvs");
});

test("OTA auto-update: both flashers seed the engine's own namespace, and both offer the switch", async () => {
  // The shared OTA engine ships in every networked flavor and reads its
  // persisted opt-in from ITS OWN NVS namespace — not "securacv", where
  // everything else the flashers seed lives. Seeding it at flash time is what
  // makes a freshly flashed board keep itself on signed releases without
  // Home Assistant, so the namespace, the key, the u8 encoding, and the
  // user-facing switch must all exist on BOTH frontends, pinned to the
  // firmware's own #defines rather than to each other's copies.
  const otaCpp = read(join(ROOT, "firmware/common/ota/src/securacv_ota.cpp"));
  assert.match(otaCpp, /#define NVS_NAMESPACE\s+"securacv_ota"/,
    "the OTA engine's NVS namespace moved — both flashers seed \"securacv_ota\"");
  assert.match(otaCpp, /#define NVS_KEY_AUTO_UPDATE\s+"auto_upd"/,
    "the OTA engine's auto-update key moved — both flashers seed \"auto_upd\"");

  // Both writers: the second namespace entry plus the u8 under it.
  const flashCoreSrc = read(join(CANARY, "assets/flash-core.js"));
  const provRs = engineRs("provisioning");
  assert.match(flashCoreSrc, /"securacv_ota"/,
    "flash-core.js buildNvsSeedImage no longer writes the securacv_ota namespace entry");
  assert.match(flashCoreSrc, /writeInt\("auto_upd", 0x01/,
    "flash-core.js must seed auto_upd as a u8 (0x01) — the engine reads it with nvs_get_u8");
  assert.match(provRs, /namespace\("securacv_ota"\)/,
    "provisioning.rs build_nvs no longer opens the securacv_ota namespace");
  assert.match(provRs, /writer\.u8\("auto_upd"/,
    "provisioning.rs build_nvs no longer seeds auto_upd as a u8");

  // The seeded page must be READABLE back by the flasher's own parser (the
  // health check uses it): two namespaces, each item under the right one.
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const img = core.buildNvsSeedImage(
    { wifi: { ssid: "Bird House", pass: "correct horse" }, autoUpdate: true }, 0x4000);
  const items = core.parseNvs(img);
  const auto = items.find((i) => i.namespace === "securacv_ota" && i.key === "auto_upd");
  assert.ok(auto && auto.value === 1,
    "parseNvs must read auto_upd back out of the securacv_ota namespace");
  assert.ok(items.some((i) => i.namespace === "securacv" && i.key === "wifi_en"),
    "the securacv namespace must keep its own tenants beside the new one");

  // The switch itself, BOTH frontends, default ON, same copy — capability
  // parity, not just constant parity (the dev-channel lesson).
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop index.html", html]]) {
    assert.ok(src.includes("Keep this Canary updated automatically"),
      `${label} lost the auto-update checkbox (or its copy drifted)`);
    assert.ok(src.includes("Ed25519-signed releases installed with A/B rollback"),
      `${label} no longer says what the updates ARE — signed releases with rollback`);
  }
  assert.match(flashJs, /otaChk\.checked = true/,
    "the browser checkbox must default ON");
  assert.match(html, /id="auto-update" checked/,
    "the desktop checkbox must default ON");
  assert.match(appJs, /autoUpdate: !!\(\$\("auto-update"\)/,
    "desktop readProvisioning never reads the auto-update checkbox");
  // Unchecked is a CHOICE — an explicit 0, never a silently absent key.
  const declined = core.parseNvs(core.buildNvsSeedImage({ autoUpdate: false }, 0x4000));
  const off = declined.find((i) => i.namespace === "securacv_ota" && i.key === "auto_upd");
  assert.ok(off && off.value === 0, "unchecking must seed an explicit auto_upd = 0");
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
                      "download", "not-in-download", "read-stall", "layout"]) {
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

  // 12. Review hardening (Codex on #1549). The diagnostic report enforces
  //     public-only IN THE BUILDER, both frontends: no hardware address at
  //     all (the tail it once kept matches the board in radio range — the
  //     base MAC is the station address in every Wi-Fi frame), and the serial
  //     tail is scrubbed of credential lines (the WAP prints its AP password
  //     to serial at boot — a report someone is ASKED to paste must not carry
  //     it) and of identity lines (the Hardware ID, the self-manifest).
  for (const [name, src] of [["desktop app.js", appJs], ["browser flash-core.js", flashCore]]) {
    assert.match(src, /function sanitizeLogTail|export function sanitizeLogTail/,
      `${name} lost the serial-tail credential scrub`);
    assert.match(src, /TAIL_REDACT[\s\S]{0,300}token/,
      `${name} scrub no longer covers token lines — the WAP prints "[PROV] API TOKEN : …" ` +
      "and quick-connect token JSON to serial (review on #1551)");
    assert.match(src, /TAIL_REDACT[\s\S]{0,800}WIFI:/,
      `${name} scrub no longer covers the "[SETUP] QR WIFI:…;P:<key>;;" line every setup ` +
      "network prints, which carries the flagship's SoftAP key (sweep F350 review)");
    assert.ok(!/function macTail|export function macTail/.test(src),
      `${name} has a MAC-tail helper again — no slice of the MAC is safe to paste`);
    const builder = fnSource(src, "buildDiagnosticReport");
    assert.ok(!/MAC/.test(builder) && !/info\.mac\b/.test(builder),
      `${name} report prints a MAC line again — a stable identifier in a pastebin`);
    assert.match(src, /sanitizeLogTail\(String\(info\.logTail\)/,
      `${name} report no longer scrubs the tail in the builder`);
  }
  // ...and the two lists are the same list: a pattern added to one frontend
  // and not the other leaves half the users pasting the line it covers.
  const redactList = (src) => {
    const m = src.match(/const TAIL_REDACT = \[\n([\s\S]*?)\n\];/);
    assert.ok(m, "TAIL_REDACT array not found");
    return m[1].split("\n").map((ln) => ln.trim()).filter(Boolean)
      .map((ln) => ln.match(/^(\/.+?\/[a-z]*),/)?.[1] ?? `<not a pattern: ${ln}>`);
  };
  assert.deepStrictEqual(redactList(appJs), redactList(flashCore),
    "desktop app.js and browser flash-core.js scrub different serial lines");
  const identityList = (src) => {
    const m = src.match(/const TAIL_IDENTITY = \[\n([\s\S]*?)\n\];/);
    assert.ok(m, "TAIL_IDENTITY array not found");
    return m[1].split("\n").map((ln) => ln.trim()).filter(Boolean)
      .map((ln) => ln.match(/^(\/.+?\/[a-z]*),/)?.[1] ?? `<not a pattern: ${ln}>`);
  };
  assert.deepStrictEqual(identityList(appJs), identityList(flashCore),
    "desktop app.js and browser flash-core.js mark different identity lines");
  assert.ok(identityList(flashCore).length >= 2, "the identity list lost the Hardware ID or the manifest line");
});

test("parity wave 2: the board passport, the install verdict, and 'we've met this board'", () => {
  // Wave 2 of the 2026-08 inventory. The browser has always read the board
  // before writing to it; the desktop app flashed blind — no idea what was
  // resident, so no way to say "this is a downgrade" or "you've flashed this
  // one before". These assertions keep that eyesight.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const healthRs = engineRs("health");
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
  assert.match(nativeRs, /let mut tail: Vec<String>/,
    "espflash's output must survive into the error, or every failure is `unknown`");
  assert.match(nativeRs, /tail\.join\("\\n"\)/,
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
  const provRs = engineRs("provisioning");
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
  const cmRs = engineRs("changemap");
  const libRsSrc = nativeRs;
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
  // …and the path is the webview's, so it is validated before a byte is read
  // (an absolute, canonicalized .bin regular file ≤ 32 MiB — image.rs).
  assert.match(libRsSrc, /if let Some\(old\) = read_safety_copy\(bp\)/,
    "flash must read the safety copy through image::read_safety_copy, never an unchecked fs::read");
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
  const intakeRs = engineRs("intake");
  const libRsSrc = nativeRs;
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

  // Both read the same head window, wide enough to reach a classic ESP32's
  // bootloader at 0x1000: with 4 KB, every correctly installed classic board
  // read as blank, and a mirrored one was never caught. (The bootloader-
  // placement test below holds the window past every offset in the ROM table.)
  const jsWindow = /export const FLASH_ALIAS_WINDOW = (0x[0-9a-fA-F]+);/.exec(intakeJs);
  const rsWindow = /pub const ALIAS_WINDOW: usize = (0x[0-9a-fA-F]+);/.exec(intakeRs);
  assert.ok(jsWindow && rsWindow, "couldn't read the alias window out of intake.js / intake.rs");
  assert.strictEqual(Number(jsWindow[1]), Number(rsWindow[1]),
    "intake.js FLASH_ALIAS_WINDOW and intake.rs ALIAS_WINDOW drifted apart");
  const runIntake = jsFnText(read(join(CANARY, "assets/flash.js")), "runIntake", "flash.js");
  assert.match(runIntake, /const win = intake\.FLASH_ALIAS_WINDOW;/,
    "the browser's capacity probe no longer reads intake.FLASH_ALIAS_WINDOW");
  assert.match(runIntake, /readFlashChunked\(esploader, 0, win\)/,
    "the browser's head read no longer uses the alias window");
  assert.match(runIntake, /readFlashChunked\(esploader, candidates\[i\], win\)/,
    "the browser's probe reads no longer use the alias window");
  assert.match(nativeRs, /declared >= 2 \* intake::ALIAS_WINDOW as u64/,
    "the desktop's capacity check no longer gates on its own window");

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
  const provRs = engineRs("provisioning");
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

test("secret drawer: the consent copy names exactly the stores the native side can answer", () => {
  // app.js words every "Remember" note from secretStore.where(), keyed on the
  // string secret_backend returns. A backend the Rust side can answer but
  // where() doesn't name falls through to "this app's local settings" — a
  // consent note that understates where a password went; a name where()
  // knows but Rust never returns is dead copy. One set, both sides.
  const storeRs = read(join(ROOT, "desktop/src-tauri/src/secret_store.rs"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const strings = (text) => new Set([...text.matchAll(/"([a-z-]+)"/g)].map((m) => m[1]));
  const sorted = (set) => [...set].sort();

  const contract = /fn backend_names_are_the_contract\(\)[\s\S]*?matches!\(\s*backend\(\),([^)]*)\)/.exec(storeRs);
  assert.ok(contract, "secret_store.rs lost backend_names_are_the_contract — re-point this gate at its new home");
  const names = strings(contract[1]);
  assert.ok(names.has("none"), "the backend contract must keep \"none\" — the fail-closed answer");

  // Every name the native side can actually return is in the contract:
  // backend()'s per-platform tail expressions and probe_backend()'s arms.
  // (A whole-fn slice, to the closing col-0 brace: nativeFnBody stops at the
  // first attribute, and backend()'s arms are each behind a #[cfg].)
  const rsFn = (name) => {
    const m = new RegExp(`\\n(?:pub\\s+)?(?:async\\s+)?fn\\s+${name}\\s*\\(`).exec(storeRs);
    assert.ok(m, `couldn't find fn ${name} in desktop/src-tauri/src/secret_store.rs`);
    const rest = storeRs.slice(m.index + 1);
    return rest.slice(0, rest.indexOf("\n}\n"));
  };
  const tails = [...rsFn("backend").matchAll(/^\s*"([a-z-]+)"\s*$/gm)].map((m) => m[1]);
  const arms = [...rsFn("probe_backend").matchAll(/=>\s*"([a-z-]+)"/g)].map((m) => m[1]);
  assert.ok(tails.length >= 3, "couldn't read backend()'s per-platform answers out of secret_store.rs");
  assert.ok(arms.length >= 2, "couldn't read probe_backend()'s answers out of secret_store.rs");
  for (const a of [...tails, ...arms]) {
    assert.ok(names.has(a), `secret_store.rs can answer "${a}" but its contract test doesn't list it`);
  }
  // The Linux answer is PROBED, and the probe fails closed to "none".
  assert.match(rsFn("probe_backend"), /_ => "none"/,
    "probe_backend() must fall back to \"none\" on any failure — a wrong \"secret-service\" makes the consent note a promise the app can't keep");

  // where(): every non-"none" name gets its own wording; "none" is the fallback.
  const where = /\n  where\(\) \{([\s\S]*?)\n  \},/.exec(appJs);
  assert.ok(where, "desktop app.js secretStore.where() moved — re-point this gate at it");
  const worded = new Set([...where[1].matchAll(/this\.backend === "([a-z-]+)"/g)].map((m) => m[1]));
  assert.match(where[1], /: "this app's local settings";/,
    "where() must end on the honest no-store wording");
  assert.deepStrictEqual(sorted(new Set([...worded, "none"])), sorted(names),
    "desktop app.js where() and secret_store.rs name different secret stores");
});

// The drawer's store paths, RUN rather than read: the Flasher's secretStore
// lifted out of app.js against a stubbed native side whose store calls are
// held open until the test answers them, the way a locked keyring holds one
// open behind its unlock prompt.
function loadSecretStore(backend, prefsSecrets) {
  const src = read(join(ROOT, "desktop/src/app.js"));
  const m = /\nconst secretStore = \{[\s\S]*?\n\};\n/.exec(src);
  assert.ok(m, "desktop/src/app.js lost the secretStore object — re-point this gate at it");
  const prefs = { secrets: { ...prefsSecrets }, secretKeys: Object.keys(prefsSecrets) };
  const store = {};      // what the OS store holds; a write lands when it is answered
  const calls = [];      // [cmd, args, answer(how)] in invoke order
  const logs = [];
  const invoke = (cmd, args) => {
    if (cmd === "secret_backend") return Promise.resolve(backend);
    let answer;
    const p = new Promise((resolve, reject) => {
      // answer() = the store does it; {reject: e} = it refuses; {value: v} =
      // it answers v (a read taken before some other write landed).
      answer = (how = {}) => {
        if ("reject" in how) return reject(how.reject);
        if ("value" in how) return resolve(how.value);
        if (cmd === "secret_set") store[args.key] = args.value;
        if (cmd === "secret_delete") delete store[args.key];
        return resolve(cmd === "secret_get" ? (args.key in store ? store[args.key] : null) : null);
      };
    });
    calls.push([cmd, args, answer]);
    return p;
  };
  const secretStore = new Function("invoke", "prefs", "savePrefs", "logEvent",
    `${m[0]}\nreturn secretStore;`)(invoke, prefs, () => {}, (level, msg) => logs.push([level, msg]));
  return { secretStore, prefs, store, calls, logs };
}
const drain = async () => { for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r)); };
const storeCalls = (calls, cmd, key) => calls.filter(([c, a]) => c === cmd && a.key === key);

test("secret drawer: a refusing store is never downgraded to the prefs file, and a locked keyring never holds up a read", async () => {
  // 1. The OS store exists but refuses the write (locked, declined): the
  //    consent note promised the OS store, so set() stores NOWHERE and says
  //    so — it never quietly drops the password into the prefs file.
  {
    const { secretStore, prefs, calls, logs } = loadSecretStore("secret-service", {});
    const saving = secretStore.set("wifi:home", "hunter2");
    await drain();
    storeCalls(calls, "secret_set", "wifi:home")[0][2]({ reject: "locked" });
    assert.strictEqual(await saving, false, "a refused OS-store write must report failure");
    assert.ok(!("wifi:home" in prefs.secrets),
      "a refused OS-store write fell back to the prefs file — the consent note named the OS store");
    assert.ok(logs.some(([level]) => level === "err"), "a refused OS-store write must be logged, not silent");
  }
  // 2. Launch with a password left in prefs and an unlock prompt nobody has
  //    answered (the adoption pass's write held open): a restore's get() must
  //    still ask the store and answer from the prefs copy, not wait on it.
  {
    const { secretStore, calls } = loadSecretStore("secret-service", { "wifi:home": "old" });
    const reading = secretStore.get("wifi:home");
    await drain();
    const read1 = storeCalls(calls, "secret_get", "wifi:home")[0];
    assert.ok(read1, "get() is waiting behind the adoption pass — a locked keyring's prompt would hold the restore");
    read1[2]();
    assert.strictEqual(await reading, "old", "get() must fall back to the prefs copy while the store has nothing");
    assert.strictEqual(storeCalls(calls, "secret_set", "wifi:home").length, 1,
      "the adoption pass should still be out here, its one write unanswered");
  }
  // 3. A save made while the pass is still out lands AFTER it, never under
  //    it: the pass writes the OLD value, so a newer one written first would
  //    be overwritten once the prompt is answered. Same for a delete, which
  //    would otherwise see the forgotten password come back.
  {
    const { secretStore, store, calls } = loadSecretStore("secret-service", { "wifi:home": "old" });
    await secretStore.init();
    const saving = secretStore.set("wifi:home", "new");
    await drain();
    const writes = () => storeCalls(calls, "secret_set", "wifi:home");
    assert.strictEqual(writes().length, 1, "set() wrote while the adoption pass was still out — it must wait for it");
    writes()[0][2]();            // the prompt is answered: the pass writes the old value
    await drain();
    assert.strictEqual(writes().length, 2, "set() never wrote after the adoption pass finished");
    writes()[1][2]();
    assert.strictEqual(await saving, true);
    assert.strictEqual(store["wifi:home"], "new", "the adoption pass overwrote a newer saved password");
  }
  {
    const { secretStore, store, calls } = loadSecretStore("secret-service", { "wifi:home": "old" });
    await secretStore.init();
    const forgetting = secretStore.delete("wifi:home");
    await drain();
    assert.strictEqual(storeCalls(calls, "secret_delete", "wifi:home").length, 0,
      "delete() ran while the adoption pass was still out — the pass would put the password back");
    storeCalls(calls, "secret_set", "wifi:home")[0][2]();
    await drain();
    storeCalls(calls, "secret_delete", "wifi:home")[0][2]();
    assert.strictEqual(await forgetting, true);
    assert.ok(!("wifi:home" in store), "a forgotten password came back from the adoption pass");
  }
  // 4. A key the pass moves while get() is out is still found: the store's
  //    answer predates the move, and the prefs copy is gone after it.
  {
    const { secretStore, prefs, calls } = loadSecretStore("secret-service", { "wifi:home": "v" });
    await secretStore.init();
    const reading = secretStore.get("wifi:home");
    await drain();
    const [read1] = storeCalls(calls, "secret_get", "wifi:home");
    const [move] = storeCalls(calls, "secret_set", "wifi:home");
    assert.ok(read1 && move, "expected the read and the adoption write both in flight");
    move[2]();                   // the move lands and drops the prefs copy…
    await drain();
    assert.ok(!("wifi:home" in prefs.secrets), "the adoption pass should have dropped the prefs copy");
    read1[2]({ value: null });   // …then the store's answer from before it arrives
    assert.strictEqual(await reading, "v", "get() lost a password the adoption pass moved while it was asking");
  }
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
  // The other writers spell the tag too: dev_channel.py (every firmware
  // release, a tag rebuild, the Refresh button) and the button's own record
  // and guard steps. A rename the readers took and these did not would keep
  // every release writing the dev channel's manifests to a tag nothing reads,
  // and the guard checking that same orphan, all green.
  const script = read(join(ROOT, ".github/scripts/dev_channel.py"));
  assert.match(script, new RegExp(`^DEV_TAG = "${tag}"$`, "m"),
    `.github/scripts/dev_channel.py's DEV_TAG is not "${tag}", the tag the flashers read`);
  const refresh = read(join(ROOT, ".github/workflows/dev-channel-refresh.yml"));
  const paths = [...refresh.matchAll(/releases\/tags\/([A-Za-z0-9._${}-]+)/g)].map((m) => m[1]);
  assert.ok(paths.length >= 1, "dev-channel-refresh.yml reads fw-dev-latest by releases/tags/<tag>");
  for (const p of paths) {
    assert.strictEqual(p, tag, `dev-channel-refresh.yml reads releases/tags/${p}, not the flashers' ${tag}`);
  }
  for (const [t] of refresh.matchAll(/\bfw-dev-[A-Za-z0-9-]+/g)) {
    assert.strictEqual(t, tag, `dev-channel-refresh.yml names ${t}, not the flashers' ${tag}`);
  }
});

test("dev channel: every flasher says where it stands against its pin from the two trains, in the same words", async () => {
  // fw-dev-latest held a 2.4.15 catalog and no OTA manifest under stable 2.4.17
  // while all three frontends called it "cut ahead of stable" (RELEASE_LESSONS
  // 2026-10-27). A stable release now copies itself there whenever no newer
  // dev build is, and the train a flasher compares with is its own pin (built
  // in, for the desktop Flasher and the Lab), not stable's live version: so
  // the sentence is the dev train against the pin, said by
  // devChannelStanding: flash-core.js for the browser page and the Lab bench,
  // its copy in desktop/src/app.js for the Flasher.
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const desktop = new Function(
    jsFnText(appJs, "compareVersions", "desktop/src/app.js") + "\n" +
    jsFnText(appJs, "devChannelStanding", "desktop/src/app.js") + "\nreturn devChannelStanding;")();
  const cases = [
    ["2.4.18-dev.1", "2.4.17", /2\.4\.18-dev\.1, newer than the 2\.4\.17 release this flasher is pinned to/],
    // the next stable, copied to the dev channel, seen by an app pinned to the last one
    ["2.4.18", "2.4.17", /^The dev channel carries 2\.4\.18, newer than the 2\.4\.17 release this flasher is pinned to\.$/],
    ["2.4.17", "2.4.17", /2\.4\.17, the same version as the release this flasher is pinned to/],
    ["2.4.15", "2.4.17", /2\.4\.15, older than the 2\.4\.17 release/],
    ["2.4.17-rc.1", "2.4.17", /older than the 2\.4\.17 release/],
    [undefined, "2.4.17", /^$/],
    ["not-a-version", "2.4.17", /^$/],
  ];
  for (const [dev, stable, said] of cases) {
    const browser = core.devChannelStanding(dev, stable);
    assert.match(browser, said, `browser: ${dev} against ${stable}`);
    assert.equal(desktop(dev, stable), browser, `the two flashers disagree on ${dev} against ${stable}`);
  }
  // Said where the user reads it: each dev banner, from the loaded dev
  // manifest's train and the stable catalog's...
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  assert.match(flashJs, /core\.devChannelStanding\(m\.fw_train, state\.catalog\.fw_train\)/);
  assert.match(nativeJs, /core\.devChannelStanding\(st\.manifest && st\.manifest\.fw_train, catalog\.fw_train\)/);
  assert.match(appJs, /\$\("dev-standing"\)\.textContent = state\.devChannel\s*\?\s*devChannelStanding\(m && m\.fw_train, state\.catalog\.fw_train\)/);
  assert.match(html, /id="dev-banner"[\s\S]{0,300}<span id="dev-standing"><\/span>/);
  // ...and no copy promises it unconditionally, or calls the pin "stable".
  for (const [label, src] of [["canary-local/assets/flash.js", flashJs], ["canary-local/assets/flash-native.js", nativeJs],
    ["desktop/src/index.html", html], ["desktop/src/app.js", appJs], ["canary-local/assets/flash-core.js", read(join(CANARY, "assets/flash-core.js"))]]) {
    assert.doesNotMatch(src, /cut ahead of stable|images are cut ahead|dev prerelease, ahead of stable|not yet promoted to stable|channel is ahead of stable|when one is ahead of stable/,
      `${label} calls the dev channel ahead of stable without knowing stable's version`);
  }
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
  // Provisioning moved into .github/actions/setup-platformio (CI.md R10), so
  // the requirement is expressed as that action's `extras:` input; an inline
  // `pip install` is the pre-R10 shape and still counts if it ever returns.
  assert.match(wf, /pip install[^\n]*cryptography|extras:[^\n]*cryptography/,
    "signing needs `cryptography` (ota_release.py imports it) — install it " +
    "(setup-platformio `extras:`), or the sign path dies after the build " +
    "instead of before it");

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
  // An app-only PlatformIO build opens with the 0xE9 image magic, and so does
  // an S3/C3/C6 factory image (a classic ESP32's keeps 4 KB of erased flash
  // before its bootloader), so byte 0 can't tell an app from a factory image —
  // the only honest discriminator is the partition table at 0x8000. Anything written from offset
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
  assert.match(engineRs("image"), /fn check_local_image\b/,
    "native lost flash-engine image.rs:check_local_image — the app-only-build refusal");
  assert.match(nativeFnBody(engineRs("image"), "check_local_image"), /PARTITION_TABLE_OFFSET/,
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

test("bootloader placement: both flashers refuse an image whose bootloader isn't where the chip's ROM loads it", async () => {
  // fw-v2.4.17's classic-ESP32 factory images (ESP32-CAM, WROOM DevKit) carried
  // their bootloader at 0x0; a classic ESP32's ROM loads it from 0x1000, so
  // neither could boot, and the partition-table gate above passed both. Each
  // flasher now checks the image against the chip in hand before any write, on
  // the catalog path and the local-file path, from ONE table that
  // firmware/scripts/make_factory.py also merges by.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const imageRs = engineRs("image");
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);

  // 1. One table, three copies, held equal: where each chip's ROM loads the
  //    bootloader, and the chip id that bootloader's header names.
  const makeFactory = read(join(ROOT, "firmware/scripts/make_factory.py"));
  const pyBlock = /\nROM_BOOTLOADER = \{([\s\S]*?)\n\}/.exec(makeFactory);
  assert.ok(pyBlock, "couldn't find ROM_BOOTLOADER in firmware/scripts/make_factory.py");
  const py = Object.fromEntries([...pyBlock[1].matchAll(/"(esp32\w*)":\s*\((0x[0-9a-fA-F]+),\s*(\d+)\)/g)]
    .map((m) => [m[1], [Number(m[2]), Number(m[3])]]));
  const rsBlock = /pub const ROM_BOOTLOADER: \[\(&str, usize, u16\); \d+\] = \[([\s\S]*?)\];/.exec(imageRs);
  assert.ok(rsBlock, "couldn't find ROM_BOOTLOADER in flash-engine image.rs");
  const rs = Object.fromEntries([...rsBlock[1].matchAll(/\("(esp32\w*)",\s*(0x[0-9a-fA-F]+),\s*(\d+)\)/g)]
    .map((m) => [m[1], [Number(m[2]), Number(m[3])]]));
  assert.ok(py.esp32?.[0] === 0x1000 && py.esp32s3?.[1] === 9,
    `make_factory.py's table didn't parse: ${JSON.stringify(py)}`);
  assert.deepStrictEqual({ ...core.ROM_BOOTLOADER }, py,
    "flash-core.js ROM_BOOTLOADER drifted from make_factory.py's ROM_BOOTLOADER");
  assert.deepStrictEqual(rs, py,
    "flash-engine image.rs ROM_BOOTLOADER drifted from make_factory.py's ROM_BOOTLOADER");
  // …and the header byte the chip id sits at.
  const pyIdAt = /\nIMAGE_CHIP_ID_AT = (\d+)/.exec(makeFactory);
  const rsIdAt = /pub const IMAGE_CHIP_ID_AT: usize = (\d+);/.exec(imageRs);
  assert.ok(pyIdAt && rsIdAt, "couldn't read IMAGE_CHIP_ID_AT out of make_factory.py / image.rs");
  const idAt = Number(pyIdAt[1]);
  assert.strictEqual(Number(rsIdAt[1]), idAt, "image.rs IMAGE_CHIP_ID_AT drifted from make_factory.py's");
  // Every chip the catalog ships has a row: a chip with none is never judged
  // by the flashers, and make_factory.py refuses to merge it, which drops that
  // product from every release. (This runs whenever flash.json changes.)
  for (const p of catalog.products) {
    assert.notStrictEqual(core.romBootloader(p.chip), null,
      `${p.id} is a ${p.chip}, which ROM_BOOTLOADER has no row for — add the chip's ROM ` +
      "bootloader offset and image chip id to make_factory.py, flash-core.js and image.rs together");
  }
  // The capacity probe's head window must reach past every offset the table
  // lists, or every correctly installed board of that chip reads as blank and
  // a mirrored part is never caught: a chip that loads from 0x2000 (the C5,
  // the P4) needs a wider window first. (Wave 4a holds intake.rs's to it.)
  const hex = (n) => `0x${n.toString(16)}`;
  const intake = await import(pathToFileURL(join(CANARY, "assets/intake.js")).href);
  const furthest = Math.max(...Object.values(py).map(([at]) => at));
  assert.ok(intake.FLASH_ALIAS_WINDOW > furthest,
    `intake.js FLASH_ALIAS_WINDOW (${hex(intake.FLASH_ALIAS_WINDOW)}) doesn't reach a bootloader ` +
    `at ${hex(furthest)} — widen it and intake.rs ALIAS_WINDOW together`);

  // 2. The same words. The desktop's refusal, rendered from its own format
  //    strings, equals the browser's for the same image and chip.
  const body = nativeFnBody(imageRs, "check_bootloader_offset");
  const fmt = /Err\(format!\(\s*"([\s\S]*?)"\s*\)\)/.exec(body);
  const elseFmt = /format!\("( \(it has one at )\{o:#x\}( instead\))"\)/.exec(body);
  const anotherFmt = /None if id_at\(at\)\.is_some\(\) => "([^"]*)"\.into\(\)/.exec(body);
  assert.ok(fmt && elseFmt && anotherFmt, "couldn't read check_bootloader_offset's refusal out of image.rs");
  const template = fmt[1].replace(/\\\n\s*/g, "");
  const nativeWords = (chip, at, other) => template.replaceAll("{chip}", chip)
    .replace("{at:#x}", hex(at))
    .replace("{elsewhere}", other === null ? "" : other === "another" ? anotherFmt[1]
      : elseFmt[1] + hex(other) + elseFmt[2]);
  // A factory image with a bootloader for chip id `id` at `at` (null: none).
  const image = (at, id) => {
    const b = new Uint8Array(0x8100).fill(0xff);
    if (at !== null) { b[at] = 0xe9; b[at + idAt] = id & 0xff; b[at + idAt + 1] = id >> 8; }
    b[0x8000] = 0xaa; b[0x8001] = 0x50;
    return b;
  };
  for (const [chip, at, id, rom, other] of [
    ["ESP32", 0x0, 0, 0x1000, 0x0],               // fw-v2.4.17's classic-ESP32 shape
    ["ESP32-S3", 0x1000, 0, 0x0, null],           // a classic image on an S3
    ["ESP32-C3", 0x0, py.esp32s3[1], 0x0, "another"], // an S3 image on a C3
    ["ESP32-C6", null, 0, 0x0, null],             // no bootloader anywhere
  ]) {
    const v = core.bootloaderPlacement(image(at, id), chip);
    assert.strictEqual(v.ok, false, `the browser passed a ${chip} image with its bootloader at ${at}`);
    assert.strictEqual(v.reason, nativeWords(chip, rom, other),
      `the two flashers word the ${chip} refusal differently — keep flash-core.js bootloaderPlacement ` +
      "and image.rs check_bootloader_offset in the same words");
  }
  // …and an image laid out by each row of the table passes on its chip.
  for (const [chip, [at, id]] of Object.entries(py)) {
    assert.ok(core.bootloaderPlacement(image(at, id), chip).ok, `a correct ${chip} image was refused`);
  }

  // 3. Both flashers run it before writing, on every path that writes an image
  //    at 0x0 for a chip it has read.
  const startFlash = jsFnText(flashJs, "startFlash", "flash.js");
  const gateAt = startFlash.indexOf("core.bootloaderPlacement(bytes, state.chip)");
  assert.ok(gateAt >= 0, "the browser's catalog install (flash.js:startFlash) no longer runs core.bootloaderPlacement");
  for (const later of ["esploader.eraseFlash(", "esploader.writeFlash("]) {
    assert.ok(gateAt < startFlash.indexOf(later), `startFlash calls ${later} before the bootloader check`);
  }
  assert.match(jsFnText(flashJs, "onLocalFile", "flash.js"), /core\.bootloaderPlacement\(bytes, state\.chip\)/,
    "the browser's local-file install (flash.js:onLocalFile) no longer runs core.bootloaderPlacement");
  const engineFlash = nativeFnBody(engineRs("flash"), "flash");
  const nativeGateAt = engineFlash.indexOf("check_bootloader_offset(&downloaded, &detected_chip)?");
  assert.ok(nativeGateAt >= 0, "the shared engine's catalog flash no longer runs check_bootloader_offset");
  assert.ok(nativeGateAt < engineFlash.indexOf("stage_firmware("),
    "the engine stages (and writes) the image before the bootloader check");
  assert.match(nativeFnBody(libRs, "flash_local_file"), /check_bootloader_offset\(&bytes, &detected_chip\)\?/,
    "the desktop's local-file install (lib.rs flash_local_file) no longer runs check_bootloader_offset");
  assert.match(jsFnText(appJs, "onFlashLocalFile", "desktop/src/app.js"), /detectedChip: state\.chip/,
    "the desktop frontend no longer tells flash_local_file which chip is connected");

  // 4. The refusal reads as itself in every classifier — not "unknown", which
  //    coaches download mode at an image no retry can fix.
  const refusal = new Error(nativeWords("ESP32", 0x1000, 0x0));
  for (const [where, classify] of [
    ["flash-core.js", core.classifyFlashError],
    ["desktop/src/app.js", new Function(jsFnText(appJs, "classifyFlashError", "desktop/src/app.js") + "\nreturn classifyFlashError;")()],
    ["flash-native.js", new Function(jsFnText(nativeJs, "classifyFlashError", "flash-native.js") + "\nreturn classifyFlashError;")()],
  ]) {
    assert.strictEqual(classify(refusal).kind, "layout", `${where} doesn't name a bootloader-placement refusal`);
  }

  // 5. …and neither failure card offers a next attempt that refuses the same
  //    image again: no clean install (erasing the chip doesn't move an image's
  //    bootloader), and on the desktop no "Try again" either (it re-runs the
  //    same install; the browser's reconnects to its picker, its only exit).
  assert.match(jsFnText(flashJs, "flashError", "flash.js"),
    /if \(!opts\.rescue && !opts\.eraseAll && v\.kind !== "layout"\)/,
    "the browser's failure card offers a clean install after a layout refusal");
  assert.match(appJs, /renderFlashFailureActions\(\{ retry: onFlash, kind: c\.kind \}\)/,
    "the desktop's failure card no longer hears which kind of failure it follows");
  const actions = jsFnText(appJs, "renderFlashFailureActions", "desktop/src/app.js");
  assert.match(actions, /const refused = opts\.kind === "layout";\s*if \(!refused\) \{\s*button\("Try again"/,
    "the desktop's failure card offers \"Try again\" after a layout refusal");
  assert.match(actions, /if \(!refused && first && !first\.checked\)/,
    "the desktop's failure card offers a clean install after a layout refusal");
  assert.match(nativeJs, /c\.kind === "layout"\s*\?\s*"Nothing was written — pick an image laid out for this chip\."/,
    "the Lab bench's failure card coaches download mode and a retry after a layout refusal");
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
  assert.match(nativeFnBody(engineRs("flash"), "flash"), /erase_first\.unwrap_or\(false\)/,
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

test("no install promises to keep the settings region: both flashers' verdicts and first-contact copy", async () => {
  // Both flashers write a factory image from offset 0 whose settings region
  // (NVS) is blank (make_factory.py; flash.js writes { data, address: 0 },
  // the engine runs write-bin 0x0), so every install, update and reinstall
  // included, starts the settings over and the next boot makes a new witness
  // key. The Update verdict said "Your settings region is left alone", the
  // Reinstall verdict "rewrites it byte-for-byte — harmless", and the
  // browser's first-contact button "it's mine — keep its data" (#1843's
  // sweep, fourth pass). Every verdict a board already set up can get (all
  // but "fresh") now ends on the same sentence, in both frontends.
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const said = /^const USB_INSTALL_STARTS_OVER =\n((?:  "[^"\n]*"(?: \+|;)\n)+)/m.exec(appJs);
  assert.ok(said, "desktop/src/app.js lost USB_INSTALL_STARTS_OVER");
  const desktopSentence = new Function(`return ${said[1].trim().replace(/;$/, "")};`)();
  const desktop = new Function(said[0] +
    jsFnText(appJs, "compareVersions", "desktop/src/app.js") + "\n" +
    jsFnText(appJs, "installVerdict", "desktop/src/app.js") + "\nreturn installVerdict;")();
  assert.equal(desktopSentence, core.USB_INSTALL_STARTS_OVER, "the two flashers say it in the same words");
  assert.match(core.USB_INSTALL_STARTS_OVER, /starts the settings region over/);
  assert.match(core.USB_INSTALL_STARTS_OVER, /a new one; an over-the-air update keeps both\.$/);
  const vision = { id: "securacv-canary-vision", name: "Canary Vision" };
  const wap = { id: "securacv-canary-wap", name: "Canary WAP" };
  const cases = { update: ["2.1.0", vision], downgrade: ["2.3.0", vision], same: ["2.2.0", vision],
                  switch: ["2.2.0", wap], unknown: ["??", vision] };
  for (const [kind, [ver, was]] of Object.entries(cases)) {
    for (const [who, fn] of [["browser", core.installVerdict], ["desktop", desktop]]) {
      const v = fn({ current: { version: ver }, currentProduct: was, product: vision, version: "2.2.0" });
      assert.equal(v.kind, kind, `${who} ${kind}`);
      assert.ok(v.detail.endsWith(core.USB_INSTALL_STARTS_OVER), `${who} ${kind}: ${v.detail}`);
      assert.doesNotMatch(v.detail, /left alone|harmless|byte-for-byte/, `${who} ${kind}: ${v.detail}`);
    }
  }
  // The browser's first-contact claim waives the full erase and nothing
  // more: the install still writes the settings region.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  assert.match(flashJs, /el\("button", "ghost small", "it's mine — skip the full erase"\)/);
  assert.doesNotMatch(flashJs, /keep its data|keep its\s*" \+\s*"identity key|keep its identity key/);
  assert.match(flashJs, /Its identity key and settings start over " \+\s*"either way/);
  // The desktop's box says the same of unticking it.
  const html = read(join(ROOT, "desktop/src/index.html"));
  assert.match(html, /so the board comes up with a new identity key either way/);
});

test("security fuses: both flashers read them, off ONE field table", () => {
  // This used to assert the desktop app disclosed the check as browser-only
  // (espflash has no fuse-read command — still true of v3.3.0 and v4.3.0).
  // The desktop now speaks the read-only sliver of the ROM protocol itself
  // (efuse.rs: SYNC + READ_REG, nothing that can write), so the gap is
  // closed — and the risk moves to the two field tables drifting apart.
  // Pin every (key, bit, width) between intake.js and efuse.rs.
  const browser = read(join(CANARY, "assets/flash.js"));
  const intakeJs = read(join(CANARY, "assets/intake.js"));
  const efuseRs = read(join(ROOT, "desktop/src-tauri/src/efuse.rs"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));

  // Both sides still HAVE the check, wired to their UIs.
  assert.match(browser, /efuseBlock0Addrs|readSecurityEfuses/,
    "browser flasher lost its security-fuse read");
  assert.match(html, /id="coldstart-efuse"/, "desktop lost its fuse-check UI");
  assert.match(appJs, /invoke\("read_security_efuses"/,
    "desktop frontend no longer invokes the native fuse read");
  assert.match(libRs, /efuse::read_security_efuses/,
    "native fuse-read command fell out of the invoke handler");

  // The intake doc's "where the two flashers differ" section is the public
  // record of this check; it must describe the native read, not the old gap.
  const intakeDoc = read(join(ROOT, "docs/unflashed_board_intake.md"));
  assert.ok(intakeDoc.includes("desktop/src-tauri/src/efuse.rs"),
    "docs/unflashed_board_intake.md no longer names efuse.rs — its two-flashers section is stale");
  assert.doesNotMatch(intakeDoc, /genuinely browser-only|desktop app cannot read eFuses/,
    "docs/unflashed_board_intake.md still says the desktop cannot read eFuses");

  // The block-0 geometry agrees.
  const jsOffset = intakeJs.match(/EFUSE_BLOCK0_RD_OFFSET\s*=\s*(0x[0-9a-fA-F]+)/);
  const rsOffset = efuseRs.match(/EFUSE_BLOCK0_RD_OFFSET:\s*u32\s*=\s*(0x[0-9a-fA-F_]+)/);
  assert.ok(jsOffset && rsOffset, "couldn't read EFUSE_BLOCK0_RD_OFFSET from both sides");
  assert.strictEqual(parseInt(rsOffset[1].replace(/_/g, ""), 16), parseInt(jsOffset[1], 16),
    "block-0 read offset drifted between intake.js and efuse.rs");
  const jsWords = intakeJs.match(/EFUSE_BLOCK0_WORDS\s*=\s*(\d+)/);
  const rsWords = efuseRs.match(/EFUSE_BLOCK0_WORDS:\s*usize\s*=\s*(\d+)/);
  assert.ok(jsWords && rsWords && jsWords[1] === rsWords[1],
    "block-0 word count drifted between intake.js and efuse.rs");

  // Every security field: same key, same bit, same width, both sides. The
  // browser gets EFUSE_BASE from the vendored esptool at runtime; native pins
  // the bases, so each pinned base must exist in the vendored bundle.
  const fieldSet = (pairs) => new Set(pairs.map(([k, b, w]) => `${k}@${b}@${w}`));
  const jsFields = fieldSet(
    [...intakeJs.matchAll(/key:\s*"([A-Z0-9_]+)",\s*bit:\s*(\d+),\s*width:\s*(\d+)/g)]
      .map((m) => [m[1], m[2], m[3]]));
  const rsFields = fieldSet([
    ...[...efuseRs.matchAll(/field\(\s*"([A-Z0-9_]+)",\s*(\d+),\s*(\d+),/g)].map((m) => [m[1], m[2], m[3]]),
    ...[...efuseRs.matchAll(/key:\s*"([A-Z0-9_]+)",\s*bit:\s*(\d+),\s*width:\s*(\d+)/g)].map((m) => [m[1], m[2], m[3]]),
    ...[...efuseRs.matchAll(/\(\s*"([A-Z0-9_]+)",\s*(\d+),\s*(\d+)\s*\)/g)].map((m) => [m[1], m[2], m[3]]),
  ]);
  assert.ok(jsFields.size >= 10, "couldn't parse the intake.js field tables");
  assert.deepStrictEqual([...rsFields].sort(), [...jsFields].sort(),
    "the security-fuse field tables drifted between intake.js and efuse.rs");

  // Per-chip USB tables can't be swapped between chips unnoticed: compare
  // each chip's block on both sides, not just the union.
  for (const chip of ["ESP32-S3", "ESP32-C3", "ESP32-C6"]) {
    const jsBlock = intakeJs.match(new RegExp(`"${chip}":\\s*\\[([^\\]]*)\\]`));
    const rsBlock = efuseRs.match(new RegExp(`"${chip}" => Some\\(vec!\\[([^\\]]*)\\]`));
    assert.ok(jsBlock && rsBlock, `couldn't find the ${chip} USB-fuse table on both sides`);
    const tuples = (s, re) => [...s.matchAll(re)].map((m) => `${m[1]}@${m[2]}`).sort();
    assert.deepStrictEqual(
      tuples(rsBlock[1], /"([A-Z0-9_]+)",\s*(\d+)/g),
      tuples(jsBlock[1], /key:\s*"([A-Z0-9_]+)",\s*bit:\s*(\d+)/g),
      `${chip} USB-fuse bits drifted between intake.js and efuse.rs`);
  }

  // Native EFUSE_BASE addresses come from the vendored esptool-js bundle.
  const bundle = read(join(CANARY, "assets/vendor/esptool-js/bundle.js"));
  const bundleBases = new Set([...bundle.matchAll(/EFUSE_BASE\s*=\s*(\d+)/g)].map((m) => Number(m[1])));
  const nativeBases = [...efuseRs.matchAll(/"(ESP32-[A-Z0-9]+)"\s*=>\s*Some\(0x([0-9a-fA-F_]+)\)/g)]
    .map((m) => [m[1], parseInt(m[2].replace(/_/g, ""), 16)]);
  assert.ok(nativeBases.length >= 3, "couldn't parse efuse.rs EFUSE_BASE table");
  for (const [chip, base] of nativeBases) {
    assert.ok(bundleBases.has(base),
      `efuse.rs pins EFUSE_BASE ${base.toString(16)} for ${chip}, but the vendored ` +
      "esptool-js bundle has no chip at that base — the address is wrong or esptool moved it");
  }
});

test("neither flasher hands a BOOT-held board to its firmware: not at the end of the fuse read, not on the hello card", () => {
  // Holding BOOT is what keeps a marketplace board's firmware from running
  // (on native USB it can type at the desktop). The desktop's fuse read used
  // to end in a hard reset, and to enter the ROM with the classic sequence on
  // a USB-Serial/JTAG port, which holds RTS alone first: that port's own hard
  // reset. The browser's live voice booted any readable firmware right after
  // intake. efuse.rs's own tests replay the read's line moves against a
  // scripted board; these hold the places that used to boot it.
  const efuseRs = read(join(ROOT, "desktop/src-tauri/src/efuse.rs"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  assert.doesNotMatch(efuseRs, /fn hard_reset/,
    "the desktop fuse read can reset the board into its firmware again");
  // The About rows' block-1/2 reads ride the same ROM session, between block 0
  // and the park, and cannot leave the function early (they answer an Option).
  assert.match(efuseRs,
    /let read = read_block0_in_rom\(link, efuse_base, usb_serial_jtag\)\s*\.map\(\|words\| \(words, read_chip_fields\(link, efuse_base, chip\)\)\);\s*park\(link\);\s*read\s*\}/,
    "the desktop fuse read no longer parks its lines on every way out");
  assert.match(efuseRs, /fn read_chip_fields<L: RomLink>\([^)]*\) -> Option<ChipFields> \{/,
    "the About rows' fuse read can return an error past the park");
  assert.match(efuseRs, /if attempt == 0 && !usb_serial_jtag \{\s*classic_reset\(link\)\?;/,
    "the desktop fuse read runs the classic sequence on a USB-Serial/JTAG port again");
  // That choice is the port's own: the port the read opens is asked what it
  // is, and that answer, untouched, is what reaches the enter sequence.
  const fromPort = nativeFnBody(efuseRs, "read_fuses_from_port");
  assert.match(fromPort, /\{\s*let usb_serial_jtag = is_usb_serial_jtag\(port_name\);/,
    "the desktop fuse read no longer asks the port whether it is a USB-Serial/JTAG port");
  assert.match(fromPort, /\n    read_fuses\(&mut link, efuse_base, chip, usb_serial_jtag\)\s*\}\s*$/,
    "the desktop fuse read no longer hands the port's kind to the enter sequence");
  assert.equal((fromPort.match(/\busb_serial_jtag\b/g) || []).length, 2,
    "the port's kind is changed between the port and the enter sequence");
  // Every reset the module can issue lives in the two enter sequences: outside
  // them (and the tests' scripted boards) nothing asserts RTS, and the port's
  // own RTS write is reached only through PortLines::rts.
  const shipped = efuseRs.slice(0, efuseRs.indexOf("#[cfg(test)]"));
  const outsideEnter = shipped.replace(/\nfn (classic_reset|usb_jtag_serial_reset)\([\s\S]*?\n\}\n/g, "\n");
  assert.equal((shipped.match(/\nfn (classic_reset|usb_jtag_serial_reset)\(/g) || []).length, 2,
    "efuse.rs's two enter sequences moved; re-point this pin");
  assert.doesNotMatch(outsideEnter, /rts\(true\)/,
    "the desktop fuse read asserts RTS outside its enter sequences: a reset into the firmware");
  assert.equal((shipped.match(/write_request_to_send\(/g) || []).length, 1,
    "the desktop fuse read drives RTS around PortLines::rts");
  assert.match(flashJs,
    /if \(state\.current && !state\.current\.unknown && state\.heldBoot === false\) \{\s*const voice = el\("section", "flash-card flash-voice"\)/,
    "the browser's live voice boots a board the user held BOOT on, or didn't say");
  assert.match(flashJs, /function coldStartCard\(\) \{[\s\S]{0,400}state\.heldBoot = null;/,
    "the browser's cold-start answer leaks from one board to the next");
});

test("health check: native parsers pin the browser's byte-magics, and the UI reaches the command", () => {
  // The native health parsers (health.rs) reimplement the browser's flash-core
  // parsers. Their magics/offsets/namespaces MUST match byte-for-byte, or the
  // same chip reads differently on each surface. Pin the shared constants.
  const healthRs = engineRs("health");
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

test("the catalog hatch moment renders on both flashers, behind the same receipt gate", () => {
  // The post-flash "first flight" (products[].hatch, authored once in
  // gen_flash.py's HATCH_MOMENTS) used to render only on the desktop — the
  // browser validated it and then never showed it, so half the users kept
  // the generic next-step card. Both frontends must render the catalog copy,
  // and both must honor serial_receipt the same way: false → the steps land
  // right after the write; anything else → they wait for the board's live
  // serial self-manifest (the boot receipt).
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const browser = read(join(CANARY, "assets/flash.js"));
  const flashCore = read(join(CANARY, "assets/flash-core.js"));

  // Desktop: catalog hatch preferred over hardcoded copy, receipt-gated.
  assert.match(appJs, /product\.hatch && Array\.isArray\(product\.hatch\.steps\)/,
    "desktop hatchMoment no longer prefers the catalog's product.hatch");
  assert.match(appJs, /function requiresLiveReceipt/,
    "desktop lost requiresLiveReceipt — the receipt gate on its hatch card");

  // Browser: the same gate lives once in flash-core (pure, host-tested)…
  assert.match(flashCore, /export function requiresLiveReceipt/,
    "browser lost core.requiresLiveReceipt — the shared receipt gate");
  assert.match(flashCore, /export function hatchMoment/,
    "browser lost core.hatchMoment — the catalog hatch can never render");
  // …and both surfaces actually render it: the done card for receipt-less
  // products, the monitor's identity card for receipt-gated ones.
  const hatchCard = read(join(CANARY, "assets/hatch-card.js"));
  assert.match(hatchCard, /export function hatchMomentCard/,
    "browser flasher lost the shared hatch-steps card (assets/hatch-card.js)");
  assert.match(browser, /hatchMomentCard/,
    "flash.js no longer mounts the hatch-steps card");
  assert.match(browser, /!core\.requiresLiveReceipt\(product\)/,
    "the browser done card no longer shows hatch steps for serial_receipt:false products");
  assert.match(browser, /core\.requiresLiveReceipt\(opts\.hatchProduct\)/,
    "the browser monitor no longer shows hatch steps when the boot receipt lands");

  // The Vision routing — the gap Codex flagged on #1569. A Vision's
  // serial_receipt is true but its prove path is the camera bench, never the
  // monitor, so the receipt-gated monitor card above can NEVER fire for it;
  // without an explicit route, Vision owners are the only users who never
  // see their first flight. The browser's "after both receipts" moment is
  // the two-port pair completing, and a session can finish the pair on
  // either screen — so BOTH must mount the hatch when both ports are in:
  // flash.js's done card (module-first sessions) and we2-flash.js's
  // module-done screen (ESP32-first sessions, the routed path).
  const we2FlashJs = read(join(CANARY, "assets/we2-flash.js"));
  assert.match(browser, /parts\.esp32 && parts\.we2[\s\S]{0,120}hatchMomentCard/,
    "flash.js's done card no longer hatches a completed Vision pair (module-first sessions)");
  assert.match(we2FlashJs, /parts\.esp32 && parts\.we2[\s\S]{0,400}hatchMomentCard/,
    "we2-flash.js's module-done screen no longer hatches a completed Vision pair");
  assert.match(we2FlashJs, /isVisionBoard\(p\)/,
    "we2-flash.js must pick a catalog vision product for the hatch copy — " +
    "the vision products share one hatch moment (pinned in flash.test.js)");
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
    // The flagship tree has no runtime_config.h (this used to try to read one
    // and pass on the failed read). Its broker reader is securacv_mqtt.cpp,
    // and it reads a different NVS shape from the one either flasher writes:
    // mqtt_host as a blob (getBytes) and the port as a u32 (getUInt), where
    // both builders write a string and a u16 — so a seed from here would read
    // as empty, and broker_nvs is honestly false. Pin the reason, so a reader
    // that moves to the seed's shape fails here and the capability is revisited.
    if (p.family === "canary") {
      const mqtt = read(join(ROOT, proj, "lib/securacv_mqtt/src/securacv_mqtt.cpp"));
      assert.match(mqtt, /nvs\.getBytes\(NVS_KEY_MQTT_HOST, /,
        "the flagship's broker reader no longer reads mqtt_host as a blob — if it now reads the flasher's string " +
        "entry, teach gen_flash.py reads_broker() this file and let broker_nvs follow");
      assert.match(mqtt, /nvs\.getUInt\(NVS_KEY_MQTT_PORT, /, "the flagship's broker reader no longer reads the port as a u32");
      assert.equal(p.broker_nvs, false, `${p.id}: the flagship cannot read the flashers' broker seed, so broker_nvs must be false`);
      continue;
    }
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

test("broker TLS: both flashers offer the mode, the CA and the fingerprint, and read them the same way", async () => {
  // Wave 3 landed the three NVS keys in both builders (the key-set test above
  // already compares them) but no form on either side — so a frontend could
  // lose, or never gain, the controls while that test stayed green. This is
  // the auto-update pattern: capability parity, pinned to the firmware's own
  // table, on BOTH frontends at once (AGENTS.md rule 7).
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const provRs = engineRs("provisioning");
  const logicH = read(join(ROOT, "firmware/common/network/mqtt_transport_logic.h"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);

  // The mode table is the firmware's ("do not renumber"); both frontends
  // offer exactly it, in the same words, with plain selected on both.
  const fwModes = [...logicH.matchAll(/^\s*(?:Plain|Ca|Fingerprint|InsecureLab)\s*=\s*(\d)/gm)]
    .map((m) => Number(m[1]));
  assert.deepStrictEqual(fwModes, [0, 1, 2, 3], "mqtt_transport_logic.h Mode table moved");
  assert.deepStrictEqual(core.MQTT_TLS_MODES, { plain: 0, ca: 1, fingerprint: 2, insecure: 3 });
  const optBlock = /const MQTT_TLS_OPTIONS = \[([\s\S]*?)\];/.exec(flashJs);
  assert.ok(optBlock, "flash.js lost MQTT_TLS_OPTIONS (the browser's mode select)");
  const browserOpts = [...optBlock[1].matchAll(/\[(\d), "([^"]+)"\]/g)].map((m) => [Number(m[1]), m[2]]);
  const selBlock = /<select id="mqtt-tls"[^>]*>([\s\S]*?)<\/select>/.exec(html);
  assert.ok(selBlock, "index.html lost select#mqtt-tls (the desktop's mode select)");
  const desktopOpts = [...selBlock[1].matchAll(/<option value="(\d)"(?: selected)?>([^<]+)<\/option>/g)]
    .map((m) => [Number(m[1]), m[2]]);
  assert.deepStrictEqual(browserOpts.map((o) => o[0]), fwModes, "browser offers a mode set that is not the firmware's");
  assert.deepStrictEqual(desktopOpts, browserOpts, "TLS mode options differ between the two flashers");
  // The lab option says what it is, and neither frontend preselects it —
  // not by default, and (desktop) not by a remembered profile either.
  const lab = browserOpts.find((o) => o[0] === 3)[1];
  assert.match(lab, /NOT verified/, "the lab option must say the broker is not verified");
  assert.match(lab, /warns on every connect/, "the lab option must say the firmware warns on every connect");
  assert.match(flashJs, /tlsSel\.value = String\(core\.MQTT_TLS_MODES\.plain\)/, "browser select must default to plain");
  assert.match(selBlock[1], /<option value="0" selected>/, "desktop select must default to plain");
  assert.ok(!/<option value="[1-3]" selected/.test(selBlock[1]), "desktop preselects a TLS mode");
  assert.match(appJs, /if \(\$\("mqtt-tls"\)\.value === String\(MQTT_TLS\.insecure\)\) \$\("mqtt-tls"\)\.value = String\(MQTT_TLS\.plain\)/,
    "desktop restoreProv must not carry the lab mode forward from the profile");
  // The desktop select is the owner's REMEMBERED mode. tlsRowsRefresh never
  // resets it for a product with no broker block (review of the first
  // version: it did, and the next persistProv wrote plain over the profile
  // for every Canary or WAP chosen after a display), and for a plain-only
  // product it PARKS the mode behind the Plain it shows — the profile keeps
  // the owner's, persistProv skips the stand-in, the next product gets it
  // back. Held by the shape of the one place that writes the select.
  const rows = /function tlsRowsRefresh\(product = state\.product\) \{([\s\S]*?)\n\}/.exec(appJs);
  assert.ok(rows, "desktop tlsRowsRefresh moved");
  assert.deepStrictEqual(
    rows[1].split("\n").filter((l) => /\bsel\.value = /.test(l)).map((l) => l.trim()),
    ["sel.value = String(MQTT_TLS.plain);", "sel.value = tlsParked;"],
    "tlsRowsRefresh may write the select only to park (plain-only product) or to unpark");
  assert.match(rows[1],
    /if \(broker && !tlsOk\) \{\s*if \(tlsParked === null\) tlsParked = sel\.value;\s*sel\.value = String\(MQTT_TLS\.plain\);\s*\} else if \(tlsParked !== null\) \{\s*sel\.value = tlsParked;\s*tlsParked = null;\s*\}/,
    "the Plain shown for a plain-only product must park the owner's mode, never replace it; no broker block → hands off");
  assert.match(appJs, /function persistProv\(\) \{[\s\S]*?if \(id === "mqtt-tls" && tlsParked !== null\) return;[\s\S]*?\n\}/,
    "persistProv must not write a parked (stand-in) TLS mode over the owner's profile");
  // The CA and the pin are `required` exactly while shown (an empty one is
  // caught by the form's own prompt before the native builder refuses the
  // incomplete pair), and the rows are refreshed for every product chosen.
  assert.match(rows[1], /\$\("mqtt-ca"\)\.required = broker && mode === MQTT_TLS\.ca;/, "desktop CA must be required exactly while shown");
  assert.match(rows[1], /\$\("mqtt-fp"\)\.required = broker && mode === MQTT_TLS\.fingerprint;/, "desktop pin must be required exactly while shown");
  const chosen = /function onProductChosen\(p, ver\) \{([\s\S]*?)\n\}/.exec(appJs);
  assert.ok(chosen, "desktop onProductChosen moved");
  assert.match(chosen[1], /^\s*tlsRowsRefresh\(p\);/m, "onProductChosen must refresh the TLS rows for the product just chosen");

  // The fingerprint spelling: ONE pattern string per frontend, pinned equal,
  // and it is the firmware's set (fingerprint_normalize: any run of ':' or
  // ' ' between pairs, none inside a pair, either case) — wider than the
  // shared builder's own check, which is why both fold to 64 hex first.
  const pat = /const MQTT_FP_PATTERN = "([^"]+)"/.exec(flashJs);
  assert.ok(pat, "flash.js lost MQTT_FP_PATTERN");
  const htmlPat = /<input id="mqtt-fp"[^>]*\spattern="([^"]+)"/.exec(html);
  assert.ok(htmlPat, "index.html #mqtt-fp lost its pattern");
  assert.strictEqual(htmlPat[1], pat[1], "the two flashers accept different fingerprint spellings");
  assert.match(appJs, /new RegExp\(`\^\(\?:\$\{\$\("mqtt-fp"\)\.pattern\}\)\$`\)/,
    "desktop fingerprintNormalize must read the input's own pattern (one source per side)");
  // The placeholders too — the most likely paste is openssl's whole output
  // line, which neither pattern takes, so both say which part to paste.
  const fpPh = /const MQTT_FP_PLACEHOLDER =\s*"([^"]+)"/.exec(flashJs);
  const htmlFpPh = /<input id="mqtt-fp"[^>]*\splaceholder="([^"]+)"/.exec(html);
  assert.ok(fpPh && htmlFpPh, "a fingerprint placeholder moved");
  assert.strictEqual(htmlFpPh[1], fpPh[1], "the two flashers give different fingerprint hints");
  assert.match(fpPh[1], /Fingerprint=/, "the hint must name the part of openssl's line to paste (after Fingerprint=)");
  const caPh = /const MQTT_CA_PLACEHOLDER =\s*"([^"]+)"/.exec(flashJs);
  const htmlCaPh = /<textarea id="mqtt-ca"[^>]*\splaceholder="([^"]+)"/.exec(html);
  assert.ok(caPh && htmlCaPh, "a CA placeholder moved");
  assert.strictEqual(htmlCaPh[1], caPh[1], "the two flashers give different CA hints");
  const re = new RegExp(`^${pat[1]}$`);
  const hex = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
  const pairs = hex.match(/../g);
  const accepted = [hex, hex.toUpperCase(), pairs.join(":"), pairs.join(" "), pairs.join("  "),
    pairs.join(": "), ":" + pairs.join("::") + " ", pairs.join(":").toUpperCase()];
  const rejected = [hex.slice(0, 62), hex + "aa", pairs.join("-"), hex.slice(0, 63) + "g",
    "0a1:b" + pairs.slice(2).join(":"), pairs.slice(0, 31).join(":"), ""];
  for (const s of accepted) assert.ok(re.test(s), `pattern must accept ${JSON.stringify(s)} (the firmware does)`);
  for (const s of rejected) assert.ok(!re.test(s), `pattern must reject ${JSON.stringify(s)} (the firmware does)`);
  // provisioning.rs still skips both separators anywhere (the same set)...
  assert.match(provRs, /b\[i\] == b':' \|\| b\[i\] == b' '/, "provisioning.rs fingerprint_shape_ok no longer skips ':' and ' '");
  // ...and the folded 64-hex form is what the shared browser builder seeds.
  for (const s of accepted) {
    const folded = s.replace(/[: ]/g, "");
    const out = core.mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 2, mqttFp: folded });
    assert.strictEqual(out.strings.mqtt_fp, folded);
  }
  assert.match(flashJs, /mqttFp: mode === core\.MQTT_TLS_MODES\.fingerprint \? mqttFingerprintNormalize\(fp\.value\) : ""/,
    "browser values() must fold the fingerprint and send it for the pin mode only");
  assert.match(appJs, /mqttFp: tlsMode === MQTT_TLS\.fingerprint \? fingerprintNormalize\(\$\("mqtt-fp"\)\.value\) : ""/,
    "desktop readProvisioning must fold the fingerprint and send it for the pin mode only");

  // The CA: the builders' 3070-byte cap on both forms (+ "\n" = the
  // firmware's 3071), shown for the CA mode only, sent for the CA mode only.
  assert.match(flashJs, /const MQTT_CA_MAX = 3070/);
  assert.match(html, /<textarea id="mqtt-ca"[^>]*\smaxlength="3070"/);
  assert.match(provRs, /byte_len\(ca\) > 3070/);
  assert.match(flashJs, /ca\.classList\.toggle\("flash-hidden", mode !== core\.MQTT_TLS_MODES\.ca\)/);
  assert.match(appJs, /\$\("mqtt-ca-row"\)\.classList\.toggle\("hidden", !\(broker && mode === MQTT_TLS\.ca\)\)/);
  assert.match(flashJs, /mqttTls: mode,/);
  assert.match(flashJs, /mqttCa: mode === core\.MQTT_TLS_MODES\.ca \? ca\.value : ""/);
  assert.match(appJs, /mqttTls: tlsMode,/);
  assert.match(appJs, /mqttCa: tlsMode === MQTT_TLS\.ca \? \$\("mqtt-ca"\)\.value\.trim\(\) : ""/);

  // The port: SUGGESTED, never rewritten. Both name the firmware's own
  // failure text and offer a button; the only assignment of 8883 to the
  // port on either side is inside a click handler.
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop index.html", html]]) {
    assert.ok(src.includes("the broker did not speak TLS on this port"), `${label} lost the 8883 suggestion`);
    assert.ok(src.includes("Use 8883"), `${label}: the port suggestion must be a button, not a rewrite`);
  }
  const browserSets = flashJs.split("\n").filter((l) => l.includes('port.value = "8883"'));
  const desktopSets = appJs.split("\n").filter((l) => l.includes('$("mqtt-port").value = "8883"'));
  assert.strictEqual(browserSets.length, 1); assert.match(browserSets[0], /addEventListener\("click"/);
  assert.strictEqual(desktopSets.length, 1); assert.match(desktopSets[0], /addEventListener\("click"/);

  // Desktop persistence: mode, CA and pin are non-secrets and live in the
  // prefs profile with the host — never in the OS secret store.
  const pf = /const PROV_FIELDS = \[([^\]]+)\]/.exec(appJs);
  assert.ok(pf, "desktop PROV_FIELDS moved");
  for (const id of ["mqtt-tls", "mqtt-ca", "mqtt-fp"]) {
    assert.ok(pf[1].includes(`"${id}"`), `desktop PROV_FIELDS must remember ${id} (a non-secret, per the fleet book)`);
  }
  assert.ok(!/secretStore\.(?:set|get)\([^)]*(?:mqtt-ca|mqtt-fp|mqttCa|mqttFp)/.test(appJs),
    "the broker CA / pin must never take the OS-secret-store route");
  // Native never logs the config: Provisioning has no Debug derive to print.
  assert.ok(!/derive\([^)]*Debug[^)]*\)\]\s*\n\s*#\[serde\(rename_all = "camelCase"\)\]\s*\npub struct Provisioning/.test(provRs),
    "provisioning.rs Provisioning must stay un-Debug so a CA can never be formatted into a log");
});

test("broker TLS: both flashers' receipts name the sealed mode the same way", async () => {
  // Wave 6 sealed mqtt_tls / mqtt_ca / mqtt_fp on both sides and neither
  // receipt said so. Now each flasher prints ONE line about the broker TLS
  // it SEALED — sealed, never connected: the firmware decides the transport
  // at connect time (mqtt_transport_logic.h) and the boot self-manifest
  // carries no transport field. The two flashers share no UI code, so the
  // four-row table lives twice (flash-core.js MQTT_TLS_RECEIPT,
  // desktop/flash-engine/src/broker_receipt.rs MODE_LABELS — the engine both
  // desktop apps flash with) and this test is the only thing holding the
  // copies equal: it must fail on either one changing alone.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const coreJs = read(join(CANARY, "assets/flash-core.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const provRs = engineRs("provisioning");
  const receiptRs = engineRs("broker_receipt");
  const engineLibRs = engineRs("lib");
  const flashRs = engineRs("flash");
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);

  // ONE table. The Rust literals are read as source text, one per line (the
  // way the select's options are matched above), so a rustfmt split or a
  // hand edit to either copy alone fails here by name.
  const labels = /pub\(crate\) const MODE_LABELS: \[&str; 4\] = \[\n([\s\S]*?)\n\];/.exec(receiptRs);
  assert.ok(labels, "broker_receipt.rs lost MODE_LABELS (the native receipt table)");
  const rustLabels = labels[1].split("\n").map((l) => {
    const m = /^ {4}"([^"]+)",$/.exec(l);
    assert.ok(m, `MODE_LABELS must be four single-line literals, one per line — got ${JSON.stringify(l)}`);
    return m[1];
  });
  assert.strictEqual(rustLabels.length, 4, "the firmware knows four modes (mqtt_transport_logic.h kModeCount)");
  assert.deepStrictEqual([...core.MQTT_TLS_RECEIPT], rustLabels, "the receipt table differs between the two flashers");
  const unknown = /pub\(crate\) const MODE_UNKNOWN: &str =\n\s+"([^"]+)";/.exec(receiptRs);
  assert.ok(unknown, "broker_receipt.rs lost MODE_UNKNOWN");
  assert.strictEqual(core.MQTT_TLS_RECEIPT_UNKNOWN, unknown[1], "the unknown-mode line differs between the two flashers");
  // Keyed by the firmware's byte, in its order; each row says what was
  // sealed, never that anything connected; the lab row keeps its warnings.
  assert.match(rustLabels[0], /^plain MQTT — not encrypted/);
  assert.ok(rustLabels[1].includes("{N} bytes of PEM") && !rustLabels[1].includes("{FP}"), "row 1 carries the CA's byte count only");
  assert.ok(rustLabels[2].includes("{FP}") && !rustLabels[2].includes("{N}"), "row 2 carries the pin only");
  assert.match(rustLabels[3], /NOT verified/);
  assert.match(rustLabels[3], /warns on every connect/);
  for (const row of rustLabels) {
    assert.ok(!/connected|verified link|encrypted link/i.test(row), `a receipt row claims a connection: ${row}`);
  }

  // The byte count is the SEALED string's length on both sides — the trimmed
  // PEM plus the newline build_nvs / mqttProvisioningToNvs append. Proven for
  // ONE paste: the Rust tests' PEM fixture through the browser's builder and
  // formatter gives a number, and the Rust test pins broker_tls_summary (and
  // the NVS string item it writes) to that same literal.
  const pemConst = /const PEM: &str = "((?:[^"\\]|\\.)+)";/.exec(provRs);
  assert.ok(pemConst, "provisioning.rs tests lost their PEM fixture");
  const pem = JSON.parse(`"${pemConst[1]}"`);
  const prov = core.mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 1, mqttCa: `  ${pem}\n\n` });
  assert.strictEqual(prov.strings.mqtt_ca, pem.trim() + "\n");
  const sealedBytes = new TextEncoder().encode(prov.strings.mqtt_ca).length;
  assert.strictEqual(core.brokerTlsReceipt(prov), rustLabels[1].replace("{N}", String(sealedBytes)));
  assert.ok(provRs.includes(`assert_eq!(broker_tls_summary(&ca), Some((1, ${sealedBytes}, String::new())));`),
    `provisioning.rs's broker_tls_summary test must pin the CA fixture to ${sealedBytes} sealed bytes — the number the browser computes for the same paste`);
  assert.match(provRs, /let ca_sealed_len = if ca\.is_empty\(\) \{ 0 \} else \{ ca\.len\(\) \+ 1 \};/,
    "native must count the trimmed PEM + the newline build_nvs appends");
  assert.match(provRs, /writer\.string\("mqtt_ca", &format!\("\{ca\}\\n"\)\)/,
    "build_nvs no longer seals trimmed PEM + newline — the count above describes bytes that are not written");
  assert.match(provRs, /pub\(crate\) fn broker_tls_summary\(config: &Provisioning\) -> Option<\(u8, usize, String\)> \{\n\s+if config\.mqtt_host\.is_empty\(\) \{\n\s+return None;/,
    "native: no broker host sealed, no receipt line");

  // Native: the log line rides under the broker branch of the engine's
  // `flash` only (both desktop apps run it), is built from the summary + the
  // std-only formatter (never a config field), and the sealed line stopped
  // saying "values not logged" now that a pin is printed. The receipt
  // carries it; the Flasher's local-file path (still in its lib.rs) carries
  // None.
  assert.match(engineLibRs, /^pub mod broker_receipt;$/m, "flash-engine lib.rs must register the receipt module");
  const flashFn = nativeFnBody(flashRs, "flash");
  assert.ok(flashFn.includes("(passwords not logged)"), "the sealed line must say passwords, not values, are withheld");
  assert.ok(!nativeRs.includes("values not logged"), "'values not logged' is false once the pin is printed");
  const branch = /\n( +)if broker \{\n([\s\S]*?)\n\1\}/.exec(flashFn);
  assert.ok(branch, "flash-engine flash.rs lost its `if broker {` branch");
  assert.match(branch[2], /if let Some\(\(mode, ca_bytes, fingerprint\)\) = provisioning::broker_tls_summary\(config\)/);
  assert.match(branch[2], /let line = broker_receipt::broker_tls_receipt\(mode, ca_bytes, &fingerprint\);/);
  assert.match(branch[2], /emit\(format!\("  broker link: \{line\}"\)\);/);
  assert.match(branch[2], /broker_tls = Some\(BrokerTlsReceipt \{\n\s+mode,\n\s+ca_bytes,\n\s+fingerprint,\n\s+line,\n\s+\}\);/);
  assert.strictEqual(nativeRs.split("broker link:").length - 1, 1, "the broker line is emitted in exactly one place — the engine's broker branch");
  assert.ok(!/config\.mqtt_(?:ca|pass|host|fp|user)\b/.test(flashFn.replace(/config\.mqtt_host\.is_empty\(\)/g, "")),
    "flash() must not format a broker field of the config into a log line");
  assert.ok(!/config\.wifi_pass/.test(flashFn), "flash() must never touch the Wi-Fi password");
  assert.match(flashRs, /pub struct FlashReceipt \{[\s\S]*?pub broker_tls: Option<BrokerTlsReceipt>,\n\}/, "the engine's FlashReceipt lost broker_tls");
  assert.match(flashRs, /#\[derive\(Serialize\)\]\npub struct BrokerTlsReceipt \{\n\s+mode: u8,\n\s+ca_bytes: usize,\n\s+fingerprint: String,\n\s+line: String,\n\}/,
    "BrokerTlsReceipt must serialize mode / ca_bytes / fingerprint / line in snake_case, like the rest of the receipt");
  assert.match(nativeFnBody(libRs, "flash_local_file"), /broker_tls: None,/, "a local file seals nothing — its receipt carries None");
  // The Lab's flash command returns the engine's receipt as-is, so its
  // frontend sees the same broker_tls field the Flasher's does.
  assert.match(nativeFnBody(read(join(ROOT, "desktop-lab/src-tauri/src/flash.rs")), "flash"),
    /flash_engine::flash::flash\(&TauriHost\(app\), bundled_catalog\(\), request\)\.await/,
    "the Lab must return the engine's receipt unchanged");

  // Neither formatter can be handed the PEM: native takes a length, the
  // browser measures the builder's sealed string and nothing else of it; and
  // the std-only module never names the config, a password or the host.
  assert.match(receiptRs, /pub\(crate\) fn broker_tls_receipt\(mode: u8, ca_sealed_len: usize, fp: &str\) -> String/,
    "the native formatter takes the CA's sealed LENGTH, never the PEM");
  assert.ok(!/mqtt_pass|wifi_pass|mqtt_host|Provisioning/.test(receiptRs),
    "broker_receipt.rs must not know the config, a password or the host");
  assert.ok(!/^use /m.test(receiptRs.split("#[cfg(test)]")[0]),
    "broker_receipt.rs is std-only — `rustc --edition 2021 --test` compiles it alone, without the engine's dependencies");
  const fmt = /export function brokerTlsReceipt\(prov\) \{([\s\S]*?)\n\}/.exec(coreJs);
  assert.ok(fmt, "flash-core.js lost brokerTlsReceipt");
  const caLines = fmt[1].split("\n").filter((l) => l.includes("mqtt_ca"));
  assert.deepStrictEqual(caLines.map((l) => l.trim()),
    ["const caBytes = strings.mqtt_ca ? new TextEncoder().encode(strings.mqtt_ca).length : 0;"],
    "the browser formatter touches the sealed CA on one line — to measure it — and reports nothing else of it");
  assert.ok(!/mqtt_pass|mqttPass|mqttCa\b|mqtt_host\b(?!\))/.test(fmt[1].replace("if (!strings.mqtt_host) return null;", "")),
    "the browser formatter must not read a password, the form's CA, or print the host");

  // Desktop UI: a fourth receipt row, rendered from the RETURNED receipt
  // (host.broker_tls.line), never from the form.
  assert.match(html, /<div id="receipt-host-broker" class="receipt pending hidden">/, "index.html lost #receipt-host-broker");
  const render = /function renderReceipts\(forceVision = false\) \{([\s\S]*?)\n\}/.exec(appJs);
  assert.ok(render, "desktop renderReceipts moved");
  assert.match(render[1], /const brokerTls = host && host\.broker_tls;/, "the desktop row reads the writer's receipt");
  assert.match(render[1], /\$\("receipt-host-broker"\)\.classList\.toggle\("hidden", !brokerTls\);/, "hidden when no broker host was sealed");
  assert.match(render[1], /setReceipt\("receipt-host-broker", true, "✓ " \+ brokerTls\.line\)/, "the desktop renders the line as returned");
  assert.ok(!/mqtt-tls|mqtt-ca|mqtt-fp|MQTT_TLS\b|readProvisioning/.test(render[1]), "the desktop receipt must never read the form");

  // Browser: computed beside the sealed prov, handed to the done card with
  // the form's broker object nulled the way the Wi-Fi credentials already
  // are (it carries mqttPass and the CA PEM), and rendered as the hub line.
  assert.match(flashJs, /bakedBroker = core\.brokerTlsReceipt\(prov\);/, "the browser line comes from the sealed prov, not the form");
  const call = /setPhase\(phaseDone\(\{ \.\.\.opts,([\s\S]*?)\}\)\);/.exec(flashJs);
  assert.ok(call, "flash.js lost the phaseDone call");
  for (const kv of ["wifi: null", "mqtt: null", "provBroker: bakedBroker"]) {
    assert.ok(call[1].includes(kv), `the done card must be handed ${kv}`);
  }
  const done = /function phaseDone\(opts\) \{([\s\S]*?)\n\}/.exec(flashJs);
  assert.ok(done, "flash.js phaseDone moved");
  assert.match(done[1], /if \(opts\.provBroker\) \{/, "the browser done card lost its hub line");
  assert.ok(done[1].includes("` Hub baked in — ${opts.provBroker}`"), "the browser must render the hub line as computed");
  assert.ok(!/opts\.mqtt\b/.test(done[1]), "the done card must not reach into the form's broker object");
});

test("broker TLS: the catalog's broker_tls is the firmware's own build fact, and both flashers gate on it", () => {
  // The nightstand-c6 is built with -DCANARY_MQTT_PLAIN_ONLY and its
  // mqtt_mgr.cpp REFUSES a provisioned TLS mode at boot (never a plain
  // socket in its place). gen_flash.py derives broker_tls from the env's
  // build flags; this re-derives it from the ini text so a hand-edited
  // catalog, a flavor that gains or loses the flag, or a frontend that stops
  // gating on it all fail here by name.
  // Every env ini, line by line and keyed (asset_stem === env name for every
  // product today): the flag counts only as one bare, unquoted
  // -DCANARY_MQTT_PLAIN_ONLY or -D…=<digits> token in an env's OWN
  // build_flags — the firmware asks `#if defined(...)`, so =0 is plain-only
  // too. Named anywhere else — quoted as the same env spells its string
  // defines, split after -D, in build_unflags / build_src_flags, in a base
  // section this scan does not follow through `extends` — it FAILS here by
  // file:line rather than silently counting as "not set": gen_flash.py
  // env_defines refuses the same spellings, and a re-derivation with a blind
  // spot the generator lacks would pass a plain-only build off as TLS-capable.
  const INI_DIR = join(ROOT, "firmware/envs/platformio");
  const strict = /(?:^|\s)-DCANARY_MQTT_PLAIN_ONLY(?:=\d+)?(?=\s|$)/;
  const strictAll = new RegExp(strict.source, "g");
  const plainOnlyEnvs = new Set();
  for (const file of readdirSync(INI_DIR).filter((f) => f.endsWith(".ini")).sort()) {
    let cur = null, key = null;
    for (const [i, line] of read(join(INI_DIR, file)).split("\n").entries()) {
      const sec = /^\[(.+)\]/.exec(line);
      if (sec) { cur = sec[1]; key = null; continue; }
      if (!line.trim() || /^\s*[;#]/.test(line)) continue;
      const kv = /^([A-Za-z0-9_.:-]+)\s*=/.exec(line);
      if (kv) key = kv[1];
      // Named = any token ending in the macro name (no leading \b: the D of
      // -D sits flush against it, so a boundary there would miss -DCANARY_…
      // itself, quoted or not); CANARY_MQTT_PLAIN_ONLY_GUARD is not it.
      const named = /CANARY_MQTT_PLAIN_ONLY(?![A-Za-z0-9_])/;
      if (!named.test(line)) continue;
      assert.ok(cur && cur.startsWith("env:") && key === "build_flags" &&
        strict.test(line) && !named.test(line.replace(strictAll, " ")),
        `${file}:${i + 1} [${cur}] ${key}: CANARY_MQTT_PLAIN_ONLY is named in a place or spelling this ` +
        "re-derivation does not parse (one bare -DCANARY_MQTT_PLAIN_ONLY[=<digits>] token in an env's own " +
        "build_flags). Spell it that way, or teach this scan AND gen_flash.py env_defines the new form together.");
      plainOnlyEnvs.add(cur.slice("env:".length));
    }
  }
  assert.ok(plainOnlyEnvs.has("canary-display-nightstand-c6"),
    "the nightstand-c6 env no longer sets -DCANARY_MQTT_PLAIN_ONLY — regenerate flash.json and " +
    "update docs/FIRMWARE_VARIANT_AUDIT.md's row with it");
  const mgr = read(join(ROOT, "firmware/projects/canary-display/src/net/mqtt_mgr.cpp"));
  // The =<digits> reading above rests on the firmware asking defined-ness.
  assert.match(mgr, /#if defined\(CANARY_MQTT_PLAIN_ONLY\)/,
    "mqtt_mgr.cpp no longer asks `defined(CANARY_MQTT_PLAIN_ONLY)`: the =<digits> reading here and in gen_flash.py env_defines answers defined-ness");
  assert.ok(!/#\s*(?:el)?if\s+!?\s*CANARY_MQTT_PLAIN_ONLY\b/.test(mgr),
    "mqtt_mgr.cpp tests CANARY_MQTT_PLAIN_ONLY by value — =0 would then mean the opposite of what this scan and env_defines answer");
  assert.match(mgr, /Refusing to connect: NVS mqtt_tls=%u asks for TLS, but this flavor is/,
    "canary-display mqtt_mgr.cpp no longer refuses a TLS mode on a plain-only build");
  assert.match(mgr, /built plain-only \(OTA slot budget, CANARY_MQTT_PLAIN_ONLY\)/,
    "the firmware's own reason text moved — both flashers quote it");

  // The pinned-train gate (gen_flash.py CAPABILITY_TRAINS): the catalog
  // describes the fw-v<fw_train> images, and a build that honors TLS in the
  // working tree still offers it only once the pinned train carries it —
  // until then broker_tls is false and broker_tls_from names the train it
  // arrives with (a 2.4.15 image has no transport and would connect plain).
  const trainBefore = (a, b) => {
    const x = a.split(".").map(Number), y = b.split(".").map(Number);
    for (let i = 0; i < Math.max(x.length, y.length); i++) {
      const d = (x[i] || 0) - (y[i] || 0);
      if (d) return d < 0;
    }
    return false;
  };
  const froms = new Set();
  for (const p of catalog.products) {
    assert.strictEqual(typeof p.broker_tls, "boolean",
      `${p.id}: catalog has no broker_tls — regenerate with canary-local/tools/gen_flash.py`);
    if (p.broker_nvs !== true) {
      assert.strictEqual(p.broker_tls, false, `${p.id}: no broker in NVS, so no TLS mode to honor`);
      assert.strictEqual(p.broker_tls_from, undefined, `${p.id}: no broker in NVS, so no train to wait for`);
      continue;
    }
    // Envs are named by their asset stem (every product today); a flavor
    // that gains the flag under another naming shows up as a loud mismatch
    // here, which is the point.
    const builds = !plainOnlyEnvs.has(p.asset_stem);
    if (p.broker_tls_from !== undefined) {
      assert.ok(builds, `${p.id}: broker_tls_from names a train for a plain-only build, which never honors TLS`);
      assert.match(p.broker_tls_from, /^\d+(?:\.\d+)*$/, `${p.id}: broker_tls_from is not a train`);
      assert.ok(trainBefore(catalog.fw_train, p.broker_tls_from),
        `${p.id}: broker_tls_from ${p.broker_tls_from} is not after the pinned train ${catalog.fw_train} — ` +
        "the pinned images carry TLS, so it must be offered");
      froms.add(p.broker_tls_from);
    }
    const expect = builds && p.broker_tls_from === undefined;
    assert.strictEqual(p.broker_tls, expect,
      `${p.id}: catalog says broker_tls=${p.broker_tls} but its env ${builds ? "does not set" : "sets"} ` +
      `-DCANARY_MQTT_PLAIN_ONLY${p.broker_tls_from ? ` and the pinned train predates ${p.broker_tls_from}` : ""}. ` +
      "Regenerate with gen_flash.py — never hand-edit devices/flash.json.");
  }
  assert.ok(froms.size <= 1, `one capability, one first train — the catalog names ${[...froms].join(", ")}`);
  const c6 = catalog.products.find((p) => p.id === "securacv-canary-display-nightstand-c6");
  assert.ok(c6 && c6.broker_nvs === true && c6.broker_tls === false,
    "the nightstand-c6 reads a broker but honors no TLS mode — the case the flag exists for");

  // Both frontends gate the TLS modes on it, and both say why with the
  // firmware's own reason (never a hand-kept product-id list).
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  assert.match(flashJs, /const tlsOk = product\.broker_tls === true/, "browser must gate the TLS modes on broker_tls");
  assert.match(flashJs, /if \(v !== core\.MQTT_TLS_MODES\.plain && !tlsOk\) o\.disabled = true/,
    "browser must disable the TLS options where the build refuses them");
  assert.match(flashJs, /const mode = tlsOk \? Number\(tlsSel\.value\) \|\| 0 : core\.MQTT_TLS_MODES\.plain/,
    "browser values() must report plain where the build refuses TLS");
  assert.match(appJs, /const tlsOk = broker && product\.broker_tls === true/, "desktop must gate the TLS modes on broker_tls");
  assert.match(appJs, /o\.disabled = !tlsOk/, "desktop must disable the TLS options where the build refuses them");
  assert.match(appJs, /const tlsMode = broker && product\.broker_tls === true/,
    "desktop readProvisioning must report plain where the build refuses TLS");
  // The two reasons Plain can be all there is get two different notes — the
  // plain-only build's reason would be false for an image that predates TLS
  // (it would connect plain, not refuse) — in the same words on both.
  const trainNote = (src) => /const mqttTlsTrainNote = \(from\) =>\n([\s\S]*?);\n/.exec(src);
  assert.ok(trainNote(flashJs) && trainNote(appJs), "both flashers need the not-yet-released note (mqttTlsTrainNote)");
  assert.strictEqual(trainNote(appJs)[1], trainNote(flashJs)[1], "the not-yet-released note must read the same in both flashers");
  assert.match(flashJs, /product\.broker_tls_from \? mqttTlsTrainNote\(product\.broker_tls_from\) : MQTT_TLS_PLAIN_ONLY_NOTE/,
    "browser must pick the note by the catalog's reason");
  assert.match(appJs, /product && product\.broker_tls_from \? mqttTlsTrainNote\(product\.broker_tls_from\) : MQTT_TLS_PLAIN_ONLY_NOTE/,
    "desktop must pick the note by the catalog's reason");
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop app.js", appJs]]) {
    assert.ok(src.includes("built plain-only") && src.includes("CANARY_MQTT_PLAIN_ONLY"),
      `${label} must give the firmware's own reason for the disabled TLS modes`);
    // The id may be NAMED in a comment explaining the flag, never used in
    // code: every line that says it must be a comment line.
    for (const line of src.split("\n").filter((l) => l.includes("nightstand-c6"))) {
      assert.match(line, /^\s*\/\//,
        `${label}: gate on the catalog flag, never a hand-kept product id (${line.trim()})`);
    }
  }
});

test("broker TLS: where the pinned release is tagged in this clone, it carries the transport the catalog offers", (t) => {
  // The bug the pinned-train gate exists for, read from the release itself:
  // broker_tls was derived from the working tree while fw-v2.4.15 had no
  // TLS transport at all. A shallow CI checkout has no tags, so this skips
  // there (gen_flash.py's CAPABILITY_TRAINS row stands, and the catalog
  // drift gate pins what it produced); a developer clone checks the tag.
  const tag = `fw-v${catalog.fw_train}`;
  const tagged = spawnSync("git", ["-C", ROOT, "rev-parse", "-q", "--verify", `refs/tags/${tag}`]);
  if (tagged.error || tagged.status !== 0) {
    t.skip(`no ${tag} tag in this checkout`);
    return;
  }
  const evidence = "firmware/common/network/mqtt_transport.h";
  const present = spawnSync("git", ["-C", ROOT, "cat-file", "-e", `${tag}:${evidence}`]).status === 0;
  for (const p of catalog.products.filter((q) => q.broker_tls === true)) {
    assert.ok(present, `${p.id}: catalog offers broker TLS, but ${evidence} is absent at ${tag} — the pinned images have no TLS`);
  }
  for (const p of catalog.products.filter((q) => q.broker_tls_from !== undefined)) {
    assert.ok(!present, `${p.id}: catalog withholds broker TLS until ${p.broker_tls_from}, but ${tag} already carries ${evidence}`);
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

  // 1c. The inverse fallback on the blob-scheme side: the main canary and
  //     the wap read blobs first, so a string-typed seed (a string-scheme
  //     flasher frontend writing a blob-scheme board's keys) must be honored
  //     via getString when the key exists but getBytesLength reads 0.
  const blobLoaders = [
    "firmware/canary/lib/securacv_network/src/securacv_network.cpp",
    "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino",
  ];
  // (nvs\.| nvs->: the wap reaches its NvsManager through the RAII
  // NvsMainSession guard's operator->, the canary through a reference.)
  for (const path of blobLoaders) {
    const src = read(join(ROOT, path));
    assert.match(src, /nvs(?:\.|->)isKey\(NVS_KEY_WIFI_SSID\)/,
      `${path}: the credential loader no longer distinguishes a string-typed ` +
      "seed from an absent one — a string-seeded board would boot unprovisioned");
    assert.match(src, /nvs(?:\.|->)getString\(NVS_KEY_WIFI_SSID/,
      `${path}: the credential loader no longer falls back to a string-scheme ` +
      "seed — a string-seeded board would read empty credentials and boot unprovisioned");
  }

  // Both writers write the wifi_pass key unconditionally inside the wifi block.
  const flashCoreSrc = read(join(CANARY, "assets/flash-core.js"));
  assert.match(flashCoreSrc, /writeString\("wifi_pass", passB\)/,
    "flash-core.js no longer writes the (possibly empty) wifi_pass string key");
  const provRs = engineRs("provisioning");
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

// Run each app's wall controller against one stubbed native side and report
// what it asked for and what it told the user. The Lab's host is small enough
// to run whole; the Flasher's controller is lifted out of app.js with the two
// helpers it calls. Source-level regexes can't see ORDER or wording drift —
// running them can.
const WALL_SIGHTINGS = [
  { deviceId: "canary-wap-1", host: "canary-wap-1.local", ip: "192.168.1.50", port: 80 },
  { deviceId: "canary-display-2", host: "canary-display-2.local", ip: "192.168.1.51", port: 1 },
  { deviceId: "canary-sense-3", host: "canary-sense-3.local", ip: "fe80::1", port: 1 },
];
const WALL_BOARDS = ["http://192.168.1.50:80", "http://192.168.1.51", "http://canary-sense-3.local"];
// The Apple TV's well-known candidates (tvos WallModel.wellKnownCandidates),
// as bases — the test after the run test holds every monorepo wall to them.
const WALL_WELL_KNOWN = ["http://canary.local:8099", "http://canary.local:8799", "http://canary.local"];
const wallInvoke = (calls, sightings, caps, answer) => async (cmd, args) => {
  calls.push([cmd, args]);
  if (cmd === "native_capabilities") return caps;
  if (cmd === "fleet_scan") return sightings;
  if (cmd === "witness_discover") {
    const doc = answer && answer(Array.from(args.bases));
    if (doc) return doc;
    throw "no kernel answered on the LAN yet";
  }
  if (cmd === "companion_set_bases") return null;
  throw new Error(`the wall invoked an unexpected command: ${cmd}`);
};
const wallStatusEl = () => ({ textContent: "", classList: { toggle() {} } });
async function runLabWall(typedBase, sightings, answer) {
  const calls = [];
  const scan = wallStatusEl();
  const frame = { src: "", contentWindow: { postMessage() {} } };
  const ctx = {
    document: {
      getElementById: (id) => (id === "witness-frame" ? frame : id === "wall-scan" ? scan : null),
      addEventListener() {},
      hidden: false,
    },
    localStorage: { getItem: (k) => (k === "scv-kernel" ? typedBase : null) },
    location: { search: "" },
    URLSearchParams,
    setTimeout: () => 0,
    clearTimeout() {},
    addEventListener() {},
  };
  ctx.window = ctx;
  ctx.__TAURI__ = { core: { invoke: wallInvoke(calls, sightings, { mdns: true, notifications: false }, answer) } };
  const posted = [];
  frame.contentWindow.postMessage = (m) => posted.push(m);
  vm.runInNewContext(read(join(CANARY, "assets/witness-host.js")), ctx);
  for (let i = 0; i < 10 && !calls.some(([c]) => c === "witness_discover"); i++) {
    await new Promise((r) => setImmediate(r));
  }
  for (let i = 0; i < 10; i++) await new Promise((r) => setImmediate(r));
  return { calls, status: scan.textContent, posted };
}
async function runFlasherWall(typedHost, sightings, answer) {
  const src = read(join(ROOT, "desktop/src/app.js"));
  const grab = (re, what) => {
    const m = re.exec(src);
    assert.ok(m, `desktop/src/app.js lost ${what} — re-point the wall test at it`);
    return m[0];
  };
  const code = [
    grab(/\nfunction witnessBoardBases\([\s\S]*?\n\}\n/, "witnessBoardBases()"),
    grab(/\nfunction witnessBases\([\s\S]*?\n\}\n/, "witnessBases()"),
    grab(/\nconst witnessDiscovery = \{[\s\S]*?\n\};\n/, "the witnessDiscovery controller"),
  ].join("\n");
  const calls = [];
  const scan = wallStatusEl();
  const posted = [];
  const els = {
    "mqtt-host": { value: typedHost || "" },
    "fleet-scan-status": scan,
    "witness-frame": { contentWindow: { postMessage: (m) => posted.push(m) } },
  };
  const wall = new Function("$", "invoke", "witnessName", `${code}\nreturn witnessDiscovery;`)(
    (id) => els[id] || null, wallInvoke(calls, sightings, {}, answer), () => "New Canary");
  await wall.tick();
  return { calls, status: scan.textContent, posted };
}

test("witness wall: with no kernel, every answering board reaches the wall, once each", async () => {
  // Three nightstands, no hub: each answers /api/fleet with itself first and
  // the siblings it hears after. The wall used to stop at the first answer
  // and show ONE node. Self-reports win over a sibling's (stale) view.
  const nodes = [
    { deviceId: "a", host: "a.local", ip: "192.168.1.61", port: 80 },
    { deviceId: "b", host: "b.local", ip: "192.168.1.62", port: 80 },
    { deviceId: "c", host: "c.local", ip: "192.168.1.63", port: 80 },
  ];
  const row = (name, online, extra) => ({ name, online, product: "canary-nightstand", ...extra });
  const docs = {
    "http://192.168.1.61:80": { kernel: "A", devices: [row("A", 1, { hardware: "waveshare-esp32c6-lcd147" }), row("B", 0)] },
    "http://192.168.1.62:80": { kernel: "B", devices: [row("B", 1, { hardware: "waveshare-esp32c6-lcd147" }), row("A", 1)] },
    "http://192.168.1.63:80": { kernel: "C", devices: [row("C", 1)] },
  };
  const answer = (bases) => (bases.length === 1 ? docs[bases[0]] : null);
  for (const [name, run] of [["Lab witness-host.js", () => runLabWall(null, nodes, answer)],
                             ["Flasher app.js", () => runFlasherWall("", nodes, answer)]]) {
    const { status, posted } = await run();
    const fleet = posted.filter((m) => m && m.type === "witness:fleet").pop();
    assert.ok(fleet, `${name} posted no fleet to the wall`);
    const devs = Array.from(fleet.fleet.devices);
    assert.deepStrictEqual(devs.map((d) => d.name).sort(), ["A", "B", "C"],
      `${name}: every board must reach the wall, once each`);
    const b = devs.find((d) => d.name === "B");
    assert.strictEqual(b.online, 1, `${name}: B's own self-report must beat A's stale view of it`);
    assert.strictEqual(b.hardware, "waveshare-esp32c6-lcd147", `${name}: the self row carries the board`);
    assert.match(status, /3 Canaries on your network/, `${name} must count the merged fleet`);
  }
});

test("witness wall: both apps try the kernel before any browsed board, and say what they heard", async () => {
  // witness_discover returns the FIRST base whose /api/fleet answers, and a
  // WAP or display answers with a one-board self-report
  // (tvos/discovery/DISCOVERY.md). So a browsed board tried ahead of the
  // kernel would replace the kernel's whole fleet on the wall, every tick.
  // Both hosts: browse first, then the typed kernel, the well-known
  // canary.local:8099, canary.local:8799 and canary.local, and only then the
  // boards they heard.
  const hosts = [
    ["Lab witness-host.js", () => runLabWall("http://192.168.1.10:8099", WALL_SIGHTINGS),
      ["http://192.168.1.10:8099", ...WALL_WELL_KNOWN]],
    ["Flasher app.js", () => runFlasherWall("192.168.1.10", WALL_SIGHTINGS),
      ["http://192.168.1.10:8099", "http://192.168.1.10", ...WALL_WELL_KNOWN]],
  ];
  const statuses = [];
  for (const [name, run, kernel] of hosts) {
    const { calls, status } = await run();
    const order = calls.map(([c]) => c);
    const scanAt = order.indexOf("fleet_scan");
    const pollAt = order.indexOf("witness_discover");
    assert.ok(scanAt >= 0, `${name}'s wall no longer browses mDNS (fleet_scan) before it polls`);
    assert.ok(pollAt > scanAt, `${name}'s wall must browse (fleet_scan) BEFORE it polls witness_discover`);
    // (Array.from: the Lab's list is built in the vm's realm, with its own Array.prototype.)
    // The kernel is asked alone first; only when none answers is every
    // browsed board asked, each on its own, so all of them reach the wall.
    assert.deepStrictEqual(Array.from(calls[pollAt][1].bases), kernel,
      `${name}: the kernel addresses must be asked first, and alone`);
    const boardPolls = calls.slice(pollAt + 1).filter(([c]) => c === "witness_discover")
      .map(([, a]) => Array.from(a.bases));
    assert.deepStrictEqual(boardPolls, WALL_BOARDS.map((b) => [b]),
      `${name}: every browsed board must be asked on its own, in browse order, after the kernel`);
    assert.match(status, /^3 Canaries announced on this network — none serves the fleet document yet\.$/,
      `${name} must say the boards it heard, not "nothing answering yet"`);
    statuses.push(status);
  }
  assert.strictEqual(statuses[0], statuses[1], "the two walls word the heard-but-no-fleet status differently");

  // Nobody announcing: no board bases, and the honest "nothing answering".
  for (const [name, run] of [["Lab witness-host.js", () => runLabWall(null, [])],
                             ["Flasher app.js", () => runFlasherWall("", [])]]) {
    const { calls, status } = await run();
    const poll = calls.find(([c]) => c === "witness_discover");
    assert.deepStrictEqual(Array.from(poll[1].bases), WALL_WELL_KNOWN,
      `${name}: with no typed kernel and nothing heard, only the well-known addresses are tried`);
    assert.match(status, /nothing answering yet/, `${name} must keep the "nothing answering yet" status when nothing announced`);
  }
});

test("witness wall: every wall here probes the Apple TV's well-known addresses, in its order", () => {
  // The Wall (tvos WallModel.wellKnownCandidates) is the reference: the hub
  // convention port, the kernel's own API port (8799 — the Home Assistant
  // add-on and the Docker sidecar serve it), then the bare device; the first
  // address that serves a fleet wins. The desktop Flasher, the Lab's wall
  // host, the Lab's menu bar companion and the vendored web emulator must
  // try the same three in the same order — a list one surface forgot is a
  // kernel that surface never finds. (The website's tests/tv-wall.test.mjs
  // pins its TV app to the emulator's list, which this file pins here.)
  const swift = read(join(ROOT, "tvos/WitnessWall/Sources/WitnessWall/WallModel.swift"));
  const tv = /static let wellKnownCandidates = \[([^\]]*)\]/.exec(swift);
  assert.ok(tv, "tvos WallModel.swift lost `static let wellKnownCandidates` — re-point this test at it");
  const tvBases = [...tv[1].matchAll(/"([^"]+)"/g)].map((x) => `http://${x[1]}`);
  assert.deepStrictEqual(WALL_WELL_KNOWN, tvBases,
    "the monorepo walls' well-known list drifted from the Apple TV's wellKnownCandidates");

  // The Lab's menu bar companion polls these until the wall host names one.
  const companion = read(join(ROOT, "desktop-lab/src-tauri/src/companion.rs"));
  const defaults = /const DEFAULT_BASES: \[&str; \d+\] = \[([^\]]*)\];/.exec(companion);
  assert.ok(defaults, "companion.rs lost `const DEFAULT_BASES` — re-point this test at it");
  assert.deepStrictEqual([...defaults[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]), tvBases,
    "the Lab companion's DEFAULT_BASES must be the Apple TV's list, in its order");

  // The vendored web emulator (canonical in the website repo; both app copies
  // are byte-identical — the sync guard proves that).
  const emu = read(join(CANARY, "witness/tv-emulator.js"));
  const known = /const WELL_KNOWN = \[([^\]]*)\];/.exec(emu);
  assert.ok(known, "the vendored tv-emulator.js lost `const WELL_KNOWN` — re-point this test at it");
  assert.deepStrictEqual([...known[1].matchAll(/'([^']+)'/g)].map((x) => x[1]), tvBases,
    "the vendored emulator's WELL_KNOWN drifted from the Apple TV's — change the website's js/tv-emulator.js, then scripts/vendor_witness_emulator.sh");
});

test("mDNS browse: the Lab's fleet_scan is the Flasher's, in lockstep", () => {
  // The Lab ported the Flasher's browse of _securacv._tcp as a twin (same
  // command, DTO, constant, code) so a frontend written against either app
  // reads the other's answer unchanged. A retyped copy drifts quietly — the
  // failure would be a board one app lists and the other never hears.
  const flasherFleet = read(join(ROOT, "desktop/src-tauri/src/fleet.rs"));
  const labFleet = read(join(ROOT, "desktop-lab/src-tauri/src/fleet.rs"));
  const labRs = read(join(ROOT, "desktop-lab/src-tauri/src/lib.rs"));
  const norm = (t) => t.replace(/\s+/g, " ").trim();
  const item = (src, re, what) => {
    const m = re.exec(src);
    assert.ok(m, `couldn't find ${what}`);
    return norm(m[0]);
  };
  const pieces = [
    [/const SERVICE_TYPE: &str = "[^"]*";/, "SERVICE_TYPE"],
    [/pub struct FleetSighting \{[\s\S]*?\n\}/, "struct FleetSighting"],
    [/#\[derive\([^)]*\)\]\s*#\[serde\([^)]*\)\]\s*pub struct FleetSighting/, "FleetSighting's derive/serde attributes"],
    [/#\[tauri::command\]\s*pub async fn fleet_scan\(timeout_ms: Option<u64>\)[\s\S]*?\n\}/, "fn fleet_scan"],
    [/fn scan_blocking\(wait_ms: u64\)[\s\S]*?\n\}/, "fn scan_blocking"],
  ];
  for (const [re, what] of pieces) {
    assert.strictEqual(item(labFleet, re, `${what} in desktop-lab fleet.rs`),
      item(flasherFleet, re, `${what} in desktop fleet.rs`),
      `desktop-lab fleet.rs ${what} drifted from the Flasher's — copy it back verbatim`);
  }
  assert.match(labFleet, /const SERVICE_TYPE: &str = "_securacv\._tcp\.local\.";/,
    "the Lab must browse the service every board advertises");
  // Only the browse is ported: the Flasher's device calls carry bearer tokens
  // from its secret drawer, and the Lab has no drawer to keep them in.
  const labFleetCode = labFleet.replace(/\/\/.*$/gm, "");
  assert.doesNotMatch(labFleetCode, /fn\s+(?:fleet_device_call|device_whoami)\b|bearer_auth|Authorization|token/i,
    "desktop-lab fleet.rs must not grow the Flasher's token-bearing device calls");
  // Desktop-only, and honestly advertised: the module and the command exist
  // only where the capability says so.
  assert.match(labRs, /#\[cfg\(desktop\)\]\s*mod fleet;/, "desktop-lab lib.rs must gate mod fleet on desktop");
  assert.match(labRs, /"mdns":\s*cfg!\(desktop\)/, "desktop-lab native_capabilities must report mdns as cfg!(desktop)");
  const handlers = [...labRs.matchAll(/invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/g)].map((m) => m[1]);
  assert.strictEqual(handlers.length, 2, "expected a desktop and a non-desktop invoke_handler in desktop-lab lib.rs");
  assert.ok(handlers[0].includes("fleet::fleet_scan"), "the desktop handler must register fleet::fleet_scan");
  assert.ok(!handlers[1].includes("fleet_scan"), "the mobile handler must not register fleet_scan (no mdns there)");
  // The frontend asks before it browses, then feeds the boards to the poll.
  const labHost = read(join(CANARY, "assets/witness-host.js"));
  assert.match(labHost, /invoke\("fleet_scan"/, "Lab witness-host.js no longer browses mDNS (fleet_scan)");
  assert.match(labHost, /invoke\("native_capabilities"\)/, "Lab witness-host.js must ask native_capabilities before it browses");
  assert.match(labHost, /if \(mdns\) \{[\s\S]{0,120}invoke\("fleet_scan"/,
    "Lab witness-host.js must gate the browse on the mdns capability");
  // One crate version on both sides of the twin.
  const mdnsVer = (lock) => (/name = "mdns-sd"\nversion = "([^"]+)"/.exec(read(join(ROOT, lock))) || [])[1];
  assert.ok(mdnsVer("desktop/src-tauri/Cargo.lock"), "the Flasher's lock lost mdns-sd");
  assert.strictEqual(mdnsVer("desktop-lab/src-tauri/Cargo.lock"), mdnsVer("desktop/src-tauri/Cargo.lock"),
    "the two apps lock different mdns-sd versions — the twin is no longer the same browse");
});

// ── Native flashing: the Lab runs the Flasher's commands on the same engine ──
//
// A14: the Lab's native flash path is NOT a port of the Flasher's flash code —
// both apps call desktop/flash-engine, and each keeps only a thin Tauri
// wrapper per command. The wrappers are the one place the two can still drift
// (a renamed argument silently becomes `undefined` in one app's invoke), so
// they are held equal by text here, and the DTOs and event names are held to
// the engine.
const FLASH_COMMANDS = [
  // [command, the Flasher file that defines it]
  ["list_ports", "desktop/src-tauri/src/lib.rs"],
  ["detect_chip", "desktop/src-tauri/src/lib.rs"],
  ["fetch_manifest", "desktop/src-tauri/src/lib.rs"],
  ["flash", "desktop/src-tauri/src/lib.rs"],
  ["start_serial_monitor", "desktop/src-tauri/src/serial_monitor.rs"],
  // The Lab's post-flash monitor has the Flasher's command row (sweep A67:
  // the product's own monitor chips — flash.json products[].monitor, sweep
  // A86 — and a typed command), so the command it sends
  // through is back, held to the Flasher's text like the rest;
  // lab_bundle.test.js holds the Lab's registration to that caller.
  ["serial_monitor_send", "desktop/src-tauri/src/serial_monitor.rs"],
  ["stop_serial_monitor", "desktop/src-tauri/src/serial_monitor.rs"],
];

// One Rust fn — its attributes, signature and balanced body — whitespace
// folded and `pub` dropped (the Lab's live in a module, the Flasher's at the
// crate root). Comments stay: the argument comments are part of the contract.
const rustFnText = (src, name, where) => {
  const m = new RegExp(`\\n((?:#\\[[^\\n]*\\]\\s*\\n)*)(?:pub\\s+)?(?:async\\s+)?fn\\s+${name}\\s*\\(`).exec(src);
  assert.ok(m, `couldn't find fn ${name} in ${where}`);
  let depth = 0;
  let i = src.indexOf("{", m.index + m[0].length);
  for (; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}" && --depth === 0) break;
  }
  return src.slice(m.index + 1, i + 1).replace(/\bpub\s+/g, "").replace(/\s+/g, " ").trim();
};

test("native flashing: the Lab's flash commands are the Flasher's, on the same engine", () => {
  const labFlash = read(join(ROOT, "desktop-lab/src-tauri/src/flash.rs"));
  const labRs = read(join(ROOT, "desktop-lab/src-tauri/src/lib.rs"));
  const flasherHost = read(join(ROOT, "desktop/src-tauri/src/host.rs"));

  // 1. Same command names, same arguments, same body — by text.
  for (const [cmd, file] of FLASH_COMMANDS) {
    const flasher = rustFnText(read(join(ROOT, file)), cmd, file);
    const lab = rustFnText(labFlash, cmd, "desktop-lab/src-tauri/src/flash.rs");
    assert.strictEqual(lab, flasher,
      `the Lab's ${cmd} drifted from the Flasher's (${file}) — the two apps' wrappers ` +
      "must stay identical so either frontend's invoke works against either app");
    assert.match(flasher, /#\[tauri::command\]/, `${cmd} is not a tauri command in ${file}`);
  }
  // Every delegating wrapper calls the engine, never a local copy of the logic.
  for (const [cmd, engineFn] of [["detect_chip", "flash_engine::flash::detect_chip"],
    ["fetch_manifest", "flash_engine::flash::fetch_manifest"], ["flash", "flash_engine::flash::flash"],
    ["list_ports", "flash_engine::ports::list_ports"]]) {
    assert.ok(rustFnText(labFlash, cmd, "desktop-lab flash.rs").includes(`${engineFn}(`),
      `the Lab's ${cmd} must delegate to ${engineFn}`);
  }

  // 2. Registered where they can work, and only there: the Lab's desktop
  //    handler, never the mobile one (no USB serial on iOS/iPadOS — MOBILE.md).
  const handlers = [...labRs.matchAll(/invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/g)].map((m) => m[1]);
  assert.strictEqual(handlers.length, 2, "expected a desktop and a non-desktop invoke_handler in desktop-lab lib.rs");
  const flasherHandler = /invoke_handler\(tauri::generate_handler!\[([\s\S]*?)\]\)/.exec(libRs)[1];
  for (const [cmd] of FLASH_COMMANDS) {
    assert.match(handlers[0], new RegExp(`\\bflash::${cmd}\\b`), `the Lab's desktop handler must register flash::${cmd}`);
    assert.doesNotMatch(handlers[1], new RegExp(`\\b${cmd}\\b`), `the Lab's mobile handler must not register ${cmd}`);
    assert.match(flasherHandler, new RegExp(`\\b${cmd}\\b`), `the Flasher no longer registers ${cmd}`);
  }
  assert.match(labRs, /#\[cfg\(desktop\)\]\s*mod flash;/, "desktop-lab lib.rs must gate mod flash on desktop");
  assert.match(labRs, /\.manage\(flash_engine::monitor::SerialMonitorState::default\(\)\)/,
    "the Lab must manage the engine's SerialMonitorState, or every serial command panics");
  assert.match(labRs, /\.plugin\(tauri_plugin_shell::init\(\)\)/,
    "the Lab must init the shell plugin — Rust-side shell().sidecar() needs its state");

  // 3. The DTOs are the engine's, in both apps — never a retyped twin.
  for (const [label, src] of [["desktop lib.rs", libRs], ["desktop-lab lib.rs", labRs], ["desktop-lab flash.rs", labFlash]]) {
    assert.doesNotMatch(src, /struct\s+(?:FlashReceipt|ChipInfo|PortDto|FlashRequest)\b/,
      `${label} defines its own flash DTO — use flash_engine's, or the two apps' answers drift`);
  }

  // 4. One host shape per app: the same name, Tauri's emitter, the bundled
  //    espflash by its runtime name, each app's User-Agent, and a spawn that
  //    never leaves espflash behind — the Flasher's launch guard, the Lab's
  //    Sidecars registry killed on exit (the launch guard is not ported).
  for (const [label, src, ua] of [["desktop host.rs", flasherHost, "SecuraCV-Flasher"],
    ["desktop-lab flash.rs", labFlash, "SecuraCV-Lab"]]) {
    assert.match(src, /impl FlashHost for TauriHost \{/, `${label} must implement the engine's FlashHost as TauriHost`);
    assert.match(src, /\.sidecar\(ESPFLASH\)/, `${label} must spawn the bundled espflash by its runtime name`);
    assert.match(src, new RegExp(`const USER_AGENT: &'static str = "${ua}";`), `${label} lost its User-Agent`);
  }
  assert.match(flasherHost, /launch_guard::spawn_tracked\(/, "the Flasher's espflash spawn must stay launch-guard tracked");
  assert.match(labFlash, /running\.insert\(pid, child\)/, "the Lab must record each running espflash");
  assert.match(labRs, /RunEvent::Exit = &_event \{\s*flash::Sidecars::kill_all\(_app\);/,
    "a quitting Lab must kill the espflash it is still running");

  // 5. The events the engine speaks are the events the Flasher's frontend
  //    listens for (the Lab's flash page joins this list when it gains the
  //    native path).
  const events = new Set([...engineAllRs.matchAll(/\.emit\(\s*"((?:flash|serial):[a-z]+)"/g)].map((m) => m[1]));
  for (const e of ["flash:log", "flash:progress", "flash:changemap", "serial:log", "serial:status", "serial:receipt"]) {
    assert.ok(events.has(e), `the flash engine no longer emits ${e}`);
  }
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  for (const e of events) {
    if (e === "serial:vision") continue; // a Vision-module detail, read from the receipt instead
    assert.ok(appJs.includes(`"${e}"`), `the engine emits ${e} but the Flasher's frontend never listens for it`);
  }

  // 6. Both apps embed the one catalog for the Rust-side guards, and both
  //    depend on the engine by path (the Lab's only on desktop).
  const labBuild = read(join(ROOT, "desktop-lab/src-tauri/build.rs"));
  assert.match(labBuild, /canary-local\/devices\/flash\.json/, "the Lab's build.rs must embed the canonical flash.json");
  assert.match(labFlash, /include_str!\(concat!\(env!\("OUT_DIR"\), "\/flash\.json"\)\)/,
    "the Lab's chip guard must read the catalog build.rs embedded");
  assert.match(read(join(ROOT, "desktop/src-tauri/Cargo.toml")), /^flash-engine = \{ path = "\.\.\/flash-engine" \}/m,
    "the Flasher must depend on desktop/flash-engine");
  const labToml = read(join(ROOT, "desktop-lab/src-tauri/Cargo.toml"));
  const desktopBlock = labToml.split("[target.'cfg(not(any(target_os = \"android\", target_os = \"ios\")))'.dependencies]")[1] || "";
  assert.match(desktopBlock.split(/^\[/m)[0], /^flash-engine = \{ path = "\.\.\/\.\.\/desktop\/flash-engine" \}/m,
    "the Lab must depend on desktop/flash-engine in its desktop-only block");

  // 7. No shell grant for either webview: every sidecar spawn is Rust-side.
  for (const dir of ["desktop/src-tauri/capabilities", "desktop-lab/src-tauri/capabilities"]) {
    for (const f of readdirSync(join(ROOT, dir)).filter((n) => n.endsWith(".json"))) {
      const perms = JSON.parse(read(join(ROOT, dir, f))).permissions || [];
      for (const p of perms) {
        const id = typeof p === "string" ? p : p.identifier;
        assert.doesNotMatch(String(id), /^shell:/, `${dir}/${f} grants ${id} — espflash is spawned from Rust, never the webview`);
      }
    }
  }
});

test("native flashing: the Lab bundles the Flasher's espflash, pinned and packaged the same way", () => {
  // A sidecar is a pin set, a Linux access rule and a build-checked config
  // key (RELEASE_LESSONS 2026-09-23 (b)); a second app bundling it copies all
  // three, so all three are held to the Flasher's here.
  const flasherWf = read(join(ROOT, ".github/workflows/desktop-flasher-release.yml"));
  const labWf = read(join(ROOT, ".github/workflows/desktop-release.yml"));

  const step = (wf, name, label) => {
    const i = wf.indexOf(`      - name: ${name}\n`);
    assert.ok(i >= 0, `${label} has no "${name}" step`);
    const rest = wf.slice(i + 1);
    const end = rest.search(/\n      - name: |\n      # ──/);
    return (end >= 0 ? rest.slice(0, end) : rest).trimEnd();
  };

  // 1. One flash engine, pinned in ONE file both workflows read
  //    (.github/espflash-pins.env, A21) — never a second copy in either
  //    workflow, where it could drift and where no release-targets.yml watch
  //    would see a bump.
  const PINS = ".github/espflash-pins.env";
  const pinKeys = ["ESPFLASH_VERSION", "ESPFLASH_SHA256_AARCH64_APPLE_DARWIN",
    "ESPFLASH_SHA256_X86_64_APPLE_DARWIN", "ESPFLASH_SHA256_X86_64_UNKNOWN_LINUX_GNU"];
  const pinLines = read(join(ROOT, PINS)).split("\n").filter((l) => l && !l.startsWith("#"));
  const pins = Object.fromEntries(pinLines.map((l) => [l.slice(0, l.indexOf("=")), l.slice(l.indexOf("=") + 1)]));
  assert.deepStrictEqual(Object.keys(pins).sort(), [...pinKeys].sort(),
    `${PINS} must pin exactly the espflash version and the three per-target sha256s, once each`);
  assert.strictEqual(pinLines.length, pinKeys.length, `${PINS} pins a key twice`);
  assert.match(pins.ESPFLASH_VERSION, /^\d+\.\d+\.\d+$/, "ESPFLASH_VERSION must be a plain x.y.z version");
  for (const key of pinKeys.slice(1)) {
    assert.match(pins[key], /^[0-9a-f]{64}$/, `${key} must be a full sha256 digest`);
  }
  for (const [label, wf] of [["desktop-release.yml", labWf], ["desktop-flasher-release.yml", flasherWf]]) {
    assert.doesNotMatch(wf, /^\s+ESPFLASH_(?:VERSION|SHA256_\w+):/m,
      `${label} pins espflash itself again — the one copy lives in ${PINS}`);
  }
  // Both read it through the same step, before either bundle step runs.
  const loadName = "Load the espflash pins";
  const load = step(labWf, loadName, "desktop-release.yml");
  assert.strictEqual(load, step(flasherWf, loadName, "desktop-flasher-release.yml"),
    `the Lab's "${loadName}" step drifted from the Flasher's — keep them identical`);
  assert.ok(load.includes(`pins=${PINS}\n`), `"${loadName}" must read ${PINS}`);
  for (const [label, wf] of [["desktop-release.yml", labWf], ["desktop-flasher-release.yml", flasherWf]]) {
    const at = wf.indexOf(`- name: ${loadName}\n`);
    for (const name of ["Bundle espflash sidecar (macOS universal)", "Bundle espflash sidecar (Linux x86_64)"]) {
      assert.ok(at >= 0 && at < wf.indexOf(`- name: ${name}\n`), `${label} must load the pins before "${name}"`);
    }
  }
  // Run the step exactly as the release does — on the real file, and on the
  // shapes it must refuse — so a parse that only ever runs on a release
  // runner is exercised on every PR that touches it.
  const runBody = /\n        run: \|\n([\s\S]*)$/.exec(load);
  assert.ok(runBody, `couldn't find the run: block of "${loadName}"`);
  const script = runBody[1].split("\n").map((l) => l.replace(/^ {10}/, "")).join("\n");
  const runLoad = (pinsText) => {
    const dir = mkdtempSync(join(tmpdir(), "espflash-pins-"));
    try {
      mkdirSync(join(dir, ".github"));
      writeFileSync(join(dir, PINS), pinsText);
      const envFile = join(dir, "github_env");
      writeFileSync(envFile, "");
      const r = spawnSync("bash", ["-c", script], { cwd: dir, env: { PATH: process.env.PATH, GITHUB_ENV: envFile }, encoding: "utf8" });
      return { status: r.status, env: read(envFile), out: (r.stdout || "") + (r.stderr || "") };
    } finally { rmSync(dir, { recursive: true, force: true }); }
  };
  const real = read(join(ROOT, PINS));
  const ok = runLoad(real);
  assert.strictEqual(ok.status, 0, `"${loadName}" refused the real ${PINS}: ${ok.out}`);
  assert.deepStrictEqual(ok.env.split("\n").filter(Boolean).sort(),
    pinKeys.map((k) => `${k}=${pins[k]}`).sort(), `"${loadName}" must export exactly the four pins`);
  const sha = pins.ESPFLASH_SHA256_X86_64_UNKNOWN_LINUX_GNU;
  for (const [why, text] of [
    ["a missing pin", real.replace(/^ESPFLASH_SHA256_X86_64_APPLE_DARWIN=.*\n/m, "")],
    ["a pin given twice", real + `ESPFLASH_SHA256_X86_64_UNKNOWN_LINUX_GNU=${sha}\n`],
    ["a short digest", real.replace(sha, sha.slice(1))],
    ["an uppercase digest", real.replace(sha, sha.toUpperCase())],
    ["a quoted version", real.replace(/^ESPFLASH_VERSION=(.*)$/m, 'ESPFLASH_VERSION="$1"')],
    ["a stranger key", real + "ESPFLASH_EXTRA=1\n"],
    ["shell in the file", real + "$(touch pwned)\n"],
  ]) {
    const bad = runLoad(text);
    assert.notStrictEqual(bad.status, 0, `"${loadName}" accepted ${why}`);
    assert.match(bad.out, /::error file=\.github\/espflash-pins\.env::/, `"${loadName}" must name ${PINS} when it refuses ${why}`);
  }
  // Every sha256 the bundle steps look up is one the file pins: the macOS
  // step derives ESPFLASH_SHA256_<TRIPLE> from each triple it fetches, the
  // Linux step names its variable outright.
  const macStep = step(flasherWf, "Bundle espflash sidecar (macOS universal)", "desktop-flasher-release.yml");
  const fetched = [...macStep.matchAll(/^\s+fetch ([a-z0-9_-]+)$/gm)].map((m) => m[1]);
  assert.deepStrictEqual(fetched, ["aarch64-apple-darwin", "x86_64-apple-darwin"], "the macOS step fetches a different set of slices");
  for (const triple of fetched) {
    const key = `ESPFLASH_SHA256_${triple.toUpperCase().replace(/-/g, "_")}`;
    assert.ok(key in pins, `the macOS step verifies espflash-${triple} against ${key}, which ${PINS} doesn't pin`);
  }
  const linuxVars = [...step(flasherWf, "Bundle espflash sidecar (Linux x86_64)", "desktop-flasher-release.yml")
    .matchAll(/\$\{(ESPFLASH_\w+)\}/g)].map((m) => m[1]);
  assert.ok(linuxVars.includes("ESPFLASH_SHA256_X86_64_UNKNOWN_LINUX_GNU"), "the Linux step lost its sha256 check variable");
  for (const v of linuxVars) assert.ok(v in pins, `the Linux step reads ${v}, which ${PINS} doesn't pin`);

  // 2. The same bundling steps, sidecar directory aside — sha check, lipo and
  //    the per-arch/universal architecture proof included.
  for (const name of ["Bundle espflash sidecar (macOS universal)", "Bundle espflash sidecar (Linux x86_64)"]) {
    const lab = step(labWf, name, "desktop-release.yml");
    const flasher = step(flasherWf, name, "desktop-flasher-release.yml");
    assert.match(lab, /desktop-lab\/src-tauri\/binaries/, `the Lab's "${name}" must fill the Lab's binaries/`);
    assert.strictEqual(lab.replaceAll("desktop-lab/src-tauri", "desktop/src-tauri"), flasher,
      `the Lab's "${name}" step drifted from the Flasher's — keep them identical but for the sidecar directory`);
    assert.match(flasher, /sha256sum -c -/, `"${name}" must verify the download against its pin`);
  }
  assert.match(step(labWf, "Bundle espflash sidecar (macOS universal)", "desktop-release.yml"),
    /lipo -archs "\$bin\/espflash-universal-apple-darwin"/,
    "the universal espflash must be PROVEN to carry both slices (RELEASE_LESSONS (z))");
  // The steps run before the bundle is built.
  assert.ok(labWf.indexOf("Bundle espflash sidecar (Linux x86_64)") < labWf.indexOf("- name: Build & publish (unsigned)"),
    "the Lab must bundle espflash before tauri-action builds the app");

  // 3. Where Tauri looks: externalBin on exactly the platforms the release
  //    bundles (macOS + Linux), never in the base config — tauri-build
  //    enforces externalBin for every target, and no iOS espflash exists.
  const labConf = JSON.parse(read(join(ROOT, "desktop-lab/src-tauri/tauri.conf.json")));
  assert.strictEqual(labConf.bundle.externalBin, undefined,
    "the Lab's base tauri.conf.json must not name externalBin — the iPad shell's build would demand an iOS espflash");
  for (const plat of ["macos", "linux"]) {
    const conf = JSON.parse(read(join(ROOT, `desktop-lab/src-tauri/tauri.${plat}.conf.json`)));
    assert.deepStrictEqual(conf.bundle.externalBin, ["binaries/espflash"],
      `desktop-lab tauri.${plat}.conf.json must bundle binaries/espflash`);
  }
  const flasherConf = JSON.parse(read(join(ROOT, "desktop/src-tauri/tauri.conf.json")));
  assert.ok((flasherConf.bundle.externalBin || []).includes("binaries/espflash"), "the Flasher lost its espflash externalBin");
  // The sidecar the PR check stubs is the one the Linux config names.
  const labCheck = read(join(ROOT, ".github/workflows/desktop-lab-check.yml"));
  assert.match(labCheck, /want="binaries\/espflash-\$\(rustc -vV/,
    "desktop-lab-check.yml must stub binaries/espflash-<host triple>, or tauri-build refuses to compile the crate");

  // 4. One udev rule, byte-equal, installed by each .deb under its OWN path:
  //    dpkg refuses a package that owns a path another installed package owns.
  const rules = (p) => readFileSync(join(ROOT, p));
  assert.ok(rules("desktop-lab/src-tauri/packaging/canary-serial.rules")
    .equals(rules("desktop/src-tauri/packaging/canary-serial.rules")),
  "the Lab's canary-serial.rules drifted from the Flasher's — copy it back byte for byte");
  const debFiles = (conf) => (((conf.bundle || {}).linux || {}).deb || {}).files || {};
  const labDest = Object.entries(debFiles(labConf)).find(([, src]) => src === "packaging/canary-serial.rules");
  const flasherDest = Object.entries(debFiles(flasherConf)).find(([, src]) => src === "packaging/canary-serial.rules");
  assert.ok(labDest && flasherDest, "both .debs must install packaging/canary-serial.rules");
  assert.match(labDest[0], /^\/usr\/lib\/udev\/rules\.d\/6[0-9]-[a-z-]+\.rules$/, "the Lab's rule must land in udev's rules.d, before ModemManager's 77-mm-*");
  assert.notStrictEqual(labDest[0], flasherDest[0],
    "the two .debs install the rule at the same path — dpkg would refuse the second app");
  // …and the heredoc each INSTALL.md hands an AppImage user to paste is the
  // same rule (RELEASE_LESSONS principle 13: a hand copy nothing ties to the
  // file is a copy that drifts).
  const ruleLines = rules("desktop/src-tauri/packaging/canary-serial.rules").toString("utf8")
    .split("\n").map((l) => l.trim()).filter((l) => l && !l.startsWith("#"));
  assert.ok(ruleLines.length >= 2, "couldn't read the rule lines out of canary-serial.rules");
  for (const doc of ["desktop/INSTALL.md", "desktop-lab/INSTALL.md"]) {
    const m = /sudo tee \/etc\/udev\/rules\.d\/61-securacv-canary\.rules >\/dev\/null <<'EOF'\n([\s\S]*?)\n\s*EOF\n/
      .exec(read(join(ROOT, doc)));
    assert.ok(m, `${doc} lost its udev-rule heredoc`);
    assert.deepStrictEqual(m[1].split("\n").map((l) => l.trim()).filter(Boolean), ruleLines,
      `${doc}'s udev-rule heredoc drifted from packaging/canary-serial.rules — paste the rule lines back`);
  }
  // The Linux step proves what it bundled is an x86-64 ELF, and a source
  // build (which no sha256 pin covers) is never silent — both workflows, as
  // the step-equality check above keeps them.
  const linuxStep = step(labWf, "Bundle espflash sidecar (Linux x86_64)", "desktop-release.yml");
  assert.match(linuxStep, /\*"ELF 64-bit"\*"x86-64"\*\) ;;/, "the Linux espflash must be proven an x86-64 ELF");
  for (const name of ["Bundle espflash sidecar (macOS universal)", "Bundle espflash sidecar (Linux x86_64)"]) {
    assert.match(step(labWf, name, "desktop-release.yml"), /echo "::warning::prebuilt espflash-\$triple not downloadable;[^\n]*\n\s*cargo install espflash/,
      `"${name}"'s cargo-install fallback must announce itself with a ::warning::`);
  }
});

// The text of a top-level JS function (declaration through its balanced
// body) — the grabFn idiom above, for any source.
const jsFnText = (src, name, where) => {
  const i = src.indexOf("function " + name + "(");
  assert.ok(i >= 0, `couldn't find function ${name} in ${where}`);
  const b = src.indexOf("{", src.indexOf(")", i));
  let d = 0;
  for (let k = b; k < src.length; k++) {
    if (src[k] === "{") d++;
    else if (src[k] === "}" && --d === 0) return src.slice(i, k + 1);
  }
  assert.fail(`unbalanced function ${name} in ${where}`);
};

// Does a frontend's BAUD_RETRY_KINDS name `engine`?
const BAUD_RETRY_SET_HAS_ENGINE = (src) =>
  /const BAUD_RETRY_KINDS = new Set\(\[[^\]]*"engine"/.test(src);

test("native flashing: the Lab's flash page gives the Flasher's diagnostics, and serial lights only where it works", () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const labRs = read(join(ROOT, "desktop-lab/src-tauri/src/lib.rs"));

  // 1. The espflash-aware classifier is the Flasher's, verbatim (the third
  //    source beside app.js and flash-core.js) — and it names espflash's
  //    real failures the way the Flasher does.
  const nativeClassifier = jsFnText(nativeJs, "classifyFlashError", "flash-native.js");
  assert.strictEqual(nativeClassifier, jsFnText(appJs, "classifyFlashError", "desktop/src/app.js"),
    "flash-native.js classifyFlashError drifted from the Flasher's (desktop/src/app.js) — copy it back verbatim");
  const classify = new Function(nativeClassifier + "\nreturn classifyFlashError;")();
  const generic = "espflash exited with code 1. The board can't be bricked — " +
    "put it back in download mode and try again.\n";
  for (const [tail, kind] of [
    ["Error: Failed to open serial port\nCaused by: Device or resource busy", "port-busy"],
    ["Error: Permission denied (os error 13)", "permission"],
    ["Error: Failed to connect to the device\nCaused by: No serial data received", "not-in-download"],
    ["Error: Serial port disconnected\nCaused by: device not configured", "device-lost"],
    ["", "unknown"],
  ]) {
    assert.strictEqual(classify(new Error(generic + tail)).kind, kind,
      `flash-native.js misclassifies espflash's tail: ${tail.slice(0, 40) || "(none)"}`);
  }
  // A bundled espflash that cannot start (the wrong CPU, a missing loader, a
  // file without its execute bit) is its own kind in both frontends — never
  // `unknown` (which coaches download mode) and never the port's `permission`.
  // The errors are built from the Rust that produces them, so a reworded
  // spawn_error or host message fails here instead of silently falling back.
  const archHint = /pub const ARCH_MISMATCH_HINT: &str =\s*"([\s\S]*?)";/
    .exec(read(join(ROOT, "desktop/hub-core/src/hub_sidecar.rs")));
  assert.ok(archHint, "couldn't parse ARCH_MISMATCH_HINT from hub-core hub_sidecar.rs");
  const archText = archHint[1].replace(/\\\n\s*/g, "").replace(/\\"/g, '"');
  const sidecarRs = engineRs("sidecar");
  const withHint = /format!\("could not start \{name\}: \{hint\} \(\{raw\}\)"\)/;
  const bare = /format!\("could not start \{name\}: \{raw\}"\)/;
  assert.match(sidecarRs, withHint, "spawn_error's hinted wording moved — the frontends match its prefix");
  assert.match(sidecarRs, bare, "spawn_error's bare wording moved — the frontends match its prefix");
  const flasherHostRs = read(join(ROOT, "desktop/src-tauri/src/host.rs"));
  const labFlashRs = read(join(ROOT, "desktop-lab/src-tauri/src/flash.rs"));
  for (const [label, src] of [["desktop host.rs", flasherHostRs], ["desktop-lab flash.rs", labFlashRs]]) {
    assert.match(src, /\.map_err\(\|e\| format!\("bundled espflash missing: \{e\}"\)\)/,
      `${label}'s unresolvable-sidecar wording moved — the frontends match its prefix`);
  }
  assert.strictEqual(jsFnText(nativeJs, "spawnReason", "flash-native.js"),
    jsFnText(appJs, "spawnReason", "desktop/src/app.js"),
    "flash-native.js spawnReason drifted from the Flasher's — copy it back verbatim");
  const reasonOf = new Function(jsFnText(appJs, "spawnReason", "desktop/src/app.js") + "\nreturn spawnReason;")();
  const classifyFlasher = new Function(jsFnText(appJs, "classifyFlashError", "desktop/src/app.js") +
    "\nreturn classifyFlashError;")();
  for (const [err, reason] of [
    // macOS given a slice-less binary, and Linux's twin: spawn_error's hint.
    [`could not start espflash: ${archText} (Bad CPU type in executable (os error 86))`,
      "Bad CPU type in executable (os error 86)"],
    [`could not start espflash: ${archText} (Exec format error (os error 8))`, "Exec format error (os error 8)"],
    // A dynamically linked espflash whose loader is missing reports ENOENT…
    ["could not start espflash: No such file or directory (os error 2)", "No such file or directory (os error 2)"],
    // …and one without its execute bit reports EACCES — port-shaped words.
    ["could not start espflash: Permission denied (os error 13)", "Permission denied (os error 13)"],
    ["bundled espflash missing: current executable path has no parent", "current executable path has no parent"],
  ]) {
    for (const [where, fn] of [["flash-native.js", classify], ["desktop/src/app.js", classifyFlasher]]) {
      const c = fn(err);
      assert.strictEqual(c.kind, "engine", `${where} reads a sidecar that never started as ${c.kind}: ${err.slice(0, 60)}`);
      assert.doesNotMatch(c.hint, /hold BOOT/, `${where} coaches download mode for a sidecar that never started`);
    }
    assert.strictEqual(reasonOf(err), reason, `spawnReason lost the system's reason in: ${err.slice(0, 60)}`);
  }
  // …while espflash that RAN and was refused the port keeps the port's kind.
  assert.strictEqual(classify(new Error(generic + "Error: Permission denied (os error 13)")).kind, "permission");
  assert.ok(!BAUD_RETRY_SET_HAS_ENGINE(nativeJs) && !BAUD_RETRY_SET_HAS_ENGINE(appJs),
    "a sidecar that cannot start fails identically at every speed — never retry `engine`");
  // Both identify() calls say it with the system's reason and no driver note
  // (the port was never opened, so a USB-bridge driver can't be the cause).
  for (const [where, src] of [["desktop/src/app.js", appJs], ["flash-native.js", nativeJs]]) {
    assert.ok(src.includes('const engine = c.kind === "engine";'), `${where} identify() lost the engine kind`);
    assert.ok(src.includes('const reason = engine ? ` (The system said: ${spawnReason(firstLine)})` : "";'),
      `${where} identify() must give the system's reason for a sidecar that never started`);
    assert.ok(src.includes("!osLevel && !engine && bridge"), `${where} identify() must not blame a USB driver when the engine never started`);
  }

  // withoutLocalFile() rewrites ONE sentence of that verbatim classifier (the
  // "local .bin under Advanced" install lives in the Flasher, not here). If
  // app.js rewords it, the replace silently does nothing — so the sentence it
  // replaces must still be in the download hint, word for word.
  const replaced = /const withoutLocalFile = \(hint\) =>\s*hint\.replace\("([^"]+)",/.exec(nativeJs);
  assert.ok(replaced, "couldn't find flash-native.js withoutLocalFile's replace");
  const downloadHint = classify(new Error("download failed: connection reset"));
  assert.strictEqual(downloadHint.kind, "download");
  assert.ok(downloadHint.hint.includes(replaced[1]),
    "the Flasher's download hint no longer says the sentence withoutLocalFile replaces — reword both");
  // The same retry rule (never `unknown`) and the same live-receipt rule.
  const retrySet = (src, where) => {
    const m = /const BAUD_RETRY_KINDS = new Set\(\[([^\]]*)\]\)/.exec(src);
    assert.ok(m, `couldn't find BAUD_RETRY_KINDS in ${where}`);
    return m[1].replace(/\s+/g, "");
  };
  assert.strictEqual(retrySet(nativeJs, "flash-native.js"), retrySet(appJs, "app.js"),
    "the Lab retries a different set of failures down the baud ladder than the Flasher");
  assert.match(nativeJs, /const requiresLiveReceipt = \(product\) => !product \|\| product\.serial_receipt !== false;/,
    "flash-native.js must judge the live boot receipt by the Flasher's rule");
  assert.match(appJs, /return !product \|\| product\.serial_receipt !== false;/, "the Flasher's live-receipt rule moved");

  // 2. The Linux port hints the backend names are said as-is, not coached
  //    as download mode: the frontends' OS-level pattern must match BOTH of
  //    the engine's hints, in both apps.
  const hint = (name) => {
    const m = new RegExp(`pub const ${name}: &str =\\s*"([\\s\\S]*?)";`).exec(engineRs("port_hint"));
    assert.ok(m, `couldn't parse ${name} from flash-engine port_hint.rs`);
    // Rust → runtime text: line continuations folded, \" unescaped.
    return m[1].replace(/\\\n\s*/g, "").replace(/\\"/g, '"');
  };
  const osLevelNative = /const OS_LEVEL_RE = (\/[^\n]*\/i);/.exec(nativeJs);
  assert.ok(osLevelNative, "flash-native.js lost its OS-level (Linux hint) pattern");
  assert.ok(appJs.includes(osLevelNative[1] + ".test(firstLine)"),
    "flash-native.js and the Flasher's identify() must recognize the same OS-level causes");
  const osRe = new Function(`return ${osLevelNative[1]};`)();
  for (const name of ["PERMISSION_HINT", "BUSY_HINT"]) {
    assert.ok(osRe.test(hint(name)), `the frontends' OS-level pattern no longer matches the engine's ${name}`);
  }

  // 3. Every event the pipeline and the monitor stream is heard — except
  //    the change map, which needs a safety copy this bench doesn't take
  //    (it sends no backup path, so the engine never emits it).
  for (const e of ["flash:log", "flash:progress", "serial:status", "serial:log", "serial:receipt"]) {
    assert.ok(nativeJs.includes(`listen("${e}"`), `the Lab's flash page never listens for ${e}`);
  }
  assert.match(nativeJs, /backupPath: "",/, "the native bench must say it has no safety copy (backupPath empty)");

  // 4. The capability flips only where everything behind it exists: the
  //    sidecar in the macOS + Linux configs, the release steps that fill it,
  //    and a frontend that branches on it.
  const serialCap = /"serial":\s*(.*),\s*$/m.exec(labRs);
  assert.ok(serialCap, "couldn't find the Lab's serial capability");
  if (serialCap[1].trim() !== "false") {
    assert.strictEqual(serialCap[1].trim(),
      'cfg!(any(target_os = "macos", target_os = "linux")) && espflash_bundled(&app)',
      "the Lab may advertise native flashing only on the two platforms whose release bundles espflash, " +
      "and only while that espflash is really there (a compile-time cfg! alone lights the bench on a " +
      "dev build's empty stub, where every board read fails at spawn)");
    // The runtime half: resolved where the spawn looks (the shell plugin's
    // own sidecar() path), and refused unless it is a non-empty executable
    // file (the engine's looks_runnable, unit-tested there).
    assert.match(labRs, /#\[cfg\(any\(target_os = "macos", target_os = "linux"\)\)\]\s*fn espflash_bundled\(app: &tauri::AppHandle\) -> bool \{\s*flash::espflash_bundled\(app\)\s*\}/,
      "lib.rs espflash_bundled must ask src/flash.rs on the platforms that bundle espflash");
    const labFlashRs = read(join(ROOT, "desktop-lab/src-tauri/src/flash.rs"));
    const bundled = /pub fn espflash_bundled\(app: &AppHandle\) -> bool \{([\s\S]*?)\n\}/.exec(labFlashRs);
    assert.ok(bundled, "src/flash.rs lost espflash_bundled");
    assert.match(bundled[1], /app\.shell\(\)\.sidecar\(ESPFLASH\)/,
      "espflash_bundled must resolve the path the spawn uses (shell().sidecar(ESPFLASH))");
    assert.match(bundled[1], /flash_engine::sidecar::looks_runnable\(/,
      "espflash_bundled must refuse a missing, empty or non-executable sidecar (looks_runnable)");
    assert.match(engineRs("sidecar"), /pub fn looks_runnable\(path: &std::path::Path\) -> bool \{[\s\S]*?meta\.len\(\) == 0/,
      "the engine's looks_runnable must refuse the empty compile-only stub");
    for (const plat of ["macos", "linux"]) {
      const conf = JSON.parse(read(join(ROOT, `desktop-lab/src-tauri/tauri.${plat}.conf.json`)));
      assert.ok(((conf.bundle || {}).externalBin || []).includes("binaries/espflash"),
        `serial is on, but tauri.${plat}.conf.json bundles no espflash`);
    }
    const labWf = read(join(ROOT, ".github/workflows/desktop-release.yml"));
    for (const step of ["Bundle espflash sidecar (macOS universal)", "Bundle espflash sidecar (Linux x86_64)"]) {
      assert.ok(labWf.includes(`- name: ${step}`), `serial is on, but desktop-release.yml has no "${step}" step`);
    }
    assert.match(flashJs, /import \{[^}]*\bmountNativeBench\b[^}]*\} from "\.\/flash-native\.js";/,
      "serial is on, but the Flash page never loads its native bench");
  }
  // The page asks before it chooses, prefers the native engine, and an app
  // that can't flash gets the in-app card — never the website's "get Chrome".
  assert.match(flashJs, /const native = await probeNative\(\);\s*const nativeSerial = !!\(native && native\.serial\);/,
    "flash.js must ask native_capabilities (probeNative) before choosing a path");
  assert.match(flashJs, /mount\.append\(native \? renderNativeUnavailable\(native\) : renderUnsupported\(\)\);/,
    "inside the Lab app, a build that can't flash must render the in-app card, not the website's");
  assert.ok(flashJs.indexOf("probeNative()") < flashJs.indexOf('"serial" in navigator'),
    "the native probe must come before the Web Serial check");
  assert.match(nativeJs, /invoke\("native_capabilities"\)/, "flash-native.js must probe native_capabilities");
});

// RELEASE_LESSONS 2026-09-03: a network claim lives on more surfaces than the
// file being edited, and nothing held them together — so this does. The
// native bench asks GitHub which firmware is published as soon as a board is
// read (identify → refreshManifest, and again on the dev toggle), BEFORE
// anyone presses Flash; every Lab surface that states what the app fetches
// must say that, and none may keep the old "only when you press Flash" / "the
// one thing it fetches on its own" wording that became false with it.
test("the Lab's network claim says what the native bench fetches, on every surface", () => {
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  assert.match(nativeJs, /recheck\.classList\.remove\("flash-hidden"\);\s*await refreshManifest\(\);\s*renderPick\(\);/,
    "the bench's manifest read moved — if it now waits for Flash, the claim below may say so again");
  // Comments and prose flattened: `#` / `//` line markers dropped, whitespace folded.
  const flat = (t) => t.replace(/^\s*(?:#|\/\/)\s?/gm, "").replace(/\s+/g, " ");
  const surfaces = [
    ["desktop-lab/README.md", 1],
    ["desktop-lab/INSTALL.md", 1],
    ["desktop-lab/src-tauri/tauri.conf.json", 1], // the .deb/AppImage store text
    ["desktop-lab/ipad-guide.html", 1],
    ["desktop-lab/src-tauri/Cargo.toml", 1],
    ["desktop-lab/src-tauri/src/lib.rs", 1],
    [".github/workflows/desktop-release.yml", 3], // the three release bodies
  ];
  for (const [path, want] of surfaces) {
    const text = flat(read(join(ROOT, path)));
    assert.strictEqual(text.split("which signed firmware is published").length - 1, want,
      `${path} must say the Lab asks which signed firmware is published once a board is connected (${want}×)`);
    for (const stale of [/only when (?:you press|the user presses) Flash/i, /fetches on its own/i,
      /the one thing it (?:ever )?fetches/i]) {
      assert.ok(!stale.test(text), `${path} still claims ${stale} — the bench fetches the release manifest before Flash`);
    }
  }
});

// A DOM just big enough for flash-native.js: elements with children, class
// lists, text, listeners and the few properties the bench reads and writes.
function fakeDocument() {
  class Node {
    constructor(tag) {
      this.tagName = tag; this.children = []; this.parent = null; this._text = "";
      this.className = ""; this.dataset = {}; this.style = {}; this.listeners = {};
      this.value = ""; this.checked = false; this.disabled = false; this.type = ""; this.name = "";
      this.scrollTop = 0; this.scrollHeight = 0; this.attrs = {};
      const self = this;
      this.classList = {
        add: (...c) => { const s = new Set(self.className.split(/\s+/).filter(Boolean)); c.forEach((x) => s.add(x)); self.className = [...s].join(" "); },
        remove: (...c) => { self.className = self.className.split(/\s+/).filter((x) => x && !c.includes(x)).join(" "); },
        toggle: (c, on) => { const has = self.classList.contains(c); const want = on === undefined ? !has : !!on; if (want && !has) self.classList.add(c); if (!want && has) self.classList.remove(c); },
        contains: (c) => self.className.split(/\s+/).includes(c),
      };
    }
    append(...nodes) { for (const n of nodes) { if (n.parent) n.remove(); n.parent = this; this.children.push(n); } }
    remove() { if (this.parent) { this.parent.children = this.parent.children.filter((c) => c !== this); this.parent = null; } }
    set innerHTML(_) { for (const c of [...this.children]) c.remove(); this._text = ""; }
    get textContent() { return this._text + this.children.map((c) => c.textContent).join(""); }
    set textContent(t) { for (const c of [...this.children]) c.remove(); this._text = String(t); }
    setAttribute(k, v) { this.attrs[k] = v; }
    addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); }
    fire(type, ev = {}) { for (const fn of this.listeners[type] || []) fn({ target: this, ...ev }); }
    *walk() { yield this; for (const c of this.children) yield* c.walk(); }
    find(pred) { for (const n of this.walk()) if (pred(n)) return n; return null; }
  }
  return {
    createElement: (tag) => new Node(tag),
    createTextNode: (t) => { const n = new Node("#text"); n._text = String(t); return n; },
  };
}

// A product's monitor commands as the generated catalog carries them (sweep
// A86): the record every bench reads, so the harness reads the same one.
const catalogMonitor = (id, cat = catalog) => cat.products.find((p) => p.id === id).monitor;

async function runNativeBench({ flashAnswers, detectAnswer, ports, startAnswer, onListen, catalog: cat = catalog }) {
  const calls = [];
  // What the monitor commands answer, changeable mid-run: a string here is
  // thrown the way Tauri rejects an invoke, with the command's Err text.
  const answers = { send: null };
  // Every command and every touch of the form, in order — so a test can say
  // what happened BEFORE the flash (the password cleared) and after it (the
  // form redrawn from its Wi-Fi memory).
  const timeline = [];
  const flashQueue = [...(flashAnswers || [])];
  const invoke = async (cmd, args) => {
    calls.push([cmd, args]);
    timeline.push(cmd);
    switch (cmd) {
      case "native_capabilities": return { serial: true, serial_list: true };
      // A non-USB port listed FIRST: only kind "usb" may be read as a Canary.
      case "list_ports": return ports || [{ name: "/dev/ttyS0", kind: "pci", vid: null, pid: null },
                                 { name: "/dev/ttyACM0", kind: "usb", vid: 0x303a, pid: 0x1001, product: "USB JTAG/serial debug unit" }];
      // Tauri rejects an invoke with the command's Err value itself — a
      // plain string, not an Error — so the fakes throw strings too.
      case "detect_chip":
        if (typeof detectAnswer === "string") throw detectAnswer;
        return { chip: "ESP32-S3", flash_bytes: 8 * 1024 * 1024, mac: "dc:54:75:c1:22:30", mac_check: { level: "clear", label: "ok" },
          ...(detectAnswer || {}) };
      case "fetch_manifest": {
        const products = {};
        for (const p of cat.products) products[p.id] = { version: "9.9.9", chipFamily: p.chip };
        return { schema: "securacv-flash-1", products };
      }
      case "flash": {
        const next = flashQueue.shift();
        if (typeof next === "string") throw next;
        return next;
      }
      case "start_serial_monitor":
        if (typeof startAnswer === "string") throw startAnswer;
        return null;
      case "serial_monitor_send":
        if (typeof answers.send === "string") throw answers.send;
        return null;
      default: return null; // stop_serial_monitor
    }
  };
  const doc = fakeDocument();
  const saved = { window: globalThis.window, document: globalThis.document, setInterval: globalThis.setInterval };
  const restore = () => Object.assign(globalThis, saved);
  globalThis.document = doc;
  const heard = {};
  const listen = async (evt, cb) => {
    if (onListen) onListen(evt, (e, payload) => { for (const f of heard[e] || []) f({ payload }); });
    (heard[evt] = heard[evt] || []).push(cb);
    return () => { heard[evt] = (heard[evt] || []).filter((f) => f !== cb); };
  };
  globalThis.window = { __TAURI__: { core: { invoke }, event: { listen } } };
  globalThis.setInterval = () => 0; // the 1 s port poll is driven by hand here
  try {
    const native = await import(pathToFileURL(join(CANARY, "assets/flash-native.js")).href);
    assert.deepStrictEqual(await native.probeNative(), { serial: true, serial_list: true });
    const mount = doc.createElement("div");
    const form = {
      credentials: () => { timeline.push("form:read"); return { ok: true, wifi: { ssid: "home", pass: "hunter22" }, mqtt: null, autoUpdate: true }; },
      clear() { timeline.push("form:clear"); },
    };
    await native.mountNativeBench(mount, {
      catalog: cat, renderWifiFields: () => { timeline.push("form:draw"); return form; },
    });
    const run = { calls, timeline, mount, doc, answers, close: restore };
    run.emit = (evt, payload) => { for (const cb of [...(heard[evt] || [])]) cb({ payload }); };
    // How many listeners an event has right now (sweep A85 counts them).
    run.listeners = (evt) => (heard[evt] || []).length;
    run.text = () => mount.textContent;
    run.pick = async (id) => {
      const name = cat.products.find((p) => p.id === id).name;
      const row = mount.find((n) => n.tagName === "label" && n.textContent.startsWith(name));
      assert.ok(row, `the picker offers no ${id}`);
      const radio = row.children[0];
      assert.ok(!radio.disabled, `${id} is not selectable`);
      radio.fire("change");
      const go = mount.find((n) => n.tagName === "button" && n.textContent === "Flash my Canary");
      go.fire("click");
      await drain(); await drain();
    };
    return run;
  } catch (e) {
    restore();
    throw e;
  }
}

test("native flashing: the Lab's flash page drives the Flasher's commands with the Flasher's arguments", async () => {
  const labFlash = read(join(ROOT, "desktop-lab/src-tauri/src/flash.rs"));
  const camel = (s) => s.replace(/_([a-z])/g, (_, c) => c.toUpperCase());
  const rustArgs = (src, fn) => {
    const sig = new RegExp(`fn ${fn}\\(([\\s\\S]*?)\\)\\s*->`).exec(src);
    assert.ok(sig, `couldn't parse fn ${fn}`);
    // Argument names only: `name: Type` at the start of each argument (a
    // generic's inner comma — State<'_, T> — starts no argument).
    const flat = sig[1].split("\n").map((l) => l.replace(/\/\/.*$/, "").trim()).join(" ");
    return [...flat.matchAll(/(?:^|,)\s*(\w+)\s*:/g)].map((m) => m[1])
      .filter((a) => !["app", "state"].includes(a)).map(camel).sort();
  };
  const receipt = { target: "esp32-host", product_id: "securacv-canary", version: "9.9.9", release_sha256: "a".repeat(64),
    installed_sha256: "b".repeat(64), bytes_written: 1, release_verification: "ed25519+sha256", channel: "stable",
    chip_write_verified: true, provisioned: true };

  // A transport fault at the top speed, then success one rung down.
  const run = await runNativeBench({
    flashAnswers: ["espflash exited with code 1. The board can't be bricked — put it back in download mode and try again.\nError: Failed to connect to the device\nCaused by: No serial data received", receipt],
  });
  try {
  const order = run.calls.map(([c]) => c);
  assert.ok(order.indexOf("list_ports") < order.indexOf("detect_chip"), "the bench must list ports before reading the board");
  assert.deepStrictEqual(run.calls.find(([c]) => c === "detect_chip")[1], { port: "/dev/ttyACM0" },
    "only the USB port is a Canary candidate (the Flasher's kind === \"usb\" filter)");
  assert.deepStrictEqual(run.calls.find(([c]) => c === "fetch_manifest")[1], { manifestUrl: catalog.manifest_url },
    "the bench must read the catalog's pinned stable manifest");
  assert.match(run.text(), /Connected · ESP32-S3 · 8 MB flash on \/dev\/ttyACM0/);
  // Only firmware for this silicon is offered.
  for (const p of catalog.products) {
    const offered = run.mount.find((n) => n.tagName === "label" && n.textContent.startsWith(p.name + " — "));
    assert.strictEqual(!!offered, normChip(p.chip) === "ESP32S3", `${p.id} offered on an ESP32-S3: ${!!offered}`);
  }
  await run.pick("securacv-canary");
  const flashes = run.calls.filter(([c]) => c === "flash").map(([, a]) => a);
  assert.deepStrictEqual(flashes.map((a) => a.baud), [catalog.flash_baud || 921600, 460800],
    "a not-in-download failure must retry one rung down the baud ladder, like the Flasher");
  const args = flashes[1];
  assert.deepStrictEqual(Object.keys(args).sort(), rustArgs(labFlash, "flash"),
    "the bench's invoke(\"flash\") keys must be exactly the command's arguments (camelCased by Tauri)");
  assert.strictEqual(args.productId, "securacv-canary");
  assert.strictEqual(args.detectedChip, "ESP32-S3");
  assert.strictEqual(args.eraseFirst, true, "first contact must default to the full erase, as in the Flasher");
  assert.strictEqual(args.backupPath, "");
  const provFields = [...engineRs("provisioning").matchAll(/^\s+pub (\w+):/gm)].map((m) => camel(m[1]));
  const provStruct = /pub struct Provisioning \{([\s\S]*?)\n\}/.exec(engineRs("provisioning"))[1];
  const structFields = [...provStruct.matchAll(/^\s+pub (\w+):/gm)].map((m) => camel(m[1])).sort();
  assert.ok(provFields.length >= structFields.length);
  assert.deepStrictEqual(Object.keys(args.provisioning).sort(), structFields,
    "the bench's provisioning object must carry exactly the engine's Provisioning fields");
  assert.strictEqual(args.provisioning.wifiSsid, "home");
  assert.strictEqual(args.provisioning.wifiNvs, catalog.products.find((p) => p.id === "securacv-canary").wifi_nvs || "string");
  const start = run.calls.find(([c]) => c === "start_serial_monitor");
  assert.ok(start, "a successful flash must start the boot-receipt monitor");
  assert.deepStrictEqual(Object.keys(start[1]).sort(), rustArgs(labFlash, "start_serial_monitor"));
  assert.strictEqual(start[1].postFlash, true);
  assert.strictEqual(start[1].vid, 0x303a);
  // Who checked what: the app verified the release signature; espflash and
  // the chip only confirmed the write. The chip is never credited with the
  // Ed25519 check ("verified" is the signature's word alone).
  assert.match(run.text(), /Written, and espflash confirmed the write on the chip · release signature verified against the pinned key \(ed25519\+sha256\) · stable channel · installed SHA-256 b{16}…\. Your settings were sealed/);
  assert.ok(!/verified by the chip/i.test(run.text()), "the success text must not credit the chip with the release check");
  // The Wi-Fi password leaves the DOM the moment it is read — before the
  // first write, not after a success — and the form is drawn afresh once the
  // attempt ends, so a retry pre-fills from memory instead of re-reading an
  // emptied field (which would provision, and remember, an empty password).
  const tl = run.timeline;
  assert.ok(tl.indexOf("form:read") < tl.indexOf("form:clear") && tl.indexOf("form:clear") < tl.indexOf("flash"),
    "the bench must clear the Wi-Fi password right after reading it, before flashing (flash.js does)");
  assert.ok(tl.lastIndexOf("form:draw") > tl.lastIndexOf("flash"), "the form must be redrawn after the attempt");
  // The board's own receipt, as the monitor streams it: `firmware` is the
  // version string the self-manifest reports.
  run.emit("serial:receipt", { target: "esp32-host", ready: true,
    manifest: { schema: "securacv.canary.manifest/v1", board: "canary", firmware: "9.9.9" } });
  assert.match(run.text(), /✓ canary booted and answered with its receipt · firmware 9\.9\.9\./,
    "the Lab must read the board's receipt the way the Flasher does");
  } finally { run.close(); }

  // A refused permission is NOT retried at a gentler speed, and is named.
  const denied = await runNativeBench({
    flashAnswers: ["espflash exited with code 1. The board can't be bricked — put it back in download mode and try again.\nError: Permission denied (os error 13)"],
  });
  try {
    await denied.pick("securacv-canary");
    assert.strictEqual(denied.calls.filter(([c]) => c === "flash").length, 1, "a permission failure must not walk the baud ladder");
    assert.match(denied.text(), /The system wouldn't grant access to the port/);
    assert.ok(!denied.calls.some(([c]) => c === "start_serial_monitor"), "no monitor after a failed write");
    // A FAILED flash strands no password either.
    const dtl = denied.timeline;
    assert.ok(dtl.indexOf("form:clear") > -1 && dtl.indexOf("form:clear") < dtl.indexOf("flash"),
      "a failed flash must not leave the Wi-Fi password in the DOM");
    assert.ok(dtl.lastIndexOf("form:draw") > dtl.lastIndexOf("flash"), "the form must be redrawn after a failure");
  } finally { denied.close(); }

  // A download failure's advice points at the Flasher for a local .bin — this
  // bench has no "Advanced" local-file install to send anyone to.
  const offline = await runNativeBench({
    flashAnswers: ["download failed: error sending request for url (connection reset)"],
  });
  try {
    await offline.pick("securacv-canary");
    assert.match(offline.text(), /Couldn't download the firmware image/);
    assert.ok(offline.text().includes("A local .bin can be installed with the SecuraCV Flasher."),
      "the download advice must point at the Flasher for a local .bin");
    assert.ok(!/under Advanced/.test(offline.text()), "the Lab has no Advanced local-file install to point at");
  } finally { offline.close(); }

  // The backend's Linux hint is said as-is when the board can't be read.
  const permHint = /pub const PERMISSION_HINT: &str =\s*"([\s\S]*?)";/.exec(engineRs("port_hint"))[1]
    .replace(/\\\n\s*/g, "").replace(/\\"/g, '"'); // the string at runtime
  const blocked = await runNativeBench({ detectAnswer: `${permHint}\n\nespflash said:\nError: Permission denied` });
  try {
    assert.ok(blocked.text().includes(`Found /dev/ttyACM0 — ${permHint}`),
      "a Linux permission failure must show the backend's own hint, not download-mode coaching");
    assert.ok(!/Put it in download mode/.test(blocked.text()));
    assert.ok(!blocked.calls.some(([c]) => c === "fetch_manifest"), "nothing is fetched for a board that couldn't be read");
  } finally { blocked.close(); }

  // A bundled espflash that never started reads as the app's engine, with the
  // system's reason — not as download mode, and not as the CP210x driver the
  // bridge note would otherwise blame (the port was never opened).
  const stuck = await runNativeBench({
    detectAnswer: "could not start espflash: No such file or directory (os error 2)",
    ports: [{ name: "/dev/ttyUSB0", kind: "usb", vid: 0x10c4, pid: 0xea60, product: "CP2102 USB to UART Bridge Controller" }],
  });
  try {
    assert.match(stuck.text(), /Found \/dev\/ttyUSB0 — The app's flash engine couldn't start\. That's this app, not your board/);
    assert.ok(stuck.text().includes("(The system said: No such file or directory (os error 2))"),
      "the Lab must say the system's reason for a sidecar that never started");
    assert.ok(!/Put it in download mode|hold BOOT, tap RESET/.test(stuck.text()),
      "a sidecar that never started must not coach download mode");
    assert.ok(!/Silicon Labs|driver/i.test(stuck.text()), "a sidecar that never started must not blame the USB driver");
    assert.ok(!stuck.calls.some(([c]) => c === "fetch_manifest"), "nothing is fetched for a board that couldn't be read");
  } finally { stuck.close(); }

  // The camera module's own port is recognized by the catalog's USB id and
  // never read as an ESP32 (the Flasher short-circuits on the same id).
  const vid = parseInt(String(catalog.we2_module.usb_vid).replace(/^0x/i, ""), 16);
  const pid = parseInt(String(catalog.we2_module.usb_pid).replace(/^0x/i, ""), 16);
  const module = await runNativeBench({ ports: [{ name: "/dev/ttyUSB0", kind: "usb", vid, pid, product: "USB Single Serial" }] });
  try {
    assert.ok(!module.calls.some(([c]) => c === "detect_chip"), "the Vision module's port must not be read with board-info");
    assert.match(module.text(), /Grove Vision AI V2 camera module/);
  } finally { module.close(); }
});

// ── sweep A67: the Lab's post-flash monitor talks back ─────────────────────
//
// The Flasher's monitor sends commands (its chips and command box) and so
// does the browser flasher's; the Lab app's native monitor only read, and the
// Lab's serial_monitor_send registration had been removed for want of a
// caller. The row is back on the Lab's receipt card: the flashed product's
// own commands from the catalog it is handed (sweep A86 — flash.json's
// products[].monitor, which every bench reads), the Flasher's words for the
// box, each send through the Flasher's command with the newline the
// firmware's parser reads, usable only while the monitor runs, and a refused
// send said where the Flasher says it.
test("A67: the Lab's post-flash monitor has the Flasher's command row, live only while the monitor runs", async () => {
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const labFlash = read(join(ROOT, "desktop-lab/src-tauri/src/flash.rs"));
  // One source: the bench is handed the catalog and keeps no table of its own.
  assert.match(flashJs, /mountNativeBench\(mount, \{ catalog: state\.catalog, renderWifiFields \}\)/,
    "flash.js must hand the native bench the catalog its monitor reads");
  assert.doesNotMatch(nativeJs, /MONITOR_CMDS|\[\s*"h"\s*,\s*"help"\s*\]/,
    "flash-native.js must not keep its own copy of a command table");
  const cmds = catalogMonitor("securacv-canary").cmds;
  assert.ok(cmds.length >= 2, "the flagship's catalog record carries its commands");

  const receipt = { target: "esp32-host", product_id: "securacv-canary", version: "9.9.9", release_sha256: "a".repeat(64),
    installed_sha256: "b".repeat(64), bytes_written: 1, release_verification: "ed25519+sha256", channel: "stable",
    chip_write_verified: true, provisioned: false };
  const sends = (run) => run.calls.filter(([c]) => c === "serial_monitor_send").map(([, a]) => a);
  const controls = (run) => {
    const chips = [...run.mount.walk()].filter((n) => n.tagName === "button" && n.dataset.cmd);
    const input = run.mount.find((n) => n.tagName === "input" && n.attrs["aria-label"] === "Serial command");
    const send = run.mount.find((n) => n.tagName === "button" && n.textContent === "Send");
    return { chips, input, send, all: [...chips, input, send].filter(Boolean) };
  };

  const run = await runNativeBench({ flashAnswers: [receipt] });
  try {
    await run.pick("securacv-canary");
    assert.ok(run.calls.some(([c]) => c === "start_serial_monitor"), "the flash must start the monitor");
    const { chips, input, send, all } = controls(run);
    // The chips: the table's keys, in order, in the Flasher's "label (key)" form.
    assert.deepStrictEqual(chips.map((b) => b.dataset.cmd), cmds.map(([ch]) => ch));
    assert.deepStrictEqual(chips.map((b) => b.textContent), cmds.map(([ch, label]) => `${label} (${ch})`));
    // The box: the flashed firmware's placeholder (the record the Flasher
    // sets its box from), the Flasher's label and length, and a Send.
    const flasherBox = /<input id="monitor-cmd"[\s\S]*?\/>/.exec(html)[0];
    const attr = (name) => new RegExp(`${name}="([^"]*)"`).exec(flasherBox)[1];
    assert.ok(input && send, "the receipt card has no command box");
    assert.strictEqual(input.placeholder, catalogMonitor("securacv-canary").placeholder,
      "the box must teach what the flashed firmware reads");
    assert.strictEqual(input.attrs["aria-label"], attr("aria-label"));
    assert.strictEqual(input.maxLength, Number(attr("maxlength")));
    for (const c of all) assert.strictEqual(c.disabled, false, "the row must be usable while the monitor runs");

    // Each chip sends its key and a newline, through the Flasher's command
    // with exactly that command's arguments.
    for (const b of chips) b.fire("click");
    await drain();
    const camel = (x) => x.replace(/_([a-z])/g, (_, c) => c.toUpperCase());
    const sig = /fn serial_monitor_send\(([\s\S]*?)\)\s*->/.exec(labFlash);
    assert.ok(sig, "the Lab's flash.rs must define serial_monitor_send");
    const rustArgs = [...sig[1].matchAll(/(?:^|,)\s*(\w+)\s*:/g)].map((m) => m[1]).filter((a) => a !== "state").map(camel);
    assert.deepStrictEqual(sends(run).map((a) => Object.keys(a)), cmds.map(() => rustArgs),
      "invoke(\"serial_monitor_send\") must carry exactly the command's arguments");
    assert.deepStrictEqual(sends(run).map((a) => a.command), cmds.map(([ch]) => ch + "\n"),
      "each chip must send its key with the newline the firmware's parser reads");

    // The box sends what was typed plus a newline, on Enter and on Send, and
    // clears once the send went; an empty box sends nothing.
    const before = sends(run).length;
    input.value = "wifi";
    input.fire("keydown", { key: "Enter", preventDefault() {} });
    await drain();
    assert.strictEqual(input.value, "", "a sent command leaves the box empty for the next");
    input.value = "s";
    send.fire("click");
    await drain();
    input.value = "";
    send.fire("click");
    input.fire("keydown", { key: "Enter", preventDefault() {} });
    input.value = "x";
    input.fire("keydown", { key: "a" });
    await drain();
    assert.deepStrictEqual(sends(run).slice(before).map((a) => a.command), ["wifi\n", "s\n"],
      "the box must send typed text plus a newline on Enter or Send, and nothing for an empty box or another key");
    input.value = "";

    // A refused send is said in the status line — the engine's own words — and
    // the typed command stays in the box to send again.
    const notRunning = /"serial monitor is not running"/.test(engineRs("monitor")) && "serial monitor is not running";
    assert.ok(notRunning, "the engine's refusal text moved");
    run.answers.send = notRunning;
    chips[0].fire("click");
    await drain();
    assert.ok(run.text().includes(notRunning), "a refused chip send must be shown, not swallowed");
    run.emit("serial:status", "Serial monitor connected to /dev/ttyACM0 at 115200 baud.");
    input.value = "t";
    send.fire("click");
    await drain();
    assert.ok(run.text().includes(notRunning), "a refused typed send must be shown, not swallowed");
    assert.strictEqual(input.value, "t", "a refused typed command stays in the box");
    run.answers.send = null;

    // When the monitor stops, the row goes with it: every control disabled,
    // and nothing is sent.
    run.emit("serial:status", "Serial monitor stopped.");
    for (const c of all) assert.strictEqual(c.disabled, true, "the row must be disabled once the monitor stops");
    const atStop = sends(run).length;
    chips[1].fire("click");
    send.fire("click");
    await drain();
    assert.strictEqual(sends(run).length, atStop, "a stopped monitor must send nothing");
  } finally { run.close(); }

  // "Stop watching" disables the row too.
  const stopped = await runNativeBench({ flashAnswers: [receipt] });
  try {
    await stopped.pick("securacv-canary");
    const { all } = controls(stopped);
    for (const c of all) assert.strictEqual(c.disabled, false);
    stopped.mount.find((n) => n.tagName === "button" && n.textContent === "Stop watching").fire("click");
    await drain();
    for (const c of all) assert.strictEqual(c.disabled, true, "Stop watching must disable the row");
  } finally { stopped.close(); }

  // The row mirrors what the bench believes about the port: a stop heard while
  // the card is still wiring itself up (the engine speaks from its own
  // thread) has the row drawn disabled, not enabled over a stopped monitor.
  const early = await runNativeBench({ flashAnswers: [receipt],
    onListen: (evt, emit) => { if (evt === "serial:receipt") emit("serial:status", "Serial monitor stopped."); } });
  try {
    await early.pick("securacv-canary");
    const { all } = controls(early);
    assert.strictEqual(all.length, cmds.length + 2);
    for (const c of all) assert.strictEqual(c.disabled, true, "a row drawn after the monitor stopped must be disabled");
  } finally { early.close(); }

  // A monitor that never started leaves the row disabled, beside the reason.
  const unstarted = await runNativeBench({ flashAnswers: [receipt], startAnswer: "serial monitor is already running" });
  try {
    await unstarted.pick("securacv-canary");
    const { all } = controls(unstarted);
    assert.strictEqual(all.length, cmds.length + 2, "the row is drawn even when the monitor did not start");
    for (const c of all) assert.strictEqual(c.disabled, true, "a monitor that never started must leave the row disabled");
    assert.match(unstarted.text(), /but the serial monitor didn’t start \(serial monitor is already running\)/);
  } finally { unstarted.close(); }
});

// Sweep A67 review: two paths the row test above did not drive. "Read the
// board again" takes the port back from the monitor, and must take the row
// with it: a row left enabled there looks live while sendCommand refuses
// without a word. And the chips are the catalog the bench is HANDED: a bench
// that drew its own copy of a firmware's keys would pass every check that
// hands it the real catalog, so this hands it a record no firmware has.
test("A67: a re-read takes the row with the port, and the chips are the table the bench is handed", async () => {
  const receipt = { target: "esp32-host", product_id: "securacv-canary", version: "9.9.9", release_sha256: "a".repeat(64),
    installed_sha256: "b".repeat(64), bytes_written: 1, release_verification: "ed25519+sha256", channel: "stable",
    chip_write_verified: true, provisioned: false };
  const sends = (run) => run.calls.filter(([c]) => c === "serial_monitor_send").map(([, a]) => a.command);
  const controls = (run) => {
    const chips = [...run.mount.walk()].filter((n) => n.tagName === "button" && n.dataset.cmd);
    const input = run.mount.find((n) => n.tagName === "input" && n.attrs["aria-label"] === "Serial command");
    const send = run.mount.find((n) => n.tagName === "button" && n.textContent === "Send");
    return { chips, input, send, all: [...chips, input, send].filter(Boolean) };
  };

  const reread = await runNativeBench({ flashAnswers: [receipt] });
  try {
    await reread.pick("securacv-canary");
    const { chips, input, send, all } = controls(reread);
    assert.strictEqual(all.length, catalogMonitor("securacv-canary").cmds.length + 2);
    for (const c of all) assert.strictEqual(c.disabled, false, "the row must be usable while the monitor runs");
    const stopsBefore = reread.calls.filter(([c]) => c === "stop_serial_monitor").length;
    reread.mount.find((n) => n.tagName === "button" && n.textContent === "Read the board again").fire("click");
    await drain(); await drain();
    assert.strictEqual(reread.calls.filter(([c]) => c === "stop_serial_monitor").length, stopsBefore + 1,
      "a re-read must take the port back from the monitor");
    for (const c of all) assert.strictEqual(c.disabled, true, "a re-read must disable the row with the monitor");
    const atReread = sends(reread).length;
    chips[0].fire("click");
    input.value = "h";
    send.fire("click");
    await drain();
    assert.strictEqual(sends(reread).length, atReread, "a re-read monitor must send nothing");
  } finally { reread.close(); }

  const odd = structuredClone(catalog);
  odd.products.find((p) => p.id === "securacv-canary").monitor =
    { kind: "keys", cmds: [["z", "zed"], ["q", "quiet"]], placeholder: "a box no firmware has" };
  const handed = await runNativeBench({ flashAnswers: [receipt], catalog: odd });
  try {
    await handed.pick("securacv-canary");
    const { chips } = controls(handed);
    assert.deepStrictEqual(chips.map((b) => b.textContent), ["zed (z)", "quiet (q)"],
      "the chips must be the catalog record the bench is handed, not a copy of its own");
    assert.strictEqual(handed.mount.find((n) => n.tagName === "input" && n.attrs["aria-label"] === "Serial command").placeholder,
      "a box no firmware has", "the placeholder is the handed record's too");
    for (const b of chips) b.fire("click");
    await drain();
    assert.deepStrictEqual(sends(handed), ["z\n", "q\n"]);
  } finally { handed.close(); }
});

// ── sweep A85: the receipt card lets go of the previous flash ──────────────
//
// renderReceipt registered serial:status, serial:log and serial:receipt on
// every flash and removed them only on Stop watching, so each further flash
// in one session left the earlier card's three attached, writing into a card
// no longer on the page. A new card, a new flash and a re-read now each let
// go of the last card's listeners. Counted here, across flashes.
test("A85: a second flash leaves one set of monitor listeners, and a re-read leaves none", async () => {
  const receipt = { target: "esp32-host", product_id: "securacv-canary", version: "9.9.9", release_sha256: "a".repeat(64),
    installed_sha256: "b".repeat(64), bytes_written: 1, release_verification: "ed25519+sha256", channel: "stable",
    chip_write_verified: true, provisioned: false };
  const EVENTS = ["serial:status", "serial:log", "serial:receipt"];
  const counts = (run) => EVENTS.map((e) => run.listeners(e));
  const consoles = (run) => [...run.mount.walk()].filter((n) => n.tagName === "pre" && n.className === "flash-console-tall");

  const run = await runNativeBench({ flashAnswers: [receipt, receipt, receipt, "Failed to open serial port /dev/ttyACM0: busy"] });
  try {
    await run.pick("securacv-canary");
    assert.deepStrictEqual(counts(run), [1, 1, 1], "one flash, one card: one listener per serial event");
    const first = consoles(run);
    assert.strictEqual(first.length, 1);

    // The same session flashes again: the first card's listeners go with it.
    await run.pick("securacv-canary");
    assert.deepStrictEqual(counts(run), [1, 1, 1],
      "a second flash must leave one set of listeners, not stack another on the first card's");
    const second = consoles(run);
    assert.strictEqual(second.length, 1, "only the new card is on the page");
    assert.notStrictEqual(second[0], first[0], "the receipt card was drawn afresh");
    run.emit("serial:log", "boot line\n");
    assert.strictEqual(first[0].textContent, "", "the detached card must hear nothing more");
    assert.strictEqual(second[0].textContent, "boot line\n", "the new card hears the port");

    // A third flash, then a fourth that fails: the bench takes the port back
    // for the write, and a failed write draws no card, so nothing is left
    // listening to a monitor that is not running.
    await run.pick("securacv-canary");
    assert.deepStrictEqual(counts(run), [1, 1, 1], "a third flash still leaves one set");
    await run.pick("securacv-canary");
    assert.ok(run.text().includes("That port is busy"), "the fourth flash failed as scripted");
    assert.deepStrictEqual(counts(run), [0, 0, 0],
      "a flash that took the port back and failed must leave no monitor listeners behind");
  } finally { run.close(); }

  // "Read the board again" takes the port back from the monitor: the card's
  // listeners go with it.
  const reread = await runNativeBench({ flashAnswers: [receipt] });
  try {
    await reread.pick("securacv-canary");
    assert.deepStrictEqual(counts(reread), [1, 1, 1]);
    reread.mount.find((n) => n.tagName === "button" && n.textContent === "Read the board again").fire("click");
    await drain(); await drain();
    assert.deepStrictEqual(counts(reread), [0, 0, 0], "a re-read must let go of the card's listeners");
  } finally { reread.close(); }

  // Stop watching still lets go of them, as it always did.
  const stopped = await runNativeBench({ flashAnswers: [receipt] });
  try {
    await stopped.pick("securacv-canary");
    stopped.mount.find((n) => n.tagName === "button" && n.textContent === "Stop watching").fire("click");
    await drain();
    assert.deepStrictEqual(counts(stopped), [0, 0, 0], "Stop watching must let go of the card's listeners");
  } finally { stopped.close(); }
});

// ── sweep A86: each product's monitor offers its own firmware's commands ────
//
// The three benches offered one set of eight chips (the WAP sketch's menu,
// plus a `j` the sketch does not have) and a "single keys; h shows its menu"
// placeholder for every product. flash.json now carries each product's own
// (products[].monitor, gen_flash.py monitor_block). The firmware oracle below
// is a second implementation of the generator's reading (the same method —
// comments and literals blanked, #if depth counted — written again here, so a
// generator change that loosens it does not loosen this), not an independent
// one: a blind spot of the method is shared. Every chip must be a command the
// product's firmware answers with no #if around it, labeled in the
// firmware's own help-menu words.
const A86_ROLE_FIRMWARE = {
  // role -> how its firmware reads the console
  canary: { kind: "keys", handler: "firmware/canary/src/main.cpp" },
  wap: { kind: "keys", handler: "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino" },
  vision: { kind: "keys", handler: "firmware/projects/canary-vision/src/main.cpp" },
  sense: { kind: "words", handler: "firmware/common/console/tuning_console.h",
    wiring: "firmware/projects/canary-sense/src/main.cpp" },
  display: { kind: "none", tree: "firmware/projects/canary-display" },
};

// Comments, strings and char literals blanked (same length, newlines kept),
// so code is read where the compiler reads it.
const a86Strip = (src) => src.replace(
  /(?<![A-Za-z0-9_])R"([^()\\\s]{0,16})\([\s\S]*?\)\1"|\/\/[^\n]*|\/\*[\s\S]*?\*\/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'/g,
  (m) => m.replace(/[^\n]/g, " "));

// The body of C/C++ function `name`, braces counted on the stripped text.
const a86Body = (src, name) => {
  const stripped = a86Strip(src);
  const m = new RegExp(`\\b${name}\\s*\\([^;{)]*\\)\\s*(?:const\\s*)?\\{`).exec(stripped);
  assert.ok(m, `no definition of ${name}()`);
  let d = 1, i = m.index + m[0].length;
  for (; i < stripped.length && d; i++) {
    if (stripped[i] === "{") d++;
    else if (stripped[i] === "}") d--;
  }
  return { raw: src.slice(m.index + m[0].length, i - 1), stripped: stripped.slice(m.index + m[0].length, i - 1) };
};

// Lines of a body under no #if opened inside it, as [stripped, raw] pairs.
const a86Unconditional = ({ raw, stripped }) => {
  const rawLines = raw.split("\n"), out = [];
  let depth = 0;
  stripped.split("\n").forEach((line, n) => {
    if (/^\s*#\s*if/.test(line)) depth++;
    else if (/^\s*#\s*endif\b/.test(line)) depth--;
    else if (depth === 0) out.push([line, rawLines[n]]);
  });
  return out;
};
const a86Words = (t) => new Set(t.toLowerCase().replace(/-/g, "").split(/[^a-z0-9]+/).filter(Boolean));
const a86Literals = (t) => [...t.matchAll(/"((?:[^"\\\n]|\\.)*)"/g)].map((m) => m[1]);

// A key console's answers, read from one handler source: the keys
// handle_serial_commands() takes under no #if opened inside it, and each
// key's help-line words. { commands: Set, help: Map(key -> Set(words)) }.
function a86ReadKeys(src) {
  const commands = new Set(), help = new Map();
  const body = a86Body(src, "handle_serial_commands");
  for (const [line, raw] of a86Unconditional(body)) {
    for (const m of raw.matchAll(/\bcase\s+'(.)'\s*:|\b\w+\s*==\s*'(.)'/g)) {
      if (line[m.index] !== " ") commands.add(m[1] || m[2]);
    }
  }
  let helpText = body.raw;
  if (/\bprint_help\s*\(/.test(body.stripped)) helpText += a86Body(src, "print_help").raw;
  for (const lit of a86Literals(helpText)) {
    const m = /^[\s│]*(\S)(?:\s*\/\s*\S)?\s+[-:]\s+(.+?)[\s│]*$/.exec(lit);
    if (m) help.set(m[1], new Set([...(help.get(m[1]) || []), ...a86Words(m[2])]));
  }
  return { commands, help };
}

// A word console's answers (the tuning console's execute_()): its words, the
// "word arg" pairs it reads, and each word's print_help() words.
function a86ReadWords(src) {
  const commands = new Set(), args = new Set(), help = new Map();
  let current = null;
  for (const [line, raw] of a86Unconditional(a86Body(src, "execute_"))) {
    const live = (re) => [...raw.matchAll(re)].filter((m) => line[m.index] !== " ").map((m) => m[1]);
    const found = live(/\bieq_\(\s*cmd\s*,\s*"([^"]+)"\s*\)/g);
    if (found.length) { found.forEach((w) => commands.add(w)); current = found[0]; }
    for (const a of live(/\bieq_\(\s*tok\[1\]\s*,\s*"([^"]+)"\s*\)/g)) if (current) args.add(`${current} ${a}`);
  }
  for (const lit of a86Literals(a86Body(src, "print_help").raw)) {
    const m = /^\[tune\]\s+(\w+)\b/.exec(lit);
    if (m) help.set(m[1], new Set([...(help.get(m[1]) || []), ...a86Words(lit)]));
  }
  return { commands, args, help };
}

// Whether a project's source feeds its USB console to the tuning console.
const a86FeedsConsole = (src) => /\.feed\s*\(\s*\(char\)\s*Serial\s*\.\s*read\s*\(\s*\)\s*\)/.test(a86Strip(src));

// What one role's firmware answers: { commands: Set, args: Set("word arg"), help: Map(cmd -> Set(words)) }.
function a86Answered(role) {
  const fw = A86_ROLE_FIRMWARE[role];
  assert.ok(fw, `no firmware known for role ${role}`);
  if (fw.kind === "keys") return { kind: fw.kind, args: new Set(), ...a86ReadKeys(read(join(ROOT, fw.handler))) };
  if (fw.kind === "words") {
    // …and the project actually feeds its USB console to it.
    assert.ok(a86FeedsConsole(read(join(ROOT, fw.wiring))),
      "the Sense firmware no longer feeds its serial console to the tuning console");
    return { kind: fw.kind, ...a86ReadWords(read(join(ROOT, fw.handler))) };
  }
  return { kind: fw.kind, commands: new Set(), args: new Set(), help: new Map() };
}

// Every reason a product's chips are not its firmware's; [] when they are.
function a86Wrong(product, monitor) {
  const fw = a86Answered(product.role);
  const out = [];
  if (monitor.kind !== fw.kind) out.push(`${product.id}: kind ${monitor.kind}, its firmware reads ${fw.kind}`);
  for (const [cmd, label] of monitor.cmds) {
    const [word, ...rest] = cmd.split(" ");
    if (!fw.commands.has(word)) { out.push(`${product.id}: ${JSON.stringify(cmd)} is not a command its firmware answers`); continue; }
    for (const a of rest) if (!fw.args.has(`${word} ${a}`)) out.push(`${product.id}: ${JSON.stringify(cmd)} — no ${a} after ${word}`);
    const missing = [...a86Words(label)].filter((w) => !(fw.help.get(word) || new Set()).has(w));
    if (missing.length) out.push(`${product.id}: ${JSON.stringify(cmd)} labeled ${JSON.stringify(label)} — not its help line's words (${missing})`);
  }
  if (fw.kind === "keys" && !monitor.cmds.some(([c]) => c === "h")) out.push(`${product.id}: no h, yet the box says h shows the menu`);
  return out;
}

test("A86: every product's monitor chips are commands its own firmware answers, in its own words", () => {
  // The display firmware reads no serial console at all (its Serial1 is the
  // RS-485 bus), which is what its empty record says.
  const fwDir = join(ROOT, A86_ROLE_FIRMWARE.display.tree);
  const sources = [];
  const walk = (d) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) { if (![".pio", "build", "dist"].includes(e.name)) walk(p); }
      else if (/\.(c|cc|cpp|h|hpp|ino)$/i.test(e.name)) sources.push(p);
    }
  };
  walk(fwDir);
  assert.ok(sources.length > 20, "couldn't list the display firmware's sources");
  for (const f of sources) {
    assert.doesNotMatch(a86Strip(read(f)), /\bSerial\s*\.\s*(?:read|available|peek|readString|readBytes)\s*\(/,
      `${f} reads the display's serial console — its monitor record says it reads none`);
  }

  const roles = new Set();
  for (const p of catalog.products) {
    roles.add(p.role);
    const m = p.monitor;
    assert.ok(m && Array.isArray(m.cmds) && typeof m.placeholder === "string", `${p.id} carries no monitor record`);
    assert.deepStrictEqual(a86Wrong(p, m), [], `${p.id}'s chips are not its firmware's`);
    if (p.role === "display") {
      assert.deepStrictEqual(m.cmds, [], `${p.id}: a firmware that reads no commands shows no chips`);
      assert.match(m.placeholder, /reads no serial commands/, `${p.id}: the box must say the firmware reads none`);
    } else {
      assert.ok(m.cmds.length >= 2, `${p.id}: a firmware with commands offers them`);
    }
    if (m.kind === "keys") assert.match(m.placeholder, /single keys; h shows its menu/);
    if (m.kind === "words") assert.match(m.placeholder, /reads words; help lists them/);
  }
  assert.deepStrictEqual([...roles].sort(), Object.keys(A86_ROLE_FIRMWARE).sort(),
    "a product role this oracle does not know — say how its firmware reads the console");
  assert.deepStrictEqual(catalog.monitor_unknown.cmds, [], "an unidentified board shows no chips");

  // The products differ where their firmware does (the reason for A86).
  const keysOf = (id) => catalogMonitor(id).cmds.map(([c]) => c);
  assert.ok(!keysOf("securacv-canary-wap").includes("j"), "the WAP sketch has no j");
  assert.deepStrictEqual(keysOf("securacv-canary-vision").sort(), ["h", "j"]);
  assert.ok(keysOf("securacv-canary-sense").every((c) => c.length > 1), "Sense's console reads words");
  assert.strictEqual(catalogMonitor("securacv-canary").cmds.find(([c]) => c === "t")[1], "all tests",
    "the flagship's t runs its tests");

  // The oracle catches what the old one-for-all table got wrong: shown for
  // each firmware it was offered for, it fails every one of them.
  const OLD = [["h", "help"], ["j", "self-manifest"], ["i", "identity"], ["s", "status"],
               ["t", "time"], ["w", "wifi"], ["m", "system"], ["b", "battery"]];
  for (const id of ["securacv-canary", "securacv-canary-wap", "securacv-canary-vision", "securacv-canary-sense"]) {
    const p = catalog.products.find((x) => x.id === id);
    assert.notDeepStrictEqual(a86Wrong(p, { ...p.monitor, cmds: OLD }), [], `the old table passes for ${id}`);
  }
  const wap = catalog.products.find((x) => x.id === "securacv-canary-wap");
  assert.ok(a86Wrong(wap, { ...wap.monitor, cmds: [["h", "help"], ["j", "self-manifest"]] })
    .some((w) => /"j" is not a command/.test(w)), "a key the WAP does not answer must fail");
  const flagship = catalog.products.find((x) => x.id === "securacv-canary");
  assert.ok(a86Wrong(flagship, { ...flagship.monitor, cmds: [["h", "help"], ["m", "MQTT"]] })
    .some((w) => /"m" is not a command/.test(w)), "a key behind #if FEATURE_HA_MQTT must fail");

  // Each of the oracle's other checks fails on its own, one wrong thing at a time.
  const only = (product, monitor, re, why) => {
    const wrong = a86Wrong(product, monitor);
    assert.strictEqual(wrong.length, 1, `${why}: expected exactly one reason, got ${JSON.stringify(wrong)}`);
    assert.match(wrong[0], re, why);
  };
  const vision = catalog.products.find((x) => x.id === "securacv-canary-vision");
  const sense = catalog.products.find((x) => x.id === "securacv-canary-sense");
  only(flagship, { ...flagship.monitor, cmds: [["h", "help"], ["t", "time"]] }, /"t" labeled "time" — not its help line's words/,
    "a label that is not the firmware's own words for the key");
  only(sense, { ...sense.monitor, cmds: [["help", "help"], ["stream sometimes", "radar line"]] }, /no sometimes after stream/,
    "an argument execute_() does not read");
  only(vision, { ...vision.monitor, cmds: [["j", "install receipt"]] }, /no h, yet the box says h shows the menu/,
    "a key console offered without h");
  only(wap, { ...wap.monitor, kind: "words" }, /kind words, its firmware reads keys/, "the wrong kind alone");

  // The readers see code only where the compiler does.
  const keys = a86ReadKeys([
    "void handle_serial_commands() {",
    "  switch (c) {",
    "    case 'a': Serial.println(\"  a - all\"); break;",
    "    // case 'q': break;",
    "    /* case 'r': break; */",
    "    case 'f': Serial.println(\"case 's': } {\"); break;",
    "#if FEATURE_X",
    "    case 't': break;",
    "#endif",
    "  }",
    "}",
  ].join("\n"));
  assert.deepStrictEqual([...keys.commands].sort(), ["a", "f"], "commented, quoted and #if-gated keys are not answers");
  assert.deepStrictEqual([...keys.help.get("a")], ["all"]);
  const words = a86ReadWords([
    "void execute_() {",
    "  if (ieq_(cmd, \"help\")) {}",
    "  // else if (ieq_(cmd, \"zz\")) {}",
    "  /* else if (ieq_(cmd, \"yy\")) {} */",
    "  else if (ieq_(cmd, \"stream\")) { if (ieq_(tok[1], \"on\")) {} } // ieq_(tok[1], \"sometimes\")",
    "#if FEATURE_X",
    "  else if (ieq_(cmd, \"xx\")) {}",
    "#endif",
    "  Serial.println(\"ieq_(cmd, 'qq')\");",
    "}",
    "void print_help() { Serial.println(\"[tune] help - this list\"); }",
  ].join("\n"));
  assert.deepStrictEqual([...words.commands].sort(), ["help", "stream"], "commented and #if-gated words are not answers");
  assert.deepStrictEqual([...words.args], ["stream on"], "a commented argument is not one execute_() reads");
  // The wiring check: the committed Sense sketch feeds the console, a sketch
  // that reads Serial some other way (or only in a comment) does not.
  assert.ok(a86FeedsConsole(read(join(ROOT, A86_ROLE_FIRMWARE.sense.wiring))));
  assert.ok(!a86FeedsConsole("void loop() { while (Serial.available()) handle(Serial.read()); }"));
  assert.ok(!a86FeedsConsole("void loop() { // g_console.feed((char)Serial.read());\n}"));
});

test("A86: the Flasher's vocabulary rule is the browser's and the Lab's, product by product", async () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const desk = new Function(fnSource(appJs, "monitorVocabulary") + "\nreturn monitorVocabulary;")();
  for (const p of [...catalog.products, null, { id: "local" }, { monitor: { cmds: "x" } }]) {
    assert.deepStrictEqual(desk(catalog, p), core.monitorVocabulary(catalog, p),
      `the Flasher's monitorVocabulary disagrees with flash-core's for ${p && p.id}`);
  }
  assert.deepStrictEqual(core.monitorVocabulary(catalog, null), catalog.monitor_unknown);

  // The Flasher's chips, drawn by its own renderMonitorCmds for the product
  // on the bench: the WAP's, a display's (none), an unknown board's (none).
  const draw = (benchState) => {
    const nodes = {};
    const mk = (tag) => ({ tagName: tag, dataset: {}, classList: { on: new Set(), toggle(c, v) { v ? this.on.add(c) : this.on.delete(c); } },
      children: [], listeners: {}, appendChild(n) { this.children.push(n); },
      addEventListener(t, f) { this.listeners[t] = f; }, set innerHTML(_) { this.children = []; } });
    nodes["monitor-cmds"] = mk("div");
    nodes["monitor-cmd"] = mk("input");
    nodes["monitor-manifest"] = mk("button");
    const sent = [];
    const fn = new Function("$", "document", "state", "invoke", "setStatus",
      [fnSource(appJs, "monitorVocabulary"), fnSource(appJs, "monitorProductOnBench"),
       fnSource(appJs, "matchProjectToProduct"), fnSource(appJs, "productForProject"),
       fnSource(appJs, "requiresLiveReceipt"), fnSource(appJs, "renderMonitorCmds")].join("\n") +
      "\nreturn renderMonitorCmds;")((id) => nodes[id], { createElement: mk },
      { catalog, monitoring: true, ...benchState },
      async (cmd, args) => { sent.push([cmd, args]); }, () => {});
    fn();
    const chips = nodes["monitor-cmds"].children;
    chips.forEach((b) => b.listeners.click());
    return { chips: chips.map((b) => [b.dataset.cmd, b.textContent]), placeholder: nodes["monitor-cmd"].placeholder,
      receiptHidden: nodes["monitor-manifest"].classList.on.has("hidden"), sent };
  };
  const byId = (id) => catalog.products.find((p) => p.id === id);
  for (const id of ["securacv-canary", "securacv-canary-wap", "securacv-canary-vision", "securacv-canary-sense",
                    "securacv-canary-display-dash"]) {
    const m = catalogMonitor(id);
    // The chosen card, and the last host flash (which wins over the card).
    for (const st of [{ product: byId(id) }, { product: byId("securacv-canary-display-dash"), vision: { hostFlash: { product_id: id } } }]) {
      const out = draw(st);
      assert.deepStrictEqual(out.chips, m.cmds.map(([c, l]) => [c, `${l} (${c})`]), `the Flasher's chips for ${id}`);
      assert.strictEqual(out.placeholder, m.placeholder, `the Flasher's box for ${id}`);
      assert.deepStrictEqual(out.sent.map(([, a]) => a.command), m.cmds.map(([c]) => c + "\n"));
      assert.strictEqual(out.receiptHidden, byId(id).serial_receipt === false,
        `"Print receipt (j)" must show exactly where ${id}'s firmware answers j`);
    }
  }
  // Redrawn whenever the monitor starts, so the chips follow the product the
  // bench now holds (a flash, a different card) instead of the launch's.
  assert.match(fnSource(appJs, "startMonitor"), /\brenderMonitorCmds\(\);/,
    "the Flasher must redraw its command chips when its monitor starts");
  const local = draw({ product: byId("securacv-canary"), vision: { hostFlash: { product_id: "local-file" } } });
  assert.deepStrictEqual(local.chips, [], "a local file names no product: no chips");
  assert.strictEqual(local.placeholder, catalog.monitor_unknown.placeholder);
  assert.deepStrictEqual(draw({}).chips, [], "nothing chosen, nothing flashed: no chips");

  // Before a flash, the firmware the passport read off the board wins over
  // the card: "Ask it for its self-check" opens this monitor with no card
  // chosen, and a card is only what the user means to write next.
  const chipsOf = (id) => catalogMonitor(id).cmds.map(([c, l]) => [c, `${l} (${c})`]);
  const wapRead = { version: "2.0.0", projectName: "canary_wap", product: byId("securacv-canary-wap") };
  const residentCases = [
    ["a resident WAP, no card", { resident: { ...wapRead } }, "securacv-canary-wap"],
    ["a resident WAP, the Sense card chosen", { resident: { ...wapRead }, product: byId("securacv-canary-sense") }, "securacv-canary-wap"],
    ["a resident WAP, then a Sense flash", { resident: { ...wapRead }, product: byId("securacv-canary-sense"),
      vision: { hostFlash: { product_id: "securacv-canary-sense" } } }, "securacv-canary-sense"],
    ["a resident WAP this bench wrote over (no receipt), the Sense card", { resident: { ...wapRead, overwritten: true },
      product: byId("securacv-canary-sense") }, "securacv-canary-sense"],
    ["an unread resident, the Sense card", { resident: { unknown: true }, product: byId("securacv-canary-sense") }, "securacv-canary-sense"],
    ["a display read as canary_display, the flagship card", { resident: { projectName: "canary_display", product: byId("securacv-canary") },
      product: byId("securacv-canary") }, "securacv-canary-display-watch"],
    ["a resident Vision XIAO S3", { resident: { projectName: "canary-vision-xiao-s3" } }, "securacv-canary-vision-xiao-s3"],
  ];
  for (const [what, st, want] of residentCases) {
    const out = draw(st);
    assert.deepStrictEqual(out.chips, chipsOf(want), `${what}: the Flasher's chips must be ${want}'s`);
    assert.strictEqual(out.placeholder, catalogMonitor(want).placeholder, `${what}: the box`);
    assert.strictEqual(out.receiptHidden, byId(want).serial_receipt === false, `${what}: "Print receipt (j)"`);
  }
  for (const [what, st] of [["a blank board, the Sense card", { resident: { blank: true }, product: byId("securacv-canary-sense") }],
                            ["foreign firmware, the WAP card", { resident: { projectName: "esp32-hello" }, product: byId("securacv-canary-wap") }]]) {
    const out = draw(st);
    assert.deepStrictEqual(out.chips, [], `${what}: no chips — the board runs neither`);
    assert.strictEqual(out.placeholder, catalog.monitor_unknown.placeholder, `${what}: the box`);
  }

  // Every write the bench starts marks the passport's read overwritten, ends
  // what the About card says the fuse read left the board in, and draws the
  // card again — and the read-only health check does not.
  const drawn = [];
  const noted = (benchState) => new Function("state", "renderThisBoard",
    fnSource(appJs, "noteBoardWritten") + "\nreturn noteBoardWritten;")(benchState, () => drawn.push(benchState));
  const st = { resident: { ...wapRead }, efuseParked: { port: "/dev/ttyACM0", chip: "ESP32-S3" } };
  noted(st)();
  assert.strictEqual(st.resident.overwritten, true);
  assert.strictEqual(st.efuseParked, null, "a write resets the board, so the fuse read no longer says where it is");
  assert.deepStrictEqual(drawn, [st], "the About card is drawn again once the write starts");
  noted({ resident: null })(); // no read yet: nothing to mark, no throw
  for (const fn of ["onFlash", "onFlashLocalFile", "onRescueRestore", "onRescueErase"]) {
    assert.match(fnSource(appJs, fn), /\bnoteBoardWritten\(\);/, `${fn} writes to the board and must say so`);
  }
  assert.doesNotMatch(fnSource(appJs, "onHealthCheck"), /noteBoardWritten/, "a health check reads; it overwrites nothing");
  assert.doesNotMatch(fnSource(appJs, "onRescueBackup"), /noteBoardWritten/, "a backup reads; it overwrites nothing");

  // "Ask it for its self-check" asks for j only where the firmware answers it.
  const asks = (benchState) => {
    const timers = [], sent = [];
    const fn = new Function("state", "invoke", "startMonitor", "setTimeout", "$",
      [fnSource(appJs, "monitorProductOnBench"), fnSource(appJs, "matchProjectToProduct"),
       fnSource(appJs, "productForProject"), fnSource(appJs, "requiresLiveReceipt"),
       "async " + fnSource(appJs, "askSelfCheck")].join("\n") + "\nreturn askSelfCheck;")(
      { catalog, monitoring: true, busy: false, ...benchState },
      async (cmd, args) => { sent.push(args.command); }, async () => {},
      (f, ms) => { timers.push(ms); f(); }, () => ({ scrollIntoView() {} }));
    return fn().then(() => ({ timers, sent }));
  };
  return Promise.all([
    asks({ resident: { ...wapRead } }).then((r) => assert.deepStrictEqual(r.sent, [], "the WAP sketch has no j to ask")),
    asks({ resident: { projectName: "canary_display" } }).then((r) => assert.deepStrictEqual(r.sent, [], "a display reads no j")),
    asks({ resident: { projectName: "canary-sense" } }).then((r) => assert.deepStrictEqual(r.sent, [], "Sense's console has no j")),
    asks({ resident: { projectName: "canary" } }).then((r) => assert.deepStrictEqual(r.sent, ["j\n", "j\n"], "the flagship answers j")),
    asks({ resident: { projectName: "canary-vision" } }).then((r) => assert.deepStrictEqual(r.sent, ["j\n", "j\n"], "Vision answers j")),
  ]);
});

test("A86: the Lab's receipt card shows the flashed product's own commands, and none for a display", async () => {
  const receiptFor = (id) => ({ target: "esp32-host", product_id: id, version: "9.9.9", release_sha256: "a".repeat(64),
    installed_sha256: "b".repeat(64), bytes_written: 1, release_verification: "ed25519+sha256", channel: "stable",
    chip_write_verified: true, provisioned: false });
  const cases = [
    ["securacv-canary-wap", "ESP32-S3"],
    ["securacv-canary-vision", "ESP32-C3"],
    ["securacv-canary-sense", "ESP32-C6"],
    ["securacv-canary-display-dash", "ESP32-S3"],
  ];
  for (const [id, chip] of cases) {
    const m = catalogMonitor(id);
    const run = await runNativeBench({ flashAnswers: [receiptFor(id)], detectAnswer: { chip, flash_bytes: 16 * 1024 * 1024 } });
    try {
      await run.pick(id);
      assert.ok(run.text().includes("is on the board"), `${id}: the flash drew no receipt card`);
      const chips = [...run.mount.walk()].filter((n) => n.tagName === "button" && n.dataset.cmd);
      assert.deepStrictEqual(chips.map((b) => [b.dataset.cmd, b.textContent]), m.cmds.map(([c, l]) => [c, `${l} (${c})`]),
        `${id}: the receipt card's chips must be its firmware's`);
      const input = run.mount.find((n) => n.tagName === "input" && n.attrs["aria-label"] === "Serial command");
      assert.strictEqual(input.placeholder, m.placeholder, `${id}: the box must say what its firmware reads`);
      for (const b of chips) b.fire("click");
      await drain();
      assert.deepStrictEqual(run.calls.filter(([c]) => c === "serial_monitor_send").map(([, a]) => a.command),
        m.cmds.map(([c]) => c + "\n"), `${id}: each chip sends its command and a newline`);
    } finally { run.close(); }
  }
});

// The display-aware descriptor matcher both flashers resolve a board with.
// A display build whose name carries no flavor ("canary_display") matches
// only the flagship's "canary" stem in the plain substring matcher; it is a
// display, and a display reads no serial commands.
const A86_DESCRIPTORS = [
  // [projectName, product id it must resolve to (null: no catalog product)]
  ["canary_display", "securacv-canary-display-watch"],
  ["canary-display", "securacv-canary-display-watch"],
  ["CANARY_DISPLAY", "securacv-canary-display-watch"],
  ["canary-display-dash", "securacv-canary-display-dash"],
  ["canary_display_dash", "securacv-canary-display-dash"],
  ["canary-display-nightstand-c6", "securacv-canary-display-nightstand-c6"],
  ["canary_wap", "securacv-canary-wap"],
  ["canary-sense", "securacv-canary-sense"],
  ["canary-sense-wellbeing", "securacv-canary-sense-wellbeing"],
  ["canary_vision", "securacv-canary-vision"],
  ["canary-vision-xiao-s3", "securacv-canary-vision-xiao-s3"],
  ["canary", "securacv-canary"],
  ["canary-wroom", "securacv-canary-wroom"],
  ["something_else", null],
  ["", null],
];

test("A86: both flashers resolve a board's descriptor to its product, a flavorless display build to a display", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const desk = new Function([fnSource(appJs, "matchProjectToProduct"), fnSource(appJs, "productForProject")].join("\n") +
    "\nreturn productForProject;")();
  for (const [name, want] of A86_DESCRIPTORS) {
    const got = core.productForProject(catalog, name);
    assert.strictEqual(got ? got.id : null, want, `flash-core productForProject(${JSON.stringify(name)})`);
    const d = desk(catalog, name);
    assert.strictEqual(d ? d.id : null, want, `the Flasher's productForProject(${JSON.stringify(name)})`);
    if (core.looksLikeDisplayProject(name)) {
      assert.strictEqual(got.role, "display", `${name} is a display build, so it resolves to a display`);
    }
  }
  // The plain matcher is what was wrong: it names the flagship.
  assert.strictEqual(core.matchProjectToProduct(catalog, "canary_display").id, "securacv-canary");
  // A catalog with no display entries still never names the flagship.
  const bare = { ...catalog, displays: [] };
  assert.strictEqual(core.productForProject(bare, "canary_display").role, "display");
  assert.strictEqual(desk(bare, "canary_display").role, "display");
  const noDisplays = { ...catalog, displays: [], products: catalog.products.filter((p) => p.role !== "display") };
  assert.strictEqual(core.productForProject(noDisplays, "canary_display"), null);
  assert.strictEqual(desk(noDisplays, "canary_display"), null);
});

test("A86: the browser flasher's monitor offers the flashed or the identified board's own commands", async () => {
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  // flash.js's own boardProduct and monitorProduct, over a stand-in state.
  const choose = (current, opts = {}) => new Function("state", "core",
    [fnSource(flashJs, "boardProduct"), fnSource(flashJs, "monitorProduct")].join("\n") + "\nreturn monitorProduct;")(
    { catalog, current }, core)(opts);
  const vocabFor = (current, opts) => core.monitorVocabulary(catalog, choose(current, opts));
  for (const [name, want] of A86_DESCRIPTORS) {
    const v = vocabFor({ version: "1.0.0", projectName: name });
    assert.deepStrictEqual(v, want ? catalogMonitor(want) : catalog.monitor_unknown,
      `the browser monitor for a board whose descriptor reads ${JSON.stringify(name)}`);
  }
  for (const name of ["canary_display", "canary-display", "canary-display-dash"]) {
    const v = vocabFor({ projectName: name });
    assert.strictEqual(v.kind, "none", `${name}: a display's monitor offers no commands`);
    assert.deepStrictEqual(v.cmds, []);
    assert.match(v.placeholder, /reads no serial commands/);
  }
  assert.deepStrictEqual(vocabFor({ unknown: true }), catalog.monitor_unknown, "an unread descriptor names no product");
  assert.deepStrictEqual(vocabFor(null), catalog.monitor_unknown, "no board read at all");
  // After a flash, the flashed product — over whatever the board ran before —
  // and for a backup or a local file an explicit null: no chips.
  const wap = catalog.products.find((p) => p.id === "securacv-canary-wap");
  assert.deepStrictEqual(vocabFor({ projectName: "canary" }, { product: wap }), catalogMonitor("securacv-canary-wap"));
  assert.deepStrictEqual(vocabFor({ projectName: "canary" }, { product: null }), catalog.monitor_unknown);
  assert.match(flashJs, /const flashed = \(opts\.product && !opts\.isBackup && !opts\.isLocal\) \? opts\.product : null;\s*return openMonitor\(\{[^}]*hatchProduct: flashed, product: flashed \}\)/,
    "the post-flash monitor must be told which product it is watching");
  // phaseMonitor draws its chips and box from that choice, through the shared rule.
  const phase = fnSource(flashJs, "phaseMonitor");
  assert.match(phase, /const monProduct = monitorProduct\(opts\);\s*const vocab = core\.monitorVocabulary\(state\.catalog, monProduct\);/);
  assert.match(phase, /for \(const \[cmd, label\] of vocab\.cmds\)/);
  assert.match(phase, /send\(cmd \+ "\\n"\)/, "a chip sends its command and the newline a word console needs");
  assert.match(phase, /input\.placeholder = vocab\.placeholder;/);
  // The role line names the same product the monitor does.
  assert.match(fnSource(flashJs, "renderRoleLine"), /const prod = core\.productForProject\(state\.catalog, cur\.projectName\);/);
  // The identity ask (j) goes only to a firmware that answers it.
  assert.match(phase, /if \(opts\.proveIdentity && !mon\.askedManifest && core\.requiresLiveReceipt\(monProduct\)\) \{/,
    "the browser must not ask a WAP, a Sense or a display for a j it does not have");
});

test("A86: the browser monitor's status lines name the firmware's own menu command, or none", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const flashJs = read(join(CANARY, "assets/flash.js"));
  for (const p of catalog.products) {
    const v = p.monitor;
    const connected = core.monitorStatusText(v, "connected", 115200);
    const quiet = core.monitorStatusText(v, "quiet", 115200);
    assert.match(connected, /^Connected at 115200 — the board is talking\./);
    assert.match(quiet, /built without a serial console/);
    if (v.kind === "keys") {
      assert.match(connected, /Press a command, or h for its menu\.$/, `${p.id}: a key console's menu is h`);
      assert.match(quiet, /send h for its menu/);
    } else if (v.kind === "words") {
      assert.match(connected, /Press a command, or help for its list\.$/, `${p.id}: the tuning console answers help, not h`);
      assert.match(quiet, /send help for its list/);
    } else {
      assert.strictEqual(connected, "Connected at 115200 — the board is talking.", `${p.id}: a display reads no commands`);
      assert.doesNotMatch(quiet, /speak when asked|\bsend\b/, `${p.id}: nothing to send a display`);
    }
    // Whatever the line names is one of the product's own chips.
    const named = /or (\S+) for its/.exec(connected);
    if (named) assert.ok(v.cmds.some(([c]) => c === named[1]), `${p.id}: "${named[1]}" is not one of its chips`);
  }
  for (const v of [catalog.monitor_unknown, null]) {
    assert.strictEqual(core.monitorStatusText(v, "connected", 9600), "Connected at 9600 — the board is talking.");
    assert.doesNotMatch(core.monitorStatusText(v, "quiet", 9600), /speak when asked/);
  }
  // A key vocabulary without h names no menu key (the line names chips only).
  assert.strictEqual(core.monitorStatusText({ kind: "keys", cmds: [["j", "install receipt"]] }, "connected", 1),
    "Connected at 1 — the board is talking.");
  // The browser monitor shows these lines, and no hard-coded h hint remains.
  const phase = fnSource(flashJs, "phaseMonitor");
  assert.match(phase, /setStatus\(core\.monitorStatusText\(vocab, "connected", baud\)\);/);
  assert.match(phase, /setStatus\(core\.monitorStatusText\(vocab, "quiet", baud\)\);/);
  assert.doesNotMatch(flashJs, /h for its menu/, "flash.js must not tell every firmware to press h");
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

  // A canary-wap's boot receipt spells pubkey_fp in capitals (canary_wap.ino's
  // hex_to_str), and that is what each flasher hands the mint; the phone
  // derives the same fingerprint in lowercase from the key it pinned. Hex case
  // names no different key, so all three must hatch one bird from it.
  const wapFp = "7916CA487912FA1B";
  const phone = deriveCertificate(hatch, { fingerprint: wapFp.toLowerCase(), product: { name: "Canary WAP" } });
  for (const [who, cert] of [
    ["the Mac Flasher", native(hatch, wapFp, { name: "Canary WAP" }, "")],
    ["the Lab", deriveCertificate(hatch, { fingerprint: wapFp, product: { name: "Canary WAP" } })],
  ]) {
    assert.equal(cert.name, phone.name, `${who} names a canary-wap's capital fp a different bird`);
    assert.equal(cert.lineage, phone.lineage, `${who} lineage drifts on a capital fp`);
    assert.equal(cert.motto, phone.motto, `${who} motto drifts on a capital fp`);
    assert.equal(cert.ringId, phone.ringId, `${who} ring id drifts on a capital fp`);
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
  // …and `boot` must be the receipt, bound in the function that reads it. A
  // bare `boot` there resolved to the app's own boot() function (truthy, no
  // .manifest), so the regexes above passed while every certificate rolled.
  const show = /function showHatchCard\(product\) \{([\s\S]*?)\n\}/.exec(desktop);
  assert.ok(show, "couldn't find showHatchCard in desktop/src/app.js");
  assert.match(show[1], /const boot = state\.vision\.hostBoot;[\s\S]*?const bootFp = \(boot && boot\.manifest/,
    "showHatchCard must read the boot receipt (state.vision.hostBoot) before taking its fingerprint");

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

// ── Minimal mode: one quiet dress, both flashers ──────────────────────────
//
// The verbose flasher is the front door; minimal mode is the regular's
// entrance. It is a user-facing feature, so the two-flashers rule applies in
// full: if either frontend loses its toggle, its persistence, or the promise
// that quiet is opted into (never the default), half the users keep — or
// lose — a mode the other half has. These greps hold both sides together.
test("minimal mode exists on both flashers, opt-in, persisted, reversible", () => {
  const browser = read(join(CANARY, "assets", "flash.js"));
  const browserMod = read(join(CANARY, "assets", "minimal.js"));
  const browserCss = read(join(CANARY, "assets", "flash.css"));
  const desktop = read(join(ROOT, "desktop", "src", "app.js"));
  const desktopHtml = read(join(ROOT, "desktop", "src", "index.html"));
  const desktopCss = read(join(ROOT, "desktop", "src", "styles.css"));

  // Browser: a persisted preference module, mounted as a toggle, driving a
  // root class that flash.css folds prose under — plus the per-phase bridge
  // back to the full story.
  assert.match(browserMod, /nursery\.minimal/,
    "minimal.js must persist the choice under the nursery.* localStorage family");
  assert.match(browserMod, /=== "on"/,
    "minimal.js must treat anything but an explicit 'on' as off — opt-in, never sprung");
  assert.match(browser, /minimalToggle\(/,
    "flash.js must mount the minimal toggle");
  assert.match(browser, /flash-minimal/,
    "flash.js must dress #flash with the flash-minimal root class");
  assert.match(browser, /minimalMoreBar|flash-minimal-open/,
    "flash.js must offer the per-phase way back to the full story");
  assert.match(browserCss, /\.flash-minimal:not\(\.flash-minimal-open\)/,
    "flash.css must scope the folding so the reveal un-folds everything");

  // Desktop: the same mode as a density attribute — persisted in prefs,
  // toggled from the rail, folded by styles.css.
  assert.match(desktop, /prefs\.minimal/,
    "desktop app.js must persist the choice in prefs");
  assert.match(desktop, /data-density/,
    "desktop app.js must carry the mode as a data-density attribute");
  assert.match(desktopHtml, /id="density-toggle"/,
    "desktop index.html must keep the rail toggle");
  assert.match(desktopCss, /html\[data-density="minimal"\]/,
    "styles.css must fold under the density attribute");

  // The desktop's minimal console collapses espflash's progress-bar redraw
  // frames into one live line — the single biggest verbosity win, and the
  // easiest one to lose in a refactor of the log listeners.
  assert.match(desktop, /function appendFlashLog\b/,
    "desktop app.js must route flash logs through the collapsing appender");
  assert.match(desktop, /looksLikeProgressFrame/,
    "desktop app.js must keep the progress-frame detector");
});

// What minimal mode may never fold: the destructive-choice affordances. The
// first-contact wipe label was once folded into a disclosure and cost a
// review catch (see index.html's comment); minimal mode must not repeat that
// mistake by CSS, and the browser's forced-erase explainer stays visible the
// same way.
test("minimal mode never hides a destructive choice", () => {
  const browserCss = read(join(CANARY, "assets", "flash.css"));
  const desktopCss = read(join(ROOT, "desktop", "src", "styles.css"));
  assert.match(browserCss, /\.flash-reassure:not\(\.flash-forced-erase\)/,
    "flash.css must exempt the forced-erase explainer from the fold");
  assert.doesNotMatch(desktopCss, /data-density="minimal"\][^{}]*\.coldstart-ask/,
    "styles.css must never fold the first-contact wipe label under minimal mode");
  // The hub's write-target rows reuse .p-tag for the disk path and capacity —
  // the facts that tell two identical card readers apart before a destructive
  // write. The tagline fold must stay scoped to the Canary firmware picker.
  assert.match(desktopCss, /#product-list \.p-tag/,
    "styles.css must scope the tagline fold to #product-list, never app-wide");
  assert.doesNotMatch(desktopCss, /data-density="minimal"\][^{}]*(?<!#product-list )\.p-tag\b/,
    "no minimal-mode rule may fold .p-tag outside the Canary picker");
});

// And the disclosure it may never fold: the done card's safety-copy line is
// the ONLY notice that a credential-bearing file — the board's identity key
// and saved WiFi inside — just landed in the downloads folder without a
// click. (Codex catch on #1575, where a parallel fold list hid it.) The fold
// list above targets .flash-hello and .flash-voice fineprints by scope on
// purpose; this gate keeps the done card out of every present and future
// minimal-mode rule, so the warning always rides the file it describes.
test("minimal mode never folds the backup-secret notice", () => {
  const browser = read(join(CANARY, "assets", "flash.js"));
  const browserCss = read(join(CANARY, "assets", "flash.css"));
  assert.match(browser, /Treat it like a spare house key/,
    "the backup-secret wording moved — re-point this gate at its new home");
  assert.doesNotMatch(browserCss, /flash-minimal[^{}]*\.flash-done/,
    "a minimal-mode rule reaches into the done card — the backup-secret " +
    "notice lives there and must stay visible in every mode");
});

// ── Liveness: no wait may look stuck, on either flasher ─────────────────────
//
// The 2026-08 wait-state audit: a display factory image can erase for a
// minute, download for minutes, and verify for half a minute — and every one
// of those used to sit visually frozen on at least one frontend. The rule
// this gate pins: every long operation shows something that MOVES (a real
// fraction, a sweeping bar, or a ticking elapsed clock) plus honest words
// about what the silence means. Both flashers, per rule 7.
test("every long wait shows liveness on both flashers", () => {
  const browser = read(join(CANARY, "assets", "flash.js"));
  const browserCss = read(join(CANARY, "assets", "flash.css"));
  const browserCore = read(join(CANARY, "assets", "flash-core.js"));
  const appJs = read(join(ROOT, "desktop", "src", "app.js"));
  const html = read(join(ROOT, "desktop", "src", "index.html"));
  const css = read(join(ROOT, "desktop", "src", "styles.css"));
  const libRsSrc = nativeRs;

  // 1. The elapsed clock: both frontends tick seconds through every long
  //    operation — the difference between "working" and "hung".
  assert.match(browser, /flash-elapsed/,
    "browser progress cards lost their elapsed clock");
  assert.match(appJs, /still working/,
    "desktop op strip lost its quiet-stretch watchdog");
  assert.match(browser, /still working/,
    "browser progress cards lost their quiet-stretch watchdog");

  // 2. Indeterminate motion for waits with no honest number (full-chip
  //    erase, the chip's own MD5): a sweep, never a frozen bar.
  assert.match(browserCss, /flash-bar-indet/,
    "browser lost the indeterminate bar sweep");
  assert.match(browser, /pulse\(/,
    "browser progressCard lost pulse() — erase/verify would freeze the bar");
  assert.match(css, /\.op-progress \.bar-fill\.indet/,
    "desktop op strip lost its indeterminate sweep");

  // 3. The erase is narrated with a size-shaped estimate on the browser
  //    (eraseEstimateText, unit-tested in flash.test.js) and a stage line on
  //    the desktop — the chip reports nothing while erasing, so the words
  //    are the only honest signal.
  assert.match(browserCore, /export function eraseEstimateText/,
    "flash-core lost the erase estimate");
  assert.match(browser, /eraseEstimateText/,
    "the browser erase step no longer states its expected duration");
  assert.match(appJs, /stays silent until it finishes/,
    "the desktop erase stage no longer says what the silence means");

  // 4. Downloads stream with progress on both sides. The browser reads the
  //    response body; the desktop's Rust flash command emits structured
  //    flash:progress events from a chunked download.
  assert.match(browser, /resp\.body && resp\.body\.getReader/,
    "the browser image download went back to a silent arrayBuffer()");
  assert.match(libRsSrc, /flash:progress/,
    "the native flash path (flash-engine flash.rs) no longer emits download progress events");
  assert.match(libRsSrc, /resp\s*\.chunk\(\)/,
    "the native download (flash-engine net.rs) went back to a silent bytes() buffer");
  assert.match(appJs, /flash:progress/,
    "desktop never listens for the download progress it is sent");

  // 5. The desktop finally has a real bar for the Canary flow — driven by
  //    espflash's own frames — and the connect-time passport read narrates.
  assert.match(html, /id="flash-progress-wrap"/,
    "desktop lost the Canary-flow progress strip");
  assert.match(appJs, /function progressFromFrame/,
    "desktop lost the espflash frame parser — the bar has no data source");
  assert.match(libRsSrc, /passport:log/,
    "board_passport went silent again — up to 25 s with zero events");
  assert.match(appJs, /passport:log/,
    "desktop never shows the passport narration it is sent");

  // 6. The browser's write bar reaches 100 %: one tracker per file, in the
  //    callback's own (compressed) units — the old single tracker was seeded
  //    with the uncompressed length and topped out at the compression ratio.
  assert.match(browser, /fileEtas\[i\]/,
    "the browser write bar went back to the mis-scaled single tracker");
  // …and the chip's MD5 pause is named instead of mysterious.
  assert.match(browser, /recomputing its checksum \(MD5\)/,
    "the browser no longer names the chip's verify pause");
});

// The desktop's frame parser against realistic espflash/indicatif shapes —
// same grab-the-function pattern as the classifier tests above. A parser
// that can't read a frame must return null (the strip degrades to sweep +
// clock), never a wrong number.
test("progressFromFrame reads espflash's bars and refuses everything else", () => {
  const appJs = read(join(ROOT, "desktop", "src", "app.js"));
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
  const fn = new Function(
    grabFn("looksLikeProgressFrame") + "\n" + grabFn("progressFromFrame") +
    "\nreturn progressFromFrame;")();

  // Percent frames, wherever the percent sits.
  assert.strictEqual(fn("[00:00:12] [========>-------] 45%"), 0.45);
  assert.strictEqual(fn("Writing [#####>..........] 12% (0x10000)"), 0.12);
  // indicatif's precise-percent template: the whole decimal, never the
  // digits after the dot (review catch — '12.34%' must not read as 34 %).
  assert.strictEqual(fn("[=>--------------] 12.34%"), 0.1234);
  // n/m beside a bracketed bar.
  assert.strictEqual(fn("[=====>          ] 512/1024"), 0.5);
  // Not frames: narration, plain numbers, a bare fraction with no bar.
  assert.strictEqual(fn("→ downloading https://example"), null);
  assert.strictEqual(fn("✓ chip write confirmed"), null);
  assert.strictEqual(fn("Flash size: 8MB"), null);
  // Degenerate frames must degrade to null, never a wrong number.
  assert.strictEqual(fn("[=========] 1024/0"), null);
});

// The two flashers share no UI code (CLAUDE.md, "two flashers, two
// frontends"), so a user-facing diagnostic added to one must be added to the
// other or half the users keep the vague version. classifyFlashError is the
// diagnostic table; this pins that every integrity-refusal keyword the browser
// recognizes — including the Ed25519 "signed by" wording — the desktop
// recognizes too, and that the kinds and titles agree.
test("classifyFlashError: the desktop recognizes every integrity keyword the browser does, with the same kinds and titles", () => {
  const browser = read(join(CANARY, "assets/flash-core.js"));
  const desktop = read(join(CANARY, "..", "desktop/src/app.js"));
  const table = (src) => {
    const out = {};
    const re = /has\(([^)]*)\)[^\n]*\n\s*return \{ kind: "([a-z-]+)", title: "([^"]+)"/g;
    for (const m of src.matchAll(re)) {
      const words = [...m[1].matchAll(/"([^"]+)"/g)].map((w) => w[1]);
      out[m[2]] = { title: m[3], words: new Set([...(out[m[2]]?.words || []), ...words]) };
    }
    return out;
  };
  const b = table(browser), d = table(desktop);
  assert.ok(b.integrity && d.integrity, "both flashers classify an integrity failure");
  for (const w of b.integrity.words) {
    assert.ok(d.integrity.words.has(w), `desktop classifyFlashError lacks the browser's integrity keyword "${w}"`);
  }
  for (const kind of Object.keys(b)) {
    assert.ok(d[kind], `desktop classifyFlashError has no "${kind}" kind`);
    assert.equal(d[kind].title, b[kind].title, `title for "${kind}" differs between the flashers`);
  }
});

// ── parity wave 5: the connect failure, the module proof, and the monitor ────
//
// The 2026-09 re-scout of rule 7. The waves above are thorough, which is why
// the surviving gaps were concentrated in the three places they did not cover:
// the connect-failure path, the serial monitor, and the Vision-module proof —
// plus two that ran the other way, browser→desktop. Each of these was a
// user-facing diagnostic one frontend had and the other did not.

test("wave 5: a connect failure is classified before download mode is blamed, on both flashers", async () => {
  // The desktop's identify() catch carried a comment CLAIMING it made the same
  // call as the browser's connectFailed, next to code that never made it: the
  // only escape from "put it in download mode" was a regex on two Linux hint
  // strings, and lib.rs gates those to Linux. So a busy port or a denied
  // permission on macOS/Windows — where espflash says "Resource busy" /
  // "Access is denied" — was coached with the one fix that cannot help.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const browser = read(join(CANARY, "assets/flash.js"));

  assert.match(browser, /function connectFailed[\s\S]{0,200}core\.classifyFlashError\(e\)/,
    "the browser no longer classifies the connect failure");
  // The desktop must make the call in the same place — inside identify()'s
  // catch, not merely somewhere in the file (classifyFlashError has always
  // existed there; it was the CALL SITE that was missing).
  const identify = /async function identify\(([\s\S]*?)\n}\n/.exec(appJs);
  assert.ok(identify, "identify() vanished from desktop/src/app.js");
  assert.match(identify[1], /classifyFlashError\(e\)/,
    "desktop identify() no longer classifies the connect failure — a busy port or a " +
    "denied permission would be coached as 'put it in download mode' again");
  // …and a named cause must suppress the download-mode coaching, exactly as
  // the browser only shows downloadModeSteps() for not-in-download/unknown.
  assert.match(identify[1], /c\.kind !== "unknown" && c\.kind !== "not-in-download"/,
    "the desktop must keep the download-mode advice for exactly the two kinds it fixes");
  assert.match(identify[1], /\$\("download-mode"\)\.classList\.toggle\("hidden", osLevel \|\| named\)/,
    "a named cause must hide the download-mode gesture — it is the wrong fix for it");
  assert.match(browser, /v\.kind === "not-in-download" \|\| v\.kind === "unknown"[\s\S]{0,400}downloadModeSteps\(\)/,
    "the browser no longer gates its download-mode steps on the kind");

  // The proof that this actually helps: run the desktop's OWN classifier over
  // the error detect_chip really produces on macOS/Windows (its generic
  // prefix plus espflash's tail) and require a named, non-download verdict.
  // Asserting the call site alone would not catch a classifier that answered
  // `unknown` for every one of these.
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
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  // lib.rs:detect_chip's own wording, which every one of these rides in on.
  const prefix = "couldn't read the chip (espflash exit 1). Put the board in download " +
    "mode (hold BOOT, tap RESET, release BOOT) and try again.\n\nespflash said:\n";
  // The three failures the Linux hints in port_hint.rs can never cover,
  // because lib.rs gates them to `cfg!(target_os = "linux")`. Each one has a
  // real fix that is not the BOOT/RESET ritual — that is the whole point.
  for (const tail of [
    "Error: Failed to open serial port\nCaused by: Device or resource busy",
    "Error: Failed to open serial port\nCaused by: Access is denied.",
    "Error: Permission denied (os error 13)",
  ]) {
    const got = classify(new Error(prefix + tail)).kind;
    assert.ok(got !== "unknown" && got !== "not-in-download",
      `"${tail.split("\n").pop()}" still classifies as ${got} — it would be coached ` +
      "as 'put it in download mode', which cannot fix a held port or a denied open");
    // …and both flashers must reach the SAME verdict for it, or the two
    // frontends would name different causes for one failure.
    assert.strictEqual(got, core.classifyFlashError(new Error(prefix + tail)).kind,
      `the flashers disagree about "${tail.split("\n").pop()}"`);
  }
  // The generic prefix alone must stay neutral: if it classified as anything,
  // every failure would inherit that verdict instead of espflash's own words.
  assert.strictEqual(classify(new Error(prefix)).kind, "unknown",
    "detect_chip's generic prefix must not classify on its own");
});

test("wave 5: the Vision-module proof is patient, and says the burn survived, on both flashers", () => {
  // One VER? probe made a module that was still REBOOTING FROM ITS OWN BURN
  // look dead, and the desktop then reported a fix-less error for a burn that
  // had completed. The browser has always retried, pulsed the reset line, and
  // retried again. Same ladder, same words, both engines.
  const we2FlashJs = read(join(CANARY, "assets/we2-flash.js"));

  assert.match(we2FlashJs, /async function wakeModule/,
    "the browser engine lost wakeModule — the patient handshake");
  assert.match(we2Rs, /fn wake_module/,
    "desktop we2.rs lost wake_module — one VER? probe is not a handshake");
  // The ladder's shape: probes, then a reset, then more probes. Asserted on
  // BOTH engines so neither can quietly collapse back to a single try.
  assert.match(we2FlashJs, /wakeModule[\s\S]{0,600}setRTS\(false\)[\s\S]{0,120}setRTS\(true\)/,
    "the browser wake no longer pulses RTS between its probe rounds");
  assert.match(we2Rs, /fn wake_module[\s\S]{0,900}hard_reset\(\)[\s\S]{0,600}from_millis\(1500\)/,
    "the desktop wake no longer resets the module between its probe rounds");
  for (const [label, src, re] of [
    ["browser", we2FlashJs, /for \(let i = 0; i < 3; i\+\+\)/g],
    ["desktop", we2Rs, /for _ in 0\.\.3/g],
  ]) {
    assert.ok((src.match(re) || []).length >= 2,
      `${label} wake must probe in TWO rounds — before the reset and after it`);
  }

  // The words, when it still doesn't answer. "The burn itself completed" is
  // the load-bearing half: without it the message reads as "your model didn't
  // get written", which is the one thing it does not mean.
  for (const [label, src] of [["browser we2-flash.js", we2FlashJs], ["desktop we2.rs", we2Rs]]) {
    assert.ok(src.includes("No AT answer after reboot, even after an automatic reset"),
      `${label} no longer names the automatic reset it already tried`);
    assert.ok(src.includes("The burn itself completed"),
      `${label} no longer says the burn survived — the message reads as a failed write`);
    // And the inference failure's remedy, which the desktop used to omit
    // entirely (a bare statement of the failure with no next step).
    assert.ok(src.includes("non-SSCMA firmware; see the device guide"),
      `${label} lost the power-cycle / non-SSCMA remedy for a refused inference`);
  }
  // The desktop must not regrow a remedy-less INVOKE error beside the good one.
  assert.ok(!/"SSCMA answered AT, but the pinned model did not complete an inference"/.test(we2Rs),
    "desktop we2.rs still returns the fix-less inference error");
});

test("wave 5: an unreachable manifest is not a missing release, on both flashers", () => {
  // A fetch that never got an ANSWER proves nothing about whether the release
  // exists. The desktop has drawn this line since it grew the pinned-tag
  // message; the browser lumped an offline user in with "no release has been
  // cut" and sent them to watch a repository instead of their Wi-Fi.
  const browser = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));

  for (const [label, src] of [["browser flash.js", browser], ["desktop app.js", appJs]]) {
    assert.ok(src.includes("\\bHTTP (\\d{3})\\b"),
      `${label} no longer distinguishes an HTTP answer from a transport failure`);
  }
  // The browser must CARRY the status out of the catch, or the render has
  // nothing to branch on.
  assert.match(browser, /__missing: true, why, httpStatus:/,
    "the browser's manifest catch no longer keeps the HTTP status");
  // …and must only make the "no release" claim when the server answered.
  assert.match(browser, /const answered = !!m\.httpStatus/,
    "the browser banner no longer asks whether anyone answered");
  assert.match(browser, /!answered\s*\n?\s*\?[\s\S]{0,240}back online/,
    "the browser no longer offers the offline reading of a failed manifest fetch");
  // The dev channel is the case with no pinned fw-v* tag to name, so it was
  // the one where the message had no address in it at all.
  assert.match(browser, /state\.devChannel \? "fw-dev-latest" : null/,
    "the browser no longer names the rolling dev pointer when it can't be derived");
});

test("wave 5: the serial monitor has a speed control and a wrong-baud self-heal on both flashers", () => {
  // A board on a non-catalog console speed showed the desktop user mojibake
  // with nothing to turn: no baud control anywhere in the monitor, and no
  // detection that the bytes weren't text. The browser has had both.
  const browser = read(join(CANARY, "assets/flash.js"));
  const browserCore = read(join(CANARY, "assets/flash-core.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  // The control.
  assert.match(browser, /flash-mon-baud/, "the browser monitor lost its baud selector");
  assert.match(html, /id="monitor-baud"/, "the desktop monitor has no baud control");
  assert.match(appJs, /invoke\("start_serial_monitor"[\s\S]{0,400}\n\s*baud,/,
    "the desktop monitor no longer opens at the chosen speed");

  // The ladder itself — same rungs, same order, both frontends. A desktop
  // copy that drifts would walk a different list than the browser's.
  const rungs = (src, re) => {
    const m = re.exec(src);
    assert.ok(m, "couldn't parse CONSOLE_BAUDS");
    return m[1].split(",").map((s) => Number(s.trim())).filter(Number.isFinite);
  };
  assert.deepStrictEqual(
    rungs(appJs, /const CONSOLE_BAUDS = \[([^\]]*)\]/),
    rungs(browserCore, /export const CONSOLE_BAUDS = \[([^\]]*)\]/),
    "the desktop's console-baud ladder drifted from the browser's");

  // The judge, and the proof it agrees with the browser's on real text. A
  // ported heuristic that answers differently is worse than none: it would
  // hop the speed on perfectly good output, or sit in soup.
  assert.match(browserCore, /export function looksLikeGarbage/, "browser lost looksLikeGarbage");
  assert.match(appJs, /function looksLikeGarbage/, "desktop lost looksLikeGarbage");
  const grab = (src, name) => {
    const i = src.indexOf("function " + name);
    let d = 0;
    const b = src.indexOf("{", src.indexOf(")", i));
    for (let k = b; k < src.length; k++) {
      if (src[k] === "{") d++;
      else if (src[k] === "}") { d--; if (!d) return src.slice(i, k + 1); }
    }
  };
  const mine = new Function(grab(appJs, "looksLikeGarbage") + "\nreturn looksLikeGarbage;")();
  const theirs = new Function(
    grab(browserCore, "looksLikeGarbage") + "\nreturn looksLikeGarbage;")();
  const soup = "��".repeat(30);
  const bootLog =
    "I (31) boot: ESP-IDF v5.1 2nd stage bootloader\nI (31) boot: compile time 12:00:00\n" +
    "I (32) boot: chip revision: v0.2\nI (33) boot.esp32s3: Boot SPI Speed : 80MHz\n";
  const helpMenu =
    "┌─ SecuraCV Canary ─┐\n│ h  help           │\n" +
    "│ j  self-manifest  │\n└──────────┘\n" +
    "the box-drawing help menu must never read as garbage at the right speed.\n";
  for (const s of ["", "short", bootLog, soup, helpMenu]) {
    assert.strictEqual(mine(s), theirs(s),
      `the two garbage judges disagree on: ${JSON.stringify(s.slice(0, 40))}`);
  }
  // Real firmware text must pass and real soup must fail, or the agreement
  // above would be satisfied by two functions that both always answer false.
  assert.strictEqual(mine(bootLog), false, "a clean boot log must not read as garbage");
  assert.strictEqual(mine(helpMenu), false, "the box-drawing help menu must not read as garbage");
  assert.strictEqual(mine(soup), true, "wrong-baud soup must read as garbage");

  // The two status lines, word for word, so the hop narrates the same way.
  for (const [label, src] of [["browser", browser], ["desktop", appJs]]) {
    assert.ok(/didn.t look like text at/.test(src),
      `${label} monitor no longer says why it changed speed`);
    assert.ok(src.includes("None of the usual speeds decoded as clean text"),
      `${label} monitor no longer says when it has run out of speeds`);
  }
  // A speed the USER chose is never walked away from, on either side.
  for (const [label, src] of [["browser", browser], ["desktop", appJs]]) {
    assert.ok(/manualBaud|manual: true/.test(src),
      `${label} monitor no longer respects a hand-picked speed`);
  }
});

test("wave 5: the local-file path names the connected chip and the guard it skips, on both flashers", () => {
  // A local .bin has no catalog product, so the chip GUARD can't run — only
  // the factory-shape gate does. The desktop has named the connected chip and
  // said so since it grew the local path; the browser's copy mentioned
  // neither, so a .bin built for a different ESP32 variant was written with
  // no warning at all.
  const browser = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  for (const [label, src] of [["browser flash.js", browser], ["desktop app.js", appJs]]) {
    // Tolerant of the line break each frontend happens to wrap at — the words
    // are the contract, not the layout.
    assert.ok(/skips the[\s\S]{0,24}catalog.s chip guard/.test(src),
      `${label} no longer says a local file skips the catalog's chip guard`);
    assert.ok(/there.s no product to compare against/.test(src),
      `${label} no longer says WHY the guard can't run`);
    assert.ok(/sure this build targets that chip/.test(src),
      `${label} no longer tells the user what to check instead`);
  }
  // …and the browser must name the chip it actually read, not just the risk.
  assert.match(browser, /Connected: \$\{state\.chipDesc \|\| state\.chip/,
    "the browser local-file note no longer names the connected chip");
});

test("wave 5: a failed install offers the same next steps on both flashers", () => {
  // The browser has always escalated inside the error card: Try again, then a
  // clean install (full erase), then a diagnostic report. The desktop named
  // the failure and stopped — its full-erase path existed only as a pre-flash
  // checkbox a stuck user had to know to go find.
  const browser = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  assert.ok(browser.includes("Clean install (full erase)"),
    "the browser error card lost its clean-install escalation");
  assert.ok(appJs.includes("Clean install (full erase)"),
    "the desktop failure card lost its clean-install escalation");
  assert.match(html, /id="flash-actions"/,
    "desktop/src/index.html has nowhere to put the failure's next steps");
  assert.match(appJs, /function renderFlashFailureActions/,
    "desktop lost the failure-actions renderer");
  assert.match(appJs, /renderFlashFailureActions\(\{ retry: onFlash, kind: c\.kind \}\)/,
    "the desktop flash catch no longer offers any next step");

  // The escalation must actually ARM the erase, not merely say it will —
  // a button that re-runs the same install is worse than no button.
  assert.match(appJs, /Clean install \(full erase\)[\s\S]{0,500}first\.checked = true/,
    "the desktop clean-install button doesn't tick the first-contact erase");
  // …and it must not be offered when the erase was already on, where it
  // would promise a different attempt and deliver the same one.
  assert.match(appJs, /if \(!refused && first && !first\.checked\)/,
    "the desktop must not offer a clean install to someone already doing one");
  // A previous failure's buttons must not outlive the run that produced them.
  assert.match(appJs, /function resetOutcome[\s\S]{0,300}renderFlashFailureActions\(null\)/,
    "a new flash must clear the previous failure's next steps");
  // The report, where the stuck user is — both frontends.
  assert.match(browser, /diagnosticReportButton\(\(\) => \(\{ stage: "install"/,
    "the browser error card lost its diagnostic report button");
  assert.match(appJs, /copyDiagnosticReport\("install"\)/,
    "the desktop failure card lost its diagnostic report button");
});

test("wave 5: customs says what the board arrived running, and whether it was cold, on both flashers", () => {
  // The browser runs six read-only intake findings; the desktop implemented
  // two, so an unrecognized marketplace image's project name printed as
  // neutral fact and the cold-start gesture the desktop TEACHES was never
  // recorded or reported.
  const intakeJs = read(join(CANARY, "assets/intake.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  for (const [label, src] of [["browser intake.js", intakeJs], ["desktop app.js", appJs]]) {
    assert.ok(/(export )?const KNOWN_STOCK/.test(src),
      `${label} lost the known-stock image table`);
    assert.ok(/(export )?function shippedWith/.test(src),
      `${label} lost shippedWith — an unrecognized image reads as neutral fact`);
    assert.ok(/(export )?function coldBootVerdict/.test(src),
      `${label} lost coldBootVerdict`);
    // The honest verdict for an image we don't know, on both sides.
    assert.ok(src.includes("we don't recognize it"),
      `${label} no longer flags an unrecognized resident image`);
    // …and the honest verdict for a read that FAILED, which must never
    // collapse into "the chip arrived erased" (the cleanest verdict there is).
    assert.ok(src.includes("that's unchecked, not clean"),
      `${label} lets an unreadable chip read as a clean one`);
  }
  // Every stock image the browser knows, the desktop knows.
  const names = (src) => new Set(
    [...src.matchAll(/\{ match: \/[^/]+\/i, name: "([^"]+)"/g)].map((m) => m[1]));
  const b = names(intakeJs), d = names(appJs);
  assert.ok(b.size >= 5, "couldn't parse KNOWN_STOCK from canary-local/assets/intake.js");
  assert.ok(b.has("an Arduino sketch"), "couldn't parse the Arduino entry from intake.js");
  for (const n of b) {
    assert.ok(d.has(n), `desktop KNOWN_STOCK lost "${n}" — it would print as unrecognized`);
  }

  // The cold-start question is ASKED on both, since neither can measure it.
  const browser = read(join(CANARY, "assets/flash.js"));
  for (const [label, src] of [["browser flash.js", browser], ["desktop index.html", html]]) {
    assert.ok(src.includes("I held BOOT while plugging in"),
      `${label} no longer asks whether the board was brought up cold`);
    assert.ok(/It.s already plugged in/.test(src),
      `${label} lost the honest second answer to the cold-start question`);
  }
  assert.match(appJs, /function wireColdStartAnswer/, "the desktop's cold-start answer is unwired");
  // An unanswered question must not render as either answer — "we didn't ask"
  // is not "they said no", and the no-answer verdict is attention-level.
  assert.match(appJs, /state\.heldBoot === undefined \? null : coldBootVerdict/,
    "the desktop renders a cold-start verdict for a question nobody answered");
  assert.match(browser, /const cold = state\.heldBoot === null \? null : intake\.coldBootVerdict\(/,
    "the browser renders a cold-start verdict for a question nobody answered");
  // And the answer describes ONE board, so it must not be inherited.
  assert.match(appJs, /function resetSteps[\s\S]{0,2000}state\.heldBoot = undefined/,
    "the cold-start answer leaks from one board to the next");
});

test("customs names a board running SecuraCV, and stays honest about the Arduino name, in the same words on both flashers", async () => {
  // Releases after 2.4.17 stamp the product's asset_stem into the app
  // descriptor (firmware/scripts/stamp_app_desc.py); the 2.4.17 ones and
  // every other Arduino build all read `arduino-lib-builder`. Both flashers
  // must say the same thing about either, and the desktop must hand its
  // matched product to the finding the way the browser does.
  const intake = await import(pathToFileURL(join(CANARY, "assets/intake.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const stock = /^const KNOWN_STOCK = \[[\s\S]*?\n\];/m.exec(appJs);
  assert.ok(stock, "desktop/src/app.js lost KNOWN_STOCK");
  const desktop = new Function(stock[0] + "\n" + jsFnText(appJs, "shippedWith", "desktop/src/app.js") +
    "\nreturn shippedWith;")();
  const wap = { id: "securacv-canary-wap", name: "Canary WAP", asset_stem: "canary-wap" };
  const flagship = { id: "securacv-canary", name: "Canary", asset_stem: "canary" };
  for (const args of [{ projectName: "canary-wap", product: wap },
                      { projectName: "arduino-lib-builder" },
                      { projectName: "micropython" }]) {
    assert.deepStrictEqual(desktop(args), intake.shippedWith(args), JSON.stringify(args));
  }
  // A substring of a stem is somebody else's image, on both (the unrecognized
  // finding's detail names each flasher's own copy: backup / safety copy).
  const demo = { projectName: "my-canary-demo", product: flagship };
  assert.equal(desktop(demo).kind, "unknown");
  assert.equal(intake.shippedWith(demo).kind, "unknown");
  assert.equal(intake.shippedWith({ projectName: "canary-wap", product: wap }).kind, "securacv");
  assert.match(intake.shippedWith({ projectName: "arduino-lib-builder" }).detail, /up to 2\.4\.17/);
  assert.match(appJs, /shippedWith\(\{\s*projectName: r\.unknown \? null : r\.projectName,\s*product: r\.product,/,
    "the desktop's customs finding no longer gets the product it matched");
  assert.match(flashJs, /product: core\.matchProjectToProduct\(state\.catalog, projectName\)/,
    "the browser's customs finding no longer gets the product it matched");
  // Every stem a release stamps maps back to its own product on the desktop
  // too (flash.test.js holds the browser's copies to the same loop).
  const lookup = new Function([fnSource(appJs, "matchProjectToProduct"), fnSource(appJs, "productForProject")]
    .join("\n") + "\nreturn { matchProjectToProduct, productForProject };")();
  for (const p of catalog.products) {
    assert.equal(lookup.matchProjectToProduct(catalog, p.asset_stem).id, p.id, `desktop match ${p.asset_stem}`);
    assert.equal(lookup.productForProject(catalog, p.asset_stem).id, p.id, `desktop productForProject ${p.asset_stem}`);
  }
});

test("both flashers order a stamped board's version the same way, a dev train past .9 included", async () => {
  // Stamped versions parse, so the install verdict compares real versions:
  // as plain strings "dev.10" sorted before "dev.9", and an older dev image
  // over a newer one read as an Update. Same cases, same answers, both sides.
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const desktop = new Function(jsFnText(appJs, "compareVersions", "desktop/src/app.js") +
    "\nreturn compareVersions;")();
  const pairs = [["2.4.18-dev.9", "2.4.18-dev.10"], ["2.4.18-dev.9-wap", "2.4.18-dev.10-wap"],
                 ["2.4.18-dev.10", "2.4.18-rc.1"], ["2.4.18-rc.1", "2.4.18"], ["2.4.17-wap", "2.4.18-wap"],
                 ["2.4.18-wap", "2.4.18-wap"], ["2.2.0-rc1", "2.2.0-rc2"], ["esp-idf: v4.4.7 38eeba213a", "2.4.18"]];
  for (const [a, b] of pairs) {
    for (const [x, y] of [[a, b], [b, a]]) {
      assert.strictEqual(desktop(x, y), core.compareVersions(x, y), `${x} vs ${y}`);
    }
  }
  assert.strictEqual(desktop("2.4.18-dev.10", "2.4.18-dev.9"), 1);
});

test("neither flasher shows the descriptor's date as the firmware's build date", () => {
  // An Arduino build's descriptor date is the Arduino core's (fw-v2.4.17's
  // read Mar 5 2024 and Mar 31 2026) and the release stamp leaves it alone,
  // so beside "Canary 2.4.18" it told people a 2026 release was built in
  // 2024. The readback line and the passport drop it; the health report
  // keeps it, labeled, with the same note on both sides.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const note = (src, where) => {
    const m = /^const DESCRIPTOR_DATE_NOTE =\n((?:  "[^"\n]*"(?: \+|;)\n)+)/m.exec(src);
    assert.ok(m, `${where} lost DESCRIPTOR_DATE_NOTE`);
    return new Function(`return ${m[1].trim().replace(/;$/, "")};`)();
  };
  assert.equal(note(appJs, "desktop/src/app.js"), note(flashJs, "flash.js"), "the two flashers say it in the same words");
  assert.match(note(flashJs, "flash.js"), /when the Arduino core was built, not this firmware/);
  for (const [src, where] of [[flashJs, "flash.js"], [appJs, "desktop/src/app.js"]]) {
    assert.match(src, /\(s\.built \? ` · descriptor dated \$\{s\.built\}` : ""\)/, `${where}: the report's date is labeled`);
    assert.match(src, /DESCRIPTOR_DATE_NOTE\)\);/, `${where}: the report shows the note`);
    assert.doesNotMatch(src, /\(built \$\{|"Built " \+|· built \$\{/, `${where}: a date is shown as the build date again`);
  }
  assert.doesNotMatch(read(join(ROOT, "desktop/src-tauri/src/lib.rs")), /"built": if built/,
    "the passport's resident record carries the descriptor date again");
});

test("wave 5: the picker leads with one detection-led recommendation on both flashers", () => {
  // The desktop listed every chip-matching product with equal weight; the
  // browser narrows to one card and states the evidence for it. Both read the
  // same two inputs (measured flash size, resident project), so this was a
  // presentation gap, not a data one.
  const flashCore = read(join(CANARY, "assets/flash-core.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));

  assert.match(flashCore, /export function smartPick/, "the browser lost smartPick");
  assert.match(appJs, /function smartPick/, "the desktop lost its smartPick port");
  assert.match(html, /id="pick-recommend"/, "the desktop has nowhere to state the recommendation");
  assert.match(appJs, /smartPick\(state\.catalog, \{/, "the desktop picker never calls smartPick");

  // The honest-ambiguity branch is the one worth pinning: chip + size narrows
  // the field but does not always NAME the board, and "that looks like a
  // <board>" would then be a guess wearing the clothes of a measurement.
  for (const [label, src] of [["browser flash-core.js", flashCore], ["desktop app.js", appJs]]) {
    assert.ok(src.includes("this is a starting point, not"),
      `${label} smartPick no longer admits when the match is ambiguous`);
    assert.ok(/families\.size > 1/.test(src),
      `${label} smartPick no longer detects the ambiguous case`);
    assert.ok(src.includes("Recommended:"),
      `${label} smartPick no longer names its recommendation`);
    // One chip, several boards (ESP32-C6: Sense node or 1.47" display): each
    // board gets a card, never just the authored default.
    assert.ok(/function boardShortlist/.test(src),
      `${label} smartPick lost the one-card-per-board shortlist`);
    assert.ok(src.includes("More than one board ` +\n        `uses that chip"),
      `${label} smartPick no longer admits a chip that spans boards`);
  }
  // The rest of the chip's products stay reachable — folded, never filtered.
  // A recommendation that HIDES a flashable board is worse than a wall of them.
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop app.js", appJs]]) {
    assert.ok(src.includes("for this chip (developer)"),
      `${label} no longer offers the full list behind the recommendation`);
  }
  assert.match(appJs, /\[\.\.\.shortlist, \.\.\.matches\.filter\(/,
    "the desktop must ORDER by the recommendation, never filter to it");
});

test("wave 5: the firmware's commands are labeled buttons on both flashers", () => {
  // The desktop exposed exactly one (`j`) as a button; everything else had to
  // be typed blind into a box whose placeholder taught nothing. Since sweep
  // A86 the buttons are each product's own (flash.json products[].monitor),
  // and neither flasher keeps a table of its own.
  const browser = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  for (const [label, src] of [["browser flash.js", browser], ["desktop app.js", appJs]]) {
    assert.doesNotMatch(src, /MONITOR_CMDS|\[\s*"h"\s*,\s*"help"\s*\]/,
      `${label} keeps a command table of its own — the catalog's products[].monitor is the one source`);
  }
  assert.match(browser, /core\.monitorVocabulary\(state\.catalog, /, "the browser monitor's chips don't come from the catalog");
  assert.match(appJs, /monitorVocabulary\(state\.catalog, product\)/, "the desktop chips don't come from the catalog");

  assert.match(html, /id="monitor-cmds"/, "desktop/src/index.html has nowhere to put the chips");
  assert.match(appJs, /function renderMonitorCmds/, "the desktop command chips are unbuilt");
  assert.match(appJs, /invoke\("serial_monitor_send", \{ command: cmd \+ "\\n" \}\)/,
    "the desktop chips don't send their command through the serial command path");
  // Disabled with the rest of the monitor controls, or they lie about being usable.
  assert.match(appJs, /function setMonitorButtons[\s\S]{0,600}#monitor-cmds button/,
    "the desktop command chips aren't enabled/disabled with the monitor");
  // The box teaches what the firmware reads: the product's placeholder, set
  // where the chips are drawn; before any product is known, the catalog's.
  assert.match(fnSource(appJs, "renderMonitorCmds"), /input\.placeholder = vocab\.placeholder/,
    "the desktop box no longer teaches the firmware's vocabulary");
  assert.match(browser, /input\.placeholder = vocab\.placeholder/,
    "the browser box no longer teaches the firmware's vocabulary");
  const box = /<input id="monitor-cmd"[\s\S]*?\/>/.exec(html)[0];
  assert.strictEqual(/placeholder="([^"]*)"/.exec(box)[1], catalog.monitor_unknown.placeholder,
    "the Flasher's box, before a product is known, must say what the catalog's monitor_unknown says");
});

// ── "verified" is the signature's word, on both flashers and in the engine ──
//
// AGENTS.md rule 4: "verified" means an Ed25519 signature checked against a
// pinned key, nothing looser. Both flashers had drifted into using it for an
// espflash read-back, a chip MD5, a SHA-256 match and a card read-back — and
// the engine itself emitted "✓ chip write verified" for the desktop to echo.
// Those are confirmations and checks, and they now say so. This pins the
// shapes that slipped, on every surface they slipped on, so the next copy
// edit cannot quietly bring one back.
test("no flasher surface calls a read-back, a checksum or a write 'verified'", () => {
  const surfaces = [
    ["browser flash.js", read(join(CANARY, "assets/flash.js"))],
    ["browser flash-native.js", read(join(CANARY, "assets/flash-native.js"))],
    ["browser we2-flash.js", read(join(CANARY, "assets/we2-flash.js"))],
    ["desktop app.js", read(join(ROOT, "desktop/src/app.js"))],
    ["desktop index.html", read(join(ROOT, "desktop/src/index.html"))],
    ["engine flash.rs", engineRs("flash")],
    ["Flasher lib.rs", libRs],
    ["Flasher hub.rs", read(join(ROOT, "desktop/src-tauri/src/hub.rs"))],
    ["Flasher we2.rs", read(join(ROOT, "desktop/src-tauri/src/we2.rs"))],
  ];
  const banned = [
    /write (?:is )?verified/i,
    /written and verified/i,
    /read back,? and verified/i,
    /SHA-256-verified/i,
    /verified by (?:SHA-256|checksum)/i,
    /Verified — the chip/,
    /completed and verified/i,
    /verifiably holds/i,
    /local copy verified/i,
    /image verified against/i,
  ];
  for (const [label, src] of surfaces) {
    for (const re of banned) {
      const hit = re.exec(src);
      assert.ok(!hit, `${label} says "${hit && hit[0]}" — a read-back or checksum is confirmed or checked, ` +
        "never verified (AGENTS.md rule 4: Ed25519 against a pinned key, nothing looser)");
    }
  }
  // The engine and the model burn keep "verified" for the one case it is
  // true — the Ed25519 check ran — and say "checked" for checksum-only.
  const flashRs = engineRs("flash");
  assert.match(flashRs, /if release_verification == "ed25519\+sha256" \{\s*format!\(\s*"✓ release verified: /,
    "the engine's release line must say verified only when the signature was checked");
  assert.match(flashRs, /"✓ release checked: SHA-256 \{\}… \(\{release_verification\} — no release key is pinned/,
    "the engine's checksum-only release line must say checked");
  assert.match(libRs, /if model_verification == "ed25519\+sha256" \{\s*format!\(\s*"✓ model verified: /,
    "the Vision model line must say verified only when the signature was checked");
  // The desktop's stage strip keys on the engine's own words.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  assert.ok(flashRs.includes('"✓ chip write confirmed — ') && appJs.includes('["✓ chip write confirmed", '),
    "the desktop stage strip must key on the engine's chip-write line");
});

// ── rule 7: the browser's post-flash proofs and pre-flash voice, on desktop ──
//
// Four browser-only diagnostics the desktop user silently lacked: the "is it
// on?" Bluetooth check, the catalog's per-product prove block ("Prove it, two
// ways"), the WAP's live field bench, and the self-check chips a board's own
// self-manifest earns before anything is written. The two frontends share no
// code, so these hold the desktop ports to the browser's logic by running
// both — and pin the wiring the logic is useless without.
function fnSource(src, name) {
  const i = src.indexOf(`function ${name}(`);
  assert.ok(i >= 0, `couldn't find function ${name}`);
  const b = src.indexOf("{", src.indexOf(")", i));
  let d = 0;
  for (let k = b; k < src.length; k++) {
    if (src[k] === "{") d++;
    else if (src[k] === "}") { d--; if (!d) return src.slice(i, k + 1); }
  }
  throw new Error(`unbalanced function ${name}`);
}

test("rule 7: the desktop offers the browser's Bluetooth 'is it on?' check — as a pointer, never a dead end", async () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  // Where: the connect page (any time, no flash needed) and the done row.
  for (const id of ["ble-check", "ble-check-open", "prove-ble", "prove-ble-note"]) {
    assert.match(html, new RegExp(`id="${id}"`), `desktop/src/index.html lost #${id}`);
  }
  assert.match(appJs, /\$\("ble-check-open"\)\.addEventListener\("click", openBleCheck\)/);
  assert.match(appJs, /\$\("prove-ble"\)\.addEventListener\("click", openBleCheck\)/);
  // For whom: the boards that run the read-only console — the WAP alone
  // (ble_console.h is the only firmware serving it; an AP-provisioned
  // flagship serves none, so offering it there is a dead end) — the
  // browser's rule, read off the catalog's role (which IS the browser's
  // productRole).
  assert.match(flashJs, /const bleCapable = p && !opts\.isBackup && core\.productRole\(p\) === "wap";/);
  assert.match(fnSource(appJs, "bleCheckable"), /return !!product && product\.role === "wap";/);
  assert.doesNotMatch(fnSource(appJs, "bleCheckable"), /provisioning/, "the desktop offers the console check to a board that serves none");
  const grep = spawnSync("git", ["grep", "-l", "-i", "8fc1cee0", "--", "firmware"], { cwd: ROOT, encoding: "utf8" });
  assert.strictEqual(grep.status, 0, "git grep for the console's service UUID found nothing: " + (grep.stderr || ""));
  const consoleFirmware = grep.stdout.split("\n").filter(Boolean);
  assert.ok(consoleFirmware.length > 0 && consoleFirmware.every((f) => f.startsWith("firmware/projects/canary-wap/")),
    "the Bluetooth console's service UUID is served outside the WAP now: widen the check to that board: " + consoleFirmware.join(", "));
  // The console copy names the WAP, the one board that serves it. Read as
  // the user reads it: string concatenations folded (a sentence split
  // across `" + "` is one sentence), whitespace collapsed, case ignored and
  // the quotes optional, so phaseBluetoothCheck's lowercase "a Canary keeps
  // a read-only Bluetooth " + "“console”" is caught as well as the one-line
  // notes. Each needle is a sentence the flashers carried before the WAP was
  // named, which sent a flagship owner to a console it does not serve.
  const RETIRED_CONSOLE_COPY = [
    /a Canary keeps a read-only Bluetooth [“"]?console/i,   // both flashers' notes, the check's lead
    /Reach a Canary over Bluetooth/i,                       // the browser check's heading
    /running Canary(?:’|'|&rsquo;)s Bluetooth console/i,    // the browser's "Skip the cable" line
    /Already have a Canary\?/i,                             // both flashers' summary lines
  ];
  const folded = (s) => s.replace(/["'`]\s*\+\s*["'`]/g, "").replace(/\s+/g, " ");
  for (const [label, text] of [["browser flash.js", flashJs], ["desktop app.js", appJs], ["desktop index.html", html]]) {
    const flatText = folded(text);
    for (const re of RETIRED_CONSOLE_COPY) {
      assert.doesNotMatch(flatText, re, `${label}: the console copy names the WAP, the one board that serves it`);
    }
  }
  for (const p of catalog.products) {
    assert.strictEqual(p.role, core.productRole(p), `${p.id}: the catalog's role and the browser's productRole disagree`);
  }
  // Where it runs: the browser flasher, named from the catalog's repo.
  assert.match(fnSource(appJs, "openBleCheck"), /labUrl\("flash\.html"\)/);
  // The Lab app's webview has no Web Bluetooth either: its flash page says
  // where the check runs instead of sending a native-shell user into the
  // "use a Chromium browser" error.
  assert.match(flashJs, /mount\.append\(renderReassurance\(\{ nativeShell: true \}\)\)/);
  const reassure = fnSource(flashJs, "renderReassurance");
  assert.match(reassure, /if \(nativeShell\) \{[\s\S]*?in Chrome or Edge on this computer[\s\S]*?\} else \{[\s\S]*?phaseBluetoothCheck/,
    "the Lab app's Bluetooth entry must point to where the check runs, and only the browser opens it in-page");
  for (const [label, src] of [["desktop app.js", appJs], ["desktop index.html", html], ["browser flash.js (Lab app)", reassure]]) {
    // String concatenations and line wraps folded, so a sentence split
    // across source lines still reads as one.
    const flat = src.replace(/"\s*\+\s*"/g, "").replace(/\s+/g, " ");
    assert.ok(flat.includes("the check runs in the browser Lab in Chrome or Edge on this computer"),
      `${label} must name where the Bluetooth check runs`);
    // The check reads a WAP's console, and the iPhone app dials only a
    // peripheral that advertises the console service, which a WAP does not
    // (ledger A65): naming the app as a place the check runs sends an iPhone
    // owner into Bluetooth range for nothing.
    assert.ok(!/runs in the SecuraCV iPhone app|runs in the iPhone app/.test(flat),
      `${label} must not name the iPhone app as where the Bluetooth check runs`);
  }
});

test("rule 7: the desktop reads the catalog's prove block and handles every real.kind it names", () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const kinds = new Set(catalog.products.map((p) => p.prove && p.prove.real && p.prove.real.kind).filter(Boolean));
  assert.ok(kinds.size >= 3, "the catalog's prove kinds went missing");
  const prove = fnSource(appJs, "proveReal");
  for (const k of kinds) {
    // "monitor" and "glass" share the default branch on both: the console,
    // with the self-manifest asked for on the way.
    if (k === "monitor" || k === "glass") continue;
    assert.ok(prove.includes(`kind === "${k}"`), `desktop proveReal doesn't handle the catalog's ${k}`);
    assert.ok(flashJs.includes(`kind === "${k}"`), `browser proveReal doesn't handle the catalog's ${k}`);
  }
  const row = fnSource(appJs, "renderProveRow");
  assert.match(row, /const shown = DESKTOP_PROVE_REAL\[spec\.real\.kind\] \|\| spec\.real;/,
    "the desktop's proof button must carry the catalog's label, or say what it shows instead");
  // An override exists only where the desktop draws something else, and
  // says so: never for a kind it renders as the catalog describes.
  const overrides = /const DESKTOP_PROVE_REAL = \{([\s\S]*?)\n\};/.exec(appJs);
  assert.ok(overrides, "couldn't find DESKTOP_PROVE_REAL");
  assert.deepStrictEqual([...overrides[1].matchAll(/^\s{2}"([a-z-]+)": \{/gm)].map((m) => m[1]), ["bench-radar"],
    "a new desktop override needs a reason in DESKTOP_PROVE_REAL's comment and here");
  assert.match(row, /labUrl\(spec\.emulated\.href\)/, "the desktop must offer the catalog's emulated twin");
  for (const id of ["prove-row", "prove-real", "prove-twin", "prove-how"]) {
    assert.match(html, new RegExp(`id="${id}"`), `desktop/src/index.html lost #${id}`);
  }
  // Both success branches of the one-shot flash draw it.
  assert.strictEqual(fnSource(appJs, "onFlash").split("renderProveRow(product);").length - 1, 2,
    "both of onFlash's success branches must draw the prove row");
  // Every twin link resolves to a page the Lab actually ships, on the URL
  // derived from the catalog's repo — no typed host.
  const labUrl = new Function("state", fnSource(appJs, "labUrl") + "\nreturn labUrl;")({ catalog });
  for (const p of catalog.products) {
    if (!p.prove || !p.prove.emulated) continue;
    const u = labUrl(p.prove.emulated.href);
    assert.ok(u && u.startsWith(`https://${catalog.repo.split("/")[0]}.github.io/${catalog.repo.split("/")[1]}/canary-local/`),
      `${p.id}: the twin link isn't on the Lab's Pages site (${u})`);
    assert.ok(readdirSync(CANARY).includes(p.prove.emulated.href.split("#")[0]),
      `${p.id}: the twin page ${p.prove.emulated.href} isn't in canary-local/`);
  }
});

test("rule 7: the desktop's WAP field bench reads the [wap] lines exactly as the browser's", async () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const desk = new Function(fnSource(appJs, "parseWapLine") + "\nreturn parseWapLine;")();
  for (const line of [
    "[wap] rf_presence_started devices=2 confidence=high dwell=transient stir=42",
    "[wap] rf_presence_departed devices=0 confidence=low dwell=sustained stir=3",
    "[wap] sustained_presence devices=1 confidence=moderate dwell=sustained stir=180",
    "  [wap] dwell_started devices=3 confidence=high dwell=dwell stir=77  ",
    "[wap] csi_clear devices=0 confidence=low dwell=transient stir=0",
    "[sense] present count=1 range=near",
    "[wap] rf_presence_started devices=two confidence=high dwell=transient stir=4",
    "",
    null,
  ]) {
    assert.deepStrictEqual(desk(line), core.parseWapLine(line), `the two parsers disagree on ${JSON.stringify(line)}`);
  }
  // The stir meter drains at the same informative rate on both.
  assert.match(flashJs, /model\.stir = Math\.max\(0, model\.stir - 9 \* dt\)/);
  assert.match(appJs, /wapField\.stir = Math\.max\(0, wapField\.stir - 9 \* dt\)/);
  // Fed from the monitor, shown for a WAP, gone with the outcome it belongs to.
  assert.match(appJs, /feedWapField\(ev\.payload\);/);
  assert.match(fnSource(appJs, "startMonitor"), /renderWapField\(\);/);
  assert.match(fnSource(appJs, "wapProductOnBench"), /p\.role === "wap"/);
  assert.ok(catalog.products.some((p) => p.role === "wap" && p.prove.real.kind === "bench-field"),
    "the catalog's WAP no longer names the field bench as its proof");
  for (const id of ["wap-field", "wap-field-status", "wap-field-fill", "wap-field-stale"]) {
    assert.match(html, new RegExp(`id="${id}"`), `desktop/src/index.html lost #${id}`);
  }
});

test("rule 7: the desktop's self-check chips use the browser's verdict, thresholds and fail-safe included", async () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const desk = new Function(fnSource(appJs, "healthVerdict") + "\nreturn healthVerdict;")();
  for (const h of [null, undefined, NaN, -1, 0, 49, 49.9, 50, 79.9, 80, 100, 100.5, 101, Infinity, "90", true]) {
    assert.deepStrictEqual(desk(h), core.healthVerdict(h), `the verdicts disagree on ${String(h)}`);
  }
  // The same three chips, the same words, the same sanity window on the
  // temperature (a corrupted line must not raise a heat warning).
  const chips = fnSource(appJs, "renderSelfCheck");
  for (const needle of ["m.temp_c > -40 && m.temp_c < 150", "m.temp_c >= 70", " — give it air", "tamper flag raised",
    "${m.health}/100"]) {
    assert.ok(chips.includes(needle), `desktop renderSelfCheck lost ${needle}`);
    assert.ok(flashJs.includes(needle), `browser self-check chips lost ${needle}`);
  }
  assert.match(html, /id="selfcheck-chips"/);
  // Before a flash, too: a recognized resident firmware is asked twice, at
  // the browser's moments, by an observer that never resets the board…
  assert.match(fnSource(appJs, "renderPassport"), /if \(r\.product && !r\.blank && !r\.unknown\) \{[\s\S]*?askSelfCheck/);
  const ask = fnSource(appJs, "askSelfCheck");
  assert.match(ask, /setTimeout\(ask, 900\);\s*setTimeout\(ask, 2600\);/);
  assert.match(flashJs, /setTimeout\(ask, 900\);\s*setTimeout\(ask, 2600\);/);
  assert.match(ask, /if \(!state\.monitoring\) await startMonitor\(\);/, "askSelfCheck must start the monitor as an observer (no postFlash)");
  // …which hands the port back before espflash touches it…
  assert.match(fnSource(appJs, "onFlash"), /state\.busy = true;[^\n]*\n\s*await stopMonitor\(\);/);
  // …and whose answer is a self-check, never an installation receipt: not
  // before any flash, and not after one either — hostFlash outlives an
  // unplug, so the board asked may be the NEXT board, and adopting its
  // manifest would write its device id onto the flashed board's book row.
  assert.match(appJs, /renderSelfCheck\(ev\.payload && ev\.payload\.manifest\);[\s\S]{0,600}if \(!state\.vision\.hostFlash \|\| state\.selfCheckAsked\) return;\s*state\.vision\.hostBoot = ev\.payload;/,
    "a manifest the self-check asked for must not become the host boot receipt");
  assert.match(ask, /if \(state\.busy\) return;\s*state\.selfCheckAsked = true;/,
    "askSelfCheck must mark its answer as a self-check before the monitor can deliver it");
  assert.match(fnSource(appJs, "resetOutcome"), /state\.selfCheckAsked = false;/,
    "every write starts from resetOutcome, which must let the next manifest be that write's receipt");
});

test("both flashers name a saved file by the day and a counter, and the engine's helper is the one in use", async () => {
  // No file either flasher saves is named after the board: the MAC's last six
  // hex digits used to name backups and reports, and the base MAC is the
  // station address in every Wi-Fi frame the board sends.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop app.js", appJs]]) {
    assert.ok(!/macStamp/.test(src), `${label} names files after the MAC again`);
  }
  assert.match(flashJs, /const name = core\.backupFileName\(nextFileStamp\(\)\);/, "the browser's backup name moved");
  assert.match(appJs, /defaultPath: backupFileName\(nextFileStamp\(\)\),/, "the desktop's backup name must be the browser's");
  assert.match(appJs, /defaultPath: reportFileName\(nextFileStamp\(\), full\),/, "the desktop's report name must be the browser's");
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const desk = aboutBlock();
  for (const when of [new Date(2026, 0, 5, 0, 1), new Date(2026, 9, 22, 23, 59)]) {
    for (const n of [1, 2, 17]) {
      const stamp = core.fileStamp(when, n);
      assert.strictEqual(desk.fileStamp(when, n), stamp);
      assert.strictEqual(desk.backupFileName(stamp), core.backupFileName(stamp));
      assert.strictEqual(desk.reportFileName(stamp, true), core.reportFileName(stamp, true));
      assert.strictEqual(desk.reportFileName(stamp, false), core.reportFileName(stamp, false));
    }
  }
  // The day is the local one in any zone, not UTC's: two instants, run in a
  // child in a zone far from UTC on each side (CI's own zone is UTC, where
  // local and UTC getters agree and a slip to UTC would pass unseen).
  const probe = `
    const core = await import(${JSON.stringify(pathToFileURL(join(CANARY, "assets/flash-core.js")).href)});
    const app = (await import("node:fs")).readFileSync(${JSON.stringify(join(ROOT, "desktop/src/app.js"))}, "utf8");
    const block = app.match(/\\/\\/ BEGIN ABOUT-BOARD\\n([\\s\\S]*?)\\/\\/ END ABOUT-BOARD/)[1];
    const desk = new Function(block + "; return { localDay, fileStamp };")();
    const out = [];
    for (const when of [new Date(Date.UTC(2026, 9, 22, 20, 30)), new Date(Date.UTC(2026, 9, 22, 5, 0))]) {
      out.push(core.localDay(when), desk.localDay(when), core.fileStamp(when, 1), desk.fileStamp(when, 1));
    }
    console.log(JSON.stringify(out));`;
  for (const [tz, a, b] of [["Pacific/Kiritimati", "2026-10-23", "2026-10-22"],
    ["Pacific/Pago_Pago", "2026-10-22", "2026-10-21"], ["UTC", "2026-10-22", "2026-10-22"]]) {
    const r = spawnSync(process.execPath, ["--input-type=module", "-e", probe],
      { env: { ...process.env, TZ: tz }, encoding: "utf8" });
    assert.strictEqual(r.status, 0, r.stderr);
    assert.deepStrictEqual(JSON.parse(r.stdout), [a, a, `${a}-1`, `${a}-1`, b, b, `${b}-1`, `${b}-1`],
      `${tz}: a saved file's day is not the local day`);
  }
  // The automatic safety copy is named by the engine's helper — the same
  // `canary-<day>-<n>-backup.bin`, its counter the next one free in the
  // backups folder — from the day the frontend computes in the local zone.
  assert.match(libRs, /fn auto_backup_path\(app: AppHandle, day: String\)/);
  assert.match(libRs, /rescue::next_backup_filename\(&day, &taken\)/);
  assert.match(appJs, /invoke\("auto_backup_path", \{ day: localDay\(new Date\(\)\) \}\)/);
  assert.ok(engineRs("rescue").includes(`"${core.backupFileName("2026-10-22-1")}"`),
    "rescue.rs no longer pins the browser's scheme");
  assert.ok(!/fn backup_filename\(mac/.test(engineRs("rescue")), "rescue::backup_filename takes a MAC again");
});

// ── The setup network's key, off the board's own first-boot line ───────────
//
// A Sense or Vision prints its setup network and key exactly once, when the
// portal comes up; a WAP when its AP starts. Nothing else ever shows that key
// ("the key the Flasher printed" never existed until both flashers read this
// line). The browser parses it in flash-core.js; the Mac app inlines the SAME
// two patterns. Hold them equal, hold both to the firmware's own printf
// strings, and hold both frontends to rendering the result where the hatch
// steps ask for it.
test("setup-network key: one parser, both flashers, pinned to the firmware's printf", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const src = read(join(ROOT, "desktop", "src", "app.js"));
  const block = src.match(
    /const SETUP_DOOR_NOTE =[\s\S]*?\nfunction isSetupNetworkSsid\([\s\S]*?\n}\n/);
  assert.ok(block, "desktop/src/app.js no longer carries the inlined setup-network parser block");
  const native = new Function(`${block[0]}; return { SETUP_DOOR_NOTE, WIFI_PASS_REQUIRED, ` +
    `SETUP_NETWORK_LINE_RES, parseSetupNetwork, setupNetworkLine, ` +
    `SETUP_QR_LINE_RE, parseSetupQr, SETUP_SSID_RE, isSetupNetworkSsid };`)();

  // The copy and the patterns, byte for byte.
  assert.equal(native.SETUP_DOOR_NOTE, core.SETUP_DOOR_NOTE, "the 'leave blank' note drifted between flashers");
  assert.equal(native.WIFI_PASS_REQUIRED, core.WIFI_PASS_REQUIRED, "the no-password refusal drifted between flashers");
  assert.deepEqual(native.SETUP_NETWORK_LINE_RES.map((r) => r.source),
    core.SETUP_NETWORK_LINE_RES.map((r) => r.source), "the setup-network line patterns drifted");
  for (const name of ["SETUP_QR_LINE_RE", "SETUP_SSID_RE"]) {
    assert.equal(native[name].source, core[name].source, `${name} drifted between flashers`);
    assert.equal(native[name].flags, core[name].flags, `${name}'s flags drifted between flashers`);
  }

  // The firmware's own format strings — a reworded printf fails here, not on
  // a bench. The %s slots are rendered with sample values and parsed back.
  // The Sense/Vision line is built by onboarding_fsm.h announce_line (no
  // trailing newline: setup_portal.cpp prints it with Serial.println, which
  // adds CR LF) and the QR line beside it by qr_line; the WAP prints its own.
  const fsm = read(join(ROOT, "firmware/common/network/onboarding_fsm.h"));
  const senseFmt = '[SETUP] Setup network up: \\"%s\\"  password %s  (channel %u)';
  assert.ok(fsm.includes(senseFmt),
    "onboarding_fsm.h no longer builds the `[SETUP] Setup network up` line both flashers parse");
  assert.ok(fsm.includes('"[SETUP] QR "'), "onboarding_fsm.h no longer builds the `[SETUP] QR` line");
  const portal = read(join(ROOT, "firmware/common/network/setup_portal.cpp"));
  assert.ok(portal.includes("onboard::announce_line("),
    "setup_portal.cpp no longer prints the announce line through onboarding_fsm.h");
  assert.ok(portal.includes("onboard::qr_line("),
    "setup_portal.cpp no longer prints the QR line beside it");
  const wap = read(join(ROOT, "firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino"));
  const wapFmt = "[WIFI] AP started: %s (password: %s)\\n";
  assert.ok(wap.includes(wapFmt),
    "canary_wap.ino no longer prints the `[WIFI] AP started` line both flashers parse");
  const render = (fmt, ...vals) => fmt.replace(/\\"/g, '"').replace(/\\n/g, "\n")
    .replace(/%[su]/g, () => String(vals.shift()));
  const qrPayload = "WIFI:T:WPA;S:SecuraCV-sense-4dC2;P:a7Kq2pMx;;";
  const senseLog = "rst:0x1 (POWERON)\r\n[WIFI] No credentials yet — raising the setup network; sensing continues.\r\n" +
    render(senseFmt, "SecuraCV-sense-4dC2", "a7Kq2pMx", 1) + "\r\n" +
    "[SETUP] QR " + qrPayload + "\r\n" +
    "[SETUP] Join it from a phone - the setup page opens by itself (or open http://192.168.4.1/).\r\n";
  const legacyLog = render(senseFmt, "SecuraCV-4dC2-k7", "a7Kq2pMx", 6) + "\n";
  const wapLog = "[PROV] Deriving device-unique AP password...\n" +
    render(wapFmt, "SecuraCV-wap-m2G2", "cv-7hJ2kQ9mN4pR") + "[OK] WiFi AP active\n";
  for (const [label, log, want] of [
    ["sense/vision", senseLog, { ssid: "SecuraCV-sense-4dC2", key: "a7Kq2pMx" }],
    ["older firmware", legacyLog, { ssid: "SecuraCV-4dC2-k7", key: "a7Kq2pMx" }],
    ["wap", wapLog, { ssid: "SecuraCV-wap-m2G2", key: "cv-7hJ2kQ9mN4pR" }],
  ]) {
    assert.deepEqual(core.parseSetupNetwork(log), want, `browser parser misread the ${label} line`);
    assert.deepEqual(native.parseSetupNetwork(log), want, `native parser misread the ${label} line`);
    assert.equal(native.setupNetworkLine(want), core.setupNetworkLine(want), "the rendered line drifted");
    assert.ok(core.isSetupNetworkSsid(want.ssid) && native.isSetupNetworkSsid(want.ssid),
      `the ${label} name is one the matcher accepts`);
  }
  assert.equal(core.parseSetupNetwork("just boot text"), null);
  assert.equal(native.parseSetupNetwork("[SETUP] Join requested: \"Home\""), null,
    "a different [SETUP] line must not read as the network-up line");
  assert.match(core.setupNetworkLine({ ssid: "SecuraCV-4dC2", key: "a7Kq2pMx" }),
    /^Its setup network: SecuraCV-4dC2 · key a7Kq2pMx$/);
  // The QR line: both read the payload as printed, without println's CR.
  assert.equal(core.parseSetupQr(senseLog), qrPayload, "browser misread the QR line");
  assert.equal(native.parseSetupQr(senseLog), qrPayload, "native misread the QR line");
  assert.equal(native.parseSetupQr(wapLog), null, "a WAP prints no QR line");
  // The name grammar: the host test's vectors (test_onboarding_fsm.cpp
  // the_matcher_accepts_every_shape_ever_emitted), same verdict in both.
  for (const [ssid, ours] of [
    ["SecuraCV-4dC2", true], ["SecuraCV-4dC2-k7", true], ["SecuraCV-sense-4dC2", true],
    ["SecuraCV-sentinel-4dC2-k7", true], ["SecuraCV-display-CNRY", true],
    ["SecuraCV-4dC", false], ["SecuraCV-4dC2x", false], ["SecuraCV-4dC2-", false], ["SecuraCV-4dC2-k", false],
    ["SecuraCV-Sense-4dC2", false], ["SecuraCV-s3nse-4dC2", false], ["SecuraCV-vision-4dC2-k7x", false],
    ["SecuraCV-a-b-c-d", false], ["securacv-4dC2", false], ["MyWiFi", false], ["SecuraCV-", false],
    ["SecuraCV-4d C2", false],
  ]) {
    assert.equal(core.isSetupNetworkSsid(ssid), ours, `browser matcher: ${ssid}`);
    assert.equal(native.isSetupNetworkSsid(ssid), ours, `native matcher: ${ssid}`);
  }

  // Both frontends LISTEN and RENDER it: the browser's voice/monitor and done
  // card, the desktop's serial-log listener and hatch card.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const html = read(join(ROOT, "desktop", "src", "index.html"));
  assert.match(flashJs, /core\.parseSetupNetwork\(v\.buf\)/, "the browser voice no longer parses the key off the stream");
  assert.match(flashJs, /startVoice\(vcon, null, showSetupNet\)/,
    "the browser done card no longer listens to the first boot for the key");
  assert.match(flashJs, /maybeSetupNet\(buf\)/, "the browser monitor no longer renders the key line");
  assert.match(src, /feedSetupNetwork\(ev\.payload\)/, "the desktop serial:log listener no longer feeds the parser");
  assert.match(html, /id="hatch-setup-net"/, "the desktop hatch card lost its setup-network line");
  assert.match(src, /renderSetupNetwork\(\); \/\/ the key, if the first boot has printed it already/,
    "showHatchCard no longer renders a key that landed before the card");
});

// ── The Bluetooth setup door: "leave the WiFi blank" only where it is true ──
test("setup door: catalog-derived, both flashers say it, neither prefills past it", async () => {
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop", "src", "app.js"));
  const html = read(join(ROOT, "desktop", "src", "index.html"));

  // Every product carries the derived fact, and the WAP's/all-rounder's
  // hatch leads with the iPhone card exactly when the door exists.
  for (const p of catalog.products) {
    assert.equal(typeof p.setup_door, "boolean", `${p.id}: setup_door must be derived for every product`);
    if (p.provisioning === "ap") {
      const leadsWithCard = /iPhone app/.test(p.hatch.steps[0]);
      assert.equal(leadsWithCard, p.setup_door,
        `${p.id}: the hatch leads with the iPhone card iff the build compiles the door`);
      assert.match(p.hatch.steps.join(" "), /key it prints on its serial console/,
        `${p.id}: the setup-network step must name the key — "nothing to type" was false`);
      assert.doesNotMatch(p.provisioning_note, /Nothing to type/, `${p.id}: the setup network has a key`);
    }
  }
  assert.ok(catalog.products.some((p) => p.setup_door), "no product derives a setup door — gen_flash.py's has_setup_door broke");
  assert.ok(catalog.products.filter((p) => p.provisioning === "ap").some((p) => !p.setup_door),
    "the all-rounder canary compiles no door and must not be told to wait for a card");

  // Both read the catalog fact — never a role guess.
  assert.match(flashJs, /core\.hasSetupDoor\(product\)/, "browser flasher no longer gates the note on setup_door");
  assert.match(appJs, /const door = p\.setup_door === true;/, "desktop no longer gates on setup_door");
  assert.match(appJs, /if \(!door && !\$\("wifi-ssid"\)\.value\) \{\s*\n\s*invoke\("current_ssid"\)/,
    "desktop prefills the current SSID for a door-bearing product again");
  // The note sits where the decision is made, in both.
  assert.match(flashJs, /core\.SETUP_DOOR_NOTE/, "browser flasher lost the 'leave blank' note under WiFi (optional)");
  assert.match(appJs, /\(door \? " " \+ SETUP_DOOR_NOTE : ""\)/, "desktop provision note lost the 'leave blank' sentence");

  // A name with no password is refused unless the open-network box is ticked,
  // in both — same words (pinned above), same explicit choice.
  for (const [label, s] of [["browser flash.js", flashJs], ["desktop index.html", html]]) {
    assert.ok(s.includes("open network (no password)"), `${label} lost the explicit open-network choice`);
  }
  assert.match(flashJs, /core\.wifiFieldsVerdict\(ssid\.value, pass\.value, openChk\.checked\)/,
    "browser credentials() no longer runs the shared verdict");
  assert.match(appJs, /if \(\$\("wifi-ssid"\)\.value && !wifiPass && !\(\$\("wifi-open"\) && \$\("wifi-open"\)\.checked\)\) \{\s*\n\s*setStatus\("flash-result", WIFI_PASS_REQUIRED, "err"\)/,
    "desktop readProvisioning no longer refuses a name with no password");
});

// ── About this board (device details, PR 1) ────────────────────────────────
// The desktop carries the browser's About model as an inlined block (its
// bundle cannot import canary-local). Cut it out and run it beside the
// browser's: same facts in, same rows, words, order, names and projections out.
function aboutBlock() {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
  assert.ok(m, "desktop/src/app.js lost its ABOUT-BOARD block markers");
  return new Function(`${m[1]}; return { ABOUT_CANNOT, sizeWords, chipRevisionFromDesc, CHIP_FUSE_KEYS,
    decodeChipFuses, aboutBoardGroups, aboutBoardSummary, aboutBoardText, localDay, fileStamp, backupFileName,
    reportFileName, SHARE_REPORT_NOTE, FULL_REPORT_NOTE, buildDay, reportShareProjection, reportFullProjection,
    maskHardwareAddress };`)();
}

const ABOUT_FUSE_VECTORS = {
  "ESP32-S3": [
    {},
    { WAFER_VERSION_MINOR_LO: 2, PKG_VERSION: 0, BLK_VERSION_MINOR: 3, BLK_VERSION_MAJOR: 1, PSRAM_CAP: 1,
      PSRAM_VENDOR: 1, PSRAM_TEMP: 1 },
    { WAFER_VERSION_MINOR_LO: 0, WAFER_VERSION_MAJOR: 1, BLK_VERSION_MAJOR: 1, BLK_VERSION_MINOR: 1, PKG_VERSION: 1,
      FLASH_CAP: 1, FLASH_VENDOR: 2, FLASH_TEMP: 2, PSRAM_CAP: 0, PSRAM_CAP_3: 1, PSRAM_VENDOR: 2 },
    { PKG_VERSION: 7, FLASH_CAP: 7, FLASH_VENDOR: 7, PSRAM_CAP: 3, PSRAM_CAP_3: 1 },
  ],
  "ESP32-C3": [
    { PKG_VERSION: 0, FLASH_CAP: 1, FLASH_VENDOR: 2, FLASH_TEMP: 2, WAFER_VERSION_MINOR_LO: 4, BLK_VERSION_MAJOR: 1 },
    { PKG_VERSION: 3, FLASH_CAP: 4, FLASH_VENDOR: 5, BLK_VERSION_MAJOR: 0 },
  ],
  "ESP32-C6": [
    { PKG_VERSION: 1, FLASH_CAP: 2, BLK_VERSION_MINOR: 1, WAFER_VERSION_MINOR: 9 },
    { PKG_VERSION: 0, FLASH_CAP: 0, BLK_VERSION_MAJOR: 0, BLK_VERSION_MINOR: 0 },
  ],
};

test("About this board: the desktop's rows are the browser's, fact for fact", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const intake = await import(pathToFileURL(join(CANARY, "assets/intake.js")).href);
  const desk = aboutBlock();

  // The desktop decodes efuse.rs's field values with the browser's decode.
  for (const [chip, table] of Object.entries(intake.CHIP_FUSE_FIELDS)) {
    assert.deepStrictEqual(desk.CHIP_FUSE_KEYS[chip], table.map((f) => f.key), `${chip} key lists drifted`);
    for (const f of ABOUT_FUSE_VECTORS[chip]) {
      assert.deepStrictEqual(desk.decodeChipFuses(chip, f), intake.decodeChipFuses(chip, f), `${chip} decode drifted`);
    }
  }
  assert.deepStrictEqual(desk.decodeChipFuses("ESP32", {}), intake.decodeChipFuses("ESP32", {}));
  assert.deepStrictEqual(desk.ABOUT_CANNOT, core.ABOUT_CANNOT, "the cannot-say phrases drifted");

  // Every combination of what a flasher can know about a board.
  const header = core.parseBootloaderHeader(Uint8Array.from([0xe9, 3, 2, 0x2f]), "ESP32-S3");
  const images = [undefined, false, null,
    { projectName: "canary_wap", version: "2.4.16", idf: "v5.5.1", secureVersion: 2, elfSha256Prefix: "a0a1a2a3a4a5a6a7" },
    { projectName: "", version: "", idf: "", secureVersion: undefined, elfSha256Prefix: null }];
  const boots = [undefined, false, null, header, { ...header, flashBytes: 8 << 20 },
    core.parseBootloaderHeader(Uint8Array.from([0xe9, 3, 2, 0x20]), "ESP32-C6")];
  let n = 0;
  for (const chip of ["ESP32-S3", "ESP32-C3", "ESP32-C6", "ESP32", null]) {
    const fuseStates = [undefined, null, { supported: false, chip },
      ...((ABOUT_FUSE_VECTORS[chip] || []).map((f) => intake.decodeChipFuses(chip, f)))];
    for (const chipDesc of [null, "esp32s3 (revision v0.2)", "ESP32-S3 (QFN56) (revision v0.2)"]) {
      for (const fuses of fuseStates) {
        for (const image of images) {
          for (const bootloader of boots) {
            for (const bench of [false, true]) {
              const facts = { chip, chipDesc, flashBytes: chip ? 8 << 20 : null, fuses, image, bootloader, bench };
              const want = core.aboutBoardGroups(facts);
              assert.deepStrictEqual(desk.aboutBoardGroups(facts), want, `rows drifted for ${JSON.stringify(facts)}`);
              assert.strictEqual(desk.aboutBoardText(want, "X"), core.aboutBoardText(want, "X"));
              assert.strictEqual(desk.aboutBoardSummary(facts), core.aboutBoardSummary(facts));
              n++;
            }
          }
        }
      }
    }
  }
  assert.ok(n > 1000, `only ${n} fact sets compared`);

  // The saved report: the same projections, the same notes.
  const report = {
    generatedAt: "2026-10-22T21:14:09.123Z", chip: "ESP32-S3", mac: "dc:54:75:c1:22:30", flashBytes: 8 << 20,
    partitions: [{ label: "nvs", kind: "data · nvs", offset: 0x9000, size: 0x5000 }],
    slots: [{ label: "app0", subtype: 0x10, empty: false, project: "canary", version: "2.4.16",
      built: "Oct 22 2026 14:33:12", idf: "v5", active: true },
    { label: "app1", subtype: 0x11, empty: false, project: "canary", version: "2.4.15", built: "Oct  2 2026 09:05:01" }],
    ota: { updatesSeen: 3, pendingVerify: true }, coredump: { present: true, size: 1 },
    witness: { boots: 9, seq: 4, chainHeadFp: "abcd" }, witnessLog: { label: "witness_log", size: 4096 },
    verdict: { level: "attn", headline: "a crash is stored", findings: [] },
    about: core.aboutBoardGroups({ chip: "ESP32-S3", flashBytes: 8 << 20 }),
  };
  const when = new Date(2026, 9, 22, 9);
  assert.deepStrictEqual(desk.reportShareProjection(report, when), core.reportShareProjection(report, when));
  assert.deepStrictEqual(desk.reportFullProjection(report), core.reportFullProjection(report));
  assert.ok(!JSON.stringify(desk.reportShareProjection(report, when)).includes("verdict"),
    "the desktop's verdict (a crash, a pending update) is posture and stays out of the shared save");
  // A descriptor's day, never its time: lib.rs's health_check stamps each
  // slot "<date> <time>" from the descriptor, the way flash.js does.
  const deskShare = JSON.stringify(desk.reportShareProjection(report, when));
  for (const t of ["14:33", "09:05", ":12", ":01"]) {
    assert.ok(!deskShare.includes(t), `the desktop's shared save keeps a descriptor's time of day (${t})`);
  }
  assert.deepStrictEqual(desk.reportShareProjection(report, when).slots.map((x) => x.built), ["Oct 22 2026", "Oct 2 2026"]);
  assert.match(libRs, /let built = format!\("\{\} \{\}", d\.date, d\.time\)/,
    "the desktop's slot stamp changed shape; re-check that buildDay still cuts it to the day");
  for (const stamp of ["Oct 22 2026 14:33:12", "2026-10-22T14:33:12", "14:33", "", null, undefined, "Oct 22 2026"]) {
    assert.strictEqual(desk.buildDay(stamp), core.buildDay(stamp), `buildDay drifted for ${JSON.stringify(stamp)}`);
  }
  assert.strictEqual(desk.SHARE_REPORT_NOTE, core.SHARE_REPORT_NOTE);
  assert.strictEqual(desk.FULL_REPORT_NOTE, core.FULL_REPORT_NOTE);

  // The ROM tools' MAC lines are masked the same way where each draws them,
  // and what only looks like one (flash.test.js's and flash.rs's keep cases:
  // another separator, a non-hex digit in either place of an octet) is kept
  // the same way.
  for (const line of ["MAC: dc:54:75:c1:22:30", "MAC address:       DC-54-75-C1-22-30", "Chip type: esp32s3", "12:34:56"]) {
    assert.strictEqual(desk.maskHardwareAddress(line), core.maskHardwareAddress(line));
  }
  for (const keep of ["Chip is ESP32-S3 (QFN56) (revision v0.2)", "Uptime 12:34:56", "fe80::1",
                      "sha256 a1b2c3d4e5f60718", "Crystal is 40MHz", "dc.54.75.c1.22.30", "dc:54:75 c1:22:30",
                      "gg:54:75:c1:22:30", "g0:54:75:c1:22:30", "dc:54:75:c1:22:3g"]) {
    assert.strictEqual(desk.maskHardwareAddress(keep), keep, `the desktop masks what is not a MAC: ${keep}`);
    assert.strictEqual(core.maskHardwareAddress(keep), keep);
  }
});

test("About this board: the eFuse field allowlist is one table, read the same way, on both flashers", () => {
  const intakeJs = read(join(CANARY, "assets/intake.js"));
  const efuseRs = read(join(ROOT, "desktop/src-tauri/src/efuse.rs"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  // (chip, key, block, word, bit, width), per chip, both sides.
  const jsTable = (chip) => {
    const m = intakeJs.match(new RegExp(`CHIP_FUSE_FIELDS = \\{[\\s\\S]*?"${chip}": \\[([^\\]]*)\\]`));
    assert.ok(m, `couldn't find intake.js's ${chip} field allowlist`);
    return [...m[1].matchAll(/key: "([A-Z0-9_]+)", block: (\d), word: (\d), bit: (\d+), width: (\d+)/g)]
      .map((x) => x.slice(1).join("@")).sort();
  };
  const rsTable = (constName) => {
    const m = efuseRs.match(new RegExp(`const ${constName}: &\\[AllowedField\\] = &\\[([^\\]]*)\\];`));
    assert.ok(m, `couldn't find efuse.rs ${constName}`);
    return [...m[1].matchAll(/allowed\("([A-Z0-9_]+)", (\d), (\d), (\d+), (\d+)\)/g)]
      .map((x) => x.slice(1).join("@")).sort();
  };
  for (const [chip, c] of [["ESP32-S3", "ALLOWED_S3"], ["ESP32-C3", "ALLOWED_C3"], ["ESP32-C6", "ALLOWED_C6"]]) {
    assert.ok(jsTable(chip).length >= 8, `intake.js ${chip} table didn't parse`);
    assert.deepStrictEqual(rsTable(c), jsTable(chip), `${chip}: the eFuse field allowlist drifted between intake.js and efuse.rs`);
    assert.match(efuseRs, new RegExp(`"${chip}" => Some\\(${c}\\)`), `efuse.rs no longer maps ${chip} to ${c}`);
  }
  // Same read offsets for blocks 1 and 2.
  const js = intakeJs.match(/EFUSE_BLOCK_RD_OFFSETS = \{ 1: (0x[0-9a-f]+), 2: (0x[0-9a-f]+) \}/);
  const r1 = efuseRs.match(/EFUSE_BLOCK1_RD_OFFSET: u32 = (0x[0-9a-f_]+);/);
  const r2 = efuseRs.match(/EFUSE_BLOCK2_RD_OFFSET: u32 = (0x[0-9a-f_]+);/);
  assert.ok(js && r1 && r2, "couldn't read the block 1/2 offsets from both sides");
  assert.strictEqual(parseInt(r1[1], 16), parseInt(js[1], 16));
  assert.strictEqual(parseInt(r2[1], 16), parseInt(js[2], 16));
  // Masked in the reader: each word is read inside the expression that
  // reduces it to its allowed fields, so the reader never holds it under a
  // name, and the reader does nothing else; the serialized scan keeps
  // block 0's words only.
  assert.match(efuseRs, new RegExp(String.raw`\nfn read_chip_fields<L: RomLink>\(link: &mut L, efuse_base: u32, chip: &str\) -> Option<ChipFields> \{` +
    String.raw`\s*allowed_fields\(chip\)\?;\s*let mut got = ChipFields::new\(\);\s*for \(block, word\) in chip_fuse_words\(chip\) \{` +
    String.raw`\s*got\.extend\(fields_of_word\(\s*chip,\s*block,\s*word,\s*link\.read_reg\(chip_fuse_addr\(efuse_base, block, word\)\)\?,?\s*\)\);` +
    String.raw`\s*\}\s*Some\(got\)\s*\}\n`),
    "efuse.rs must reduce each block-1/2 word to its fields inside the expression that reads it, and do nothing else with it");
  assert.match(efuseRs, /words: words\[\.\.EFUSE_BLOCK0_WORDS\.min\(words\.len\(\)\)\]\.to_vec\(\)/);
  // The command reads the board as the chip it was asked about, so the
  // allowlist is that chip's.
  assert.match(nativeFnBody(efuseRs, "read_security_efuses"), /read_fuses_from_port\(&port, base, &chip\)\?;/,
    "the desktop fuse read reads the board as a chip other than the one it was given");
  assert.match(intakeJs, /Object\.assign\(fields, fieldsOfWord\(chip, block, word, await readReg\(chipFuseAddr\(efuseBase, block, word\)\)\)\)/,
    "intake.js must reduce each block-1/2 word to its fields as it is read");
  // The frontends decode field values, never words.
  assert.match(appJs, /decodeChipFuses\(chip, scan\.chip_fields\)/);
  assert.match(read(join(CANARY, "assets/flash.js")), /intake\.readChipFuses\(state\.chip, esploader\.chip && esploader\.chip\.EFUSE_BASE,/);
});

test("the hardware address: no bytes on screen until asked, none copied, saved or in a file name — both flashers and the Lab bench", async () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const html = read(join(ROOT, "desktop/src/index.html"));
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop app.js", appJs]]) {
    assert.ok(!/ID \(MAC\)/.test(src), `${label} shows "ID (MAC)" again`);
    assert.ok(!/macTail/.test(src), `${label} draws a MAC tail again`);
  }
  // The desktop: the connect lines carry the chip, never the address; the
  // nursery strip carries no tail; the reveal is the one place it is drawn.
  assert.ok(!/state\.mac \? ` · \$\{state\.mac\}`/.test(appJs), "the health note prints the MAC again");
  assert.ok(!/e\.mac\.split/.test(appJs), "the nursery strip prints a MAC tail again");
  assert.match(appJs, /setConn\("connected", `Connected · \$\{aboutBoardSummary\(aboutFacts\(\)\)\} on \$\{port\}`\)/);
  const drawn = [...appJs.matchAll(/String\(mac\)\.toUpperCase\(\)/g)];
  assert.strictEqual(drawn.length, 1, "the desktop draws the address somewhere other than its reveal");
  assert.match(fnSource(appJs, "macRevealRow"), /show\.addEventListener\("click", \(\) => \{/);
  assert.match(html, /id="this-board"/, "the desktop lost its This Board card");
  // health_check takes no MAC and returns none; the saves are projections.
  assert.ok(!/fn health_check\([^)]*\bmac\b/.test(libRs), "health_check takes the MAC again");
  assert.ok(!/"mac": mac/.test(libRs), "health_check returns the MAC again");
  assert.match(appJs, /invoke\("health_check", \{ port, chip, flashBytes, baud: flashBaud\(\) \}\)/);
  assert.match(fnSource(appJs, "onSaveHealthReport"),
    /const body = full \? reportFullProjection\(r\) : reportShareProjection\(r, Date\.now\(\)\);/);
  assert.ok(!/contents: JSON\.stringify\(r, null, 2\)/.test(appJs), "a desktop save writes the raw report again");
  for (const [label, src] of [["browser flash.js", flashJs], ["desktop app.js", appJs]]) {
    assert.ok(src.includes("Save full report for your own records"), `${label} lost the labeled full save`);
  }
  // The diagnostic gatherers hand the builders no MAC, and espflash's own
  // board-info line ("MAC address: …", printed before every flash) is masked
  // where the desktop and the Lab bench draw it.
  assert.ok(!/mac: state\.mac \|\| undefined/.test(appJs));
  assert.match(fnSource(appJs, "appendFlashLog"), /const line = maskHardwareAddress\(String\(text\)\);/);

  // The Lab's native bench: it keeps the address detect_chip returns and
  // shows none of it, and its About rows are the Flasher's for the same read.
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const desk = aboutBlock();
  const receipt = { target: "esp32-host", product_id: "securacv-canary", version: "9.9.9", release_sha256: "a".repeat(64),
    installed_sha256: "b".repeat(64), bytes_written: 1, release_verification: "ed25519+sha256", channel: "stable",
    chip_write_verified: true, provisioned: true };
  const run = await runNativeBench({
    detectAnswer: { chip_desc: "esp32s3 (revision v0.2)", revision: "v0.2", crystal_mhz: 40, features: ["WiFi", "BLE"] },
    flashAnswers: [receipt],
    // espflash prints its board info, MAC included, before the write — the
    // line arrives while the flash's log listener is up.
    onListen: (evt, emit) => { if (evt === "flash:progress") emit("flash:log", "MAC address:       dc:54:75:c1:22:30"); },
  });
  try {
    const text = run.text();
    assert.ok(!/dc:54|DC:54|c1:22:30|C1:22:30/.test(text), "the Lab bench draws the hardware address");
    assert.match(text, /Connected · ESP32-S3 v0\.2 · 8 MB flash on \/dev\/ttyACM0/);
    const facts = { chip: "ESP32-S3", chipDesc: "esp32s3 (revision v0.2)", flashBytes: 8 * 1024 * 1024, bench: true };
    const want = core.aboutBoardGroups(facts);
    assert.deepStrictEqual(desk.aboutBoardGroups(facts), want, "flash-native.js and app.js disagree on the bench's rows");
    const rows = run.mount.find((n) => n.tagName === "details" && n.textContent.startsWith("About this board"));
    assert.ok(rows, "the Lab bench lost its About this board rows");
    for (const r of want.flatMap((g) => g.rows).filter((x) => !x.reveal)) {
      assert.ok(rows.textContent.includes(r.label + r.value), `the Lab bench lost the row ${r.label}: ${r.value}`);
    }
    assert.ok(rows.textContent.includes(core.ABOUT_CANNOT.flasherOnly),
      "the bench must say the eFuse and image rows are read with the Flasher");
    // The bench's technical log masks the board-info line's address.
    await run.pick("securacv-canary");
    assert.ok(!/c1:22:30/i.test(run.text()), "the Lab bench's flash log draws the MAC");
    assert.ok(run.text().includes(core.HW_ADDRESS_MASK));
  } finally {
    run.close();
  }
});

test("About this board reaches every frontend: ChipInfo's chip line, the passport's image header, the rows drawn", () => {
  const flashRs = engineRs("flash");
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const nativeJs = read(join(CANARY, "assets/flash-native.js"));
  const flashJs = read(join(CANARY, "assets/flash.js"));
  // detect_chip keeps espflash's chip line, revision, crystal and features
  // (both apps' detect_chip is this one function) …
  const chipInfo = flashRs.match(/pub struct ChipInfo \{([\s\S]*?)\n\}/)[1];
  for (const f of ["chip_desc", "revision", "crystal_mhz", "features"]) {
    assert.match(chipInfo, new RegExp(`\\b${f}:`), `ChipInfo lost ${f}`);
  }
  // … and every frontend reads it.
  assert.match(appJs, /state\.chipDesc = info\.chip_desc \|\| null;/);
  assert.match(nativeJs, /st\.chipDesc = info\.chip_desc \|\| null;/);
  // The passport reads the bootloader's header where the chip puts it, and
  // the descriptor's anti-rollback number and ELF fingerprint.
  assert.match(libRs, /async fn board_passport\(\s*app: AppHandle,\s*port: String,\s*chip: Option<String>,\s*baud: u32,\s*\)/);
  assert.match(libRs, /health::bootloader_offset\(&chip_name\)/);
  assert.match(libRs, /health::parse_bootloader_header\(&hb, &chip_name\)/);
  assert.match(libRs, /"secureVersion": d\.secure_version,\s*"elfSha256Prefix": d\.elf_sha256_prefix,/);
  assert.match(appJs, /invoke\("board_passport", \{\s*port,\s*chip: state\.chip,/);
  assert.match(flashJs, /core\.parseBootloaderHeader\(hb, state\.chip\)/);
  // … where the chip puts it: 0x1000 on the classic ESP32, 0x0 on the rest.
  assert.match(flashJs, /readFlashChunked\(esploader, core\.bootloaderOffset\(state\.chip\), 0x20\)/);
  // The rows are drawn from the shared model on all three.
  assert.match(flashJs, /core\.aboutBoardGroups\(aboutFacts\(\)\)/);
  assert.match(nativeJs, /core\.aboutBoardGroups\(aboutFacts\(\)\)/);
  assert.match(appJs, /aboutBoardGroups\(aboutFacts\(\)\)/);
});

test("the Grove module: neither flasher asks for its id, and neither shows its own model text", () => {
  const we2Flash = read(join(CANARY, "assets/we2-flash.js"));
  const benchRs = read(join(ROOT, "desktop/src-tauri/src/we2_bench.rs"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  // ID? is treated as a per-unit identifier: never sent, on either flasher.
  assert.ok(!/cmd\("ID\?"/.test(we2Flash), "the browser's Grove bench sends ID? again");
  assert.ok(!/at_command\("ID\?"/.test(we2Rs), "the desktop's Grove proof sends ID? again");
  assert.ok(!/pub module_id/.test(we2Rs), "the desktop's module receipt carries an id field again");
  // The module's model card text never reaches a screen …
  assert.ok(!/model: " \+/.test(we2Flash), "the browser shows the module's model name again");
  assert.ok(!/decoded\.name/.test(we2Flash), "the browser reads the card's name again");
  assert.match(benchRs, /fn model_card\(reply: &Value\) -> \(bool, bool\)/, "the bench's card reader returns text again");
  assert.match(benchRs, /"kind": "id", "software": software, "has_card": has_card, "person_class": person_class/);
  assert.ok(!/p\.model\b/.test(appJs), "the desktop draws the module's model text again");
  // … and a model is named only where the flasher just wrote one whose
  // SHA-256 matched the pinned release (run, not read, in the test below).
  assert.match(appJs, /SecuraCV person detection, installed and checked/);
});

// ── each frontend's own drawing code, run on a fake page ───────────────────
// Regexes over the source say what a line looks like; these say what a
// person sees. Cut a frontend's real functions out of its file and run them
// against fakeDocument(). `real` holds what the test means to exercise: the
// functions it cut (declared inside the scope), the state they read, the
// lookups and answers it controls. Every other free name the cut code reaches
// is inert — a call draws an empty element, a property is inert again — so
// what the page shows is exactly what the cut functions drew. Runtime globals
// (String, JSON, atob …) stay real unless `real` shadows them.
function pageDoc() {
  const doc = fakeDocument();
  const proto = Object.getPrototypeOf(doc.createElement("i"));
  const append = proto.append;
  proto.append = function (...nodes) {
    append.apply(this, nodes.map((n) => (typeof n === "string" ? doc.createTextNode(n) : n)));
  };
  proto.appendChild = function (n) { this.append(n); return n; };
  proto.insertBefore = function (n) { this.append(n); return n; };
  proto.replaceWith = function (n) { if (this.parent) { this.parent.append(n); this.remove(); } };
  proto.after = function (n) {
    const p = this.parent;
    if (!p) return;
    if (n.parent) n.remove();
    n.parent = p;
    p.children.splice(p.children.indexOf(this) + 1, 0, n);
  };
  Object.defineProperty(proto, "nextElementSibling", {
    get() { return this.parent ? this.parent.children[this.parent.children.indexOf(this) + 1] || null : null; },
  });
  proto.focus = proto.select = function () {};
  return doc;
}
function runCut(sources, names, real) {
  const doc = real.document;
  const inert = new Proxy(function () {}, {
    apply: () => doc.createElement("i"),
    construct: () => doc.createElement("i"),
    get: (_, k) => (k === Symbol.toPrimitive ? () => "" : k === "then" ? undefined : inert),
  });
  const scope = new Proxy(real, {
    has: (t, k) => k in t || (typeof k === "string" && !(k in globalThis)),
    get: (t, k) => (k in t ? t[k] : k === Symbol.unscopables ? undefined : inert),
    set: (t, k, v) => { t[k] = v; return true; },
  });
  return new Function("scope", `with (scope) {\n${sources.join("\n")}\nreturn { ${names.join(", ")} };\n}`)(scope);
}
const ticks = async (n = 8) => { for (let i = 0; i < n; i++) await new Promise((r) => setImmediate(r)); };
const nodesOf = (root) => [...root.walk()];
const flashJsEl = (src) => {
  const m = src.match(/const el = \(tag, cls, text\) => \{[\s\S]*?\n\};/);
  assert.ok(m, "couldn't cut the el() helper");
  return m[0];
};

// A hardware address whose bytes no other text on these cards contains, and
// a test that matches any two neighboring octets in any case, with or
// without a separator — a tail, a raw copy, an upper-cased copy all count.
const HW = "a7:3e:5b:c1:9d:f4";
const HW_SHOWN = "A7:3E:5B:C1:9D:F4";
const HW_BYTES = /a7[^0-9a-z]?3e|3e[^0-9a-z]?5b|5b[^0-9a-z]?c1|c1[^0-9a-z]?9d|9d[^0-9a-z]?f4/i;

// Everything a page puts in front of a person: its text, and every node's
// attributes and title (a hover tooltip, a label a screen reader speaks).
const drawnText = (root) => [root.textContent, ...nodesOf(root).flatMap((n) =>
  [n.title, n.ariaLabel, ...Object.values(n.attrs || {}), ...Object.values(n.dataset || {})])]
  .filter((v) => v != null).map(String).join("\n");

function holdsTheAddressBack(label, root, copied) {
  assert.ok(!HW_BYTES.test(drawnText(root)), `${label}: hardware-address bytes are on screen before Show`);
  const shows = nodesOf(root).filter((n) => n.tagName === "button" && n.textContent === "Show");
  assert.strictEqual(shows.length, 1, `${label}: the address has one Show, and only one`);
  // Copy what can be copied first: never the address, before or after Show.
  const copyAll = () => {
    for (const b of nodesOf(root).filter((n) => n.tagName === "button" && /^Copy/.test(n.textContent))) b.fire("click");
  };
  copyAll();
  shows[0].fire("click");
  copyAll();
  assert.ok(root.textContent.includes(HW_SHOWN), `${label}: Show must reveal the whole address`);
  const addr = nodesOf(root).find((n) => n.children.length === 0 && n._text === HW_SHOWN);
  assert.ok(addr && /no-?copy/.test(addr.className), `${label}: the revealed address must be marked no-copy`);
  for (const t of copied) assert.ok(!HW_BYTES.test(t), `${label}: a copy carries the hardware address`);
}

test("the hardware address: each flasher's own drawing code shows no byte of it until Show, and copies none", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const facts = { chip: "ESP32-S3", chipDesc: "esp32s3 (revision v0.2)", flashBytes: 8 << 20 };

  // The browser: the hello card (L1) and the Board report.
  {
    const doc = pageDoc();
    const copied = [];
    const real = {
      document: doc, core, navigator: { clipboard: { writeText: async (t) => { copied.push(t); } } },
      localStorage: { getItem: () => null, setItem() {} },
      state: { catalog, chip: "ESP32-S3", chipDesc: facts.chipDesc, flashBytes: 8 << 20, mac: HW,
        chipFuses: undefined, current: null, passport: null, roster: [], ptRead: "ok", bootHeader: undefined },
    };
    const page = runCut([flashJsEl(flashJs), ...["fact", "macRevealFact", "aboutFacts", "renderAboutBoard",
      "phaseConnected", "renderReport"].map((n) => fnSource(flashJs, n))], ["phaseConnected", "renderReport"], real);
    holdsTheAddressBack("browser hello card", page.phaseConnected(), copied);
    await ticks();
    copied.length = 0;
    const report = page.renderReport({ chip: "ESP32-S3", flashBytes: 8 << 20, partitions: [],
      slots: [{ label: "app0", subtype: 0x10, empty: false, project: "canary_wap", version: "2.4.16", active: true }],
      about: core.aboutBoardGroups(facts) });
    holdsTheAddressBack("browser Board report", report, copied);
    await ticks();
  }

  // The desktop: the This Board card (L1) and the health report.
  {
    const doc = pageDoc();
    const copied = [];
    const nodes = { "this-board": doc.createElement("section"), "health-report": doc.createElement("section") };
    const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
    const real = {
      document: doc, $: (id) => nodes[id] || null, prefs: {}, savePrefs() {},
      navigator: { clipboard: { writeText: async (t) => { copied.push(t); } } },
      state: { chip: "ESP32-S3", portKind: "esp32", chipDesc: facts.chipDesc, flashBytes: 8 << 20, mac: HW,
        chipFuses: undefined, passport: null, resident: null, passportPending: false, appInfo: { version: "0" } },
    };
    const page = runCut([m[1], ...["aboutFacts", "macRevealRow", "renderThisBoard", "renderHealthReport"]
      .map((n) => fnSource(appJs, n))], ["renderThisBoard", "renderHealthReport"], real);
    page.renderThisBoard();
    holdsTheAddressBack("desktop This Board card", nodes["this-board"], copied);
    await ticks();
    page.renderHealthReport({ chip: "ESP32-S3", flashBytes: 8 << 20,
      slots: [{ label: "app0", subtype: 0x10, empty: false, project: "canary_wap", version: "2.4.16", active: true }] });
    holdsTheAddressBack("desktop health report", nodes["health-report"], []);
  }
});

test("the hardware address: both flashers' reveal rows carry the help dot that says why it is hidden", async () => {
  // The same settings_help.mac text, drawn by each flasher's own help dot.
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const help = catalog.settings_help.mac;
  assert.ok(help && help.what, "flash.json lost settings_help.mac");
  {
    const doc = pageDoc();
    const page = runCut([flashJsEl(flashJs), ...["helpDot", "macRevealFact"].map((n) => fnSource(flashJs, n))], ["macRevealFact"],
      { document: doc, core, state: { catalog, mac: HW }, installHelpManagers() {} });
    const row = page.macRevealFact();
    const what = row.find((n) => /\bflash-help-what\b/.test(n.className));
    assert.strictEqual(what && what.textContent, help.what, "the browser's Hardware address row lost its help dot");
    assert.ok(!HW_BYTES.test(drawnText(row)), "the browser's row draws the address before Show");
  }
  {
    const doc = pageDoc();
    const overrides = appJs.match(/const HELP_DESKTOP_OVERRIDES = \{[\s\S]*?\n\};/);
    assert.ok(overrides, "app.js moved HELP_DESKTOP_OVERRIDES");
    const page = runCut([overrides[0], ...["helpTopic", "helpDot", "macRevealRow"].map((n) => fnSource(appJs, n))], ["macRevealRow"],
      { document: doc, state: { catalog, mac: HW } });
    for (const cls of ["tb-cell", "health-row"]) {
      const row = page.macRevealRow(cls);
      const dot = row.find((n) => n.tagName === "button" && n.className === "help-dot");
      assert.ok(dot, `the desktop's Hardware address row (${cls}) has no help dot`);
      dot.fire("click");
      const pop = row.find((n) => n.className === "help-pop");
      assert.strictEqual(pop && pop.textContent, help.what, `the desktop's help dot (${cls}) says something else`);
      assert.ok(!HW_BYTES.test(drawnText(row)), `the desktop's row (${cls}) draws the address before Show`);
    }
  }
});

test("the hardware address: both nursery strips draw no byte of it, though both keep it", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const now = Date.now();

  // The browser's roster: each entry keeps the address to recognize the
  // board if it comes back (flash.js, core.rosterAdd); the strip shows none.
  {
    const doc = pageDoc();
    const roster = core.rosterAdd([], { t: now, mac: core.formatMac(HW.replace(/:/g, "")), product: "Canary Sense",
      version: "2.4.16", preset: "Porch", wifi: true });
    assert.ok(HW_BYTES.test(JSON.stringify(roster)), "the roster no longer keeps the address; this test proves less");
    const page = runCut([flashJsEl(flashJs), fnSource(flashJs, "renderRosterStrip")], ["renderRosterStrip"],
      { document: doc, core, state: { roster } });
    const strip = page.renderRosterStrip();
    assert.match(strip.textContent, /#1Canary Sense v2\.4\.16 · dialed: Porch · WiFi baked ✓/, "the strip never drew its row");
    assert.ok(!HW_BYTES.test(drawnText(strip)), `the browser's nursery strip draws the address: ${drawnText(strip)}`);
    // Its fineprint says what the strip keeps, the way its help dot does.
    assert.ok(!/only public facts/.test(strip.textContent), "the strip still calls what it keeps only public facts");
    assert.match(strip.textContent, /hardware address, only to recognize it if it comes back — never shown/);
  }

  // The desktop's strip draws its rows as one HTML string; the stand-in page
  // keeps that string to read, attributes and all.
  {
    const doc = pageDoc();
    const box = doc.createElement("div");
    Object.defineProperty(box, "innerHTML", { get() { return this._html || ""; }, set(v) { this._html = String(v); } });
    const book = { [HW]: { mac: HW, deviceId: "front-door", productLabel: "Canary Sense", fw: "2.4.16", flashedAt: now,
      wifiBaked: true, hasToken: true } };
    const page = runCut([fnSource(appJs, "esc"), fnSource(appJs, "renderNurseryStrip")], ["renderNurseryStrip"],
      { document: doc, $: (id) => (id === "nursery-strip" ? box : null), state: { sessionStart: now - 1000 }, bookAll: () => book });
    page.renderNurseryStrip();
    assert.match(box.innerHTML, /front-door/, "the desktop's strip never drew its row");
    assert.ok(!HW_BYTES.test(box.innerHTML), `the desktop's nursery strip draws the address: ${box.innerHTML}`);
  }
});

// The desktop's report builder and its scrubbers, cut from app.js as one run:
// the hardware-address mask, the tail lists, sanitizeLogTail and the builder.
function deskReportBlock(appJs) {
  const builder = fnSource(appJs, "buildDiagnosticReport");
  const from = appJs.indexOf("const HW_ADDRESS_RE");
  assert.ok(from >= 0 && from < appJs.indexOf(builder), "app.js moved its address mask away from the report builder");
  return appJs.slice(from, appJs.indexOf(builder) + builder.length);
}

test("an espflash failure: the address its board info printed reaches no screen, event log or report", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  // espflash 3.3.0 prints its board info, "MAC address: …" included, right
  // after connecting on write-bin and read-flash, and a failure prints only a
  // few lines after it. The engine masks the errors it builds from that
  // output (flash.rs and rescue.rs, cargo test); this holds each frontend's
  // own layer to the same rule, for an error that reaches it unmasked. The
  // errors are written from the engine's own format strings.
  const writeFmt = /pub fn write_failure\(code: i32, tail: &\[String\]\) -> String \{\s*format!\(\s*"([^"]+)",/
    .exec(engineRs("flash"));
  const readFmt = /pub fn read_failure\(code: i32, offset: u32, log: &str\) -> String \{\s*format!\(\s*"([^"]+)",/
    .exec(engineRs("rescue"));
  assert.ok(writeFmt && readFmt, "write_failure's or read_failure's format string moved");
  const tail = [`MAC address:       ${HW}`,
    "[00:00:00] [========================================]      18/18      0x0",
    "[00:00:01] [========================================]     236/236     0x8000",
    "Error: espflash::timeout", "  × Error while running FlashData command",
    "  ╰─▶ Timeout while running FlashData command"];
  const writeErr = writeFmt[1].replace("{code}", "1").replace("\\n{}", "\n" + tail.join("\n"));
  const readErr = readFmt[1].replace("{code}", "1").replace("{offset:x}", "8000")
    .replace("{}", ["Chip type:         esp32s3 (revision v0.2)", `MAC address:       ${HW}`, "Error: espflash::timeout"].join("\n"));
  for (const e of [writeErr, readErr]) assert.ok(HW_BYTES.test(e) && e.includes("Error: espflash::timeout"), e);

  // 1. Both report builders: the address masked, espflash's words kept, and
  //    the error scrubbed the way the serial tail is, the same on both sides.
  const deskBuild = new Function(`${deskReportBlock(appJs)}\nreturn buildDiagnosticReport;`)();
  const errorOf = (r) => r.slice(r.indexOf("\nerror: ") + 8).split("\n--- ")[0];
  for (const error of [writeErr, readErr, `write failed\n[PROV]   API TOKEN : 0123456789abcdef\nDevice ID: front-door-${HW}`]) {
    const reports = { desktop: deskBuild({ stage: "install", error }), browser: core.buildDiagnosticReport({ stage: "install", error }) };
    for (const [who, r] of Object.entries(reports)) {
      assert.ok(!HW_BYTES.test(r), `${who}'s diagnostic report carries the hardware address:\n${r}`);
      assert.ok(!r.includes("0123456789abcdef"), `${who}'s diagnostic report carries a credential from the error`);
      if (error !== writeErr) continue;
      assert.ok(r.includes(`MAC address:       ${core.HW_ADDRESS_MASK}`), `${who}: the masked line should stay readable`);
      assert.ok(r.includes("Error: espflash::timeout") && r.includes("espflash exited with code 1"),
        `${who} lost espflash's own words`);
    }
    assert.strictEqual(errorOf(reports.desktop), errorOf(reports.browser), "the two builders scrub an error differently");
  }

  // 2. The desktop: a failed install, the report copied from its card, and a
  //    failed health check — each drawn, and kept in the event log the About
  //    view lists (prefs, saved in the webview's storage).
  const doc = pageDoc();
  const nodes = {};
  const copied = [];
  const prefs = {};
  const strip = { begin() {}, stage() {}, frac() {}, indet() {}, fail() {}, end() {} };
  const answers = { flash: writeErr, health_check: readErr };
  const real = {
    document: doc, $: (id) => (nodes[id] = nodes[id] || doc.createElement("div")), prefs, savePrefs() {}, renderAbout() {},
    state: { product: { id: "securacv-canary" }, flashBytes: 0, chip: "ESP32-S3", portKind: "esp32", port: "/dev/ttyACM0",
      catalog: {}, vision: {}, appInfo: { version: "0" }, busy: false },
    flashFitVerdict: () => ({ fits: true }), efuseStopFindings: () => [], readProvisioning: () => null,
    opFor: () => strip, listen: async () => () => {}, withBaudLadder: (fn) => fn(921600),
    // Tauri rejects an invoke with the command's Err text, a plain string.
    invoke: async (cmd) => { if (answers[cmd]) throw answers[cmd]; return null; },
    navigator: { platform: "test", userAgent: "test", clipboard: { writeText: async (t) => { copied.push(t); } } },
  };
  const page = runCut([deskReportBlock(appJs), ...["setStatus", "logEvent", "classifyFlashError"].map((n) => fnSource(appJs, n)),
    ...["onFlash", "copyDiagnosticReport", "onHealthCheck"].map((n) => "async " + fnSource(appJs, n))],
  ["onFlash", "copyDiagnosticReport", "onHealthCheck", "logEvent"], real);
  // The event log keeps an entry's first 160 characters, which reach into
  // espflash's tail for this failure: an unmasked entry would keep the address.
  assert.ok(HW_BYTES.test(`Flash failed [not-in-download]: ${writeErr}`.slice(0, 160)), "this failure proves less of the event log");
  await page.onFlash();
  assert.match(nodes["flash-result"].textContent, /espflash::timeout/, "the install never failed on the stand-in page");
  assert.ok(!HW_BYTES.test(nodes["flash-result"].textContent), `the install's failure line draws the address: ${nodes["flash-result"].textContent}`);
  await page.copyDiagnosticReport("install");
  assert.strictEqual(copied.length, 1, "the failed install's report was not copied");
  assert.match(copied[0], /espflash::timeout/);
  assert.ok(!HW_BYTES.test(copied[0]), `the copied diagnostic report carries the address:\n${copied[0]}`);
  real.state.chip = "ESP32-S3"; // the install's finally lets the watcher re-read the board
  await page.onHealthCheck();
  assert.match(nodes["health-result"].textContent, /reading 0x8000/, "the health check never failed on the stand-in page");
  assert.ok(!HW_BYTES.test(nodes["health-result"].textContent), `the health check's failure draws the address: ${nodes["health-result"].textContent}`);
  page.logEvent("err", `espflash said: MAC address: ${HW}`);
  assert.strictEqual(prefs.log.length, 3, "the install, the health check and the direct entry should each be logged");
  for (const entry of prefs.log) assert.ok(!HW_BYTES.test(entry.msg), `the event log keeps the address: ${entry.msg}`);
  // An entry saved before the store masked (earlier versions kept its first
  // 160 characters as they came) is masked where the About view lists it.
  {
    let html = "";
    const sink = { set innerHTML(v) { html = v; }, querySelectorAll: () => [] };
    const about = runCut([appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/)[1],
      ...["esc", "eventLogText", "renderAbout"].map((n) => fnSource(appJs, n))], ["renderAbout"], {
      document: doc, $: (id) => (id === "about-body" ? sink : null), state: { appInfo: { version: "0" } },
      prefs: { log: [{ t: 0, kind: "err", msg: `Flash failed [not-in-download]: MAC address: ${HW}` },
        // …or with no label in front of it, where only the whole-address mask reaches
        { t: 0, kind: "err", msg: `Rescue failed: the board at ${HW} stopped answering` },
        // …or cut partway through the address, which no whole-address mask matches
        { t: 0, kind: "err", msg: `Health check failed: MAC address:       ${HW.slice(0, 7)}` },
        // …or a safety copy named by the address's last six hex digits, whole or cut
        { t: 0, kind: "ok", msg: `Safety copy saved: /Users/me/backups/canary-${HW.replace(/:/g, "").slice(-6)}-20261020T1230-backup.bin` },
        { t: 0, kind: "ok", msg: `Safety copy saved: /Users/me/backups/canary-${HW.replace(/:/g, "").slice(-6, -2)}` }] },
      ATLAS_LINKS: [], ATLAS_FAMILY_GROUP: "family", secretStore: { where: () => "the keychain" },
    });
    about.renderAbout();
    assert.match(html, /Flash failed \[not-in-download\]: MAC address: /, "the About view never listed the saved entry");
    assert.match(html, /Health check failed: MAC address: /, "the About view never listed the cut entry");
    assert.match(html, /Rescue failed: the board at .* stopped answering/, "the About view never listed the unlabeled entry");
    assert.strictEqual(html.split("Safety copy saved: /Users/me/backups/canary-").length - 1, 2,
      "the About view never listed the safety-copy entries");
    // Today's names are the day and a counter, and stay readable.
    const today = runCut([fnSource(appJs, "eventLogText")], ["eventLogText"],
      { document: doc, maskHardwareAddress: (t) => t, HW_ADDRESS_MASK: "[hidden]" });
    for (const keep of ["Safety copy saved: /b/canary-2026-10-22-1-backup.bin", "MAC check: clear", "MAC: clear",
                        "canary-beef12 hatched"]) {
      assert.strictEqual(today.eventLogText(keep), keep);
    }
    assert.ok(!HW_BYTES.test(html), "the About view lists an earlier version's event-log entry with the address in it");
  }

  // 3. The Lab's native bench: its failure card's Details, as well as its log.
  const run = await runNativeBench({ flashAnswers: Array(8).fill(writeErr) });
  try {
    await run.pick("securacv-canary");
    const details = run.mount.find((n) => n.tagName === "details" && n.textContent.startsWith("Details"));
    assert.ok(details && /espflash::timeout/.test(details.textContent), "the bench's failure card lost its Details");
    assert.ok(!HW_BYTES.test(run.text()), "the Lab bench's failure card draws the hardware address");
  } finally {
    run.close();
  }
});

test("the desktop's This Board card: a new board never wears the last board's image or fuses", async () => {
  const intake = await import(pathToFileURL(join(CANARY, "assets/intake.js")).href);
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const doc = pageDoc();
  const nodes = { "this-board": doc.createElement("section"), passport: doc.createElement("div") };
  const A = "/dev/ttyACM0", B = "/dev/ttyACM1";
  const boards = {
    [A]: { chip: "ESP32-S3", chip_desc: "esp32s3 (revision v0.2)", flash_bytes: 8 << 20, mac: "aa:bb:cc:00:00:01" },
    [B]: { chip: "ESP32-C3", chip_desc: "esp32c3 (revision v0.4)", flash_bytes: 4 << 20, mac: "aa:bb:cc:00:00:02" },
  };
  // Board A's passport answers at once; board B's is still being read (8-25 s on hardware).
  const passports = {
    [A]: { blank: false, resident: { version: "2.4.16-wap", projectName: "canary_wap", idf: "v5.5.1", secureVersion: 0,
      elfSha256Prefix: "a0a1a2a3a4a5a6a7" },
    bootloader: { flashMode: "DIO", flashMhz: 80, flashMhzEither: null, flashBytes: 8 << 20, codes: { mode: 2, size: 3, freq: 15 } } },
  };
  const timers = [];
  const state = { port: A, catalog: { flash_baud: 921600 }, chip: null, passport: null, resident: null,
    passportPending: false };
  const real = {
    document: doc, state, $: (id) => nodes[id] || doc.createElement("div"), prefs: {}, savePrefs() {},
    WE2_VID: 0x1a86, WE2_PID: 0x55d3, matchProjectToProduct: () => null,
    setTimeout: (fn) => { timers.push(fn); return timers.length; }, clearTimeout() {},
    invoke: async (cmd, args) => {
      if (cmd === "detect_chip") return boards[args.port];
      if (cmd === "board_passport") return passports[args.port] || new Promise(() => {});
      throw new Error(`unexpected ${cmd}`);
    },
  };
  const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
  const page = runCut([m[1], ...["aboutFacts", "macRevealRow", "renderThisBoard", "renderPassport"].map((n) => fnSource(appJs, n)),
    "async " + fnSource(appJs, "readPassport"), "async " + fnSource(appJs, "identify")], ["identify"], real);
  const card = () => nodes["this-board"].textContent;
  const strip = () => nodes.passport.textContent;

  await page.identify({ name: A, vid: 0x303a, pid: 0x1001 });
  await ticks();
  assert.match(card(), /canary_wap 2\.4\.16-wap/, "board A's image should be on its card");
  assert.match(strip(), /Running canary_wap 2\.4\.16-wap/, "board A's passport strip should name its image");
  // Board A's fuses were read (the "Read the chip's fuses" button).
  state.chipFuses = intake.decodeChipFuses("ESP32-S3", { PKG_VERSION: 0, FLASH_CAP: 1, FLASH_VENDOR: 2, BLK_VERSION_MAJOR: 1 });
  timers.length = 0;

  // The picker switches to board B (the port-select handler's resets).
  Object.assign(state, { port: B, portInfo: null, portKind: null, chip: null, failedPort: null });
  page.identify({ name: B, vid: 0x303a, pid: 0x1001 });
  await ticks();
  const text = card();
  assert.match(text, /ESP32-C3 · v0\.4/, "board B's chip should be on the card");
  for (const stale of ["canary_wap", "2.4.16", "a0a1a2a3", "v5.5.1", "the chip has", "DIO", "QFN56", "Present"]) {
    assert.ok(!text.includes(stale), `board B's card shows board A's "${stale}" while B's passport is read`);
  }
  assert.match(text, /Installed imageNot checked yet/, "B's image is not read yet, and the card says so");
  // The passport strip beside the card lets go of board A too.
  assert.ok(!/canary_wap|2\.4\.16/.test(strip()), `the passport strip shows board A's image under board B: ${strip()}`);
  assert.match(text, /PackageNot checked yet/, "B's fuses are not read yet, and the card says so");

  // B's passport read never answers: the 25 s settle gives up, and the card
  // says the image was not checked rather than "not checked yet" for good.
  assert.strictEqual(timers.length, 1, "B's passport read should arm one settle timer");
  timers[0]();
  assert.match(card(), /Installed imageNot checked(?! yet)/, "the settle timer must redraw the card");
});

test("the Grove module: a model is named only after this flasher's SHA-256-matched write, never from the module's card", async () => {
  const we2Flash = read(join(CANARY, "assets/we2-flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const we2 = await import(pathToFileURL(join(CANARY, "assets/we2-core.js")).href);
  const card = (c) => (c ? { data: Buffer.from(JSON.stringify(c)).toString("base64") } : { code: 0 });
  const CARDS = {
    none: null,
    other: { name: "Traffic", classes: ["car", "bus"] },
    // A card's own text can say anything, including a capability this
    // project refuses; it must never reach the screen.
    person: { name: "Face Recognition v2", description: "identify who is at the door", classes: ["person"] },
  };
  const at = (info, sent) => ({ start() {}, stop() {},
    cmd: async (c) => { sent.push(c); return c === "INFO?" ? info : { code: 0 }; } });
  const browserPage = (info, sent = []) => {
    const doc = pageDoc();
    return { doc, page: runCut([flashJsEl(we2Flash), fnSource(we2Flash, "phaseModuleDone"),
      fnSource(we2Flash, "phaseModuleBench")], ["phaseModuleDone", "phaseModuleBench"], {
      document: doc, burnCardBase64: we2.burnCardBase64, makeAtClient: () => at(info, sent),
      wakeModule: async () => ({ data: { software: "2025.01.02" } }), sleep: async () => {},
      visionSession: { parts: () => ({ esp32: false, we2: true }) },
      mountBench: () => ({ node: doc.createElement("div"), stop: async () => {} }),
    }) };
  };
  const ctx = { catalog: { products: [] }, back() {} };
  const s = { t: { close: async () => {} } };

  // After the burn: the pinned release is named, a file the person picked is
  // not — on screen, and in the card the burn stores on the module, which
  // every later reader of AT+INFO? (both benches, SenseCraft) takes as the
  // module's claim about its model.
  for (const pinned of [true, false]) {
    const sent = [];
    const { page } = browserPage({ code: 0 }, sent);
    const box = page.phaseModuleDone(ctx, s, { pinned, version: "1", sha256: "ab".repeat(32) });
    await ticks();
    const proof = nodesOf(box).find((n) => n.tagName === "p" && /^✓ /.test(n._text));
    assert.ok(proof, `pinned=${pinned}: the proof line never landed`);
    // The step after the burn writes one card (a file the person picked
    // gets it before the burn too: the test below).
    const writes = sent.filter((c) => c.startsWith("INFO="));
    assert.strictEqual(writes.length, 1, `pinned=${pinned}: the step after the burn should write exactly one card`);
    const stored = /^INFO="([A-Za-z0-9+/=]+)"$/.exec(writes[0]);
    assert.ok(stored, `pinned=${pinned}: the card write is not INFO="<base64>": ${writes[0]}`);
    const written = JSON.parse(Buffer.from(stored[1], "base64").toString("utf8"));
    assert.strictEqual(written.sha256, "ab".repeat(32), "the card should carry the bytes' own SHA-256");
    if (pinned) {
      assert.match(proof._text, /^✓ SecuraCV person detection, installed and checked\. /);
      assert.match(proof._text, /carries our model card/);
      assert.deepStrictEqual(written, we2.modelInfoJson({ version: "1", sha256: "ab".repeat(32) }));
    } else {
      assert.match(proof._text, /^✓ Your model file, installed\. /);
      assert.ok(!/person|SecuraCV|model card/i.test(proof._text), "a local file must never be named for the person");
      assert.ok(!written.name, `a local file's card names a model: ${written.name}`);
      assert.deepStrictEqual(written.classes, [], "a local file's card must list no class");
      assert.ok(!/person|SecuraCV/i.test(JSON.stringify(written)), `a local file's card claims our model: ${JSON.stringify(written)}`);
      // A later bench on the same module reads that card back: "object" labels.
      const later = browserPage({ data: stored[1] }).page.phaseModuleBench(ctx, s);
      await ticks();
      const idLine = nodesOf(later).find((n) => n.tagName === "p" && /^SSCMA firmware /.test(n._text));
      assert.match(idLine._text, /lists no person class — labels will read “object”/);
    }
  }

  // The bench reads a card it did not write: it picks the label map and
  // names nothing. The desktop's line is the browser's, word for word.
  const desk = (p) => {
    const doc = pageDoc();
    const nodes = { "bench-id": doc.createElement("p") };
    const liveBench = { session: null };
    const page = runCut([fnSource(appJs, "onBenchEvent")], ["onBenchEvent"],
      { document: doc, $: (id) => nodes[id] || doc.createElement("i"), liveBench });
    page.onBenchEvent({ payload: { kind: "id", software: "2025.01.02", ...p } });
    return { line: nodes["bench-id"].textContent, liveBench };
  };
  for (const [kind, c] of Object.entries(CARDS)) {
    const { page } = browserPage(card(c));
    const box = page.phaseModuleBench(ctx, s);
    await ticks();
    const idLine = nodesOf(box).find((n) => n.tagName === "p" && /^SSCMA firmware /.test(n._text));
    assert.ok(idLine, `${kind}: the bench's id line never landed`);
    const line = idLine._text;
    assert.ok(!/person detection|SecuraCV|Face|identify|Traffic/i.test(line), `${kind}: the bench names a model: ${line}`);
    assert.match(line, kind === "person" ? /labels will read “person”/ : /labels will read “object”/);
    if (kind === "person") assert.match(line, /module’s own claim/);
    const d = desk({ has_card: !!c, person_class: kind === "person" });
    assert.strictEqual(d.line + ". Start the preview below.", line, `${kind}: the desktop's bench line drifted`);
    assert.strictEqual(d.liveBench.personClass, kind === "person");
  }
  // The desktop names a model in one place, after flash_vision_module, which
  // takes only the pinned manifest and refuses any other bytes.
  assert.strictEqual(appJs.split("SecuraCV person detection").length - 1, 1);
  assert.match(fnSource(appJs, "onFlashModule"),
    /invoke\("flash_vision_module", \{\s*port: state\.port,\s*manifestUrl: state\.catalog\.we2_module\.manifest_url,\s*\}\)/);
});

test("the Grove module: a burn of a file the person picked replaces the card an earlier pinned burn left, or says it could not", async () => {
  // The stand-in module's SSCMA firmware keeps one model card, apart from
  // the model a burn writes, so a burn alone leaves the card it found. This
  // one starts with SecuraCV's person card (an earlier pinned burn). It
  // answers AT while `up` is true. A card write it takes gets an ok reply;
  // with `ack` false it gets none, and with `ack` "err" it gets an error
  // reply (the SSCMA frame's nonzero code), and either way nothing is stored.
  // With `burnFails` the burn throws before it writes the module; `retry`
  // then presses "Reset and retry" with the module as `retry.upBefore` says
  // the failed burn left it, and burns again as `retry` says. A retry that
  // fails can carry its own `retry`, so a chain of attempts runs.
  const we2Flash = read(join(CANARY, "assets/we2-flash.js"));
  const we2 = await import(pathToFileURL(join(CANARY, "assets/we2-core.js")).href);
  const OURS = Buffer.from(JSON.stringify(we2.modelInfoJson({ version: "1", sha256: "cd".repeat(32) }))).toString("base64");
  const run = async ({ pinned, upBefore, upAfter, ack = true, burnFails = false, retry = null }) => {
    const mod = { card: OURS, up: upBefore };
    const order = [];
    let burn = { upAfter, burnFails };
    const at = { start() {}, stop() {}, onEvent() {},
      cmd: async (c) => {
        const w = /^INFO="([A-Za-z0-9+/=]+)"$/.exec(c);
        order.push(w ? "INFO=" : c);
        if (!mod.up) return null;
        if (c === "INFO?") return { code: 0, data: mod.card };
        if (w) {
          if (!ack) return null;
          if (ack === "err") return { code: 3 };
          mod.card = w[1];
          return { code: 0 };
        }
        return { code: 0, data: { software: "2025.01.02" } };
      } };
    class We2Flasher {
      async flashModel() { order.push("BURN"); if (burn.burnFails) throw new Error("no bootloader menu"); mod.up = burn.upAfter; }
    }
    const doc = pageDoc();
    const phases = [];
    const page = runCut([flashJsEl(we2Flash), ...["phaseModuleFlash", "phaseModuleDone", "phaseModuleBench"].map((n) => fnSource(we2Flash, n))],
      ["phaseModuleFlash", "phaseModuleBench"], {
        document: doc, burnCardBase64: we2.burnCardBase64, makeAtClient: () => at, We2Flasher,
        wakeModule: async () => (mod.up ? { code: 0, data: { software: "2025.01.02" } } : null), sleep: async () => {},
        visionSession: { parts: () => ({ esp32: false, we2: true }), markDone() {} },
        mountBench: () => ({ node: doc.createElement("div"), stop: async () => {} }),
      });
    const ctx = { catalog: { products: [], we2_module: { model_addr: "0x00400000" } }, back() {}, setPhase: (p) => { phases.push(p); } };
    const s = { t: { close: async () => {} } };
    const flashing = page.phaseModuleFlash(ctx, s,
      { bytes: new Uint8Array(4), label: "mine.tflite", sha256: "ab".repeat(32), version: "1", pinned });
    await ticks(20);
    const card = () => JSON.parse(Buffer.from(mod.card, "base64").toString("utf8"));
    const failed = (box) => {
      assert.match(box.textContent, /✗ no bootloader menu/, "the failed burn's screen never landed");
      return nodesOf(box).filter((n) => n.tagName === "p").map((n) => n._text).join("\n");
    };
    let shown = phases[0];
    // Each failed attempt's screen, in order: `said` is the first's,
    // `retrySaid` the second's.
    const failures = [];
    if (burnFails) {
      assert.ok(!shown, "a burn that threw reached its done screen");
      failures.push(failed(flashing));
      let at = flashing;
      for (let next = retry; next; next = next.retry) {
        mod.up = next.upBefore;
        burn = next;
        const again = nodesOf(at).find((n) => n.tagName === "button" && n._text === "Reset and retry");
        assert.ok(again, `attempt ${failures.length}'s failure screen offers no retry`);
        const before = phases.length;
        again.fire("click");
        await ticks(20);
        at = phases[before];
        assert.ok(at, `attempt ${failures.length + 1} never drew its screen`);
        if (!next.burnFails) { shown = phases[before + 1]; break; }
        assert.strictEqual(phases.length, before + 1, `attempt ${failures.length + 1} threw and reached its done screen`);
        failures.push(failed(at));
      }
      if (!shown) return { order, said: failures[0], retrySaid: failures[1], failures, card: card() };
    }
    const said = failures[0] || null;
    assert.ok(shown, "the burn never reached its done screen");
    const proof = nodesOf(shown).map((n) => n._text || "").find((t) => /AT answer|^✓ |test inference/.test(t));
    assert.ok(proof, "the done screen's proof line never landed");
    mod.up = true; // replugged: a later bench reads whatever card the module kept
    const later = page.phaseModuleBench(ctx, s);
    await ticks();
    const idLine = nodesOf(later).map((n) => n._text || "").find((t) => /^SSCMA firmware /.test(t));
    return { order, said, failures, proof, idLine, card: card() };
  };
  const KEPT = /may still carry the model card an earlier burn stored/;
  const UNDER_LOCAL = /until a burn completes, whatever model the module holds sits under a card that names no model/;

  // A file the person picked: its card goes on before the burn, while the
  // module still answers, so a module that never answers after the burn
  // keeps no card that speaks for an earlier model.
  {
    const r = await run({ pinned: false, upBefore: true, upAfter: false });
    assert.ok(r.order.indexOf("INFO=") >= 0 && r.order.indexOf("INFO=") < r.order.indexOf("BURN"),
      `a local file's card should go on before the burn: ${r.order.join(" ")}`);
    assert.deepStrictEqual(r.card, we2.localModelInfoJson({ sha256: "ab".repeat(32) }));
    assert.match(r.proof, /No AT answer after reboot/);
    assert.ok(!KEPT.test(r.proof), `the card did go on, but the done screen says it may not have: ${r.proof}`);
    assert.match(r.idLine, /lists no person class — labels will read “object”/);
  }
  // It answers neither before nor after the burn, or answers but never takes
  // the card, with no reply or with an error reply (before the burn, after it,
  // or both times): the done screen says the module may still carry the card
  // an earlier burn stored, which is what a later bench reads.
  for (const c of [{ upBefore: false, upAfter: false }, { upBefore: false, upAfter: true, ack: false },
    { upBefore: true, upAfter: false, ack: false }, { upBefore: true, upAfter: false, ack: "err" },
    { upBefore: false, upAfter: true, ack: "err" }, { upBefore: true, upAfter: true, ack: false },
    { upBefore: true, upAfter: true, ack: "err" }]) {
    const r = await run({ pinned: false, ...c });
    assert.match(r.proof, KEPT, `${JSON.stringify(c)}: the module kept the old card and the done screen says nothing: ${r.proof}`);
    assert.match(r.idLine, /labels will read “person”/, "the stand-in module should still hold the earlier card");
  }
  // It answers only after the burn, or both times: the card goes on after it
  // (and before it too, when it answered then).
  for (const upBefore of [false, true]) {
    const r = await run({ pinned: false, upBefore, upAfter: true });
    assert.deepStrictEqual(r.order.filter((c) => c === "INFO=" || c === "BURN"), upBefore ? ["INFO=", "BURN", "INFO="] : ["BURN", "INFO="],
      `upBefore ${upBefore}: ${r.order.join(" ")}`);
    assert.deepStrictEqual(r.card, we2.localModelInfoJson({ sha256: "ab".repeat(32) }));
    assert.match(r.proof, /^✓ Your model file, installed\. /);
    assert.ok(!KEPT.test(r.proof), r.proof);
  }
  // A burn that fails after its card went on leaves the module's model, an
  // earlier pinned one among them, under the card that claims nothing: the
  // failure screen says so. When no card went on first in any attempt at this
  // burn (no answer, no reply, or an error reply), the module keeps its
  // earlier card and model together, and a pinned burn writes no card before
  // its bytes: nothing to say then.
  {
    const r = await run({ pinned: false, upBefore: true, upAfter: true, burnFails: true });
    assert.deepStrictEqual(r.card, we2.localModelInfoJson({ sha256: "ab".repeat(32) }));
    assert.match(r.said, UNDER_LOCAL, `the card claims nothing over the old model and the failure screen says nothing: ${r.said}`);
  }
  for (const c of [{ pinned: false, upBefore: false }, { pinned: false, upBefore: true, ack: false },
    { pinned: false, upBefore: true, ack: "err" }, { pinned: true, upBefore: true }]) {
    const r = await run({ ...c, upAfter: true, burnFails: true });
    assert.deepStrictEqual(r.card, JSON.parse(Buffer.from(OURS, "base64").toString("utf8")), `${JSON.stringify(c)}: the card moved`);
    assert.ok(!UNDER_LOCAL.test(r.said), `${JSON.stringify(c)}: no card went on, but the failure screen says one did: ${r.said}`);
  }
  // "Reset and retry" after a burn that failed inside the bootloader: the
  // module answers no AT until it is reset, so the retry cannot write the card
  // again, but the card the first attempt wrote is still on. A second failure
  // says so too, and a retry that completes with no answer after it does not
  // say the module kept an earlier burn's card: it carries this file's.
  {
    const r = await run({ pinned: false, upBefore: true, upAfter: true, burnFails: true,
      retry: { upBefore: false, burnFails: true } });
    assert.match(r.said, UNDER_LOCAL);
    assert.deepStrictEqual(r.card, we2.localModelInfoJson({ sha256: "ab".repeat(32) }));
    assert.match(r.retrySaid, UNDER_LOCAL, `the first attempt's card is still on and the retry's failure says nothing: ${r.retrySaid}`);
  }
  {
    const r = await run({ pinned: false, upBefore: true, upAfter: true, burnFails: true,
      retry: { upBefore: false, upAfter: false, burnFails: false } });
    assert.match(r.proof, /No AT answer after reboot/);
    assert.ok(!KEPT.test(r.proof), `the first attempt's card is on, but the done screen says the module didn't take it: ${r.proof}`);
    assert.match(r.idLine, /lists no person class — labels will read “object”/);
  }
  {
    const r = await run({ pinned: false, upBefore: false, upAfter: true, burnFails: true,
      retry: { upBefore: false, burnFails: true } });
    assert.ok(!UNDER_LOCAL.test(r.retrySaid), `no attempt put a card on, but the retry's failure says one did: ${r.retrySaid}`);
    assert.deepStrictEqual(r.card, JSON.parse(Buffer.from(OURS, "base64").toString("utf8")));
  }
  // The flag rides every retry, not only the first: the first attempt's card
  // goes on and its burn fails, the next two fail inside the bootloader, and
  // the fourth completes with no answer after it. Each failure screen says
  // the card is on, and the done screen does not say the module kept an
  // earlier burn's card.
  {
    const inBootloader = (then) => ({ upBefore: false, upAfter: false, burnFails: true, retry: then });
    const r = await run({ pinned: false, upBefore: true, upAfter: true, burnFails: true,
      retry: inBootloader(inBootloader({ upBefore: false, upAfter: false, burnFails: false })) });
    assert.strictEqual(r.failures.length, 3, "three attempts should have failed");
    r.failures.forEach((said, i) => assert.match(said, UNDER_LOCAL,
      `attempt ${i + 1}: the first attempt's card is still on and this failure says nothing: ${said}`));
    assert.deepStrictEqual(r.order.filter((c) => c === "INFO=" || c === "BURN"), ["INFO=", "BURN", "BURN", "BURN", "BURN"],
      `only the first attempt could write the card: ${r.order.join(" ")}`);
    assert.match(r.proof, /No AT answer after reboot/);
    assert.ok(!KEPT.test(r.proof), `attempt 4: the first attempt's card is on, but the done screen says the module didn't take it: ${r.proof}`);
    assert.match(r.idLine, /lists no person class — labels will read “object”/);
  }
  // The pinned model's card waits for its bytes: before the burn the person
  // card would describe a model the module does not carry yet.
  {
    const r = await run({ pinned: true, upBefore: true, upAfter: true });
    assert.ok(r.order.indexOf("INFO=") > r.order.indexOf("BURN"), `the pinned card went on before its burn: ${r.order.join(" ")}`);
    assert.deepStrictEqual(r.card, we2.modelInfoJson({ version: "1", sha256: "ab".repeat(32) }));
    assert.match(r.proof, /carries our model card/);
  }
  // …and the done screen says the module carries it only when it acknowledged
  // the write: no reply, or an error reply, is not an acknowledgment.
  for (const nack of [false, "err"]) {
    const r = await run({ pinned: true, upBefore: true, upAfter: true, ack: nack });
    assert.match(r.proof, /^✓ SecuraCV person detection, installed and checked\. The module answered and ran one inference/);
    assert.ok(!/carries our model card/.test(r.proof), `ack ${nack}: an unacknowledged card write is reported as stored: ${r.proof}`);
    // The local file's warning is not the pinned model's: its "burn this
    // file again … its card goes on first" is false for a pinned burn.
    assert.ok(!KEPT.test(r.proof), `ack ${nack}: a pinned burn gets the local file's warning: ${r.proof}`);
  }
});

test("the Grove bench draws the labels its id line promised, on both flashers", async () => {
  // The id line (and, after a burn, the proof line) says which labels the
  // bench will draw; this runs the bench's own drawing code on a detection
  // and holds what it draws to that promise.
  const we2Flash = read(join(CANARY, "assets/we2-flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const we2 = await import(pathToFileURL(join(CANARY, "assets/we2-core.js")).href);
  const box = [120, 120, 40, 60, 92, 0];
  const canvasCtx = new Proxy({}, { get: (_, k) => (k === "measureText" ? () => ({ width: 10 }) : () => {}), set: () => true });
  const withCanvas = (doc) => {
    const make = doc.createElement;
    doc.createElement = (tag) => { const n = make(tag); if (tag === "canvas") n.getContext = () => canvasCtx; return n; };
    return doc;
  };
  const card = (c) => (c ? { data: Buffer.from(JSON.stringify(c)).toString("base64") } : { code: 0 });

  // The browser: the bench behind each phase, started, then one detection.
  const browser = async (phase, info, job) => {
    const doc = withCanvas(pageDoc());
    const handlers = [];
    const at = { start() {}, stop() {}, onEvent: (fn) => handlers.push(fn),
      cmd: async (c) => (c === "INFO?" ? info : c === "TSCORE?" || c === "TIOU?" ? { code: 0, data: 50 } : { code: 0 }) };
    const page = runCut([flashJsEl(we2Flash), ...["mountBench", phase].map((n) => fnSource(we2Flash, n))], [phase], {
      document: doc, burnCardBase64: we2.burnCardBase64, WE2_CLASSES: we2.WE2_CLASSES,
      stylizeDetections: we2.stylizeDetections, meterModel: we2.meterModel, makeAtClient: () => at,
      wakeModule: async () => ({ data: { software: "2025.01.02" } }), sleep: async () => {},
      visionSession: { parts: () => ({ esp32: false, we2: true }) },
    });
    const root = page[phase]({ catalog: { products: [], we2_module: {} }, back() {} }, { t: { close: async () => {} } }, job);
    await ticks();
    const promise = nodesOf(root).find((n) => n.tagName === "p" && /^(SSCMA firmware |✓ )/.test(n._text));
    assert.ok(promise, `${phase}: neither the id line nor the proof line landed`);
    const start = nodesOf(root).find((n) => n.tagName === "button" && n.textContent === "Start live preview");
    start.fire("click");
    await ticks();
    for (const fn of handlers) fn({ data: { boxes: [box] } });
    const drawn = nodesOf(root).find((n) => /we2-meter-label/.test(n.className)).textContent;
    return { promise: promise._text, drawn };
  };
  const label = (drawn) => (/\bperson\b/.test(drawn) ? "person" : /\bobject\b/.test(drawn) ? "object" : `neither: ${drawn}`);
  for (const [kind, c] of Object.entries({ none: null, other: { classes: ["car"] }, person: { classes: ["person"] } })) {
    const { promise, drawn } = await browser("phaseModuleBench", card(c), undefined);
    const promised = /labels will read “(\w+)”/.exec(promise)[1];
    assert.strictEqual(label(drawn), promised, `browser bench, ${kind} card: promised "${promised}", drew "${drawn}"`);
  }
  // After a burn: the pinned release is the person model; a file the person
  // picked is not known to be one, so its detections are "object".
  for (const pinned of [true, false]) {
    const { drawn } = await browser("phaseModuleDone", { code: 0 }, { pinned, version: "1", sha256: "ab".repeat(32) });
    assert.strictEqual(label(drawn), pinned ? "person" : "object", `browser, after a burn (pinned=${pinned}): drew "${drawn}"`);
  }

  // The desktop: the id event, then a detection event, through its own handler.
  for (const personClass of [true, false]) {
    const doc = pageDoc();
    const nodes = {};
    const canvas = doc.createElement("canvas");
    canvas.getContext = () => canvasCtx;
    const liveBench = { session: null, personClass: false, seen: false, thr: { tscore: 50, tiou: 45 }, lastFrame: null };
    const page = runCut(["const MODULE_CLASSES = [\"person\"];",
      ...["onBenchEvent", "benchClasses", "benchRender", "moduleDetections", "moduleSummary"].map((n) => fnSource(appJs, n))],
    ["onBenchEvent"], { document: doc, liveBench, $: (id) => (id === "bench-canvas" ? canvas : (nodes[id] = nodes[id] || doc.createElement("div"))) });
    page.onBenchEvent({ payload: { kind: "id", software: "2025.01.02", has_card: true, person_class: personClass } });
    page.onBenchEvent({ payload: { kind: "event", data: { boxes: [box] } } });
    const promised = /labels will read “(\w+)”/.exec(nodes["bench-id"].textContent)[1];
    const drawn = nodes["bench-meter-label"].textContent;
    assert.strictEqual(label(drawn), promised, `desktop bench (person_class=${personClass}): promised "${promised}", drew "${drawn}"`);
  }
});

test("each frontend's About facts: a read that failed is never an empty board", async () => {
  const core = await import(pathToFileURL(join(CANARY, "assets/flash-core.js")).href);
  const flashJs = read(join(CANARY, "assets/flash.js"));
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const installed = (facts) => core.aboutBoardGroups(facts)[1].rows.map((r) => `${r.label}: ${r.value}`).join(" | ");
  const browser = (state) => runCut([fnSource(flashJs, "aboutFacts")], ["aboutFacts"], { state }).aboutFacts();
  const desktop = (state) => runCut([fnSource(appJs, "aboutFacts")], ["aboutFacts"], { state }).aboutFacts();
  const IMG = { version: "2.4.16", projectName: "canary_wap", idf: "v5.5.1", secureVersion: 0, elfSha256Prefix: "a0a1" };

  // The browser: undefined until read, false when a read threw, null only
  // when the reads worked and found nothing.
  assert.strictEqual(browser({ current: null, ptRead: "failed" }).image, false);
  assert.strictEqual(browser({ current: { unknown: true }, ptRead: "failed" }).image, false);
  assert.strictEqual(browser({ current: { unknown: true, readFailed: true }, ptRead: "ok" }).image, false);
  assert.strictEqual(browser({ current: { unknown: true }, ptRead: "ok" }).image, null);
  assert.strictEqual(browser({ current: null, ptRead: "ok" }).image, undefined);
  assert.deepStrictEqual(browser({ current: { ...IMG, built: "x" }, ptRead: "ok" }).image,
    { projectName: "canary_wap", version: "2.4.16", idf: "v5.5.1", secureVersion: 0, elfSha256Prefix: "a0a1" });
  // A descriptor read that threw after a good partition read, with the
  // bootloader header's read failed too, reads "Not checked", never "None found".
  assert.strictEqual(installed({ chip: "ESP32-S3", ...browser({ current: { unknown: true, readFailed: true },
    ptRead: "ok", bootHeader: false }) }), "Installed image: Not checked");
  // readCurrentFirmware marks the throw so (the descriptor read is inside the try).
  assert.match(fnSource(flashJs, "readCurrentFirmware"),
    /\} catch \{[\s\S]*state\.current = \{ unknown: true, readFailed: true \};\s*\}\s*\}$/);

  // The desktop: the passport read decides; pending is "not yet", a failed
  // or unknown read is "not checked", a blank board is "none found".
  assert.strictEqual(desktop({ resident: null, passport: null }).image, undefined);
  assert.strictEqual(desktop({ resident: { ...IMG }, passportPending: true }).image, undefined);
  assert.strictEqual(desktop({ resident: { unknown: true } }).image, false);
  assert.strictEqual(desktop({ resident: { unknown: false, blank: true } }).image, null);
  assert.deepStrictEqual(desktop({ resident: { ...IMG, slot: "app0" } }).image,
    { projectName: "canary_wap", version: "2.4.16", idf: "v5.5.1", secureVersion: 0, elfSha256Prefix: "a0a1" });
  assert.strictEqual(installed({ chip: "ESP32-S3", ...desktop({ resident: { unknown: true }, passport: { bootloader: false } }) }),
    "Installed image: Not checked");
  assert.strictEqual(desktop({ passport: { bootloader: null } }).bootloader, null);
  assert.strictEqual(desktop({ passport: {} }).bootloader, undefined);

  // The desktop's fuse read, run through its own button: fields decode to
  // rows; an unsupported chip, a scan with no fields and a read that threw
  // all read "Not checked" — never "Not checked yet", never a value.
  const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
  for (const [answer, want] of [
    [{ supported: true, chip_fields: { PKG_VERSION: 0, BLK_VERSION_MAJOR: 1 } }, "ESP32-S3 (QFN56)"],
    [{ supported: false }, "Not checked"],
    [{ supported: true }, "Not checked"],
    ["the port went away", "Not checked"],
  ]) {
    const doc = pageDoc();
    const nodes = { "efuse-read": doc.createElement("button"), "efuse-result": doc.createElement("p"),
      "this-board": doc.createElement("section") };
    const state = { port: "/dev/ttyACM0", chip: "ESP32-S3", portKind: "esp32", busy: false, chipFuses: undefined,
      flashBytes: 8 << 20 };
    const page = runCut([m[1], ...["aboutFacts", "macRevealRow", "renderThisBoard", "wireEfuseRead"]
      .map((n) => fnSource(appJs, n))], ["wireEfuseRead"], {
      document: doc, state, $: (id) => nodes[id] || null, prefs: {}, savePrefs() {},
      invoke: async (cmd) => {
        assert.strictEqual(cmd, "read_security_efuses");
        if (typeof answer === "string") throw answer;
        return answer;
      },
    });
    page.wireEfuseRead();
    nodes["efuse-read"].fire("click");
    await ticks();
    assert.match(nodes["this-board"].textContent, new RegExp(`Package${want.replace(/[()]/g, "\\$&")}(?! yet)`),
      `a fuse read answering ${JSON.stringify(answer)} should read ${want}`);
  }
});

// The advice the desktop's About fuse read gives, before the click and after
// a good read. Quitting the Flasher comes first: "the About line's reason for
// quitting the Flasher first", two tests down, holds why.
const ABOUT_RESTART = "To start its firmware again, quit the Flasher first (while it is open, it can read a " +
  "board that comes back on its port, and that read stops the firmware again), then press its reset " +
  "button, or unplug it (and its battery, if it has one) and plug it back in.";
const ABOUT_NOTE = "A good read leaves the board in its ROM’s download mode, not running its firmware. " +
  ABOUT_RESTART;
const ABOUT_LEFT = "The fuse read left the board in its ROM’s download mode, not running its firmware. " +
  ABOUT_RESTART;

test("the desktop's About fuse read says, beside its button, that it leaves the board in the ROM", async () => {
  // The This Board card offers the cold-start card's fuse read for any
  // connected ESP32, not only one brought up with BOOT held. The read resets
  // the board into its ROM and does not reset it back out (efuse.rs), so a
  // good read leaves it in download mode. The person reads that, and how to
  // start the firmware again, before the click, and again where the button
  // was once a good read has run: quit the Flasher, then the reset button,
  // or the cable and the battery (a board whose battery powers it without
  // its firmware's help stays powered with only its USB cable pulled).
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
  const indexHtml = read(join(ROOT, "desktop/src/index.html"));
  // The card starts as index.html draws it (hidden until a board is read).
  const cardTag = indexHtml.match(/<section id="this-board" class="([^"]*)"/);
  assert.ok(cardTag, "couldn't find the This Board card in desktop/src/index.html");

  // Every styles.css rule that hides, read as selectors and matched against
  // the card's own tree, and above the card against index.html's ancestors
  // of <section id="this-board"> as the markup writes them (tag, id, class,
  // attributes). A selector this cannot read fails the test rather than
  // being skipped: an unread rule is one the check would pass.
  const css = read(join(ROOT, "desktop/src/styles.css")).replace(/\/\*[\s\S]*?\*\//g, "");
  const hidingSelectors = [];
  for (const [, sel, body] of css.matchAll(/([^{}]+)\{([^{}]*)\}/g)) {
    if (!/display\s*:\s*none|visibility\s*:\s*hidden/i.test(body)) continue;
    for (const one of sel.split(",").map((s) => s.trim())) {
      if (/^(from|to|\d+(\.\d+)?%)$/.test(one)) continue; // a @keyframes step, not an element
      if (/::|:(before|after|first-line|first-letter)\b/.test(one)) continue; // a pseudo-element: not the element itself
      hidingSelectors.push(one);
    }
  }
  assert.ok(hidingSelectors.includes(".hidden"), "couldn't read styles.css's .hidden rule — the check below would pass on anything");
  const PIECE = /#([\w-]+)|\.([\w-]+)|\[([\w-]+)(?:\s*=\s*["']?([^"'\]]*)["']?)?\]|:not\(([^()]*)\)|:([\w-]+)(?:\([^()]*\))?/g;
  const compound = (src, sel) => {
    const head = src.match(/^(\*|[a-zA-Z][\w-]*)?/);
    const rest = src.slice(head[0].length);
    const pieces = [...rest.matchAll(PIECE)];
    assert.strictEqual(pieces.map((p) => p[0]).join(""), rest, `couldn't read the selector "${sel}" in styles.css`);
    const tag = head[1] && head[1] !== "*" ? head[1].toLowerCase() : null;
    const tests = pieces.map(([, id, cls, attr, val, not]) =>
      id ? (e) => (e.id || e.attrs.id) === id
        : cls ? (e) => e.className.split(/\s+/).includes(cls)
          : attr ? (e) => {
            const v = attr in e.attrs ? e.attrs[attr] : e[attr] === true ? "" : e[attr] || null;
            return v !== null && v !== undefined && (val === undefined || String(v) === val);
          }
            : not !== undefined ? ((inner) => (e) => !inner(e))(compound(not.trim(), sel))
              : () => true); // any other pseudo-class (:hover, :checked …) is taken to hold
    return (e) => (!tag || e.tagName.toLowerCase() === tag) && tests.every((t) => t(e));
  };
  const parse = (sel) => {
    // Right to left: [subject, combinator, its context, …]. Past a sibling
    // combinator (+ or ~) the context is taken to hold.
    const parts = sel.split(/\s*([>+~])\s*|\s+/).filter((p) => p !== undefined && p !== "");
    const out = [];
    for (let i = parts.length - 1; i >= 0; i--) {
      if (parts[i] === "+" || parts[i] === "~") break;
      if (parts[i] === ">") { out.push(">"); continue; }
      if (out.length && out[out.length - 1] !== ">") out.push(" ");
      out.push(compound(parts[i], sel));
    }
    return out;
  };
  const rules = hidingSelectors.map((sel) => ({ sel, parts: parse(sel) }));
  const VOID = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"]);
  const open = [];
  let above = null;
  // Markup only, as csp.test.js reads it: comments, then script and style
  // elements, each stripped until nothing changes (one pass over a nested
  // "<scr<script></script>ipt>" leaves a "<script>" behind), with an end tag
  // closed by anything up to ">" ("</script >" closes it too).
  const stripAll = (text, re) => {
    let prev;
    do { prev = text; text = text.replace(re, ""); } while (text !== prev);
    return text;
  };
  const markup = stripAll(stripAll(stripAll(indexHtml, /<!--[\s\S]*?-->/g),
    /<script\b[^>]*>[\s\S]*?<\/script\b[^>]*>/gi), /<style\b[^>]*>[\s\S]*?<\/style\b[^>]*>/gi);
  for (const [, close, tag, rest] of markup.matchAll(/<(\/?)([a-zA-Z][\w-]*)\b([^>]*)>/g)) {
    const t = tag.toLowerCase();
    if (close) { const i = open.map((e) => e.tagName).lastIndexOf(t); if (i >= 0) open.length = i; continue; }
    const attrs = {};
    for (const [, k, , v] of rest.matchAll(/([\w:-]+)(\s*=\s*"([^"]*)")?/g)) attrs[k.toLowerCase()] = v ?? "";
    if (attrs.id === "this-board") { above = open.slice().reverse(); break; }
    if (!VOID.has(t) && !/\/\s*$/.test(rest)) open.push({ tagName: t, attrs, className: attrs.class || "" });
  }
  assert.ok(above && above[above.length - 1].tagName === "html" && above.some((e) => e.tagName === "body"),
    "couldn't read the This Board card's ancestors from desktop/src/index.html");
  const matches = (parts, chain, at, pi = 0) => {
    if (!parts[pi](chain[at])) return false;
    if (pi === parts.length - 1) return true;
    if (parts[pi + 1] === ">") return at + 1 < chain.length && matches(parts, chain, at + 1, pi + 2);
    for (let k = at + 1; k < chain.length; k++) if (matches(parts, chain, k, pi + 2)) return true;
    return false;
  };
  // Shown, not just drawn: nothing from the node up to and including the
  // card is hidden — by the hidden property or attribute, or by display:
  // none or visibility: hidden in an inline style (the style object, its
  // cssText, or a style attribute) or a styles.css rule that matches it
  // there. Other ways to hide (opacity, a zero size) are not checked.
  const INLINE = /(^|;)\s*(display\s*:\s*none|visibility\s*:\s*hidden)\b/i;
  const assertShown = (what, node, card) => {
    const chain = [];
    for (let n = node; n; n = n.parent) chain.push(n);
    assert.strictEqual(chain[chain.length - 1], card, `${what} is not in the card`);
    chain.push(...above);
    for (let i = 0; chain[i] !== above[0]; i++) {
      const n = chain[i];
      const rule = rules.find((r) => matches(r.parts, chain, i));
      const why = n.hidden === true ? "the hidden property" : "hidden" in n.attrs ? "the hidden attribute"
        : n.style.display === "none" ? "display: none" : n.style.visibility === "hidden" ? "visibility: hidden"
          : INLINE.test(n.style.cssText || "") ? `style.cssText "${n.style.cssText}"`
            : INLINE.test(n.attrs.style || "") ? `style="${n.attrs.style}"`
              : rule ? `styles.css "${rule.sel}"` : null;
      assert.ok(!why, `${what} is drawn but not shown: a <${n.tagName}> on its way up to the card is hidden (${why})`);
    }
  };

  // Before the click, after a good read, after a chip with no table (the
  // port is never opened) and after a read that threw (where it left the
  // board is not known).
  for (const answer of [{ supported: true, chip_fields: { PKG_VERSION: 0 } }, { supported: false }, "the port went away"]) {
    const doc = pageDoc();
    const nodes = { "efuse-read": doc.createElement("button"), "efuse-result": doc.createElement("p"),
      "this-board": doc.createElement("section") };
    nodes["efuse-read"].click = () => nodes["efuse-read"].fire("click");
    nodes["this-board"].className = cardTag[1];
    nodes["this-board"].id = "this-board";
    const state = { port: "/dev/ttyACM0", chip: "ESP32-S3", portKind: "esp32", busy: false, chipFuses: undefined,
      efuseScan: null, flashBytes: 8 << 20 };
    const asked = [];
    const page = runCut([m[1], ...["aboutFacts", "macRevealRow", "renderThisBoard", "wireEfuseRead"]
      .map((n) => fnSource(appJs, n))], ["renderThisBoard", "wireEfuseRead"], {
      document: doc, state, $: (id) => nodes[id] || null, prefs: {}, savePrefs() {},
      invoke: async (cmd) => {
        asked.push(cmd);
        if (typeof answer === "string") throw answer;
        return answer;
      },
    });
    page.wireEfuseRead();
    page.renderThisBoard();
    const card = nodes["this-board"];
    const button = () => nodesOf(card).find((n) => n.tagName === "button" && n.textContent === "Read the chip’s fuses");
    const lines = (text) => nodesOf(card).filter((n) => n.tagName !== "#text" && n._text === text);
    const offer = button();
    assert.ok(offer, "the This Board card should offer the fuse read until a read has been tried on this board");
    assert.equal(lines(ABOUT_NOTE).length, 1, "the card must say, once, that the fuse read leaves the board in the ROM");
    assert.strictEqual(lines(ABOUT_NOTE)[0].parent, offer.parent, "the line must sit beside the button it is about");
    assert.equal(lines(ABOUT_LEFT).length, 0, "nothing has been read yet");
    // Shown once "More about this board" is open: the closed <details>
    // holds the line and its button alike.
    assertShown("the line", lines(ABOUT_NOTE)[0], card);
    assertShown("its button", offer, card);
    const where = offer.parent;

    offer.fire("click");
    await ticks();
    const how = JSON.stringify(answer);
    assert.deepStrictEqual(asked, ["read_security_efuses"], "the card's button runs the one fuse read");
    assert.ok(!button(), `the button goes once a read has been tried (${how})`);
    assert.equal(lines(ABOUT_NOTE).length, 0, `the line about a read to come goes with its button (${how})`);
    if (typeof answer !== "string" && answer.supported) {
      assert.match(card.textContent, /PackageESP32-S3 \(QFN56\)/, "the read's fields reach the card");
      // The board is in the ROM now, and the card still says how to get it out.
      assert.equal(lines(ABOUT_LEFT).length, 1, "after a good read the card must say, once, where it left the board");
      const left = lines(ABOUT_LEFT)[0];
      assert.strictEqual(left.parent.tagName, where.tagName, "the line takes the button's place");
      assert.strictEqual(left.parent.className, where.className, "the line takes the button's place");
      assertShown("the line after a good read", left, card);
    } else {
      assert.equal(lines(ABOUT_LEFT).length, 0, `no read reached the ROM, so nothing says it left the board there (${how})`);
    }
  }

  // The good read is bound to the board it ran on, as efuseStopFindings
  // binds the verdict: a scan from another port or another chip says nothing
  // about where this board is.
  for (const other of [{ port: "/dev/ttyACM1", chip: "ESP32-S3" }, { port: "/dev/ttyACM0", chip: "ESP32-C3" }]) {
    const doc = pageDoc();
    const nodes = { "efuse-read": doc.createElement("button"), "this-board": doc.createElement("section") };
    const state = { port: "/dev/ttyACM0", chip: "ESP32-S3", portKind: "esp32", chipFuses: null, flashBytes: 8 << 20,
      efuseScan: { ...other, scan: { supported: true, chip_fields: { PKG_VERSION: 0 } } }, efuseParked: { ...other } };
    runCut([m[1], ...["aboutFacts", "macRevealRow", "renderThisBoard"].map((n) => fnSource(appJs, n))],
      ["renderThisBoard"], { document: doc, state, $: (id) => nodes[id] || null, prefs: {}, savePrefs() {} })
      .renderThisBoard();
    assert.ok(!nodes["this-board"].textContent.includes(ABOUT_LEFT),
      `a good read of ${other.chip} on ${other.port} must not say where it left the board on ${state.port}`);
  }
});

test("the About line's reason for quitting the Flasher first: while it is open, a board that comes back on its port is read again", async () => {
  // ABOUT_RESTART says to quit the Flasher before pressing the board's reset
  // button or re-plugging it. Outside the serial monitor the poll has no
  // unplug grace: a poll that misses the port drops the board (a re-plug, or
  // a reset that takes a chip's own USB-Serial/JTAG port off the bus for a
  // poll), and the next poll that finds it runs detect_chip (`espflash
  // board-info`) and then the passport (`espflash read-flash`, per region).
  // In the pinned espflash 3.3.0 each of those connects through a reset into
  // the ROM, loads espflash's stub and exits without resetting the chip back
  // (src/cli/mod.rs: board_info and read_flash never call reset_after), so
  // the board comes back stopped, not running its firmware. Read from that
  // crate's source; nothing here ran on a device.
  const pins = read(join(ROOT, ".github/espflash-pins.env"));
  assert.match(pins, /^ESPFLASH_VERSION=3\.3\.0$/m,
    "ABOUT_RESTART's reason was read from espflash 3.3.0's source (board-info and read-flash leave the chip in the stub) — " +
    "re-read the pinned version's board_info and read_flash, and reword the line in desktop/src/app.js if they changed");
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
  const grace = appJs.match(/^const UNPLUG_GRACE_POLLS = (\d+);$/m);
  assert.ok(grace, "couldn't cut UNPLUG_GRACE_POLLS from desktop/src/app.js");
  const doc = pageDoc();
  const nodes = { "this-board": doc.createElement("section"), "efuse-read": doc.createElement("button") };
  const S3 = { name: "/dev/ttyACM0", kind: "usb", vid: 0x303a, pid: 0x1001, product: "USB JTAG/serial debug unit" };
  let ports = [S3];
  const calls = [];
  // A good About fuse read has just run on this board.
  const state = { port: S3.name, portInfo: S3, portKind: "esp32", chip: "ESP32-S3", chipDesc: null, chipTool: null,
    busy: false, monitoring: false, detecting: false, failedPort: null, catalog: { flash_baud: 921600 },
    flashBytes: 8 << 20, passport: null, resident: null, passportPending: false,
    efuseScan: { port: S3.name, chip: "ESP32-S3", scan: { supported: true, chip_fields: { PKG_VERSION: 0 } } },
    efuseParked: { port: S3.name, chip: "ESP32-S3" }, chipFuses: null };
  const page = runCut([grace[0], "let missingPolls = 0;", m[1],
    ...["aboutFacts", "macRevealRow", "renderThisBoard", "renderPassport", "pickCandidate",
      "trackPresenceWhileMonitoring", "onDisconnect", "resetSteps"].map((n) => fnSource(appJs, n)),
    ...["readPassport", "identify", "pollPorts"].map((n) => "async " + fnSource(appJs, n))], ["pollPorts", "renderThisBoard"], {
    document: doc, state, $: (id) => nodes[id] || (nodes[id] = doc.createElement("div")), prefs: {}, savePrefs() {},
    WE2_VID: 0x2886, WE2_PID: 0x8060, matchProjectToProduct: () => null,
    setTimeout: () => 0, clearTimeout() {},
    invoke: async (cmd, args) => {
      calls.push(cmd === "list_ports" ? cmd : `${cmd} ${args.port}`);
      if (cmd === "list_ports") return ports;
      if (cmd === "detect_chip") return { chip: "ESP32-S3", flash_bytes: 8 << 20 };
      if (cmd === "board_passport") return new Promise(() => {}); // 8–25 s on hardware; not needed here
      throw new Error(`unexpected ${cmd}`);
    },
  });
  const poll = async (list) => { ports = list; calls.length = 0; await page.pollPorts(); await ticks(); return [...calls]; };
  page.renderThisBoard();
  assert.ok(nodes["this-board"].textContent.includes(ABOUT_LEFT), "the card should say where the good read left the board");

  // A port that stays (a USB-bridge board's, through its reset button) is
  // left alone: the board is already identified.
  assert.deepStrictEqual(await poll([S3]), ["list_ports"], "a port that stays must not be read again");
  // The cable comes out (or the port drops off the bus on a reset)…
  assert.deepStrictEqual(await poll([]), ["list_ports"]);
  assert.strictEqual(state.chip, null, "outside the monitor, one poll without the port is a disconnect");
  // …and the board that comes back is read again, with the Flasher's own
  // espflash calls, before anything else can run on it.
  assert.deepStrictEqual(await poll([S3]), ["list_ports", `detect_chip ${S3.name}`, `board_passport ${S3.name}`],
    "a board that comes back on the port is read again (board-info, then the passport's read-flash)");
  assert.strictEqual(state.chip, "ESP32-S3");
  // The card offers the read again, and says again to quit the Flasher first.
  assert.ok(nodes["this-board"].textContent.includes(ABOUT_NOTE), "the card should offer the read, and its advice, again");

  // While the serial monitor runs, a short gap is ridden out and nothing is
  // read: that is the one exception, and the line does not lean on it.
  state.monitoring = true;
  for (let i = 1; i < Number(grace[1]); i++) {
    assert.deepStrictEqual(await poll([]), ["list_ports"]);
    assert.strictEqual(state.chip, "ESP32-S3", `the monitor rides out ${i} missed poll(s)`);
  }
  assert.deepStrictEqual(await poll([S3]), ["list_ports"], "the monitor's board is not read again when it comes back");
});

test("the About card's after-read line speaks for the latest fuse read on this board, and only until a write", async () => {
  // ABOUT_LEFT stands on state.efuseParked: set by a read whose block 0
  // answered, cleared when the next read starts and by noteBoardWritten.
  // Every write connects through a reset of its own, and in the pinned
  // espflash 3.3.0 one that finishes calls reset_after (write-bin through
  // write_bins_to_flash's finish, erase-flash directly), a hard reset under
  // the default --after; after an install or a local file the post-flash
  // monitor is watching that boot, so the line telling the person to quit
  // the Flasher must be gone by then. A read that throws may never have
  // opened the port, or may have met a board re-plugged mid-read, so it does
  // not inherit an earlier read's line. And the line is never drawn beside
  // the offer to read again.
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const m = appJs.match(/\/\/ BEGIN ABOUT-BOARD\n([\s\S]*?)\/\/ END ABOUT-BOARD/);
  const grace = appJs.match(/^const UNPLUG_GRACE_POLLS = (\d+);$/m);
  const recheck = appJs.match(/\$\("recheck"\)\.addEventListener\("click", (\(\) => \{[\s\S]*?\n {2}\})\);/);
  assert.ok(grace && recheck, "couldn't cut UNPLUG_GRACE_POLLS or the re-read chip handler from desktop/src/app.js");
  const S3 = { name: "/dev/ttyACM0", kind: "usb", vid: 0x303a, pid: 0x1001, product: "USB JTAG/serial debug unit" };
  const GOOD = { supported: true, chip_fields: { PKG_VERSION: 0 } };
  const bench = (reads, { receipt = false } = {}) => {
    const doc = pageDoc();
    const nodes = { "efuse-read": doc.createElement("button"), "efuse-result": doc.createElement("p"),
      "this-board": doc.createElement("section") };
    nodes["efuse-read"].click = () => nodes["efuse-read"].fire("click");
    const state = { port: S3.name, portInfo: S3, portKind: "esp32", chip: "ESP32-S3", chipDesc: null, chipTool: null,
      busy: false, monitoring: false, detecting: false, failedPort: null, chipFuses: undefined, efuseScan: null,
      efuseParked: null, flashBytes: 8 << 20, passport: null, resident: null, passportPending: false, mac: null,
      product: { id: "securacv-canary" }, catalog: { flash_baud: 921600, products: [] }, vision: {},
      appInfo: { version: "0" }, localFile: { path: "/tmp/mine.bin", name: "mine.bin", size: 4096, sha256: "ab".repeat(32) } };
    const asked = [];
    const monitorStarts = [];
    const page = runCut([grace[0], "let missingPolls = 0;", m[1], `const recheckClick = ${recheck[1]};`,
      ...["aboutFacts", "macRevealRow", "renderThisBoard", "renderPassport", "wireEfuseRead", "efuseStopFindings",
        "noteBoardWritten", "setStatus", "pickCandidate", "trackPresenceWhileMonitoring", "onDisconnect", "resetSteps"]
        .map((n) => fnSource(appJs, n)),
      ...["readPassport", "identify", "pollPorts", "onFlash", "onFlashLocalFile", "runRescue", "onRescueRestore",
        "onRescueErase"].map((n) => "async " + fnSource(appJs, n))],
    ["renderThisBoard", "wireEfuseRead", "recheckClick", "onFlash", "onFlashLocalFile", "onRescueRestore", "onRescueErase"], {
      document: doc, state, $: (id) => nodes[id] || (nodes[id] = doc.createElement("div")), prefs: {}, savePrefs() {},
      WE2_VID: 0x2886, WE2_PID: 0x8060, matchProjectToProduct: () => null, setTimeout: () => 0, clearTimeout() {},
      flashFitVerdict: () => ({ fits: true }), readProvisioning: () => null,
      opFor: () => ({ begin() {}, stage() {}, frac() {}, indet() {}, fail() {}, end() {}, done() {} }),
      listen: async () => () => {}, withBaudLadder: (fn) => fn(921600), requiresLiveReceipt: () => receipt,
      startMonitor: async (o) => { monitorStarts.push(o); state.monitoring = true; },
      window: { __TAURI__: { dialog: { open: async () => "/tmp/backup.bin", confirm: async () => true } } },
      invoke: async (cmd) => {
        asked.push(cmd);
        if (cmd === "read_security_efuses") {
          const answer = reads.shift();
          if (typeof answer === "string") throw answer;
          return answer;
        }
        if (cmd === "list_ports") return [S3];
        if (cmd === "detect_chip") return { chip: "ESP32-S3", flash_bytes: 8 << 20 };
        if (cmd === "board_passport") return new Promise(() => {}); // 8–25 s on hardware; not needed here
        if (cmd === "flash") return { version: "2.4.18" };
        if (cmd === "flash_local_file") return { installed_sha256: "ab".repeat(32) };
        return null;
      },
    });
    page.wireEfuseRead();
    page.renderThisBoard();
    const card = nodes["this-board"];
    return {
      page, state, nodes, card, asked, monitorStarts,
      lines: (text) => nodesOf(card).filter((n) => n.tagName !== "#text" && n._text === text),
      offers: () => nodesOf(card).filter((n) => n.tagName === "button" && n.textContent === "Read the chip’s fuses"),
      // The cold-start card's button, which stays after the This Board button goes.
      read: async () => { nodes["efuse-read"].click(); await ticks(); },
    };
  };

  // A second read: a good one keeps the line, one that throws takes it away.
  for (const [second, keeps] of [[GOOD, true], ["Failed to connect: timed out waiting for packet header", false],
    ["the port went away", false]]) {
    const b = bench([GOOD, second]);
    await b.read();
    assert.equal(b.lines(ABOUT_LEFT).length, 1, "precondition: the good read says where it left the board");
    await b.read();
    const how = JSON.stringify(second);
    if (keeps) {
      assert.equal(b.lines(ABOUT_LEFT).length, 1, "a second good read says it again, once");
    } else {
      assert.match(b.nodes["efuse-result"].textContent, /^Fuses NOT checked — /, `the second read threw (${how})`);
      assert.equal(b.lines(ABOUT_LEFT).length, 0, `a read that threw does not inherit the good read's line (${how})`);
      assert.equal(b.offers().length, 0, `nor the offer: a read has been tried (${how})`);
    }
  }

  // Re-read chip after a good read: the fuse fields go, the offer comes
  // back with its own line, and the after-read line is not drawn beside it.
  {
    const b = bench([GOOD]);
    await b.read();
    assert.equal(b.lines(ABOUT_LEFT).length, 1, "precondition: the good read says where it left the board");
    b.asked.length = 0;
    b.page.recheckClick();
    await ticks();
    assert.deepStrictEqual(b.asked, ["list_ports", "detect_chip", "board_passport"], "re-read chip reads the board again");
    assert.equal(b.offers().length, 1, "after a re-read the card offers the fuse read again");
    assert.equal(b.lines(ABOUT_NOTE).length, 1, "with its line beside it, once");
    assert.equal(b.lines(ABOUT_LEFT).length, 0, "the offer and the after-read line are never drawn together");
  }

  // Every write the bench starts, after a good read.
  const writes = [
    ["the install", "onFlash", "flash", "flash-result", /Flashing is complete/, {}],
    ["the install of a firmware with a live receipt", "onFlash", "flash", "flash-result", /Watching the live boot/, { receipt: true }],
    ["a local file", "onFlashLocalFile", "flash_local_file", "local-result", /^Write confirmed/, {}],
    ["a restore", "onRescueRestore", "write_local_image", "rescue-result", /written — the board is rebooting into it/, {}],
    ["an erase", "onRescueErase", "erase_chip", "rescue-result", /^Chip erased/, {}],
  ];
  for (const [what, fn, cmd, result, said, opts] of writes) {
    const b = bench([GOOD], opts);
    await b.read();
    assert.equal(b.lines(ABOUT_LEFT).length, 1, `${what}: precondition, the good read says where it left the board`);
    await b.page[fn]();
    await ticks();
    assert.ok(b.asked.includes(cmd), `${what}: ${fn} should have written (${b.asked.join(", ")})`);
    assert.match(b.nodes[result].textContent, said, `${what}: the write should have finished`);
    if (fn === "onFlash" || fn === "onFlashLocalFile") {
      assert.deepStrictEqual(b.monitorStarts, [{ postFlash: true }], `${what}: the post-flash monitor watches the boot`);
    }
    assert.match(b.card.textContent, /PackageESP32-S3 \(QFN56\)/, `${what}: the card is still drawn`);
    assert.equal(b.lines(ABOUT_LEFT).length, 0,
      `${what}: espflash hard-resets the board at the end of the write, so the card must not say the fuse read left it in the ROM`);
  }
});
