'use strict';

// Evidence-pack reader — the small, dependency-free half of "drop the .zip".
//
// The Home Assistant integration hands a recipient ONE file: a zip holding the
// kernel's export bundle (securacv-events-<bucket>.json, verbatim bytes), this
// viewer (evidence_viewer.html), README.txt and WITNESS_KEY.txt. The viewer
// used to take only the .json; this module lets it take the pack itself, so the
// recipient never has to find an unzip tool first.
//
// It reads the zip's central directory and the one member that matters, and
// inflates a deflated member with the browser's own DecompressionStream
// ('deflate-raw') — no library, no network. ZIP64, encryption and any other
// compression method are refused by name rather than guessed at: a pack is a
// few hundred kilobytes, written by one known producer. Nothing here changes
// what is verified: the bytes handed back are exactly the member's bytes, and
// verify_core.js does the same work on them it does on a dropped .json.
//
// Dual-use like verify_core.js: `require()` under Node for the tests, a
// `SecuraCVZip` global in the browser (build.mjs inlines it into the viewer).

const SIG_LOCAL = 0x04034b50;
const SIG_CENTRAL = 0x02014b50;
const SIG_EOCD = 0x06054b50;
const METHOD_STORED = 0;
const METHOD_DEFLATE = 8;
const FLAG_ENCRYPTED = 0x0001;
const FLAG_UTF8 = 0x0800;
const MAX_COMMENT = 0xffff;
const ZIP64_MARK = 0xffffffff;

// The member the integration writes (contracts C7): the kernel's own filename.
const BUNDLE_MEMBER_RE = /(?:^|\/)securacv-events-\d+\.json$/;
const WITNESS_KEY_MEMBER = 'WITNESS_KEY.txt';

/** True when the first bytes are a zip local-file-header signature. */
function looksLikeZip(bytes) {
  const b = zipBytes(bytes);
  return b.length >= 4 && zipU32(b, 0) === SIG_LOCAL;
}

function zipBytes(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError('zip input must be bytes');
}
function zipU16(b, o) { return b[o] | (b[o + 1] << 8); }
function zipU32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)) + b[o + 3] * 0x1000000; }

const zipUtf8 = new TextDecoder('utf-8', { fatal: false });

/**
 * The central directory, as plain records:
 *   { name, method, flags, crc32, compressedSize, size, localHeaderOffset }
 * Throws with a plain-English reason when the file is not a zip this reader
 * handles (not a zip at all, truncated, ZIP64, multi-disk).
 */
function listEntries(input) {
  const b = zipBytes(input);
  if (!looksLikeZip(b)) throw new Error('not a zip file');
  // The end-of-central-directory record sits in the last 22 + comment bytes.
  let eocd = -1;
  const stop = Math.max(0, b.length - 22 - MAX_COMMENT);
  for (let i = b.length - 22; i >= stop; i--) {
    if (zipU32(b, i) === SIG_EOCD) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('zip is truncated: no end-of-central-directory record');
  const diskNumber = zipU16(b, eocd + 4);
  const cdDisk = zipU16(b, eocd + 6);
  const count = zipU16(b, eocd + 10);
  const cdSize = zipU32(b, eocd + 12);
  const cdOffset = zipU32(b, eocd + 16);
  if (diskNumber !== 0 || cdDisk !== 0) throw new Error('multi-disk zips are not supported');
  if (count === 0xffff || cdSize === ZIP64_MARK || cdOffset === ZIP64_MARK) throw new Error('ZIP64 archives are not supported');
  if (cdOffset + cdSize > b.length) throw new Error('zip is truncated: central directory runs past the end');

  const entries = [];
  let p = cdOffset;
  for (let i = 0; i < count; i++) {
    if (p + 46 > b.length || zipU32(b, p) !== SIG_CENTRAL) throw new Error('zip central directory is damaged');
    const flags = zipU16(b, p + 8);
    const method = zipU16(b, p + 10);
    const crc32 = zipU32(b, p + 16);
    const compressedSize = zipU32(b, p + 20);
    const size = zipU32(b, p + 24);
    const nameLen = zipU16(b, p + 28);
    const extraLen = zipU16(b, p + 30);
    const commentLen = zipU16(b, p + 32);
    const localHeaderOffset = zipU32(b, p + 42);
    if (compressedSize === ZIP64_MARK || size === ZIP64_MARK || localHeaderOffset === ZIP64_MARK) {
      throw new Error('ZIP64 archives are not supported');
    }
    const nameBytes = b.subarray(p + 46, p + 46 + nameLen);
    // Names are UTF-8 when flagged; the producer writes ASCII either way.
    const name = (flags & FLAG_UTF8) || zipIsAscii(nameBytes) ? zipUtf8.decode(nameBytes) : zipLatin1(nameBytes);
    entries.push({ name, method, flags, crc32, compressedSize, size, localHeaderOffset });
    p += 46 + nameLen + extraLen + commentLen;
  }
  return entries;
}

function zipIsAscii(bytes) {
  for (let i = 0; i < bytes.length; i++) if (bytes[i] > 0x7f) return false;
  return true;
}
function zipLatin1(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return s;
}

// CRC-32 (IEEE), the zip's own integrity check — so a truncated download reads
// as "the pack is damaged" instead of "the evidence failed verification".
const ZIP_CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function zipCrc32(bytes) {
  let c = 0xffffffff;
  for (let i = 0; i < bytes.length; i++) c = ZIP_CRC_TABLE[(c ^ bytes[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** Inflate raw deflate bytes with the platform's DecompressionStream. */
async function zipInflateRaw(bytes) {
  if (typeof DecompressionStream === 'undefined') {
    const err = new Error('this browser cannot unpack a compressed zip (no DecompressionStream)');
    err.code = 'no_decompression_stream';
    throw err;
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  const buffer = await new Response(stream).arrayBuffer();
  return new Uint8Array(buffer);
}

/**
 * The bytes of one member, exactly as stored. Stored members are sliced;
 * deflated members are inflated; anything else is refused by name. The CRC-32
 * the archive recorded is checked, as is the uncompressed size.
 */
async function readMember(input, entry) {
  const b = zipBytes(input);
  if (entry.flags & FLAG_ENCRYPTED) throw new Error(`"${entry.name}" is encrypted; evidence packs are never encrypted`);
  const h = entry.localHeaderOffset;
  if (h + 30 > b.length || zipU32(b, h) !== SIG_LOCAL) throw new Error(`zip local header for "${entry.name}" is damaged`);
  const nameLen = zipU16(b, h + 26);
  const extraLen = zipU16(b, h + 28);
  const start = h + 30 + nameLen + extraLen;
  const end = start + entry.compressedSize;
  if (end > b.length) throw new Error(`zip is truncated inside "${entry.name}"`);
  const raw = b.subarray(start, end);
  let data;
  if (entry.method === METHOD_STORED) data = raw;
  else if (entry.method === METHOD_DEFLATE) data = await zipInflateRaw(raw);
  else throw new Error(`"${entry.name}" uses compression method ${entry.method}; only stored and deflate are supported`);
  if (data.length !== entry.size) throw new Error(`"${entry.name}" unpacked to ${data.length} bytes, the archive says ${entry.size} — the pack is damaged`);
  if (zipCrc32(data) !== entry.crc32) throw new Error(`"${entry.name}" fails the archive's own CRC-32 check — the pack is damaged`);
  return data;
}

/**
 * Which member is the evidence: the kernel-named bundle
 * (securacv-events-<bucket>.json) when present, else the one .json that is not
 * an index. More than one candidate is an error, not a guess.
 */
function pickBundleMember(entries) {
  const files = entries.filter((e) => !e.name.endsWith('/'));
  const named = files.filter((e) => BUNDLE_MEMBER_RE.test(e.name));
  if (named.length === 1) return named[0];
  if (named.length > 1) throw new Error(`the pack holds ${named.length} event exports; unzip it and drop one .json`);
  const jsons = files.filter((e) => /\.json$/i.test(e.name) && !/(?:^|\/)index\.json$/i.test(e.name));
  if (jsons.length === 1) return jsons[0];
  if (jsons.length === 0) throw new Error('the pack holds no .json evidence bundle');
  throw new Error(`the pack holds ${jsons.length} .json files and none is named securacv-events-<bucket>.json; unzip it and drop the right one`);
}

/**
 * Parse WITNESS_KEY.txt (`verifying_key=<64 hex>\nfingerprint=<8 hex>\nkernel=<id>\n`)
 * into { verifying_key, fingerprint, kernel }. This is the OWNER's claim that
 * traveled with the pack — the viewer shows it, and never treats it as the
 * independently obtained key that confirms authorship.
 */
function parseWitnessKey(text) {
  const out = {};
  for (const line of String(text).split(/\r?\n/)) {
    const m = line.match(/^\s*([a-z_]+)\s*=\s*(.*?)\s*$/);
    if (m) out[m[1]] = m[2];
  }
  if (out.verifying_key && !/^[0-9a-f]{64}$/i.test(out.verifying_key)) delete out.verifying_key;
  if (out.fingerprint && !/^[0-9a-f]{8,}$/i.test(out.fingerprint)) delete out.fingerprint;
  return out;
}

/**
 * Everything the viewer wants from a dropped pack:
 *   { memberName, text, members, witnessKey }
 * `text` is the bundle member's UTF-8 text, byte-for-byte what the kernel
 * exported; `witnessKey` is the parsed WITNESS_KEY.txt or null.
 */
async function extractEvidence(input) {
  const b = zipBytes(input);
  const entries = listEntries(b);
  const member = pickBundleMember(entries);
  const bytes = await readMember(b, member);
  const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  let witnessKey = null;
  const keyEntry = entries.find((e) => e.name === WITNESS_KEY_MEMBER || e.name.endsWith(`/${WITNESS_KEY_MEMBER}`));
  if (keyEntry) {
    try {
      witnessKey = parseWitnessKey(new TextDecoder('utf-8').decode(await readMember(b, keyEntry)));
    } catch {
      witnessKey = null; // the key note is a courtesy; the bundle is the evidence
    }
  }
  return { memberName: member.name, text, members: entries.map((e) => e.name), witnessKey };
}

const zipApi = {
  looksLikeZip,
  listEntries,
  readMember,
  pickBundleMember,
  parseWitnessKey,
  extractEvidence,
  crc32: zipCrc32,
  BUNDLE_MEMBER_RE,
  WITNESS_KEY_MEMBER,
};

if (typeof module !== 'undefined' && module.exports) module.exports = zipApi;
if (typeof globalThis !== 'undefined') globalThis.SecuraCVZip = zipApi;
