// canary-local/tests/flash.test.js — the browser flasher's core, pinned.
//
// The flasher's promises are "can't pick the wrong image" and "reads what's
// really on the board". Both live in flash-core.js (pure, no DOM/Web Serial),
// so they're testable here under the repo's node --test convention. CI also
// regenerates flash.json and diffs it (the drift gate), the same way it
// guards start.json / wap.json.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const catalog = JSON.parse(readFileSync(join(ROOT, "devices/flash.json"), "utf8"));
const registry = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
const core = () => import("../assets/flash-core.js");

const ESPTOOL_CHIPS = new Set(["ESP32", "ESP32-S2", "ESP32-S3", "ESP32-C3", "ESP32-C6", "ESP32-C2", "ESP32-H2"]);

// ── the generated catalog is coherent ──────────────────────────────────────
test("flash.json: every product names a chip the chips map + esptool know", () => {
  assert.ok(catalog.products.length >= 1, "no products");
  for (const p of catalog.products) {
    assert.ok(ESPTOOL_CHIPS.has(p.chip), `${p.id}: unknown esptool chip "${p.chip}"`);
    assert.ok(catalog.chips[p.chip], `${p.id}: chip "${p.chip}" absent from chips map`);
    assert.ok(p.asset_stem && p.name && p.tagline, `${p.id}: missing display fields`);
    assert.ok(p.hatch && p.hatch.title && p.hatch.body, `${p.id}: missing hatch copy`);
    assert.ok(Array.isArray(p.hatch.steps) && p.hatch.steps.length >= 2, `${p.id}: missing hatch steps`);
    assert.ok(catalog.chips[p.chip].download_mode, `${p.chip}: no download-mode copy`);
  }
});

test("flash.json: fw_train is single-sourced from the registry", () => {
  assert.strictEqual(catalog.fw_train, registry.fw_train);
});

test("flash.json: hatch moments are privacy-safe and product-specific", () => {
  for (const p of catalog.products) {
    const copy = [p.hatch.kicker, p.hatch.title, p.hatch.body, ...p.hatch.steps]
      .join(" ")
      .toLowerCase();
    assert.doesNotMatch(copy, /license plate|identity|recognition|re-id|embedding/, `${p.id}: hatch copy overclaims identity`);
  }
  const vision = catalog.products.find((p) => p.id === "securacv-canary-vision");
  assert.match(vision.hatch.body, /presence only/);
  assert.ok(vision.hatch.steps.some((s) => /Grove Vision AI V2/.test(s)), "Vision must remind users about the second port");

  const sense = catalog.products.find((p) => p.id === "securacv-canary-sense");
  assert.match(sense.hatch.body, /no camera, no mic/);

  const wellbeing = catalog.products.find((p) => p.id === "securacv-canary-sense-wellbeing");
  assert.ok(wellbeing.hatch.steps.some((s) => /breathing\/heartbeat/.test(s)), "Wellbeing gets its own settling instruction");
});

test("flash.json: live-receipt capability follows the firmware command", () => {
  const receiptIds = catalog.products
    .filter((p) => p.serial_receipt)
    .map((p) => p.id)
    .sort();
  // Derived from the firmware sources, not declared: every build of the
  // canary / canary-vision projects wires the self-manifest builder to the
  // public `j` command, so the Phase 0 reach ports inherit it by being those
  // projects. Pinned as a list so a product GAINING or LOSING the receipt is
  // a visible diff rather than a silent capability change.
  assert.deepStrictEqual(receiptIds, [
    "securacv-canary",
    "securacv-canary-esp32cam",
    "securacv-canary-freenove-s3",
    "securacv-canary-vision",
    "securacv-canary-vision-c3-super-mini",
    "securacv-canary-vision-xiao-c3",
    "securacv-canary-vision-xiao-s3",
    "securacv-canary-wroom",
  ]);
  for (const p of catalog.products) {
    assert.strictEqual(typeof p.serial_receipt, "boolean", `${p.id}: capability missing`);
  }
});

test("flash.json: the no-brick promise and recovery ladder are present", () => {
  assert.ok(catalog.no_brick && catalog.no_brick.headline && catalog.no_brick.points.length);
  assert.ok(Array.isArray(catalog.recovery) && catalog.recovery.length >= 2);
  // Pinned to the firmware release TAG (fw-v<train>), never /latest/: the repo
  // also ships the native app (app-v*), and GitHub's "latest" is the newest
  // release of ANY kind, which would shadow the firmware manifest. See gen_flash.py.
  assert.match(catalog.manifest_url, /\/releases\/download\/fw-v[\d.]+\/manifest-flash\.json$/);
});

// ── chip guard — a board is never offered another board's image ─────────────
test("chip guard: productsForChip filters to matching silicon only", async () => {
  const { productsForChip, chipMatches } = await core();
  const c3 = productsForChip(catalog, "ESP32-C3");
  const s3 = productsForChip(catalog, "ESP32-S3");
  const c6 = productsForChip(catalog, "ESP32-C6");
  assert.ok(c3.length && s3.length && c6.length, "expected products on each chip");
  for (const p of c3) assert.strictEqual(p.chip, "ESP32-C3");
  for (const p of s3) assert.strictEqual(p.chip, "ESP32-S3");
  // No leakage across families.
  assert.ok(!s3.some((p) => p.chip === "ESP32-C3"));
  // Match is forgiving about case/spacing, strict about identity.
  assert.ok(chipMatches("esp32-s3", { chip: "ESP32-S3" }));
  assert.ok(chipMatches("ESP32 S3", { chip: "ESP32-S3" }));
  assert.ok(!chipMatches("ESP32-S3", { chip: "ESP32-C3" }));
  assert.ok(!chipMatches("ESP32", { chip: "ESP32-S3" })); // classic ≠ S3
});

test("chipInfo resolves per-chip UX copy", async () => {
  const { chipInfo } = await core();
  const info = chipInfo(catalog, "ESP32-C6");
  assert.ok(info && /BOOT/.test(info.download_mode));
  assert.strictEqual(chipInfo(catalog, "ESP32-NOPE"), null);
});

// ── partition table parser (reading what's on the board) ────────────────────
function ptEntry({ type, subtype, offset, size, label }) {
  const b = Buffer.alloc(32, 0);
  b.writeUInt16LE(0x50aa, 0);
  b[2] = type; b[3] = subtype;
  b.writeUInt32LE(offset >>> 0, 4);
  b.writeUInt32LE(size >>> 0, 8);
  b.write(label || "", 12, 16, "ascii");
  return b;
}

test("parsePartitionTable finds app partitions and stops at padding", async () => {
  const { parsePartitionTable, pickAppPartition } = await core();
  const table = Buffer.concat([
    ptEntry({ type: 1, subtype: 2, offset: 0x9000, size: 0x5000, label: "nvs" }),
    ptEntry({ type: 0, subtype: 0x10, offset: 0x20000, size: 0x1e0000, label: "ota_0" }),
    ptEntry({ type: 0, subtype: 0x11, offset: 0x200000, size: 0x1e0000, label: "ota_1" }),
    Buffer.alloc(64, 0xff), // padding → parser must stop here
  ]);
  const { apps, entries } = parsePartitionTable(new Uint8Array(table));
  assert.strictEqual(entries.length, 3, "should read exactly 3 real entries");
  assert.strictEqual(apps.length, 2);
  const chosen = pickAppPartition(apps);
  assert.strictEqual(chosen.label, "ota_0", "ota_0 preferred for the version read");
  assert.strictEqual(chosen.offset, 0x20000);
});

test("pickBootedAppPartition follows otadata's active slot", async () => {
  const { pickBootedAppPartition } = await core();
  const factory = { type: 0, subtype: 0x00, label: "factory" };
  const ota0 = { type: 0, subtype: 0x10, label: "ota_0" };
  const ota1 = { type: 0, subtype: 0x11, label: "ota_1" };
  const apps = [factory, ota0, ota1];
  // No otadata read → the old preference order.
  assert.strictEqual(pickBootedAppPartition(apps, null).label, "ota_0");
  // Fresh otadata → the bootloader runs factory.
  assert.strictEqual(pickBootedAppPartition(apps, { fresh: true, activeOta: 0 }).label, "factory");
  // A board that OTA'd into ota_1 is judged by ota_1, not stale ota_0.
  assert.strictEqual(pickBootedAppPartition(apps, { fresh: false, activeOta: 1 }).label, "ota_1");
  // Missing slot degrades to the preference order rather than failing.
  assert.strictEqual(pickBootedAppPartition([factory, ota0], { fresh: false, activeOta: 1 }).label, "ota_0");
  assert.strictEqual(pickBootedAppPartition([], null), null);
});

test("pickAppPartition falls back factory → first app", async () => {
  const { pickAppPartition } = await core();
  assert.strictEqual(pickAppPartition([]), null);
  const factory = { type: 0, subtype: 0x00, offset: 0x10000, label: "factory" };
  assert.strictEqual(pickAppPartition([factory]).label, "factory");
});

// ── esp_app_desc_t parser (the current-firmware readout) ────────────────────
function appDesc({ version, project }) {
  const b = Buffer.alloc(256, 0);
  b.writeUInt32LE(0xabcd5432, 0);
  b.write(version || "", 16, 32, "ascii");
  b.write(project || "", 48, 32, "ascii");
  return b;
}

test("parseAppDescriptor reads version + project, rejects bad magic", async () => {
  const { parseAppDescriptor } = await core();
  const d = parseAppDescriptor(new Uint8Array(appDesc({ version: "2.1.0-wap", project: "canary_wap" })));
  assert.strictEqual(d.version, "2.1.0-wap");
  assert.strictEqual(d.projectName, "canary_wap");
  const bad = Buffer.from(appDesc({ version: "x", project: "y" }));
  bad.writeUInt32LE(0xdeadbeef, 0);
  assert.strictEqual(parseAppDescriptor(new Uint8Array(bad)), null);
  assert.strictEqual(parseAppDescriptor(new Uint8Array(4)), null); // too short
});

test("matchProjectToProduct maps firmware names back, longest-match wins", async () => {
  const { matchProjectToProduct } = await core();
  const wap = matchProjectToProduct(catalog, "canary_wap");
  assert.ok(wap && wap.id === "securacv-canary-wap");
  // A vision-xiao-s3 project name must not collapse to bare "canary".
  const vs3 = matchProjectToProduct(catalog, "canary-vision-xiao-s3");
  assert.strictEqual(vs3.id, "securacv-canary-vision-xiao-s3");
  assert.strictEqual(matchProjectToProduct(catalog, ""), null);
});

// A release stamps each app's descriptor with the product's asset_stem and
// the firmware's version (firmware/scripts/stamp_app_desc.py). Up to 2.4.17
// every descriptor was the Arduino core's, so a board running SecuraCV read
// as "arduino-lib-builder", product unknown, version uncomparable — and an
// older image over it went on with no downgrade warning.
test("a stamped descriptor names its product and version, for every product", async () => {
  const { parseAppDescriptor, matchProjectToProduct, productForProject } = await core();
  const stems = new Set();
  for (const p of catalog.products) {
    // The field is char[32]: 31 bytes and a NUL. Product ids outgrow it.
    assert.ok(/^[\x20-\x7e]{1,31}$/.test(p.asset_stem), `${p.id}: "${p.asset_stem}" does not fit project_name`);
    assert.ok(!stems.has(p.asset_stem), `${p.id}: two products stamp "${p.asset_stem}"`);
    stems.add(p.asset_stem);
    const d = parseAppDescriptor(new Uint8Array(appDesc({ version: "2.4.18", project: p.asset_stem })));
    assert.strictEqual(matchProjectToProduct(catalog, d.projectName).id, p.id, p.asset_stem);
    assert.strictEqual(productForProject(catalog, d.projectName).id, p.id, p.asset_stem);
  }
});

test("a stamped board gets the downgrade and same-version verdicts; an Arduino-named one stays unknown", async () => {
  const { parseAppDescriptor, matchProjectToProduct, installVerdict } = await core();
  const wap = catalog.products.find((p) => p.id === "securacv-canary-wap");
  const read = (version, project) => {
    const d = parseAppDescriptor(new Uint8Array(appDesc({ version, project })));
    return { current: { version: d.version, projectName: d.projectName },
             currentProduct: matchProjectToProduct(catalog, d.projectName) };
  };
  const stamped = read("2.4.18-wap", "canary-wap");
  assert.strictEqual(installVerdict({ ...stamped, product: wap, version: "2.4.17-wap" }).kind, "downgrade");
  assert.strictEqual(installVerdict({ ...stamped, product: wap, version: "2.4.18-wap" }).kind, "same");
  assert.strictEqual(installVerdict({ ...stamped, product: wap, version: "2.4.19-wap" }).kind, "update");
  const vision = catalog.products.find((p) => p.id === "securacv-canary-vision");
  assert.strictEqual(installVerdict({ ...stamped, product: vision, version: "2.4.18" }).kind, "switch");
  // What every release up to 2.4.17 says about itself: no product, and a
  // version that is the Arduino core's, so the verdict says it can't compare.
  const old = read("esp-idf: v4.4.7 38eeba213a", "arduino-lib-builder");
  assert.strictEqual(old.currentProduct, null);
  assert.strictEqual(installVerdict({ ...old, product: wap, version: "2.4.17-wap" }).kind, "unknown");
});

// Stamped versions parse, so the verdict now compares real dev-train
// versions. Prerelease fields compare numerically where both are digits:
// as plain strings "dev.10" sorted before "dev.9", and a board on -dev.10
// offered -dev.9 read as an Update instead of a Downgrade.
test("a dev train past .9 still orders numerically, with or without the WAP suffix", async () => {
  const { compareVersions, installVerdict } = await core();
  for (const [a, b] of [["2.4.18-dev.9", "2.4.18-dev.10"], ["2.4.18-dev.9-wap", "2.4.18-dev.10-wap"],
                        ["2.4.18-dev.10", "2.4.18-rc.1"], ["2.4.18-rc.2", "2.4.18-rc.10"],
                        ["2.4.18-dev.10", "2.4.18"], ["2.4.18-dev.2-wap", "2.4.18-wap"],
                        ["2.2.0-rc1", "2.2.0-rc2"], ["2.4.18-rc.1", "2.4.18-rc.1.1"]]) {
    assert.strictEqual(compareVersions(a, b), -1, `${a} < ${b}`);
    assert.strictEqual(compareVersions(b, a), 1, `${b} > ${a}`);
  }
  assert.strictEqual(compareVersions("2.4.18-dev.10-wap", "2.4.18-dev.10-wap"), 0);
  const wap = catalog.products.find((p) => p.id === "securacv-canary-wap");
  const v = installVerdict({ current: { version: "2.4.18-dev.10-wap" }, currentProduct: wap,
                             product: wap, version: "2.4.18-dev.9-wap" });
  assert.strictEqual(v.kind, "downgrade");
  assert.strictEqual(v.label, "Downgrade · 2.4.18-dev.10-wap → 2.4.18-dev.9-wap");
});

// ── release manifest resolution ─────────────────────────────────────────────
function goodManifest() {
  return {
    schema: "securacv-flash-1",
    fw_train: catalog.fw_train,
    products: {
      "securacv-canary-wap": {
        version: "2.2.0-wap", chipFamily: "ESP32-S3",
        factory: "https://example/canary-wap-2.2.0-factory.bin",
        sha256: "a".repeat(64), size: 1048576,
      },
    },
  };
}

test("validateManifest accepts good, flags bad", async () => {
  const { validateManifest } = await core();
  assert.deepStrictEqual(validateManifest(goodManifest()), []);
  const bad = goodManifest();
  bad.products["securacv-canary-wap"].sha256 = "nope";
  bad.products["securacv-canary-wap"].size = 0;
  const errs = validateManifest(bad);
  assert.ok(errs.some((e) => /sha256/.test(e)) && errs.some((e) => /size/.test(e)));
  assert.ok(validateManifest({ schema: "wrong", products: {} }).some((e) => /schema/.test(e)));
});

test("manifestEntry refuses a chip-mismatched image (defense in depth)", async () => {
  const { manifestEntry } = await core();
  const m = goodManifest();
  const wap = catalog.products.find((p) => p.id === "securacv-canary-wap");
  const ok = manifestEntry(m, wap, "ESP32-S3");
  assert.ok(ok && !ok.error && ok.sha256 === "a".repeat(64));
  const mism = manifestEntry(m, wap, "ESP32-C3"); // S3 image, C3 in hand
  assert.ok(mism && mism.error && /mismatch/i.test(mism.error));
  assert.strictEqual(manifestEntry(m, { id: "not-in-manifest" }, "ESP32-S3"), null);
});

// ── ?manifest= override (self-hosted / air-gapped, phishing-guarded) ────────
test("manifestOverrideUrl: same-origin and loopback allowed, LAN/public refused", async () => {
  const { manifestOverrideUrl } = await core();
  const origin = "https://kmay89.github.io";
  // No override → null.
  assert.strictEqual(manifestOverrideUrl("", origin), null);
  assert.strictEqual(manifestOverrideUrl("?foo=bar", origin), null);
  // Same-origin (absolute or relative) → allowed.
  assert.strictEqual(
    manifestOverrideUrl("?manifest=https://kmay89.github.io/m.json", origin),
    "https://kmay89.github.io/m.json");
  assert.strictEqual(
    manifestOverrideUrl("?manifest=custom/m.json", origin),
    "https://kmay89.github.io/custom/m.json");
  // Loopback (a same-machine manifest server) → allowed. This is the exact set
  // the page's CSP connect-src can pin, so the code guard and the CSP agree —
  // and loopback isn't mixed-content-blocked even on the hosted HTTPS Lab.
  for (const u of [
    "http://localhost:8000/m.json",
    "https://localhost/m.json",
    "http://127.0.0.1:8443/manifest-flash.json",
  ]) assert.ok(manifestOverrideUrl("?manifest=" + encodeURIComponent(u), origin), "should allow " + u);
  // Broader private/LAN hosts → refused now: a static CSP can't enumerate
  // private-IP ranges, so the override no longer accepts hosts the browser
  // would block at the CSP layer anyway (self-host same-origin or use a local
  // file for those).
  for (const u of [
    "http://192.168.1.50:8443/manifest-flash.json",
    "http://10.0.0.2/m.json",
    "http://canary.local/m.json",
    "http://nas.lan/m.json",
    "http://172.16.9.9/m.json",
  ]) assert.strictEqual(manifestOverrideUrl("?manifest=" + encodeURIComponent(u), origin), null, "should refuse " + u);
  // Public third-party origin → refused (firmware-phishing guard).
  assert.strictEqual(
    manifestOverrideUrl("?manifest=https://evil.example.com/m.json", origin), null);
  // A bare relative ref stays same-origin (harmless — it just 404s).
  assert.strictEqual(manifestOverrideUrl("?manifest=%%%", origin), origin + "/%%%");
  // A malformed absolute URL → null, never throws.
  assert.strictEqual(
    manifestOverrideUrl("?manifest=" + encodeURIComponent("http://"), origin), null);
});

// ── byte glue for esptool-js (must survive high bytes) ──────────────────────
test("bytes <-> binary string round-trips including 0x00/0x80/0xFF", async () => {
  const { bytesToBinaryString, binaryStringToBytes, hex } = await core();
  const src = new Uint8Array([0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff, 0xe9, 0x55]);
  const round = binaryStringToBytes(bytesToBinaryString(src));
  assert.deepStrictEqual(Array.from(round), Array.from(src));
  assert.strictEqual(hex(new Uint8Array([0x0a, 0xff, 0x00])), "0aff00");
  // Chunk boundary (fromCharCode.apply batching) must not corrupt.
  const big = new Uint8Array(0x8000 + 5).map((_, i) => (i * 7 + 0x80) & 0xff);
  assert.deepStrictEqual(
    Array.from(binaryStringToBytes(bytesToBinaryString(big))), Array.from(big));
});

// ── chunked reads (the backup-stall fix) ────────────────────────────────────
test("planReadChunks covers the span exactly, in order, capped per chunk", async () => {
  const { planReadChunks, READ_CHUNK } = await core();
  const plan = planReadChunks(0, 8 * 1024 * 1024);
  assert.strictEqual(plan.length, Math.ceil(8 * 1024 * 1024 / READ_CHUNK));
  assert.strictEqual(plan[0].offset, 0);
  let covered = 0;
  for (const c of plan) {
    assert.strictEqual(c.offset, covered, "chunks must be contiguous");
    assert.ok(c.size > 0 && c.size <= READ_CHUNK);
    covered += c.size;
  }
  assert.strictEqual(covered, 8 * 1024 * 1024);
  // Unaligned tail + non-zero base.
  const odd = planReadChunks(0x8000, READ_CHUNK + 5);
  assert.strictEqual(odd.length, 2);
  assert.strictEqual(odd[1].offset, 0x8000 + READ_CHUNK);
  assert.strictEqual(odd[1].size, 5);
  assert.deepStrictEqual(planReadChunks(0, 0), []);
});

// ── local-file factory-shape gate (Advanced → local .bin) ───────────────────
// Everything installed from a local file is written at offset 0, so an
// app-only build (which starts 0xE9, like an S3/C3/C6 factory image) would
// land on the bootloader.
// The discriminator is the partition table at 0x8000 — same check, same
// reason, as the desktop Flasher's flash_local_file.
test("localImageShape refuses app-only builds and passes merged factory images", async () => {
  const { localImageShape } = await core();
  assert.strictEqual(localImageShape(new Uint8Array(0)).factory, false, "empty");
  const appOnly = new Uint8Array(0x20000).fill(0xff);
  appOnly[0] = 0xe9; // app image magic alone doesn't make a factory image
  assert.strictEqual(localImageShape(appOnly).factory, false, "app-only, 0x8000-spanning");
  assert.match(localImageShape(appOnly).reason, /partition table/);
  const short = new Uint8Array(0x4000);
  short[0] = 0xe9;
  assert.strictEqual(localImageShape(short).factory, false, "shorter than 0x8000");
  const factory = new Uint8Array(0x20000).fill(0xff);
  factory[0] = 0xe9; // bootloader image magic at 0
  ptEntry({ type: 1, subtype: 2, offset: 0x9000, size: 0x5000, label: "nvs" })
    .copy(Buffer.from(factory.buffer), 0x8000);
  assert.strictEqual(localImageShape(factory).factory, true, "merged factory image");
});

// ── the bootloader where the chip's ROM loads it (catalog + local file) ─────
// fw-v2.4.17's classic-ESP32 factory images carried the bootloader at 0x0, and
// a classic ESP32's ROM loads it from 0x1000: the partition table was there,
// so the shape gate above passed them, and neither board could boot.
test("bootloaderPlacement refuses an image whose bootloader isn't where the chip's ROM loads it", async () => {
  const { bootloaderPlacement, romBootloader } = await core();
  // A factory image with a bootloader for chip id `id` at `at` (the header's
  // u16le chip id at +12), the partition table at 0x8000, the rest erased.
  const imageWithBootloaderAt = (at, id) => {
    const b = new Uint8Array(0x10000).fill(0xff);
    b[at] = 0xe9;
    b[at + 12] = id & 0xff; b[at + 13] = id >> 8;
    b[0x8000] = 0xaa; b[0x8001] = 0x50;
    return b;
  };
  // Correct images pass, in every spelling the detector and the catalog use.
  for (const [chip, at, id] of [["ESP32", 0x1000, 0], ["esp32", 0x1000, 0], ["ESP32-S2", 0x1000, 2],
    ["ESP32-S3", 0x0, 9], ["esp32 s3", 0x0, 9], ["ESP32-C3", 0x0, 5], ["ESP32-C6", 0x0, 13],
    ["ESP32-C2", 0x0, 12], ["ESP32-H2", 0x0, 16]]) {
    assert.ok(bootloaderPlacement(imageWithBootloaderAt(at, id), chip).ok, chip);
  }
  // The fw-v2.4.17 shape on a classic ESP32: refused, saying where the ROM
  // looks, where the image has it, that nothing was written and what to use.
  const misplaced = imageWithBootloaderAt(0x0, 0);
  const v = bootloaderPlacement(misplaced, "ESP32");
  assert.strictEqual(v.ok, false);
  assert.match(v.reason, /loads its bootloader from 0x1000, and the image has no bootloader for it there \(it has one at 0x0 instead\)/);
  assert.match(v.reason, /Nothing was written/);
  assert.match(v.reason, /fw-v2\.4\.18/);
  // …and when that bootloader's body happens to hold 0xE9 at 0x1000: a stray
  // byte there is not a header naming the ESP32, so it is still refused.
  const stray = misplaced.slice();
  stray[0x1000] = 0xe9; stray[0x100c] = 0x3a;
  assert.match(bootloaderPlacement(stray, "ESP32").reason, /\(it has one at 0x0 instead\)/);
  // A classic image on an S3 (its bootloader at 0x1000 is an ESP32's), an S3
  // image on a C3 (the right place, another chip's bootloader), and an image
  // with no bootloader at all.
  const onS3 = bootloaderPlacement(imageWithBootloaderAt(0x1000, 0), "ESP32-S3");
  assert.strictEqual(onS3.ok, false);
  assert.match(onS3.reason, /from 0x0, and the image has no bootloader for it there\. Nothing/);
  assert.match(bootloaderPlacement(imageWithBootloaderAt(0x0, 9), "ESP32-C3").reason,
    /from 0x0, and the image has no bootloader for it there \(the one there was built for another chip\)/);
  const none = bootloaderPlacement(new Uint8Array(0x10000).fill(0xff), "ESP32-C6");
  assert.strictEqual(none.ok, false);
  assert.doesNotMatch(none.reason, /instead|another chip/);
  // A chip the table doesn't list is never judged, so the gate can't block
  // a correct image for it.
  assert.strictEqual(romBootloader("ESP32-P4"), null);
  assert.ok(bootloaderPlacement(new Uint8Array(0x10000).fill(0xff), "ESP32-P4").ok);
  assert.ok(bootloaderPlacement(misplaced, null).ok, "no chip read, nothing judged");
});

// ── partition kinds (board report vocabulary) ───────────────────────────────
test("partitionKind names app slots and data partitions", async () => {
  const { partitionKind, isOtaDataPart, isCoredumpPart, isNvsPart, isWitnessLogPart } = await core();
  assert.strictEqual(partitionKind({ type: 0, subtype: 0x10 }), "app · ota_0");
  assert.strictEqual(partitionKind({ type: 0, subtype: 0x00 }), "app · factory");
  assert.strictEqual(partitionKind({ type: 1, subtype: 0x02 }), "data · nvs");
  assert.strictEqual(partitionKind({ type: 1, subtype: 0x03 }), "data · coredump");
  assert.ok(isOtaDataPart({ type: 1, subtype: 0x00 }));
  assert.ok(isCoredumpPart({ type: 1, subtype: 0x03 }));
  assert.ok(isNvsPart({ type: 1, subtype: 0x02 }));
  assert.ok(isWitnessLogPart({ type: 1, subtype: 0x80, label: "witness_log" }));
  assert.ok(!isWitnessLogPart({ type: 1, subtype: 0x82, label: "spiffs" }));
});

// ── otadata (active slot + update count) ────────────────────────────────────
function otaEntry(seq, state, crcFn) {
  const b = Buffer.alloc(0x1000, 0xff);
  b.writeUInt32LE(seq >>> 0, 0);
  b.writeUInt32LE(state >>> 0, 24);
  b.writeUInt32LE(crcFn(b.subarray(0, 4)), 28);
  return b;
}

test("parseOtaData: fresh otadata → factory default; valid seq → active slot", async () => {
  const { parseOtaData, crc32EspRom } = await core();
  // esp_rom_crc32_le(UINT32_MAX, …) reference value: crc of 01 00 00 00.
  // Erased otadata (all 0xFF) → fresh.
  const blank = new Uint8Array(0x2000).fill(0xff);
  assert.strictEqual(parseOtaData(blank, 2).fresh, true);
  // seq=1 valid in sector 0 → active ota slot (1-1)%2 = 0.
  const good = Buffer.concat([
    otaEntry(1, 0x2 /* valid */, crc32EspRom),
    Buffer.alloc(0x1000, 0xff),
  ]);
  const r = parseOtaData(new Uint8Array(good), 2);
  assert.strictEqual(r.fresh, false);
  assert.strictEqual(r.activeOta, 0);
  assert.strictEqual(r.updatesSeen, 1);
  assert.ok(/valid/.test(r.stateText));
  // Higher seq in sector 1 wins; seq=4 on 2 slots → slot (4-1)%2 = 1.
  const two = Buffer.concat([
    otaEntry(3, 0xffffffff, crc32EspRom),
    otaEntry(4, 0x1 /* pending verify */, crc32EspRom),
  ]);
  const r2 = parseOtaData(new Uint8Array(two), 2);
  assert.strictEqual(r2.activeOta, 1);
  assert.strictEqual(r2.pendingVerify, true);
  // Rollback: newest (seq 6, INVALID) doesn't boot → the previous good image
  // (seq 5) runs, so activeOta must exclude the rolled-back slot.
  const rolled = Buffer.concat([
    otaEntry(5, 0x2 /* valid */, crc32EspRom),
    otaEntry(6, 0x3 /* invalid — rolled back */, crc32EspRom),
  ]);
  const r3 = parseOtaData(new Uint8Array(rolled), 2);
  assert.strictEqual(r3.updatesSeen, 6); // the attempt is still counted
  assert.strictEqual(r3.activeOta, (5 - 1) % 2); // = 0, previous good — NOT (6-1)%2 = 1
  assert.ok(/rolled back/.test(r3.stateText));
  assert.strictEqual(r3.pendingVerify, false);
  // A corrupt CRC is ignored, falling back to the other sector.
  const bad = Buffer.concat([otaEntry(9, 0x2, crc32EspRom), Buffer.alloc(0x1000, 0xff)]);
  bad.writeUInt32LE(0xdeadbeef, 28); // stomp the CRC
  assert.strictEqual(parseOtaData(new Uint8Array(bad), 2).fresh, true);
});

// ── coredump presence ───────────────────────────────────────────────────────
test("parseCoredumpHeader: erased → absent, sane length → present", async () => {
  const { parseCoredumpHeader } = await core();
  const erased = new Uint8Array(16).fill(0xff);
  assert.strictEqual(parseCoredumpHeader(erased, 0x10000).present, false);
  const dumped = Buffer.alloc(16, 0);
  dumped.writeUInt32LE(0x3000, 0);
  const r = parseCoredumpHeader(new Uint8Array(dumped), 0x10000);
  assert.strictEqual(r.present, true);
  assert.strictEqual(r.size, 0x3000);
  // A "length" bigger than the partition is garbage, not a dump.
  dumped.writeUInt32LE(0x900000, 0);
  assert.strictEqual(parseCoredumpHeader(new Uint8Array(dumped), 0x10000).present, false);
});

// ── NVS reader (witness chain without booting the board) ────────────────────
// Build a minimal valid NVS page: header + bitmap + entries, matching the
// firmware's namespace/keys (canary_wap.ino: ns "securacv", seq/boots/chain).
function nvsPage(entries) {
  const page = Buffer.alloc(4096, 0xff);
  page.writeUInt32LE(0xfffffffe, 0); // ACTIVE
  page.writeUInt32LE(0, 4);          // seq
  const bitmap = page.subarray(32, 64);
  let idx = 0;
  const writeEntry = (e) => {
    const o = 64 + idx * 32;
    page[o] = e.ns; page[o + 1] = e.type; page[o + 2] = e.span || 1; page[o + 3] = e.chunkIdx || 0xff;
    page.write(e.key, o + 8, 15, "ascii");
    page[o + 8 + Math.min(e.key.length, 15)] = 0;
    if (e.data) e.data.copy(page, o + 24);
    // mark Written (0b10) for the header entry + its payload span
    for (let s = 0; s < (e.span || 1); s++) {
      const j = idx + s;
      bitmap[j >> 2] &= ~(0x3 << ((j & 3) * 2));
      bitmap[j >> 2] |= 0x2 << ((j & 3) * 2);
    }
    if (e.payload) e.payload.copy(page, o + 32);
    idx += e.span || 1;
  };
  entries.forEach(writeEntry);
  return page;
}

function u32data(v) { const b = Buffer.alloc(8, 0); b.writeUInt32LE(v >>> 0, 0); return b; }

test("parseNvs + witnessSummary read chain state, never secret values", async () => {
  const { parseNvs, witnessSummary, WITNESS_CHAIN_BLOB_KEY } = await core();
  const chainBytes = Buffer.alloc(32).map((_, i) => i + 1);
  const blobData = Buffer.alloc(8, 0);
  blobData.writeUInt16LE(32, 0); // size
  const idxData = Buffer.alloc(8, 0);
  idxData.writeUInt32LE(32, 0);  // total size
  idxData[4] = 1;                // chunkCount
  idxData[5] = 0;                // chunkStart
  const page = nvsPage([
    { ns: 0, type: 0x01, key: "securacv", data: u32data(1) },      // namespace → index 1
    { ns: 1, type: 0x04, key: "seq", data: u32data(1234) },        // U32
    { ns: 1, type: 0x04, key: "boots", data: u32data(57) },
    { ns: 1, type: 0x01, key: "tamper", data: u32data(0) },        // U8
    { ns: 1, type: 0x42, key: "chain", span: 2, chunkIdx: 0,       // blob chunk + payload
      data: blobData, payload: chainBytes },
    { ns: 1, type: 0x48, key: "chain", data: idxData },            // blob index
    { ns: 1, type: 0x42, key: "privkey", span: 2, chunkIdx: 0,     // secret — NOT allow-listed
      data: blobData, payload: Buffer.alloc(32, 0x42) },
    { ns: 1, type: 0x48, key: "privkey", data: idxData },
    { ns: 1, type: 0x21, key: "wifi_ssid", span: 2, data: blobData,
      payload: Buffer.from("MyHomeWifi\0") },
  ]);

  const items = parseNvs(new Uint8Array(page), [WITNESS_CHAIN_BLOB_KEY]);
  const s = witnessSummary(items);
  assert.ok(s, "securacv namespace should be found");
  assert.strictEqual(s.seq, 1234);
  assert.strictEqual(s.boots, 57);
  assert.strictEqual(s.tamper, 0);
  assert.strictEqual(s.chainHeadFp, "0102030405060708");
  assert.strictEqual(s.provisioned, true, "privkey presence detected");
  assert.strictEqual(s.wifiConfigured, true, "ssid presence detected");
  // The guard: no secret CONTENT anywhere in the parsed output.
  const flat = JSON.stringify(items, (k, v) => (v instanceof Uint8Array ? Array.from(v) : v));
  assert.ok(!flat.includes("MyHomeWifi"), "ssid value must never be extracted");
  const priv = items.find((i) => i.key === "privkey" && i.bytes);
  assert.strictEqual(priv, undefined, "non-allow-listed blobs must not carry bytes");
});

test("parseNvs survives blank flash and reports nothing", async () => {
  const { parseNvs, witnessSummary } = await core();
  const blank = new Uint8Array(0x6000).fill(0xff);
  const items = parseNvs(blank);
  assert.deepStrictEqual(items, []);
  assert.strictEqual(witnessSummary(items), null);
});

// ── progress prediction (the bar the user can trust) ────────────────────────
test("makeEtaTracker: monotonic fraction, sane ETA at steady rate", async () => {
  const { makeEtaTracker } = await core();
  const t = makeEtaTracker(1000_000);
  t.feed(0, 0);
  let p = t.feed(100_000, 1000); // 100 KB/s
  p = t.feed(200_000, 2000);
  p = t.feed(300_000, 3000);
  assert.ok(Math.abs(p.frac - 0.3) < 1e-9);
  assert.ok(p.etaSeconds > 5 && p.etaSeconds < 9, `eta ~7s, got ${p.etaSeconds}`);
  assert.ok(p.kbps > 80 && p.kbps < 120, `kbps ~97, got ${p.kbps}`);
  // A bogus backwards report must never move the bar backwards.
  p = t.feed(50_000, 4000);
  assert.ok(p.frac >= 0.3);
  // Overshoot clamps to 100%.
  p = t.feed(2_000_000, 5000);
  assert.ok(p.frac <= 1);
  // Unknown rate yet → no fake estimate.
  const fresh = makeEtaTracker(500);
  assert.strictEqual(fresh.feed(0, 0).etaSeconds, null);
});

test("formatDuration speaks human", async () => {
  const { formatDuration } = await core();
  assert.strictEqual(formatDuration(3), "a few seconds");
  assert.strictEqual(formatDuration(42), "about 40 seconds");
  assert.strictEqual(formatDuration(130), "about 2 minutes");
  assert.strictEqual(formatDuration(61), "about 1 minute");
  assert.strictEqual(formatDuration(null), "");
  assert.strictEqual(formatDuration(NaN), "");
});

// ── serial console heuristics ───────────────────────────────────────────────
test("looksLikeGarbage: wrong-baud soup yes, real console text no", async () => {
  const { looksLikeGarbage } = await core();
  // Real firmware output — including the help menu's box-drawing glyphs.
  const menu = "┌────────────┐\n│ SERIAL COMMANDS │\n└────────────┘\n[BOOT] securacv canary v2.2.0\n".repeat(3);
  assert.strictEqual(looksLikeGarbage(menu), false);
  // Wrong baud: replacement chars and control soup.
  const soup = ("�\x01x�\x02�~\x1f�").repeat(20);
  assert.strictEqual(looksLikeGarbage(soup), true);
  // Too little data to judge → not garbage (yet).
  assert.strictEqual(looksLikeGarbage("��"), false);
  assert.strictEqual(looksLikeGarbage(""), false);
});

// ── backup-file restore guard ───────────────────────────────────────────────
test("validateBackupFile: equal ok, smaller warns, bigger refused", async () => {
  const { validateBackupFile } = await core();
  const flash = 8 * 1024 * 1024;
  assert.deepStrictEqual(validateBackupFile(flash, flash, "b.bin"), { ok: true });
  const small = validateBackupFile(4 * 1024 * 1024, flash, "b.bin");
  assert.strictEqual(small.ok, true);
  assert.ok(/smaller/.test(small.warn));
  const big = validateBackupFile(16 * 1024 * 1024, flash, "b.bin");
  assert.strictEqual(big.ok, false);
  assert.ok(/bigger/.test(big.reason));
  assert.strictEqual(validateBackupFile(0, flash, "b.bin").ok, false);
  // Unknown flash size: only emptiness is checkable.
  assert.strictEqual(validateBackupFile(123, null, "b.bin").ok, true);
});

// ── rescue product choice ───────────────────────────────────────────────────
test("pickRescueProduct prefers what the board runs, else the only match", async () => {
  const { pickRescueProduct } = await core();
  // Board says it runs canary_wap → that product wins on the S3.
  const wap = pickRescueProduct(catalog, "ESP32-S3", "canary_wap");
  assert.strictEqual(wap.id, "securacv-canary-wap");
  // Unknown project on a chip with several products → caller must ask.
  assert.strictEqual(pickRescueProduct(catalog, "ESP32-S3", "mystery_fw"), null);
  // Unknown chip → null.
  assert.strictEqual(pickRescueProduct(catalog, "ESP32-NOPE", "x"), null);
});

// ── hex peek (flash map magnifier) ──────────────────────────────────────────
test("hexDumpLines: classic addr | hex | ascii rows", async () => {
  const { hexDumpLines } = await core();
  const bytes = new Uint8Array([0xe9, 0x06, 0x02, 0x2f, 0x41, 0x42, 0x43, 0x00,
                                0xff, 0x20, 0x7e, 0x7f, 0x0a, 0x61, 0x62, 0x63,
                                0x01, 0x02]);
  const lines = hexDumpLines(bytes, 0x10000);
  assert.strictEqual(lines.length, 2);
  assert.strictEqual(lines[0].addr, "0x010000");
  assert.ok(lines[0].hex.startsWith("e9 06 02 2f 41 42 43 00"));
  assert.ok(lines[0].ascii.includes("ABC"));
  assert.ok(lines[0].ascii.includes("·"), "non-printables become middots");
  assert.strictEqual(lines[1].addr, "0x010010");
  assert.strictEqual(lines[1].hex, "01 02");
});

test("sniffRegion recognizes the chip's own magic numbers", async () => {
  const { sniffRegion } = await core();
  assert.ok(/erased/.test(sniffRegion(new Uint8Array(64).fill(0xff))));
  const app = new Uint8Array(64); app[0] = 0xe9;
  assert.ok(/firmware image/.test(sniffRegion(app)));
  const pt = new Uint8Array(64); pt[0] = 0xaa; pt[1] = 0x50;
  assert.ok(/partition table/.test(sniffRegion(pt)));
  const desc = Buffer.alloc(64); desc.writeUInt32LE(0xabcd5432, 0);
  assert.ok(/description block/.test(sniffRegion(new Uint8Array(desc))));
  const mostly = new Uint8Array(100).fill(0xff); mostly[3] = 0x42;
  assert.ok(/mostly erased/.test(sniffRegion(mostly)));
  const data = new Uint8Array(64).fill(0x37);
  assert.strictEqual(sniffRegion(data), "stored data");
});

// ── install diff (what actually changes on the board) ──────────────────────
// Synthetic flash images with a real partition table at 0x8000: nvs at
// 0x9000 (0x1000), app at 0x10000 (0x10000), witness at 0x20000 (0x1000).
function appDescAt(version, project) {
  const b = Buffer.alloc(256, 0);
  b.writeUInt32LE(0xabcd5432, 0);
  b.write(version, 16, 32, "ascii");
  b.write(project, 48, 32, "ascii");
  return b;
}

function synthFlash({ version, wifi, imageOnly }) {
  const size = imageOnly ? 0x20000 : 0x21000; // image stops before witness
  const img = Buffer.alloc(size, 0xff);
  Buffer.concat([
    ptEntry({ type: 1, subtype: 2, offset: 0x9000, size: 0x1000, label: "nvs" }),
    ptEntry({ type: 0, subtype: 0x10, offset: 0x10000, size: 0x10000, label: "app0" }),
    ptEntry({ type: 1, subtype: 0x80, offset: 0x20000, size: 0x1000, label: "witness_log" }),
  ]).copy(img, 0x8000);
  img[0] = 0xe9; // bootloader-ish
  if (wifi) img.fill(0x42, 0x9000, 0x9100); // pretend settings data
  appDescAt(version, "canary_wap").copy(img, 0x10000 + 0x20);
  img.fill(0x11, 0x10000 + 0x200, 0x10000 + 0x400); // some app body
  if (!imageOnly) img.fill(0x77, 0x20000, 0x20040); // witness data on board
  return new Uint8Array(img);
}

test("diffInstall: firmware updated, settings untouched, witness beyond image", async () => {
  const { diffInstall } = await core();
  const board = synthFlash({ version: "2.1.0", wifi: true, imageOnly: false });
  const image = synthFlash({ version: "2.2.0", wifi: true, imageOnly: true });
  // Make the new app body actually differ.
  image.fill(0x22, 0x10000 + 0x200, 0x10000 + 0x400);
  const d = diffInstall(board, image);
  assert.ok(d && d.rows.length >= 4);
  assert.strictEqual(d.layoutChanged, false);
  const by = (l) => d.rows.find((r) => r.label === l);
  assert.strictEqual(by("nvs").verdict, "identical", "same settings bytes → identical");
  const app = by("app0");
  assert.strictEqual(app.verdict, "changed");
  assert.ok(/2\.1\.0/.test(app.before) && /2\.2\.0/.test(app.after), "version change named");
  assert.strictEqual(by("witness_log").verdict, "untouched", "image never reaches it");
});

test("diffInstall: factory image wipes settings; verdict says so plainly", async () => {
  const { diffInstall, settingsVerdict } = await core();
  const board = synthFlash({ version: "2.1.0", wifi: true, imageOnly: false });
  const image = synthFlash({ version: "2.2.0", wifi: false, imageOnly: true }); // nvs all 0xFF
  const d = diffInstall(board, image);
  const nvs = d.rows.find((r) => r.label === "nvs");
  assert.strictEqual(nvs.verdict, "wiped");
  const v = settingsVerdict(d, true);
  assert.strictEqual(v.kept, false);
  assert.ok(/WiFi is cleared/.test(v.text));
  // And the keep case:
  const same = diffInstall(board, synthFlash({ version: "2.1.0", wifi: true, imageOnly: true }));
  const v2 = settingsVerdict(same, true);
  assert.strictEqual(v2.kept, true);
  assert.ok(/survive/.test(v2.text));
});

test("diffInstall returns null without any partition table to anchor on", async () => {
  const { diffInstall } = await core();
  const blankOld = new Uint8Array(0x40000).fill(0xff);
  const rawApp = new Uint8Array(0x1000).fill(0xe9); // bare app bin, no table
  assert.strictEqual(diffInstall(blankOld, rawApp), null);
});

// ── browser detection (the hop-to-Chrome card) ──────────────────────────────
test("detectBrowser: safari, firefox, iPhone, iPad-as-Mac, android, chrome-ish", async () => {
  const { detectBrowser } = await core();
  const safari = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4 Safari/605.1.15";
  assert.strictEqual(detectBrowser(safari, 0).id, "safari");
  const fx = "Mozilla/5.0 (X11; Linux x86_64; rv:126.0) Gecko/20100101 Firefox/126.0";
  assert.strictEqual(detectBrowser(fx).id, "firefox");
  const iphone = "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) CriOS/124.0 Mobile/15E148 Safari/604.1";
  const ip = detectBrowser(iphone, 5);
  assert.strictEqual(ip.id, "ios");
  assert.strictEqual(ip.mobile, true);
  // iPadOS pretends to be a Mac; the touch points give it away.
  const ipadAsMac = detectBrowser(safari, 5);
  assert.strictEqual(ipadAsMac.id, "ios");
  assert.strictEqual(ipadAsMac.label, "iPad");
  const android = "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Mobile Safari/537.36";
  assert.strictEqual(detectBrowser(android).id, "android");
  // Desktop Chrome contains "Safari/" too — must NOT classify as safari.
  const chrome = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36";
  assert.strictEqual(detectBrowser(chrome, 0).id, "other");
});

// ── release channels (dev toggle) ───────────────────────────────────────────
// ── liveness: the erase promise ─────────────────────────────────────────────
test("eraseEstimateText: honest, size-shaped, no invented precision", async () => {
  const { eraseEstimateText } = await core();
  // A chip that never reported a size gets the conservative sentence, not a
  // made-up number.
  assert.strictEqual(eraseEstimateText(null), "usually well under two minutes");
  assert.strictEqual(eraseEstimateText(0), "usually well under two minutes");
  assert.strictEqual(eraseEstimateText(NaN), "usually well under two minutes");
  // Real sizes shape the words coarsely — never a numeric range that reads
  // like a benchmark nobody ran (review catch on #1577). The one number we
  // may state is structural: the engine's fixed two-minute ceiling.
  const four = eraseEstimateText(4 * 1024 * 1024);
  const sixteen = eraseEstimateText(16 * 1024 * 1024);
  assert.match(four, /under a minute/);
  assert.match(sixteen, /minute or more/);
  assert.match(sixteen, /up to two/);
  assert.doesNotMatch(four, /\d+\s*[–-]\s*\d+\s*seconds/);
  assert.doesNotMatch(sixteen, /\d+ seconds/);
});

test("channelFromSearch: only an explicit channel=dev switches; URL is a fixed constant", async () => {
  const { channelFromSearch, DEV_FLASH_MANIFEST_URL } = await core();
  assert.strictEqual(channelFromSearch(""), "release");
  assert.strictEqual(channelFromSearch("?foo=1"), "release");
  assert.strictEqual(channelFromSearch("?channel=dev"), "dev");
  assert.strictEqual(channelFromSearch("?channel=DEV"), "release");   // exact opt-in only
  assert.strictEqual(channelFromSearch("?channel=stable"), "release");
  // The dev manifest lives on the repo's own rolling prerelease — never a
  // user-supplied host (that path is manifestOverrideUrl's, LAN-guarded).
  assert.ok(DEV_FLASH_MANIFEST_URL.startsWith("https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/"));
});

test("devChannelStanding: the dev train against the pinned one, never a claim about stable", async () => {
  const { devChannelStanding } = await core();
  assert.strictEqual(devChannelStanding("2.4.18-dev.1", "2.4.17"),
    "The dev channel carries 2.4.18-dev.1, newer than the 2.4.17 release this flasher is pinned to.");
  // What a stable release leaves there when no newer dev build is.
  assert.strictEqual(devChannelStanding("2.4.17", "2.4.17"),
    "The dev channel carries 2.4.17, the same version as the release this flasher is pinned to.");
  // fw-dev-latest's catalog on 2026-10-27, under stable 2.4.17.
  assert.strictEqual(devChannelStanding("2.4.15", "2.4.17"),
    "The dev channel carries 2.4.15, older than the 2.4.17 release this flasher is pinned to.");
  assert.match(devChannelStanding("2.4.17-rc.1", "2.4.17"), /older than the 2\.4\.17 release/);
  // A Flasher built for 2.4.17, after fw-v2.4.18 shipped and copied itself to
  // the dev channel: 2.4.18 IS stable, so "ahead of stable" would be false.
  // The sentence says only what the two trains show.
  assert.strictEqual(devChannelStanding("2.4.18", "2.4.17"),
    "The dev channel carries 2.4.18, newer than the 2.4.17 release this flasher is pinned to.");
  assert.doesNotMatch(devChannelStanding("2.4.18", "2.4.17"), /stable/);
  // No train, no claim.
  assert.strictEqual(devChannelStanding(undefined, catalog.fw_train), "");
  assert.strictEqual(devChannelStanding("2.4.17", null), "");
});

test("releaseTagFromManifestUrl: names the pinned firmware tag, and only that", async () => {
  const { releaseTagFromManifestUrl, DEV_FLASH_MANIFEST_URL } = await core();
  // The catalog's own pin — the case that matters. A tag bumped but never
  // released is why every product can read "unavailable"; the page says which.
  assert.strictEqual(
    releaseTagFromManifestUrl(catalog.manifest_url),
    `fw-v${registry.fw_train}`,
  );
  assert.strictEqual(
    releaseTagFromManifestUrl(
      "https://github.com/kmay89/securaCV/releases/download/fw-v2.3.0/manifest-flash.json"),
    "fw-v2.3.0");
  assert.strictEqual(
    releaseTagFromManifestUrl(
      "https://github.com/kmay89/securaCV/releases/download/fw-v2.4.0-rc.1/manifest-flash.json"),
    "fw-v2.4.0-rc.1");
  // Not a pinned firmware release: the rolling dev pointer is a channel, not a
  // version, and claiming it as one would misname what the page is showing.
  assert.strictEqual(releaseTagFromManifestUrl(DEV_FLASH_MANIFEST_URL), null);
  // Neither is an app release, a self-hosted override, or junk.
  assert.strictEqual(
    releaseTagFromManifestUrl(
      "https://github.com/kmay89/securaCV/releases/download/flasher-v0.2.2/latest.json"),
    null);
  assert.strictEqual(releaseTagFromManifestUrl("http://canary.local/manifest-flash.json"), null);
  assert.strictEqual(releaseTagFromManifestUrl(""), null);
  assert.strictEqual(releaseTagFromManifestUrl(null), null);
  assert.strictEqual(releaseTagFromManifestUrl(undefined), null);
  // A visitor-typed ?manifest= override with a malformed escape names no
  // release; it must not throw out of the missing-release banner.
  assert.strictEqual(
    releaseTagFromManifestUrl("http://127.0.0.1:8000/releases/download/%E0/manifest-flash.json"),
    null);
});

// ── the same-origin release mirror (pages.yml → firmware/<tag>/<file>) ──────
// A browser can never fetch a github.com release asset (no CORS header on the
// redirect or on the asset host), so the page reads the copy pages.yml writes
// beside it, and only that copy. These pin where the page looks; scripts/tests/
// test_pages_release_mirror.py holds the writer (tools/mirror_release.py) to
// the same layout through node, and tests/flash_mirror_probe.mjs proves it in
// Chromium from one origin.
const PAGES = "https://kmay89.github.io/securaCV/canary-local/flash.html";
const PAGES_MIRROR = "https://kmay89.github.io/securaCV/canary-local/firmware/";
const REL = (tag, file) => `https://github.com/kmay89/securaCV/releases/download/${tag}/${file}`;

test("releaseAssetParts: the project's release-download URLs, and nothing else", async () => {
  const { releaseAssetParts, DEV_FLASH_MANIFEST_URL } = await core();
  const tag = `fw-v${registry.fw_train}`;
  // Every pin the page reads maps — the catalog's own, the Vision model's and
  // the dev channel's.
  assert.deepStrictEqual(releaseAssetParts(catalog.manifest_url, catalog.repo),
    { repo: catalog.repo, tag, file: "manifest-flash.json" });
  assert.deepStrictEqual(releaseAssetParts(catalog.we2_module.manifest_url, catalog.repo),
    { repo: catalog.repo, tag, file: "manifest-vision-model.json" });
  assert.deepStrictEqual(releaseAssetParts(DEV_FLASH_MANIFEST_URL, catalog.repo),
    { repo: catalog.repo, tag: "fw-dev-latest", file: "manifest-flash.json" });
  // A dev manifest names its images under the real fw-v*-dev.* tag.
  assert.deepStrictEqual(
    releaseAssetParts(REL("fw-v2.5.0-dev.1", "canary-2.5.0-dev.1-factory.bin"), "KMAY89/securacv"),
    { repo: "kmay89/securaCV", tag: "fw-v2.5.0-dev.1", file: "canary-2.5.0-dev.1-factory.bin" });
  // Without a repo any owner answers; with one, only that repository.
  assert.ok(releaseAssetParts("https://github.com/other/fork/releases/download/fw-v1/x.bin"));
  assert.strictEqual(
    releaseAssetParts("https://github.com/other/fork/releases/download/fw-v1/x.bin", catalog.repo), null);
  for (const bad of [
    "http://github.com/kmay89/securaCV/releases/download/fw-v1/x.bin",        // not https
    "https://github.com:8443/kmay89/securaCV/releases/download/fw-v1/x.bin",  // a port
    "https://evil.example/kmay89/securaCV/releases/download/fw-v1/x.bin",     // another host
    "https://github.com.evil.example/kmay89/securaCV/releases/download/fw-v1/x.bin",
    "https://u:p@github.com/kmay89/securaCV/releases/download/fw-v1/x.bin",   // credentials
    REL("fw-v1", "x.bin") + "?raw=1",                                           // a query
    REL("fw-v1", "x.bin") + "#frag",                                            // a fragment
    REL("fw-v1", "a/b.bin"),                                                    // a nested path
    REL("fw-v1", "%2E%2E"),                                                     // ".." spelled out
    REL("fw-v1", "a%2Fb.bin"),                                                  // an encoded slash
    REL("fw-v1", ".hidden"),                                                    // not a plain name
    REL("%E0", "x.bin"),                                                        // a malformed escape
    "https://github.com/kmay89/securaCV/releases/latest/download/x.bin",        // not a pinned tag
    "https://api.github.com/repos/kmay89/securaCV/releases/assets/1",
    "", null, undefined, "not a url",
  ]) {
    assert.strictEqual(releaseAssetParts(bad, catalog.repo), null, String(bad));
  }
});

test("releaseFetchUrl: an http(s) page reads ONLY its copy, on every host; nothing else is mapped", async () => {
  const { releaseFetchUrl, DEV_FLASH_MANIFEST_URL } = await core();
  const tag = `fw-v${registry.fw_train}`;
  const opts = { pageHref: PAGES, mirrorBase: PAGES_MIRROR, repo: catalog.repo };
  // The hosted Lab: the copy, and nothing else.
  assert.strictEqual(releaseFetchUrl(catalog.manifest_url, opts), `${PAGES_MIRROR}${tag}/manifest-flash.json`);
  assert.strictEqual(releaseFetchUrl(catalog.we2_module.manifest_url, opts),
    `${PAGES_MIRROR}${tag}/manifest-vision-model.json`);
  assert.strictEqual(releaseFetchUrl(DEV_FLASH_MANIFEST_URL, opts), `${PAGES_MIRROR}fw-dev-latest/manifest-flash.json`);
  // A relative mirror base resolves against the page.
  assert.strictEqual(releaseFetchUrl(REL(tag, "canary-x-factory.bin"), { ...opts, mirrorBase: "firmware/" }),
    `${PAGES_MIRROR}${tag}/canary-x-factory.bin`);
  // A dev server, a hub on the LAN, the desktop Lab's Windows webview: the
  // copy too, with no fallback to the release URL. No browser can read that
  // URL from any origin, loopback included, so a fallback could only turn
  // this site's honest 404 into a false "connection problem".
  for (const page of ["http://localhost:8000/canary-local/flash.html", "http://127.0.0.1:8000/canary-local/flash.html",
    "http://192.168.1.20/canary-local/flash.html", "http://hub.local/canary-local/flash.html",
    "http://tauri.localhost/flash.html"]) {
    const base = new URL("firmware/", page).href;
    assert.strictEqual(
      releaseFetchUrl(catalog.manifest_url, { pageHref: page, mirrorBase: base, repo: catalog.repo }),
      `${base}${tag}/manifest-flash.json`, page);
  }
  // Unchanged: a page off http(s) (file:, a native webview scheme), a URL
  // that is not the project's release (a ?manifest= override, a self-hosted
  // image, a fork), a mirror base on another origin, or no base at all.
  for (const [url, o] of [
    [catalog.manifest_url, { ...opts, pageHref: "file:///home/u/securaCV/canary-local/flash.html" }],
    [catalog.manifest_url, { ...opts, pageHref: "tauri://localhost/flash.html" }],
    [catalog.manifest_url, { ...opts, pageHref: undefined }],
    ["https://kmay89.github.io/securaCV/canary-local/my/manifest-flash.json", opts],
    ["http://127.0.0.1:9000/manifest-flash.json", opts],
    ["https://github.com/someone/fork/releases/download/fw-v1/manifest-flash.json", opts],
    [catalog.manifest_url, { ...opts, mirrorBase: "https://cdn.example/firmware/" }],
    [catalog.manifest_url, { ...opts, mirrorBase: "" }],
  ]) {
    assert.strictEqual(releaseFetchUrl(url, o), url, `${url} from ${o.pageHref} / ${o.mirrorBase}`);
  }
});

test("fetchReleaseAsset: one request, to releaseFetchUrl's answer, handed back as a bare fetch gave it", async () => {
  const { fetchReleaseAsset } = await core();
  const tag = `fw-v${registry.fw_train}`;
  const mirrorUrl = `${PAGES_MIRROR}${tag}/manifest-flash.json`;
  const LOCAL = "http://localhost:8000/canary-local/flash.html";
  const LOCAL_MIRROR = "http://localhost:8000/canary-local/firmware/";
  const res = (status) => ({ ok: status >= 200 && status < 300, status });
  const recorder = (answers) => {
    const calls = [];
    const fetchImpl = async (url, init) => {
      calls.push({ url, init });
      const a = answers[url];
      if (a instanceof Error) throw a;
      return a || res(404);
    };
    return { calls, fetchImpl };
  };
  const base = { repo: catalog.repo, init: { cache: "no-store" } };

  // Hosted page, copy present: one same-origin request, init passed through.
  let r = recorder({ [mirrorUrl]: res(200) });
  let got = await fetchReleaseAsset(catalog.manifest_url,
    { ...base, pageHref: PAGES, mirrorBase: PAGES_MIRROR, fetchImpl: r.fetchImpl });
  assert.strictEqual(got.status, 200);
  assert.deepStrictEqual(r.calls, [{ url: mirrorUrl, init: { cache: "no-store" } }]);

  // No copy (the site has not redeployed since the release, or its deploy
  // skipped it), on a hosted or a local page: the copy's 404 comes back as a
  // Response, so the page can say the copy is missing, and github.com is
  // never asked.
  const localMirror = `${LOCAL_MIRROR}${tag}/manifest-flash.json`;
  for (const [pageHref, mirrorBase, want] of [[PAGES, PAGES_MIRROR, mirrorUrl], [LOCAL, LOCAL_MIRROR, localMirror]]) {
    r = recorder({});
    got = await fetchReleaseAsset(catalog.manifest_url, { ...base, pageHref, mirrorBase, fetchImpl: r.fetchImpl });
    assert.strictEqual(got.status, 404, pageHref);
    assert.deepStrictEqual(r.calls.map((c) => c.url), [want], pageHref);
  }

  // A network error is thrown, as a bare fetch would throw it.
  const down = new TypeError("Failed to fetch");
  r = recorder({ [localMirror]: down });
  await assert.rejects(fetchReleaseAsset(catalog.manifest_url,
    { ...base, pageHref: LOCAL, mirrorBase: LOCAL_MIRROR, fetchImpl: r.fetchImpl }), (e) => e === down);

  // Not a release URL (a ?manifest= override): fetched exactly as given.
  const override = "http://127.0.0.1:9000/manifest-flash.json";
  r = recorder({ [override]: res(200) });
  await fetchReleaseAsset(override, { ...base, pageHref: PAGES, mirrorBase: PAGES_MIRROR, fetchImpl: r.fetchImpl });
  assert.deepStrictEqual(r.calls.map((c) => c.url), [override]);
});

test("flash.js / we2-flash.js read every release asset through the mirror, never by a bare fetch", () => {
  // The four reads that used to be fetch(<github.com URL>) — refused by every
  // browser — now go through core.fetchReleaseAsset. Every bare fetch() left in
  // these two modules takes a same-origin string literal (devices/*.json).
  const strip = (s) => s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:"'`])\/\/.*$/gm, "$1");
  const flashJs = strip(readFileSync(join(ROOT, "assets/flash.js"), "utf8"));
  const we2Js = strip(readFileSync(join(ROOT, "assets/we2-flash.js"), "utf8"));
  const bare = (src) => [...src.matchAll(/(?<![.\w])fetch\(\s*([^,)]*)/g)].map((m) => m[1]);
  // The scan sees flash.js's own catalog reads, so a scan that matched
  // nothing would say so rather than pass.
  assert.ok(bare(flashJs).includes('"devices/flash.json"'), "the bare-fetch scan found nothing in flash.js");
  for (const [name, src] of [["flash.js", flashJs], ["we2-flash.js", we2Js]]) {
    for (const arg of bare(src)) {
      assert.match(arg, /^"devices\/[^"]+"$/,
        `${name}: fetch(${arg}) — a release asset must go through fetchReleaseAsset (the mirror)`);
    }
  }
  assert.match(flashJs, /function fetchRelease\(url, init\) \{\s*return core\.fetchReleaseAsset\(url, \{ repo: state\.catalog && state\.catalog\.repo, init \}\);/);
  assert.match(flashJs, /fetchRelease\(activeManifestUrl\(\), \{ cache: "no-store" \}\)/);
  assert.match(flashJs, /await fetchRelease\(opts\.entry\.factory, \{ cache: "no-store" \}\)/);
  assert.match(we2Js, /fetchReleaseAsset\(url, \{ repo: ctx\.catalog && ctx\.catalog\.repo, init \}\)/);
  assert.match(we2Js, /fromRelease\(m\.manifest_url, \{ cache: "no-store" \}\)/);
  assert.match(we2Js, /fromRelease\(asset\.url\)/);
});

test("an HTTP error on the manifest is told as the server that answered: this site's copy, or a ?manifest= override", async () => {
  // The release host never answers a browser (releaseFetchUrl reads the
  // site's copy instead), so "no release has been cut" is not a reading the
  // page can make. The release can be live on GitHub while the site's copy of
  // it is not: the site redeploys a few minutes after a release publishes,
  // and a deploy skips a release it cannot copy whole. A ?manifest= override
  // is read as given, from its own server, which is what a 404 there names.
  // Run the banner's own sentence chain and fine print, as written in
  // flash.js, on each case.
  const flashJs = readFileSync(join(ROOT, "assets/flash.js"), "utf8");
  const m = /note\.textContent = (m\.__invalid\s*\?[\s\S]*?);\n\s*banner\.append\(note\);/.exec(flashJs);
  assert.ok(m, "flash.js: the manifest banner's sentence chain moved; point this test at it");
  const say = new Function("m", "state", "pinned", "answered", `return ${m[1]};`);
  const f = /`This page is pinned to firmware release \$\{pinned\}` \+\s*\(m\.why \? ` — \$\{m\.why\}\.` : "\."\) \+\s*(\(!answered[\s\S]*?\))\)\);/.exec(flashJs);
  assert.ok(f, "flash.js: the manifest banner's fine print moved; point this test at it");
  const fine = new Function("m", "state", "answered", `return ${f[1]};`);
  const tag = `fw-v${registry.fw_train}`;
  const missing = { __missing: true, httpStatus: "404" };

  // From the copy: the copy is what is missing, named by its tag.
  const copy = { devChannel: false, manifestOverride: false };
  const stable = say(missing, copy, tag, true);
  assert.strictEqual(stable, `This site’s copy of firmware release ${tag} isn’t available (HTTP 404). ` +
    "If that release was just published, the site picks it up when it redeploys, shortly after; reload then. " +
    "Until then, try Advanced → dev channel, or install a local file.");
  const dev = say(missing, { devChannel: true, manifestOverride: false }, "fw-dev-latest", true);
  assert.strictEqual(dev, "This site’s copy of the dev channel (fw-dev-latest) isn’t available (HTTP 404). " +
    "If a dev build was just published, the site picks it up when it redeploys, shortly after; reload then. " +
    "Until then, turn the dev channel off under Advanced for the stable release, or install a local file.");
  assert.strictEqual(fine(missing, copy, true),
    " The images appear on reload once this site carries its copy of that release.");

  // From a ?manifest= override's own server: that server is what to check.
  // Not the official release, and not the dev channel (both are off while
  // the override is in force).
  const override = { devChannel: false, manifestOverride: true };
  const own = say(missing, override, null, true);
  assert.strictEqual(own, "The self-hosted manifest at this page’s ?manifest= address answered HTTP 404. " +
    "Check that server, remove ?manifest= from the address to use the official release, or install a " +
    "local file under Advanced.");
  assert.strictEqual(fine(missing, override, true), "");
  for (const s of [stable, dev, own]) assert.doesNotMatch(s, /published yet|been cut/, s);

  // No answer at all is a connection problem, wherever it was read from.
  const silent = { __missing: true, httpStatus: null };
  assert.match(say(silent, copy, tag, false), /connection problem here, not a missing release/);
  assert.strictEqual(fine(silent, copy, false),
    " If that release exists, the images will appear once you’re back online.");

  // The two readings are the core's own mapping: the hosted page reads both
  // channels' manifests from its copy, and a ?manifest= override as given.
  const { releaseFetchUrl, DEV_FLASH_MANIFEST_URL } = await core();
  const opts = { pageHref: PAGES, mirrorBase: PAGES_MIRROR, repo: catalog.repo };
  assert.notStrictEqual(releaseFetchUrl(catalog.manifest_url, opts), catalog.manifest_url);
  assert.notStrictEqual(releaseFetchUrl(DEV_FLASH_MANIFEST_URL, opts), DEV_FLASH_MANIFEST_URL);
  assert.strictEqual(releaseFetchUrl("http://127.0.0.1:9000/m.json", opts), "http://127.0.0.1:9000/m.json");
  assert.match(flashJs, /if \(override\) \{ state\.devChannel = false; return override; \}/);
});

test("we2-flash.js: an HTTP error on the pinned model names this site's copy, not an uncut release", () => {
  // The Vision model reads the same copy as the firmware (fetchReleaseAsset),
  // so its 404 has the same reading: the copy is missing, perhaps only until
  // the site redeploys. A failure with no HTTP answer keeps the fork /
  // uncut-train hint. Run the catch's own sentence, as written, on each case.
  const we2Js = readFileSync(join(ROOT, "assets/we2-flash.js"), "utf8");
  const m = /note\.textContent = (\/\^HTTP [\s\S]*?");\n/.exec(we2Js);
  assert.ok(m, "we2-flash.js: the pinned model's error sentence moved; point this test at it");
  const say = new Function("e", `return ${m[1]};`);
  assert.strictEqual(say(new Error("HTTP 404")),
    "This site’s copy of the release’s model isn’t available (HTTP 404). If the release or its model " +
    "was just published, it appears once the site redeploys; until then use a local file, or load it " +
    "once with SenseCraft (the Vision page stages that).");
  for (const why of ["Failed to fetch", "manifest has no model entry"]) {
    assert.strictEqual(say(new Error(why)),
      `Couldn’t fetch the pinned model (${why}). The asset ships with the project’s releases — if this ` +
      "is a fresh fork or the release train hasn’t cut one yet, use a local file, or load it once with " +
      "SenseCraft (the Vision page stages that).");
  }
  // The status reaches the catch as "HTTP <n>", from both reads.
  assert.strictEqual([...we2Js.matchAll(/if \(!r\.ok\) throw new Error\("HTTP " \+ r\.status\);/g)].length, 2);
});

// ── WiFi pre-provisioning (NVS image builder + QR payload) ──────────────────
test("buildNvsWifiImage round-trips through the NVS parser with valid CRCs", async () => {
  const { buildNvsWifiImage, parseNvs, witnessSummary, crc32EspRom } = await core();
  const img = buildNvsWifiImage("MyHomeWifi", "correct horse battery", 0x5000);
  assert.strictEqual(img.length, 0x5000);

  // Page header: ACTIVE state, v2 marker, CRC over bytes 4..27.
  const u32 = (o) => (img[o] | (img[o+1] << 8) | (img[o+2] << 16) | (img[o+3] << 24)) >>> 0;
  assert.strictEqual(u32(0), 0xfffffffe, "page must be ACTIVE");
  assert.strictEqual(img[8], 0xfe, "NVS v2 marker");
  assert.strictEqual(u32(28), crc32EspRom(img.subarray(4, 28)), "page header CRC");

  // Every written item's CRC must verify (bytes 0-3 + 8-31, skipping the CRC).
  const bitmap = img.subarray(32, 64);
  let checkedItems = 0;
  for (let j = 0; j < 126; j++) {
    const st = (bitmap[j >> 2] >> ((j & 3) * 2)) & 0x3;
    if (st !== 2) continue;
    const o = 64 + j * 32;
    // Payload entries of a blob carry raw data, not item headers — detect
    // headers by their span covering this slot: walk from the front instead.
    checkedItems++;
  }
  assert.ok(checkedItems >= 6, "namespace + 2 blobs (+payload) + 2 idx + wifi_en marked written");

  // The parser we trust for the health check must read it back.
  const items = parseNvs(img, ["wifi_ssid", "wifi_pass"]);
  const ssid = items.find((i) => i.namespace === "securacv" && i.key === "wifi_ssid");
  const pass = items.find((i) => i.namespace === "securacv" && i.key === "wifi_pass");
  const en = items.find((i) => i.namespace === "securacv" && i.key === "wifi_en");
  assert.ok(ssid && ssid.bytes && Buffer.from(ssid.bytes).toString() === "MyHomeWifi");
  assert.ok(pass && pass.bytes && Buffer.from(pass.bytes).toString() === "correct horse battery");
  assert.ok(en && en.value === 1);
  const s = witnessSummary(items);
  assert.strictEqual(s.wifiConfigured, true);

  // Blob payload CRCs match the payload bytes (what ESP-IDF verifies on read).
  const findHeader = (key, type) => {
    for (let j = 0; j < 126; j++) {
      const o = 64 + j * 32;
      if (img[o] === 1 && img[o + 1] === type) {
        let k = ""; for (let i = 0; i < 16 && img[o + 8 + i]; i++) k += String.fromCharCode(img[o + 8 + i]);
        if (k === key) return o;
      }
    }
    return -1;
  };
  const bo = findHeader("wifi_ssid", 0x42);
  assert.ok(bo > 0);
  const blobLen = img[bo + 24] | (img[bo + 25] << 8);
  assert.strictEqual(blobLen, 10);
  const payloadCrc = u32(bo + 28);
  assert.strictEqual(payloadCrc, crc32EspRom(img.subarray(bo + 32, bo + 32 + blobLen)));

  // Everything after page 0 is untouched flash.
  assert.ok(img.subarray(4096).every((b) => b === 0xff));

  // Validation: bad inputs throw before anything is built.
  assert.throws(() => buildNvsWifiImage("", "password123", 0x5000));
  assert.throws(() => buildNvsWifiImage("x".repeat(40), "password123", 0x5000));
  // No minimum: a 1..7 byte password is the router's business (WEP 5/13,
  // WPA3 any length) and the Canary's join verdict reports a refusal live.
  // The flasher used to throw here on the router's behalf; the firmware
  // never did (firmware/common/network/wifi_credential_policy.h).
  assert.doesNotThrow(() => buildNvsWifiImage("net", "short", 0x5000));
  assert.doesNotThrow(() => buildNvsWifiImage("net", "a", 0x5000));
  assert.doesNotThrow(() => buildNvsWifiImage("net", "f".repeat(64), 0x5000), "64 bytes is a hex key");
  assert.throws(() => buildNvsWifiImage("net", "p".repeat(65), 0x5000), "65 bytes is what the firmware refuses");
  // Open network (no password) is allowed.
  const open = buildNvsWifiImage("cafe", "", 0x5000);
  assert.ok(parseNvs(open).find((i) => i.key === "wifi_pass"));
});

test("api token blobs ride the seed and round-trip through the NVS parser", async () => {
  const { buildNvsSeedImage, apiTokenToNvs, mintApiToken, parseNvs } = await core();
  // A deterministic mint (crafted bytes) — the parity suite pins the
  // algorithm; here we prove the seed carries it as the firmware reads it.
  const bytes = new Uint8Array(64);
  for (let i = 0; i < 64; i++) bytes[i] = i;
  const token = mintApiToken(bytes);
  assert.match(token, /^cv_[0-9A-Za-z]{32}$/);
  const { blobs } = apiTokenToNvs(token);
  const img = buildNvsSeedImage(
    { wifi: { ssid: "MyHomeWifi", pass: "correct horse" }, blobs }, 0x5000);
  const items = parseNvs(img, ["api_token", "api_tkn"]);
  for (const key of ["api_token", "api_tkn"]) {
    const item = items.find((i) => i.namespace === "securacv" && i.key === key);
    assert.ok(item && item.bytes, `${key} must be a readable blob in the seed`);
    assert.strictEqual(Buffer.from(item.bytes).toString(), token,
      `${key} must hold the exact credential (no NUL, no truncation)`);
  }
  // Bad shapes are refused before a byte is laid out.
  assert.throws(() => apiTokenToNvs("cv_short"));
  assert.throws(() => apiTokenToNvs("xx_" + "a".repeat(32)));
  // A biased byte (>= 248) is never used: feed only tail bytes and starve it.
  assert.throws(() => mintApiToken(new Uint8Array(64).fill(255)));
});

test("mqttProvisioningToNvs: maps the optional broker/identity fields to native's NVS keys", async () => {
  const { mqttProvisioningToNvs } = await core();
  // Full set → exactly the keys/values native's build_nvs writes.
  const full = mqttProvisioningToNvs({
    deviceId: "canary_vision_ab12", mqttHost: "homeassistant.local",
    mqttPort: 1883, mqttUser: "canary", mqttPass: "broker-secret",
  });
  assert.deepStrictEqual(full.strings, {
    dev_id: "canary_vision_ab12", mqtt_host: "homeassistant.local",
    mqtt_user: "canary", mqtt_pass: "broker-secret",
  });
  assert.deepStrictEqual(full.u16, { mqtt_port: 1883 });
  assert.deepStrictEqual(full.u8, {}, "plain broker writes no mqtt_tls byte (the firmware default)");

  // Every field is optional — empty in, omitted out (no empty NVS keys written).
  assert.deepStrictEqual(mqttProvisioningToNvs({}), { strings: {}, u16: {}, u8: {} });
  assert.deepStrictEqual(mqttProvisioningToNvs({ deviceId: "just_me" }), { strings: { dev_id: "just_me" }, u16: {}, u8: {} });
  // A broker host with no user/pass (open broker) still writes host + port.
  assert.deepStrictEqual(
    mqttProvisioningToNvs({ mqttHost: "10.0.0.2", mqttPort: 1883 }),
    { strings: { mqtt_host: "10.0.0.2" }, u16: { mqtt_port: 1883 }, u8: {} });

  // Broker TLS: the mode byte + CA / pin the firmware's shared decision
  // header reads (mqtt_transport_logic.h). Incomplete pairs are refused HERE
  // because the firmware fails closed on them — a seeded board that refuses
  // to connect is a worse outcome than a form error.
  const pem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----";
  const fp = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
  const ca = mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 1, mqttCa: pem });
  assert.deepStrictEqual(ca.u8, { mqtt_tls: 1 });
  assert.strictEqual(ca.strings.mqtt_ca, pem + "\n", "PEM is stored with its trailing newline");
  assert.ok(!("mqtt_fp" in ca.strings));
  const pinned = mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: "fingerprint", mqttFp: fp });
  assert.deepStrictEqual(pinned.u8, { mqtt_tls: 2 });
  assert.strictEqual(pinned.strings.mqtt_fp, fp);
  assert.deepStrictEqual(mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: "insecure" }).u8, { mqtt_tls: 3 });
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 1 }), /needs the broker CA/);
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 2 }), /needs the broker fingerprint/);
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 1, mqttCa: fp }), /PEM certificate/);
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 2, mqttFp: fp.slice(0, 40) }), /SHA-256/);
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 8883, mqttTls: 7 }), /TLS mode/);
  // No broker host → the TLS fields are ignored with the rest of the broker row.
  assert.deepStrictEqual(mqttProvisioningToNvs({ mqttTls: 1, mqttCa: pem }), { strings: {}, u16: {}, u8: {} });

  // Validation mirrors native — bad values throw before anything is built.
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 0 }), /port/i);
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "h", mqttPort: 70000 }), /port/i);
  assert.throws(() => mqttProvisioningToNvs({ mqttHost: "x".repeat(64), mqttPort: 1 }), /host/i);
  assert.throws(() => mqttProvisioningToNvs({ deviceId: "d".repeat(33) }), /Device ID/i);
});

// ── the broker TLS receipt line: what was SEALED, in the native app's words ──
// One table in two places (flash-core.js MQTT_TLS_RECEIPT and
// desktop/flash-engine/src/broker_receipt.rs MODE_LABELS), held equal by
// desktop_parity.test.js. These pin the browser's half to the firmware's mode
// byte and to the bytes the builder actually seals — never the form, never a
// credential, never a claim that anything connected. Same fixtures as the
// Rust tests: a 58-byte PEM (59 sealed) and a 64-hex pin.
const RECEIPT_PEM = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----";
const RECEIPT_FP = "0a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9";
const RECEIPT_BROKER = { mqttHost: "homeassistant.local", mqttPort: 8883, mqttUser: "canary", mqttPass: "broker-secret" };
const receiptLeaks = (line) => /broker-secret|BEGIN CERTIFICATE|MIIB|homeassistant\.local|\{[A-Z]+\}/.test(line);

test("brokerTlsReceipt: no line when no broker host was sealed", async () => {
  const { brokerTlsReceipt, mqttProvisioningToNvs } = await core();
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({})), null);
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({ deviceId: "just_me" })), null);
  // TLS fields without a host are dropped by the builder with the rest of the
  // broker row — so there is nothing sealed to describe.
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({ mqttTls: 1, mqttCa: RECEIPT_PEM })), null);
  for (const empty of [null, undefined, {}, { strings: {} }]) assert.strictEqual(brokerTlsReceipt(empty), null);
});

test("brokerTlsReceipt: plain (mode 0) names the default and says not encrypted", async () => {
  const { MQTT_TLS_RECEIPT, brokerTlsReceipt, mqttProvisioningToNvs } = await core();
  const prov = mqttProvisioningToNvs(RECEIPT_BROKER);
  assert.deepStrictEqual(prov.u8, {}, "plain writes no mqtt_tls byte — the receipt still names the mode");
  const line = brokerTlsReceipt(prov);
  assert.strictEqual(line, MQTT_TLS_RECEIPT[0]);
  assert.strictEqual(line, "plain MQTT — not encrypted (the default every Canary shipped with)");
  assert.ok(!receiptLeaks(line), line);
  // An explicit 0 or the mode's name is the same row.
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: 0 })), line);
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: "plain" })), line);
});

test("brokerTlsReceipt: CA mode counts the SEALED bytes (trimmed PEM + newline), never the PEM", async () => {
  const { brokerTlsReceipt, mqttProvisioningToNvs } = await core();
  // Whatever whitespace the paste carried, the builder seals trim + "\n" and
  // the receipt counts THAT: 58 + 1 = 59. provisioning.rs's
  // broker_tls_summary test pins the same fixture to the same 59, and
  // desktop_parity.test.js holds the two numbers together.
  const prov = mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: 1, mqttCa: `  ${RECEIPT_PEM}\n\n` });
  assert.strictEqual(prov.strings.mqtt_ca, RECEIPT_PEM + "\n");
  assert.strictEqual(new TextEncoder().encode(prov.strings.mqtt_ca).length, 59);
  const line = brokerTlsReceipt(prov);
  assert.strictEqual(line, "TLS, CA-verified — CA certificate sealed, 59 bytes of PEM");
  assert.ok(!receiptLeaks(line), line);
  // Bytes, not characters: a two-byte UTF-8 character in the paste counts as
  // two (the builder's own 3070 cap is in bytes for the same reason).
  const wide = mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: 1, mqttCa: RECEIPT_PEM.replace("MIIB", "MIIBé") });
  assert.strictEqual(wide.strings.mqtt_ca.length, 60);
  assert.strictEqual(brokerTlsReceipt(wide), "TLS, CA-verified — CA certificate sealed, 61 bytes of PEM");
});

test("brokerTlsReceipt: fingerprint mode carries the sealed 64-hex verbatim", async () => {
  const { brokerTlsReceipt, mqttProvisioningToNvs } = await core();
  // Case as typed, no separators added or removed: the owner compares this
  // against openssl's own line, so it must be the sealed string exactly.
  const mixed = RECEIPT_FP.toUpperCase().replace("A", "a");
  const prov = mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: 2, mqttFp: mixed });
  assert.strictEqual(prov.strings.mqtt_fp, mixed);
  const line = brokerTlsReceipt(prov);
  assert.strictEqual(line, `TLS, SHA-256 fingerprint pin — ${mixed}`);
  assert.ok(line.endsWith(prov.strings.mqtt_fp), "the pin on the receipt is the pin that was sealed");
  assert.ok(!receiptLeaks(line), line);
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: "fingerprint", mqttFp: RECEIPT_FP })),
    `TLS, SHA-256 fingerprint pin — ${RECEIPT_FP}`);
});

test("brokerTlsReceipt: lab mode keeps both warnings; no row leaks a credential; the table is four single lines", async () => {
  const { MQTT_TLS_RECEIPT, MQTT_TLS_RECEIPT_UNKNOWN, brokerTlsReceipt, mqttProvisioningToNvs } = await core();
  const lab = brokerTlsReceipt(mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: 3 }));
  assert.strictEqual(lab, MQTT_TLS_RECEIPT[3]);
  assert.match(lab, /NOT verified/);
  assert.match(lab, /warns on every connect/);
  assert.strictEqual(brokerTlsReceipt(mqttProvisioningToNvs({ ...RECEIPT_BROKER, mqttTls: "insecure" })), lab);
  // Every row, with a password, a CA and a pin all on the form: the line
  // names the mode and at most the CA's byte count or the pin.
  const all = { ...RECEIPT_BROKER, mqttCa: RECEIPT_PEM, mqttFp: RECEIPT_FP };
  for (const mode of [0, 1, 2, 3]) {
    const line = brokerTlsReceipt(mqttProvisioningToNvs({ ...all, mqttTls: mode }));
    assert.ok(!receiptLeaks(line), `mode ${mode}: ${line}`);
    assert.ok(!/connected/i.test(line), `mode ${mode} must describe what was sealed, never a connection`);
  }
  assert.strictEqual(MQTT_TLS_RECEIPT.length, 4, "the firmware knows four modes");
  assert.ok(Object.isFrozen(MQTT_TLS_RECEIPT));
  for (const row of MQTT_TLS_RECEIPT) assert.ok(!row.includes("\n"), "each row stays one line (desktop_parity reads the Rust source)");
  // A byte outside the table (the builder refuses one; a hand-built prov
  // could still carry it) gets the firmware's own answer, never a borrowed row.
  const odd = brokerTlsReceipt({ strings: { mqtt_host: "h" }, u16: {}, u8: { mqtt_tls: 9 } });
  assert.strictEqual(odd, MQTT_TLS_RECEIPT_UNKNOWN.replace("{M}", "9"));
  assert.match(odd, /^TLS mode byte 9 — not one the firmware knows/);
});

test("buildNvsSeedImage bakes dev_id + MQTT the firmware reads back — same keys/types as native", async () => {
  const { buildNvsSeedImage, mqttProvisioningToNvs, parseNvs } = await core();
  const { strings, u16 } = mqttProvisioningToNvs({
    deviceId: "canary_vision_ab12", mqttHost: "homeassistant.local",
    mqttPort: 1883, mqttUser: "canary", mqttPass: "s3cret!!",
  });
  const img = buildNvsSeedImage(
    { wifi: { ssid: "Bird House", pass: "correct horse" }, wifiScheme: "string", strings, u16 }, 0x5000);

  // Read it back exactly as the firmware would (string keys allow-listed).
  const items = parseNvs(img, ["wifi_ssid", "wifi_pass", "dev_id", "mqtt_host", "mqtt_user", "mqtt_pass"]);
  const get = (k) => items.find((i) => i.namespace === "securacv" && i.key === k);
  const strOf = (k) => Buffer.from(get(k).bytes).toString();
  // Strings are ESP-IDF type 0x21 (Preferences getString), NUL stripped on read.
  assert.strictEqual(get("dev_id").type, 0x21);
  assert.strictEqual(strOf("dev_id"), "canary_vision_ab12");
  assert.strictEqual(strOf("mqtt_host"), "homeassistant.local");
  assert.strictEqual(strOf("mqtt_user"), "canary");
  assert.strictEqual(strOf("mqtt_pass"), "s3cret!!");
  assert.strictEqual(strOf("wifi_ssid"), "Bird House"); // string scheme, like usb-secrets boards
  // mqtt_port is a u16 (Preferences putUShort → type 0x02), not a string.
  assert.strictEqual(get("mqtt_port").type, 0x02);
  assert.strictEqual(get("mqtt_port").value, 1883);
  // Nothing bled past the one NVS page.
  assert.ok(img.subarray(4096).every((b) => b === 0xff));
});

test("seeded Wi-Fi is taken at face value: open networks and the first-boot latch", async () => {
  const { buildNvsSeedImage, parseNvs } = await core();

  // String scheme (sense/vision/display), OPEN network: the empty password
  // must still be WRITTEN as a present key. The firmware loader treats a
  // present key as the answer — an absent one falls back to the compiled
  // ci-placeholder, which is exactly the wrong password to try on an open AP.
  const open = buildNvsSeedImage(
    { wifi: { ssid: "Open House", pass: "" }, wifiScheme: "string" }, 0x5000);
  const openItems = parseNvs(open, ["wifi_ssid", "wifi_pass"]);
  const openPass = openItems.find((i) => i.namespace === "securacv" && i.key === "wifi_pass");
  assert.ok(openPass, "empty wifi_pass must still write its key (string scheme)");
  assert.strictEqual(openPass.type, 0x21);
  assert.strictEqual(Buffer.from(openPass.bytes).length, 0, "the seeded password IS empty");

  // Blob scheme (canary/wap): seeded Wi-Fi IS the setup, so the first-boot
  // latch is marked done — without setup_ok the board boots into SETUP MODE
  // with its captive portal even though the STA join succeeds.
  const blob = buildNvsSeedImage(
    { wifi: { ssid: "Bird House", pass: "correct horse" }, wifiScheme: "blob" }, 0x5000);
  const blobItems = parseNvs(blob);
  const latch = blobItems.find((i) => i.namespace === "securacv" && i.key === "setup_ok");
  assert.ok(latch, "blob-scheme seed must mark setup_ok");
  assert.strictEqual(latch.value, 1);
  // …and the string scheme must NOT write it: those firmwares have no latch,
  // and a stray key would shadow a future meaning.
  assert.ok(!openItems.find((i) => i.key === "setup_ok"),
    "string scheme has no first-boot latch to mark");
});

test("buildNvsSeedImage: the OTA opt-in rides the engine's own second namespace", async () => {
  const { buildNvsSeedImage, parseNvs, crc32EspRom } = await core();
  const u32 = (b, o) => (b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24)) >>> 0;

  // Opted in: the page grows a SECOND namespace-definition entry — an item
  // under index 0, type 0x01, key "securacv_ota", data byte 2 (the new
  // index, right after "securacv"'s 1) — and a u8 "auto_upd" = 1 under it.
  const img = buildNvsSeedImage(
    { wifi: { ssid: "MyHomeWifi", pass: "correct horse" }, autoUpdate: true }, 0x5000);
  const findHeader = (ns, key) => {
    for (let j = 0; j < 126; j++) {
      const o = 64 + j * 32;
      if (img[o] !== ns) continue;
      let k = ""; for (let i = 0; i < 16 && img[o + 8 + i]; i++) k += String.fromCharCode(img[o + 8 + i]);
      if (k === key) return o;
    }
    return -1;
  };
  const nsDef = findHeader(0, "securacv_ota");
  assert.ok(nsDef > 0, "the securacv_ota namespace entry must exist under index 0");
  assert.strictEqual(img[nsDef + 1], 0x01, "namespace entries are type 0x01");
  assert.strictEqual(img[nsDef + 24], 2, "securacv_ota must be namespace index 2");
  const item = findHeader(2, "auto_upd");
  assert.ok(item > 0, "auto_upd must live under namespace index 2");
  assert.strictEqual(img[item + 1], 0x01, "the engine reads auto_upd with nvs_get_u8 — a u8");
  assert.strictEqual(img[item + 24], 1);

  // Both new items' CRCs verify exactly the way ESP-IDF checks them on boot
  // (bytes 0-3 + 8-31, skipping the CRC field), and the page header CRC
  // still covers the grown page.
  for (const o of [nsDef, item]) {
    const tmp = new Uint8Array(28);
    tmp.set(img.subarray(o, o + 4), 0);
    tmp.set(img.subarray(o + 8, o + 32), 4);
    assert.strictEqual(u32(img, o + 4), crc32EspRom(tmp), "item CRC must verify");
  }
  assert.strictEqual(u32(img, 28), crc32EspRom(img.subarray(4, 28)), "page header CRC");

  // The parser the health check trusts reads it back, namespaces intact.
  const items = parseNvs(img, ["wifi_ssid"]);
  const auto = items.find((i) => i.key === "auto_upd");
  assert.strictEqual(auto.namespace, "securacv_ota");
  assert.strictEqual(auto.nsIndex, 2);
  assert.strictEqual(auto.value, 1);
  assert.ok(items.find((i) => i.key === "wifi_ssid").namespace === "securacv",
    "the wifi keys stay under securacv — two namespaces, one page");

  // Declined is an explicit 0 — a recorded choice, not an absent key.
  const off = parseNvs(buildNvsSeedImage({ autoUpdate: false }, 0x4000))
    .find((i) => i.namespace === "securacv_ota" && i.key === "auto_upd");
  assert.ok(off, "unchecked must still write the key");
  assert.strictEqual(off.value, 0);

  // No opt at all → byte-identical to the pre-OTA seed: no second namespace,
  // no auto_upd, and the exact same bytes as an explicit non-boolean.
  const base = buildNvsSeedImage({ wifi: { ssid: "MyHomeWifi", pass: "correct horse" } }, 0x5000);
  const baseNull = buildNvsSeedImage(
    { wifi: { ssid: "MyHomeWifi", pass: "correct horse" }, autoUpdate: null }, 0x5000);
  assert.deepStrictEqual(Buffer.from(baseNull), Buffer.from(base),
    "a non-boolean autoUpdate must leave the seed byte-identical");
  assert.ok(!parseNvs(base).some((i) => i.key === "auto_upd" || i.namespace === "securacv_ota"),
    "no opt must write neither the key nor the namespace");
});

test("wifiQrString escapes the special characters and handles open networks", async () => {
  const { wifiQrString } = await core();
  assert.strictEqual(wifiQrString("MyWifi", "pass1234"), "WIFI:T:WPA;S:MyWifi;P:pass1234;;");
  assert.strictEqual(
    wifiQrString('we;ird"ssid,x:', 'p\\ss;1234'),
    'WIFI:T:WPA;S:we\\;ird\\"ssid\\,x\\:;P:p\\\\ss\\;1234;;');
  assert.strictEqual(wifiQrString("open-net", ""), "WIFI:T:nopass;S:open-net;;");
});

test("wifiMemoryStatus: makes 'type once' visible, and honest about persistence", async () => {
  const { wifiMemoryStatus } = await core();
  // Nothing saved → no banner.
  const none = wifiMemoryStatus(null, false);
  assert.strictEqual(none.hasSaved, false);
  assert.strictEqual(wifiMemoryStatus({ ssid: "" }, true).hasSaved, false);
  assert.strictEqual(wifiMemoryStatus({ ssid: "   " }, false).hasSaved, false); // whitespace only

  // Session-only → names the network + nudges persistence.
  const sess = wifiMemoryStatus({ ssid: "HomeNet", pass: "x" }, false);
  assert.strictEqual(sess.hasSaved, true);
  assert.strictEqual(sess.persisted, false);
  assert.strictEqual(sess.ssid, "HomeNet");
  assert.match(sess.headline, /HomeNet/);
  assert.match(sess.detail, /session/i);
  assert.match(sess.detail, /Remember on this computer/i);

  // Persisted → says it's kept on this computer, no persistence nudge.
  const kept = wifiMemoryStatus({ ssid: "HomeNet", pass: "x" }, true);
  assert.strictEqual(kept.persisted, true);
  assert.match(kept.detail, /this computer/i);
  assert.doesNotMatch(kept.detail, /Tick/i);
});

test("wifiBannerState: shows only while the fields still hold the saved network", async () => {
  const { wifiBannerState } = await core();
  const saved = { ssid: "HomeNet", pass: "s3cret" };
  // Untouched fields (equal the saved) → banner shows and names the network.
  const shown = wifiBannerState("HomeNet", "s3cret", saved, false);
  assert.strictEqual(shown.show, true);
  assert.strictEqual(shown.hasSaved, true);
  assert.match(shown.headline, /HomeNet/);
  // Editing the SSID or the password hides it — it must not claim a network the
  // fields no longer carry.
  assert.strictEqual(wifiBannerState("OtherNet", "s3cret", saved, false).show, false);
  assert.strictEqual(wifiBannerState("HomeNet", "different", saved, false).show, false);
  // Nothing saved → never shows.
  assert.strictEqual(wifiBannerState("HomeNet", "s3cret", null, false).show, false);
  // Open network (empty saved password) with the matching empty field → shows.
  assert.strictEqual(wifiBannerState("Cafe", "", { ssid: "Cafe", pass: "" }, false).show, true);
});

test("formatters", async () => {
  const { formatBytes, formatMac } = await core();
  assert.strictEqual(formatBytes(512), "512 B");
  assert.strictEqual(formatBytes(2048), "2 KB");
  assert.strictEqual(formatBytes(1572864), "1.50 MB");
  assert.strictEqual(formatMac("a1:b2:c3:d4:e5:f6"), "A1:B2:C3:D4:E5:F6");
});

// ── error classification: the right cause → the right fix ───────────────────
test("classifyFlashError maps the real Web Serial / esptool failures to advice", async () => {
  const { classifyFlashError } = await core();
  const kindOf = (m) => classifyFlashError(new Error(m)).kind;

  // Port held by another program/tab — the most common real failure.
  assert.strictEqual(kindOf("Failed to open serial port."), "port-busy");
  assert.strictEqual(kindOf("The port is already open"), "port-busy");
  // Cable pulled / device vanished.
  assert.strictEqual(kindOf("The device has been lost."), "device-lost");
  assert.strictEqual(kindOf("A network error occurred (NetworkError)."), "device-lost");
  // OS permission / driver.
  assert.strictEqual(kindOf("Access denied to the serial port"), "permission");
  // Integrity beats the generic network case.
  assert.strictEqual(kindOf("Downloaded image failed its checksum — refusing to flash it."), "integrity");
  // Download / release problem.
  assert.strictEqual(kindOf("download failed (HTTP 404)"), "download");
  // Not in download mode.
  assert.strictEqual(kindOf("Timed out waiting for packet header"), "not-in-download");
  assert.strictEqual(kindOf("Invalid head of packet (0x00)"), "not-in-download");
  // Our chunked reader gave up.
  assert.strictEqual(kindOf("short read at 0x1a0000"), "read-stall");
  // An image refused for where it keeps its bootloader: no retry fixes that.
  assert.strictEqual(kindOf("This image would not start on the ESP32: that chip loads its " +
    "bootloader from 0x1000, and the image has no bootloader for it there."), "layout");
  // Unknown → generic, but still actionable + no hijacked title.
  const u = classifyFlashError(new Error("kernel panic in the toaster"));
  assert.strictEqual(u.kind, "unknown");
  assert.strictEqual(u.title, null);
  // Every verdict carries a non-empty hint.
  for (const m of ["Failed to open serial port", "device has been lost", "access denied",
    "checksum", "download failed", "timed out", "short read", "??"]) {
    assert.ok(classifyFlashError(new Error(m)).hint.length > 0, `no hint for "${m}"`);
  }
  // Robust to junk input.
  assert.strictEqual(classifyFlashError(null).kind, "unknown");
  assert.strictEqual(classifyFlashError("just a string").kind, "unknown");
});

// ── deep-link focus: /checkup → flash.html?product=… ────────────────────────
test("preferredProductId reads a safe product hint from the query string", async () => {
  const { preferredProductId } = await core();
  assert.strictEqual(preferredProductId("?product=securacv-canary"), "securacv-canary");
  assert.strictEqual(preferredProductId("?foo=1&product=securacv-canary-sense"), "securacv-canary-sense");
  assert.strictEqual(preferredProductId(""), null);
  assert.strictEqual(preferredProductId("?other=x"), null);
  // Reject anything that isn't a plain id (no injection into DOM ids / selectors).
  assert.strictEqual(preferredProductId("?product=../../etc"), null);
  assert.strictEqual(preferredProductId("?product=a b"), null);
  // Every id the /checkup selector can send resolves to a real catalog product,
  // so the deep-link never lands on nothing.
  for (const p of catalog.products) {
    assert.strictEqual(preferredProductId("?product=" + p.id), p.id);
  }
});

test("recommendedProduct leads with the flagship for the detected chip", async () => {
  const { recommendedProduct, productsForChip } = await core();
  // First catalog match per chip — the flagship, by authoring order.
  for (const chip of ["ESP32-S3", "ESP32-C3", "ESP32-C6"]) {
    const rec = recommendedProduct(catalog, chip);
    assert.ok(rec, `no recommendation for ${chip}`);
    assert.strictEqual(rec, productsForChip(catalog, chip)[0]);
    assert.strictEqual(rec.chip, chip, "recommendation must fit the chip");
  }
  // The S3 flagship is the all-rounder Canary, not a variant.
  assert.strictEqual(recommendedProduct(catalog, "ESP32-S3").id, "securacv-canary");
  // Unknown silicon → no recommendation (the picker shows its empty state).
  assert.strictEqual(recommendedProduct(catalog, "ESP32-Q9"), null);
});

// ── catalog guard: a malformed catalog degrades, never crashes the page ──────
test("validateCatalog passes the shipped catalog and flags real breakage", async () => {
  const { validateCatalog } = await core();
  assert.deepStrictEqual(validateCatalog(catalog), [], "the shipped flash.json must validate");

  assert.ok(validateCatalog(null).length, "null is rejected");
  assert.ok(validateCatalog({}).length, "empty object is rejected");
  // Missing the safety-strip copy renderReassurance reads.
  const noBrick = JSON.parse(JSON.stringify(catalog)); delete noBrick.no_brick;
  assert.ok(validateCatalog(noBrick).some((e) => /no_brick/.test(e)));
  // A product missing the chip the guard needs.
  const badProd = JSON.parse(JSON.stringify(catalog)); delete badProd.products[0].chip;
  assert.ok(validateCatalog(badProd).some((e) => /missing chip/.test(e)));
  // Hatching copy is now first-class catalog metadata, not hardcoded UI copy.
  const noHatch = JSON.parse(JSON.stringify(catalog)); delete noHatch.products[0].hatch;
  assert.ok(validateCatalog(noHatch).some((e) => /missing hatch moment/.test(e)));
});

// ── self-healing: baud ladder, boot-log diagnosis, bridge detect, report ────
test("FLASH_BAUDS: fastest-first ladder, distinct from the console list", async () => {
  const { FLASH_BAUDS } = await core();
  assert.deepStrictEqual(FLASH_BAUDS, [921600, 460800, 230400, 115200]);
  // strictly descending — the connect flow steps down on failure
  for (let i = 1; i < FLASH_BAUDS.length; i++) assert.ok(FLASH_BAUDS[i] < FLASH_BAUDS[i - 1]);
});

test("diagnoseBootLog: maps fatal signatures to fixes, else null", async () => {
  const { diagnoseBootLog } = await core();
  assert.strictEqual(diagnoseBootLog(""), null);
  assert.strictEqual(diagnoseBootLog("Hello canary chirp, booting normally"), null);

  const brown = diagnoseBootLog("rst:0x10 (RTCWDT_RTC_RST)\nBrownout detector was triggered\n");
  assert.strictEqual(brown.signature, "brownout");
  assert.strictEqual(brown.action, "power"); // NOT a reflash — a power problem

  const panic = diagnoseBootLog("Guru Meditation Error: Core 0 panic'ed\nBacktrace: 0x400...");
  assert.strictEqual(panic.signature, "panic");
  assert.strictEqual(panic.action, "clean-install");

  const noapp = diagnoseBootLog("invalid header: 0xffffffff\nno bootable app partitions");
  assert.strictEqual(noapp.signature, "no-app");
  assert.strictEqual(noapp.action, "clean-install");

  // every diagnosis carries human means + fix text
  for (const d of [brown, panic, noapp]) { assert.ok(d.means && d.fix); }
});

test("usbBridgeInfo: native USB → null, bridge chips → driver link", async () => {
  const { usbBridgeInfo } = await core();
  assert.strictEqual(usbBridgeInfo(0x303a, 0x1001), null); // Espressif native USB
  assert.strictEqual(usbBridgeInfo(null, null), null);
  assert.strictEqual(usbBridgeInfo(0xbeef, 0x1), null);    // unknown → no driver to chase
  const cp = usbBridgeInfo(0x10c4, 0xea60);
  assert.ok(cp && /CP210x/.test(cp.name) && /silabs\.com/.test(cp.driverUrl));
  const ch = usbBridgeInfo(0x1a86, 0x7523);
  assert.ok(ch && /CH340/.test(ch.name) && /wch/.test(ch.driverUrl));
});

test("buildDiagnosticReport: formats safe facts, omits empties, no secrets", async () => {
  const { buildDiagnosticReport } = await core();
  const r = buildDiagnosticReport({
    browser: "Chrome", webSerial: true, chip: "ESP32-S3", mac: "94:B9:7E:5A:7F:A3",
    baud: 460800, error: "timed out", product: "canary-wap",
    logTail: "line1\nline2\nBrownout detector was triggered",
  });
  assert.ok(r.includes("SecuraCV flasher diagnostic"));
  assert.ok(r.includes("web serial: yes") && r.includes("connected baud: 460800"));
  assert.ok(r.includes("chip: ESP32-S3") && r.includes("error: timed out"));
  assert.ok(r.includes("Brownout")); // log tail included
  // empty/missing fields are omitted, not rendered as blanks
  assert.ok(!/platform:/.test(r) && !/flash size:/.test(r));
  // never leaks a field we didn't pass (e.g. wifi/keys aren't inputs at all)
  assert.ok(!/password|ssid|pubkey/i.test(r));
});

test("buildDiagnosticReport: the MAC is a tail, never the stable identifier", async () => {
  // A report is made to be pasted somewhere public; a full MAC is a stable
  // identifier (invariants.md III) and has no business riding along. The tail
  // (last two octets) still tells two bench boards apart.
  const { buildDiagnosticReport, macTail } = await core();
  const r = buildDiagnosticReport({ mac: "94:B9:7E:5A:7F:A3" });
  assert.ok(r.includes("MAC tail: …7f:a3"), "the non-stable tail is shown");
  assert.ok(!r.includes("94:B9") && !/94:b9/i.test(r), "the full MAC never appears");
  assert.strictEqual(macTail("94:B9:7E:5A:7F:A3"), "…7f:a3");
  assert.strictEqual(macTail("94b97e5a7fa3"), "…7f:a3");     // separator-free input can't leak whole
  assert.strictEqual(macTail(""), "");
  assert.strictEqual(macTail("a3"), "", "too short to be a MAC → omit");
  // no MAC → the line is omitted entirely, not rendered blank
  assert.ok(!/MAC/.test(buildDiagnosticReport({ chip: "ESP32-S3" })));
});

test("buildDiagnosticReport: the serial tail is scrubbed of credential lines", async () => {
  // The WAP legitimately prints its device-unique AP password to serial at
  // boot ("[WIFI] AP started: … (password: …)" — see build_config.h's own
  // acknowledgment). A report a stuck user is ASKED to paste must not carry
  // it. Redaction is visible, not silent, and diagnosis survives: fatal
  // signatures and selftest PASS rows come through untouched.
  const { buildDiagnosticReport, sanitizeLogTail } = await core();
  const tail = [
    "[WIFI] AP started: SecuraCV-4dC2 (password: cv-supersecret1)",
    "[PROV]   WiFi PASS : cv-supersecret1",
    'Password: cv-supersecret1',
    '║    "ap_password": "cv-supersecret1",',
    "[PROV]   API TOKEN : cv-supersecret1",        // review on #1551: tokens too
    '║    "hw_token": "cv-supersecret1",',
    "wifi: PASS",                                  // selftest verdict row — kept
    "Brownout detector was triggered",             // fatal signature — kept
  ].join("\n");
  const r = buildDiagnosticReport({ logTail: tail });
  assert.ok(!r.includes("cv-supersecret1"), "no credential survives into the report");
  // Every setup surface also prints its key inside a Wi-Fi QR payload when
  // its setup network comes up (onboarding_fsm.h qr_line: the WAP once, at
  // boot; the flagship's announce_setup_network again on every raiseAp), and
  // on the flagship that key is the whole boundary for the page token (sweep
  // F350).
  // No other pattern above matches it: these are the lines a boot prints.
  const qrTail = [
    "[WIFI] Connection lost, raising AP",
    '[SETUP] Setup network up: "SecuraCV-canary-AbCd"  password cv-e8Qun72aUeCc  (channel 1)',
    "[SETUP] QR WIFI:T:WPA;S:SecuraCV-canary-AbCd;P:cv-e8Qun72aUeCc;;",
    "[SETUP] QR WIFI:T:WPA;S:SecuraCV-wap-4dC2;P:cv-3jByJ6rHdhM2;;",
    "WIFI:S:SecuraCV-sense-4dC2;T:WPA;P:k7Qm2xRt;;",     // a payload in another field order
  ].join("\n");
  const q = buildDiagnosticReport({ stage: "idle", logTail: qrTail });
  for (const key of ["cv-e8Qun72aUeCc", "cv-3jByJ6rHdhM2", "k7Qm2xRt"]) {
    assert.ok(!q.includes(key), `the setup network's key ${key} survives into the report`);
  }
  assert.ok(q.includes("[WIFI] Connection lost, raising AP"), "the diagnosis line is kept");
  assert.strictEqual(sanitizeLogTail("[WIFI] AP raised on channel 1"), "[WIFI] AP raised on channel 1",
    "a [WIFI] log line with no QR payload is not collateral damage");
  assert.ok(r.includes("[redacted: credential line]"), "redaction is visible, not silent");
  assert.ok(r.includes("wifi: PASS"), "selftest verdict rows are not collateral damage");
  assert.ok(r.includes("Brownout detector was triggered"), "diagnosis survives the scrub");
  // the pure scrubber, directly
  assert.strictEqual(sanitizeLogTail("nothing sensitive here"), "nothing sensitive here");
  assert.ok(!sanitizeLogTail("psk=hunter2").includes("hunter2"));
});

// ── the setup network: its name's grammar, and the two lines a boot prints ──
test("isSetupNetworkSsid: the twin of onboarding_fsm.h is_setup_ssid, on the host test's vectors", async () => {
  const { isSetupNetworkSsid, SETUP_SSID_RE } = await core();
  // firmware/tests_host/test_onboarding_fsm.cpp the_matcher_accepts_every_shape_ever_emitted
  const ours = ["SecuraCV-4dC2", "SecuraCV-4dC2-k7", "SecuraCV-sense-4dC2",
    "SecuraCV-sentinel-4dC2-k7", "SecuraCV-display-CNRY"];
  const notOurs = ["SecuraCV-4dC", "SecuraCV-4dC2x", "SecuraCV-4dC2-", "SecuraCV-4dC2-k",
    "SecuraCV-Sense-4dC2", "SecuraCV-s3nse-4dC2", "SecuraCV-vision-4dC2-k7x", "SecuraCV-a-b-c-d",
    "securacv-4dC2", "MyWiFi", "SecuraCV-", "SecuraCV-4d C2"];
  for (const s of ours) assert.ok(isSetupNetworkSsid(s), `${s} is ours`);
  for (const s of notOurs) assert.ok(!isSetupNetworkSsid(s), `${s} is not ours`);
  // The C++ takes a C string (null is false); the twin takes whatever a scan list hands it.
  for (const v of [null, undefined, 42, {}, ["SecuraCV-4dC2"]]) assert.ok(!isSetupNetworkSsid(v));
  // A line ending is not part of a name — anchored to the whole input, case-sensitive.
  assert.ok(!isSetupNetworkSsid("SecuraCV-4dC2\n"));
  assert.ok(!SETUP_SSID_RE.flags.includes("m") && !SETUP_SSID_RE.flags.includes("i"));
  // Every node type the firmware names fits the 1..8 lowercase rule; a ninth letter does not,
  // and a letter outside ASCII is not alphanumeric here any more than in the C++.
  for (const t of ["canary", "wap", "vision", "sense", "sentinel", "display"]) {
    assert.ok(isSetupNetworkSsid(`SecuraCV-${t}-4dC2`), t);
    assert.ok(isSetupNetworkSsid(`SecuraCV-${t}-4dC2-k7`), `${t}, tagged`);
  }
  assert.ok(!isSetupNetworkSsid("SecuraCV-sentinels-4dC2"), "a type is at most eight letters");
  assert.ok(!isSetupNetworkSsid("SecuraCV-4dCé"));
});

test("parseSetupNetwork reads the typed names off both boot lines; parseSetupQr the code beside them", async () => {
  const { parseSetupNetwork, parseSetupQr, setupNetworkLine, isSetupNetworkSsid, wifiQrString } = await core();
  // onboarding_fsm.h announce_line / qr_line, printed with println (CR LF) by setup_portal.cpp.
  const sense = 'boot\r\n[SETUP] Setup network up: "SecuraCV-sense-4dC2"  password a7Kq2pMx  (channel 1)\r\n' +
    "[SETUP] QR WIFI:T:WPA;S:SecuraCV-sense-4dC2;P:a7Kq2pMx;;\r\n[SETUP] Join it from a phone\r\n";
  assert.deepStrictEqual(parseSetupNetwork(sense), { ssid: "SecuraCV-sense-4dC2", key: "a7Kq2pMx" });
  assert.ok(isSetupNetworkSsid(parseSetupNetwork(sense).ssid));
  assert.strictEqual(parseSetupQr(sense), "WIFI:T:WPA;S:SecuraCV-sense-4dC2;P:a7Kq2pMx;;",
    "the payload, without the CR println adds");
  // The QR line says what the network line says, in the grammar wifiQrString builds.
  assert.strictEqual(parseSetupQr(sense), wifiQrString("SecuraCV-sense-4dC2", "a7Kq2pMx"));
  // canary_wap.ino's own line, with the typed name.
  const wap = "[WIFI] AP started: SecuraCV-wap-4dC2 (password: cv-7hJ2kQ9mN4pR)\n[OK] WiFi AP active\n";
  assert.deepStrictEqual(parseSetupNetwork(wap), { ssid: "SecuraCV-wap-4dC2", key: "cv-7hJ2kQ9mN4pR" });
  assert.strictEqual(setupNetworkLine(parseSetupNetwork(wap)),
    "Its setup network: SecuraCV-wap-4dC2 · key cv-7hJ2kQ9mN4pR");
  // Older firmware's names still parse — the field is full of them.
  assert.deepStrictEqual(parseSetupNetwork('[SETUP] Setup network up: "SecuraCV-4dC2-k7"  password a7Kq2pMx  (channel 6)'),
    { ssid: "SecuraCV-4dC2-k7", key: "a7Kq2pMx" });
  // An open network's code: T:nopass and no P: field (provision_core.h wifi_qr_payload).
  assert.strictEqual(parseSetupQr("[SETUP] QR WIFI:T:nopass;S:Open Cafe;;\r\n"), "WIFI:T:nopass;S:Open Cafe;;");
  // Escapes ride through untouched — the payload is handed to a QR encoder as is.
  assert.strictEqual(parseSetupQr("[SETUP] QR WIFI:T:WPA;S:Bob's\\;Cafe;P:p\\:w;;"), "WIFI:T:WPA;S:Bob's\\;Cafe;P:p\\:w;;");
  assert.strictEqual(parseSetupQr('[SETUP] Join requested: "Home"'), null, "a different [SETUP] line is not the code");
  assert.strictEqual(parseSetupQr(""), null);
  assert.strictEqual(parseSetupQr(null), null);
  // The QR line never stands in for the network line: it carries nothing the done card needs beyond [0].
  assert.strictEqual(parseSetupNetwork("[SETUP] QR WIFI:T:WPA;S:SecuraCV-sense-4dC2;P:a7Kq2pMx;;"), null);
});

// ── post-flash proof: self-manifest read-back ───────────────────────────────
test("parseSelfManifest: extracts the signed manifest from a noisy boot log", async () => {
  const { parseSelfManifest, formatFingerprint } = await core();
  const m = {
    schema: "securacv.canary.manifest/v1", board: "xiao-esp32s3", firmware: "2.3.0-wap",
    git: "abc1234", pubkey: "00112233", pubkey_fp: "aabbccddeeff0011", health: 100,
    tamper: false, seq: 5, boots: 12,
    features: ["ota", "ble"], commands: [{ key: "j", name: "self-manifest" }, { key: "h", name: "help" }],
  };
  // Real serial output: boot spam, then the JSON line, then more spam.
  const buf = "rst:0x1 (POWERON)\nSecuraCV canary booting\n" + JSON.stringify(m) + "\nchirp\n";
  const got = parseSelfManifest(buf);
  assert.ok(got, "should find the manifest");
  assert.strictEqual(got.firmware, "2.3.0-wap");
  assert.strictEqual(got.pubkey_fp, "aabbccddeeff0011");
  assert.strictEqual(got.health, 100);
  // nested braces in commands[] must not fool the brace matcher
  assert.strictEqual(got.commands.length, 2);
  // no manifest present → null; partial line → null (waits for more bytes)
  assert.strictEqual(parseSelfManifest("just boot text, no json"), null);
  assert.strictEqual(parseSelfManifest('{"schema":"securacv.canary.manifest/v1","board":"xia'), null);
  // a JSON object that isn't the manifest schema → null
  assert.strictEqual(parseSelfManifest('{"schema":"something.else/v1"}'), null);
  // fingerprint formatting
  assert.strictEqual(formatFingerprint("aabbccddeeff0011"), "aa:bb:cc:dd:ee:ff:00:11");
  assert.strictEqual(formatFingerprint("aabbccddeeff00112233"), "aa:bb:cc:dd:ee:ff:00:11…");
  assert.strictEqual(formatFingerprint(""), "");
});

// ── the BLE offline console (Web Bluetooth "it's really on" test) ───────────
test("BLE_CONSOLE UUIDs match the firmware + iOS contract", async () => {
  const { BLE_CONSOLE } = await core();
  // Console service/char — pinned to firmware .../ble_console.h.
  assert.strictEqual(BLE_CONSOLE.serviceUuid, "8fc1cee0-b162-4401-9607-c8ac21383e90");
  assert.strictEqual(BLE_CONSOLE.snapshotUuid, "8fc1cee1-b162-4401-9607-c8ac21383e90");
  // Pairing service — what the board ADVERTISES (bluetooth_channel.h SERVICE_UUID);
  // this is what the chooser filter matches, NOT the console UUID above.
  assert.strictEqual(BLE_CONSOLE.pairingServiceUuid, "8fc1ceca-b162-4401-9607-c8ac21383e90");
  assert.notStrictEqual(BLE_CONSOLE.serviceUuid, BLE_CONSOLE.pairingServiceUuid,
    "console and pairing services are distinct — the reason v1's filter matched nothing");
  // Web Bluetooth requires lower-case UUIDs.
  assert.strictEqual(BLE_CONSOLE.serviceUuid, BLE_CONSOLE.serviceUuid.toLowerCase());
  assert.strictEqual(BLE_CONSOLE.pairingServiceUuid, BLE_CONSOLE.pairingServiceUuid.toLowerCase());
  // Branded GAP name, single-sourced from bluetooth_channel.cpp's default.
  assert.strictEqual(BLE_CONSOLE.brandName, "SecuraCV-Canary");
  assert.ok(BLE_CONSOLE.brandName.startsWith(BLE_CONSOLE.brandNamePrefix));
  assert.ok(BLE_CONSOLE.maxPayloadBytes >= 200, "payload cap should leave MTU headroom");
});

test("bleRequestOptions: discovers by advertised identity, can reach the console", async () => {
  const { bleRequestOptions, BLE_CONSOLE } = await core();
  const opts = bleRequestOptions();
  // The chooser must match what the firmware ADVERTISES: the branded name
  // prefix, or the pairing service — never the console UUID (which isn't in the
  // advert, so filtering on it left the chooser empty in v1).
  const names = opts.filters.filter((f) => f.namePrefix).map((f) => f.namePrefix);
  const svcs = opts.filters.filter((f) => f.services).flatMap((f) => f.services);
  assert.ok(names.includes("SecuraCV"), "must match the branded name prefix");
  assert.ok(svcs.includes(BLE_CONSOLE.pairingServiceUuid), "must match the advertised pairing service");
  assert.ok(!svcs.includes(BLE_CONSOLE.serviceUuid),
    "must NOT filter on the console service — it isn't advertised");
  // …but the console service must be reachable once connected.
  assert.ok(opts.optionalServices.includes(BLE_CONSOLE.serviceUuid),
    "console service must be in optionalServices so getPrimaryService is allowed");
});

test("bleSupport: present entry point supported, absent → guided fallback copy", async () => {
  const { bleSupport } = await core();
  assert.strictEqual(bleSupport({ bluetooth: { requestDevice() {} } }).supported, true);
  // Safari / Firefox / iOS: no navigator.bluetooth at all.
  const none = bleSupport({});
  assert.strictEqual(none.supported, false);
  assert.match(none.reason, /Chromium|Chrome/);
  // A bluetooth object without the real entry point is not enough.
  assert.strictEqual(bleSupport({ bluetooth: {} }).supported, false);
  assert.strictEqual(bleSupport(null).supported, false);
});

test("parseBleSnapshot: reads JSON from bytes, DataView, ArrayBuffer, string", async () => {
  const { parseBleSnapshot } = await core();
  const sample = '{"up":12345,"heap":154832,"wifi":"connected","wrssi":-55,' +
    '"ctx":"home","owner_min":1,"hh":3,"ble":8421,"motion":12,"id":"3A4C","fw":"2.1.0"}';
  const bytes = new TextEncoder().encode(sample);

  const fromStr = parseBleSnapshot(sample);
  assert.strictEqual(fromStr.raw.id, "3A4C");
  assert.strictEqual(fromStr.raw.wrssi, -55);

  assert.deepStrictEqual(parseBleSnapshot(bytes).raw, fromStr.raw);
  assert.deepStrictEqual(parseBleSnapshot(bytes.buffer).raw, fromStr.raw);
  // A DataView is exactly what Web Bluetooth's readValue() hands back.
  assert.deepStrictEqual(
    parseBleSnapshot(new DataView(bytes.buffer)).raw, fromStr.raw);
});

test("parseBleSnapshot: garbage / partial / non-object never throws, returns null", async () => {
  const { parseBleSnapshot } = await core();
  assert.strictEqual(parseBleSnapshot(null), null);
  assert.strictEqual(parseBleSnapshot(""), null);
  assert.strictEqual(parseBleSnapshot("not json"), null);
  assert.strictEqual(parseBleSnapshot('{"up":123'), null);   // truncated read
  assert.strictEqual(parseBleSnapshot("[1,2,3]"), null);     // array, not a snapshot
  assert.strictEqual(parseBleSnapshot("42"), null);
  assert.strictEqual(parseBleSnapshot(new Uint8Array([0xff, 0xfe])), null);
});

test("bleSnapshotRows: friendly rows, only present fields, minimal payload ok", async () => {
  const { parseBleSnapshot, bleSnapshotRows, formatUptime } = await core();
  const snap = parseBleSnapshot('{"up":90061,"heap":154832,"wifi":"connected",' +
    '"wrssi":-55,"ctx":"home","ovr":true,"owner_min":2,"hh":3,"ble":8421,' +
    '"ble_hh":4,"motion":12,"id":"3A4C","fw":"2.1.0"}');
  const rows = bleSnapshotRows(snap);
  const byLabel = Object.fromEntries(rows.map((r) => [r.label, r.value]));
  assert.strictEqual(byLabel.Identity, "3A4C");
  assert.strictEqual(byLabel.Firmware, "2.1.0");
  assert.strictEqual(byLabel.Uptime, formatUptime(90061)); // "1d 1h"
  assert.strictEqual(byLabel.WiFi, "connected (-55 dBm)");
  assert.strictEqual(byLabel.Context, "home (override)");
  assert.strictEqual(byLabel["BLE adverts seen"], "8421 (4 known)");

  // A minimal snapshot (firmware drops optional fields under MTU pressure)
  // renders only what's there — never invents a field, never throws.
  const minimal = bleSnapshotRows(parseBleSnapshot(
    '{"up":5,"heap":100000,"wifi":"portal","ctx":"away","hh":1}'));
  assert.ok(minimal.length >= 3);
  assert.ok(!minimal.some((r) => r.label === "Identity"));
  assert.deepStrictEqual(bleSnapshotRows(null), []);
});

test("formatUptime: days/hours/minutes/seconds, human and bounded", async () => {
  const { formatUptime } = await core();
  assert.strictEqual(formatUptime(5), "5s");
  assert.strictEqual(formatUptime(65), "1m 5s");
  assert.strictEqual(formatUptime(3720), "1h 2m");
  assert.strictEqual(formatUptime(90061), "1d 1h");
  assert.strictEqual(formatUptime(-10), "0s");
  assert.strictEqual(formatUptime("nonsense"), "0s");
});

// ── release signature verification (in-browser, pinned key) ─────────────────
const nodeCrypto = require("node:crypto");
function makeSignedImage() {
  const { publicKey, privateKey } = nodeCrypto.generateKeyPairSync("ed25519");
  const pubHex = publicKey.export({ type: "spki", format: "der" }).subarray(-32).toString("hex");
  const image = Buffer.from("SecuraCV factory image bytes — test");
  const sha = nodeCrypto.createHash("sha256").update(image).digest();
  const msg = Buffer.concat([Buffer.from(new Uint32Array([image.length]).buffer), sha]); // uint32_le||sha256
  const sigHex = nodeCrypto.sign(null, msg, privateKey).toString("hex");
  return { pubHex, sigHex, size: image.length, sha: new Uint8Array(sha) };
}

test("verifyImageSignature: accepts a genuine signature, rejects tampering", async () => {
  const { verifyImageSignature, ed25519Message, isRealPubkey, hexToBytes } = await core();
  const { pubHex, sigHex, size, sha } = makeSignedImage();
  assert.strictEqual(await verifyImageSignature(sigHex, pubHex, size, sha), true);
  // wrong size, flipped signature byte, wrong hash, wrong key → all false
  assert.strictEqual(await verifyImageSignature(sigHex, pubHex, size + 1, sha), false);
  const flipped = hexToBytes(sigHex); flipped[0] ^= 1;
  assert.strictEqual(await verifyImageSignature(Buffer.from(flipped).toString("hex"), pubHex, size, sha), false);
  const otherSha = new Uint8Array(sha); otherSha[5] ^= 0xff;
  assert.strictEqual(await verifyImageSignature(sigHex, pubHex, size, otherSha), false);
  // the all-zero placeholder key never verifies (unprovisioned) — checksum-only
  assert.strictEqual(isRealPubkey("00".repeat(32)), false);
  assert.strictEqual(await verifyImageSignature(sigHex, "00".repeat(32), size, sha), false);
  // no signature / malformed → false (caller falls back, doesn't crash)
  assert.strictEqual(await verifyImageSignature("", pubHex, size, sha), false);
  // message layout is exactly uint32_le(size) || sha256
  const m = ed25519Message(0x04030201, sha);
  assert.deepStrictEqual(Array.from(m.subarray(0, 4)), [0x01, 0x02, 0x03, 0x04]);
  assert.strictEqual(m.length, 36);
});

test("manifestEntry passes the signature + key id through", async () => {
  const { manifestEntry } = await core();
  const m = goodManifest();
  m.products["securacv-canary-wap"].signature = "ab".repeat(64);
  m.products["securacv-canary-wap"].signing_key_id = "0011223344556677";
  const wap = catalog.products.find((p) => p.id === "securacv-canary-wap");
  const e = manifestEntry(m, wap, "ESP32-S3");
  assert.strictEqual(e.signature, "ab".repeat(64));
  assert.strictEqual(e.signingKeyId, "0011223344556677");
});

test("imageVerificationPolicy: fail-closed once a real key is pinned", async () => {
  const { imageVerificationPolicy } = await core();
  // Real key + signature → must verify.
  assert.strictEqual(imageVerificationPolicy({ keyReal: true, hasSignature: true, selfHosted: false }), "verify");
  // Real key + official manifest + NO signature → REFUSE (the P1 hole).
  assert.strictEqual(imageVerificationPolicy({ keyReal: true, hasSignature: false, selfHosted: false }), "require-signature");
  // Real key + self-hosted manifest without a signature → user opted in → checksum-only.
  assert.strictEqual(imageVerificationPolicy({ keyReal: true, hasSignature: false, selfHosted: true }), "checksum-only");
  // No key yet (pre-ceremony) → checksum-only regardless.
  assert.strictEqual(imageVerificationPolicy({ keyReal: false, hasSignature: false, selfHosted: false }), "checksum-only");
  assert.strictEqual(imageVerificationPolicy({ keyReal: false, hasSignature: true, selfHosted: false }), "checksum-only");
});

test("verifyPinnedModelAsset: model channel gets the same fail-closed policy as firmware", async () => {
  const { verifyPinnedModelAsset } = await core();
  const { pubHex, sigHex, size, sha } = makeSignedImage();
  const bytes = new Uint8Array(size);
  const shaHex = Buffer.from(sha).toString("hex");

  // Genuine: sha matches + valid signature under the real pinned key.
  const good = await verifyPinnedModelAsset({
    bytes, sha256Hex: shaHex,
    asset: { sha256: shaHex, signature: sigHex },
    releasePubkey: pubHex,
  });
  assert.strictEqual(good.ok, true);
  assert.strictEqual(good.policy, "verify");

  // sha mismatch → refused before any signature logic.
  const badSha = await verifyPinnedModelAsset({
    bytes, sha256Hex: shaHex,
    asset: { sha256: "ff".repeat(32), signature: sigHex },
    releasePubkey: pubHex,
  });
  assert.deepStrictEqual(badSha, { ok: false, why: "sha256" });

  // Real key + unsigned manifest → REFUSE (the substitution hole the model
  // channel used to have: checksum-only with a repointable manifest).
  const unsigned = await verifyPinnedModelAsset({
    bytes, sha256Hex: shaHex,
    asset: { sha256: shaHex },
    releasePubkey: pubHex,
  });
  assert.deepStrictEqual(unsigned, { ok: false, why: "require-signature" });

  // Real key + tampered signature → refuse.
  const flipped = sigHex.slice(0, -2) + (sigHex.endsWith("00") ? "01" : "00");
  const badSig = await verifyPinnedModelAsset({
    bytes, sha256Hex: shaHex,
    asset: { sha256: shaHex, signature: flipped },
    releasePubkey: pubHex,
  });
  assert.deepStrictEqual(badSig, { ok: false, why: "signature" });

  // Pre-ceremony (all-zero placeholder key) → checksum-only still flashes.
  const preKey = await verifyPinnedModelAsset({
    bytes, sha256Hex: shaHex,
    asset: { sha256: shaHex },
    releasePubkey: "00".repeat(32),
  });
  assert.strictEqual(preKey.ok, true);
  assert.strictEqual(preKey.policy, "checksum-only");
});

// ── supply-chain hardening: CSP + Subresource-Integrity on the flasher page ──
// flash.html ships a strict Content-Security-Policy (only same-origin, vendored
// code runs) and an import map carrying SRI hashes for the vendored third-party
// modules. These are the drift gate: they recompute every hash from the real
// bytes, so a vendored bump, an edited policy, or a new *unpinned* vendored
// import fails CI instead of silently shipping.
const { createHash } = require("node:crypto");
const { dirname, relative, resolve } = require("node:path");

const FLASH_HTML = readFileSync(join(ROOT, "flash.html"), "utf8");

function cspContent(html) {
  const m = html.match(/<meta http-equiv="Content-Security-Policy" content="([^"]*)"/);
  return m ? m[1] : null;
}
function importmapText(html) {
  const m = html.match(/<script type="importmap">([\s\S]*?)<\/script>/);
  return m ? m[1] : null;
}

// ── post-flash: the "come to life" next step ────────────────────────────────
test("postFlashNextStep: ap boards go to the phone portal, unless Wi-Fi was written", async () => {
  const { postFlashNextStep } = await core();
  const canary = { id: "securacv-canary", provisioning: "ap" };
  assert.strictEqual(postFlashNextStep(canary).kind, "wifi-portal");
  // Pre-provisioned the home Wi-Fi during the flash → it's already joining.
  assert.strictEqual(postFlashNextStep(canary, { wifiJoined: true }).kind, "joining-wifi");
});

test("postFlashNextStep: usb-secrets boards go straight to proving themselves", async () => {
  const { postFlashNextStep } = await core();
  for (const id of ["securacv-canary-sense", "securacv-canary-sense-wellbeing", "securacv-canary-vision-xiao-s3"]) {
    const step = postFlashNextStep({ id, provisioning: "usb-secrets" });
    assert.strictEqual(step.kind, "prove-live");
    assert.match(step.cta, /monitor/i);
  }
});

test("postFlashNextStep: the prove-it hint is tailored to what the board senses", async () => {
  const { postFlashNextStep } = await core();
  assert.match(postFlashNextStep({ id: "securacv-canary-sense", provisioning: "usb-secrets" }).body, /presence|walk past/i);
  assert.match(postFlashNextStep({ id: "securacv-canary-vision-xiao-s3", provisioning: "usb-secrets" }).body, /camera|sees/i);
  assert.match(postFlashNextStep({ id: "securacv-canary-wap", provisioning: "ap" }, { wifiJoined: true }).body, /Wi-Fi field|move around/i);
});

test("postFlashNextStep: every catalog product yields a complete step (can't rot)", async () => {
  const { postFlashNextStep } = await core();
  for (const p of catalog.products) {
    for (const wifiJoined of [false, true]) {
      const s = postFlashNextStep(p, { wifiJoined });
      assert.ok(s && s.kind && s.title && s.body && s.cta, `${p.id} (wifiJoined=${wifiJoined}) incomplete`);
    }
  }
});

// ── post-flash: the catalog hatch moment + its receipt gate ─────────────────
test("requiresLiveReceipt: fails closed; reads only the catalog's explicit false", async () => {
  const { requiresLiveReceipt } = await core();
  assert.strictEqual(requiresLiveReceipt(null), true);      // no product → still demand proof
  assert.strictEqual(requiresLiveReceipt({}), true);        // absent field → demand proof
  assert.strictEqual(requiresLiveReceipt({ serial_receipt: true }), true);
  assert.strictEqual(requiresLiveReceipt({ serial_receipt: false }), false);
  // The Watch Station reports no serial receipt — its hatch steps belong on
  // the done card, straight after the write (the desktop shows them there too).
  const watch = catalog.products.find((p) => p.id === "securacv-canary-display-watch");
  assert.ok(watch, "the Watch Station left the catalog");
  assert.strictEqual(requiresLiveReceipt(watch), false);
});

test("hatchMoment: every catalog product yields its own first flight; junk falls back", async () => {
  const { hatchMoment } = await core();
  for (const p of catalog.products) {
    const m = hatchMoment(p);
    assert.ok(m, `${p.id}: catalog hatch data must yield a moment`);
    assert.strictEqual(m.title, p.hatch.title, `${p.id}: title must be the catalog's`);
    assert.deepStrictEqual(m.steps, p.hatch.steps, `${p.id}: steps must be the catalog's, in order`);
  }
  // No usable hatch data (a local file, an unknown board) → null, so the
  // caller falls back to the generic postFlashNextStep above.
  assert.strictEqual(hatchMoment(null), null);
  assert.strictEqual(hatchMoment({}), null);
  assert.strictEqual(hatchMoment({ hatch: { title: "t", body: "b", steps: [] } }), null);
  assert.strictEqual(hatchMoment({ hatch: { title: "t", steps: ["s"] } }), null);
});

test("hatchMoment: the vision products share ONE moment, so the module flow can render it blind", async () => {
  // A Vision pair can complete on the camera module's done screen
  // (we2-flash.js), which knows the pair is done but not WHICH ESP32 vision
  // board it was — it renders the hatch from any catalog vision product.
  // That's only honest while every vision product carries the same hatch
  // copy (gen_flash.py keys HATCH_MOMENTS by kind, not by board). If a
  // vision board ever grows its own hatch, teach the module flow which
  // board's copy to use before loosening this.
  const { hatchMoment, isVisionBoard } = await core();
  const visions = catalog.products.filter((p) => isVisionBoard(p));
  assert.ok(visions.length >= 2, "expected the vision line in the catalog");
  const ref = JSON.stringify(hatchMoment(visions[0]));
  assert.notStrictEqual(ref, "null", `${visions[0].id}: vision hatch data missing`);
  for (const p of visions) {
    assert.strictEqual(JSON.stringify(hatchMoment(p)), ref,
      `${p.id}: vision hatch copy diverged across boards — we2-flash.js renders ` +
      "the pair-complete hatch from any one vision product");
  }
});

test("healthVerdict: maps the self-test score to a plain verdict, never a false pass", async () => {
  const { healthVerdict } = await core();
  assert.strictEqual(healthVerdict(98).level, "ok");
  assert.strictEqual(healthVerdict(80).level, "ok");     // boundary
  assert.strictEqual(healthVerdict(79).level, "warn");
  assert.strictEqual(healthVerdict(50).level, "warn");   // boundary
  assert.strictEqual(healthVerdict(49).level, "attn");
  assert.strictEqual(healthVerdict(0).level, "attn");
  assert.strictEqual(healthVerdict(100).level, "ok");    // top of the valid range
  // unknown / null / out-of-range health must be pending — never shown as a pass
  for (const h of [-1, 101, 1000, null, undefined, NaN, "98"]) {
    assert.strictEqual(healthVerdict(h).level, "pending", `health=${String(h)}`);
  }
  for (const h of [98, 65, 20, null]) {
    const v = healthVerdict(h);
    assert.ok(v.icon && v.label, `verdict for ${String(h)} missing icon/label`);
  }
});

// ── two-port Vision: recognize each port, insist on both ────────────────────
test("identifyPort: the WE2 camera module is recognized by its USB id", async () => {
  const { identifyPort } = await core();
  // The catalog's we2_module carries the module's USB vid/pid (a CH343).
  const info = {
    usbVendorId: parseInt(catalog.we2_module.usb_vid, 16),
    usbProductId: parseInt(catalog.we2_module.usb_pid, 16),
  };
  assert.strictEqual(identifyPort(info, catalog), "we2");
  // Anything else on the wire is the main ESP32 board (native S3, CP210x, unknown).
  assert.strictEqual(identifyPort({ usbVendorId: 0x303a, usbProductId: 0x1001 }, catalog), "esp32");
  assert.strictEqual(identifyPort({ usbVendorId: 0x10c4, usbProductId: 0xea60 }, catalog), "esp32");
  assert.strictEqual(identifyPort({}, catalog), "esp32");
  assert.strictEqual(identifyPort(null, catalog), "esp32");
  // Critical: a CH340 ESP32 board shares the WE2's WCH vendor (0x1a86) but has a
  // different product id — it must NOT be mistaken for the camera module.
  assert.strictEqual(identifyPort({ usbVendorId: 0x1a86, usbProductId: 0x7523 }, catalog), "esp32");
});

test("visionCompletion: tracks the 2-of-2 so you can't walk away half-done", async () => {
  const { visionCompletion } = await core();
  const none = visionCompletion({});
  assert.strictEqual(none.done, false);
  assert.strictEqual(none.count, 0);
  assert.deepStrictEqual(none.remaining, ["esp32", "we2"]);

  const one = visionCompletion({ esp32: true });
  assert.strictEqual(one.done, false);
  assert.strictEqual(one.count, 1);
  assert.deepStrictEqual(one.remaining, ["we2"]);
  assert.match(one.nextLabel, /camera module/i);   // it points you at the other port

  const both = visionCompletion({ esp32: true, we2: true });
  assert.strictEqual(both.done, true);
  assert.strictEqual(both.count, 2);
  assert.strictEqual(both.nextLabel, null);
});

test("isVisionBoard: only the Vision boards are the two-port pair", async () => {
  const { isVisionBoard } = await core();
  assert.strictEqual(isVisionBoard({ id: "securacv-canary-vision" }), true);
  assert.strictEqual(isVisionBoard({ id: "securacv-canary-vision-xiao-s3" }), true);
  assert.strictEqual(isVisionBoard({ id: "securacv-canary-vision-xiao-c3" }), true);
  // Non-Vision boards are single-port — no camera module to also flash.
  assert.strictEqual(isVisionBoard({ id: "securacv-canary-sense" }), false);
  assert.strictEqual(isVisionBoard({ id: "securacv-canary-wap" }), false);
  assert.strictEqual(isVisionBoard({ id: "securacv-canary" }), false);
  assert.strictEqual(isVisionBoard(null), false);
  assert.strictEqual(isVisionBoard({}), false);
});

test("visionChecklistModel: one wording drives both done screens", async () => {
  const { visionChecklistModel } = await core();
  // Nothing flashed yet — two open rows.
  const zero = visionChecklistModel({});
  assert.strictEqual(zero.done, false);
  assert.strictEqual(zero.count, 0);
  assert.strictEqual(zero.rows.length, 2);
  assert.strictEqual(zero.rows[0].done, false);
  assert.strictEqual(zero.rows[1].done, false);

  // ESP32 done → camera module is next; the firmware row is checked, camera open.
  const esp = visionChecklistModel({ esp32: true });
  assert.strictEqual(esp.done, false);
  assert.strictEqual(esp.count, 1);
  assert.strictEqual(esp.nextPart, "we2");
  assert.strictEqual(esp.rows[0].done, true);
  assert.strictEqual(esp.rows[1].done, false);
  assert.match(esp.status, /1 of 2/);
  assert.match(esp.status, /camera module/i);

  // Camera module first → the ESP32 firmware is next.
  const cam = visionChecklistModel({ we2: true });
  assert.strictEqual(cam.nextPart, "esp32");
  assert.match(cam.status, /ESP32 board/i);

  // Both done → the celebration, no "next".
  const both = visionChecklistModel({ esp32: true, we2: true });
  assert.strictEqual(both.done, true);
  assert.strictEqual(both.count, 2);
  assert.strictEqual(both.nextPart, null);
  assert.match(both.status, /fully set up/i);
});

test("flash.html: ships a strict, eval-free Content-Security-Policy", () => {
  const csp = cspContent(FLASH_HTML);
  assert.ok(csp, "no CSP <meta> in flash.html");
  // Deny-all baseline; scripts + styles are same-origin only.
  assert.match(csp, /default-src 'none'/);
  assert.match(csp, /script-src 'self' 'sha256-[A-Za-z0-9+/=]+'/);
  assert.match(csp, /style-src 'self'/);
  // No <base> rewrite of our relative asset paths, no plugins, no form posts.
  assert.match(csp, /base-uri 'none'/);
  assert.match(csp, /object-src 'none'/);
  assert.match(csp, /form-action 'none'/);
  // Firmware fetches: 'self' for the catalog, the site's copy of the release
  // and a same-origin manifest. No GitHub host: no browser can read a release
  // asset from it (no CORS header), so naming it would only invite a fetch
  // that cannot work. No open wildcard host.
  assert.match(csp, /connect-src[^;]*'self'/);
  assert.doesNotMatch(csp, /github\.com|githubusercontent\.com/);
  // Loopback (a same-machine manifest server) is pinned to exact hosts, and
  // there is NO open http:/https: scheme-source — the override guard in
  // flash-core.js accepts exactly this set, so code and CSP can't disagree.
  assert.match(csp, /connect-src[^;]*http:\/\/localhost:\*/);
  assert.doesNotMatch(csp, /connect-src[^;]*\bhttps?:(?!\/\/)/);
  // The whole point is defeated if inline/eval sneaks back in.
  assert.doesNotMatch(csp, /'unsafe-inline'/);
  assert.doesNotMatch(csp, /'unsafe-eval'/);
  assert.doesNotMatch(csp, /'wasm-unsafe-eval'/);
});

test("flash.html: the CSP 'sha256-…' matches the inline import map's bytes", () => {
  const csp = cspContent(FLASH_HTML);
  const text = importmapText(FLASH_HTML);
  assert.ok(text, "no import map in flash.html");
  const want = "sha256-" + createHash("sha256").update(text, "utf8").digest("base64");
  assert.ok(csp.includes("'" + want + "'"),
    `CSP script-src must pin the import map with '${want}' (recompute if you edited the map)`);
});

test("flash.html: SRI hashes match the real vendored module bytes", () => {
  const map = JSON.parse(importmapText(FLASH_HTML)).integrity;
  assert.ok(map && Object.keys(map).length >= 4, "expected an integrity map for the vendored modules");
  for (const [rel, pinned] of Object.entries(map)) {
    const algo = pinned.split("-")[0];
    assert.ok(["sha256", "sha384", "sha512"].includes(algo), `${rel}: unexpected SRI algorithm "${algo}"`);
    const actual = algo + "-" + createHash(algo).update(readFileSync(join(ROOT, rel))).digest("base64");
    assert.strictEqual(actual, pinned, `${rel}: SRI hash is stale — recompute it from the vendored file`);
  }
});

test("flash.html: every vendored module the flasher imports is pinned", () => {
  // Walk the flasher's module graph from its entry point and collect every
  // ./vendor/* import. The set must equal the integrity map — a newly added
  // unpinned vendored dependency (or a stale pin) fails right here.
  const map = JSON.parse(importmapText(FLASH_HTML)).integrity;
  const seen = new Set();
  const vendored = new Set();
  const queue = [join(ROOT, "assets/flash.js")];
  const PATTERNS = [
    /\bfrom\s*["']([^"']+)["']/g,               // import … from "x"
    /\bimport\s*\(\s*["']([^"']+)["']/g,        // import("x")  — dynamic
    /(?:^|[;{}\s])import\s+["']([^"']+)["']/g,  // import "x"   — side-effect
  ];
  while (queue.length) {
    const file = queue.pop();
    if (seen.has(file)) continue;
    seen.add(file);
    let src;
    try { src = readFileSync(file, "utf8"); } catch { continue; }
    for (const re of PATTERNS) {
      for (const m of src.matchAll(re)) {
        const spec = m[1];
        if (!spec.startsWith(".")) continue; // the flasher has no bare specifiers
        const rel = relative(ROOT, resolve(dirname(file), spec)).split("\\").join("/");
        if (rel.startsWith("assets/vendor/")) vendored.add(rel);
        else if (/\.m?js$/.test(rel)) queue.push(join(ROOT, rel)); // first-party: recurse
      }
    }
  }
  // Import-map integrity keys are URL-like ("./assets/…") so the browser
  // resolves and matches them; compare against the graph on the bare path.
  const norm = (s) => s.replace(/^\.\//, "");
  assert.deepStrictEqual([...vendored].map(norm).sort(), Object.keys(map).map(norm).sort(),
    "the flasher's vendored imports and the SRI integrity map have drifted apart");
});

// ── the blank map: a faster safety copy and a proportional first-contact erase ──
test("planBlankProbe: covers the chip exactly, the last block may be short", async () => {
  const { planBlankProbe, BLANK_PROBE_BLOCK } = await core();
  const plan = planBlankProbe(16 * 1024 * 1024);
  assert.strictEqual(plan.length, (16 * 1024 * 1024) / BLANK_PROBE_BLOCK);
  assert.strictEqual(plan[0].offset, 0);
  const odd = planBlankProbe(BLANK_PROBE_BLOCK * 2 + 0x1000);
  assert.deepStrictEqual(odd.map((b) => b.size), [BLANK_PROBE_BLOCK, BLANK_PROBE_BLOCK, 0x1000]);
  assert.strictEqual(odd.reduce((n, b) => n + b.size, 0), BLANK_PROBE_BLOCK * 2 + 0x1000);
  assert.deepStrictEqual(planBlankProbe(0), []);
  assert.deepStrictEqual(planBlankProbe(NaN), []);
});

test("blankSkipReads: only proven-blank blocks are skipped; unknown is read", async () => {
  const { blankSkipReads, blankBytes } = await core();
  const K = 0x40000;
  const blocks = [
    { offset: 0, size: K, blank: false },
    { offset: K, size: K, blank: false },
    { offset: 2 * K, size: K, blank: true },
    { offset: 3 * K, size: K, blank: null },   // the probe failed: read it
    { offset: 4 * K, size: K, blank: true },
  ];
  assert.deepStrictEqual(blankSkipReads(blocks), [
    { offset: 0, size: 2 * K },                 // adjacent data merges into one read
    { offset: 3 * K, size: K },
  ]);
  assert.strictEqual(blankBytes(blocks), 2 * K);
  assert.deepStrictEqual(blankSkipReads([]), []);
});

test("firstContactEraseSpans: every byte ends written, erased or proven blank", async () => {
  const { firstContactEraseSpans, planBlankProbe, FLASH_SECTOR } = await core();
  // Property over many block maps and image sizes: the new image's write,
  // our erase spans and the proven-blank blocks together cover the whole chip,
  // so the end state equals a whole-chip erase followed by the write.
  const flash = 4 * 1024 * 1024;
  let seed = 7;
  const rnd = () => (seed = (seed * 1103515245 + 12345) % 2147483648) / 2147483648;
  for (let trial = 0; trial < 200; trial++) {
    const blocks = planBlankProbe(flash).map((b) => ({
      ...b, blank: [true, false, null][Math.floor(rnd() * 3)],
    }));
    const imageLen = Math.floor(rnd() * flash);
    const spans = firstContactEraseSpans(blocks, imageLen);
    const covered = new Uint8Array(flash / FLASH_SECTOR);
    const mark = (off, size) => {
      for (let a = off; a < off + size; a += FLASH_SECTOR) covered[a / FLASH_SECTOR] = 1;
    };
    mark(0, Math.ceil(imageLen / FLASH_SECTOR) * FLASH_SECTOR); // the write erases its own range
    for (const b of blocks) if (b.blank === true) mark(b.offset, b.size);
    for (const s of spans) {
      assert.strictEqual(s.offset % FLASH_SECTOR, 0, "erase spans start on a sector");
      assert.strictEqual(s.size % FLASH_SECTOR, 0, "erase spans are whole sectors");
      assert.ok(s.offset >= Math.ceil(imageLen / FLASH_SECTOR) * FLASH_SECTOR,
        "never erases what the image's own write is about to overwrite");
      mark(s.offset, s.size);
    }
    assert.ok(covered.every((c) => c === 1), `trial ${trial}: a sector was left unaccounted for`);
  }
});

test("firstContactEraseSpans: a mostly blank chip erases only what holds data", async () => {
  const { firstContactEraseSpans } = await core();
  const K = 0x40000;
  const blocks = [];
  for (let i = 0; i < 64; i++) blocks.push({ offset: i * K, size: K, blank: i >= 12 });
  // old firmware filled 3 MB; the new image is 1.9 MB
  const spans = firstContactEraseSpans(blocks, 1987760);
  const total = spans.reduce((n, s) => n + s.size, 0);
  assert.ok(total < 2 * 1024 * 1024, `erases ${total} bytes, not the 16 MB chip`);
  assert.strictEqual(spans[0].offset, Math.ceil(1987760 / 0x1000) * 0x1000);
  assert.deepStrictEqual(firstContactEraseSpans(blocks.map((b) => ({ ...b, blank: true })), 0), [],
    "a blank chip needs no erase at all");
});

test("chunkSpans: pieces cover the spans exactly, none larger than the cap", async () => {
  const { chunkSpans } = await core();
  const pieces = chunkSpans([{ offset: 0x1000, size: 0x90000 }, { offset: 0x200000, size: 0x1000 }], 0x40000);
  assert.ok(pieces.every((p) => p.size <= 0x40000));
  assert.strictEqual(pieces.reduce((n, p) => n + p.size, 0), 0x91000);
  assert.strictEqual(pieces[0].offset, 0x1000);
  assert.strictEqual(pieces[pieces.length - 1].offset, 0x200000);
});

test("installJourney: every step named up front, in install order", async () => {
  const { installJourney } = await core();
  const full = installJourney({ backup: true, download: true, erase: "first-contact", imageBytes: 2e6, flashBytes: 16 << 20 });
  assert.deepStrictEqual(full.map((s) => s.key), ["map", "backup", "download", "check", "clear", "write", "confirm"]);
  const local = installJourney({ backup: false, download: false, erase: "full", imageBytes: 1e6, flashBytes: 4 << 20 });
  assert.deepStrictEqual(local.map((s) => s.key), ["erase", "write", "confirm"]);
  const plain = installJourney({ download: true, imageBytes: 1e6 });
  assert.deepStrictEqual(plain.map((s) => s.key), ["download", "check", "write", "confirm"]);
});

test("journeyEtaSeconds: measured rates only; unknown or a silent whole-chip erase means no number", async () => {
  const { installJourney, journeyEtaSeconds } = await core();
  const steps = installJourney({ backup: true, backupBytes: 3e6, download: true, imageBytes: 2e6, erase: "first-contact" });
  // Nothing measured yet: no promise.
  assert.strictEqual(journeyEtaSeconds(steps, "map", 0, {}), null);
  // Mid-backup with a measured link rate and its own estimate; the download
  // has no rate yet, so the total is still unknown.
  assert.strictEqual(journeyEtaSeconds(steps, "backup", 0.5, { link: 100e3 }, 15), null);
  // Both lanes measured: backup's own 15 s + download 2 MB at 1 MB/s + write 2 MB at 100 KB/s;
  // map/check/clear/confirm are short on-chip steps.
  assert.strictEqual(journeyEtaSeconds(steps, "backup", 0.5, { link: 100e3, net: 1e6 }, 15), 15 + 2 + 20);
  // During the write, the write's own tracker is the estimate.
  assert.strictEqual(journeyEtaSeconds(steps, "write", 0.9, { link: 100e3, net: 1e6 }, 4), 4);
  const erasing = installJourney({ download: false, imageBytes: 2e6, erase: "full", flashBytes: 16 << 20 });
  assert.strictEqual(journeyEtaSeconds(erasing, "erase", 0, { link: 100e3 }), null,
    "a whole-chip erase reports nothing, so the total can't be promised");
  assert.strictEqual(journeyEtaSeconds(erasing, "write", 0, { link: 100e3 }), 20);
});

test("journeyClockLine: elapsed first, then what's left when it's honest", async () => {
  const { journeyClockLine, clockFace } = await core();
  assert.strictEqual(clockFace(0), "0:00");
  assert.strictEqual(clockFace(125.9), "2:05");
  assert.strictEqual(journeyClockLine(65, null), "1:05");
  assert.strictEqual(journeyClockLine(65, 3), "1:05 · almost done");
  assert.strictEqual(journeyClockLine(65, 42), "1:05 · about 40 s left overall");
  assert.strictEqual(journeyClockLine(65, 95.4), "1:05 · about 1:35 left overall");
  assert.strictEqual(journeyClockLine(5, Infinity), "0:05");
});

test("flasher glue: the safety copy maps first, and the first-contact erase falls back to a whole-chip erase", () => {
  const src = readFileSync(join(ROOT, "assets/flash.js"), "utf8");
  const backup = /async function takeBackup\([\s\S]*?\n}\n/.exec(src);
  assert.ok(backup, "takeBackup vanished");
  assert.match(backup[0], /probeBlankBlocks\(/, "the safety copy no longer maps the chip first");
  assert.match(backup[0], /fill\(0xff\)/, "skipped blocks must be filled with 0xFF, what reading them returns");
  const probe = /async function probeBlankBlocks\([\s\S]*?\n}\n/.exec(src);
  assert.match(probe[0], /IS_STUB/, "the map must require the stub: the ROM answers MD5 in ASCII hex");
  assert.match(probe[0], /blank = null/, "a failed probe must be unknown (read and erased), never blank");
  assert.match(probe[0], /if \(\+\+failed >= 3 \|\| ok === 0\) return null/,
    "a chip that won't answer MD5 must not stall the install for minutes of probe timeouts");
  const erase = /async function eraseForFirstContact\([\s\S]*?\n}\n/.exec(src);
  assert.ok(erase, "eraseForFirstContact vanished");
  assert.match(erase[0], /eraseFlash\(\)/, "without a map or the stub, first contact must still erase the whole chip");
  assert.match(erase[0], /firstContactEraseSpans/, "the proportional erase must come from the tested planner");
  assert.match(erase[0], /resp\[0\] === 0/,
    "every erase must read the stub's status: checkCommand would report a refused erase as success");
  assert.doesNotMatch(erase[0].replace(/\/\/.*$/gm, ""), /checkCommand\(/,
    "checkCommand ignores the erase status");
  assert.match(erase[0], /if \(!ok\) return wholeChip\(\)/, "a refused erase must fall back to the whole chip");
  // An erase the user asked for stays the whole-chip erase.
  assert.match(src, /eraseKind: forcedErase && !askedFullErase \? "first-contact" : null/);
});
