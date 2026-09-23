// canary-local/tests/catalog_funnel.test.js — the guided funnel's reasoning,
// pinned. The finder (catalog-funnel.js) narrows the whole manifest to one
// recommended case from up to three answers; these tests hold its pure logic to
// real catalog.json so a mapping can't silently rot:
//   · build filters to the right device family — "a display" is every case a
//     display device's manifest takes, not only the two with a workshop
//   · "weather" only recommends weather-capable cases; "field" the rugged trio
//   · a desk mount pre-checks a stand; weather pre-checks the seal
//   · the recommended variant is a real released flavor when one exists
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const cat = JSON.parse(readFileSync(join(ROOT, "devices/catalog.json"), "utf8"));

// devices/<slug>/device.json — slug → family, the display line's source of truth
function manifestFamilies() {
  const dir = join(ROOT, "../devices");
  const fam = new Map();
  for (const slug of readdirSync(dir).filter((d) => existsSync(join(dir, d, "device.json")))) {
    fam.set(slug, JSON.parse(readFileSync(join(dir, slug, "device.json"), "utf8")).family);
  }
  return fam;
}

let F;
test("load the funnel module", async () => {
  F = await import("../assets/catalog-funnel.js");
  assert.ok(F.funnelResult && F.funnelQuestions && F.recommendFor);
  assert.strictEqual(F.funnelQuestions().length, 3, "three questions");
});

test("build narrows to the chosen device family", async () => {
  const r = F.funnelResult(cat.products, { build: "canary-vision", place: "indoor", mount: "any" });
  assert.ok(r.primary, "a vision build resolves to a case");
  assert.ok((r.primary.device_compat || []).includes("canary-vision"),
    "the primary case fits canary-vision");
});

test("weather only recommends weather-capable cases; indoor prefers simple", async () => {
  const wet = F.funnelResult(cat.products, { build: "canary-wap", place: "weather", mount: "wall" });
  assert.ok(wet.primary.env && (wet.primary.env.cer >= 2 || wet.primary.env.ip),
    "weather → an env-rated case");
  // and the seal is pre-checked
  assert.ok(wet.recommend.options.includes("opt_seal"),
    "weather pre-checks opt_seal");
});

test("field recommends the rugged, device-agnostic carriers", async () => {
  const r = F.funnelResult(cat.products, { build: "canary-vision", place: "field", mount: "any" });
  const rugged = new Set(["field_case", "hammond_chassis", "relay_solar"]);
  assert.ok(rugged.has(r.primary.id) || (r.primary.env && r.primary.env.cer >= 4),
    `field → a rugged carrier (got ${r.primary.id})`);
  // no vision case is itself rugged, so the funnel relaxes and says so
  assert.ok(r.relaxed && r.note, "field relaxes off the device and explains why");
});

test("a display build offers every case a display device takes", async () => {
  const B = await import("../assets/catalog-browse.js");
  const fam = manifestFamilies();
  const display = [...fam].filter(([, f]) => f === "canary-display").map(([slug]) => slug);
  // matchesBuild keys the display line on the slug prefix — hold it to the manifests
  for (const [slug, f] of fam) {
    assert.strictEqual(slug.startsWith("canary-display-"), f === "canary-display",
      `${slug}: the canary-display- slug prefix is exactly the canary-display family`);
  }
  const pool = cat.products.filter((p) => F.matchesBuild(B.productSummary(p), "display"))
    .map((p) => p.id).sort();
  const want = cat.products.filter((p) => (p.device_compat || []).some((d) => display.includes(d)))
    .map((p) => p.id).sort();
  assert.deepStrictEqual(pool, want, "the display answer is every display device's case");
  for (const id of ["watch_station", "dash_display", "s3_lcd7", "s3_touch169",
    "c6_display", "s3_lcd147", "c3_lcd147"]) {
    assert.ok(pool.includes(id), `the display answer offers ${id}`);
  }
});

test("a desk mount pre-checks a stand when the case has one", async () => {
  // s3_touch169 / s3_lcd7 (display) carry opt_stand — the desk answer ranks a
  // stand-bearing case first, and pre-checks its stand
  const hasStand = (p) => (p.options || []).some((o) => o.id === "opt_stand");
  assert.ok(hasStand(cat.products.find((p) => p.id === "s3_touch169")), "the 1.69 puck carries opt_stand");
  const r = F.funnelResult(cat.products, { build: "display", place: "indoor", mount: "desk" });
  assert.ok(r.primary, "a display desk build resolves");
  assert.ok(hasStand(r.primary), `a desk display build recommends a case with a stand (got ${r.primary.id})`);
  assert.ok(r.recommend.options.includes("opt_stand"), "desk mount pre-checks opt_stand");
});

test("the recommendation names a real released flavor when one exists", async () => {
  const r = F.funnelResult(cat.products, { build: "canary-wap", place: "indoor", mount: "wall" });
  const rec = F.recommendFor(r.primary, { place: "indoor", mount: "wall" });
  if (rec.variant) {
    const v = (r.primary.variants || []).find((x) => x.id === rec.variant);
    assert.ok(v, "the recommended variant id is a real variant of the product");
  }
});
