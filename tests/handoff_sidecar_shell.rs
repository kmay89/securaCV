//! The POSIX shell recipe (`scripts/verify_handoff_sidecar.sh`) agrees with
//! the Rust verifier: it accepts a generated unseal sidecar (with a
//! recipient) and a quorum export sidecar (with a recipient), and a
//! one-byte change to the file or the sidecar makes it exit non-zero. Runs
//! under `sh` and, when present, `dash`; needs `openssl` (3.x), `jq` and
//! `sha256sum` — no `xxd`. Skips (and says so) when a tool is missing.

use anyhow::Result;
use ed25519_dalek::SigningKey;
use std::path::Path;
use std::process::Command;
use witness_kernel::break_glass::{
    Approval, BreakGlass, BreakGlassToken, QuorumPolicy, TrusteeEntry, TrusteeId, UnlockRequest,
};
use witness_kernel::handoff::{self, HandoffRecipient};
use witness_kernel::{
    CandidateEvent, EventType, ExportOptions, InferenceBackend, Kernel, KernelConfig,
    ModuleDescriptor, TimeBucket, ZonePolicy, EXPORT_EVENTS_ENVELOPE_ID,
};

const SCRIPT: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/scripts/verify_handoff_sidecar.sh"
);

fn have(tool: &str, args: &[&str]) -> bool {
    Command::new(tool)
        .args(args)
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

fn tools_present() -> bool {
    let openssl3 = Command::new("openssl")
        .arg("version")
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).to_string())
        .is_some_and(|v| {
            v.split_whitespace()
                .nth(1)
                .and_then(|ver| ver.split('.').next())
                .and_then(|major| major.parse::<u32>().ok())
                .is_some_and(|m| m >= 3)
        });
    if !openssl3 || !have("jq", &["--version"]) || !have("sha256sum", &["--version"]) {
        println!("skipped: the shell recipe needs openssl >= 3.0, jq and sha256sum on PATH");
        return false;
    }
    true
}

fn shells() -> Vec<&'static str> {
    let mut out = vec!["sh"];
    if Path::new("/usr/bin/dash").exists() || Path::new("/bin/dash").exists() {
        out.push("dash");
    }
    out
}

fn run(shell: &str, sidecar: &Path, file: &Path, key: &str) -> (bool, String) {
    let o = Command::new(shell)
        .arg(SCRIPT)
        .arg(sidecar)
        .arg(file)
        .arg(key)
        .output()
        .expect("the shell runs");
    (
        o.status.success(),
        format!(
            "{}{}",
            String::from_utf8_lossy(&o.stdout),
            String::from_utf8_lossy(&o.stderr)
        ),
    )
}

fn cfg() -> KernelConfig {
    KernelConfig {
        db_path: ":memory:".to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(3600),
        device_key_seed: "devkey:test:a1b2c3d4e5f6a7b8c9d0".to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn grant(kernel: &mut Kernel, c: &KernelConfig, envelope: &str) -> Result<BreakGlassToken> {
    let alice = SigningKey::from_bytes(&[41u8; 32]);
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: alice.verifying_key().to_bytes(),
        }],
    )?;
    if kernel.break_glass_policy().is_none() {
        kernel.set_break_glass_policy(&policy)?;
    }
    let bucket = TimeBucket::now(600)?;
    let request = UnlockRequest::new(envelope, c.ruleset_hash, "incident", bucket)?;
    let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &alice);
    let (result, receipt) =
        BreakGlass::authorize(&policy, &request, std::slice::from_ref(&approval), bucket);
    let mut token = result?;
    let granted = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, granted)?;
    Ok(token)
}

fn flip_last_byte(path: &Path) -> Result<Vec<u8>> {
    let original = std::fs::read(path)?;
    let mut changed = original.clone();
    let last = changed.len() - 1;
    changed[last] ^= 1;
    std::fs::write(path, &changed)?;
    Ok(original)
}

#[test]
fn shell_recipe_matches() -> Result<()> {
    if !tools_present() {
        return Ok(());
    }
    let dir = tempfile::tempdir()?;
    let c = cfg();
    let mut kernel = Kernel::open(&c)?;
    let desc = ModuleDescriptor {
        id: "test_module",
        allowed_event_types: &[EventType::BoundaryCrossingObjectLarge],
        requested_capabilities: &[],
        supported_backends: &[InferenceBackend::Stub],
    };
    kernel.append_event_checked(
        &desc,
        CandidateEvent {
            event_type: EventType::BoundaryCrossingObjectLarge,
            time_bucket: TimeBucket {
                start_epoch_s: 600,
                size_s: 600,
            },
            zone_id: "zone:test".into(),
            confidence: 0.5,
            correlation_token: None,
            attestation: None,
        },
        &c.kernel_version,
        &c.ruleset_id,
        c.ruleset_hash,
    )?;
    let key = hex::encode(kernel.device_verifying_key().to_bytes());
    let recipient = HandoffRecipient::new(
        "Dana Reyes",
        Some("Example County Records Office"),
        Some(&"ab".repeat(32)),
    )?;

    // An unseal sidecar with a recipient.
    let token = grant(&mut kernel, &c, "vault:1")?;
    let redemption = kernel.redeem_token_durably(&token)?;
    let raw = dir.path().join("vault-1.raw");
    let out = handoff::release_unsealed(
        &mut kernel,
        b"securacv-fixture \x00\xff binary".to_vec(),
        &handoff::UnsealRelease {
            raw_path: &raw,
            redemption_entry_hash: redemption,
            granted_entry_hash: token.receipt_entry_hash(),
            recipient: Some(&recipient),
            created_bucket: handoff::handoff_bucket_now()?,
            tool_version: "test",
        },
        &mut handoff::FsReleaseIo,
    )?;

    // A quorum export sidecar with a recipient.
    let mut export_token = grant(&mut kernel, &c, EXPORT_EVENTS_ENVELOPE_ID)?;
    let salt = handoff::fresh_salt();
    let bundle = kernel.export_events_bundle_authorized_with(
        c.ruleset_hash,
        ExportOptions::default(),
        &mut export_token,
        Some(handoff::recipient_commitment(&salt, &recipient)),
    )?;
    let bundle_bytes = serde_json::to_vec(&bundle)?;
    let bundle_path = dir.path().join("bundle.json");
    std::fs::write(&bundle_path, &bundle_bytes)?;
    let sidecar = handoff::build_export_sidecar(
        &kernel.conn,
        &bundle_bytes,
        "bundle.json",
        Some((&recipient, Some(salt))),
        handoff::handoff_bucket_now()?,
        "test",
    )?;
    let export_sidecar = dir.path().join("bundle.json.securacv-handoff.json");
    handoff::write_sidecar_new(&export_sidecar, &sidecar.to_canonical_bytes()?)?;

    for shell in shells() {
        for (side, file) in [(&out.sidecar_path, &raw), (&export_sidecar, &bundle_path)] {
            let (ok, text) = run(shell, side, file, &key);
            assert!(ok, "{shell}: {text}");
            assert!(
                text.contains("OK   recipient: opens the signed commitment"),
                "{shell}: {text}"
            );
            assert!(
                text.contains("OK: the records verify under the key you pinned"),
                "{text}"
            );
            // A one-byte change to the file, then to the sidecar.
            let original = flip_last_byte(file)?;
            let (ok, text) = run(shell, side, file, &key);
            assert!(!ok, "{shell}: a changed file must fail: {text}");
            assert!(
                text.contains("FAIL file: sha256 does not match the sidecar"),
                "{text}"
            );
            std::fs::write(file, &original)?;
            let mut v: serde_json::Value = serde_json::from_slice(&std::fs::read(side)?)?;
            let payload = v["records"][0]["payload_json"]
                .as_str()
                .unwrap()
                .replacen("1", "2", 1);
            v["records"][0]["payload_json"] = serde_json::Value::String(payload);
            let tampered = dir.path().join("tampered.json");
            std::fs::write(&tampered, serde_json::to_vec(&v)?)?;
            let (ok, text) = run(shell, &tampered, file, &key);
            assert!(!ok, "{shell}: a changed sidecar must fail: {text}");
            // And another pinned key.
            let (ok, _) = run(shell, side, file, &"00".repeat(32));
            assert!(!ok);
        }
    }
    Ok(())
}

#[test]
fn hex2bin_round_trips_every_byte() -> Result<()> {
    let all: Vec<u8> = (0u8..=255).collect();
    let hex = hex::encode(&all);
    let script = std::fs::read_to_string(SCRIPT)?;
    let def = script
        .lines()
        .find(|l| l.starts_with("hex2bin() {"))
        .expect("the decoder line");
    for shell in shells() {
        let o = Command::new(shell)
            .arg("-c")
            .arg(format!("{def}\nhex2bin \"$1\"", def = def))
            .arg("hex2bin-test")
            .arg(&hex)
            .output()?;
        assert!(
            o.status.success(),
            "{shell}: {}",
            String::from_utf8_lossy(&o.stderr)
        );
        assert_eq!(o.stdout, all, "{shell}");
    }
    Ok(())
}
