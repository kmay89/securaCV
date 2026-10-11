//! Unit tests for `CapabilityTokenManager`.
//!
//! `tests/api_capability_tokens.rs` covers the HTTP layer end-to-end. These
//! tests target the manager directly so wrong-token, wrong-bucket, malformed
//! input, rotation and the post-rotation grace each have a focused failure
//! signal.
//!
//! `firmware/scripts/regression_check.sh` only greps for `ct_eq`. These tests
//! enforce the *behavior* the constant-time comparison is meant to guarantee:
//! any byte-difference rejects the token, no matter where it falls.

use witness_kernel::api::{CapabilityTokenManager, CAPABILITY_TOKEN_GRACE_SECS};
use witness_kernel::TimeBucket;

fn fixed_bucket(start_epoch_s: u64) -> TimeBucket {
    TimeBucket {
        start_epoch_s,
        size_s: 600,
    }
}

/// A clock inside `bucket`: its first second.
fn inside(bucket: TimeBucket) -> u64 {
    bucket.start_epoch_s
}

#[test]
fn validate_accepts_current_token_for_current_bucket() {
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let token = mgr.token_hex();
    mgr.validate(&token, bucket, inside(bucket))
        .expect("current token validates");
}

#[test]
fn validate_rejects_token_for_different_bucket() {
    // No rotation has happened, so there is no previous slot for the grace
    // to admit: a bucket mismatch is simply "expired".
    let bucket = fixed_bucket(1_700_000_000);
    let other_bucket = fixed_bucket(1_700_000_600);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let token = mgr.token_hex();
    let err = mgr
        .validate(&token, other_bucket, inside(other_bucket))
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
    assert!(mgr.validate(&forged, bucket, inside(bucket)).is_err());
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
    assert!(mgr.validate(&forged, bucket, inside(bucket)).is_err());
}

#[test]
fn validate_rejects_non_hex_input() {
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let now = inside(bucket);
    assert!(mgr
        .validate("not-hex-not-hex-not-hex", bucket, now)
        .is_err());
    assert!(mgr.validate("zzzz", bucket, now).is_err());
    assert!(mgr.validate("", bucket, now).is_err());
}

#[test]
fn validate_rejects_wrong_length_hex() {
    let bucket = fixed_bucket(1_700_000_000);
    let mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let now = inside(bucket);
    // Valid hex, wrong length (16 bytes instead of 32).
    let too_short = "0".repeat(32);
    assert!(mgr.validate(&too_short, bucket, now).is_err());
    // 64 bytes — too long.
    let too_long = "a".repeat(128);
    assert!(mgr.validate(&too_long, bucket, now).is_err());
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

    // Past the grace, the previously valid token fails against the new
    // bucket, and the previous bucket is no longer current at all.
    let long_after = next_bucket.start_epoch_s + CAPABILITY_TOKEN_GRACE_SECS + 1;
    assert!(mgr.validate(&original, next_bucket, long_after).is_err());
    assert!(mgr.validate(&original, bucket, long_after).is_err());
    // The new token is good for the new bucket.
    mgr.validate(&mgr.token_hex(), next_bucket, inside(next_bucket))
        .expect("rotated token validates");
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

    mgr.validate(&token_before, bucket, inside(bucket))
        .expect("token still valid after no-op rotation");
}

// D4 grace: a client that read the token file just before the boundary
// (the HA coordinator, the bridge) presents the old token on its next poll;
// it is honored until the old bucket's end plus the grace, and not a
// second longer. Bounded by construction: the grace is a constant.
#[test]
fn rotated_token_accepted_within_grace() {
    assert_eq!(CAPABILITY_TOKEN_GRACE_SECS, 90);
    let bucket = fixed_bucket(1_700_000_000);
    let next_bucket = fixed_bucket(1_700_000_600);
    let mut mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let old = mgr.token_hex();
    assert!(mgr.rotate_if_needed(next_bucket).expect("rotate"));

    let previous_end = bucket.start_epoch_s + u64::from(bucket.size_s);
    mgr.validate(&old, next_bucket, previous_end + 89)
        .expect("old token honored 89 s past its bucket");
    mgr.validate(&old, next_bucket, previous_end)
        .expect("old token honored right at the boundary");
    // And the new token is good throughout.
    mgr.validate(&mgr.token_hex(), next_bucket, previous_end + 89)
        .expect("current token validates");
}

#[test]
fn rotated_token_rejected_after_grace() {
    let bucket = fixed_bucket(1_700_000_000);
    let next_bucket = fixed_bucket(1_700_000_600);
    let mut mgr = CapabilityTokenManager::new(bucket).expect("manager");
    let old = mgr.token_hex();
    assert!(mgr.rotate_if_needed(next_bucket).expect("rotate"));

    let previous_end = bucket.start_epoch_s + u64::from(bucket.size_s);
    let err = mgr
        .validate(&old, next_bucket, previous_end + 91)
        .expect_err("old token dead 91 s past its bucket");
    assert!(
        err.to_string().contains("invalid"),
        "unexpected error: {err}"
    );
    assert!(mgr
        .validate(
            &old,
            next_bucket,
            previous_end + CAPABILITY_TOKEN_GRACE_SECS
        )
        .is_err());

    // Two rotations on: only the LAST rotated-out token is ever held, so the
    // one before it is gone even inside what would have been its grace.
    let third = fixed_bucket(1_700_001_200);
    let second = mgr.token_hex();
    assert!(mgr.rotate_if_needed(third).expect("rotate"));
    assert!(mgr
        .validate(&old, third, next_bucket.start_epoch_s + 10)
        .is_err());
    mgr.validate(&second, third, inside(third))
        .expect("the most recent old token has its grace");
}
