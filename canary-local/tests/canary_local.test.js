// canary-local/tests/canary_local.test.js — Node tests for the page's
// DOM-free logic (repo convention: CI tests the exact shipped source).
//
//   node --test canary-local/tests/
//
// Coverage: MQTT wildcard matching (the shell's broker semantics), the
// witness canonical/signature format (must equal what trust.cpp
// rebuilds before Ed25519::verify — pinned here as a golden string),
// LED cadence translation, registry ↔ dist artifact integrity, and the
// vendored Witness Wall against the fleet contract.
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

// Both apps iframe the website's Witness Wall emulator, vendored byte for byte
// (witness/PROVENANCE.txt). The fleet contract it reads is
// tvos/discovery/DISCOVERY.md: only `name` is required, and a silent `online`
// is NOT a presence claim. The website fixed its canonical copy after the apps
// had vendored it, and nothing here noticed, because
// scripts/check_witness_emulator_sync.sh compares the two app copies with each
// other, never with the contract. So replay the contract's own vectors through
// every `online:` the vendored emulator derives from a fleet row, in both apps.
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
    assert.doesNotMatch(src, /online[^,;\n]*(!==\s*false|===\s*undefined\s*\?\s*true|\?\?\s*true)/,
      `${rel} still defaults a silent \`online\` to present in some other shape`);
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
