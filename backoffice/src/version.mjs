// What build this is: the package version (bumped with CHANGELOG.md), the commit CI stamped into the
// image, and when. Shown on /healthz, the Health page, Settings, `backoffice version` and the doctor,
// and written to the audit log the first time a build boots on a database (deploy.boot) — so "what is
// running, since when, and what ran before it" is answered from the app itself, not from the server.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

export const VERSION = JSON.parse(readFileSync(fileURLToPath(new URL('../package.json', import.meta.url)), 'utf8')).version;

export const build = (cfg) => ({ version: VERSION, sha: String(cfg.buildSha || 'dev').slice(0, 12), date: cfg.buildDate || null });
export const describeBuild = (cfg) => { const b = build(cfg); return `${b.version} · build ${b.sha}${b.date ? ` · ${b.date.slice(0, 10)}` : ''}`; };

// The first boot of a build on this database writes one audit row naming it and the build before it.
export function recordBoot(db, cfg) {
  const b = build(cfg); const prev = db.setting('build:last', '');
  if (prev === b.sha) return null;
  db.setSetting('build:last', b.sha); db.setSetting('build:booted_at', new Date().toISOString());
  db.audit('system', 'deploy.boot', 'build', b.sha, { from: prev || null, version: b.version, env: cfg.env, date: b.date });
  return { from: prev || null, to: b.sha };
}
