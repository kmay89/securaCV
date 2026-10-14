// Host test for the setup wizard's decisions (privacy_witness_kernel/wizard/
// index.html, the "setup-flow" section): which camera screen a preflight
// answer leads to, what the preflight checklist offers for a missing piece,
// what the save sends, and what the done screen says and asks for.
//
// The section is lifted out of the page and evaluated on its own (no DOM,
// no fetch), the way test_wizard_mesh_pairing.test.js lifts the mesh
// section. The page's markup is read beside it for the ids and copy the
// product review pinned down.
//
// Run: node --test privacy_witness_kernel/tests/test_wizard_setup_flow.test.js
// (CI: pwk-wizard-tests.yml.)

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const vm = require("node:vm");

const PAGE = process.env.WIZARD_PAGE || join(__dirname, "..", "wizard", "index.html");
const src = readFileSync(PAGE, "utf8");

function slice(text, from, to) {
  const a = text.indexOf(from);
  const b = a < 0 ? -1 : text.indexOf(to, a);
  assert.ok(a >= 0 && b > a, `marker not found: ${from}`);
  return text.slice(a, b);
}

function withoutScripts(text) {
  const lower = text.toLowerCase();
  let out = "";
  let at = 0;
  for (;;) {
    const open = lower.indexOf("<script", at);
    if (open < 0) break;
    out += text.slice(at, open);
    const close = lower.indexOf("</script", open);
    const end = close < 0 ? -1 : lower.indexOf(">", close);
    at = end < 0 ? text.length : end + 1;
  }
  return out + text.slice(at);
}

const SECTION = slice(src, "// setup-flow: begin", "// setup-flow: end");
// Values built inside the vm realm have that realm's Array/Object prototypes;
// deepStrictEqual compares prototypes, so compare their plain JSON shape.
const plain = (x) => JSON.parse(JSON.stringify(x));
const MARKUP = withoutScripts(src);
const IDS = new Set([...MARKUP.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]));

function flow() {
  const ctx = {
    escHtml: (s) => String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"),
  };
  vm.createContext(ctx);
  vm.runInContext(SECTION +
    "\n;globalThis.__t = { wizardPath, preflightView, frigateSummary, summaryRows, savePayload, donePlan, integrationStatusView };\n",
    ctx);
  return ctx.__t;
}

const LINKS = {
  mosquitto: "https://my.home-assistant.io/redirect/supervisor_addon/?addon=core_mosquitto",
  frigate: "https://my.home-assistant.io/redirect/supervisor_addon/?addon=ccab4aaf_frigate&repository_url=x",
  hacs: "https://my.home-assistant.io/redirect/hacs_repository/?owner=kmay89&repository=securacv-homeassistant&category=integration",
  integrations: "https://my.home-assistant.io/redirect/integrations/",
  dashboards: "https://my.home-assistant.io/redirect/lovelace_dashboards/",
};

const FRIGATE_RUNNING = {
  mosquitto: true, frigate: true, frigate_running: true, frigate_slug: "ccab4aaf_frigate",
  frigate_config_found: true, frigate_cameras_read: true,
  frigate_cameras: ["front_door", "driveway", "backyard"],
  mqtt_service: true, mqtt_host: "core-mosquitto", mqtt_port: 1883, mqtt_auth: true,
};

const FORM = {
  cameras: [{ id: 1, name: "Front Door", url: "rtsp://10.0.0.9/s", zone_id: "zone:front_door" }],
  retentionDays: 7, retentionLabel: "7 days (recommended)", seal: "events",
  digestEnabled: true, digestTime: "08:00",
};

// ---------------------------------------------------------------------------
// Which camera screen
// ---------------------------------------------------------------------------

test("a running Frigate with a readable config skips the RTSP screen and carries its cameras", () => {
  const t = flow();
  const plan = t.wizardPath(FRIGATE_RUNNING);
  assert.strictEqual(plan.path, "frigate");
  assert.deepStrictEqual(plain(plan.cameras), ["front_door", "driveway", "backyard"]);
  assert.strictEqual(plan.camerasRead, true);
  assert.strictEqual(plan.running, true);
  // The page's copy of the list is its own: mutating it leaves the checks alone.
  plan.cameras.push("x");
  assert.strictEqual(FRIGATE_RUNNING.frigate_cameras.length, 3);
});

test("an installed but stopped Frigate with a config still uses Frigate's list", () => {
  const t = flow();
  const plan = t.wizardPath(Object.assign({}, FRIGATE_RUNNING, { frigate_running: false }));
  assert.strictEqual(plan.path, "frigate");
  assert.strictEqual(plan.running, false);
});

test("no Frigate, or no readable Frigate config, is the RTSP screen", () => {
  const t = flow();
  assert.strictEqual(t.wizardPath({ frigate: false }).path, "rtsp");
  assert.strictEqual(t.wizardPath(Object.assign({}, FRIGATE_RUNNING, { frigate_config_found: false })).path, "rtsp");
  assert.strictEqual(t.wizardPath(null).path, "rtsp");
  assert.deepStrictEqual(plain(t.wizardPath({}).cameras), []);
});

test("a Frigate whose config has no readable camera list means every camera Frigate reports", () => {
  const t = flow();
  const plan = t.wizardPath(Object.assign({}, FRIGATE_RUNNING, { frigate_cameras_read: false, frigate_cameras: [] }));
  assert.strictEqual(plan.path, "frigate");
  assert.deepStrictEqual(plain(plan.cameras), []);
  const summary = t.frigateSummary(plan, FRIGATE_RUNNING);
  assert.match(summary, /could not read/);
  assert.match(summary, /SecuraCV will seal every person, car, dog and cat event Frigate reports\./);
});

test("the Frigate confirmation reads like the reviewed sentence", () => {
  const t = flow();
  const plan = t.wizardPath(FRIGATE_RUNNING);
  const summary = t.frigateSummary(plan, FRIGATE_RUNNING);
  assert.match(summary, /^Frigate is running with 3 cameras — front_door, driveway, backyard, and Mosquitto is connected\. Nothing to type\./);
  assert.match(summary, /seal every person, car, dog and cat event Frigate reports on them/);
});

// ---------------------------------------------------------------------------
// The preflight checklist: links, not Supervisor installs
// ---------------------------------------------------------------------------

test("a missing Mosquitto or Frigate is offered as a my-HA link, never an install call", () => {
  const t = flow();
  const view = t.preflightView({ mosquitto: false, frigate: false, mqtt_service: false }, LINKS);
  assert.strictEqual(view.ready, false);
  assert.strictEqual(view.title, "Prerequisites need attention");
  assert.match(view.html, new RegExp(`href="${LINKS.mosquitto.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")}"`));
  assert.match(view.html, /Install Mosquitto</);
  assert.match(view.html, /Install Frigate</);
  assert.match(view.html, /target="_blank" rel="noopener"/);
  assert.doesNotMatch(view.html, /installPrereq|preflight\/install|for me/);
});

test("everything present reads ready and names Frigate's cameras", () => {
  const t = flow();
  const view = t.preflightView(FRIGATE_RUNNING, LINKS);
  assert.strictEqual(view.ready, true);
  assert.strictEqual(view.title, "Prerequisites ready");
  assert.match(view.html, /✓ Frigate NVR is running with 3 cameras — front_door, driveway, backyard/);
  assert.match(view.html, /✓ Mosquitto MQTT broker is running/);
  assert.match(view.html, /auto-discovered \(core-mosquitto:1883, authenticated\)/);
  assert.doesNotMatch(view.html, /href=/);
});

test("the checklist escapes what the server reports", () => {
  const t = flow();
  const view = t.preflightView(Object.assign({}, FRIGATE_RUNNING, {
    frigate_cameras: ["<img src=x onerror=alert(1)>"], mqtt_host: "a\"b",
  }), LINKS);
  assert.doesNotMatch(view.html, /<img/);
  assert.match(view.html, /&lt;img/);
  assert.match(view.html, /a&quot;b/);
});

// ---------------------------------------------------------------------------
// The save
// ---------------------------------------------------------------------------

test("a Frigate-path save names Frigate's cameras and nothing to write for Frigate", () => {
  const t = flow();
  const plan = t.wizardPath(FRIGATE_RUNNING);
  const payload = t.savePayload(FORM, plan, { reconfigure: false, deviceKeySeed: "ab".repeat(32) });
  assert.strictEqual(payload.mode, "frigate");
  assert.strictEqual(payload.frigate_detected, true);
  assert.deepStrictEqual(plain(payload.cameras), [{ name: "front_door" }, { name: "driveway" }, { name: "backyard" }]);
  assert.strictEqual(payload.retention_days, 7);
  assert.strictEqual(payload.enable_reviews, false);
  assert.strictEqual(payload.digest_enabled, true);
  assert.strictEqual(payload.digest_time, "08:00");
  assert.strictEqual(payload.device_key_seed, "ab".repeat(32));
  // The toggles nothing consumed are gone from the wire.
  assert.ok(!("pattern_alerts" in payload) && !("integrity_alerts" in payload));
});

test("an RTSP-path save carries the typed cameras with their URLs", () => {
  const t = flow();
  const plan = t.wizardPath({ frigate: false });
  const payload = t.savePayload(Object.assign({}, FORM, { seal: "reviews", retentionDays: 30 }), plan,
    { reconfigure: false, deviceKeySeed: "cd".repeat(32) });
  assert.strictEqual(payload.frigate_detected, false);
  assert.deepStrictEqual(plain(payload.cameras), [{ name: "Front Door", url: "rtsp://10.0.0.9/s", zone_id: "zone:front_door" }]);
  assert.strictEqual(payload.enable_reviews, true);
  assert.strictEqual(payload.retention_days, 30);
});

test("a re-run sends no device key: the existing identity is kept", () => {
  const t = flow();
  const plan = t.wizardPath(FRIGATE_RUNNING);
  const payload = t.savePayload(FORM, plan, { reconfigure: true, deviceKeySeed: "ab".repeat(32) });
  assert.ok(!("device_key_seed" in payload), "no seed on a re-run");
  assert.ok(!("current_device_key_seed" in payload), "the page never holds the current seed");
});

test("the summary names the cameras, the retention, what to seal and the digest", () => {
  const t = flow();
  const rows = t.summaryRows(FORM, t.wizardPath(FRIGATE_RUNNING));
  const by = Object.fromEntries(rows);
  assert.strictEqual(by["Cameras"], "front_door, driveway, backyard");
  assert.strictEqual(by["Keep sealed events for"], "7 days (recommended)");
  assert.strictEqual(by["What to seal"], "Every tracked object");
  assert.strictEqual(by["Daily digest"], "08:00");
  assert.ok(!("Clip retention" in by) && !("Pattern-break alerts" in by) && !("Integrity failure alerts" in by));
  const none = Object.fromEntries(t.summaryRows(FORM, t.wizardPath(Object.assign({}, FRIGATE_RUNNING, { frigate_cameras: [] }))));
  assert.strictEqual(none["Cameras"], "every camera Frigate reports");
});

// ---------------------------------------------------------------------------
// The done screen
// ---------------------------------------------------------------------------

const SAVE_OK = {
  ok: true, frigate_detected: true,
  frigate_config: "Frigate keeps its own camera config — nothing was written to it. SecuraCV seals the events of: front_door, driveway, backyard.",
  digest: { created: true, detail: "daily digest at 08:00 via notify.mobile_app_pixel" },
  cameras: ["front_door", "driveway", "backyard"],
};

function status(integration) {
  return { ok: true, integration, links: LINKS };
}

test("with install_integration on and nothing installed, the primary action installs and restarts", () => {
  const t = flow();
  const d = t.donePlan(SAVE_OK, status({ state: "absent", version: null, carried_version: "0.9.0",
                                          install_integration: true, loaded: false, hacs_url: LINKS.hacs }));
  assert.strictEqual(d.primary.kind, "install");
  assert.strictEqual(d.primary.label, "Install the integration and restart Home Assistant");
  assert.ok(d.lines.some((l) => l.includes("Frigate keeps its own camera config")));
  assert.ok(d.lines.some((l) => l.startsWith("✓ Daily digest automation created")));
  assert.ok(d.lines.some((l) => l.includes("ready to install from this app")));
  assert.ok(d.after.some((l) => l.includes("After the restart: Settings → Devices & Services → \"SecuraCV discovered\" → Configure")));
  assert.ok(d.after.some((l) => l.includes("Settings → Dashboards → Add dashboard → SecuraCV")));
  assert.ok(d.after.some((l) => l.includes("first sealed event appears within a minute")));
  for (const line of [...d.lines, ...d.after]) {
    assert.doesNotMatch(line, /appear(s)? automatically via MQTT discovery/);
  }
});

test("with install_integration off, the primary action is the HACS link", () => {
  const t = flow();
  const d = t.donePlan(SAVE_OK, status({ state: "absent", version: null, carried_version: "0.9.0",
                                          install_integration: false, loaded: false, hacs_url: LINKS.hacs }));
  assert.strictEqual(d.primary.kind, "hacs");
  assert.strictEqual(d.primary.label, "Install the SecuraCV integration from HACS");
  assert.strictEqual(d.primary.href, LINKS.hacs);
  assert.ok(d.lines.some((l) => l.includes("install_integration is off")));
});

test("an installed but unloaded integration asks for the one restart", () => {
  const t = flow();
  for (const state of ["managed", "foreign", "modified"]) {
    const d = t.donePlan(SAVE_OK, status({ state, version: "0.9.0", carried_version: "0.9.0",
                                            install_integration: true, loaded: false, hacs_url: LINKS.hacs }));
    assert.strictEqual(d.primary.kind, "restart", state);
    assert.strictEqual(d.primary.label, "Restart Home Assistant to load the SecuraCV integration");
  }
  const foreign = t.donePlan(SAVE_OK, status({ state: "foreign", version: "0.9.0", install_integration: true, loaded: false }));
  assert.ok(foreign.lines.some((l) => l.includes("by HACS or by hand")));
});

test("a loaded integration needs no restart: the next step is the discovered card", () => {
  const t = flow();
  const d = t.donePlan(SAVE_OK, status({ state: "foreign", version: "0.9.0", carried_version: "0.9.0",
                                          install_integration: true, loaded: true, hacs_url: LINKS.hacs }));
  assert.strictEqual(d.primary.kind, "integrations");
  assert.strictEqual(d.primary.href, LINKS.integrations);
  assert.ok(d.after[0].includes("\"SecuraCV discovered\" card is there"));
  assert.ok(d.after[0].includes("Press Configure — nothing to type"));
});

test("a digest that was skipped is said to be skipped, with the reason", () => {
  const t = flow();
  const d = t.donePlan(Object.assign({}, SAVE_OK, { digest: { created: false, detail: "no mobile-app notify service found" } }),
    status({ state: "absent", install_integration: true, loaded: false }));
  assert.ok(d.lines.some((l) => l === "Daily digest was NOT set up: no mobile-app notify service found"));
  const off = t.donePlan(Object.assign({}, SAVE_OK, { digest: { created: false, detail: "digest disabled in the wizard" } }),
    status({ state: "absent", install_integration: true, loaded: false }));
  assert.ok(off.lines.some((l) => l === "Daily digest: disabled (your choice in Settings)."));
});

test("the done screen survives a missing status answer", () => {
  const t = flow();
  const d = t.donePlan(SAVE_OK, null);
  assert.strictEqual(d.primary.kind, "hacs");
  assert.ok(d.after.length >= 3);
});

// ---------------------------------------------------------------------------
// The status panel's integration box
// ---------------------------------------------------------------------------

test("the status panel offers install, restart, HACS or nothing as the state demands", () => {
  const t = flow();
  assert.strictEqual(t.integrationStatusView({ state: "absent", install_integration: true }, LINKS).action, "install");
  const hacs = t.integrationStatusView({ state: "absent", install_integration: false, hacs_url: LINKS.hacs }, LINKS);
  assert.strictEqual(hacs.action, "hacs");
  assert.strictEqual(hacs.href, LINKS.hacs);
  const unloaded = t.integrationStatusView({ state: "managed", version: "0.9.0", loaded: false }, LINKS);
  assert.strictEqual(unloaded.action, "restart");
  assert.match(unloaded.text, /placed by this app/);
  const loaded = t.integrationStatusView({ state: "foreign", version: "0.9.0", loaded: true }, LINKS);
  assert.strictEqual(loaded.action, "none");
  assert.match(loaded.text, /HACS updates it/);
  const edited = t.integrationStatusView({ state: "modified", version: "0.9.0", loaded: null }, LINKS);
  assert.strictEqual(edited.action, "none");
  assert.match(edited.text, /leaves it alone/);
});

// ---------------------------------------------------------------------------
// The markup: what the screens show and no longer show
// ---------------------------------------------------------------------------

test("the settings screen has the witness-log retention and the what-to-seal choice, not the retired toggles", () => {
  for (const id of ["retention", "seal-events", "seal-reviews", "digest-enabled", "digest-time"]) {
    assert.ok(IDS.has(id), `element ${id}`);
  }
  for (const id of ["pattern-alerts", "integrity-alerts"]) {
    assert.ok(!IDS.has(id), `${id} is gone`);
  }
  const settings = slice(MARKUP, '<div class="card hidden" id="screen-3">', '<div class="card hidden" id="screen-4">');
  assert.match(settings, /Keep sealed events for/);
  assert.match(settings, /<option value="7" selected>7 days/);
  assert.match(settings, /<option value="30">30 days/);
  assert.match(settings, /<option value="365">1 year/);
  assert.match(settings, /Every tracked object \(default\)/);
  assert.match(settings, /Only Frigate alerts/);
  assert.doesNotMatch(settings, /Clip retention|kept forever/);
});

test("the camera screens say to name cameras by place, not by person", () => {
  const cameras = slice(MARKUP, '<div class="card hidden" id="screen-2">', '<div class="card hidden" id="screen-3">');
  assert.match(cameras, /named by place, not by\s+person/);
  assert.match(cameras, /Name cameras by place/);
  assert.ok(IDS.has("frigate-cameras") && IDS.has("rtsp-cameras") && IDS.has("frigate-camera-list"));
});

test("the pitch is the witness log, not a camera with a memory", () => {
  const welcome = slice(MARKUP, '<div class="card" id="screen-1">', '<div class="card hidden" id="screen-2">');
  assert.match(welcome, /The verified witness log for Frigate\./);
  assert.match(welcome, /Frigate keeps the video; SecuraCV keeps the proof/);
  assert.match(welcome, /never faces, plates or footage/);
  assert.doesNotMatch(welcome, /24-hour memory/);
});

test("the done screen builds its one action from the server's state; no fixed restart button, no MQTT-discovery promise", () => {
  const done = slice(MARKUP, '<div class="card hidden" id="screen-5">', '<div class="card hidden" id="screen-mesh">');
  assert.ok(IDS.has("done-primary") && IDS.has("done-after") && IDS.has("done-results"));
  assert.doesNotMatch(done, /Restart Home Assistant Now/);
  assert.doesNotMatch(done, /appear automatically via MQTT discovery/);
  assert.doesNotMatch(done, /Restart Home Assistant to activate/);
});

test("the status panel has no Lovelace YAML generator; the strategy dashboard replaced it", () => {
  assert.ok(!IDS.has("dashboard-yaml") && !IDS.has("dashboard-box"));
  assert.doesNotMatch(src, /generateDashboard|copyDashboard|zoneEntityId/);
  assert.ok(IDS.has("status-integration"));
});

test("the page makes no Supervisor install call and keeps the key off the wire on re-runs", () => {
  assert.doesNotMatch(src, /api\/preflight\/install|installPrereq/);
  assert.match(src, /api\/integration\/install/);
  assert.match(src, /api\/restart-ha/);
});
