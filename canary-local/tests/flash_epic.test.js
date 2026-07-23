// canary-local/tests/flash_epic.test.js — the flasher's insight layer, pinned.
//
// The "epic" additions to the browser flasher: the install verdict (update /
// downgrade / reinstall, said out loud), board roles (a display SHOWS), the
// per-setting help registry, the Vision flash-time dials → NVS seed, and the
// displays block that lets the flasher name a screen board and boot its 1:1
// emulator. All pure logic in flash-core.js + generated facts in flash.json,
// under the repo's node --test convention. (A separate file from
// flash.test.js on purpose — that one is churning in open PRs.)

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const catalog = JSON.parse(readFileSync(join(ROOT, "devices/flash.json"), "utf8"));
const core = () => import("../assets/flash-core.js");

// ── the generated epic blocks are coherent ─────────────────────────────────
test("flash.json: epic blocks validate", async () => {
  const c = await core();
  assert.deepStrictEqual(c.validateEpicCatalog(catalog), []);
});

test("flash.json: every product carries a role the core agrees with", async () => {
  const c = await core();
  for (const p of catalog.products) {
    assert.strictEqual(p.role, c.productRole(p.id), p.id);
  }
});

test("flash.json: vision detect dials — defaults inside bounds, presets too", () => {
  const vision = catalog.products.filter((p) => p.role === "vision");
  assert.ok(vision.length >= 1, "no vision products");
  for (const p of vision) {
    const d = p.detect;
    assert.ok(d, `${p.id}: vision product without detect dials`);
    assert.strictEqual(d.nvs.namespace, "securacv");
    assert.deepStrictEqual(
      Object.values(d.nvs.keys).sort(),
      ["det_dwell", "det_lost", "det_score", "det_target"],
      "NVS keys must be exactly the four detect_config.cpp reads");
    for (const [k, v] of Object.entries(d.defaults)) {
      const [lo, hi] = d.bounds[k];
      assert.ok(v >= lo && v <= hi, `${p.id}: default ${k}=${v} outside [${lo},${hi}]`);
    }
    assert.strictEqual(d.presets[0].id, "ships", "first preset is the shipped defaults");
    assert.deepStrictEqual(d.presets[0].values, d.defaults);
    for (const pr of d.presets) {
      for (const [k, v] of Object.entries(pr.values)) {
        const [lo, hi] = d.bounds[k];
        assert.ok(v >= lo && v <= hi, `${p.id}: preset ${pr.id} ${k}=${v} outside [${lo},${hi}]`);
      }
    }
  }
});

test("flash.json: sense reflexes are honest about being compile-time", () => {
  const sense = catalog.products.filter((p) => p.role === "sense");
  assert.ok(sense.length >= 2, "expected default + wellbeing sense products");
  for (const p of sense) {
    assert.ok(p.reflexes, `${p.id}: sense product without reflexes`);
    assert.strictEqual(p.reflexes.applies, "compile-time");
    for (const k of p.reflexes.knobs) {
      assert.ok(k.id && k.macro && k.unit, `${p.id}: knob missing id/macro/unit`);
      assert.ok(Number.isInteger(k.value) && k.value > 0, `${p.id}: knob ${k.id} value`);
      assert.ok(catalog.settings_help[k.id], `${p.id}: knob ${k.id} has no help topic`);
    }
  }
  const wb = sense.find((p) => /wellbeing/.test(p.id));
  assert.ok(wb.reflexes.knobs.some((k) => k.id === "vitals_lock_ms"),
    "wellbeing build carries the vitals knobs");
});

test("flash.json: displays name real emulator builds on disk", () => {
  assert.ok(catalog.displays.length >= 2, "watch + dash expected");
  for (const d of catalog.displays) {
    assert.ok(existsSync(join(ROOT, d.emulator.module)),
      `${d.id}: emulator module missing: ${d.emulator.module}`);
    assert.ok(d.emulator.factory && d.emulator.fw_version && d.emulator.lvgl,
      `${d.id}: emulator facts incomplete`);
    assert.ok(d.glass.w > 0 && d.glass.h > 0, `${d.id}: glass dimensions`);
  }
});

test("flash.json: help defaults quote the parsed firmware values", () => {
  const vision = catalog.products.find((p) => p.id === "securacv-canary-vision");
  const h = catalog.settings_help;
  assert.ok(h.det_score.default.includes(String(vision.detect.defaults.score)));
  assert.ok(h.det_lost.default.includes(String(vision.detect.defaults.lost_ms)));
  const sense = catalog.products.find((p) => p.id === "securacv-canary-sense");
  const debounce = sense.reflexes.knobs.find((k) => k.id === "present_debounce_ms");
  assert.ok(h.present_debounce_ms.default.includes(String(debounce.value)));
});

// ── version compare + the install verdict ──────────────────────────────────
test("compareVersions: ordering, prereleases, unparseables", async () => {
  const c = await core();
  assert.strictEqual(c.compareVersions("2.1.0", "2.2.0"), -1);
  assert.strictEqual(c.compareVersions("2.2.0", "2.2.0"), 0);
  assert.strictEqual(c.compareVersions("v2.2.0", "2.2.0"), 0);
  assert.strictEqual(c.compareVersions("2.10.0", "2.9.9"), 1);
  assert.strictEqual(c.compareVersions("2.2", "2.2.0"), 0);
  assert.strictEqual(c.compareVersions("2.2.0-rc1", "2.2.0"), -1);
  assert.strictEqual(c.compareVersions("2.2.0-rc1", "2.2.0-rc2"), -1);
  assert.strictEqual(c.compareVersions("not-embedded", "2.2.0"), null);
  assert.strictEqual(c.compareVersions("", "2.2.0"), null);
});

test("installVerdict: all six kinds, with honest copy", async () => {
  const c = await core();
  const vision = { id: "securacv-canary-vision", name: "Canary Vision" };
  const wap = { id: "securacv-canary-wap", name: "Canary WAP" };

  assert.strictEqual(c.installVerdict({ current: null, product: vision, version: "2.2.0" }).kind, "fresh");
  assert.strictEqual(c.installVerdict({ current: { unknown: true }, product: vision, version: "2.2.0" }).kind, "fresh");

  const up = c.installVerdict({
    current: { version: "2.1.0" }, currentProduct: vision, product: vision, version: "2.2.0" });
  assert.strictEqual(up.kind, "update");
  assert.ok(up.label.includes("2.1.0") && up.label.includes("2.2.0"));

  const down = c.installVerdict({
    current: { version: "2.3.0" }, currentProduct: vision, product: vision, version: "2.2.0" });
  assert.strictEqual(down.kind, "downgrade");
  assert.ok(/older/i.test(down.detail), "a downgrade says so plainly");

  const same = c.installVerdict({
    current: { version: "2.2.0" }, currentProduct: vision, product: vision, version: "2.2.0" });
  assert.strictEqual(same.kind, "same");

  const sw = c.installVerdict({
    current: { version: "2.2.0" }, currentProduct: wap, product: vision, version: "2.2.0" });
  assert.strictEqual(sw.kind, "switch");
  assert.ok(sw.detail.includes("Canary WAP"), "names what the board runs today");

  const unk = c.installVerdict({
    current: { version: "??" }, currentProduct: vision, product: vision, version: "2.2.0" });
  assert.strictEqual(unk.kind, "unknown");
});

// ── roles ──────────────────────────────────────────────────────────────────
test("productRole + roleVerb: displays SHOW, everything else senses", async () => {
  const c = await core();
  assert.strictEqual(c.productRole("canary-display-watch"), "display");
  assert.strictEqual(c.productRole("canary-display-dash"), "display");
  assert.strictEqual(c.productRole("securacv-canary-sense-wellbeing"), "sense");
  assert.strictEqual(c.productRole("securacv-canary-vision-xiao-c3"), "vision");
  assert.strictEqual(c.productRole("securacv-canary-wap"), "wap");
  assert.strictEqual(c.productRole("securacv-canary"), "canary");
  assert.strictEqual(c.roleVerb("display"), "shows");
  assert.strictEqual(c.roleVerb("sense"), "senses");
});

test("postFlashNextStep: a display board's next step is the glass", async () => {
  const c = await core();
  const step = c.postFlashNextStep({ id: "canary-display-watch", provisioning: "ap" }, { wifiJoined: true });
  assert.strictEqual(step.kind, "watch-glass");
  assert.ok(/glass|screen|SHOWS/i.test(step.body));
  // and the ap-portal path still wins when wifi wasn't baked (setup first)
  const portal = c.postFlashNextStep({ id: "securacv-canary", provisioning: "ap" }, {});
  assert.strictEqual(portal.kind, "wifi-portal");
});

test("looksLikeDisplayProject + displayFor: naming a screen build off the wire", async () => {
  const c = await core();
  assert.ok(c.looksLikeDisplayProject("canary_display"));
  assert.ok(c.looksLikeDisplayProject("canary-display-watch"));
  assert.ok(!c.looksLikeDisplayProject("canary-vision"));
  assert.ok(!c.looksLikeDisplayProject(""));
  const d = c.displayFor(catalog, "canary_display");
  assert.ok(d, "a display project name matches a catalog display");
  assert.strictEqual(c.displayFor(catalog, "canary-display-dash").id, "canary-display-dash");
  assert.strictEqual(c.displayFor(catalog, "canary-vision"), null);
});

// ── help registry ──────────────────────────────────────────────────────────
test("helpTopic: lookup, and graceful nulls", async () => {
  const c = await core();
  const t = c.helpTopic(catalog, "erase_all");
  assert.ok(t && t.label && t.what);
  assert.strictEqual(c.helpTopic(catalog, "no-such-topic"), null);
  assert.strictEqual(c.helpTopic({}, "erase_all"), null);
  assert.strictEqual(c.helpTopic(null, "erase_all"), null);
});

// ── Vision dials → NVS seed ────────────────────────────────────────────────
test("detectValuesToNvs: clamps to catalog bounds, maps to detect_config keys", async () => {
  const c = await core();
  const dials = catalog.products.find((p) => p.id === "securacv-canary-vision").detect;
  const nvs = c.detectValuesToNvs({ score: 75, lost_ms: 1000, dwell_ms: 5000, target: 0 }, dials);
  assert.deepStrictEqual(nvs.u8, { det_target: 0, det_score: 75 });
  assert.deepStrictEqual(nvs.u32, { det_lost: 1000, det_dwell: 5000 });
  const clamped = c.detectValuesToNvs({ score: 500, lost_ms: 1, dwell_ms: 1e9 }, dials);
  assert.strictEqual(clamped.u8.det_score, dials.bounds.score[1]);
  assert.strictEqual(clamped.u32.det_lost, dials.bounds.lost_ms[0]);
  assert.strictEqual(clamped.u32.det_dwell, dials.bounds.dwell_ms[1]);
  // partial sets only carry what was given
  const partial = c.detectValuesToNvs({ score: 80 }, dials);
  assert.deepStrictEqual(Object.keys(partial.u32), []);
  assert.deepStrictEqual(c.detectValuesToNvs(null, dials), { u8: {}, u32: {} });
});

test("buildNvsSeedImage: ints + wifi round-trip through the flasher's own NVS parser", async () => {
  const c = await core();
  const img = c.buildNvsSeedImage({
    wifi: { ssid: "birdhouse", pass: "hunter22" },
    u8: { det_target: 0, det_score: 85 },
    u32: { det_lost: 5000, det_dwell: 15000 },
  }, 0x6000);
  const items = c.parseNvs(img, ["wifi_ssid"]);
  const get = (k) => items.find((i) => i.key === k && i.namespace === "securacv");
  assert.strictEqual(get("det_score").value, 85);
  assert.strictEqual(get("det_target").value, 0);
  assert.strictEqual(get("det_lost").value, 5000);
  assert.strictEqual(get("det_dwell").value, 15000);
  assert.strictEqual(get("wifi_en").value, 1);
  assert.strictEqual(new TextDecoder().decode(get("wifi_ssid").bytes), "birdhouse");
});

test("buildNvsSeedImage: ints alone (the Vision usb-secrets case) still valid", async () => {
  const c = await core();
  const img = c.buildNvsSeedImage({ u8: { det_score: 70 }, u32: { det_lost: 1500 } }, 0x4000);
  const items = c.parseNvs(img);
  assert.ok(items.some((i) => i.key === "det_score" && i.value === 70 && i.namespace === "securacv"));
  assert.ok(!items.some((i) => i.key === "wifi_en"), "no wifi keys when none given");
});

test("buildNvsSeedImage: refuses bad keys and values", async () => {
  const c = await core();
  assert.throws(() => c.buildNvsSeedImage({ u8: { "a-key-way-too-long": 1 } }, 0x4000));
  assert.throws(() => c.buildNvsSeedImage({ u8: { k: 300 } }, 0x4000));
  assert.throws(() => c.buildNvsSeedImage({ u32: { k: -1 } }, 0x4000));
  assert.throws(() => c.buildNvsSeedImage({ u8: { k: 1.5 } }, 0x4000));
});

test("buildNvsWifiImage: unchanged behavior through the general builder", async () => {
  const c = await core();
  const img = c.buildNvsWifiImage("nest", "chirpchirp", 0x4000);
  const items = c.parseNvs(img, ["wifi_ssid", "wifi_pass"]);
  const get = (k) => items.find((i) => i.key === k);
  assert.strictEqual(new TextDecoder().decode(get("wifi_ssid").bytes), "nest");
  assert.strictEqual(get("wifi_en").value, 1);
  assert.throws(() => c.buildNvsWifiImage("", "x".repeat(8), 0x4000));
  assert.throws(() => c.buildNvsWifiImage("ok", "short", 0x4000));
});

// ── the pre-flash passport ─────────────────────────────────────────────────
test("passportRows: formats the read-only probes, flags what matters", async () => {
  const c = await core();
  const rows = c.passportRows({
    otadata: { updates: 4 },
    witness: { boots: 123, seq: 456, tamper: 1 },
    coredump: { present: true },
  });
  const byId = Object.fromEntries(rows.map((r) => [r.id, r]));
  assert.strictEqual(byId.updates.value, "4");
  assert.strictEqual(byId.boots.value, "123");
  assert.strictEqual(byId.tamper.tone, "warn");
  assert.strictEqual(byId.crash.tone, "warn");
  const clean = c.passportRows({ coredump: { present: false } });
  assert.strictEqual(clean.find((r) => r.id === "crash").tone, "ok");
  assert.deepStrictEqual(c.passportRows({}), []);
  assert.deepStrictEqual(c.passportRows(), []);
});
