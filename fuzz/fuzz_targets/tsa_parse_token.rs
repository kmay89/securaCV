#![no_main]
//! Fuzz the timestamp-token half of `src/tsa.rs`.
//!
//! `parse_token` and `parse_token_imprint` read the same attacker-reachable
//! DER as `parse_response`, one layer in — the imprint path in particular
//! walks down to a fixed 32-byte hash, so a length field that disagrees with
//! reality is exactly the shape of bug worth hunting.
//!
//! The consistency property runs one way only, and the direction matters.
//! `parse_token` succeeding does **not** imply the imprint extracts: a token
//! carrying a SHA-1 imprint parses correctly and is then rejected by the
//! 32-byte `try_into`, which is the intended behavior rather than a bug. What
//! must hold is the converse — if the imprint came out, the token itself
//! parsed, and the bytes handed back are the ones the token actually carries.
//! Asserting the other direction would report a crash on every valid SHA-1
//! token and bury the real findings.

use libfuzzer_sys::fuzz_target;
use witness_kernel::tsa;

fuzz_target!(|data: &[u8]| {
    let token = tsa::parse_token(data);
    let imprint = tsa::parse_token_imprint(data);

    if let Ok(imprint) = imprint {
        let token = token.expect(
            "parse_token_imprint returned a digest for bytes parse_token rejected — \
             the imprint path reached a token the token path cannot read",
        );
        assert_eq!(
            token.imprint.as_slice(),
            imprint.as_slice(),
            "the extracted imprint is not the one the parsed token carries"
        );
    }
});
