// Host tests for assets/hatchery.js — the whimsical name + birth certificate a
// Canary earns when it hatches. Pure + RNG-injectable, so the assembly is fully
// deterministic here (a scripted RNG forces each pick) and mirrors the native
// app's mintCertificate. Also asserts the real committed hatch.json is shaped the
// way the assembly expects.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const mod = () => import("../assets/hatchery.js");
const hatch = JSON.parse(readFileSync(join(__dirname, "..", "devices", "hatch.json"), "utf8"));

// A scripted RNG: returns the given values in order, then 0. Lets each pick be
// pinned exactly (base → title? → house → [ring] → title → motto).
const scripted = (seq) => { let i = 0; return () => (i < seq.length ? seq[i++] : 0); };

test("mintCertificate: deterministic name/lineage/ring/motto from a fixed RNG", async () => {
  const { mintCertificate } = await mod();
  // All-zeros → first of every list; a device id becomes the Ring ID (no ring roll).
  const cert = mintCertificate(hatch, {
    product: { name: "Canary Vision" }, deviceId: "canary_test_01", rng: () => 0,
  });
  assert.strictEqual(cert.base, hatch.first[0]);                 // "Pip"
  assert.strictEqual(cert.name, `${hatch.titles[0]} ${hatch.first[0]} ${hatch.house[0]}`);
  assert.strictEqual(cert.species, "Canary Vision");
  assert.strictEqual(cert.lineage, `${hatch.ordinals[1]} of its name, ${hatch.house[0].replace(/^the /, "")}`);
  assert.strictEqual(cert.ringId, "canary_test_01");             // provisioned id wins
  assert.strictEqual(cert.motto, hatch.mottoes[0]);
});

test("mintCertificate: title is skipped when the roll misses title_chance", async () => {
  const { mintCertificate } = await mod();
  // seq: base=first[0], withTitle roll=0.9 (>= 0.6 → no title), house=house[0], motto=mottoes[0].
  const cert = mintCertificate(hatch, {
    deviceId: "id_x", rng: scripted([0, 0.9, 0, 0]),
  });
  assert.strictEqual(cert.name, `${hatch.first[0]} ${hatch.house[0]}`); // no title prefix
});

test("mintCertificate: the ordinal climbs with prior hatches of the same base", async () => {
  const { mintCertificate } = await mod();
  const cert = mintCertificate(hatch, {
    deviceId: "id_y", rng: () => 0,
    fleet: [{ base: hatch.first[0] }, { base: hatch.first[0] }], // two already
  });
  assert.strictEqual(cert.lineage.startsWith(hatch.ordinals[3]), true, "third of its name");
});

test("pickFreshBase: avoids used bases and the one to avoid; reuses only when spent", async () => {
  const { pickFreshBase } = await mod();
  assert.strictEqual(pickFreshBase(["a", "b", "c"], ["a"], "b", () => 0), "c");
  // Pool spent → reuse allowed rather than failing.
  assert.strictEqual(pickFreshBase(["a"], ["a"], null, () => 0), "a");
});

test("genRing: PREFIX-XXX-XXX from a 24-bit value", async () => {
  const { genRing } = await mod();
  assert.strictEqual(genRing("CNRY", () => 0), "CNRY-000-000");
  assert.strictEqual(genRing("CNRY", () => 0.5), "CNRY-800-000");
});

test("mintCertificate: an empty/absent spec yields null (no certificate, no throw)", async () => {
  const { mintCertificate } = await mod();
  assert.strictEqual(mintCertificate(null), null);
  assert.strictEqual(mintCertificate({ first: [] }), null);
  assert.strictEqual(mintCertificate({}), null);
});

test("the committed hatch.json is shaped the way the assembly expects", () => {
  for (const k of ["first", "titles", "house", "mottoes", "ordinals"]) {
    assert.ok(Array.isArray(hatch[k]) && hatch[k].length, `hatch.json.${k} must be a non-empty array`);
  }
  assert.strictEqual(typeof hatch.ring_prefix, "string");
  assert.strictEqual(typeof hatch.title_chance, "number");
  assert.ok(hatch.certificate && typeof hatch.certificate === "object");
});
