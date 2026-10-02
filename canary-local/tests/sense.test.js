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
  const { withFakeDom, fakeBus } = require("./fixtures/fake_dom.js");
  await withFakeDom(async () => {
    const { buildMqtt } = await import("../assets/sense-ui.js");
    const bus = fakeBus();
    const wrap = buildMqtt(data, bus);
    const topic = (sfx) => `securacv/${data.device.id_example}/${sfx}`;
    const stream = () => wrap.all("wap-mqtt-ev").map((r) => [r.children[0].textContent, r.children[1].textContent]);
    const retained = () => Object.fromEntries(wrap.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
    const chain0 = topicEx("chain").length;
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
  });
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
  // wellbeing: BPMs only while locked with exactly one target (bpm_valid)
  const v = { locked: true, breath_bpm: 14, heart_bpm: 68 };
  assert.strictEqual(senseLabSnapshot(fsm("present", "1", "near"), v).breath_bpm, 14);
  const two = senseLabSnapshot(fsm("present", "2+", "near"), v);
  assert.deepStrictEqual([two.breathing_locked, two.breath_bpm, two.heart_bpm], [true, null, null],
    "a second person drops the BPMs at once; the lock rides out lost_ms");
  // due: any change but the range band alone, and every heartbeat
  const a = senseLabSnapshot(fsm("present", "1", "mid"), null);
  assert.ok(senseStateDue(a, null, false), "the first (boot) publish");
  assert.ok(!senseStateDue({ ...a, range: "near" }, a, false), "a range band alone waits for the heartbeat");
  assert.ok(senseStateDue({ ...a, range: "near" }, a, true), "...and goes out on it");
  assert.ok(senseStateDue({ ...a, occupants: "2+" }, a, false));
  assert.ok(senseStateDue(senseLabSnapshot(fsm("present", "1", "mid"), { ...v, locked: false }), a, false), "a build switch");
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
  const { buildRadarLab, buildMqtt, buildSandbox } = await import("../assets/sense-ui.js");
  const topic = (sfx) => `securacv/${data.device.id_example}/${sfx}`;
  const scene = (id) => data.sandbox.find((s) => s.id === id);
  const rowOf = (id, sfx) => scene(id).mqtt.find((p) => p.suffix === sfx).payload;
  const at = (payload, k, n) => JSON.stringify({ ...JSON.parse(payload), [k]: n });
  const cfg = data.fsm.presence, vit = data.fsm.vitals;
  const chain0 = topicEx("chain").length;

  async function bench(fn) {
    await withFakeDom(() => withFakeClock(async (clock) => {
      const bus = fakeBus();
      buildRadarLab(data, bus);
      const pane = buildMqtt(data, bus);
      const pad = buildSandbox(data, bus);
      let t = performance.now();
      const run = async (ms) => { for (let e = 0; e < ms; e += 50) { t += 50; clock.frame(t); } await clock.advance(ms); };
      const click = (id) => pad.all("wap-sand-card").find((c) => c.children[0].textContent === scene(id).label).click();
      let seen = new Set();
      // the rows published since the last call, oldest first (the stream
      // prepends, so new rows sit at the top)
      const fresh = () => {
        const rows = pane.all("wap-mqtt-ev").filter((r) => !seen.has(r));
        rows.forEach((r) => seen.add(r));
        return rows.reverse().map((r) => [r.children[0].textContent, r.children[1].textContent])
          .filter(([tp]) => tp.startsWith("securacv/"));
      };
      const retained = () => Object.fromEntries(pane.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
      await run(200);   // the lab's first frame: Unknown -> Clear, the boot publish
      fresh();
      await fn({ run, click, fresh, retained });
    }));
  }

  // presence-only build: walk in, close in, everyone leaves
  await bench(async ({ run, click, fresh, retained }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
    // mr60_presence.cpp's count and band follow every frame while presence
    // waits out the debounce, and drive_fsms dirties the row on the count
    // change: so the device publishes a clear row counting one occupant first
    const counting = JSON.stringify({ ...JSON.parse(rowOf("walk", "state")), presence: false, presence_state: "clear" });
    assert.deepStrictEqual(fresh(), [[topic("state"), counting], [topic("events"), rowOf("walk", "events")],
      [topic("state"), rowOf("walk", "state")]], "walk: the debounce window's row, the event, then the state row");
    assert.strictEqual(retained()[topic("chain")], rowOf("walk", "chain"));
    assert.strictEqual(retained()[topic("state")], rowOf("walk", "state"), "the retained snapshot did not land the example over it");

    click("approach");
    await run(200);
    assert.deepStrictEqual(fresh(), [], "a range band alone dirties nothing (drive_fsms)");
    await run(data.mqtt.heartbeat_ms);
    assert.deepStrictEqual(fresh(), [[topic("state"), rowOf("approach", "state")]], "the heartbeat carries it");

    click("leave");
    await run(cfg.clear_ms + 500);
    assert.deepStrictEqual(fresh(), [[topic("events"), at(rowOf("leave", "events"), "seq", chain0 + 2)],
      [topic("state"), rowOf("leave", "state")]]);
    assert.strictEqual(retained()[topic("chain")], at(rowOf("leave", "chain"), "length", chain0 + 2));
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

  // the radar UART unplugged: Unknown, count and band dropped, radar_ok false
  await bench(async ({ run, click, fresh }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
    fresh();
    click("stall");
    await run(cfg.stall_ms + 500);
    assert.deepStrictEqual(fresh(), [[topic("state"), rowOf("stall", "state")]], "health, not a witness event");
  });

  // wellbeing: the lock confirms, then a second person suppresses it
  await bench(async ({ run, click, fresh, retained }) => {
    click("walk");
    await run(cfg.debounce_ms + 700);
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
    assert.deepStrictEqual(now[0], [topic("events"), at(rowOf("second", "events"), "seq", chain0 + 2)]);
    const held = JSON.parse(now[1][1]);
    assert.deepStrictEqual([held.occupants, held.breathing_locked, held.breath_bpm, held.heart_bpm, held.last_event],
      ["2+", true, null, null, "occupancy_changed"], "the BPMs go at once; the lock rides out lost_ms");
    await run(vit.lost_ms + 500);
    assert.strictEqual(retained()[topic("state")], rowOf("second", "state"));
  });
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
  assert.ok(ev.stalled && ev.countChanged);
  const presence = read(join(REPO, "firmware/common/sensors/mmwave_mr60/mr60_presence.cpp"));
  assert.ok(presence.includes("        state_ = Presence::Unknown;\n        count_ = CountBucket::Zero;"));
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
