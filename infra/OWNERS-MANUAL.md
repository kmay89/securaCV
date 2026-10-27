# The owner's manual — the server, in plain language

This is for the person who owns the company, not for the person who wrote the
kit. It says what the server is, why each decision was made, what can break,
what that costs you, and what to do. Nothing here needs to be remembered: come
back and read it when something feels wrong.

The technical runbook is [`README.md`](README.md) beside this file. This page
is the calm version.

## What you are running

One small rented computer. It runs three copies of one program: the
**backoffice** (orders, customers, stock, shipping, support mail, the books —
the real one), a **staging** copy (the same program on last night's data,
where every change lands first, with test keys so it can charge and mail
nobody), and a **public demo** with made-up data. Every page of each says
which it is, in the title and beside the company name, so a tab is never
mistaken for another. The website itself is not on this computer; it is static
pages served by Netlify, and the store page keeps working even when this
computer is off (it falls back to Stripe's own payment links).

Money never touches this computer. Stripe's page takes the card; your server
is told afterwards that an order was paid. That is why there is no card data
to protect here.

## The decisions, and why each one is safe to make today

| Decision | What was chosen | Why | How hard to change later |
|---|---|---|---|
| Where the computer is rented | DigitalOcean, a "Basic" droplet, 2 GB memory, Ubuntu 24.04, New York, backups on | One product, a flat price (about $14 a month with backups), a dashboard a non-technical person can read, US data centers. Amazon is built for teams with an operations department; its bill and its console are the headache you are trying to avoid | Thirty minutes. The server is rebuilt from a text file; nothing about the kit is DigitalOcean-specific |
| How the computer is set up | A text file (`cloud-init.yaml`) pasted once when the server is created | A server built by hand is a server nobody can rebuild. This one is built by a file in the repository, so a replacement is identical | Not a decision you revisit |
| Who can log in | Only your laptop's key. No passwords, no root login, a watchdog that bans anyone guessing | Passwords get guessed; keys do not | Add a second laptop's key any time |
| Updates | Security patches install themselves; the computer restarts at 04:30 if a patch needs it | A server nobody patches is the most common way small businesses get broken into | Nothing to do |
| Where the data lives | On the computer's disk, in one folder per program | Simple to back up, simple to move | It is the only thing on the box that matters; see backups |
| Backups | Two layers. DigitalOcean snapshots the whole computer weekly (the switch you turned on). The backoffice itself encrypts its data every night and copies it to Backblaze (a separate company, free at this size), and once a week it restores that copy to prove it works | A backup that was never restored is a hope, not a backup. The weekly drill turns it into a fact you can see in the backoffice's health page | Either layer can be swapped; keep two |
| Certificates (the padlock) | Automatic, from Let's Encrypt | Free, renews itself every two months, nothing to remember | Nothing to do |
| Deploying new versions | A merge on GitHub ships itself to **staging**. Production changes only when you press one button on GitHub ("Promote backoffice to production"), which sends the build staging is running | Nothing reaches the real backoffice without a person deciding it, and what reaches it has already run on yesterday's real data for as long as you cared to look; the server only accepts one exact command from GitHub and nothing else | Can be turned off by removing three secrets on GitHub |
| Going back | The same button with the previous build's name (`kit ps` shows it), or one command on the server. A new version takes a copy of the data before it changes anything, so a bad one is undone in two steps: the previous build, then that copy | A change you cannot undo is a change you cannot make calmly | Nothing to do |

## What can break, what it costs, what you do

Ordered from most to least likely. "Customers" means what a buyer on the
website sees.

| What | How likely | Customers see | You do | Time |
|---|---|---|---|---|
| The domain name lapses | The most common small-business outage anywhere | The whole site is gone | Keep auto-renew on at your registrar and the card there current. Check once a year | 5 minutes a year |
| The card on file at DigitalOcean expires | Likely, once every few years | Nothing for a while; DigitalOcean emails you, then stops the server after a grace period | Update the card. Put a calendar reminder two months before expiry | 5 minutes |
| A security restart at 04:30 | Monthly, normal | Nothing. The programs restart on their own in under a minute | Nothing | 0 |
| The backoffice crashes or will not start after an update | Occasional | Nothing. The store page notices the backoffice is unreachable and sells by Stripe's payment links instead; order mail is delayed, not lost | Log in and run `kit doctor`; it names the problem and the fix. The deploy also rolls itself back to the previous version if the new one is not healthy within two minutes | 10 minutes |
| Stripe is down | Rare, hours a year | Checkout fails with a message that nothing was charged | Nothing; it is Stripe's to fix | 0 |
| Mail stops sending | Occasional (a changed mailbox password, a provider limit) | Order confirmations stop | The backoffice's health page shows the outbox stuck; `kit env backoffice` to fix the mail password, then `kit deploy backoffice` | 10 minutes |
| The disk fills up | Rare; logs rotate and the data is small | Nothing until it is full, then errors | `kit doctor` warns at 85 percent. Resize the droplet one size up in the dashboard | 10 minutes |
| The whole computer is lost (provider fault, you deleted it, it is compromised) | Rare | The store keeps selling by payment links; the backoffice is gone until rebuilt | Follow "When the server dies" below | 30 minutes |
| You lose the password manager entry for the server | The only truly bad case | Nothing at first | Backups are encrypted with a key that is in that entry. Without it, the off-site backups cannot be opened; the DigitalOcean snapshot (unencrypted, on their side) is the fallback. Do not let this happen: the entry is the business | — |

## The two things to never lose

1. **The password manager entry named "SecuraCV server".** It holds every
   value the server was told (the `.env` file's contents: Stripe keys, mail
   password, backup bucket and its encryption key, the authenticator secret).
   A replacement server is those values pasted back in. Keep it in a
   password manager with its own backup (1Password, Bitwarden), not in a
   notes app.
2. **Your authenticator app.** The backoffice login needs its six-digit
   code. Most authenticator apps back themselves up; make sure yours does, or
   keep the setup secret in the entry above (it is printed once at setup).

Everything else can be regenerated.

## When the server dies

This is the drill, and it is the same drill whether the computer broke, was
deleted, or you simply want to move to another provider.

1. Create a new droplet exactly as the first time: Ubuntu 24.04, 2 GB,
   backups on, and paste the user-data file (render it again from the
   repository if you no longer have it; it needs only your SSH key and an
   email).
2. Point `ops.securacv.com` and `demo.securacv.com` at the new address.
3. Log in, run `kit app add backoffice`, then `kit env backoffice` and paste
   the values from the password manager entry.
4. `kit deploy backoffice latest`, then restore last night's copy:
   `kit offline backoffice node bin/backoffice.mjs restore --offsite`
   (it stops the app, pulls the copy back, checks it, puts it in place, and
   starts the app again).
5. Run `kit doctor`. Green means done.

Under half an hour. You lose at most the orders since last night's backup,
and Stripe still has every one of those: the backoffice's daily check against
Stripe lists each paid charge that has no order here, with its Stripe id, and
Stripe's dashboard can resend those events so they are written down again.

## Your routine

There is none. The server does not need to be looked at. When a change you
asked for has merged, open staging (it is there within minutes, on
yesterday's real data), click around what changed, and when you are
satisfied press **Promote backoffice to production** on GitHub — that is
the whole release process. If you want a habit, once a month log in and run
`kit doctor`; it prints a line per thing it checked, green or a one-line fix. The backoffice's own health page shows
the last backup, the last restore drill, and the mail outbox without logging
in to the server at all.

## What the numbers mean

- DigitalOcean: about $12 a month for the computer, about $2.40 for backups.
- Backblaze B2: free under 10 GB, which this is for years.
- Let's Encrypt, Caddy, Docker, Ubuntu: free.
- Stripe: their percentage per sale, nothing monthly.
- A mailbox for hello@: about $6 a month at Google Workspace or Fastmail.

Nothing else recurs. A surprise bill from this setup is not possible; the
only variable charge is Stripe's, and it only happens when you were paid.

## Words you will meet

- **Droplet**: DigitalOcean's word for a rented computer.
- **cloud-init / user-data**: the text file pasted at creation that sets the
  computer up. It is in this folder; a copy with your key filled in is what
  you paste.
- **kit**: the small tool on the server (`kit doctor`, `kit deploy`, …).
  Every command prints what it did; none of them can lose data.
- **.env**: the file holding the server's secrets. `kit env backoffice`
  opens it. Its contents are the password manager entry.
- **Deploy**: start a newer version of a program. On this server it happens
  by itself after a merge on GitHub, and it rolls back by itself if the new
  version is not healthy.
- **Snapshot**: DigitalOcean's weekly copy of the whole computer. Restoring
  one is a button in their dashboard.
- **Off-site backup**: the backoffice's own nightly encrypted copy at
  Backblaze. The one that is proven weekly.

## When to ask for help

Any time `kit doctor` prints something red that its one-line fix does not
clear, or anything in this manual did not happen the way it says. Paste the
output of `kit doctor` and `kit logs backoffice`; neither contains a secret.
