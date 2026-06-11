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
  signal.

SecuraCV's no-outbound-network principle is preserved: nothing in
`witnessd` calls a TSA. Anchoring happens only when an operator (or an
operator's cron job) runs `log_anchor`, and an offline flow exists for
air-gapped deployments.

## Anchoring the chain head

Online (build with `--features tsa`):

```sh
log_anchor request --db witness.db --url https://freetsa.org/tsr
```

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
chain history, so it is safe to import long after the query was written,
even though the chain has moved on.

Public TSAs (no account needed): `https://freetsa.org/tsr`,
`http://timestamp.digicert.com` (`--allow-http`; the token's own signature
makes transport integrity non-critical), `http://zeitstempel.dfn.de`.
Anchoring the same head at **two independent TSAs** removes the single
point of trust.

## Anchoring an export

Exports can be anchored the same way: `envelope_verify` prints the
envelope's whole-envelope SHA-256 digest; pass it explicitly:

```sh
log_anchor request --db witness.db --url https://freetsa.org/tsr \
    --digest <whole_envelope_digest_hex>
```

The anchor is stored with subject `digest`, tying the handed-over artifact
itself — not just the chain it came from — to a point in time.

## Verifying anchors

```sh
log_anchor verify --db witness.db --ca tsa-ca.pem
```

Per anchor this checks, in order of increasing externality:

1. **Imprint consistency** — the stored token actually covers the hash the
   anchor row claims (parsed by SecuraCV's own RFC 3161 reader).
2. **Chain membership** — that hash is a recorded sealed-event hash or
   checkpoint head in this database. A tampered or regenerated chain fails
   here: its history no longer contains the anchored state.
3. **The TSA countersignature** (with `--ca`) — delegated to an
   *independent implementation*, `openssl ts -verify`, the same
   second-implementation stance as the dual Rust/JS envelope verifiers.
   Without `--ca` the structural checks still run and the exact openssl
   command is printed for out-of-band verification.

A verifier needs only the DB (or the exported token), the TSA's public CA
certificate, and openssl — no SecuraCV toolchain is required for the
trust-critical step.

## Honest limits

- An anchor proves the chain head existed **at or before** the token's
  `genTime`. It cannot prove events did *not* exist earlier, and it says
  nothing about history recorded **after** the last anchor — anchor on a
  schedule to keep that window small.
- Trust shifts from the device clock to the **TSA's** key and clock.
  Reputable TSAs run audited HSM infrastructure, and anchoring at two
  independent authorities makes the residual risk multiplicative.
- Tokens embed the TSA's certificate (`certReq` is always set), so they
  remain verifiable after the TSA rotates keys — but verify against the
  TSA's published CA, not the embedded leaf alone.
- The anchors table is auxiliary evidence *about* the chain; it is not
  itself hash-chained. Deleting an anchor destroys proof, it never forges
  any — the protection is the usual one for backups: export tokens
  (`tsa_anchors.token_der`) alongside your DB backups.
