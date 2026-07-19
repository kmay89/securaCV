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
    assert.ok(catalog.chips[p.chip].download_mode, `${p.chip}: no download-mode copy`);
  }
});

test("flash.json: fw_train is single-sourced from the registry", () => {
  assert.strictEqual(catalog.fw_train, registry.fw_train);
});

test("flash.json: the no-brick promise and recovery ladder are present", () => {
  assert.ok(catalog.no_brick && catalog.no_brick.headline && catalog.no_brick.points.length);
  assert.ok(Array.isArray(catalog.recovery) && catalog.recovery.length >= 2);
  assert.strictEqual(catalog.manifest_url.includes("releases/latest/download"), true);
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

test("manifestEntry refuses a chip-mismatched image (defence in depth)", async () => {
  const { manifestEntry } = await core();
  const m = goodManifest();
  const wap = catalog.products.find((p) => p.id === "securacv-canary-wap");
  const ok = manifestEntry(m, wap, "ESP32-S3");
  assert.ok(ok && !ok.error && ok.sha256 === "a".repeat(64));
  const mism = manifestEntry(m, wap, "ESP32-C3"); // S3 image, C3 in hand
  assert.ok(mism && mism.error && /mismatch/i.test(mism.error));
  assert.strictEqual(manifestEntry(m, { id: "not-in-manifest" }, "ESP32-S3"), null);
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

test("formatters", async () => {
  const { formatBytes, formatMac } = await core();
  assert.strictEqual(formatBytes(512), "512 B");
  assert.strictEqual(formatBytes(2048), "2 KB");
  assert.strictEqual(formatBytes(1572864), "1.50 MB");
  assert.strictEqual(formatMac("a1:b2:c3:d4:e5:f6"), "A1:B2:C3:D4:E5:F6");
});
