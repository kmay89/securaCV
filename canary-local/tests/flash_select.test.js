// canary-local/tests/flash_select.test.js — the detection-led picker.
//
// Pins the firmware-selection model (docs/browser_flasher.md § How the
// picker chooses): the family layer that keeps a ten-product line
// un-intimidating, and smartPick — the priority ladder that turns what the
// flasher can READ (chip, measured flash size, the firmware already on the
// board, a ?product= ask) into ONE recommended card with a plain-language
// reason. Every branch must state its evidence: magic that explains itself
// is trust; magic that doesn't is a guess.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const CL = join(__dirname, "..");
const catalog = JSON.parse(readFileSync(join(CL, "devices/flash.json"), "utf8"));

async function core() {
  return import("../assets/flash-core.js");
}

const MB = 1024 * 1024;

// ── the family layer ────────────────────────────────────────────────────────

test("families cover every product exactly once", async () => {
  const c = await core();
  const fams = c.familiesIn(catalog);
  assert.ok(fams.length >= 5, "the five product stories");
  const ids = new Set(fams.map((f) => f.id));
  assert.strictEqual(ids.size, fams.length, "family ids unique");
  for (const p of catalog.products) {
    assert.ok(ids.has(p.family), `${p.id}: family '${p.family}' exists`);
  }
});

test("a family with variants asks ONE plain question; every member answers", () => {
  for (const fam of catalog.families) {
    const members = catalog.products.filter((p) => p.family === fam.id);
    assert.ok(members.length >= 1, `${fam.id}: has products`);
    assert.ok(fam.name && fam.pitch, `${fam.id}: name + pitch`);
    if (members.length > 1) {
      assert.ok(fam.pick && fam.pick.endsWith("?") || fam.pick.includes("?"),
        `${fam.id}: multi-product family asks a question`);
      for (const m of members) {
        assert.ok(m.pick_label, `${m.id}: answers the family question`);
      }
    }
  }
});

test("every product carries the board facts detection narrows on", () => {
  for (const p of catalog.products) {
    assert.ok(Number.isInteger(p.flash_mb) && p.flash_mb > 0,
      `${p.id}: flash_mb`);
    assert.ok(p.board_label && p.board_label.length > 3, `${p.id}: board_label`);
  }
});

// ── board narrowing (chip + measured flash size) ────────────────────────────

test("S3 + 16 MB names the Waveshare panel family exactly", async () => {
  const c = await core();
  const set = c.productsForBoard(catalog, "ESP32-S3", 16 * MB);
  assert.deepStrictEqual(set.map((p) => p.id),
    ["securacv-canary-display-dash", "securacv-canary-display-dash-modes"]);
});

test("S3 + 8 MB is the XIAO class; unknown or odd sizes never empty the set",
  async () => {
    const c = await core();
    const xiao = c.productsForBoard(catalog, "ESP32-S3", 8 * MB);
    assert.ok(xiao.every((p) => p.flash_mb === 8));
    assert.ok(xiao.some((p) => p.id === "securacv-canary"), "flagship present");
    const unknown = c.productsForBoard(catalog, "ESP32-S3", null);
    assert.strictEqual(unknown.length,
      c.productsForChip(catalog, "ESP32-S3").length, "no size → chip set");
    const odd = c.productsForBoard(catalog, "ESP32-S3", 32 * MB);
    assert.strictEqual(odd.length,
      c.productsForChip(catalog, "ESP32-S3").length,
      "unmatched size falls back — never an empty picker");
  });

// ── smartPick: the priority ladder ──────────────────────────────────────────

test("what you asked for outranks everything", async () => {
  const c = await core();
  const pick = c.smartPick(catalog, {
    chip: "ESP32-S3", flashBytes: 16 * MB,
    currentProject: "canary_display_dash",
    preferredId: "securacv-canary-display-dash-modes",
  });
  assert.strictEqual(pick.kind, "picked");
  assert.strictEqual(pick.product.id, "securacv-canary-display-dash-modes");
  assert.match(pick.why, /you picked/);
});

test("what the board already runs outranks the silicon guess", async () => {
  const c = await core();
  const pick = c.smartPick(catalog, {
    chip: "ESP32-C6", currentProject: "canary-sense-wellbeing",
  });
  assert.strictEqual(pick.kind, "current");
  assert.strictEqual(pick.product.id, "securacv-canary-sense-wellbeing");
  assert.match(pick.why, /already runs/);
});

test("silicon speaks: S3 + 16 MB recommends the Dash with the evidence stated",
  async () => {
    const c = await core();
    const pick = c.smartPick(catalog, {
      chip: "ESP32-S3", chipLabel: "ESP32-S3", flashBytes: 16 * MB,
    });
    assert.strictEqual(pick.kind, "board");
    assert.strictEqual(pick.product.id, "securacv-canary-display-dash");
    assert.match(pick.why, /16 MB/);
    assert.match(pick.why, /Waveshare/);
  });

test("S3 + 8 MB stays with the authored flagship (the XIAO class leads with Canary)",
  async () => {
    const c = await core();
    const pick = c.smartPick(catalog, { chip: "ESP32-S3", flashBytes: 8 * MB });
    assert.strictEqual(pick.product.id, "securacv-canary");
    assert.strictEqual(pick.kind, "board");
  });

test("no extra evidence → the chip's authored default, same as ever", async () => {
  const c = await core();
  const pick = c.smartPick(catalog, { chip: "ESP32-S3" });
  assert.strictEqual(pick.kind, "chip");
  assert.strictEqual(pick.product.id,
    c.recommendedProduct(catalog, "ESP32-S3").id);
  const c6 = c.smartPick(catalog, { chip: "ESP32-C6" });
  assert.strictEqual(c6.product.id, "securacv-canary-sense");
});

test("C3: both vision boards are 4 MB, so size cannot pretend to know which",
  async () => {
    const c = await core();
    const pick = c.smartPick(catalog, { chip: "ESP32-C3", flashBytes: 4 * MB });
    assert.strictEqual(pick.kind, "chip",
      "no narrowing → no board claim (honesty over cleverness)");
  });

test("an unknown chip yields null, never a guess", async () => {
  const c = await core();
  assert.strictEqual(c.smartPick(catalog, { chip: "RP2040" }), null);
});

test("every why is a sentence, and no branch borrows another's evidence",
  async () => {
    const c = await core();
    const picks = [
      c.smartPick(catalog, { chip: "ESP32-S3" }),
      c.smartPick(catalog, { chip: "ESP32-S3", flashBytes: 16 * MB }),
      c.smartPick(catalog, { chip: "ESP32-S3", currentProject: "canary_wap" }),
      c.smartPick(catalog, {
        chip: "ESP32-S3", preferredId: "securacv-canary-wap" }),
    ];
    for (const p of picks) {
      assert.ok(p && /\.$/.test(p.why.trim()), `${p.kind}: why is a sentence`);
    }
    assert.deepStrictEqual(picks.map((p) => p.kind),
      ["chip", "board", "current", "picked"]);
  });
