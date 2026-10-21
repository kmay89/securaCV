// Backups: a consistent SQLite copy via VACUUM INTO, nightly, kept N deep.
// Restore is `cp`. Off-site is the operator's rclone/rsync line in cron —
// one file, so any sync tool works.
import { mkdirSync, readdirSync, unlinkSync, statSync } from 'node:fs';
import { join } from 'node:path';

export function backup(db, dir, { keep = 14, now = new Date() } = {}) {
  mkdirSync(dir, { recursive: true });
  const name = `backoffice-${now.toISOString().replace(/[:.]/g, '-').slice(0, 19)}.db`;
  const path = join(dir, name);
  db.raw.exec(`VACUUM INTO '${path.replace(/'/g, "''")}'`);
  const files = readdirSync(dir).filter(f => /^backoffice-.*\.db$/.test(f)).sort();
  for (const f of files.slice(0, Math.max(0, files.length - keep))) unlinkSync(join(dir, f));
  return { path, bytes: statSync(path).size, kept: Math.min(files.length, keep) };
}
export const listBackups = (dir) => { try { return readdirSync(dir).filter(f => /^backoffice-.*\.db$/.test(f)).sort().reverse().map(f => ({ name: f, bytes: statSync(join(dir, f)).size })); } catch { return []; } };
