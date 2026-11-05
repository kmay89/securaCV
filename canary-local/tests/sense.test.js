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
//     rangeBandOf, countBucketOf, makePresenceFSM, makeRadarLabLoop, and
//     the vitals lock it takes from sense-sim.js) so the placement lab
//     provably runs the firmware's semantics at the firmware's constants,
//     held to mr60_presence.cpp and mr60_vitals.cpp built natively.

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

// Sweeps A30 and A31: the Sense page's MQTT rows are the firmware's whole
// payloads. A sandbox scene used to spell only the fields it changed (an
// events row with no envelope, chain {"length":+1}, a three-key state that
// the pane pushed over the retained state row), and the pane's lab handler
// hand-wrote its events row (no v, alg, sig or bucket_uptime_s) and its chain
// row (no fp: HA's signature.py reads that as unsigned).
const fnKeys = (src, signature, from, to) => {
  let body = src.split(signature)[1].split("\n}\n")[0];
  if (from) body = body.slice(body.indexOf(from), body.indexOf(to, body.indexOf(from)));
  return [...body.matchAll(/\\"([a-z_0-9]+)\\":/g)].map((m) => m[1]);
};
const witnessCpp = read(join(PRJ, "src/witness.cpp"));
const senseKeys = () => {
  const stateWell = fnKeys(mqttCpp, "void publish_state_retained(", "snprintf(msg, sizeof(msg),", "canary::cfg::get()");
  const vitals = ["breathing_locked", "breath_bpm", "heart_bpm"];
  return {
    state: stateWell.filter((k) => !vitals.includes(k)),
    stateWellbeing: stateWell,
    events: [...fnKeys(mainCpp, "static void record_event_now(", "const int n = snprintf(msg, sizeof(msg),", "canary::cfg::get()"),
             ...fnKeys(witnessCpp, "bool sign_event_envelope(")],
    chain: fnKeys(mqttCpp, "void publish_chain_retained(", "if (signed_ok) {", "} else {"),
  };
};
const topicEx = (sfx) => JSON.parse(data.mqtt.topics.find((t) => t.suffix === sfx).payload);

test("the state, events and chain examples are keyed as the firmware publishes them", () => {
  const want = senseKeys();
  assert.ok(mqttCpp.includes('#ifdef CANARY_SENSE_VITALS\n           "\\"breathing_locked\\":%s,"'), "the vitals block moved");
  assert.deepStrictEqual(Object.keys(topicEx("state")), want.state);
  const state = data.mqtt.topics.find((t) => t.suffix === "state");
  assert.deepStrictEqual(Object.keys(JSON.parse(state.wellbeing)), want.stateWellbeing, "the wellbeing state, whole");
  assert.deepStrictEqual(Object.keys(topicEx("events")), want.events);
  assert.deepStrictEqual(Object.keys(topicEx("chain")), want.chain);
});

test("every sandbox publish is its topic's payload with the scene's fields laid over it", () => {
  const wellbeing = JSON.parse(data.mqtt.topics.find((t) => t.suffix === "state").wellbeing);
  let n = 0;
  for (const sc of data.sandbox) {
    for (const pub of sc.mqtt) {
      const at = `${sc.id} → ${pub.suffix}`;
      if (!pub.set && !pub.advance) {
        // publish_identify_echo writes the bare word
        assert.ok(mqttCpp.includes('publish_checked("IDFY", topics.identify_echo, active ? "on" : "off",'));
        assert.deepStrictEqual([pub.suffix, pub.payload], ["identify", "on"], at);
        continue;
      }
      const base = pub.base === "wellbeing" ? wellbeing : topicEx(pub.suffix);
      const got = JSON.parse(pub.payload);
      assert.deepStrictEqual(Object.keys(got), Object.keys(base), `${at}: every key of the topic, in its order`);
      for (const k of pub.advance) assert.strictEqual(got[k], base[k] + 1, `${at}: ${k}`);
      for (const [k, v] of Object.entries(pub.set)) assert.deepStrictEqual(got[k], v, `${at}: ${k}`);
      for (const k of Object.keys(base))
        if (!(k in pub.set) && !pub.advance.includes(k)) assert.deepStrictEqual(got[k], base[k], `${at}: ${k} kept`);
      n++;
    }
  }
  assert.ok(n >= 12, "the scenes' publishes went missing");
  // a witnessed event's seq is the chain length it leaves (record_event_now)
  assert.ok(mainCpp.includes("? canary::witness::chain_length() + 1"));
  for (const sc of data.sandbox) {
    const ev = sc.mqtt.find((p) => p.suffix === "events");
    const ch = sc.mqtt.find((p) => p.suffix === "chain");
    assert.strictEqual(!!ev, !!ch, sc.id + ": an event and its chain head go together");
    if (ev) assert.strictEqual(JSON.parse(ev.payload).seq, JSON.parse(ch.payload).length, sc.id);
  }
});

test("the lab's events and chain rows are sense.json's, laid over (senseEventPayload)", async () => {
  const { senseEventPayload, senseChainPayload } = await import("../assets/sense-ui.js");
  const ex = topicEx("events");
  const e = { event: "presence_cleared", presence: "clear", occupants: "0", range: "unknown" };
  const ev = senseEventPayload(ex, e, 500);
  assert.deepStrictEqual(Object.keys(ev), Object.keys(ex), "every key, in record_event_now's order");
  assert.deepStrictEqual([ev.event, ev.presence, ev.occupants, ev.range, ev.seq], [e.event, e.presence, e.occupants, e.range, 500]);
  for (const k of ["v", "alg", "fp", "sig", "bucket_uptime_s", "signed", "device_id", "device_type"])
    assert.deepStrictEqual(ev[k], ex[k], k + " rides along");
  const chain = senseChainPayload(topicEx("chain"), 500);
  assert.deepStrictEqual(Object.keys(chain), Object.keys(topicEx("chain")));
  assert.strictEqual(chain.length, 500);
  assert.strictEqual(chain.latest_hash, "…");
  assert.strictEqual(chain.fp, data.device.fp_example, "the envelope fp HA's verifier needs");
});

test("the MQTT pane publishes a lab event, then the chain head at its seq", async () => {
  const { withFakeDom, withFakeClock, fakeBus } = require("./fixtures/fake_dom.js");
  await withFakeDom(() => withFakeClock(async (clock) => {
    const { buildMqtt } = await import("../assets/sense-ui.js");
    const bus = fakeBus();
    const wrap = buildMqtt(data, bus);
    const topic = (sfx) => `securacv/${data.device.id_example}/${sfx}`;
    const stream = () => wrap.all("wap-mqtt-ev").map((r) => [r.children[0].textContent, r.children[1].textContent]);
    const retained = () => Object.fromEntries(wrap.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
    const chain0 = topicEx("chain").length;
    bus.emit("mqtt");
    bus.emit("labevent", { event: "presence_detected", presence: "present", occupants: "1", range: "near" });
    bus.emit("labevent", { event: "occupancy_changed", presence: "present", occupants: "2+", range: "near" });
    const evs = stream().filter(([t]) => t === topic("events")).map(([, p]) => JSON.parse(p));
    assert.strictEqual(evs.length, 2);
    assert.deepStrictEqual(Object.keys(evs[0]), Object.keys(topicEx("events")));
    assert.deepStrictEqual([evs[0].event, evs[0].seq, evs[0].occupants], ["occupancy_changed", chain0 + 2, "2+"]);
    for (const k of ["v", "alg", "fp", "sig"]) assert.ok(k in evs[0], k);
    const chain = JSON.parse(retained()[topic("chain")]);
    assert.strictEqual(chain.length, chain0 + 2, "the head sits at the last event's seq");
    assert.strictEqual(chain.fp, data.device.fp_example);

    // a pushed scene (lights) replaces the retained state with the whole row
    bus.emit("sandboxpub", { pubs: data.sandbox.find((s) => s.id === "lights").mqtt });
    const state = JSON.parse(retained()[topic("state")]);
    assert.deepStrictEqual(Object.keys(state), Object.keys(topicEx("state")));
    assert.strictEqual(state.lux, 1);
    assert.ok(retained()[topic("state")].includes('"lux":1.0'), "publish_state_retained's %.1f");
    bus.emit("sandboxpub", { pubs: data.sandbox.find((s) => s.id === "identify").mqtt });
    assert.ok(stream().some(([t, p]) => t === topic("identify") && p === "on"), "the bare identify echo, not retained");
    assert.ok(!(topic("identify") in retained()));
    await clock.advance(150 * data.mqtt.topics.length + 200);   // let the retained snapshot finish
    assert.strictEqual(JSON.parse(retained()[topic("chain")]).length, chain0 + 2, "the snapshot lands the chain as it stands");
  }));
});

test("the state row: the lab's snapshot over the build's example, published when main.cpp would", async () => {
  const { senseLabSnapshot, senseStateDue, senseStatePayload, senseStateJson } = await import("../assets/sense-ui.js");
  const st = data.mqtt.topics.find((t) => t.suffix === "state");
  const templates = { base: JSON.parse(st.payload), wellbeing: JSON.parse(st.wellbeing) };
  const fsm = (state, count, range) => ({ state, count, range });
  // presence-only build: no vitals keys at all
  const clear = senseLabSnapshot(fsm("clear", "0", "unknown"), null);
  assert.deepStrictEqual(clear, { presence: false, presence_state: "clear", occupants: "0", range: "unknown", radar_ok: true });
  assert.strictEqual(senseLabSnapshot(fsm("unknown", "0", "unknown"), null).radar_ok, false, "radar_ok is not-unknown");
  // wellbeing: the VitalsFSM's event (sense-sim.js) gives the block, BPMs
  // only while bpm_valid (locked with exactly one target), as
  // publish_state_retained writes them
  const v = { lock: "locked", bpm_valid: true, breath_bpm: 14, heart_bpm: 68 };
  assert.strictEqual(senseLabSnapshot(fsm("present", "1", "near"), v).breath_bpm, 14);
  const two = senseLabSnapshot(fsm("present", "2+", "near"), { ...v, bpm_valid: false, breath_bpm: 0, heart_bpm: 0 });
  assert.deepStrictEqual([two.breathing_locked, two.breath_bpm, two.heart_bpm], [true, null, null],
    "a second person drops the BPMs at once; the lock rides out lost_ms");
  assert.ok(mqttCpp.includes('  if (s.bpm_valid) {\n    snprintf(breath_val, sizeof(breath_val), "%u", (unsigned)s.breath_bpm);'),
    "publish_state_retained writes the BPMs only while bpm_valid");
  // due: any change but the range band alone, and every heartbeat
  const a = senseLabSnapshot(fsm("present", "1", "mid"), null);
  assert.ok(senseStateDue(a, null, false), "the first (boot) publish");
  assert.ok(!senseStateDue({ ...a, range: "near" }, a, false), "a range band alone waits for the heartbeat");
  assert.ok(senseStateDue({ ...a, range: "near" }, a, true), "...and goes out on it");
  assert.ok(senseStateDue({ ...a, occupants: "2+" }, a, false));
  assert.ok(senseStateDue(senseLabSnapshot(fsm("present", "1", "mid"), { ...v, lock: "lost", bpm_valid: false }), a, false), "a build switch");
  // the row: the build's keys in the firmware's order, lux carried over
  const row = senseStatePayload(templates, { ...templates.base, lux: 1.0 }, clear);
  assert.deepStrictEqual(Object.keys(row), Object.keys(templates.base));
  assert.strictEqual(row.lux, 1.0);
  assert.strictEqual(row.presence_state, "clear");
  const wrow = senseStatePayload(templates, templates.base, two);
  assert.deepStrictEqual(Object.keys(wrow), Object.keys(templates.wellbeing));
  assert.ok(senseStateJson(row).includes('"lux":1.0'), "publish_state_retained's %.1f");
  assert.ok(mainCpp.includes("if (g_state_dirty) {\n    publish_state_now(now);"), "the dirty row goes out in the loop");
  assert.ok(mainCpp.includes("canary::net::publish_heartbeat(TOPICS, g_snap);\n    publish_state_now(now);"), "and on the heartbeat");
  assert.strictEqual(data.mqtt.heartbeat_ms, cint(cfgDefault, "CS_HEARTBEAT_MS"));
});

// The radar lab, the MQTT pane and the sandbox on one bus, on the fake DOM and
// the test's clock. Each lab-driven scene is clicked and the lab's frame loop
// runs until its FSM gets there; the pane must then have published exactly
// the rows sense.json lists for that scene (seq and the chain length counted
// on from the rows before). The pane used to publish no state row from the
// lab at all, so the walk, approach, sit, second, leave and stall rows were
// data no page rendered.
test("each sandbox scene, played through the radar lab, publishes the rows sense.json lists", async () => {
  const { withFakeDom, withFakeClock, fakeBus } = require("./fixtures/fake_dom.js");
  const { buildRadarLab, buildMqtt, buildSandbox, stallUnplugMs } = await import("../assets/sense-ui.js");
  const topic = (sfx) => `securacv/${data.device.id_example}/${sfx}`;
  const scene = (id) => data.sandbox.find((s) => s.id === id);
  const rowOf = (id, sfx) => scene(id).mqtt.find((p) => p.suffix === sfx).payload;
  const at = (payload, k, n) => JSON.stringify({ ...JSON.parse(payload), [k]: n });
  const cfg = data.fsm.presence, vit = data.fsm.vitals;
  const chain0 = topicEx("chain").length;
  // a scene's rows advance seq and the chain head by one from the examples
  // (gen_sense.py's event_pub); played through the lab, each lands at the
  // chain's next length, so the bench counts the chain itself
  for (const id of ["walk", "second", "leave"]) {
    assert.strictEqual(JSON.parse(rowOf(id, "events")).seq, chain0 + 1, id);
    assert.strictEqual(JSON.parse(rowOf(id, "chain")).length, chain0 + 1, id);
  }

  async function bench(fn) {
    await withFakeDom(() => withFakeClock(async (clock) => {
      const bus = fakeBus();
      // every event the lab signs, online or not, oldest first
      const signed = [];
      bus.on("labevent", (e) => signed.push(e.event));
      buildRadarLab(data, bus);
      const pane = buildMqtt(data, bus);
      const pad = buildSandbox(data, bus);
      let t = performance.now();
      const run = async (ms) => { for (let e = 0; e < ms; e += 50) { t += 50; clock.frame(t); } await clock.advance(ms); };
      const click = (id) => pad.all("wap-sand-card").find((c) => c.children[0].textContent === scene(id).label).click();
      // the chain length the lab has reached; next() is the seq the next
      // witnessed event takes
      let length = chain0;
      const next = () => ++length;
      let seen = new Set();
      let lastSeq = null;
      // the rows published since the last call, oldest first (the stream
      // prepends, so new rows sit at the top); every events row's seq is the
      // one after the last, so no scene skips one or signs one twice
      const fresh = () => {
        const rows = pane.all("wap-mqtt-ev").filter((r) => !seen.has(r));
        rows.forEach((r) => seen.add(r));
        const out = rows.reverse().map((r) => [r.children[0].textContent, r.children[1].textContent])
          .filter(([tp]) => tp.startsWith("securacv/"));
        for (const [tp, p] of out) {
          if (tp !== topic("events")) continue;
          const { seq } = JSON.parse(p);
          if (lastSeq !== null) assert.strictEqual(seq, lastSeq + 1, "an events row skipped or repeated a seq");
          lastSeq = seq;
        }
        return out;
      };
      const retained = () => Object.fromEntries(pane.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
      // the lab's first frame finds the room empty: Unknown -> Clear, and
      // drive_fsms signs presence_cleared on it (A106). The broker is not up
      // yet, so it chains and is not published; the chain head the bench
      // lands on connect is one past the example's.
      await run(200);
      assert.deepStrictEqual(signed, ["presence_cleared"], "the boot's first empty frame signs presence_cleared");
      next();
      fresh();
      await fn({ run, click, fresh, retained, next, signed, length: () => length });
    }));
  }

  // presence-only build: walk in, close in, everyone leaves
  await bench(async ({ run, click, fresh, retained, next, length }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
    // mr60_presence.cpp's count follows the debounced state (F131): it reads
    // 0 while presence waits out the debounce, so nothing dirties the row
    // until Present, and no clear row counts an occupant first
    assert.deepStrictEqual(fresh(), [[topic("events"), at(rowOf("walk", "events"), "seq", next())],
      [topic("state"), rowOf("walk", "state")]], "walk: the event, then the state row");
    assert.strictEqual(retained()[topic("chain")], at(rowOf("walk", "chain"), "length", length()));
    assert.strictEqual(retained()[topic("state")], rowOf("walk", "state"), "the retained snapshot did not land the example over it");

    click("approach");
    await run(200);
    assert.deepStrictEqual(fresh(), [], "a range band alone dirties nothing (drive_fsms)");
    await run(data.mqtt.heartbeat_ms);
    assert.deepStrictEqual(fresh(), [[topic("state"), rowOf("approach", "state")]], "the heartbeat carries it");

    click("leave");
    await run(cfg.clear_ms + 500);
    assert.deepStrictEqual(fresh(), [[topic("events"), at(rowOf("leave", "events"), "seq", next())],
      [topic("state"), rowOf("leave", "state")]]);
    assert.strictEqual(retained()[topic("chain")], at(rowOf("leave", "chain"), "length", length()));
  });

  // the lights go out with someone inside; the lux rides every later row
  await bench(async ({ run, click, fresh, retained }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
    fresh();
    click("lights");
    assert.deepStrictEqual(fresh(), [[topic("state"), rowOf("lights", "state")]]);
    await run(data.mqtt.heartbeat_ms + 200);
    assert.strictEqual(retained()[topic("state")], rowOf("lights", "state"), "the heartbeat keeps the lux");
  });
  await bench(async ({ run, click, retained }) => {
    click("lights");   // an empty room: the scene walks someone in first
    await run(cfg.debounce_ms + 700);
    assert.strictEqual(retained()[topic("state")], rowOf("lights", "state"));
  });

  // the radar UART unplugged: Unknown, count and band dropped, radar_ok
  // false; when it comes back (the scene plugs it in again stallUnplugMs()
  // after the unplug, 3 s past the stall window: A125) to the person still
  // inside, presence is earned afresh and signed
  await bench(async ({ run, click, fresh, next, signed }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
    fresh();
    next();
    click("stall");
    await run(cfg.stall_ms + 500);
    assert.deepStrictEqual(fresh(), [[topic("state"), rowOf("stall", "state")]], "health, not a witness event");
    await run(stallUnplugMs(cfg) - cfg.stall_ms);
    await run(cfg.debounce_ms + 700);
    assert.deepStrictEqual(fresh().filter(([tp]) => tp === topic("events")),
      [[topic("events"), at(rowOf("walk", "events"), "seq", next())]], "back to the person inside: presence_detected, never a clear over a body");
    assert.deepStrictEqual(signed, ["presence_cleared", "presence_detected", "presence_detected"]);
  });

  // A106: the radar unplugged over an empty room and plugged in again: its
  // first frame finds the room empty, Unknown -> Clear, and the lab signs
  // presence_cleared as drive_fsms does (the lab used to sign it only from
  // present, so this row never came)
  await bench(async ({ run, click, fresh, retained, next, length, signed }) => {
    click("stall");
    await run(cfg.stall_ms + 500);
    // the row as it stands carries the boot's presence_cleared as its last event
    assert.deepStrictEqual(fresh(), [[topic("state"), at(rowOf("stall", "state"), "last_event", "presence_cleared")]],
      "health, not a witness event");
    await run(stallUnplugMs(cfg) - cfg.stall_ms);
    await run(200);
    assert.deepStrictEqual(fresh(), [[topic("events"), at(rowOf("leave", "events"), "seq", next())],
      [topic("state"), rowOf("leave", "state")]], "back to an empty room: presence_cleared, and the clear row");
    assert.strictEqual(retained()[topic("chain")], at(rowOf("leave", "chain"), "length", length()));
    assert.deepStrictEqual(signed, ["presence_cleared", "presence_cleared"], "boot and the return each sign one");
  });

  // a second visit that returns to a bucket the first visit had moved to:
  // drive_fsms signs any count move while Present, so the second visit's
  // "2+" is witnessed again (the lab used to compare with the last count it
  // had published in any visit, and skipped it)
  await bench(async ({ run, click, fresh }) => {
    const events = () => fresh().filter(([tp]) => tp === topic("events")).map(([, p]) => JSON.parse(p).event);
    click("walk");
    await run(cfg.debounce_ms + 700);
    click("second");
    await run(200);
    click("leave");
    await run(cfg.clear_ms + 500);
    assert.deepStrictEqual(events(), ["presence_detected", "occupancy_changed", "presence_cleared"]);
    click("walk");
    await run(cfg.debounce_ms + 700);
    click("second");
    await run(200);
    assert.deepStrictEqual(events(), ["presence_detected", "occupancy_changed"], "the second visit's 2+ is signed");
  });

  // wellbeing: the lock confirms, then a second person suppresses it
  await bench(async ({ run, click, fresh, retained, next }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
    next();
    click("approach");
    await run(data.mqtt.heartbeat_ms + 200);
    fresh();
    click("sit");
    await run(vit.lock_ms + 500);
    const sit = fresh();
    assert.deepStrictEqual(sit[sit.length - 1], [topic("state"), rowOf("sit", "state")], "the lock and its BPMs");
    click("second");
    await run(200);
    const now = fresh();
    assert.deepStrictEqual(now[0], [topic("events"), at(rowOf("second", "events"), "seq", next())]);
    const held = JSON.parse(now[1][1]);
    assert.deepStrictEqual([held.occupants, held.breathing_locked, held.breath_bpm, held.heart_bpm, held.last_event],
      ["2+", true, null, null, "occupancy_changed"], "the BPMs go at once; the lock rides out lost_ms");
    await run(vit.lost_ms + 500);
    assert.strictEqual(retained()[topic("state")], rowOf("second", "state"));
  });
});

test("the MQTT pane's note names the non-retained topics and claims no step the lab leaves out", async () => {
  const { withFakeDom, fakeBus } = require("./fixtures/fake_dom.js");
  const native = require("./fixtures/mr60_presence_native.js");
  const live = data.mqtt.topics.filter((t) => !t.retained).map((t) => t.suffix);
  assert.deepStrictEqual(live, ["events", "identify"]);
  await withFakeDom(async () => {
    const { buildMqtt } = await import("../assets/sense-ui.js");
    const note = buildMqtt(data, fakeBus()).all("wap-note")[0].textContent;
    for (const sfx of live) assert.ok(note.includes(`securacv/${data.device.id_example}/${sfx}`), sfx);
    assert.doesNotMatch(note, /exact strings/);
    for (const k of ["sig", "public_key", "hash", "fp", "uptime_s", "ts_ms", "bucket_uptime_s"]) assert.ok(note.includes(k), k);
    // the count is no longer a step the lab leaves out (F131): it follows
    // the debounced state on the device and in the lab alike
    assert.doesNotMatch(note, /occupancy_changed to 0 occupants/, "the count is not an omission any more");
    assert.match(note, /0 until presence is confirmed, held while the room empties, and back to 0 with presence_cleared/);
    // nor is the presence_cleared from Unknown (A106): drive_fsms records it
    // on every tick that turns Clear, the boot's first empty frame and a
    // radar back from a stall to an empty room included, and the radar lab
    // signs by the same rule (radarLabSigns, held to the native driver on
    // every tick by the parity test below). The note says so, and claims no
    // step left out.
    assert.doesNotMatch(note, /left out|the lab does not/, "the note claims no omission");
    assert.match(note, /Every tick that turns Clear records a presence_cleared, as on the device, so the radar's first frame of an empty room records one too \(at boot, and when the radar comes back from a stall\)\./);
    assert.ok(mainCpp.includes(native.MAIN_GATES[0]), "drive_fsms signs presence_cleared on any tick that turns Clear");
  });
  assert.doesNotMatch(read(join(ROOT, "assets/sense.js")), /exact MQTT/, "the sandbox lede");
});

// record_event_now chains first and returns before publishing while the
// broker is down; the loop's publish phase is skipped too. So before the
// bench connects nothing reaches the pane, and the snapshot on connect lands
// the chain and the state as they stand (an offline gap is a jump in length).
test("before the broker is up, the lab's events chain but publish nothing", async () => {
  const { withFakeDom, withFakeClock, fakeBus } = require("./fixtures/fake_dom.js");
  assert.ok(mainCpp.includes("  if (!canary::net::mqtt_connected()) return;"), "record_event_now's offline return");
  await withFakeDom(() => withFakeClock(async (clock) => {
    const { buildMqtt } = await import("../assets/sense-ui.js");
    const bus = fakeBus();
    const wrap = buildMqtt(data, bus);
    const topic = (sfx) => `securacv/${data.device.id_example}/${sfx}`;
    const retained = () => Object.fromEntries(wrap.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
    bus.emit("labevent", { event: "presence_detected", presence: "present", occupants: "1", range: "mid" });
    bus.emit("labstate", { snap: { presence: true, presence_state: "present", occupants: "1", range: "mid", radar_ok: true } });
    assert.deepStrictEqual(wrap.all("wap-mqtt-ev"), [], "nothing streams offline");
    assert.deepStrictEqual(retained(), {}, "nothing is retained offline");
    bus.emit("mqtt");
    await clock.advance(150 * data.mqtt.topics.length + 200);
    assert.strictEqual(JSON.parse(retained()[topic("chain")]).length, topicEx("chain").length + 1, "the chain moved offline");
    assert.strictEqual(JSON.parse(retained()[topic("state")]).last_event, "presence_detected");
    assert.ok(!wrap.all("wap-mqtt-ev").some((r) => r.children[0].textContent === topic("events")), "the offline event is not replayed");
  }));
});

test("a stall drops the count with the link, as mr60_presence.cpp does", async () => {
  const { makePresenceFSM } = await import("../assets/sense-ui.js");
  const cfg = data.fsm.presence;
  const fsm = makePresenceFSM(cfg);
  let t = 1000;
  fsm.reset(t);
  const two = { hasTarget: true, count: 2, distanceCm: 200 };
  fsm.tick(two, t);
  fsm.tick(two, t + cfg.debounce_ms);
  assert.deepStrictEqual([fsm.state, fsm.count], ["present", "2+"]);
  const ev = fsm.tick(null, t + cfg.debounce_ms + cfg.stall_ms);
  assert.deepStrictEqual([fsm.state, fsm.count, fsm.range], ["unknown", "0", "unknown"]);
  // the stall is a state change: it carries the zero and reports no count
  // move of its own (F131)
  assert.ok(ev.stalled && ev.stateChanged && !ev.countChanged);
  const presence = read(join(REPO, "firmware/common/sensors/mmwave_mr60/mr60_presence.cpp"));
  assert.ok(presence.includes("        state_ = Presence::Unknown;\n        count_ = CountBucket::Zero;"));
});

// The wellbeing build's VitalsConfig, by its firmware field names, from
// sense.json's vitals block.
const firmwareVitalsConfig = () => {
  const v = data.fsm.vitals;
  return { lock_confirm_ms: v.lock_ms, lock_lost_ms: v.lost_ms, breath_min_bpm: v.breath_bpm[0],
    breath_max_bpm: v.breath_bpm[1], heart_min_bpm: v.heart_bpm[0], heart_max_bpm: v.heart_bpm[1] };
};

// F131: the radar lab's presence FSM against the firmware's own, built from
// mr60_presence.cpp with the host's g++ (fixtures/mr60_presence_native.js),
// on the same frames: state, count, band and both change flags, every tick,
// and (A106) what the lab signs on that tick (radarLabSigns, the rule its
// loop publishes by) against what drive_fsms signs. The traces walk a room
// the way the decoder reports it (a presence flag whose count is still 0
// before the radar's count frame lands), a boot and a stall that come back
// to an empty room, seeded 10,000-tick random walks, and the wellbeing
// build's traces. The lab's tick takes { hasTarget, count, distanceCm } or
// null for a loop with no presence frame.
test("the radar lab's presence FSM ticks, and signs, as mr60_presence.cpp and drive_fsms do, frame for frame", async (t) => {
  const native = require("./fixtures/mr60_presence_native.js");
  for (const gate of native.MAIN_GATES) assert.ok(mainCpp.includes(gate), "main.cpp's drive_fsms gate the driver mirrors: " + gate);
  const b = native.build();
  if (b.skip) { t.skip(b.skip); return; }
  const { makePresenceFSM, radarLabSigns } = await import("../assets/sense-ui.js");
  const p = data.fsm.presence;
  const fw = { present_debounce_ms: p.debounce_ms, clear_timeout_ms: p.clear_ms, stall_timeout_ms: p.stall_ms,
    near_cm: p.near_cm, mid_cm: p.mid_cm };
  // the ticks on which the firmware signs presence_cleared leaving Unknown
  const clearedFromUnknown = (trace, rows) => {
    let prev = "unknown", i = 0, n = 0;
    for (const k of trace) {
      if ("reset" in k) { prev = "unknown"; continue; }
      const r = rows[i++];
      if (prev === "unknown" && r.events.includes("presence_cleared")) n++;
      prev = r.state;
    }
    return n;
  };
  // the wellbeing build's traces too: a vitals frame is, to the presence
  // FSM, a loop with no presence frame (the lab's null)
  const vitals = Object.entries(native.vitalsTraces(fw, firmwareVitalsConfig())).map(([n, tr]) => ["vitals " + n, tr]);
  for (const [name, trace] of [...Object.entries(native.traces(fw)), ...vitals]) {
    const want = native.runFirmware(fw, trace);
    // the A106 traces reach what they are named for: the boot's first empty
    // frame, and a radar back from each stall to an empty room
    if (name === "boot") assert.strictEqual(clearedFromUnknown(trace, want), 1, name);
    if (name === "restart") assert.strictEqual(clearedFromUnknown(trace, want), 3, name);
    if (name === "fuzz") assert.ok(clearedFromUnknown(trace, want) >= 50 && want.filter((r) => r.stalled).length >= 100, name);
    const fsm = makePresenceFSM(p);
    const got = [];
    for (const k of trace) {
      if ("reset" in k) { fsm.reset(k.reset); continue; }
      const frame = "target" in k ? { hasTarget: k.target, count: k.count, distanceCm: k.cm } : null;
      const ev = fsm.tick(frame, k.t);
      got.push({ state: fsm.state, count: fsm.count, range: fsm.range, state_changed: ev.stateChanged,
        count_changed: ev.countChanged, stalled: ev.stalled, events: radarLabSigns(ev, fsm).map(([e]) => e) });
    }
    // the first differing tick, named, before the whole-trace compare
    const at = got.findIndex((g, i) => JSON.stringify(g) !== JSON.stringify(want[i]));
    if (at >= 0) assert.deepStrictEqual(got[at], want[at], `trace "${name}", tick ${at}`);
    assert.deepStrictEqual(got, want, `trace "${name}"`);
    // and what the firmware signs on it: no occupancy_changed while the
    // count reads 0, so an emptying room signs presence_cleared alone
    const signed = want.flatMap((r) => r.events.map((e) => [e, r.count]));
    assert.ok(!signed.some(([e, c]) => e === "occupancy_changed" && c === "0"), `trace "${name}" signs no occupancy_changed to 0`);
    assert.ok(want.every((r) => r.state === "present" || r.count === "0"), `trace "${name}": no count beside a state other than present`);
  }
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

test("the console prints main.cpp's MQTT scene, Hardware ID included, where setup() prints it", async () => {
  // main.cpp: the fleet advert, then the scene (art, separator, boot_kv rows
  // in boot_banner.cpp's "    %-12s%s"), then the one bounded connect.
  const kv = (k, v) => "    " + k.padEnd(12) + v;
  const boot = data.serial.boot;
  const at = (pred, what) => { const i = boot.findIndex(pred); assert.ok(i >= 0, "no " + what); return i; };
  const mdns = at((s) => s.tag === "[MDNS]", "[MDNS] line");
  const art = at((s) => s.text.endsWith("Connecting to MQTT..."), "MQTT scene");
  const hw = at((s) => s.text === kv("Hardware ID", data.device.hwid_example), "Hardware ID line");
  const conn = at((s) => s.tag === "[MQTT]" && s.text.startsWith("Connecting "), "[MQTT] connect line");
  assert.ok(mdns < art && art < hw && hw < conn, "the scene sits between the advert and the connect");
  assert.strictEqual(boot[hw - 1].text, kv("Device ID", data.device.id_example));
  assert.strictEqual(boot[hw + 1].text, kv("Heartbeat", "every " + cint(cfgDefault, "CS_HEARTBEAT_MS") + " ms"));
  assert.ok(mainCpp.includes('boot_kv("Hardware ID", devid_hex);'));
  // the page shows the scene as printed: a line with no tag keeps its indent
  const { bootLines } = await import("../assets/sense-ui.js");
  const lines = bootLines(data.serial).map((l) => l.text);
  assert.ok(lines.includes(kv("Hardware ID", data.device.hwid_example)), "the Hardware ID line lost its indent");
  assert.ok(lines.includes("           .  ((( o )))  ))     Connecting to MQTT..."));
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

test("a radar that returns with a target never passes through clear (mirrors mr60_presence.cpp)", async () => {
  const { makePresenceFSM } = await import("../assets/sense-ui.js");
  const cfg = data.fsm.presence;
  const fsm = makePresenceFSM(cfg);
  const target = { hasTarget: true, count: 1, distanceCm: 200 };
  const empty = { hasTarget: false, count: 0, distanceCm: 0 };
  let t = 1000;
  fsm.reset(t);
  fsm.tick(target, t);
  fsm.tick(target, t + cfg.debounce_ms);
  assert.strictEqual(fsm.state, "present");
  // link drops with the target in view -> Unknown
  t += cfg.debounce_ms + cfg.stall_ms;
  assert.ok(fsm.tick(null, t).stalled);
  assert.strictEqual(fsm.state, "unknown");
  // link returns still reporting the target: no Clear, and no instant
  // Present off the pre-stall debounce clock — the firmware signs
  // presence_cleared on Clear, and a body is in view
  t += 10;
  const back = fsm.tick(target, t);
  assert.strictEqual(fsm.state, "unknown", "must not pass through clear over a body");
  assert.ok(!back.stateChanged, "the returning frame changes nothing yet");
  fsm.tick(target, t + cfg.debounce_ms - 1);
  assert.strictEqual(fsm.state, "unknown", "a fresh debounce must run");
  fsm.tick(target, t + cfg.debounce_ms);
  assert.strictEqual(fsm.state, "present");
  // ...and a link that returns reporting no target goes to clear at once
  t += cfg.debounce_ms + cfg.stall_ms;
  assert.ok(fsm.tick(null, t).stalled);
  assert.strictEqual(fsm.tick(empty, t + 10).state, "clear");
});

// The radar lab on the fake DOM, with a clock that keeps step with its
// frames: performance.now() reads t0 while the lab is built (its FSMs reset
// there and its heartbeat starts), and each pass of run() is one animation
// frame STEP ms after the last followed by STEP ms of the timers, so the
// stall scene's unplug ends between the passes it should. Every serial line
// and state row the lab sends is kept with the pass and the time it came on.
const STEP = 50;
async function onLabBench(d, fn, t0 = 1000) {
  const { withFakeDom, withFakeClock, fakeBus } = require("./fixtures/fake_dom.js");
  const { buildRadarLab, buildSandbox } = await import("../assets/sense-ui.js");
  await withFakeDom(() => withFakeClock(async (clock) => {
    const bus = fakeBus();
    const lab = { t: t0, pass: 0, lines: [], states: [] };
    bus.on("serial", ({ text }) => lab.lines.push({ pass: lab.pass, t: lab.t, text }));
    bus.on("labstate", ({ snap }) => lab.states.push({ pass: lab.pass, t: lab.t, snap }));
    const realNow = performance.now;
    performance.now = () => t0;
    try { lab.wrap = buildRadarLab(d, bus); } finally { performance.now = realNow; }
    lab.pad = buildSandbox(d, bus);
    lab.run = async (ms) => {
      for (let e = 0; e < ms; e += STEP) {
        lab.pass++;
        lab.t += STEP;
        clock.frame(lab.t);
        await clock.advance(STEP);
      }
    };
    lab.cmd = (cmd) => bus.emit("labcmd", { cmd });
    lab.toggle = (id) => lab.wrap.all("sense-toggle").find((b) => b.dataset.id === id).click();
    lab.scene = (id) => lab.pad.all("wap-sand-card").find((c) => c.children[0].textContent === d.sandbox.find((s) => s.id === id).label).click();
    lab.readout = () => lab.pad.all("wap-sandbox-read")[0].textContent;
    await fn(lab);
  }));
}

// Sweep A124: the radar lab's vitals lock against the firmware's own. The lab
// used to run a vitals mirror of its own that no test held, and it differed
// from mr60_vitals.cpp three ways: it took a continuous "plausible" flag
// rather than frames, sat in Unknown until it locked (reaching Lost from
// Unknown only lost_ms after the last valid tick, where the firmware goes
// Lost on its first vitals frame), and had no bpm_valid and no stalled flag.
// It now ticks sense-sim.js's VitalsFSM (held to mr60_vitals.cpp by
// senselab.test.js, A107) on the frames its radar sends. Here the lab itself
// runs a room on the fake DOM: walk in, close in, sit still on the wellbeing
// build (the lock), move (the lock's deadline drops it), still again, a
// second person (bpm_valid stops at once, the lock rides out its window and
// drops), they leave (the lock is earned afresh), the radar unplugged
// (Unknown, and the lock's deadline) and back, and the wellbeing build
// switched off (a presence-only build prints no vitals line). The frames that
// room makes, by the lab's rule (a presence frame each pass; after it, while
// someone is still, within the vitals range and Present on the wellbeing
// build, a vitals frame with the wellbeing row's BPMs; nothing while
// unplugged), are played through the firmware's drive_fsms
// (fixtures/mr60_presence_native.js, -DCANARY_SENSE_VITALS). On every pass the
// page must print the firmware's [presence] and [vitals] lines and publish
// its snapshot, and on every frame the lab's VitalsFSM must tick as g_vitals
// does (lock, its change, stall, bpm_valid, the BPMs, and the single-target
// verdict it was handed). Five planted firmware changes must each be caught.
test("the radar lab's vitals lock ticks as mr60_vitals.cpp and drive_fsms do, through a still, moving, shared and unplugged room (A124)", async (t) => {
  const native = require("./fixtures/mr60_presence_native.js");
  for (const gate of native.VITALS_GATES) assert.ok(mainCpp.includes(gate), "main.cpp's drive_fsms vitals lines the driver compiles: " + gate);
  assert.ok(mainCpp.includes('    boot_linef("[vitals] breathing %s%s", locked ? "locked" : "lost",\n               vev.stalled ? " (stall)" : "");'),
    "drive_fsms prints the lock line on lock_changed, with ' (stall)' on the deadline's drop");
  const b = native.build();
  if (b.skip) { t.skip(b.skip); return; }
  const ui = await import("../assets/sense-ui.js");
  const S = await import("../assets/sense-sim.js");
  const p = data.fsm.presence, v = data.fsm.vitals;
  const fw = { present_debounce_ms: p.debounce_ms, clear_timeout_ms: p.clear_ms, stall_timeout_ms: p.stall_ms, near_cm: p.near_cm, mid_cm: p.mid_cm };
  const vcfg = firmwareVitalsConfig();
  const well = JSON.parse(data.mqtt.topics.find((x) => x.suffix === "state").wellbeing);
  assert.ok(well.breath_bpm >= vcfg.breath_min_bpm && well.breath_bpm <= vcfg.breath_max_bpm && well.heart_bpm >= vcfg.heart_min_bpm
    && well.heart_bpm <= vcfg.heart_max_bpm, "the wellbeing row's BPMs are plausible to the firmware");
  const L = v.lock_ms, X = v.lost_ms, D = p.debounce_ms, U = p.stall_ms + 3000;
  const t0 = 1000;

  // the room, as the lab's labcmd handler and toggles place it (sense-ui.js)
  const FOV = (data.radar.fov_deg * Math.PI / 180) / 2, MAXR = data.radar.presence_max_m, VITR = data.radar.vitals_range_m;
  const cone = ([x, y]) => { const d = Math.hypot(x, y); return d <= MAXR && d >= 0.1 && Math.abs(Math.atan2(y, x)) <= FOV ? d : 0; };
  const room = { p1: [5.9, 1.6], p2: null, still: false, wellbeing: false, unplugged: 0 };
  // [ms after boot, what the visitor does to the lab, what that does to the room]
  const script = [
    [500, (lab) => lab.cmd("walkin"), () => { room.p1 = [3.0, 0.4]; }],
    [1500, (lab) => lab.cmd("near"), () => { room.p1 = [1.1, 0.1]; }],
    [2000, (lab) => lab.cmd("still"), () => { room.still = room.wellbeing = true; }],
    [3000 + L, (lab) => lab.toggle("still"), () => { room.still = false; }],
    [3500 + L + X, (lab) => lab.toggle("still"), () => { room.still = true; }],
    [4000 + 2 * L + X, (lab) => lab.cmd("person2"), () => { room.p2 = [4.6, -1.4]; }],
    [4500 + 2 * L + 2 * X, (lab) => lab.toggle("p2"), () => { room.p2 = null; }],
    [5000 + 3 * L + 2 * X, (lab) => lab.cmd("stall"), () => { room.unplugged = U / STEP; }],
    [6000 + 4 * L + 2 * X + U + D, (lab) => lab.toggle("well"), () => { room.wellbeing = false; }],
  ];
  const END = 7000 + 4 * L + 3 * X + U + D;

  // the lab's VitalsFSM, every tick: what it was handed and what it said
  const proto = S.VitalsFSM.prototype, realTick = proto.tick;
  const seen = [];
  proto.tick = function (frame, single, now) {
    const ev = realTick.call(this, frame, single, now);
    seen.push({ fsm: this, frame: { ...frame }, single, now, ev: { ...ev } });
    return ev;
  };
  const passes = [];
  let lines, states;
  try {
    await onLabBench(data, async (lab) => {
      let si = 0;
      for (let ms = STEP; ms <= END; ms += STEP) {
        while (si < script.length && script[si][0] < ms) { script[si][1](lab); script[si][2](); si++; }
        const unplugged = room.unplugged > 0;
        if (unplugged) room.unplugged--;
        const people = [room.p1, room.p2].filter(Boolean);
        const ds = people.map(cone).filter((d) => d > 0);
        const frame = unplugged ? null : ds.length ? { hasTarget: true, count: ds.length, distanceCm: Math.min(...ds) * 100 }
          : { hasTarget: false, count: 0, distanceCm: 0 };
        const near = people.some((q) => cone(q) > 0 && cone(q) <= VITR);
        passes.push({ t: lab.t + STEP, frame, vitals: !!frame && room.wellbeing && room.still && near, wellbeing: room.wellbeing, still: room.still });
        await lab.run(STEP);
      }
      assert.strictEqual(si, script.length, "the whole script ran");
      ({ lines, states } = lab);
    }, t0);
  } finally {
    proto.tick = realTick;
  }

  // the room's frames through the firmware: its presence FSM first (a vitals
  // frame moves no presence: it lands on the same ms as the presence frame
  // before it, inside every window), then the vitals frames where the room
  // and the firmware's Present call for them, and drive_fsms on the lot
  const pTick = (q) => (q.frame ? { t: q.t, target: q.frame.hasTarget, count: q.frame.count, cm: Math.round(q.frame.distanceCm) } : { t: q.t });
  const presence = native.runFirmware(fw, [{ reset: t0 }, ...passes.map(pTick)]);
  passes.forEach((q, i) => {
    if (q.frame && q.frame.hasTarget)
      assert.strictEqual(ui.rangeBandOf(q.frame.distanceCm, p), presence[i].range, `pass ${i + 1}: the lab's unrounded cm bands as the firmware's`);
  });
  const trace = [{ reset: t0 }], passOf = [];
  passes.forEach((q, i) => {
    trace.push(pTick(q));
    passOf.push(i);
    if (q.vitals && presence[i].state === "present") {
      trace.push({ t: q.t, breath: well.breath_bpm, heart: well.heart_bpm });
      passOf.push(i);
    }
  });
  const want = native.runFirmware(fw, trace, vcfg);
  want.forEach((r, i) => {
    if (!("breath" in trace[i + 1])) assert.strictEqual(r.state, presence[passOf[i]].state, `tick ${i}: a vitals frame moved presence`);
  });

  // The lab against a firmware run of the room's frames: the first
  // difference, named, or null. Per pass, the console lines in tick order
  // and the state row it published (a row is due wherever the firmware's
  // snapshot moved, the range band alone aside); then tick by tick, the
  // VitalsFSM's inputs and event against g_vitals's.
  const linesBy = passes.map(() => []);
  for (const l of lines) linesBy[l.pass - 1].push(l.text);
  const pubBy = new Map(states.map((s) => [s.pass - 1, s.snap]));
  const moved = (a, b) => !b || [...new Set([...Object.keys(a), ...Object.keys(b)])].some((k) => k !== "range" && a[k] !== b[k]);
  const frameKey = (f) => (f.kind === S.FrameKind.Vitals ? `V ${f.breath_rate} ${f.heart_rate}`
    : f.kind === S.FrameKind.Presence ? `P ${f.has_target ? 1 : 0} ${f.target_count} ${f.distance_cm}` : f.kind === S.FrameKind.None ? "N" : `? ${f.kind}`);
  const traceKey = (k, q) => ("breath" in k ? `V ${k.breath} ${k.heart}` : "target" in k ? `P ${k.target ? 1 : 0} ${k.count} ${q.frame.distanceCm}` : "N");
  function differs(rows) {
    const exp = passes.map(() => ({ lines: [], snap: null }));
    rows.forEach((r, i) => {
      const q = passes[passOf[i]], o = exp[passOf[i]];
      if (r.state_changed) o.lines.push(`[presence] -> ${r.state}${r.stalled ? " (radar stall)" : ""}`);
      if (r.vitals.lock_changed && q.wellbeing) o.lines.push(`[vitals] breathing ${r.vitals.lock === "locked" ? "locked" : "lost"}${r.vitals.stalled ? " (stall)" : ""}`);
      o.snap = { presence: r.state === "present", presence_state: r.state, occupants: r.count, range: r.range, radar_ok: r.state !== "unknown" };
      if (q.wellbeing) {
        Object.assign(o.snap, { breathing_locked: r.vitals.breathing_locked,
          breath_bpm: r.vitals.bpm_valid ? r.vitals.breath_bpm : null, heart_bpm: r.vitals.bpm_valid ? r.vitals.heart_bpm : null });
      }
    });
    for (let k = 0; k < passes.length; k++) {
      const at = `pass ${k + 1} (t=${passes[k].t})`;
      if (JSON.stringify(linesBy[k]) !== JSON.stringify(exp[k].lines))
        return `${at}: the console said ${JSON.stringify(linesBy[k])} where the firmware says ${JSON.stringify(exp[k].lines)}`;
      const pub = pubBy.get(k);
      if (pub && JSON.stringify(pub) !== JSON.stringify(exp[k].snap))
        return `${at}: the lab published ${JSON.stringify(pub)} where the firmware's snapshot is ${JSON.stringify(exp[k].snap)}`;
      if (!pub && moved(exp[k].snap, k && exp[k - 1].snap)) return `${at}: the firmware's state row is dirty and the lab published none`;
    }
    if (seen.length !== rows.length) return `the lab ticked a VitalsFSM ${seen.length} times where drive_fsms ticks g_vitals ${rows.length}`;
    if (new Set(seen.map((s) => s.fsm)).size !== 1) return "the lab ticked more than one VitalsFSM";
    for (let i = 0; i < rows.length; i++) {
      const s = seen[i], r = rows[i], k = trace[i + 1];
      const got = { t: s.now, frame: frameKey(s.frame), single: s.single, lock: s.ev.lock, lock_changed: s.ev.lock_changed,
        stalled: s.ev.stalled, bpm_valid: !!s.ev.bpm_valid, breath_bpm: s.ev.breath_bpm || 0, heart_bpm: s.ev.heart_bpm || 0 };
      const fwRow = { t: k.t, frame: traceKey(k, passes[passOf[i]]), single: r.count === "1", lock: r.vitals.lock, lock_changed: r.vitals.lock_changed,
        stalled: r.vitals.stalled, bpm_valid: r.vitals.bpm_valid, breath_bpm: r.vitals.breath_bpm, heart_bpm: r.vitals.heart_bpm };
      if (JSON.stringify(got) !== JSON.stringify(fwRow)) return `tick ${i}: the lab's VitalsFSM ${JSON.stringify(got)}, the firmware's ${JSON.stringify(fwRow)}`;
    }
    return null;
  }
  const real = differs(want);
  assert.strictEqual(real, null, real);

  // the room reaches what it is for, on the firmware's own rows (so a
  // retuned window cannot empty the compare)
  const ticks = want.map((r, i) => ({ r, k: trace[i + 1], q: passes[passOf[i]], prev: want[i - 1] }));
  const firstV = ticks.find(({ k }) => "breath" in k);
  assert.ok(firstV && firstV.prev.vitals.lock === "unknown" && firstV.r.vitals.lock === "lost" && firstV.r.vitals.lock_changed && !firstV.r.vitals.stalled,
    "the first vitals frame: Unknown to Lost, seen data not yet confirmed (the old mirror sat in Unknown)");
  const confirms = ticks.filter(({ r }) => r.vitals.lock_changed && r.vitals.lock === "locked");
  assert.strictEqual(confirms.length, 4, "the lock is earned four times: sitting, still again, alone again, back from the stall");
  assert.strictEqual(confirms[0].k.t, firstV.k.t + L, "lock_confirm_ms after the first valid frame");
  const drops = ticks.filter(({ r }) => r.vitals.stalled);
  assert.strictEqual(drops.length, 4, "the deadline drops the lock four times");
  assert.ok(drops.every(({ r }) => r.vitals.lock === "lost" && r.vitals.lock_changed && !r.vitals.bpm_valid), "each drop is to Lost, BPMs gone");
  assert.ok(drops[0].r.count === "1" && !drops[0].q.still, "moving: a single person's lock drops on the deadline");
  assert.strictEqual(drops[1].r.count, "2+", "a second person: the lock drops on the deadline with two in the room");
  assert.strictEqual(drops[2].r.state, "unknown", "unplugged: the lock drops while the radar is silent");
  assert.ok(!drops[3].q.wellbeing && !linesBy[passOf[want.indexOf(drops[3].r)]].some((l) => l.startsWith("[vitals]")),
    "on the presence-only build the drop prints no vitals line");
  assert.ok(ticks.some(({ r, prev }) => r.count === "2+" && prev.count === "1" && prev.vitals.bpm_valid && !r.vitals.bpm_valid && r.vitals.lock === "locked"),
    "a second person stops bpm_valid at once while the lock holds");
  assert.ok(ticks.some(({ r }) => r.stalled && r.state === "unknown"), "the radar stall turns presence Unknown");
  assert.ok(lines.some((l) => l.text === "[vitals] breathing lost (stall)") && lines.some((l) => l.text === "[vitals] breathing locked"));

  // the lab builds no vitals machine of its own: one VitalsFSM, sense-sim.js's
  const src = read(join(ROOT, "assets/sense-ui.js"));
  assert.ok(src.includes('import { VitalsFSM, FrameKind } from "./sense-sim.js";'), "the lab's vitals lock is sense-sim.js's port");
  assert.strictEqual(src.split("new VitalsFSM(").length, 2, "one VitalsFSM, the lab loop's");
  assert.ok(!("makeVitalsFSM" in ui) && !src.includes("makeVitalsFSM"), "the lab's own vitals mirror is gone");
  assert.doesNotMatch(src, /\block\s*=\s*"(?:unknown|lost|locked)"/, "no lock state kept by hand");

  // and the compare bites: each planted change to the firmware is caught
  const V = "mr60_vitals.cpp";
  const mutants = [
    ["lock_confirm_ms halved", [[V, "elapsed(now_ms, valid_since_ms_, cfg_.lock_confirm_ms)", "elapsed(now_ms, valid_since_ms_, cfg_.lock_confirm_ms / 2)"]]],
    ["a lock stall that never clears (the lock's deadline fires again on every tick once it has)",
      [[V, "if (lock_ != VitalsLock::Lost && lock_ != VitalsLock::Unknown &&", "if (lock_ != VitalsLock::Unknown &&"]]],
    ["a radar stall that never clears (mr60_presence.cpp's clock stops in Unknown, so presence never returns)",
      [["mr60_presence.cpp", "    last_frame_ms_ = now_ms;\n\n    // Track raw target edges", "    if (state_ != Presence::Unknown) last_frame_ms_ = now_ms;\n\n    // Track raw target edges"]]],
    ["bpm_valid inverted", [[V, "ev.bpm_valid  = (lock_ == VitalsLock::Locked) && single_target;", "ev.bpm_valid  = !((lock_ == VitalsLock::Locked) && single_target);", 2]]],
    ["the exactly-one-target suppression dropped", [[V, "const bool valid = single_target && plausible(frame);", "const bool valid = plausible(frame);"]]],
  ];
  for (const [what, swaps] of mutants) {
    const m = native.buildMutant(swaps);
    const caught = differs(native.runFirmware(fw, trace, vcfg, m.bin));
    assert.ok(caught, `the planted change "${what}" passes the compare`);
  }
});

// Sweep A125: the stall scene unplugged the radar for a literal 8000 ms, but
// sense_config.h lets the stall seed be tuned anywhere in
// SENSE_STALL_MS_LO..HI, and gen_sense.py writes the scene's blurb from it
// ("No frame for <stall_ms> ms → Unknown"). Past 8000 ms the scene never
// reached Unknown and the blurb was false on the page. The unplug is now
// stallUnplugMs(): the stall window and 3 s more. Each tuning is played on a
// copy of sense.json in memory (sense.json itself is held to config.h by the
// pins above), its blurb rewritten as gen_sense.py writes it: the card says
// the window, the lab reaches Unknown exactly that long after its last
// frame, and the radar comes back 3 s later to whoever is there.
test("the stall scene reaches Unknown and comes back wherever the stall window is tuned, and its blurb holds (A125)", async () => {
  const senseCfgH = read(join(PRJ, "include/canary/sense_config.h"));
  const bound = (n) => Number(senseCfgH.match(new RegExp(`SENSE_STALL_MS_${n}\\s*=\\s*(\\d+);`))[1]);
  const LO = bound("LO"), HI = bound("HI");
  assert.deepStrictEqual([LO, HI], [1000, 20000], "the stall seed's bounds");
  const p = data.fsm.presence;
  const BLURB = (ms) => `No frame for ${ms} ms → Unknown`;
  const stall = data.sandbox.find((s) => s.id === "stall");
  assert.ok(stall.blurb.startsWith(BLURB(p.stall_ms) + " "), "the scene's blurb names the window gen_sense.py read: " + stall.blurb);
  // each tuning's verdict, null when the scene does what its blurb says
  const verdicts = {};
  for (const ms of [LO, p.stall_ms, 9000, HI]) {
    for (const inside of [true, false]) {
      const d = JSON.parse(JSON.stringify(data));
      d.fsm.presence.stall_ms = ms;
      const sc = d.sandbox.find((s) => s.id === "stall");
      sc.blurb = sc.blurb.replace(BLURB(p.stall_ms), BLURB(ms));
      const at = `stall_ms ${ms}, ${inside ? "someone inside" : "an empty room"}`;
      await onLabBench(d, async (lab) => {
        if (inside) {
          lab.scene("walk");
          await lab.run(p.debounce_ms + 700);
          assert.ok(lab.lines.some((l) => l.text === "[presence] -> present"), `${at}: walked in`);
        } else {
          await lab.run(200);
        }
        const lastFrame = lab.t;
        const from = lab.lines.length;
        lab.scene("stall");
        const said = lab.readout().match(/No frame for (\d+) ms → Unknown/);
        assert.ok(said && Number(said[1]) === ms, `${at}: the card says ${lab.readout()}`);
        // long enough to see a late return too
        await lab.run(ms + 3000 + p.debounce_ms + 5000);
        const after = lab.lines.slice(from);
        const unknown = after.find((l) => l.text === "[presence] -> unknown (radar stall)");
        const back = unknown && after.find((l) => l.t > unknown.t && l.text.startsWith("[presence] -> "));
        // the first frame after the unplug is a pass past it; a target then
        // earns presence afresh over the debounce, an empty room is Clear at once
        const plugged = lastFrame + ms + 3000 + STEP;
        const want = inside ? ["[presence] -> present", plugged + p.debounce_ms] : ["[presence] -> clear", plugged];
        verdicts[at] = !unknown ? `never reached Unknown (${JSON.stringify(after.map((l) => l.text))})`
          : unknown.t - lastFrame !== ms ? `Unknown ${unknown.t - lastFrame} ms after the last frame, not the blurb's ${ms}`
            : !back ? "the radar never came back"
              : back.text !== want[0] || back.t !== want[1] ? `came back as ${JSON.stringify([back.text, back.t - lastFrame])}, not ${JSON.stringify([want[0], want[1] - lastFrame])} (ms after the last frame)`
                : after.filter((l) => l.text.startsWith("[presence] -> ")).length !== 2 ? `said more: ${JSON.stringify(after.map((l) => l.text))}`
                  : null;
      });
    }
  }
  assert.deepStrictEqual(Object.entries(verdicts).filter(([, v]) => v), [], "each tuning reaches Unknown on the blurb's ms and comes back 3 s on");
  const { stallUnplugMs } = await import("../assets/sense-ui.js");
  for (const ms of [LO, 9000, HI]) assert.strictEqual(stallUnplugMs({ ...p, stall_ms: ms }), ms + 3000);
  assert.strictEqual(stallUnplugMs(p), 8000, "at today's stall window the scene is unchanged");
  assert.ok(read(join(ROOT, "assets/sense-ui.js")).includes("setTimeout(() => { world.stalled = false; }, stallUnplugMs(cfg));"),
    "the scene's unplug is stallUnplugMs of the lab's presence config");
});

test("bootLines flattens banner+boot+ready in order with mapped classes", async () => {
  const { bootLines } = await import("../assets/sense-ui.js");
  const lines = bootLines(data.serial);
  assert.strictEqual(lines.length,
    data.serial.banner.length + data.serial.boot.length + data.serial.ready.length);
  for (const l of lines) { assert.ok(typeof l.text === "string"); assert.ok(/^wap-/.test(l.cls)); }
});
