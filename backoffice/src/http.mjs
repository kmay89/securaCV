// The smallest HTTP toolkit that keeps us honest: a router with :params,
// body parsing with a size cap, cookies, CSRF, a per-IP rate limiter and
// HTML escaping. No framework — the whole thing is readable in one sitting.
import { randomBytes, timingSafeEqual } from 'node:crypto';
import { createHash } from 'node:crypto';

// The one script the admin ships: on a phone, scroll the current section's link into view in the
// top strip. Inline, so it needs no second request — and allowed by hash, so the policy stays
// "no script" for everything else; a changed script changes the hash and allows itself.
export const NAV_SCRIPT = '(function(){var a=document.querySelector("nav a.on");if(a&&matchMedia("(max-width:799px)").matches)a.scrollIntoView({inline:"center",block:"nearest"})})()';
const NAV_SCRIPT_HASH = `'sha256-${createHash('sha256').update(NAV_SCRIPT).digest('base64')}'`;

export const escapeHtml = (s) => String(s ?? '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');

export class HttpError extends Error {
  constructor(status, message, extra = {}) { super(message); this.status = status; Object.assign(this, extra); }
}

export function parseCookies(req) {
  const out = {};
  for (const part of (req.headers.cookie || '').split(';')) {
    const i = part.indexOf('='); if (i < 0) continue;
    out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
  }
  return out;
}

// SameSite=Lax, not Strict: a link the phone's camera hands to the browser (the slip's code, a
// board's label) is a top-level navigation from outside the site, and Strict withholds the
// session on exactly that hop — the scan would land on the login page. Lax sends the cookie on
// top-level GETs only; every write is a POST under the CSRF token and the cross-site refusal.
export function cookie(name, value, { maxAge = 0, secure = false, path = '/' } = {}) {
  const bits = [`${name}=${encodeURIComponent(value)}`, `Path=${path}`, 'HttpOnly', 'SameSite=Lax'];
  if (secure) bits.push('Secure');
  if (maxAge) bits.push(`Max-Age=${maxAge}`);
  if (maxAge < 0) bits.push('Max-Age=0');
  return bits.join('; ');
}

const MAX_BODY = 1 << 20; // 1 MiB

export function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = []; let size = 0;
    req.on('data', c => { size += c.length; if (size > MAX_BODY) { reject(new HttpError(413, 'body too large')); req.destroy(); } else chunks.push(c); });
    req.on('end', () => resolve(Buffer.concat(chunks)));
    req.on('error', reject);
  });
}

export function parseBody(raw, contentType = '') {
  if (!raw || !raw.length) return {};
  const ct = contentType.split(';')[0].trim().toLowerCase();
  if (ct === 'application/json') { let v; try { v = JSON.parse(raw.toString('utf8')); } catch { throw new HttpError(400, 'bad JSON'); } return v && typeof v === 'object' ? v : {}; }   // `null`, a number or a string is not a body a handler can read fields from
  if (ct === 'application/x-www-form-urlencoded' || ct === '') {
    const out = Object.create(null);
    for (const [k, v] of new URLSearchParams(raw.toString('utf8'))) {
      if (k === '__proto__' || k === 'constructor' || k === 'prototype') continue;
      if (k.endsWith('[]')) (out[k.slice(0, -2)] ||= []).push(v); else out[k] = v;
    }
    return out;
  }
  return { _raw: raw.toString('utf8') };
}

// --- a lockout: after N failures in the window the key is refused until it clears (login).
export class Lockout {
  constructor({ limit = 8, windowMs = 15 * 60_000 } = {}) { this.limit = limit; this.windowMs = windowMs; this.fails = new Map(); }
  locked(key, now = Date.now()) { const arr = (this.fails.get(key) || []).filter(t => now - t < this.windowMs); this.fails.set(key, arr); return arr.length >= this.limit; }
  fail(key, now = Date.now()) { const arr = (this.fails.get(key) || []).filter(t => now - t < this.windowMs); arr.push(now); this.fails.set(key, arr); if (this.fails.size > 10_000) this.fails.clear(); return arr.length; }
  clear(key) { this.fails.delete(key); }
}

// --- rate limiter: a sliding window per key, in memory. Public endpoints only.
export class RateLimiter {
  constructor({ limit = 20, windowMs = 60_000 } = {}) { this.limit = limit; this.windowMs = windowMs; this.hits = new Map(); }
  allow(key, now = Date.now()) {
    const arr = (this.hits.get(key) || []).filter(t => now - t < this.windowMs);
    if (arr.length >= this.limit) { this.hits.set(key, arr); return false; }
    arr.push(now); this.hits.set(key, arr);
    if (this.hits.size > 10_000) this.hits.clear(); // never grow without bound
    return true;
  }
}

export const token = (n = 24) => randomBytes(n).toString('base64url');

// A whole number from a form field or a JSON body, or a clear refusal. NaN binds to SQLite as
// NULL (a NOT NULL error, a 500), 1.5 lands in an INTEGER column as a REAL, Infinity as itself:
// none of them may reach a row. `fallback` answers an empty field; otherwise empty is refused too.
export function int(v, { name = 'value', min = -Infinity, max = Infinity, fallback } = {}) {
  if ((v === '' || v == null) && fallback !== undefined) return fallback;
  const n = typeof v === 'number' ? v : Number(String(v ?? '').trim());
  if (String(v ?? '').trim() === '' || !Number.isInteger(n)) throw new HttpError(400, `${name} must be a whole number${v == null || String(v).trim() === '' ? '' : ` (got ${JSON.stringify(String(v)).slice(0, 40)})`}`);
  if (n < min) throw new HttpError(400, `${name} must be at least ${min}`);
  if (n > max) throw new HttpError(400, `${name} must be at most ${max}`);
  return n;
}

// A JSON body field that must be a list (lines on an order, items on a purchase order).
export function asArray(v, name = 'items') { if (!Array.isArray(v)) throw new HttpError(400, `${name} must be an array`); return v; }

// One CSV cell: quoted when it holds a comma, a quote or a line break, and a leading =, +, -, @,
// tab or CR prefixed with an apostrophe, so a spreadsheet never runs a customer's name as a
// formula (CSV injection). A plain negative number keeps its sign.
export const csvCell = (v) => { let s = String(v ?? ''); if (/^[=+\-@\t\r]/.test(s) && !/^-?\d+(\.\d+)?$/.test(s)) s = `'${s}`; return /[",\n\r]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s; };
export const csvLine = (cells) => cells.map(csvCell).join(',');
export const safeEqual = (a, b) => { const x = Buffer.from(String(a)), y = Buffer.from(String(b)); return x.length === y.length && timingSafeEqual(x, y); };

// --- router
export class Router {
  constructor() { this.routes = []; }
  add(method, pattern, handler, opts = {}) {
    const keys = []; const re = new RegExp('^' + pattern.replace(/\/:([a-zA-Z_]+)/g, (_, k) => { keys.push(k); return '/([^/]+)'; }) + '/?$');
    this.routes.push({ method, re, keys, handler, opts });
    return this;
  }
  get(p, h, o) { return this.add('GET', p, h, o); }
  post(p, h, o) { return this.add('POST', p, h, o); }
  match(method, path) {
    for (const r of this.routes) {
      if (r.method !== method && !(r.method === 'GET' && method === 'HEAD')) continue;
      const m = r.re.exec(path); if (!m) continue;
      const params = {}; try { r.keys.forEach((k, i) => { params[k] = decodeURIComponent(m[i + 1]); }); } catch { throw new HttpError(400, 'a path segment is not valid percent-encoding'); }
      return { ...r, params };
    }
    return null;
  }
}

// --- responses
export function send(res, status, body, headers = {}) {
  const buf = typeof body === 'string' ? Buffer.from(body) : body;
  const { 'Content-Type': contentType = 'text/plain; charset=utf-8', ...rest } = headers;
  res.setHeader('Content-Type', contentType);
  res.writeHead(status, { 'Content-Length': buf.length, 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'X-Frame-Options': 'DENY', 'Permissions-Policy': 'camera=(), microphone=(), geolocation=(), payment=(), usb=()', 'Cross-Origin-Opener-Policy': 'same-origin', 'Cross-Origin-Resource-Policy': 'same-origin', 'X-Robots-Tag': 'noindex, nofollow, noarchive, noai, noimageai', ...rest });
  res.end(buf);
}
// Admin pages are never cached by a browser or a proxy; a public page passes its own Cache-Control.
export const html = (res, body, status = 200, headers = {}) => send(res, status, body, { 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store', 'Content-Security-Policy': `default-src 'none'; script-src ${NAV_SCRIPT_HASH}; style-src 'unsafe-inline'; img-src 'self' data:; frame-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'`, ...headers });
export const json = (res, obj, status = 200, headers = {}) => send(res, status, JSON.stringify(obj, null, 2), { 'Content-Type': 'application/json; charset=utf-8', ...headers });
export const text = (res, body, status = 200, headers = {}) => send(res, status, body, { 'Content-Type': 'text/plain; charset=utf-8', ...headers });
export const redirect = (res, to, headers = {}) => { res.writeHead(303, { Location: to, ...headers }); res.end(); };

// X-Forwarded-For is believed only from a proxy we run: loopback (Caddy on the same box), or the
// private ranges (Caddy in its own container on the compose network — the kit's shape; the app
// publishes no port, so nothing else can reach it from there). TRUSTED_PROXIES narrows or widens it:
// a comma list of `loopback`, `private`, exact addresses or v4 CIDRs. Without this every visitor
// behind the kit's Caddy shared one address, and one bad actor's failures locked everyone out.
let TRUSTED = ['loopback', 'private'];
export const setTrustedProxies = (list) => { TRUSTED = Array.isArray(list) ? list : String(list || '').split(',').map(s => s.trim()).filter(Boolean); };
const v4 = (ip) => { const m = /^(?:::ffff:)?(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(ip); return m ? ((+m[1] << 24) | (+m[2] << 16) | (+m[3] << 8) | +m[4]) >>> 0 : null; };
export function isTrustedProxy(peer, list = TRUSTED) {
  const n = v4(peer);
  for (const rule of list) {
    if (rule === 'loopback' && (peer === '::1' || (n != null && n >>> 24 === 127))) return true;
    if (rule === 'private' && (n != null ? (n >>> 24 === 10 || (n >>> 20) === 0xac1 || (n >>> 16) === 0xc0a8) : /^f[cd]/i.test(peer))) return true;
    if (rule === peer || rule === peer.replace(/^::ffff:/, '')) return true;
    const cidr = /^(\d+\.\d+\.\d+\.\d+)\/(\d+)$/.exec(rule);
    if (cidr && n != null) { const base = v4(cidr[1]); const bits = +cidr[2]; const mask = bits === 0 ? 0 : (~0 << (32 - bits)) >>> 0; if (base != null && (n & mask) === (base & mask)) return true; }
  }
  return false;
}
export function clientIp(req) {
  const peer = req.socket?.remoteAddress || '';
  const fwd = req.headers['x-forwarded-for'];
  if (fwd && isTrustedProxy(peer)) return String(fwd).split(',')[0].trim() || peer;
  return peer;
}
