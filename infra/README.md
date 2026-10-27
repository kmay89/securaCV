# One server, many apps — the repeatable kit

The whole company runs on one small VPS: Caddy at the edge, every project a
container that only Caddy can reach, data on the server's own disk, proven
backups off-site, deploys on merge, and a doctor that says whether the server
is healthy. Adding a project is three files. Adding a domain is one. A new
server is one paste.

Cost: about $4–6 a month for the box (Hetzner CX22, DigitalOcean's smallest
droplet — any Ubuntu 24.04 host with 2 GB; the kit is provider-agnostic), $0
for the off-site backup bucket at this size (Backblaze B2 or Cloudflare R2),
$0 for certificates (Let's Encrypt through Caddy). Nothing else recurs.

Three apps come with it: the **backoffice** (production), **backoffice-staging**
(the same image on last night's production copy, with test keys — every merge
lands here first) and **backoffice-demo** (synthetic data, public). Production
changes only when a person presses **Promote backoffice to production** in
Actions, naming the build staging runs.

```
/srv/kit/infra/           this directory, a sparse checkout of the repo (kit update = git pull)
/srv/edge/                Caddy: compose.yml, Caddyfile, sites/<host>.caddy — one file per hostname
/srv/apps/<name>/         one app: compose.yml, .env (0640, ops:kit), .tag (what is deployed)
/srv/data/<name>/         that app's data — the only thing on the box that matters
/srv/sites/<host>/        a static site Caddy serves from disk (optional)
```

## The shape, and why

- **One edge.** Caddy owns :80 and :443, gets and renews certificates by
  itself, and forwards each hostname to one container on the private
  `edge` network. Apps publish **no ports**; the firewall allows 22, 80, 443
  and nothing else. There is exactly one place TLS, headers and routing live.
- **One container per app, one directory per app.** `compose.yml` says which
  image; `.env` holds its secrets (mode 0640, owner `ops`, group `kit` so the
  deploy user can start it; never in git); `/srv/data/<name>`
  is its disk. Delete the directory and the app is gone; nothing else knows it.
- **Images come from CI, not from the server.** The server never builds. CI
  publishes `ghcr.io/<owner>/<image>:<sha>` and `:latest`; a deploy is "pull
  this tag, start it, wait until it is healthy, or put the old tag back".
- **Deploys arrive over SSH as a locked-down user.** The CI key can run one
  command, `deploy <app> <tag>`, and nothing else (an SSH forced command). A
  leaked CI key can redeploy an app; it cannot open a shell.
- **The box is disposable; the data is not.** A new server is cloud-init plus
  the `.env` files plus a restore from the off-site backup. The backoffice
  proves its own backups (opens them, checks integrity, balances the books,
  drills a restore weekly); the kit's job is to keep `/srv/data` on a real
  disk and the server patched.

> Not the technical reader? [`OWNERS-MANUAL.md`](OWNERS-MANUAL.md) is the
> same server in plain language: why each decision, what can break, what it
> costs, what to do, and the two things never to lose.

## First server (about fifteen minutes, most of it DNS)

1. **Make the user-data.** From this directory on your laptop:
   ```sh
   infra/bin/render-cloud-init --ssh-key ~/.ssh/id_ed25519.pub --email you@example.com > /tmp/user-data.yaml
   ```
   That is `cloud-init.yaml` with your key and the ACME contact filled in. Read
   it; it is short.
2. **Create the server** at any provider, Ubuntu 24.04, and paste the file as
   "user data" / "cloud-init". Give it a static IPv4. Point DNS `A` records at
   it for every hostname you will serve (`ops.example.com`, `demo.example.com`).
3. **Wait for it** (three to five minutes), then `ssh ops@<ip>` and run
   `kit doctor`. Everything but the apps should be green: firewall on, SSH
   keys only, unattended upgrades on, fail2ban on, Docker up, Caddy up.
4. **Add the first app:**
   ```sh
   kit app add backoffice          # copies the compose, makes /srv/data/backoffice and a 0640 .env from the example
   kit env backoffice              # opens the .env in $EDITOR: paste the real values
   kit site add ops.example.com backoffice:8787
   kit deploy backoffice latest    # pulls, starts, waits for healthy
   ```
   Then `https://ops.example.com` is live with a certificate. Run
   `backoffice setup` once inside it (`kit run backoffice node bin/backoffice.mjs setup`).
5. **Add staging beside it.** Same three lines with the staging app, and a
   second hostname: `kit app add backoffice-staging`, `kit env
   backoffice-staging` (Stripe **test** keys, production's bucket values so
   it can read the backups, nothing else live — the app refuses a live key),
   `kit site add staging.example.com backoffice-staging:8787`, `kit deploy
   backoffice-staging latest`. Then `kit cron` installs the nightly refresh
   (a new server already has it): at 04:10 staging stops, restores the latest
   off-site production copy, and starts — so every morning staging is
   yesterday's data on whatever merged. `kit refresh backoffice-staging` does
   it now. Log in with your production credentials; the pages say *staging*
   in the title and a banner, mail and issues are muted, and nothing it does
   reaches the backup bucket.
6. **Let CI deploy from now on — to staging.** On the server: `kit deploy-key`
   prints a fresh key pair's public half already installed for the `deploy`
   user with the forced command, and the private half once, for you to put in
   the repository's secrets as `DEPLOY_SSH_KEY`, with `DEPLOY_HOST` (the IP or
   hostname) and `DEPLOY_KNOWN_HOSTS` (the line it also prints, so CI pins the
   server's host key and a wrong server is refused). `DEPLOY_APPS` is the
   space-separated list to redeploy on every merge, `backoffice-staging` by
   default (add `backoffice-demo` for the public demo; never `backoffice` —
   the workflow refuses it). Set the variable `DEPLOY_URL` to staging's
   address and the workflow proves from outside that staging answers
   `ok staging <build>` on `/healthz` after each merge. It does nothing at all
   while the secrets are absent.
7. **Production moves when you say so.** Actions → **Promote backoffice to
   production** → Run. With the tag field empty it asks the server which build
   staging runs, deploys that tag to `backoffice` (the usual pull, start, wait
   for healthy, roll back by itself if not), and with the variable
   `PRODUCTION_URL` set proves production answers `ok production <build>`.
   The backoffice writes a `deploy.boot` audit row the first time a build runs
   on its database, so Home → Activity says what went live and when. Roll
   back is the same button with the previous tag (`kit ps` on the server
   shows it), or `kit deploy backoffice <previous>` over SSH.

## Adding the next project

Three files. A project is anything that ships as a container and answers
`/healthz` (or any path; say which in the compose healthcheck).

```sh
kit app add myapp --image ghcr.io/you/myapp      # compose.yml from the template, data dir, empty .env
kit env myapp
kit site add myapp.example.com myapp:3000
kit deploy myapp latest
```

A static site is one file and a directory: put the files in
`/srv/sites/<host>/` and `kit site add <host> static`. Caddy serves it with the
same headers and certificate handling as everything else.

CI for the new project: copy `.github/workflows/backoffice-deploy.yml`, change
the image workflow it waits for and the app name. The server side needs
nothing new — the forced command already accepts any app under `/srv/apps`.

## Day to day

| Need | Command |
|---|---|
| Is the server well? | `kit doctor` (firewall, SSH, updates, fail2ban, Docker, edge, each app's health, disk, memory, what tag runs where) |
| What is running? | `kit ps` |
| Logs | `kit logs backoffice` (`-f` follows) |
| Which build runs where | `kit ps`, or `kit tag <app>`; the app's `/healthz` says `ok <env> <build>` from outside |
| Promote staging's build to production | Actions → "Promote backoffice to production" (empty tag = what staging runs) |
| Refresh staging from last night's production copy | `kit refresh backoffice-staging` (cron does it at 04:10; `kit cron` installs that on an older server) |
| Deploy a specific build | `kit deploy backoffice <sha>` (the 12-char tag CI publishes) |
| Roll back | `kit deploy backoffice <previous tag>` — `kit ps` shows the one before |
| Change a secret | `kit env backoffice`, then `kit deploy backoffice` (same tag, restarts with the new env) |
| Add or change a hostname | edit `/srv/edge/sites/<host>.caddy`, then `kit reload` |
| Update the kit itself | `kit update` (git pull of this directory) |
| A shell in a container | `kit run backoffice sh` |
| Restore the backoffice from off-site | `kit offline backoffice node bin/backoffice.mjs restore --offsite` (stops it, restores, starts it) |

## When the server dies

Make a new one (steps 1–3 above, same hostnames). `kit app add` each app,
paste each `.env` back from your password manager, then for the backoffice
restore the latest off-site backup — `kit offline backoffice node bin/backoffice.mjs restore --offsite`,
which stops the app, pulls the copy back, puts it in place and starts the
app again; it needs `BACKUP_S3_*` and `BACKUP_ENCRYPTION_KEY` in the `.env`,
the same values, so keep them in the password manager too. The whole thing is
under half an hour, and the weekly restore drill is what makes that a fact
rather than a hope.

## What the kit deliberately does not do

- **No orchestration.** One box, Compose, restart policies. The day a project
  needs more than one box is the day to read about Kubernetes, not before;
  everything here (images from CI, one `.env` per app, `/healthz`) carries
  over unchanged.
- **No secrets manager.** Secrets are files two users can read (`ops` and
  `deploy`, the `kit` group) on a box only you can reach by key, plus the same values in a password manager. That is the
  right size for one operator; the moment there are several, a vault earns
  its keep.
- **No server-side backups of `/srv/data`.** Each app owns its backups: the
  backoffice encrypts, ships off-site and proves restore. A project without
  that has not earned real data yet.
- **No build on the server.** If it is not in a registry, it does not deploy.

## Security, plainly

SSH: keys only, root login off, a sudo user (`ops`) and a no-sudo deploy user
(`deploy`) whose key is bound to one forced command. The deploy user is in
the `docker` group, which is root-equivalent on the box — the forced command
is what fences it, so never give that key a shell. Both users share the `kit`
group, which owns `/srv/apps` and `/srv/data` (setgid, mode 2770): `ops` makes
an app, `deploy` reads its compose and `.env` and rewrites its `.tag`, and no
other account on the box reads either. An app's data directory is handed to
the uid its image runs as (the backoffice's `node`) by `kit deploy`, from
inside a container, so the SQLite file is the process's own from the first
write. Firewall: deny by default,
22/80/443 in. Patches: unattended security upgrades with a 04:30 reboot when a
kernel needs one. fail2ban watches sshd. Containers: no published ports,
`no-new-privileges`, all capabilities dropped, memory and PID limits, logs
rotated. Caddy: HSTS, `nosniff`, no `Server` header, TLS 1.2+ with
Let's Encrypt. Everything an app does beyond that (its own CSRF, sessions,
rate limits, cookies) is the app's; the backoffice's are in its SECURITY.md.
