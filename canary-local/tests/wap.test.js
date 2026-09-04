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

test("pillForEvent maps CSI events to the right presence pill", async () => {
  const { pillForEvent } = await import("../assets/wap-ui.js");
  assert.strictEqual(pillForEvent("motion"), "Motion");
  assert.strictEqual(pillForEvent("empty"), "Quiet");
  assert.strictEqual(pillForEvent("subtle"), "Presence");
  assert.strictEqual(pillForEvent("mic_mute"), null);
  // every pill it can return is one the page knows how to render
  const known = new Set(data.sensing.pills.map((p) => p.name));
  for (const ev of ["motion", "subtle", "empty", "active", "present"]) {
    const p = pillForEvent(ev);
    if (p) assert.ok(known.has(p), "pillForEvent returned unknown pill: " + p);
  }
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
