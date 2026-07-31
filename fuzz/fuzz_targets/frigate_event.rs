#![no_main]
//! Fuzz the Frigate MQTT payload parsers (`src/transport/frigate.rs`).
//!
//! This is the widest-open input in the kernel. The broker is on the LAN, the
//! event topics are not privileged, and MQTT brokers on a home network are
//! routinely unauthenticated — so *any* device on the network can choose these
//! bytes exactly, without needing to compromise anything first. A camera, a
//! smart plug, or a laptop that joined the guest network can publish whatever
//! it likes to the topic the kernel is subscribed to.
//!
//! Both entry points read the same JSON shapes, so both are driven from the
//! same input.

use libfuzzer_sys::fuzz_target;
use witness_kernel::transport::frigate;

fuzz_target!(|data: &[u8]| {
    let _ = frigate::parse_frigate_event(data);
    let _ = frigate::parse_review_event(data);
});
