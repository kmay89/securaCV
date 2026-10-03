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
//   · the turned glass's reads pass a line off it, or a card
//     outside its halo                                         → "turned glass (F184)"
//   · the turned glass is drawn mirrored or flipped, its halo
//     reported where it is not drawn, its lines inked nowhere,
//     its first frame unturned, or its card off the layout      → "turned glass (F184)"
//   · a saved rotation lands after power-on, or a dist without
//     the binding boots unturned                               → "saved rotation (F184)"
//   · the turned walk falls out of the probe or the glass test
//     out of CI                                                → "turned wiring (F184)"
//   · a turned flavor's panel or turn typed in a probe, or read
//     from the wrong pin map                                   → "turned glasses (F204, F206)"
//   · the turned splash read misses the splash or half of it   → "splash coverage (F206)"
//   · the nightlight flavor's wiring, HAL turn, preset or the
//     emulator's flavor lists drift apart                      → "nightlight flavor (F204)"
//   · the clock slows after the splash began, or a probe stops
//     reading the turned splash or booting a turned glass      → "splash and turned boots (F206)"
//   · the drift check passes a new flavor's uncommitted bundle → "dist drift (F204)"
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
    probe.includes("phoneJoined = await readScene(perchAt, (st) => st.bird && st.bird.shown, 4000);"),
  "the probe reads each scene's bird through its breath, not once");
  assert.ok(probe.includes("if (!flourishHop(phoneJoined)) break;"), "a flourish's hop earns a re-read, not a pass");
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
  // An idle flourish's hop (at least 8 px for 560 ms) inside the reads
  // fails the run and marks it for a re-read; the breath alone, or a bird
  // up to 5 px off its seat, is never taken for a hop (it fails as above,
  // with no re-read), and one further off is re-read and fails again.
  const { flourishHop } = await import("./bird_perch.mjs");
  const hopped = reads(0, 0, 3000).map((r, k) => (k >= 10 && k < 16 ? { ...r, bird: { ...r.bird, y: 36 - 8 } } : r));
  assert.ok(flourishHop(hopped));
  assert.match(breathOnSeat(hopped), /drawn at 100,28/);
  for (const dy of [-5, -3, -1, 0, 1, 3, 7]) assert.ok(!flourishHop(reads(dy, 0, 3000)), `${dy} px off is no hop`);
  assert.ok(flourishHop(reads(-10, 0, 3000)), "a bird 10 px high on every read is re-read, and fails again");
});

test("turned glass (F184): linesOnGlass fails a line that leaves the glass, and only that", async () => {
  const { linesOnGlass } = await import("./onboard_glass.mjs");
  const glass = { w: 480, h: 800 };
  const line = (x, y, w, h, extra = {}) => ({ x, y, w, h, shown: true, opa: 255, text: "Scan with your phone", ...extra });
  assert.strictEqual(linesOnGlass([line(8, 120, 464, 43), line(0, 0, 480, 800)], glass), null);
  // Off any edge, by a pixel: named with its box.
  assert.match(linesOnGlass([line(8, 120, 473, 43)], glass), /1 line\(s\) leave the 480x800 glass: .*Scan with your phone.*\[8,120,473,43\]/);
  assert.match(linesOnGlass([line(-1, 120, 100, 20)], glass) || "", /F184/);
  assert.match(linesOnGlass([line(10, -1, 100, 20)], glass) || "", /F184/);
  assert.match(linesOnGlass([line(10, 790, 100, 11)], glass) || "", /F184/);
  // The 800x480 glass's own row on the turned glass: the old F156 cut.
  assert.match(linesOnGlass([line(8, 300, 784, 43)], glass) || "", /784/);
  // A line that does not draw is not held: hidden, faded out, or empty.
  for (const extra of [{ shown: false }, { opa: 0 }, { text: "" }, { text: "   " }]) {
    assert.strictEqual(linesOnGlass([line(-50, -50, 900, 900, extra)], glass), null, JSON.stringify(extra));
  }
});

test("turned glass (F184): linesCut fails a line LVGL cut to an ellipsis, as its own text says", async () => {
  const { linesCut } = await import("./onboard_glass.mjs");
  const line = (text, extra = {}) => ({ x: 8, y: 122, w: 464, h: 40, shown: true, opa: 255, text, ...extra });
  assert.strictEqual(linesCut([line("Scan with your phone"), line("password  p7Rm2Kqf")]), null);
  // The portrait dash's Join title before F156, in the 36 px face whose dots
  // the framebuffer read takes for letters.
  assert.match(linesCut([line("Scan with your phone...")]), /1 line\(s\) cut to an ellipsis: \["Scan with your phone\.\.\."\]/);
  // A network name shortened around "..." on purpose keeps its tail.
  assert.strictEqual(linesCut([line("Basement-Mesh-E...nder-Office-5G")]), null);
  // Only a line that draws.
  assert.strictEqual(linesCut([line("cut...", { shown: false }), line("cut...", { opa: 0 })]), null);
});

test("turned glass (F184): cardInHalo holds the QR card over its halo's center, sides inside the stroke", async () => {
  const { cardInHalo, haloOf } = await import("./onboard_glass.mjs");
  // The 800 px glass's Join card (232 px, 10 px corners) in its 300 px ring,
  // as on the portrait dash: the card's box is its white pixels, inclusive.
  const halo = { cx: 240, cy: 300, r: 150, stroke: 3, shown: true };
  const card = (side, dx = 0, dy = 0) => {
    const x0 = 240 - side / 2 + dx, y0 = 300 - side / 2 + dy;
    return { box: [x0, y0, x0 + side - 1, y0 + side - 1] };
  };
  const dash = cardInHalo(card(232), halo);
  assert.strictEqual(dash.fail, null);
  assert.deepStrictEqual([dash.sides, dash.reach, dash.inner], [116, 159.9, 147]);
  // Its corners reach past the ring (F155): failed only when held.
  assert.match(cardInHalo(card(232), halo, { corners: true }).fail, /corners reach 159\.9 px .* past its stroke/);
  // Under Heirloom the Join stack sets the card 5 px above the ring's center
  // (F155): still over it, its sides 121 px out, inside the stroke.
  const heir = cardInHalo(card(232, 0, -5), halo);
  assert.strictEqual(heir.fail, null);
  assert.deepStrictEqual([heir.offset, heir.sides], [[0, -5], 121]);
  // Beside the center, or past the stroke at a side, it fails.
  assert.match(cardInHalo(card(100, 60, 0), halo).fail, /does not stand over its halo's center/);
  assert.match(cardInHalo(card(100, 0, -51), halo).fail, /does not stand over its halo's center/);
  assert.match(cardInHalo(card(232, 0, -32), halo).fail, /sides reach 148 px/);
  assert.match(cardInHalo(card(296), halo).fail, /sides reach 148 px/);
  assert.strictEqual(cardInHalo(card(294), halo).fail, null);
  // The landscape nightlight's 150 px ring and 104 px card (F157): corners
  // inside the stroke, so held there.
  const night = { cx: 243, cy: 90, r: 75, stroke: 3, shown: true };
  const nightCard = { box: [243 - 52, 90 - 52, 243 + 51, 90 + 51] };
  const n = cardInHalo(nightCard, night, { corners: true });
  assert.strictEqual(n.fail, null);
  assert.ok(n.inner - n.reach > 2 && n.inner - n.reach < 3, `corners ${n.inner - n.reach} px inside`);
  // Nothing to hold is a failure, not a pass.
  assert.match(cardInHalo(null, halo).fail, /no QR card/);
  assert.match(cardInHalo(card(232), null).fail, /no halo/);
  // The halo is the scene's largest shown arc.
  assert.deepStrictEqual(haloOf([{ ...halo, r: 40 }, halo, { ...halo, r: 400, shown: false }]), halo);
  assert.strictEqual(haloOf([]), null);
  assert.strictEqual(haloOf([{ ...halo, shown: false }]), null);
});

// A synthetic glass for the framebuffer reads (F184): w x h RGBA, black.
function glassFrame(w, h) {
  const data = new Uint8ClampedArray(w * h * 4);
  for (let i = 3; i < data.length; i += 4) data[i] = 255;
  return { w, h, data };
}
function paint(fr, x, y, v) {
  const i = (y * fr.w + x) * 4;
  fr.data[i] = fr.data[i + 1] = fr.data[i + 2] = v;
}
// The glass drawn wrong: mirrored left-right, flipped top-bottom, or both
// (upside down). Each keeps the glass's shape, so no size check sees it.
function redraw(fr, how) {
  const out = glassFrame(fr.w, fr.h);
  for (let y = 0; y < fr.h; y++) {
    for (let x = 0; x < fr.w; x++) {
      const sx = how === "mirror" || how === "upside-down" ? fr.w - 1 - x : x;
      const sy = how === "flip" || how === "upside-down" ? fr.h - 1 - y : y;
      const i = (sy * fr.w + sx) * 4, o = (y * fr.w + x) * 4;
      for (let k = 0; k < 4; k++) out.data[o + k] = fr.data[i + k];
    }
  }
  return out;
}
// A join card as onboard_ui draws it: a white card with 10 px rounded corners
// (the glass shows behind them), 12 px padding, and a 21-module code at 4 px
// a module: finders at three corners with their light separators, the timing
// rows, and fixed pseudo-random data everywhere else (bottom-right included).
function qrGlass() {
  const fr = glassFrame(200, 160);
  const N = 21, m = 4, pad = 12, R = 10;
  const side = N * m + 2 * pad;
  const left = 46, top = 20;
  for (let y = top; y < top + side; y++) {
    for (let x = left; x < left + side; x++) {
      const px = x + 0.5, py = y + 0.5;
      const ax = px < left + R ? left + R : px > left + side - R ? left + side - R : null;
      const ay = py < top + R ? top + R : py > top + side - R ? top + side - R : null;
      if (ax !== null && ay !== null && Math.hypot(px - ax, py - ay) > R) continue;
      paint(fr, x, y, 255);
    }
  }
  let seed = 7;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) >> 16) & 1;
  const finder = (i, j) => i === 0 || i === 6 || j === 0 || j === 6 || (i >= 2 && i <= 4 && j >= 2 && j <= 4);
  for (let r = 0; r < N; r++) {
    for (let c = 0; c < N; c++) {
      let d;
      if (r < 8 && c < 8) d = r < 7 && c < 7 && finder(c, r);
      else if (r < 8 && c >= N - 8) d = r < 7 && c >= N - 7 && finder(c - (N - 7), r);
      else if (r >= N - 8 && c < 8) d = r >= N - 7 && c < 7 && finder(c, r - (N - 7));
      else if (r === 6 || c === 6) d = (r + c) % 2 === 0;
      else d = rnd() === 1;
      if (!d) continue;
      for (let y = 0; y < m; y++) for (let x = 0; x < m; x++) paint(fr, left + pad + c * m + x, top + pad + r * m + y, 0);
    }
  }
  return { fr, card: [left, top, left + side - 1, top + side - 1] };
}

test("turned glass (F184): qrFinders/qrUpright hold the join QR upright, and a mirrored, flipped or upside-down glass fails", async () => {
  const { qrFinders, qrUpright } = await import("./onboard_glass.mjs");
  const { fr, card } = qrGlass();
  const f = qrFinders(fr, card, 10);
  assert.deepStrictEqual(f, { box: [58, 32, 141, 115], module: 4, tl: true, tr: true, bl: true, br: false });
  assert.strictEqual(qrUpright(f), null);
  // Drawn wrong, the glass keeps its shape and the card stays a white box,
  // but a finder lands at bottom-right.
  const box = (how) => {
    const [x0, y0, x1, y1] = card;
    const mx = how !== "flip", my = how !== "mirror";
    return [mx ? fr.w - 1 - x1 : x0, my ? fr.h - 1 - y1 : y0, mx ? fr.w - 1 - x0 : x1, my ? fr.h - 1 - y0 : y1];
  };
  for (const how of ["mirror", "flip", "upside-down"]) {
    const g = qrFinders(redraw(fr, how), box(how), 10);
    assert.strictEqual(g.br, true, `${how}: a finder at bottom-right`);
    assert.match(qrUpright(g), /finder patterns stand at .*br.* not top-left, top-right and bottom-left/, how);
  }
  // A fourth finder (a corrupted code, or a glass drawn twice over) is not
  // an upright code either.
  const four = qrGlass();
  const N = 21, m = 4, left = 46 + 12 + (N - 7) * m, top = 20 + 12 + (N - 7) * m;
  for (let j = -1; j < 7; j++) {
    for (let i = -1; i < 7; i++) {
      const d = i >= 0 && j >= 0 && (i === 0 || i === 6 || j === 0 || j === 6 || (i >= 2 && i <= 4 && j >= 2 && j <= 4));
      for (let y = 0; y < m; y++) for (let x = 0; x < m; x++) paint(four.fr, left + i * m + x, top + j * m + y, d ? 0 : 255);
    }
  }
  const f4 = qrFinders(four.fr, four.card, 10);
  assert.deepStrictEqual([f4.tl, f4.tr, f4.bl, f4.br], [true, true, true, true]);
  assert.match(qrUpright(f4), /stand at tl, tr, bl, br/);
  // The glass behind the card's rounded corners is not the code's: read as
  // if the card were square, the corners' dark bounds the code and no finder
  // is found.
  assert.match(qrUpright(qrFinders(fr, card, 0)), /stand at no corner/);
  // A card with no code fails rather than passing on nothing.
  const blank = glassFrame(40, 40);
  for (let y = 5; y < 35; y++) for (let x = 5; x < 35; x++) paint(blank, x, y, 255);
  assert.strictEqual(qrFinders(blank, [5, 5, 34, 34], 0), null);
  assert.match(qrUpright(null), /holds no QR code/);
});

test("turned glass (F184): haloInk/haloInked hold the halo the firmware reports to the stroke on the glass", async () => {
  const { haloInk, haloInked } = await import("./onboard_glass.mjs");
  const ring = (v) => {
    const fr = glassFrame(200, 200);
    for (let y = 0; y < 200; y++) {
      for (let x = 0; x < 200; x++) {
        const d = Math.hypot(x - 100, y - 100);
        if (d >= 47 && d < 50) paint(fr, x, y, v);
      }
    }
    return fr;
  };
  const halo = { cx: 100, cy: 100, r: 50, stroke: 3 };
  // The Join halo at its breath's faintest still reads (8 on black).
  for (const v of [16, 8]) {
    const ink = haloInk(ring(v), halo);
    assert.deepStrictEqual(ink, { stroke: [v, v, v, v], outside: [0, 0, 0, 0] });
    assert.strictEqual(haloInked(ink, 0, 3), null);
  }
  // A circle reported wider than drawn, with no stroke, or off center, reads
  // the glass where its stroke should be.
  assert.match(haloInked(haloInk(ring(16), { ...halo, r: 70 }), 0, 3), /right, left, bottom, top/);
  assert.match(haloInked(haloInk(ring(16), { ...halo, stroke: 0 }), 0, 3), /not the one on the glass/);
  assert.match(haloInked(haloInk(ring(16), { ...halo, cx: 50 }), 0, 3), /not the one on the glass at its (right|left)/);
  // Ink just outside the stroke (a ring drawn wider than reported) fails
  // too, even where the stroke is still the brighter of the two.
  const wide = ring(16);
  for (let x = 152; x < 156; x++) paint(wide, x, 100, 10);
  const wideInk = haloInk(wide, halo);
  assert.ok(wideInk.stroke[0] > wideInk.outside[0] + 3, "the stroke outshines what is outside it");
  assert.match(haloInked(wideInk, 0, 3), /at its right \(/);
  // A glass no brighter than the margin is not a stroke.
  assert.match(haloInked(haloInk(ring(3), halo), 0, 3), /right, left, bottom, top/);
  // F204: a halo 2 px from the glass's edge (the landscape nightlight's, at
  // the panel's right) has nothing 3 px outside it on that side but the
  // edge; its stroke there is held against the background alone. Before,
  // the off-glass read (-1) failed it on a halo the glass draws right.
  const edge = glassFrame(152, 200);
  for (let y = 0; y < 200; y++) {
    for (let x = 0; x < 152; x++) {
      const d = Math.hypot(x - 100, y - 100);
      if (d >= 47 && d < 50) paint(edge, x, y, 16);
    }
  }
  const edgeInk = haloInk(edge, halo);
  assert.strictEqual(edgeInk.outside[0], -1, "3 px right of the stroke is off the glass");
  assert.strictEqual(haloInked(edgeInk, 0, 3), null);
  // ...but a stroke that is not there still fails, edge or not.
  const bare = glassFrame(152, 200);
  assert.match(haloInked(haloInk(bare, halo), 0, 3), /right, left, bottom, top/);
  // ...and so does a stroke no brighter than the background beside the edge.
  assert.match(haloInked(haloInk(edge, halo), 16, 3), /right/);
});

test("turned glass (F184): linesInk/linesInked find each drawn line's ink where the firmware says it is", async () => {
  const { linesInk, linesInked } = await import("./onboard_glass.mjs");
  const fr = glassFrame(300, 100);
  for (let x = 24; x < 110; x += 3) for (let y = 34; y < 46; y++) paint(fr, x, y, 200);
  const label = (extra = {}) => ({ x: 20, y: 30, w: 100, h: 20, shown: true, opa: 255, text: "Scan with your phone", ...extra });
  const reads = linesInk(fr, [label()]);
  assert.strictEqual(reads.length, 1);
  assert.ok(reads[0].ink > 0);
  assert.strictEqual(linesInked(reads), null);
  // A flipped or mirrored glass draws the line elsewhere; a blank one nowhere.
  for (const g of [redraw(fr, "flip"), redraw(fr, "mirror"), glassFrame(300, 100)]) {
    assert.match(linesInked(linesInk(g, [label()])), /1 line\(s\) the firmware draws show no ink .*Scan with your phone/);
  }
  // Only lines that draw whole on the glass are read: hidden, fading, empty
  // or off the glass (linesOnGlass names those) are not.
  for (const extra of [{ shown: false }, { opa: 200 }, { text: " " }, { x: 250 }, { y: -1 }]) {
    assert.deepStrictEqual(linesInk(glassFrame(300, 100), [label(extra)]), [], JSON.stringify(extra));
  }
});

test("turned glass (F184): framesOnGlass holds every frame to one glass, a turned boot's from the first", async () => {
  const { framesOnGlass } = await import("./onboard_glass.mjs");
  const turned = { w: 480, h: 800 };
  // main.cpp's order: display_init announces the panel, and the first flush
  // after lvgl_port_set_rotation re-announces it turned before it lands.
  assert.strictEqual(framesOnGlass([{ w: 800, h: 480, frames: 0 }, { w: 480, h: 800, frames: 0 }], 95, turned), null);
  // The rotation worn after the splash: the splash drew on the native panel.
  assert.match(framesOnGlass([{ w: 800, h: 480, frames: 0 }, { w: 480, h: 800, frames: 90 }], 200, turned),
    /first frame on a 800x480 glass, not the 480x800/);
  // A native walk: whatever the first frame was on, and nothing after.
  assert.strictEqual(framesOnGlass([{ w: 800, h: 480, frames: 0 }], 10, null), null);
  assert.match(framesOnGlass([{ w: 800, h: 480, frames: 0 }, { w: 480, h: 800, frames: 5 }], 9, null),
    /changed to 480x800 after 5 frame\(s\) on 800x480/);
  assert.match(framesOnGlass([{ w: 480, h: 800, frames: 0 }, { w: 800, h: 480, frames: 40 }], 60, turned),
    /changed to 800x480 after 40/);
  // Nothing drawn, or drawn before any announcement: no pass.
  assert.match(framesOnGlass([{ w: 480, h: 800, frames: 0 }], 0, turned), /no frame/);
  assert.match(framesOnGlass([{ w: 480, h: 800, frames: 3 }], 9, turned), /before it announced/);
  assert.match(framesOnGlass([], 9, null), /before it announced/);
});

test("turned glass (F184): cardAtLayout and haloAtLayout hold the card and halo where onboard_layout.h seats them", async () => {
  const { cardAtLayout, haloAtLayout, cardInHalo } = await import("./onboard_glass.mjs");
  const layout = { ring: { x: 90, y: 250, d: 300, stroke: 3 }, card: { x: 124, y: 284, side: 232, radius: 10 } };
  const halo = { cx: 240, cy: 400, r: 150, stroke: 3, shown: true };
  const card = (dx = 0, dy = 0) => ({ box: [124 + dx, 284 + dy, 355 + dx, 515 + dy] });
  assert.strictEqual(cardAtLayout(card(), layout), null);
  assert.strictEqual(cardAtLayout(card(1, -1), layout), null, "within the anti-aliased edge");
  // Cards cardInHalo passes, inside the halo but not where the layout puts
  // them: 25 px right, 25 px up, 18 px down and right.
  for (const [dx, dy] of [[25, 0], [0, -25], [18, 18]]) {
    assert.strictEqual(cardInHalo(card(dx, dy), halo).fail, null, `${dx},${dy} is inside the halo`);
    assert.match(cardAtLayout(card(dx, dy), layout), /seats it at \[124,284,355,515\]/, `${dx},${dy}`);
  }
  // Heirloom's stack (5 px up) is held to Heirloom's layout, not the default's.
  assert.match(cardAtLayout(card(0, -5), layout), /edges off by \[0,-5,0,-5\]/);
  assert.strictEqual(cardAtLayout(card(0, -5), { ...layout, card: { ...layout.card, y: 279 } }), null);
  assert.match(cardAtLayout(card(), null), /names no Join layout/);
  assert.match(cardAtLayout(null, layout), /no QR card/);
  // The halo: its center, radius and stroke as the ring's box and stroke name them.
  assert.strictEqual(haloAtLayout(halo, layout), null);
  for (const bad of [{ r: 170 }, { stroke: 0 }, { cx: 90 }, { cy: 240 }]) {
    assert.match(haloAtLayout({ ...halo, ...bad }, layout), /seats it about 240,400 \(r 150, stroke 3\)/, JSON.stringify(bad));
  }
  // A card and halo moved together (laid out from the landscape glass's dims
  // on the portrait one) stay "inside" each other; the layout fails both.
  const moved = { ...halo, cy: 240 };
  assert.strictEqual(cardInHalo(card(0, -160), moved).fail, null);
  assert.ok(cardAtLayout(card(0, -160), layout) && haloAtLayout(moved, layout));
  assert.match(haloAtLayout(halo, null), /names no Join layout/);
  assert.match(haloAtLayout(null, layout), /no halo/);
});

test("bird on glass (F184): birdOnGlass holds the moving bird on the glass; birdPerch keeps its message", async () => {
  const { birdOnGlass, birdPerch } = await import("./bird_perch.mjs");
  const glass = { w: 480, h: 800 };
  const st = (b) => ({ bird: { w: 120, h: 120, shown: true, ...b }, glass, labels: [{ x: 0, y: 300, w: 480, h: 40, shown: true, opa: 255, text: "You're in." }] });
  assert.strictEqual(birdOnGlass(st({ x: 180, y: 254 })), null, "the hop over a fading line is on the glass");
  assert.match(birdPerch(st({ x: 180, y: 254 })) || "", /drawn over/, "birdPerch still holds it clear of text");
  for (const b of [{ x: 180, y: -1 }, { x: 361, y: 254 }, { x: -1, y: 254 }, { x: 180, y: 681 }]) {
    assert.match(birdOnGlass(st(b)), /off the 480x800 glass \(F64\)/, JSON.stringify(b));
    assert.strictEqual(birdPerch(st(b)), birdOnGlass(st(b)));
  }
  assert.strictEqual(birdOnGlass(st({ x: 180, y: -50, shown: false })), null);
  assert.strictEqual(birdOnGlass({ bird: null, glass }), null);
});

// A stand-in for the emulator module the shell drives: every export a cwrap
// that records its call (and what a real one would return), and the
// framebuffer/strings it reads.
const FAKE_JOIN = { ring: { x: 90, y: 250, d: 300, stroke: 3 }, card: { x: 124, y: 284, side: 232, radius: 10 } };
function fakeModule({ preset = true, arcs = true, join = true, presetAnswer = null } = {}) {
  const calls = [];
  const factory = async () => ({
    cwrap: (name) => (...args) => {
      calls.push([name, ...args]);
      if (name === "emu_preset_rotation") return presetAnswer ?? (args[0] >= 0 && args[0] <= 3 ? 1 : 0);
      if (name === "emu_screen_arcs") return 8;
      if (name === "emu_onboard_join") return 16;
      return 0;
    },
    _emu_preset_rotation: preset ? () => 0 : undefined,
    _emu_screen_arcs: arcs ? () => 0 : undefined,
    _emu_onboard_join: join ? () => 0 : undefined,
    HEAPU8: new Uint8Array(16),
    UTF8ToString: (ptr) => (ptr === 16 ? JSON.stringify(FAKE_JOIN)
      : JSON.stringify([{ x: 90, y: 250, w: 300, h: 300, cx: 240, cy: 400, r: 150, stroke: 3, shown: 1 }])),
  });
  const canvas = { getContext: () => ({}), addEventListener: () => {} };
  return { calls, factory, canvas };
}

test("saved rotation (F184): the shell stages it before power-on, and a dist without the binding refuses", async () => {
  const { CanaryEmulator } = await shell();
  const order = (calls) => calls.map((c) => c[0]).filter((n) => n === "emu_preset_rotation" || n === "emu_power_on");
  // Staged with the value asked, before the power button.
  let m = fakeModule();
  await new CanaryEmulator(m.factory, { canvas: m.canvas }).start({ firstMeeting: true, rotation: 1 });
  assert.deepStrictEqual(order(m.calls), ["emu_preset_rotation", "emu_power_on"]);
  assert.deepStrictEqual(m.calls.find((c) => c[0] === "emu_preset_rotation"), ["emu_preset_rotation", 1]);
  // After every NVS preseed, too: the firmware stores it over the staged flash.
  const names = m.calls.map((c) => c[0]);
  assert.ok(names.lastIndexOf("emu_nvs_preseed_hex") < names.indexOf("emu_preset_rotation"));
  // No rotation asked, none staged (the native walks and the Lab boot as before).
  m = fakeModule();
  await new CanaryEmulator(m.factory, { canvas: m.canvas }).start({});
  assert.deepStrictEqual(order(m.calls), ["emu_power_on"]);
  // A dist built before the binding, or a refused value: no boot at all.
  m = fakeModule({ preset: false });
  await assert.rejects(new CanaryEmulator(m.factory, { canvas: m.canvas }).start({ rotation: 1 }),
    /no emu_preset_rotation \(rebuild it\)/);
  assert.ok(!m.calls.some((c) => c[0] === "emu_power_on"), "an old dist must not boot unturned");
  m = fakeModule({ presetAnswer: 0 });
  await assert.rejects(new CanaryEmulator(m.factory, { canvas: m.canvas }).start({ rotation: 1 }), /refused rotation 1/);
  assert.ok(!m.calls.some((c) => c[0] === "emu_power_on"));
  // The halo's circle, decoded; absent from an old dist, it throws.
  m = fakeModule();
  let emu = new CanaryEmulator(m.factory, { canvas: m.canvas });
  await emu.start({});
  assert.deepStrictEqual(await emu.screenArcs(),
    [{ x: 90, y: 250, w: 300, h: 300, cx: 240, cy: 400, r: 150, stroke: 3, shown: true }]);
  m = fakeModule({ arcs: false });
  emu = new CanaryEmulator(m.factory, { canvas: m.canvas });
  await emu.start({});
  await assert.rejects(emu.screenArcs(), /no emu_screen_arcs \(rebuild it\)/);
  // The Join layout the firmware names (onboard_ui_join_layout), decoded;
  // absent from an old dist, it throws rather than holding nothing.
  m = fakeModule();
  emu = new CanaryEmulator(m.factory, { canvas: m.canvas });
  await emu.start({});
  assert.deepStrictEqual(await emu.onboardJoin(), FAKE_JOIN);
  m = fakeModule({ join: false });
  emu = new CanaryEmulator(m.factory, { canvas: m.canvas });
  await emu.start({});
  await assert.rejects(emu.onboardJoin(), /no emu_onboard_join \(rebuild it\)/);
});

test("turned wiring (F184): the firmware wears the saved rotation, the glass turns with LVGL, the probe walks it", () => {
  // The firmware: LVGL 8's dash branch turns the display; the emulator stores
  // the rotation through the settings store before setup() reads it back.
  const port = read(join(REPO, "firmware/projects/canary-display/src/ui/lvgl_port.cpp"));
  const v8 = /#if defined\(CD_FLAVOR_DASH\) && LVGL_VERSION_MAJOR < 9([\s\S]*?)#endif/.exec(port)?.[1] || "";
  assert.ok(v8.includes("s_disp_drv.sw_rotate = 1;") && v8.includes("lv_disp_set_rotation(d, r);"),
    "lvgl_port_set_rotation turns LVGL 8's display on the dash glass");
  assert.ok(v8.includes("case canary::glass::ROT_PORTRAIT:      r = LV_DISP_ROT_90;"));
  const emuMain = read(join(ROOT, "emulator/src/emu_main.cpp"));
  assert.match(emuMain, /EMSCRIPTEN_KEEPALIVE int emu_preset_rotation\(int rot\)/);
  const body = /int main\(\) \{([\s\S]*?)\n\}/.exec(emuMain)?.[1] || "";
  assert.ok(body.indexOf("save_rotation(g_saved_rotation)") > body.indexOf("while (!g_power)") &&
    body.indexOf("save_rotation(g_saved_rotation)") < body.indexOf("setup();"),
  "the saved rotation is stored after power-on and before setup()");
  for (const step of ["settings_init();", "settings_mut().rotation = (uint8_t)rot;", "settings_mark_dirty();", "settings_loop("]) {
    assert.ok(emuMain.includes(step), `save_rotation goes through the settings store: ${step}`);
  }
  // main.cpp wears the saved rotation once in setup(), after the glass comes
  // up and BEFORE the splash and the onboarding (what the preset relies on;
  // the turned walk's framesOnGlass holds the same order off the canvas).
  const mainCpp = read(join(REPO, "firmware/projects/canary-display/src/main.cpp"));
  const setupBody = /\nvoid setup\(\) \{([\s\S]*?)\n\}\n/.exec(mainCpp)?.[1] || "";
  const wear = "canary::ui::lvgl_port_set_rotation(canary::glass::settings().rotation);";
  const at = (needle) => setupBody.indexOf(needle);
  assert.strictEqual(setupBody.split(wear).length - 1, 1, "setup() wears the saved rotation exactly once");
  assert.ok(at("g_display_ok = canary::ui::lvgl_port_init();") >= 0 && at("canary::ui::splash_play(") >= 0 &&
    at("canary::net::provision_run(") >= 0, "setup() still names lvgl_port_init, splash_play and provision_run");
  assert.ok(at(wear) > at("g_display_ok = canary::ui::lvgl_port_init();"), "the rotation is worn after lvgl_port_init()");
  assert.ok(at(wear) < at("canary::ui::splash_play("), "the rotation is worn before the splash");
  assert.ok(at(wear) < at("canary::net::provision_run("), "the rotation is worn before the onboarding");
  // The glass follows what LVGL did to the pixels, and tells the page its shape.
  const hal = read(join(ROOT, "emulator/src/emu_hal_display.cpp"));
  assert.ok(hal.includes("_lv_refr_get_disp_refreshing()") && hal.includes("d->driver->sw_rotate"));
  assert.ok(hal.includes("js_display_ready(g_view_w, g_view_h, kRoundMask);"));
  assert.match(hal, /int emu_fb_width\(void\) \{ return g_view_w; \}/);
  const binding = read(join(ROOT, "emulator/src/emu_bindings.cpp"));
  assert.match(binding, /EMSCRIPTEN_KEEPALIVE const char\* emu_screen_arcs\(void\)/);
  assert.ok(binding.includes("lv_obj_check_type(obj, &lv_arc_class)"));
  // The Join layout the firmware evaluates (onboard_ui_join_layout), with the
  // header's stroke and corner radius: what the probe holds the card and arc to.
  assert.match(binding, /EMSCRIPTEN_KEEPALIVE const char\* emu_onboard_join\(void\)/);
  assert.ok(binding.includes("canary::ui::onboard_ui_join_layout(&b)") &&
    binding.includes("canary::ui::onboardlayout::kRingStroke") && binding.includes("canary::ui::onboardlayout::kCardRadius"));
  // The page: the harness passes only 0..3 and the shell stages it.
  const harness = read(join(ROOT, "emulator/web/harness.js"));
  assert.ok(harness.includes("const ROTATIONS = { 0: 0, 1: 1, 2: 2, 3: 3 };") &&
    harness.includes("rotation: rotationParam === null ? null : ROTATIONS[rotationParam],"));
  // ...and logs every shape the firmware announces with the frames drawn by then.
  assert.ok(/onDisplayReady: \(w, h, round\) => \{\s*state\.shapes\.push\(\{ w, h, frames: state\.flushes \}\);/.test(harness) &&
    harness.includes("onFrame: () => { state.flushes++; },") && harness.includes("shapes: [] };"),
  "the harness logs each announced glass shape with the frame count");
  // The probe walks the turned dash and holds the new reads on it.
  const probe = read(join(__dirname, "onboard_probe.mjs"));
  // The turned glass is derived, not typed: turned_glass.mjs reads each
  // flavor's pin map and turn from the sources (held in its own test below),
  // and the probe walks the ones whose flavor has a dist bundle.
  assert.ok(probe.includes("const TURNED = turnedGlasses(await readTurnedSources(ROOT, readFile)).filter((t) => RUN.includes(t.flavor));"),
    "TURNED comes from turned_glass.mjs, read from the sources");
  assert.ok(!/\bglass: \{ w: \d+/.test(probe) && !/\brotation: \d/.test(probe), "the probe types no glass or turn of its own");
  assert.ok(probe.includes("const turnArg = turn ? `&rotation=${turn.rotation}&timescale=${SPLASH_SCALE}` : \"\";"));
  assert.ok(probe.includes("check(cv[0] === turn.glass.w && cv[1] === turn.glass.h,"), "the turned walk checks the glass's size");
  // Every frame on one glass, on a turned walk the turned one from the first
  // frame: checked at the first-boot line and at the end of the walk.
  assert.ok(probe.includes("const bad = framesOnGlass(st.shapes, st.frames, turn ? turn.glass : null);") &&
    probe.includes('const framesAtBoot = await holdFrames("by the first-boot line");') &&
    probe.includes('const framesAtEnd = await holdFrames("by the end of the walk");'));
  // The card and halo held to the layout, the stroke inked, on both Join reads.
  for (const needle of ["const bad = cardAtLayout(joinCardNow, layout) || haloAtLayout(halo, layout);",
    "const ink = haloInked(await onCanvas(haloInk, [halo]), 0, 3);",
    "const h = cardInHalo(joinCardNow, halo, { corners: turn.corners, cornerR: CARD_RADIUS });",
    'if (turn) inHalo = await holdJoin("the join scene", card);',
    'if (turn) await holdJoin("with the stuck-phone hint up", cardHint);']) {
    assert.ok(probe.includes(needle), `the turned walk holds the Join layout: ${needle}`);
  }
  // The QR upright on every walk, before and after the hint.
  assert.ok(probe.includes("const upright = qrUpright(finders);") && probe.includes("const uprightHint = qrUpright("));
  assert.ok(probe.includes("const helloLines = linesOnGlass(st.labels, st.glass) || linesCut(st.labels);") &&
    probe.includes("const pjLines = linesOnGlass(st.labels, st.glass) || linesCut(st.labels);"),
  "every bird read also holds the lines");
  assert.ok(probe.includes("const off = linesOnGlass(ls, glass) || linesCut(ls) ||\n      linesInked(await onCanvas(linesInk, [], \"window.__emu.screenLabels()\"));"),
    "every scene read holds the lines, and their ink");
  for (const scene of ['await holdLines("the join scene");', 'await holdLines("the join scene with the stuck-phone hint");',
    'await holdLines("the phone-joined scene");', "await holdLines(`after a ${reason} failure`);",
    "await holdLines(`${scene}: the Connecting scene`);", 'await holdLines("the face after onboarding");']) {
    assert.ok(probe.includes(scene), `the probe holds the lines: ${scene}`);
  }
  // Connecting after both joins it can read, and Success through the hop.
  assert.ok(probe.includes('await readConnecting("joining with a wrong key");') &&
    probe.includes('await readConnecting("joining with the right key");'));
  assert.ok(probe.includes("const bad = linesOnGlass(st.labels, st.glass) || linesCut(st.labels) || birdOnGlass(st);") &&
    probe.includes('check(successReads > 0, "the Success scene was gone before the probe could read it");'));
  assert.ok(probe.includes('if (WALK !== "native") for (const t of TURNED) { await walkHarness(t.flavor, t);'));
  // CI: the native glass test runs where an earlier red cannot skip it —
  // after the third-party cache is restored, before every step that reads
  // the committed dist (a stale dist turns the probes red, and GitHub skips
  // each later step) — and fetches build.sh's own pinned LVGL on a cold
  // cache, where build.sh will find it.
  const wf = read(join(REPO, ".github/workflows/canary-local.yml"));
  const gt = wf.indexOf("bash canary-local/emulator/test/glass_turn.sh");
  assert.ok(gt > wf.indexOf("- name: Cache third-party sources"), "glass_turn.sh runs after the third-party cache");
  for (const later of ["node canary-local/tests/boot_probe.mjs", "node canary-local/tests/onboard_probe.mjs",
    "node canary-local/tests/csp_probe.mjs", "./build.sh all", "Dist drift check"]) {
    assert.ok(wf.indexOf(later) > gt, `glass_turn.sh runs before ${later}`);
  }
  const gtStep = /- name: Turned glass[\s\S]*?run: bash canary-local\/emulator\/test\/glass_turn\.sh\n/.exec(wf)?.[0] || "";
  assert.ok(gtStep && !/^\s+if:/m.test(gtStep), "the step runs unconditionally");
  const gtSh = read(join(ROOT, "emulator/test/glass_turn.sh"));
  assert.ok(gtSh.includes(`sed -n 's/^LVGL_TAG="\\([^"]*\\)"$/\\1/p' "$EMU/build.sh"`) &&
    gtSh.includes('git clone --depth 1 --branch "$tag" https://github.com/lvgl/lvgl.git "$EMU/third_party/lvgl"'),
  "glass_turn.sh fetches build.sh's LVGL_TAG into build.sh's third_party/lvgl when it is absent");
  const buildSh = read(join(ROOT, "emulator/build.sh"));
  assert.ok(buildSh.includes('if [[ ! -d "$TP/lvgl" ]]; then') && /^LVGL_TAG="v8\.4\.\d+"$/m.test(buildSh),
    "build.sh reuses a checkout already at third_party/lvgl, and pins an 8.4 tag");
});

test("turned glasses (F204, F206): turned_glass.mjs reads each turned flavor's panel and turn from the sources", async () => {
  const T = await import("./turned_glass.mjs");
  const fs = require("node:fs/promises");
  const src = await T.readTurnedSources(REPO, fs.readFile);
  // The real tree: the dash turned portrait in software, the nightlight stood
  // on its edge in hardware — each on the pin map build.sh wires for it.
  assert.deepStrictEqual(T.turnedGlasses(src), [
    { flavor: "dash", rotation: 1, name: "portrait", panel: { w: 800, h: 480 }, glass: { w: 480, h: 800 }, corners: false },
    { flavor: "nightlight", rotation: 1, name: "landscape", panel: { w: 180, h: 320 }, glass: { w: 320, h: 180 }, corners: true },
  ]);
  assert.strictEqual(T.flavorBoard(src.buildSh, "dash"), "waveshare-esp32s3-lcd43");
  assert.strictEqual(T.flavorBoard(src.buildSh, "nightlight"), "waveshare-esp32c3-lcd147");
  assert.strictEqual(T.flavorBoard(src.buildSh, "watch"), "xiao-esp32s3-round");
  assert.strictEqual(T.flavorBoard(src.buildSh, "nosuch"), null);
  // Derived, not typed: move a fact in the sources and the table follows.
  const edit = (patch) => T.turnedGlasses({ ...src, ...patch });
  assert.strictEqual(edit({ glassSettings: src.glassSettings.replace(/\bROT_PORTRAIT\s*=\s*\d+/, "ROT_PORTRAIT = 3") })[0].rotation, 3);
  assert.deepStrictEqual(edit({ orientation: src.orientation.replace(/\bR90\s*=\s*\d+/, "R90 = 2") })[1].glass,
    { w: 180, h: 320 }, "an even turn keeps the panel's sides");
  const wider = (board) => src.pinsH(board).replace(/^#define LCD_WIDTH\s+\d+/m, "#define LCD_WIDTH 200");
  assert.deepStrictEqual(edit({ pinsH: wider })[1].glass, { w: 320, h: 200 });
  // ...and a fact it cannot read fails, naming it.
  assert.throws(() => edit({ buildSh: src.buildSh.replace('PINS_DIR="$FW/boards/waveshare-esp32c3-lcd147/pins"', "") }),
    /no pin map for the nightlight flavor/);
  assert.throws(() => edit({ orientation: src.orientation.replace(/\bR90\s*=\s*\d+/, "RIGHT = 1") }), /Orient::R90/);
  assert.throws(() => edit({ glassSettings: "" }), /ROT_PORTRAIT/);
  assert.throws(() => edit({ pinsH: () => "" }), /LCD_WIDTH/);
});

test("splash coverage (F206): the turned walk must see the bird and every line kHello types, whole", async () => {
  const T = await import("./turned_glass.mjs");
  const lines = T.helloLines(read(join(REPO, "firmware/common/story/story_scripts.h")));
  assert.strictEqual(lines[0], "Oh! Hello.");
  assert.ok(lines.length >= 8 && lines.includes("Not that kind of bird. Call me %s."), "every typed beat, the pseudonym's too");
  assert.throws(() => T.helloLines("nothing here"), /kHelloBeats/);
  assert.ok(T.isWholeLine("Not that kind of bird. Call me 4f2a1c9e.", "Not that kind of bird. Call me %s."));
  assert.ok(!T.isWholeLine("Not that kind of bird. Call me", "Not that kind of bird. Call me %s."), "half typed is not whole");
  assert.ok(!T.isWholeLine("Oh! Hello", "Oh! Hello."));
  const label = (text, extra = {}) => ({ text, shown: true, opa: 255, ...extra });
  const typed = (text) => ({ labels: [label(text.replace("%s", "4f2a1c9e"))], bird: { shown: true } });
  const all = lines.map(typed);
  assert.deepStrictEqual(T.splashCoverage(all, lines), { reads: lines.length, bird: lines.length, seen: lines, missing: [] });
  // A run that left before the last line, a line seen only half typed, a
  // line only on a hidden or faded label, and no bird: each is named.
  const short = T.splashCoverage(all.slice(0, -1), lines);
  assert.deepStrictEqual(short.missing, [lines[lines.length - 1]]);
  const half = T.splashCoverage([...all.slice(1), { labels: [label("Oh! Hel")], bird: null }], lines);
  assert.deepStrictEqual(half.missing, ["Oh! Hello."]);
  const hidden = T.splashCoverage([...all.slice(1), { labels: [label("Oh! Hello.", { shown: false })] }], lines);
  assert.deepStrictEqual(hidden.missing, ["Oh! Hello."]);
  const faded = T.splashCoverage([...all.slice(1), { labels: [label("Oh! Hello.", { opa: 0 })] }], lines);
  assert.deepStrictEqual(faded.missing, ["Oh! Hello."]);
  assert.strictEqual(T.splashCoverage(lines.map((l) => ({ labels: [label(l.replace("%s", "x1"))], bird: null })), lines).bird, 0);
});

test("nightlight flavor (F204): build.sh builds it, the HAL turns its panel, the preset stages its own key, and the lists agree", () => {
  const build = read(join(ROOT, "emulator/build.sh"));
  const branch = /elif \[\[ "\$FLAVOR" == "nightlight" \]\]; then\n([\s\S]*?)\nelse\n/.exec(build)?.[1] || "";
  assert.ok(branch.includes('PINS_DIR="$FW/boards/waveshare-esp32c3-lcd147/pins"') &&
    branch.includes('CFG_DIR="$FW/configs/canary-display/nightlight"'), "the nightlight's pin map and config");
  // The env's lean budget, for the nightlight alone (the faces its glass wears).
  const ini = read(join(REPO, "firmware/envs/platformio/canary-display.ini"));
  const env = /\[env:canary-display-nightlight-c3\]([\s\S]*?)(?=\n\[env:|$)/.exec(ini)?.[1] || "";
  assert.ok(env.includes("-DCD_LEAN_BUILD=1"), "the env builds lean");
  assert.match(build, /if \[\[ "\$FLAVOR" == "nightlight" \]\]; then\n  DEFINES\+=\(-DCD_LEAN_BUILD=1\)\nfi/);
  assert.strictEqual(build.split("+=(-DCD_LEAN_BUILD=1)").length - 1, 1, "no other flavor builds lean");
  assert.ok(read(join(REPO, "firmware/configs/canary-display/nightlight/config.h")).includes("#define CD_NIGHTLIGHT"));
  // The flavor lists agree: every display flavor build.sh wires (and builds
  // in `all`) is a harness entry loading its bundle by build.sh's factory.
  const wired = [...build.matchAll(/^\s{2}([a-z0-9]+)\)\s+EXPORT_NAME="(createCanaryEmu[A-Za-z0-9]+)" ;;$/gm)]
    .map((m) => [m[1], m[2]]);
  const dash = /^\s{2}\*\)\s+EXPORT_NAME="(createCanaryEmuDash)" ;;$/m.exec(build)?.[1];
  assert.ok(dash, "the dash is the case's default");
  const fromBuild = Object.fromEntries([...wired, ["dash", dash]]);
  const all = /if \[\[ "\$FLAVOR" == "all" \]\]; then([\s\S]*?)exit 0/.exec(build)?.[1] || "";
  for (const f of Object.keys(fromBuild)) assert.ok(all.includes(`"$0" ${f}\n`), `build.sh all builds ${f}`);
  const harness = read(join(ROOT, "emulator/web/harness.js"));
  const listed = Object.fromEntries([...harness.matchAll(/^\s{2}([a-z0-9]+):\s*\{ src: "\.\.\/dist\/canary-display-([a-z0-9]+)\.js",\s*factory: "([A-Za-z0-9]+)" \},$/gm)]
    .map((m) => { assert.strictEqual(m[1], m[2], `${m[1]} loads its own bundle`); return [m[1], m[3]]; }));
  assert.deepStrictEqual(listed, fromBuild, "harness.js FLAVORS is build.sh's display flavors, factory for factory");
  assert.strictEqual(fromBuild.nightlight, "createCanaryEmuNightlight");
  // The HAL: the hardware turn reshapes the framebuffer to the logical frame
  // (flushes land unturned), and no IMU answers.
  const hal = read(join(ROOT, "emulator/src/emu_hal_display.cpp"));
  const nl = [...hal.matchAll(/#ifdef CD_NIGHTLIGHT\n([\s\S]*?)#endif/g)].map((m) => m[1]).join("\n");
  assert.match(nl, /void display_set_rotation\(uint8_t rot\) \{\s*if \(!g_fb\) return;[^\n]*\n\s*panel_turn\(rot\);\s*\}/);
  assert.ok(nl.includes("g_view_w = side ? EMU_H : EMU_W;") && nl.includes("js_display_ready(g_view_w, g_view_h, kRoundMask);"),
    "a turn reshapes the glass and tells the page");
  assert.ok(nl.includes("put565(g_fb + ((size_t)fy * g_view_w + fx) * 4, *s);"), "a flush lands in the logical frame");
  assert.match(nl, /bool imu_init\(\) \{[\s\S]*?return false;\s*\}/);
  // CI's native glass test builds the nightlight config too, and runs it.
  const gtSh = read(join(ROOT, "emulator/test/glass_turn.sh"));
  assert.ok(gtSh.includes('NL_CFG="$FW/configs/canary-display/nightlight"') &&
    gtSh.includes('NL_PINS="$FW/boards/waveshare-esp32c3-lcd147/pins"') &&
    /\n"\$OUT\/nl_glass_turn_test"\n?$/.test(gtSh), "glass_turn.sh builds and runs the nightlight's hardware turn");
  const gtTest = read(join(ROOT, "emulator/test/glass_turn_test.cpp"));
  assert.ok(gtTest.includes("canary::hal::display_set_rotation(rot);\n    canary::ui::lvgl_port_set_panel_rotation(rot);") &&
    gtTest.includes("for (uint8_t rot : {1, 3, 2, 0, 1}) check_panel_glass(rot, true);"),
  "the native test turns the nightlight's panel the way main.cpp does, every way");
  // The preset: the nightlight's own key, through its own setter, after power-on
  // and before setup() (the order the dash's test above holds for main()).
  const emuMain = read(join(ROOT, "emulator/src/emu_main.cpp"));
  const save = /void save_rotation\(int rot\) \{([\s\S]*?)\n\}/.exec(emuMain)?.[1] || "";
  assert.match(save, /#ifdef CD_NIGHTLIGHT\s*canary::care::nightlight_begin\(\);\s*canary::care::nightlight_set_rotation\(\(uint8_t\)rot\);\s*#else/);
  const glue = read(join(REPO, "firmware/projects/canary-display/src/care/nightlight.cpp"));
  assert.ok(glue.includes('constexpr const char* NVS_NS = "scv-nl";') && glue.includes('put_u8("rot", s_rot);') &&
    glue.includes('s_rot = (uint8_t)(p.getUChar("rot", 0) & 3);'), "nightlight_set_rotation writes, and begin reads, scv-nl/rot");
  // main.cpp: the nightlight's prefs load before the glass, and its saved
  // rotation is worn once, after lvgl_port_init and before the splash and
  // the onboarding — the order the turned walk's framesOnGlass holds.
  const mainCpp = read(join(REPO, "firmware/projects/canary-display/src/main.cpp"));
  const setupBody = /\nvoid setup\(\) \{([\s\S]*?)\n\}\n/.exec(mainCpp)?.[1] || "";
  const at = (needle) => setupBody.indexOf(needle);
  const wear = "canary::hal::display_set_rotation(rot0);\n      canary::ui::lvgl_port_set_panel_rotation(rot0);";
  assert.strictEqual(setupBody.split(wear).length - 1, 1, "setup() wears the nightlight's rotation exactly once");
  assert.ok(at("canary::care::nightlight_begin();") >= 0 && at("canary::care::nightlight_begin();") < at(wear));
  assert.ok(at(wear) > at("g_display_ok = canary::ui::lvgl_port_init();"), "after lvgl_port_init()");
  assert.ok(at(wear) < at("canary::ui::splash_play(") && at(wear) < at("canary::net::provision_run("),
    "before the splash and the onboarding");
});

test("splash and turned boots (F206): the harness slows the clock from power-on; the probes read the turned splash and boot the turned glasses", () => {
  const harness = read(join(ROOT, "emulator/web/harness.js"));
  assert.ok(harness.includes('const timeScaleParam = q.get("timescale");') &&
    harness.includes("if (timeScale !== null && !(Number.isFinite(timeScale) && timeScale > 0 && timeScale <= 64)) {") &&
    /rotation: rotationParam === null \? null : ROTATIONS\[rotationParam\],\n\s*timeScale,\n/.test(harness),
  "?timescale= is checked and handed to start()");
  const shell = read(join(ROOT, "emulator/web/emu-shell.js"));
  const start = /async start\(\{[\s\S]*?\n  \}\n/.exec(shell)?.[0] || "";
  assert.ok(start.includes("rotation = null, timeScale = null } = {}"));
  const scale = start.indexOf("if (timeScale !== null && timeScale !== undefined) this.c.timeScale(timeScale);");
  assert.ok(scale > 0 && scale < start.indexOf("this.c.power();"), "the clock is slowed before power-on");
  // The turned walk reads the splash from the first frame to the first-boot
  // line, holds every read, and must have seen it whole.
  const probe = read(join(__dirname, "onboard_probe.mjs"));
  const splash = /let splashRead = null;\n    if \(turn\) \{([\s\S]*?)\n    \}\n/.exec(probe)?.[1] || "";
  assert.ok(splash.includes("window.__state.flushes > 0"), "from the firmware's first frame");
  assert.ok(splash.includes("while (!(await serial()).includes('First boot - onboarding AP \"SecuraCV-')) {"));
  assert.ok(splash.includes("check(st.glass.w === turn.glass.w && st.glass.h === turn.glass.h,"));
  assert.ok(splash.includes("const bad = linesOnGlass(st.labels, st.glass) || linesCut(st.labels) || birdPerch(st);"));
  assert.ok(splash.includes("splashRead = splashCoverage(reads, HELLO_LINES);") &&
    splash.includes("check(splashRead.bird > 0 && splashRead.missing.length === 0,"), "the splash must have been read whole");
  assert.ok(splash.includes("await E(() => window.__emu.setTimeScale(1));"), "the clock is restored after the splash");
  assert.ok(probe.includes('const HELLO_LINES = helloLines(await readFile(join(ROOT, "firmware/common/story/story_scripts.h"), "utf8"));'));
  assert.match(probe, /const SPLASH_SCALE = 0\.\d+;/);
  // boot_probe boots each turned glass whose flavor it boots, and holds the
  // turned size, every frame on that glass, and the face's bird.
  const boot = read(join(__dirname, "boot_probe.mjs"));
  assert.ok(boot.includes("const TURNED = turnedGlasses(await readTurnedSources(ROOT, readFile)).filter((t) => RUN.includes(t.flavor));"));
  assert.ok(boot.includes("for (const t of TURNED) { await bootOnce(t.flavor, t); booted.push(`${t.flavor}@${t.name}`); }"));
  const once = /async function bootOnce\(flavor, turn = null\) \{([\s\S]*?)\n\}\n/.exec(boot)?.[1] || "";
  assert.ok(once.includes("const turnArg = turn ? `&rotation=${turn.rotation}` : \"\";"));
  assert.ok(once.includes("if (st.glass.w !== turn.glass.w || st.glass.h !== turn.glass.h) {") &&
    once.includes("const frames = framesOnGlass(st.shapes, st.flushes, turn.glass);") &&
    once.indexOf("const perch = birdPerch(st);") > once.indexOf("const frames = framesOnGlass("),
  "a turned boot holds its glass, its frames and its face's bird");
});

test("dist drift (F204): the drift check fails on a bundle this tree builds that the committed dist lacks", () => {
  // Run the workflow step's own script in a scratch repository: one bundle
  // committed, then a new flavor's bundle left untracked (what build.sh all
  // leaves when a flavor is added and the dist is not rebuilt). git diff
  // alone cannot see an untracked file, so the old step passed it.
  const wf = read(join(REPO, ".github/workflows/canary-local.yml"));
  const step = /      - name: Dist drift check[^\n]*\n        run: \|\n([\s\S]*?)(?=\n      - name:)/.exec(wf)?.[1];
  assert.ok(step, "canary-local.yml has its dist drift step");
  const script = step.split("\n").map((l) => l.replace(/^ {10}/, "")).join("\n");
  const { mkdtempSync, mkdirSync, writeFileSync, rmSync } = require("node:fs");
  const dir = mkdtempSync(join(require("node:os").tmpdir(), "drift-"));
  try {
    const sh = (cmd) => spawnSync("bash", ["-c", cmd], { cwd: dir, encoding: "utf8" });
    mkdirSync(join(dir, "canary-local/emulator/dist"), { recursive: true });
    writeFileSync(join(dir, "canary-local/emulator/dist/canary-display-watch.js"), "watch bytes\n");
    assert.strictEqual(sh("git init -q && git -c user.email=t@t -c user.name=t add . && " +
      "git -c user.email=t@t -c user.name=t commit -qm base").status, 0);
    writeFileSync(join(dir, "drift.sh"), `set -eo pipefail\n${script}\n`);
    // the committed dist matches: the step passes
    assert.strictEqual(sh("bash drift.sh").status, 0, "a dist that matches passes");
    // a changed bundle fails, as it always did
    writeFileSync(join(dir, "canary-local/emulator/dist/canary-display-watch.js"), "watch bytes, rebuilt\n");
    assert.strictEqual(sh("bash drift.sh").status, 1, "a changed bundle fails");
    sh("git checkout -q -- canary-local/emulator/dist");
    // a new flavor's bundle the committed dist lacks fails, named, and the
    // binary patch carries it
    writeFileSync(join(dir, "canary-local/emulator/dist/canary-display-nightlight.js"), "nightlight bytes\n");
    const r = sh("bash drift.sh");
    assert.strictEqual(r.status, 1, "an untracked new bundle fails the drift check");
    assert.match(r.stdout, /bundles this tree builds that the committed dist lacks[\s\S]*canary-display-nightlight\.js/);
    const patch = r.stdout.split("\n").filter((l) => l.startsWith("PATCH:")).map((l) => l.slice(6)).join("");
    const diff = require("node:zlib").gunzipSync(Buffer.from(patch, "base64")).toString("utf8");
    assert.match(diff, /new file mode[\s\S]*canary-display-nightlight\.js/, "the patch adds the new bundle");
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
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
