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

test("one board_label, one board — no two registry boards share a label", () => {
  // Regression: the Sense-board products (canary, wap) and the plain-XIAO
  // Vision port all said "Seeed XIAO ESP32-S3", so the owner of a Sense
  // board could not find the firmware written FOR it — the flagship card
  // never said "Sense". A label that names two different physical boards
  // is a label that names neither.
  const byLabel = new Map();
  for (const p of catalog.products) {
    const boardId = p.tier && p.tier.board_id;
    if (!boardId) continue;
    const seen = byLabel.get(p.board_label);
    if (seen === undefined) byLabel.set(p.board_label, boardId);
    else assert.strictEqual(seen, boardId,
      `board_label "${p.board_label}" names both ${seen} and ${boardId}`);
  }
  const canary = catalog.products.find((p) => p.id === "securacv-canary");
  assert.match(canary.board_label, /Sense/,
    "the flagship must say Sense — that's the board it drives");
});

// ── board narrowing (chip + measured flash size) ────────────────────────────

test("S3 + 16 MB narrows to the 16 MB boards — no longer a single family", async () => {
  // The Freenove reach port (docs/strategy/30) is also an S3 with 16 MB, so
  // this measurement stopped being an identification the moment it landed.
  // The set is asserted exactly so a future board silently joining it is a
  // visible change, not a quiet loss of precision.
  const c = await core();
  const set = c.productsForBoard(catalog, "ESP32-S3", 16 * MB);
  assert.deepStrictEqual(set.map((p) => p.id),
    ["securacv-canary-freenove-s3",
     "securacv-canary-display-dash", "securacv-canary-display-dash-modes",
     "securacv-canary-display-dash7", "securacv-canary-display-nightstand7",
     "securacv-canary-display-nightstand-s3",
     "securacv-canary-display-touch169",
     "securacv-canary-display-amoled241"]);
  assert.ok(new Set(set.map((p) => p.family)).size > 1,
    "spans families — smartPick must not claim to have named the board");
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

// ── the size gate (flashFitVerdict) ─────────────────────────────────────────
// The chip guard says "right silicon"; this says "enough of it". The exact
// failure it exists for: a Freenove 16 MB image written to an 8 MB XIAO
// Sense flashes cleanly and then boot-loops before printing a single line.

test("an image needing more flash than the chip has is refused, with the cause named",
  async () => {
    const c = await core();
    const freenove = catalog.products.find(
      (p) => p.id === "securacv-canary-freenove-s3");
    const v = c.flashFitVerdict(freenove, 8 * MB);
    assert.strictEqual(v.fits, false);
    assert.strictEqual(v.needMb, 16);
    assert.strictEqual(v.haveMb, 8);
    assert.match(v.why, /16 MB/);
    assert.match(v.why, /8 MB/);
    assert.match(v.why, /boot-loop/, "the verdict names the symptom");
    assert.match(v.why, /wrong-board image, not a broken board/,
      "the verdict absolves the hardware");
  });

test("smaller image on a bigger chip is headroom, not a refusal", async () => {
  const c = await core();
  const xiao = catalog.products.find((p) => p.id === "securacv-canary");
  assert.strictEqual(c.flashFitVerdict(xiao, 16 * MB).fits, true);
  assert.strictEqual(c.flashFitVerdict(xiao, 8 * MB).fits, true);
});

test("an unmeasured chip is never judged — unknown size blocks nothing", async () => {
  const c = await core();
  const freenove = catalog.products.find(
    (p) => p.id === "securacv-canary-freenove-s3");
  assert.strictEqual(c.flashFitVerdict(freenove, null).fits, true);
  assert.strictEqual(c.flashFitVerdict(freenove, undefined).fits, true);
  assert.strictEqual(c.flashFitVerdict(null, 8 * MB).fits, true);
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

test("silicon narrows but does not name: S3 + 16 MB says so out loud",
  async () => {
    const c = await core();
    const pick = c.smartPick(catalog, {
      chip: "ESP32-S3", chipLabel: "ESP32-S3", flashBytes: 16 * MB,
    });
    assert.strictEqual(pick.kind, "board");
    assert.match(pick.why, /16 MB/);
    // Several boards fit S3 + 16 MB, so the copy must offer a starting point
    // rather than assert "that looks like a <board>" — the whole point of the
    // ambiguity branch in smartPick.
    assert.match(pick.why, /more than one board matches/i);
    assert.doesNotMatch(pick.why, /looks like a/);
  });

test("tier copy never borrows the word reserved for signature checks", () => {
  // AGENTS.md rule 4: "Verified" means an Ed25519 signature checked against a
  // pinned key — nothing looser. This copy renders inches from the flasher's
  // real signature check, so a bench result must never wear that word.
  for (const p of catalog.products) {
    const said = `${p.tier.label} ${p.tier.line}`;
    assert.doesNotMatch(said, /verif/i,
      `${p.id}: tier copy says "${said}" — bench work is called bench work`);
  }
});

test("per-product access notes survive dedup — both classic boards are shown", () => {
  // Regression: both frontends render the access cards once each, keyed on
  // `access.key`. When that key was the FAMILY, the ESP32-CAM and the WROOM
  // DevKit collided (both `canary`) and whichever sorted second — the WROOM's
  // "pick the CP2102, not an ESP32" note — was silently dropped.
  const withAccess = catalog.products.filter((p) => p.access);
  const keys = withAccess.map((p) => p.access.key);
  for (const p of withAccess) {
    assert.ok(p.access.key, `${p.id}: access entry carries no dedup key`);
  }
  const rendered = new Set(keys);
  for (const id of ["securacv-canary-esp32cam", "securacv-canary-wroom"]) {
    const p = catalog.products.find((x) => x.id === id);
    assert.ok(rendered.has(p.access.key), `${id}: access note deduped away`);
  }
  assert.notStrictEqual(
    catalog.products.find((x) => x.id === "securacv-canary-esp32cam").access.key,
    catalog.products.find((x) => x.id === "securacv-canary-wroom").access.key,
    "two boards needing opposite instructions must not share a dedup key");
  // Family-keyed notes still collapse: Sense's two products share one card.
  const sense = withAccess.filter((p) => p.family === "sense");
  assert.ok(sense.length > 1 && new Set(sense.map((p) => p.access.key)).size === 1,
    "family-level notes should still render once for the family");
});

test("every product states a support tier, and the reach ports state the loud one",
  async () => {
    // The tier is what makes offering an unproven image honest, so it is the
    // catalog's job to always carry one — not the page's job to remember.
    for (const p of catalog.products) {
      assert.ok(p.tier && p.tier.label && p.tier.line, `${p.id}: no tier`);
      assert.strictEqual(typeof p.tier.first, "boolean", `${p.id}: tier.first`);
    }
    const cam = catalog.products.find((p) => p.id === "securacv-canary-esp32cam");
    assert.ok(cam.tier.first, "a compile-tested port must read as never-booted");
    assert.match(cam.tier.line, /nobody has run it on real hardware/i);
    const flagship = catalog.products.find((p) => p.id === "securacv-canary");
    assert.strictEqual(flagship.tier.first, false, "the benched flagship is not 'first'");
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
