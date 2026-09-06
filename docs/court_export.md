# Court export — the disclosure kit a lawyer can file

`court_export` assembles a **court-ready disclosure kit** around an export
bundle: the evidence, its digests, a custody-and-control record rendered from
the signed receipts, the RFC 3161 anchor tokens that fix it in time,
pre-filled FRE 902(13)/(14) certification drafts, a plain-English system
description, and verification instructions that opposing counsel can run with
`sha256sum` and `openssl` alone. It is the packaging layer the evidence rules
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
envelope_digest=$(sha256sum witness_export.json | cut -d' ' -f1)
log_anchor request --db witness.db --url https://freetsa.org/tsr --digest "$envelope_digest"

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
| `anchors/*.der` | The RFC 3161 tokens relevant to THIS disclosure, verbatim as bare DER TimeStampTokens (verified with `openssl ts -verify … -token_in`; a `.tsr` is a full response and takes no such flag), each one's embedded imprint checked against the digest its row claims: tokens over the bundle's exact bytes (called out as such) plus chain-head tokens. Digest anchors for other exports are deliberately excluded — including them would disclose that those exports exist |
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
   this ledger. If no valid stored anchor covers the bundle's exact
   bytes, the kit still assembles but says so loudly — in the terminal, in
   `VERIFICATION.md`, and as `"anchored": false` in the manifest — with the
   exact `log_anchor` command to fix it (which accepts the same
   `--device-key-seed` / `--db-key` options for an encrypted database).
4. **Fresh output directory**: the manifest attests every file in the kit,
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

Design lineage: this is the "court export" move from the provenance-interop
research ([`security/PROVENANCE_INTEROP.md`](security/PROVENANCE_INTEROP.md)
§2 move 4), specified in
[`../spec/quorum_unseal_v2.md`](../spec/quorum_unseal_v2.md) §5.
