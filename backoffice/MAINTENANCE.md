# Maintenance — the minute, the ten, the half hour, and the bad day

The backoffice is one process and one file. Keeping it honest takes a few
minutes a week, and every one of them is a reading, not a chore: the system
writes down what it did and proves what it holds, and you look.

## Every day: one minute, in your inbox

The morning digest (`OPERATOR_EMAIL`, `DIGEST_HOUR`) ends with a SECURITY
block. Read its first line. **"quiet"** means nobody failed to sign in,
nobody was blocked or locked out, nobody signed in from a new network, and
nothing changed about people, tokens, flags or settings. Anything else is
named on the lines below it, with the address or the person.

Keep the digests. The last line of the block is the **anchor** — the audit
chain's head, `id:mac`. It is the one copy of the record that is not on the
server. A week of them is enough.

## Every week: ten minutes

1. Open **Home**. Do what the cards say.
2. Open **Health**. Every row green? Then nothing is silently wrong. Read the
   Security block at the bottom once, even on a quiet week, so that an unquiet
   one reads as different.
3. Open **Exceptions**. Empty? Then nothing needs a decision.
4. `backoffice verify` (or trust the daily `verify` job's row on Health).

## Every month: half an hour

- `backoffice verify --anchor <the anchor from a digest about a week old>`.
  "still on the chain" is the answer you want.
- **Settings → API tokens.** Each one still has a reason to exist and a
  recent *Last used*? Revoke the rest.
- **People.** Everyone listed still works here, with the role they need?
- **Your account.** The Health page says when the oldest owner password
  turns a year old; rotate before it does. A new authenticator is set up on
  the same page, and the old one works until the new one has been seen.
- **Backups.** The Health rows for the nightly copy, the off-site copy and
  the restore drill are green; `backoffice restore --offsite` into a scratch
  directory once a quarter is the only proof that matters.
- **The build.** `backoffice version` against the repository; the Node row on
  Health says how long this runtime is maintained.

## When you update

```
git pull                      # or pull the image
backoffice doctor             # on the old build: everything green before you move
systemctl restart backoffice  # or docker compose up -d
backoffice doctor             # on the new one: the self-check rows, the schema row
```

A migration copies the database first (`backups/pre-migrate-…`); a build that
finds a newer schema than it knows refuses to start and says which. Both are
in the schema row on Health. Nothing else about an update needs a human.

## The bad day: something is not yours

A sign-in you did not make. A token used that nobody uses. A change to a
person or a setting you did not make. An audit chain the doctor calls broken.
In this order, and nothing in parallel:

1. **Lock down.** `backoffice lockdown --why '…'` or Settings → *Lock down
   now*. Every session (yours too), every API token and every sign-in link is
   gone, in one audited step. Nothing else changes. Takes a second.
2. **Rotate what the intruder may hold.** Sign in again (password +
   authenticator), change your password on *Your account*, set up a new
   authenticator there, and have every other person do the same. Rotate
   `STRIPE_WEBHOOK_SECRET`, `SMTP_URL`/`IMAP_URL` credentials, `SHIPPO_API_TOKEN`
   and the backup bucket's key if the environment file could have been read.
3. **Read.** The Security block for the last 24 hours, then the **Audit log**
   filtered to the actor you did not expect (`token:<name>`, an email, or
   *system*), then from the first odd row forward. Every action here is
   written down with who and when.
4. **Prove the record.** `backoffice verify --anchor <yesterday's anchor>`.
   - Chain OK, anchor on it: what you read is what happened.
   - Chain OK, anchor *not* on it: the log was rewritten end to end, key
     included. Treat the box as the intruder's; restore from a backup whose
     chain the same command verifies, on a fresh host, with a fresh
     `audit.key`.
   - Chain OK, anchor on it: everything up to that anchor is what happened.
     The rows since the last digest are not yet anchored; read them as
     claims, not proof, and compare them with what the intruder would have
     wanted hidden (a sign-in, a token, a change to a person).
   - Chain broken at a row: everything before it is trustworthy, the row and
     after are not. Same answer.
5. **Narrow the door.** Set `ADMIN_ALLOW_CIDRS` to your networks and
   `SESSION_BIND_IP=1` if they were not already; a stolen cookie is then worth
   nothing from anywhere else.
6. **Tell who needs telling.** If customer data was readable, the website's
   privacy page states what was promised to them; Stripe holds the cards,
   never this database.

The whole list is written so that it can be followed at three in the morning
without judgment calls. The judgment comes afterward, with coffee.

## What is automated, so that you need not remember it

| Every | What | Where it shows |
|---|---|---|
| 30 s | the mail outbox | Mail |
| 10 min | abandoned-checkout follow-ups, the SLA watch | Support, Exceptions |
| hour | stock, invoices, tracking, automations | Home cards |
| day | a checked backup (off-site when configured), the Stripe reconcile, the **audit chain verify**, the digest | Health |
| week | the restore drill | Health |
| boot | the self-check: key private, files private, Node maintained, chain tail verifies | Health, the console |
