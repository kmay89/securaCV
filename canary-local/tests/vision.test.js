// canary-local/tests/vision.test.js — the Vision page's honesty gate.
//
// Two jobs, mirroring tests/wap.test.js:
//  1. Cross-check devices/vision.json against its sources of truth (the
//     canary-vision firmware, the Grove Vision AI V2 docs, the registry,
//     boards.json) so a hand-edit that bypasses the generator — or firmware
//     drift — is caught here, not just by the generator's own asserts.
//  2. Exercise the DOM-free cores the page ships (vision-ui.js: bestBox,
//     bboxToVoxel, iou, nms, aimPayload, makeFsm, bootLines, mqttApply) and
//     pin them against the firmware's own math so the "mirrors the firmware
//     line for line" claim can't rot.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const FW = join(REPO, "firmware/projects/canary-vision");

const read = (p) => readFileSync(p, "utf8");
const data = JSON.parse(read(join(ROOT, "devices/vision.json")));
const registry = JSON.parse(read(join(ROOT, "devices/registry.json")));
const boards = JSON.parse(read(join(ROOT, "devices/boards.json")));

const configH = read(join(FW, "include/canary/config.h"));
const versionH = read(join(FW, "include/canary/version.h"));
const detectCfgH = read(join(FW, "include/canary/detect_config.h"));
const topicsH = read(join(FW, "include/canary/topics.h"));
const mainCpp = read(join(FW, "src/main.cpp"));
const visionMgrCpp = read(join(FW, "src/vision/vision_mgr.cpp"));
const fsmCpp = read(join(FW, "src/state/presence_fsm.cpp"));
const haCpp = read(join(FW, "src/ha/ha_discovery.cpp"));
const guide = read(join(REPO, "docs/hardware/grove_vision_ai_v2_guide.md"));
const gettingStarted = read(join(REPO, "docs/hardware/canary_vision_getting_started.md"));

const num = (src, name) => {
  const m = src.match(new RegExp(name + "\\s*=\\s*(\\d+)"));
  assert.ok(m, name + " not found in firmware");
  return +m[1];
};

// ── 1. shape + sanity floors ───────────────────────────────────────────────
test("vision.json has every section the page requires", () => {
  for (const k of ["device", "module", "ports", "assembly", "model_load", "detect", "serial",
                   "mqtt", "aim", "flash", "placement", "tuning", "sandbox", "roadmap",
                   "troubleshooting", "recovery", "docs"])
    assert.ok(data[k], "missing section: " + k);
});

test("counts are not thin (a broken parse would fail here)", () => {
  assert.strictEqual(data.mqtt.discovery.entities.length, 19);
  assert.ok(data.mqtt.topics.length >= 15, "too few MQTT topics");
  assert.ok(data.serial.boot.length >= 20, "boot log too short");
  assert.ok(data.sandbox.length >= 5, "too few sandbox scenes");
  assert.ok(data.placement.use_cases.length >= 4, "too few placement presets");
  assert.ok(data.troubleshooting.length >= 6, "too few symptom rows");
  assert.strictEqual(data.tuning.length, 4);
  assert.strictEqual(data.device.hosts.length, 3);
});

// ── 2. version + identity cross-checks ─────────────────────────────────────
test("firmware version + identity match the headers and the registry train", () => {
  const m = versionH.match(/#define CANARY_FW_VERSION\s+"([^"]+)"/);
  assert.ok(m, "CANARY_FW_VERSION not found");
  assert.strictEqual(data.device.fw_version, m[1]);
  assert.strictEqual(data.device.fw_train, registry.fw_train);
  assert.match(configH, new RegExp('DEVICE_TYPE\\s*=\\s*"' + data.device.device_type + '"'));
  assert.match(configH, new RegExp('DEVICE_ID\\s*=\\s*"' + data.device.id_example + '"'));
  assert.strictEqual(data.device.board_id, boards.device_board["canary-vision"]);
  assert.ok(boards.boards[data.device.board_id], "board id not in boards.json");
});

// ── 3. detection semantics are the firmware's constants ────────────────────
test("detect block mirrors config.h seeds and detect_config.h bounds", () => {
  assert.strictEqual(data.detect.person_target, num(configH, "PERSON_TARGET"));
  assert.strictEqual(data.detect.score_min, num(configH, "SCORE_MIN"));
  assert.strictEqual(data.detect.lost_timeout_ms, num(configH, "LOST_TIMEOUT_MS"));
  assert.strictEqual(data.detect.dwell_start_ms, num(configH, "DWELL_START_MS"));
  assert.strictEqual(data.detect.voxel.cols, num(configH, "VOXEL_COLS"));
  assert.strictEqual(data.detect.voxel.rows, num(configH, "VOXEL_ROWS"));
  assert.strictEqual(data.detect.frame.w, num(configH, "FRAME_W"));
  assert.strictEqual(data.detect.frame.h, num(configH, "FRAME_H"));
  assert.strictEqual(data.detect.invoke_period_ms, num(configH, "INVOKE_PERIOD_MS"));
  assert.deepStrictEqual(data.detect.bounds.score,
    [num(detectCfgH, "DETECT_SCORE_MIN_LO"), num(detectCfgH, "DETECT_SCORE_MIN_HI")]);
  assert.deepStrictEqual(data.detect.bounds.lost_ms,
    [num(detectCfgH, "DETECT_LOST_MS_LO"), num(detectCfgH, "DETECT_LOST_MS_HI")]);
  assert.deepStrictEqual(data.detect.bounds.dwell_ms,
    [num(detectCfgH, "DETECT_DWELL_MS_LO"), num(detectCfgH, "DETECT_DWELL_MS_HI")]);
});

test("aim block mirrors the firmware's cadence constants + payload keys", () => {
  assert.strictEqual(data.aim.publish_ms, num(configH, "AIM_PUBLISH_MS"));
  assert.strictEqual(data.aim.idle_publish_ms, num(configH, "AIM_IDLE_PUBLISH_MS"));
  assert.strictEqual(data.aim.auto_off_ms, num(configH, "AIM_AUTO_OFF_MS"));
  for (const k of data.aim.payload_keys)
    assert.ok(mainCpp.includes('\\"' + k + '\\"'), "aim payload key drifted: " + k);
});

test("event vocabulary is exactly the FSM's emit set", () => {
  const emitted = [...fsmCpp.matchAll(/emit\(out_event,\s*"([a-z_]+)"/g)].map((m) => m[1]);
  assert.deepStrictEqual(data.detect.events, [...new Set(emitted)].sort());
  for (const sc of data.sandbox)
    if (sc.event) assert.ok(emitted.includes(sc.event), "sandbox claims unknown event: " + sc.event);
});

// ── 4. MQTT topics + HA entities trace to the firmware ─────────────────────
test("every topic suffix is a real template in topics.h", () => {
  const suffixes = [...topicsH.matchAll(/"securacv\/%s\/([a-z_/]+)"/g)].map((m) => m[1]);
  assert.deepStrictEqual(data.mqtt.topics.map((t) => t.suffix), suffixes,
    "vision.json topics diverge from topics.h");
});

test("every HA discovery entity name is a real literal in ha_discovery.cpp", () => {
  for (const e of data.mqtt.discovery.entities) {
    const inJson = haCpp.includes('\\"name\\":\\"' + e.name + '\\"');
    const inNumbers = new RegExp('"' + e.name + '"').test(haCpp);
    assert.ok(inJson || inNumbers, "entity not in firmware: " + e.name);
  }
});

test("cfg example carries the firmware's own JSON keys", () => {
  for (const k of Object.keys(data.mqtt.cfg_state_example))
    assert.ok(data.tuning.some((t) => t.key === k), "cfg key not in tuning table: " + k);
});

// ── 5. serial boot lines trace to firmware sources ─────────────────────────
test("boot anchors exist in the firmware's serial output", () => {
  const wifiCpp = read(join(FW, "src/net/wifi_mgr.cpp"));
  const mqttCpp = read(join(FW, "src/net/mqtt_mgr.cpp"));
  const banner = read(join(REPO, "firmware/common/boot/boot_banner.cpp"));
  assert.ok(banner.includes("Waking up..."));
  assert.ok(banner.includes("This is your privacy witness device."));
  assert.ok(mainCpp.includes("What can I see?"));
  assert.ok(mainCpp.includes("Connecting to MQTT..."));
  assert.ok(visionMgrCpp.includes("Grove Vision AI ID=%d"));
  assert.ok(wifiCpp.includes("Connected IP=%s RSSI=%ddBm"));
  assert.ok(mqttCpp.includes('log_line("MQTT", "Connected.")'));
  assert.ok(haCpp.includes("Home Assistant discovery published (retained)."));
  assert.ok(data.serial.banner.some((l) => l.includes("SecuraCV Canary Vision")));
  assert.ok(data.serial.boot.some((l) => l.text.includes("Grove Vision AI ID=")));
});

// ── 6. docs-sourced sections still match the docs ──────────────────────────
test("two-port rule, model-load steps and symptoms are the docs' own", () => {
  assert.ok(guide.includes("Model work → module port. Firmware work → XIAO port."));
  assert.ok(data.model_load.url.startsWith("https://sensecraft.seeed.cc/"));
  assert.ok(gettingStarted.includes(data.model_load.url));
  assert.ok(gettingStarted.includes("Select Model → Person Detection"));
  assert.ok(guide.includes("we2_iic_bootloader_recover"));
  for (const t of data.troubleshooting.slice(0, 3))
    assert.ok(guide.includes(t.symptom.slice(0, 20)), "symptom drifted: " + t.symptom);
});

test("placement presets stay inside the firmware's tunable bounds", () => {
  const b = data.detect.bounds;
  for (const uc of data.placement.use_cases) {
    assert.ok(uc.preset.score >= b.score[0] && uc.preset.score <= b.score[1], uc.id + " score");
    assert.ok(uc.preset.lost_ms >= b.lost_ms[0] && uc.preset.lost_ms <= b.lost_ms[1], uc.id + " lost");
    assert.ok(uc.preset.dwell_ms >= b.dwell_ms[0] && uc.preset.dwell_ms <= b.dwell_ms[1], uc.id + " dwell");
  }
});

// ── 7. DOM-free cores mirror the firmware's math ───────────────────────────
test("bestBox: class filter, threshold, highest score — vision_mgr.cpp's rule", async () => {
  const { bestBox } = await import("../assets/vision-ui.js");
  // the rule as written in the firmware
  assert.ok(visionMgrCpp.includes("if (b.target != det.person_target) continue;"));
  assert.ok(visionMgrCpp.includes("if (b.score < det.score_min) continue;"));
  const cfg = { person_target: 0, score_min: 70 };
  const boxes = [
    { x: 10, y: 10, w: 5, h: 5, score: 95, target: 8 },   // cat: right score, wrong class
    { x: 20, y: 20, w: 5, h: 5, score: 60, target: 0 },   // person under threshold
    { x: 30, y: 30, w: 5, h: 5, score: 80, target: 0 },
    { x: 40, y: 40, w: 5, h: 5, score: 91, target: 0 },   // best
  ];
  assert.strictEqual(bestBox(boxes, cfg).score, 91);
  assert.strictEqual(bestBox([boxes[0], boxes[1]], cfg), null);
  assert.strictEqual(bestBox([], cfg), null);
  // exactly-at-threshold is kept (firmware uses <, not <=)
  assert.strictEqual(bestBox([{ x: 0, y: 0, w: 1, h: 1, score: 70, target: 0 }], cfg).score, 70);
});

test("bboxToVoxel: the firmware's integer math, clamps included", async () => {
  const { bboxToVoxel } = await import("../assets/vision-ui.js");
  assert.ok(visionMgrCpp.includes("c = (px * C) / FRAME_W;"));
  const g = { cols: 3, rows: 3, w: 240, h: 240 };
  // center of frame lands center cell
  assert.deepStrictEqual(bboxToVoxel({ x: 100, y: 100, w: 40, h: 40 }, g),
    { r: 1, c: 1, rows: 3, cols: 3 });
  // exactly at the right edge clamps to the last cell (cx=240 → c=3 → clamp 2)
  assert.deepStrictEqual(bboxToVoxel({ x: 220, y: 220, w: 40, h: 40 }, g).c, 2);
  assert.deepStrictEqual(bboxToVoxel({ x: -10, y: -10, w: 4, h: 4 }, g), { r: 0, c: 0, rows: 3, cols: 3 });
  // integer division truncation matches C: cx=79 → 79*3/240 = 0.9875 → 0
  assert.strictEqual(bboxToVoxel({ x: 79, y: 0, w: 0, h: 0 }, g).c, 0);
  assert.strictEqual(bboxToVoxel({ x: 80, y: 0, w: 0, h: 0 }, g).c, 1);
});

test("aimPayload: the firmware's key set, key for key", async () => {
  const { aimPayload } = await import("../assets/vision-ui.js");
  const g = { cols: 3, rows: 3, w: 240, h: 240 };
  const p = aimPayload({ person_now: true, bbox: { x: 96, y: 88, w: 64, h: 128, score: 91 },
                         voxel: { r: 1, c: 1 } }, g);
  assert.deepStrictEqual(Object.keys(p), data.aim.payload_keys);
  assert.strictEqual(p.present, true);
  assert.strictEqual(p.fw, 240);
  const empty = aimPayload({ person_now: false }, g);
  assert.strictEqual(empty.present, false);
  assert.strictEqual(empty.vr, -1);
});

test("makeFsm: the event order presence_fsm.cpp enforces", async () => {
  const { makeFsm } = await import("../assets/vision-ui.js");
  const cfg = { dwell_start_ms: 1000, lost_timeout_ms: 500,
                interaction_window_ms: 3000, zone_interaction_ms: 2500 };
  const fsm = makeFsm();
  assert.strictEqual(fsm.tick(true, 0, cfg), "presence_started");
  assert.strictEqual(fsm.tick(true, 100, cfg), null);
  assert.strictEqual(fsm.tick(true, 1000, cfg), "dwell_started");
  assert.strictEqual(fsm.tick(true, 1100, cfg), null); // latches the dwell
  // silence: dwell_ended fires the tick before presence_ended (firmware order)
  assert.strictEqual(fsm.tick(false, 1500, cfg), null);      // within lost timeout
  assert.strictEqual(fsm.tick(false, 1700, cfg), "dwell_ended");
  assert.strictEqual(fsm.tick(false, 1701, cfg), "presence_ended");
  // the qualified (dwelled) visit signs interaction_likely inside the window
  assert.strictEqual(fsm.tick(false, 1800, cfg), "interaction_likely");
  assert.strictEqual(fsm.lastReason, "dwell_then_left");
  assert.strictEqual(fsm.tick(false, 1900, cfg), null); // emitted once, not again
  // a short visit skips dwell — and earns no interaction event
  assert.strictEqual(fsm.tick(true, 6000, cfg), "presence_started");
  assert.strictEqual(fsm.tick(false, 6600, cfg), "presence_ended");
  assert.strictEqual(fsm.tick(false, 6700, cfg), null);
  assert.strictEqual(fsm.lastReason, null);
});

test("makeFsm: the stable-zone latch qualifies a visit without dwell", async () => {
  const { makeFsm } = await import("../assets/vision-ui.js");
  // dwell far away so only the zone path can qualify (firmware lines 61-66)
  const cfg = { dwell_start_ms: 60000, lost_timeout_ms: 500,
                interaction_window_ms: 3000, zone_interaction_ms: 2500 };
  const fsm = makeFsm();
  assert.strictEqual(fsm.tick(true, 0, cfg, "1,1"), "presence_started");
  assert.strictEqual(fsm.tick(true, 1000, cfg, "1,1"), null);
  assert.strictEqual(fsm.tick(true, 2600, cfg, "1,1"), null); // zone window passed → latch
  assert.strictEqual(fsm.tick(false, 3200, cfg), "presence_ended");
  assert.strictEqual(fsm.tick(false, 3300, cfg), "interaction_likely");
  assert.strictEqual(fsm.lastReason, "zone_interaction_then_left");
  // moving between cells restarts the stable clock: no latch, no event
  const fsm2 = makeFsm();
  fsm2.tick(true, 0, cfg, "0,0");
  fsm2.tick(true, 1500, cfg, "0,1");
  fsm2.tick(true, 2900, cfg, "0,2");
  assert.strictEqual(fsm2.tick(false, 3500, cfg), "presence_ended");
  assert.strictEqual(fsm2.tick(false, 3600, cfg), null);
  // outside the post-leave window nothing fires either (firmware line 99)
  const fsm3 = makeFsm();
  fsm3.tick(true, 0, cfg, "1,1");
  fsm3.tick(true, 2600, cfg, "1,1");
  assert.strictEqual(fsm3.tick(false, 3200, cfg), "presence_ended");
  assert.strictEqual(fsm3.tick(false, 6300, cfg), null); // 3.1 s after leave
});

test("iou + nms behave like a de-dup pass", async () => {
  const { iou, nms } = await import("../assets/vision-ui.js");
  const a = { x: 100, y: 100, w: 40, h: 40, score: 90, target: 0 };
  const same = { ...a, score: 70 };
  const far = { x: 200, y: 200, w: 40, h: 40, score: 80, target: 0 };
  assert.ok(iou(a, same) > 0.99);
  assert.strictEqual(iou(a, far), 0);
  const kept = nms([a, same, far], 0.45);
  assert.strictEqual(kept.length, 2);
  assert.ok(kept.every((b) => b.score !== 70), "lower duplicate must be suppressed");
  // with threshold 1.0 nothing is suppressed
  assert.strictEqual(nms([a, same, far], 1.0).length, 3);
});

test("bootLines + mqttApply contracts", async () => {
  const { bootLines, mqttApply } = await import("../assets/vision-ui.js");
  const lines = bootLines(data.serial);
  assert.ok(lines.length >= data.serial.boot.length);
  assert.ok(lines.some((l) => l.text.includes("Grove Vision AI ID=")));
  const store = {};
  mqttApply(store, { topic: "a", payload: "1", retain: true });
  mqttApply(store, { topic: "b", payload: "2", retain: false });
  mqttApply(store, { topic: "a", clear: true });
  assert.deepStrictEqual(store, {});
});

test("VisionSim wires the cores together end to end", async () => {
  const { VisionSim } = await import("../assets/vision-ui.js");
  const sim = new VisionSim(data);
  sim.run("walk");
  const events = [];
  sim.on("event", (name) => events.push(name));
  // drive ~14 s of sim time at the firmware's invoke cadence
  for (let t = 0; t < 14000; t += data.detect.invoke_period_ms)
    sim.tick(data.detect.invoke_period_ms, data.detect.invoke_period_ms);
  assert.ok(events.includes("presence_started"), "walk never started presence: " + events);
  assert.ok(events.includes("presence_ended"), "walk never ended presence: " + events);
  // the cat alone must claim nothing
  const sim2 = new VisionSim(data);
  const events2 = [];
  sim2.on("event", (n) => events2.push(n));
  sim2.run("cat");
  for (let t = 0; t < 8000; t += data.detect.invoke_period_ms)
    sim2.tick(data.detect.invoke_period_ms, data.detect.invoke_period_ms);
  assert.deepStrictEqual(events2, [], "the cat is not person-class; nothing may publish");
});
