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

test("sense presets: pet & sleep presets encode the researched feasibility contract", () => {
  const wb = catalog.products.find((p) => p.id === "securacv-canary-sense-wellbeing");
  const def = catalog.products.find((p) => p.id === "securacv-canary-sense");
  const byId = (p) => Object.fromEntries(p.reflexes.presets.map((pr) => [pr.id, pr]));
  const wbP = byId(wb), defP = byId(def);

  // Mouse: a MOVEMENT preset, on BOTH builds, that never CHANGES the vitals
  // bands — a mouse's heart/breath sit far outside this module's reported
  // band, so the preset must leave every breath/heart/lock knob at the shipped
  // default rather than pretend to tune it. (Catalog presets carry a fully
  // resolved value map, so the honest check is "equals ships", not "absent".)
  for (const P of [wbP, defP]) {
    assert.ok(P.mouse_cage, "mouse_cage preset present on every Sense build");
    const ships = P.ships.values;
    for (const k of ["breath_min_bpm", "breath_max_bpm", "heart_min_bpm", "heart_max_bpm",
                     "vitals_lock_ms", "vitals_lost_ms"]) {
      if (!(k in ships)) continue; // presence-only build has no vitals knobs at all
      assert.strictEqual(P.mouse_cage.values[k], ships[k],
        `mouse_cage must not retune ${k} — a mouse's vitals are unreadable by this module`);
    }
    // Close cage mount: the tightest near band the firmware allows.
    assert.strictEqual(P.mouse_cage.values.range_near_cm, 50, "mouse cage mount is close-range");
  }
  // And the movement preset genuinely moves the PRESENCE knobs off shipped.
  assert.notStrictEqual(wbP.mouse_cage.values.clear_timeout_ms, wbP.ships.values.clear_timeout_ms,
    "mouse_cage should tune the clear timeout (stillness → asleep) off the default");

  // Dog & human sleep: VITALS presets, wellbeing-only (the presence-only build
  // has no breath/heart knobs, so offering them there would be dishonest).
  for (const id of ["dog_kennel", "human_sleep"]) {
    assert.ok(wbP[id], `${id} present on the wellbeing build`);
    assert.ok(!defP[id], `${id} must be hidden on the presence-only build (no vitals knobs)`);
    for (const k of ["breath_min_bpm", "breath_max_bpm", "heart_min_bpm", "heart_max_bpm"]) {
      assert.ok(Number.isInteger(wbP[id].values[k]), `${id} sets ${k}`);
    }
  }

  // Dog bands must be a real superset of the human bands (a dog's resting rates
  // run faster and its small breeds' hearts run higher), and min<max holds.
  const d = wbP.dog_kennel.values, h = wbP.human_sleep.values;
  assert.ok(d.breath_max_bpm > h.breath_max_bpm, "dog breathing band reaches higher than human");
  assert.ok(d.heart_max_bpm > h.heart_max_bpm, "dog heart band reaches higher than human (small breeds)");
  for (const v of [d, h]) {
    assert.ok(v.breath_min_bpm < v.breath_max_bpm, "breath band min < max");
    assert.ok(v.heart_min_bpm < v.heart_max_bpm, "heart band min < max");
  }

  // Every pet/sleep preset has an ⓘ explainer that states the feasibility honestly.
  for (const id of ["mouse_cage", "dog_kennel", "human_sleep"]) {
    assert.ok(catalog.settings_help[id] && catalog.settings_help[id].what,
      `${id} needs a help topic stating what it can and can't do`);
  }
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

test("buildNvsSeedImage: auto_upd rides along and round-trips under the OTA engine's namespace", async () => {
  const c = await core();
  const img = c.buildNvsSeedImage({
    wifi: { ssid: "birdhouse", pass: "hunter22" },
    u8: { det_score: 85 },
    autoUpdate: true,
  }, 0x6000);
  const items = c.parseNvs(img);
  // The engine (securacv_ota.cpp) opens its OWN namespace — the key must sit
  // there, not under "securacv" with everything else.
  const auto = items.find((i) => i.namespace === "securacv_ota" && i.key === "auto_upd");
  assert.ok(auto, "auto_upd must be readable under securacv_ota");
  assert.strictEqual(auto.value, 1);
  assert.ok(!items.some((i) => i.namespace === "securacv" && i.key === "auto_upd"),
    "auto_upd must not leak into the securacv namespace");
  assert.ok(items.some((i) => i.namespace === "securacv" && i.key === "det_score"),
    "the dials stay under securacv beside the second namespace");
  // Declined → an explicit 0 (a recorded choice); left out → no key at all.
  assert.strictEqual(
    c.parseNvs(c.buildNvsSeedImage({ autoUpdate: false }, 0x4000))
      .find((i) => i.namespace === "securacv_ota" && i.key === "auto_upd").value, 0);
  assert.ok(!c.parseNvs(c.buildNvsSeedImage({ u8: { det_score: 70 } }, 0x4000))
    .some((i) => i.key === "auto_upd"), "no opt, no key");
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
  assert.strictEqual(displays.length, 10,
    "watch + dash + dash-modes + dash7 + nightstand7 + nightstand-s3 + touch169 "
    + "+ amoled241 + nightstand-c6 + nightlight-c3");
  for (const p of displays) {
    assert.ok(["ESP32-S3", "ESP32-C6", "ESP32-C3"].includes(p.chip), `${p.id}: chip ${p.chip}`);
    assert.strictEqual(p.provisioning, "on-glass");
    assert.ok(p.provisioning_note.length > 40, `${p.id}: provisioning copy`);
    assert.ok(p.hatch && /glass/i.test(p.hatch.title), `${p.id}: a display's hatch is the glass`);
  }
  // The twin link must never overclaim. A display either links to a twin
  // that REALLY boots (its own, or an aliased sibling that shares its glass
  // and face), or it carries no emulated link at all — what it must never
  // do is deep-link `fleet.html#<id>` for an id the registry has never
  // heard of, which lands on the generic gallery while the copy promises
  // "the same firmware, in the browser".
  const twinIds = new Set(catalog.displays.map((d) => d.id));
  for (const p of displays) {
    const emu = p.prove && p.prove.emulated;
    if (!emu) continue;                       // honest silence is allowed
    const hash = emu.href.split("#")[1];
    assert.ok(hash, `${p.id}: emulated proof must deep-link a twin`);
    assert.ok(twinIds.has(hash),
      `${p.id}: links twin '${hash}' which is not in catalog.displays`);
    // An aliased twin (a sibling's build) must not claim to be the 1:1 one.
    const own = p.id.replace(/^securacv-/, "");
    if (hash !== own) {
      assert.ok(!/1:1/.test(emu.label),
        `${p.id}: borrows ${hash}'s twin, so it must not say "1:1"`);
    }
  }
  // ...and at least the flagships really do have their own.
  for (const id of ["securacv-canary-display-watch", "securacv-canary-display-dash"]) {
    const p = displays.find((d) => d.id === id);
    assert.ok(p.prove.emulated.href.endsWith(id.replace(/^securacv-/, "")),
      `${id}: the flagship displays keep their own 1:1 twin`);
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

test("parseSenseLine: the tuning console's [radar] stream — coarse, vitals, raw", async () => {
  const c = await core();
  // presence-only build: just the coarse triple + link health
  const p = c.parseSenseLine("[radar] state=present count=1 range=near errs=0");
  assert.strictEqual(p.kind, "radar");
  assert.strictEqual(p.presence, "present");
  assert.strictEqual(p.count, "1");
  assert.strictEqual(p.range, "near");
  assert.strictEqual(p.frame_errs, 0);
  assert.strictEqual(p.lock, undefined);
  assert.strictEqual(p.raw, undefined);
  // wellbeing build: lock + P1 numerics ride along
  const w = c.parseSenseLine("[radar] state=present count=1 range=mid lock=locked breath=14 heart=67 errs=2");
  assert.strictEqual(w.lock, "locked");
  assert.strictEqual(w.breath, 14);
  assert.strictEqual(w.heart, 67);
  assert.strictEqual(w.frame_errs, 2);
  assert.strictEqual(c.parseSenseLine("[radar] state=clear count=0 range=unknown lock=lost breath=0 heart=0 errs=0").lock, "lost");
  // `raw on` bench mode: the attended-cable detail block
  const r = c.parseSenseLine(
    "[radar] state=present count=1 range=near lock=locked breath=14 heart=67 " +
    "raw_dist=178cm raw_count=1 raw_breath=14 raw_heart=66 errs=0");
  assert.deepStrictEqual(r.raw, { dist_cm: 178, count: 1, breath: 14, heart: 66 });
  assert.strictEqual(c.parseSenseLine("[radar] state=nonsense count=9 range=weird"), null);
});

test("parseCfgLine: the [cfg] snapshot is the tuning UI's single source of truth", async () => {
  const c = await core();
  const cfg = c.parseCfgLine(
    "[cfg] debounce=300 clear=1500 stall=5000 near=150 mid=350 vlock=4000 " +
    "vlost=6000 breath_min=6 breath_max=30 heart_min=40 heart_max=130 stream=1000 raw=0");
  assert.strictEqual(cfg.kind, "cfg");
  assert.strictEqual(cfg.values.debounce, 300);
  assert.strictEqual(cfg.values.heart_max, 130);
  assert.strictEqual(cfg.values.breath_min, 6);
  assert.strictEqual(cfg.stream, 1000);
  assert.strictEqual(cfg.raw, false);
  // stream/raw are session state, never knobs
  assert.strictEqual(cfg.values.stream, undefined);
  assert.strictEqual(cfg.values.raw, undefined);
  // presence-only build carries fewer knobs — still a valid snapshot
  const p = c.parseCfgLine("[cfg] debounce=200 clear=800 stall=5000 near=150 mid=350 stream=0 raw=1");
  assert.strictEqual(p.values.vlock, undefined);
  assert.strictEqual(p.stream, 0);
  assert.strictEqual(p.raw, true);
  // the firmware's multi-line [CFG] boot block is NOT the snapshot
  assert.strictEqual(c.parseCfgLine("[CFG]"), null);
  assert.strictEqual(c.parseCfgLine("Radar reflexes: debounce=300ms"), null);
  assert.strictEqual(c.parseCfgLine(""), null);
});

test("parseTuneLine: one verdict per command — ok pops, err glows", async () => {
  const c = await core();
  assert.deepStrictEqual(c.parseTuneLine("[tune] ok debounce=500"),
    { kind: "tune", ok: true, text: "debounce=500" });
  assert.deepStrictEqual(c.parseTuneLine("[tune] err unknown knob 'flux' (try 'help')"),
    { kind: "tune", ok: false, text: "unknown knob 'flux' (try 'help')" });
  assert.strictEqual(c.parseTuneLine("[tune] commands:"), null, "help text is not a verdict");
  assert.strictEqual(c.parseTuneLine("[radar] state=clear count=0 range=unknown"), null);
});

test("senseLineTone: the live log classifies every console voice", async () => {
  const c = await core();
  assert.strictEqual(c.senseLineTone("[radar] state=present count=1 range=near errs=0"), "stream");
  assert.strictEqual(c.senseLineTone("[sense] present count=1 range=near"), "sense");
  assert.strictEqual(c.senseLineTone("[presence] -> clear"), "sense");
  assert.strictEqual(c.senseLineTone("[vitals] breathing locked"), "vitals");
  assert.strictEqual(c.senseLineTone("[cfg] debounce=300 stream=1000 raw=0"), "cfg");
  assert.strictEqual(c.senseLineTone("[CFG]"), "cfg");
  assert.strictEqual(c.senseLineTone("[tune] ok near=200"), "tune");
  assert.strictEqual(c.senseLineTone("[tune] err line too long (max 95 chars)"), "err");
  assert.strictEqual(c.senseLineTone("[health] up 120s heap 187KB frame_errs 0"), "health");
  assert.strictEqual(c.senseLineTone("anything else"), "plain");
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

test("minimal module: safe headless — the full story by default, quiet is a choice", async () => {
  const m = await import("../assets/minimal.js");
  assert.strictEqual(m.minimalEnabled(), false, "minimal is opted into, never sprung");
  assert.doesNotThrow(() => m.setMinimalEnabled(true)); // no localStorage here — still safe
  assert.strictEqual(m.minimalEnabled(), false, "no storage means the choice can't stick — still off");
  assert.strictEqual(m.minimalToggle(), null, "no document — no chip, and no throw either");
});

test("flash.json: the coach's lesson deck — real stages, unique ids, teaching-weight copy", () => {
  const deck = catalog.lessons;
  assert.ok(Array.isArray(deck) && deck.length >= 10, "a deck worth dealing");
  const KNOWN_STAGES = new Set(["safety copy", "download", "authentic", "erase", "writ", "any"]);
  const ids = new Set();
  for (const l of deck) {
    assert.ok(l.id && !ids.has(l.id), `duplicate/missing lesson id: ${l.id}`);
    ids.add(l.id);
    assert.ok(KNOWN_STAGES.has(l.stage), `${l.id}: unknown stage tag ${l.stage}`);
    assert.ok(l.title && l.body && l.body.length > 60, `${l.id}: copy too thin to teach`);
  }
  // every long stage has at least one lesson of its own
  for (const s of ["safety copy", "download", "authentic", "erase", "writ"]) {
    assert.ok(deck.some((l) => l.stage === s), `no lesson for the ${s} stage`);
  }
});

test("pickLesson: stage-matched first, generic after, never repeats, ends honestly", async () => {
  const c = await core();
  const shown = [];
  // A backup stage deals a backup lesson first.
  const first = c.pickLesson(catalog, "saving a safety copy of the board first", shown);
  assert.strictEqual(first.stage, "safety copy");
  shown.push(first.id);
  const second = c.pickLesson(catalog, "saving a safety copy of the board first", shown);
  assert.strictEqual(second.stage, "safety copy");
  assert.notStrictEqual(second.id, first.id);
  shown.push(second.id);
  // Backup lessons exhausted → the generic pool takes over.
  const third = c.pickLesson(catalog, "saving a safety copy of the board first", shown);
  assert.strictEqual(third.stage, "any");
  // A write stage prefers write lessons.
  assert.strictEqual(c.pickLesson(catalog, "writing firmware + your settings", []).stage, "writ");
  // Deal the whole deck → the picker says so with null, exactly once dry.
  const all = [];
  for (;;) {
    const l = c.pickLesson(catalog, "writing firmware", all);
    if (!l) break;
    all.push(l.id);
  }
  assert.strictEqual(all.length, catalog.lessons.length, "every lesson reachable");
  assert.strictEqual(c.pickLesson({}, "writing", []), null, "no deck, no coach");
});

test("flash.json: PARITY — every Canary proves itself two ways, real + emulated twin", () => {
  const REAL_KINDS = new Set(["monitor", "bench-field", "bench-camera", "bench-radar", "glass"]);
  const twinIds = new Set(catalog.displays.map((d) => d.id));
  for (const p of catalog.products) {
    const pr = p.prove;
    assert.ok(pr && pr.real, `${p.id}: no real proof — parity broken`);
    assert.ok(REAL_KINDS.has(pr.real.kind), `${p.id}: unknown real proof kind ${pr.real.kind}`);
    assert.ok(pr.real.label && pr.real.how, `${p.id}: real proof needs label + how`);
    if (p.role !== "display") {
      // Every non-display Canary proves itself both ways: its twin is a
      // whole lab page (vision / senselab / …), not a per-board WASM build,
      // so there is never a reason for one to be missing.
      assert.ok(pr.emulated, `${p.id}: no emulated twin — parity broken`);
    }
    if (!pr.emulated) continue;
    assert.ok(pr.emulated.label && pr.emulated.how, `${p.id}: twin needs label + how`);
    // the twin page must actually exist — no dead links, ever
    const page = pr.emulated.href.split("#")[0];
    assert.ok(existsSync(join(ROOT, page)), `${p.id}: twin page missing: ${page}`);
  }
  // A display's twin link must reach a twin that REALLY boots. Its own is
  // the 1:1 case; a sibling that shares its glass and face is allowed but
  // must not call itself 1:1; and a board still waiting on its WASM build
  // carries no emulated link at all rather than a link to nowhere. (Before
  // this, every display deep-linked `#<its own id>` whether the registry
  // knew that id or not — dash-modes and the nightstand C6 both quietly
  // landed on the generic gallery under "the same firmware, in the browser".)
  for (const p of catalog.products.filter((x) => x.role === "display")) {
    if (!p.prove.emulated) continue;
    const hash = p.prove.emulated.href.split("#")[1];
    assert.ok(twinIds.has(hash), `${p.id}: twin '${hash}' is not a real emulator`);
  }
});

test("flash.json: ABOUT — legal & provenance parsed from the files of record", () => {
  const a = catalog.about;
  assert.ok(a, "catalog has an about block");
  assert.ok(a.product.includes("Canary Nursery"), "product names the Nursery");
  assert.ok(/^© \d{4} /.test(a.copyright), "copyright starts © <year>");
  assert.strictEqual(a.license.name, "Apache-2.0");
  assert.ok(existsSync(join(ROOT, "..", a.license.file)), "LICENSE file exists at repo root");
  assert.ok(a.source.startsWith("https://github.com/"), "source links the repository");
  assert.ok(a.privacy.length > 40, "privacy line says something real");
  // every vendored module the flasher loads gets a credit, and every credit
  // links a provenance file that actually exists — no dead links, ever
  const names = a.vendors.map((v) => v.name);
  for (const want of ["esptool-js", "md5", "ed25519", "qrcode"]) {
    assert.ok(names.includes(want), `vendor credit missing: ${want}`);
  }
  for (const v of a.vendors) {
    assert.ok(v.package && v.license, `${v.name}: needs package + license`);
    assert.ok(existsSync(join(ROOT, v.file)), `${v.name}: provenance file missing: ${v.file}`);
  }
});
