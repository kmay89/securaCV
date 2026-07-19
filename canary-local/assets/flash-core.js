// canary-local/assets/flash-core.js — the browser flasher's DOM-free core.
//
// Everything here is pure and testable (tests/flash.test.js runs it under
// `node --test`, the repo convention): the chip guard, the ESP32 image
// parsers that let the page read what firmware is *currently* on a board,
// the release-manifest resolution, and the byte<->binary-string glue
// esptool-js needs. No DOM, no Web Serial, no esptool import — flash.js owns
// all of that and imports this.

// ── chip guard ───────────────────────────────────────────────────────────
// esptool reports chips as "ESP32-S3", "ESP32-C3", "ESP32-C6", "ESP32", …;
// flash.json uses the same strings. Match forgivingly (case, spacing) so a
// board is only ever shown images built for its own silicon.

export function normalizeChip(name) {
  // Fold case, spaces, and hyphens so "ESP32-S3" / "esp32 s3" / "ESP32S3"
  // all compare equal — while ESP32 vs ESP32-S3 vs ESP32-C3 stay distinct.
  return String(name || "").trim().toUpperCase().replace(/[\s-]+/g, "");
}

export function chipMatches(detected, product) {
  return normalizeChip(detected) === normalizeChip(product && product.chip);
}

// Products flashable onto the detected chip, in catalog order.
export function productsForChip(catalog, detected) {
  const list = (catalog && catalog.products) || [];
  return list.filter((p) => chipMatches(detected, p));
}

export function chipInfo(catalog, detected) {
  const chips = (catalog && catalog.chips) || {};
  const key = Object.keys(chips).find((c) => chipMatches(detected, { chip: c }));
  return key ? chips[key] : null;
}

// ── little-endian readers ─────────────────────────────────────────────────
export function u16le(b, o) {
  return b[o] | (b[o + 1] << 8);
}
export function u32le(b, o) {
  // >>> 0 keeps it an unsigned 32-bit value.
  return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
}

// A NUL-terminated ASCII string out of a fixed field; printable chars only,
// so garbage flash never renders as control codes in the UI.
export function cstr(b, off, len) {
  let s = "";
  for (let i = 0; i < len; i++) {
    const c = b[off + i];
    if (c === 0 || c === undefined) break;
    if (c >= 0x20 && c < 0x7f) s += String.fromCharCode(c);
  }
  return s;
}

// ── ESP32 partition table (flashed at 0x8000) ─────────────────────────────
// 32-byte entries: magic 0xAA50 (LE 0x50AA in byte order 0xAA,0x50),
// type(u8) subtype(u8) offset(u32) size(u32) label[16] flags(u32).
export const PARTITION_MAGIC = 0x50aa; // bytes 0xAA,0x50 → u16le
const APP_SUBTYPE = {
  0x00: "factory",
  0x10: "ota_0",
  0x11: "ota_1",
  0x12: "ota_2",
  0x20: "test",
};

export function parsePartitionTable(bytes) {
  const entries = [];
  for (let o = 0; o + 32 <= bytes.length; o += 32) {
    const magic = u16le(bytes, o);
    if (magic !== PARTITION_MAGIC) break; // 0xFFFF padding or MD5 marker → done
    const type = bytes[o + 2];
    const subtype = bytes[o + 3];
    const offset = u32le(bytes, o + 4);
    const size = u32le(bytes, o + 8);
    const label = cstr(bytes, o + 12, 16);
    entries.push({ type, subtype, offset, size, label });
  }
  const apps = entries.filter((e) => e.type === 0x00);
  return { entries, apps };
}

// Which app partition to read a version out of: prefer ota_0, then factory,
// then whatever app comes first. (Reading otadata to know the *live* slot is
// deliberately out of scope — for a first-flash tool "there's canary-wap
// ~2.1.0 on here" is the honest, useful signal, and it never blocks flashing.)
export function pickAppPartition(apps) {
  if (!apps || !apps.length) return null;
  const byName = (n) => apps.find((a) => APP_SUBTYPE[a.subtype] === n);
  return byName("ota_0") || byName("factory") || apps[0];
}

// ── esp_app_desc_t (256 bytes, at app_offset + 0x20) ──────────────────────
// magic(u32)=0xABCD5432, secure_ver(u32), reserv(u32*2), version[32],
// project_name[32], time[16], date[16], idf_ver[32], sha256[32], …
export const APP_DESC_MAGIC = 0xabcd5432;
export const APP_DESC_OFFSET = 0x20;

export function parseAppDescriptor(bytes) {
  if (!bytes || bytes.length < 128) return null;
  if (u32le(bytes, 0) !== APP_DESC_MAGIC) return null;
  return {
    version: cstr(bytes, 16, 32),
    projectName: cstr(bytes, 48, 32),
    time: cstr(bytes, 80, 16),
    date: cstr(bytes, 96, 16),
    idfVer: cstr(bytes, 112, 32),
  };
}

// Map a firmware image's compiled project_name back to a catalog product, so
// a read-back can say "this is a Canary WAP" and not just a version string.
// project_name is set per firmware; match by asset_stem/id fragments.
export function matchProjectToProduct(catalog, projectName) {
  const pn = String(projectName || "").toLowerCase();
  if (!pn) return null;
  const list = (catalog && catalog.products) || [];
  // Longest asset_stem match wins (canary-vision-xiao-s3 before canary).
  let best = null;
  for (const p of list) {
    const stem = String(p.asset_stem).toLowerCase();
    const bare = stem.replace(/-/g, "_");
    if (pn.includes(stem) || pn.includes(bare)) {
      if (!best || stem.length > String(best.asset_stem).length) best = p;
    }
  }
  return best;
}

// ── release manifest (manifest-flash.json, published by CI) ───────────────
// { schema, fw_train, release_url, products: { <id>: {version, chipFamily,
//   factory, sha256, size} } }. The flasher trusts the catalog for the chip
// guard and this for the actual bytes.
export const MANIFEST_SCHEMA = "securacv-flash-1";

export function validateManifest(m) {
  const errs = [];
  if (!m || typeof m !== "object") return ["manifest is not an object"];
  if (m.schema !== MANIFEST_SCHEMA)
    errs.push(`unexpected schema "${m.schema}" (want "${MANIFEST_SCHEMA}")`);
  if (!m.products || typeof m.products !== "object")
    errs.push("manifest has no products map");
  else {
    for (const [id, e] of Object.entries(m.products)) {
      if (!e || typeof e !== "object") { errs.push(`${id}: not an object`); continue; }
      if (!e.factory) errs.push(`${id}: missing factory url`);
      if (!/^[0-9a-f]{64}$/i.test(e.sha256 || "")) errs.push(`${id}: bad sha256`);
      if (!(e.size > 0)) errs.push(`${id}: bad size`);
      if (!e.chipFamily) errs.push(`${id}: missing chipFamily`);
    }
  }
  return errs;
}

// Resolve the release entry for a product, guarding that the manifest's
// chipFamily agrees with the physically-detected chip (defence in depth: the
// catalog already filtered by chip; this refuses a manifest that contradicts
// the silicon in hand).
export function manifestEntry(manifest, product, detected) {
  if (!manifest || !manifest.products || !product) return null;
  const e = manifest.products[product.id];
  if (!e) return null;
  if (detected && !chipMatches(detected, { chip: e.chipFamily })) {
    return { error: `manifest offers ${e.chipFamily} for ${product.id}, but a ` +
      `${detected} is connected — refusing (chip mismatch)` };
  }
  return { version: e.version, factory: e.factory, sha256: String(e.sha256 || "").toLowerCase(),
    size: e.size, chipFamily: e.chipFamily, releaseNotes: e.release_notes, product };
}

// Resolve the `?manifest=<url>` override for self-hosted / air-gapped use.
// Security: an unrestricted override on the public Lab would be a
// firmware-phishing vector (a crafted link pointing the flasher at a hostile
// manifest+image). So we accept only a **same-origin** manifest, or one on a
// **private/LAN/localhost** host — exactly the hosts the OTA engine trusts for
// plain-HTTP local update servers (docs/firmware_ota.md § transport policy).
// Anything else returns null and the flasher falls back to the signed release.
export function manifestOverrideUrl(search, pageOrigin) {
  let raw;
  try { raw = new URLSearchParams(search || "").get("manifest"); } catch { return null; }
  if (!raw) return null;
  let u;
  try { u = new URL(raw, pageOrigin); } catch { return null; }
  if (pageOrigin && u.origin === pageOrigin) return u.href; // same-origin: always fine
  const host = u.hostname.toLowerCase();
  const localName = host === "localhost" ||
    /\.(local|lan|internal|home\.arpa)$/.test(host);
  const privateIp =
    /^127\./.test(host) || /^10\./.test(host) || /^192\.168\./.test(host) ||
    /^169\.254\./.test(host) || /^172\.(1[6-9]|2\d|3[01])\./.test(host);
  return (localName || privateIp) ? u.href : null;
}

// ── esptool-js byte glue ──────────────────────────────────────────────────
// writeFlash wants each file's `data` as a *binary string* (one char per
// byte); readFlash hands back a Uint8Array. Keep both conversions here, pure.
export function bytesToBinaryString(bytes) {
  let s = "";
  const CHUNK = 0x8000; // avoid arg-count blowups on String.fromCharCode
  for (let i = 0; i < bytes.length; i += CHUNK) {
    s += String.fromCharCode.apply(null, bytes.subarray(i, i + CHUNK));
  }
  return s;
}

export function binaryStringToBytes(str) {
  const out = new Uint8Array(str.length);
  for (let i = 0; i < str.length; i++) out[i] = str.charCodeAt(i) & 0xff;
  return out;
}

export function hex(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += bytes[i].toString(16).padStart(2, "0");
  return s;
}

// ── formatting ─────────────────────────────────────────────────────────────
export function formatBytes(n) {
  if (!Number.isFinite(n)) return "—";
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(0)} KB`;
  return `${(n / (1024 * 1024)).toFixed(2)} MB`;
}

// esptool's readMac returns "aa:bb:cc:dd:ee:ff"; normalize to upper for display.
export function formatMac(mac) {
  return String(mac || "").trim().toUpperCase();
}

// Human ETA/throughput line for the progress panel.
export function throughput(bytes, ms) {
  if (!ms || ms <= 0) return "";
  const kbps = bytes / 1024 / (ms / 1000);
  return `${kbps.toFixed(0)} KB/s`;
}
