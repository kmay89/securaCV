/*
 * The evidence pack ships the offline viewer, and the integration copies it
 * from this directory (contracts C7). The file here must be the viewer the
 * repo builds and tests — byte for byte — or a recipient gets a viewer nobody
 * tested. Run: node --test custom_components/securacv/www/securacv-evidence-viewer.test.js
 *
 * To refresh: `node viewer/build.mjs && cp viewer/evidence_viewer.html
 * custom_components/securacv/www/securacv-evidence-viewer.html`.
 */
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");

const WWW = __dirname;
const REPO = path.join(WWW, "..", "..", "..");

test("securacv-evidence-viewer.html is byte-identical to the built viewer/evidence_viewer.html", () => {
  const shipped = fs.readFileSync(path.join(WWW, "securacv-evidence-viewer.html"));
  const built = fs.readFileSync(path.join(REPO, "viewer", "evidence_viewer.html"));
  assert.ok(shipped.equals(built),
    "custom_components/securacv/www/securacv-evidence-viewer.html differs from viewer/evidence_viewer.html — "
    + "run `node viewer/build.mjs` and copy the result over");
});

test("the shipped viewer is self-contained and reads the pack it travels in", () => {
  const html = fs.readFileSync(path.join(WWW, "securacv-evidence-viewer.html"), "utf8");
  // Offline by construction: no external script, stylesheet or fetch.
  assert.doesNotMatch(html, /<script[^>]+src=/);
  assert.doesNotMatch(html, /<link[^>]+href=["']https?:/);
  assert.doesNotMatch(html, /\bfetch\(/);
  // The three cores are inlined, and the drop zone takes the pack itself.
  assert.match(html, /globalThis\.SecuraCVVerify = api/);
  assert.match(html, /globalThis\.SecuraCVTimeline = timelineApi/);
  assert.match(html, /globalThis\.SecuraCVZip = zipApi/);
  assert.match(html, /Drop an evidence pack \(\.zip\) or event export \(\.json\)/);
});

test("evidence-README.txt is the recipient's recipe, in plain words", () => {
  const text = fs.readFileSync(path.join(WWW, "evidence-README.txt"), "utf8");
  assert.match(text, /evidence_viewer\.html/);
  assert.match(text, /Nothing is uploaded/);
  assert.match(text, /self-attested/);
  assert.match(text, /WITNESS_KEY\.txt/);
  assert.match(text, /does not prove/i);
  // Two paragraphs of recipe before the reference material: the first says
  // what the file is and how to open it, the second what the verdicts mean.
  const paragraphs = text.split(/\n\s*\n/).filter((p) => p.trim());
  assert.ok(paragraphs.length >= 2);
  assert.match(paragraphs[0], /zip|pack/i);
});
