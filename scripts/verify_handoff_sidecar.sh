#!/bin/sh
# Offline check of a SecuraCV handoff sidecar. Needs sha256sum, openssl >= 3.0, jq. No xxd.
#
#   scripts/verify_handoff_sidecar.sh <sidecar> <file> <device-pubkey-hex>
#
# PUB is the device key you pinned out of band (64 hex) — never the sidecar's
# own. Checks the file digest, every embedded record's entry hash and Ed25519
# signature (the signing domain is chosen here, by ledger, never read from the
# sidecar), the grant -> redemption -> unseal-record / export joins, the
# plaintext pairing and the recipient commitment. It does not check PQ
# signatures, the quorum behind the grant, or TSA countersignatures:
# `handoff_sidecar verify --db ... --ca ...` does. docs/handoff_sidecars.md.
#
# Alternative hex decoder, where GNU coreutils is present:
#   hex2bin() { printf %s "$1" | tr a-f A-F | basenc --base16 -d; }
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
