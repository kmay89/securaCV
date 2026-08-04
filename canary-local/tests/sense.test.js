// canary-local/tests/sense.test.js — the Sense page's honesty gate.
//
// Two jobs, mirroring tests/wap.test.js:
//  1. Cross-check devices/sense.json against its sources of truth (the
//     canary-sense firmware, the flavor configs, the MR60 driver headers,
//     the design doc, the registry) so a hand-edit that bypasses the
//     generator — or firmware drift — is caught here, not just by the
//     generator's own asserts. Every threshold, topic, HA entity, boot line
//     and LED color the page shows must still exist in the source it
//     claims to come from.
//  2. Exercise the DOM-free cores the page ships (sense-ui.js: bootLines,
//     rangeBandOf, countBucketOf, makePresenceFSM, makeVitalsFSM) so the
//     placement lab provably runs the firmware's semantics at the
//     firmware's constants.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const PRJ = join(REPO, "firmware/projects/canary-sense");

const read = (p) => readFileSync(p, "utf8");
const data = JSON.parse(read(join(ROOT, "devices/sense.json")));
const registry = JSON.parse(read(join(ROOT, "devices/registry.json")));

const mainCpp = read(join(PRJ, "src/main.cpp"));
const mqttCpp = read(join(PRJ, "src/net/mqtt_mgr.cpp"));
const discCpp = read(join(PRJ, "src/ha/ha_discovery.cpp"));
const topicsH = read(join(PRJ, "include/canary/topics.h"));
const versionH = read(join(PRJ, "include/canary/version.h"));
const cfgDefault = read(join(REPO, "firmware/configs/canary-sense/default/config.h"));
const cfgWellbeing = read(join(REPO, "firmware/configs/canary-sense/wellbeing/config.h"));
const pinsH = read(join(REPO, "firmware/boards/xiao-esp32c6-mr60/pins/pins.h"));
const uartH = read(join(REPO, "firmware/common/sensors/mmwave_mr60/mr60_uart.h"));
const design = read(join(REPO, "docs/canary_sense_mr60bha2_design.md"));
const readme = read(join(PRJ, "README.md"));

const cint = (src, macro) => Number(src.match(new RegExp("#define\\s+" + macro + "\\s+(\\d+)"))[1]);

// ── 1. shape + sanity floors ───────────────────────────────────────────────
test("sense.json has every section the page requires", () => {
  for (const k of ["device", "radar", "fsm", "events", "provisioning", "serial",
                   "mqtt", "use_cases", "capabilities", "placement", "tuning", "sandbox", "docs"])
    assert.ok(data[k], "missing section: " + k);
});

test("counts are not thin (a broken parse would fail here)", () => {
  assert.ok(data.mqtt.discovery.entities.length >= 14, "too few HA entities");
  assert.strictEqual(data.radar.protocol.frames.length, 5);
  assert.ok(data.serial.banner.length >= 20, "banner too short");
  assert.ok(data.serial.boot.length >= 10, "boot log too short");
  assert.ok(data.sandbox.length >= 6, "too few sandbox scenarios");
  assert.ok(data.tuning.knobs.length >= 5 && data.tuning.errors.length >= 6, "tuning thin");
  assert.ok(data.placement.mounts.length === 3 && data.placement.avoid.length >= 5, "placement thin");
});

// ── 2. version + identity cross-checks ─────────────────────────────────────
test("firmware version matches version.h and rides the registry train", () => {
  const m = versionH.match(/CANARY_FW_VERSION\s+"([^"]+)"/);
  assert.strictEqual(data.device.fw_version, m[1]);
  assert.strictEqual(data.device.fw_train, registry.fw_train);
  assert.ok(data.device.fw_version.startsWith(data.device.fw_train));
});

test("device identity matches the flavor config and the board pins header", () => {
  assert.ok(cfgDefault.includes('CS_DEVICE_TYPE          "' + data.device.device_type + '"'));
  assert.ok(pinsH.includes('BOARD_NAME              "' + data.device.board + '"'));
  assert.ok(pinsH.includes('BOARD_ID                "' + data.device.board_id + '"'));
});

// ── 3. every FSM threshold the page teaches is the firmware's own ──────────
test("presence thresholds are configs/canary-sense/default/config.h verbatim", () => {
  const p = data.fsm.presence;
  assert.strictEqual(p.debounce_ms, cint(cfgDefault, "CS_PRESENT_DEBOUNCE_MS"));
  assert.strictEqual(p.clear_ms, cint(cfgDefault, "CS_CLEAR_TIMEOUT_MS"));
  assert.strictEqual(p.stall_ms, cint(cfgDefault, "CS_RADAR_STALL_MS"));
  assert.strictEqual(p.near_cm, cint(cfgDefault, "CS_RANGE_NEAR_CM"));
  assert.strictEqual(p.mid_cm, cint(cfgDefault, "CS_RANGE_MID_CM"));
});

test("vitals thresholds are the wellbeing config's own", () => {
  const v = data.fsm.vitals;
  assert.strictEqual(v.lock_ms, cint(cfgWellbeing, "CS_VITALS_LOCK_MS"));
  assert.strictEqual(v.lost_ms, cint(cfgWellbeing, "CS_VITALS_LOST_MS"));
  assert.deepStrictEqual(v.breath_bpm, [cint(cfgWellbeing, "CS_BREATH_MIN_BPM"), cint(cfgWellbeing, "CS_BREATH_MAX_BPM")]);
  assert.deepStrictEqual(v.heart_bpm, [cint(cfgWellbeing, "CS_HEART_MIN_BPM"), cint(cfgWellbeing, "CS_HEART_MAX_BPM")]);
});

test("the LED grammar is main.cpp's led_for_presence, color for color", () => {
  const rgb = {};
  for (const s of data.fsm.presence.states) rgb[s.name] = s.rgb;
  assert.ok(mainCpp.includes(`led_show(${rgb.Present.join(", ")})`), "Present LED drifted");
  assert.ok(mainCpp.includes(`led_show(${rgb.Clear.join(", ")})`), "Clear LED drifted");
  assert.ok(mainCpp.includes(`led_show(${rgb.Unknown.join(", ")})`), "Unknown LED drifted");
});

test("event names + chokepoint vocabulary are real firmware strings", () => {
  for (const e of data.events) assert.ok(mainCpp.includes('"' + e + '"'), "event drifted: " + e);
  for (const w of ["present", "clear", "unknown", "near", "mid", "far", "2+"])
    assert.ok(mainCpp.includes('"' + w + '"'), "vocab drifted: " + w);
  assert.ok(mainCpp.includes("(now_ms / 1000UL / 600UL) * 600UL"), "10-min bucketing drifted");
});

// ── 4. the radar protocol facts trace to the vendored decoder ──────────────
test("wire-protocol frame ids are mr60_uart.h's constants", () => {
  for (const f of data.radar.protocol.frames)
    assert.ok(uartH.includes(f.id), "frame id drifted: " + f.id + " (" + f.name + ")");
  assert.ok(uartH.includes("MR60_SOF = " + data.radar.protocol.sof));
  assert.ok(uartH.includes("c ^= b; c = ~c;"), "checksum recipe drifted");
});

test("the kit wiring facts are pins.h's own", () => {
  assert.match(data.radar.link, new RegExp("TX GPIO" + cint(pinsH, "RADAR_UART_TX")));
  assert.match(data.radar.link, new RegExp("RX GPIO" + cint(pinsH, "RADAR_UART_RX")));
  assert.match(data.radar.link, new RegExp(String(cint(pinsH, "RADAR_UART_BAUD"))));
});

// ── 5. provisioning is the README's own quickstart ─────────────────────────
test("every provisioning command is in the firmware README", () => {
  for (const s of data.provisioning.steps)
    for (const cmd of s.cmd.split("   # or: "))
      assert.ok(readme.includes(cmd.trim()), "quickstart cmd drifted: " + cmd);
});

test("no-portal claims are true: no HTTP server, no SoftAP in the config", () => {
  assert.ok(cfgDefault.includes("#define FEATURE_HTTP_SERVER         0"));
  assert.ok(cfgDefault.includes("#define FEATURE_WIFI_AP             0"));
});

test("Track B facts are the design doc's own", () => {
  assert.ok(design.includes("mqtt_statestream"));
  assert.ok(design.includes("adapter-attested"));
  assert.ok(design.includes("seeed_mr60bha2"));
});

// ── 6. MQTT topics + HA discovery trace to the firmware ────────────────────
test("every topic suffix is built by topics.h", () => {
  for (const t of data.mqtt.topics.concat(data.mqtt.subscribed))
    assert.ok(topicsH.includes('"securacv/%s/' + t.suffix + '"'), "topic not in topics.h: " + t.suffix);
});

test("every HA entity object_id + name is in ha_discovery.cpp", () => {
  for (const e of data.mqtt.discovery.entities) {
    assert.ok(discCpp.includes('"' + e.object_id + '"'), "entity drifted: " + e.object_id);
    assert.ok(discCpp.includes('\\"name\\":\\"' + e.name + '\\"'), "entity name drifted: " + e.name);
  }
});

test("vitals entities are flavor-gated in the firmware, as the page claims", () => {
  const idx = discCpp.indexOf("#ifdef CANARY_SENSE_VITALS");
  assert.ok(idx > 0, "vitals gate missing");
  for (const e of data.mqtt.discovery.entities.filter((x) => x.flavor !== "default"))
    assert.ok(discCpp.indexOf('"' + e.object_id + '"') > idx,
      e.object_id + " must be inside the CANARY_SENSE_VITALS gate");
});

test("only events + identify echo are non-retained", () => {
  for (const t of data.mqtt.topics) {
    const wantRetained = t.suffix !== "events" && t.suffix !== "identify";
    assert.strictEqual(t.retained, wantRetained, t.suffix);
  }
});

test("sandbox scenarios only publish to real topics", () => {
  const suffixes = new Set(data.mqtt.topics.map((t) => t.suffix));
  for (const sc of data.sandbox)
    for (const pub of sc.mqtt || [])
      assert.ok(suffixes.has(pub.suffix), "sandbox publishes unknown topic: " + pub.suffix);
});

// ── 7. serial log lines trace to firmware sources ──────────────────────────
test("boot banner + radar scene anchors exist in the sources", () => {
  const banner = read(join(REPO, "firmware/common/boot/boot_banner.cpp"));
  for (const a of ["Waking up...", "This is your privacy witness device.", "The canary is singing."])
    assert.ok(banner.includes(a), "banner drifted: " + a);
  assert.ok(mainCpp.includes("Who is in the room?"));
  assert.ok(data.serial.banner.some((l) => l.includes("SecuraCV Canary Sense")));
  assert.ok(data.serial.banner.some((l) => l.includes("MR60BHA2 60GHz FMCW radar")));
});

test("runtime serial lines are main.cpp's own printf formats", () => {
  assert.ok(mainCpp.includes('"[presence] -> %s%s"'));
  assert.ok(mainCpp.includes('"[vitals] breathing %s%s"'));
  assert.ok(mainCpp.includes('"[health] up %lus  heap %luKB  frame_errs %lu"'));
});

// ── 8. placement/tuning provenance discipline ──────────────────────────────
test("every placement/tuning claim carries a source label", () => {
  const SRC = new Set(["repo", "seeed", "esphome", "community"]);
  for (const m of data.placement.mounts) assert.ok(SRC.has(m.src), "mount missing src");
  for (const a of data.placement.avoid) assert.ok(SRC.has(a.src), "avoid missing src");
  for (const k of data.tuning.knobs) assert.ok(SRC.has(k.src), "knob missing src");
  for (const e of data.tuning.errors) assert.ok(SRC.has(e.src), "error missing src");
  for (const f of data.tuning.reality.flags) assert.ok(SRC.has(f.src), "reality flag missing src");
});

test("repo-labeled tuning knobs carry the firmware's numbers", () => {
  const knob = (n) => data.tuning.knobs.find((k) => k.name === n);
  assert.strictEqual(knob("present_debounce_ms").value, data.fsm.presence.debounce_ms);
  assert.strictEqual(knob("clear_timeout_ms").value, data.fsm.presence.clear_ms);
  assert.strictEqual(knob("stall_timeout_ms").value, data.fsm.presence.stall_ms);
});

test("the single-target vitals rule is a code rule, and the page says so", () => {
  assert.ok(mainCpp.includes("const bool single_target = (pev.count == CountBucket::One);"));
  const err = data.tuning.errors.find((e) => e.cause.includes("2+ people"));
  assert.ok(err && err.src === "repo");
});

// ── 9. DOM-free cores (sense-ui.js) — the lab runs firmware semantics ──────
test("rangeBandOf mirrors band_of() at the firmware's gates", async () => {
  const { rangeBandOf } = await import("../assets/sense-ui.js");
  const cfg = data.fsm.presence;
  assert.strictEqual(rangeBandOf(cfg.near_cm, cfg), "near");
  assert.strictEqual(rangeBandOf(cfg.near_cm + 1, cfg), "mid");
  assert.strictEqual(rangeBandOf(cfg.mid_cm, cfg), "mid");
  assert.strictEqual(rangeBandOf(cfg.mid_cm + 1, cfg), "far");
  assert.strictEqual(rangeBandOf(0, cfg), "unknown");
});

test("countBucketOf buckets 0/1/2+ and never a precise count", async () => {
  const { countBucketOf } = await import("../assets/sense-ui.js");
  assert.strictEqual(countBucketOf(0), "0");
  assert.strictEqual(countBucketOf(1), "1");
  assert.strictEqual(countBucketOf(2), "2+");
  assert.strictEqual(countBucketOf(7), "2+");
});

test("the presence FSM debounces, clears and stalls at the real constants", async () => {
  const { makePresenceFSM } = await import("../assets/sense-ui.js");
  const cfg = data.fsm.presence;
  const fsm = makePresenceFSM(cfg);
  let t = 1000;
  fsm.reset(t);
  const target = { hasTarget: true, count: 1, distanceCm: 200 };
  const empty = { hasTarget: false, count: 0, distanceCm: 0 };
  // a target must SUSTAIN the debounce window before Present
  fsm.tick(target, t);
  fsm.tick(target, t + cfg.debounce_ms - 50);
  assert.notStrictEqual(fsm.state, "present", "fired before debounce");
  fsm.tick(target, t + cfg.debounce_ms);
  assert.strictEqual(fsm.state, "present");
  assert.strictEqual(fsm.range, "mid");
  // absence must sustain clear_ms before Clear
  t += 5000;
  fsm.tick(empty, t);
  fsm.tick(empty, t + cfg.clear_ms - 50);
  assert.strictEqual(fsm.state, "present", "cleared too early");
  fsm.tick(empty, t + cfg.clear_ms);
  assert.strictEqual(fsm.state, "clear");
  // silence (no frame at all) past stall_ms is Unknown — deadline before data
  const ev = fsm.tick(null, t + cfg.clear_ms + cfg.stall_ms);
  assert.strictEqual(fsm.state, "unknown");
  assert.ok(ev.stalled, "stall transition must be flagged");
});

test("the vitals FSM locks only on sustained single-target vitals", async () => {
  const { makeVitalsFSM } = await import("../assets/sense-ui.js");
  const v = data.fsm.vitals;
  const fsm = makeVitalsFSM(v);
  let t = 1000;
  fsm.reset(t);
  fsm.tick(true, true, t);
  fsm.tick(true, true, t + v.lock_ms - 50);
  assert.notStrictEqual(fsm.lock, "locked", "locked before the confirm window");
  fsm.tick(true, true, t + v.lock_ms);
  assert.strictEqual(fsm.lock, "locked");
  // a second person (singleTarget=false) must drop the lock after lost_ms
  t += 10000;
  fsm.tick(true, false, t);
  fsm.tick(true, false, t + v.lost_ms);
  assert.strictEqual(fsm.lock, "lost", "2+ targets must lose the lock");
});

test("bootLines flattens banner+boot+ready in order with mapped classes", async () => {
  const { bootLines } = await import("../assets/sense-ui.js");
  const lines = bootLines(data.serial);
  assert.strictEqual(lines.length,
    data.serial.banner.length + data.serial.boot.length + data.serial.ready.length);
  for (const l of lines) { assert.ok(typeof l.text === "string"); assert.ok(/^wap-/.test(l.cls)); }
});
