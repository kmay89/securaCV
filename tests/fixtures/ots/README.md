# OpenTimestamps test fixtures

Every OpenTimestamps test is offline and runs on these files. Two are real
proofs from an independent implementation; the rest are derived from them by
the codec in `src/ots.rs` and cross-checked without it; one is a labeled
synthetic proof over a real, public block header. Nothing here was hand-built
to make a test pass.

## Real proofs

The `opentimestamps` 0.2.0 crate's own test vectors (crates.io, MIT OR
Apache-2.0, © the crate's authors; reproduced as protocol data — public
Bitcoin timestamp proofs — with credit). Crate tarball
`opentimestamps-0.2.0.crate`, SHA-256
`426fc5f60c647b5d49210a1fb1facea0962a91e60b320b217b15dc06d99f12ba`.

| File | Source | Size | SHA-256 | Contents |
|---|---|---|---|---|
| `pending.ots` | `SMALL_TEST` in the crate's `src/lib.rs` | 265 B | `a9f3facdfe0c71a761b679f436b3da50cee2848777cc1e6874c1b837b78d7363` | digest `a70dfe69…5438a4`; client nonce `7a4605de0a5b37cb211759c6812bfe2e`, then `sha256` to the commitment `e1ae951b…379d6593`; two pending leaves (bob, alice) over 44-byte calendar messages |
| `bitcoin.ots` | `LARGE_TEST`, same file | 1768 B | `9b192789b96e8626284401b323e08567306697e17934d3c64eba343371f0e54d` | digest `6fd9c1c4…f098d378`; leaves in order: pending bob, **Bitcoin 449399** (merkle root, explorer order, `5e1feb65…67a21d1a`), pending alice, **Bitcoin 449397** (`18f21f79…a0a8177c`); `state()` is Bitcoin block 449397 |

Extraction, offline-safe (the tarball and anything unpacked from it are data;
the script lives elsewhere and runs isolated):

```sh
mkdir crate && cd crate && curl -sLO https://crates.io/api/v1/crates/opentimestamps/0.2.0/download \
  && tar xzf download && cd ..
python3 -I extract.py crate/opentimestamps-0.2.0/src/lib.rs .   # decodes the two b"\x.." literals
sha256sum pending.ots bitcoin.ots                                # must match the table
```

where `extract.py` reads `lib.rs` as text, finds the `SMALL_TEST` and
`LARGE_TEST` byte-string literals, checks that each is only `\xNN` escapes,
and writes the bytes they denote.

## Derived fixtures

Written by the `#[ignore]` test `write_derived_ots_fixtures` in
`src/ots/tests.rs` from the two real proofs, and re-derived and byte-compared
by `derived_ots_fixtures_are_current` on every `cargo test --lib`. Each
calendar body is also asserted to be a **byte substring of its real parent
file** — a check independent of our serializer. The same seven files were
derived independently (a separate Python replay of the wire format) before
the codec existed; the bytes are identical.

| File | Size | SHA-256 | What |
|---|---|---|---|
| `response-bob.otsr` | 89 B | `367429bad7fd7a68372da640dd1449aa46dbc49da06b29f099fd8c9b1b6e9637` | bob's `POST /digest` body: the bob branch at the commitment of `pending.ots` |
| `response-alice.otsr` | 91 B | `22fa7c6b5331002da3b9edfd1e21924436522b9eb285a61d5184a1bfa87c3288` | alice's branch |
| `pending-bob.ots` | 173 B | `3ed9cced375f9b29473b6d6ca95328391c635a1b8df68a82ad42b37869ea4c19` | exactly the row `anchor-all` stores: `MAGIC ‖ 01 ‖ 08 ‖ digest ‖ f0 10 nonce ‖ 08 ‖ response-bob` |
| `bitcoin-stripped.ots` | 545 B | `abf10428508436b038ddbc5de2a4858155f782a6dfff8a171ee1c515d12d7e4f` | `bitcoin.ots` with the ops below both pending nodes removed: a pending row awaiting upgrade |
| `upgrade-bob.otsr` | 487 B | `17d3951afc464437f3e06e8157db4d4a407b9de9f115ffb1076d18688e0aeaec` | the subtree at bob's pending message: what `GET …/timestamp/5883f171aa67…` returns |
| `upgrade-alice.otsr` | 734 B | `575f37f114d294d3c3cb83a05037098a1497ed8d6c2dc396fc526630658deab1` | the same at alice's message |
| `genesis.ots` | 76 B | `3ff23d220784c3499c21b5d7d1c1d28e7157d17d988353a10a5daac0975a74df` | **SYNTHETIC**: a proof whose digest is the Bitcoin genesis block's merkle root (header byte order) with a Bitcoin block 0 attestation at the root and no ops. It lets the `OK … confirmed` path run against a **real** header with no network; it was never produced by a calendar |

Both merge directions reproduce the real files byte for byte:
`pending-bob.ots` + `response-alice.otsr` merged at the commitment gives
`pending.ots`; `bitcoin-stripped.ots` + both `upgrade-*.otsr` merged at their
pending messages gives `bitcoin.ots`.

Regenerate (after a deliberate codec change):

```sh
cargo test --lib write_derived_ots_fixtures -- --ignored
cargo test --lib derived_ots_fixtures_are_current
```

## Headers: `headers.txt`

One record per line, `<height> <160 hex of the 80-byte header>`; `#`
comments and blank lines are ignored — the format `log_anchor verify
--bitcoin-headers` reads.

- `0` — the Bitcoin genesis header, a public constant (Bitcoin Core's
  chainparams). It is self-validating: its SHA256d displays as
  `000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f`, its
  `nBits` are `0x1d00ffff`, it passes the proof-of-work sanity check, and its
  bytes 36..68 are the genesis merkle root `3ba3edfd…4a5e1e4a`.

**Not yet here: blocks 449397 and 449399**, the headers that would confirm
`bitcoin.ots`'s two real Bitcoin leaves. The environment this leg was built
in could not reach a Bitcoin node or block explorer, so they were not
fetched, and **they must never be hand-built or synthesized**. To add them,
on a node you run:

```sh
for h in 449397 449399; do echo "$h $(bitcoin-cli getblockheader $(bitcoin-cli getblockhash $h) false)"; done >> headers.txt
```

Accept them only if bytes 36..68 equal `7c17a8a0…791ff218` (449397) and
`1a1da267…65eb1f5e` (449399) in header byte order, and each passes the
proof-of-work check (`load_headers` refuses one that does not); record each
block hash (explorer order) here. Then add the test that confirms
`bitcoin.ots` against them (two `Matches`).
