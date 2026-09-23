// canary-local/tests/onboard.test.js — the display's first-boot portal in the
// emulator, pinned to the firmware it runs.
//
// The emulator compiles canary-display's net/provision.cpp verbatim; the fleet
// page's phone (assets/onboard-phone.js) walks it through the shims in
// emulator/src/emu_radio.cpp + emu_webserver.cpp. What can rot, and where:
//   · the portal page moves and the CSP-pinned copy does not  → "generated data"
//   · the Lab's srcdoc transform drifts from the generator's  → "stripPortal"
//   · the phone builds a /join body the portal would not       → "joinBody"
//   · the zone picker / bars stop matching the portal's script → "zone presets", "bars"
//   · the phone's walk accepts an impossible step              → "phoneStep"
//   · a provisioned boot drops into onboarding (key typo)      → "preseed keys"
//   · the build stops compiling the real portal                → "build compiles"
//   · the DNS plumbing misreads the firmware's reply shape     → "DNS"
//   · the gates fall out of CI                                 → "CI runs"
//
// The browser half (the real wasm answering) is tests/onboard_probe.mjs, in
// canary-local.yml's wasm job. Node only; reads source text.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");
const { spawnSync } = require("node:child_process");
const { createHash } = require("node:crypto");

const ROOT = join(__dirname, "..");      // canary-local/
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");
const PROVISION = read(join(REPO, "firmware/projects/canary-display/src/net/provision.cpp"));
const PORTAL_SRC = /PORTAL_HTML\[\]\s+PROGMEM\s*=\s*R"HTML\(([\s\S]*?)\)HTML"/.exec(PROVISION)[1];
const PORTAL_SCRIPT = /<script\b[^>]*>([\s\S]*?)<\/script\s*>/i.exec(PORTAL_SRC)[1];
const DATA = JSON.parse(read(join(ROOT, "devices/display_portal.json")));

const phone = () => import("../assets/onboard-phone.js");
const shell = () => import("../emulator/web/emu-shell.js");

test("generated data: gen_display_portal.py --check passes against the firmware", () => {
  const r = spawnSync("python3", [join(ROOT, "tools/gen_display_portal.py"), "--check"], { encoding: "utf8" });
  assert.strictEqual(r.status, 0, `gen_display_portal.py --check failed:\n${r.stdout}${r.stderr}`);
  assert.strictEqual(DATA.captive.html_sha256, createHash("sha256").update(PORTAL_SRC, "utf8").digest("hex"),
    "html_sha256 is the served PORTAL_HTML's (the probe compares GET / against it)");
});

test("generated data: routes and captive facts are provision.cpp's", () => {
  const routes = [...PROVISION.matchAll(/ctx\.server\.on\("([^"]+)",\s*HTTP_([A-Z]+),/g)]
    .map((m) => ({ method: m[2], path: m[1] }));
  assert.deepStrictEqual(DATA.captive.routes, routes);
  assert.deepStrictEqual(routes.map((r) => `${r.method} ${r.path}`),
    ["GET /", "GET /scan", "POST /join", "GET /status"], "the four routes the phone drives");
  assert.strictEqual(DATA.captive.not_found_redirect, "http://192.168.4.1/");
  assert.strictEqual(DATA.captive.ip, "192.168.4.1");
  assert.strictEqual(DATA.captive.http_port, 80);
  assert.strictEqual(DATA.captive.dns_port, 53);
  assert.strictEqual(DATA.ap.max_stations, 1, "one phone at a time (the hardening the portal chose)");
  for (const needle of ["Time zone", "<select id=\"tz\">", "Connect your display"]) {
    assert.ok(PORTAL_SRC.includes(needle) && DATA.captive.html.includes(needle), `captive copy ${needle}`);
  }
  // The /status failure reasons are the fleet-wide table's labels.
  const policy = read(join(REPO, "firmware/common/network/wifi_join_policy.h"));
  for (const label of Object.values(DATA.join_failures)) {
    assert.ok(policy.includes(`"${label}"`), `join failure label ${label} is in wifi_join_policy.h`);
  }
  assert.ok(PROVISION.includes("return canary::net::join_failure_label(classify_status(st));"),
    "the portal's /status reason is join_failure_label()");
});

test("stripPortal: the Lab's runtime transform is the generator's, byte for byte", async () => {
  const { stripPortal } = await phone();
  assert.strictEqual(stripPortal(PORTAL_SRC), DATA.captive.html);
  assert.doesNotMatch(DATA.captive.html, /<script\b/i);
  assert.doesNotMatch(DATA.captive.html, /\sstyle\s*=/i);
  assert.strictEqual(DATA.captive.stripped.script_blocks, 1);
  // A nested script cannot re-form after one pass; attributes go from every tag.
  assert.strictEqual(stripPortal("<p>a</p><scr<script>x</script>ipt>y</script>"), "<p>a</p>");
  assert.strictEqual(stripPortal('<div class="a" style="x" onclick=\'y\'>t</div>'), '<div class="a">t</div>');
  // Nor can an attribute: removing one must not splice a live handler together.
  assert.strictEqual(stripPortal('<a  onfoo="1"onclick=2>t</a>'), '<a>t</a>');
  assert.strictEqual(stripPortal('<A ONCLICK="x" Style=y>t</A>'), '<A>t</A>');
});

test("zone presets: the phone reads the portal script's TZS, and preselects as it does", async () => {
  const { portalTzPresets, tzPreselect, tzKeepLabel } = await phone();
  const presets = portalTzPresets(PORTAL_SRC);
  assert.deepStrictEqual(presets, DATA.tz_presets);
  assert.ok(presets.length >= 20);
  // The portal: `(','+TZS[i][2]+',').indexOf(','+guess+',')`, first hit wins,
  // the option list is [sentinel, ...presets].
  assert.ok(PORTAL_SCRIPT.includes("if((','+TZS[i][2]+',').indexOf(','+guess+',')>=0){hit=i;break}"));
  assert.ok(PORTAL_SCRIPT.includes("s.selectedIndex=hit>=0?hit+1:0;"));
  const ny = presets.findIndex((r) => r[2].split(",").includes("America/New_York"));
  assert.strictEqual(tzPreselect(presets, "America/New_York"), ny + 1);
  assert.strictEqual(tzPreselect(presets, "America/Detroit"), ny + 1, "a secondary IANA name hits its row");
  assert.strictEqual(tzPreselect(presets, "Mars/Olympus_Mons"), 0, "unknown → keep current");
  assert.strictEqual(tzPreselect(presets, ""), 0);
  assert.ok(PORTAL_SCRIPT.includes("o.textContent='Keep current setting'"));
  assert.strictEqual(tzKeepLabel(presets, ""), "Keep current setting");
  assert.strictEqual(tzKeepLabel(presets, "UTC0"), "Keep UTC");
  assert.strictEqual(tzKeepLabel(presets, "LT-2"), "Keep LT-2", "an unlisted rule is named, not guessed");
});

test("joinBody: the /join body the portal's script builds, exactly", async () => {
  const { joinBody } = await phone();
  assert.ok(PORTAL_SCRIPT.includes(
    "var body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('pw').value)\n" +
    "+(tzv?'&tz='+encodeURIComponent(tzv):'');"), "the portal's body construction moved — re-mirror joinBody");
  assert.strictEqual(joinBody("Home Net", "p&ss=1", ""), "ssid=Home%20Net&pass=p%26ss%3D1");
  assert.strictEqual(joinBody("HomeNet", "k", "EST5EDT,M3.2.0,M11.1.0"),
    "ssid=HomeNet&pass=k&tz=EST5EDT%2CM3.2.0%2CM11.1.0");
  // provision.cpp's handle_join caps: the phone must not paper over them.
  assert.ok(PROVISION.includes("if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {"));
});

test("bars: the portal's three-bar thresholds", async () => {
  const { barsFor } = await phone();
  assert.ok(PORTAL_SCRIPT.includes("var n=r>-60?3:r>-72?2:1"));
  assert.deepStrictEqual([-40, -60, -61, -72, -73, -90].map(barsFor), [3, 2, 2, 1, 1, 1]);
});

test("phoneStep: the phone's walk only takes steps the portal allows", async () => {
  const { phoneStep, PHASES } = await phone();
  const walk = (evs, from = "idle") => evs.reduce((p, e) => phoneStep(p, e), from);
  assert.strictEqual(walk(["softap-up", "joined", "portal", "pick", "submit", "status-success", "softap-down"]), "gone");
  assert.strictEqual(walk(["softap-up", "joined", "portal", "pick", "submit", "status-fail"]), "failed");
  assert.strictEqual(walk(["softap-up", "joined", "portal", "pick", "submit", "status-fail", "submit", "status-success"]), "done");
  assert.strictEqual(walk(["softap-up", "joined", "portal", "left"]), "ap", "canceling the sheet drops back to the Wi-Fi list");
  assert.strictEqual(walk(["softap-up", "joined", "portal", "softap-down"]), "idle", "an AP that vanishes mid-walk is not a success");
  assert.strictEqual(walk(["submit"], "captive"), "captive", "no join without a picked network");
  assert.strictEqual(walk(["status-success"], "form"), "form", "a verdict only lands on a join in flight");
  assert.strictEqual(walk(["portal"], "ap"), "ap", "no captive page before the phone is on the AP");
  for (const p of PHASES) assert.strictEqual(phoneStep(p, "reset"), "idle");
});

test("preseed keys: the provisioned boot writes the NVS keys runtime_config.cpp reads", async () => {
  const src = read(join(ROOT, "emulator/web/emu-shell.js"));
  const cfg = read(join(REPO, "firmware/projects/canary-display/src/runtime_config.cpp"));
  assert.ok(cfg.includes('prefs.begin("securacv"'), "runtime_config.cpp's namespace");
  for (const key of ["wifi_ssid", "wifi_pass"]) {
    assert.ok(cfg.includes(`"${key}"`), `runtime_config.cpp reads ${key}`);
    assert.ok(src.includes(`this._nvsPut("securacv", "${key}", HOME_LAN.`), `emu-shell.js preseeds ${key}`);
  }
  assert.match(src, /if \(provisioned && !firstMeeting\) \{\n\s+this\._nvsPut\("securacv", "wifi_ssid"/,
    "Wi-Fi is preseeded unless this is a first meeting (a factory-fresh unit opens the portal)");
  const app = read(join(ROOT, "assets/app.js"));
  for (const key of ["securacv/wifi_ssid", "securacv/wifi_pass", "scv-hello/"]) {
    assert.ok(app.includes(`"${key}"`), `a "meet again" reboot forgets ${key}`);
  }
  // The scenario's home router takes the very key the provisioned boot holds.
  const { HOME_LAN, demoLan, lanSpec } = await shell();
  const home = demoLan().filter((n) => n.home);
  assert.deepStrictEqual(home.map((n) => [n.ssid, n.pass, n.secure]), [[HOME_LAN.ssid, HOME_LAN.pass, true]]);
  assert.strictEqual(lanSpec([{ ssid: "A b", rssi: -60.4, secure: false, home: false, pass: "" }]), "412062 -60 0 0 -");
});

test("DNS: the query the phone sends and the reply shape dns_build_response makes", async () => {
  const { dnsQueryBytes, parseDnsReply, parseHeaders } = await shell();
  const q = dnsQueryBytes("captive.apple.com", 1, 0xbeef);
  assert.strictEqual(Buffer.from(q).toString("hex"),
    "beef010000010000000000000763617074697665056170706c6503636f6d0000010001");
  // The firmware's reply, built per provision_core.h: header+question echoed,
  // flags 0x84|RD, ANCOUNT 1, pointer C00C, A/IN, TTL 60, RDLENGTH 4, the IP.
  const reply = Uint8Array.from([...q.slice(0, 12), ...q.slice(12)]);
  reply[2] = 0x85; reply[3] = 0; reply[7] = 1;
  const full = Uint8Array.from([...reply, 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0x3c, 0, 4, 192, 168, 4, 1]);
  const r = parseDnsReply(full);
  assert.deepStrictEqual([r.id, r.response, r.authoritative, r.rcode, r.ancount, r.a],
    [0xbeef, true, true, 0, 1, "192.168.4.1"]);
  const core = read(join(REPO, "firmware/common/network/provision_core.h"));
  assert.ok(core.includes("out[2] = (uint8_t)(0x84 | (query[2] & 0x01));"), "reply flags as parsed here");
  assert.ok(core.includes("0x00, 0x00, 0x00, 0x3C, // TTL 60 s"));
  // NODATA for AAAA: no answers.
  const nodata = Uint8Array.from(reply); nodata[7] = 0;
  assert.strictEqual(parseDnsReply(nodata).a, null);
  assert.deepStrictEqual(parseHeaders("Cache-Control: no-store\nLocation: http://192.168.4.1/\n"),
    { "cache-control": "no-store", location: "http://192.168.4.1/" });
});

test("build compiles the real portal against the shims — no stub left behind", () => {
  const build = read(join(ROOT, "emulator/build.sh"));
  assert.ok(build.includes('"$PROJ/src/net/provision.cpp"'), "build.sh compiles net/provision.cpp");
  const net = read(join(ROOT, "emulator/src/emu_net.cpp"));
  assert.doesNotMatch(net, /\bbool provision_needed\(\)|\bvoid provision_run\(/, "emu_net.cpp still stubs provision");
  // provision.cpp's radio calls exist on the shim (a missing one is a compile
  // error in CI; this names it first).
  const wifi = read(join(ROOT, "emulator/shim/WiFi.h"));
  for (const api of ["softAP(", "softAPIP(", "softAPgetStationNum(", "softAPdisconnect(", "scanNetworks(",
    "scanComplete(", "scanDelete(", "SSID(", "encryptionType(", "begin(", "status("]) {
    assert.ok(wifi.includes(api), `shim/WiFi.h lacks ${api}`);
  }
  // The shim's verdicts are the ones provision.cpp classifies into reasons.
  const radio = read(join(ROOT, "emulator/src/emu_radio.cpp"));
  assert.ok(radio.includes("if (n.secure && n.pass != g_join_pass) return WL_CONNECT_FAILED;"));
  assert.ok(radio.includes("return WL_NO_SSID_AVAIL;"));
  assert.ok(PROVISION.includes("case WL_NO_SSID_AVAIL:  return canary::net::JoinFailure::NotFound;"));
  assert.ok(PROVISION.includes("case WL_CONNECT_FAILED: return canary::net::JoinFailure::BadPassword;"));
});

test("CI runs the generator check, this test and the browser probe", () => {
  const wf = read(join(REPO, ".github/workflows/canary-local.yml"));
  for (const needle of [
    "python3 canary-local/tools/gen_display_portal.py --check",
    "node --test canary-local/tests/onboard.test.js",
    "node canary-local/tests/onboard_probe.mjs",
  ]) {
    assert.ok(wf.includes(needle), `${needle} is not wired into canary-local.yml`);
  }
  // Chained like gen_wap → gen_csp: the portal data is checked before the CSP.
  assert.ok(wf.indexOf("gen_display_portal.py --check") < wf.indexOf("python3 canary-local/tools/gen_csp.py --check"),
    "the portal data gate runs before the CSP gate that hashes it");
});
