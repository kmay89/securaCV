// canary-local/tests/canary_local.test.js — Node tests for the page's
// DOM-free logic (repo convention: CI tests the exact shipped source).
//
//   node --test canary-local/tests/
//
// Coverage: MQTT wildcard matching (the shell's broker semantics), the
// witness canonical/signature format (must equal what trust.cpp
// rebuilds before Ed25519::verify — pinned here as a golden string),
// LED cadence translation, and registry ↔ dist artifact integrity.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");

async function importShell() {
  return import("../emulator/web/emu-shell.js");
}
async function importGuides() {
  return import("../assets/guides.js");
}

test("MQTT single-level wildcard matches the fleet topics", async () => {
  const { topicMatches } = await importShell();
  assert.ok(topicMatches("securacv/+/status", "securacv/canary_wap_garage/status"));
  assert.ok(topicMatches("securacv/+/chain", "securacv/x/chain"));
  assert.ok(!topicMatches("securacv/+/status", "securacv/x/health"));
  assert.ok(!topicMatches("securacv/+/status", "securacv/x/y/status"));
  assert.ok(!topicMatches("securacv/+/status", "other/x/status"));
  assert.ok(topicMatches("securacv/fleet/ack", "securacv/fleet/ack"));
  assert.ok(topicMatches("securacv/#", "securacv/a/b/c"));
});

test("witness chain canonical matches trust.cpp's locked format", async () => {
  // trust.cpp: snprintf("%s|v%d|chain|%s|%lu|%s", SIG_PREFIX="securacv-canary-sig",
  // SCHEMA_V=1, device_id, length, latest_hash). If this golden ever
  // breaks, the page's witnesses sign something the firmware won't
  // verify — and every ✓ on the emulated glass silently degrades.
  const id = "canary_vision_frontdoor";
  const len = 41;
  const hash = "ab".repeat(32);
  const canonical = `securacv-canary-sig|v1|chain|${id}|${len}|${hash}`;
  // Source-of-truth cross-check against the firmware tree:
  const trust = readFileSync(
    join(ROOT, "../firmware/projects/canary-display/src/trust.cpp"), "utf8");
  assert.match(trust, /securacv-canary-sig/);
  assert.match(trust, /%s\|v%d\|chain\|%s\|%lu\|%s/);
  assert.strictEqual(
    canonical,
    "securacv-canary-sig|v1|chain|canary_vision_frontdoor|41|" + "ab".repeat(32)
  );
});

test("simulated witness signs what the firmware verifies", async () => {
  const { SimWitness } = await importShell();
  const w = new SimWitness({ id: "t1", deviceType: "canary-sense", name: "T", room: "R" });
  const chain = await w.chainPayload();
  assert.strictEqual(typeof chain.length, "number");
  assert.match(chain.latest_hash, /^[0-9a-f]{64}$/);
  if (w.pubHex) {
    // WebCrypto Ed25519 available (Node ≥ 19): verify our own signature
    // over the exact canonical, round-tripping the b64url encoding.
    assert.match(w.pubHex, /^[0-9a-f]{64}$/, "health payload pubkey is 64-hex");
    assert.match(chain.sig, /^[A-Za-z0-9_-]{86}$/, "sig is b64url, no padding");
    const canonical = `securacv-canary-sig|v1|chain|t1|${chain.length}|${chain.latest_hash}`;
    const sigBytes = Buffer.from(chain.sig.replaceAll("-", "+").replaceAll("_", "/") + "==", "base64");
    const key = await crypto.subtle.importKey(
      "raw", Buffer.from(w.pubHex, "hex"), "Ed25519", false, ["verify"]);
    const ok = await crypto.subtle.verify(
      "Ed25519", key, sigBytes, new TextEncoder().encode(canonical));
    assert.ok(ok, "signature verifies over the canonical string");
  }
});

test("LED cadences: every documented grammar row translates", async () => {
  const { LED_GRAMMAR, ledSequence } = await importGuides();
  for (const g of LED_GRAMMAR) {
    const seq = ledSequence(g.pattern);
    assert.ok(seq.length >= 2, g.pattern);
    assert.ok(seq.every(([on, ms]) => typeof on === "boolean" && ms > 0));
  }
  // count-coded groups carry their count
  assert.strictEqual(ledSequence("groups of 5").filter(([on]) => on).length, 5);
  assert.strictEqual(ledSequence("groups of 2").filter(([on]) => on).length, 2);
});

test("registry entries with emulators point at real artifacts", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  assert.ok(reg.devices.length >= 5);
  for (const dev of reg.devices) {
    if (!dev.emulator) continue;
    const mod = join(ROOT, dev.emulator.module);
    assert.ok(existsSync(mod), `${dev.id}: ${dev.emulator.module} exists`);
    const meta = JSON.parse(readFileSync(mod.replace(/\.js$/, ".meta.json"), "utf8"));
    assert.strictEqual(meta.fw_version, reg.fw_train,
      `${dev.id}: artifact fw (${meta.fw_version}) matches registry train (${reg.fw_train})`);
    const src = readFileSync(mod, "utf8");
    assert.ok(src.includes(dev.emulator.factory),
      `${dev.id}: factory ${dev.emulator.factory} exported by artifact`);
  }
});

test("fw_train matches the firmware tree's CANARY_FW_VERSION", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  const vh = readFileSync(
    join(ROOT, "../firmware/projects/canary-display/include/canary/version.h"), "utf8");
  const m = vh.match(/CANARY_FW_VERSION "([^"]+)"/);
  assert.ok(m, "version.h parses");
  assert.strictEqual(reg.fw_train, m[1]);
});
