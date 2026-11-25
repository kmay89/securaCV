#![no_main]
//! Fuzz a custody recipient public file (JSON) and a recipient key file's TEK1 header, refused before any KDF
//! (`src/vault/custody/`). The bytes are whatever a holder or an operator
//! was handed, so a stranger chooses them. The checks live in
//! `witness_kernel::vault::custody::fuzz::recipient_decode`: no panic, and anything
//! that decodes re-encodes to the bytes it came from.

use libfuzzer_sys::fuzz_target;
use witness_kernel::vault::custody::fuzz::recipient_decode;

fuzz_target!(|data: &[u8]| {
    recipient_decode(data);
});
