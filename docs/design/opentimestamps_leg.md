# The OpenTimestamps leg of `anchor-all` (design, as built)

**Status: built for per-device subjects.** `spec/quorum_unseal_v2.md` §4
names "RFC 3161 to two TSAs … plus **OpenTimestamps** as a
trust-kind-independent leg". This is the account of how that leg was built,
what a verifier checks, what was rejected and why, and what is still open.
The operator's view is [`../timestamping.md`](../timestamping.md), "The
OpenTimestamps leg"; the kit reader's is [`../court_export.md`](../court_export.md).

## The decision in one paragraph

`log_anchor anchor-all --policy P` gains an OpenTimestamps leg on the same
cadence and over the same planned subjects as the TSA leg: every ledger head,
or the per-subject empty-ledger sentinel when a ledger is empty. Each planned
subject is submitted to every calendar the policy's optional `ots` object
names, as a **nonced commitment** `SHA256(subject_hash ‖ 16 fresh random
bytes)` — the reference client's own blinding — fresh per (subject,
calendar, run), sentinels included, so no calendar ever sees a ledger head or
a public sentinel and no calendar can link two runs or two calendars. Each
answer is stored as one complete `.ots` file per (subject, calendar) in a new
frozen table, `ots_anchors`, never in `tsa_anchors`. Earlier rows still
pending and within a fixed 7-day horizon of the run each get exactly one upgrade
GET, before the run's stamps; a reply that reaches a Bitcoin block-header
attestation is merged into the stored proof. The kernel gets a hand-written
`.ots` codec (`src/ots.rs`, always compiled, on `sha2` and `hex`), pinned
byte for byte to the `opentimestamps` crate's two real test proofs. Only the
two HTTP calls sit behind the non-default Cargo feature `ots = ["ureq"]`.
Verification is network-silent everywhere: `run_full_verify` checks OTS rows
structurally only; `log_anchor verify` takes one Bitcoin source,
`--bitcoin-headers <file>`, and says `confirmed` only when a proof's path
reaches the merkle root of a header the operator supplied. Policy coverage
counts only header-confirmed rows. The court kit packages
`anchors/ots-NNNN-<subject>.ots` under a separate `ots_anchors` MANIFEST
array and says "attests Bitcoin block N", never "confirmed". Retention keeps
chain heads anchored in either table. Nothing prints "Verified". No block
time is stored anywhere.

## What a verifier checks

| Stored fact | Check | Source of truth | Tool |
|---|---|---|---|
| Which digest the proof covers | the 32 bytes after `MAGIC ‖ 01 ‖ 08` equal the row's `subject_hash` (database) or the evidence's `sha256sum` (kit) | the `.ots` bytes | `log_anchor verify`; `ots verify -d <hex>` |
| The commitment path | replay every op from the digest: `append(nonce)`, `sha256`, then the calendar's `append`/`prepend`/`sha256` steps | arithmetic on the proof (the codec executes ops while parsing, as the reference does) | both |
| What a calendar received | a `Pending{uri}` leaf; its message is the calendar's own commitment | the proof | `ots info`, `log_anchor list` |
| That Bitcoin holds it | a Bitcoin leaf `{height}` whose 32-byte message equals bytes 36..68 of the block header, in header byte order (explorers print it reversed) | the header | `log_anchor verify --bitcoin-headers`; `ots verify` with a node; an explorer by hand |
| The supplied header is mainnet-shaped | `SHA256d(header)` read little-endian ≤ the target its `nBits` encodes, and that target ≤ Bitcoin's minimum-difficulty target `0x1d00ffff` | consensus proof-of-work rule on a lone header | the `--bitcoin-headers` loader — **a sanity check only**: it catches typos, testnet or regtest headers and the wrong file; a forger with modest hardware can satisfy it. The header source is the verifier's trust root |
| Row caches are honest | `attestation` and `bitcoin_height` equal what the proof's leaves say | re-derived from `proof` on every read | `log_anchor verify` (FAIL), `run_full_verify` (warning) |
| Ledger membership (head kinds) | `subject_hash` is in the declared ledger's history: a live row or a signature-verified checkpoint | `tsa::hash_in_ledger`, shared with the TSA leg | `log_anchor verify`, `run_full_verify` |
| Not pretending | pending never covers; an unchecked Bitcoin leaf is `UNVERIFIED` and never covers; kit text says "attests" | this design | — |

What a verifier **cannot** check: whether the calendar was honest (irrelevant
once a Bitcoin path exists — the calendar only relayed the commitment);
whether their header source is the real chain (that is their trust base);
who created the commitment (OpenTimestamps proves existence, not identity).

## The wire format the codec implements

Cited against the `opentimestamps` 0.2.0 crate (MIT OR Apache-2.0) and, for
what that crate does not implement (bounds, canonical order, the calendar
protocol), the Python reference `opentimestamps` 0.5.0 and
`opentimestamps-client` 0.8.0, used as the protocol reference only.

- **File** = `\x00OpenTimestamps\x00\x00Proof\x00\xbf\x89\xe2\xe8\x84\xe8\x92\x94` ‖
  version `1` ‖ file-hash op `0x08` (SHA-256) ‖ 32 digest bytes ‖ a timestamp ‖
  EOF. Trailing bytes are an error. SHA-1 (`0x02`) and RIPEMD-160 (`0x03`)
  file digests are refused: our rows hold SHA-256 hashes.
- **varuint**: LEB128, at most 9 bytes (63 bits) — the reference's bound. The
  crate's reader has none.
- **Timestamp**: a node at a message; items, each but the last prefixed
  `0xff`; an item is `0x00 ‖ attestation` (a leaf) or `op ‖ sub-timestamp`.
  A repeated op in one node is an error (the reference's rule); a repeated
  attestation is refused too, which keeps the byte-exact round trip. The
  recursion budget is the reference's: 256, so 255 nested ops parse.
- **Ops**: `0x08` sha256, `0xf0` append, `0xf1` prepend, `0xf2` reverse,
  `0xf3` hexlify are executed. `0x02` sha1, `0x03` ripemd160 and `0x67`
  keccak256 are refused with `op 0x.. (<name>) is not supported by this
  verifier; check the proof with the ots client` — no calendar emits them and
  executing them would need two more hash crates. Anything else is
  `unknown op tag`. Argument 1..=4096 bytes; input message ≤ 4096 (hexlify
  ≤ 2048); result ≤ 4096.
- **Attestation** = 8-byte tag ‖ varbytes payload ≤ 8192, **consumed exactly**.
  Bitcoin `0588960d73d71901`: a varuint height ≤ 2³²−1. Pending
  `83dfe30d2ef90c8e`: a URI ≤ 1000 bytes of `[A-Za-z0-9._/:-]`. Any other tag
  (Litecoin, Ethereum, a future kind) is kept verbatim and round-trips; it is
  never a Bitcoin claim.
- **Canonical serialization**, the reference's `Timestamp.serialize` order,
  by explicit comparators (never a derived `Ord`): attestations first —
  different kinds by their 8-byte tag (Bitcoin before Pending), Bitcoin by
  height, Pending by URI bytes, unknown by `(tag, payload)`; then ops by
  `(tag, argument bytes)`. Both real fixtures round-trip byte for byte, and
  two merge reproductions rebuild them from derived parts.
- **Calendar protocol**: stamp is `POST {url}/digest` with the raw 32-byte
  commitment and `Accept: application/vnd.opentimestamps.v1`; a 200 body is a
  bare timestamp over the submitted bytes, ≤ 10 000 bytes. Upgrade is
  `GET {uri}/timestamp/{hex of the pending leaf's message}` — the calendar's
  own commitment, not ours; 404 means not yet committed. No redirect is
  followed (the reference follows same-host https redirects; we follow
  none), and ureq's default User-Agent is sent, as on the TSA leg.
- **Merge**: union the attestations, union the ops, recurse where an op exists
  on both sides. The pending leaf is kept beside the new path, as `ots
  upgrade` leaves it.

The second implementation is the Python `ots` client: `VERIFICATION.md` names
it, and an optional skip-if-absent test runs `ots info` on the fixtures and on
a proof the codec composed. A fuzz target (`ots_parse`) holds the parser to
"never panics" and "canonical form is a fixed point".

## Storage

```sql
CREATE TABLE IF NOT EXISTS ots_anchors (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_bucket_start INTEGER NOT NULL,
  created_bucket_size INTEGER NOT NULL,
  subject TEXT NOT NULL,
  subject_hash BLOB NOT NULL,
  calendar_url TEXT NOT NULL,
  proof BLOB NOT NULL,
  attestation TEXT NOT NULL,
  bitcoin_height INTEGER,
  upgraded_bucket_start INTEGER,
  ledger_id INTEGER
);
```

The `CREATE TABLE` text is frozen (pinned by a unit test); later columns go
through the reserved, empty `OTS_EXTRA_COLUMNS`, as on `tsa_anchors`.
`subject`, `subject_hash`, the bucket columns and `ledger_id` have the
`tsa_anchors` semantics. `calendar_url` is provenance (the URL the commitment
was submitted to) and selects which pending leaf the upgrade pass polls.
`proof` is the complete canonical `.ots` file over `subject_hash`, readable by
`ots info` at every stage. `attestation` (`pending` | `bitcoin`) and
`bitcoin_height` (the lowest Bitcoin height) are caches every verifier
re-derives. Not stored: the nonce outside the proof, any wall-clock instant,
any block time, the calendar's display name, anything about events.

The table is created lazily by the first insert; no verb creates it
speculatively, readers treat absence as "no OpenTimestamps anchors", and
`witnessd` never creates it. The one UPDATE turns a pending row into a
Bitcoin row, once, and only after checking that the new proof covers the same
digest and contains every leaf of the stored one: a merge only adds, and a
proof is self-checking, so a tampered update can drop evidence but never
forge it. Before either the INSERT or the UPDATE, the canonical bytes are
parsed back and must reproduce themselves: a calendar body is parsed with the
full recursion budget, but `compose` and `merge_at` graft it below ops it was
not parsed under, so a body deep enough could otherwise yield a stored file
that nests past the limit and that no reader (ours or the `ots` client)
accepts. A refused upgrade leaves the pending row and its readable proof
untouched.

Retention keeps an anchored chain head as a signed checkpoint when a row in
`tsa_anchors` **or** `ots_anchors` covers it (each clause only when its table
exists); otherwise a head anchored only on this leg would be pruned and
`run_full_verify` would fail the verdict with `anchor_regression`.

## Invariant III

`docs/timestamping.md`, "The OpenTimestamps leg and Invariant III", states the
eight-point argument in full: no kernel involvement; a constant submission
count (`|planned subjects| × |calendars|`, sentinels substituted, no skip, the
leg runs whether or not the TSA leg failed); upgrade count a function of
cadence and calendar latency (at most one GET per pending row within 7 days
of the run's bucket on either side, upgrades before stamps, no rows created); random request content (the
nonce); the operator's schedule (no sleep, poll or wait; a feature-less build
refuses a policy with `ots` before the first request); no precise external
time in any record (a height is not a time; buckets only); egress compiled
in only under `ots`, verification silent; offline mirrors online.

Residuals, stated so nobody weakens them later: a calendar learns the source
address, the cadence and the per-run count (a configuration constant plus a
bounded number of upgrade GETs); a calendar can stall a proof forever
(availability, not forgery); the chain holds the calendar's merkle root, not
ours; a failed run changes that run's count; an operator one-off
(`request --policy` over a bundle digest, an `import --ots-dir`, a repeated
import) stores pending rows that later `anchor-all` runs also poll, adding a
bounded number of upgrade GETs over the next ≤ 7 days — at most one per such
row per run, each naming a commitment the calendar already holds, and none
driven by a witnessed event; a court kit discloses its
packaged proofs (nonces, calendar hostnames, block height ≈ anchoring time to
a block interval), kit-scoped and selected by hash; an explorer lookup tells
the explorer which block the verifier cares about.

## Policy

An optional, additive `ots` object on `securacv-anchor-policy:v1` — no format
bump, since `validate` rejects other format literals exactly as
`deny_unknown_fields` rejects a new key:

```json
"ots": {
  "calendars": [
    { "name": "alice", "url": "https://alice.btc.calendar.opentimestamps.org" },
    { "name": "bob",   "url": "https://bob.btc.calendar.opentimestamps.org" }
  ],
  "required": false
}
```

One to eight calendars, names unique across both rosters, URLs restricted to
the pending-attestation charset with no trailing `/`, query or fragment.
`subjects` applies to both legs; `tsas` stays ≥ 1. Coverage
(`evaluate_ots`): per subject, the current head wins when at least one of its
rows is header-confirmed, else the newest confirmed hash by `(bucket, id)`
with `current head: NO`; otherwise `NOT confirmed` with the pending and
unchecked counts. Pending never covers; unchecked never covers; rows of
another kind (sentinel and digest rows included) never count.
`--require-current` does not apply to this leg — Bitcoin confirmation lags by
hours by design. With `required: true`, `verify --policy` refuses to run
without `--bitcoin-headers`, and each non-empty subject without a confirmed
proof fails the verdict.

## Verify tiers

`OK … confirmed in Bitcoin block N against the supplied header, block hash
<hash>` (a leaf reaches a supplied header's merkle root) · `UNVERIFIED …
attests Bitcoin block N; merkle root NOT checked` (no header file) or `… no
header for block N in <file>` · `UNVERIFIED … pending (not yet in a Bitcoin
block) at <uri>` (with the beyond-horizon note past 7 days) · `FAIL` with a
problem per line, including any Bitcoin branch whose supplied header
contradicts it, even when another branch matches. `run_full_verify` never
checks Bitcoin (there is no header input, and it runs at `witnessd` boot):
it reports `ots_anchors_checked`, warns on a malformed row, an unparseable
proof, a digest mismatch or a cache disagreement, and fails the verdict with
`anchor_regression` on the `ots_anchors` ledger when a head-kind row's hash
has left its ledger.

## Court kit

`anchors/ots-NNNN-<subject>.ots`, the stored proof verbatim, selected by the
TSA leg's rule — by hash first (the bundle's bytes, this disclosure's export
receipt entry), label last (chain heads in this database's history) — with
everything else left out silently. MANIFEST gains `ots_anchor_count`,
`ots_bundle_attestation`, `ots_receipt_attestation` (`bitcoin` | `pending` |
`none`; `bitcoin` means the proof attests a block, not that anything was
confirmed) and the `ots_anchors[]` array; `anchors[]`, `anchor_count`,
`anchored` and `receipt_anchored` keep their RFC 3161 meaning, and an OTS
proof never silences the "no RFC 3161 anchor covers this bundle" warning.
`VERIFICATION.md` gives `ots info`, `ots --no-bitcoin verify -d <hex>` and the
block whose merkle root the reader looks up; the custody record says
"attests Bitcoin block N (merkle root … — check it against a header from a
source you trust; this kit records the height only)". A bundle digest gets a
proof through `log_anchor request --policy` (online) or
`query --policy` + `import --ots-dir` (offline).

The kit's `VERIFICATION.md` places `### OpenTimestamps proofs (optional)`
inside `## 3. Verify the timestamp anchors`, after the openssl recipes and
before `## 3b. Verify the clock-provenance records`. The design brief said
both "under step 3" and "after step 3b"; an unnumbered `###` placed after 3b
would read as part of the clock-provenance step, so it stays under step 3,
where the README's "the optional OpenTimestamps check in step 3" points. The
README's sentence about the proofs follows their state: proofs that attest a
block "attest Bitcoin blocks"; pending proofs are "pending (not yet in a
Bitcoin block) and are not yet evidence of time".

## Compatibility

| Reader / writer mix | Behavior |
|---|---|
| pre-OTS readers on a DB with `ots_anchors` | never read it; a pre-OTS `witnessd` still prunes OTS-only-anchored chain heads — upgrade it first; `relabel --ots` repairs |
| pre-OTS `log_anchor` given a policy with `ots` | refused at load (unknown field) — add the key after every reader is upgraded |
| new binaries on a DB without the table | output unchanged; `ots_anchors_checked` omitted; kit keys `0` / `none` / `[]` |
| `anchor-all` with a policy without `ots` | identical to before, byte for byte; the table is never created |
| build with `tsa` but not `ots`, policy with `ots` | online verbs refuse before any request; offline flow, import, verify, list and the kit work |
| frozen formats | `tsa_anchors`, `token_der`, `TimestampToken`, `ExportReceipt`, the policy format literal, the existing MANIFEST keys and the verdict vocabulary are untouched; `Cargo.lock` gains no crate |

## Rejected, with reasons

- **A `kind` column on `tsa_anchors`.** `token_der` is a DER TimeStampToken by
  contract, and every existing reader (`run_full_verify`, `court_export`,
  `log_anchor verify`, the clock-provenance observer) would mishandle an OTS
  row. A separate table is invisible to every older reader.
- **The `opentimestamps` crate.** Its varuint reader has no byte bound; it
  ignores the attestation payload length for Bitcoin and Pending, so neither
  the 8192-byte bound nor exact consumption holds; it pulls `env_logger` and
  `log` as library dependencies; it has no calendar client, merge, header
  check or canonical order.
- **`opentimestamps-client` (Rust).** Third party, a newer MSRV, an async HTTP
  stack, and it verifies against a hosted explorer by default.
- **A policy format bump.** Buys nothing over an optional key; the
  clock-provenance threshold made the same call.
- **An `ots` role in `require_roles`, or `min_calendars`.** Roles are
  declarations about TSAs; a Bitcoin-confirmed proof needs no trust in its
  calendar, so counting calendars adds no assurance — redundancy is declaring
  two.
- **`--bitcoin-rpc <url>` or an explorer lookup in `verify`.** A URL can name
  a hosted endpoint that then learns which blocks this database cares about.
  `verify` stays network-silent.
- **A per-run Merkle tip over all subjects.** That is the spec's fleet
  aggregate, a second composition algorithm.
- **Dropping the nonce.** The sentinel is a public constant: a calendar would
  learn which ledgers are empty, when the first event lands, and whether a
  head changed between runs.
- **A product User-Agent; following redirects; storing a block time; a
  `--wait` loop; a configurable horizon; executing SHA-1/RIPEMD-160/Keccak;
  mixing OTS entries into MANIFEST `anchors[]`; a clock-provenance record per
  OTS row** (a block height is not an issuer time).

## Fixtures

`tests/fixtures/ots/` (its README is the provenance record): the crate's two
real proofs; seven files derived from them by the codec (and checked to be
byte substrings of their parents, independently of our serializer); a
synthetic genesis proof with the real genesis header, which runs the `OK`
path offline. The real headers for blocks 449397 and 449399, which would
confirm the crate's real Bitcoin proof, were not obtainable from this
implementation's environment; adding those two lines (fetched once from a
node, accepted only when bytes 36..68 equal the proof's roots and the
proof-of-work check holds) is an open item, and the test that uses them is
not yet written.

## Follow-ups

- The two real header lines (449397, 449399) and the test that confirms the
  real Bitcoin fixture against them.
- One live `anchor-all` against a public calendar before a release note
  claims interoperability (ureq sends no `Content-Type` on the POST; the
  reference server reads the body by length).
- A loopback-only node RPC as a header source, under a later feature.
- The fleet aggregate of `spec/quorum_unseal_v2.md` §4 (one Merkle tip per
  run across devices).
- A live differential fixture captured with the Python client.
