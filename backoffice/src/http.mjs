// The smallest HTTP toolkit that keeps us honest: a router with :params,
// body parsing with a size cap, cookies, CSRF, a per-IP rate limiter and
// HTML escaping. No framework — the whole thing is readable in one sitting.
import { randomBytes, timingSafeEqual } from 'node:crypto';

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

export function cookie(name, value, { maxAge = 0, secure = false, path = '/' } = {}) {
  const bits = [`${name}=${encodeURIComponent(value)}`, `Path=${path}`, 'HttpOnly', 'SameSite=Strict'];
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
  if (ct === 'application/json') { try { return JSON.parse(raw.toString('utf8')); } catch { throw new HttpError(400, 'bad JSON'); } }
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
      const params = {}; r.keys.forEach((k, i) => { params[k] = decodeURIComponent(m[i + 1]); });
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
  res.writeHead(status, { 'Content-Length': buf.length, 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'X-Frame-Options': 'DENY', ...rest });
  res.end(buf);
}
export const html = (res, body, status = 200, headers = {}) => send(res, status, body, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Security-Policy': "default-src 'none'; style-src 'unsafe-inline'; img-src 'self' data:; form-action 'self'; base-uri 'none'; frame-ancestors 'none'", ...headers });
export const json = (res, obj, status = 200, headers = {}) => send(res, status, JSON.stringify(obj, null, 2), { 'Content-Type': 'application/json; charset=utf-8', ...headers });
export const text = (res, body, status = 200, headers = {}) => send(res, status, body, { 'Content-Type': 'text/plain; charset=utf-8', ...headers });
export const redirect = (res, to, headers = {}) => { res.writeHead(303, { Location: to, ...headers }); res.end(); };

export function clientIp(req) {
  // Trust X-Forwarded-For only from a loopback proxy (Caddy/nginx on the same box).
  const peer = req.socket?.remoteAddress || '';
  const fwd = req.headers['x-forwarded-for'];
  if (fwd && (peer === '127.0.0.1' || peer === '::1' || peer === '::ffff:127.0.0.1')) return String(fwd).split(',')[0].trim();
  return peer;
}
