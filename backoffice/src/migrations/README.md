# Migrations

One `.sql` file per change, named `NNN_what_it_does.sql`, numbered without gaps.
`openDb` applies every file not yet in `schema_migrations`, each in its own
transaction, in name order, on every boot of the app and the CLI. There is no
"down" migration; a deploy that must go back goes back to the copy taken
before it (below). `tests/migrations.test.mjs` holds every rule on this page.

## The rules

1. **Additive only.** A migration creates tables, adds columns and indexes,
   and fills them. It does not `DROP`, `RENAME`, or narrow a column. The build
   before it must still run on the database after it, because that is what a
   rollback is: the previous image on the current file.
2. **Contract later, and say so.** When a column or table is dead, the removal
   is its own migration in a later release than the one that stopped using it,
   with `-- contract: <what, and which release stopped using it>` as its first
   line. The test refuses a destructive statement without that line.
3. **No transactions, no PRAGMAs inside a file.** `openDb` wraps each file in
   `BEGIN`/`COMMIT` and strips `PRAGMA` lines (the journal mode is set once,
   outside any transaction). A file that opens its own transaction would nest.
4. **Idempotent where SQLite lets you** (`CREATE TABLE IF NOT EXISTS`,
   `CREATE INDEX IF NOT EXISTS`). `ALTER TABLE ADD COLUMN` cannot be, which is
   why each file runs exactly once and is recorded.
5. **Data moves are migrations too**, and they are written as `UPDATE … WHERE
   <column> IS NULL` so a re-run after a crash changes nothing.

## What the app does around them

- **A copy before the first pending migration.** On a file database that already
  has migrations, `openDb` writes `backups/pre-migrate-<stamp>-<first pending>.db`
  (`VACUUM INTO`, consistent, no lock held) before applying anything, and keeps
  the newest five. Rolling a deploy back is: deploy the previous tag, then
  `backoffice restore backups/pre-migrate-….db` (or `kit offline backoffice …`
  on the server). The Health page counts the copies.
- **A newer schema is refused.** A database carrying migrations this build does
  not know (an older image started after a newer one migrated the file) does
  not open; the message names them. `BACKOFFICE_ALLOW_NEWER_SCHEMA=1` opens it
  anyway, for reading or backing up during a rescue, never for serving.
