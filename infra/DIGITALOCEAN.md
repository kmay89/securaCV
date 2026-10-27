# The server on DigitalOcean — the first time, in order

[`README.md`](README.md) is the kit; this page is the same first server with
every DigitalOcean screen named, in the order that does not have to be undone.
About an hour, most of it waiting on DNS and reading what `kit doctor` says.
[`OWNERS-MANUAL.md`](OWNERS-MANUAL.md) says why each decision was made.

Three hostnames are assumed: `ops.securacv.com` (production),
`staging.securacv.com` (last night's data, test keys, where every merge lands)
and, if you want the public demo, `demo.securacv.com`. Change them everywhere
at once or nowhere.

## 0. Before you open DigitalOcean

Have these in hand; each is a stop later if it is missing.

- **An SSH key on your laptop**, ed25519: `ssh-keygen -t ed25519` if
  `~/.ssh/id_ed25519.pub` does not exist. The private half never leaves the
  laptop; a second laptop gets its own key, added later.
- **The password manager entry** named *SecuraCV server*, empty for now. Every
  value below goes into it as you make it. The manual's rule: this entry is
  the business.
- **Where `securacv.com`'s DNS lives** (the registrar, or Netlify DNS) and a
  login to it. You will add three `A` records.
- **The support mailbox** (`hello@securacv.com`) with an *app password* for
  SMTP and IMAP. Google Workspace and Fastmail both issue one under the
  account's security settings; the account password itself is never used.
- **Stripe**, with test mode open. Live keys come in GO_LIVE step 5, not now.
- **A backup bucket.** Backblaze B2 is free at this size and is a different
  company from the box, which is the point of off-site: make a bucket named
  for example `securacv-backoffice`, private, and an *application key*
  limited to that bucket (read and write for production; a second, read-only
  one for staging). Note the endpoint (`s3.us-west-004.backblazeb2.com` or
  similar) and region (`us-west-004`). DigitalOcean Spaces works too
  (`https://nyc3.digitaloceanspaces.com/<bucket>/backoffice`,
  `BACKUP_S3_REGION=nyc3`, a Spaces access key), at $5 a month; the
  backoffice's own *read back* check tells you either is right.
- Two long random strings, made now and pasted into the entry:
  ```sh
  openssl rand -base64 48    # BACKUP_ENCRYPTION_KEY — seals every off-site copy; without it the copies are noise
  openssl rand -hex 32       # BACKOFFICE_AUDIT_KEY — marks every audit row; a new server needs it to verify the old rows
  ```

## 1. The user-data, on your laptop

```sh
git clone https://github.com/kmay89/securaCV.git && cd securaCV
infra/bin/render-cloud-init --ssh-key ~/.ssh/id_ed25519.pub --email you@yourdomain > ~/user-data.yaml
```

The email is the Let's Encrypt contact (certificate expiry warnings go
there; they never fire while Caddy runs). Open the file once; it is short,
and it is the whole server.

## 2. The droplet

DigitalOcean → **Create → Droplets**.

| Screen | Choose | Why |
|---|---|---|
| Region | **New York** (NYC3, or whichever NYC is offered) | Nearest to where parcels ship from; US data stays in the US |
| Image | **Ubuntu 24.04 (LTS) x64** | What the user-data was written for |
| Size | **Basic → Regular → 2 GB / 1 vCPU** (about $12) | Comfortable; the kit adds 1 GB of swap. 1 GB runs it; 2 GB is the one you never think about again |
| Backups | **Enable**, weekly (about $2.40) | The second backup layer: a whole-machine snapshot DigitalOcean keeps, unencrypted on their side. The backoffice's own sealed off-site copy is the first |
| Authentication | **SSH key** → add `~/.ssh/id_ed25519.pub` | Passwords are never enabled; the user-data turns them off |
| Advanced options | **Add initialization scripts (free)** → paste `~/user-data.yaml` whole | This is the server. Nothing is done by hand after it |
| Hostname | `edge` | What the user-data calls it; `kit` does not care |
| Tags, project | as you like | — |

Create it. Copy the IPv4 address into the password manager entry.

**Reserved IP** (Networking → Reserved IPs → assign to the droplet): free
while attached. The DNS records and GitHub's `DEPLOY_HOST` then survive the
day you rebuild the droplet from the same user-data. Use the reserved address
everywhere below.

## 3. The cloud firewall

Networking → **Firewalls → Create**. Inbound: SSH 22 (from *your* address if it
is fixed, else all), HTTP 80, HTTPS 443. Outbound: all. Apply it to the
droplet. The box runs `ufw` with the same rules; two fences, one list.

## 4. DNS

At the domain's DNS, three `A` records to the reserved address: `ops`,
`staging`, `demo` (skip `demo` if you will not run the public demo). Low TTL
(300). Caddy asks Let's Encrypt for a certificate the first time each name is
requested, and Let's Encrypt looks the name up — so the records must be live
before step 6. `dig +short ops.securacv.com` from the laptop says when they
are.

## 5. First login

Three to five minutes after creation:

```sh
ssh ops@<reserved ip>
kit doctor
```

Everything but the apps is green: firewall on, keys only, unattended
upgrades, fail2ban, Docker, the edge network, Caddy. Red means the user-data
did not finish; `sudo cloud-init status --long` says where, and the fix is
to destroy the droplet and create it again from the same file — never to
patch it by hand.

## 6. Production

```sh
kit app add backoffice
kit env backoffice
```

That opens `/srv/apps/backoffice/.env` (0640, readable by `ops` and the
`deploy` user only). Fill it; the comments in
[`../backoffice/.env.example`](../backoffice/.env.example) explain every
knob, and these are the ones a first server cannot go without:

| Key | Value | Note |
|---|---|---|
| `BACKOFFICE_BASE_URL` | `https://ops.securacv.com` | Production refuses to start on anything but https |
| `SITE_ORIGIN` | `https://securacv.com` | CORS for the website's forms and store calls |
| `SUPPORT_ADDR`, `MAIL_FROM` | the mailbox | `Errer Labs <hello@securacv.com>` |
| `SMTP_URL`, `IMAP_URL` | the mailbox's submission and IMAP servers with the app password | `@` in the user becomes `%40`; `smtp://…:587` is STARTTLS, `smtps://…:465` implicit TLS |
| `OPERATOR_EMAIL` | **your own inbox**, not `hello@` | The morning digest lands here, and its last line is the audit chain's anchor: the one copy off the box. It must be a mailbox the server does not read |
| `OPERATOR_TZ`, `SHIP_FROM_STATE` | `America/New_York`, `MA` | — |
| `STRIPE_WEBHOOK_SECRET`, `STRIPE_API_KEY` | **test-mode** values | The shadow fortnight (GO_LIVE step 1); live ones later |
| `BACKUP_S3_URL`, `_KEY`, `_SECRET`, `_REGION` | the bucket from step 0 | `https://<endpoint>/<bucket>/backoffice` |
| `BACKUP_ENCRYPTION_KEY` | the first random string | — |
| `BACKOFFICE_AUDIT_KEY` | the second random string | Set it explicitly so it is in the password manager; left blank the server makes one in `/srv/data/backoffice/audit.key`, which a rebuilt server would not have |
| `SESSION_BIND_IP` | `1` | A stolen session cookie is worthless from any other network. `ADMIN_ALLOW_CIDRS=<your office address>/32` instead, if your address never changes |
| `NTFY_URL` | optional | A push for new orders and security events; or set it later under Settings |

Save. Then the hostname and the first deploy:

```sh
kit site add ops.securacv.com backoffice:8787     # Caddy gets the certificate on first request
kit deploy backoffice latest                      # pulls ghcr.io/kmay89/securacv-backoffice:latest, waits for healthy
```

`kit deploy` prints the tag it ran, hands the data directory to the image's
user, and rolls back by itself if the container is not healthy within two
minutes. The first lines of `kit logs backoffice` are the self-check:
`6 checks pass (audit key from the environment)`.

The first owner, from inside the container:

```sh
kit run backoffice node bin/backoffice.mjs setup --email you@yourdomain --name 'Your Name'
```

It asks for a password (twelve characters or more) and prints an `otpauth://`
line. In your authenticator app choose *enter a key manually* and paste the
`secret=` value from it (or paste the whole line into a QR generator and
scan). Then open `https://ops.securacv.com` and sign in with both.

## 7. Prove the backups before anything else runs

On the Health page, in this order:

1. **Back up now** → read *taken and checked · off-site copy read back*.
2. **Run the restore drill** → read *the off-site copy boots, 0 orders*.
3. Note the four rows *Last backup checked*, *Off-site copy*, *Off-site copy
   sealed*, *Restore drill* are green.

A wrong bucket value fails here, with the words, while nothing is at stake.

## 8. Watched from outside

DigitalOcean → **Monitoring → Uptime → Create check**: HTTPS,
`https://ops.securacv.com/healthz`, alert by email. That answers 503 when the
database or the scheduler is wrong. For the full doctor's verdict add a
second check on `https://ops.securacv.com/api/health` with a read-only
token in the `Authorization: Bearer …` header
(`kit run backoffice node bin/backoffice.mjs token create monitor --read-only`).
Paste the check's URL on the Health page under *Watched from outside*.

## 9. Staging beside it

```sh
kit app add backoffice-staging
kit env backoffice-staging          # test keys; production's bucket with the READ-ONLY application key; nothing live
kit site add staging.securacv.com backoffice-staging:8787
kit deploy backoffice-staging latest
kit refresh backoffice-staging      # restores last night's production copy into it; cron does this at 04:10 from now on
```

Its pages say *staging* in the title; it mails nothing and writes nothing to
the bucket. Sign in with your production credentials.

## 10. Let GitHub deploy to staging, and you promote to production

On the server, once:

```sh
kit deploy-key
```

It prints three things. In the repository, **Settings → Secrets and
variables → Actions**:

| Secret | Value |
|---|---|
| `DEPLOY_HOST` | the reserved address |
| `DEPLOY_SSH_KEY` | the private key block, shown once |
| `DEPLOY_KNOWN_HOSTS` | the `known_hosts` line it printed |

| Variable | Value |
|---|---|
| `DEPLOY_APPS` | `backoffice-staging` (add `backoffice-demo` if you run the demo; never `backoffice`) |
| `DEPLOY_URL` | `https://staging.securacv.com` |
| `PRODUCTION_URL` | `https://ops.securacv.com` |

From the next merge on, staging updates itself and the workflow proves it
from outside. Production moves only by **Actions → Promote backoffice to
production → Run**, which sends the build staging runs, waits for healthy,
and rolls back if not.

## 11. The website

The store already reads the live catalog from `https://ops.securacv.com/store/`
(the website's `_headers` names it in `connect-src`). When the contact and
waitlist forms move to the backoffice (`../backoffice/deploy/website-forms.html`),
the one site-side edit is adding `https://ops.securacv.com` to `form-action`
and to `connect-src` in that file; `tests/csp.test.mjs` over there pins it.

## 12. The entry in the password manager, complete

By now it holds: the reserved address; the SSH key's name; every line of both
`.env` files; the two random strings; the bucket's endpoint, both application
keys and their ids; the Stripe account; the mailbox's app password; the
DigitalOcean login and the card it bills. `kit env backoffice` on a new box is
pasting from here. Nothing in the repository, nothing on the server, is the
only copy of any of it.

## 13. Then GO_LIVE

[`../backoffice/GO_LIVE.md`](../backoffice/GO_LIVE.md) from step 1: the shadow
fortnight with Stripe's test cards, the morning digest arriving, then the
live keys and the people invited with the smallest role that does their job.
[`../backoffice/MAINTENANCE.md`](../backoffice/MAINTENANCE.md) is the routine
after that: a minute a day, ten a week.

## What you did not do, on purpose

- No password login, no root login, no port but 22, 80 and 443.
- No build on the server; it pulls what CI published.
- No secret on the server that is not also in the password manager.
- No production deploy by merge; a person presses the button.
- No certificate to renew, no patch to apply, no backup to remember.
