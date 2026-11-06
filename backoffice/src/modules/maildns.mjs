// The mail domain, as the receiving side sees it.
//
// "Why does our mail land in spam?" is the one mail question a small company
// cannot answer from inside its own relay: Gmail and Outlook decide by the
// domain's public DNS. Three records settle most of it — an SPF TXT that names
// the relay, a DMARC TXT at _dmarc.<domain> with a policy, and an MX so replies
// and bounces have somewhere to go. DKIM is the relay's own key under a selector
// we cannot guess, so it is named in the fix and not checked.
//
// The `maildns` job looks every six hours and leaves the answer in settings
// (`mail_dns:last`); the doctor reads that, so a Health page never waits on DNS.
// The resolver is injectable, so the tests need no network.
import { promises as dnsPromises } from 'node:dns';
import { nowIso } from '../db.mjs';

const KEY = 'mail_dns:last';
const TIMEOUT_MS = 5000;

// The domain customers see mail from: MAIL_FROM ("Errer Labs <hello@errer.test>") first, else SUPPORT_ADDR.
export function mailDomainOf(cfg = {}) {
  const m = String(cfg.mailFrom || cfg.supportAddr || '').match(/@([A-Za-z0-9.-]+\.[A-Za-z]{2,})\s*>?\s*$/);
  return m ? m[1].toLowerCase() : null;
}

const notFound = (e) => ['ENOTFOUND', 'ENODATA', 'ESERVFAIL'].includes(e?.code);
const txt = async (resolver, name) => { try { return (await resolver.resolveTxt(name)).map(parts => parts.join('')); } catch (e) { if (notFound(e)) return []; throw e; } };

export async function checkMailDomain(cfg, { resolver = null } = {}) {
  const domain = mailDomainOf(cfg);
  if (!domain) return { domain: null, ok: null, error: 'no MAIL_FROM or SUPPORT_ADDR', at: nowIso() };
  const r = resolver || new dnsPromises.Resolver({ timeout: TIMEOUT_MS, tries: 1 });
  const out = { domain, at: nowIso(), spf: { found: false, record: null }, dmarc: { found: false, policy: null, record: null }, mx: { found: false, hosts: [] }, problems: [], ok: null, error: null };
  try {
    const spf = (await txt(r, domain)).find(s => /^v=spf1(\s|$)/i.test(s));
    if (spf) out.spf = { found: true, record: spf.slice(0, 300) };
    const dmarc = (await txt(r, `_dmarc.${domain}`)).find(s => /^v=DMARC1(\s|;|$)/i.test(s));
    if (dmarc) out.dmarc = { found: true, policy: (dmarc.match(/(?:^|;)\s*p=([a-z]+)/i) || [])[1]?.toLowerCase() || null, record: dmarc.slice(0, 300) };
    let mx = []; try { mx = await r.resolveMx(domain); } catch (e) { if (!notFound(e)) throw e; }
    out.mx = { found: mx.length > 0, hosts: mx.sort((a, b) => a.priority - b.priority).map(x => x.exchange).slice(0, 5) };
  } catch (e) { out.error = `${e.code || 'error'}: ${String(e.message || e).slice(0, 120)}`; return out; }
  if (!out.spf.found) out.problems.push('no SPF record: receivers cannot tell your relay from a forger');
  else if (/\+all/.test(out.spf.record)) out.problems.push('SPF ends in +all, which allows everyone — use -all or ~all');
  if (!out.dmarc.found) out.problems.push('no DMARC record: Gmail and Yahoo require one for bulk senders, and it is what tells receivers to trust SPF/DKIM');
  else if (out.dmarc.policy === 'none') out.problems.push('DMARC policy is p=none — fine while watching reports; move to p=quarantine once the relay is the only sender');
  if (!out.mx.found) out.problems.push('no MX record: replies and bounces to this domain have nowhere to go');
  out.ok = out.spf.found && out.dmarc.found && out.mx.found;
  return out;
}

export const lastMailDomainCheck = (db) => { try { return JSON.parse(db.setting(KEY) || 'null'); } catch { return null; } };
export const rememberMailDomainCheck = (db, result) => { db.setSetting(KEY, JSON.stringify(result)); return result; };
