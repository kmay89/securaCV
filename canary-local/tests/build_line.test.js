// canary-local/tests/build_line.test.js — the build line's completeness gate.
//
// build-line.README.md makes a structural promise: "If it's not in here, it
// doesn't appear on the line. That's the point: nothing gets orphaned." And
// the cross-surface standard (docs/design/help_ecosystem_layout.md, rule 1)
// says a surface's map lives in one machine-readable place and CI FAILS when
// the map and the surface disagree. The website repo has that gate
// (tests/site-map.test.mjs); until this file, the Lab did not — which is how
// three finished, tested benches (catalog.html, find.html, smoke.html) sat
// off the line, invisible to the shell, the room, the site map, and the
// desktop app, while remaining perfectly functional. This test makes that
// class of drift impossible: every committed page must be on the manifest,
// every manifest reference must resolve, and the manifest's own invariants
// (unique slugs, resolvable redirects, room slugs, the room's offline slug
// map) hold by construction.
const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync } = require("node:fs");
const { join, relative, sep, basename } = require("node:path");

const ROOT = join(__dirname, "..");            // canary-local/
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");
const M = JSON.parse(read(join(ROOT, "build-line.json")));

// ── helpers over the manifest ────────────────────────────────────────────────

const stageBenches = (s) =>
  s.tracks ? s.tracks.flatMap((t) => t.benches) : s.benches || [];
const allBenches = M.stages.flatMap(stageBenches);
const allSite = M.stages.flatMap((s) => s.site || []);
const allDepths = allBenches.flatMap((b) => b.depths || []);
const docItems = M.htmlDocumentation.groups.flatMap((g) => g.items);

// Every local page the manifest can put on a surface.
const referencedPages = new Set(
  [
    M.onramp.lab,
    ...allBenches.map((b) => b.lab),
    ...allDepths.map((d) => d.lab),
    ...docItems.map((i) => i.href).filter((h) => !/^https?:/.test(h)),
  ].map((p) => p.split("#")[0]),
);

// ── the manifest's own invariants ────────────────────────────────────────────

test("six stages, 1..6, ids unique; every stage has benches or tracks", () => {
  assert.strictEqual(M.stages.length, 6);
  assert.deepStrictEqual(M.stages.map((s) => s.n), [1, 2, 3, 4, 5, 6]);
  assert.strictEqual(new Set(M.stages.map((s) => s.id)).size, 6);
  for (const s of M.stages) {
    assert.ok(stageBenches(s).length > 0, `stage ${s.id} has no benches`);
    for (const b of stageBenches(s)) {
      for (const k of ["noun", "slug", "lab", "desc"]) {
        assert.ok(b[k], `stage ${s.id} bench ${b.slug || b.noun}: missing ${k}`);
      }
    }
  }
});

test("slugs are unique across benches and site links (the shell's route space)", () => {
  const slugs = [
    M.onramp.slug,
    ...allBenches.map((b) => b.slug),
    ...allSite.map((x) => x.slug).filter(Boolean),
  ];
  const dupes = slugs.filter((s, i) => slugs.indexOf(s) !== i);
  assert.deepStrictEqual(dupes, [], `duplicate slugs: ${dupes}`);
  // Reserved shell views can never be shadowed by a bench.
  for (const reserved of ["overview", "all"]) {
    assert.ok(!slugs.includes(reserved), `slug "${reserved}" shadows a shell view`);
  }
});

test("every lab page the manifest names exists on disk", () => {
  for (const page of referencedPages) {
    let ok = true;
    try { readFileSync(join(ROOT, page)); } catch { ok = false; }
    assert.ok(ok, `build-line.json names "${page}", which is not on disk`);
  }
});

test("every redirect target resolves to a defined slug ($redirects_note, enforced)", () => {
  const known = new Set([
    M.onramp.slug,
    ...allBenches.map((b) => b.slug),
    ...allSite.map((x) => x.slug).filter(Boolean),
  ]);
  for (const [from, to] of Object.entries(M.redirects)) {
    assert.ok(known.has(to), `redirect "${from}" → "${to}" points at no defined slug`);
    assert.ok(!known.has(from), `redirect source "${from}" is also a live slug`);
  }
});

test("every room station slug resolves; stations are exactly 1..6", () => {
  const known = new Set([
    M.onramp.slug,
    ...allBenches.map((b) => b.slug),
    ...allSite.map((x) => x.slug).filter(Boolean),
  ]);
  assert.deepStrictEqual(Object.keys(M.room).sort(), ["1", "2", "3", "4", "5", "6"]);
  for (const [station, tools] of Object.entries(M.room)) {
    for (const t of tools) {
      assert.ok(t.e && t.slug, `room station ${station}: tool needs {e, slug}`);
      assert.ok(known.has(t.slug), `room station ${station}: unknown slug "${t.slug}"`);
    }
  }
});

// ── the completeness gate: no committed page may be off the line ────────────

// Pages that are legitimately not navigable surfaces: generator inputs and
// test fixtures. Everything else committed under canary-local must be a
// bench, a depth, the onramp, or an htmlDocumentation entry.
const NOT_A_SURFACE = new Set([
  "voice/template.html",
  // The vendored Witness Wall emulator (see witness/PROVENANCE.txt): an
  // embedded asset iframed by the witness-wall.html bench, not a nav surface.
  // scripts/check_witness_emulator_sync.sh keeps it byte-identical to the
  // Flasher's copy.
  "witness/witness.html",
]);
const SKIP_DIRS = new Set(["tests", "node_modules", "third_party", ".build"]);

function htmlPages(dir, out = []) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (SKIP_DIRS.has(entry.name)) continue;
    const p = join(dir, entry.name);
    if (entry.isDirectory()) htmlPages(p, out);
    else if (entry.name.endsWith(".html")) out.push(relative(ROOT, p).split(sep).join("/"));
  }
  return out;
}

test("every committed page is on the build line (bench, depth, onramp, or documentation)", () => {
  for (const page of htmlPages(ROOT)) {
    if (NOT_A_SURFACE.has(page)) continue;
    assert.ok(
      referencedPages.has(page),
      `${page} is committed but appears nowhere in build-line.json — add it as a ` +
        `bench/depth under the right stage if it's part of the build journey, or ` +
        `under htmlDocumentation.groups if it's a support page ` +
        `(see build-line.README.md, "HTML documentation inventory")`,
    );
  }
});

// ── the once-orphaned benches stay registered (eyes.test.js house style) ────

test("the Case Catalog is a Build bench and the guided finder is its depth", () => {
  const build = M.stages.find((s) => s.id === "build");
  const cat = build.benches.find((b) => b.slug === "catalog");
  assert.ok(cat, "catalog.html must be registered as a bench under Build");
  assert.strictEqual(cat.lab, "catalog.html");
  assert.ok(
    (cat.depths || []).some((d) => d.lab === "find.html"),
    "find.html must be a depth of The Case Catalog",
  );
});

test("the smoke-alarm listener is a Sound-track bench, marked real", () => {
  const sense = M.stages.find((s) => s.id === "sense");
  const sound = (sense.tracks || []).find((t) => t.track === "sound");
  assert.ok(sound, "stage 4 must carry a sound track — 'more senses will dock here'");
  const smoke = sound.benches.find((b) => b.lab === "smoke.html");
  assert.ok(smoke, "smoke.html must be registered under the sound track");
  assert.strictEqual(smoke.real, true, "the bench boots real firmware wasm — say so");
  assert.match(
    sense.optionsNote || "",
    /sound/i,
    "stage 4's optionsNote must name every docked sense, including sound",
  );
});

test("the Lab's own site map page is in the manifest it renders", () => {
  assert.ok(
    docItems.some((i) => i.href === "site-map.html"),
    "site-map.html must be listed in htmlDocumentation — a map that omits itself is off the line",
  );
});

// ── the room's offline slug map matches the manifest ────────────────────────

test("the room's FILE2SLUG fallback (assets/room.js) covers every bench and depth page", () => {
  // The room's script is an external file (the page carries a no-inline-script
  // CSP), so the offline slug map is read from there.
  const room = read(join(ROOT, "assets/room.js"));
  const m = room.match(/var FILE2SLUG = \{([^}]*)\}/);
  assert.ok(m, "assets/room.js lost its FILE2SLUG fallback map");
  const map = {};
  for (const pair of m[1].matchAll(/'([^']+)':'([^']+)'/g)) map[pair[1]] = pair[2];

  const expect = { [basename(M.onramp.lab, ".html")]: M.onramp.slug };
  for (const b of allBenches) {
    expect[basename(b.lab, ".html")] = b.slug;
    for (const d of b.depths || []) expect[basename(d.lab, ".html")] ||= b.slug;
  }
  for (const [file, slug] of Object.entries(expect)) {
    assert.strictEqual(
      map[file], slug,
      `room.js FILE2SLUG: "${file}" should map to "${slug}" (got "${map[file]}") — ` +
        `the offline fallback must route every bench page the manifest routes`,
    );
  }
});

// ── CI wiring: this gate cannot be silently dropped ─────────────────────────

test("CI runs this gate", () => {
  const workflow = read(join(REPO, ".github/workflows/canary-local.yml"));
  assert.ok(
    workflow.includes("canary-local/tests/build_line.test.js"),
    "build_line.test.js is not wired into canary-local.yml",
  );
});
