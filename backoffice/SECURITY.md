# Security — what the backoffice defends against, and what it does not

The backoffice holds the operational state of a company of one: who bought
what, who wrote in, what is on the shelf, the books. It is not the system of
record for money (Stripe is), for mail (the mailbox is) or for the catalog
(git is). That shapes what is worth defending and what a breach would cost.

## The door

- **People, each with their own door.** Every person has a password (scrypt,
  N=16384) and an authenticator (TOTP, RFC 6238, SHA-1, 30 s, ±1 step).
  Both, every time. Nobody is ever handed a password: an owner creates an
  invitation, which is a link that works once and for 48 hours; the person
  chooses their own password and scans their own authenticator, and the
  account opens only when a code from it has been seen. The invitation can
  be handed over any way the owner trusts (and is mailed when a relay is
  configured); it carries no secret of its own.
- **Roles, checked on every request.** `owner` (everything, including
  people, tokens, settings, the books and forgetting a customer), `ops`
  (runs the shop; not people, tokens, settings or the books), `support`
  (tickets, customers, warranty, notes and tags; reads the rest), `viewer`
  (reads, changes nothing). The rule lives in one function (`can()` in
  `auth.mjs`), by path and method, and the pipeline applies it before any
  handler runs — the sidebar hides what a role cannot open, but hiding is
  never the enforcement. A role change or a disable applies to the person's
  live sessions at once. The last active owner cannot be demoted, disabled
  or removed. API tokens are owner-made and not bound to a role; their
  scope is the limit.
- **Every action under a name.** The audit log and every order timeline
  record the person's email (or `token:<name>` for the API), set by the
  pipeline for the request, so no handler can forget to say who.
- **Lockout, twice.** Eight failed logins from one address in fifteen
  minutes and that address waits the window out; eight failures against one
  person and that person waits, the right password included. Every attempt,
  locked or not, is in the audit log with its address; the Health page
  counts the last 24 hours and names anyone locked out.
- **Sessions.** A random 256-bit id in an `HttpOnly; SameSite=Lax`
  cookie (`Secure` when the base URL is https), 12 hours absolute, a new id
  on every login, deleted on logout. A CSRF token per session rides in every
  form; a POST whose browser says it came from another site
  (`Sec-Fetch-Site`) is refused before the body is read.
- **API tokens.** `bo_<id>.<secret>`; the secret is scrypt-hashed, so a copy
  of the table verifies nothing. A token can be read-only (`--read-only`,
  or the Scope choice on Settings): GET only, for a dashboard or a monitor.
  Revocation is immediate.
- **Rotation.** Each person changes their own password on *Your account*
  (the current one is required) and sets up a new authenticator there; the
  old authenticator keeps working until a code from the new one is seen,
  so a half-finished switch locks nobody out. *Log out everywhere else*
  ends their other sessions. The Health page warns when the oldest owner
  password is a year old.
- **Recovery.** A lost authenticator is recovered by a person, not a
  network: an owner sends a fresh invitation (the same link flow, which
  replaces the password and authenticator), or, for the last owner,
  `backoffice setup --force` from a shell with access to the data
  directory. There are no backup codes by design — another owner, or the
  shell, *is* the recovery factor.

## The pages

- Every admin response carries `Content-Security-Policy: default-src 'none'`
  with inline styles only — there is no client-side JavaScript to inject
  into — plus `X-Frame-Options: DENY`, `nosniff`, `Referrer-Policy:
  no-referrer`, `Permissions-Policy` (camera, microphone, location, payment,
  USB all off), `Cross-Origin-Opener-Policy` and `Cross-Origin-Resource-Policy`
  `same-origin`, and `Cache-Control: no-store`. HSTS (one year, subdomains)
  is added when served over https.
- All HTML is escaped on output; all SQL is parameterized; request bodies
  are capped at 1 MiB; the error page never carries an exception message
  (it is logged, the client gets a generic line).
- The public surface is small and rate-limited per address: the Stripe
  webhook (signature + timestamp tolerance + idempotency on event id), the
  website forms (honeypot + 30/min per IP + 5 per 10 min per email, and one
  acknowledgement per address per hour so the forms cannot be turned on a
  third party's inbox), the status page, confirm/unsubscribe links (random
  144-bit tokens), invoice and order links (HMAC of the number, compared in
  constant time).
- A Stripe event id is claimed before the work, marked applied after it,
  and released if the work fails: two deliveries at the same instant apply
  once, a delivery that failed is left for Stripe to retry, and a claim the
  process died on is taken over by the next redelivery once it is a minute
  old. Only an applied event is ever answered "duplicate".
- Every form carries a one-time token beside the CSRF token: the first
  submit runs, a second with the same token (a double click, a browser
  retry) is answered "already submitted" and changes nothing. A refused
  submit frees its token so the corrected form goes through.
- A bug (a 500) is logged, answered with a generic line, and raised as an
  exception keyed on the route, so it shows on Home and, with a GitHub
  token, as an issue. A row that would collide with one that exists is a
  409 that names the constraint, never a 500.
- Numbers arrive whole or not at all: quantities, cents, stock moves,
  reorder points, receipts and intervals are checked at the route and again
  in the module (`NaN` would bind as NULL, `1.5` would land in an INTEGER
  column as a REAL). Every CSV the admin downloads is formula-safe: a cell
  starting with `=`, `+`, `-` or `@` is prefixed with an apostrophe, so a
  customer's name is never run by a spreadsheet.

## The data

- The database file and every backup are mode `0600`.
- Backups are proven, not assumed: each is opened, integrity-checked, row
  counts compared with the live file, the ledger summed. The off-site copy
  is sealed with AES-256-GCM under a key derived (scrypt) from
  `BACKUP_ENCRYPTION_KEY` before it leaves the machine, read back with a
  HEAD, and pulled down and opened again by the weekly drill. The bucket
  never holds a readable database.
- Secrets live in the environment, never in the database or the audit log;
  the Settings page shows which are set, not what they are.
- Shipping is guarded against the mistakes that put a wrong box in the mail:
  a tracking number is normalized and may sit on one order only; the
  fulfill CSV lists paid and packed orders whatever filter the link named;
  a hand-typed US address needs a two-letter state and a 5 or 5+4 ZIP, and
  one Stripe delivered that a label cannot carry is flagged on the order
  and both benches; a board goes into one unshipped order; a MAC belongs to
  one serial. "Refunded" is what a recorded refund makes an order, never a
  status anyone sets, and a refund in full before shipping returns the
  parts to stock.
- Mail: no tracking pixels, no remote images, ever. Outbound SMTP is
  STARTTLS-or-nothing (`smtp://`) or SMTPS; credentials are never offered on
  a plain connection. Marketing mail goes only to double opt-in addresses,
  re-checked at send time, never to a bounced one, always with one-click
  unsubscribe (RFC 8058).
- Demo mode is a hard guard in code: while the database carries the demo
  flag, SMTP, IMAP, GitHub and Stripe are blanked whatever the environment
  says, and every URL is forced to the loopback listener.

## What is not covered, said plainly

- **A compromised host.** Root on the box reads everything; the database is
  not encrypted at rest beyond file permissions. Full-disk encryption is the
  host's job (Fly volumes and most VPS images do it).
- **A compromised operator device.** A session cookie stolen from a logged-in
  browser is a session until it expires or you log out. Keep the session
  short (`SESSION_HOURS`) and log out on shared machines.
- **Denial of service.** The rate limiter is per-address and in memory; a
  distributed flood needs the reverse proxy (Caddy, Fly) to absorb it.
- **A malicious reviewer.** Nobody outside this repository has audited the
  code. The tests pin every control above; read them before trusting it
  with real orders, and send findings to the support address.

## Reporting

A security issue in this code: open a private report through the
repository's security advisories, or mail the support address with
"security" in the subject — the helpdesk files it as urgent.
