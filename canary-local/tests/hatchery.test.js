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

// ── Derived names: one bird, one name, on every surface ──────────────────
//
// The certificate used to live in one app's local storage, which made it a
// nickname rather than a certificate. It is now derived from the device's key,
// so these tests guard the property that makes that work: the SAME key must
// produce the SAME name here, in the Mac Flasher, and in the iPhone app. The
// Swift side is pinned to vectors generated from this very module
// (ios/Shared/HatchSpec.swift), so a change here that isn't regenerated fails
// the iOS build rather than shipping a bird with two names.

const derive = () => import("../tools/hatchery/derive.mjs");

test("the same key always derives the same certificate", async () => {
  const { deriveCertificate } = await derive();
  const a = deriveCertificate(hatch, { fingerprint: "a3f7c1d2e4b58690" });
  const b = deriveCertificate(hatch, { fingerprint: "A3F7C1D2E4B58690" });
  assert.equal(a.name, b.name, "case is presentation, not identity");
  assert.ok(a.name.includes(a.base));
  assert.equal(a.derived, true);
});

test("different keys spread across the name lists", async () => {
  const { deriveCertificate } = await derive();
  const names = new Set();
  for (let i = 0; i < 64; i++) {
    names.add(deriveCertificate(hatch, { fingerprint: i.toString(16).padStart(16, "0") }).name);
  }
  assert.ok(names.size > 20, `expected spread, got ${names.size} distinct names`);
});

test("no fingerprint means no derived certificate", async () => {
  const { deriveCertificate } = await derive();
  assert.equal(deriveCertificate(hatch, { fingerprint: "" }), null);
  assert.equal(deriveCertificate(hatch, {}), null);
});

test("the ring id prefers the device's own slug, else the fingerprint", async () => {
  const { deriveCertificate } = await derive();
  assert.equal(
    deriveCertificate(hatch, { fingerprint: "a3f7c1d2e4b58690", deviceId: "canary-a3f7" }).ringId,
    "canary-a3f7");
  assert.equal(
    deriveCertificate(hatch, { fingerprint: "a3f7c1d2e4b58690" }).ringId,
    "A3F7C1D2E4B58690");
});

test("mintCertificate derives when it knows the key, and rolls when it doesn't", async () => {
  const m = await mod();
  const first = m.mintCertificate(hatch, { fingerprint: "deadbeefcafef00d" });
  const again = m.mintCertificate(hatch, { fingerprint: "deadbeefcafef00d" });
  assert.equal(first.name, again.name, "a key names its bird the same way every time");
  const rolled = m.mintCertificate(hatch, { rng: () => 0.25 });
  assert.ok(rolled && rolled.name, "a board with no key still hatches with a name");
});

test("the committed Swift vectors match this module", async () => {
  // The iOS app embeds vectors generated from here. If they drift, the phone
  // and the Lab would name the same Canary differently — so the generated
  // output is checked against a fresh derivation.
  const { deriveCertificate } = await derive();
  const swift = readFileSync(
    join(__dirname, "..", "..", "ios", "Shared", "HatchSpec.swift"), "utf8");
  const rows = [...swift.matchAll(/Vector\(fingerprint: "([0-9a-f]+)",\s*\n\s*name: "([^"]+)"/g)];
  assert.ok(rows.length >= 4, "expected the generated vectors to be present");
  for (const [, fp, name] of rows) {
    assert.equal(deriveCertificate(hatch, { fingerprint: fp }).name, name,
      `HatchSpec.swift is stale for ${fp} — re-run tools/hatchery/gen_hatch_swift.mjs`);
  }
});
