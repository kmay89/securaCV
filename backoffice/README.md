# backoffice — the paid services, run by us

The business side of Errer Labs / SecuraCV as one self-hosted process and one
SQLite file: every service a small hardware company normally rents, built to
the shape of the best one and cut down to what this company actually needs.
No SaaS seats, no per-contact pricing, no tracking pixel, no vendor that can
read the customer list. Zero npm dependencies — it is Node 22 and `node:sqlite`,
readable end to end in an afternoon, which is the point: the whole company has
to fit in a context window (strategy doc 20, principle 5).

Stripe stays. A payment processor is the one thing nobody self-hosts, and
[store-README](https://github.com/kmay89/securacv_website/blob/main/store-README.md)
already chose it. Everything *around* Stripe is here.

| You would normally pay for | Here | What it keeps that the paid one does |
|---|---|---|
| A storefront's order admin (Shopify) | **Orders** | paid → packed → shipped → delivered, batches by ISO week, tracking mail, refunds, the fulfill.mjs CSV, Stripe webhook + CSV catch-up |
| A helpdesk (Zendesk, Help Scout) | **Support** | a shared inbox read over IMAP, numbered tickets with a thread token, auto-acknowledgment, canned replies, SLA timer that raises an exception, kinds (security goes urgent) |
| A CRM + newsletter audience (HubSpot, Mailchimp) | **Contacts, Campaigns** | double opt-in, one-click unsubscribe (RFC 8058), segments, per-SKU waitlist with a single "in stock" mail |
| A transactional sender (Postmark, SendGrid) | **Mail** | an outbox queue, its own SMTP client (STARTTLS/SMTPS, AUTH), retries, consent re-checked at send time |
| An inventory app | **Inventory** | parts/printed/finished stock, every change a logged move, recipes (what a packed SKU consumes), buildable count, reorder exceptions |
| A returns / RMA tool | **Warranty** | the six defined fix-not-return answers from terms-of-sale §7, a $0 reship order, cost tracking, warranty rate |
| Serial / asset tracking | **Devices** | the jig's `create_manifest.py` output imported; serial ↔ order ↔ firmware hash; the packing-slip QA scan |
| Invoicing (FreshBooks) | **Invoices** | PO-based invoices for schools and businesses, net-N terms, a printable customer link, overdue exceptions |
| Subscription admin | **Plans** | the support-plan mirror of Stripe Billing: who, what, renews when, MRR |
| Bookkeeping (QuickBooks) | **Books** | double-entry in cents, every sale/refund/payout/expense posted, monthly P&L, trial balance, sales tax by state, year-end CSV for an accountant |
| A status page + uptime monitor (Statuspage, UptimeRobot) | **Status** | HTTP probes, 90-day uptime, incidents, public `/status` and `/status.json` |
| Form backends (Formspree) | **Forms** | `/forms/contact`, `/waitlist`, `/request`, `/feedback`, `/press`, `/wholesale` — honeypot + rate limit, straight into tickets and the waitlist |
| An ops dashboard (Metabase, Baremetrics) | **Dashboard** | the numbers a company of one steers by. **Not** visitor analytics — the site has none and never will |
| Alerting (PagerDuty) | **Exceptions** | doc 20, principle 4: thresholds open one deduplicated GitHub issue and close it themselves. Silence means healthy |
| Backups | **Backups** | nightly `VACUUM INTO`, kept N deep; off-site is one rsync line |
| An admin login | **Auth** | one operator, password + authenticator code, strict cookies, CSRF, bearer tokens for the CLI and an AI operator |

What is deliberately **not** here: the attestation service (LICENSING.md §2b
is a product service with a timestamping authority behind it, not back-office
plumbing), and anything that moves money on its own. Refunds, payouts and
Stripe going live stay human clicks; this records them.

## Two minutes to play with it

No accounts, no keys, nothing leaves your machine:

```sh
git clone https://github.com/kmay89/securaCV && cd securaCV/backoffice
node bin/backoffice.mjs demo      # an operator + a realistic week: orders, tickets, stock, books, a claim, an invoice
node bin/backoffice.mjs serve     # http://127.0.0.1:8787
node bin/backoffice.mjs code      # the authenticator code the login page asks for (the demo prints the password)
```

Log in, then: pack and ship an order (it consumes the recipe parts and
queues the tracking mail — queued, not sent, until `SMTP_URL` is set),
answer the ticket that is past its SLA, resolve the warped-lid warranty
claim, post an expense and watch the trial balance stay balanced, open
`/status` for the public page. `backoffice help` lists the CLI side of the
same data. The demo only loads into an empty database; point
`BACKOFFICE_DATA_DIR` at a fresh directory to start over.

## Fifteen minutes to running

```sh
cd backoffice
cp .env.example .env            # fill in: BASE_URL, SITE_ORIGIN, SUPPORT_ADDR, SMTP_URL, IMAP_URL, STRIPE_WEBHOOK_SECRET
set -a; . ./.env; set +a
node bin/backoffice.mjs setup   # operator password + the authenticator QR; seeds the status probes
node bin/backoffice.mjs serve   # http://127.0.0.1:8787 — put Caddy in front (deploy/Caddyfile)
```

Or `docker compose -f deploy/docker-compose.yml --env-file .env up -d` for the
app + Caddy with automatic TLS, or `deploy/backoffice.service` for systemd.
Node 22.13 or newer is the only requirement.

Then, once each:

1. **Stripe → Developers → Webhooks** → endpoint `BASE_URL/webhooks/stripe`,
   events `checkout.session.completed`, `charge.refunded`, `payout.paid`,
   `customer.subscription.*`, `invoice.paid`. Paste the signing secret into
   `.env`. Put the store SKU in each Payment Link product's **metadata** as
   `sku` — that is how a line item resolves to `store.json` without guessing.
2. **The support mailbox**: an app password for SMTP and IMAP on the address
   in `SUPPORT_ADDR`. The inbox job reads unseen mail every two minutes and
   marks it seen; replies go out from the same address.
3. **The website forms**: `deploy/website-forms.html` is the markup; the one
   edit on the site side is adding the backoffice host to `form-action` and
   `connect-src` in `_headers`.
4. **GitHub token** (optional): a fine-grained PAT with issues read/write on
   this repo, so exceptions land in the one inbox the company uses.
5. `backoffice token create claude` → a bearer token for the API, so an AI
   operator can run the queue from anywhere (`GET /api` lists every call).

## The weekly batch, now

store-README's Sunday night, with the hand steps gone:

```sh
backoffice orders --status paid                 # what came in (webhook) — or `orders import stripe.csv` to catch up
backoffice orders fulfill-csv --batch 2026-W43 > orders.csv
node ../../securacv_website/scripts/fulfill.mjs orders.csv        # validate addresses, slips, labels (its own README)
backoffice inv buildable wap-kit                # the honest stock number before you promise more
backoffice devices import manifest.json --batch 2026-W43 --sku wap-kit   # the jig's output → serial registry
# pack against the slips; scan each serial on its order page (or: assign via the API)
backoffice order pack EL-000123                 # consumes the recipe parts
backoffice order ship EL-000123 --carrier USPS --tracking 9400…     # mails the customer
backoffice queue                                # anything a person owes an answer, oldest first
```

Nothing above types a SKU, an address, or a price by hand: the catalog is
`store.json`, the address is what Stripe collected, COGS is frozen from the
store's `cogs` recipe at the moment of sale.

## How each piece behaves (the rules it keeps)

- **Orders** — one row per shipment; an unknown SKU is a refused order, not a
  guessed one; idempotent on Stripe's event id, so redelivery is safe;
  `packed` consumes inventory through recipes; `shipped` mails tracking;
  refunds are recorded here, issued in Stripe.
- **Support** — a ticket's subject carries `[T-000045 k3f9ab]`; a reply with
  the token, or with our `Message-ID` in `In-Reply-To`/`References`, threads;
  anything else opens a ticket. Bounces and out-of-office mail become notes,
  never tickets, and never get an auto-reply. First response past
  `SUPPORT_SLA_HOURS` raises `sla:T-…`.
- **Consent** — campaign mail goes only to contacts who clicked the confirm
  link, carries `List-Unsubscribe` + `List-Unsubscribe-Post`, and is
  suppressed at send time if consent was withdrawn after it was queued. Order,
  ticket, warranty, invoice and the single waitlist "in stock" mail are
  transactional: things the person asked for.
- **Books** — every transaction balances or throws; the trial balance page
  says "balanced" or names a bug. Accounts are the handful a Schedule C
  wants. It is bookkeeping, not tax advice: the ledger CSV goes to a human.
- **Warranty** — the category *is* the answer (terms-of-sale §7 table,
  encoded in `warranty.mjs ANSWERS`); "send the original back" is always no.
- **Status** — three consecutive failed probes raise `probe:<name>`; the next
  pass resolves it. The public page says plainly that an outage here never
  touches a Canary.
- **Exceptions** — keyed, deduplicated, self-resolving. A key that fires
  nightly and keeps being closed by hand is a wrong threshold (doc 20
  tripwire); fix the system, not the habit.
- **Privacy** — contacts exist because they bought, wrote or asked. No
  tracking pixels, no open/click beacons, no third party sees the list. Mail
  is plain text. The admin sets `noindex` and every cookie is `HttpOnly;
  SameSite=Strict`.

## Layout

```
bin/backoffice.mjs      the CLI — same functions as the web admin, `backoffice help`
src/db.mjs              node:sqlite, migrations/, numbers (EL-, T-, WC-, INV-YYYY-), audit log, savepoint transactions
src/http.mjs            router, body parsing, cookies, CSRF, rate limiter, escaping
src/render.mjs          server-rendered HTML; no client JavaScript
src/auth.mjs            scrypt password, TOTP (RFC 6238), sessions, API tokens
src/server.mjs          public endpoints, the JSON API, the request pipeline
src/admin.mjs           one page per module
src/jobs.mjs            outbox · inbox · probes · sla · stock · invoices · books · backup
src/modules/            catalog contacts mail imap stripe orders inventory devices warranty support
                        campaigns ledger invoices subscriptions status exceptions dashboard backup
tests/                  node --test; fakes for SMTP, IMAP, Stripe signatures, probes, GitHub
deploy/                 Dockerfile, docker-compose.yml, Caddyfile, systemd unit, website-forms.html
```

## Testing it, now and over time

Four layers, cheapest first; the first three run on every push to
`backoffice/` (`.github/workflows/backoffice.yml`), so a change that breaks
the business logic, the HTTP surface or a form cannot merge.

| Layer | What it proves | Run it |
|---|---|---|
| Unit + module tests | every rule in every module: books balance, SKUs resolve, consent is enforced, TOTP matches RFC 6238, mail threads, probes raise and resolve. Fakes stand in for SMTP, IMAP, Stripe and GitHub, so no network, no credentials | `npm test` (under a second) |
| End-to-end over HTTP | the server as a client sees it: login + CSRF, public forms, a signed Stripe webhook creating an order, the bearer API driving it to shipped, unsubscribe, invoice links | part of `npm test` (`tests/server.test.mjs`) |
| The admin in a real browser | the forms as a person submits them — log in, ship an order, answer a ticket from a canned reply, receive stock, post an expense, log out — with zero page or console errors. This is the layer that catches a button whose form is missing a field | `npm i --no-save playwright && npx playwright install chromium`, then `npm test`; it skips (and says so) when Playwright is absent, and CI makes a skip a failure |
| You, on the demo | anything the tests did not think of | `backoffice demo` → `serve` → click around |

The pattern for adding coverage: a bug found by hand becomes an assertion
in the layer that *should* have caught it. The logout button missing its
CSRF token was invisible to the HTTP layer and is now a browser assertion;
the canned reply that could not be sent without typing was found by the
browser test's first run.

Day to day, the two commands that matter are `npm test` before a push and
`backoffice demo` + `serve` when a change touches a page. Nothing here
needs a Stripe account, a mailbox or a domain until you deploy for real.

## The API, for an operator who is a program

```sh
curl -H "Authorization: Bearer bo_…" https://ops.securacv.com/api/snapshot
curl -H "Authorization: Bearer bo_…" https://ops.securacv.com/api/tickets?status=open
curl -X POST -H "Authorization: Bearer bo_…" -H 'content-type: application/json' \
     -d '{"body":"Replacement board shipped today.","close":true}' https://ops.securacv.com/api/tickets/T-000045/reply
curl -X POST -H "Authorization: Bearer bo_…" https://ops.securacv.com/api/jobs/outbox
```

`GET /api` lists every endpoint. Everything the CLI does, the API does; what
the admin shows, `/api/snapshot` returns.

## Restore, migrate, leave

The company is `data/backoffice.db`. Restore is `cp`. Leaving is `sqlite3
backoffice.db .dump`, or the ledger/orders CSV exports — there is no export
fee and no format that is ours alone.
