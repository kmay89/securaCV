# Why securaCV exports work this way

*A plain-language explainer for owners, and for anyone you hand evidence to.
No cryptography background needed.*

## Why isn't this just "download the clip", like Ring?

A consumer camera gives you a video file. A video file is just bytes — anyone
with a laptop can cut frames out of it, change its timestamps, or synthesize it
entirely, and nothing about the file itself can prove otherwise. The moment it
matters (an insurance dispute, a court, a newsroom), its credibility rests on
trusting whoever handed it over.

securaCV inverts this. The device records **less** — semantic events ("large
object crossed the boundary in zone A, around 14:10, confidence 0.93") instead
of raw video — so that it can **prove** what it does record:

- Every event is **signed** by a key that exists only inside your device.
  Anyone can check the signature; only your device could have produced it.
- Events are **chained**: each one contains a fingerprint (hash) of the one
  before it. Editing, deleting, or reordering any event breaks every link
  after it, and the break is detectable by anyone, offline.
- Every export leaves a **signed receipt** in that same chain. You cannot
  quietly extract data — the act of disclosure is itself part of the
  tamper-evident record, labeled with how it was authorized.

So when you hand someone a securaCV evidence bundle, you're not asking them to
trust you. They drop the file into the offline viewer (or run `envelope_verify`)
and the math checks itself — no account, no network, no vendor involved.

## What a green check actually proves

A passing verification proves:

- These events were recorded **by this specific device** (the key fingerprint
  in the bundle), **in this order**.
- **Nothing was added, removed, or altered** since each event was sealed.
- Every disclosure of this data left a signed, chained receipt.

It deliberately does **not** prove:

- That the detector interpreted the scene correctly — only that this is what
  the device recorded at the time.
- That nothing happened outside the device's coverage, or while it was off.
  (When the device *knows* it couldn't observe — power loss, storage full,
  clock jumps — it seals an explicit failure record, so gaps are declared
  rather than hidden.)
- Exact times (next section).
- Who physically operated or had access to the device.

Honest limits are the point: a system that claims to prove everything proves
nothing.

## Why are the times "fuzzy"?

Exported events carry a 10-minute **time bucket**, and the bucket label is
additionally **jittered** by up to ±2 minutes at export. This is deliberate
(Invariant III, `spec/event_contract.md`):

- Coarse times are enough for "was there activity that evening?" — the
  questions an owner or adjudicator actually asks.
- Precise timestamps are a surveillance tool: they let event streams be
  correlated with other data (phone records, neighbors' cameras, license-plate
  readers) to track specific people. Your camera shouldn't be usable as a
  tracking sensor against bystanders — or against you.

The jitter is applied to the exported label only; window selection and
integrity checks use the true buckets internally, so fuzzing never breaks
verification.

## Why do some exports need trustee approval — and some not?

There are two tiers, matched to two kinds of risk:

- **Event exports** (the coarse, privacy-filtered artifact described above)
  can be made by the owner alone with `export_events --self-export`. The
  credential is your **device key seed** — without it the database can't even
  be decrypted, let alone receipted. The export still writes a signed receipt
  labeled `self_export`, so it's always visible afterwards that an
  owner-authorized disclosure happened. This is the everyday path, and it's as
  easy as any other camera's export.
- **Sealed vault evidence** (the richest data the system holds) can only be
  unsealed with approval from a quorum of **trustees** you chose in advance
  (break-glass). This protects you from coercion — "a key that can be
  compelled, will be" — and protects everyone the camera saw from a single
  person quietly bulk-extracting evidence. Each unseal attempt, granted or
  denied, is receipted.

You can also run an event export through break-glass (`--break-glass-token`)
when you want the disclosure itself countersigned by your trustees.

## Your device key seed — the one thing to manage

The seed (the `DEVICE_KEY_SEED` value, e.g. `devkey:...`) is the root of
everything: it derives both the database encryption key and the signing key.
The event database is **encrypted at rest by default** (SQLCipher) — a stolen
device or SD card yields ciphertext, not your event history.

- **Generate** it once, randomly (e.g. `openssl rand -hex 32`), when you set
  the device up.
- **Store** it offline — a password manager or a piece of paper in a safe.
  Never put it in shell history, logs, or cloud notes. Anyone holding the seed
  can read your database and sign new entries as your device.
- **If you lose it**: the database becomes unreadable and the device can sign
  nothing new — there is deliberately no recovery, because a recoverable key
  is a compellable key. Already-exported bundles remain verifiable forever;
  they embed the public key.
- **Rotation** is supported and itself recorded as a verifiable chain entry —
  see `docs/db_key_rotation.md`.

## When verification fails

A red result is not automatically proof of foul play — a truncated download or
an export interrupted halfway fails the same checks. The tools now tell you
*where* the chain broke, *what kind* of check failed, and what to do next
(`log_verify` for a device database, `envelope_verify` or the offline viewer
for a bundle). The one rule that always applies: **never edit a database or
bundle to "fix" it** — preserve the failing copy, re-export a fresh one, and
compare.

## Further reading

- `docs/security/SECURITY_MODEL.md` — the full security model, written for
  non-engineers.
- `spec/event_contract.md` — exactly what an event may and may not contain.
- `spec/evidence_envelope.md` — the bundle format and verification rules.
- `spec/threat_model.md` — who this defends against, and who it can't.
