# 31 — The backoffice: every paid service, run by us

**Thesis.** A hardware company of one is normally a bundle of subscriptions
before it sells a single kit: a storefront admin, a helpdesk, a newsletter
tool, a transactional mailer, an inventory app, a returns tool, invoicing,
bookkeeping, a status page, an uptime monitor, a form backend, an alerting
service, a dashboard. Each is $10–60 a month, each holds a slice of the
customer list, and each is a login the operator (or the AI operating for
them) has to remember. Doc 20 made the company's operations *code*; this
doc makes its *services* code too. The result is
[`backoffice/`](../../backoffice/README.md): one process, one SQLite file,
zero dependencies, every service above as a module, and Stripe — the one
thing nobody self-hosts — left in place.

The method was the one doc 20 and doc 28 already use: take the best product
in each category, keep the shape that makes it good, drop everything a
one-person shop does not need, and bind it to this company's existing
sources of truth (`store.json` for the catalog, the terms of sale for the
warranty answers, the jig's manifest for serials, GitHub issues for the
inbox).

## The map, service by service

| Function | Copied from | Kept | Dropped | Bound to |
|---|---|---|---|---|
| Orders | Shopify's order admin | the state machine, status tabs, batches, a timeline per order, notes and tags, bulk actions, packing slips, tracking mail, refunds ledger, idempotent webhook | variants, multi-channel, themes | `store.json` SKUs + `cogs`; the website's `fulfill.mjs` CSV |
| Products | Shopify's products page | the catalog joined to buildable stock, sales per period, margin, waitlist, claims | the editor (price and copy live in `store.json`, in git) | `store.json`; the inventory recipes |
| Customers | Shopify's customers page | lifetime spend, order count, segments, tags, a timeline | customer accounts, logins | the contacts table; every order and ticket |
| Discounts | Shopify's discounts | percent / fixed / free-shipping codes, minimums, windows, use caps, cost in the books | automatic discounts, buy-X-get-Y | Stripe promotion codes (captured from the webhook) |
| Analytics | Shopify's reports | sales / orders / AOV / margin / returning rate against the previous period, series, top products, by state, fulfillment speed, support volume | sessions, conversion, any visitor number (the site keeps none) | the orders table, on every request |
| Support | Help Scout | shared inbox, numbered threads, canned replies, first-response SLA, kinds | agents, assignment, macros, satisfaction surveys | the support mailbox over IMAP/SMTP; GitHub issues for the public front door |
| Contacts & campaigns | Mailchimp | double opt-in, one-click unsubscribe, segments, plain-text campaigns | open/click tracking, A/B, templates gallery, automation builders | the per-SKU waitlist on the store |
| Mail | Postmark | a queue, retries, consent enforced at send time | deliverability dashboards, webhooks, streams | any SMTP relay, including self-hosted |
| Inventory | inFlow / Katana | moves with reasons, recipes, buildable count, reorder points | purchase orders, multi-location, barcoding | the BOM-priced parts; `store.json`'s per-batch `stock` |
| Warranty | a Zendesk RMA app | claim → defined answer → $0 reship order → cost | return labels, inspection states | terms-of-sale §7's table, verbatim |
| Devices | an asset register | serial ↔ MAC ↔ firmware hash ↔ order | fleet telemetry (a Canary never phones home) | `firmware/provisioning/create_manifest.py` |
| Invoices & plans | FreshBooks + Stripe Billing | PO invoices, net-N, printable link, overdue; plan mirror and MRR | time tracking, estimates, client portal | LICENSING.md §2c (month-to-month support plans) |
| Books | QuickBooks | double-entry, P&L, trial balance, sales tax by state, year-end CSV | bank feeds, payroll, tax filing | every order/refund/payout posts itself; doc 23's expense lines are the accounts |
| Status | Statuspage + UptimeRobot | probes, 90-day uptime, incidents, public JSON | subscribers, component groups, SLAs | the website, the Lab, the release host, `store.json` |
| Forms | Formspree | honeypot, rate limit, CORS, redirect-back | file uploads, spreadsheets export | `/forms/*` → tickets and the waitlist |
| Exceptions | PagerDuty | keyed, deduplicated, self-resolving | on-call schedules, escalation | deduplicated GitHub issues (doc 20 §4) |
| Dashboard | Baremetrics | the handful of numbers that matter | visitor analytics — the site has none, by rule | every module's stats function |
| Auth | — | one operator, password + TOTP, strict cookies, CSRF, bearer tokens | roles, SSO | the CLI and an AI operator use the same API |

## What this changes in the system map (doc 20)

Three rows move from "systemized, human-executed" to "running":

| Function | Was | Now |
|---|---|---|
| **Fulfillment** | Stripe daily email → hand CSV → PirateShip | webhook → orders → `fulfill-csv` → the website's `fulfill.mjs`; serial scanned on the order; tracking mailed on `ship` |
| **Support & community** | GitHub issues + a mailbox read by hand | GitHub issues stay the public door; the mailbox is a queue with an SLA that raises an exception |
| **Storefront** | `store.json` → Payment Links, orders live in Stripe | same, plus the order queue, waitlist, consent and COGS-at-sale live here |

And three functions that had no row get one: **books** (nightly reconcile
check on the Stripe balance), **status** (probes on the four things a
customer can reach), **warranty** (claims with a defined answer).

## Why not the SaaS, honestly

- **Cost** is the small reason: the bundle is ~$150–300/month, about half of
  doc 23's fixed overhead, and it scales per contact and per seat.
- **The customer list** is the real one. A privacy company whose customer
  list sits in five vendors' databases is making a promise its suppliers
  can break. Here it is one file on one box the operator holds.
- **Operability by a program** is the one doc 20 cares about. Every SaaS
  has an API, but each is different, each needs its own token, and none
  exposes the whole business in one call. `GET /api/snapshot` does.

## What deliberately stays human (unchanged from doc 20)

Money movement (refunds and payouts are *recorded* here, *issued* in Stripe),
prices (the catalog is the website's `store.json`, a human edit), safety and
the brand's word. And one addition: **consent**. The system can queue a
campaign; it cannot grant anyone permission to receive it — only their click
on the confirm link does.

## Tripwires

- If a module grows a feature the one-person shop never uses, it is drift
  toward the product it copied. Remove it.
- If any fact gets a second home (a price in the backoffice *and* in
  `store.json`), collapse it — that is doc 20's second tripwire, and the
  catalog loader exists precisely so it does not happen.
- If an exception key fires and is resolved by hand more than twice, the
  threshold is wrong.
- If the test suite ever needs a network or a credential, a boundary leaked.
