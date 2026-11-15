// Every outbound call the backoffice makes (Stripe, USPS, the backup bucket, the site's catalog) goes
// through here, so none of them can hang a job or a request forever: a call that gives no signal of its
// own is abandoned after 20 s. A caller that knows its call is long (a backup upload) passes a longer
// signal. Tests pass their own fetchImpl and never reach the network.
export const timedFetch = (url, init = {}) => fetch(url, { ...init, signal: init.signal ?? AbortSignal.timeout(20_000) });

// Is this address one nothing on the open internet can be? Loopback, unspecified, the private and shared
// (CGNAT) ranges, link-local, the cloud metadata range (169.254/16 is link-local), documentation, benchmarking,
// multicast and reserved — for v4; and for v6 the same judged on the v4 a mapped (::ffff:a.b.c.d), NAT64
// (64:ff9b::/96), 6to4 (2002::/16) or v4-compatible (::a.b.c.d) address carries, plus loopback and unspecified,
// ULA (fc00::/7), link- and site-local, documentation (2001:db8::/32), multicast and the local-use NAT64 block.
// A string that is not an address is answered true: nothing proves it public. Brackets and a zone id are
// tolerated. The probe refuses a URL whose host is one of these, and the status job checks the resolved
// address again at connect time, so a name that resolves to the inside gets no further than a literal would.
import { isIP } from 'node:net';
import { expand6 } from './guard.mjs';
const v4n = (s) => { const m = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(s); if (!m) return null; const p = m.slice(1).map(Number); return p.every(n => n <= 255) ? ((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]) >>> 0 : null; };
const private4 = (n) => { const a = n >>> 24, b = (n >>> 16) & 255, c = (n >>> 8) & 255; return a === 0 || a === 10 || a === 127 || (a === 100 && b >= 64 && b <= 127) || (a === 169 && b === 254) || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 0 && c === 0) || (a === 192 && b === 168) || (a === 198 && (b === 18 || b === 19)) || a >= 224; };
export function isPrivateAddress(ip) {
  const s = String(ip ?? '').trim().replace(/^\[|\]$/g, '').replace(/%.*$/, '');
  const v = isIP(s);
  if (v === 4) { const n = v4n(s); return n == null || private4(n); }
  if (v !== 6) return true;
  const x = expand6(s); if (!x) return true;
  const w = x.split(':').map(h => parseInt(h, 16)); const embedded = (i) => private4((((w[i] << 16) | w[i + 1]) >>> 0));
  if (w[0] === 0 && w[1] === 0 && w[2] === 0 && w[3] === 0 && w[4] === 0) return w[5] === 0xffff ? embedded(6) : true;   // ::ffff:a.b.c.d by the v4 rules; ::, ::1, ::a.b.c.d never public
  if (w[0] === 0x64 && w[1] === 0xff9b) return w[2] === 0 && w[3] === 0 && w[4] === 0 && w[5] === 0 ? embedded(6) : true;   // NAT64: the v4 inside it; 64:ff9b:1::/48 is local
  if (w[0] === 0x2002) return embedded(1);                                     // 6to4: the v4 inside it
  if ((w[0] & 0xfe00) === 0xfc00 || (w[0] & 0xffc0) === 0xfe80 || (w[0] & 0xffc0) === 0xfec0 || (w[0] & 0xff00) === 0xff00) return true;   // ULA, link-local, site-local, multicast
  return w[0] === 0x2001 && w[1] === 0xdb8;                                     // documentation
}

// Every body read from another server (a probe's page, the catalog, a Stripe or Shippo answer) comes through
// here, so a peer that streams forever cannot fill memory: past maxBytes the body is abandoned and the read
// throws. A stand-in response in a test (no stream) is read whole.
export async function readCapped(response, maxBytes = 2 * 1024 * 1024) {
  if (typeof response?.body?.getReader !== 'function') return String(await response.text());
  const reader = response.body.getReader(); const chunks = []; let n = 0;
  for (;;) {
    const { done, value } = await reader.read(); if (done) break;
    n += value.byteLength; if (n > maxBytes) { reader.cancel().catch(() => {}); throw new Error(`response body over ${maxBytes} bytes — abandoned`); }
    chunks.push(value);
  }
  return Buffer.concat(chunks).toString('utf8');
}
export const readJsonCapped = async (response, maxBytes) => typeof response?.body?.getReader === 'function' ? JSON.parse(await readCapped(response, maxBytes)) : response.json();
