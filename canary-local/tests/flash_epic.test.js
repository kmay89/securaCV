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

test("flash.json: sense reflexes are runtime dials — NVS-backed, bounded, preset", () => {
  const sense = catalog.products.filter((p) => p.role === "sense");
  assert.ok(sense.length >= 2, "expected default + wellbeing sense products");
  for (const p of sense) {
    const r = p.reflexes;
    assert.ok(r, `${p.id}: sense product without reflexes`);
    assert.strictEqual(r.applies, "runtime");
    assert.strictEqual(r.nvs.namespace, "securacv");
    for (const k of r.knobs) {
      assert.ok(k.id && k.macro && k.unit, `${p.id}: knob missing id/macro/unit`);
      assert.ok(k.nvs && k.nvs.startsWith("sns_"), `${p.id}: knob ${k.id} needs an sns_* NVS key`);
      assert.ok(k.nvs.length <= 15, `${p.id}: NVS key ${k.nvs} too long`);
      const [lo, hi] = k.bounds;
      assert.ok(Number.isInteger(k.value) && k.value >= lo && k.value <= hi,
        `${p.id}: knob ${k.id} default ${k.value} outside [${lo},${hi}]`);
      assert.ok(catalog.settings_help[k.id], `${p.id}: knob ${k.id} has no help topic`);
      assert.strictEqual(r.nvs.keys[k.id], k.nvs, `${p.id}: nvs.keys map disagrees for ${k.id}`);
    }
    assert.strictEqual(r.presets[0].id, "ships", "first preset is the shipped defaults");
    assert.deepStrictEqual(r.presets[0].values,
      Object.fromEntries(r.knobs.map((k) => [k.id, k.value])));
    const byId = Object.fromEntries(r.knobs.map((k) => [k.id, k]));
    for (const pr of r.presets) {
      for (const [kid, v] of Object.entries(pr.values)) {
        const [lo, hi] = byId[kid].bounds;
        assert.ok(v >= lo && v <= hi, `${p.id}: preset ${pr.id} ${kid}=${v} outside [${lo},${hi}]`);
      }
    }
  }
  const wb = sense.find((p) => /wellbeing/.test(p.id));
  assert.ok(wb.reflexes.knobs.some((k) => k.id === "vitals_lock_ms"),
    "wellbeing build carries the vitals knobs");
  const def = sense.find((p) => !/wellbeing/.test(p.id));
  assert.ok(!def.reflexes.knobs.some((k) => k.id === "vitals_lock_ms"),
    "presence-only build does not advertise vitals knobs");
});

test("reflexValuesToNvs: maps knob ids to sns_* keys, clamped to each knob's bounds", async () => {
  const c = await core();
  const sense = catalog.products.find((p) => p.id === "securacv-canary-sense-wellbeing");
  const r = c.reflexDials(catalog, sense);
  assert.ok(r, "wellbeing reflexes are runtime dials");
  const nvs = c.reflexValuesToNvs({
    present_debounce_ms: 500, clear_timeout_ms: 5000,
    range_near_cm: 100, range_mid_cm: 250,
    vitals_lock_ms: 999999,     // clamps to hi
  }, r);
  assert.strictEqual(nvs.u32.sns_debounce, 500);
  assert.strictEqual(nvs.u32.sns_clear, 5000);
  assert.strictEqual(nvs.u32.sns_near, 100);
  assert.strictEqual(nvs.u32.sns_mid, 250);
  const vlock = r.knobs.find((k) => k.id === "vitals_lock_ms");
  assert.strictEqual(nvs.u32.sns_vlock, vlock.bounds[1]);
  assert.ok(!("sns_vlost" in nvs.u32), "only provided knobs are carried");
  assert.deepStrictEqual(c.reflexValuesToNvs(null, r), { u32: {} });
  // a reflex preset seeds a valid NVS image end to end
  const img = c.buildNvsSeedImage({ u32: nvs.u32 }, 0x6000);
  const items = c.parseNvs(img);
  assert.ok(items.some((i) => i.key === "sns_debounce" && i.value === 500 && i.namespace === "securacv"));
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
    otadata: { updatesSeen: 4 },
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

// ── the Nursery: wifi for every board, the radar's voice, the roster ────────
test("flash.json: every product declares its wifi NVS scheme, honestly", () => {
  for (const p of catalog.products) {
    assert.ok(["blob", "string"].includes(p.wifi_nvs), `${p.id}: wifi_nvs`);
    // The generator derives this from each firmware's own source: the
    // getString-reading runtime_configs (sense/vision/display) take string
    // entries; the ap family (canary/wap) reads blobs.
    if (p.provisioning === "ap") assert.strictEqual(p.wifi_nvs, "blob", p.id);
    else assert.strictEqual(p.wifi_nvs, "string", p.id);
  }
});

test("flash.json: the display boards are flashable products now", () => {
  const displays = catalog.products.filter((p) => p.role === "display");
  assert.strictEqual(displays.length, 2, "watch + dash");
  for (const p of displays) {
    assert.strictEqual(p.chip, "ESP32-S3");
    assert.strictEqual(p.provisioning, "on-glass");
    assert.ok(p.provisioning_note.length > 40, `${p.id}: provisioning copy`);
    assert.ok(p.hatch && /glass/i.test(p.hatch.title), `${p.id}: a display's hatch is the glass`);
    // and each flashable display still has its 1:1 emulator twin
    const twin = catalog.displays.find((d) => p.id.includes(d.id));
    assert.ok(twin, `${p.id}: no emulator twin in catalog.displays`);
  }
});

test("buildNvsSeedImage: string-scheme wifi round-trips (the sense/vision read path)", async () => {
  const c = await core();
  const img = c.buildNvsSeedImage({
    wifi: { ssid: "birdhouse", pass: "chirpchirp" },
    wifiScheme: "string",
    u32: { sns_debounce: 500 },
  }, 0x6000);
  const items = c.parseNvs(img, ["wifi_ssid", "wifi_pass"]);
  const get = (k) => items.find((i) => i.key === k && i.namespace === "securacv");
  const ssid = get("wifi_ssid");
  assert.strictEqual(ssid.type, 0x21, "a STRING entry — getString finds it");
  assert.strictEqual(new TextDecoder().decode(ssid.bytes), "birdhouse");
  assert.strictEqual(new TextDecoder().decode(get("wifi_pass").bytes), "chirpchirp");
  assert.ok(!get("wifi_en"), "wifi_en is the blob family's key — not written here");
  assert.strictEqual(get("sns_debounce").value, 500, "reflex seeds ride the same image");
  // and the blob scheme is untouched by the addition
  const blobImg = c.buildNvsSeedImage({ wifi: { ssid: "nest", pass: "chirpchirp" } }, 0x4000);
  const blobItems = c.parseNvs(blobImg, ["wifi_ssid"]);
  const bssid = blobItems.find((i) => i.key === "wifi_ssid");
  assert.strictEqual(bssid.type, 0x42, "blob by default, as before");
});

test("parseSenseLine: the radar bench reads every console shape", async () => {
  const c = await core();
  assert.deepStrictEqual(c.parseSenseLine("[sense] present count=1 range=near"),
    { kind: "sense", presence: "present", count: "1", range: "near" });
  assert.deepStrictEqual(c.parseSenseLine("[sense] clear count=0 range=unknown"),
    { kind: "sense", presence: "clear", count: "0", range: "unknown" });
  assert.deepStrictEqual(c.parseSenseLine("[sense] present count=2+ range=far"),
    { kind: "sense", presence: "present", count: "2+", range: "far" });
  assert.deepStrictEqual(c.parseSenseLine("[presence] -> present"),
    { kind: "presence", presence: "present", stalled: false });
  assert.strictEqual(c.parseSenseLine("[presence] -> unknown (radar stall)").stalled, true);
  assert.deepStrictEqual(c.parseSenseLine("[vitals] breath=14 heart=67 bpm"),
    { kind: "bpm", breath: 14, heart: 67 });
  assert.deepStrictEqual(c.parseSenseLine("[vitals] breathing locked"),
    { kind: "vitals", locked: true, stalled: false });
  assert.strictEqual(c.parseSenseLine("[vitals] breathing lost (stall)").locked, false);
  const h = c.parseSenseLine("[health] up 120s  heap 187KB  frame_errs 0");
  assert.deepStrictEqual(h, { kind: "health", up_s: 120, heap_kb: 187, frame_errs: 0 });
  assert.strictEqual(c.parseSenseLine("random noise"), null);
  assert.strictEqual(c.parseSenseLine(""), null);
});

test("roster: add, find, and the progression lines", async () => {
  const c = await core();
  let r = c.rosterAdd([], { t: 1000000, mac: "a4:cf:12:00:a4:3b", product: "Canary Sense",
    version: "2.2.0", preset: "Bedside / sleep watch", wifi: true });
  r = c.rosterAdd(r, { t: 1300000, mac: "a4:cf:12:00:ff:01", product: "Canary Vision",
    version: "2.2.0", preset: null, wifi: false });
  assert.strictEqual(r.length, 2);
  assert.strictEqual(r[0].n, 1);
  assert.strictEqual(r[1].n, 2);
  const found = c.rosterFind(r, "a4:cf:12:00:a4:3b");
  assert.strictEqual(found.product, "Canary Sense");
  assert.strictEqual(found.wifi, true);
  assert.strictEqual(c.rosterFind(r, "no:pe"), null);
  assert.strictEqual(c.rosterFind(r, null), null);
  assert.strictEqual(c.macTail("a4:cf:12:00:a4:3b"), "…a4:3b");
  const lines = c.rosterLines(r, 1300000 + 120000);
  assert.strictEqual(lines[0].label, "Canary Sense v2.2.0");
  assert.strictEqual(lines[0].mac, "…a4:3b");
  assert.ok(lines[0].extras.includes("Bedside"));
  assert.ok(lines[0].extras.includes("WiFi baked"));
  assert.strictEqual(lines[0].ago, "7 min ago");
  assert.strictEqual(lines[1].extras, "");
  assert.strictEqual(lines[1].ago, "2 min ago");
  // never store credentials — the entry shape is public facts only
  assert.deepStrictEqual(Object.keys(found).sort(),
    ["mac", "n", "preset", "product", "t", "version", "wifi"]);
});

test("parseWapLine: the field bench reads the WAP's transition lines", async () => {
  const c = await core();
  const ev = c.parseWapLine("[wap] rf_presence_started devices=2 confidence=high dwell=transient stir=42");
  assert.deepStrictEqual(ev, {
    kind: "wap", event: "rf_presence_started", devices: 2, confidence: "high",
    dwell: "transient", stir: 42, present: true, departed: false,
  });
  const gone = c.parseWapLine("[wap] rf_presence_departed devices=0 confidence=low dwell=sustained stir=3");
  assert.strictEqual(gone.departed, true);
  assert.strictEqual(gone.present, false);
  assert.strictEqual(c.parseWapLine("[wap] sustained_presence devices=1 confidence=moderate dwell=sustained stir=180").stir, 100);
  assert.strictEqual(c.parseWapLine("[sense] present count=1 range=near"), null);
  assert.strictEqual(c.parseWapLine(""), null);
});

test("flash.json: the full-polish help topics all exist (explainers for everything)", () => {
  const h = catalog.settings_help;
  for (const id of ["chip", "mac", "flash_size", "updates_seen", "boots", "witness_records",
                    "tamper_flag", "crash_record", "self_check", "temperature",
                    "health_check", "serial_monitor", "rescue", "verdict", "journey",
                    "roster", "chirps", "radar_bench", "field_bench"]) {
    assert.ok(h[id] && h[id].label && h[id].what, `missing help topic: ${id}`);
    assert.ok(h[id].what.length > 40, `${id}: help copy too thin to teach`);
  }
});

test("chirp module: safe headless — off by default, every call a quiet no-op", async () => {
  const c = await import("../assets/chirp.js");
  assert.strictEqual(c.chirpsEnabled(), false, "sound is invited, never sprung");
  assert.doesNotThrow(() => c.chirp("hello"));
  assert.doesNotThrow(() => c.chirp("hatch"));
  assert.doesNotThrow(() => c.chirp("no-such-song"));
  assert.doesNotThrow(() => c.setChirpsEnabled(true)); // no localStorage here — still safe
});
