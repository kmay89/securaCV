#![no_main]
//! Fuzz a custody share file (text or binary SHR1), then its container copy's header
//! (`src/vault/custody/`). The bytes are whatever a holder or an operator
//! was handed, so a stranger chooses them. The checks live in
//! `witness_kernel::vault::custody::fuzz::share_decode`: no panic, and anything
//! that decodes re-encodes to the bytes it came from.

use libfuzzer_sys::fuzz_target;
use witness_kernel::vault::custody::fuzz::share_decode;

fuzz_target!(|data: &[u8]| {
    share_decode(data);
});
