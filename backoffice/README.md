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
| Order management (Shopify's order admin) | **Orders** | status tabs with counts, filters by batch / tag / date / code / SKU, a timeline per order (placed, packed, shipped, refund, notes, tags, address changes, boards scanned), bulk tag / print / deliver / cancel (never bulk pack — the bench ticks each line), packing slips, re-send tracking, the weekly **Batches** page, one search box over everything |
| Alerting (PagerDuty) | **Exceptions** | doc 20, principle 4: thresholds open one deduplicated GitHub issue and close it themselves. Silence means healthy |
| Backups you can trust | **Backups** | every copy is opened and proven (integrity, row counts, the books balanced); an off-site copy to any S3-compatible bucket, sealed with AES-256-GCM before it leaves, read back with a HEAD; a weekly drill pulls the latest copy down and proves it boots; `backoffice restore` is one command |
| Reconciliation | **Reconcile** | daily, with a restricted Stripe key: every paid payout is in the books (posted automatically), every successful charge has an order — a gap is an exception with the ids |
| An uptime dashboard, a runbook, a morning report | **Health** | the doctor's checklist (configuration, backups and their proofs, books, probes, jobs, logins, disk) with what to do about each; the same list is `backoffice doctor`, `/api/health` (503 when anything fails, for an outside monitor) and the morning digest email |
| An audit trail | **Audit log** | every action by anyone or anything, append-only, filterable, CSV |
| A fulfillment app's pick-and-pack flow | **Pack bench / Ship bench** | one order at a time through the weekly batch: pick list, serial scan, one button; then tracking in, one button, the customer mailed |
| Buying the label in the admin (Shopify Shipping) | **Ship bench** | with `SHIPPO_API_TOKEN`: one button buys the USPS label for the box the order was packed in, writes the tracking and postage onto the order, ships it and opens the PDF to print; refuses a bad address or a blank return address before any money moves |
| Abandoned checkout recovery + the conversion number | **Marketing · The checkout** | every hosted checkout counted from Stripe's own events (opened, paid, abandoned, recovered — no pixel, no cookie); an abandoned one with an email gets one follow-up with Stripe's link that reopens the same cart, never a second |
| Editing products in the admin (Shopify's product editor) | **Products** | a product's price, name, batch cap, hidden flag and sale window edited on its page and live on the site at once; the file (`store.json`) stays the base, the page prints the patch it would need, the doctor counts what is edited here |
| Launchpad (Shopify Plus): a scheduled launch | **Products** | "on sale from" and "until" on the listing: listed today, sells from the date, the site says when it opens |
| Sales tax nexus tracking (Avalara, TaxJar) | **Books · Where sales tax is owed** | the trailing year's orders by state against the common thresholds; the home state always; the doctor fails when nothing is collected and tax is owed |
| Customer account self-service | **The customer's page** | no account to make: the signed link in every mail shows where it is and lets them fix the address while it is on the shelf |
| The admin app's push notification | **Notify** | a new order mails `OPERATOR_EMAIL` the minute it lands, and pushes to an ntfy topic if you set one |
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
| **Your own server (the way we run it)** | ~$4–6/month for the box, everything else $0 | [`infra/`](../infra/README.md): one small VPS, Caddy at the edge, every app a container, deploys on merge. `kit app add backoffice`, `kit site add ops.example.com backoffice:8787`, `kit deploy backoffice latest`. The public demo is the same image as a second app (`backoffice-demo`) on its own hostname. |
| Render | free tier (sleeps when idle, ~30 s to wake) | New → Blueprint → this repo; `backoffice/deploy/render.yaml` is read automatically (demo only: the free tier has no disk) |
| A laptop, for a meeting | — | `docker run --rm -e BACKOFFICE_DEMO=1 -p 8787:8787 ghcr.io/kmay89/securacv-backoffice` |

The login page shows the demo credentials, so anyone with the link can
walk in and press every button — that is the point, and the hourly reset
is what makes it safe to leave up. Once a URL exists, the website's Engine
Room page can carry a "try the demo" link to it.

## Going live, and staying reliable

[`GO_LIVE.md`](GO_LIVE.md) is the order: three copies that each say which
they are (production, staging on last night's data, the demo), a shadow
fortnight in Stripe test mode, backups proven in two places (sealed off-site,
a weekly restore drill), an outside monitor on `/api/health`, the morning
digest, then the live keys. Every merge deploys to staging; production
changes only by the "Promote backoffice to production" button
([`../docs/RELEASE_BUTTONS.md`](../docs/RELEASE_BUTTONS.md)), and a build
takes a copy of the data before any migration it applies
([`src/migrations/README.md`](src/migrations/README.md)). The Health page and `backoffice doctor` show the same checklist,
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
   events `checkout.session.completed`, `checkout.session.async_payment_succeeded`,
   `charge.refunded`, `charge.dispute.created`, `charge.dispute.closed`,
   `payout.paid`, `customer.subscription.*`, `invoice.paid`. Paste the signing secret into
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

### The first order, from nothing — and the test that keeps it true

`/demo/first-order` (demo mode) empties the database to day zero (the people
stay) and walks one synthetic customer through the whole chain a step at a
time: the first parts received, the first boards flashed, a question through
the website form and its answer, the Checkout sale by signed webhook (and
Stripe's retry of it, applied once), the bench with its checks, the label
from the manifest, the mail, the carrier saying delivered, the follow-up
automation, a warranty fix through the same bench, a partial refund, a
payout, the week's jobs and the doctor. Every step runs the real code path
and shows what it did, where to look and what it checked.

The steps live once, in `src/modules/walkthrough.mjs`, and
`tests/first_order.test.mjs` runs the same list end to end and holds every
check. **A new flow is a new step there**: write what it does, how to look,
and what must be true afterwards, and the demo page, the test and
`backoffice first-order` (the shell) all get it.

### Your phone is the scanner

No scanner, no app: the iPhone's own camera reads a QR code and opens the link,
and the backoffice is already a web page. Log in once on the phone's browser
(the session lasts `SESSION_HOURS`, 12 by default) and the codes do the rest.

- **The packing slip carries a code** (`/q/o/<number>`). Scan it and the order
  opens where it stands: the pack bench if it is paid, the ship bench if it is
  packed, the order page once it has shipped. So the slip in your hand is the
  order on the screen, and a tracking number goes to the box it belongs to.
- **Each board gets a label** — Devices → "Print board labels", a QR and the
  serial per sticker (`/q/b/<serial>`). Scan one with the pack bench open on an
  order and the phone shows the board, the order and one button: "Put … into
  EL-…". A board the order does not ask for, or one more than it holds, shows
  why instead of the button. Scanning a board with no bench open just shows the
  board — where it is, which order has it — which is also how a returned unit
  is looked up. A scan never changes anything by itself; the tap posts the same
  form the bench does.
- **Ship the batch from the manifest.** The website's `fulfill.mjs --buy` buys
  the labels and writes `manifest.json`; paste it at the ship bench and every
  packed order it names is shipped with its tracking and label cost, and the
  customers get their mail. The same manifest twice changes nothing; an order
  it names that is not packed, or already shipped with another number, is
  reported by name and the rest still goes.

Three things to know before the first batch. Use Safari itself on the phone,
not a copy added to the home screen (iOS keeps those cookie jars apart, and the
camera opens links in Safari). The Health page says whether
`BACKOFFICE_BASE_URL` is a real address — the codes encode it, and the default
points at the machine itself. And a second tap on a bench button is never an
error: an order already packed says so and nothing moves twice.

The codes are made here (`src/qr.mjs`, the same encoder the authenticator
enrollment page uses; `tests/users.test.mjs` holds it to a reference encoder
bit for bit), so a slip never carries a code the phone cannot read. They encode
`BACKOFFICE_BASE_URL`, so set it to the address the phone reaches.

### The storefront's server half

The website is static; what it cannot know on its own it asks here, over
four public routes (CORS to `SITE_ORIGIN`): `GET /store/catalog` (price,
what is sellable now and how many, when the current batch closes and ships),
`GET /store/estimate?zip=` (the sentence a shopper reads before paying:
"Order by Sunday 11 pm · ships Tue, Oct 27 · arrives Oct 29–Oct 31"),
`POST /store/checkout` (the cart → a hosted Stripe Checkout Session, the only
place a card is ever typed) and `GET /store/session/:id` (what the thanks page
shows once the webhook has recorded the order).

- **Stock is not a number typed in `store.json`.** What is sellable now is the
  batch cap (`stock` in `store.json`) minus what this batch has sold, and never
  more than the shelf can build once the orders already paid but not yet
  packed have taken their parts. A kit carrying `authorization: pending-fcc`
  is refused at checkout, by name, whatever the site shows.
- **The ship date is the batch cadence** — `BATCH_CLOSE_DAY` at
  `BATCH_CLOSE_HOUR` in `OPERATOR_TZ`, shipping `BATCH_SHIP_DAY` (Sunday 11 pm
  → Tuesday unset) — and **the arrival window is the same estimate** the
  order page and the tracking mail use after shipping, counted from the ship
  date. One method, before and after the sale. The Checkout Session carries
  both on its shipping option, so Stripe's page says the same thing.
- **The sale lands by webhook as before**; a site order also gets "We have
  your order" with the batch, the ship date, the window and the status link.
- **Tax.** The `stripe_tax` feature flag (Settings → Feature flags, or
  `FLAG_STRIPE_TAX=1`; `TAX_MODE=stripe` is the old spelling) turns on Stripe
  Tax (computed by the shipping address at checkout, a per-transaction fee),
  which is the honest setting from the first taxable sale. Off collects
  nothing, which is only right while you owe sales tax nowhere. The books
  carry whatever Stripe collected either way.
- **Demo mode takes no payments**: `/store/checkout` answers 503 and the site
  falls back to its own cart; the walkthrough drives `createCheckout` against
  a stub Stripe instead. `tests/storefront.test.mjs` holds the cadence across
  zone and weekday boundaries, ZIP → state, availability, every refusal, the
  exact session Stripe is asked for, and the routes.

### The pack bench is a gate

One or two people pack here, so the bench does not try to be a warehouse
system; it makes the four mistakes that cost a reship refusable by name. The
box card shows the carton, its inside size and the weight it should read, large.
Each line is a tick as it goes in. The lines that ship a pre-flashed board say
how many, and the board card counts the scans against that — a board flashed for
a different kit, or one more than the order holds, is refused at the scan (with
a × to take one back out). The scale field is checked against the plan. The
"Packed" button stays dim until all of that is done, and the server checks the
same proof when the form posts, so the gate holds without the browser's help.
"Pack anyway" exists for the day something is off and the box must still go; it
needs a reason, and what was skipped goes on the timeline with it. How many
boards a line needs comes from the data, never a guess: its recipe's board
parts (a SKU some registered board carries as its `board`), or, with no recipe,
one per unit when registered boards are flashed for that SKU. Unknown data gates
nothing. The order page's "Mark packed" sends you to the bench for that order.

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
- **The scale.** At either bench a measured weight is checked against the plan;
  past the tolerance (`fulfillment.weight_tolerance_oz` in `store.json`, 1.5 oz
  when unset) the pack or ship is refused with the delta — a missing or extra
  item is the usual cause — unless "anyway" is ticked, and then the timeline says so.
- **The postage.** The plan's cost is an estimate for a mid zone (the file says
  which); the label's real price, typed at the ship bench, is booked to
  `expenses:postage` and shown beside the shipping the customer paid. The
  **delivery estimate** the customer sees is bands by state from
  `SHIP_FROM_STATE` (the website's `fulfillment.ship_from.state` when unset),
  two to seven business days, replaced by the carrier's own word when USPS
  tracking is configured.

## What the customer is told, and when

The customer hears from us at every turn, by mail, and can see the same thing
any time on their page (`/o/<number>/<token>`, the link in every mail and on the
thanks page — no login). The promise the store made at checkout is written on
the order (`promised_ship_on`, `promised_arrive_by`), and every line below reads
it, so a mail and the page never disagree.

| Moment | Mail | Their page says |
|---|---|---|
| Checkout timed out | one mail, an hour later, with Stripe's link that reopens the same cart — only if they typed an email, and never a second | — |
| Paid | "We have your order": items, the batch it ships in, the arrival window, the page link | "Ships Tue, Nov 3 · arrives Nov 5–Nov 9"; a form to fix the address while it is on the shelf |
| Packed | — | "Packed and waiting for the label — ships Tue, Nov 3" |
| The ship day passes | the late apology, **a person's click** (a Decide card counts them; once per order), with a refund offer | "Running later than planned — it was due to ship Tue, Nov 3", and the same offer |
| Shipped | "On its way": carrier, tracking, the estimate | "On its way · estimated Nov 5–Nov 9", the tracking link |
| Carrier reports a problem | the carrier note, **a person's click**, once | "The carrier reports a problem", what happens next |
| Delivered (carrier or a click) | the "settled in?" automation, three days later | "Delivered Nov 6" |
| The address will not make a label | the address question, **a person's click**, once | unchanged (it still ships on the promise once fixed) |
| Canceled | by itself: what is coming back and when | "Canceled Oct 28", the money line |
| Refunded, in part or full | by itself, whoever recorded it (page, API, Stripe) | "Refunded $15.00", keep what arrived |
| A support ticket | the acknowledgment with the ticket number and the SLA; every reply in the same thread | — |

Every order page has a **The customer knows** card: what their page says right
now, the promise, every mail with whether it was sent, queued or failed, and the
nudges still owed. A failed customer mail is *now* on the home page and the
sidebar: the page says they were told, and they were not.

## How each piece behaves (the rules it keeps)

- **Where to look is computed, never typed.** `src/attention.mjs` turns every
  deadline into one of three levels — *now* (red: a clock has run out, or a
  person is already being failed), *soon* (amber: inside a stated window), *calm*
  (no mark) — and every page reads it from there: a count on the sidebar section
  colored by the worst thing inside it, the home page's focus strip and cards, the
  Decide queue ranked by the clock each card is against, the mark on a table
  row's left edge, a due date in a cell. The windows are the operator's promises,
  stated once in `WINDOWS` (two days to ship an order, six hours before an SLA,
  a week on a warranty claim); a new kind of work gets a level by stating its
  deadline, not by picking a color. The brand gold means "an action" and nothing
  else — a chart series never wears it.

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
  SameSite=Lax` (Lax so a code the phone's camera scans opens the bench
  logged in; every write is a POST under the CSRF token and the cross-site
  refusal).

## Feature flags

A change that should exist in the code before it is on for everyone is a
flag: declared once in `src/flags.mjs` (name, kind, default, owner, what it
does), read at the one branch with `isOn(db, cfg, 'name')`, flipped on
Settings or through `POST /api/flags/:name`, every flip an audit row. The
environment can pin one (`FLAG_<NAME>=1|0`), which is how staging runs
tomorrow's behavior on yesterday's data; Settings shows which source answers.
Two kinds: a *rollout* carries a `remove_by` date and the doctor names it once
that passes (a rollout is temporary — when it is on for good, delete the flag
and the branch it guarded); a *switch* is an operator's standing choice and
has no date. `tests/flags.test.mjs` holds the precedence, the refusals and the
audit.

## Layout

```
bin/backoffice.mjs      the CLI — same functions as the web admin, `backoffice help`
src/db.mjs              node:sqlite, migrations/, numbers (EL-, T-, WC-, INV-YYYY-), audit log, savepoint transactions
src/http.mjs            router, body parsing, cookies, CSRF, rate limiter, escaping
src/render.mjs          server-rendered HTML; no client JavaScript
src/modules/listing.mjs the store listing edited here (price, name, cap, hidden, window) laid over store.json; the patch for the file
src/modules/nexus.mjs   where sales tax is owed: the trailing year by state against the common thresholds; the doctor's sentence
src/modules/labels.mjs  the label bought from the ship bench (Shippo), the demo's pretend one, the readable fallback
src/modules/recovery.mjs every hosted checkout written down, the one follow-up an abandoned one gets, the conversion number
src/modules/notify.mjs  the new-order nudge to the operator: a mail, and a push if NTFY_URL is set
src/modules/promise.mjs the promise made at checkout, one honest line per state for the customer, the mails a person sends with one click, what the customer has been told
src/attention.mjs       the one scale of urgency (now / soon / calm) from stated deadlines; the sidebar badges, the home page, the Decide queue, every table row and the digest read it
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
| The admin in a real browser | the forms as a person submits them — log in, ship an order, answer a ticket from a canned reply, receive stock, post an expense, note and tag an order, create and try a discount code, tag a customer, read a product, the analytics charts, the header search, bulk-tag a batch, log out — with zero page or console errors. This is the layer that catches a button whose form is missing a field | `npm i --no-save playwright && npx playwright install chromium`, then `npm test`; it skips (and says so) when Playwright is absent, and CI makes a skip a failure |
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

## On a phone and an iPad

The admin is one HTML layout for every screen, and `tests/mobile.test.mjs`
holds it to the rules that make it usable with a thumb, on every section
and the detail pages a day's work lands on, at a phone's width and an
iPad's: no page is wider than the screen (a wide table scrolls inside its
own box), every navigation link, button and field is at least 40 px tall,
every field is 16 px type or larger (iOS zooms the page when a smaller one
is focused), no text is under 12 px, and a table of five or more columns
folds into cards on a phone — the first named column leads, every other
cell carries its own label — so nothing needs a sideways scroll to read.
Fields open the keyboard they need (digits for counts and money, a decimal
pad for a scale reading, no autocorrect on serials and tracking numbers).
The pack and ship benches and the phone-scanner pages were built for a
phone first; the rest follows these rules by construction, so a new page
made from `layout()`, `table()` and `field()` passes without thought, and
one that does not fails the test by name.

