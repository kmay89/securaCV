# 27 — Liability-minimization posture: how a one-person shop stays a hard target

> The reasoning behind [`/TERMS.md`](../../TERMS.md) and the risk-shaped edits to
> the industrial channel (doc 26). Goal in the founder's words: *"as unsueable as
> possible — zero liability."*
>
> **This is not legal advice, and no one can make you literally unsueable.**
> Anyone can file anything. What you *can* do is make a *valid, large, un-shifted*
> claim as unlikely as possible — shrink the surface, cap what's left, shift it by
> contract, insure the remainder, and refuse the jobs that carry catastrophic
> tails. **Have a licensed attorney in your state finish anything binding**,
> especially with industrial/safety exposure. Prices and the brand's word stay
> human (doc 20); so does signing off on legal posture.

## 1. The frame: you can't be unsueable, you can be a hard target

Liability exposure = **likelihood of a valid claim × size of exposure × the share
that lands on you**. You attack all three multiplicatively:

| Lever | What it does |
|---|---|
| Entity shield | moves the *who* off your personal assets |
| Liability cap | shrinks the *size* to fees paid |
| Refuse the dangerous | deletes the catastrophic-tail *likelihood* |
| Don't hold data | deletes the breach *surface* |
| Marketing discipline | stops you *creating* warranties to be sued on |
| Insurance | pays even for meritless claims (defense cost is the real tax) |

No single one is "zero." Stacked, they turn a solo shop into a target not worth
suing — which is the practical meaning of "zero liability."

## 2. The two highest-leverage moves

**(a) Refuse life-safety work — reposition control → awareness.** As a solo
operator with no PE stamp and thin insurance, the one exposure that can end you is
a **life-safety failure** ("the detector was supposed to stop the machine"). The
fix isn't a better disclaimer — it's **not selling that function at all.** Every
detector is repositioned from *control* to **awareness / operations / evidence**:

- ❌ "person in cell → e-stops the machine"
- ✅ "person in cell → **an alert + a logged, tamper-evident event that feeds
  *your own* certified safety system**"

You provide a signal; the customer's *existing, certified* safety systems remain
primary and independent. If you never sell the thing that's meant to prevent the
crush, you never own that failure. This barely costs value — **insurers and
auditors pay for the evidence, not the e-stop.** It is the single biggest
reducer, and doc 26's catalog and the `/industry` page are rebuilt around it.

**(b) Cap liability at fees paid + exclude consequential damages.** The one clause
that matters most in `/TERMS.md`. It converts "unlimited catastrophic exposure"
into "worst case, they get their money back." Everything downstream (a missed
detection, a bad tune, downtime) is bounded to the engagement fee, and lost
profit / injury-derived economic loss is excluded outright. A cap a court might
narrow is still far better than no cap.

## 3. The entity shield (foundation)

Run **everything** — contracts, invoices, the store, services — through **Errer
Labs, LLC**, and keep the veil intact: separate bank account, no commingling of
personal and business money, adequate capitalization, contracts signed in the
entity's name, no personal guarantees. Caveats worth knowing: an LLC does **not**
shield your own personally-committed negligent acts, and courts can "pierce" a
veil that's a sham. So the LLC is necessary, not sufficient — it's the floor the
other layers stand on.

## 4. The contract stack (`/TERMS.md`)

Every paid engagement rides on written terms a lawyer finalizes. The load-bearing
clauses, in order of importance: **not-a-safety-device** (§3), **limitation of
liability / the cap** (§5), **AS-IS / no warranties** (§2), **indemnification
running to us** (§6), **customer owns install + acceptance + compliance** (§4),
and **arbitration + short claim window** (§10). A signed order/SOW per job
incorporates these by reference. The template is deliberately plain-language so
you actually understand what you're agreeing to — but it is a *template*, flagged
throughout for counsel to finish.

## 5. Marketing discipline (don't manufacture warranties)

Marketing copy can *create* liability — an express warranty or a misrepresentation
you get sued on. Banned words and their safe swaps:

| ❌ Never | ✅ Instead |
|---|---|
| "guarantees safety", "prevents accidents" | "advisory awareness signal" |
| "certified", "compliant" (unless truly certified) | "designed to support your compliance" |
| "tamper-proof", "unhackable" | "tamper-evident" |
| "100% accurate", "never misses" | "best-effort detection; false positives/negatives happen" |
| "safety system/control" | "feeds your own certified safety systems" |

This discipline already governs the device copy (doc 08 §7, the website fact-tests
that forbid "tamper-proof"); doc 27 extends it explicitly to the *services* pitch.

## 6. Technical posture that lowers the duty

Some risk is deleted by how the thing is built, not just what the contract says:

- **Advisory-only by construction** — coarse claim + score, no control authority
  shipped by us; the customer wires any action into their own systems.
- **No data retention** — we train on samples they send and hand back a model that
  runs on their hardware; footage never flows to us. **You can't breach or leak
  what you never hold** — this near-zeroes the privacy/data-breach tail, for free.
- **The free self-serve path = no relationship = no duty.** Every DIY user is pure
  Apache-2.0 (AS-IS, no warranty, no liability). Keeping self-serve prominent means
  most of the world never becomes a customer you owe a duty to.

## 7. Insurance (the backstop that makes it real)

Even a meritless suit costs money to defend — that defense cost is the real,
everyday liability tax, and insurance is what pays it. Carry, sized to revenue:
**general liability**, **product liability** (for any hardware you sell),
**professional / E&O** (for the advice and the detectors), and consider **tech
E&O / cyber**. This is already a line in the doc-23 overhead model; treat it as
non-optional the day the first services invoice goes out.

## 8. The checklist (what to actually do)

- [ ] Form / confirm **Errer Labs, LLC**; open a dedicated bank account; never
      commingle. Sign everything as the entity.
- [ ] Have an attorney finish `/TERMS.md` for your state; attach it to every SOW.
- [ ] Bind **E&O + general + product liability** before the first paid services
      engagement.
- [ ] Reposition the **Recipe catalog to awareness/ops/evidence** — no life-safety
      control functions sold (done in doc 26 + `/industry`).
- [ ] Put the **"advisory, not a safety device"** notice *prominently* on
      `/industry` and in every proposal — not buried (done).
- [ ] Keep the **marketing word-list** (§5) — extend the site fact-tests to flag
      the banned services claims if this becomes a recurring page.
- [ ] Formalize **no-footage-retention** in writing (in `/TERMS.md` §7).

## 9. What this deliberately does *not* claim

It does not claim to make you unsueable, to be legally sufficient, or to substitute
for a lawyer. It is a **posture** — the structural choices that make a solo shop a
poor target and a bounded one. The residual, irreducible step is human: **a real
attorney, real insurance, and the discipline to say no to the dangerous job.**
