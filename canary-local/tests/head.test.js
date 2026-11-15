// canary-local/tests/head.test.js — every Lab page's <head>, in two halves.
//
// The Lab is a website: its pages are linked, shared and searched for. Until
// 2026-11 none had a canonical link or a share card, none said "SecuraCV" in
// its title, and twenty-one of twenty-six carried a description too long for
// a search result to show whole. tools/gen_head.py splits each head the way
// the website's scripts/make-head.mjs does:
//
//   · hand-written, in the page: <title> and the description — copy;
//   · derived, between two markers: the canonical link and the share tags.
//
// What can rot, and which test catches it:
//   · a title or description changed and nobody re-stamped  → "--check passes"
//   · a page ships with no block, or two                    → "exactly one derived block"
//   · a canonical goes relative again (index.html's was)     → "absolute canonical"
//   · a title drops the Lab's name, or outgrows a result    → "the hand-written half"
//   · a description regrows past what a result shows         → "the hand-written half"
//   · "nothing phones anywhere" comes back in a description  → "no retired claim"
//   · a <title> lands in <body> again (room.html's did)      → "the title is in the head"
//   · the share card stops being a file the website serves   → "the share card exists"
//   · the gate is dropped from CI                            → "CI runs this gate"
//
// Runs under "page logic tests" (.github/workflows/canary-local.yml); reads
// source text only, plus one run of the generator's own --check.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, readdirSync } = require("node:fs");
const { join } = require("node:path");
const { spawnSync } = require("node:child_process");

const ROOT = join(__dirname, "..");        // canary-local/
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");

const BASE = "https://kmay89.github.io/securaCV/canary-local/";
const PAGES = readdirSync(ROOT).filter((f) => f.endsWith(".html")).sort();
const html = new Map(PAGES.map((p) => [p, read(join(ROOT, p))]));
const headOf = (page) => {
  const text = html.get(page);
  const end = text.indexOf("</head>");
  assert.ok(end > 0, `${page} has no </head>`);
  return text.slice(0, end);
};
const unescape = (s) => s.replace(/&quot;/g, '"').replace(/&#x27;|&#39;/g, "'")
  .replace(/&lt;/g, "<").replace(/&gt;/g, ">").replace(/&amp;/g, "&");
const titleOf = (page) => unescape((/<title>([^<]*)<\/title>/.exec(headOf(page)) || [])[1] || "");
const descOf = (page) => unescape((/<meta name="description" content="([^"]*)"/.exec(headOf(page)) || [])[1] || "");

test("the Lab's pages are found", () => {
  assert.ok(PAGES.length >= 26, `expected the Lab's pages, found ${PAGES.length}`);
});

test("gen_head.py --check passes on the committed tree", () => {
  const r = spawnSync("python3", [join(ROOT, "tools/gen_head.py"), "--check"], { encoding: "utf8" });
  assert.strictEqual(r.status, 0, `gen_head.py --check failed (status ${r.status}):\n${r.stdout}${r.stderr}`);
});

test("every page carries exactly one derived block, inside its <head>", () => {
  for (const page of PAGES) {
    const text = html.get(page);
    const begins = text.split("<!-- head:derived").length - 1;
    const ends = text.split("<!-- /head:derived -->").length - 1;
    assert.strictEqual(begins, 1, `${page} has ${begins} derived-head blocks`);
    assert.strictEqual(ends, 1, `${page} has ${ends} derived-head closers`);
    assert.ok(headOf(page).includes("<!-- /head:derived -->"), `${page}: the derived block is outside <head>`);
  }
});

test("every page has one absolute canonical on the Lab's own address, and the share tags agree with it", () => {
  for (const page of PAGES) {
    const head = headOf(page);
    const canon = [...head.matchAll(/<link rel="canonical" href="([^"]*)"/g)].map((m) => m[1]);
    assert.strictEqual(canon.length, 1, `${page} has ${canon.length} canonical links`);
    assert.ok(canon[0].startsWith(BASE), `${page}: canonical ${canon[0]} is not an absolute Lab address`);
    // index.html is a redirect to the shell and says so; every other page is its own address.
    assert.strictEqual(canon[0], BASE + (page === "index.html" ? "lab.html" : page));
    const og = (p) => (new RegExp(`<meta property="og:${p}" content="([^"]*)"`).exec(head) || [])[1];
    assert.strictEqual(og("url"), canon[0], `${page}: og:url and the canonical link disagree`);
    assert.strictEqual(og("site_name"), "SecuraCV Lab");
    assert.strictEqual(unescape(og("description")), descOf(page), `${page}: og:description is not the page's description`);
    assert.ok(titleOf(page).startsWith(unescape(og("title"))), `${page}: og:title is not the page's title`);
    assert.match(head, /<meta name="twitter:card" content="summary_large_image"/);
  }
});

test("the hand-written half: a title that names the Lab, a description a result can show whole", () => {
  const titles = new Map(), descs = new Map();
  for (const page of PAGES) {
    const title = titleOf(page), desc = descOf(page);
    assert.ok(title, `${page} has no <title> in its head`);
    assert.match(title, / — SecuraCV(?: Lab)?$/, `${page}: title "${title}" must end with the Lab's name`);
    assert.ok(title.length <= 65, `${page}: title is ${title.length} characters`);
    assert.ok(!/canary\.local/.test(title),
      `${page}: "canary.local" in a title is a hostname to a search result, and a Canary's own`);
    assert.ok(desc.length >= 70 && desc.length <= 160, `${page}: description is ${desc.length} characters (70–160)`);
    assert.ok(!titles.has(title), `${page} and ${titles.get(title)} share a title`);
    assert.ok(!descs.has(desc), `${page} and ${descs.get(desc)} share a description`);
    titles.set(title, page);
    descs.set(desc, page);
  }
});

test("no description makes the retired network claim", () => {
  // "Nothing phones home / anywhere" was retired on the website by the claims
  // audit: devices and pages do make requests (updates, the device list). The
  // Lab's line is what the page does with what you give it.
  for (const page of PAGES) {
    assert.ok(!/phones? (?:home|anywhere)|nothing leaves/i.test(descOf(page)),
      `${page}: its description says what the network does not do — say "runs in your browser; nothing is uploaded"`);
  }
});

test("the title is in the head, once", () => {
  for (const page of PAGES) {
    const text = html.get(page);
    assert.strictEqual(text.split("<title>").length - 1, 1, `${page} has more than one <title>`);
    assert.ok(text.indexOf("<title>") < text.indexOf("</head>"), `${page}: its <title> is in the body`);
  }
});

test("the share card every page points at is a file the website says it publishes", () => {
  const contract = JSON.parse(read(join(ROOT, "devices/site_contract.json")));
  assert.ok(Array.isArray(contract.files), "the carried site contract has no file list");
  for (const page of PAGES) {
    const image = (/<meta property="og:image" content="([^"]*)"/.exec(headOf(page)) || [])[1];
    assert.ok(image, `${page} has no og:image`);
    const url = new URL(image);
    assert.strictEqual(url.origin, "https://securacv.com");
    assert.ok(contract.files.includes(url.pathname), `${page}: ${image} is not a file the website publishes`);
  }
});

test("CI runs this gate and the generator's --check", () => {
  const workflow = read(join(REPO, ".github/workflows/canary-local.yml"));
  for (const needle of [
    "node --test canary-local/tests/head.test.js",
    "python3 canary-local/tools/gen_head.py --check",
  ]) {
    assert.ok(workflow.includes(needle), `${needle} is not wired into canary-local.yml`);
  }
});
