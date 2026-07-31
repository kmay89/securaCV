#![no_main]
//! Fuzz canonical JSON — the byte string signatures are computed over.
//!
//! This target is not really hunting for a panic. Canonicalization sits
//! directly under the evidence-envelope digest (`spec/evidence_envelope.md`
//! §9), so the bugs that matter here are *semantic*: if the same value can
//! canonicalize two different ways, or if canonical output means something
//! different when read back, then a signature over one meaning can be
//! presented as a signature over another. A crash-only fuzzer sails straight
//! past that class of defect, so the properties are asserted explicitly.
//!
//! Three properties, each of which would be a signature-correctness bug:
//!
//!   1. **Deterministic** — the same value canonicalizes to the same bytes
//!      every time. (Map iteration order is the usual way this breaks.)
//!   2. **Meaning-preserving** — canonical bytes re-parse to an equal value.
//!      If they don't, the digest covers something other than what was signed.
//!   3. **Idempotent** — canonicalizing the re-parsed value reproduces the
//!      same bytes. This is what lets a second implementation (the JavaScript
//!      verifier in `viewer/verify_core.js`) reach the same digest.
//!
//! The round trip is exact by construction: the scheme rejects floats and
//! caps integers at ±(2^53-1), so nothing survives canonicalization that
//! could lose precision on the way back.

use libfuzzer_sys::fuzz_target;
use serde_json::Value;
use witness_kernel::canonical_json::to_canonical_bytes;

fuzz_target!(|data: &[u8]| {
    let Ok(value) = serde_json::from_slice::<Value>(data) else {
        return;
    };

    // Rejection is a valid outcome (floats, oversized integers) and is not
    // what this target is testing.
    let Ok(canonical) = to_canonical_bytes(&value) else {
        return;
    };

    // 1. Deterministic.
    let again = to_canonical_bytes(&value).expect("canonicalization failed on its second run");
    assert_eq!(
        canonical, again,
        "canonicalization is not deterministic — the same value produced two different \
         byte strings, so two signers would sign different bytes for the same evidence"
    );

    // 2. Meaning-preserving.
    let reparsed: Value = serde_json::from_slice(&canonical)
        .expect("canonical output is not valid JSON");
    assert_eq!(
        value, reparsed,
        "canonical output does not re-parse to the value it came from — the digest \
         would cover something other than what was signed"
    );

    // 3. Idempotent.
    let recanonical =
        to_canonical_bytes(&reparsed).expect("re-canonicalizing canonical output failed");
    assert_eq!(
        canonical, recanonical,
        "canonicalization is not idempotent — an independent verifier reading these \
         bytes back would compute a different digest"
    );
});
