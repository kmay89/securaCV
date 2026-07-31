#![no_main]
//! Fuzz the vault's additional-authenticated-data header parser.
//!
//! `decode_aad` reads a `u32` length from the input and then takes a slice of
//! that length from the same input — the exact pattern where an attacker-chosen
//! length either panics on the slice or silently reinterprets neighboring
//! bytes. The AAD binds an envelope to its ruleset, so a parser that can be
//! steered into reading the wrong window is a parser that can be steered into
//! binding the wrong ruleset.

use libfuzzer_sys::fuzz_target;
use witness_kernel::vault::crypto::decode_aad;

fuzz_target!(|data: &[u8]| {
    let _ = decode_aad(data);
});
