#![no_main]
//! Fuzz the module-runtime candidate-event parser.
//!
//! `parse_event_payload` is the boundary where a *module* — third-party code
//! running against the adapter contract — hands the kernel something it claims
//! is an event. The parser's job is to reject anything that isn't exactly the
//! declared shape, including unknown extra fields, so it is deliberately
//! strict; this target's job is to make sure "strict" never means "panics on
//! the way to saying no."
//!
//! Note the two-stage input: raw bytes become a `serde_json::Value` first, so
//! the fuzzer explores JSON structure rather than byte soup. That is what the
//! parser actually sees at runtime.

use libfuzzer_sys::fuzz_target;
use serde_json::Value;
use witness_kernel::module_runtime::event_payload::parse_event_payload;

fuzz_target!(|data: &[u8]| {
    let Ok(value) = serde_json::from_slice::<Value>(data) else {
        return;
    };
    let _ = parse_event_payload(&value);
});
