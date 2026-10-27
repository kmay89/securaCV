// canary-local/tests/flash_mirror_probe.mjs — the flasher reads its release
// from its own origin, in a real browser, under its real policy.
//
// A browser can never fetch a github.com release asset: the /releases/download/
// redirect carries no Access-Control-Allow-Origin and neither does the asset
// host, so on the hosted Lab every manifest and image fetch failed and the page
// called every product unavailable (.github/RELEASE_LESSONS.md, 2026-10-27).
// pages.yml now copies the pinned releases beside the pages, at
// firmware/<tag>/<file> (tools/mirror_release.py), and flash-core.js
// fetchReleaseAsset reads that copy and nothing else. flash.test.js pins the
// mapping and that flash.js / we2-flash.js route every release read through
// it; this probe proves it in Chromium, with flash.html's generated CSP
// enforced:
//
//   · a PUBLIC host (lab.example.test, mapped to loopback) and a LOOPBACK
//     page (127.0.0.1) alike: the stable manifest, one of its images, the
//     Vision model manifest and its model, and the dev channel's manifest and
//     an image it names under another tag all come from the page's own
//     origin, each image's SHA-256 matches its manifest, a release that is not
//     mirrored answers the mirror's 404, and not one request leaves for
//     github.com or its asset host. The loopback page is the one that used to
//     fall back to the release URL, which no browser can read from any
//     origin; the policy names neither host, so a fallback that came back
//     would show here as a request or a CSP violation.
//
// The mirror served here is a fixture built in memory (the release's real
// layout, made-up bytes): the probe proves where the page looks, not what a
// release holds. It cannot drive an install: that needs a board on a cable
// (flash.js reaches the manifest only after a chip answers), so it calls the
// helper flash.js and we2-flash.js call, from the page, with the same
// arguments.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set). Prints
// FLASH_MIRROR_PROBE_OK / exits 0 on success.

import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { readFileSync } from "node:fs";
import { createHash, randomBytes } from "node:crypto";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { indexTree, lookup } from "./probe_server.mjs";

const ROOT = resolve(join(dirname(fileURLToPath(import.meta.url)), "../.."));
const FILES = indexTree(ROOT);
const TYPES = {
  ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const fail = (m) => { console.error("FLASH_MIRROR_PROBE_FAIL:", m); process.exit(1); };

// ── the fixture mirror, laid out as pages.yml lays out the real one ─────────
const catalog = JSON.parse(readFileSync(join(ROOT, "canary-local/devices/flash.json"), "utf8"));
const core = await import("../assets/flash-core.js");
const pin = core.releaseAssetParts(catalog.manifest_url, catalog.repo);
const visionPin = core.releaseAssetParts(catalog.we2_module.manifest_url, catalog.repo);
const devPin = core.releaseAssetParts(core.DEV_FLASH_MANIFEST_URL, catalog.repo);
if (!pin || !visionPin || !devPin) fail("a pinned manifest URL is not a release-download URL the page can map");
const REL = (tag, file) => `https://github.com/${catalog.repo}/releases/download/${tag}/${file}`;
const sha256 = (b) => createHash("sha256").update(b).digest("hex");
const product = catalog.products[0];
const DEV_TAG = "fw-v9.9.9-dev.1"; // a dev manifest names images under the real dev tag

const image = randomBytes(8192);
const model = randomBytes(4096);
const devImage = randomBytes(6144);
const manifest = {
  schema: core.MANIFEST_SCHEMA, fw_train: catalog.fw_train, repo: catalog.repo,
  products: { [product.id]: {
    version: catalog.fw_train, chipFamily: product.chip,
    factory: REL(pin.tag, "probe-factory.bin"), sha256: sha256(image), size: image.length,
  } },
};
const visionManifest = {
  version: catalog.fw_train, tag: visionPin.tag,
  model: { name: "probe-model.tflite", url: REL(visionPin.tag, "probe-model.tflite"),
    sha256: sha256(model), size: model.length },
};
const devManifest = {
  schema: core.MANIFEST_SCHEMA, fw_train: "9.9.9-dev.1", repo: catalog.repo,
  products: { [product.id]: {
    version: "9.9.9-dev.1", chipFamily: product.chip,
    factory: REL(DEV_TAG, "probe-dev-factory.bin"), sha256: sha256(devImage), size: devImage.length,
  } },
};
const at = (tag, file) => `/canary-local/${core.RELEASE_MIRROR_DIR}/${tag}/${file}`;
const MIRROR = new Map([
  [at(pin.tag, pin.file), Buffer.from(JSON.stringify(manifest))],
  [at(pin.tag, "probe-factory.bin"), image],
  [at(visionPin.tag, visionPin.file), Buffer.from(JSON.stringify(visionManifest))],
  [at(visionPin.tag, "probe-model.tflite"), model],
  [at(devPin.tag, devPin.file), Buffer.from(JSON.stringify(devManifest))],
  [at(DEV_TAG, "probe-dev-factory.bin"), devImage],
]);

// ── one origin serves the repo and the mirror; every request is logged ──────
const served = [];
const server = createServer(async (req, res) => {
  try {
    const path = req.url.split("?")[0];
    served.push(path);
    if (path === "/favicon.ico") { res.writeHead(204); return res.end(); }
    const mirrored = MIRROR.get(path);
    if (mirrored) {
      res.writeHead(200, { "content-type": "application/octet-stream" });
      return res.end(mirrored);
    }
    // the URL never becomes a path: it is looked up in the tree's index
    // (probe_server.mjs), so the path that reaches readFile is the index's
    const file = lookup(FILES, req.url);
    if (!file) { res.writeHead(404); return res.end("not found"); }
    const body = await readFile(file);
    res.writeHead(200, { "content-type": TYPES[extname(file)] || "application/octet-stream" });
    res.end(body);
  } catch { res.writeHead(404); res.end("not found"); }
});
await new Promise((ok) => server.listen(0, "127.0.0.1", ok));
const port = server.address().port;
const PUBLIC = `http://lab.example.test:${port}`;

const browser = await pw.chromium.launch({
  ...(process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}),
  // lab.example.test stands in for the Pages host, resolved to this loopback
  // server. Over plain http it is not a secure context (headless Chromium
  // ignores --unsafely-treat-insecure-origin-as-secure), so the page boots to
  // its no-Web-Serial card there, and the SHA-256s are taken here in node;
  // neither changes what is fetched from where, which is the claim.
  args: ["--host-resolver-rules=MAP lab.example.test 127.0.0.1"],
});

// Everything the pages try to send off-origin, recorded and refused: the
// release host and its asset CDN are exactly what a browser cannot read.
const offsite = [];
async function openPage(origin) {
  const page = await browser.newPage();
  const errors = [];
  // A console error carries the URL it is about, so the one load each page
  // is MEANT to fail (the unmirrored release) can be told from any other.
  page.on("console", (m) => {
    if (m.type() === "error") errors.push({ text: "console: " + m.text(), url: (m.location() || {}).url || "" });
  });
  page.on("pageerror", (e) => errors.push({ text: "pageerror: " + String(e), url: "" }));
  await page.addInitScript(() => {
    window.__csp = [];
    document.addEventListener("securitypolicyviolation",
      (e) => window.__csp.push(`${e.violatedDirective} ${e.blockedURI}`));
  });
  await page.route(/^https:\/\/(?:github\.com|[^/]*\.githubusercontent\.com)\//, (route) => {
    offsite.push(route.request().url());
    return route.abort("blockedbyclient");
  });
  await page.goto(`${origin}/canary-local/flash.html`, { waitUntil: "load", timeout: 30000 });
  // A booted page: the Connect phase (a secure context, so Web Serial) or the
  // no-Web-Serial card (lab.example.test over plain http).
  await page.waitForSelector("#flash .flash-connect, #flash .flash-unsupported", { timeout: 15000 })
    .catch(() => fail(`${origin}: the flasher page did not boot`));
  return { page, errors };
}

// In the page: the helper flash.js (fetchRelease) and we2-flash.js
// (fromRelease) call, with their arguments, on every pin.
async function readReleases(page, product, devTag) {
  return page.evaluate(async ({ product, devTag }) => {
    const core = await import("./assets/flash-core.js");
    const cat = await (await fetch("devices/flash.json")).json();
    const rel = (url, init) => core.fetchReleaseAsset(url, { repo: cat.repo, init });
    // The bytes go back to node to be hashed (no crypto.subtle off a secure context).
    const bytes = (buf) => [...new Uint8Array(buf)];
    const out = {};
    const man = await (await rel(cat.manifest_url, { cache: "no-store" })).json();
    const entry = man.products[product];
    const img = await (await rel(entry.factory, { cache: "no-store" })).arrayBuffer();
    out.image = { want: entry.sha256, got: bytes(img), size: img.byteLength };
    const vm = await (await rel(cat.we2_module.manifest_url, { cache: "no-store" })).json();
    const mdl = await (await rel(vm.model.url)).arrayBuffer();
    out.model = { want: vm.model.sha256, got: bytes(mdl) };
    const dm = await (await rel(core.DEV_FLASH_MANIFEST_URL, { cache: "no-store" })).json();
    const dimg = await (await rel(dm.products[product].factory, { cache: "no-store" })).arrayBuffer();
    out.dev = { want: dm.products[product].sha256, got: bytes(dimg), tagged: dm.products[product].factory.includes(devTag) };
    // A release this site does not carry (an uncut tag): the mirror's answer.
    const missing = `https://github.com/${cat.repo}/releases/download/fw-v0.0.0-probe/manifest-flash.json`;
    try {
      const r = await rel(missing, { cache: "no-store" });
      out.missing = { status: r.status };
    } catch (e) {
      out.missing = { threw: String(e && e.message || e) };
    }
    out.csp = window.__csp.slice();
    return out;
  }, { product, devTag });
}

// readReleases hands back each asset's bytes; hash them here.
const digests = (got) => {
  for (const k of ["image", "model", "dev"]) got[k].got = sha256(Buffer.from(got[k].got));
  return got;
};
const mirrorPaths = [...MIRROR.keys()];
const MISSING_PATH = at("fw-v0.0.0-probe", "manifest-flash.json");
// Every error but the deliberate miss: the mirror's 404 for it.
const unexpected = (errors, origin) => errors
  .filter((e) => !(/Failed to load resource/.test(e.text) && e.url === origin + MISSING_PATH))
  .map((e) => `${e.text}${e.url ? " @ " + e.url : ""}`);
try {
  // ── a public host, then a loopback page: the copy, and only the copy ─────
  for (const [label, origin] of [["public host", PUBLIC], ["loopback", `http://127.0.0.1:${port}`]]) {
    served.length = 0;
    const { page, errors } = await openPage(origin);
    const got = digests(await readReleases(page, product.id, DEV_TAG));
    for (const k of ["image", "model", "dev"]) {
      if (got[k].want !== got[k].got) fail(`${label}: ${k} SHA-256 ${got[k].got} != manifest ${got[k].want}`);
    }
    if (got.image.size !== image.length) fail(`${label}: image is ${got.image.size} bytes, not ${image.length}`);
    if (!got.dev.tagged) fail(`${label}: the dev manifest's image did not name the dev tag`);
    for (const p of mirrorPaths) {
      if (!served.includes(p)) fail(`${label}: ${p} was never read from the page's own origin`);
    }
    if (got.missing.status !== 404) {
      fail(`${label}: an unmirrored release answered ${JSON.stringify(got.missing)}, not the mirror's 404`);
    }
    if (!served.includes(MISSING_PATH)) fail(`${label}: the unmirrored release was not looked for in the mirror`);
    if (offsite.length) fail(`${label}: requests left the origin:\n  ${offsite.join("\n  ")}`);
    if (got.csp.length) fail(`${label}: CSP violations:\n  ${got.csp.join("\n  ")}`);
    const pageErrors = unexpected(errors, origin);
    if (pageErrors.length) fail(`${label}: page errors:\n  ` + pageErrors.join("\n  "));
    console.log(`${label}: ${mirrorPaths.length} mirror files read same-origin, ` +
      "SHA-256 matches, unmirrored release → 404, 0 off-origin requests");
  }

  console.log("FLASH_MIRROR_PROBE_OK");
  process.exit(0);
} catch (e) {
  fail(String((e && e.stack) || e));
} finally {
  await browser.close().catch(() => {});
  server.close();
}
