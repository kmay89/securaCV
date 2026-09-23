// Host test for firmware/canary's setup wizard page
// (canary/lib/securacv_webui/src/securacv_setup_page.cpp): the hub step's
// broker-TLS controls, driven exactly as a browser would drive them.
//
// The page is reachable at canary.local/setup for the life of the device, and
// the API it talks to was built so that a `tls` field the body does not name
// leaves the stored mode alone (test_mqtt_tls_fields.cpp,
// absent_fields_write_nothing_and_keep_the_current_decision). The review of
// the first cut found the wizard defeating that on the one UI the product
// has: it sent `tls:<select>` on every save with Plain preselected and never
// read /api/mqtt/status, so re-running the page to change a hub password
// silently downgraded a TLS unit to plain. This pins the fix from the
// outside: the script is lifted out of the C++ raw string and run against a
// stub DOM + a recording fetch, and the body that reaches
// POST /api/mqtt/config is what is asserted on.
//
// Run: node --test firmware/tests_host/test_canary_setup_page.test.js
// (the Makefile's `run` target does, after the C++ suites).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const PAGE = join(__dirname, "..", "canary", "lib", "securacv_webui", "src", "securacv_setup_page.cpp");
const src = readFileSync(PAGE, "utf8");

const html = (() => {
  const m = src.match(/R"HTML\(([\s\S]*?)\)HTML"/);
  assert.ok(m, "CANARY_SETUP_HTML raw string not found");
  return m[1];
})();
// The page has exactly one inline script block; lift it out by its literal
// markers (no HTML-filtering regex: this is the firmware's own raw string
// being read for execution under the stub DOM, not untrusted markup being
// sanitized — CodeQL reads a <script> regex as the latter).
const script = (() => {
  const open = html.indexOf("<script>");
  const close = open < 0 ? -1 : html.indexOf("</script>", open);
  assert.ok(open >= 0 && close > open, "the page's <script> not found");
  assert.strictEqual(html.indexOf("<script", open + 1), -1, "the page has one script block");
  return html.slice(open + "<script>".length, close);
})();

// ── a stub DOM seeded from the markup ───────────────────────────────────────
// Every element the script reaches for by id starts with the value the
// markup gives it (an <input value="…">, the <select>'s selected option), so
// the test exercises the page's real defaults, not the stub's.
function seedValues() {
  const values = {};
  for (const tag of html.matchAll(/<(?:input|textarea|select)\b[^>]*>/g)) {
    const id = tag[0].match(/\bid="([^"]+)"/);
    if (!id) continue;
    const v = tag[0].match(/\bvalue="([^"]*)"/);
    values[id[1]] = v ? v[1] : "";
  }
  const sel = html.match(/<select[^>]*\bid="mtls"[^>]*>([\s\S]*?)<\/select>/);
  assert.ok(sel, "the Encryption select is in the markup");
  const chosen = sel[1].match(/<option value="(\d)" selected>/);
  assert.ok(chosen, "the Encryption select has a preselected option");
  values.mtls = chosen[1];
  return values;
}

function makeElement(id, value) {
  const el = {
    id,
    value: value === undefined ? "" : value,
    textContent: "",
    disabled: false,
    style: {},
    listeners: {},
    children: [],
    classes: new Set(),
    addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); },
    fire(type, evt) { for (const fn of this.listeners[type] || []) fn.call(this, evt || {}); },
    focus() {},
    appendChild(kid) { this.children.push(kid); },
    querySelector() { return { textContent: "" }; },
    get firstChild() { return { textContent: "" }; },
  };
  el.classList = {
    contains: (c) => el.classes.has(c),
    add: (c) => el.classes.add(c),
    remove: (c) => el.classes.delete(c),
  };
  Object.defineProperty(el, "innerHTML", { set() { el.children = []; }, get() { return ""; } });
  return el;
}

// Run the page's script once against canned API answers. `routes` maps a
// "METHOD /path" to either a JSON value or an Error (a route the firmware
// does not have: r.json() rejects the way a 404 HTML body would).
async function boot(routes, opts) {
  const values = seedValues();
  const els = new Map();
  const document = {
    getElementById(id) {
      if (!els.has(id)) els.set(id, makeElement(id, values[id]));
      return els.get(id);
    },
    createElement(tag) { return makeElement("<" + tag + ">"); },
  };
  const calls = [];
  const fetch = (url, opts) => {
    const method = (opts && opts.method) || "GET";
    const key = method + " " + url;
    calls.push({ key, url, method, headers: (opts && opts.headers) || {}, body: opts && opts.body });
    const answer = routes[key];
    return Promise.resolve({
      json: () => (answer instanceof Error ? Promise.reject(answer) : Promise.resolve(answer)),
    });
  };
  // eslint-disable-next-line no-new-func
  // `Intl` is the host's unless a test swaps it (the time zone seed, F28).
  new Function("document", "fetch", "setInterval", "clearInterval", "Intl", script)(
    document, fetch, () => 0, () => {}, (opts && opts.Intl) || Intl);
  await settle();
  return { $: (id) => document.getElementById(id), calls, values };
}

// Drain the promise chains the script builds (fetch → json → then → then).
async function settle() {
  for (let i = 0; i < 8; i++) await new Promise((r) => setImmediate(r));
}

const configBody = (calls) => {
  const c = calls.filter((x) => x.key === "POST /api/mqtt/config");
  assert.strictEqual(c.length, 1, "exactly one POST /api/mqtt/config, got " + c.map((x) => x.key));
  return JSON.parse(c[0].body);
};

const onWifi = { ok: true, sta_connected: true, sta_ip: "192.168.1.42" };
const saved = { ok: true, transport: "tls-fingerprint" };

// ── the markup and the script agree ─────────────────────────────────────────
test("every id the script reaches for exists in the markup", () => {
  const ids = new Set([...script.matchAll(/\$\('([a-z0-9]+)'\)/g)].map((m) => m[1]));
  assert.ok(ids.size >= 20, "the script names its elements by id");
  for (const id of ids) assert.ok(html.includes('id="' + id + '"'), "no element with id=" + id);
  for (const id of ["mtls", "mca", "mfp", "mcakept", "mfpkept", "mportnudge"]) assert.ok(ids.has(id), "the script drives #" + id);
});

test("the Encryption select's default is Plain, mode 0, and the four modes are the firmware's", () => {
  const values = seedValues();
  assert.strictEqual(values.mtls, "0");
  const sel = html.match(/<select[^>]*\bid="mtls"[^>]*>([\s\S]*?)<\/select>/)[1];
  assert.deepStrictEqual([...sel.matchAll(/<option value="(\d)"/g)].map((m) => m[1]), ["0", "1", "2", "3"]);
});

// ── the finding: a re-save must not flip a TLS unit to plain ────────────────
test("once on Wi-Fi, the wizard reads /api/mqtt/status and pre-sets host, port and the mode from it", async () => {
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": { ok: true, connected: true, host: "hub.lan", port: 8883, tls: "tls-fingerprint", tls_mode: 2, transport: "tls-fingerprint", ca_set: false, fp_set: true },
  });
  assert.ok(calls.some((c) => c.key === "GET /api/mqtt/status"), "the page asked what the Canary holds");
  assert.strictEqual($("mhost").value, "hub.lan");
  assert.strictEqual($("mport").value, "8883");
  assert.strictEqual($("mtls").value, "2", "the select shows the stored mode, not the markup's Plain");
  assert.strictEqual($("mfpbox").style.display, "block", "the field the stored mode uses is shown");
  assert.strictEqual($("mfpkept").style.display, "block", "and says a pin is already held");
  assert.strictEqual($("mportnudge").style.display, "none", "8883 needs no nudge");
});

test("changing only the hub password on a pinned unit sends no tls and no fp: the stored mode and pin stand", async () => {
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": { ok: true, host: "hub.lan", port: 8883, tls_mode: 2, ca_set: false, fp_set: true },
    "POST /api/mqtt/config": saved,
  });
  $("mpass").value = "new-hub-password";
  $("mqttsave").fire("click");
  await settle();
  const body = configBody(calls);
  assert.strictEqual(body.host, "hub.lan");
  assert.strictEqual(body.port, 8883);
  assert.strictEqual(body.password, "new-hub-password");
  assert.ok(!("tls" in body), "an untouched select sends no tls — the first cut sent tls:0 here and downgraded the unit");
  assert.ok(!("fp" in body), "an empty fingerprint field on a unit that holds a pin sends no fp");
  assert.ok(!calls.some((c) => c.key === "POST /api/mqtt/ca"), "no CA trip for a pinned unit");
  assert.match($("mqttmsg").textContent, /^Saved/, "the save went through: " + $("mqttmsg").textContent);
});

test("when /api/mqtt/status cannot be read, an untouched select still sends no tls (never a blind Plain)", async () => {
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": new Error("404 text/html"),
    "POST /api/mqtt/config": { ok: true, transport: "tls-ca" },
  });
  assert.strictEqual($("mtls").value, "0", "the markup's default stays");
  $("mpass").value = "new-hub-password";
  $("mqttsave").fire("click");
  await settle();
  assert.ok(!("tls" in configBody(calls)), "no tls in the body: the Canary keeps whatever mode it holds");
});

test("a mode the person chose IS sent — Plain included", async () => {
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": { ok: true, host: "hub.lan", port: 8883, tls_mode: 2, fp_set: true },
    "POST /api/mqtt/config": { ok: true, transport: "plain" },
  });
  $("mtls").value = "0";
  $("mtls").fire("change");
  $("mqttsave").fire("click");
  await settle();
  assert.strictEqual(configBody(calls).tls, 0, "Plain, chosen on purpose, travels");
});

test("a fresh unit set to CA mode: the PEM travels on its own route first, then the config with tls:1", async () => {
  const pem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----";
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": { ok: true, tls_mode: 0, ca_set: false, fp_set: false },
    "POST /api/mqtt/ca": { ok: true, ca_set: true },
    "POST /api/mqtt/config": { ok: true, transport: "tls-ca" },
  });
  $("mtls").value = "1";
  $("mtls").fire("change");
  assert.strictEqual($("mcabox").style.display, "block");
  assert.strictEqual($("mcakept").style.display, "none", "nothing is held yet");
  assert.strictEqual($("mportnudge").style.display, "block", "TLS on 1883 gets the 8883 suggestion");
  $("use8883").fire("click");
  $("mqttsave").fire("click");
  await settle();
  assert.match($("mqttmsg").textContent, /Paste the broker’s CA certificate first/, "no CA, no save");
  $("mca").value = pem;
  $("mqttsave").fire("click");
  await settle();
  const order = calls.filter((c) => c.method === "POST").map((c) => c.key);
  assert.deepStrictEqual(order, ["POST /api/mqtt/ca", "POST /api/mqtt/config"], "the CA lands before the mode that needs it");
  assert.strictEqual(calls.find((c) => c.key === "POST /api/mqtt/ca").body, pem, "the PEM is the raw body");
  const body = configBody(calls);
  assert.strictEqual(body.tls, 1);
  assert.strictEqual(body.port, 8883);
  assert.strictEqual($("mcakept").style.display, "block", "after the save the page knows a CA is held");
});

test("a unit that already holds a CA, re-saved in CA mode with the box empty, skips the CA trip and keeps tls out of the body", async () => {
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": { ok: true, host: "hub.lan", port: 8883, tls_mode: 1, ca_set: true, fp_set: false },
    "POST /api/mqtt/config": { ok: true, transport: "tls-ca" },
  });
  assert.strictEqual($("mcakept").style.display, "block");
  $("muser").value = "canary";
  $("mqttsave").fire("click");
  await settle();
  assert.ok(!calls.some((c) => c.key === "POST /api/mqtt/ca"), "the held CA stands in for the empty box");
  const body = configBody(calls);
  assert.ok(!("tls" in body));
  assert.strictEqual(body.username, "canary");
});

test("the API's refusal text is shown, never a secret, and a refused save leaves the select where the person put it", async () => {
  const { $, calls } = await boot({
    "GET /api/wifi/status": onWifi,
    "GET /api/mqtt/status": { ok: true, tls_mode: 0 },
    "POST /api/mqtt/config": { ok: false, error: "tls_refused", reason: "broker TLS mode is fingerprint but no pin is stored" },
  });
  $("mtls").value = "2";
  $("mtls").fire("change");
  $("mfp").value = "not-a-pin";
  $("mqttsave").fire("click");
  await settle();
  assert.strictEqual(configBody(calls).fp, "not-a-pin", "the page does not judge the pin; the firmware does");
  assert.match($("mqttmsg").textContent, /Couldn’t save: broker TLS mode is fingerprint but no pin is stored/);
  assert.ok(!$("mqttmsg").textContent.includes("not-a-pin"), "the message is the API's text, not an echo");
  assert.strictEqual($("mtls").value, "2");
});

test("the success line and the Restart button no longer contradict each other", () => {
  assert.match(script, /the hub link needs no restart/);
  assert.match(script, /Restart only when you are done here/);
});

// ── the household time zone seed (repo sweep F28) ──────────────────────────
// The join carries the phone's own IANA zone as tz_iana, beside — never in
// place of — the credentials; a browser that cannot tell sends no field.
async function joinBody(opts) {
  const { $, calls } = await boot({
    "GET /api/wifi/scan": { networks: [] },
    "POST /api/wifi/connect": { ok: true },
  }, opts);
  $("ssid").value = "Home";
  $("ssid").fire("input");
  $("pass").value = "hunter2";
  $("join").fire("click");
  await settle();
  const c = calls.filter((x) => x.key === "POST /api/wifi/connect");
  assert.strictEqual(c.length, 1, "exactly one join POST");
  return JSON.parse(c[0].body);
}

test("the join sends the phone's time zone as tz_iana with the credentials", async () => {
  const fakeIntl = { DateTimeFormat: () => ({ resolvedOptions: () => ({ timeZone: "Europe/Berlin" }) }) };
  const body = await joinBody({ Intl: fakeIntl });
  assert.strictEqual(body.ssid, "Home");
  assert.strictEqual(body.password, "hunter2");
  assert.strictEqual(body.tz_iana, "Europe/Berlin");
});

test("a browser that cannot name its zone sends no tz_iana, and the join still goes", async () => {
  for (const intl of [
    { DateTimeFormat: () => ({ resolvedOptions: () => ({}) }) },
    { DateTimeFormat: () => { throw new Error("no Intl"); } },
    { DateTimeFormat: () => ({ resolvedOptions: () => ({ timeZone: "X".repeat(48) }) }) },
  ]) {
    const body = await joinBody({ Intl: intl });
    assert.deepStrictEqual(Object.keys(body).sort(), ["password", "ssid"]);
  }
});
