//! Unit tests for `CapabilityTokenManager`.
//!
//! `tests/api_capability_tokens.rs` covers the HTTP layer end-to-end. These
//! tests target the manager directly so wrong-token, wrong-bucket, malformed
//! input, and rotation paths each have a focused failure signal.
//!
//! `firmware/scripts/regression_check.sh` only greps for `ct_eq`. These tests
//! enforce the *behavior* the constant-time comparison is meant to guarantee:
//! any byte-difference rejects the token, no matter where it falls.

use witness_kernel::api::CapabilityTokenManager;
use witness_kernel::TimeBucket;

fn fixed_bucket(start_epoch_s: u64) -> TimeBucket {
    TimeBucket {
        start_epoch_s,
        size_s: 600,
    }
}

#[test]
fn validate_accepts_current_token_for_current_bucket() {
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let token = mgr.token_hex();
    mgr.validate(&token, bucket)
        .expect("current token validates");
}

#[test]
fn validate_rejects_token_for_different_bucket() {
    let bucket = fixed_bucket(1_700_000_000);
    let other_bucket = fixed_bucket(1_700_000_600);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let token = mgr.token_hex();
    let err = mgr
        .validate(&token, other_bucket)
        .expect_err("different bucket must fail");
    assert!(
        err.to_string().contains("expired"),
        "unexpected error: {err}"
    );
}

#[test]
fn validate_rejects_first_byte_difference() {
    // The constant-time comparison must reject *any* differing byte. Flipping
    // just the first byte simulates an attacker probing one position at a time.
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let mut bytes = hex::decode(mgr.token_hex()).expect("hex");
    bytes[0] ^= 0x01;
    let forged = hex::encode(bytes);
    assert!(mgr.validate(&forged, bucket).is_err());
}

#[test]
fn validate_rejects_last_byte_difference() {
    // Mirror of the above for the trailing byte. With a non-constant-time
    // comparator the timing of this rejection would differ from the first-byte
    // case; the test exists to keep that surface area covered behaviorally.
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let mut bytes = hex::decode(mgr.token_hex()).expect("hex");
    let last = bytes.len() - 1;
    bytes[last] ^= 0x80;
    let forged = hex::encode(bytes);
    assert!(mgr.validate(&forged, bucket).is_err());
}

#[test]
fn validate_rejects_non_hex_input() {
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    assert!(mgr.validate("not-hex-not-hex-not-hex", bucket).is_err());
    assert!(mgr.validate("zzzz", bucket).is_err());
    assert!(mgr.validate("", bucket).is_err());
}

#[test]
fn validate_rejects_wrong_length_hex() {
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    // Valid hex, wrong length (16 bytes instead of 32).
    let too_short = "0".repeat(32);
    assert!(mgr.validate(&too_short, bucket).is_err());
    // 64 bytes — too long.
    let too_long = "a".repeat(128);
    assert!(mgr.validate(&too_long, bucket).is_err());
}

#[test]
fn rotate_changes_token_when_bucket_changes() {
    let bucket = fixed_bucket(1_700_000_000);
    let next_bucket = fixed_bucket(1_700_000_600);
    let mut mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let original = mgr.token_hex();

    let rotated = mgr.rotate_if_needed(next_bucket).expect("rotate");
    assert!(
        rotated,
        "rotate_if_needed must return true on bucket change"
    );
    assert_ne!(
        mgr.token_hex(),
        original,
        "token bytes must change on rotation"
    );

    // The previously valid token must now fail against the new bucket.
    assert!(mgr.validate(&original, next_bucket).is_err());
    // And the previous bucket is no longer current at all.
    assert!(mgr.validate(&original, bucket).is_err());
}

#[test]
fn rotate_is_noop_for_same_bucket() {
    let bucket = fixed_bucket(1_700_000_000);
    let mut mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let token_before = mgr.token_hex();

    let rotated = mgr.rotate_if_needed(bucket).expect("rotate");
    assert!(
        !rotated,
        "rotate_if_needed must return false when bucket unchanged"
    );
    assert_eq!(mgr.token_hex(), token_before, "token must not change");

    mgr.validate(&token_before, bucket)
        .expect("token still valid after no-op rotation");
}
