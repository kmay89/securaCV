// Discount codes. Stripe applies a promotion code at checkout; this is the
// book of record for what each code is, when it runs, how often it was used
// and what it cost (the `income:discounts` contra account). A manual order
// or the API can apply one too, with the same rules.
import { nowIso } from '../db.mjs';
import { currentTz } from '../render.mjs';

export const KINDS = ['percent', 'fixed', 'free_shipping'];
export const normCode = (s) => String(s || '').trim().toUpperCase();

export function createDiscount(db, { code, kind, value = 0, min_subtotal_cents = 0, starts_at = null, ends_at = null, max_uses = null, note = null }) {
  const c = normCode(code);
  if (!/^[A-Z0-9][A-Z0-9_-]{1,29}$/.test(c)) throw new Error('a code is 2–30 letters, digits, - or _');
  if (!KINDS.includes(kind)) throw new Error(`kind must be one of ${KINDS.join(', ')}`);
  const v = Number(value) || 0;
  if (kind === 'percent' && (v < 1 || v > 100)) throw new Error('a percent discount is 1–100');
  if (kind === 'fixed' && v < 1) throw new Error('a fixed discount is at least 1 cent');
  if (db.get('SELECT code FROM discounts WHERE code = ?', c)) throw new Error(`${c} already exists`);
  db.run('INSERT INTO discounts (code, kind, value, min_subtotal_cents, starts_at, ends_at, max_uses, status, note, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, \'active\', ?, ?)',
    c, kind, kind === 'free_shipping' ? 0 : v, Number(min_subtotal_cents) || 0, starts_at || null, ends_at || null, max_uses ? Number(max_uses) : null, note || null, nowIso());
  db.audit('web', 'discount.create', 'discount', c, { kind, value: v });
  return discountByCode(db, c);
}
export const discountByCode = (db, code) => db.get('SELECT * FROM discounts WHERE code = ?', normCode(code));
export const listDiscounts = (db) => db.all(`SELECT d.*, (SELECT COUNT(*) FROM orders o WHERE o.discount_code = d.code AND o.status != 'canceled') AS uses,
  (SELECT COALESCE(SUM(o.discount_cents),0) FROM orders o WHERE o.discount_code = d.code AND o.status != 'canceled') AS given_cents,
  (SELECT COALESCE(SUM(o.total_cents - o.refunded_cents),0) FROM orders o WHERE o.discount_code = d.code AND o.status != 'canceled') AS sales_cents
  FROM discounts d ORDER BY d.status, d.created_at DESC`);
export function setDiscountStatus(db, code, status) {
  if (!['active', 'disabled'].includes(status)) throw new Error('status is active or disabled');
  const c = normCode(code); if (!db.get('SELECT code FROM discounts WHERE code = ?', c)) throw new Error(`no such code ${c}`);
  db.run('UPDATE discounts SET status = ? WHERE code = ?', status, c); db.audit('web', `discount.${status}`, 'discount', c);
  return discountByCode(db, c);
}
export const describe = (d) => d.kind === 'percent' ? `${d.value}% off` : d.kind === 'fixed' ? `$${(d.value / 100).toFixed(2)} off` : 'free shipping';

// The rules, in one place: a code counts only while active, inside its window,
// over its minimum and under its use cap. The answer carries the reason, so
// the form can say why — never a silent zero.
// A window given as a date ("ends 2026-11-30") is that whole calendar day where the operator is: the code
// works through the last minute of the 30th in OPERATOR_TZ, not until midnight UTC the night before. A
// window given with a time is compared as the instant it names.
const localDay = (iso, tz) => { try { const p = Object.fromEntries(new Intl.DateTimeFormat('en-US', { timeZone: tz || 'UTC', year: 'numeric', month: '2-digit', day: '2-digit' }).formatToParts(new Date(iso)).map(x => [x.type, x.value])); return `${p.year}-${p.month}-${p.day}`; } catch { return String(iso).slice(0, 10); } };
const dateOnly = (v) => /^\d{4}-\d{2}-\d{2}$/.test(String(v));
export function evaluate(db, code, { subtotal_cents = 0, shipping_cents = 0, at = nowIso(), tz = currentTz() } = {}) {
  const d = discountByCode(db, code);
  if (!d) return { ok: false, reason: 'unknown code', discount_cents: 0 };
  if (d.status !== 'active') return { ok: false, reason: 'code is disabled', discount_cents: 0, discount: d };
  const day = localDay(at, tz);
  if (d.starts_at && (dateOnly(d.starts_at) ? day < d.starts_at : at < d.starts_at)) return { ok: false, reason: `starts ${d.starts_at.slice(0, 10)}`, discount_cents: 0, discount: d };
  if (d.ends_at && (dateOnly(d.ends_at) ? day > d.ends_at : at > d.ends_at)) return { ok: false, reason: `ended ${d.ends_at.slice(0, 10)}`, discount_cents: 0, discount: d };
  if (subtotal_cents < d.min_subtotal_cents) return { ok: false, reason: `needs a subtotal of $${(d.min_subtotal_cents / 100).toFixed(2)}`, discount_cents: 0, discount: d };
  if (d.max_uses != null) { const used = db.get('SELECT COUNT(*) AS n FROM orders WHERE discount_code = ? AND status != \'canceled\'', d.code).n; if (used >= d.max_uses) return { ok: false, reason: `used ${used} of ${d.max_uses} times`, discount_cents: 0, discount: d }; }
  const cents = d.kind === 'percent' ? Math.round(subtotal_cents * d.value / 100) : d.kind === 'fixed' ? Math.min(d.value, subtotal_cents) : shipping_cents;
  return { ok: true, reason: describe(d), discount_cents: cents, free_shipping: d.kind === 'free_shipping', discount: d };
}
