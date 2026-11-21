# Verification methodology — checking the record without trusting SecuraCV

This is the procedure a third party follows when all they hold is the files:
a database or an export, the device's public key from an out-of-band source,
and the open tools named below. It is written in the order that party works
in. Nothing here asks the reader to take SecuraCV's word for a result:
every step either recomputes a hash or checks an Ed25519 signature against a
key the reader pinned. Most steps (§2–§6) run SecuraCV's own open-source
tools, which a reader can build from source and audit, and the three
implementations of §4 were all written by SecuraCV's authors, so their
agreement guards against a bug in one of them, not against their authors.
The steps that need no SecuraCV software at all are:

- the court kit's `VERIFICATION.md` (§7, §8), with `sha256sum`, `xxd` and
  `openssl`;
- the handoff-sidecar shell recipe (§5);
- `openssl ts` and `ots` (§6).

A reader who trusts no SecuraCV build cross-checks the tools' verdicts with
those.

It is the procedure the `procedural` validation rows of the maps point at
([`part11.md`](part11.md) row 11.10(a)-validation,
[`eu_gmp_annex11.md`](eu_gmp_annex11.md) row annex11-§4-validation). Whether
running it is enough for a given organization's validation is that
organization's determination ([README](README.md#what-the-operator-decides)).

## 1. What "Verified" means

"Verified" means an Ed25519 signature checked against a key the reader
pinned, and every SHA-256 recomputed from the bytes — nothing looser. The
verifier's verdict is exactly one of three strings
(`src/verify_runner.rs`, `VerifyReport.verdict`):

| Verdict | What it proves |
|---|---|
| `valid` | the chain and every signature check out under a key supplied from outside the database |
| `self-consistent; identity unverified` | the chain is internally consistent under the key the database itself names; it proves nothing about which device wrote it |
| `verification failed` | something did not check out: tampering, a wrong key or a stale database; the `failure` field says which check |

"Self-consistent" is internal consistency, not identity. A reader who has not
pinned the device key has not verified anything about where the record came
from.

## 2. Pin the device key out of band

Take the device's public key from somewhere the database cannot have
written: the pairing receipt, the operator's custody record, or a copy the
reader received at enrollment. Then:

```sh
log_verify --db witness.db --public-key-file device.pub --json
# expect "verdict":"valid"
```

After a device-key rotation, inspect the lineage with
`log_verify --db witness.db --lineage` and pin the genesis key; rows after a
rotation are signed by the key current at the time and are checked against
the lineage ([`docs/log_verify.md`](../log_verify.md)).

## 3. The ledgers

- `log_verify --db witness.db --public-key-file device.pub --verbose` prints
  one line per receipt (`receipt N: hash=<16 hex> OK`) and per
  clock-provenance record (`clock-provenance record N: hash=<16 hex> OK`).
- `break_glass receipts --db witness.db --public-key-file device.pub --verbose`
  prints the rendered record of each custody act: the name, the date and time
  (a 10-minute bucket) and the meaning of each approval.
- `break_glass policy history --public-key-file device.pub` walks the
  quorum-policy history and should print `History chain VALID` (CPS 8.4).

## 4. Three implementations

Three implementations check the same chain:

- Rust: the kernel's own verifier (`src/verify.rs`, `src/verify_runner.rs`).
- JS: [`viewer/verify_core.js`](../../viewer/verify_core.js), whose agreement
  with the Rust verifier on export bundles is pinned by
  `viewer/verify_core.test.js` (`describe('export-bundle verification parity'`).
- Python: [`custom_components/securacv/kernel_verify.py`](../../custom_components/securacv/kernel_verify.py)
  (`hash_entry`), a line-for-line port of the JS math (its module docstring
  says so), held to the JS on the shared vector by
  `custom_components/securacv/tests/test_kernel_verify.py`.

Why agreement matters: no single implementation is trusted. The shared vector
`tests/fixtures/envelope/sealed_log_document_vector.json` pins all three to
the same verdicts, so a divergence between them fails a test. The Python
verifier is a port of the JS one, so the two are not independent of each
other: a mistake carried into the port is not caught by their agreement. All
three are SecuraCV's, so agreement guards against a slip in one of them, not
against their authors; the no-SecuraCV recipes named at the top are the check
that does not depend on them.

## 5. Exports

- `export_verify --db witness.db --bundle export.json --public-key-file device.pub`
  checks a bundle against its signed export receipt.
- `envelope_verify --bundle export.json --public-key <hex>` checks the
  envelope's own signature.
- `handoff_sidecar verify --file <file> --sidecar <sidecar> --public-key-file <device.pub>`
  checks a handoff sidecar ([`docs/handoff_sidecars.md`](../handoff_sidecars.md),
  which also gives a shell recipe that needs no SecuraCV software).

## 6. Anchors

- `log_anchor verify --db witness.db --ca tsa-ca.pem` checks every RFC 3161
  anchor against the TSA's CA ([`docs/timestamping.md`](../timestamping.md#verifying-anchors)).
- `openssl ts -verify … -token_in` checks a single token with no SecuraCV
  software at all.
- `ots info` and `ots --no-bitcoin verify -d <hex>` read an OpenTimestamps
  proof.

What an OpenTimestamps proof that attests a Bitcoin block means: the digest
existed before that block was mined. It does not say who wrote the record,
and it says nothing about the record's content beyond its hash. An RFC 3161
token says the same about a TSA's clock, under the TSA's key.

## 7. Clock provenance

The court kit's `VERIFICATION.md` step 3b re-derives each clock-provenance
record with `sha256sum`, `xxd` and `openssl`: the record says what the
device's own clock read against a TSA token's time, clamped, with the poll
gap that bounds the reading ([`docs/timestamping.md`](../timestamping.md#clock-provenance-device-clock-vs-tsa-gentime)).

## 8. The court kit

A kit's `VERIFICATION.md` is the whole procedure for one disclosure, with
`sha256sum` and `openssl` only ([`docs/court_export.md`](../court_export.md#what-the-kit-contains)).
It is the version of this page a court's expert runs.

## 9. Designed, not in this build

Each of these is labeled "designed" and this section is rewritten when its
pull request lands (`spec/quorum_unseal_v2.md` §4):

- the Merkle tree and signed checkpoint over every ledger (item 3 F, `log_tree`) — designed;
- inclusion proofs for a disclosed record (item 3 G) — designed;
- reviewer-signed review attestations (item 3 H, `log_review`) — designed;
- fleet witnessing of checkpoints (item 3 I) — designed.

## 10. The SIEM projection

Designed (item 7 CP2, [`SIEM_PROJECTION.md`](SIEM_PROJECTION.md)): an
`audit_projection check` that re-derives a projection byte for byte from the
database, and a per-line signature recipe. This section becomes the
procedure when CP2 lands.

## 11. What no step can show

- that the host was not compromised ([`docs/root_paradox.md`](../root_paradox.md));
- how accurately a detector detects (validation sheets, item 7 CP3);
- tail rows dropped within one tick before anchoring (item 3's residual);
- anything before the oldest retained event;
- any time finer than the 10-minute bucket.
