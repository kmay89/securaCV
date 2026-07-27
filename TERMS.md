# Services terms — template

> ⚠️ **This is a plain-language template, not legal advice, and not a
> finished contract.** It is a starting scaffold for the terms that govern paid
> SecuraCV / Errer Labs services (industrial detectors, integration help,
> support, attestation). **Have a licensed attorney in your jurisdiction review
> and finish it before you rely on it or put it in front of a customer.** Nothing
> here makes anyone "unsueable" — its job is to shrink and cap risk, in writing,
> up front. Bracketed `[…]` items are decisions for you and your lawyer.
>
> The *software* is not covered here — it's Apache-2.0 ([`LICENSE`](LICENSE)),
> which already disclaims warranty and liability. The *names/logos* are in
> [`TRADEMARK.md`](TRADEMARK.md); who-pays-what is in [`LICENSING.md`](LICENSING.md).
> This file is only about **paid services**, and the reasoning behind it is in
> [`docs/strategy/27-liability-minimization-posture.md`](docs/strategy/27-liability-minimization-posture.md).

## 0. The one thing to internalize

We sell an **advisory awareness, operations, and evidence signal** — a best-effort
detector and a tamper-evident record. **We do not sell a safety device, a safety
control, or a guarantee.** Every clause below serves that sentence.

## 1. Who and what

These terms are between the customer ("you") and **Errer Labs, LLC** ("we," "us").
They cover paid services: detector builds, tuning, integration guidance, support,
and attestation. Work is performed **remotely**. Anything physical on your site —
mounting, wiring, electrical work, commissioning — is done by **you or your
qualified contractor**, from our written runbook. We are not present on your site
and do not supervise physical installation.

## 2. No warranties — provided "AS IS"

The services and anything we deliver (models, configurations, runbooks, reports)
are provided **"AS IS" and "AS AVAILABLE," with no warranties of any kind**,
express or implied. We specifically disclaim any implied warranties of
**merchantability, fitness for a particular purpose, accuracy, reliability,
non-infringement, and uninterrupted or error-free operation**. Detection is
**statistical and best-effort**: it will sometimes miss events (false negatives)
and sometimes fire wrongly (false positives). We do not warrant any detection rate.

## 3. Not a safety device — the load-bearing clause

A Canary and any detector we build is a **witness and an advisory signal, not a
safety device and not a safety control.**

- It is **not** a substitute for, and must **never** be relied on as, a machine
  guard, interlock, light curtain, emergency stop, life-safety system, fire/egress
  system, or any certified or regulated safety control.
- You must **independently maintain all certified safety systems** required for
  your site and processes, and those systems must remain **fully functional and
  primary** whether or not our signal is present, correct, or available.
- If you choose to route our advisory signal into any of your own systems, **that
  wiring, its safety rating, and its consequences are yours** — you are
  responsible for ensuring our signal is never a single point of safety failure.
- Nothing we provide is professional engineering, safety-certification, legal, or
  regulatory-compliance advice. We are **not** a licensed professional engineer.

## 4. What is yours to do

You are responsible for: choosing where and how the device is mounted and powered
(by a qualified person); validating detection accuracy **in your own environment**
before relying on it; **final acceptance testing** and sign-off; compliance with
all laws, regulations, standards, and workplace/worker-privacy rules that apply to
you (including notifying and consulting workers/representatives where required);
and the security of your own network and systems.

## 5. Limitation of liability — the cap

To the maximum extent permitted by law:

- **We are not liable for any indirect, incidental, special, consequential,
  exemplary, or punitive damages**, or for lost profits, lost production,
  downtime, loss of data, or business interruption — even if advised of the
  possibility.
- **We are not liable for personal injury, death, or property damage** arising
  from your deployment, use of, reliance on, or inability to rely on the services
  or any detector, except to the extent a court finds it was caused directly by
  our own gross negligence or willful misconduct [confirm carve-out with counsel;
  some jurisdictions won't enforce a full injury waiver].
- **Our total aggregate liability for any and all claims is capped at the fees you
  actually paid us for the specific engagement giving rise to the claim** (or `[$X]`,
  whichever is `[lower]`).

## 6. Indemnification

You will defend, indemnify, and hold us harmless from third-party claims, losses,
and costs (including reasonable legal fees) arising from your deployment or use of
the services, your physical installation, your reliance on an advisory signal, or
your failure to maintain your own safety and compliance systems.

## 7. Your data stays yours

We train on samples **you** provide and deliver a model that runs on **your**
hardware. **We do not retain your footage** after delivery, and inference runs
on-device — no footage flows to us or to any cloud on the data path. You own your
data and your deployment; you are the controller of any personal data it may
touch, and you are responsible for your lawful basis to operate it.

## 8. Intellectual property

You receive a non-exclusive license to use the specific model, configuration, and
runbook we deliver, for your own sites. Our underlying **Recipe framework, methods,
and library remain ours**, and we may reuse and resell the general approach and any
non-customer-specific detector to others. The SecuraCV software stays Apache-2.0.

## 9. Founding-customer / pilot work

Early engagements are explicitly **pilots / co-development**: exploratory, priced
as such, with **no guarantee of a working result**. If a pilot can't meet your
need, the remedy is `[stop / partial refund per §5 cap]`, not damages.

## 10. Term, changes, and disputes

- Either party may end an ongoing engagement `[with N days' notice]`; a support or
  retainer lapse never disables anything already running on your hardware.
- These terms are governed by the laws of `[your state]`, without regard to
  conflict-of-laws rules.
- **Disputes are resolved by binding arbitration in `[your county/state]`,** on an
  individual basis; **class actions and jury trials are waived** [enforceability
  varies — confirm with counsel].
- Any claim must be brought within **`[one year]`** of the event, or it is waived
  [subject to what your jurisdiction allows].
- If any clause is unenforceable, the rest stands, and the unenforceable clause is
  narrowed to the minimum change needed.

## 11. Housekeeping

This document is a template and a statement of intent, not a substitute for a
contract drafted or reviewed by your attorney, and not legal advice. Where a
signed order/SOW and these terms conflict, `[the signed order controls / these
control]`. Questions: open a GitHub issue, or email `errerlabs@gmail.com`.
