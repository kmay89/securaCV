// Drift-gate: the family map. devices/ecosystem.json is the ONE place the
// SecuraCV family is named — each surface's canonical URL and an honest
// availability status. Two surfaces render their own copy of it: the iPhone
// app's EcosystemMap (ios/Shared/EcosystemMap.swift) and the desktop
// Flasher's Atlas (desktop/src/app.js ATLAS_LINKS). Before this file those
// copies were pinned only to their own honesty rules, not to each other —
// exactly the two-flashers drift this suite exists to stop.
//
// The gate is designed so a STATUS FLIP HURTS on purpose: the day the tvOS
// app (or the iPhone app) reaches its store, someone edits ecosystem.json's
// status from "store-pending" to "shipping" — and these tests then fail
// every consumer whose copy still says "pending", so all the surfaces stop
// telling the old story in the same change. Availability copy that drifts
// from status is an overclaim (non-negotiable #4) or a stale apology; both
// directions fail.
//
// Like desktop_parity.test.js this reads source TEXT (and evaluates the
// Atlas table with the same new Function technique), so it needs no Swift
// or Rust toolchain and runs in the "page logic tests" job — which matters:
// the iOS CI is gated on ios/** changes, so a JSON-only edit would never
// reach the Swift belt (EcosystemMapTests reads this file too, type-checked)
// without the text pins here.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..", "..");     // repo root
const CANARY = join(__dirname, "..");         // canary-local/
const read = (p) => readFileSync(p, "utf8");

const map = JSON.parse(read(join(CANARY, "devices/ecosystem.json")));
const STATUSES = new Set(["shipping", "in-browser", "store-pending"]);

// The surfaces each consumer must carry (a consumer may be a subset of the
// family — the iPhone app doesn't list itself — but never a stray superset).
const IOS_SURFACES = ["lab", "flasher", "lab-app", "witness-wall", "hub"];
const ATLAS_SURFACES = ["lab", "lab-app", "witness-wall", "ios-app"];

test("the family map is well-formed", () => {
  for (const base of [map.site, map.repo]) {
    assert.match(String(base), /^https:\/\/[^/]+(\/[^/]+)*$/,
      `ecosystem.json base "${base}" must be an https URL with no trailing slash`);
  }
  assert.deepStrictEqual(
    Object.keys(map.surfaces).sort(),
    ["flasher", "hub", "ios-app", "lab", "lab-app", "witness-wall"],
    "the family gained or lost a surface — update the consumers AND this list, together");

  for (const [id, s] of Object.entries(map.surfaces)) {
    assert.ok(s.name && typeof s.name === "string", `${id} has no name`);
    assert.ok(STATUSES.has(s.status),
      `${id} status "${s.status}" isn't in the vocabulary (${[...STATUSES].join(", ")})`);
    assert.ok(s.url.startsWith(map.site + "/") || s.url.startsWith(map.repo + "/"),
      `${id} url ${s.url} strays off the brand domains — routing, never a funnel`);
  }
});

test("the desktop Flasher's Atlas tells the same family story", () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const start = appJs.indexOf("const SITE = ");
  const atlasIdx = appJs.indexOf("const ATLAS_LINKS", start);
  const end = appJs.indexOf("\n];", atlasIdx);
  assert.ok(start >= 0 && atlasIdx > start && end > atlasIdx,
    "couldn't find SITE/REPO/ATLAS_LINKS in desktop/src/app.js — update this extraction");
  const atlas = new Function(
    appJs.slice(start, end + 3) + "\nreturn { SITE, REPO, ATLAS_LINKS };")();

  assert.strictEqual(atlas.SITE, map.site, "desktop SITE != ecosystem.json site");
  assert.strictEqual(atlas.REPO, map.repo, "desktop REPO != ecosystem.json repo");

  const cards = atlas.ATLAS_LINKS.flatMap(([, items]) => items)
    .map(([name, blurb, url]) => ({ name, blurb, url }));

  for (const id of ATLAS_SURFACES) {
    const s = map.surfaces[id];
    // Cards may carry a parenthetical the map doesn't ("… (Apple TV)").
    const card = cards.find((c) => c.name.startsWith(s.name));
    assert.ok(card, `the Atlas lost its ${id} card ("${s.name}")`);
    assert.strictEqual(card.url, s.url,
      `Atlas ${id} card links ${card.url}, ecosystem.json says ${s.url}`);
    if (s.status === "store-pending") {
      assert.match(card.blurb, /pending/i,
        `${id} is store-pending — the Atlas card must say so, not imply a download`);
    } else {
      assert.ok(!/pending/i.test(card.blurb),
        `${id} ships (${s.status}) but the Atlas card still apologizes with "pending"`);
    }
  }
});

test("the iPhone app's family map tells the same family story", () => {
  const swift = read(join(ROOT, "ios/Shared/EcosystemMap.swift"));

  assert.ok(swift.includes(`site = "${map.site}"`),
    "EcosystemMap.site != ecosystem.json site");
  assert.ok(swift.includes(`repo = "${map.repo}"`),
    "EcosystemMap.repo != ecosystem.json repo");
  // The two URL helpers the surface rows lean on — pin their suffixes so the
  // per-row resolution below stands on asserted ground, not on memory.
  assert.ok(swift.includes(`labURL = URL(string: site + "/lab")!`),
    "EcosystemMap.labURL moved — update this belt and the resolution below");
  assert.ok(swift.includes(`downloadURL = URL(string: site + "/download")!`),
    "EcosystemMap.downloadURL moved — update this belt and the resolution below");

  // Slice each surface's initializer out of the source and read its fields.
  const surfaceBlock = (id) => {
    const at = swift.indexOf(`id: "${id}"`);
    assert.ok(at >= 0, `the iPhone family map lost its ${id} surface`);
    const next = swift.indexOf("EcosystemSurface(", at);
    return swift.slice(at, next >= 0 ? next : swift.length);
  };
  const urlOf = (block, id) => {
    if (/url: labURL\)/.test(block)) return map.site + "/lab";
    if (/url: downloadURL\)/.test(block)) return map.site + "/download";
    const m = block.match(/url: URL\(string: (site|repo) \+ "([^"]+)"\)!/);
    assert.ok(m, `couldn't read the ${id} surface's url from EcosystemMap.swift`);
    return (m[1] === "site" ? map.site : map.repo) + m[2];
  };

  for (const id of IOS_SURFACES) {
    const s = map.surfaces[id];
    const block = surfaceBlock(id);
    const name = block.match(/name: "([^"]*)"/);
    const availability = block.match(/availability: "([^"]*)"/);
    assert.ok(name && availability, `couldn't parse the ${id} surface's fields`);
    assert.strictEqual(name[1], s.name,
      `iPhone map names ${id} "${name[1]}", ecosystem.json says "${s.name}"`);
    assert.strictEqual(urlOf(block, id), s.url,
      `iPhone map links ${id} somewhere ecosystem.json doesn't`);
    if (s.status === "store-pending") {
      assert.match(availability[1], /pending/i,
        `${id} is store-pending — the iPhone availability line must say so`);
    } else {
      assert.ok(!/pending/i.test(availability[1]),
        `${id} ships (${s.status}) but the iPhone copy still apologizes with "pending"`);
    }
  }

  // Nothing extra: a surface the iPhone invents is a claim the family map
  // never blessed (the const.py "and nothing extra" rule).
  for (const m of swift.matchAll(/^\s+id: "([a-z-]+)",\s*$/gm)) {
    assert.ok(map.surfaces[m[1]],
      `iPhone map carries "${m[1]}" — not a surface ecosystem.json names`);
  }
});

test("the Flasher's About panel derives its family footer from the Atlas", () => {
  const appJs = read(join(ROOT, "desktop/src/app.js"));
  const start = appJs.indexOf("const SITE = ");
  const atlasIdx = appJs.indexOf("const ATLAS_LINKS", start);
  const end = appJs.indexOf("\n];", atlasIdx);
  assert.ok(start >= 0 && atlasIdx > start && end > atlasIdx,
    "couldn't find SITE/REPO/ATLAS_LINKS in desktop/src/app.js — update this extraction");
  const atlas = new Function(
    appJs.slice(start, end + 3) +
    "\nreturn { ATLAS_FAMILY_GROUP, ATLAS_LINKS };")();

  // The named group exists and is the one carrying the family cards test 2
  // already pins to ecosystem.json — so the About footer, which renders THIS
  // group, inherits those pins for free.
  const group = atlas.ATLAS_LINKS.find(([t]) => t === atlas.ATLAS_FAMILY_GROUP);
  assert.ok(group, "ATLAS_FAMILY_GROUP doesn't name a group in ATLAS_LINKS");
  const names = group[1].map(([name]) => name);
  for (const id of ["ios-app", "lab-app", "witness-wall"]) {
    assert.ok(names.some((n) => n.startsWith(map.surfaces[id].name)),
      `the family group lost its ${id} card — the About footer would too`);
  }

  // And renderAbout really derives from it (a find on the named group), so
  // there is no third hand-mirrored copy of the family story to rot.
  assert.ok(appJs.includes("ATLAS_LINKS.find(([t]) => t === ATLAS_FAMILY_GROUP)"),
    "renderAbout no longer derives its family footer from ATLAS_FAMILY_GROUP — " +
    "if the footer grew its own copy, pin it here like the Atlas cards");
});

test("the Lab's family view reads ecosystem.json live, status copy derived", () => {
  const shell = read(join(CANARY, "assets/lab-shell.js"));

  // The consumer that structurally cannot drift: names/urls/statuses come off
  // the JSON at render time…
  assert.ok(shell.includes('fetch("devices/ecosystem.json"'),
    "lab-shell.js familyView no longer fetches devices/ecosystem.json at runtime");

  // …and the only mirrored piece is the status VOCABULARY, pinned here: the
  // copy map must cover exactly the statuses the family map may use, and the
  // word "pending" may appear only under store-pending (an apology on a
  // shipping surface and a download-implying line on a pending one both lie).
  const cStart = shell.indexOf("const FAMILY_STATUS_COPY = {");
  const cEnd = shell.indexOf("\n};", cStart);
  assert.ok(cStart >= 0 && cEnd > cStart,
    "couldn't find FAMILY_STATUS_COPY in lab-shell.js — update this extraction");
  const copy = new Function(
    shell.slice(cStart, cEnd + 3) + "\nreturn FAMILY_STATUS_COPY;")();

  assert.deepStrictEqual(Object.keys(copy).sort(), [...STATUSES].sort(),
    "FAMILY_STATUS_COPY and the ecosystem.json status vocabulary diverged");
  for (const [status, line] of Object.entries(copy)) {
    assert.ok(line && typeof line === "string", `${status} has no copy`);
    if (status === "store-pending") {
      assert.match(line, /pending/i,
        "store-pending copy must say pending, not imply a download");
    } else {
      assert.ok(!/pending/i.test(line),
        `${status} ships but its copy still apologizes with "pending"`);
    }
  }
});
