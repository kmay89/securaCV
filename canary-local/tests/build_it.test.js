// Host tests for assets/build-it.js — the BOM row's distributor link.
//
// The link is the one place a fetched third-party string becomes an href a
// visitor clicks. bom_pricing.py checks it on fetch and gen_enclosures.py
// checks it again on generate; this pins the third check, at the point of
// use, so a build.json from anywhere still can't put a hostile scheme in a
// row. The graceful case matters just as much: no URL must render exactly
// the muted "mfr · mpn" text it always did, never a dead link.

const { test } = require("node:test");
const assert = require("node:assert");

// build-it.js uses the ambient `document` (via its `el` helper), so a tiny
// global stub is enough — no browser, no jsdom.
globalThis.document = {
  createElement: (tag) => ({
    tag,
    className: "",
    textContent: "",
    href: undefined,
    target: undefined,
    rel: undefined,
    title: undefined,
    listeners: {},
    children: [],
    append(...kids) { this.children.push(...kids); },
    addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); },
  }),
};

const mod = () => import("../assets/build-it.js");

const row = (url) => ({
  ref: "U1", qty: "1", mfr: "Seeed Studio", mpn: "102010469",
  live: url === undefined ? undefined : { unit_usd: 13.99, stock: 42, src: "digikey", url },
});

test("a verified row links to the distributor, safely", async () => {
  const { partNode, liveUrl } = await mod();
  const url = "https://www.digikey.com/en/products/detail/seeed/102010469/1";
  assert.equal(liveUrl(row(url)), url);

  const n = partNode(row(url));
  assert.equal(n.tag, "a");
  assert.equal(n.href, url);
  assert.equal(n.textContent, "Seeed Studio · 102010469");
  assert.equal(n.target, "_blank");
  // noreferrer as well as noopener: the distributor gets a click, not our
  // visitor's page URL.
  assert.equal(n.rel, "noopener noreferrer");
});

test("clicking the link doesn't also toggle the row's note", async () => {
  const { partNode } = await mod();
  const n = partNode(row("https://www.digikey.com/x"));
  let stopped = 0;
  n.listeners.click.forEach((fn) => fn({ stopPropagation: () => { stopped++; } }));
  assert.equal(stopped, 1);
});

test("a seeded row is plain muted text, not a link", async () => {
  const { partNode, liveUrl } = await mod();
  for (const r of [row(undefined), row(null), row("")]) {
    assert.equal(liveUrl(r), null);
    const n = partNode(r);
    assert.equal(n.tag, "span", "must not be an anchor");
    assert.equal(n.className, "muted");
    assert.equal(n.textContent, "Seeed Studio · 102010469");
    assert.equal(n.href, undefined);
  }
});

test("a non-https url is never rendered as a link", async () => {
  const { partNode, liveUrl } = await mod();
  for (const bad of ["javascript:alert(1)", "http://www.digikey.com/x",
                     "//evil.example/x", "data:text/html,x", 42, {}]) {
    assert.equal(liveUrl(row(bad)), null, `liveUrl accepted ${String(bad)}`);
    assert.equal(partNode(row(bad)).tag, "span", `linked ${String(bad)}`);
  }
});

test("a row with no MPN renders nothing at all", async () => {
  const { partNode } = await mod();
  assert.equal(partNode({ ref: "X1", qty: "1", mfr: "Generic", mpn: "" }), "");
});
