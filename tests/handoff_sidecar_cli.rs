//! End-to-end tests for the handoff-sidecar emitters and the
//! `handoff_sidecar` tool: `break_glass unseal` (recipient file, stale
//! sidecar, refusal before the burn, the pinned output lines), the drill,
//! `export_events` (declared context, quorum conflict, the sidecar-failure
//! path), `handoff_sidecar emit | verify | redact` (exit codes, verdict
//! lines, no overwrite, read-only), and `export_verify`'s disclosure lines.
//! All offline.

use anyhow::Result;
use ed25519_dalek::SigningKey;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use witness_kernel::break_glass::{
    Approval, BreakGlass, BreakGlassToken, BreakGlassTokenFile, QuorumPolicy, TrusteeEntry,
    TrusteeId, UnlockRequest,
};
use witness_kernel::{
    CandidateEvent, EventType, InferenceBackend, Kernel, KernelConfig, ModuleDescriptor,
    TimeBucket, Vault, VaultConfig, ZonePolicy, EXPORT_EVENTS_ENVELOPE_ID,
};

const SEED: &str = "devkey:test:a1b2c3d4e5f6a7b8c9d0";
const SECRET: &[u8] = b"securacv-fixture";

fn cfg(db: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(3600),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn alice() -> SigningKey {
    SigningKey::from_bytes(&[41u8; 32])
}

fn policy() -> Result<QuorumPolicy> {
    QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: alice().verifying_key().to_bytes(),
        }],
    )
}

fn grant(
    kernel: &mut Kernel,
    c: &KernelConfig,
    envelope: &str,
) -> Result<(BreakGlassToken, [u8; 32])> {
    let bucket = TimeBucket::now(600)?;
    let request = UnlockRequest::new(envelope, c.ruleset_hash, "incident", bucket)?;
    let approval = Approval::signed(TrusteeId::new("alice"), request.request_hash(), &alice());
    let (result, receipt) = BreakGlass::authorize(
        &policy()?,
        &request,
        std::slice::from_ref(&approval),
        bucket,
    );
    let mut token = result?;
    let granted = kernel.log_break_glass_receipt(&receipt, &[approval])?;
    kernel.sign_break_glass_token(&mut token, granted)?;
    Ok((token, granted))
}

fn add_event(kernel: &mut Kernel, c: &KernelConfig) -> Result<()> {
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
    Ok(())
}

struct Site {
    dir: tempfile::TempDir,
    db: PathBuf,
    vault: PathBuf,
    token: PathBuf,
    grant: [u8; 32],
    device_pub_file: PathBuf,
    device_pub: String,
}

/// A database with a 1-of-1 policy, one event, a sealed envelope `vault:1`,
/// and an unseal token file for it.
fn site() -> Result<Site> {
    let dir = tempfile::tempdir()?;
    let db = dir.path().join("witness.db");
    let vault = dir.path().join("vault");
    let c = cfg(&db);
    let mut kernel = Kernel::open(&c)?;
    add_event(&mut kernel, &c)?;
    let policy = policy()?;
    kernel.set_break_glass_policy(&policy)?;
    let (mut seal_token, _) = grant(&mut kernel, &c, "vault-1")?;
    let vk = kernel.device_verifying_key();
    let v = Vault::new(VaultConfig {
        local_path: vault.clone(),
        crypto_mode: policy.vault.crypto_mode,
        key_material: witness_kernel::VaultKeyMaterial::from_env(),
    })?;
    let mut raw = SECRET.to_vec();
    v.seal(
        "vault-1",
        &mut seal_token,
        c.ruleset_hash,
        &mut raw,
        &vk,
        |h| kernel.break_glass_receipt_outcome("vault-1", c.ruleset_hash, h),
    )?;
    let (token, granted) = grant(&mut kernel, &c, "vault-1")?;
    let token_path = dir.path().join("unseal.token.json");
    std::fs::write(
        &token_path,
        serde_json::to_string(&BreakGlassTokenFile::from_token(&token)?)?,
    )?;
    let device_pub = hex::encode(vk.to_bytes());
    let device_pub_file = dir.path().join("device.pub");
    std::fs::write(&device_pub_file, format!("{device_pub}\n"))?;
    drop(kernel);
    Ok(Site {
        dir,
        db,
        vault,
        token: token_path,
        grant: granted,
        device_pub_file,
        device_pub,
    })
}

fn s(p: &Path) -> String {
    p.to_string_lossy().to_string()
}

fn bin(name: &str) -> Command {
    let path = match name {
        "break_glass" => env!("CARGO_BIN_EXE_break_glass"),
        "export_events" => env!("CARGO_BIN_EXE_export_events"),
        "export_verify" => env!("CARGO_BIN_EXE_export_verify"),
        "handoff_sidecar" => env!("CARGO_BIN_EXE_handoff_sidecar"),
        _ => unreachable!(),
    };
    let mut cmd = Command::new(path);
    cmd.env_remove("DEVICE_KEY_SEED")
        .env_remove("SECURACV_DB_KEY");
    cmd
}

fn unseal_cmd(site: &Site, out: &Path, extra: &[&str]) -> Output {
    bin("break_glass")
        .args([
            "unseal",
            "--envelope",
            "vault-1",
            "--token",
            &s(&site.token),
            "--db",
            &s(&site.db),
            "--ruleset-id",
            "ruleset:test",
            "--vault-path",
            &s(&site.vault),
            "--output-dir",
            &s(out),
            "--device-key-seed",
            SEED,
        ])
        .args(extra)
        .output()
        .expect("break_glass runs")
}

fn text(b: &[u8]) -> String {
    String::from_utf8_lossy(b).to_string()
}

fn redemption_rows(db: &Path) -> Result<usize> {
    let kernel = Kernel::open(&cfg(db))?;
    let mut stmt = kernel
        .conn
        .prepare("SELECT payload_json FROM break_glass_receipts")?;
    let rows: Vec<String> = stmt
        .query_map([], |r| r.get(0))?
        .collect::<rusqlite::Result<_>>()?;
    Ok(rows.iter().filter(|p| p.contains("TokenRedeemed")).count())
}

#[cfg(unix)]
fn mode(p: &Path) -> u32 {
    use std::os::unix::fs::PermissionsExt;
    std::fs::metadata(p).unwrap().permissions().mode() & 0o777
}

#[test]
fn unseal_with_a_recipient_file_writes_both_files_0600_and_verifies() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    let recipient = site.dir.path().join("recipient.json");
    std::fs::write(
        &recipient,
        r#"{"name":"Dana Reyes","organization":"Example County Records Office"}"#,
    )?;
    let o = unseal_cmd(&site, &out, &["--recipient-file", &s(&recipient)]);
    assert!(o.status.success(), "{}", text(&o.stderr));
    let stdout = text(&o.stdout);
    let raw = out.join("vault-1.raw");
    let sidecar = PathBuf::from(format!("{}.securacv-handoff.json", raw.display()));
    let lines: Vec<&str> = stdout.lines().collect();
    assert_eq!(
        lines[0],
        format!("Unsealed envelope written to {}", raw.display())
    );
    assert_eq!(
        lines[1],
        "  sha256:          89e4120edf60957bde82c27f35ef282ba6608dfdc29b454534ae136da1cd9dd2 (16 bytes)"
    );
    assert!(
        lines[2].starts_with("  unseal record:   ")
            && lines[2].ends_with(
                "… appended to the receipt chain (device-signed; pairs this file to its grant)"
            ),
        "{}",
        lines[2]
    );
    assert_eq!(
        lines[3],
        format!(
            "  handoff sidecar: {} (mode 0600; hand it over with the file)",
            sidecar.display()
        )
    );
    assert_eq!(
        lines[4],
        "  recipient:       committed in the signed unseal record; named only in the handoff sidecar"
    );
    assert!(!stdout.contains("Dana"), "the name is never printed");
    assert_eq!(std::fs::read(&raw)?, SECRET);
    #[cfg(unix)]
    {
        assert_eq!(mode(&raw), 0o600);
        assert_eq!(mode(&sidecar), 0o600);
    }
    let v = bin("handoff_sidecar")
        .args([
            "verify",
            "--file",
            &s(&raw),
            "--public-key-file",
            &s(&site.device_pub_file),
        ])
        .output()?;
    assert_eq!(v.status.code(), Some(0), "{}", text(&v.stdout));
    let vout = text(&v.stdout);
    assert!(vout.contains("This sidecar is unsigned. It carries device-signed records; everything else in it is declared by the tool that wrote it."));
    assert!(
        vout.lines()
            .last()
            .unwrap()
            .starts_with("OK: handoff sidecar checked — VALID (identity anchored out-of-band)."),
        "{vout}"
    );
    Ok(())
}

#[test]
fn a_bad_recipient_key_is_refused_before_the_burn() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    let o = unseal_cmd(
        &site,
        &out,
        &["--recipient-name", "Dana", "--recipient-key", "abcd"],
    );
    assert!(!o.status.success());
    assert!(
        text(&o.stderr)
            .contains("recipient key must be 32 bytes (Ed25519 public key, 64 hex), got 2"),
        "{}",
        text(&o.stderr)
    );
    assert_eq!(redemption_rows(&site.db)?, 0, "nothing was burned");
    Ok(())
}

#[test]
fn a_stale_sidecar_is_replaced_and_an_obstacle_refuses_before_the_burn() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    std::fs::create_dir_all(&out)?;
    let sidecar = out.join("vault-1.raw.securacv-handoff.json");
    // A non-removable obstacle: a non-empty directory at the sidecar path.
    std::fs::create_dir_all(sidecar.join("x"))?;
    let o = unseal_cmd(&site, &out, &[]);
    assert!(!o.status.success());
    let err = text(&o.stderr);
    assert!(
        err.contains(&format!(
            "refusing to unseal: a stale handoff sidecar at {} could not be moved aside (",
            sidecar.display()
        )) && err.contains("); nothing was recorded and the token is not spent"),
        "{err}"
    );
    assert_eq!(redemption_rows(&site.db)?, 0);
    // A stale FILE is replaced, and nothing is left aside.
    std::fs::remove_dir_all(&sidecar)?;
    std::fs::write(&sidecar, b"stale")?;
    let o = unseal_cmd(&site, &out, &[]);
    assert!(o.status.success(), "{}", text(&o.stderr));
    assert_ne!(std::fs::read(&sidecar)?, b"stale");
    assert!(text(&o.stdout).contains("  recipient:       none named"));
    let mut names: Vec<String> = std::fs::read_dir(&out)?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect();
    names.sort();
    assert_eq!(names, ["vault-1.raw", "vault-1.raw.securacv-handoff.json"]);
    Ok(())
}

/// A re-run that fails before its own sidecar exists — here the token is
/// already spent — leaves the earlier disclosure's sidecar byte-identical:
/// it holds the only copy of the salts that open the earlier unseal record.
#[test]
fn a_failed_rerun_keeps_the_earlier_unseal_sidecar() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    let o = unseal_cmd(&site, &out, &["--recipient-name", "Dana Reyes"]);
    assert!(o.status.success(), "{}", text(&o.stderr));
    let raw = out.join("vault-1.raw");
    let sidecar = out.join("vault-1.raw.securacv-handoff.json");
    let first = std::fs::read(&sidecar)?;
    let o = unseal_cmd(&site, &out, &[]);
    assert!(!o.status.success(), "a spent token unseals nothing");
    assert_eq!(std::fs::read(&sidecar)?, first, "restored byte-identical");
    assert_eq!(std::fs::read(&raw)?, SECRET);
    let names: Vec<String> = std::fs::read_dir(&out)?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with('.'))
        .collect();
    assert!(names.is_empty(), "nothing is left aside: {names:?}");
    // The restored sidecar still verifies against the earlier file.
    let v = bin("handoff_sidecar")
        .args([
            "verify",
            "--file",
            &s(&raw),
            "--public-key-file",
            &s(&site.device_pub_file),
        ])
        .output()?;
    assert_eq!(v.status.code(), Some(0), "{}", text(&v.stdout));
    Ok(())
}

#[test]
fn the_drill_records_the_unseal_and_verifies_its_sidecar() {
    let o = bin("break_glass")
        .args(["drill", "--threshold", "2", "--trustees", "3"])
        .output()
        .expect("drill runs");
    let out = text(&o.stdout);
    assert!(o.status.success(), "{out}\n{}", text(&o.stderr));
    let unsealed = out
        .find("unsealed it with a fresh quorum token")
        .expect("unseal line");
    let recorded = out
        .find("recorded the unseal and verified its handoff sidecar")
        .expect("the new drill line");
    assert!(recorded > unsealed);
    assert!(out.contains("DRILL PASSED"));
}

fn export_cmd(db: &Path, output: &Path, extra: &[&str]) -> Output {
    bin("export_events")
        .args([
            "--db-path",
            &s(db),
            "--ruleset-id",
            "ruleset:test",
            "--device-key-seed",
            SEED,
            "--output",
            &s(output),
            "--jitter-s",
            "0",
            "--ui",
            "plain",
        ])
        .args(extra)
        .output()
        .expect("export_events runs")
}

#[test]
fn self_export_with_requested_by_and_recipient_declares_both() -> Result<()> {
    let site = site()?;
    let bundle = site.dir.path().join("bundle.json");
    let o = export_cmd(
        &site.db,
        &bundle,
        &[
            "--self-export",
            "--requested-by",
            "Dana Owner",
            "--reason",
            "owner-recovery",
            "--case-ref",
            "HOA-114",
            "--recipient-name",
            "Pat Recipient",
        ],
    );
    assert!(o.status.success(), "{}", text(&o.stderr));
    let out = text(&o.stdout);
    assert!(out.contains("  context:         Dana Owner, owner-recovery, case HOA-114 — declared by the exporting operator, recorded in the signed receipt"), "{out}");
    assert!(out.contains(
        "  recipient:       committed in the signed receipt; named only in the handoff sidecar"
    ));
    let sidecar = PathBuf::from(format!("{}.securacv-handoff.json", bundle.display()));
    assert!(out.contains(&format!(
        "  handoff sidecar: {} (mode 0600)",
        sidecar.display()
    )));
    assert!(!out.contains("Pat Recipient"));
    let b: witness_kernel::ExportBundle = serde_json::from_slice(&std::fs::read(&bundle)?)?;
    let ctx = b.receipt_entry.receipt.context.expect("context");
    assert_eq!(ctx.operator.expect("operator").requester_name, "Dana Owner");
    assert!(ctx.recipient_commitment.is_some());
    let v = bin("handoff_sidecar")
        .args([
            "verify",
            "--file",
            &s(&bundle),
            "--public-key",
            &site.device_pub,
        ])
        .output()?;
    assert_eq!(v.status.code(), Some(0), "{}", text(&v.stdout));
    assert!(text(&v.stdout).contains("OK         recipient: opens the commitment in the signed export receipt — declared by the operator at disclosure, not verified by the device"));

    // export_verify prints the disclosure lines.
    let o = bin("export_verify")
        .args([
            "--db",
            &s(&site.db),
            "--bundle",
            &s(&bundle),
            "--device-key-seed",
            SEED,
        ])
        .output()?;
    let out = text(&o.stdout);
    assert!(o.status.success(), "{out}\n{}", text(&o.stderr));
    assert!(out.contains("  disclosure context: Dana Owner (owner-recovery, case HOA-114) — declared by the exporting operator, not trustee-consented"), "{out}");
    assert!(out.contains(
        "  recipient: committed in the signed receipt (named only in a handoff sidecar)"
    ));
    Ok(())
}

#[test]
fn a_quorum_export_refuses_requested_by_before_anything_is_burned() -> Result<()> {
    let site = site()?;
    let bundle = site.dir.path().join("bundle.json");
    let o = export_cmd(
        &site.db,
        &bundle,
        &[
            "--break-glass-token",
            &s(&site.token),
            "--requested-by",
            "Dana",
            "--reason",
            "incident-review",
        ],
    );
    assert_eq!(o.status.code(), Some(2), "{}", text(&o.stderr));
    assert!(
        text(&o.stderr).contains("cannot be used with"),
        "{}",
        text(&o.stderr)
    );
    assert_eq!(redemption_rows(&site.db)?, 0);
    assert!(!bundle.exists());
    Ok(())
}

#[test]
fn a_quorum_export_names_its_grant_and_the_walk_is_green() -> Result<()> {
    let site = site()?;
    // A token for export:events.
    let c = cfg(&site.db);
    let mut kernel = Kernel::open(&c)?;
    let (token, granted) = grant(&mut kernel, &c, EXPORT_EVENTS_ENVELOPE_ID)?;
    drop(kernel);
    let token_path = site.dir.path().join("export.token.json");
    std::fs::write(
        &token_path,
        serde_json::to_string(&BreakGlassTokenFile::from_token(&token)?)?,
    )?;
    let bundle = site.dir.path().join("bundle.json");
    let o = export_cmd(
        &site.db,
        &bundle,
        &[
            "--break-glass-token",
            &s(&token_path),
            "--recipient-name",
            "Pat Recipient",
        ],
    );
    assert!(o.status.success(), "{}", text(&o.stderr));
    let out = text(&o.stdout);
    assert!(out.contains(&format!(
        "  consent:         recorded on the authorizing grant {}… (trustee-consented; named by reference)",
        &hex::encode(granted)[..16]
    )), "{out}");
    let b: witness_kernel::ExportBundle = serde_json::from_slice(&std::fs::read(&bundle)?)?;
    assert_eq!(b.receipt_entry.receipt.authorizing_receipt, Some(granted));
    assert!(b
        .receipt_entry
        .receipt
        .context
        .as_ref()
        .unwrap()
        .operator
        .is_none());
    let report = Kernel::open(&c)?.verify_sealed_log()?;
    assert!(report.chain_valid, "{:?}", report.error);
    assert_eq!(report.export_receipts_authorized, Some(1));
    let o = bin("export_verify")
        .args([
            "--db",
            &s(&site.db),
            "--bundle",
            &s(&bundle),
            "--device-key-seed",
            SEED,
        ])
        .output()?;
    assert!(text(&o.stdout).contains(&format!(
        "  authorizing grant: {} (consent recorded on the grant)",
        hex::encode(granted)
    )));
    // The sidecar embeds grant, redemption, export receipt and verifies.
    let v = bin("handoff_sidecar")
        .args([
            "verify",
            "--file",
            &s(&bundle),
            "--public-key",
            &site.device_pub,
            "--db",
            &s(&site.db),
            "--device-key-seed",
            SEED,
        ])
        .output()?;
    assert_eq!(v.status.code(), Some(0), "{}", text(&v.stdout));
    assert!(text(&v.stdout).contains(
        "OK         links: the export receipt names the grant, and the grant was redeemed once"
    ));
    Ok(())
}

#[test]
fn an_export_sidecar_failure_keeps_the_bundle_and_exits_1() -> Result<()> {
    let site = site()?;
    let bundle = site.dir.path().join("bundle.json");
    let o = bin("export_events")
        .env("SECURACV_TEST_HANDOFF_OBSTACLE", "1")
        .args([
            "--db-path",
            &s(&site.db),
            "--ruleset-id",
            "ruleset:test",
            "--device-key-seed",
            SEED,
            "--output",
            &s(&bundle),
            "--self-export",
            "--recipient-name",
            "Pat Recipient",
            "--ui",
            "plain",
        ])
        .output()?;
    assert_eq!(o.status.code(), Some(1));
    let err = text(&o.stderr);
    let sha = hex::encode(<sha2::Sha256 as sha2::Digest>::digest(std::fs::read(
        &bundle,
    )?));
    let sidecar = format!("{}.securacv-handoff.json", bundle.display());
    assert!(err.contains(&format!(
        "ERROR: export bundle written to {} (sha256 {sha}) but its handoff sidecar could not be written (a file already exists at {sidecar}; a handoff sidecar is never written over another file). The bundle and its signed export receipt are kept. Recover the sidecar with:\n  handoff_sidecar emit --file {} --db {}",
        bundle.display(),
        bundle.display(),
        site.db.display()
    )), "{err}");
    assert!(err.contains("The receipt commits to the recipient you named; that commitment's salt existed only in the sidecar that was not written, so a recovered sidecar records the recipient as committed but not disclosed."));
    assert!(!err.contains("Pat Recipient"));
    assert!(bundle.exists());
    Ok(())
}

fn emit(site: &Site, file: &Path, extra: &[&str]) -> Output {
    bin("handoff_sidecar")
        .args([
            "emit",
            "--file",
            &s(file),
            "--db",
            &s(&site.db),
            "--device-key-seed",
            SEED,
        ])
        .args(extra)
        .output()
        .expect("handoff_sidecar runs")
}

#[test]
fn emit_is_read_only_never_overwrites_and_reports_its_bindings() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    let o = unseal_cmd(&site, &out, &["--recipient-name", "Dana Reyes"]);
    assert!(o.status.success(), "{}", text(&o.stderr));
    let raw = out.join("vault-1.raw");
    let db_before = std::fs::read(&site.db)?;

    // Refuses to overwrite the original sidecar.
    let o = emit(&site, &raw, &["--grant", &hex::encode(site.grant)]);
    assert_eq!(o.status.code(), Some(2));
    assert!(text(&o.stderr).contains(&format!(
        "refusing to overwrite {}.securacv-handoff.json: a handoff sidecar is never refreshed in place — write the new one with --out <PATH>",
        raw.display()
    )));
    // A recipient beside a committing row is refused.
    let o = emit(
        &site,
        &raw,
        &[
            "--grant",
            &hex::encode(site.grant),
            "--out",
            &s(&site.dir.path().join("x.json")),
            "--recipient-name",
            "Dana Reyes",
        ],
    );
    assert_eq!(o.status.code(), Some(2));
    assert!(text(&o.stderr).contains("the signed unseal record commits to a recipient; its salt existed only in the original handoff sidecar"));
    // --grant gives pairing declared, committed_not_disclosed.
    let recovered = site.dir.path().join("recovered.json");
    let anchors = site.dir.path().join("anchors");
    let o = emit(
        &site,
        &raw,
        &[
            "--grant",
            &hex::encode(site.grant),
            "--out",
            &s(&recovered),
            "--anchors-out",
            &s(&anchors),
        ],
    );
    assert_eq!(o.status.code(), Some(0), "{}", text(&o.stderr));
    let stdout = text(&o.stdout);
    assert_eq!(
        stdout,
        format!(
            "handoff sidecar written to {}\n  describes: unseal of vault-1.raw (sha256 89e4120edf60957b…)\n  pairing:   declared\n  recipient: committed_not_disclosed\n  anchors:   0 token(s) referenced (0 over the file bytes, 0 over the disclosure record)\n",
            recovered.display()
        )
    );
    assert!(anchors.is_dir());
    #[cfg(unix)]
    assert_eq!(mode(&recovered), 0o600);
    assert_eq!(
        std::fs::read(&site.db)?,
        db_before,
        "emit never writes the database"
    );
    // A non-bundle without a selector.
    let o = emit(&site, &raw, &["--out", &s(&site.dir.path().join("y.json"))]);
    assert_eq!(o.status.code(), Some(2));
    assert!(text(&o.stderr).contains(
        "is not an export bundle; for an unsealed file name its disclosure with --grant <HEX>"
    ));
    Ok(())
}

#[test]
fn verify_exit_codes_and_verdict_lines() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    assert!(unseal_cmd(&site, &out, &[]).status.success());
    let raw = out.join("vault-1.raw");
    let verify = |extra: &[&str]| {
        bin("handoff_sidecar")
            .args(["verify", "--file", &s(&raw)])
            .args(extra)
            .output()
            .unwrap()
    };
    let o = verify(&[]);
    assert_eq!(o.status.code(), Some(0));
    assert!(text(&o.stdout).lines().last().unwrap().starts_with("OK: handoff sidecar checked — SELF-CONSISTENT, IDENTITY UNVERIFIED. The device key came from the sidecar itself; pass --public-key-file <device.pub> obtained out of band for an evidentiary verdict."));
    let o = verify(&["--public-key-file", &s(&site.device_pub_file)]);
    assert_eq!(o.status.code(), Some(0));
    assert!(text(&o.stdout).contains("— VALID (identity anchored out-of-band)."));
    std::fs::write(&raw, b"other bytes")?;
    let o = verify(&["--public-key-file", &s(&site.device_pub_file)]);
    assert_eq!(o.status.code(), Some(1));
    assert!(text(&o.stdout)
        .lines()
        .last()
        .unwrap()
        .starts_with("FAIL: handoff sidecar verification failed — "));
    let o = bin("handoff_sidecar")
        .args(["verify", "--file", &s(&site.dir.path().join("missing.raw"))])
        .output()?;
    assert_eq!(o.status.code(), Some(2));
    let o = bin("handoff_sidecar").args(["verify"]).output()?;
    assert_eq!(o.status.code(), Some(2), "usage error");
    Ok(())
}

#[test]
fn redact_writes_0600_without_the_recipient_and_refuses_overwrite() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    assert!(unseal_cmd(&site, &out, &["--recipient-name", "Dana Reyes"])
        .status
        .success());
    let sidecar = out.join("vault-1.raw.securacv-handoff.json");
    let redacted = site.dir.path().join("redacted.json");
    let redact = || {
        bin("handoff_sidecar")
            .args(["redact", "--sidecar", &s(&sidecar), "--out", &s(&redacted)])
            .output()
            .unwrap()
    };
    let o = redact();
    assert_eq!(o.status.code(), Some(0), "{}", text(&o.stderr));
    assert_eq!(
        text(&o.stdout),
        format!(
            "handoff sidecar written to {} without the recipient (recipient: committed_not_disclosed)\n",
            redacted.display()
        )
    );
    assert!(!String::from_utf8_lossy(&std::fs::read(&redacted)?).contains("Dana"));
    #[cfg(unix)]
    assert_eq!(mode(&redacted), 0o600);
    let o = redact();
    assert_eq!(o.status.code(), Some(2));
    assert!(text(&o.stderr).contains("refusing to overwrite"));
    // A pretty-printed sidecar is refused.
    let pretty = site.dir.path().join("pretty.json");
    let v: serde_json::Value = serde_json::from_slice(&std::fs::read(&sidecar)?)?;
    std::fs::write(&pretty, serde_json::to_vec_pretty(&v)?)?;
    let o = bin("handoff_sidecar")
        .args([
            "redact",
            "--sidecar",
            &s(&pretty),
            "--out",
            &s(&site.dir.path().join("z.json")),
        ])
        .output()?;
    assert!(text(&o.stderr).contains("the handoff sidecar is not canonical securacv-cjson-v1 bytes (re-emit it with handoff_sidecar emit; never edit or pretty-print it)"));
    Ok(())
}

/// `emit --grant` on an export:events grant refuses: that grant consented to
/// an event export, never to an unseal, and the export path (`emit --file
/// <bundle>`) is the one that describes it.
#[test]
fn emit_refuses_an_unseal_sidecar_over_an_export_grant() -> Result<()> {
    let site = site()?;
    let export_grant = {
        let c = cfg(&site.db);
        let mut kernel = Kernel::open(&c)?;
        let (token, granted) = grant(&mut kernel, &c, EXPORT_EVENTS_ENVELOPE_ID)?;
        kernel.redeem_token_durably(&token)?;
        granted
    };
    let file = site.dir.path().join("any.bin");
    std::fs::write(&file, b"any bytes")?;
    let out = site.dir.path().join("any.json");
    let o = emit(
        &site,
        &file,
        &["--grant", &hex::encode(export_grant), "--out", &s(&out)],
    );
    assert_eq!(o.status.code(), Some(2), "{}", text(&o.stdout));
    assert!(text(&o.stderr).contains(
        "an export:events redemption is not an unseal — exports are receipted in the export chain (emit the export's handoff sidecar with --file <bundle>)"
    ), "{}", text(&o.stderr));
    assert!(!out.exists(), "nothing is written");
    Ok(())
}

/// `emit --anchors-out` writes each referenced token as
/// `<DIR>/<token_sha256>.der`, create-new: a rerun over identical bytes is
/// fine, a different file at that name is refused and left as it is.
#[test]
fn emit_anchors_out_writes_each_token_by_its_sha256() -> Result<()> {
    let site = site()?;
    let out = site.dir.path().join("out");
    let o = unseal_cmd(&site, &out, &[]);
    assert!(o.status.success(), "{}", text(&o.stderr));
    let raw = out.join("vault-1.raw");
    // The TSA fixture token's imprint is sha256("securacv-fixture"), the
    // unsealed bytes: a digest anchor over the file.
    let token = witness_kernel::tsa::parse_response(include_bytes!("fixtures/tsa/reply.tsr"))?;
    {
        let kernel = Kernel::open(&cfg(&site.db))?;
        witness_kernel::tsa::ensure_anchor_table(&kernel.conn)?;
        let file_sha: [u8; 32] = <sha2::Sha256 as sha2::Digest>::digest(SECRET).into();
        witness_kernel::tsa::insert_anchor(
            &kernel.conn,
            "digest",
            &file_sha,
            "https://tsa.invalid",
            &token,
        )?;
    }
    let token_sha = hex::encode(<sha2::Sha256 as sha2::Digest>::digest(&token.token_der));
    let anchors = site.dir.path().join("anchors");
    let der = anchors.join(format!("{token_sha}.der"));
    let run = |name: &str| {
        emit(
            &site,
            &raw,
            &[
                "--grant",
                &hex::encode(site.grant),
                "--out",
                &s(&site.dir.path().join(name)),
                "--anchors-out",
                &s(&anchors),
            ],
        )
    };
    let o = run("first.json");
    assert_eq!(o.status.code(), Some(0), "{}", text(&o.stderr));
    assert!(text(&o.stdout).contains(
        "  anchors:   1 token(s) referenced (1 over the file bytes, 0 over the disclosure record)\n"
    ), "{}", text(&o.stdout));
    assert_eq!(std::fs::read(&der)?, token.token_der);
    #[cfg(unix)]
    assert_eq!(mode(&der), 0o600);
    // A rerun to a fresh --out over the identical token file succeeds.
    let o = run("second.json");
    assert_eq!(o.status.code(), Some(0), "{}", text(&o.stderr));
    assert_eq!(std::fs::read(&der)?, token.token_der);
    // A different file at that name is refused and left as it is.
    std::fs::remove_file(&der)?;
    std::fs::write(&der, b"not the token")?;
    let o = run("third.json");
    assert_ne!(o.status.code(), Some(0));
    assert!(
        text(&o.stderr).contains(&format!("a file already exists at {}", der.display())),
        "{}",
        text(&o.stderr)
    );
    assert_eq!(std::fs::read(&der)?, b"not the token");
    Ok(())
}

/// Re-running `export_events` on the same `--output` with a token that
/// cannot authorize anything fails before its bundle is written, and leaves
/// the earlier bundle's handoff sidecar (the only opening of its recipient
/// commitment) byte-identical.
#[test]
fn a_failed_export_rerun_keeps_the_earlier_sidecar() -> Result<()> {
    let site = site()?;
    let bundle = site.dir.path().join("bundle.json");
    let o = export_cmd(
        &site.db,
        &bundle,
        &["--self-export", "--recipient-name", "Pat Recipient"],
    );
    assert!(o.status.success(), "{}", text(&o.stderr));
    let sidecar = PathBuf::from(format!("{}.securacv-handoff.json", bundle.display()));
    let first_bundle = std::fs::read(&bundle)?;
    let first_sidecar = std::fs::read(&sidecar)?;
    let bad_token = site.dir.path().join("bad.token.json");
    std::fs::write(&bad_token, b"{\"not\":\"a token\"}")?;
    let o = export_cmd(&site.db, &bundle, &["--break-glass-token", &s(&bad_token)]);
    assert!(!o.status.success());
    assert_eq!(std::fs::read(&bundle)?, first_bundle);
    assert_eq!(
        std::fs::read(&sidecar)?,
        first_sidecar,
        "restored byte-identical"
    );
    let hidden: Vec<String> = std::fs::read_dir(site.dir.path())?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with(".bundle"))
        .collect();
    assert!(hidden.is_empty(), "nothing is left aside: {hidden:?}");
    Ok(())
}
