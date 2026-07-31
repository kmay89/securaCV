# Fuzzing the parsers that eat bytes we didn't write

Every target here wraps a function that parses **input an attacker can
choose**. That is the whole selection rule: if the bytes come from a remote
service, a broker any LAN device can publish to, or a file on a card someone
else could have touched, it belongs in this directory. If the input only ever
comes from our own code, it doesn't — a fuzz target over trusted input burns
CI time and finds nothing.

The repo already had good *static* assurance (CodeQL across five languages,
`cargo-audit`, `cargo-deny`, the firmware route-auth lint). This is the first
piece of **dynamic** assurance: nothing else in CI runs the parsers against
input designed to break them.

## The targets, and why each one is here

| Target | Parses | Who controls the bytes |
|---|---|---|
| `tsa_parse_response` | RFC-3161 `TimeStampResp`, via the **hand-rolled DER reader** in `src/tsa.rs` | A remote TSA, or anyone who can MITM it. Hand-written DER over network bytes is the highest-risk parser in the tree. |
| `tsa_parse_token` | The `ContentInfo` token and its imprint | Same, one layer in |
| `vault_envelope_decode` | The sealed-vault binary container (`src/vault/format.rs`) | Whoever last held the SD card — precisely the adversary the vault exists for |
| `vault_decode_aad` | The vault's additional-authenticated-data header | Same |
| `frigate_event` | Frigate MQTT event/review payloads | **Any device on the LAN** that can publish to the broker topic |
| `canonical_json` | JSON → canonical byte form (`src/canonical_json.rs`) | Anything that reaches a signing path — see the property note below |
| `event_payload` | Module-runtime candidate events | Adapter/module output, i.e. third-party code |

## Two targets check more than "it didn't crash"

Absence of a panic is a weak property. Where a stronger one exists, the target
asserts it:

- **`canonical_json` asserts determinism and stability.** Canonicalization sits
  under signatures, so a canonicalizer that emits two different byte strings
  for the same value — or one that changes the value on a round trip — is a
  signature-correctness bug, not a crash. Two structurally equal inputs must
  canonicalize identically, and the output must re-parse to an equal value.
  This class of bug is invisible to a crash-only fuzzer and is exactly the kind
  that turns into "attacker gets a valid signature over a different meaning."
- **`vault_envelope_decode` asserts re-encode stability.** Anything that
  decodes must re-encode to bytes that decode the same way again. A decoder
  that accepts a container it cannot faithfully reproduce is one that disagrees
  with its own writer.

## Running them

```sh
sudo apt-get install libseccomp-dev   # or the equivalent — see below
cargo install cargo-fuzz              # once
rustup toolchain install nightly      # libFuzzer needs nightly

cargo +nightly fuzz list
cargo +nightly fuzz run tsa_parse_response
cargo +nightly fuzz run tsa_parse_response -- -max_total_time=60   # what CI does
```

**If the link step fails with `unable to find library -lseccomp`**, that's the
missing `libseccomp-dev` above — the kernel links libseccomp on Linux for
`witnessd`'s syscall filter, so every target compiles happily and then dies at
the very last step. Nothing is wrong with the targets.

`fuzz/seeds/<target>/` holds committed starting inputs — real DER, a real
vault header, a real Frigate payload. Seeds matter more than fuzzer time here:
a structured format like DER is nearly unreachable from random bytes, so
without a valid seed the fuzzer spends its whole budget failing the first
length check and reports a confident green having tested almost nothing.

## When a target finds something

`cargo-fuzz` writes the input to `fuzz/artifacts/<target>/`. Commit it to
`fuzz/seeds/<target>/` as a regression seed in the same PR as the fix — CI
replays every seed on every run, so a fixed bug stays fixed.

## CI

`.github/workflows/fuzz.yml` runs each target for a bounded time on a schedule
and on changes to the parsers, and replays the committed seed corpus on every
PR. The PR-time job is the fast one: it is a regression gate, not a hunt.
