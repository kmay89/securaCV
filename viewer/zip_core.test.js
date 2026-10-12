'use strict';

// Tests for the evidence-pack (.zip) reader the offline viewer inlines.
//
//   node --test viewer/zip_core.test.js
//
// The zips are written here, by hand, from the format's own fields (local
// header, central directory, end record) with Node's zlib for the deflate
// member — so the reader is tested against the format, not against itself,
// and against both member methods the integration's zipfile writer can emit.

const { describe, it } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');

const Z = require('./zip_core');

const BUNDLE_PATH = path.join(__dirname, '..', 'tests', 'fixtures', 'export_bundle', 'valid_bundle.json');
const BUNDLE_TEXT = fs.readFileSync(BUNDLE_PATH, 'utf8');
const WITNESS_KEY_TEXT = 'verifying_key=' + 'ab'.repeat(32) + '\nfingerprint=ab12cd34\nkernel=pwk_1a2b3c4d\n';

// ---- a minimal zip writer (test-only) -------------------------------------

function le16(n) { const b = Buffer.alloc(2); b.writeUInt16LE(n); return b; }
function le32(n) { const b = Buffer.alloc(4); b.writeUInt32LE(n >>> 0); return b; }

/** members: [{ name, data: Buffer, deflate: bool, flags?: number, method?: number }] */
function writeZip(members, { comment = '' } = {}) {
  const locals = [];
  const centrals = [];
  let offset = 0;
  for (const m of members) {
    const data = Buffer.isBuffer(m.data) ? m.data : Buffer.from(m.data, 'utf8');
    const method = m.method != null ? m.method : (m.deflate ? 8 : 0);
    const stored = method === 8 ? zlib.deflateRawSync(data) : data;
    const crc = Z.crc32(data);
    const name = Buffer.from(m.name, 'utf8');
    const flags = m.flags || 0;
    const fixed = Buffer.concat([
      le16(20), le16(flags), le16(method), le16(0), le16(0),
      le32(m.badCrc ? (crc ^ 0xffffffff) >>> 0 : crc), le32(stored.length), le32(data.length),
      le16(name.length), le16(0),
    ]);
    const local = Buffer.concat([le32(0x04034b50), fixed, name, stored]);
    const central = Buffer.concat([
      le32(0x02014b50), le16(20), fixed, le16(0), le16(0), le16(0), le32(0), le32(offset), name,
    ]);
    locals.push(local);
    centrals.push(central);
    offset += local.length;
  }
  const cd = Buffer.concat(centrals);
  const commentBuf = Buffer.from(comment, 'utf8');
  const eocd = Buffer.concat([
    le32(0x06054b50), le16(0), le16(0), le16(members.length), le16(members.length),
    le32(cd.length), le32(offset), le16(commentBuf.length), commentBuf,
  ]);
  return new Uint8Array(Buffer.concat([...locals, cd, eocd]));
}

const PACK = [
  { name: 'securacv-events-1760140800.json', data: BUNDLE_TEXT, deflate: true },
  { name: 'evidence_viewer.html', data: '<!doctype html><title>viewer</title>', deflate: true },
  { name: 'README.txt', data: 'Open evidence_viewer.html…', deflate: false },
  { name: 'WITNESS_KEY.txt', data: WITNESS_KEY_TEXT, deflate: false },
];

describe('looksLikeZip', () => {
  it('recognizes a zip by its local-header signature and nothing else', () => {
    assert.equal(Z.looksLikeZip(writeZip(PACK)), true);
    assert.equal(Z.looksLikeZip(new TextEncoder().encode('{"artifact":{}}')), false);
    assert.equal(Z.looksLikeZip(new Uint8Array([0x50, 0x4b])), false);
  });
});

describe('listEntries', () => {
  it('reads the central directory, with a trailing comment in the way', () => {
    const entries = Z.listEntries(writeZip(PACK, { comment: 'written by securacv' }));
    assert.deepEqual(entries.map((e) => e.name), PACK.map((m) => m.name));
    assert.equal(entries[0].method, 8);
    assert.equal(entries[2].method, 0);
    assert.equal(entries[0].size, Buffer.byteLength(BUNDLE_TEXT));
  });

  it('refuses what it cannot read, by name', () => {
    assert.throws(() => Z.listEntries(new TextEncoder().encode('not a zip')), /not a zip file/);
    const truncated = writeZip(PACK).slice(0, 40);
    assert.throws(() => Z.listEntries(truncated), /truncated/);
  });
});

describe('readMember', () => {
  it('returns a stored member byte-for-byte', async () => {
    const zip = writeZip(PACK);
    const entries = Z.listEntries(zip);
    const key = await Z.readMember(zip, entries.find((e) => e.name === 'WITNESS_KEY.txt'));
    assert.equal(new TextDecoder().decode(key), WITNESS_KEY_TEXT);
  });

  it('inflates a deflated member with DecompressionStream and checks the CRC', async () => {
    const zip = writeZip(PACK);
    const entries = Z.listEntries(zip);
    const bundle = await Z.readMember(zip, entries[0]);
    assert.equal(new TextDecoder().decode(bundle), BUNDLE_TEXT);
  });

  it('calls a damaged pack damaged, not "evidence rejected"', async () => {
    const zip = writeZip([{ name: 'securacv-events-1.json', data: BUNDLE_TEXT, deflate: false, badCrc: true }]);
    const entries = Z.listEntries(zip);
    await assert.rejects(Z.readMember(zip, entries[0]), /CRC-32 check — the pack is damaged/);
  });

  it('refuses encrypted members and unknown compression methods', async () => {
    const enc = writeZip([{ name: 'securacv-events-1.json', data: BUNDLE_TEXT, flags: 0x0001 }]);
    await assert.rejects(Z.readMember(enc, Z.listEntries(enc)[0]), /encrypted/);
    const bz = writeZip([{ name: 'securacv-events-1.json', data: BUNDLE_TEXT, method: 12 }]);
    await assert.rejects(Z.readMember(bz, Z.listEntries(bz)[0]), /compression method 12/);
  });
});

describe('pickBundleMember', () => {
  const entry = (name) => ({ name, method: 0, flags: 0, crc32: 0, compressedSize: 0, size: 0, localHeaderOffset: 0 });
  it("prefers the kernel's own filename and never an index", () => {
    const picked = Z.pickBundleMember([entry('index.json'), entry('README.txt'), entry('securacv-events-1760140800.json'), entry('evidence_viewer.html')]);
    assert.equal(picked.name, 'securacv-events-1760140800.json');
    // A lone .json by another name still counts; a directory prefix is fine.
    assert.equal(Z.pickBundleMember([entry('pack/export.json'), entry('pack/README.txt')]).name, 'pack/export.json');
    assert.equal(Z.pickBundleMember([entry('pack/'), entry('pack/securacv-events-5.json')]).name, 'pack/securacv-events-5.json');
  });
  it('refuses to guess between several bundles or none', () => {
    assert.throws(() => Z.pickBundleMember([entry('securacv-events-1.json'), entry('securacv-events-2.json')]), /holds 2 event exports/);
    assert.throws(() => Z.pickBundleMember([entry('a.json'), entry('b.json')]), /holds 2 \.json files/);
    assert.throws(() => Z.pickBundleMember([entry('README.txt'), entry('index.json')]), /no \.json evidence bundle/);
  });
});

describe('parseWitnessKey', () => {
  it('reads the three lines and drops malformed values', () => {
    assert.deepEqual(Z.parseWitnessKey(WITNESS_KEY_TEXT), {
      verifying_key: 'ab'.repeat(32), fingerprint: 'ab12cd34', kernel: 'pwk_1a2b3c4d',
    });
    assert.deepEqual(Z.parseWitnessKey('verifying_key=nope\nfingerprint=zz\nkernel=x\n'), { kernel: 'x' });
    assert.deepEqual(Z.parseWitnessKey(''), {});
  });
});

describe('extractEvidence', () => {
  it('hands back the bundle text verbatim plus the pack key note, for both member methods', async () => {
    for (const deflate of [true, false]) {
      const members = PACK.map((m) => ({ ...m, deflate }));
      const pack = await Z.extractEvidence(writeZip(members));
      assert.equal(pack.memberName, 'securacv-events-1760140800.json');
      assert.equal(pack.text, BUNDLE_TEXT, 'the bundle bytes are exactly what the kernel exported');
      assert.deepEqual(JSON.parse(pack.text), JSON.parse(BUNDLE_TEXT));
      assert.deepEqual(pack.members, PACK.map((m) => m.name));
      assert.equal(pack.witnessKey.fingerprint, 'ab12cd34');
      assert.equal(pack.witnessKey.verifying_key, 'ab'.repeat(32));
    }
  });

  it('works without WITNESS_KEY.txt and names the problem when there is no bundle', async () => {
    const pack = await Z.extractEvidence(writeZip([PACK[0], PACK[2]]));
    assert.equal(pack.witnessKey, null);
    await assert.rejects(Z.extractEvidence(writeZip([PACK[2]])), /no \.json evidence bundle/);
    await assert.rejects(Z.extractEvidence(new TextEncoder().encode('{}')), /not a zip file/);
  });

  it('accepts an ArrayBuffer as well as a Uint8Array', async () => {
    const bytes = writeZip(PACK);
    const pack = await Z.extractEvidence(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
    assert.equal(pack.memberName, 'securacv-events-1760140800.json');
  });
});

describe('the built viewer', () => {
  it('inlines this reader and wires the drop zone to it', () => {
    const html = fs.readFileSync(path.join(__dirname, 'evidence_viewer.html'), 'utf8');
    assert.match(html, /globalThis\.SecuraCVZip = zipApi/);
    assert.match(html, /Z\.looksLikeZip\(bytes\)/);
    assert.match(html, /accept="application\/json,\.json,application\/zip,\.zip"/);
    // The pack's own key is shown, never poured into the trusted-key field.
    assert.match(html, /packKeyNote\(report\.device_public_key\)/);
    assert.doesNotMatch(html, /trustkey'\)\.value = /);
  });
});
