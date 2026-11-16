# Handoff sidecars — the operator and verifier guide

A **handoff sidecar** is the file that travels with a disclosed file:
`<file>.securacv-handoff.json`, beside it. It carries the device-signed
receipt rows of the disclosure that released the file — the trustees'
grant, the redemption that spent the token, and the device's own unseal
record or export receipt — so a downstream chain-of-custody system can
check them without the hub. The design record is
[`docs/design/handoff_sidecars.md`](design/handoff_sidecars.md); the
normative bullet is `spec/quorum_unseal_v2.md` §5.

The sidecar itself is **unsigned**. It is a carrier: every fact it vouches
for is in a device-signed row it embeds verbatim, or is labeled
*declared*. It adds no signing domain.

## Who writes one

| Act | Sidecar | Recipient input |
|---|---|---|
| `break_glass unseal` | `<output-dir>/<envelope>.raw.securacv-handoff.json` | `--recipient-file` (preferred) or `--recipient-name/--recipient-org/--recipient-key` |
| the break-glass console (`/breakglass/unseal`) | beside the `.raw`; the reply's `handoff_sidecar` names it | none yet (`recipient_binding: none`) |
| `break_glass drill` | in its sandbox, then verified under the drill key | none |
| `export_events` (both modes, `--output-dir` runs included) | beside the bundle; pruned with it by `--keep` | yes |
| `court_export` | verifies and copies a sidecar beside the bundle, or generates one | none |
| `handoff_sidecar emit` | after the fact, read-only, never overwrites | yes (declared only) |
| `witness_api GET /export/bundle` | none (the bytes go over HTTP) | run `emit` on the saved bundle |

Every sidecar is written **0600**, create-new: it can name a recipient.

## What it says — every key

The bytes are canonical `securacv-cjson-v1` (keys sorted, no whitespace, no
trailing newline), so `sha256sum` of a sidecar is well-defined. Read one
with `jq .`. A pretty-printed or re-serialized copy FAILs verification:
re-emit, never edit.

```json
{
  "anchors": [
    {"covers": "disclosure_record", "kind": "rfc3161", "subject_hash": "<64 hex>", "token_sha256": "<64 hex>"}
  ],
  "created_bucket": {"size_s": 600, "start_epoch_s": 1794177000},
  "device_public_key": "<64 hex>",
  "disclosure": "unseal",
  "file": {"bytes": 1048576, "name": "vault-1.raw", "sha256": "<64 hex>"},
  "format": "securacv-handoff:v1",
  "pairing": "plaintext_commitment",
  "recipient": {"name": "Dana Reyes", "organization": "Example County Records Office"},
  "recipient_binding": "receipt_commitment",
  "records": [
    {"approvals_json": "[…verbatim…]", "entry_hash": "<hex>", "ledger": "break_glass_receipts",
     "payload_json": "{…verbatim Granted…}", "prev_hash": "<hex>", "role": "grant", "signature": "<128 hex>"},
    {"approvals_json": "[]", "entry_hash": "<hex>", "ledger": "break_glass_receipts",
     "payload_json": "{…TokenRedeemed…}", "prev_hash": "<hex>", "role": "redemption", "signature": "<128 hex>"},
    {"approvals_json": "[]", "entry_hash": "<hex>", "ledger": "break_glass_receipts",
     "payload_json": "{…Unsealed…}", "prev_hash": "<hex>", "role": "unsealed", "signature": "<128 hex>"}
  ],
  "salts": {"plaintext": "<64 hex>", "recipient": "<64 hex>"},
  "tool_version": "<CARGO_PKG_VERSION>"
}
```

| Key | Values | Standing |
|---|---|---|
| `anchors[]` | `{covers, kind: "rfc3161", subject_hash, token_sha256}` entries, selected by hash only; `[]` when none | third-party token, checked by imprint (and by countersignature with `--ca`) |
| `created_bucket` | `{size_s: 600, start_epoch_s: multiple of 600}`, the emission bucket and the **only** time value the sidecar owns | declared |
| `device_public_key` | 64 hex; the one lineage key every record verifies under | checked against `--public-key*` or the DB lineage |
| `disclosure` | `"unseal"` \| `"export"` | checked against `records` |
| `file` | `{bytes, name (basename only; must pass check_text_field; display_safe on output), sha256}` | recomputable by anyone holding the file |
| `format` | `"securacv-handoff:v1"` | — |
| `pairing` | `"plaintext_commitment"` (an `unsealed` record plus `salts.plaintext` open over the file) \| `"export_receipt"` (the file is the bundle whose receipt is the embedded record) \| `"declared"` | checked |
| `recipient` | `null` \| `{"key"?, "name", "organization"?}` | always **declared** |
| `recipient_binding` | `"receipt_commitment"` \| `"committed_not_disclosed"` \| `"declared"` \| `"none"` | checked against contents |
| `records[]` | role order: unseal `grant, redemption[, unsealed]`; quorum export `grant, redemption, export_receipt`; other exports `export_receipt`. `approvals_json` only on `break_glass_receipts` rows; `pq_*` only when stored | device-signed |
| `salts` | `{plaintext: hex\|null, recipient: hex\|null}` | opening material |
| `tool_version` | `CARGO_PKG_VERSION` | declared |

**Never present:** row ids, `created_at`, chain ordinals, TSA DER or `genTime`, the database, vault or token-file path, host names, a signing `domain`, or the token nonce outside its embedded row.

### The three pairings

- `plaintext_commitment` — the sidecar embeds the device-signed `Unsealed`
  row and the salt that opens its plaintext commitment:
  `SHA256(le32(34) ‖ "securacv:pwk:unsealed-plaintext:v1" ‖ salt ‖ FILE)`.
  The device decrypted the envelope to exactly these bytes.
- `export_receipt` — the file is the export bundle whose signed receipt is
  the embedded export record.
- `declared` — no signed record this sidecar can open binds the file
  (a sidecar recovered with `emit` after the original was lost, or an
  unseal by a build that wrote no `Unsealed` row).

### The four recipient bindings

The recipient is always **declared** by the operator: the device never
checks who received a file. What the commitment does is fix the
declaration at disclosure time, salted —
`SHA256(le32(33) ‖ "securacv:pwk:handoff-recipient:v1" ‖ salt ‖ recipient_cjson)`
— so it cannot be swapped later, and so a ledger holder cannot test
candidate names.

| Binding | Meaning |
|---|---|
| `receipt_commitment` | the sidecar names the recipient and opens the commitment in the signed row |
| `committed_not_disclosed` | a signed row commits to a recipient this sidecar does not name (`redact`, a recovered `emit`, a `court_export`-generated sidecar) |
| `declared` | the sidecar names a recipient no signed row commits to |
| `none` | no recipient |

Prefer `--recipient-file r.json` (`{"name": …, "organization"?: …, "key"?: …}`):
a name typed as a flag lands in shell history. The file's contents are
never echoed in an error, and no salt or recipient name is ever printed.

## Unsealing: the order, and what each error means

`break_glass unseal`:

1. Reads the token and checks its envelope and ruleset; validates the
   recipient (a bad recipient spends nothing).
2. Moves an earlier sidecar at the output path aside (a hidden
   `.<name>.stale-<rand>` beside it), then proves a sidecar can be created
   there (create and remove). The earlier sidecar is restored if anything
   below fails before the new sidecar exists — a spent or wrong token, a
   wrong seed, a console denial — because it holds the only copy of the
   salts that open the earlier unseal record; it is deleted once the new
   unseal succeeds. If the move or the probe fails:
   `refusing to unseal: a stale handoff sidecar at <path> could not be moved aside (<e>); nothing was recorded and the token is not spent`, or
   `refusing to unseal: a handoff sidecar cannot be created at <path> (<e>); nothing was recorded and the token is not spent`.
3. Burns the token and appends the signed `TokenRedeemed` row (unchanged).
4. Decrypts into memory.
5. Appends the device-signed `Unsealed` row (the redemption it completes,
   the salted plaintext commitment, an optional salted recipient
   commitment). If it cannot: `the unseal could not be recorded in the receipt chain (…); the cleartext was discarded and nothing was written. The token is spent — its redemption is recorded — so the trustees must approve a new request to unseal again.`
6. Builds the sidecar and verifies it in memory under the device key.
7. Writes the **sidecar first**. If that fails: `the unseal is recorded (unseal record <h16>…) but its handoff sidecar could not be written to <path> (<e>); the cleartext was discarded and nothing was written, because the salt that opens the signed record exists only in that sidecar. The token is spent — the trustees must approve a new request to unseal again.`
8. Writes the `.raw` (0600). If that fails, the sidecar is removed:
   `the cleartext could not be written to <raw> (<e>); nothing was released and its handoff sidecar was removed. The unseal record stays in the receipt chain (the device did decrypt the envelope). The token is spent — the trustees must approve a new request to unseal again.`

On success:

```
Unsealed envelope written to <raw>
  sha256:          <file sha256> (<n> bytes)
  unseal record:   <h16>… appended to the receipt chain (device-signed; pairs this file to its grant)
  handoff sidecar: <raw>.securacv-handoff.json (mode 0600; hand it over with the file)
  recipient:       committed in the signed unseal record; named only in the handoff sidecar
```

(`recipient:       none named` when no recipient was given.) Hand over the
file and its sidecar together. Then anchor the disclosure record, not the
media digest: `log_anchor … --subject break_glass_receipt_head` right after
the unseal fixes the `Unsealed` row in time without handing a TSA a
fingerprint of the media.

Exports apply the same stale rule (the earlier sidecar comes back if the
export fails before its bundle is written, and is deleted once the new
bundle replaces the old one), then: the receipt is appended, the bundle
written, then the handoff sidecar — before the optional C2PA sidecar, so a
C2PA failure never skips it. A handoff sidecar failure keeps the bundle (it
carries its own signed receipt), exits 1, and prints a secret-free recovery
command (`handoff_sidecar emit --file <bundle> --db <db>`).

Every sidecar is created at its final path with `O_CREAT|O_EXCL` at mode
0600: it never overwrites and never follows a symlink, and it needs no hard
link, so a FAT or exFAT stick or an SMB share takes it. Something already
at the path is refused with `a file already exists at <path>; a handoff sidecar is never written over another file`.

## `handoff_sidecar`

```
handoff_sidecar emit   --file <PATH> [--db witness.db] [--device-key-seed <S> | --db-key <HEX>]
                       [--grant <HEX> | --unseal-record <HEX>] [--out <PATH>] [--anchors-out <DIR>]
                       [--recipient-file <PATH> | --recipient-name <NAME> [--recipient-org <ORG>] [--recipient-key <HEX>]]
handoff_sidecar verify --file <PATH> [--sidecar <PATH>] [--public-key <HEX> | --public-key-file <FILE>]
                       [--db <PATH> [--device-key-seed <S> | --db-key <HEX>]] [--anchors-dir <DIR>] [--ca <PEM>]...
handoff_sidecar redact --sidecar <PATH> --out <PATH>
```

Exit codes: 0 no check failed; 1 a check failed; 2 a usage error, or the
tool could not run (an unreadable file, an unopenable database, a refusal).

- **`emit`** opens the database read-only, consumes nothing, and never
  overwrites (`refusing to overwrite <path>: a handoff sidecar is never refreshed in place — write the new one with --out <PATH>`).
  An export bundle is located in the export chain by its receipt's entry
  hash. An unsealed file is named by `--grant` (the Granted receipt's entry
  hash, from `break_glass receipts`) or `--unseal-record` (printed by
  `break_glass unseal`); selection is by hash only — there is no search by
  name, recipient or time. Because the salts existed only in the original
  sidecar, a recovered unseal sidecar has `pairing: declared`, and recipient
  flags beside a committing row are refused. `--anchors-out <DIR>` writes
  each referenced token as `<DIR>/<token_sha256>.der`.
- **`verify`** prints one line per check (`OK`, `FAIL`, `UNVERIFIED`,
  `DECLARED`) and a verdict in `log_verify`'s vocabulary. `DECLARED` and
  `UNVERIFIED` never fail it. `--public-key-file` pins the device key
  obtained out of band (without it the verdict is
  `SELF-CONSISTENT, IDENTITY UNVERIFIED`). `--db` adds the deep checks:
  the key lineage, every embedded record byte-identical to its row, both
  receipt chains end to end with the grant's quorum re-derived under its
  policy era, PQ signatures, and tokens from `tsa_anchors`.
  `--anchors-dir` resolves tokens by their SHA-256 (a court kit's
  `anchors/` works as is); `--ca <tsa-ca.pem>` checks each token's
  countersignature through the local `openssl`.
- **`redact`** writes a copy without the recipient name and its salt
  (`receipt_commitment` becomes `committed_not_disclosed`), 0600,
  create-new. Use it to forward a sidecar, or before `court_export`, which
  copies a supplied sidecar verbatim.

The lines `verify` prints:

```
handoff sidecar {sidecar} for {file}
This sidecar is unsigned. It carries device-signed records; everything else in it is declared by the tool that wrote it.
OK         format: securacv-handoff:v1, canonical (securacv-cjson-v1)
FAIL       format: not in canonical securacv-cjson-v1 form — the sidecar was edited after it was written (re-emit it; never edit or pretty-print it)
FAIL       format: {unsupported format '{f}' | unknown key or malformed: {serde error} | created_bucket is not a 600 s bucket}
OK         file: sha256 {hex} and {n} bytes match the sidecar
FAIL       file: sha256 {hex} does not match the sidecar's {hex2} — this sidecar does not describe this file
FAIL       device key: the sidecar's records name key {fp16}…, not the key you supplied (after a key rotation, pass --db to check the lineage)
OK         record {role}: entry hash re-derives; device signature checks under {fp16}…
FAIL       record {role}: entry hash does not re-derive from prev_hash and payload_json
FAIL       record {role}: device signature does not check under {fp16}…
FAIL       links: {U-rule or R-rule detail, verbatim}
OK         links: the unseal record names the redemption, and the redemption names the grant
OK         links: the export receipt names the grant, and the grant was redeemed once
OK         consent: the grant's recorded request context re-derives the request hash its {n} trustee(s) approved ({ids})
UNVERIFIED consent: the quorum behind the grant is re-derived against its policy era only with --db
OK         pairing: this file opens the plaintext commitment in the device-signed unseal record
OK         pairing: this file is the export bundle whose receipt is the embedded export record
DECLARED   pairing: no device-signed record this sidecar can open binds this file to the envelope; the tool that wrote the sidecar says it does
FAIL       pairing: this file does not open the plaintext commitment in the signed unseal record — wrong file or wrong salt
OK         recipient: opens the commitment in the signed {unseal record|export receipt} — declared by the operator at disclosure, not verified by the device
OK         recipient: the signed {unseal record|export receipt} commits to a recipient this sidecar does not disclose
DECLARED   recipient: {name}{ ({org})} — named in this sidecar only; no signed record commits to it
OK         recipient: none named
FAIL       recipient: does not open the commitment in the signed {unseal record|export receipt}
FAIL       binding: the sidecar says {key}={value}; its contents support {value2}
OK         anchor (rfc3161, covers {the file bytes|the disclosure record}): token {t16}… found; imprint matches; countersignature checks under {ca}
UNVERIFIED anchor (rfc3161, covers …): token {t16}… found; imprint matches; countersignature not checked (pass --ca <tsa-ca.pem>)
UNVERIFIED anchor (rfc3161, covers …): token {t16}… not supplied (pass --db or --anchors-dir)
FAIL       anchor (rfc3161, covers …): token {t16}… covers {hex16}…, not the {file's sha256|disclosure record's entry hash}
FAIL       anchor (rfc3161, covers …): countersignature does not check under any --ca given ({e})
UNVERIFIED anchor ({kind}): this build does not check that anchor kind
OK         database: every embedded record is a row of this database, both receipt chains verify end to end, the grant's quorum re-derives, and the device key is in its lineage
FAIL       database: record {role} is not in this database's {ledger} (the sidecar did not come from this database, or the row was removed)
FAIL       database: this database's {ledger} does not verify ({e})
OK: handoff sidecar checked — VALID (identity anchored out-of-band). {d} item(s) are declared, not attested by the device.
OK: handoff sidecar checked — SELF-CONSISTENT, IDENTITY UNVERIFIED. The device key came from the sidecar itself; pass --public-key-file <device.pub> obtained out of band for an evidentiary verdict. {d} item(s) are declared, not attested by the device.
FAIL: handoff sidecar verification failed — {n} check(s) failed.
```

## The shell recipe (no SecuraCV software)

`scripts/verify_handoff_sidecar.sh <sidecar> <file> <device-pubkey-hex>`
needs `sha256sum`, `openssl` ≥ 3.0 and `jq` — no `xxd`. It re-derives the
file digest, every record's entry hash and Ed25519 signature (the signing
domain is chosen by the script from `ledger`, never read from the sidecar),
the grant → redemption → unseal record / export joins, the plaintext
pairing and the recipient commitment. It checks Ed25519 only, not PQ, and
not the TSA countersignature (`handoff_sidecar verify --db --ca` does).
The export pairing greps the bundle for the exact signed receipt bytes, so
it holds for bundles as `export_events` writes them (compact JSON); a
bundle re-serialized by another tool needs `handoff_sidecar verify`.

```sh
#!/bin/sh
# Offline check of a SecuraCV handoff sidecar. Needs sha256sum, openssl >= 3.0, jq. No xxd.
set -eu
S=$1; F=$2; PUB=$3   # PUB: the device key you pinned out of band (64 hex) — never the sidecar's own
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
hex2bin() { s=$1; while [ -n "$s" ]; do b=${s%"${s#??}"}; s=${s#??}; printf "\\$(printf %03o "0x$b")"; done; }
HEX='map(("0123456789abcdef"[(./16|floor):(./16|floor)+1])+("0123456789abcdef"[(.%16):(.%16)+1]))|join("")'
fail() { echo "FAIL $*"; exit 1; }
rec() { jq -j --arg r "$1" '.records[]|select(.role==$r)|.payload_json' "$S"; }
eh()  { jq -j --arg r "$1" '.records[]|select(.role==$r)|.entry_hash' "$S"; }
# 1. the sidecar describes this file, under the key you pinned
[ "$(sha256sum "$F" | cut -c1-64)" = "$(jq -j .file.sha256 "$S")" ] || fail "file: sha256 does not match the sidecar"
[ "$(jq -j .device_public_key "$S")" = "$PUB" ] || fail "device key: the sidecar names a different key"
hex2bin "302a300506032b6570032100$PUB" > "$T/k.der"
openssl pkey -pubin -inform DER -in "$T/k.der" -out "$T/k.pem"
# 2. every record: entry hash and device signature (domain chosen by ledger, by this script)
n=$(jq '.records|length' "$S"); i=0
while [ "$i" -lt "$n" ]; do
  r=".records[$i]"; role=$(jq -j "$r.role" "$S")
  case $(jq -j "$r.ledger" "$S") in
    break_glass_receipts) dom='securacv:pwk:break-glass-receipt:v2'; len='\043\000\000\000' ;;
    export_receipts)      dom='securacv:pwk:export-receipt:v2';      len='\036\000\000\000' ;;
    *) fail "$role: unknown ledger" ;;
  esac
  got=$({ hex2bin "$(jq -j "$r.prev_hash" "$S")"; jq -j "$r.payload_json" "$S"; } | sha256sum | cut -c1-64)
  [ "$got" = "$(jq -j "$r.entry_hash" "$S")" ] || fail "$role: entry hash does not re-derive"
  { printf "$len%s" "$dom"; hex2bin "$got"; } | openssl dgst -sha256 -binary > "$T/m"
  hex2bin "$(jq -j "$r.signature" "$S")" > "$T/s"
  openssl pkeyutl -verify -pubin -inkey "$T/k.pem" -rawin -in "$T/m" -sigfile "$T/s" >/dev/null || fail "$role: device signature"
  echo "OK   record $role"; i=$((i+1))
done
# 3. joins and pairing
case $(jq -j .disclosure "$S") in
  unseal)
    [ "$(rec redemption | jq -j ".outcome.TokenRedeemed.receipt_entry_hash|$HEX")" = "$(eh grant)" ] || fail "links: redemption"
    if [ "$(jq -j .pairing "$S")" = plaintext_commitment ]; then
      [ "$(rec unsealed | jq -j ".outcome.Unsealed.redemption_entry_hash|$HEX")" = "$(eh redemption)" ] || fail "links: unseal record"
      got=$({ printf '\042\000\000\000%s' 'securacv:pwk:unsealed-plaintext:v1'; hex2bin "$(jq -j .salts.plaintext "$S")"; cat "$F"; } | sha256sum | cut -c1-64)
      [ "$got" = "$(rec unsealed | jq -j ".outcome.Unsealed.plaintext_commitment|$HEX")" ] || fail "pairing"
      echo "OK   pairing: this file opens the signed plaintext commitment"
      want=$(rec unsealed | jq -j ".outcome.Unsealed.recipient_commitment // empty|$HEX")
    else echo "DECLARED pairing"; want=""; fi ;;
  export)
    grep -qF -- "\"receipt\":$(rec export_receipt),\"prev_hash\":" "$F" || fail "pairing: the signed receipt is not in this bundle"
    a=$(rec export_receipt | jq -j ".authorizing_receipt // empty|$HEX")
    [ -z "$a" ] || [ "$a" = "$(eh grant)" ] || fail "links: authorizing grant"
    echo "OK   pairing: the bundle embeds the signed export receipt"
    want=$(rec export_receipt | jq -j ".context.recipient_commitment // empty|$HEX") ;;
esac
# 4. recipient
if [ "$(jq -j .recipient_binding "$S")" = receipt_commitment ]; then
  got=$({ printf '\041\000\000\000%s' 'securacv:pwk:handoff-recipient:v1'; hex2bin "$(jq -j .salts.recipient "$S")"; jq -cjS .recipient "$S"; } | sha256sum | cut -c1-64)
  [ "$got" = "$want" ] || fail "recipient: does not open the signed commitment"
  echo "OK   recipient: opens the signed commitment (declared by the operator, not verified by the device)"
fi
echo "OK: the records verify under the key you pinned"
```

Hashes inside signed payloads are `[u8; 32]` integer arrays; the jq `HEX`
function converts them. Where GNU coreutils is present, an alternative
decoder is `hex2bin() { printf %s "$1" | tr a-f A-F | basenc --base16 -d; }`.

## What it does not prove — the residuals

| # | Residual | Bound / mitigation |
|---|---|---|
| 1 | The sidecar holds the recipient's plaintext and both salts; whoever holds it learns the recipient and can open both commitments | 0600; meant for that recipient. Forward it with `handoff_sidecar redact` (`committed_not_disclosed`); `court_export` copies a supplied sidecar verbatim, so redact first to keep the name out of a kit |
| 2 | `--recipient-name` lands in shell history | `--recipient-file` is documented first |
| 3 | A lost unseal sidecar makes the signed pairing permanently unopenable (its salt existed only there) | Never lost at write time (sidecar-first, fail-closed). A sidecar deleted later degrades to `pairing: declared` via `emit`; the commitment stays in the ledger, an honest crypto-shred |
| 4 | `file.sha256` in the sidecar is an unsalted media digest | It sits beside the file it hashes. Docs advise anchoring the **disclosure record** (`log_anchor … --subject break_glass_receipt_head` right after the unseal), not the raw file's digest, which would hand a TSA a media fingerprint |
| 5 | The grant row travels whole: purpose, requester context, trustee ids, approval signatures | A signed row cannot be partially disclosed; spec §5 asks the sidecar to name purpose and approvals. Deployments wanting pseudonymous trustees enroll role ids |
| 6 | Every ledger reader sees that an unseal of envelope E happened, in which bucket | Already true via `TokenRedeemed` (#1936); `Unsealed` adds no time and no identity |
| 7 | `device_public_key` is a stable device identifier given to the recipient | Already in every bundle and kit; a disclosure is not telemetry |
| 8 | A recipient commitment binds a declaration; it does not verify who received the file | Rendered "declared … not verified". It stops later substitution only |
| 9 | A device-key holder can forge any row, including `Unsealed`; an `Unsealed` row at the chain tail can be dropped (the row is not required) | The same trust level as every receipt; the anchored `break_glass_receipt_head` sees the drop, and the sidecar holder holds the row (`verify --db` → `FAIL database: record unsealed is not in …`) |
| 10 | Fail-closed costs a trustee ceremony if the append or the sidecar write fails after decrypt | Chosen: no cleartext without its record and its sidecar. The stale check before the burn removes the realistic cause |
| 11 | A decrypt failure after the burn spends the token (pre-existing in #1936) | Documented; a pre-burn check is a follow-up (`docs/design/handoff_sidecars.md`) |
| 12 | Console unseals take no recipient | `recipient_binding: none`; a later `emit` cannot add one beside a commitment, and adds a `declared` one otherwise |
| 13 | Hub API exports get no sidecar | `handoff_sidecar emit --file <bundle>` |
| 14 | Unseals by #1936-era builds have no `Unsealed` row | `emit --grant` yields `pairing: declared` |
| 15 | The shell recipe checks Ed25519 only, not PQ | `handoff_sidecar verify --db` checks PQ |
| 16 | `created_bucket` of a recovery `emit` reveals when recovery happened | 600 s granularity |
| 17 | A lost export sidecar loses the recipient opening | The bundle still carries its signed receipt; the error says so; `emit` gives `committed_not_disclosed` |

## Upgrade order

The `Unsealed` outcome is a new variant of the break-glass receipt. A
reader built before it fails on such a row (`unknown variant`), and a
`witnessd` of that era boots into safe mode. Upgrade `witnessd`,
`break_glass`, `log_verify`, `envelope_verify`, `court_export` and every
observer host **before the first unseal recorded by this build** — the
same rule the token-redemption rows carried. A bundle whose receipt
carries `context` or `authorizing_receipt` (every quorum export from this
build) needs a verifier from this build, including the website's copy of
`viewer/verify_core.js`, which ships in the same release.
