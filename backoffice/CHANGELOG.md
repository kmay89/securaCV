# Backoffice changelog

What changed between the builds the server runs. The version is `package.json`'s;
the build hash on `/healthz`, the Health page and `backoffice version` is the commit
CI stamped into the image. `tests/environment.test.mjs` holds the top entry here to
the package version, so neither moves without the other.

## 0.7.0

- **Sign-in links.** "Email me a sign-in link" on the login page (with a mail
  relay): the link stands in for the password, never for the authenticator.
  Good for `SIGNIN_LINK_MINUTES` (15), once, and only in the browser that
  asked for it — that browser holds a nonce cookie the link checks, so a link
  read out of a mailbox somewhere else opens nothing. Token and nonce are
  stored hashed. The answer to a request is the same for any address, so
  there is nothing to enumerate; a wrong code counts like a wrong password.
  `login_links`, migration 014.
- **The watch** (`src/guard.mjs`). A scanner — sixty missing or refused pages
  from one address in ten minutes — is blocked for ten minutes (never a
  session, never the store, the webhook or a customer's page) and the phone
  hears. An address locked out after eight failed logins: the phone hears,
  once an hour per address. A sign-in from a network the person has not used
  in thirty days: the person is mailed, the phone hears, the audit log says.
  `ADMIN_ALLOW_CIDRS` makes the admin and its door exist only for your
  networks (everything else sees a 404; the store, the webhook and the
  customers' pages are untouched). `SESSION_BIND_IP=1` ends a session that
  turns up from another network than it was opened on. Four doctor rows.
- **The boundary.** Every text an outsider writes — a ticket's subject and
  body, a reply by mail, a form, a buyer's name and notes from Stripe — is
  scrubbed of zero-width, bidi-override and control characters on the way
  in, so nothing can hide an instruction or spoof a name. The JSON API marks
  those fields as data, not instructions (`_untrusted`, `_notice`, and the
  same in headers) for the AI operator the API is built for.
- **Crawler hygiene.** `X-Robots-Tag: noindex, nofollow, noarchive, noai,
  noimageai` on every answer; `/robots.txt` disallows all.

## 0.6.0

- **The store listing, edited here.** A product's price, name, this batch's
  cap, whether it shows at all, and the window it sells in (a launch: listed
  today, on sale from a date; or an end) are edited on its Products page and
  served by `/store/catalog` the moment they are saved — the website takes the
  live price, name, hidden flag and window from that answer, so the price on
  the page and the price Stripe charges are one number. `store.json` stays the
  base and the fallback; the Products page prints the patch the file would need
  and the doctor counts what is edited here. `product_overrides`, migration 013,
  `src/modules/listing.mjs`.
- **Your phone, in Settings.** Paste an ntfy topic, keep or drop the new-order
  nudge, send a test push. The environment's `NTFY_URL` still wins; staging and
  the demo push nowhere. A doctor row says where a new order reaches you.
- **Where sales tax is owed.** `src/modules/nexus.mjs` reads the trailing
  year's orders by state against the common 2026 thresholds (the home state
  always; $100,000 in most states, some with 200 orders, a few at $250,000 or
  $500,000; five with none): Books shows every state's standing, and the doctor
  fails when nothing is collected but tax is owed, or a state is within 20% of
  its line. Collecting is the `stripe_tax` flag; registering is a human's step.

## 0.5.0

- **Labels from the ship bench.** With `SHIPPO_API_TOKEN` and a `SHIP_FROM_*`
  return address set, the bench has one more button: buy the USPS label for
  the box the order was packed in, write the tracking number and the postage
  onto the order, ship it (the customer gets their mail) and open the PDF to
  print. It refuses an address that would not make a label and a blank return
  address before any money moves. The demo draws a pretend label
  (`/orders/:id/label`, also the readable fallback for any shipped order).
  The doctor has a row for it. `src/modules/labels.mjs`.
- **The customer fixes their own address.** On their signed page, while the
  order is on the shelf and no label exists: the same validation the label
  uses, a line on the timeline under "customer", rate-limited per order.
  Once the label exists, the label is the address.
- **Abandoned checkouts, counted and followed up once.** Every hosted
  Checkout Session is written down (`checkout_sessions`, migration 012);
  Stripe's events move it to paid, expired (with the recovery link Stripe
  mints, which reopens the same cart for 30 days) or recovered. An expired
  session with an email gets one mail, `CHECKOUT_RECOVERY_HOURS` later,
  unless an order from that address has arrived since; never a second.
  Marketing shows the checkout: opened, paid and the conversion, abandoned
  and what it was worth, written to, came back. Home's "All the numbers"
  has the conversion. No pixel, no cookie, nothing recorded about a visitor.
  `src/modules/recovery.mjs`, the `recovery` job.
- **A new order reaches your phone.** The minute the webhook lands: a mail
  to `OPERATOR_EMAIL` (on unless `NOTIFY_NEW_ORDER=0`) and, only if
  `NTFY_URL` is set, one push to that ntfy topic — number, money, lines,
  ship day, never the address. `src/modules/notify.mjs`.
- Staging and the demo buy no postage and push to no phone.

## 0.4.0

- **The customer always knows where their order is.** The promise the store
  made at checkout — the day it ships, the window it arrives in — is written
  on the order (`promised_ship_on`, `promised_arrive_by`, migration 011) and
  every surface reads it. The customer's page (`/o/<number>/<token>`) now has
  one honest line for every state: "ships Tue, Nov 3 · arrives Nov 5–Nov 9"
  while it waits, "Packed and waiting for the label" once pulled, "Running
  later than planned — it was due to ship …" with an apology and a refund
  offer the moment a batch slips, the carrier's word and a tracking link in
  transit, "The carrier reports a problem" with what happens next, "Delivered",
  and for a cancel or a refund what money is coming back and when. The
  stepper shows the promised ship day before it happens. The support address
  is on the page.
- **A cancel and a refund mail by themselves**, from the transition and the
  refund record, whoever triggered them (the order page, the API, Stripe's
  webhook, a lost dispute): what is coming back, when banks show it, keep
  what arrived.
- **Three mails a person sends with one click, once per order:** the late
  apology (a Decide card counts the late customers not yet told; "Send the
  apology" tells them all), the carrier-problem note when tracking reports an
  alert, and the address question when the address would not make a label —
  each also a button on the order page, each a line on the order's timeline.
- **"The customer knows", on every order page:** what their page says right
  now, the promise made at checkout, every mail sent to them with whether it
  was sent, queued or failed (and the error), and the nudges that are still
  owed.
- **Urgency follows the promise, not a flat window.** An order is *now* past
  the day it was promised to ship, *soon* on the ship day itself, calm before
  — so the weekly batch no longer makes a four-day-old order amber. A carrier
  alert and a failed customer mail are *now* in their own right.
- A demo scenario, "A batch slips", shows the whole path.

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
