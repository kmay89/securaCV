#!/usr/bin/env node
// Stage the Lab's web root for the native bundle — desktop-lab/dist/.
//
// WHY: Tauri embeds `frontendDist` and serves it as the entire origin. Until
// this script existed, `frontendDist` was `canary-local/` itself, so every URL
// the frontend aims at its SIBLING enclosure library —
// `../docs/hardware/enclosure/…`, used by the workshop, the enclosure lab, the
// assembly lab and the chooser for preview images, STL meshes and .scad
// downloads — walked out of the bundle and 404'd. In the app you saw a broken
// image where a package render belongs, and an empty 3D viewport for every part
// whose mesh is not one of the few committed under canary-local/enclosures/.
// In a browser the same URLs are fine, because there canary-local/ has a real
// parent. The fix is packaging, not frontend: give the app a root that has the
// same parent, mirroring the layout the website already deploys.
//
// The mirror list lives in ../frontend-stage.json (shared with the CI drift
// gate, canary-local/tests/lab_bundle.test.js) — not here — so the manifest is
// one file both the builder and the guard read.
//
// Run: node scripts/stage-frontend.mjs   (tauri runs it via before*Command)

import { cp, mkdir, rm, readFile, writeFile, stat, readdir } from "node:fs/promises";
import { existsSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const APP = resolve(HERE, "..");           // desktop-lab/
const REPO = resolve(APP, "..");           // repo root
const DIST = join(APP, "dist");

const manifest = JSON.parse(await readFile(join(APP, "frontend-stage.json"), "utf8"));

const die = (msg) => {
  // ::error:: so a CI run names the problem in THIS step, rather than letting
  // the bundler abort minutes later with "resource … doesn't exist".
  console.error(`::error::stage-frontend: ${msg}`);
  process.exit(1);
};

// Count what we shipped, so the build log says how big the web root is instead
// of leaving "did it copy?" to be answered by unzipping a .dmg.
async function measure(dir) {
  let files = 0;
  let bytes = 0;
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const p = join(dir, entry.name);
    if (entry.isDirectory()) {
      const sub = await measure(p);
      files += sub.files;
      bytes += sub.bytes;
    } else {
      files += 1;
      bytes += (await stat(p)).size;
    }
  }
  return { files, bytes };
}

await rm(DIST, { recursive: true, force: true });
await mkdir(DIST, { recursive: true });

for (const rel of manifest.mirror) {
  const src = join(REPO, rel);
  if (!existsSync(src)) die(`${rel} is in frontend-stage.json but not in the repo`);
  const dest = join(DIST, rel);
  await mkdir(dirname(dest), { recursive: true });
  // dereference: RELEASE_LESSONS principle 1 — a symlink copied AS a link
  // dangles once it is inside a bundle, and bundlers reject that with a
  // confusing "doesn't exist" long after this step reported success.
  await cp(src, dest, { recursive: true, dereference: true });
}

for (const rel of manifest.prune) {
  await rm(join(DIST, rel), { recursive: true, force: true });
}

// The bundle root mirrors the repo root, so the entry page is one level down.
// tauri.conf.json points the window straight at it; this redirect is the same
// one the site deploys at its root, and keeps a bare "/" working.
await writeFile(
  join(DIST, "index.html"),
  `<!doctype html>
<meta charset="utf-8">
<meta http-equiv="refresh" content="0; url=${manifest.entry}">
<title>SecuraCV Lab</title>
<a href="${manifest.entry}">Open the Lab</a>
`,
);

for (const rel of [manifest.entry, ...manifest.sentinels]) {
  const p = join(DIST, rel);
  if (!existsSync(p)) die(`staged root is missing ${rel} — the app would ship it broken`);
  if ((await stat(p)).size === 0) die(`staged ${rel} is empty`);
}

const { files, bytes } = await measure(DIST);
console.log(
  `stage-frontend: ${manifest.mirror.join(", ")} → desktop-lab/dist ` +
    `(${files} files, ${(bytes / 1024 / 1024).toFixed(1)} MB), entry ${manifest.entry}`,
);
