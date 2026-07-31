#![no_main]
//! Fuzz the sealed-vault container decoder (`src/vault/format.rs`).
//!
//! These bytes come off the SD card, and `unseal()` decodes them **before**
//! any break-glass check runs — so the adversary the vault exists to defend
//! against (someone who got hold of the card) is the one choosing this input.
//! Both container versions are length-prefixed binary: V1 reads a `u32` length
//! and then a slice of that length, V2 adds a magic and a version byte. Length
//! fields read from the input and used to slice the input are the classic
//! shape of the bug this target is looking for.
//!
//! Beyond not panicking, a decoder must agree with its own writer: anything
//! that decodes has to re-encode to bytes that decode again to the same
//! container. A decoder that accepts something it cannot reproduce has quietly
//! become a second, undocumented format.

use libfuzzer_sys::fuzz_target;
use witness_kernel::vault::format::VaultEnvelope;

fuzz_target!(|data: &[u8]| {
    let Ok(envelope) = VaultEnvelope::decode(data) else {
        return;
    };

    // Re-encoding may legitimately fail (an oversized field that decode
    // tolerated), but if it succeeds the result must be self-consistent.
    let Ok(reencoded) = envelope.encode() else {
        return;
    };

    let again = VaultEnvelope::decode(&reencoded)
        .expect("a container we encoded ourselves failed to decode");

    let again_encoded = again
        .encode()
        .expect("re-encoding a round-tripped container failed");

    assert_eq!(
        reencoded, again_encoded,
        "decode/encode is not stable — the container does not round-trip to itself"
    );
});
