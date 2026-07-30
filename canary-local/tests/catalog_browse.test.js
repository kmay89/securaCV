// canary-local/tests/catalog_browse.test.js — the catalog browse's facet logic,
// pinned. The browse (catalog-browse.js) is a faceted gallery over ALL products
// in catalog.json; these tests hold its pure helpers to the manifest so a
// facet can't silently stop matching:
//   · every product summarizes without throwing and carries a family/env label
//   · env labels are honest — a rated case is a "target", never "verified"
//   · facets tally real values with correct counts
//   · applyFacets narrows AND-across-groups / OR-within-a-group
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const cat = JSON.parse(readFileSync(join(ROOT, "devices/catalog.json"), "utf8"));

let B;
test("load the browse module", async () => {
  B = await import("../assets/catalog-browse.js");
  assert.ok(B.facetsFor && B.applyFacets && B.productSummary && B.envLabel);
});

test("every product summarizes with a family and an env label", async () => {
  for (const p of cat.products) {
    const s = B.productSummary(p);
    assert.ok(s.family, `${p.id}: has a family`);
    assert.ok(s.envLabel, `${p.id}: has an env label`);
    assert.ok(Array.isArray(s.devices), `${p.id}: devices is a list`);
    assert.ok(Array.isArray(s.alternatives), `${p.id}: alternatives is a list`);
  }
});

test("env labels are honest (rated → a target, unrated otherwise)", async () => {
  const field = cat.products.find((p) => p.id === "field_case");
  assert.strictEqual(B.envLabel(field), "CER-4", "field case reads its CER level");
  const bench = cat.products.find((p) => p.id === "bench_fixture");
  assert.strictEqual(B.envLabel(bench), "unrated", "an unrated case is 'unrated'");
  // no product should ever be labeled verified — env.verified is always false
  for (const p of cat.products) {
    if (p.env) assert.strictEqual(p.env.verified, false, `${p.id}: env is a target`);
  }
});

test("facets tally real values with counts that sum to the catalog", async () => {
  const facets = B.facetsFor(cat.products);
  const status = facets.find((g) => g.key === "status");
  assert.ok(status, "a status facet exists");
  const total = status.values.reduce((n, v) => n + v.count, 0);
  assert.strictEqual(total, cat.products.length,
    "every product falls in exactly one status bucket");
  const type = facets.find((g) => g.key === "type");
  assert.ok(type.values.every((v) => ["primary", "accessory"].includes(v.value)),
    "type facet values are primary/accessory");
});

test("applyFacets narrows: AND across groups, OR within a group", async () => {
  const all = cat.products.length;
  // OR within a group: released + in development = everything
  const released = B.applyFacets(cat.products, { status: new Set(["released"]) });
  const indev = B.applyFacets(cat.products, { status: new Set(["in development"]) });
  assert.strictEqual(released.length + indev.length, all, "status buckets partition");
  assert.ok(released.length > 0 && indev.length > 0, "both buckets are non-empty");
  // AND across groups: released ∧ configurable ⊆ released
  const both = B.applyFacets(cat.products,
    { status: new Set(["released"]), configurable: new Set(["configurable"]) });
  assert.ok(both.length <= released.length, "adding a group only narrows");
  for (const p of both) {
    const s = B.productSummary(p);
    assert.ok(s.released > 0 && s.userOptions > 0, `${p.id}: matches both facets`);
  }
  // empty selection = no narrowing
  assert.strictEqual(B.applyFacets(cat.products, {}).length, all, "no facets → all");
});
