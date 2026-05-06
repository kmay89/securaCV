//! Privacy-critical TimeBucket coarsening tests.
//!
//! `spec/event_contract.md` §3 mandates a 5-minute floor for time buckets so
//! event timestamps cannot be used to reconstruct fine-grained activity. These
//! tests pin the math and the floor so a refactor that loosens either fails
//! loudly in CI.

use witness_kernel::{TimeBucket, MIN_BUCKET_SIZE_S};

#[test]
fn validate_bucket_size_accepts_minimum() {
    TimeBucket::validate_bucket_size(MIN_BUCKET_SIZE_S)
        .expect("MIN_BUCKET_SIZE_S must be a valid bucket size");
}

#[test]
fn validate_bucket_size_rejects_below_minimum() {
    assert!(TimeBucket::validate_bucket_size(MIN_BUCKET_SIZE_S - 1).is_err());
    assert!(TimeBucket::validate_bucket_size(60).is_err());
    assert!(TimeBucket::validate_bucket_size(0).is_err());
}

#[test]
fn min_bucket_size_is_five_minutes() {
    // Pin the spec value. If someone narrows MIN_BUCKET_SIZE_S to "improve UX",
    // this assertion forces them to update the spec deliberately.
    assert_eq!(
        MIN_BUCKET_SIZE_S, 300,
        "spec/event_contract.md §3 requires a 5-minute (300 s) minimum bucket"
    );
}

#[test]
fn coarsen_to_aligns_start_to_bucket_boundary() {
    // 601 seconds into the epoch with a 10-minute bucket should snap to 600.
    let fine = TimeBucket {
        start_epoch_s: 601,
        size_s: 300,
    };
    let coarse = fine.coarsen_to(600).expect("coarsen to 10 minutes");
    assert_eq!(coarse.start_epoch_s, 600);
    assert_eq!(coarse.size_s, 600);
}

#[test]
fn coarsen_to_widens_bucket_size() {
    let fine = TimeBucket {
        start_epoch_s: 1_700_000_300,
        size_s: 300,
    };
    let coarse = fine.coarsen_to(900).expect("coarsen to 15 minutes");
    assert_eq!(coarse.size_s, 900);
    assert!(coarse.start_epoch_s <= fine.start_epoch_s);
    assert_eq!(coarse.start_epoch_s % 900, 0);
}

#[test]
fn coarsen_to_is_idempotent_at_same_size() {
    let bucket = TimeBucket {
        start_epoch_s: 1_700_000_400,
        size_s: 600,
    };
    let again = bucket.coarsen_to(600).expect("idempotent coarsening");
    assert_eq!(again, bucket);
}

#[test]
fn coarsen_to_rejects_below_minimum_bucket() {
    let bucket = TimeBucket {
        start_epoch_s: 1_700_000_000,
        size_s: 600,
    };
    // Attempting to "narrow" to 60 seconds violates the spec floor and must error.
    assert!(bucket.coarsen_to(60).is_err());
    assert!(bucket.coarsen_to(MIN_BUCKET_SIZE_S - 1).is_err());
}

#[test]
fn now_rejects_below_minimum_bucket() {
    assert!(TimeBucket::now(60).is_err());
    assert!(TimeBucket::now(0).is_err());
    TimeBucket::now(MIN_BUCKET_SIZE_S).expect("minimum size accepted");
    TimeBucket::now_10min().expect("ten-minute helper accepted");
}
