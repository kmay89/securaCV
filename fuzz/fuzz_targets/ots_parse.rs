#![no_main]
//! Fuzz the OpenTimestamps codec in `src/ots.rs`.
//!
//! `Proof::parse` reads stored `ots_anchors` proofs inside `witnessd` at
//! boot (`run_full_verify`) and in every `log_anchor` verb; `parse_timestamp`
//! reads calendar bodies off the network and out of `--ots-dir`. Both must
//! never panic and must stay within their bounds, and whatever parses must
//! re-serialize to bytes that parse to the same tree (canonical form is a
//! fixed point).

use libfuzzer_sys::fuzz_target;
use witness_kernel::ots;

fuzz_target!(|data: &[u8]| {
    if let Ok(proof) = ots::Proof::parse(data) {
        let bytes = proof.to_bytes();
        let again = ots::Proof::parse(&bytes).expect("a canonical re-serialization parses");
        assert_eq!(again, proof, "canonical form is a fixed point");
        let _ = proof.state();
        let _ = ots::check_bitcoin_leaves(&proof, None);
    }
    if let Ok(node) = ots::parse_timestamp(data, &[0u8; 32]) {
        let bytes = ots::serialize_node(&node);
        let again = ots::parse_timestamp(&bytes, &[0u8; 32]).expect("re-serialization parses");
        assert_eq!(again, node);
    }
});
