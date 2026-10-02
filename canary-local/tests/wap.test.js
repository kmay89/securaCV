// canary-local/tests/wap.test.js — the WAP page's honesty gate.
//
// Two jobs, mirroring tests/homeassistant.test.js:
//  1. Cross-check devices/wap.json against its sources of truth (the
//     canary-wap firmware, the registry, boards.json) so a hand-edit that
//     bypasses the generator — or firmware drift — is caught here, not just
//     by the generator's own asserts. Every SSID, route, MQTT topic, HA
//     entity, boot line and wizard label the page shows must still exist in
//     the source it claims to come from.
//  2. Exercise the DOM-free cores the page ships (wap-ui.js: withId,
//     bootLines, mqttApply, pillForEvent) so their contracts can't rot.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const FW = join(REPO, "firmware/projects/canary-wap/arduino/canary_wap");

const read = (p) => readFileSync(p, "utf8");
const data = JSON.parse(read(join(ROOT, "devices/wap.json")));
const registry = JSON.parse(read(join(ROOT, "devices/registry.json")));
const boards = JSON.parse(read(join(ROOT, "devices/boards.json")));

const ino = read(join(FW, "canary_wap.ino"));
const wapServerH = read(join(FW, "wap_server.h"));
const setupWizardH = read(join(FW, "setup_wizard.h"));
const setupPageH = read(join(FW, "setup_page_html.h"));
const bootBannerCpp = read(join(FW, "boot_banner.cpp"));
const csiMqttCpp = read(join(FW, "csi_mqtt.cpp"));
const companionH = read(join(FW, "companion_pwa.h"));
const doc = read(join(REPO, "docs/getting_started_canary.md"));

// ── 1. shape + sanity floors ───────────────────────────────────────────────
test("wap.json has every section the page requires", () => {
  for (const k of ["device", "ap", "captive", "routes", "wizard", "serial", "mqtt", "sensing", "sandbox", "docs"])
    assert.ok(data[k], "missing section: " + k);
});

test("counts are not thin (a broken parse would fail here)", () => {
  assert.strictEqual(data.wizard.steps.length, 5);
  assert.strictEqual(data.mqtt.discovery.entities.length, 24);
  assert.ok(data.serial.boot.length >= 15, "boot log too short");
  assert.ok(data.mqtt.topics.length >= 10, "too few MQTT topics");
  assert.ok(data.sandbox.length >= 5, "too few sandbox scenarios");
  assert.strictEqual(data.sensing.pills.length, 5);
});

// ── 2. version + identity cross-checks ─────────────────────────────────────
test("firmware version matches the .ino constant and rides the registry train", () => {
  const m = ino.match(/FIRMWARE_VERSION\s*=\s*"([^"]+)"/);
  assert.ok(m, "FIRMWARE_VERSION not found in canary_wap.ino");
  assert.strictEqual(data.device.fw_version, m[1]);
  assert.strictEqual(data.device.fw_train, registry.fw_train);
  assert.ok(data.device.fw_version.startsWith(data.device.fw_train));
});

test("device_type matches the .ino and the board mapping matches boards.json", () => {
  assert.match(ino, new RegExp('DEVICE_TYPE\\s*=\\s*"' + data.device.device_type + '"'));
  // device_board maps a device to a LIST of boards (primary first); the device's
  // declared board_id must be one of them
  const wapBoards = [].concat(boards.device_board["canary-wap"]);
  assert.ok(wapBoards.includes(data.device.board_id),
    `${data.device.board_id} not in canary-wap board mapping (${wapBoards.join(", ")})`);
  assert.ok(boards.boards[data.device.board_id], "board id not in boards.json");
});

// ── 3. the SoftAP / captive facts trace to the firmware ────────────────────
test("AP + setup constants are the firmware's own", () => {
  assert.ok(wapServerH.includes('AP_SSID_PREFIX    = "' + data.ap.ssid_prefix + '"'));
  assert.ok(data.ap.ssid_example.startsWith(data.ap.ssid_prefix));
  assert.strictEqual(data.ap.ip, "192.168.4.1");
  assert.ok(ino.includes('"SecuraCV-%s", suffix'), "AP ssid format drifted");
  assert.ok(ino.includes('"cv-%s", encoded'), "AP password format drifted");
  assert.ok(setupWizardH.includes('prefs.begin("' + data.ap.nvs_namespace + '"'));
  assert.ok(setupWizardH.includes('getBool("' + data.ap.nvs_key + '"'));
});

test("captive.html is the firmware's CAPTIVE_PORTAL_HTML, verbatim", () => {
  const m = setupPageH.match(/R"HTML\(([\s\S]*)\)HTML"/);
  assert.ok(m, "CAPTIVE_PORTAL_HTML not found");
  assert.strictEqual(data.captive.html, m[1], "captive html drifted from firmware");
  assert.match(data.captive.html, /Set up your Canary/);
  // the OS-probe success tokens are the host-tested contract
  const probe = read(join(FW, "captive_probe.h"));
  assert.ok(probe.includes("Microsoft NCSI") && probe.includes("Microsoft Connect Test"));
});

// ── 4. every setup route still exists in the firmware ──────────────────────
test("setup routes are real string literals in the firmware", () => {
  for (const r of ["/companion", "/api/wifi/scan", "/api/wifi/connect", "/api/wifi/ap-only", "/api/wifi/pair-token"]) {
    assert.ok(data.routes.some((x) => x.path === r), "route missing from wap.json: " + r);
    assert.ok(ino.includes(r), "route not in firmware: " + r);
  }
});

// ── 5. MQTT topics + HA discovery trace to csi_mqtt.cpp ────────────────────
test("MQTT prefix + topic pattern are the firmware's", () => {
  assert.ok(csiMqttCpp.includes('DEFAULT_PREFIX = "' + data.mqtt.prefix + '"'));
  assert.ok(csiMqttCpp.includes('"%s/%s/%s"'), "build_topic format drifted");
  assert.ok(csiMqttCpp.includes('DISCOVERY_PREFIX = "' + data.mqtt.discovery.prefix + '"'));
});

test("only MOMENT topics are non-retained (the retention rule the page relies on)", () => {
  // Two topics carry moments rather than state, and a moment must never be
  // redelivered as if current: a retained `events` row would replay an old
  // event into HA's history on every reconnect, and a retained `tamper`
  // row would re-fire the general tamper sensor — it triggers on ANY
  // publish, retained delivery included — turning one real tamper into an
  // alarm on every subscribe. Everything else is state and stays retained.
  const MOMENTS = ["events", "tamper"];
  for (const suffix of MOMENTS) {
    const t = data.mqtt.topics.find((x) => x.suffix === suffix);
    assert.ok(t && t.retained === false, suffix + " must be non-retained");
  }
  for (const t of data.mqtt.topics)
    if (!MOMENTS.includes(t.suffix) && !String(t.payload).startsWith('"'))
      assert.strictEqual(t.retained, true, t.suffix + " should be retained");
});

test("every MQTT topic suffix appears in csi_mqtt.cpp", () => {
  for (const t of data.mqtt.topics.concat(data.mqtt.subscribed)) {
    const leaf = t.suffix.split("/").pop();
    assert.ok(csiMqttCpp.includes('"' + t.suffix + '"') || csiMqttCpp.includes('"' + leaf + '"'),
      "topic not in firmware: " + t.suffix);
  }
});

// Sweep A32: the events example used to carry seven of the wire body's
// seventeen keys (no module, category, privacy, timestamp, zone, confidence,
// duration_sec, replay...) and a state, "motion", that no module emits. Every
// topic's example is now keyed as the firmware writes it: the snprintf
// format's keys, in its order (csi_event_wire.h's for the events and tamper
// bodies, the one builder both trees publish through).
const eventWireH = read(join(FW, "csi_event_wire.h"));
const fnBody = (src, signature) => {
  const i = src.indexOf(signature);
  assert.ok(i >= 0, "not found: " + signature);
  const j = src.indexOf("\n}\n", i);
  return src.slice(i, j < 0 ? undefined : j);
};
const slice = (src, from, to) => {
  const i = src.indexOf(from);
  assert.ok(i >= 0, "not found: " + from);
  const j = src.indexOf(to, i + from.length);
  assert.ok(j >= 0, "not found: " + to);
  return src.slice(i, j);
};
const keysOf = (fragment) => [...fragment.matchAll(/\\"([a-z_0-9]+)\\":/g)].map((m) => m[1]);
function firmwareKeys() {
  const wire = fnBody(eventWireH, "inline size_t build_event_body(");
  const health = fnBody(csiMqttCpp, "void publish_health(");
  const signedBranch = (sig) => keysOf(slice(fnBody(csiMqttCpp, sig), "if (signed_ok) {", "} else {"));
  return {
    status: keysOf(fnBody(csiMqttCpp, "void publish_status(")),
    events: [...keysOf(slice(wire, "const int n = snprintf(body, cap,", "if (n <= 0")),
             ...keysOf(slice(wire, "if (signed_ok) {", "} else {"))],
    chain: signedBranch("void publish_chain("),
    counts: signedBranch("void publish_counts("),
    // the battery-less branch, then the tamper levels, each sent only when reported
    health: [...keysOf(slice(health, "} else {", "if (n <= 0")), ...keysOf(slice(health, "if (n <= 0", "if (len + 1"))],
    tamper: keysOf(fnBody(eventWireH, "inline size_t build_tamper_bridge_body(")),
    sensing: keysOf(slice(ino, "char sensing_json[320];", "if (sn > 0")),
    mesh: keysOf(fnBody(csiMqttCpp, "void publish_mesh(")),
    chirp: keysOf(fnBody(csiMqttCpp, "void publish_chirp_state(")),
    beacon: keysOf(fnBody(csiMqttCpp, "void publish_beacon_state(")),
    "update/state": [...new Set([...fnBody(ino, "static void ota_publish_update_state() {")
      .matchAll(/doc\["([a-z_]+)"\]/g)].map((m) => m[1]))],
  };
}
const OPTIONAL_KEYS = { health: ["sd_mounted", "enclosure_open"], "update/state": ["release_url", "release_summary"] };

test("every MQTT topic example is keyed as the firmware publishes it (sweep A32)", () => {
  const want = firmwareKeys();
  assert.deepStrictEqual(want.events.slice(-4), ["v", "alg", "fp", "sig"], "the wire body ends in the envelope");
  assert.ok(want.events.length >= 20, "csi_event_wire.h's body parsed thin: " + want.events);
  for (const t of data.mqtt.topics) {
    if (t.payload.startsWith('"')) continue;   // the bare ON/OFF and muted/live strings
    assert.ok(want[t.suffix], t.suffix + ": no firmware format to hold the example to");
    const keys = Object.keys(JSON.parse(t.payload));
    const opt = OPTIONAL_KEYS[t.suffix] || [];
    assert.deepStrictEqual(keys, want[t.suffix].filter((k) => !opt.includes(k) || keys.includes(k)),
      t.suffix + ": not the firmware's keys, in its order");
  }
  // the bare strings are csi_mqtt.cpp's own
  assert.ok(csiMqttCpp.includes('const char* pl = enabled ? "ON" : "OFF";'));
  assert.ok(csiMqttCpp.includes('const char* pl = muted ? "muted" : "live";'));
});

test("the events example is a row the WAP commits: its module, type, state and id space", () => {
  const ev = JSON.parse(data.mqtt.topics.find((t) => t.suffix === "events").payload);
  const presence = read(join(FW, "core_presence.cpp"));
  const states = presence.match(/const char\* STATE_NAMES\[STATE__COUNT\] = \{\s*([^}]*)\}/)[1].match(/"([a-z]+)"/g)
    .map((x) => x.slice(1, -1));
  assert.ok(presence.includes(`(void)csi_event_emit("${ev.module}", "${ev.type}", &v);`), ev.module + "/" + ev.type);
  assert.ok(states.includes(ev.state), `core.presence emits ${states}, not ${ev.state}`);
  assert.ok(!states.includes("motion"), "the old example's state is still not one");
  // csi_event_wire.h writes the state name as event_type too
  assert.ok(eventWireH.includes("    (unsigned long)event_id,\n    state_s,\n"));
  assert.strictEqual(ev.event_type, ev.state);
  assert.deepStrictEqual([ev.category, ev.privacy], ["event", "p0"], "a presence row is a P0 event");
  // ids start at kIdSpaceBase on every device (sweep F46)
  const base = Number(read(join(FW, "csi_event_id_floor.h")).match(/constexpr uint32_t kIdSpaceBase = (0x[0-9A-Fa-f]+)u;/)[1]);
  assert.ok(ev.event_id >= base && ev.event_id < 0xF0000000, `event_id ${ev.event_id} is not in [kIdSpaceBase, kHoldLimit)`);
  assert.strictEqual(ev.signed, true, "a body with the envelope says signed");
});

test("the health example is the FULL build's: an SD card mounted, no tamper contact", () => {
  const health = JSON.parse(data.mqtt.topics.find((t) => t.suffix === "health").payload);
  assert.strictEqual(health.sd_mounted, true, "the boot log mounts a card, so health carries sd_mounted");
  assert.ok(!("enclosure_open" in health), "FEATURE_TAMPER_GPIO is off in every shipped profile");
  assert.ok(ino.includes("tamper_lv.sd_mounted = (g_hw.sd_state != SD_ABSENT) ? 1 : 0;"));
  assert.ok(data.serial.boot.some((b) => b.text === "SD card ready for witness records"));
});

test("every HA discovery entity object_id is a real one", () => {
  assert.strictEqual(data.mqtt.discovery.entities.length, 24);
  for (const e of data.mqtt.discovery.entities)
    assert.ok(csiMqttCpp.includes('"' + e.object_id + '"'), "entity not in firmware: " + e.object_id);
  assert.strictEqual(data.mqtt.discovery.counts.entities, "24/24");
});

// ── 6. serial boot lines trace to a firmware source ────────────────────────
test("tagged boot lines exist in the firmware's serial output", () => {
  const sources = ino + csiMqttCpp + bootBannerCpp;
  const anchors = [
    "Camera ready for peek", "SD card ready for witness records",
    "Starting WiFi Access Point", "AP started:", "Pull-OTA engine ready",
  ];
  for (const a of anchors) assert.ok(sources.includes(a), "boot anchor drifted: " + a);
  assert.ok(bootBannerCpp.includes("The canary is singing. Everything is working."));
  assert.ok(data.serial.banner.some((l) => l.includes("SecuraCV Canary WAP")));
  assert.ok(data.serial.ready.some((l) => l.includes("The canary is singing")));
});

// ── 7. wizard labels + sensing pills trace to their sources ────────────────
test("every wizard step title is in companion_pwa.h", () => {
  for (const s of data.wizard.steps) assert.ok(companionH.includes(s.title), "wizard title drifted: " + s.title);
});

test("sensing pills are the getting-started guide's own", () => {
  for (const p of data.sensing.pills) assert.ok(doc.includes("**" + p.name + "**"), "pill drifted: " + p.name);
});

test("sandbox scenarios only publish to real topics", () => {
  const suffixes = new Set(data.mqtt.topics.map((t) => t.suffix));
  for (const sc of data.sandbox)
    for (const pub of sc.mqtt || [])
      assert.ok(suffixes.has(pub.suffix), "sandbox publishes unknown topic: " + pub.suffix);
});

// Sweep A30: a scene used to publish only the fields it changed — an events
// row with no v/alg/fp/sig, and chain {"length":+1}, which is not JSON and
// replaced the retained chain row — under a pane that says its payloads are
// csi_mqtt.cpp's exact strings. Each publish is now the topic's example with
// the scene's fields laid over it, and the page lays them over whatever the
// topic says by then.
test("every sandbox publish is its topic's payload with the scene's fields laid over it", async () => {
  const { scenePayload } = await import("../assets/wap-ui.js");
  const example = Object.fromEntries(data.mqtt.topics.map((t) => [t.suffix, t.payload]));
  const want = firmwareKeys();
  let n = 0;
  for (const sc of data.sandbox) {
    for (const pub of sc.mqtt) {
      const at = `${sc.id} → ${pub.suffix}`;
      if (!pub.set && !pub.advance) {
        // publish_mic_state writes the bare word, not a JSON string
        assert.strictEqual(pub.suffix, "mic/state", at);
        assert.ok(["muted", "live"].includes(pub.payload), `${at}: ${pub.payload}`);
        continue;
      }
      const base = JSON.parse(example[pub.suffix]);
      const got = JSON.parse(pub.payload);
      assert.deepStrictEqual(Object.keys(got), Object.keys(base), `${at}: every key of the topic, in its order`);
      assert.deepStrictEqual(Object.keys(got), want[pub.suffix].filter((k) => !(OPTIONAL_KEYS[pub.suffix] || []).includes(k) || k in got),
        `${at}: the firmware's keys`);
      // the first click is the overlay on the example; the page's own function agrees
      assert.strictEqual(scenePayload(base, pub), pub.payload, `${at}: wap.json and scenePayload disagree`);
      for (const k of pub.advance) assert.strictEqual(got[k], base[k] + 1, `${at}: ${k} moves on by one`);
      for (const [k, v] of Object.entries(pub.set)) assert.deepStrictEqual(got[k], v, `${at}: ${k}`);
      n++;
    }
  }
  assert.ok(n >= 15, "the scenes' publishes went missing");
});

test("every sandbox events row is one a WAP module commits; the WAP has no panic pad", () => {
  const presence = read(join(FW, "core_presence.cpp"));
  const states = presence.match(/const char\* STATE_NAMES\[STATE__COUNT\] = \{\s*([^}]*)\}/)[1].match(/"([a-z]+)"/g)
    .map((x) => x.slice(1, -1));
  const rows = data.sandbox.flatMap((sc) => sc.mqtt.filter((p) => p.suffix === "events").map((p) => JSON.parse(p.payload)));
  assert.ok(rows.length >= 3);
  for (const r of rows) {
    assert.deepStrictEqual([r.module, r.type, r.category, r.privacy], ["core.presence", "presence_changed", "event", "p0"]);
    assert.ok(states.includes(r.state) && r.event_type === r.state, r.state);
  }
  // a committed row is a witness record: counts and chain move together, in
  // the order the loop publishes them
  assert.ok(ino.includes("      csi_mqtt::publish_counts(g_health.records_created);\n" +
                         "      csi_mqtt::publish_chain(g_device.seq, g_device.chain_head);"));
  for (const sc of data.sandbox) {
    const order = sc.mqtt.map((p) => p.suffix);
    assert.ok(order.indexOf("counts") >= 0 && order.indexOf("counts") + 1 === order.indexOf("chain"), sc.id + ": " + order);
  }
  // silent_panic is firmware/canary's touch pad (securacv_touch); no WAP source emits it
  const sources = ["canary_wap.ino", "csi_mqtt.cpp", "core_presence.cpp", "acoustic_events_module.cpp",
    "tamper_events_module.cpp"].map((f) => read(join(FW, f))).join("\n");
  assert.ok(!sources.includes("silent_panic"));
  assert.ok(!JSON.stringify(data.sandbox).includes("silent_panic"), "a scene publishes an event no WAP emits");
});

test("the MQTT pane publishes each scene over the topic as it stands, retained by the topic's own flag", async () => {
  const { withFakeDom, fakeBus } = require("./fixtures/fake_dom.js");
  await withFakeDom(async () => {
    const { buildMqtt } = await import("../assets/wap-ui.js");
    const bus = fakeBus();
    const wrap = buildMqtt(data, bus);
    const id = data.device.id_example;
    const topic = (sfx) => `${data.mqtt.prefix}/${id}/${sfx}`;
    const stream = () => wrap.all("wap-mqtt-ev").map((r) => [r.children[0].textContent, r.children[1].textContent]);
    const retained = () => Object.fromEntries(wrap.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
    const scene = (sid) => data.sandbox.find((s) => s.id === sid);
    const ex = (sfx) => JSON.parse(data.mqtt.topics.find((t) => t.suffix === sfx).payload);

    bus.emit("event", scene("wave"));
    // the first click is wap.json's payload, verbatim
    const [ev1] = stream().filter(([t]) => t === topic("events"));
    assert.strictEqual(ev1[1], scene("wave").mqtt[0].payload);
    assert.strictEqual(retained()[topic("chain")], scene("wave").mqtt[2].payload, "chain is retained, whole");
    assert.ok(!(topic("events") in retained()), "events is not retained");

    bus.emit("event", scene("leave"));
    const evs = stream().filter(([t]) => t === topic("events")).map(([, p]) => JSON.parse(p));
    assert.strictEqual(evs.length, 2);
    assert.strictEqual(evs[0].event_id, ex("events").event_id + 2, "the next committed row takes the next id");
    assert.strictEqual(evs[0].state, "empty");
    assert.strictEqual(evs[0].motion, 2, "leave's own scores, not wave's");
    assert.deepStrictEqual(Object.keys(evs[0]), Object.keys(ex("events")));
    const chain = JSON.parse(retained()[topic("chain")]);
    const counts = JSON.parse(retained()[topic("counts")]);
    assert.strictEqual(chain.length, ex("chain").length + 2);
    assert.strictEqual(counts.total, ex("counts").total + 2);
    assert.strictEqual(chain.latest_hash, "…", "a moved head is not the example's hash");
    for (const k of ["v", "alg", "fp", "sig"]) assert.strictEqual(chain[k], ex("chain")[k], "the envelope rides along");

    bus.emit("event", scene("smoke"));
    bus.emit("event", scene("smoke"));
    const sensing = JSON.parse(retained()[topic("sensing")]);
    assert.strictEqual(sensing.acoustic_event, "smoke_alarm_t3");
    assert.strictEqual(sensing.t3_detected, ex("sensing").t3_detected + 2, "t3_detected counts detections");
    assert.deepStrictEqual(Object.keys(sensing), Object.keys(ex("sensing")));

    bus.emit("event", scene("mute"));
    assert.strictEqual(retained()[topic("mic/state")], "muted", "publish_mic_state's bare word");
    assert.strictEqual(JSON.parse(retained()[topic("chain")]).length, ex("chain").length + 5, "a mute is a witness record too");
  });
});

// The pane's "how to read this" (A30 review): the non-retained topics come
// from each topic's flag (the old note said only events, though tamper is
// live-only too), and it says what is elided and how the timing differs,
// instead of claiming the exact strings. The sandbox lede may not claim them.
test("the MQTT pane's note names every non-retained topic and what the sandbox stages", async () => {
  const { withFakeDom, fakeBus } = require("./fixtures/fake_dom.js");
  const live = data.mqtt.topics.filter((t) => !t.retained).map((t) => t.suffix);
  assert.deepStrictEqual(live, ["events", "tamper"]);
  assert.ok(csiMqttCpp.includes("publish_tamper_bridge"), "the tamper bridge");
  await withFakeDom(async () => {
    const { buildMqtt, paneNote } = await import("../assets/wap-ui.js");
    const wrap = buildMqtt(data, fakeBus());
    const note = wrap.all("wap-note")[0].textContent;
    assert.strictEqual(note, "How to read this: " + paneNote(data.mqtt, data.device.id_example));
    for (const sfx of live) assert.ok(note.includes(`${data.mqtt.prefix}/${data.device.id_example}/${sfx}`), sfx);
    assert.match(note, /are not retained, every other topic is/);
    assert.doesNotMatch(note, /exact strings|only [^ ]+ is non-retained/);
    for (const elided of ["sig", "public_key", "hash"]) assert.ok(note.includes(elided), elided);
    assert.match(note, /bundle closes/);
    assert.match(note, /acoustic\.events row/);
  });
  assert.doesNotMatch(read(join(ROOT, "assets/wap.js")), /exact MQTT/, "the sandbox lede");
});

// The sandbox's main path: tap a card on an offline bench. wap.js emits
// online, mqtt and the scene in one click, so the scene publishes before the
// retained snapshot (160 ms a topic) reaches chain and counts. The snapshot
// must land each topic as it then stands, not the example over the scene's row.
test("a scene clicked as the bench connects is not undone by the retained snapshot", async () => {
  const { withFakeDom, fakeBus } = require("./fixtures/fake_dom.js");
  await withFakeDom(async () => {
    const { buildMqtt } = await import("../assets/wap-ui.js");
    const bus = fakeBus();
    const wrap = buildMqtt(data, bus);
    const id = data.device.id_example;
    const topic = (sfx) => `${data.mqtt.prefix}/${id}/${sfx}`;
    const retained = () => Object.fromEntries(wrap.all("wap-mqtt-row").map((r) => [r.children[0].textContent, r.children[1].textContent]));
    const scene = (sid) => data.sandbox.find((s) => s.id === sid);
    const ex = (sfx) => JSON.parse(data.mqtt.topics.find((t) => t.suffix === sfx).payload);
    const nRetained = data.mqtt.topics.filter((t) => t.retained).length + 1; // + the LWT's online

    bus.emit("online"); bus.emit("mqtt"); bus.emit("event", scene("smoke"));
    assert.strictEqual(JSON.parse(retained()[topic("chain")]).length, ex("chain").length + 1);
    await new Promise((r) => setTimeout(r, 160 * nRetained + 400));
    assert.ok(retained()[topic("status")], "the snapshot landed");
    assert.strictEqual(JSON.parse(retained()[topic("chain")]).length, ex("chain").length + 1, "chain stays where the scene left it");
    assert.strictEqual(JSON.parse(retained()[topic("counts")]).total, ex("counts").total + 1, "counts too");
    const sensing = JSON.parse(retained()[topic("sensing")]);
    assert.strictEqual(sensing.acoustic_event, "smoke_alarm_t3", "the smoke row is not reverted to the example");
    assert.strictEqual(sensing.t3_detected, ex("sensing").t3_detected + 1);
    // untouched topics land their example
    assert.strictEqual(retained()[topic("mesh")], data.mqtt.topics.find((t) => t.suffix === "mesh").payload);

    bus.emit("event", scene("wave"));
    assert.strictEqual(JSON.parse(retained()[topic("chain")]).length, ex("chain").length + 2, "the next click is +2");
    assert.strictEqual(JSON.parse(retained()[topic("counts")]).total, ex("counts").total + 2);
  });
});

// ── 8. DOM-free cores (wap-ui.js) ──────────────────────────────────────────
test("withId substitutes the device id into templates", async () => {
  const { withId } = await import("../assets/wap-ui.js");
  assert.strictEqual(withId("securacv/<id>/status", "canary-x"), "securacv/canary-x/status");
  assert.strictEqual(withId("a/<device_id>/b", "z"), "a/z/b");
  assert.strictEqual(withId("no-vars", "z"), "no-vars");
});

test("bootLines flattens banner+boot+ready in order with mapped classes", async () => {
  const { bootLines } = await import("../assets/wap-ui.js");
  const lines = bootLines(data.serial);
  assert.strictEqual(lines.length,
    data.serial.banner.length + data.serial.boot.length + data.serial.ready.length);
  for (const l of lines) { assert.ok(typeof l.text === "string"); assert.ok(/^wap-/.test(l.cls)); }
  // an [OK] line becomes wap-ok; a [WIFI]/[MQTT] line becomes wap-net
  const okIdx = data.serial.boot.findIndex((s) => s.tag === "[OK]");
  assert.strictEqual(lines[data.serial.banner.length + okIdx].cls, "wap-ok");
});

test("mqttApply obeys retention (retained stored, events not, clear removes)", async () => {
  const { mqttApply } = await import("../assets/wap-ui.js");
  const store = {};
  mqttApply(store, { topic: "a/status", payload: "{on}", retain: true });
  assert.strictEqual(store["a/status"], "{on}");
  mqttApply(store, { topic: "a/events", payload: "{ev}", retain: false });
  assert.ok(!("a/events" in store), "non-retained event must not be stored");
  mqttApply(store, { topic: "a/status", clear: true });
  assert.ok(!("a/status" in store), "clear must remove");
});

test("a scene's pill is its own, one the page renders; a mute moves no pill", async () => {
  const { pillForScene } = await import("../assets/wap-ui.js");
  const known = new Set(data.sensing.pills.map((p) => p.name));
  for (const sc of data.sandbox) {
    const p = pillForScene(sc);
    if (sc.pill) assert.strictEqual(p, sc.pill, sc.id);
    if (p) assert.ok(known.has(p), sc.id + ": pill " + p + " is not one the page renders");
  }
  assert.strictEqual(pillForScene(data.sandbox.find((s) => s.id === "mute")), null);
  // the sit scene's row is the firmware's `quiet`; the guide's Quiet pill
  // means an empty room, so the pill must not follow the word
  const sit = data.sandbox.find((s) => s.id === "sit");
  assert.strictEqual(sit.event, "quiet");
  assert.strictEqual(pillForScene(sit), "Presence");
});

// A presence scene says one word on every surface a click moves: the console
// line, the event word and the events row's state, and its scores land in that
// state under core.presence's derive_target_state at the default (balanced)
// preset, read here from core_presence.cpp itself.
test("every presence scene's words are its row's state, and its scores land there", () => {
  const cp = read(join(FW, "core_presence.cpp"));
  const bal = cp.match(/default: base_motion = (\d+); base_active = (\d+); base_breathing = (\d+); break; \/\/ balanced/);
  assert.ok(bal, "core_presence.cpp's balanced preset");
  const [mThr, aThr, bThr] = bal.slice(1).map(Number);
  assert.ok(cp.includes('csi_module_settings_int(s, "core.presence.preset",      1);'), "balanced is the default preset");
  assert.ok(cp.includes('csi_module_settings_int(s, "core.presence.sensitivity", 50);'), "the slider defaults to neutral");
  const body = cp.slice(cp.indexOf("State derive_target_state("), cp.indexOf("void on_init("));
  const order = ["return STATE_TOGETHER", "return STATE_ACTIVE", "return STATE_QUIET", "if (motion >= s_motion_threshold) return STATE_SUBTLE", "return STATE_EMPTY"]
    .map((n) => body.indexOf(n));
  assert.ok(order.every((i, k) => i > 0 && (k === 0 || i > order[k - 1])), "derive_target_state's order: together, active, quiet, subtle, empty");
  const derive = (motion, breathing) =>
    motion >= Math.min(aThr + 20, 127) ? null // together or active, by the streak
      : motion >= aThr ? "active" : breathing >= bThr ? "quiet" : motion >= mThr ? "subtle" : "empty";
  let n = 0;
  for (const sc of data.sandbox) {
    const row = (sc.mqtt || []).find((p) => p.suffix === "events");
    if (!row) continue;
    const e = JSON.parse(row.payload);
    if (e.module !== "core.presence") continue;
    n++;
    assert.strictEqual(sc.event, e.state, sc.id + ": event word vs the row's state");
    assert.strictEqual(sc.serial, "witness record: state=" + e.state, sc.id + ": console line vs the row's state");
    assert.strictEqual(derive(e.motion, e.breathing), e.state,
      sc.id + ": motion " + e.motion + ", breathing " + e.breathing + " under " + [mThr, aThr, bThr]);
  }
  assert.ok(n >= 3, "the wave, sit and leave scenes publish presence rows");
  const ex = JSON.parse(data.mqtt.topics.find((t) => t.suffix === "events").payload);
  assert.strictEqual(derive(ex.motion, ex.breathing), ex.state, "the events example's scores land in its state");
});

// ── 5. the flash section — the bench skills can't go stale ─────────────────
const fwReadme = read(join(REPO, "firmware/projects/canary-wap/README.md"));
const makefile = read(join(REPO, "firmware/projects/canary-wap/Makefile"));
const pioIni = read(join(REPO, "firmware/projects/canary-wap/platformio.ini"));
const benchJs = read(join(ROOT, "emulator/web/bench.js"));

test("flash: both toolchains present, commands still real build machinery", () => {
  const f = data.flash;
  assert.ok(f, "wap.json has no flash section");
  assert.strictEqual(f.toolchains.length, 2);
  const pio = f.toolchains.find((t) => t.id === "platformio");
  const ard = f.toolchains.find((t) => t.id === "arduino");
  assert.ok(pio && ard);
  // every make command the page teaches is a real Makefile target
  for (const c of pio.commands) {
    if (c.cmd.startsWith("make ")) assert.ok(makefile.includes(c.cmd.split(" ")[1] + ":"), c.cmd);
  }
  assert.ok(pio.commands.some((c) => c.cmd === "make upload"), "make upload missing");
  assert.match(pioIni, /src_dir\s*=\s*arduino\/canary_wap/);
  // the Arduino path teaches exactly what the README says
  assert.ok(fwReadme.includes(ard.boards_url), "boards URL drifted from README");
  for (const [k, v] of ard.board_config) assert.ok(fwReadme.includes(`${k}: **${v}**`), `${k} drifted`);
  for (const lib of ard.libraries) assert.ok(fwReadme.includes(lib.split(" by ")[0]), lib);
  assert.ok(fwReadme.includes(ard.sketch));
});

test("flash: BOOT/RESET facts match the .ino constants and the ROM's own strings", () => {
  const f = data.flash;
  const boot = f.buttons.find((b) => b.id === "boot");
  const gpio = ino.match(/BOOT_BUTTON_GPIO\s*=\s*(\d+)/);
  assert.strictEqual(boot.gpio, Number(gpio[1]));
  const longMs = Number(ino.match(/BOOT_LONG_PRESS_MS\s*=\s*(\d+)/)[1]);
  assert.ok(boot.gestures.some((g) => g.includes(`>${longMs / 1000} s`)), "factory-reset hold drifted");
  // the download ritual ends in the mask ROM's real strap line (bench.js prints it)
  assert.ok(benchJs.includes("DOWNLOAD(USB/UART0)"));
  assert.ok(f.download_mode.rom_line.includes("DOWNLOAD(USB/UART0)"));
  const steps = f.download_mode.steps.join(" ").toLowerCase();
  assert.ok(steps.indexOf("hold boot") < steps.indexOf("reset"), "BOOT is held before RESET is tapped");
});

test("flash: troubleshooting is the README's own, none invented", () => {
  for (const t of data.flash.troubleshooting) {
    assert.ok(fwReadme.includes(t.symptom), "symptom not in README: " + t.symptom);
    assert.ok(t.fixes.length >= 1, t.symptom + " has no fixes");
  }
  assert.ok(data.flash.troubleshooting.some((t) => t.symptom.includes("not detected")),
    "the port-not-found flow (the frustrating one) must be taught");
});

// ── 6. the cable rig's DOM-free spine ──────────────────────────────────────
test("cable spine: trails away from the port and stays behind the connector", async () => {
  const { cablePoint, CABLE_BEZIER, WAP_PORT, WAP_LED } = await import("../assets/wap-ui.js");
  // t0 sits just behind the strain relief; t1 is the far, drooping end
  const p0 = cablePoint(0), p1 = cablePoint(1);
  assert.deepStrictEqual(p0, CABLE_BEZIER[0]);
  assert.ok(p1[2] < p0[2], "the lead trails backward (−Z, away from the port)");
  assert.ok(p1[1] < p0[1], "the lead droops downward");
  for (let i = 1; i <= 10; i++) { // the spine never doubles back toward the port
    assert.ok(cablePoint(i / 10)[2] <= cablePoint((i - 1) / 10)[2] + 1e-6);
  }
  // the port is on the base's −X wall; the light pipe is on the lid face —
  // both measured from the committed compact STLs (33.7 × 37.6 base)
  assert.ok(WAP_PORT[0] < -16 && WAP_PORT[0] > -17.5, "port off the −X wall");
  assert.ok(Math.abs(WAP_PORT[1]) < 0.01, "USB slot is y-centered (usb_w 10.5 notch)");
  assert.ok(WAP_LED[2] > 7, "light pipe sits on the lid face, toward the viewer");
});
