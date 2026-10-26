// A QR code as inline SVG, so an authenticator app can scan a secret off the
// page without any image file, script or outside service. Byte mode, error
// correction level M, versions 1–12 (up to 287 bytes), the standard masks
// chosen by the standard penalty. ISO/IEC 18004; checked against a
// reference encoder in tests/users.test.mjs.
import { escapeHtml as e } from './http.mjs';

// [total codewords, EC codewords per block, data codewords per block…] for level M.
const VERSIONS = [null,
  [26, 10, [16]], [44, 16, [28]], [70, 26, [44]], [100, 18, [32, 32]], [134, 24, [43, 43]], [172, 16, [27, 27, 27, 27]],
  [196, 18, [31, 31, 31, 31]], [242, 22, [38, 38, 39, 39]], [292, 22, [36, 36, 36, 37, 37]], [346, 26, [43, 43, 43, 43, 44]],
  [404, 30, [50, 51, 51, 51, 51]], [466, 22, [36, 36, 36, 36, 36, 36, 37, 37]]];
const ALIGN = [null, [], [6, 18], [6, 22], [6, 26], [6, 30], [6, 34], [6, 22, 38], [6, 24, 42], [6, 26, 46], [6, 28, 50], [6, 30, 54], [6, 32, 58]];

// GF(256) with the QR polynomial 0x11d.
const EXP = new Uint8Array(512), LOG = new Uint8Array(256);
for (let i = 0, x = 1; i < 255; i++) { EXP[i] = x; LOG[x] = i; x <<= 1; if (x & 0x100) x ^= 0x11d; }
for (let i = 255; i < 512; i++) EXP[i] = EXP[i - 255];
const mul = (a, b) => (a && b) ? EXP[LOG[a] + LOG[b]] : 0;
function ecCodewords(data, n) {
  let gen = [1]; for (let i = 0; i < n; i++) { const next = new Array(gen.length + 1).fill(0); for (let j = 0; j < gen.length; j++) { next[j] ^= gen[j]; next[j + 1] ^= mul(gen[j], EXP[i]); } gen = next; }
  const rem = new Array(n).fill(0).concat(); const buf = [...data, ...new Array(n).fill(0)];
  for (let i = 0; i < data.length; i++) { const c = buf[i]; if (!c) continue; for (let j = 0; j < gen.length; j++) buf[i + j] ^= mul(gen[j], c); }
  return buf.slice(data.length);
}

function bch(value, poly, bits, len) { let v = value << (len - bits); const top = 1 << (len - 1); const pbits = poly.toString(2).length; for (let i = len - 1; i >= len - bits; i--) if (v & (1 << i)) v ^= poly << (i - (pbits - 1)); return (value << (len - bits)) | v; }
const formatBits = (mask) => bch(mask, 0x537, 5, 15) ^ 0x5412;          // level M = 00 in the top two bits
const versionBits = (v) => bch(v, 0x1f25, 6, 18);

export function qrMatrix(text, { mask: forced = null } = {}) {
  const bytes = Buffer.from(String(text), 'utf8');
  let v = 1; for (; v <= 12; v++) { const [total, ec, blocks] = VERSIONS[v]; const cap = total - ec * blocks.length - (v < 10 ? 2 : 3); if (bytes.length <= cap) break; }
  if (v > 12) throw new Error('too long for a QR code here (287 bytes at most)');
  const [total, ecPer, blocks] = VERSIONS[v]; const dataLen = blocks.reduce((a, b) => a + b, 0);
  // bit stream: mode 0100, count, bytes, terminator, pad
  const bits = []; const push = (val, n) => { for (let i = n - 1; i >= 0; i--) bits.push((val >> i) & 1); };
  push(4, 4); push(bytes.length, v < 10 ? 8 : 16); for (const b of bytes) push(b, 8);
  push(0, Math.min(4, dataLen * 8 - bits.length)); while (bits.length % 8) bits.push(0);
  const data = []; for (let i = 0; i < bits.length; i += 8) data.push(parseInt(bits.slice(i, i + 8).join(''), 2));
  for (let p = 0; data.length < dataLen; p++) data.push(p % 2 ? 0x11 : 0xec);
  // blocks and interleave
  const dBlocks = [], eBlocks = []; let off = 0;
  for (const len of blocks) { const d = data.slice(off, off + len); off += len; dBlocks.push(d); eBlocks.push(ecCodewords(d, ecPer)); }
  const out = []; const maxD = Math.max(...blocks);
  for (let i = 0; i < maxD; i++) for (const d of dBlocks) if (i < d.length) out.push(d[i]);
  for (let i = 0; i < ecPer; i++) for (const ecb of eBlocks) out.push(ecb[i]);
  if (out.length !== total) throw new Error('codeword count');
  // the matrix: 1 dark, 0 light, with a parallel "reserved" map for function patterns
  const n = v * 4 + 17; const m = Array.from({ length: n }, () => new Uint8Array(n)); const fixed = Array.from({ length: n }, () => new Uint8Array(n));
  const set = (r, c, val) => { m[r][c] = val; fixed[r][c] = 1; };
  const finder = (r0, c0) => { for (let r = -1; r <= 7; r++) for (let c = -1; c <= 7; c++) { const rr = r0 + r, cc = c0 + c; if (rr < 0 || cc < 0 || rr >= n || cc >= n) continue; const on = r >= 0 && r <= 6 && c >= 0 && c <= 6 && (r === 0 || r === 6 || c === 0 || c === 6 || (r >= 2 && r <= 4 && c >= 2 && c <= 4)); set(rr, cc, on ? 1 : 0); } };
  finder(0, 0); finder(0, n - 7); finder(n - 7, 0);
  for (let i = 8; i < n - 8; i++) { set(6, i, i % 2 === 0 ? 1 : 0); set(i, 6, i % 2 === 0 ? 1 : 0); }
  for (const r of ALIGN[v]) for (const c of ALIGN[v]) { if ((r <= 8 && c <= 8) || (r <= 8 && c >= n - 9) || (r >= n - 9 && c <= 8)) continue; /* the three that would sit on a finder */ for (let dr = -2; dr <= 2; dr++) for (let dc = -2; dc <= 2; dc++) set(r + dr, c + dc, Math.max(Math.abs(dr), Math.abs(dc)) !== 1 ? 1 : 0); }
  // reserve format areas (+ the dark module) and version areas
  for (let i = 0; i < 9; i++) { if (i !== 6) { fixed[8][i] = 1; fixed[i][8] = 1; } } for (let i = 0; i < 8; i++) { fixed[8][n - 1 - i] = 1; fixed[n - 1 - i][8] = 1; } set(n - 8, 8, 1);
  if (v >= 7) for (let i = 0; i < 6; i++) for (let j = 0; j < 3; j++) { fixed[i][n - 11 + j] = 1; fixed[n - 11 + j][i] = 1; }
  // place data: two-column zigzag from the right, skipping column 6
  const stream = []; for (const b of out) for (let i = 7; i >= 0; i--) stream.push((b >> i) & 1);
  let k = 0, up = true;
  for (let c = n - 1; c > 0; c -= 2) { if (c === 6) c--; for (let i = 0; i < n; i++) { const r = up ? n - 1 - i : i; for (const cc of [c, c - 1]) { if (fixed[r][cc]) continue; m[r][cc] = k < stream.length ? stream[k] : 0; k++; } } up = !up; }
  // masks: pick the one with the lowest penalty
  const MASKS = [(r, c) => (r + c) % 2 === 0, (r) => r % 2 === 0, (r, c) => c % 3 === 0, (r, c) => (r + c) % 3 === 0, (r, c) => (Math.floor(r / 2) + Math.floor(c / 3)) % 2 === 0, (r, c) => (r * c) % 2 + (r * c) % 3 === 0, (r, c) => ((r * c) % 2 + (r * c) % 3) % 2 === 0, (r, c) => ((r + c) % 2 + (r * c) % 3) % 2 === 0];
  const apply = (mask) => { const g = m.map(row => Uint8Array.from(row)); for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) if (!fixed[r][c] && MASKS[mask](r, c)) g[r][c] ^= 1; writeFormat(g, n, mask); if (v >= 7) writeVersion(g, n, v); return g; };
  let best = null, bestScore = Infinity;
  for (let mask = 0; mask < 8; mask++) { if (forced != null && mask !== forced) continue; const g = apply(mask); const s = penalty(g, n); if (s < bestScore) { bestScore = s; best = g; } }
  return best;
}
function writeFormat(g, n, mask) {
  const f = formatBits(mask); const bit = (i) => (f >> i) & 1;
  for (let i = 0; i < 6; i++) g[8][i] = bit(14 - i); g[8][7] = bit(8); g[8][8] = bit(7); g[7][8] = bit(6); for (let i = 0; i < 6; i++) g[5 - i][8] = bit(5 - i);
  for (let i = 0; i < 8; i++) g[n - 1 - i][8] = bit(14 - i); for (let i = 0; i < 8; i++) g[8][n - 8 + i] = bit(7 - i);
  g[n - 8][8] = 1;
}
function writeVersion(g, n, v) { const b = versionBits(v); for (let i = 0; i < 18; i++) { const bit = (b >> i) & 1; g[Math.floor(i / 3)][n - 11 + (i % 3)] = bit; g[n - 11 + (i % 3)][Math.floor(i / 3)] = bit; } }
export const _penalty = (g, n) => penalty(g, n);
function penalty(g, n) {
  let s = 0;
  for (let r = 0; r < n; r++) for (let c = 0, run = 0; c <= n; c++) { if (c < n && g[r][c] === (c ? g[r][c - 1] : g[r][0]) && c) run++; else { if (run >= 5) s += run - 2; run = 1; } }
  for (let c = 0; c < n; c++) for (let r = 0, run = 0; r <= n; r++) { if (r < n && r && g[r][c] === g[r - 1][c]) run++; else { if (run >= 5) s += run - 2; run = 1; } }
  for (let r = 0; r < n - 1; r++) for (let c = 0; c < n - 1; c++) { const x = g[r][c]; if (g[r][c + 1] === x && g[r + 1][c] === x && g[r + 1][c + 1] === x) s += 3; }
  const P1 = [1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0], P2 = [0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1];
  const line = (get) => { for (let i = 0; i <= n - 11; i++) { let a = true, b = true; for (let j = 0; j < 11; j++) { const x = get(i + j); if (x !== P1[j]) a = false; if (x !== P2[j]) b = false; } if (a || b) s += 40; } };
  for (let r = 0; r < n; r++) line((i) => g[r][i]); for (let c = 0; c < n; c++) line((i) => g[i][c]);
  let dark = 0; for (const row of g) for (const x of row) dark += x; const pct = (dark * 100) / (n * n); const k = Math.floor(Math.abs(pct - 50) / 5); s += k * 10;
  return s;
}
export function qrSvg(text, { size = 200, label = '' } = {}) {
  const g = qrMatrix(text); const n = g.length; const q = 4; const W = n + 2 * q;
  let d = ''; for (let r = 0; r < n; r++) for (let c = 0; c < n; c++) if (g[r][c]) d += `M${c + q} ${r + q}h1v1h-1z`;
  return `<svg viewBox="0 0 ${W} ${W}" width="${size}" height="${size}" role="img" aria-label="${e(label || 'QR code')}" shape-rendering="crispEdges" style="background:#fff;border-radius:8px"><rect width="${W}" height="${W}" fill="#fff"/><path d="${d}" fill="#000"/></svg>`;
}
