// canary-local/tests/vision.test.js — the Vision page's honesty gate.
//
// Two jobs, mirroring tests/wap.test.js:
//  1. Cross-check devices/vision.json against its sources of truth (the
//     canary-vision firmware, the Grove Vision AI V2 docs, the registry,
//     boards.json) so a hand-edit that bypasses the generator — or firmware
//     drift — is caught here, not just by the generator's own asserts.
//  2. Execute the committed WebAssembly core built from the production
//     detection pipeline, NVS tuning, voxel tracker and presence FSM. The
//     page has no JavaScript behavior mirror that can silently drift.

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
const detectionPipelineH = read(join(FW, "include/canary/vision/detection_pipeline.h"));
const visionBuild = read(join(ROOT, "emulator/build.sh"));
// The committed emulator/dist/canary-vision-core.js; LAB_CORES=native builds
// this tree's sources instead (tests/native/README.md, sweep A40).
const visionFactory = require("./native/cores.js").coreFactory("canary-vision-core");

async function firmwareCore() {
  const { createVisionFirmwareCore } = await import("../emulator/web/vision-core.js");
  return createVisionFirmwareCore(visionFactory);
}

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
  assert.strictEqual(data.mqtt.discovery.entities.length, 20);
  assert.ok(data.mqtt.topics.length >= 15, "too few MQTT topics");
  assert.ok(data.serial.boot.length >= 20, "boot log too short");
  assert.ok(data.sandbox.length >= 5, "too few sandbox scenes");
  assert.ok(data.placement.use_cases.length >= 4, "too few placement presets");
  assert.ok(data.troubleshooting.length >= 6, "too few symptom rows");
  assert.strictEqual(data.tuning.length, 5);
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
  // device_board maps a device to a LIST of boards (primary first)
  const visionBoards = [].concat(boards.device_board["canary-vision"]);
  assert.ok(visionBoards.includes(data.device.board_id),
    `${data.device.board_id} not in canary-vision board mapping (${visionBoards.join(", ")})`);
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
test("watch profiles trace to detect_profiles.h", () => {
  const profH = read(join(FW, "include/canary/detect_profiles.h"));
  assert.ok(Array.isArray(data.detect.profiles), "detect.profiles missing");
  assert.ok(data.detect.profiles.length >= 2, "watch profile table thin");
  assert.strictEqual(data.detect.profiles[0].key, "room_presence",
    "profile 0 must stay the room_presence default");
  assert.ok(data.detect.profiles.some((p) => p.key === "litter_box"),
    "litter_box watch profile missing");
  for (const p of data.detect.profiles) {
    assert.ok(profH.includes(`"${p.key}"`), "profile key not in firmware: " + p.key);
    assert.ok(profH.includes(`"${p.label}"`), "profile label not in firmware: " + p.label);
  }
});

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

// The Presence and Dwelling entities (sweep HA25 made them able to turn on):
// the class the page names is the device_class the discovery announces.
// Presence is an occupancy sensor (the page used to say motion), and
// Dwelling announces no class (the page used to say occupancy).
test("the Presence and Dwelling entity descriptions name the class discovery announces", () => {
  const block = (name) => haCpp.split('\\"name\\":\\"' + name + '\\",')[1].split("publish_cfg(")[0];
  const cls = (name) => (block(name).match(/\\"device_class\\":\\"(\w+)\\"/) || [])[1];
  const desc = (name) => data.mqtt.discovery.entities.find((e) => e.name === name).desc;
  assert.strictEqual(cls("Presence"), "occupancy");
  assert.match(desc("Presence"), /occupancy class/);
  assert.doesNotMatch(desc("Presence"), /motion/);
  assert.strictEqual(cls("Dwelling"), undefined);
  assert.doesNotMatch(desc("Dwelling"), /class/);
});

// What the page says the voxel and the lengths mean (the A39/F130 review):
// the settled cell stays after a visit ends, and each visit starts its own
// tracker (sweep F152: reset at boot and on the frame that starts a visit),
// so a visit opens on its own cell; dwell_ended's dwell_ms and visit_ms run
// to the frame that declared the person gone, lost timeout included.
// Data-only: it needs no core, so it holds the words whatever dist is
// committed.
test("the pane note and the Voxel entity say how the settled cell and the lengths behave", () => {
  const note = data.mqtt.pane.clock.note;
  const tracker = read(join(FW, "src/state/voxel_tracker.cpp"));
  // the facts the words stand on
  assert.ok(fsmCpp.includes("s.voxel = voxel_tracker_.stable();"));
  assert.strictEqual(fsmCpp.split("voxel_tracker_.reset();").length - 1, 2,
    "reset in PresenceFSM::reset() and in open_visit, where a visit starts");
  assert.ok(fsmCpp.includes("EventMsg& out_event) {\n  voxel_tracker_.reset();\n  voxel_tracker_.update(first_cell, seen_ms);\n"),
    "a new visit resets the tracker before its first sighting seeds the cell (sweep F152)");
  assert.strictEqual(fsmCpp.split('emit(out_event, "presence_started")').length - 1, 1,
    "open_visit is the one place a visit starts (sweep F186)");
  assert.strictEqual(mainCpp.split("fsm.reset();").length - 1, 1, "which main.cpp calls once, at boot");
  assert.ok(tracker.includes("  stable_ = Voxel{-1,-1,0,0};"));
  assert.ok(fsmCpp.includes("if (presence_ && (now_ms - last_seen_ms_) > canary::cfg::detect().lost_timeout_ms) {"));
  assert.ok(fsmCpp.includes("ended_dwell_ms_ = now_ms - dwell_start_ms_;"));
  assert.ok(fsmCpp.includes("last_visit_ms_ = now_ms - presence_start_ms_;"));
  // the words
  assert.match(note, /stays put once the frame is empty; each visit starts on its own cell, the one presence_started saw/);
  assert.doesNotMatch(note, /not reset between visits|starts on the last one's cell/, "F152: each visit starts its own tracker");
  assert.match(note, /That length and visit_ms run to the frame that declared the person gone, so both include the lost timeout/);
  const voxel = data.mqtt.discovery.entities.find((e) => e.name === "Voxel");
  assert.match(voxel.desc, /last settled in/);
  assert.match(voxel.desc, /it stays after they leave, and reads -1,-1 only until someone is seen/);
  assert.doesNotMatch(voxel.desc, /occupied cell/, "the sensor is the settled cell, not the frame's occupied one");
});

// The JSON keys an snprintf format in a firmware function writes, in order.
const fmtKeys = (src, signature, from, to) => {
  let body = src.split(signature)[1].split("\n}\n")[0];
  if (from) body = body.slice(body.indexOf(from), body.indexOf(to, body.indexOf(from)));
  return [...body.matchAll(/\\"([a-z_]+)\\":/g)].map((m) => m[1]);
};
const flatKeys = (o) => Object.entries(o).flatMap(([k, v]) =>
  [k, ...(v && typeof v === "object" && !Array.isArray(v) ? flatKeys(v) : [])]);

test("cfg example carries the firmware's own JSON keys", () => {
  // publish_detect_cfg_retained's keys, in order; each but profile_label
  // (the profile's display name) is a key the tuning table explains
  const mqttCpp = read(join(FW, "src/net/mqtt_mgr.cpp"));
  const keys = fmtKeys(mqttCpp, "bool publish_detect_cfg_retained(");
  assert.deepStrictEqual(Object.keys(data.mqtt.cfg_state_example), keys);
  for (const k of keys.filter((x) => x !== "profile_label"))
    assert.ok(data.tuning.some((t) => t.key === k), "cfg key not in tuning table: " + k);
});

// Sweep A26: the MQTT pane's rows used to be hand-written in vision-ui.js
// ({"fw":…,"public_key":"ed25519:…"} on health, {"length":1,"head":"…"} on
// chain). They come from vision.json now, keyed as the firmware publishes.
test("every MQTT pane row is keyed as the firmware publishes it", () => {
  const mqttCpp = read(join(FW, "src/net/mqtt_mgr.cpp"));
  const witnessCpp = read(join(FW, "src/witness.cpp"));
  const want = {
    status: fmtKeys(mqttCpp, "void publish_status_retained("),
    "cfg/state": fmtKeys(mqttCpp, "bool publish_detect_cfg_retained("),
    state: fmtKeys(mqttCpp, "void publish_state_retained("),
    health: fmtKeys(mqttCpp, "void publish_health_retained("),
    chain: fmtKeys(mqttCpp, "void publish_chain_retained(", "if (signed_ok) {", "} else {"),
    events: [...fmtKeys(mainCpp, "static void publish_event_json(", "} else {", "canary::net::publish_event("),
      ...fmtKeys(witnessCpp, "bool sign_event_envelope(")],
  };
  const pane = data.mqtt.pane;
  assert.ok(pane && Array.isArray(pane.online), "vision.json carries the pane's rows");
  const rows = [...pane.online, pane.events];
  for (const [suffix, keys] of Object.entries(want)) {
    const row = rows.find((r) => r.suffix === suffix);
    assert.ok(row, `the pane has no ${suffix} row`);
    assert.deepStrictEqual(flatKeys(JSON.parse(row.payload)), keys, `${suffix}: not the firmware's keys`);
    assert.ok(topicsH.includes(`"securacv/%s/${suffix}"`), `${suffix}: not a topic topics.h builds`);
  }
  // aim/state is a bare ON/OFF, not a JSON string
  assert.ok(mqttCpp.includes('enabled ? "ON" : "OFF"'));
  assert.strictEqual(rows.find((r) => r.suffix === "aim/state").payload, "OFF");
});

test("a sandbox event publishes in the firmware's shape, the sandbox's values laid over it", async () => {
  const { vizEventPayload } = await import("../assets/vision-ui.js");
  const example = JSON.parse(data.mqtt.pane.events.payload);
  const clock = { t0_ms: data.mqtt.pane.clock.t0_ms, occupancy: data.mqtt.pane.occupancy };
  // a snapshot as the firmware core returns it (vision_core_bindings.cpp):
  // the frame's cell under sample, the tracker's settled cell under fsm
  const snap = { t: 5000.7,
                 sample: { bbox: { x: 10, y: 20, w: 30, h: 40, score: 88 }, voxel: { r: 2, c: 0, rows: 3, cols: 3 },
                           person_count: 2, posture: "ambiguous", proximity: "far", voxel_mask: 64 + 8 },
                 fsm: { presence: true, dwelling: false, confidence: 88, presence_ms: 3200, dwell_ms: 0,
                        visit_ms: 0, voxel: { r: 1, c: 0, rows: 3, cols: 3 } }, reason: null };
  const ev = vizEventPayload(example, "presence_started", snap, 77, clock);
  assert.deepStrictEqual(Object.keys(ev), Object.keys(example), "every key, in the firmware's order");
  assert.strictEqual(ev.event, "presence_started");
  assert.strictEqual(ev.seq, 77);
  assert.strictEqual(ev.presence, "present");
  assert.strictEqual(ev.occupants, "1");
  assert.strictEqual(ev.confidence, 88);
  // sweep A39: the voxel is the settled cell publish_event_json writes, not the frame's
  assert.deepStrictEqual(ev.voxel, { rows: 3, cols: 3, r: 1, c: 0 });
  assert.deepStrictEqual(Object.keys(ev.voxel), Object.keys(example.voxel), "the voxel's keys, in the firmware's order");
  assert.deepStrictEqual(ev.bbox, { x: 10, y: 20, w: 30, h: 40 });
  // sweep A37: the clocks and coarse features are the snapshot's, not the example's
  assert.strictEqual(ev.ts_ms, clock.t0_ms + 5000, "the core's clock (t >>> 0) plus the rows' own ts_ms");
  assert.strictEqual(ev.bucket_uptime_s, Math.floor(ev.ts_ms / 1000 / 600) * 600);
  assert.deepStrictEqual([ev.presence_ms, ev.dwell_ms, ev.visit_ms], [3200, 0, 0]);
  assert.deepStrictEqual([ev.posture, ev.proximity, ev.occupancy], ["ambiguous", "far", "two"]);
  assert.strictEqual(ev.occ_mask, 72, "the pipeline's own mask (types.h: bit r*cols + c), every box's cell");
  assert.ok(read(join(FW, "include/canary/types.h")).includes("occupied 3x3 cells: bit (r*cols + c)"));
  for (const k of ["v", "alg", "fp", "sig"]) assert.strictEqual(ev[k], example[k], `the ${k} envelope field rides along`);
  // with a reason: right after the name, as publish_event_json's reason branch writes it
  const left = vizEventPayload(example, "presence_ended",
    { t: 9000, sample: { bbox: null, voxel: { r: -1, c: -1, rows: 3, cols: 3 }, person_count: 0, posture: "unknown", proximity: "unknown", voxel_mask: 0 },
      fsm: { presence: false, dwelling: false, confidence: 0, presence_ms: 0, dwell_ms: 0,
             visit_ms: 4000, voxel: { r: 1, c: 0, rows: 3, cols: 3 } }, reason: "lost" },
    78, { ...clock, visit_ms: 9999 });
  const keys = Object.keys(left);
  assert.strictEqual(keys[keys.indexOf("event") + 1], "reason");
  assert.strictEqual(left.reason, "lost");
  assert.strictEqual(left.presence, "clear");
  assert.strictEqual(left.occupants, "0");
  assert.strictEqual(left.occupancy, "none", "nobody in frame reads as no occupancy");
  assert.deepStrictEqual([left.posture, left.proximity, left.occ_mask, left.visit_ms], ["unknown", "unknown", 0, 4000],
    "visit_ms is the core's last_visit_ms_ (A39), never a latch the caller keeps");
  assert.deepStrictEqual([left.voxel.r, left.voxel.c], [1, 0], "the empty frame leaves the settled cell where it was");
  assert.ok(mainCpp.includes('"\\"event\\":\\"%s\\","\n        "\\"reason\\":\\"%s\\","'),
    "main.cpp's reason branch still writes reason right after event");
  // the occupancy words are optical_features.h's buckets
  const optical = read(join(FW, "include/canary/vision/optical_features.h"));
  assert.deepStrictEqual(data.mqtt.pane.occupancy,
    [...optical.split("inline const char* occupancy_name(int count) {")[1].split("\n}\n")[0].matchAll(/return "([a-z]+)";/g)].map((m) => m[1]));
});

// Sweep A39: the committed core returns what the pane reads. The tick JSON's
// "fsm" object is the FSM snapshot publish_event_json writes from; a dist
// built before vision_core_bindings.cpp returned the settled cell and
// visit_ms has neither, and the pane would read undefined. This holds the
// committed dist's keys to the bindings source, so a stale dist fails here,
// by name, rather than as a wrong number further down.
test("the committed Vision core returns the fsm keys its bindings print (sweep A39)", async () => {
  const bindings = read(join(ROOT, "emulator/vision/vision_core_bindings.cpp"));
  const fmt = bindings.split('"\\"fsm\\":{')[1].split('"\\"event\\":%s')[0];
  const printed = [...fmt.matchAll(/\\"(\w+)\\":/g)].map((m) => m[1]);
  assert.deepStrictEqual(printed, ["presence", "dwelling", "confidence", "presence_ms", "dwell_ms", "visit_ms",
    "voxel", "r", "c", "rows", "cols"], "the bindings' fsm object");
  const core = await firmwareCore();
  core.reset();
  const idle = core.tick(1000, []);
  const flat = (o) => Object.entries(o).flatMap(([k, v]) => (v && typeof v === "object" ? [k, ...flat(v)] : [k]));
  assert.deepStrictEqual(flat(idle.fsm), printed,
    "canary-local/emulator/dist/canary-vision-core.js predates vision_core_bindings.cpp: rebuild the dist " +
    "(build.sh vision, or Actions -> \"Rebuild emulator dist (pinned emsdk)\"); until then, LAB_CORES=native " +
    "runs these tests on the tree's sources (tests/native/README.md)");
  // nobody seen yet: the tracker's reset cell, which the pane's retained state row shows
  const stateRow = JSON.parse(data.mqtt.pane.online.find((r) => r.suffix === "state").payload);
  const v = idle.fsm.voxel;
  assert.deepStrictEqual(stateRow.voxel, { rows: v.rows, cols: v.cols, r: v.r, c: v.c });
  assert.deepStrictEqual(stateRow.voxel, { rows: 0, cols: 0, r: -1, c: -1 });
  assert.strictEqual(idle.fsm.visit_ms, 0);
});

// Sweep A37, end to end: the pane's events, driven by VisionSim on the
// committed firmware core, carry the FSM's own clocks and the frame's own
// coarse features. Before it, ts_ms, presence_ms, dwell_ms and visit_ms kept
// the example's values for every event, and posture and proximity read
// upright / mid for any box while someone was present. Sweep F130: the
// dwell_ended row carries the dwell it closed. Sweep A39: the voxel is the
// tracker's settled cell and visit_ms the FSM's last_visit_ms_, both from
// the core, where the pane used to show the frame's cell and keep its own
// visit latch. Both need a dist built from this tree's presence_fsm.cpp and
// vision_core_bindings.cpp (the test below names a stale one).
test("the pane's clocks and coarse features follow the sandbox (sweep A37)", async () => {
  const { withFakeDom, fakeBus } = require("./fixtures/fake_dom.js");
  const optical = read(join(FW, "include/canary/vision/optical_features.h"));
  const knob = (n) => +optical.match(new RegExp("#define " + n + " (\\d+)"))[1];
  const posture = (w, h) => (h * 100 >= w * knob("OPT_POSTURE_UPRIGHT_RATIO_X100") ? "upright"
    : w * 100 >= h * knob("OPT_POSTURE_HORIZONTAL_RATIO_X100") ? "horizontal" : "ambiguous");
  const proximity = (w, h) => {
    const pct = Math.floor((w * h * 100) / (data.detect.frame.w * data.detect.frame.h));
    return pct >= knob("OPT_PROXIMITY_NEAR_PCT") ? "near" : pct <= knob("OPT_PROXIMITY_FAR_PCT") ? "far" : "mid";
  };
  await withFakeDom(async () => {
    const { buildMqtt, VisionSim } = await import("../assets/vision-ui.js");
    const bus = fakeBus();
    const outer = buildMqtt(data, bus);
    const sim = new VisionSim(data, await firmwareCore());
    const base = "securacv/" + data.device.id_example + "/";
    const payloadOf = (topic) => {
      const r = outer.all("vis-mqtt-row").find((x) => x.all("vis-mqtt-topic")[0].children[0].textContent === base + topic);
      return JSON.parse(r.all("vis-mqtt-payload")[0].textContent);
    };
    const seen = [];
    bus.on("sim-event", ({ name, snap }) => seen.push({ name, t: snap.t >>> 0, snap, ev: payloadOf("events"), state: payloadOf("state") }));
    sim.on("event", (name, snap) => bus.emit("sim-event", { name, snap }));
    sim.run("linger");
    for (let i = 0; i < 400 && !seen.some((e) => e.name === "presence_ended"); i++)
      sim.tick(data.detect.invoke_period_ms, data.detect.invoke_period_ms);
    const at = (n) => seen.find((e) => e.name === n);
    for (const n of ["presence_started", "dwell_started", "dwell_ended", "presence_ended"]) assert.ok(at(n), "the linger never emitted " + n);
    const t0 = data.mqtt.pane.clock.t0_ms;
    for (const e of seen) {
      assert.strictEqual(e.ev.ts_ms, t0 + e.t, e.name + ": ts_ms is the clock the event fired at");
      assert.strictEqual(e.ev.presence_ms, e.snap.fsm.presence_ms, e.name);
      // the pane publishes the core's dwell_ms on the event and its state row
      assert.strictEqual(e.ev.dwell_ms, e.snap.fsm.dwell_ms, e.name + ": dwell_ms is the core's");
      assert.strictEqual(e.state.dwell_ms, e.ev.dwell_ms, e.name + ": and the state row's");
      // only dwell_ended carries a dwell: dwell_started sets dwell_start_ms_
      // on its own tick, and the rest are not dwelling
      if (e.name !== "dwell_ended") assert.strictEqual(e.ev.dwell_ms, 0, e.name + ": dwell_ms on an event row");
      assert.strictEqual(e.state.ts_ms, e.ev.ts_ms, e.name + ": the state row goes out on the same tick");
      assert.strictEqual(e.state.presence_ms, e.ev.presence_ms);
      const bb = e.snap.sample.bbox;
      assert.strictEqual(e.ev.posture, bb ? posture(bb.w, bb.h) : "unknown", e.name + ": posture is the box's");
      assert.strictEqual(e.ev.proximity, bb ? proximity(bb.w, bb.h) : "unknown", e.name + ": proximity is the box's");
      assert.strictEqual(e.ev.occ_mask, e.snap.sample.voxel_mask);
      // sweep A39: the voxel and visit_ms are the FSM snapshot's, as publish_event_json writes them
      const fv = e.snap.fsm.voxel;
      assert.deepStrictEqual(e.ev.voxel, { rows: fv.rows, cols: fv.cols, r: fv.r, c: fv.c }, e.name + ": the settled cell");
      assert.deepStrictEqual(e.state.voxel, e.ev.voxel, e.name + ": and the state row's");
      assert.strictEqual(e.ev.visit_ms, e.snap.fsm.visit_ms, e.name + ": visit_ms is the core's");
    }
    const start = at("presence_started"), dwell = at("dwell_started"), end = at("presence_ended");
    assert.strictEqual(start.ev.presence_ms, 0, "presence starts on that tick");
    assert.strictEqual(dwell.ev.presence_ms, dwell.t - start.t, "dwell_started: time present so far");
    assert.ok(dwell.ev.presence_ms >= sim.cfg.dwell_start_ms, "past the dwell start");
    assert.strictEqual(dwell.ev.dwell_ms, 0, "dwell_start_ms_ is set on the tick that emits dwell_started");
    // dwell_ended: still present (presence_ended follows), the stay so far,
    // and the length of the dwell it closed, on the running dwell's clock
    // (sweep F130: PresenceFSM latches it for that tick; it used to say 0,
    // because dwelling_ is cleared before dwell_ended's snapshot)
    const dend = at("dwell_ended");
    assert.ok(dend.ev.presence_ms > 0, "dwell_ended is sent while present");
    assert.strictEqual(dend.ev.presence_ms, dend.t - start.t, "dwell_ended: time present so far");
    assert.ok(dend.t - dwell.t > 0, "the dwell lasted");
    assert.strictEqual(dend.ev.dwell_ms, dend.t - dwell.t, "dwell_ended reports the dwell it closed");
    assert.strictEqual(dend.ev.presence_ms - dend.ev.dwell_ms, dwell.ev.presence_ms,
      "the dwell began where dwell_started said the stay stood");
    assert.strictEqual(end.ev.dwell_ms, 0, "presence_ended: the dwell length is not carried on");
    assert.strictEqual(dend.ev.presence, "present");
    assert.strictEqual(start.ev.visit_ms, 0, "no stay has ended yet");
    assert.strictEqual(end.ev.presence_ms, 0, "presence is over");
    assert.strictEqual(end.ev.visit_ms, end.t - start.t, "presence_ended reports the stay it closed");
    assert.deepStrictEqual([end.ev.posture, end.ev.proximity, end.ev.occupancy], ["unknown", "unknown", "none"],
      "no box, no coarse features: not the example's upright / mid");
    // the empty frame has no cell; the device still names the cell the person settled in
    assert.strictEqual(end.snap.sample.voxel.r, -1, "presence_ended's frame is empty");
    assert.ok(end.ev.voxel.r >= 0 && end.ev.voxel.c >= 0, "the settled cell stays put");
    assert.deepStrictEqual(end.ev.voxel, dend.ev.voxel, "no one seen since dwell_ended: the tracker did not move");
    // the pane says which values stay illustrative
    assert.strictEqual(outer.all("vis-mqtt-note")[0].textContent, data.mqtt.pane.clock.note);
    assert.match(data.mqtt.pane.clock.note, /the voxel \(its tracker's settled cell/);
    assert.doesNotMatch(data.mqtt.pane.clock.note, /frame's cell \(|does not return/, "A39: the core returns both");
    assert.match(data.mqtt.pane.clock.note, /the length of the dwell it closed on dwell_ended/);
    assert.doesNotMatch(data.mqtt.pane.clock.note, /dwell_ms is 0 on every event row/, "F130: dwell_ended carries the dwell");
    assert.ok(fsmCpp.includes("s.dwell_ms    = dwelling_ ? (now_ms - dwell_start_ms_) : ended_dwell_ms_;"),
      "the device's dwell_ms is the running dwell or the one that just ended");
    assert.ok(read(join(FW, "src/state/presence_fsm.cpp")).includes("s.voxel = voxel_tracker_.stable();"),
      "the device publishes its tracker's settled cell");
  });
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

// ── 7. the browser executes the production firmware core ──────────────────
test("committed wasm contract is exact and its build compiles production sources", async () => {
  const core = await firmwareCore();
  core.assertGeneratedData(data);
  assert.strictEqual(core.contract.schema, "securacv.canary-vision.core/v1");
  assert.ok(visionMgrCpp.includes("detection::sample_from_boxes(boxes, det)"));
  for (const source of ["detect_config.cpp", "presence_fsm.cpp", "voxel_tracker.cpp",
                        "vision_core_bindings.cpp", "vision_core_shim.cpp"])
    assert.ok(visionBuild.includes(source), "wasm build omitted " + source);
  assert.ok(visionBuild.includes("detection_pipeline.h"));
  assert.ok(visionBuild.includes('"$0" vision'), "the all target must rebuild Vision");
});

test("firmware wasm: live tuning returns the production clamps", async () => {
  const core = await firmwareCore();
  assert.deepStrictEqual(core.configure({
    person_target: -1, score_min: -1, lost_timeout_ms: 1, dwell_start_ms: 1,
  }), {
    person_target: 0, score_min: data.detect.bounds.score[0],
    lost_timeout_ms: data.detect.bounds.lost_ms[0],
    dwell_start_ms: data.detect.bounds.dwell_ms[0],
  });
  assert.deepStrictEqual(core.configure({
    person_target: 999, score_min: 999, lost_timeout_ms: 999999, dwell_start_ms: 9999999,
  }), {
    person_target: 255, score_min: data.detect.bounds.score[1],
    lost_timeout_ms: data.detect.bounds.lost_ms[1],
    dwell_start_ms: data.detect.bounds.dwell_ms[1],
  });
});

test("firmware wasm: class filter, threshold and highest score", async () => {
  const core = await firmwareCore();
  assert.ok(detectionPipelineH.includes("if (box.target != det.person_target) continue;"));
  assert.ok(detectionPipelineH.includes("if (box.score < det.score_min) continue;"));
  const cfg = { person_target: 0, score_min: 70 };
  core.configure({ ...data.detect, ...cfg });
  const boxes = [
    { x: 10, y: 10, w: 5, h: 5, score: 95, target: 8 },   // cat: right score, wrong class
    { x: 20, y: 20, w: 5, h: 5, score: 60, target: 0 },   // person under threshold
    { x: 30, y: 30, w: 5, h: 5, score: 80, target: 0 },
    { x: 40, y: 40, w: 5, h: 5, score: 91, target: 0 },   // best
  ];
  assert.strictEqual(core.tick(0, boxes).sample.bbox.score, 91);
  assert.strictEqual(core.tick(1, [boxes[0], boxes[1]]).sample.person_now, false);
  assert.strictEqual(core.tick(2, []).sample.person_now, false);
  // exactly-at-threshold is kept (firmware uses <, not <=)
  assert.strictEqual(core.tick(3,
    [{ x: 0, y: 0, w: 1, h: 1, score: 70, target: 0 }]).sample.bbox.score, 70);
});

test("firmware wasm: bbox-to-voxel integer math and clamps", async () => {
  const core = await firmwareCore();
  assert.ok(detectionPipelineH.includes("const int64_t col = (px * safe_cols) / FRAME_W;"));
  // center of frame lands center cell
  assert.deepStrictEqual(core.tick(0,
    [{ x: 100, y: 100, w: 40, h: 40, score: 90, target: 0 }]).sample.voxel,
    { r: 1, c: 1, rows: 3, cols: 3 });
  // exactly at the right edge clamps to the last cell (cx=240 → c=3 → clamp 2)
  assert.strictEqual(core.tick(1,
    [{ x: 220, y: 220, w: 40, h: 40, score: 90, target: 0 }]).sample.voxel.c, 2);
  assert.deepStrictEqual(core.tick(2,
    [{ x: -10, y: -10, w: 4, h: 4, score: 90, target: 0 }]).sample.voxel,
    { r: 0, c: 0, rows: 3, cols: 3 });
  // integer division truncation matches C: cx=79 → 79*3/240 = 0.9875 → 0
  assert.strictEqual(core.tick(3,
    [{ x: 79, y: 0, w: 0, h: 0, score: 90, target: 0 }]).sample.voxel.c, 0);
  assert.strictEqual(core.tick(4,
    [{ x: 80, y: 0, w: 0, h: 0, score: 90, target: 0 }]).sample.voxel.c, 1);
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

test("firmware wasm: the production FSM event order", async () => {
  const core = await firmwareCore();
  core.configure({ ...data.detect, dwell_start_ms: 1000, lost_timeout_ms: 500 });
  const person = [{ x: 100, y: 100, w: 40, h: 80, score: 90, target: 0 }];
  assert.strictEqual(core.tick(0, person).event, "presence_started");
  assert.strictEqual(core.tick(100, person).event, null);
  assert.strictEqual(core.tick(1000, person).event, "dwell_started");
  assert.strictEqual(core.tick(1100, person).event, null); // the dwell runs on
  // silence: dwell_ended fires the tick before presence_ended (firmware order)
  assert.strictEqual(core.tick(1500, []).event, null);      // within lost timeout
  assert.strictEqual(core.tick(1700, []).event, "dwell_ended");
  assert.strictEqual(core.tick(1701, []).event, "presence_ended");
  // the qualified (dwelled) visit signs interaction_likely inside the window
  const interaction = core.tick(1800, []);
  assert.strictEqual(interaction.event, "interaction_likely");
  assert.strictEqual(interaction.reason, "dwell_then_left");
  assert.strictEqual(core.tick(1900, []).event, null); // emitted once, not again
  // a short visit skips dwell — and earns no interaction event
  assert.strictEqual(core.tick(6000, person).event, "presence_started");
  assert.strictEqual(core.tick(6600, []).event, "presence_ended");
  const shortVisit = core.tick(6700, []);
  assert.strictEqual(shortVisit.event, null);
  assert.strictEqual(shortVisit.reason, null);
});

test("firmware wasm: a stable voxel qualifies interaction without dwell", async () => {
  const core = await firmwareCore();
  core.configure({ ...data.detect, dwell_start_ms: 60000, lost_timeout_ms: 500 });
  const person = [{ x: 100, y: 100, w: 40, h: 80, score: 90, target: 0 }];
  assert.strictEqual(core.tick(0, person).event, "presence_started");
  assert.strictEqual(core.tick(1000, person).event, null);
  assert.strictEqual(core.tick(2600, person).event, null); // firmware zone window passed → latch
  assert.strictEqual(core.tick(3200, []).event, "presence_ended");
  const interaction = core.tick(3300, []);
  assert.strictEqual(interaction.event, "interaction_likely");
  assert.strictEqual(interaction.reason, "zone_interaction_then_left");

  // The same qualified visit expires after the production post-leave window.
  core.reset();
  core.configure({ ...data.detect, dwell_start_ms: 60000, lost_timeout_ms: 500 });
  core.tick(0, person);
  core.tick(2600, person);
  assert.strictEqual(core.tick(3200, []).event, "presence_ended");
  assert.strictEqual(core.tick(6301, []).event, null);
});

// Sweep F152: each visit starts its own voxel tracker, so a short visit after
// a long one is not an interaction, and presence_started names the cell the
// visit began in. Before it the tracker kept the earlier visit's settle time
// and cell, so this revisit ended in zone_interaction_then_left and opened on
// the first visit's cell. And (F152's review) a qualifying visit followed by a
// sighting on the very frame after its presence_ended still reports
// interaction_likely, on the frame after the next presence_started, with its
// own visit_ms; the FSM before it sent nothing. Needs a dist built from this
// tree's presence_fsm.cpp (CI's pinned-emsdk rebuild); on an older dist it
// fails here.
test("firmware wasm: each visit starts its own interaction clock and cell (sweep F152)", async () => {
  const core = await firmwareCore();
  core.configure({ ...data.detect, dwell_start_ms: 60000, lost_timeout_ms: 500 });
  const at = (cx, cy) => [{ x: cx - 20, y: cy - 40, w: 40, h: 80, score: 90, target: 0 }];
  const center = at(120, 120), corner = at(200, 40);
  const events = [];
  const run = (from, to, boxes) => {
    let last = null;
    for (let t = from; t < to; t += 100) {
      const k = core.tick(t, boxes);
      if (k.event) events.push(k.reason ? k.event + ":" + k.reason : k.event);
      if (k.event === "presence_started") last = k;
    }
    return last;
  };
  // a 4 s visit settled in the center cell qualifies by the zone rule
  const first = run(0, 4000, center);
  assert.deepStrictEqual([first.fsm.voxel.r, first.fsm.voxel.c], [1, 1]);
  run(4000, 9000, []);
  assert.deepStrictEqual(events, ["presence_started", "presence_ended", "interaction_likely:zone_interaction_then_left"]);
  // a 1 s pass through the top-right cell does not, and opens on its own cell
  events.length = 0;
  const pass = run(9000, 10000, corner);
  assert.deepStrictEqual([pass.sample.voxel.r, pass.sample.voxel.c], [0, 2]);
  assert.deepStrictEqual([pass.fsm.voxel.r, pass.fsm.voxel.c], [0, 2],
    "presence_started names the cell this visit began in, not the last visit's (1,1)");
  run(10000, 15000, []);
  assert.deepStrictEqual(events, ["presence_started", "presence_ended"],
    "a 1 s visit after a 4 s one is not an interaction");
  // and a long settled visit after it still is
  events.length = 0;
  run(15000, 19000, corner);
  run(19000, 24000, []);
  assert.deepStrictEqual(events, ["presence_started", "presence_ended", "interaction_likely:zone_interaction_then_left"]);
  // a 4 s visit, then someone in the frame right after its presence_ended:
  // the ended visit's interaction_likely still goes out, one frame later
  events.length = 0;
  run(24000, 28000, center);
  let t = 28000, ended = null;
  for (; !ended; t += 100) { const k = core.tick(t, []); if (k.event) { events.push(k.event); ended = k; } }
  const again = core.tick(t, corner);
  assert.strictEqual(again.event, "presence_started");
  const late = core.tick(t + 100, corner);
  assert.strictEqual(late.event, "interaction_likely", "the ended visit's report, owed");
  assert.strictEqual(late.reason, "zone_interaction_then_left");
  assert.strictEqual(late.fsm.visit_ms, ended.fsm.visit_ms, "with the ended visit's length");
  assert.ok(late.fsm.presence, "sent while the next visit is present");
});

// Sweep F186: a dweller seen on the frame after dwell_ended starts no second
// dwell in the same stay. That frame ends the stay (presence_ended), the next
// visit opens on the frame after (presence_started, from 0), and the ended
// stay's interaction_likely follows, with its length. The FSM before it took
// the dwell_started branch again on that frame, so the lingering alert paged
// twice. Needs a dist built from this tree's presence_fsm.cpp (CI's
// pinned-emsdk rebuild); on an older dist it fails here, and it passes with
// LAB_CORES=native.
test("firmware wasm: a dweller seen on the frame after dwell_ended opens the next visit, not a second dwell (sweep F186)", async () => {
  const core = await firmwareCore();
  core.configure({ ...data.detect, dwell_start_ms: 1000, lost_timeout_ms: 500 });
  const person = [{ x: 100, y: 100, w: 40, h: 80, score: 90, target: 0 }];
  const events = [];
  const tick = (t, boxes) => {
    const k = core.tick(t, boxes);
    if (k.event) events.push(k.reason ? k.event + ":" + k.reason : k.event);
    return k;
  };
  for (let t = 0; t < 2000; t += 100) tick(t, person);
  let t = 2000;
  while (tick(t, []).event !== "dwell_ended") t += 100;
  t += 100;
  const ended = tick(t, person);
  assert.strictEqual(ended.event, "presence_ended", "the frame after dwell_ended ends the stay, seen or not");
  assert.strictEqual(ended.fsm.visit_ms, t, "the stay's length, to that frame");
  const again = tick(t + 100, person);
  assert.strictEqual(again.event, "presence_started", "the sighting opens the next visit on the frame after");
  assert.strictEqual(again.fsm.presence_ms, 0);
  const late = tick(t + 200, person);
  assert.strictEqual(late.event, "interaction_likely");
  assert.strictEqual(late.reason, "dwell_then_left");
  assert.strictEqual(late.fsm.visit_ms, ended.fsm.visit_ms, "the ended stay's length");
  for (let u = t + 300; u < t + 600; u += 100) tick(u, person);
  for (let u = t + 600; u < t + 5000; u += 100) tick(u, []);
  assert.deepStrictEqual(events, ["presence_started", "dwell_started", "dwell_ended", "presence_ended",
    "presence_started", "interaction_likely:dwell_then_left", "presence_ended"], "one dwell per stay");
});

// Sweep F202: a person last seen on the frame that sends dwell_started leaves
// a stay that dwelled, and its interaction_likely says dwell_then_left, settled
// in one cell or moving to a new cell every 0.5 s. That frame returned before
// the FSM latched the dwell for the leave, so the settled stay reported
// zone_interaction_then_left and the moving one no interaction_likely at all,
// on a Lab pane that says "Leave afterwards and the qualified visit signs
// interaction_likely (dwell_then_left)". Needs a dist built from this tree's
// presence_fsm.cpp (CI's pinned-emsdk rebuild); on an older dist it fails
// here, and it passes with LAB_CORES=native.
test("firmware wasm: a dweller last seen on the dwell_started frame leaves with dwell_then_left (sweep F202)", async () => {
  const core = await firmwareCore();
  assert.ok(data.sandbox.some((s) => s.blurb.includes("the qualified visit signs interaction_likely (dwell_then_left)")),
    "the sandbox's linger blurb this test stands behind");
  const at = (cx, cy) => [{ x: cx - 20, y: cy - 40, w: 40, h: 80, score: 90, target: 0 }];
  const ring = [[40, 40], [120, 40], [200, 40], [200, 120], [200, 200], [120, 200], [40, 200], [40, 120]];
  for (const moving of [false, true]) {
    core.reset();
    core.configure({ ...data.detect });
    const events = [];
    let t = 1000, i = 0;
    for (;; t += 100, i++) {
      const [cx, cy] = moving ? ring[Math.floor(i / 5) % ring.length] : [120, 120];
      const k = core.tick(t, at(cx, cy));
      if (k.event) events.push(k.reason ? k.event + ":" + k.reason : k.event);
      if (k.event === "dwell_started") break;
      assert.ok(t < 1000 + data.detect.dwell_start_ms, "the dwell starts at dwell_start_ms");
    }
    for (const stop = t + 10000; (t += 100) < stop;) {
      const k = core.tick(t, []);
      if (k.event) events.push(k.reason ? k.event + ":" + k.reason : k.event);
    }
    assert.deepStrictEqual(events, ["presence_started", "dwell_started", "dwell_ended", "presence_ended",
      "interaction_likely:dwell_then_left"], moving ? "settled cell moving" : "settled in one cell");
  }
});

// Sweep A42: a box near the int range's ends lands in the cell, and reads the
// posture and proximity, that exact arithmetic says. The Lab's sandbox and
// this core's ABI take any int box (the device's SSCMA boxes have uint16
// fields). The pipeline took the center (x + w/2) and the cell (px * cols) in
// int and the area (long)w*h and the posture products in long, and long is 32
// bits on this wasm32 core: the item's box, two billion pixels wide, read
// proximity "unknown" here and "near" on a 64-bit host, whose long holds the
// product. That half cannot be seen natively. LAB_CORES=native and the
// firmware host suites build with a 64-bit long, so the posture or area put
// back in long passes them all; this test on the dist is the CI gate that
// catches it in the bytes the Lab serves, and vision_wasm32.test.js catches
// it from the sources (a clang wasm32 build of the pipeline beside a g++
// one, sweep A49). The oracle is BigInt, where nothing an int box makes
// overflows. Sweep F221: a box with a side that is not positive reads
// proximity unknown, as its posture does; the area was the bare w * h,
// positive when both sides are negative, and the oracle followed it. The
// fixed rows each name the overflow (or the side) they used to hit; the grid
// crosses the int range's ends with the frame's own sizes. Needs a dist
// built from this tree's detection_pipeline.h and optical_features.h (CI's
// pinned-emsdk rebuild); on an older dist it fails here, and it passes with
// LAB_CORES=native.
test("firmware wasm: a box near the int range's ends reads what exact arithmetic says (sweep A42)", async () => {
  const core = await firmwareCore();
  const opticalH = read(join(FW, "include/canary/vision/optical_features.h"));
  const define = (name) => {
    const m = opticalH.match(new RegExp("#define " + name + "\\s+(\\d+)"));
    assert.ok(m, name + " not found in optical_features.h");
    return BigInt(m[1]);
  };
  const UPRIGHT = define("OPT_POSTURE_UPRIGHT_RATIO_X100");
  const HORIZONTAL = define("OPT_POSTURE_HORIZONTAL_RATIO_X100");
  const NEAR = define("OPT_PROXIMITY_NEAR_PCT");
  const FAR = define("OPT_PROXIMITY_FAR_PCT");
  const { rows, cols } = data.detect.voxel;
  const FW_ = BigInt(data.detect.frame.w), FH_ = BigInt(data.detect.frame.h);
  // C++ division truncates toward zero, as BigInt's does; then the grid clamps.
  const cell = (at, extent, n, frame) => {
    const c = (BigInt(at) + BigInt(Math.trunc(extent / 2))) * BigInt(n) / frame;
    return Number(c < 0n ? 0n : (c > BigInt(n - 1) ? BigInt(n - 1) : c));
  };
  const posture = (w, h) => {
    if (w <= 0 || h <= 0) return "unknown";
    if (BigInt(h) * 100n >= BigInt(w) * UPRIGHT) return "upright";
    if (BigInt(w) * 100n >= BigInt(h) * HORIZONTAL) return "horizontal";
    return "ambiguous";
  };
  const proximity = (w, h) => {
    if (w <= 0 || h <= 0) return "unknown";
    const area = BigInt(w) * BigInt(h);
    const pct = area * 100n / (FW_ * FH_);
    return pct >= NEAR ? "near" : (pct <= FAR ? "far" : "mid");
  };
  const oracle = ([x, y, w, h]) => {
    const r = cell(y, h, rows, FH_), c = cell(x, w, cols, FW_);
    return { r, c, posture: posture(w, h), proximity: proximity(w, h), mask: 1 << (r * cols + c) };
  };
  core.reset();
  core.configure({ ...data.detect, person_target: 0, score_min: 50 });
  let t = 0;
  const read1 = ([x, y, w, h]) => {
    const s = core.tick(t += 100, [{ x, y, w, h, score: 90, target: 0 }]).sample;
    return { r: s.voxel.r, c: s.voxel.c, posture: s.posture, proximity: s.proximity, mask: s.voxel_mask };
  };
  const I = 2147483647;
  const fixed = [
    // the item's box: (long)w*h and w*100 overflowed a 32-bit long
    [[0, 0, 2000000000, 70], { r: 0, c: 2, posture: "horizontal", proximity: "near", mask: 4 }],
    // w*h past INT32_MAX, every side and posture product in range
    [[0, 0, 50000, 50000], { r: 2, c: 2, posture: "ambiguous", proximity: "near", mask: 256 }],
    [[0, 0, I, I], { r: 2, c: 2, posture: "ambiguous", proximity: "near", mask: 256 }],
    // h*100 past INT32_MAX: the posture products
    [[0, 0, 1, I], { r: 2, c: 0, posture: "upright", proximity: "near", mask: 64 }],
    [[0, 0, I, 1], { r: 0, c: 2, posture: "horizontal", proximity: "near", mask: 4 }],
    // x + w/2 past INT_MAX: the center
    [[2000000000, 2000000000, 1000000000, 1000000000], { r: 2, c: 2, posture: "ambiguous", proximity: "near", mask: 256 }],
    // x + w/2 under INT_MIN
    [[-2000000000, 100, -2000000000, 40], { r: 1, c: 0, posture: "unknown", proximity: "unknown", mask: 8 }],
    // px * cols past INT_MAX, the center in range: the cell
    [[1000000000, 0, 0, 0], { r: 0, c: 2, posture: "unknown", proximity: "unknown", mask: 4 }],
    // both sides negative: no area, so no proximity (sweep F221; it read mid)
    [[0, 0, -100, -100], { r: 0, c: 0, posture: "unknown", proximity: "unknown", mask: 1 }],
    // and one whose product passes INT32_MAX (it read near)
    [[0, 0, -2000000000, -100], { r: 0, c: 0, posture: "unknown", proximity: "unknown", mask: 1 }],
  ];
  for (const [box, want] of fixed) {
    assert.deepStrictEqual(oracle(box), want, "the oracle on " + JSON.stringify(box));
    assert.deepStrictEqual(read1(box), want, "the core on " + JSON.stringify(box));
  }
  const V = [-I - 1, -2000000000, -1, 0, 1, data.detect.frame.w, 50000, 2000000000, I];
  const wrong = [];
  for (const x of V) for (const y of V) for (const w of V) for (const h of V) {
    const box = [x, y, w, h], got = read1(box), want = oracle(box);
    if (JSON.stringify(got) !== JSON.stringify(want)) wrong.push({ box, got, want });
  }
  assert.deepStrictEqual(wrong.slice(0, 3), [], `${wrong.length} of ${V.length ** 4} boxes read otherwise than exact arithmetic`);
});

// Sweep F221: a box with a side that is not positive has no area, so its
// proximity reads unknown, as its posture always did. sample_from_boxes
// handed classify_proximity the bare w * h, positive when both sides are
// negative: a -100 by -100 box read mid (its posture unknown). The Lab's
// sandbox and this core's ABI take any int box; the device's SSCMA sides are
// unsigned. Needs a dist built from this tree's detection_pipeline.h (CI's
// pinned-emsdk rebuild); on an older dist it fails here, and it passes with
// LAB_CORES=native.
test("firmware wasm: a box with a side that is not positive reads no proximity (sweep F221)", async () => {
  const core = await firmwareCore();
  core.reset();
  core.configure({ ...data.detect });
  let t = 0;
  const read1 = (w, h) => core.tick(t += 100, [{ x: 120, y: 120, w, h, score: 90, target: 0 }]).sample;
  for (const [w, h] of [[-100, -100], [-1, -1], [-2000000000, -100], [-100, 100], [100, -100], [0, 80], [80, 0]]) {
    const s = read1(w, h);
    assert.ok(s.person_now, `${w} by ${h}: the box is still the frame's person`);
    assert.deepStrictEqual([s.posture, s.proximity], ["unknown", "unknown"], `${w} by ${h}: no posture, no proximity`);
  }
  // the same sides, positive, still read their area (100 x 100 is 17% of 240 x 240)
  assert.deepStrictEqual([read1(100, 100).proximity, read1(1, 1).proximity, read1(240, 240).proximity], ["mid", "far", "near"]);
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
  const sim = new VisionSim(data, await firmwareCore());
  sim.run("walk");
  const events = [];
  sim.on("event", (name) => events.push(name));
  // drive ~14 s of sim time at the firmware's invoke cadence
  for (let t = 0; t < 14000; t += data.detect.invoke_period_ms)
    sim.tick(data.detect.invoke_period_ms, data.detect.invoke_period_ms);
  assert.ok(events.includes("presence_started"), "walk never started presence: " + events);
  assert.ok(events.includes("presence_ended"), "walk never ended presence: " + events);
  // the cat alone must claim nothing
  const sim2 = new VisionSim(data, await firmwareCore());
  const events2 = [];
  sim2.on("event", (n) => events2.push(n));
  sim2.run("cat");
  for (let t = 0; t < 8000; t += data.detect.invoke_period_ms)
    sim2.tick(data.detect.invoke_period_ms, data.detect.invoke_period_ms);
  assert.deepStrictEqual(events2, [], "the cat is not person-class; nothing may publish");
});

// buildMqtt itself, on a few lines of fake DOM: the rows it renders are
// vision.json's, the sandbox moves them, and a reconnect republishes the
// retained ones as they stand (the firmware's connect does), not as they
// first were.
function fakeDom() {
  class El {
    constructor(tag) {
      this.tagName = String(tag).toUpperCase(); this.children = []; this.parent = null;
      this.className = ""; this._text = ""; this.scrollTop = 0; this.offsetWidth = 0;
      this.classList = {
        add: (c) => { if (!this.className.split(" ").includes(c)) this.className = (this.className + " " + c).trim(); },
        remove: (c) => { this.className = this.className.split(" ").filter((x) => x && x !== c).join(" "); },
      };
    }
    append(...kids) {
      for (let k of kids) {
        if (typeof k === "string") { const t = new El("#text"); t._text = k; k = t; }
        if (k.parent) k.remove();
        k.parent = this; this.children.push(k);
      }
    }
    remove() { if (this.parent) this.parent.children = this.parent.children.filter((c) => c !== this); this.parent = null; }
    get textContent() { return this._text + this.children.map((c) => c.textContent).join(""); }
    set textContent(v) { this._text = String(v); this.children = []; }
    querySelector(sel) {
      const cls = sel.replace(/^\./, "");
      for (const c of this.children) {
        if (c.className.split(" ").includes(cls)) return c;
        const hit = c.querySelector(sel);
        if (hit) return hit;
      }
      return null;
    }
  }
  return { createElement: (t) => new El(t), createTextNode: (s) => { const t = new El("#text"); t._text = s; return t; },
           body: { contains: () => true } };
}
function fakeBus() {
  const fns = {};
  return { on: (t, f) => (fns[t] ||= []).push(f), emit: (t, p) => (fns[t] || []).forEach((f) => f(p || {})), has: () => false };
}

test("the MQTT pane renders vision.json's rows, moves them with the sandbox, and reconnects as they stand", async () => {
  const saved = globalThis.document;
  globalThis.document = fakeDom();
  try {
    const { buildMqtt } = await import("../assets/vision-ui.js");
    const bus = fakeBus();
    const outer = buildMqtt(data, bus);
    const scroll = outer.querySelector("vis-mqtt-scroll");
    const rows = () => Object.fromEntries(scroll.children.filter((r) => r.className === "vis-mqtt-row" || r.className.includes("vis-mqtt-row"))
      .map((r) => [r.querySelector("vis-mqtt-topic").children[0].textContent, r.querySelector("vis-mqtt-payload").textContent]));
    const base = "securacv/" + data.device.id_example + "/";
    bus.emit("online");
    let now = rows();
    for (const r of data.mqtt.pane.online) assert.strictEqual(now[base + r.suffix], r.payload, `${r.suffix}: not vision.json's row`);
    const chain0 = JSON.parse(now[base + "chain"]).length;

    // the core's tick shape (vision_core_bindings.cpp): the settled cell under fsm
    const cell = { r: 1, c: 2, rows: 3, cols: 3 };
    const snap = { sample: { bbox: { x: 1, y: 2, w: 3, h: 4, score: 90 }, voxel: cell },
                   fsm: { presence: true, dwelling: false, visit_ms: 0, voxel: cell }, reason: null };
    bus.emit("sim-event", { name: "presence_started", snap });
    bus.emit("sim-event", { name: "dwell_started", snap: { ...snap, fsm: { ...snap.fsm, dwelling: true } } });
    now = rows();
    assert.strictEqual(JSON.parse(now[base + "chain"]).length, chain0 + 2, "each event advances the signed head");
    assert.strictEqual(JSON.parse(now[base + "chain"]).latest_hash, "…", "a moved head is not the example's hash");
    assert.strictEqual(JSON.parse(now[base + "events"]).seq, chain0 + 2);
    assert.strictEqual(JSON.parse(now[base + "state"]).dwelling, true);

    bus.emit("online");
    now = rows();
    assert.strictEqual(JSON.parse(now[base + "chain"]).length, chain0 + 2, "a reconnect republishes the chain as it stands");
    assert.strictEqual(JSON.parse(now[base + "state"]).last_event, "dwell_started");
    assert.strictEqual(now[base + "health"], data.mqtt.pane.online.find((r) => r.suffix === "health").payload);

    // a tuning slider republishes cfg/state whole, as publish_detect_cfg_retained
    // writes it (the profile and its label included), with the new values
    const mqttCpp = read(join(FW, "src/net/mqtt_mgr.cpp"));
    bus.emit("cfg", { cfg: { person_target: 0, score_min: 61, lost_timeout_ms: 4321, dwell_start_ms: 9876 }, key: "score_min" });
    const cfgRow = JSON.parse(rows()[base + "cfg/state"]);
    assert.deepStrictEqual(Object.keys(cfgRow), fmtKeys(mqttCpp, "bool publish_detect_cfg_retained("));
    assert.strictEqual(cfgRow.profile_label, data.mqtt.cfg_state_example.profile_label);
    assert.deepStrictEqual([cfgRow.score, cfgRow.lost_ms, cfgRow.dwell_ms], [61, 4321, 9876]);

    // aim/state is publish_aim_state_retained's bare ON / OFF, not JSON
    assert.ok(mqttCpp.includes('publish_checked("AIM", topics.aim_state, enabled ? "ON" : "OFF", true);'));
    bus.emit("aim-state", { on: true });
    assert.strictEqual(rows()[base + "aim/state"], "ON");
    bus.emit("aim-state", { on: false });
    assert.strictEqual(rows()[base + "aim/state"], "OFF");
  } finally {
    globalThis.document = saved;
  }
});
