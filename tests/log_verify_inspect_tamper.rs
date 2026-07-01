//! Regression gate: `log_verify --lineage` / `--checkpoints` exist to
//! diagnose corrupted or tampered databases — they must never panic on the
//! very data they exist to inspect. A short/garbage key BLOB in
//! `device_key_history.public_key` or `checkpoints.signer_public_key`
//! previously panicked the printers' raw `[..16]` hex slices.

use std::process::Command;
use std::time::Duration;

use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, ZonePolicy,
};

const SEED: &str = "devkey:inspect_tamper:00112233445566778899";

fn module_desc() -> ModuleDescriptor {
    ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    }
}

fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str) {
    kernel
        .append_event_checked(
            &module_desc(),
            CandidateEvent {
                event_type: EventType::BoundaryCrossingObjectLarge,
                time_bucket: TimeBucket {
                    start_epoch_s: 600,
                    size_s: 600,
                },
                zone_id: zone.to_string(),
                confidence: 0.5,
                correlation_token: None,
            },
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )
        .expect("seal event");
}

/// Build an on-disk witness.db with a rotated lineage and a checkpoint, then
/// truncate the recorded key BLOBs the way a tamperer (or disk corruption)
/// could: epoch 2's public key and the checkpoint's signer key both become a
/// 2-byte blob.
fn build_tampered_db(db_path: &str) {
    let cfg = KernelConfig {
        db_path: db_path.to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(3600),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    };
    let mut kernel = Kernel::open(&cfg).expect("open kernel");
    seal_event(&mut kernel, &cfg, "zone:a");
    kernel
        .enforce_retention_with_checkpoint(Duration::from_secs(0))
        .expect("checkpoint");
    kernel
        .rotate_device_identity("devkey:inspect_tamper:1111111111111111")
        .expect("rotate");
    seal_event(&mut kernel, &cfg, "zone:b");

    let changed = kernel
        .conn
        .execute(
            "UPDATE device_key_history SET public_key = x'AABB' \
             WHERE epoch = (SELECT MAX(epoch) FROM device_key_history)",
            [],
        )
        .expect("tamper epoch key");
    assert_eq!(changed, 1, "expected to truncate exactly one epoch key");
    let changed = kernel
        .conn
        .execute("UPDATE checkpoints SET signer_public_key = x'AABB'", [])
        .expect("tamper checkpoint signer");
    assert_eq!(
        changed, 1,
        "expected to truncate exactly one checkpoint signer"
    );
    // kernel dropped -> connection closed, db flushed to disk
}

fn run_log_verify(db_path: &str, flag: &str) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", db_path, flag])
        .env("DEVICE_KEY_SEED", SEED)
        .output()
        .expect("spawn log_verify")
}

#[test]
fn lineage_inspector_survives_short_epoch_key() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let db_path = db_path.to_str().expect("utf8 path");
    build_tampered_db(db_path);

    let output = run_log_verify(db_path, "--lineage");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    assert!(
        !stderr.contains("panicked"),
        "--lineage panicked on a short epoch key blob\n--- stderr ---\n{stderr}"
    );
    // The inspector's whole point: keep walking and label the damage.
    assert!(
        stdout.contains("INVALID") || stdout.contains("BROKEN"),
        "--lineage must still label the tampered epoch\n--- stdout ---\n{stdout}"
    );
}

#[test]
fn checkpoint_inspector_survives_short_signer_key() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let db_path = db_path.to_str().expect("utf8 path");
    build_tampered_db(db_path);

    let output = run_log_verify(db_path, "--checkpoints");
    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    assert!(
        !stderr.contains("panicked"),
        "--checkpoints panicked on a short signer key blob\n--- stderr ---\n{stderr}"
    );
    // The unresolvable signer must still be clearly marked untrusted.
    assert!(
        stdout.contains("UNTRUSTED key"),
        "--checkpoints must still mark the unresolvable signer\n--- stdout ---\n{stdout}"
    );
}
