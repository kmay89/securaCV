# Security — what the backoffice defends against, and what it does not

The backoffice holds the operational state of a company of one: who bought
what, who wrote in, what is on the shelf, the books. It is not the system of
record for money (Stripe is), for mail (the mailbox is) or for the catalog
(git is). That shapes what is worth defending and what a breach would cost.

## The door

- **One operator.** A password (scrypt, N=16384) and a TOTP code (RFC 6238,
  SHA-1, 30 s, ±1 step). Both, every time. There are no roles because there
  are no other users; adding them is a migration, not a redesign.
- **Lockout.** Eight failed logins from one address in fifteen minutes and
  that address waits the window out — the right password included. Every
  attempt, locked or not, is in the audit log with its address; the Health
  page counts the last 24 hours.
- **Sessions.** A random 256-bit id in an `HttpOnly; SameSite=Strict`
  cookie (`Secure` when the base URL is https), 12 hours absolute, a new id
  on every login, deleted on logout. A CSRF token per session rides in every
  form; a POST whose browser says it came from another site
  (`Sec-Fetch-Site`) is refused before the body is read.
- **API tokens.** `bo_<id>.<secret>`; the secret is scrypt-hashed, so a copy
  of the table verifies nothing. A token can be read-only (`--read-only`,
  or the Scope choice on Settings): GET only, for a dashboard or a monitor.
  Revocation is immediate.
- **Rotation.** `backoffice setup --force` sets a new password and a new
  authenticator secret. The Health page warns when the password is a year
  old.
- **Recovery.** A lost authenticator is recovered on the server, never over
  the network: `backoffice setup --force` from a shell with access to the
  data directory. There are no backup codes by design — the shell *is* the
  recovery factor, and it needs the host's own credentials.

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
  website forms (honeypot + 30/min), the status page, confirm/unsubscribe
  links (random 144-bit tokens), invoice links (HMAC of the number).

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
