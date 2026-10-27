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
- **Lockout, twice, both per address.** Eight failed logins from one
  address in fifteen minutes and that address waits the window out; eight
  failures against one person *from one address* and that pair waits, the
  right password included. A stranger guessing from anywhere cannot keep
  the owner out of their own backoffice: the person's own browser still
  gets in. The lock lives in memory (a restart clears it); the users table
  keeps the count and the time as a record, so the People page can say
  "locked out". Every attempt, locked or not, is in the audit log with its
  address; the Health page counts the last 24 hours and names anyone locked
  out. An unknown email runs the same password work as a known one, so the
  answer takes the same time either way.
- **A code is good for one login.** The 30-second step an authenticator
  code matched is remembered per person; the same code presented again, at
  or before that step, is refused. A code read over a shoulder or off a
  screenshot is one login, not two. The step after is accepted as usual.
- **Sessions.** A random 256-bit id in an `HttpOnly; SameSite=Lax`
  cookie (`Secure` when the base URL is https), 12 hours absolute, a new id
  on every login, deleted on logout. A CSRF token per session rides in every
  form; a POST whose browser says it came from another site
  (`Sec-Fetch-Site` other than `same-origin` or `none`) is refused before
  the body is read. A session cookie on an `/api` write needs the same
  token (an `x-csrf` header or `_csrf` in the body); only a bearer token
  writes to the API without one.
- **Which address is "the address".** The limiter, the lockout and the
  audit log use the peer's address unless the peer is a trusted proxy, in
  which case the last `X-Forwarded-For` hop it appended is used.
  `TRUSTED_PROXIES` names the trusted peers (`loopback,private` by default:
  Caddy on the same box or network; add a CIDR or an address, or set it
  empty when nothing sits in front). A forged header from the open internet
  never moves the count onto somebody else.
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
- **A sign-in link, in place of the password.** With a mail relay, the
  login page offers "Email me a sign-in link". The link stands in for the
  password, never for the authenticator: it opens a page that asks for the
  code. It is good for `SIGNIN_LINK_MINUTES` (15) and once; the browser that
  asked for it holds a nonce cookie the link checks, so a link read out of a
  mailbox on another device opens nothing (a 404, the same as an expired or
  a used one). The token and the nonce are stored as SHA-256 hashes. The
  answer to a request is the same whether the address is a person here or
  not, so there is nothing to enumerate; a wrong code on the link counts
  like a wrong password, against the same locks.
- **The watch.** Every sign-in from a network (a /24, a /48) the person has
  not used in thirty days mails the person ("log out everywhere else, change
  your password, tell an owner" if it was not them) and pushes to the phone,
  and is in the audit log. An address locked out after eight failed logins
  pushes to the phone, once an hour per address. A scanner — sixty missing
  or refused pages from one address in ten minutes — is blocked for ten
  minutes and pushed about; a person with a session is never blocked, and
  neither are the store, the webhook, the status page or a customer's page,
  so a block is never a denial of service against the business.
  `ADMIN_ALLOW_CIDRS` makes the admin and its door exist only for the
  networks you name (a home, an office, a VPN); every other network sees a
  404 for every admin path, while the store, the webhook and the customers'
  pages stay open. `SESSION_BIND_IP=1` ends a session the moment it turns up
  from another network than it was opened on, which is what a stolen cookie
  does; a phone that changes networks signs in again. Both are off by
  default and the Health page says which is on.
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
- What Stripe sends in the wrong order is refused, not lost: a refund whose
  order is not here yet (the session's own event failed or is still on the
  way) is a 400, Stripe retries it for three days, and the retry applies once
  the order exists. A chargeback tags the order, raises an exception with the
  evidence deadline, and resolves when Stripe closes it (a loss is recorded
  as the refund it is). A Checkout in subscription or setup mode is never a
  shop order; a charge on an invoice is never a missing one.
- An order sold past the week's cap or the shelf (two checkouts for the last
  unit in the same minute, a Payment Link outside the store) is recorded as
  paid, tagged `oversold`, and raised as an exception that says by how much;
  the fix (parts, a date, a refund) is a person's call, never a silent refusal
  of money already taken.
- Packing is one order at a time at the bench: every line ticked, the boards
  scanned, the scale read. There is no bulk "packed", and a pack from the
  API without the check says "bench check not run" on the timeline.
- A canceled or fully refunded order lets its boards and its parts go back
  to stock; a shipped one keeps its record. A refund reverses its share of
  the sales tax off the liability, so the tax table is net of refunds.
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
  token **and** `GITHUB_REPO` set, as an issue. An issue body describes a
  customer's order or ticket, so the repository must be private; email
  addresses are redacted before the body leaves the box, and the full text
  stays in the exceptions table here. A row that would collide with one that exists is a
  409 that names the constraint, never a 500.
- Numbers arrive whole or not at all: quantities, cents, stock moves,
  reorder points, receipts and intervals are checked at the route and again
  in the module (`NaN` would bind as NULL, `1.5` would land in an INTEGER
  column as a REAL). Every CSV the admin downloads is formula-safe: a cell
  starting with `=`, `+`, `-` or `@` is prefixed with an apostrophe, so a
  customer's name is never run by a spreadsheet.

## The boundary

The API is read by an AI operator as often as by a person, and every ticket,
mail, form and payment carries text an outsider wrote. Two rules, both
mechanical:

- **Scrubbed on the way in.** A ticket's subject and body, a reply by mail,
  a form submission, a buyer's name and notes from Stripe: each passes
  `sanitize()` (`src/guard.mjs`) before it is stored or shown, which removes
  zero-width characters, every bidi embedding, override and isolate, the
  byte-order mark, soft hyphens and C0 controls (a newline and a tab stay),
  normalizes to NFC and caps the length. Nothing can hide an instruction in
  an invisible run, spoof a name with a direction override, or smuggle a
  control sequence into a terminal that prints the log.
- **Marked on the way out.** Every API answer that carries outsider text
  names those fields in `_untrusted` and says in `_notice` that they are
  data to read, never instructions to follow, whatever they say; the same
  two travel as `X-Untrusted-Fields` and `X-Boundary-Notice` headers. An
  operator built on a language model reads the record with its provenance
  attached. Nothing here makes a model safe; it makes the provenance
  impossible to miss.
- **Crawlers.** Every answer carries `X-Robots-Tag: noindex, nofollow,
  noarchive, noai, noimageai` and `/robots.txt` disallows everything; the
  admin and the customers' pages are nobody's training data.

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
  distributed flood needs the reverse proxy (Caddy, Fly) to absorb it. What
  the process does hold on its own: a request body over 1 MiB is cut, a
  connection that opens and sends nothing is dropped after 15 seconds, a
  request is given a minute, and every outbound call (Stripe, SMTP, IMAP,
  S3, GitHub, a probe) is abandoned after 20 seconds (two minutes for a
  backup upload), so one dead peer never pins the scheduler.
- **Two outbound calls the operator chooses.** Shippo (`SHIPPO_API_TOKEN`)
  receives one order's shipping address and parcel when the ship bench buys
  its label — the same call the website's `fulfill.mjs` makes. An ntfy topic
  (`NTFY_URL`) receives a new order's number, money, lines and ship day,
  never a name or an address. Both are off unless set, both are muted in
  staging and the demo, and both are named on the Health page.
- **An outbound probe to the inside.** The Health page's "probe a URL" is
  refused for loopback, link-local and private addresses and for
  metadata hosts, so it cannot be turned on the box or its network.
- **A malicious reviewer.** Nobody outside this repository has audited the
  code. The tests pin every control above; read them before trusting it
  with real orders, and send findings to the support address.

## Reporting

A security issue in this code: open a private report through the
repository's security advisories, or mail the support address with
"security" in the subject — the helpdesk files it as urgent.
