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
| A newsletter tool (Mailchimp) | **Mail** | the paid features, free: an audience with segments and live counts (customers, never-bought, recent, idle, bought a SKU, waitlist, tag, kind, state), a designed HTML mail rendered from the plain text you write (no pixels, no remote images), preview, test send, scheduling, automations (welcome after opt-in, settled-in after delivery, a quiet-half-year nudge — once per event, consent re-checked at send time), bounce suppression, and a report that counts replies and orders in the week after by address instead of opens |
| A transactional sender (Postmark, SendGrid) | **Mail** | an outbox queue, its own SMTP client (STARTTLS/SMTPS, AUTH), retries, consent re-checked at send time |
| A storefront's products page | **Products** | the catalog (`store.json`) joined to what the business knows: buildable stock, open units, sold per period, margin, who is waiting, what came back; a weekly sales chart per SKU |
| A CRM with lifetime value | **Customers** | orders, lifetime spend, average order, first/last order, tags, segments (repeat buyers, opt-in, waitlist, schools & orgs), a per-customer timeline |
| A discounts engine | **Discounts** | percent / fixed / free-shipping codes with minimums, windows and use caps; Stripe's code captured from the webhook; a manual order priced by the same rules; cost booked to a contra account |
| Sales analytics (Shopify reports) | **Analytics** | total / net / gross sales, orders, average order, units, margin, returning rate, refunds, discounts — each against the previous period; sales and orders over time, top products, by state, by channel, fulfillment speed, support volume, warranty rate; CSV. No visitor numbers — the site keeps none |
| An inventory app | **Inventory** | parts/printed/finished stock, every change a logged move, recipes (what a packed SKU consumes), buildable count, reorder exceptions |
| A returns / RMA tool | **Warranty** | the six defined fix-not-return answers from terms-of-sale §7, a $0 reship order, cost tracking, warranty rate |
| Serial / asset tracking | **Devices** | the jig's `create_manifest.py` output imported; serial ↔ order ↔ firmware hash; the packing-slip QA scan |
| Invoicing (FreshBooks) | **Invoices** | PO-based invoices for schools and businesses, net-N terms, a printable customer link, overdue exceptions |
| Subscription admin | **Plans** | the support-plan mirror of Stripe Billing: who, what, renews when, MRR |
| Bookkeeping (QuickBooks) | **Books** | double-entry in cents, every sale/refund/payout/expense posted, monthly P&L, trial balance, sales tax by state, year-end CSV for an accountant |
| A status page + uptime monitor (Statuspage, UptimeRobot) | **Status** | HTTP probes, 90-day uptime, incidents, public `/status` and `/status.json` |
| Form backends (Formspree) | **Forms** | `/forms/contact`, `/waitlist`, `/request`, `/feedback`, `/press`, `/wholesale` — honeypot + rate limit, straight into tickets and the waitlist |
| An ops dashboard (Metabase, Baremetrics) | **Home** | what needs doing today, the last 30 days against the 30 before, a sales chart, the activity feed. **Not** visitor analytics — the site has none and never will |
| Order management (Shopify's order admin) | **Orders** | status tabs with counts, filters by batch / tag / date / code / SKU, a timeline per order (placed, packed, shipped, refund, notes, tags, address changes, boards scanned), bulk pack / tag / print, packing slips, re-send tracking, the weekly **Batches** page, one search box over everything |
| Alerting (PagerDuty) | **Exceptions** | doc 20, principle 4: thresholds open one deduplicated GitHub issue and close it themselves. Silence means healthy |
| Backups you can trust | **Backups** | every copy is opened and proven (integrity, row counts, the books balanced); an off-site copy to any S3-compatible bucket, sealed with AES-256-GCM before it leaves, read back with a HEAD; a weekly drill pulls the latest copy down and proves it boots; `backoffice restore` is one command |
| Reconciliation | **Reconcile** | daily, with a restricted Stripe key: every paid payout is in the books (posted automatically), every successful charge has an order — a gap is an exception with the ids |
| An uptime dashboard, a runbook, a morning report | **Health** | the doctor's checklist (configuration, backups and their proofs, books, probes, jobs, logins, disk) with what to do about each; the same list is `backoffice doctor`, `/api/health` (503 when anything fails, for an outside monitor) and the morning digest email |
| An audit trail | **Audit log** | every action by anyone or anything, append-only, filterable, CSV |
| A fulfillment app's pick-and-pack flow | **Pack bench / Ship bench** | one order at a time through the weekly batch: pick list, serial scan, one button; then tracking in, one button, the customer mailed |
| A purchasing module (an ERP's) | **Purchasing** | purchase orders drafted from the reorder points in one click (grouped by supplier, at the reorder quantity), marked ordered with an expected date, received into stock line by line and booked as inventory; "buildable" looks ahead to when the parts land |
| A marketing dashboard (Klaviyo's) | **Marketing** | the funnel from a signup to a repeat buyer, where people come from, what every campaign and automation did in the week after (orders and replies, no pixels), the codes, and customers and sales by state on a tile map that fetches nothing from a map provider |
| Carrier tracking + a customer order page (Shopify's) | **Delivery** | a delivery estimate in every tracking mail (business days, a band a day across the country), a signed public status page per order with the stepper, the carrier link and the items, and — with USPS API credentials — the hourly carrier poll that marks delivered on the carrier's day and raises a carrier alert as an exception |
| A recall / safety-notice tool | **Recall** | by batch, firmware version or SKU: every board, where it shipped, who has it; one transactional notice each with their serials filled in, noted on every order, sent only by a second, separate button |
| A human-in-the-loop queue | **Decide (on Home)** | the system drafts the next move and says why — approve a drafted PO, send the one in-stock mail, answer a ticket with the canned reply that matches, mark a week-old parcel delivered, remind about an overdue invoice — one button each; nothing acts on its own, and a card disappears when its condition clears |
| A privacy desk (GDPR tooling) | **Customers** | the right to a copy (everything held about a person as one JSON) and the right to be forgotten (name, addresses, messages, attachments and mail blanked in one transaction; order numbers, amounts and dates kept so the books still add up), both logged |
| Attachments in a helpdesk | **Support** | the photo of the warped lid and the PDF of the purchase order travel with the mail, stored with the message (images, PDFs and text, 5 MB each), shown inline, served same-origin and sandboxed |
| User accounts and permissions (what every SaaS charges per seat for) | **People** | each person with their own password and authenticator, invited by a link that works once (they set their own password and scan a QR off the page — no QR service, the code is drawn here), four roles checked on every request (owner, ops, support, viewer), every action written down under the person's name, lockout per address and per person, a role change or a disable that applies to live sessions at once, *Your account* for password and authenticator rotation |
| An admin login | **Auth** | password + authenticator code for everyone, lockout after eight failures, strict cookies, CSRF plus a cross-site refusal, read-only or read-write bearer tokens. The whole model is in [`SECURITY.md`](SECURITY.md) |

What is deliberately **not** here: the attestation service (LICENSING.md §2b
is a product service with a timestamping authority behind it, not back-office
plumbing), and anything that moves money on its own. Refunds, payouts and
Stripe going live stay human clicks; this records them.

## Two minutes to play with it (demo mode)

No accounts, no keys, nothing leaves your machine:

```sh
git clone https://github.com/kmay89/securaCV && cd securaCV/backoffice
npm run demo            # wipes ./data-demo, seeds a synthetic week, serves http://127.0.0.1:8787
```

The login form comes filled in (the demo owner's email, password `demo1234`,
the current authenticator code) — press **Log in**. The demo people (an
owner, an ops person, a support person, a viewer, one open invitation) share
that password and authenticator so you can log in as any of them and see
what each role may do. Sixty days of a small company are
waiting: ~45 orders across nine weekly batches, repeat buyers, five discount
codes, a refund and a cancellation, answered tickets, the books balanced.
Every page carries a **Demo** banner (every name, order and dollar is synthetic), and **Demo scenarios** in
the nav is a page of buttons that push synthetic events through the real
code paths: a signed Stripe webhook that creates an order, a refund, a
payout, an inbound support email, a customer reply, an out-of-office
bounce, a website waitlist signup with double opt-in, a probe going down
three times and recovering, delivering the outbox, running every scheduled
job. Each shows its result and the affected pages update.

What makes it safe to leave running: the database carries a `demo` flag,
and while it does the app refuses every real service — SMTP, IMAP, GitHub,
Stripe's API are blanked no matter what the environment says, the webhook
accepts only the demo signing secret, probes point at the app itself, and
the outbox marks mail "sent" without opening a socket. The data is
unmistakably synthetic: people are named Placeholder, Fixture and Stub,
every address ends in the reserved `.example.test`, the school is "Example
Academy (fictional)". `npm run dev` serves the same data with auto-restart
on file change, for developing against it. `backoffice demo --reset`
starts over; the operator login survives.

## Put the demo online for others

The website is static files, so the demo has to run on a host of its own.
The container does the whole job: on an empty database with
`BACKOFFICE_DEMO=1` it seeds itself, and with `BACKOFFICE_DEMO_RESET_MINUTES`
it starts over on a cadence so every visitor finds the seeded week. CI
publishes the image to `ghcr.io/kmay89/securacv-backoffice` on every merge
to main. Pick one:

| Where | Cost | How |
|---|---|---|
| Fly.io | free allowance | from `backoffice/`: `fly apps create <name>` then `fly deploy --config deploy/fly.toml --app <name>` (builds on Fly's remote builder; no local Docker) → `https://<name>.fly.dev` |
| Fly.io, no terminal | free allowance | Fly dashboard → Account → Tokens → create one; add it to this repo as the secret `FLY_API_TOKEN`; Actions → "Backoffice demo → Fly.io" → Run workflow. It creates the app and a 1 GB data volume, deploys **one** machine on it, and redeploys on every merge that touches `backoffice/`. (One machine matters: two would be two databases, and a login on one is unknown to the other — the "every other page bounces to the login screen" symptom.) |
| Render | free tier (sleeps when idle, ~30 s to wake) | New → Blueprint → this repo; `backoffice/deploy/render.yaml` is read automatically |
| Any VPS with Docker | ~$4–6/month | point a DNS name at it, then `DEMO_HOST=demo.example.com docker compose -f backoffice/deploy/docker-compose.demo.yml up -d` (Caddy gets the certificate) |
| A laptop, for a meeting | — | `docker run --rm -e BACKOFFICE_DEMO=1 -p 8787:8787 ghcr.io/kmay89/securacv-backoffice` |

The login page shows the demo credentials, so anyone with the link can
walk in and press every button — that is the point, and the hourly reset
is what makes it safe to leave up. Once a URL exists, the website's Engine
Room page can carry a "try the demo" link to it.

## Going live, and staying reliable

[`GO_LIVE.md`](GO_LIVE.md) is the order: a shadow fortnight in Stripe test
mode, backups proven in two places (sealed off-site, a weekly restore
drill), an outside monitor on `/api/health`, the morning digest, then the
live keys. The Health page and `backoffice doctor` show the same checklist,
green when each is done. [`SECURITY.md`](SECURITY.md) says what is defended
and what is not.

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

### How the shipping numbers are made

Three numbers show up around an order, and each has one source.

- **The box.** The website's `shipping.json` (next to `store.json`; `SHIPPING_JSON`
  overrides the path or URL) holds the carton catalog with inner dimensions and
  each SKU's packed parcel. `src/modules/packing.mjs` is a port of the website's
  `scripts/box-fit.mjs` — first-fit decreasing, a box fits when every parcel fits
  by sorted dimensions and the summed volume stays under the inner volume × the
  fill factor, the plan is the cheaper of "one box" and "greedy by cost per packed
  volume", a box is priced by billable weight (actual or dimensional, whichever is
  greater) off the Ground Advantage tiers, a flat-rate box by its flat price — and
  `tests/fixtures/boxfit_expected.json` is that script's own output on eighteen
  orders, which the port must reproduce. The plan is made from the lines when an
  order is **packed** and frozen on it (`pack_plan`); the pack bench, the ship
  bench, the packing slip and the fulfill CSV (`box`, `expected_oz`, `pieces`,
  `service`) all read the same plan. A multi-line order is one shipment: its
  boxes are planned together, and the CSV says how many lines it has so the label
  tool does not make one label per row.
- **The scale.** At the ship bench a measured weight is checked against the plan;
  past the tolerance (`fulfillment.weight_tolerance_oz` in `store.json`, 1.5 oz
  when unset) the ship is refused with the delta — a missing or extra item is the
  usual cause — unless "ship anyway" is ticked, and then the timeline says so.
- **The postage.** The plan's cost is an estimate for a mid zone (the file says
  which); the label's real price, typed at the ship bench, is booked to
  `expenses:postage` and shown beside the shipping the customer paid. The
  **delivery estimate** the customer sees is bands by state from
  `SHIP_FROM_STATE` (the website's `fulfillment.ship_from.state` when unset),
  two to seven business days, replaced by the carrier's own word when USPS
  tracking is configured.

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
src/admin.mjs           one page per module; src/admin_shop.mjs the storefront half (products, customers, discounts, analytics, batches, search, packing slips);
                        src/admin_ops.mjs the pack and ship benches, Health, the audit log; src/admin_mail.mjs campaigns, automations, the audience
src/charts.mjs          inline-SVG charts: columns, line, ranked bars, sparklines, stat tiles — CSS hover tooltips, a table view under each
src/jobs.mjs            outbox (+ scheduled campaigns) · inbox · probes · sla · stock · invoices · books · backup (checked, off-site) · restore_drill · reconcile · digest · automations · demo_reset
src/modules/            catalog contacts mail imap stripe orders discounts analytics products search inventory devices warranty support
                        campaigns automations ledger invoices subscriptions status exceptions dashboard backup reconcile health demo
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
| The admin in a real browser | the forms as a person submits them — log in, ship an order, answer a ticket from a canned reply, receive stock, post an expense, note and tag an order, create and try a discount code, tag a customer, read a product, the analytics charts, the header search, bulk-pack a batch, log out — with zero page or console errors. This is the layer that catches a button whose form is missing a field | `npm i --no-save playwright && npx playwright install chromium`, then `npm test`; it skips (and says so) when Playwright is absent, and CI makes a skip a failure |
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
