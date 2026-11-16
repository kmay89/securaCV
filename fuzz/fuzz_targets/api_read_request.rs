#![no_main]
//! Fuzz the Event API's hand-rolled HTTP request parser (`read_request` in
//! `src/api/mod.rs`).
//!
//! The Event API speaks HTTP/1.1 over a parser written in this repo, not a
//! library. On the default loopback bind only local processes reach it; once
//! the port is published for pairing or a Witness Wall, **any host on the
//! LAN** chooses these bytes, and the parser runs on them before any
//! credential is looked at — the token itself is pulled out of the parsed
//! headers.
//!
//! Beyond not panicking, the entry point asserts the parser's own bounds: a
//! request body is never buffered past the drain bound, and a body a route
//! would accept is exactly its declared `Content-Length` — the header/body
//! boundary is where a hand-written HTTP reader goes wrong.

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    witness_kernel::api::fuzz_read_request(data);
});
