# Go live — from "impressive demo" to "the tool I rely on"

The code is tested; what makes it reliable is the practice around it. This
is the order. Each step has a check you can see on the Health page or run
with `backoffice doctor`.

## 0. Three copies, each saying which it is

The server runs production (`BACKOFFICE_ENV=production`), staging and the
demo from the same image (`infra/README.md`). Every merge deploys to
staging, which runs on last night's production copy with test keys; a
button promotes a build to production. Before anything below, check the
page title and the badge beside the company name: they say *staging* or
*dev* when it is not the real one, and `backoffice version` (or `/healthz`)
says which build. Production refuses to start on a demo seed, on a file
staging labeled, or without https, so the wrong data in the wrong place
fails loudly rather than quietly.

## 1. The shadow fortnight (Stripe test mode, a throwaway mailbox)

Run it for real with nothing at stake.

- `backoffice setup`, then set `STRIPE_WEBHOOK_SECRET` from a **test-mode**
  webhook endpoint pointed at `https://<host>/webhooks/stripe`, and a
  restricted **test-mode** `STRIPE_API_KEY` (read charges, payouts, checkout
  sessions).
- Point `IMAP_URL` / `SMTP_URL` at a mailbox you made for this.
- Buy your own products with Stripe's test cards. Watch each order arrive,
  pack it on the Pack bench, ship it on the Ship bench, see the tracking
  mail land in the throwaway inbox. Reply to that mail; watch the ticket
  thread.
- Every morning for two weeks, compare the Orders page with the Stripe
  test dashboard. They must agree to the order. `backoffice reconcile`
  says so in one line.

- Set `OPERATOR_TZ` and `SHIP_FROM_STATE` so the times you read and the
  delivery estimates customers read are yours. If you want the carrier's
  word rather than the estimate, make a USPS Developer Portal app with the
  Tracking API and set `USPS_CLIENT_ID` / `USPS_CLIENT_SECRET`; `backoffice
  track` asks about every open parcel once, so you can watch an order go
  "out for delivery" → "delivered" on the carrier's day.
- Open a test order's status page (the link is in its tracking mail and on
  the order page). That is what a customer sees; it needs no login.

**Check:** Health shows *Reconciled with Stripe · 0 missing* every day.

## 2. Backups that are proven, in two places

- Create a bucket on any S3-compatible store (Backblaze B2 and Cloudflare
  R2 are free at this size). Set `BACKUP_S3_URL`, `_KEY`, `_SECRET`,
  `_REGION`, and a long `BACKUP_ENCRYPTION_KEY` that you also keep
  somewhere the server is not (a password manager).
- Press **Back up now** on Health. Read the line: *taken and checked ·
  off-site copy read back*.
- Press **Run the restore drill**. Read the line: *the off-site copy boots,
  N orders, 0.1 h old*.
- Once, on a laptop: download the latest `.db.enc`, run
  `backoffice backup check <file>` after decrypting with the key (the drill
  does exactly this), and open it with `BACKOFFICE_DB=<file> backoffice
  serve`. Now you know restore works *away from the server*.

**Check:** Health shows *Last backup checked*, *Off-site copy*, *Off-site
copy sealed* and *Restore drill* all green. The drill repeats weekly on its
own; a failure opens an exception.

## 3. Watched from outside

The app cannot report its own death. Point any uptime checker (a free
tier is plenty) at `https://<host>/healthz` and `https://<host>/api/health`
with a read-only token (`backoffice token monitor --read-only`): the second
answers 503 whenever any health check fails, so the checker's alert *is*
the doctor's verdict. Record the checker's URL on Health.

**Check:** *Watched from outside* is green.

## 4. The morning digest

Set `OPERATOR_EMAIL` and `DIGEST_HOUR` (UTC). Tomorrow's mail is today's
list, the thirty-day numbers, and the health checklist. If it ever stops
arriving, that is itself the alarm.

## 5. Go live

- Switch `STRIPE_WEBHOOK_SECRET` and `STRIPE_API_KEY` to live mode; point
  the live webhook at the same URL.
- Set `SUPPORT_ADDR`, the real `IMAP_URL` / `SMTP_URL` (a relay with SPF and
  DKIM for your domain — never a home connection), `MAIL_FROM`.
- Set `BACKOFFICE_BASE_URL=https://…` (it is also what the slip and board-label
  codes encode, so the phone can open them) so cookies are `Secure` and HSTS is
  on. Health checks it.
- For the first month, keep comparing Orders with the Stripe dashboard once
  a week. `reconcile` runs daily and raises an exception on any gap.
- Invite the people who will work in it, from *People* (or `backoffice users
  invite`), each with the smallest role that does their job: `ops` for whoever
  packs and ships, `support` for whoever answers mail, `viewer` for the
  accountant. The invitation is a link; they choose their own password and
  scan their own authenticator. Nobody shares a login, so the audit log can
  say who did what. Keep two owners, so losing one phone is an invitation,
  not an outage.

## The weekly ten minutes, forever

1. Open Home. Read the cards. Do what they say.
2. Open Health. Everything green? Then nothing is silently wrong.
3. Open Exceptions. Empty? Then nothing needs a decision.
4. Once a month, read the Audit log for the word *failed*.

That is the whole practice. Everything else is automated and recorded.
