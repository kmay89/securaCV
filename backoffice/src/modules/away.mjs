// Away mode: one date the whole company reads.
//
// A one-person business goes away — a week off, a trade show, the flu — and
// the promises it makes have to follow. The operator sets the day they are
// back (Settings → Away, or `backoffice away 2026-12-01`) and from then on:
//
//   the store      the batch cadence counts from the return day, so the catalog,
//                  the ZIP estimate, Stripe's shipping option and the promise written
//                  on every order say the first batch after it ("Back Mon, Dec 1 ·
//                  ships Tue, Dec 9"), never a Tuesday nobody is there to pack
//   support        a ticket opened meanwhile is acknowledged with the date, and its
//                  SLA clock starts the morning of the return; the SLA job raises no
//                  alarm about a wait the customer was told about
//   the admin      Home carries the strip, the doctor has a row, the digest says it
//
// It ends on its own on the return day (the date is compared, not a flag), and
// "I'm back" ends it early. Nothing about orders, stock or people changes; the
// state is two rows in `settings`, and every set and clear is an audit row.
import { zoneParts, dayLabel } from './storefront.mjs';
import { currentTz, zonedToUtc } from '../render.mjs';
import { sanitize } from '../guard.mjs';

const KEY_UNTIL = 'away:until', KEY_NOTE = 'away:note', KEY_SET_AT = 'away:set_at';
const YMD = /^\d{4}-\d{2}-\d{2}$/;
const D = 86400000;
export const RETURN_HOUR = 9;   // the SLA clock for a ticket opened while away starts at this hour (operator time) on the return day

const pad = (n) => String(n).padStart(2, '0');
export const ymdOf = (p) => `${p.y}-${pad(p.m)}-${pad(p.d)}`;
export const partsOf = (ymd) => { const [y, m, d] = String(ymd).split('-').map(Number); return { y, m, d, weekday: new Date(Date.UTC(y, m - 1, d)).getUTCDay() }; };
export const todayIn = (tz, now = Date.now()) => ymdOf(zoneParts(new Date(now), tz || currentTz()));
const validDate = (s) => YMD.test(s) && (() => { const [y, m, d] = s.split('-').map(Number); const u = new Date(Date.UTC(y, m - 1, d)); return u.getUTCFullYear() === y && u.getUTCMonth() === m - 1 && u.getUTCDate() === d; })();

// Where the away date stands today. `on` only while today is before the return day: the morning of
// that day the store is on its normal cadence again without anyone pressing anything.
export function awayState(db, cfg = {}, { now = Date.now() } = {}) {
  const tz = cfg.tz || currentTz();
  const until = db.setting(KEY_UNTIL, '') || '';
  if (!validDate(until)) return { on: false, until: null, until_label: '', note: '', days_left: 0, expired: false, resume_at: null, set_at: null };
  const today = todayIn(tz, now); const on = until > today;
  const resume = zonedToUtc(`${until}T${pad(RETURN_HOUR)}:00:00`, tz);
  return { on, until, until_label: dayLabel(partsOf(until)), note: on ? db.setting(KEY_NOTE, '') || '' : '', days_left: on ? Math.round((Date.parse(until) - Date.parse(today)) / D) : 0, expired: !on, resume_at: resume, set_at: db.setting(KEY_SET_AT, null) };
}
// What the cadence takes: the return day while away, else nothing.
export const awayUntilFor = (db, cfg = {}, now = Date.now()) => { const a = awayState(db, cfg, { now }); return a.on ? a.until : null; };

export function setAway(db, { until, note = '' } = {}, { actor = 'web', cfg = {}, now = Date.now() } = {}) {
  until = String(until || '').trim();
  if (!validDate(until)) throw new Error('the day you are back is a date, YYYY-MM-DD');
  const today = todayIn(cfg.tz || currentTz(), now);
  if (until <= today) throw new Error(`the day you are back has to be after today (${today})`);
  if (Date.parse(until) - Date.parse(today) > 365 * D) throw new Error('that is more than a year away — set a nearer date and move it later');
  note = sanitize(String(note || ''), { max: 200 }).replace(/\s+/g, ' ').trim();
  db.tx(() => { db.setSetting(KEY_UNTIL, until); db.setSetting(KEY_NOTE, note); db.setSetting(KEY_SET_AT, new Date(now).toISOString()); });
  db.audit(actor, 'away.set', 'settings', 'away', { until, note: note || undefined });
  return awayState(db, cfg, { now });
}

export function clearAway(db, { actor = 'web', cfg = {}, now = Date.now() } = {}) {
  const before = awayState(db, cfg, { now });
  if (!before.until) return before;
  db.run('DELETE FROM settings WHERE key IN (?, ?, ?)', KEY_UNTIL, KEY_NOTE, KEY_SET_AT);
  db.audit(actor, 'away.clear', 'settings', 'away', { was_until: before.until, early: before.on });
  return awayState(db, cfg, { now });
}

// The SLA clock for a ticket opened now: the usual hours from now, or, while away, the same hours
// counted from the morning of the return — the customer is told that date, so the alarm is set to it.
export function slaDueAt(db, cfg = {}, { now = Date.now() } = {}) {
  const hours = Number(cfg.slaHours) || 24; const a = awayState(db, cfg, { now });
  const from = a.on && a.resume_at ? Math.max(now, Date.parse(a.resume_at)) : now;
  return new Date(from + hours * 3600_000).toISOString();
}

// The sentence in the acknowledgment mail about when they will hear back.
export function ackWhen(db, cfg = {}, { now = Date.now() } = {}) {
  const hours = Number(cfg.slaHours) || 24; const a = awayState(db, cfg, { now });
  return a.on ? `we're away until ${a.until_label}${a.note ? ` (${a.note})` : ''}, and you'll hear back within ${hours} hours of being back.` : `you'll hear back within ${hours} hours, usually sooner.`;
}

// Tickets a person owes an answer, for the away row and the home strip.
export const waitingTickets = (db) => db.get("SELECT COUNT(*) AS n FROM tickets WHERE status IN ('open', 'waiting_on_us')").n;
