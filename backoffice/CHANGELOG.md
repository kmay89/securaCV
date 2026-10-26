# Backoffice changelog

What changed between the builds the server runs. The version is `package.json`'s;
the build hash on `/healthz`, the Health page and `backoffice version` is the commit
CI stamped into the image. `tests/environment.test.mjs` holds the top entry here to
the package version, so neither moves without the other.

## 0.3.0

- **One scale of urgency, everywhere.** `src/attention.mjs` ranks everything that
  wants the admin by its deadline into three levels — *now* (a clock has run out
  or a person is already being failed), *soon* (inside a stated window), *calm* —
  and every surface reads it from there: a count on the sidebar section colored by
  the worst thing inside it, the home page's focus strip (the single most pressing
  thing, full width, with its clock) and cards, the Decide queue ranked by the
  clock each card is against, a mark on a table row's left edge, a due date in a
  cell ("overdue 15 h", "waiting 4 d", "due in 24 h"). The windows are stated once
  (`WINDOWS`: two days to ship, six hours before an SLA, a week on a claim) and the
  morning digest's TODAY list is the same list, so no two views disagree.
- **Support opens on what needs us.** The ticket list's default tab is open +
  waiting on us, ordered by SLA, with the subject the widest column; the other
  states are tabs. A ticket's fact strip says "first reply overdue 15 h" rather
  than two timestamps to subtract.
- **Orders name the late ones.** A line above the list counts the orders past the
  shipping promise and the oldest; each unshipped row shows how long it has waited.
- **The home activity feed** no longer runs its time column into the text, names
  the person not their whole address, and lets routine system rows (a backup
  taken, a drill passed) sit back so what a person did stands out. The sales
  chart wears a series color, so the brand gold means "an action" and nothing
  else on the page.

## 0.2.0

- **Every page says which environment it is.** `BACKOFFICE_ENV` (production,
  staging, demo, development) is a badge beside the company name, a prefix in
  the title, the second word of `/healthz`, and the first row of the doctor.
  Production refuses a demo seed, a database another environment labeled, and
  a base URL that is not https; staging mutes mail and issues, never writes to
  the backup bucket, and refuses a live Stripe key.
- **The build is named.** CI stamps the commit and date into the image; the
  first boot of a build on a database writes a `deploy.boot` audit row naming
  the build before it.
- **Migrations are guarded.** A checked copy is taken before the first pending
  migration on a file database; a database carrying migrations this build does
  not know refuses to open (a rollback can no longer run old code on a newer
  schema); migrations are additive by rule, and a test holds the rule.
- **Feature flags.** A registry in `src/flags.mjs`, toggled on Settings, overridable
  per environment with `FLAG_<NAME>`, every flip audited, and a doctor row that
  names a flag past its removal date. `stripe_tax` is the first.
- **Staging.** `infra/apps/backoffice-staging`: the same image on yesterday's
  production copy with test keys; merges deploy to staging, a button promotes a
  build to production.

## 0.1.x

- The storefront's server half, hosted checkout, delivery estimates (#1869).
- The owner's manual and a working off-site restore (#1870).
- The admin fits a phone and an iPad, with a test that holds it there (#1871).
- The hardening pass: process nets, the security review, the workflow review (#1872).
