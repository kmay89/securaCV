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

// ── chunked flash reads ────────────────────────────────────────────────────
// esptool-js's readFlash issues ONE read command for the whole span and
// streams it back with a ~4 MB un-acked window; on multi-megabyte reads a
// single lost byte desyncs SLIP and the transfer stalls (upstream #218).
// The fix is to plan many small reads — each its own command, each cheap to
// retry — and stitch the results. Pure math here; flash.js drives the wire.
export const READ_CHUNK = 0x10000; // 64 KB: small enough to retry, big enough to fly

export function planReadChunks(offset, size, chunk = READ_CHUNK) {
  const plan = [];
  if (!(size > 0)) return plan;
  let o = offset >>> 0;
  const end = o + size;
  while (o < end) {
    const n = Math.min(chunk, end - o);
    plan.push({ offset: o, size: n });
    o += n;
  }
  return plan;
}

// ── partition naming (for the board report) ───────────────────────────────
const DATA_SUBTYPE = {
  0x00: "otadata", 0x01: "phy_init", 0x02: "nvs", 0x03: "coredump",
  0x04: "nvs_keys", 0x05: "efuse", 0x06: "undefined",
  0x81: "fat", 0x82: "spiffs", 0x83: "littlefs",
};

export function partitionKind(e) {
  if (!e) return "?";
  if (e.type === 0x00) {
    if (e.subtype === 0x00) return "app · factory";
    if (e.subtype >= 0x10 && e.subtype < 0x20) return `app · ota_${e.subtype - 0x10}`;
    if (e.subtype === 0x20) return "app · test";
    return `app · 0x${e.subtype.toString(16)}`;
  }
  if (e.type === 0x01) {
    const n = DATA_SUBTYPE[e.subtype];
    return n ? `data · ${n}` : `data · 0x${e.subtype.toString(16)}`;
  }
  return `0x${e.type.toString(16)} · 0x${e.subtype.toString(16)}`;
}

export const isOtaDataPart = (e) => e && e.type === 0x01 && e.subtype === 0x00;
export const isNvsPart = (e) => e && e.type === 0x01 && e.subtype === 0x02;
export const isCoredumpPart = (e) => e && e.type === 0x01 && e.subtype === 0x03;
export const isWitnessLogPart = (e) => e && e.type === 0x01 && /witness/i.test(e.label || "");

// ── otadata (which A/B slot is live, how many updates it has seen) ─────────
// Two esp_ota_select_entry_t records, one per 4 KB sector:
//   ota_seq u32 | seq_label[20] | ota_state u32 | crc32(ota_seq) u32
// The bootloader trusts the highest CRC-valid seq; active app slot is
// (seq - 1) % <number of ota slots>.
const OTA_STATE = {
  0x0: "new (never booted)",
  0x1: "pending verify",
  0x2: "valid",
  0x3: "invalid (rolled back)",
  0x4: "aborted",
  0xffffffff: "normal",
};

// esp_rom_crc32_le(UINT32_MAX, buf, len): reflected 0xEDB88320, init 0,
// final complement. (Seeding with ~UINT32_MAX is what zeroes the init.)
export function crc32EspRom(bytes) {
  let c = 0;
  for (let i = 0; i < bytes.length; i++) {
    c ^= bytes[i];
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

export function parseOtaData(bytes, otaSlotCount) {
  const entries = [];
  for (const off of [0x0, 0x1000]) {
    if (off + 32 > bytes.length) break;
    const seq = u32le(bytes, off);
    const state = u32le(bytes, off + 24);
    const crc = u32le(bytes, off + 28);
    const crcOk = crc === crc32EspRom(bytes.subarray(off, off + 4));
    entries.push({ seq, state, crcOk });
  }
  const valid = entries.filter((e) => e.seq !== 0xffffffff && e.crcOk);
  if (!valid.length || !otaSlotCount) {
    // Fresh otadata: the bootloader falls back to factory, else ota_0.
    return { fresh: true, activeOta: 0, updatesSeen: 0, stateText: "factory default" };
  }
  const best = valid.reduce((a, b) => (b.seq > a.seq ? b : a));
  return {
    fresh: false,
    activeOta: (best.seq - 1) % otaSlotCount,
    updatesSeen: best.seq,
    stateText: OTA_STATE[best.state] || `0x${best.state.toString(16)}`,
    pendingVerify: best.state === 0x1,
  };
}

// ── coredump header (has this board ever crashed hard?) ────────────────────
// ESP-IDF writes the dump's total length as the partition's first word; an
// erased partition reads 0xFFFFFFFF. Presence is the interesting bit — the
// dump itself stays on the board.
export function parseCoredumpHeader(bytes, partitionSize) {
  if (!bytes || bytes.length < 4) return { present: false };
  const len = u32le(bytes, 0);
  const present = len !== 0xffffffff && len > 0 && len <= (partitionSize || 0x10000);
  return present ? { present: true, size: len } : { present: false };
}

// ── NVS reader (witness-chain state without booting the board) ─────────────
// ESP-IDF NVS: 4 KB pages — header(32) + entry-state bitmap(32) + 126×32-byte
// entries. Entry: ns u8 | type u8 | span u8 | chunkIdx u8 | crc u32 |
// key[16] | data[8]. Strings/blobs put length in the data field and the
// payload in the following (span-1) entries.
//
// PRIVACY GUARD: values are extracted ONLY for integer types and for blob
// keys explicitly allow-listed by the caller. Everything else (key material,
// WiFi credentials, tokens, certs) is reported as presence + size, never
// content — and the report UI only ever surfaces the allow-listed set.
const NVS_WRITTEN = 2; // 2-bit entry state 0b10
const NVS_INT_TYPES = { 0x01: 1, 0x11: 1, 0x02: 2, 0x12: 2, 0x04: 4, 0x14: 4, 0x08: 8, 0x18: 8 };

export function parseNvs(bytes, allowBlobKeys = []) {
  const allow = new Set(allowBlobKeys);
  const nsNames = {};   // index → namespace name
  const items = [];     // { nsIndex, key, type, value?, size? }
  const blobChunks = {}; // "ns/key" → [{chunkIdx, data}]

  for (let page = 0; page + 4096 <= bytes.length; page += 4096) {
    const state = u32le(bytes, page);
    if (state === 0xffffffff) continue; // uninitialized page
    const bitmap = bytes.subarray(page + 32, page + 64);
    for (let idx = 0; idx < 126; idx++) {
      const st = (bitmap[idx >> 2] >> ((idx & 3) * 2)) & 0x3;
      if (st !== NVS_WRITTEN) continue;
      const o = page + 64 + idx * 32;
      const ns = bytes[o], type = bytes[o + 1], span = bytes[o + 2] || 1, chunkIdx = bytes[o + 3];
      const key = cstr(bytes, o + 8, 16);
      if (!key) { idx += span - 1; continue; }

      if (ns === 0 && type === 0x01) {
        nsNames[bytes[o + 24]] = key; // namespace definition entry
      } else if (NVS_INT_TYPES[type]) {
        let v = 0;
        for (let i = NVS_INT_TYPES[type] - 1; i >= 0; i--) v = v * 256 + bytes[o + 24 + i];
        items.push({ nsIndex: ns, key, type, value: v });
      } else if (type === 0x21 || type === 0x41 || type === 0x42) {
        // string | legacy blob | v2 blob-data chunk — size u16 in the data
        // field, payload inline in the following (span-1) entries.
        const size = u16le(bytes, o + 24);
        if (type === 0x42 && allow.has(key)) {
          const data = bytes.slice(o + 32, o + 32 + Math.min(size, (span - 1) * 32));
          (blobChunks[`${ns}/${key}`] ||= []).push({ chunkIdx, data });
        }
        if (type === 0x41) {
          const item = { nsIndex: ns, key, type: 0x42, size };
          if (allow.has(key)) item.bytes = bytes.slice(o + 32, o + 32 + Math.min(size, (span - 1) * 32));
          items.push(item);
        }
        if (type === 0x21) items.push({ nsIndex: ns, key, type, size });
      } else if (type === 0x48) { // v2 blob index: authoritative total size
        items.push({ nsIndex: ns, key, type: 0x42, size: u32le(bytes, o + 24) });
      }
      idx += span - 1; // skip payload entries
    }
  }

  for (const item of items) {
    item.namespace = nsNames[item.nsIndex] || String(item.nsIndex);
    if (item.type === 0x42 && allow.has(item.key)) {
      const chunks = blobChunks[`${item.nsIndex}/${item.key}`];
      if (chunks) {
        chunks.sort((a, b) => a.chunkIdx - b.chunkIdx);
        let total = 0; chunks.forEach((c) => { total += c.data.length; });
        const buf = new Uint8Array(total);
        let p = 0; chunks.forEach((c) => { buf.set(c.data, p); p += c.data.length; });
        item.bytes = item.size ? buf.subarray(0, item.size) : buf;
      }
    }
  }
  return items;
}

// The firmware's own NVS map (canary_wap.ino): namespace "securacv" holds the
// witness-chain fast-boot cache. Chain head is the only blob we ever read out.
export const WITNESS_NVS_NAMESPACE = "securacv";
export const WITNESS_CHAIN_BLOB_KEY = "chain";

export function witnessSummary(items) {
  const ours = items.filter((i) => i.namespace === WITNESS_NVS_NAMESPACE);
  if (!ours.length) return null;
  const get = (k) => ours.find((i) => i.key === k);
  const chain = get(WITNESS_CHAIN_BLOB_KEY);
  return {
    seq: get("seq")?.value ?? null,          // witness records chained so far
    boots: get("boots")?.value ?? null,      // lifetime boot counter
    tamper: get("tamper")?.value ?? null,    // firmware's own tamper flag
    logSeq: get("logseq")?.value ?? null,
    chainHeadFp: chain?.bytes ? hex(chain.bytes.subarray(0, 8)) : null,
    provisioned: !!get("privkey"),           // identity key exists (never read)
    wifiConfigured: !!get("wifi_ssid"),      // presence only, never the value
  };
}

// ── progress prediction (a bar the user can trust) ─────────────────────────
// One tracker per stage. The fraction is monotonic — a progress bar must
// never move backwards — and the rate is an EMA so the "time left" estimate
// doesn't jitter with every packet. Timestamps are passed in, so this stays
// pure and testable.
export function makeEtaTracker(total) {
  let lastT = null, lastDone = 0, rate = 0, frac = 0; // rate: bytes/ms
  return {
    feed(done, now) {
      done = Math.min(Math.max(done, lastDone), total || done);
      if (lastT == null) { lastT = now; lastDone = done; }
      else if (done > lastDone) {
        const dt = now - lastT;
        if (dt > 0) {
          const r = (done - lastDone) / dt;
          rate = rate ? rate * 0.75 + r * 0.25 : r;
        }
        lastT = now; lastDone = done;
      }
      if (total > 0) frac = Math.max(frac, Math.min(1, done / total));
      const remain = Math.max(0, (total || 0) - lastDone);
      return {
        frac,
        kbps: rate * 1000 / 1024,
        etaSeconds: rate > 0 && total > 0 ? remain / rate / 1000 : null,
      };
    },
  };
}

export function formatDuration(s) {
  if (!Number.isFinite(s) || s == null || s < 0) return "";
  if (s < 8) return "a few seconds";
  if (s < 60) return `about ${Math.max(10, Math.round(s / 5) * 5)} seconds`;
  const m = Math.round(s / 60);
  return `about ${m} minute${m === 1 ? "" : "s"}`;
}

// ── serial console heuristics ───────────────────────────────────────────────
// Baud candidates for the monitor, most likely first. The firmware console is
// console_baud (115200); 74880 is the classic ESP32 boot-ROM rate; the rest
// cover common sketches. On native-USB boards (every current Canary) the baud
// barely matters — CDC ignores it — so the default just works.
export const CONSOLE_BAUDS = [115200, 74880, 9600, 230400, 460800, 921600];

// Wrong-baud output decodes as a soup of U+FFFD replacement chars and raw
// control bytes. Real firmware text — including the help menu's box-drawing
// glyphs — decodes cleanly. Needs a decent sample before it will judge.
export function looksLikeGarbage(text) {
  if (!text || text.length < 60) return false;
  let bad = 0, n = 0;
  for (const ch of text) {
    const c = ch.codePointAt(0);
    n++;
    if (c === 0xfffd) bad++;
    else if (c < 0x20 && c !== 9 && c !== 10 && c !== 13) bad++;
    else if (c === 0x7f) bad++;
  }
  return bad / n > 0.2;
}

// ── restoring a saved backup file (forward-compatible recovery) ─────────────
// A backup is raw flash bytes, so restoring one never depends on firmware
// version — but the file must plausibly belong on this chip.
export function validateBackupFile(byteLength, flashBytes, name) {
  if (!(byteLength > 0)) return { ok: false, reason: "that file is empty" };
  if (flashBytes && byteLength > flashBytes) {
    return { ok: false, reason: `that file is bigger than this chip's flash ` +
      `(${formatBytes(byteLength)} vs ${formatBytes(flashBytes)}) — it can't be from this board` };
  }
  if (flashBytes && byteLength !== flashBytes) {
    return { ok: true, warn: `heads up: ${name || "this file"} is smaller than the chip ` +
      `(${formatBytes(byteLength)} of ${formatBytes(flashBytes)}) — it'll be written from the ` +
      `start of flash and the rest is left untouched` };
  }
  return { ok: true };
}

// Which product a rescue should offer first: the one the board is already
// running if we could read it, else the chip's only match, else the caller
// shows a picker.
export function pickRescueProduct(catalog, chip, currentProjectName) {
  const matches = productsForChip(catalog, chip);
  if (!matches.length) return null;
  const current = matchProjectToProduct(catalog, currentProjectName);
  if (current && matches.some((p) => p.id === current.id)) return current;
  return matches.length === 1 ? matches[0] : null;
}

// ── hex peek (the flash map's magnifying glass) ────────────────────────────
// Classic hex dump lines: address | 16 bytes | ASCII. Pure formatting so the
// report can show real bytes read off the board without any surprises.
export function hexDumpLines(bytes, baseAddr = 0, width = 16) {
  const lines = [];
  for (let o = 0; o < bytes.length; o += width) {
    const row = bytes.subarray(o, o + width);
    let hexs = "", ascii = "";
    for (let i = 0; i < row.length; i++) {
      hexs += row[i].toString(16).padStart(2, "0") + (i === 7 ? "  " : " ");
      ascii += row[i] >= 0x20 && row[i] < 0x7f ? String.fromCharCode(row[i]) : "·";
    }
    lines.push({
      addr: "0x" + (baseAddr + o).toString(16).padStart(6, "0"),
      hex: hexs.trimEnd(),
      ascii,
    });
  }
  return lines;
}

// One honest sentence about what a region's first bytes look like — driven
// by the same magic numbers the chip itself uses.
export function sniffRegion(bytes) {
  if (!bytes || !bytes.length) return "nothing readable";
  let ff = 0;
  for (let i = 0; i < bytes.length; i++) if (bytes[i] === 0xff) ff++;
  if (ff === bytes.length) return "erased — nothing stored here yet (all 0xFF)";
  if (bytes[0] === 0xe9) return "ESP32 firmware image — 0xE9 is the chip's own \"program starts here\" marker";
  if (bytes.length >= 2 && u16le(bytes, 0) === PARTITION_MAGIC) return "partition table entries (magic 0xAA50) — the board's map of itself";
  if (bytes.length >= 4 && u32le(bytes, 0) === APP_DESC_MAGIC) return "firmware description block (magic 0xABCD5432)";
  if (ff / bytes.length > 0.9) return "mostly erased, a little data at the edges";
  return "stored data";
}

// ── install diff (what will actually change on the board) ──────────────────
// At install time we hold both worlds in memory: the safety copy (every byte
// currently on the board) and the image about to be written. Comparing them
// region-by-region answers the real questions — is the firmware updated, do
// my settings (WiFi, identity, witness chain) survive, what's untouched —
// with byte-level certainty instead of guesses.
//
// Regions come from the NEW image's partition table when it carries one
// (factory images do), else from the board's current table; if both exist
// and disagree, the layout itself is changing and we say so.
function tableFrom(bytes) {
  if (!bytes || bytes.length < 0x8000 + 32) return { entries: [], apps: [] };
  return parsePartitionTable(bytes.subarray(0x8000, Math.min(0x8c00, bytes.length)));
}

function regionAllFF(bytes, off, end) {
  for (let i = off; i < end; i++) if (bytes[i] !== 0xff) return false;
  return true;
}

function countDiff(a, b, off, end) {
  let n = 0;
  for (let i = off; i < end; i++) if (a[i] !== b[i]) n++;
  return n;
}

export function diffInstall(oldBytes, newBytes) {
  const newPt = tableFrom(newBytes);
  const oldPt = tableFrom(oldBytes);
  const table = newPt.entries.length ? newPt : oldPt;
  if (!table.entries.length) return null; // nothing to anchor a map to

  const layoutChanged = !!(newPt.entries.length && oldPt.entries.length &&
    JSON.stringify(newPt.entries) !== JSON.stringify(oldPt.entries));

  const rows = [];
  // The system area first: bootloader + the partition map itself.
  const firstOff = Math.min(...table.entries.map((e) => e.offset));
  if (firstOff > 0) {
    rows.push(describeRegion(oldBytes, newBytes,
      { label: "system", type: 0xfe, subtype: 0, offset: 0, size: firstOff },
      "bootloader + partition map"));
  }
  for (const e of table.entries) rows.push(describeRegion(oldBytes, newBytes, e));
  return { layoutChanged, rows, imageLength: newBytes.length };
}

function describeRegion(oldBytes, newBytes, e, kindOverride) {
  const row = {
    label: e.label || partitionKind(e),
    kind: kindOverride || partitionKind(e),
    offset: e.offset, size: e.size,
    type: e.type, subtype: e.subtype,
  };
  const end = e.offset + e.size;
  if (e.offset >= newBytes.length) {
    row.verdict = "untouched"; // the image never reaches this region
    return row;
  }
  const cmpEnd = Math.min(end, newBytes.length, oldBytes.length);
  const diff = countDiff(oldBytes, newBytes, e.offset, cmpEnd);
  if (diff === 0) { row.verdict = "identical"; return row; }
  if (regionAllFF(newBytes, e.offset, cmpEnd)) {
    row.verdict = "wiped"; // written as erased flash: a factory-fresh region
  } else {
    row.verdict = "changed";
    row.changedPct = Math.round((diff / (cmpEnd - e.offset)) * 100) || 1;
  }
  // App slots: name the change in firmware terms when descriptors are legible.
  if (e.type === 0x00) {
    const read = (bytes) => {
      const o = e.offset + APP_DESC_OFFSET;
      if (o + 256 > bytes.length) return null;
      return parseAppDescriptor(bytes.subarray(o, o + 256));
    };
    const before = read(oldBytes), after = read(newBytes);
    if (before) row.before = `${before.projectName || "?"} ${before.version || ""}`.trim();
    if (after) row.after = `${after.projectName || "?"} ${after.version || ""}`.trim();
  }
  return row;
}

// The question people actually ask, answered from the diff: do my settings
// survive this install?
export function settingsVerdict(diff, hadWifi) {
  if (!diff) return null;
  const nvs = diff.rows.find((r) => r.type === 0x01 && r.subtype === 0x02);
  if (!nvs) return null;
  if (nvs.verdict === "identical" || nvs.verdict === "untouched") {
    return { kept: true, text: hadWifi
      ? "Your settings survive — saved WiFi, device identity and witness-chain counters stay exactly as they are."
      : "The settings area is untouched." };
  }
  return { kept: false, text: hadWifi
    ? "Your settings are reset — the saved WiFi is cleared, so the board comes up with its setup network again, like the first day."
    : "The settings area is reset to factory-fresh." };
}

// ── browser detection (for the "hop to Chrome" card) ───────────────────────
// Only used when Web Serial is absent, to name what the user IS on and point
// the arrow at a browser that works. iPadOS masquerades as a Mac, hence the
// touch-points hint.
export function detectBrowser(ua, maxTouchPoints = 0) {
  ua = String(ua || "");
  if (/iPhone|iPod/.test(ua)) return { id: "ios", label: "iPhone", icon: "📱", mobile: true };
  if (/iPad/.test(ua) || (/Macintosh/.test(ua) && maxTouchPoints > 1)) {
    return { id: "ios", label: "iPad", icon: "📱", mobile: true };
  }
  if (/Android/.test(ua)) return { id: "android", label: "Android", icon: "📱", mobile: true };
  if (/Firefox\//.test(ua)) return { id: "firefox", label: "Firefox", icon: "🦊", mobile: false };
  if (/Safari\//.test(ua) && !/Chrome|Chromium|Edg\/|OPR\//.test(ua)) {
    return { id: "safari", label: "Safari", icon: "🧭", mobile: false };
  }
  return { id: "other", label: "this browser", icon: "🌐", mobile: false };
}

// ── release channels (docs/RELEASE_PROCESS.md) ─────────────────────────────
// The dev channel's one stable address: the rolling fw-dev-latest prerelease
// that CI re-points on every fw-v*-dev.*/-rc.* tag. This is a fixed
// first-party constant, deliberately NOT routed through manifestOverrideUrl
// (which guards arbitrary URLs) — ?channel=dev can only ever mean this URL.
export const DEV_FLASH_MANIFEST_URL =
  "https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/manifest-flash.json";

export function channelFromSearch(search) {
  try {
    return new URLSearchParams(search || "").get("channel") === "dev" ? "dev" : "release";
  } catch {
    return "release";
  }
}

// ── WiFi pre-provisioning (write NVS, not just read it) ────────────────────
// The firmware reads its WiFi from the standard NVS partition — namespace
// "securacv", blobs wifi_ssid / wifi_pass, bool wifi_en (canary_wap.ino).
// So the flasher can hand a board its network at install time by writing a
// minimal, VALID ESP-IDF NVS page containing exactly those three keys; the
// firmware's first boot then adds its own identity beside them. If this
// image were ever malformed, ESP-IDF's nvs_flash_init erases the region and
// the board simply falls back to its setup network — graceful by design.
//
// Format (ESP-IDF NVS v2, mirroring the parser above): 4 KB page =
// header(32) + entry-state bitmap(32) + 126×32-byte items. All CRCs are
// esp_rom_crc32_le(UINT32_MAX, …) — crc32EspRom.

const NVS_PAGE = 4096;

function nvsItemCrc(page, o) {
  // Item CRC covers bytes 0-3 (ns,type,span,chunk) and 8-31 (key+data),
  // skipping the CRC field itself.
  const tmp = new Uint8Array(28);
  tmp.set(page.subarray(o, o + 4), 0);
  tmp.set(page.subarray(o + 8, o + 32), 4);
  return crc32EspRom(tmp);
}

function nvsWr32(page, o, v) {
  page[o] = v & 0xff; page[o + 1] = (v >>> 8) & 0xff;
  page[o + 2] = (v >>> 16) & 0xff; page[o + 3] = (v >>> 24) & 0xff;
}

export function buildNvsWifiImage(ssid, pass, partitionSize) {
  const enc = new TextEncoder();
  const ssidB = enc.encode(String(ssid));
  const passB = enc.encode(String(pass || ""));
  if (ssidB.length < 1 || ssidB.length > 32)
    throw new Error("WiFi name must be 1-32 bytes");
  if (passB.length !== 0 && (passB.length < 8 || passB.length > 63))
    throw new Error("WiFi password must be 8-63 characters (or empty for an open network)");
  if (!(partitionSize >= NVS_PAGE)) throw new Error("nvs partition too small");

  const img = new Uint8Array(partitionSize).fill(0xff);
  const page = img.subarray(0, NVS_PAGE);
  let idx = 0; // next free 32-byte item slot

  const markWritten = (j) => { page[32 + (j >> 2)] &= ~(1 << ((j & 3) * 2)); };
  const itemBase = (j) => 64 + j * 32;

  const writeHeader = (o, ns, type, span, chunk, key) => {
    page[o] = ns; page[o + 1] = type; page[o + 2] = span; page[o + 3] = chunk;
    for (let i = 0; i < 16; i++) page[o + 8 + i] = i < key.length ? key.charCodeAt(i) : 0;
  };
  const finishItem = (j) => { nvsWr32(page, itemBase(j) + 4, nvsItemCrc(page, itemBase(j))); };

  // Namespace definition: "securacv" → index 1.
  writeHeader(itemBase(idx), 0, 0x01, 1, 0xff, WITNESS_NVS_NAMESPACE);
  page[itemBase(idx) + 24] = 1;
  for (let i = 1; i < 8; i++) page[itemBase(idx) + 24 + i] = 0xff;
  finishItem(idx); markWritten(idx); idx++;

  const writeBlob = (key, bytes) => {
    // v2 blob = one BLOB_DATA chunk (payload in the following entries) +
    // one BLOB_IDX carrying the total size.
    const payloadEntries = Math.ceil(bytes.length / 32);
    const o = itemBase(idx);
    writeHeader(o, 1, 0x42, 1 + payloadEntries, 0 /* chunkStart VER_0 + 0 */, key);
    page[o + 24] = bytes.length & 0xff; page[o + 25] = (bytes.length >> 8) & 0xff;
    page[o + 26] = 0xff; page[o + 27] = 0xff;               // reserved
    nvsWr32(page, o + 28, crc32EspRom(bytes));              // payload CRC
    finishItem(idx); markWritten(idx);
    for (let i = 0; i < payloadEntries; i++) markWritten(idx + 1 + i);
    page.set(bytes, o + 32);                                // 0xFF-padded tail
    idx += 1 + payloadEntries;

    const oi = itemBase(idx);
    writeHeader(oi, 1, 0x48, 1, 0xff, key);
    nvsWr32(page, oi + 24, bytes.length);
    page[oi + 28] = 1;    // chunkCount
    page[oi + 29] = 0;    // chunkStart (VER_0)
    page[oi + 30] = 0xff; page[oi + 31] = 0xff;
    finishItem(idx); markWritten(idx); idx++;
  };

  writeBlob("wifi_ssid", ssidB);
  writeBlob("wifi_pass", passB);

  // wifi_en = true (u8). Preferences putBool stores a u8.
  const oe = itemBase(idx);
  writeHeader(oe, 1, 0x01, 1, 0xff, "wifi_en");
  page[oe + 24] = 1;
  for (let i = 1; i < 8; i++) page[oe + 24 + i] = 0xff;
  finishItem(idx); markWritten(idx); idx++;

  // Page header: ACTIVE, seq 0, version 0xFE (v2); CRC over bytes 4..27.
  nvsWr32(page, 0, 0xfffffffe);
  nvsWr32(page, 4, 0);
  page[8] = 0xfe;
  for (let i = 9; i < 28; i++) page[i] = 0xff;
  nvsWr32(page, 28, crc32EspRom(page.subarray(4, 28)));
  return img;
}

// The standard WiFi-QR payload (WPA assumed unless the password is empty).
// Escaping per the de-facto spec: backslash the chars \ ; , " :
export function wifiQrString(ssid, pass) {
  const esc = (s) => String(s).replace(/([\\;,":])/g, "\\$1");
  return pass
    ? `WIFI:T:WPA;S:${esc(ssid)};P:${esc(pass)};;`
    : `WIFI:T:nopass;S:${esc(ssid)};;`;
}

// ── error classification (turn raw failures into an actionable first line) ──
// Web Serial + esptool + fetch all throw wildly different messages for the
// handful of things that actually go wrong at a USB port. Fold them into one
// verdict so the error cards can say WHAT happened and WHAT to do — instead of
// only surfacing a raw string under "technical details". Pure + tested; flash.js
// renders {title, hint} and still keeps the raw message for the curious.
export function classifyFlashError(err) {
  const msg = String((err && (err.message || err.name)) || err || "").toLowerCase();
  const has = (...subs) => subs.some((s) => msg.includes(s));

  // Another program/tab already holds the port (Arduino IDE, PlatformIO
  // monitor, a second flasher tab). The single most common real failure.
  if (has("failed to open serial port", "port is already open", "already open",
          "the port is closed") ||
      (has("open") && has("access", "busy", "in use")))
    return { kind: "port-busy", title: "That port is busy",
      hint: "Another program or browser tab is holding the board. Close the Arduino " +
        "IDE / PlatformIO serial monitor and any other flasher tab, then unplug, " +
        "replug, and try again." };

  // Cable pulled or board vanished mid-operation.
  if (has("device has been lost", "device lost", "no device", "disconnected",
          "networkerror", "network error", "the device was lost"))
    return { kind: "device-lost", title: "The board disappeared",
      hint: "The connection dropped — usually the cable, the port, or the board " +
        "resetting. Reseat the USB-C cable (a data cable, not charge-only) and " +
        "reconnect. Nothing was harmed — you can't brick it from here." };

  // OS-level permission / missing driver.
  if (has("access denied", "permission", "not allowed", "securityerror"))
    return { kind: "permission", title: "The system wouldn't grant access to the port",
      hint: "On Linux, add yourself to the dialout group (sudo usermod -aG dialout " +
        "$USER, then log out and back in); on Windows, install the board's USB-serial " +
        "driver. Then reconnect." };

  // Integrity failure — checked BEFORE the generic network case so a checksum
  // message never reads as a download problem.
  if (has("checksum", "sha-256", "sha256", "md5", "hash mismatch", "failed its"))
    return { kind: "integrity", title: "The image failed its integrity check",
      hint: "Nothing was written. The download may have been corrupted, or the signed " +
        "release is mid-update — wait a moment and try again." };

  // Fetching the image/manifest failed (a network/release issue, not the board).
  if (has("download failed", "http ", "failed to fetch", "load failed", "fetch"))
    return { kind: "download", title: "Couldn't download the firmware image",
      hint: "That's a network or release issue, not your board. Check your connection " +
        "and try again; if it persists, the signed release may be mid-update. You can " +
        "also install a local .bin under Advanced." };

  // Board present but not answering the ROM bootloader → not in download mode.
  if (has("timed out", "timeout", "no serial data", "failed to sync", "sync",
          "invalid head of packet", "packet content transfer stopped",
          "wrong boot mode"))
    return { kind: "not-in-download", title: "The board isn't answering the bootloader",
      hint: "It's almost certainly not in download mode. Hold BOOT, tap RESET, release " +
        "BOOT, then reconnect." };

  // Our chunked reader gave up on a region after its retries.
  if (has("short read", "stalled", "read timeout"))
    return { kind: "read-stall", title: "A read stalled partway",
      hint: "Reseat the cable and try again — a shorter, known-good USB-C data cable is " +
        "the usual fix." };

  return { kind: "unknown", title: null, // caller keeps its own generic title
    hint: "If this keeps happening: unplug the board, plug it back in, put it in download " +
      "mode (hold BOOT, tap RESET, release BOOT), and retry." };
}

// ── deep-link focus (arriving from the website's /checkup selector) ─────────
// The /checkup "Flash in browser" button links here as ?product=<id>. We honour
// it only as a FOCUS hint — the picker still shows just the products the
// detected chip can take (the chip guard always wins), and if the requested id
// is among them we lead with that one instead of a wall of options. A bad/absent
// param simply means "show the normal list". Pure + tested.
export function preferredProductId(search) {
  let v;
  try { v = new URLSearchParams(search || "").get("product"); } catch { return null; }
  return v && /^[a-z0-9-]+$/i.test(v) ? v : null;
}

// The product to LEAD WITH when the user hasn't asked for a specific one — so
// plugging a board in just works instead of presenting a menu. The catalog is
// authored flagship-first per chip (canary before wap; vision before its
// variants), so the first product that fits the detected silicon is the honest
// "recommended for you". null only if nothing targets this chip.
export function recommendedProduct(catalog, detected) {
  return productsForChip(catalog, detected)[0] || null;
}

// ── catalog shape guard ─────────────────────────────────────────────────────
// flash.js reads specific fields off devices/flash.json the moment it loads
// (no_brick.points, recovery[], products[].chip for the chip guard). A valid-
// JSON-but-malformed catalog would otherwise throw mid-render and leave a blank
// page. Validate up front and degrade to a clear message instead. Pure + tested.
export function validateCatalog(cat) {
  const errs = [];
  if (!cat || typeof cat !== "object") return ["catalog is not an object"];
  if (!Array.isArray(cat.products))
    errs.push("catalog.products must be an array");
  if (!cat.chips || typeof cat.chips !== "object")
    errs.push("catalog.chips must be an object");
  if (!cat.no_brick || !cat.no_brick.headline || !Array.isArray(cat.no_brick.points))
    errs.push("catalog.no_brick { headline, why, points[] } is required (safety strip)");
  if (!Array.isArray(cat.recovery))
    errs.push("catalog.recovery must be an array");
  (Array.isArray(cat.products) ? cat.products : []).forEach((p, i) => {
    const who = (p && p.id) || `#${i}`;
    if (!p || typeof p !== "object") { errs.push(`products[${i}] is not an object`); return; }
    if (!p.id) errs.push(`products[${i}] missing id`);
    if (!p.chip) errs.push(`product ${who} missing chip (the chip guard needs it)`);
    if (!p.name) errs.push(`product ${who} missing name`);
  });
  return errs;
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

// ── self-healing: connect/flash baud ladder ─────────────────────────────────
// The flash transfer speed to try, fastest first. esptool syncs the ROM at
// romBaudrate (115200) and only then switches to one of these; flaky cables,
// unpowered hubs, and long USB runs choke at the top speed but work fine a
// rung down. The connect flow walks this on failure so "it won't flash" heals
// itself instead of dead-ending. (Distinct from CONSOLE_BAUDS, which is the
// read-only monitor's guess-the-log-speed list.)
export const FLASH_BAUDS = [921600, 460800, 230400, 115200];

// ── self-healing: turn a boot log into a diagnosis + fix ────────────────────
// After a flash the device boots and prints to serial. A handful of fatal
// signatures have specific, actionable fixes — surface those instead of raw
// text. Returns the most-severe match as {signature, means, fix, action} or
// null. `action` is a machine hint the UI maps to a one-click recovery:
// "clean-install" (reflash with a full erase) or "power" (not a reflash — a
// power/cable problem the user must fix). Ordered most-specific first.
const BOOT_SIGNATURES = [
  {
    signature: "brownout",
    test: /brownout detector was triggered|\bBROWNOUT_RST\b|rst:0x[0-9a-f]+ \(BROWNOUT/i,
    means: "The board browned out — it isn’t getting enough power to boot.",
    fix: "This isn’t the firmware. Use a different USB port (straight into the " +
      "computer, not a hub), or a powered hub, and a shorter good-quality data " +
      "cable. Then reconnect.",
    action: "power",
  },
  {
    signature: "panic",
    test: /guru meditation|backtrace:|panic'?ed|abort\(\) was called|assert failed/i,
    means: "The new firmware crashed as it started up.",
    fix: "A clean install usually clears this — it wipes stale settings a previous " +
      "firmware may have left behind. Reconnect and choose “clean install”.",
    action: "clean-install",
  },
  {
    signature: "no-app",
    test: /invalid header: 0x|no bootable app partitions|ota_data partition|not found any bootable/i,
    means: "The bootloader couldn’t find a complete, valid app to run.",
    fix: "Reconnect and flash again with a full erase (clean install) so a whole " +
      "image is laid down from scratch.",
    action: "clean-install",
  },
  {
    signature: "flash-error",
    test: /flash read err|checksum failed|corrupt|e \(\d+\) esp_image/i,
    means: "The board had trouble reading its own flash.",
    fix: "Reseat the cable and do a clean install. If it keeps happening across " +
      "several boards, that one’s flash chip may be failing.",
    action: "clean-install",
  },
];

export function diagnoseBootLog(text) {
  const t = String(text || "");
  if (t.length < 8) return null;
  for (const s of BOOT_SIGNATURES) {
    if (s.test.test(t)) {
      return { signature: s.signature, means: s.means, fix: s.fix, action: s.action };
    }
  }
  return null;
}

// ── self-healing: USB-UART bridge detection (driver hints) ──────────────────
// The XIAO boards use the ESP32's native USB (VID 0x303A) — no driver needed.
// A couple of variants (and clones) use a USB-serial bridge chip that needs an
// OS driver on Windows/older macOS; when the board is one of those and the port
// won't open, point at the exact driver instead of a mystifying "nothing
// happened". Returns {name, driverUrl, note} for a known bridge, else null
// (native USB or unknown → no driver to chase).
const USB_BRIDGES = {
  0x10c4: { name: "Silicon Labs CP210x", driverUrl: "https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers" },
  0x1a86: { name: "WCH CH340/CH343", driverUrl: "https://www.wch-ic.com/downloads/CH341SER_EXE.html" },
  0x0403: { name: "FTDI FT232", driverUrl: "https://ftdichip.com/drivers/vcp-drivers/" },
};
const NATIVE_USB_VENDORS = new Set([0x303a]); // Espressif native USB-Serial/JTAG

export function usbBridgeInfo(usbVendorId, usbProductId) {
  if (usbVendorId == null) return null;
  if (NATIVE_USB_VENDORS.has(usbVendorId)) return null;
  const b = USB_BRIDGES[usbVendorId];
  if (!b) return null;
  return {
    name: b.name,
    driverUrl: b.driverUrl,
    note: `This board talks over a ${b.name} USB-serial chip. If it won’t connect, ` +
      `install that chip’s driver (Windows and older macOS need it), then reconnect.`,
    vid: usbVendorId,
    pid: usbProductId == null ? null : usbProductId,
  };
}

// ── self-healing: a copy-paste diagnostic report (never get stuck) ──────────
// One click turns "I'm stuck" into an actionable, paste-into-Discussions block.
// Public-only by construction: it takes a plain object of already-safe facts
// (no WiFi credentials, no keys) and formats them. Pure + testable.
export function buildDiagnosticReport(info = {}) {
  const lines = ["SecuraCV flasher diagnostic", "==========================="];
  const add = (label, val) => {
    if (val === undefined || val === null || val === "") return;
    lines.push(`${label}: ${val}`);
  };
  add("when", info.when);
  add("browser", info.browser);
  add("platform", info.platform);
  add("web serial", info.webSerial === undefined ? undefined : (info.webSerial ? "yes" : "no"));
  add("firmware train", info.catalogVersion);
  add("chip", info.chipDesc || info.chip);
  add("MAC", info.mac);
  add("flash size", info.flashBytes ? formatBytes(info.flashBytes) : undefined);
  add("USB device", info.usb);
  add("chosen product", info.product);
  add("connected baud", info.baud);
  add("stage", info.stage);
  add("error", info.error);
  if (info.logTail) {
    lines.push("--- last serial output ---");
    lines.push(String(info.logTail).split("\n").slice(-12).join("\n"));
  }
  return lines.join("\n") + "\n";
}
