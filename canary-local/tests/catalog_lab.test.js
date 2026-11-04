// canary-local/tests/catalog_lab.test.js — the enclosure lab's catalog-driven
// configurator logic, pinned. The lab (enclosure-lab.js) now renders live,
// constraint-aware controls from catalog.json instead of read-only param text;
// these tests hold the pure data helpers behind that UI to the manifest:
//   · every committed set maps to a catalog product + variant (so the lab can
//     always find a configurator for what a pill shows)
//   · the OpenSCAD export vector carries the user axes + user options and omits
//     engineering knobs (the audience split, honored on the way out)
//   · variant matching resolves an axis choice back to the committed STL that
//     ships it, or null for a combination we don't ship
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const cat = JSON.parse(readFileSync(join(ROOT, "devices/catalog.json"), "utf8"));
const enc = JSON.parse(readFileSync(join(ROOT, "devices/enclosures.json"), "utf8"));

// The module is ESM; import() it once for the whole suite.
let LAB;
test("load the lab module", async () => {
  LAB = await import("../assets/enclosure-lab.js");
  assert.ok(LAB.catalogVariantIndex && LAB.scadParamVector && LAB.matchVariant);
});

test("every committed set maps to a catalog product + variant", async () => {
  const idx = LAB.catalogVariantIndex(cat);
  for (const s of enc.sets) {
    const hit = idx.get(s.id);
    assert.ok(hit, `set ${s.id} has a catalog product`);
    assert.strictEqual(hit.variant.id, s.id);
    assert.ok(hit.product.scad, `${s.id}: product carries a scad`);
  }
});

test("configurable siblings are the other pinned-selector variants", async () => {
  const idx = LAB.catalogVariantIndex(cat);
  const weather = idx.get("vision-xiao-weather");
  const sibIds = weather.siblings.map((v) => v.id);
  assert.ok(sibIds.includes("vision-xiao-indoor"), "indoor is a sibling flavor");
  assert.ok(sibIds.includes("vision-devkit-indoor"), "devkit is a sibling flavor");
  // vision-mount-kit has empty selects (an addon, not a flavor) → never a sibling
  assert.ok(!sibIds.includes("vision-mount-kit"), "the mount kit is not a flavor");
  assert.ok(!sibIds.includes("vision-xiao-weather"), "not its own sibling");
});

test("the export vector carries user axes + user options, drops engineering", async () => {
  const vision = cat.products.find((p) => p.id === "vision_enclosure");
  const axisVals = { host: "xiao", preset: "vision_weather" };
  const optVals = {};
  for (const o of vision.options) optVals[o.id] = !!o.default;
  const vec = LAB.scadParamVector(vision, axisVals, optVals);
  assert.strictEqual(vec.preset, "vision_weather", "user axis is in the vector");
  assert.strictEqual(vec.host, "xiao", "second user axis is in the vector");
  assert.ok("opt_led" in vec, "a user option is in the vector");
  assert.ok(!("lid_ribs" in vec), "an engineering option is NOT in the vector");
});

test("options seed by type: enum keeps its value, bool coerces to boolean", async () => {
  // mount_style is an enum option on the vision/sense/wap enclosures — its
  // default must survive as a string, not collapse to a boolean.
  const vision = cat.products.find((p) => p.id === "vision_enclosure");
  const mount = vision.options.find((o) => o.id === "mount_style");
  assert.ok(mount && mount.type === "enum", "mount_style is an enum option");
  const seeded = LAB.seedOptionValues(vision.options);
  assert.strictEqual(seeded.mount_style, mount.default,
    "enum option seeds to its string default, not true/false");
  assert.strictEqual(typeof seeded.opt_led, "boolean", "a bool option seeds boolean");
  // engineering options are not part of the user surface
  assert.ok(!("lid_ribs" in seeded), "engineering options are not seeded");
});

test("editing an option flips a named preset to custom (SCAD override safety)", async () => {
  const wap = cat.products.find((p) => p.id === "wap_enclosure");
  // WAP has a preset axis whose enum includes "custom"
  assert.strictEqual(LAB.presetAfterEdit(wap, "battery_full"), "custom",
    "a named preset falls back to custom when an option is edited");
  assert.strictEqual(LAB.presetAfterEdit(wap, "custom"), "custom",
    "already-custom stays custom");
  // a product without a custom-capable preset axis leaves the value alone
  const dash = cat.products.find((p) => p.id === "dash_display");
  assert.strictEqual(LAB.presetAfterEdit(dash, "whatever"), "whatever",
    "no preset axis → nothing to override, value unchanged");
});

test("matchVariant resolves an axis choice to its committed STL, else null", async () => {
  const idx = LAB.catalogVariantIndex(cat);
  const self = idx.get("vision-xiao-weather").variant;
  const sibs = [self, ...idx.get("vision-xiao-weather").siblings];
  assert.strictEqual(
    LAB.matchVariant(sibs, { host: "xiao", preset: "vision_weather" }),
    "vision-xiao-weather", "the exact flavor resolves to its set");
  assert.strictEqual(
    LAB.matchVariant(sibs, { host: "devkit", preset: "vision_indoor" }),
    "vision-devkit-indoor", "another flavor resolves to its set");
  assert.strictEqual(
    LAB.matchVariant(sibs, { host: "devkit", preset: "vision_weather" }),
    null, "a combination we don't ship a mesh for is a custom (null) match");
});

// Sweep C24: each SCAD-parameter group's note sat whole in its <summary>, up
// to 2058 characters (Ventilation) on one line beside the group's name.
// noteParts() splits it: the summary says the first sentence (`lead`), and a
// longer note opens with the group as its first row. Held here to every note
// the committed enclosures.json carries (enclosures.json itself is the site
// contract's, not edited for this).
const ENDS = /[.!?](?=\s|$)/g;
function committedNotes() {
  const out = new Map();
  const walk = (o) => {
    if (Array.isArray(o)) return o.forEach(walk);
    if (!o || typeof o !== "object") return;
    for (const g of Array.isArray(o.groups) ? o.groups : []) if (g && g.note) out.set(g.note, g.name);
    Object.values(o).forEach(walk);
  };
  walk(enc);
  return out;
}

test("C24: noteParts gives the summary each committed note's first sentence, and loses nothing", async () => {
  assert.strictEqual(typeof LAB.noteParts, "function", "enclosure-lab.js exports noteParts");
  const notes = committedNotes();
  assert.ok(notes.size >= 100, `the committed notes are read (${notes.size})`);
  const squash = (t) => t.replace(/\s+/g, " ").trim();
  let split = 0, whole = 0;
  for (const [note, name] of notes) {
    const { lead, rest } = LAB.noteParts(note);
    // lead + rest is the note: nothing dropped, nothing added
    assert.strictEqual(squash(rest ? `${lead} ${rest}` : lead), squash(note), `${name}: lead + rest is not the note`);
    assert.ok(lead.length > 0 && /[A-Za-z0-9]/.test(lead), `${name}: an empty lead`);
    assert.strictEqual(rest, rest.trim(), `${name}: rest is trimmed`);
    // the lead is at most the first sentence: it ends where the first
    // sentence end does on every committed note (none of them opens with an
    // abbreviation, a range or an enumerator that the stricter reading skips)
    const first = ENDS.exec(note.trim());
    ENDS.lastIndex = 0;
    if (!first) {
      assert.deepStrictEqual({ lead, rest }, { lead: note.trim(), rest: "" }, `${name}: a note with no sentence end is all lead`);
      whole += 1;
      continue;
    }
    assert.strictEqual(lead, note.trim().slice(0, first.index + 1), `${name}: the lead is not the first sentence`);
    assert.ok(!/[.!?]\s/.test(lead), `${name}: the lead holds more than one sentence: ${lead}`);
    if (rest) split += 1;
  }
  assert.ok(split > 30 && whole > 30, `both kinds of note are held (${split} split, ${whole} whole)`);
  // the longest notes, by name: a short lead, and the rest is still there
  const byLen = [...notes].sort((a, b) => b[0].length - a[0].length);
  assert.deepStrictEqual(byLen.slice(0, 4).map(([n, name]) => [name, n.length]),
    [["Ventilation", 2058], ["USB-A", 1527], ["Press dot", 947], ["Board location", 868]]);
  assert.strictEqual(LAB.noteParts(byLen[0][0]).lead,
    "side-wall slots only; the row sits BEHIND the light band so the white line stays whole.");
  assert.strictEqual(LAB.noteParts(byLen[2][0]).lead, "the pip on the pad face.");
  assert.strictEqual(LAB.noteParts(byLen[1][0]).lead, 'the `port = "usb_a"` build only: the ESP32-S3-LCD-1.47.',
    "a sentence that ends on a board's version number ends after it, not inside it");
  for (const [n] of byLen.slice(0, 4)) assert.ok(LAB.noteParts(n).lead.length < 100, "a long note's summary is one short sentence");
});

test("C24: noteParts never cuts inside a number, an abbreviation, a range or an enumerator", async () => {
  const P = (t) => LAB.noteParts(t);
  // no sentence end at all: the whole note is the lead, and no row opens
  for (const t of ["v1.2 holds", "Ø3 hole + adhesive ePTFE patch", "the 1.47 panel", "...", "…", "", "   "])
    assert.deepStrictEqual(P(t), { lead: t.trim(), rest: "" }, JSON.stringify(t));
  assert.deepStrictEqual(P(undefined), { lead: "", rest: "" });
  assert.deepStrictEqual(P(null), { lead: "", rest: "" });
  // a mark at the very end is the end of the one sentence
  assert.deepStrictEqual(P("Ends with?"), { lead: "Ends with?", rest: "" });
  assert.deepStrictEqual(P("the pad face."), { lead: "the pad face.", rest: "" });
  // an abbreviation is not an end
  assert.deepStrictEqual(P("e.g. the pad"), { lead: "e.g. the pad", rest: "" });
  assert.deepStrictEqual(P("the pad (e.g. a TPU dot)"), { lead: "the pad (e.g. a TPU dot)", rest: "" });
  assert.deepStrictEqual(P("press bosses (i.e. The pads) carry it. Then the lid."),
    { lead: "press bosses (i.e. The pads) carry it.", rest: "Then the lid." });
  assert.deepStrictEqual(P("tol vs. the slot width. Measure it."), { lead: "tol vs. the slot width.", rest: "Measure it." });
  // a number at a sentence's end ends it after the number, never inside it
  assert.deepStrictEqual(P("the stick is v1.2. It holds."), { lead: "the stick is v1.2.", rest: "It holds." });
  // a range, an ellipsis and an enumerator are not ends
  assert.deepStrictEqual(P("spanning y -4.4 .. +12.0 on the wall. The row moved."),
    { lead: "spanning y -4.4 .. +12.0 on the wall.", rest: "The row moved." });
  assert.deepStrictEqual(P("wait... Then it clicks. Done."), { lead: "wait... Then it clicks.", rest: "Done." });
  assert.deepStrictEqual(P("four things: 1. THE OPENING IS A RECTANGLE. 2. THE SHELL."),
    { lead: "four things: 1. THE OPENING IS A RECTANGLE.", rest: "2. THE SHELL." });
  // a note that opens with a period has no empty lead
  assert.deepStrictEqual(P(". the lid. The rest."), { lead: ". the lid.", rest: "The rest." });
  assert.deepStrictEqual(P(".hidden file"), { lead: ".hidden file", rest: "" });
  // a sentence may open in lower case: these notes open them with identifiers
  assert.deepStrictEqual(P('two of them. part="coupon" prints it.'), { lead: "two of them.", rest: 'part="coupon" prints it.' });
  // "!" ends a sentence too
  assert.deepStrictEqual(P("Do not clamp the glass! It cracks."), { lead: "Do not clamp the glass!", rest: "It cracks." });
});

test("C24: the parameter dump shows each group's lead, and the whole note only where there is more", () => {
  const src = readFileSync(join(ROOT, "assets/enclosure-lab.js"), "utf8");
  const dump = /function renderParamDump\(scad, open\) \{\n([\s\S]*?)\n\}\n/.exec(src)?.[1] || "";
  assert.ok(dump.includes("const { lead, rest } = noteParts(g.note);"), "the dump splits each note");
  assert.ok(dump.includes('lead ? el("span", "muted", "  " + lead) : ""'), "the summary carries the lead");
  assert.ok(dump.includes('if (rest) d.append(el("div", "pnote muted", String(g.note).trim()));'),
    "the whole note is the group's first row, and only when there is more than the lead (no empty row)");
  assert.ok(!/g\.note \?/.test(dump), "the summary no longer carries the whole note");
  const css = readFileSync(join(ROOT, "assets/canary-local.css"), "utf8");
  assert.match(css, /\n\.pnote \{[^}]+\}/, "the note row has its rule");
});
