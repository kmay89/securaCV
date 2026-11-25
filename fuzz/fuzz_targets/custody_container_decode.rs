#![no_main]
//! Fuzz a custody container (MKC1) header, bounds only, with no key
//! (`src/vault/custody/`). The bytes are whatever a holder or an operator
//! was handed, so a stranger chooses them. The checks live in
//! `witness_kernel::vault::custody::fuzz::container_decode`: no panic, and anything
//! that decodes re-encodes to the bytes it came from.

use libfuzzer_sys::fuzz_target;
use witness_kernel::vault::custody::fuzz::container_decode;

fuzz_target!(|data: &[u8]| {
    container_decode(data);
});
