# Trusted timestamping (RFC 3161 anchors)

The witness chain proves *internal* consistency: every sealed event is
signed by the device key and hash-linked to its predecessor, so nothing can
be altered or removed without breaking verification. What the chain cannot
prove by itself is **when** it existed. A verifier has to take the device's
clock — and the continued secrecy of the device key — on trust: an attacker
who obtained the key could fabricate an entire plausible history, back-dated
at will.

Anchoring closes that gap. `log_anchor` sends the 32-byte chain head hash
to a public **Time Stamping Authority** (TSA), which countersigns it with
its own key and clock (RFC 3161). The returned token is third-party proof
that *this exact chain state existed at this time*. Everything recorded
before an anchor is thereby fixed in time by a party that is not the
device, not the operator, and not SecuraCV:

- **Back-dating becomes impossible**, even with the device key: forged
  "old" history cannot carry an anchor from the past.
- **The device clock drops out of the trust base** for everything at or
  before each anchor.
- A sealed export can be shown to a court **to predate a dispute** — the
  strongest answer available to "this footage log was fabricated later".

Anchor regularly (a daily cron is plenty) and an adversary's window to
rewrite history shrinks to the gap since the last anchor.

## What leaves the device (privacy)

A timestamp request is a DER `TimeStampReq` containing the SHA-256 digest
and a random nonce — **nothing else**. No events, no zone names, no device
identifiers, no quantities. The hash is one-way: the TSA learns nothing
about what it is timestamping.

Two things the TSA *does* learn, stated honestly:

- **Your IP address**, like any server you contact. Route through a proxy,
  VPN, or Tor if the association between your address and "operates a
  witness device" matters to you.
- **That something was anchored at this moment.** Anchor on a fixed
  schedule (cron), not in reaction to incidents, and the timing carries no
  signal — and `anchor-all` keeps the request *count* per run constant
  (`|subjects| × |TSAs|`) for the same reason.

SecuraCV's no-outbound-network principle is preserved: nothing in
`witnessd` calls a TSA. Anchoring happens only when an operator (or an
operator's cron job) runs `log_anchor`, and an offline flow exists for
air-gapped deployments.

## Anchoring a ledger head

An anchor row names what it covers — its **subject**. Four ledgers have a
head that can be anchored, selected with `--subject`:

| `--subject` | What is anchored |
|---|---|
| `chain_head` (default) | the newest sealed-event `entry_hash` |
| `export_receipt_head` | the newest export-receipt `entry_hash` |
| `break_glass_receipt_head` | the newest break-glass-receipt `entry_hash` |
| `policy_head` | the newest policy-change-history `entry_hash` |

Anything else is a bare `digest` (`--digest <hex>` or `--file <path>`, which
hashes the file's bytes in-tool); `--subject`, `--digest` and `--file` are
mutually exclusive. The subject is a *label the row claims*; every verifier
re-derives what it can from the token and the ledgers, and a court kit
packages by hash, never by label (see [court_export.md](court_export.md)).

Online (build with `--features tsa`):

```sh
log_anchor request --db witness.db --url https://freetsa.org/tsr
log_anchor request --db witness.db --url https://freetsa.org/tsr --subject policy_head
# two TSAs in one run: each URL gets its own nonce and its own row
log_anchor request --db witness.db --url https://freetsa.org/tsr --url http://zeitstempel.dfn.de --allow-http
```

`--url` repeats. A partial success stays stored — the rows from the TSAs
that answered are kept — and the command exits non-zero with a line that
names the count (`1 of 2 TSA request(s) failed`).

Offline / air-gapped — no special build, no network access from the
device:

```sh
log_anchor query --db witness.db --out chain.tsq
# move chain.tsq to any machine with internet access:
curl -s -H 'Content-Type: application/timestamp-query' \
     --data-binary @chain.tsq https://freetsa.org/tsr > chain.tsr
# move chain.tsr back:
log_anchor import --db witness.db --response chain.tsr --url https://freetsa.org/tsr
```

Import correlates the response by its message imprint against recorded
ledger history, so it can be imported after the chain has moved on —
**within the retention window** (`[retention]`, default 7 days): retention
preserves only heads that already have an anchor row, so a response that
arrives after its head was pruned can only be stored as a `digest` anchor,
and `import` says so. `import` labels the row by the ledger the imprint is
found in: a sealed-event hash or checkpoint head is stored as `chain_head`,
a receipt-ledger or policy-history hash as that ledger's head subject, and a
miss as `digest` with the stderr note `imprint … is not in this DB's chain
or receipt-ledger history`. `--response` repeats, so a batch of `.tsr`
files imports in one command.

`policy_head` is empty — `anchor-all` anchors its sentinel — on a database
whose quorum policy was only ever written through the ungated
`Kernel::set_break_glass_policy` (fixtures, `drill` sandboxes); every
user-facing path (`policy set`, guided setup, the served console) is
quorum-gated and writes the history row that becomes the head.

Public TSAs (no account needed): `https://freetsa.org/tsr`,
`http://timestamp.digicert.com` (`--allow-http`; the token's own signature
makes transport integrity non-critical), `http://zeitstempel.dfn.de`.
Anchoring the same head at **two independent TSAs** removes the single
point of trust — the anchor policy below makes that a checked property.

## Anchoring an export

`court_export` recognizes exactly one digest: the SHA-256 of the export
bundle file bytes (what `sha256sum` prints). Anchor it with:

```sh
log_anchor request --db witness.db --url https://freetsa.org/tsr --file witness_export.json
```

The anchor is stored with subject `digest`, tying the handed-over artifact
itself — not just the chain it came from — to a point in time.
`envelope_verify`'s whole-envelope digest is a property of the
evidence-envelope format; it may be anchored as a `digest`, but it is not
what a court kit looks for.

## Two TSAs and the anchor policy

An **anchor policy** is a format-tagged JSON file the operator writes once.
It names the TSAs, the role each is *declared* to play, the ledger heads to
anchor, and what "covered" means:

```json
{
  "format": "securacv-anchor-policy:v1",
  "tsas": [
    { "name": "dfn",     "role": "qualified",   "url": "https://zeitstempel.dfn.de", "ca": "ca/dfn-tsa-ca.pem",
      "declaration": "Operator's statement of why this TSA is treated as eIDAS-qualified, who checked the trusted list, and when. Recorded, never evaluated." },
    { "name": "freetsa", "role": "independent", "url": "https://freetsa.org/tsr",    "ca": "ca/freetsa-ca.pem",
      "allow_http": false,
      "cert_sha256": ["<64-hex of the TSA signing certificate, optional>"] }
  ],
  "subjects": ["chain_head", "export_receipt_head", "break_glass_receipt_head", "policy_head"],
  "require_roles": ["qualified", "independent"],
  "min_distinct_tsas": 2
}
```

`ca` paths resolve relative to the policy file. Unknown fields, a missing
format tag, a `digest` subject, a role no TSA declares, or a `min_distinct_tsas`
larger than the roster are refused at load with a message that names the
field. `qualified` records the operator's statement that the TSA is
eIDAS-qualified (ETSI EN 319 421/422); the tools never verify that status.
`independent` is likewise a declaration. The `declaration` text is recorded
alongside, never evaluated.

**Scheduled anchoring** (online, `--features tsa`) — the cron entry:

```sh
log_anchor --db witness.db anchor-all --policy anchor-policy.json
```

`anchor-all` sends the same number of requests every run — every configured
subject to every configured TSA, an empty ledger anchored over a fixed
per-subject sentinel — so the request count never tracks whether an export,
an unseal, or a policy change happened. What a TSA can still see is the
imprint: an unchanged imprint repeating across runs for a receipt ledger (or
for the chain head if heartbeats are disabled) and the run in which it
changes. Only anchoring a per-run ledger-heads leaf (the Merkle-tree work in
`spec/quorum_unseal_v2.md` §4) removes that residual; adding a subject to the
policy changes the request pattern once. Two consequences are by design: a
second token over an unchanged head is a harmless duplicate row (coverage
dedupes by hash), and an empty ledger produces a `digest` row over its
sentinel (`SHA256("securacv:anchor:empty-ledger:v1:" ‖ subject)`), which
`list` names as such and which never counts toward coverage. The summary line
is `anchor-all: {stored} stored, {failed} failed ({subjects}×{tsas}
requested; {sentinels} over empty-ledger sentinels)`.

**Offline** — the same rule, one query file per configured subject, from a
read-only open of the database:

```sh
log_anchor --db witness.db anchor-all --policy anchor-policy.json --offline-dir out/
# prints one curl line per (subject, TSA) pair, e.g.
#   curl -s -H 'Content-Type: application/timestamp-query' --data-binary @out/chain_head.tsq https://zeitstempel.dfn.de > out/chain_head.dfn.tsr
# submit every .tsq to every TSA, bring the .tsr files back, then:
log_anchor --db witness.db import --policy anchor-policy.json --response out/*.tsr
log_anchor --db witness.db verify --policy anchor-policy.json
```

`import --policy` infers the TSA from a `{subject}.{name}.tsr` file name (or
takes `--tsa NAME`) and, when `openssl` is on `PATH`, refuses to store a
response that does not verify under that entry's CA. Bring the responses back
within the retention window: a head pruned before import can only be stored
as `digest`.

**Checking coverage:**

```sh
log_anchor --db witness.db verify --policy anchor-policy.json [--require-current]
```

`verify --policy` requires the `openssl` CLI (it refuses to run without it
rather than degrade). Every row is checked as in "Verifying anchors" below,
then attributed to a policy entry: the entry whose CA validated the
countersignature. Pins attribute only after `openssl ts -verify` succeeded
under that entry's CA, and only to break a tie between overlapping CA bundles
— two vendors whose leaves chain to the same public root. With no pin to
break such a tie the row is `FAIL` (`verifies under the CA of both dfn and
freetsa — their CA bundles overlap and no cert_sha256 pin singles one out; add
cert_sha256 pins to disambiguate, or use leaf-issuer CA files`); a declared
`tsa_name` that contradicts the attribution, or a token certificate outside
the entry's pins, is also `FAIL`. Per policy subject the tool then reports the
newest head whose `OK` rows cover every required role via distinct entries:

```
policy: chain_head: covered by dfn (qualified, declared), freetsa (independent, declared) — anchors #12, #13, bucket 2026-09-07 06:00 UTC; current head: yes
policy: export_receipt_head: NOT covered — anchored by dfn (qualified, declared) only; missing role(s): independent
  anchor now: log_anchor --db witness.db anchor-all --policy anchor-policy.json
policy: break_glass_receipt_head: ledger is empty; nothing to cover
note: 'qualified' and 'independent' are the operator's declarations in anchor-policy.json; this tool checks countersignatures, count and distinctness, not legal status.
anchor policy anchor-policy.json: NOT SATISFIED (1 subject(s) uncovered)
```

`current head: NO (ledger advanced since)` is normal in steady state — the
chain head moves with every heartbeat — and is not a failure unless
`--require-current` is given (ceremony close-out). The final line is
`anchor policy anchor-policy.json: SATISFIED` with exit 0, or `NOT SATISFIED
(…)` with exit 1, as above.

## TSA identity

`tsa_url` is a note the operator recorded; the TSA's identity is read from
the token's embedded signing certificate and `SignerInfo`, and is unverified
until `--ca` or `--policy` checks the countersignature. `list` shows it as
`signer cert sha256:<16 hex>… (<CN>)`; `verify` re-derives it from the token
every time and reports a row whose cached identity disagrees with its token
as `FAIL`. A token whose CMS layout the reader cannot follow still imports —
its identity columns stay empty and `list` says `(not readable)`.

## Verifying anchors

```sh
log_anchor verify --db witness.db --ca tsa-ca.pem [--ca other-tsa-ca.pem]
```

Per anchor this checks, in order of increasing externality:

1. **Imprint consistency** — the stored token actually covers the hash the
   anchor row claims (parsed by SecuraCV's own RFC 3161 reader).
2. **Ledger membership** — the hash is in the ledger the row declares:
   sealed-event hash or checkpoint head for `chain_head`, the
   export-receipt, break-glass-receipt, or policy-change history for the
   receipt-head kinds. A tampered or regenerated ledger fails here: its
   history no longer contains the anchored state.
3. **The TSA countersignature** (with `--ca`) — delegated to an
   *independent implementation*, `openssl ts -verify`, the same
   second-implementation stance as the dual Rust/JS envelope verifiers.
   `--ca` repeats; each is tried in order and the row's `OK` line records
   which one validated it (`under <ca>`). Without `--ca` (or `--policy`) the
   structural checks still run, every row is `UNVERIFIED`, and the exact
   openssl command is printed for out-of-band verification.

No line reads `OK` without an openssl countersignature success. A verifier
needs only the DB (or the exported token), the TSA's public CA certificate,
and openssl — no SecuraCV toolchain is required for the trust-critical step.

## Honest limits

- An anchor proves the ledger head existed **at or before** the token's
  `genTime`. It cannot prove events did *not* exist earlier, and it says
  nothing about history recorded **after** the last anchor — anchor on a
  schedule to keep that window small.
- Trust shifts from the device clock to the **TSA's** key and clock.
  Reputable TSAs run audited HSM infrastructure, and anchoring at two
  independent authorities makes the residual risk multiplicative.
- Tokens embed the TSA's certificate (`certReq` is always set), so they
  remain verifiable after the TSA rotates keys — but verify against the
  TSA's published CA, not the embedded leaf alone.
- The anchors table is auxiliary evidence *about* the ledgers; it is not
  itself hash-chained. Deleting an anchor destroys proof, it never forges
  any — the protection is the usual one for backups: export tokens
  (`tsa_anchors.token_der`) alongside your DB backups.
- Retention keeps every anchored chain head as a device-signed checkpoint
  row; heads pruned by a build older than this one, or by an older
  `witnessd` still running against a database newer tools anchor into, are
  not recoverable — `log_anchor relabel` records that; upgrade `witnessd`
  first. `relabel --id N --subject digest` leaves the token untouched,
  re-checks the imprint, refuses a head that is newer than the signed
  retention cutoff (that shape is truncation, not a legacy prune), and never
  upgrades a subject.
- No shipped image carries the `openssl` CLI or the `tsa` feature; run
  `verify --ca`/`--policy` and online `request` from an operator host. The
  images do ship `log_anchor` for the offline flow and the structural
  `list`/`verify` checks.
- `list`, `verify`, `query`, and `anchor-all --offline-dir` open the
  database read-only and create nothing; a database with no anchors table
  reads as `no anchors stored`.

### Compatibility across versions

| Reader / writer mix | Behavior |
|---|---|
| older `log_anchor list`/`verify` on a newer DB (new subject literals, four new columns, extra checkpoint rows) | lists rows unchanged; new literals fall through to `digest anchor, chain membership not applicable`; preserved heads read as `in chain history` via `checkpoints` |
| older `court_export` on a newer DB | new receipt-head rows are not packaged (it recognizes only `chain_head` and the bundle digest) — privacy-safe; preserved heads pass its history check |
| older `log_verify` / `witnessd` on a newer DB | unaffected (they never read `tsa_anchors`; the latest checkpoint is still the real cutoff) |
| new binaries on an older DB / legacy eight-column anchors table | read-only verbs probe the columns and never migrate; write verbs (`request`, `import`, online `anchor-all`, `relabel`) add the columns on open |
| **older `witnessd` keeps writing while new `log_anchor`/`court_export` anchor into its DB** | anchored chain heads are still lost at every retention pass — only this release's `witnessd` writes the preserving checkpoints. `verify` then reports `anchored hash is not in chain history`; `relabel` is the remediation. **Upgrade `witnessd` before anchoring chain heads with these tools** |
| conformance fixture (no `tsa_anchors`) | retention and packaging take the no-table branch |
| offline `.tsq` whose `.tsr` is imported after the head was pruned | no row existed when retention ran, so nothing preserved the head; stored as `digest` with the stderr note; no membership, no policy credit — complete the round trip inside the retention window |
