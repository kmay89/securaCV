//! Hold the kernel to the post-quantum material in
//! `tests/fixtures/pqcrypto_kat/`, written while it linked `pqcrypto-mldsa`
//! 0.1.2 (the README there says how): a device keeps the ML-DSA-44 key its
//! database holds, and a log it signed with that key keeps verifying in
//! strict mode. These tests passed unchanged on `pqcrypto-mldsa` before
//! `ml-dsa` replaced it.
//!
//! The byte-level checks on the key and signature encodings are unit tests in
//! `src/crypto/signatures.rs` (`pqcrypto_kat`); the vault's are in
//! `src/vault/crypto.rs`.

#![cfg(feature = "pqc-signatures")]

use std::process::Command;
use std::time::Duration;

use witness_kernel::crypto::signatures::{PqSignature, SignatureMode, SignatureSet};
use witness_kernel::{
    envelope, verify_envelope, CandidateEvent, EventType, EvidenceEnvelope, InferenceBackend,
    Kernel, KernelConfig, ModuleDescriptor, TimeBucket, ZonePolicy,
};

const ENVELOPE_PATH: &str = "tests/fixtures/pqcrypto_kat/hybrid_envelope.json";
const MLDSA_KAT_PATH: &str = "tests/fixtures/pqcrypto_kat/mldsa44_kat.json";
const SEED: &str = "devkey:pqcrypto-kat:0123456789abcdef0123";

fn config(db_path: &str) -> KernelConfig {
    KernelConfig {
        db_path: db_path.to_string(),
        ruleset_id: "ruleset:pqcrypto-kat".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:pqcrypto-kat"),
        kernel_version: "pqcrypto-kat-0".to_string(),
        retention: Duration::from_secs(3600),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn seal_event(kernel: &mut Kernel, cfg: &KernelConfig, zone: &str, bucket_start: u64) {
    let desc = ModuleDescriptor {
        id: "pqcrypto_kat_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    kernel
        .append_event_checked(
            &desc,
            CandidateEvent {
                event_type: EventType::BoundaryCrossingObjectLarge,
                time_bucket: TimeBucket {
                    start_epoch_s: bucket_start,
                    size_s: 600,
                },
                zone_id: zone.to_string(),
                confidence: 0.75,
                correlation_token: None,
                attestation: None,
            },
            &cfg.kernel_version,
            &cfg.ruleset_id,
            cfg.ruleset_hash,
        )
        .expect("seal event");
}

fn load_envelope() -> EvidenceEnvelope {
    let bytes =
        std::fs::read(ENVELOPE_PATH).unwrap_or_else(|e| panic!("read {ENVELOPE_PATH}: {e}"));
    serde_json::from_slice(&bytes).unwrap_or_else(|e| panic!("parse {ENVELOPE_PATH}: {e}"))
}

fn kat_hex(name: &str) -> Vec<u8> {
    let text = std::fs::read_to_string(MLDSA_KAT_PATH)
        .unwrap_or_else(|e| panic!("read {MLDSA_KAT_PATH}: {e}"));
    let doc: serde_json::Value =
        serde_json::from_str(&text).unwrap_or_else(|e| panic!("parse {MLDSA_KAT_PATH}: {e}"));
    let field = doc[name]
        .as_str()
        .unwrap_or_else(|| panic!("{MLDSA_KAT_PATH}: no string field {name}"));
    hex::decode(field).unwrap_or_else(|e| panic!("{MLDSA_KAT_PATH}: {name} is not hex: {e}"))
}

fn pq_of(signatures: &SignatureSet) -> &PqSignature {
    signatures
        .pq_signature
        .as_ref()
        .expect("the fixture carries a post-quantum signature on every signed row")
}

/// Every signature set the envelope carries: the retention checkpoint, each
/// sealed entry, each export receipt and the envelope's own receipt entry.
fn signature_sets(envelope: &mut EvidenceEnvelope) -> Vec<(&'static str, &mut SignatureSet)> {
    let mut sets: Vec<(&'static str, &mut SignatureSet)> = Vec::new();
    if let Some(checkpoint) = envelope.ledgers.checkpoints.latest.as_mut() {
        sets.push(("checkpoint", &mut checkpoint.signatures));
    }
    for entry in envelope.ledgers.sealed_events.entries.iter_mut() {
        sets.push(("sealed event", &mut entry.signatures));
    }
    for entry in envelope.ledgers.export_receipts.entries.iter_mut() {
        sets.push(("export receipt", &mut entry.signatures));
    }
    sets.push((
        "export receipt entry",
        &mut envelope.export_receipt_entry.signatures,
    ));
    sets
}

#[test]
fn envelope_signed_by_the_pqcrypto_kernel_verifies_in_strict_mode() {
    let mut envelope = load_envelope();
    assert_eq!(
        envelope
            .provenance
            .pq_public_key
            .as_ref()
            .map(|key| key.len()),
        Some(1312),
        "the fixture carries an ML-DSA-44 public key"
    );
    let sets = signature_sets(&mut envelope);
    assert_eq!(
        sets.iter().map(|(what, _)| *what).collect::<Vec<_>>(),
        [
            "checkpoint",
            "sealed event",
            "sealed event",
            "export receipt",
            "export receipt entry"
        ]
    );
    for (what, set) in &sets {
        let pq = pq_of(set);
        assert_eq!(pq.scheme_id, "mldsa44", "{what}");
        assert_eq!(pq.signature.len(), 2420, "{what}");
    }

    let report = verify_envelope(&envelope, SignatureMode::Strict)
        .expect("a log the pqcrypto kernel signed must keep verifying in strict mode");
    assert!(report.pq_checked);
    assert_eq!(report.sealed_events, 2);
    assert_eq!(report.export_receipts, 1);

    let report = verify_envelope(&envelope, SignatureMode::Compat).expect("and in compat mode");
    assert!(report.pq_checked);
}

#[test]
fn envelope_with_one_tampered_pq_signature_fails_in_both_modes() {
    let pristine = load_envelope();
    let count = signature_sets(&mut pristine.clone()).len();
    for index in 0..count {
        let mut tampered = pristine.clone();
        let what = {
            let mut sets = signature_sets(&mut tampered);
            let (what, set) = &mut sets[index];
            let pq = set.pq_signature.as_mut().expect("a pq signature");
            // A byte of the challenge hash: the signature still decodes, so
            // the refusal is the verification itself.
            pq.signature[7] ^= 0x01;
            *what
        };
        // The digest is recomputed so the refusal is the signature's, not the
        // whole-envelope fingerprint's.
        tampered.whole_envelope_digest =
            envelope::compute_whole_envelope_digest(&tampered).expect("digest");
        for mode in [SignatureMode::Strict, SignatureMode::Compat] {
            assert!(
                verify_envelope(&tampered, mode).is_err(),
                "{mode:?} accepted a tampered post-quantum signature on the {what} (set {index})"
            );
        }
    }
}

#[test]
fn envelope_stripped_of_its_pq_signatures_fails_strict_and_passes_compat() {
    let mut stripped = load_envelope();
    for (_, set) in signature_sets(&mut stripped) {
        set.pq_signature = None;
    }
    stripped.whole_envelope_digest =
        envelope::compute_whole_envelope_digest(&stripped).expect("digest");
    assert!(
        verify_envelope(&stripped, SignatureMode::Strict).is_err(),
        "strict requires the post-quantum signature"
    );
    let report =
        verify_envelope(&stripped, SignatureMode::Compat).expect("compat needs Ed25519 only");
    assert!(!report.pq_checked);
}

fn run_log_verify(db_path: &str, extra: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_log_verify"))
        .args(["--db", db_path, "--sig-mode", "strict", "--ui", "plain"])
        .args(extra)
        .env("DEVICE_KEY_SEED", SEED)
        .output()
        .expect("spawn log_verify")
}

/// A database whose `device_metadata` holds the key the pqcrypto kernel
/// stored: the kernel loads it, signs with it, and `log_verify` passes the
/// resulting log in strict mode against that same stored public key.
#[test]
fn kernel_loads_a_stored_pqcrypto_key_and_its_log_verifies_strict() {
    let public_key = kat_hex("pq_public_key");
    let secret_key = kat_hex("pq_secret_key");

    let dir = tempfile::tempdir().expect("tempdir");
    let db_path = dir.path().join("witness.db");
    let db_path = db_path.to_str().expect("utf8 path");
    let cfg = config(db_path);

    // First open: the schema and a keypair of this build's own. Replace that
    // keypair with the stored one before anything is signed with it.
    {
        let kernel = Kernel::open(&cfg).expect("open kernel");
        assert_ne!(
            kernel.device_pq_public_key_for_verify_only(),
            Some(public_key.clone())
        );
        let changed = kernel
            .conn
            .execute(
                "UPDATE device_metadata SET pq_public_key = ?1, pq_secret_key = ?2 WHERE id = 1",
                rusqlite::params![public_key, secret_key],
            )
            .expect("store the pqcrypto keypair");
        assert_eq!(changed, 1);
    }

    {
        let mut kernel = Kernel::open(&cfg).expect("reopen with the stored key");
        assert_eq!(
            kernel.device_pq_public_key_for_verify_only(),
            Some(public_key.clone()),
            "the kernel must keep the key its database holds"
        );
        seal_event(&mut kernel, &cfg, "zone:a", 600);
        seal_event(&mut kernel, &cfg, "zone:b", 1200);
        kernel
            .enforce_retention_with_checkpoint(Duration::from_secs(0))
            .expect("checkpoint");
        seal_event(&mut kernel, &cfg, "zone:a", 1800);

        // The key was not replaced or rewritten on the way.
        let (stored_public, stored_secret): (Vec<u8>, Vec<u8>) = kernel
            .conn
            .query_row(
                "SELECT pq_public_key, pq_secret_key FROM device_metadata WHERE id = 1",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .expect("read the stored keypair");
        assert_eq!(stored_public, public_key);
        assert_eq!(stored_secret, secret_key);
        let (rows, hybrid_rows): (i64, i64) = kernel
            .conn
            .query_row(
                "SELECT COUNT(*), COALESCE(SUM(pq_scheme = 'mldsa44' \
                 AND length(pq_signature) = 2420), 0) FROM sealed_events",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .expect("count hybrid rows");
        assert!(rows >= 1, "an entry is retained");
        assert_eq!(
            hybrid_rows, rows,
            "every retained entry carries an ML-DSA-44 signature"
        );
    }

    let output = run_log_verify(db_path, &[]);
    assert!(
        output.status.success(),
        "strict verification failed\n--- stdout ---\n{}\n--- stderr ---\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );

    // Named explicitly, the stored public key gives the same verdict...
    let output = run_log_verify(db_path, &["--pq-public-key", &hex::encode(&public_key)]);
    assert!(
        output.status.success(),
        "strict verification with the stored key named failed\n--- stderr ---\n{}",
        String::from_utf8_lossy(&output.stderr)
    );

    // ...and any other ML-DSA-44 public key refuses the log. One altered byte
    // of t1 is still a well-formed key, for a different keypair.
    let mut other_key = public_key.clone();
    other_key[100] ^= 0x01;
    let output = run_log_verify(db_path, &["--pq-public-key", &hex::encode(&other_key)]);
    assert!(
        !output.status.success(),
        "strict verification passed under a different post-quantum public key"
    );
}

/// A `pq_secret_key` column that is not a private key any key generation
/// wrote is an error from `Kernel::open`, with nothing signed and no panic.
#[test]
fn kernel_refuses_a_corrupt_stored_pq_secret_key_without_panicking() {
    let public_key = kat_hex("pq_public_key");
    let stored = kat_hex("pq_secret_key");

    // The first three-bit coefficient of s1 set to 7, which encodes no
    // coefficient; and a column cut short.
    let mut out_of_range = stored.clone();
    out_of_range[128] |= 0b111;
    let truncated = stored[..2559].to_vec();

    for (secret_key, expected) in [
        (
            out_of_range,
            "invalid pq secret key bytes: a coefficient of s1 or s2 is out of range",
        ),
        (
            truncated,
            "invalid pq secret key bytes: expected 2560 bytes, got 2559",
        ),
    ] {
        let dir = tempfile::tempdir().expect("tempdir");
        let db_path = dir.path().join("witness.db");
        let cfg = config(db_path.to_str().expect("utf8 path"));
        {
            let kernel = Kernel::open(&cfg).expect("open kernel");
            kernel
                .conn
                .execute(
                    "UPDATE device_metadata SET pq_public_key = ?1, pq_secret_key = ?2 \
                     WHERE id = 1",
                    rusqlite::params![public_key, secret_key],
                )
                .expect("store the corrupt keypair");
        }
        match Kernel::open(&cfg) {
            Ok(_) => panic!("the kernel opened with a corrupt post-quantum key"),
            Err(e) => assert_eq!(e.to_string(), expected),
        }
    }
}
