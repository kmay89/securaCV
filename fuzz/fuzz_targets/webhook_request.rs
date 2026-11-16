#![no_main]
//! Fuzz the webhook adapter's HTTP listener (`handle_connection` in
//! `src/adapter/webhook.rs`).
//!
//! The webhook is the one ingress built to take requests from devices we did
//! not write: anything that can open a TCP connection to the listener chooses
//! these bytes, and the request line, the headers and the body length are all
//! read by a hand-written parser before authentication decides anything.
//!
//! The entry point serves the input as one whole connection, twice. With no
//! authentication, anything forwarded to the adapter must respect the
//! listener's bounds (body size, a path without its query string). Under
//! replay-protected HMAC nothing may be forwarded at all: the fuzzer does not
//! hold the secret, so a forwarded message would be an authentication bypass,
//! not a parse quirk.

use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    witness_kernel::adapter::webhook::fuzz_handle_connection(data);
});
