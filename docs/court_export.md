# Court export — the disclosure kit a lawyer can file

`court_export` assembles a **court-ready disclosure kit** around an export
bundle: the evidence, its digests, a custody-and-control record rendered from
the signed receipts, the RFC 3161 anchor tokens that fix it in time,
pre-filled FRE 902(13)/(14) certification drafts, a plain-English system
description, and verification instructions that opposing counsel can run with
`sha256sum` and `openssl` alone (the clock-provenance records add `xxd`,
`cut` and `date`). It is the packaging layer the evidence rules
actually consume — the cryptography underneath already existed
([`security/PROVENANCE_INTEROP.md`](security/PROVENANCE_INTEROP.md) §1.1).

The tool **authorizes nothing and touches no raw media**: it packages what an
already-authorized export disclosed. It fails closed — a bundle that does not
verify, or whose receipt is not in the producing database's export-receipt
chain under the same device identity, is refused, never packaged.

## Usage

```sh
# 1. Export (either auth mode; see docs on export_events)
export_events --self-export --db witness.db --output witness_export.json

# 2. Anchor the exact bundle bytes at a TSA (strongly recommended first)
log_anchor request --db witness.db --url https://freetsa.org/tsr --file witness_export.json
# or, with an anchor policy: log_anchor anchor-all --db witness.db --policy anchor-policy.json

# 2b. Optionally anchor the export-receipt chain head too (this export's
#     receipt is the newest row right after the export)
log_anchor request --db witness.db --url https://freetsa.org/tsr --subject export_receipt_head

# 3. Assemble the kit
court_export --bundle witness_export.json --db witness.db --output-dir court_kit/
```

For an encrypted database, pass `--device-key-seed` (or `DEVICE_KEY_SEED`)
exactly as `log_verify` does. A `.c2pa` sidecar sitting beside the bundle is
copied into the kit automatically — it signs those exact bytes and travels
with them.

## What the kit contains

| File | What it is |
|---|---|
| `README.md` | Reading order and the FRE 902(11) notice reminder |
| `SYSTEM_DESCRIPTION.md` | The FRE 901(b)(9) "silent witness" foundation: what produced the evidence and why its output is accurate, honest limits included |
| `CUSTODY_AND_CONTROL.md` | The item, its digests, its authorization (self-export vs. trustee quorum), and its position in the signed receipt chain |
| `VERIFICATION.md` | Steps runnable with `sha256sum` + `openssl` only; the SecuraCV-tooling path is optional depth |
| `CERTIFICATION_FRE_902_13.md` / `_14.md` | Draft 28 U.S.C. § 1746 declarations, digests pre-filled, blanks for the certifier |
| `evidence/` | The bundle exactly as exported (plus its C2PA sidecar, if any) |
| `anchors/*.der` | The RFC 3161 tokens relevant to THIS disclosure, verbatim as bare DER TimeStampTokens (verified with `openssl ts -verify … -token_in`; a `.tsr` is a full response and takes no such flag), each one's embedded imprint checked against the digest its row claims: tokens over the bundle's exact bytes (called out as such), any token over this disclosure's own signed export receipt entry hash (called out as such, whatever its stored subject — `export_receipt_head` or a `digest` anchor made from `receipts --verbose`), plus chain-head tokens. Digest anchors for other exports, later export-receipt heads, empty-ledger sentinels, break-glass and policy heads are deliberately excluded — including them would disclose that those acts exist |
| `clock_provenance/clock-NNNN.json` + `clock-NNNN.payload.json` | For every packaged token that has one, the producing device's own clock-provenance record ([`timestamping.md`](timestamping.md), "Clock provenance"): what the device's clock read against that token's `genTime`. The `.payload.json` file is the exact signed bytes; the `.json` beside it carries `prev_hash`, `entry_hash`, the signing domain, the Ed25519 signature and the lineage key it verifies under. Absent when no packaged token has a record |
| `evidence/<bundle>.securacv-handoff.json` | The handoff sidecar ([`handoff_sidecars.md`](handoff_sidecars.md)): this disclosure's device-signed records — the export receipt, and for a quorum export its grant and redemption — verbatim, for a downstream chain-of-custody system. Copied verbatim when one sits beside the bundle (after it verifies), generated otherwise |
| `anchors/ots-NNNN-<subject>.ots` | The OpenTimestamps proofs relevant to THIS disclosure ([`design/opentimestamps_leg.md`](design/opentimestamps_leg.md)), the stored proof verbatim, selected by the same rule as the tokens — by hash first (bundle bytes, this disclosure's receipt entry), chain heads in this database's history last — and each one's digest checked against the row. Each says either "attests Bitcoin block N" (with the merkle root the reader looks up) or "pending (not yet in a Bitcoin block)"; never "confirmed". Absent when no proof qualifies |
| `MANIFEST.json` | Machine-readable list of every kit file with its SHA-256 |

## What it checks before packaging

1. **The bundle verifies** (`verify_export_bundle`): receipt signature, entry
   hash, artifact hash. An unverifiable bundle is refused.
2. **Custody**: the **entire export-receipt chain verifies from genesis**
   (every row's prev-hash link, entry hash, and device signature, under the
   database's pinned key — the custody record says "receipt N of M in the
   signed chain", and that sentence is only written over a chain that
   actually holds), the bundle's receipt is one of the verified rows, the
   database's pinned device identity is the bundle's signer, and the stored
   signature is byte-identical to the bundle's (Ed25519 signing is
   deterministic, so this ties the row to the key, not just to colliding
   content).
3. **Anchoring**: every packaged token's DER message imprint must parse and
   equal the digest its anchor row claims — a token proves only what it
   embeds, so an unparseable or mismatched token is excluded with a warning
   rather than counted. A chain-head anchor must additionally reference a
   hash recorded in this database's chain history, or it fixes nothing about
   this ledger. A token is packaged as covering **this disclosure's export
   receipt** by hash alone — its `subject_hash` equals the bundle's receipt
   `entry_hash`, whatever subject text the row carries — because that hash is
   the disclosure's own signed receipt, already proven a member of the
   verified chain in check 2; a mislabeled row cannot widen the rule, and
   every other row (other digests, later export-receipt heads, break-glass and
   policy heads, sentinels) is skipped without a trace. The manifest records
   `"receipt_anchored": true|false` beside `"anchored"` and lists every
   packaged token in an `anchors` array (`file`, the row's stored `subject`,
   `covers` ∈ `bundle_bytes` / `receipt_entry` / `chain_head`, and a `tsa`
   object holding the operator-declared TSA name recorded on the anchor row
   (`declared_name` — a policy label the token does not attest; null for
   rows anchored without a policy) and the signer-certificate SHA-256 read
   from the packaged token (`signer_cert_sha256`, null when the token embeds
   no certificate)). If no
   valid stored anchor covers the bundle's exact bytes, the kit still
   assembles but says so loudly — in the terminal, in `VERIFICATION.md`, and
   as `"anchored": false` in the manifest — with the exact `log_anchor`
   command to fix it (which accepts the same `--device-key-seed` / `--db-key`
   options for an encrypted database) and its offline equivalent.
   `VERIFICATION.md` states that the receipt entry hash is recomputable from
   the evidence file alone (the offline viewer's export-bundle mode or the
   library's `verify_export_bundle`); the database-side checks in its step 4
   are a separate, deeper check.
4. **Clock provenance**: the producing database's clock-provenance ledger is
   walked from genesis (every link, every entry hash over the stored payload
   literal, every device signature under the key lineage the kit already
   derived). A ledger that does not verify packages **no** clock record —
   the terminal prints a `WARNING`, the manifest says
   `"clock_provenance_ledger": "verification failed"`, and the custody
   record and `VERIFICATION.md` say nothing is asserted about the device's
   clock (fail closed). Over a ledger that holds, the record naming each
   packaged anchor is packaged only when its `token_sha256` equals the
   SHA-256 of the token bytes actually packaged; a mismatch is left out with
   a `WARNING` and counted in `README.md` and the custody record. `CUSTODY_AND_CONTROL.md` gains a **Time source** section
   (one line per packaged record: the offset, the poll gap, how long after
   issuance the row was stored, and the sentence "an event's bucket under a
   device offset is the same width shifted — a bias correction adds no
   precision"); `VERIFICATION.md` gains **step 3b**, which re-derives every
   stored fact in POSIX `sh` with `sha256sum`, `xxd`, `cut`, `date` and
   `openssl pkeyutl -rawin` (Ed25519 over `SHA256(u32le(32) ‖
   "securacv:pwk:clock-provenance:v1" ‖ entry_hash)`). Every hex value is
   written into the commands, so no JSON parser is needed; the entry hash is
   recomputed from the payload bytes and the signed message built from that
   recomputation; the verifying key is the device public key the custody
   record and the evidence file name — never a key read from the record
   under check — and a record that verifies under another key of the
   device's lineage (a rotation) says the kit alone cannot tie that key to
   the device. Step 2 is generated per record: an in-bucket check for a
   measured offset, a one-sided check at the ±3600 s clamp (a bound, not a
   reading), "does not apply" for a binding-only record. `README.md` names
   the directory as item 7, in the same three states as the manifest
   (packaged; absent; not packaged because the ledger did not verify). The manifest
   gains two additive keys: `clock_provenance_ledger` —
   `"self-consistent; identity unverified"`, `"verification failed"` or
   `"absent"`, never "verified" (the ledger is checked under the lineage the
   kit derived; whether that lineage was pinned out of band is
   `identity_verified`) — and, per entry of `anchors`, a `clock_provenance`
   object (`file`, `payload_file`, `record_id`, `observed_offset_s`,
   `poll_gap_s`, `binding_only`, `signer_is_device_key`) or `null`. Only
   records the packaging-time chain walk checked are packaged: one
   `witnessd` commits between the walk and the listing is left for the next
   kit. The terminal summary adds
   `clock:     N clock-provenance record(s) packaged; ledger <status>`.
   `clock_provenance_head` anchors, which fix the ledger itself in time,
   are never packaged: like the other receipt-ledger heads they would
   disclose activity outside this export, so a kit carries the records of
   its own tokens and leaves the ledger's own anchors in the producing
   database.
5. **Handoff sidecar**: a `<bundle>.securacv-handoff.json` beside the input
   bundle is checked against the bundle and the database
   (`handoff_sidecar verify --db` semantics, under the lineage the kit
   derives) **before the kit directory exists**. Any FAIL refuses the whole
   kit: `custody break: the handoff sidecar beside the bundle does not
   describe this bundle (<subject>: <detail>). Remove or replace <path>
   (handoff_sidecar emit writes a fresh one) — a kit must not carry a sidecar
   that contradicts its evidence. Refusing to package.` A sidecar that holds
   is copied verbatim (`source: supplied`) — it may name the recipient, so
   run `handoff_sidecar redact` first and put the redacted copy beside the
   bundle to keep the name out of a kit. Without one, the kit generates one
   (`source: generated`; recipient binding `committed_not_disclosed` when the
   receipt commits to a recipient, else `none`). `CUSTODY_AND_CONTROL.md`
   gains a **Disclosure context** section — authorization, the authorizing
   grant (with the trustees it names), requester, reason code, case
   reference, requester key, and a **Source of these fields** row that says
   whether they are trustee-consented (read from the grant, bound into the
   request hash the trustees approved) or declared by the exporting operator
   (a self-export), and how the recipient is recorded — and a **Handoff
   sidecar** section. `VERIFICATION.md` step 1 adds the sidecar's
   `sha256sum`, step 3 notes that each `token_sha256` in the sidecar equals a
   line of `sha256sum anchors/*.der`, and step 4 adds `handoff_sidecar verify
   --file evidence/<bundle> --sidecar evidence/<sidecar> --anchors-dir
   anchors --public-key-file <device.pub> [--ca <tsa-ca.pem>]` (or the shell
   recipe). `README.md` names it as item 8. The manifest gains
   `evidence.context` (null, or `{operator, recipient_commitment}`),
   `evidence.authorizing_receipt` (null or hex) and a `handoff` object
   (`file`, `sha256`, `source`, `pairing`, `recipient_binding`); `files[]`
   hashes the sidecar. The terminal prints
   `handoff:   evidence/<sidecar> (supplied, verified | generated); recipient: <binding>`.
6. **OpenTimestamps proofs**: a proof is packaged only when it parses and
   its digest equals the row's hash, else `WARNING: ots anchor #N excluded
   from the kit: …`; a chain-head proof whose hash is not in this database's
   history is excluded likewise. Its state is derived from the proof, never
   from the row's cache. `MANIFEST.json` gains four additive keys, always
   present: `ots_anchor_count`, `ots_bundle_attestation` and
   `ots_receipt_attestation` (`"bitcoin"` | `"pending"` | `"none"` — the
   strongest state among proofs covering the bundle bytes or the receipt
   entry; `"bitcoin"` means the proof attests a Bitcoin block, not that
   anything was confirmed), and `ots_anchors[]` (`file`, `subject`, `covers`,
   `calendar_url`, `attestation`, `bitcoin_height`, `merkle_root` in explorer
   byte order, `pending_uris`; height and root `null` while pending; no
   `clock_provenance` key). `anchors[]`, `anchor_count`, `anchored` and
   `receipt_anchored` keep their RFC 3161 meaning, and an OpenTimestamps
   proof never silences the "no RFC 3161 anchor covers this bundle"
   warning. `VERIFICATION.md` step 3 gains `### OpenTimestamps proofs
   (optional)` — `ots info`, `ots --no-bitcoin verify -d <hex>`, and the
   block whose merkle root to compare against a node or explorer you trust
   (or, for a pending proof, what `ots upgrade` would do) — and its intro
   names the `ots` client. The subsection sits inside step 3, after the
   openssl recipes and before step 3b. The README's sentence about the
   proofs follows their state: proofs that attest a block "attest Bitcoin
   blocks for digests named in `CUSTODY_AND_CONTROL.md`"; when every proof is
   pending it says they "are pending (not yet in a Bitcoin block) and are not
   yet evidence of time". The terminal summary adds
   `ots:       N proof(s), B attest a Bitcoin block, P pending`. A bundle
   digest gets a proof from `log_anchor request --policy p.json --file
   bundle.json` (online) or `query --policy` then `import --ots-dir`.
7. **Fresh output directory**: the manifest attests every file in the kit,
   so `--output-dir` must be absent or empty — a directory with any
   pre-existing content is refused.

## Honest limits

- The certifications are **drafts prepared by software** for the person who
  performed the verification to review with counsel. Nothing here is legal
  advice.
- Timestamp tokens bound timing from above ("existed no later than"); an
  unanchored bundle's timing rests on the device clock and key.
- The kit renders times at the 10-minute bucket granularity the receipts
  carry — the disclosure discloses nothing finer (Invariant III).
- A clock-provenance record states what the device's clock read when it
  first saw a token. It is the device's own signed statement; it says
  nothing about when any event happened, and the evidence's 10-minute
  buckets stay 10-minute buckets. The only instant it yields to the second
  is the time of the anchoring act, which the token already fixes.
- An OpenTimestamps proof in the kit records a block **height** only. The
  kit says "attests Bitcoin block N"; confirmation is the comparison the
  reader runs — the proof's merkle root against block N's header from a
  source they trust. A match shows the bytes existed before block N was
  mined, to roughly a block interval; it says nothing about who made them.
  A pending proof shows only that a calendar received a commitment.

Design lineage: this is the "court export" move from the provenance-interop
research ([`security/PROVENANCE_INTEROP.md`](security/PROVENANCE_INTEROP.md)
§2 move 4), specified in
[`../spec/quorum_unseal_v2.md`](../spec/quorum_unseal_v2.md) §5.
