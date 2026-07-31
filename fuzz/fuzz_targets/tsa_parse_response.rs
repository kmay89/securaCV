#![no_main]
//! Fuzz the hand-rolled DER reader in `src/tsa.rs`.
//!
//! `parse_response` walks a DER `TimeStampResp` that arrived over the network
//! from a Time Stamping Authority. Two things make it the highest-risk parser
//! in the tree: the DER reader is written by hand rather than delegated to a
//! vetted ASN.1 crate, and the bytes belong to a remote party we do not
//! control (or to anyone positioned to answer in its place).
//!
//! The property is simply that it never panics: every malformed input must
//! come back as an `Err`, not an index-out-of-bounds, a capacity overflow, or
//! an arithmetic overflow on a length field.

use libfuzzer_sys::fuzz_target;
use witness_kernel::tsa;

fuzz_target!(|data: &[u8]| {
    // A parse failure is the expected outcome for almost every input; we are
    // asserting *how* it fails, which libFuzzer checks by catching the abort.
    let _ = tsa::parse_response(data);
});
