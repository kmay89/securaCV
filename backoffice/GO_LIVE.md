# Go live — from "impressive demo" to "the tool I rely on"

The code is tested; what makes it reliable is the practice around it. This
is the order. Each step has a check you can see on the Health page or run
with `backoffice doctor`.

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

**Check:** Health shows *Reconciled with Stripe · 0 missing* every day.

## 2. Backups that are proven, in two places

- Create a bucket on any S3-compatible store (Backblaze B2 and Cloudflare
  R2 are free at this size). Set `BACKUP_S3_URL`, `_KEY`, `_SECRET`,
  `_REGION`, and a long `BACKUP_ENCRYPTION_KEY` that you also keep
  somewhere the server is not (a password manager).
- Press **Back up now** on Health. Read the line: *taken and verified ·
  off-site copy read back*.
- Press **Run the restore drill**. Read the line: *the off-site copy boots,
  N orders, 0.1 h old*.
- Once, on a laptop: download the latest `.db.enc`, run
  `backoffice backup verify <file>` after decrypting with the key (the drill
  does exactly this), and open it with `BACKOFFICE_DB=<file> backoffice
  serve`. Now you know restore works *away from the server*.

**Check:** Health shows *Last backup verified*, *Off-site copy*, *Off-site
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
- Set `BACKOFFICE_BASE_URL=https://…` so cookies are `Secure` and HSTS is
  on. Health checks it.
- For the first month, keep comparing Orders with the Stripe dashboard once
  a week. `reconcile` runs daily and raises an exception on any gap.

## The weekly ten minutes, forever

1. Open Home. Read the cards. Do what they say.
2. Open Health. Everything green? Then nothing is silently wrong.
3. Open Exceptions. Empty? Then nothing needs a decision.
4. Once a month, read the Audit log for the word *failed*.

That is the whole practice. Everything else is automated and recorded.
