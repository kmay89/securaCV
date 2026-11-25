// canary-local/tests/canary_local.test.js — Node tests for the page's
// DOM-free logic (repo convention: CI tests the exact shipped source).
//
//   node --test canary-local/tests/
//
// Coverage: MQTT wildcard matching (the shell's broker semantics), the
// witness canonical/signature format (must equal what trust.cpp
// rebuilds before Ed25519::verify — pinned here as a golden string),
// LED cadence translation, registry ↔ dist artifact integrity, the cards'
// status prose against the train, and the vendored Witness Wall against the
// fleet contract.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync, readdirSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");

async function importShell() {
  return import("../emulator/web/emu-shell.js");
}
async function importGuides() {
  return import("../assets/guides.js");
}

test("MQTT single-level wildcard matches the fleet topics", async () => {
  const { topicMatches } = await importShell();
  assert.ok(topicMatches("securacv/+/status", "securacv/canary_wap_garage/status"));
  assert.ok(topicMatches("securacv/+/chain", "securacv/x/chain"));
  assert.ok(!topicMatches("securacv/+/status", "securacv/x/health"));
  assert.ok(!topicMatches("securacv/+/status", "securacv/x/y/status"));
  assert.ok(!topicMatches("securacv/+/status", "other/x/status"));
  assert.ok(topicMatches("securacv/fleet/ack", "securacv/fleet/ack"));
  assert.ok(topicMatches("securacv/#", "securacv/a/b/c"));
});

test("witness chain canonical matches trust.cpp's locked format", async () => {
  // trust.cpp: snprintf("%s|v%d|chain|%s|%lu|%s", SIG_PREFIX="securacv-canary-sig",
  // SCHEMA_V=1, device_id, length, latest_hash). If this golden ever
  // breaks, the page's witnesses sign something the firmware won't
  // verify — and every ✓ on the emulated glass silently degrades.
  const id = "canary_vision_frontdoor";
  const len = 41;
  const hash = "ab".repeat(32);
  const canonical = `securacv-canary-sig|v1|chain|${id}|${len}|${hash}`;
  // Source-of-truth cross-check against the firmware tree:
  const trust = readFileSync(
    join(ROOT, "../firmware/projects/canary-display/src/trust.cpp"), "utf8");
  assert.match(trust, /securacv-canary-sig/);
  assert.match(trust, /%s\|v%d\|chain\|%s\|%lu\|%s/);
  assert.strictEqual(
    canonical,
    "securacv-canary-sig|v1|chain|canary_vision_frontdoor|41|" + "ab".repeat(32)
  );
});

test("simulated witness signs what the firmware verifies", async () => {
  const { SimWitness } = await importShell();
  const w = new SimWitness({ id: "t1", deviceType: "canary-sense", name: "T", room: "R" });
  const chain = await w.chainPayload();
  assert.strictEqual(typeof chain.length, "number");
  assert.match(chain.latest_hash, /^[0-9a-f]{64}$/);
  if (w.pubHex) {
    // WebCrypto Ed25519 available (Node ≥ 19): verify our own signature
    // over the exact canonical, round-tripping the b64url encoding.
    assert.match(w.pubHex, /^[0-9a-f]{64}$/, "health payload pubkey is 64-hex");
    assert.match(chain.sig, /^[A-Za-z0-9_-]{86}$/, "sig is b64url, no padding");
    const canonical = `securacv-canary-sig|v1|chain|t1|${chain.length}|${chain.latest_hash}`;
    const sigBytes = Buffer.from(chain.sig.replaceAll("-", "+").replaceAll("_", "/") + "==", "base64");
    const key = await crypto.subtle.importKey(
      "raw", Buffer.from(w.pubHex, "hex"), "Ed25519", false, ["verify"]);
    const ok = await crypto.subtle.verify(
      "Ed25519", key, sigBytes, new TextEncoder().encode(canonical));
    assert.ok(ok, "signature verifies over the canonical string");
  }
});

test("LED cadences: every documented grammar row translates", async () => {
  const { LED_GRAMMAR, ledSequence } = await importGuides();
  for (const g of LED_GRAMMAR) {
    const seq = ledSequence(g.pattern);
    assert.ok(seq.length >= 2, g.pattern);
    assert.ok(seq.every(([on, ms]) => typeof on === "boolean" && ms > 0));
  }
  // count-coded groups carry their count
  assert.strictEqual(ledSequence("groups of 5").filter(([on]) => on).length, 5);
  assert.strictEqual(ledSequence("groups of 2").filter(([on]) => on).length, 2);
});

test("concept entries are honesty-fenced: no emulator, real docs, sourced facts", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  const concepts = reg.devices.filter((d) => d.kind === "concept");
  assert.ok(concepts.length >= 1, "the fence guard concept is in the registry");
  for (const dev of concepts) {
    assert.match(dev.status, /coming soon/i, `${dev.id} says coming-soon to your face`);
    assert.ok(!dev.emulator, `${dev.id} claims no live firmware`);
    assert.ok(!dev.enclosure, `${dev.id} claims no printable enclosure`);
    for (const doc of dev.docs || [])
      assert.ok(existsSync(join(ROOT, "..", doc)), `${dev.id} doc exists: ${doc}`);
    const c = dev.concept;
    assert.ok(c?.idea && c.points?.length && c.plan?.length, `${dev.id} explains itself`);
    assert.ok(c.radio?.length >= 3, `${dev.id} carries researched radio facts`);
    assert.ok(c.sources?.length >= 2, `${dev.id} cites its research sources`);
    for (const s of c.sources)
      assert.match(s.url, /^https:\/\//, `${dev.id} source '${s.name}' is a real link`);
    assert.ok(
      dev.senses.every((x) => /concept/i.test(x)),
      `${dev.id}'s senses all admit they are concepts`);
  }
});

test("registry entries with emulators point at real artifacts", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  assert.ok(reg.devices.length >= 5);
  for (const dev of reg.devices) {
    if (!dev.emulator) continue;
    const mod = join(ROOT, dev.emulator.module);
    assert.ok(existsSync(mod), `${dev.id}: ${dev.emulator.module} exists`);
    const meta = JSON.parse(readFileSync(mod.replace(/\.js$/, ".meta.json"), "utf8"));
    assert.strictEqual(meta.fw_version, reg.fw_train,
      `${dev.id}: artifact fw (${meta.fw_version}) matches registry train (${reg.fw_train})`);
    const src = readFileSync(mod, "utf8");
    assert.ok(src.includes(dev.emulator.factory),
      `${dev.id}: factory ${dev.emulator.factory} exported by artifact`);
  }
});

// The Specs tab's Web row is a statement about a device's LAN surface, on a
// privacy product. Seven display cards said "first-boot portal only" while
// every canary-display flavor starts the glass mirror's WebServer on :80
// right after provisioning (main.cpp setup(), no #if around it) and
// advertises it over mDNS. Held here: the firmware still serves it on every
// flavor, and every display manifest's card names the port, each page the
// server registers, and its /api — never "only".
test("every display card's Web row names the glass mirror the firmware serves", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  const cards = new Map(reg.devices.map((d) => [d.id, d]));
  const fw = join(ROOT, "../firmware/projects/canary-display/src");
  // 1. unconditional: the init (setup) and the tick (loop) at #if depth 0
  const main = readFileSync(join(fw, "main.cpp"), "utf8").split("\n");
  let depth = 0;
  const seen = {};
  for (const line of main) {
    const t = line.trim();
    if (/^#\s*if(n?def)?\b/.test(t)) depth++;
    else if (/^#\s*endif\b/.test(t)) depth--;
    for (const call of ["glass_web_init()", "glass_web_tick("]) {
      if (!t.startsWith("//") && t.includes(`canary::net::${call}`)) seen[call] = depth;
    }
  }
  for (const call of ["glass_web_init()", "glass_web_tick("]) {
    assert.strictEqual(seen[call], 0,
      `main.cpp's ${call} is no longer unconditional — the display cards' Web row says every flavor serves :80`);
  }
  // 2. what it serves: the port and the GET pages outside /api
  const web = readFileSync(join(fw, "net/glass_web.cpp"), "utf8");
  const port = /new WebServer\((\d+)\)/.exec(web);
  assert.ok(port, "glass_web.cpp still constructs its WebServer");
  const pages = [...web.matchAll(/->on\("([^"]+)",\s*HTTP_GET/g)].map((m) => m[1])
    .filter((p) => !p.startsWith("/api/"));
  assert.ok(pages.includes("/") && web.includes('->on("/api/'), "glass_web.cpp route table parses");
  // 3. every display manifest's card says so
  const dir = join(ROOT, "../devices");
  let n = 0;
  for (const slug of readdirSync(dir).filter((d) => existsSync(join(dir, d, "device.json")))) {
    const m = JSON.parse(readFileSync(join(dir, slug, "device.json"), "utf8"));
    if (m.family !== "canary-display") continue;
    const card = cards.get(m.lab?.card || slug);
    assert.ok(card, `${slug}: its Lab card ${m.lab?.card || slug} is in registry.json`);
    const says = card.network.web;
    assert.doesNotMatch(says, /\bonly\b/, `${card.id}: Web row "${says}" understates the LAN surface`);
    assert.ok(says.includes(`:${port[1]}`), `${card.id}: Web row names port ${port[1]}`);
    for (const p of [...pages, "/api"]) {
      assert.ok(says.split(/[\s(),]+/).includes(p), `${card.id}: Web row names ${p}`);
    }
    n++;
  }
  assert.ok(n >= 9, `every display manifest checked (${n})`);
  // …and the Glass row beside it prints no raw null for a panel without touch
  const glassRow = readFileSync(join(ROOT, "assets/app.js"), "utf8").split("\n")
    .find((l) => l.includes('row("Glass"'));
  assert.ok(glassRow, "app.js specsView still has its Glass row");
  assert.match(glassRow, /dev\.glass\.touch \?/,
    "specsView prints glass.touch without testing it — a touchless panel reads 'touch null'");
  assert.ok(reg.devices.some((d) => d.glass && d.glass.touch === null), "a touchless panel is still carried");
});

test("fw_train matches the firmware tree's CANARY_FW_VERSION", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  const vh = readFileSync(
    join(ROOT, "../firmware/projects/canary-display/include/canary/version.h"), "utf8");
  const m = vh.match(/CANARY_FW_VERSION "([^"]+)"/);
  assert.ok(m, "version.h parses");
  assert.strictEqual(reg.fw_train, m[1]);
});

// A card's status is prose, and prose that names a firmware version drifts the
// moment the train moves: the Vision card still said "fw 2.2.0" with the
// unified train at 2.4.15, and the Lab's Specs tab (assets/app.js specsView,
// on Pages and in the desktop Lab) prints a card's status verbatim. (The
// website's lab.html renders this file too, but a released card only as the
// word "released", so it never showed the number.) The train lives in
// `fw_train` (held to version.h above); a status string may repeat it, never
// contradict it. A version counts with or without a space after "fw"/"v"/
// "firmware", and a two-part one counts once a prefix says it is firmware.
test("no card's status names a firmware version other than fw_train", () => {
  const reg = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
  const VERSION = /(?<![\w.])((?:firmware|fw|v)\s*v?)?(\d+\.\d+(?:\.\d+)?)(?!\.?\d)/gi;
  const versionsIn = (s) => [...s.matchAll(VERSION)]
    .filter(([, prefix, v]) => prefix || v.split(".").length === 3).map(([, , v]) => v);
  // The scanner itself, on the spellings a status could drift into.
  for (const [s, want] of [
    ["released, fw 2.2.0", ["2.2.0"]], ["released, fw2.2.0", ["2.2.0"]],
    ["released, fw v2.2.0.", ["2.2.0"]], ["released (2.2.0)", ["2.2.0"]],
    ["firmware 2.3.1-rc1", ["2.3.1"]], ["released, fw 2.2", ["2.2"]],
    ["phase 2 complete", []], ["a 1.5 m range, 11 tests", []],
  ]) assert.deepStrictEqual(versionsIn(s), want, `the version scanner reads "${s}" wrong`);
  let statuses = 0;
  for (const dev of reg.devices) {
    if (typeof dev.status !== "string") continue;
    statuses++;
    for (const v of versionsIn(dev.status)) {
      assert.strictEqual(v, reg.fw_train,
        `${dev.id}: status "${dev.status}" names fw ${v}, but the registry's train is ${reg.fw_train} ` +
        `— drop the version from the status (the card's train is fw_train), don't retype it`);
    }
  }
  assert.ok(statuses >= 5, `the registry's cards still carry a status (${statuses})`);
});

// Both apps iframe the website's Witness Wall emulator, vendored from the
// website (witness/PROVENANCE.txt). The fleet contract it reads is
// tvos/discovery/DISCOVERY.md: only `name` is required, and a silent `online`
// is NOT a presence claim. The website fixed its canonical copy after the apps
// had vendored it, and nothing here noticed, because
// scripts/check_witness_emulator_sync.sh compares the two app copies with each
// other, never with the contract. So, in both apps: replay the contract's own
// vectors through the three fleet-row `online` derivations (appear,
// witness:fleet, connect), and hold every other `online:` the emulator writes
// to a bare literal, so a new default spelled some other way cannot slip past
// the replay. A literal is the host's or the simulation's own claim; the
// /api/fleet poll adding a newcomer as `online: true` is one, and it is the
// website's to fix (it cannot be told from `just flashed` by shape).
test("both apps' vendored Witness Wall reads a silent `online` as offline", () => {
  const { vectors } = JSON.parse(readFileSync(
    join(ROOT, "../tvos/witness-core/tests/fixtures/fleet_contract_vectors.json"), "utf8"));
  assert.ok(vectors.length >= 3, "the contract vectors parse");
  for (const rel of ["witness/tv-emulator.js", "../desktop/src/witness/tv-emulator.js"]) {
    const src = readFileSync(join(ROOT, rel), "utf8");
    const sites = [...src.matchAll(/online:\s*([A-Za-z_$][\w$]*)\.online\s*([!=]==)\s*(true|false)\b/g)];
    assert.ok(sites.length >= 3,
      `${rel}: ${sites.length} fleet-row \`online\` derivations found (appear, witness:fleet, connect)`);
    for (const [expr, row, op, lit] of sites) {
      const derive = new Function(row, `return ${row}.online ${op} ${lit};`);
      for (const x of vectors) {
        const body = JSON.parse(x.input);
        const rows = Array.isArray(body) ? body : body.devices;
        rows.forEach((r, i) => assert.strictEqual(derive(r), x.normalized.devices[i].online,
          `${rel}: \`${expr}\` reads vector "${x.name}" row ${i} ${JSON.stringify(r)} wrong — ` +
          "a silent `online` is never a presence claim; re-vendor with scripts/vendor_witness_emulator.sh"));
      }
    }
    // Every `online:` value is one of the replayed derivations or a literal.
    const values = [...src.matchAll(/\bonline:\s*([^,}\n]*)/g)].map(([, v]) => v.trim());
    assert.ok(values.length >= sites.length, `${rel}: the \`online:\` scan found the derivations`);
    for (const v of values) {
      assert.match(v, /^(?:true|false|[A-Za-z_$][\w$]*\.online === true)$/,
        `${rel}: \`online: ${v}\` is neither a literal nor \`<row>.online === true\` — ` +
        "a silent `online` is never a presence claim");
    }
    assert.doesNotMatch(src, /online[^,;\n]*(!==\s*false|===\s*undefined\s*\?\s*true|\?\?\s*true)/,
      `${rel} still defaults a silent \`online\` to present in some other shape`);
  }
});

// The birth day (sweep A72). A Canary's /api/fleet row carries born_day (days
// since the Unix epoch, a UTC day) and born_exact, and the contract vectors
// carry a `born` line per row: what every reader must render, held to the
// Apple TV core's normalized pair by tvos/witness-core/tests/fleet_contract.rs.
// The vendored emulator renders it from the raw rows its live fleet is built
// from (witness:fleet from the native witness_discover command, or a connect),
// through one self-contained bornLine(row, locale). Lift that function out of
// both apps' copies and replay every row: "Born" only when the device says
// the day is exact, "First dated" otherwise, nothing without a day a Canary
// could stamp. A born day is a UTC day and the emulator formats it in UTC; CI
// runs in UTC, where a copy that slipped back to the local zone would print
// the same day and pass, so this test runs west of Greenwich (where the local
// day of a UTC midnight is the day before) and puts the zone back after.
test("both apps' vendored Witness Wall says Born only when the device says the day is exact", (t) => {
  const savedTZ = process.env.TZ;
  process.env.TZ = "Pacific/Honolulu";
  t.after(() => { if (savedTZ === undefined) delete process.env.TZ; else process.env.TZ = savedTZ; });
  const { vectors } = JSON.parse(readFileSync(
    join(ROOT, "../tvos/witness-core/tests/fixtures/fleet_contract_vectors.json"), "utf8"));
  const utcDay = (iso) => new Date(`${iso}T00:00:00Z`).toLocaleDateString("en-US",
    { timeZone: "UTC", year: "numeric", month: "short", day: "numeric" });
  for (const rel of ["witness/tv-emulator.js", "../desktop/src/witness/tv-emulator.js"]) {
    const src = readFileSync(join(ROOT, rel), "utf8");
    const fn = src.match(/\n {2}function bornLine\(row, locale\) \{[\s\S]*?\n {2}\}\n/);
    assert.ok(fn, `${rel} has no self-contained bornLine(row, locale) — re-vendor with scripts/vendor_witness_emulator.sh`);
    const bornLine = new Function(`${fn[0]}\nreturn bornLine;`)();
    let lines = 0;
    for (const x of vectors) {
      const body = JSON.parse(x.input);
      const rows = Array.isArray(body) ? body : body.devices;
      const born = x.born || rows.map(() => null);
      rows.forEach((r, i) => {
        const want = born[i] ? `${born[i].label} ${utcDay(born[i].utc_date)}` : "";
        assert.strictEqual(bornLine(r, "en-US"), want,
          `${rel}: vector "${x.name}" row ${i} ${JSON.stringify(r)} — re-vendor with scripts/vendor_witness_emulator.sh`);
        if (want) lines += 1;
      });
    }
    assert.ok(lines >= 4, `${rel}: the vectors pin Born and First dated lines (${lines})`);
    // The line reaches the live rows the wall draws from, both ways in.
    const fed = [...src.matchAll(/online: [A-Za-z_$][\w$]*\.online === true, born: bornLine\(([A-Za-z_$][\w$]*)\)/g)];
    assert.ok(fed.length >= 2, `${rel}: witness:fleet and connect both carry the row's birth day (${fed.length})`);
    // And both places the emulator shows a live row print it: a tile's toast,
    // and the connect panel's device list (escaped, since that one is HTML).
    // The toast says the tile's own state first: offline for an offline row,
    // presence only for a live one, the simulated chain for the demo alone.
    // A live row's toast is plain (no tick): nothing was checked.
    assert.match(src, /showToast\([A-Za-z_$][\w$]* \+ \([A-Za-z_$][\w$]* \? ' · offline' : liveFleet \? ' · online · presence only — not verified' : ' · online · chain intact'\) \+ \(([A-Za-z_$][\w$]*) && \1\.born \? ' · ' \+ \1\.born : ''\), !!liveFleet\)/,
      `${rel}: a live tile's toast lost its offline branch, its presence-only wording or its birth-day line, or grew back its tick — re-vendor with scripts/vendor_witness_emulator.sh`);
    // The script carries tv-emulator.js and highlight.js: any other import is
    // a file missing beside the copy, and a module that never runs.
    const imports = [...src.matchAll(/^import\b[^'"]*['"]([^'"]+)['"]/gm)].map((m) => m[1]);
    assert.deepStrictEqual(imports, ["./highlight.js"],
      `${rel}: imports a file the vendoring does not carry`);
    assert.match(src, /\$\{([A-Za-z_$][\w$]*)\.born \? ' · ' \+ esc\(\1\.born\) : ''\}/,
      `${rel}: the connect panel lost its escaped birth-day line — re-vendor with scripts/vendor_witness_emulator.sh`);
  }
});

// ── CI wiring: a test file here is a gate only once CI runs it ─────────────
// canary-local.yml names each file on its own `node --test` line (repo
// convention: no glob, no runner), so a new file is silently not a gate until
// someone lists it. scene_figures, body_dims and device_models sat unlisted
// while the Lab's prose (render_probe.mjs, the README, a workflow comment)
// said they held their guards. A comment naming a file does not run it: only
// `node --test` in command position counts.
test("CI runs every test file in this folder", () => {
  const wf = readFileSync(join(ROOT, "../.github/workflows/canary-local.yml"), "utf8");
  const ran = [];
  for (const raw of wf.split("\n")) {
    const line = raw.trim().replace(/^(?:-\s*)?run:\s*/, "");
    if (line.startsWith("#")) continue;
    for (const seg of line.split(/&&|\|\||;|\|/)) {
      const words = seg.trim().split(/\s+/);
      if (words[0] !== "node" || words[1] !== "--test") continue;
      for (const w of words.slice(2)) {
        if (w.startsWith("#")) break;
        if (!w.startsWith("-")) ran.push(w.replace(/^["']|["']$/g, ""));
      }
    }
  }
  const glob = (p) => new RegExp(`^${p.replace(/[.+?^${}()[\]\\]/g, "\\$&").replace(/\*/g, "[^/]*")}$`);
  const files = readdirSync(__dirname).filter((n) => /\.test\.m?js$/.test(n));
  assert.ok(files.includes("canary_local.test.js"), "the folder listing found this file");
  const unrun = files.filter((n) => !ran.some((p) => glob(p).test(`canary-local/tests/${n}`)));
  assert.deepStrictEqual(unrun, [],
    `run by no \`node --test\` line in .github/workflows/canary-local.yml: ${unrun.join(", ")} — ` +
    `list each in the logic-tests job's "Node tests" step`);
});

// ── the filament finish system (finishes.js) ───────────────────────────────
test("finishes: a curated two-tone set, Canary the bold default", async () => {
  const { FINISHES, activeFinish, setFinish } = await import("../assets/finishes.js");
  assert.ok(FINISHES.length >= 3, "at least Canary/Walnut/Graphite");
  assert.strictEqual(activeFinish().id, "canary", "Canary is the default (no storage in Node)");
  for (const f of FINISHES) {
    for (const k of ["shell", "shell2", "gasket", "beacon"])
      assert.ok(Array.isArray(f[k]) && f[k].length === 3, `${f.id}.${k} is an RGB triple`);
    assert.match(f.swatch, /^#[0-9a-f]{6}$/i, `${f.id} has a hex swatch`);
    // two-tone: the secondary is a genuinely different (darker) shade
    const lum = (c) => 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
    assert.ok(lum(f.shell2) < lum(f.shell) + 0.02, `${f.id}: secondary is not lighter than the body`);
  }
  // setting a finish is sticky and idempotent
  assert.strictEqual(setFinish("walnut").id, "walnut");
  assert.strictEqual(activeFinish().id, "walnut");
  assert.strictEqual(setFinish("nope").id, "walnut", "unknown id is ignored");
  setFinish("canary"); // restore for any later import consumers
});

test("finishes: finishColor cross-fades only filament roles, functional parts pass through", async () => {
  const { finishColor, setFinish, FINISHES } = await import("../assets/finishes.js");
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const canary = FINISHES.find((f) => f.id === "canary");
  setFinish("canary");
  await sleep(1650); // let any in-flight cross-fade settle (fadeDur 1500)
  // a settled finish returns the exact role color; unknown roles pass through
  assert.deepStrictEqual(finishColor("shell"), canary.shell);
  assert.deepStrictEqual(finishColor("beacon"), canary.beacon);
  assert.strictEqual(finishColor("glass"), null, "non-filament role is untouched");
  assert.strictEqual(finishColor(null), null);
  // a fresh pick begins from the previous color — the first frame of the
  // cross-fade is at (or extremely close to) where it was, not a hard jump
  const graphite = FINISHES.find((f) => f.id === "graphite");
  setFinish("graphite");
  const first = finishColor("shell");
  const dToOld = Math.hypot(...first.map((v, i) => v - canary.shell[i]));
  const dToNew = Math.hypot(...first.map((v, i) => v - graphite.shell[i]));
  assert.ok(dToOld < dToNew, "the fade starts nearer the outgoing color than the incoming one");
  setFinish("canary");
});

test("finishes: the showcase cycles then stops for good on a manual pick", async () => {
  const { startFinishShowcase, stopFinishShowcase, showcaseRunning, setFinish } =
    await import("../assets/finishes.js");
  assert.strictEqual(showcaseRunning(), false, "not running at rest");
  startFinishShowcase();
  assert.strictEqual(showcaseRunning(), true, "starts");
  setFinish("walnut");                 // a manual pick ends the showcase
  assert.strictEqual(showcaseRunning(), false, "a pick stops it");
  startFinishShowcase();
  stopFinishShowcase();                 // and it can be stopped directly
  assert.strictEqual(showcaseRunning(), false);
  setFinish("canary");
});

// Sweep A62: fleet.html#%E0 threw URIError at the end of boot(), because the
// deep link was decoded bare. The helper is run here as shipped (lifted out of
// app.js and evaluated), so the test is the code, not a description of it.
test("a malformed fleet.html deep link opens nothing instead of throwing (A62)", () => {
  const vm = require("node:vm");
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  const m = app.match(/^const deepLinkId = (\(hash\) => \{.*\});$/m);
  assert.ok(m, "app.js lost its one-line deepLinkId helper");
  const deepLinkId = vm.runInNewContext(m[1]);
  assert.strictEqual(deepLinkId("#%E0"), "", "a broken escape must open nothing, not throw");
  assert.strictEqual(deepLinkId("#%"), "");
  assert.strictEqual(deepLinkId("#canary-display"), "canary-display");
  assert.strictEqual(deepLinkId("#canary%2Ddisplay"), "canary-display");
  assert.strictEqual(deepLinkId(""), "");
  assert.match(app, /const target = deepLinkId\(location\.hash\);/,
    "boot() must read its deep link through deepLinkId, not a bare decodeURIComponent");
});

test("no Lab page decodes a visitor-typed address outside a try (the A62 class)", () => {
  // decodeURIComponent throws on a malformed escape, and an address is
  // whatever the visitor typed or pasted. Each call sits on the line of the
  // `try` that catches it, so a new one is visibly guarded or names itself here.
  // file -> why its decode is not held here (each entry must say why its input
  // is not an address). Empty: flash-core.js's releaseTagFromManifestUrl, the
  // last one out, decodes the active manifest URL, which a ?manifest= override
  // makes visitor-typed, so it is held like the rest.
  const ALLOWED = new Map([]);
  const offenders = [];
  for (const f of readdirSync(join(ROOT, "assets")).filter((n) => n.endsWith(".js")).sort()) {
    if (ALLOWED.has(f)) continue;
    readFileSync(join(ROOT, "assets", f), "utf8").split("\n").forEach((line, i) => {
      if (/decodeURIComponent\(/.test(line) && !/\btry\s*\{/.test(line)) offenders.push(`assets/${f}:${i + 1}`);
    });
  }
  assert.deepStrictEqual(offenders, [],
    "decode inside a try on the same line (malformed input opens nothing), " +
      "or allowlist the file here with the reason its input is not an address");
});

// Sweep LP3: a sheet a deep link opens at load (fleet.html#<id>, or index.html#
// forwarded there) built its Web Audio voice with no press on the page, so the
// browser refused every resume() (59 to 84 console warnings per load) and the
// power-on chirp went unheard. The display sheet's audio block and its corner
// sound toggle are run here as shipped (lifted out of app.js), with the boot's
// own priming lifted out of bootInner, against a stand-in document and a
// stand-in AudioContext, so the test is the code, not a description of it.
//
// The stand-in plays by the rules headless Chromium 153 was measured to keep
// (--autoplay-policy=document-user-activation-required): a context made with
// no activation logs nothing and starts held ("suspended"), unless the browser
// lets the tab sound without one (a new tab a click on a Lab page opened
// starts "running" while hasBeenActive is false); resume() or a source's
// start() on a held context is refused with a console warning. `firefox`
// models the other documented answer: navigator.getAutoplayPolicy, and a
// warning at a refused creation.
function displayVoice({ activation = "none", autoplay = false, firefox = false, muted = false } = {}) {
  const vm = require("node:vm");
  const app = readFileSync(join(ROOT, "assets/app.js"), "utf8");
  const start = app.indexOf("  // Web Audio chime");
  const end = app.indexOf("  // The page ships only the watch/dash twins");
  assert.ok(start > 0 && end > start, "app.js lost the display sheet's Web Audio block");
  const block = app.slice(start, end);
  assert.match(block, /canary-sound-toggle/, "the corner sound toggle is part of the lifted block");
  // bootInner's body up to the emulator it starts: what a boot does to the
  // voice before the firmware's first note.
  const bootAt = app.indexOf("const bootInner = async (opts = {}) => {");
  const bootEnd = app.indexOf("ctx.emu = new CanaryEmulator(factory", bootAt);
  assert.ok(bootAt > end && bootEnd > bootAt, "app.js lost bootInner's priming");
  const bootPrime = app.slice(app.indexOf("=> {", bootAt) + 4, bootEnd);
  const page = { active: false, hidden: false };
  const listeners = new Map();              // "type/capture" -> Set(fn)
  const key = (t, c) => `${t}/${c === true || c?.capture === true}`;
  const body = {
    children: [],
    append(n) { this.children.push(n); },
  };
  const document = {
    body,
    get hidden() { return page.hidden; },
    getElementById(id) { return body.children.find((n) => n.id === id) || null; },
    addEventListener(t, fn, c) { (listeners.get(key(t, c)) || listeners.set(key(t, c), new Set()).get(key(t, c))).add(fn); },
    removeEventListener(t, fn, c) { listeners.get(key(t, c))?.delete(fn); },
  };
  const el = (tag) => ({
    tag, id: "", textContent: "", attrs: {}, handlers: [],
    setAttribute(k, v) { this.attrs[k] = v; },
    getAttribute(k) { return this.attrs[k] ?? null; },
    addEventListener(t, fn) { if (t === "click") this.handlers.push(fn); },
    remove() { const i = body.children.indexOf(this); if (i >= 0) body.children.splice(i, 1); },
    click() { for (const fn of this.handlers) fn({ type: "click" }); },
  });
  // Dispatch a trusted event: `activates` says whether the browser counts it
  // as an activation (a mouse pointerdown and a key do; a touch pointerdown
  // does not, its pointerup does; Esc does not), set before listeners run.
  const press = (type, activates = true) => {
    if (activates) page.active = true;
    for (const fn of [...(listeners.get(`${type}/true`) || [])]) fn({ type });
  };
  const count = (types = ["pointerdown", "pointerup", "keydown"]) =>
    types.reduce((n, t) => n + (listeners.get(`${t}/true`)?.size || 0), 0);
  const total = () => [...listeners.values()].reduce((n, set) => n + set.size, 0);
  // notes: heard (written to a running context); held: written to a held one
  const log = { contexts: [], refused: 0, notes: [], held: [] };
  const allowed = () => page.active || autoplay;
  class AudioContext {
    constructor() {
      log.contexts.push(this);
      this.state = allowed() ? "running" : "suspended";
      if (firefox && !allowed()) log.refused++;
      this.currentTime = 0;
      this.destination = {};
    }
    resume() { if (allowed()) this.state = "running"; else log.refused++; }
    close() { this.state = "closed"; }
    createOscillator() { return this.node(); }
    createGain() { return this.node(); }
    createBiquadFilter() { return this.node(); }
    node() {
      const ac = this;
      return {
        connect(n) { return n; },
        // a source's start() starts a held context where the page may sound
        start() { if (ac.state !== "running") { if (allowed()) ac.state = "running"; else log.refused++; } },
        stop() {},
        frequency: { value: 0, setValueAtTime(f) { (ac.state === "running" ? log.notes : log.held).push(f); } },
        gain: { value: 0, setTargetAtTime() {} },
        Q: { value: 0 },
      };
    }
  }
  const navigator = activation === "absent" ? {} : { userActivation: { get hasBeenActive() { return page.active; } } };
  if (firefox) navigator.getAutoplayPolicy = (t) => (t === "audiocontext" && !allowed() ? "disallowed" : "allowed");
  // "absent": a browser without navigator.userActivation, on a page that was
  // pressed (the code cannot ask it, so it does not gate)
  if (activation === "clicked" || activation === "absent") page.active = true;
  const store = new Map(muted ? [["canary-sound", "off"]] : []);
  const localStorage = { getItem: (k) => store.get(k) ?? null, setItem: (k, v) => store.set(k, String(v)) };
  const ctx = { dispose: [] };
  const build = () => vm.runInNewContext(
    `(() => {\n${block}\nconst setShade = () => {};\nconst boot = () => {${bootPrime}};\n` +
      "return { tone, boot, get audio() { return audio; } };\n})()",
    { document, navigator, AudioContext, localStorage, ctx, el },
  );
  const v = { api: build(), press, count, total, log, page, body };
  v.toggle = () => document.getElementById("canary-sound-toggle");
  v.live = () => log.contexts.filter((c) => c.state !== "closed");
  v.dispose = () => { for (const d of ctx.dispose.splice(0)) d(); };
  v.reopen = () => { v.api = build(); };
  return v;
}

test("a deep-linked display sheet asks nothing of the browser before a press, and the first press wakes it (LP3)", () => {
  // At load: the boot primes the voice and the firmware writes its power-on
  // chirp, and nothing is asked of a browser that would refuse it.
  const v = displayVoice();
  v.api.boot();
  for (let i = 0; i < 60; i++) v.api.tone(1046 + i, 0.5);
  assert.strictEqual(v.log.refused, 0, "no resume(), start() or note the browser would refuse (the console warnings)");
  assert.deepStrictEqual([...v.log.notes, ...v.log.held], [], "the notes before the first press go unheard, and none is written to a held context");
  assert.ok(v.log.contexts.length <= 1, "at most one context, made at boot and left held");
  assert.strictEqual(v.count(), 3, "one capture listener per waking event");
  // A touch's pointerdown does not activate the page, and Esc activates
  // nothing: the listener waits for the event that does.
  v.press("pointerdown", false);
  v.press("keydown", false);
  assert.ok(v.live().every((c) => c.state === "suspended"), "an event that did not activate the page wakes nothing");
  assert.strictEqual(v.count(), 3, "and the listener is still there for the one that will");
  v.press("pointerup");
  assert.strictEqual(v.log.contexts.length, 1, "the activating press wakes one voice");
  assert.strictEqual(v.api.audio.state, "running", "inside the gesture");
  assert.strictEqual(v.count(), 0, "the first-press listener removes itself");
  v.api.tone(1318, 0.5);
  assert.deepStrictEqual(v.log.notes, [1318], "the next note the firmware writes is heard");
  assert.strictEqual(v.log.refused, 0);
});

test("the first key press wakes a deep-linked sheet's voice too (LP3)", () => {
  const v = displayVoice();
  v.api.boot();
  v.api.tone(1046, 0.5);
  v.press("keydown");
  assert.strictEqual(v.api.audio?.state, "running", "a key press is an activation, and wakes the voice");
  assert.strictEqual(v.count(), 0);
  v.api.tone(1568, 0.5);
  assert.deepStrictEqual(v.log.notes, [1568]);
  assert.strictEqual(v.log.refused, 0);
});

test("a sheet the browser lets sound without a press plays its power-on chirp (LP3 review)", () => {
  // The flasher's "boot its screen" links open fleet.html#<id> in a new tab
  // (target=_blank): that tab has had no press (hasBeenActive is false), yet
  // Chromium starts its audio. Activation is not the browser's verdict; the
  // context's state is.
  const v = displayVoice({ autoplay: true });
  v.api.boot();
  v.api.tone(1046, 0.5);
  v.api.tone(1318, 0.5);
  assert.deepStrictEqual(v.log.notes, [1046, 1318], "the chirp the firmware writes at boot is heard");
  assert.strictEqual(v.api.audio?.state, "running");
  assert.strictEqual(v.log.contexts.length, 1);
  assert.strictEqual(v.log.refused, 0);
});

test("where the browser says beforehand it would refuse, no context is made until the press (LP3)", () => {
  // Firefox answers navigator.getAutoplayPolicy("audiocontext") and warns at a
  // refused creation, so a held context is not free there.
  const v = displayVoice({ firefox: true });
  v.api.boot();
  v.api.tone(1046, 0.5);
  assert.strictEqual(v.log.contexts.length, 0, "no context the browser said it would refuse");
  assert.strictEqual(v.log.refused, 0);
  v.press("pointerdown");
  assert.strictEqual(v.api.audio?.state, "running", "the first press makes the voice inside the gesture");
  v.api.tone(1318, 0.5);
  assert.deepStrictEqual(v.log.notes, [1318]);
  assert.strictEqual(v.log.refused, 0);
});

test("a click-opened sheet, and a browser without userActivation, play at once as before (LP3)", () => {
  for (const activation of ["clicked", "absent"]) {
    const v = displayVoice({ activation });
    assert.strictEqual(v.count(), 0, `${activation}: no first-press listener when the page may already play`);
    v.api.boot();
    assert.strictEqual(v.api.audio?.state, "running", `${activation}: the boot from a press primes the voice`);
    v.api.tone(1046, 0.5);
    assert.strictEqual(v.log.contexts.length, 1, `${activation}: one voice`);
    assert.deepStrictEqual(v.log.notes, [1046], `${activation}: and the chirp is heard`);
  }
});

test("mute holds through the first press, and open/close never stacks the listener (LP3)", () => {
  const m = displayVoice({ muted: true });
  m.api.boot();
  m.press("pointerdown");
  m.api.tone(1046, 0.5);
  assert.strictEqual(m.log.contexts.length, 0, "a muted sheet makes no voice at boot, on the first press, nor after it");
  assert.strictEqual(m.count(), 0, "the listener is gone once the page is activated, muted or not");
  // Twenty deep-linked opens and closes on one document, none pressed: each
  // sheet's closure hooks the document and its dispose unhooks it, so the
  // twenty-first open finds one first-press listener per event, not 21.
  const v = displayVoice();
  for (let i = 0; i < 20; i++) {
    v.api.boot();
    assert.strictEqual(v.count(), 3, `open ${i + 1}: one first-press listener per waking event`);
    v.dispose();
    assert.strictEqual(v.total(), 0, `close ${i + 1}: every listener the sheet added is gone (visibilitychange too)`);
    assert.strictEqual(v.live().length, 0, `close ${i + 1}: the sheet's context is closed with it`);
    v.reopen();
  }
  v.api.boot();
  assert.strictEqual(v.count(), 3, "after twenty cycles the document still holds one per event");
  // And the press that finally comes wakes exactly one voice.
  v.press("pointerdown");
  assert.deepStrictEqual(v.live().map((c) => c.state), ["running"], "one voice, woken by the one listener left");
  assert.strictEqual(v.log.refused, 0, "and not one refusal over twenty-one boots");
});

test("the corner sound toggle mutes the sheet that is open, after a close and a reopen (LP3 review)", () => {
  // The toggle used to be made once per document: its click drove the FIRST
  // sheet's closure, so after a close and a reopen, muting flipped the button
  // and the stored choice while the open sheet kept sounding.
  const v = displayVoice({ activation: "clicked" });
  v.api.boot();
  v.dispose();
  v.reopen();
  v.api.boot();
  v.api.tone(1046, 0.5);
  assert.deepStrictEqual(v.log.notes, [1046], "the reopened sheet sounds");
  v.toggle().click();
  assert.strictEqual(v.toggle().getAttribute("aria-pressed"), "false", "the button reads muted");
  v.api.tone(1318, 0.5);
  v.api.tone(1568, 0.5);
  assert.deepStrictEqual(v.log.notes, [1046], "and the open sheet is silent after the mute");
  v.toggle().click();
  v.api.tone(1760, 0.5);
  assert.deepStrictEqual(v.log.notes, [1046, 1760], "unmuting it sounds again");
  // One toggle per open sheet, and none once it closes.
  assert.strictEqual(v.body.children.filter((n) => n.id === "canary-sound-toggle").length, 1, "the open sheet has one toggle");
  v.dispose();
  assert.strictEqual(v.toggle(), null, "closing the sheet takes its toggle with it");
});
