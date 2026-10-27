// Doc 20, principle 4: exceptions summon humans; schedules never do. A
// system that crosses a threshold raises a keyed exception; the first raise
// opens a deduplicated GitHub issue (when a token is configured) and every
// later raise just bumps last_seen. Resolution comments and closes it.
// Silence means healthy. The inbox of the company is the issue tracker.
import { nowIso } from '../db.mjs';
import { timedFetch } from '../net.mjs';

export async function raise(db, cfg, key, title, body, { fetchImpl = timedFetch } = {}) {
  const now = nowIso();
  const existing = db.get('SELECT * FROM exceptions WHERE key = ?', key);
  if (existing && !existing.resolved_at) { db.run('UPDATE exceptions SET last_seen_at = ?, body = ? WHERE key = ?', now, body, key); return { ...existing, bumped: true }; }
  let issue = null;
  // The issue is a summons, not a record: customer emails are redacted before the body leaves the box (the full
  // text stays in the exceptions table here), and nothing is posted unless a repository is named.
  if (cfg.githubToken && cfg.githubRepo) {
    const redact = (s) => String(s).replace(/[\w.+-]+@[\w-]+(?:\.[\w-]+)+/g, '[email]');
    try {
      const r = await fetchImpl(`https://api.github.com/repos/${cfg.githubRepo}/issues`, { method: 'POST', headers: gh(cfg), body: JSON.stringify({ title: redact(title), body: `${redact(body)}\n\n---\n_Opened by the backoffice (\`${key}\`). It closes itself when the condition clears._`, labels: ['backoffice', 'exception'] }) });
      if (r.ok) issue = (await r.json()).number; else db.audit('system', 'exception.github.failed', 'exception', key, { status: r.status });
    } catch (e) { db.audit('system', 'exception.github.failed', 'exception', key, { error: String(e.message || e) }); }
  }
  db.run('INSERT INTO exceptions (key, title, body, github_issue, opened_at, resolved_at, last_seen_at) VALUES (?, ?, ?, ?, ?, NULL, ?) ON CONFLICT(key) DO UPDATE SET title = excluded.title, body = excluded.body, github_issue = excluded.github_issue, opened_at = excluded.opened_at, resolved_at = NULL, last_seen_at = excluded.last_seen_at',
    key, title, body, issue, now, now);
  db.audit('system', 'exception.raise', 'exception', key, { issue });
  return db.get('SELECT * FROM exceptions WHERE key = ?', key);
}

export async function resolve(db, cfg, key, note = 'Condition cleared.', { fetchImpl = timedFetch } = {}) {
  const e = db.get('SELECT * FROM exceptions WHERE key = ? AND resolved_at IS NULL', key);
  if (!e) return null;
  db.run('UPDATE exceptions SET resolved_at = ? WHERE key = ?', nowIso(), key);
  if (e.github_issue && cfg.githubToken) {
    try {
      await fetchImpl(`https://api.github.com/repos/${cfg.githubRepo}/issues/${e.github_issue}/comments`, { method: 'POST', headers: gh(cfg), body: JSON.stringify({ body: `${note}\n\n_Closed by the backoffice._` }) });
      await fetchImpl(`https://api.github.com/repos/${cfg.githubRepo}/issues/${e.github_issue}`, { method: 'PATCH', headers: gh(cfg), body: JSON.stringify({ state: 'closed', state_reason: 'completed' }) });
    } catch (err) { db.audit('system', 'exception.github.failed', 'exception', key, { error: String(err.message || err) }); }
  }
  db.audit('system', 'exception.resolve', 'exception', key);
  return db.get('SELECT * FROM exceptions WHERE key = ?', key);
}

const gh = (cfg) => ({ authorization: `Bearer ${cfg.githubToken}`, accept: 'application/vnd.github+json', 'content-type': 'application/json', 'user-agent': 'securacv-backoffice' });
export const openExceptions = (db) => db.all('SELECT * FROM exceptions WHERE resolved_at IS NULL ORDER BY opened_at');
export const listExceptions = (db, limit = 100) => db.all('SELECT * FROM exceptions ORDER BY resolved_at IS NOT NULL, opened_at DESC LIMIT ?', limit);
