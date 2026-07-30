# Ideas — how a suggestion becomes a shipped feature

Anyone can steer this project without writing a line of code. This is the whole
path, end to end: where an idea goes, how it's ranked, who reads it, and what
each step looks like from both sides — the person suggesting, and the
maintainer triaging.

Two audiences, so read the half you need:

- **You have an idea** → [The short version](#the-short-version) and
  [Why this lives on GitHub](#why-this-lives-on-github).
- **You're triaging** → [The maintainer loop](#the-maintainer-loop) and
  [The moving parts](#the-moving-parts-and-how-they-break).

---

## The short version

1. **Say it in a sentence** at [securacv.com/ideas](https://securacv.com/ideas),
   or straight from the [idea form](https://github.com/kmay89/securaCV/issues/new?template=idea.yml).
   One box is required. No jargon, no template to learn, no reproduction steps —
   "I wish it would text me when it sees motion" is a complete, useful idea.
2. **It opens as a GitHub issue** labeled `idea`, and appears on the
   [Community Ideas board](https://securacv.com/ideas) within a minute.
3. **A bot replies** confirming it landed and showing where the vote button is.
4. **People vote** with a 👍 reaction on the issue's first message. The board is
   sorted by that count.
5. **A maintainer triages it** and moves it along the label lane below. The board
   translates each label into plain English, so anyone can see status without
   knowing what a label is.

The lane, and what the board shows for each:

| Label | Board says | Means |
|---|---|---|
| *(none yet)* | **Gathering votes** | Open, unranked, waiting for backing |
| `planned` | **Planned** | Accepted — it's on the roadmap |
| `in-progress` | **Building now** | Someone is actively writing it |
| `shipped` | **Shipped** | Built and released |

Closed-but-not-shipped ideas drop off the board rather than sitting there as
tombstones — but the issue stays public and readable, with the reason written on
it. Declining an idea is done in the open or not at all. That last part is a
promise we make, not one the platform enforces; see
[the note on deletion](#why-this-lives-on-github) for where our word is all
you have.

---

## Why this lives on GitHub

The most common reaction to "go post it on GitHub" is *that's a website for
programmers, and I am not one.* Fair. Here's the honest case for why the
suggestion box for a privacy camera is a code-hosting site, and where that
choice genuinely costs you something.

**Because it puts your idea in the same room as the work.** This isn't a
suggestion inbox that feeds a private roadmap someone else keeps. When
somebody starts building your idea, the code links itself into your thread
automatically. You can watch it get written, watch it get reviewed, and see
the exact firmware release it ships in — from the page where you first asked
for it. Almost no product lets you do that.

**Because the everyday moves leave a trail.** Not because we *can't* touch your
words. Any project owner can moderate their own issue tracker — up to deleting
a thread outright — and claiming otherwise would be the kind of overclaim this
project fails a build over. What GitHub actually gives you is a record of the
ordinary moves: an edited comment carries a visible edit history anyone can
open, a closed idea shows who closed it and when, and the thread is public for
as long as it exists. If we say no, the reason sits on your page where you and
everyone else can see it.

**Deletion is the exception, and it's worth naming plainly.** An owner can
delete an issue outright. GitHub leaves no public tombstone when they do, no
one is notified, and on a new or quiet idea it's entirely possible nobody would
notice. There is no structural guarantee against it — only our word, and
whatever copies other people happen to have made. We would rather point at that
gap than let you find it yourself, because a page arguing you can trust us is
the worst place to be caught rounding up.

So the honest version is narrow: everything *short of deletion* is on the
record, and a private suggestion form doesn't even offer that much. It's a
weaker claim than "we can't touch it" — and the one that survives someone going
and checking, which is the only kind worth making here.

**Because the votes are real.** A vote is a reaction from a real account, which
means we can't inflate the numbers and neither can a competitor. "This was the
most-wanted idea" is a claim you can go and verify in about four seconds. On a
form we host ourselves, you'd just have to take our word for it.

**Because we'd rather not know who you are.** We ask for no email address, run
no accounts, and keep no database of the people who talk to us — because we
don't have one to keep. GitHub already handles identity, and that one free
account works for every other open-source project you'll ever want to talk to.
For a project whose entire pitch is *witnessing without watching*, running the
suggestion box on a mailing-list harvester would be a bit rich.

**Because it outlives us.** Everything here is public, archived, and exportable.
If this project ended tomorrow, the record of what people asked for and what was
decided doesn't go anywhere.

**What it actually costs you.** One free account, about thirty seconds to make,
and a page of unfamiliar grey chrome the first time. That's the real trade, and
we're not going to pretend it's nothing — it's why
[securacv.com/ideas](https://securacv.com/ideas) exists as a friendlier front
door, why the site walks you to the exact button, and why the intake bot's first
job is telling you where the vote is. If even that is too much, the
[feedback desk](https://securacv.com/feedback) takes plain email and a
maintainer copies it in for you.

---

## The maintainer loop

Triage is small and should stay small.

1. **Read it.** Every idea gets read by a human. The intake bot has already
   labeled and welcomed; you're not needed for that.
2. **Make the title votable.** The title is the headline everyone votes on, so
   fix it if it's vague — `Idea: text me on motion` beats `Idea: notifications`.
   Edit it, don't ask; the form was written to take one sentence from someone
   with no context on our vocabulary, and tidying it is the job.
3. **Merge duplicates toward the one with the most votes**, close the other with
   `duplicate`, and link them. Splitting a vote count across two issues is the
   main way this board loses signal.
4. **Move the label when reality moves** — `planned` when it's accepted,
   `in-progress` when someone actually starts, `shipped` when it's in a release.
   The board is only as honest as this step; a stale `in-progress` on something
   nobody has touched in four months is a lie told to the person who asked.
5. **Say no out loud.** If an idea conflicts with the invariants in
   [`spec/invariants.md`](../spec/invariants.md) — and some will, because "just
   let me see the footage" is a natural thing to want from a camera — close it
   with the reason and a link to why. That refusal is documentation; a silently
   stale issue is not.

An idea that gets accepted doesn't become a special kind of work item. It
becomes a normal issue and then a normal PR, subject to the same bar as
everything else in [`CONTRIBUTING.md`](../CONTRIBUTING.md).

---

## The moving parts, and how they break

Four pieces, in two repos. They are joined by one string — the label name
`idea` — and that joint is the whole failure mode.

| Piece | Where | Job |
|---|---|---|
| The form | [`.github/ISSUE_TEMPLATE/idea.yml`](../.github/ISSUE_TEMPLATE/idea.yml) | Asks for one sentence; applies `idea` |
| The labels | [`.github/labels.yml`](../.github/labels.yml) + [`labels.yml` workflow](../.github/workflows/labels.yml) | Guarantees those labels exist |
| The intake bot | [`.github/workflows/idea-intake.yml`](../.github/workflows/idea-intake.yml) | Re-applies `idea`; welcomes; explains voting |
| The board | `js/ideas.js` in [securacv_website](https://github.com/kmay89/securacv_website) | Lists `labels=idea` live from the public API |

**The failure that already happened, and why the labels are now code.** GitHub
**silently ignores** a label named in an issue template when that label doesn't
exist in the repository. The issue opens normally; it just opens unlabeled, with
no warning to the submitter, no error in any log, and nothing on the maintainer
side that looks wrong. The `idea` label had never been created — so every idea
anyone submitted would have been invisible to the board, which would have read
*"No ideas yet — be the first"* indefinitely while quietly holding a pile of
them. Both halves look healthy in isolation. That's what makes it nasty.

Hence: the labels are declared in a file, applied by a workflow, and visible in
a diff. If you ever rename `idea`, you are editing two repositories in the same
change — this one and `js/ideas.js` — or the board goes dark again.

The status labels have the same coupling, one step weaker: `js/ideas.js`
`statusOf()` maps label names to the plain-language badges, so a status label
that isn't in the set it recognizes silently reads as *"Gathering votes."*
Wrong, but not fatal — the idea still shows up.

**To bring the labels back by hand:** Actions → **Labels** → *Run workflow*. It
is additive and idempotent, so running it when nothing is wrong does nothing.

---

## Related

- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — for when an idea turns into a patch
- [`docs/FAQ.md`](FAQ.md) — the questions people actually ask
- [`docs/GLOSSARY.md`](GLOSSARY.md) — every proper noun in the project
- [`.github/CI.md`](../.github/CI.md) — the workflow rules the two new workflows follow
