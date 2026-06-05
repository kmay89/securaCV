//! tamper_demo — runnable proof that tampering is detected end-to-end.
//!
//! Demonstrates the v1 acceptance criterion "log_verify detects tampering
//! end-to-end" without any external setup:
//!   1. Create a signed witness log (real Kernel, real Ed25519 signatures).
//!   2. Verify the untampered log — passes.
//!   3. Rewrite one stored event's payload directly in the database.
//!   4. Re-verify — the hash chain + signature check reject it.
//!
//! This uses the production `Kernel` and `verify::*` paths, not a
//! reimplementation, so a green run is real evidence of the property.

use anyhow::Result;
use rand::TryRng;
use rusqlite::Connection;

use witness_kernel::crypto::signatures::SignatureMode;
use witness_kernel::{
    db_key_seed_from_env, resolve_db_encryption_key, signing_key_from_seed, verify, verify_helpers,
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, ZonePolicy,
};

const RULESET_ID: &str = "ruleset:demo";

/// Open the SQLCipher-encrypted demo database with the device-derived key.
fn open_encrypted(path: &str, seed: &str) -> Result<Connection> {
    let signing_key = signing_key_from_seed(seed)?;
    let db_key = resolve_db_encryption_key(
        &signing_key,
        db_key_seed_from_env().as_ref().map(|s| s.as_str()),
    );
    let conn = Connection::open(path)?;
    conn.pragma_update(None, "key", format!("x'{}'", &*db_key))?;
    Ok(conn)
}

/// Append one signed witness event for the given zone.
fn write_event(kernel: &mut Kernel, zone: &str) -> Result<()> {
    let module = ModuleDescriptor {
        id: "demo-module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    let cand = CandidateEvent {
        event_type: EventType::BoundaryCrossingObjectLarge,
        time_bucket: TimeBucket::now(600)?,
        zone_id: zone.to_string(),
        confidence: 0.9,
        correlation_token: None,
    };
    kernel.append_event_checked(
        &module,
        cand,
        env!("CARGO_PKG_VERSION"),
        RULESET_ID,
        KernelConfig::ruleset_hash_from_id(RULESET_ID),
    )?;
    Ok(())
}

/// Run the production verification path over the sealed event log.
/// Returns the number of verified events, or an error if the chain or any
/// signature fails to validate.
fn verify_events(path: &str, seed: &str, pubkey_hex: &str) -> Result<u64> {
    let conn = open_encrypted(path, seed)?;
    let verifying_key = verify_helpers::load_verifying_key(&conn, Some(pubkey_hex), None)?;
    let checkpoint = verify::latest_checkpoint(&conn)?;
    let count = verify::verify_events_with(
        &conn,
        &verifying_key,
        checkpoint.chain_head_hash,
        SignatureMode::Compat,
        None,
        |_, _| {},
    )?;
    Ok(count)
}

fn cleanup(db_path: &str) {
    let _ = std::fs::remove_file(db_path);
    let _ = std::fs::remove_file(format!("{}-wal", db_path));
    let _ = std::fs::remove_file(format!("{}-shm", db_path));
}

fn run(db_path: &str) -> Result<()> {
    let mut seed_bytes = [0u8; 32];
    rand::rngs::SysRng
        .try_fill_bytes(&mut seed_bytes[..])
        .expect("OS RNG unavailable");
    let seed = format!("devkey:{}", hex::encode(seed_bytes));

    println!("SecuraCV tamper-evidence demo");
    println!("=============================");
    println!();
    println!("[1/4] Creating a signed witness log");
    println!("      db: {}", db_path);

    let pubkey_hex;
    {
        let mut kernel = Kernel::open(&KernelConfig {
            db_path: db_path.to_string(),
            ruleset_id: RULESET_ID.to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET_ID),
            kernel_version: env!("CARGO_PKG_VERSION").to_string(),
            retention: std::time::Duration::from_secs(3600),
            device_key_seed: seed.clone(),
            zone_policy: ZonePolicy::default(),
        })?;
        write_event(&mut kernel, "zone:door")?;
        write_event(&mut kernel, "zone:hallway")?;
        write_event(&mut kernel, "zone:window")?;
        pubkey_hex = hex::encode(kernel.device_key_for_verify_only());
        println!("      wrote 3 signed witness events");
        println!("      device public key: {}...", &pubkey_hex[..16]);
    }

    println!();
    println!("[2/4] Verifying the untampered log (expect PASS)");
    let n = verify_events(db_path, &seed, &pubkey_hex)?;
    println!(
        "      OK: {} events verified — chain + signatures intact",
        n
    );

    println!();
    println!("[3/4] Tampering: rewriting event #2's payload in the database");
    {
        let conn = open_encrypted(db_path, &seed)?;
        let changed = conn.execute(
            "UPDATE sealed_events SET payload_json = '{\"tampered\":\"by attacker\"}' WHERE id = 2",
            [],
        )?;
        if changed != 1 {
            return Err(anyhow::anyhow!(
                "failed to tamper: expected 1 row modified, got {}",
                changed
            ));
        }
        println!("      modified {} row(s) directly in the DB", changed);
    }

    println!();
    println!("[4/4] Re-verifying the tampered log (expect FAIL)");
    match verify_events(db_path, &seed, &pubkey_hex) {
        Ok(_) => {
            return Err(anyhow::anyhow!(
                "tampering was NOT detected — verification passed on a modified log"
            ));
        }
        Err(e) => {
            println!("      DETECTED: verification rejected the tampered log");
            println!("      reason: {}", e);
        }
    }

    println!();
    println!("Result: tamper-evidence works end-to-end.");
    println!("A single altered byte breaks the hash chain and the signature check.");
    Ok(())
}

fn main() -> Result<()> {
    let mut path = std::env::temp_dir();
    let suffix: u64 = rand::random();
    path.push(format!("securacv_tamper_demo_{}.db", suffix));
    let db_path = path.to_string_lossy().to_string();

    let result = run(&db_path);
    cleanup(&db_path);
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    struct TempDb {
        path: String,
    }

    impl TempDb {
        fn new() -> Self {
            let mut path = std::env::temp_dir();
            let suffix: u64 = rand::random();
            path.push(format!("tamper_demo_test_{}.db", suffix));
            Self {
                path: path.to_string_lossy().to_string(),
            }
        }
    }

    impl Drop for TempDb {
        fn drop(&mut self) {
            cleanup(&self.path);
        }
    }

    /// The full demo cycle: a clean log verifies, and the same log fails
    /// verification after a single payload byte is altered.
    #[test]
    fn demo_detects_tampering() -> Result<()> {
        let db = TempDb::new();

        let mut seed_bytes = [0u8; 32];
        rand::rngs::SysRng
            .try_fill_bytes(&mut seed_bytes[..])
            .expect("OS RNG unavailable");
        let seed = format!("devkey:{}", hex::encode(seed_bytes));

        let pubkey_hex;
        {
            let mut kernel = Kernel::open(&KernelConfig {
                db_path: db.path.clone(),
                ruleset_id: RULESET_ID.to_string(),
                ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET_ID),
                kernel_version: env!("CARGO_PKG_VERSION").to_string(),
                retention: std::time::Duration::from_secs(3600),
                device_key_seed: seed.clone(),
                zone_policy: ZonePolicy::default(),
            })?;
            write_event(&mut kernel, "zone:door")?;
            write_event(&mut kernel, "zone:hallway")?;
            write_event(&mut kernel, "zone:window")?;
            pubkey_hex = hex::encode(kernel.device_key_for_verify_only());
        }

        // Clean log verifies.
        let n = verify_events(&db.path, &seed, &pubkey_hex)?;
        assert_eq!(n, 3u64, "all three events should verify before tampering");

        // Tamper with one event.
        {
            let conn = open_encrypted(&db.path, &seed)?;
            let changed = conn.execute(
                "UPDATE sealed_events SET payload_json = '{\"tampered\":true}' WHERE id = 2",
                [],
            )?;
            assert_eq!(changed, 1, "exactly one row should be tampered");
        }

        // Tampered log must fail verification.
        let result = verify_events(&db.path, &seed, &pubkey_hex);
        assert!(
            result.is_err(),
            "verification must reject the tampered log, got: {:?}",
            result
        );

        Ok(())
    }
}
