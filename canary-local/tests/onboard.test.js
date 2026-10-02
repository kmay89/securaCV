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
const PORTAL_SCRIPT = /<script\b[^>]*>([\s\S]*?)<\/script\b[^>]*>/i.exec(PORTAL_SRC)[1];
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

test("bird seat (F89): the probe holds the drawn bird to the seat onboard_layout.h names", async () => {
  const { birdOnSeat } = await import("./bird_perch.mjs");
  const seat = { x: 100, y: 36, w: 40, h: 40, breath: 2 };
  const bird = (dx, dy, extra = {}) => ({ x: 100 + dx, y: 36 + dy, w: 40, h: 40, shown: true, ...extra });
  for (const dy of [-2, -1, 0, 1, 2]) {
    assert.strictEqual(birdOnSeat({ bird: bird(0, dy), seat }), null, `breathing ${dy} px is on the seat`);
  }
  // Off the seat: past the breath, beside it, another size, off stage, no seat.
  // The round watch's bird before F64: at the panel's center, y 98.
  assert.match(birdOnSeat({ bird: bird(0, 62), seat }), /drawn at 100,98/);
  assert.match(birdOnSeat({ bird: bird(0, 3), seat }), /seats it at 100,36/);
  assert.match(birdOnSeat({ bird: bird(0, -3), seat }), /drawn at 100,33/);
  assert.match(birdOnSeat({ bird: bird(1, 0), seat }), /drawn at 101,36/);
  assert.match(birdOnSeat({ bird: bird(0, 0, { w: 64, h: 64 }), seat }), /\(64x64\)/);
  assert.match(birdOnSeat({ bird: bird(0, 0, { shown: false }), seat }), /none is on stage/);
  assert.match(birdOnSeat({ bird: null, seat }), /none is on stage/);
  assert.match(birdOnSeat({ bird: bird(0, 0), seat: null }), /no onboarding scene names a seat/);
  // The seat is the firmware's: the binding asks onboard_ui (which asks
  // onboard_layout.h's bird_seat), the shell reads it, the probe uses it in
  // the Hello and PhoneJoined scenes.
  const binding = read(join(ROOT, "emulator/src/emu_bindings.cpp"));
  assert.match(binding, /EMSCRIPTEN_KEEPALIVE const char\* emu_onboard_seat\(void\)/);
  assert.ok(binding.includes("canary::ui::onboard_ui_bird_seat(&x, &y, &d)"));
  assert.ok(binding.includes("canary::ui::onboardlayout::kBirdBreath"));
  const ui = read(join(REPO, "firmware/projects/canary-display/src/ui/onboard_ui.cpp"));
  assert.strictEqual((ui.match(/onboardlayout::bird_seat\(s_glass, WIDE, s_join, s_halo, (st|s_stage)\)/g) || []).length, 2,
    "onboard_ui.cpp seats the bird and reports the seat through the same bird_seat() call");
  const shellSrc = read(join(ROOT, "emulator/web/emu-shell.js"));
  assert.ok(shellSrc.includes('M.cwrap("emu_onboard_seat", "number", [])'));
  const probe = read(join(__dirname, "onboard_probe.mjs"));
  assert.ok(probe.includes("const helloSeat = breathOnSeat(hello);"), "the probe holds the Hello scene's bird");
  assert.ok(probe.includes("const onSeat = breathOnSeat(phoneJoined);"), "the probe holds the PhoneJoined scene's bird");
  assert.ok(probe.includes("const helloPerch = birdPerch(st);"), "the Hello bird is on the glass and clear of text");
  assert.ok(probe.includes("const hello = await readScene(first, helloUp, 60000);") &&
    probe.includes("const phoneJoined = await readScene(perchAt, (st) => st.bird && st.bird.shown, 4000);"),
  "the probe reads each scene's bird through its breath, not once");
});

// canary_mark's breath as LVGL 8.4 draws it: start_bob(1400, 2) scaled by
// the temperament, lv_anim_path_ease_in_out (lv_bezier3 with control points
// 0, 50, 952, 1024) with playback, and lv_anim's integer rounding (the eased
// step times the span, shifted down 10 bits, so it rounds toward -inf). The
// top of the swing is drawn only around the tick it completes (here: two
// 5 ms ticks). Returns the offset from the seat at `ms` after the breath
// started.
function lvBreath(ms, halfMs = 1400, amp = 2) {
  const bezier3 = (t, u0, u1, u2, u3) => {
    const r = 1024 - t;
    const r2 = Math.floor((r * r) / 1024), r3 = Math.floor((r2 * r) / 1024);
    const t2 = Math.floor((t * t) / 1024), t3 = Math.floor((t2 * t) / 1024);
    return Math.floor((r3 * u0) / 1024) + Math.floor((3 * r2 * t * u1) / 1048576) +
      Math.floor((3 * r * t2 * u2) / 1048576) + Math.floor((t3 * u3) / 1024);
  };
  const at = Math.floor(ms / 5) * 5 % (2 * halfMs);
  if (at === halfMs) return amp;  // the tick the swing completes
  const back = at > halfMs;
  const t = Math.floor(((back ? at - halfMs : at) * 1024) / halfMs);
  const step = bezier3(t, 0, 50, 952, 1024);
  const from = back ? amp : -amp;
  return Math.floor((step * -2 * from) / 1024) + from;
}

test("bird breath (F89): breathOnSeat holds a scene's bird to its seat exactly, wherever its reads fall", async () => {
  const { breathOnSeat, birdOnSeat } = await import("./bird_perch.mjs");
  // The model is LVGL's: from the seat less 2 to the seat plus 2 and back,
  // the low end and the seat plus 1 each held for about a third of a swing.
  const offs = new Set();
  for (let ms = 0; ms < 2800; ms += 5) offs.add(lvBreath(ms));
  assert.deepStrictEqual([...offs].sort((a, b) => a - b), [-2, -1, 0, 1, 2]);
  const ticks = (v) => Array.from({ length: 560 }, (_, k) => lvBreath(k * 5)).filter((o) => o === v).length;
  assert.ok(ticks(-2) * 5 > 700 && ticks(1) * 5 > 700 && ticks(2) >= 1 && ticks(2) <= 2,
    `the breath holds -2 for ${ticks(-2) * 5} ms, +1 for ${ticks(1) * 5} ms and +2 for ${ticks(2)} tick(s) a cycle`);

  // Reads of a bird drawn `dy` px off a seat at y 36, every `every` ms from
  // `phase` ms into its breath, for `span` ms.
  const seat = { x: 100, y: 36, w: 40, h: 40, breath: 2 };
  const reads = (dy, phase, span, every = 100, halfMs = 1400) => {
    const out = [];
    for (let ms = phase; ms <= phase + span; ms += every) {
      out.push({ seat, bird: { x: 100, y: 36 + dy + lvBreath(ms, halfMs), w: 40, h: 40, shown: true } });
    }
    return out;
  };
  for (const phase of [0, 300, 700, 1100, 1500, 1900, 2300, 2700]) {
    // On its seat: every whole swing passes, at the breath's slowest pace too.
    assert.strictEqual(breathOnSeat(reads(0, phase, 3000)), null, `on the seat from ${phase} ms`);
    assert.strictEqual(breathOnSeat(reads(0, phase, 3700, 100, 1750)), null, `on the seat, slow breath, from ${phase} ms`);
    // Off it by a pixel either way, or by the 3 px a single read let through
    // (the review's watch case), it fails wherever the reads fall.
    for (const dy of [-3, -1, 1, 3]) {
      assert.match(breathOnSeat(reads(dy, phase, 3000)) || "", /F89/, `${dy} px off the seat from ${phase} ms`);
    }
  }
  // The Hello scene: read on the slowed clock from the bird's first breath
  // to the scene's end (2.6 s), it holds; 1 px off, it fails.
  assert.strictEqual(breathOnSeat(reads(0, 0, 2600, 5)), null);
  assert.match(breathOnSeat(reads(1, 0, 2600, 5)), /drawn at 100,39/);
  assert.match(breathOnSeat(reads(-1, 0, 2600, 5)), /drawn at 100,33/);
  // A single read can pass a bird 1 px off (the old check), and so can
  // every read of a run that misses the swing's top tick; the run cannot.
  const oneOff = reads(1, 50, 3000);
  assert.ok(oneOff.every((r) => birdOnSeat(r) === null), "each read of a bird 1 px low sits within the breath");
  assert.match(breathOnSeat(oneOff), /ran y 35\.\.38/);
  // Reads that miss a swing fail rather than pass on what they did not see,
  // and so does a bird that never breathes.
  assert.match(breathOnSeat(reads(0, 1000, 400)), /a whole swing reaches 34 and at least 37/);
  const still = reads(0, 0, 3000).map((r) => ({ ...r, bird: { ...r.bird, y: 36 } }));
  assert.match(breathOnSeat(still), /ran y 36\.\.36/);
  // Each read is held as birdOnSeat holds it, and the seat stays put.
  const aside = reads(0, 0, 3000).map((r) => ({ ...r, bird: { ...r.bird, x: 101 } }));
  assert.match(breathOnSeat(aside), /drawn at 101,/);
  const moved = reads(0, 0, 3000);
  moved[5] = { ...moved[5], seat: { ...seat, y: 37 } };
  assert.match(breathOnSeat(moved), /seat changed within the scene/);
  assert.match(breathOnSeat([]), /no reads/);
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
