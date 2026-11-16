//! Tests for `src/handoff.rs`: the pinned commitment vectors, the sidecar's
//! canonical form and Invariant III, the verify state machine and its exact
//! lines, anchors selected by hash, the fail-closed release order, the
//! no-clobber writer, and the no-sockets source scan.

use super::*;
use crate::disclosure_tests::{fixture, Fixture};
use crate::EXPORT_EVENTS_ENVELOPE_ID;
use ed25519_dalek::SigningKey;

const BUCKET: TimeBucket = TimeBucket {
    start_epoch_s: 1_794_177_000,
    size_s: 600,
};
const PLAINTEXT: &[u8] = b"securacv-fixture";

fn dana() -> HandoffRecipient {
    HandoffRecipient::new("Dana Reyes", Some("Example County Records Office"), None).unwrap()
}

/// A recording [`ReleaseIo`] whose writes can be made to fail.
#[derive(Default)]
struct FakeIo {
    fail_sidecar: bool,
    fail_raw: bool,
    sidecar: Option<(PathBuf, Vec<u8>)>,
    raw_written: bool,
    removed: Vec<PathBuf>,
}

impl ReleaseIo for FakeIo {
    fn write_sidecar(&mut self, path: &Path, bytes: &[u8]) -> Result<()> {
        if self.fail_sidecar {
            return Err(anyhow!("disk full (simulated)"));
        }
        self.sidecar = Some((path.to_path_buf(), bytes.to_vec()));
        Ok(())
    }
    fn write_raw(&mut self, _path: &Path, _bytes: &[u8]) -> Result<()> {
        if self.fail_raw {
            return Err(anyhow!("disk full (simulated)"));
        }
        self.raw_written = true;
        Ok(())
    }
    fn remove(&mut self, path: &Path) {
        self.removed.push(path.to_path_buf());
    }
}

struct Unsealed {
    f: Fixture,
    dir: tempfile::TempDir,
    out: UnsealOutput,
    grant: [u8; 32],
    redemption: [u8; 32],
}

fn unseal(recipient: Option<&HandoffRecipient>) -> Result<Unsealed> {
    let mut f = fixture()?;
    let (grant, redemption) = f.redeemed("vault:1")?;
    let dir = tempfile::tempdir()?;
    let raw = dir.path().join("vault-1.raw");
    let out = release_unsealed(
        &mut f.kernel,
        PLAINTEXT.to_vec(),
        &UnsealRelease {
            raw_path: &raw,
            redemption_entry_hash: redemption,
            granted_entry_hash: grant,
            recipient,
            created_bucket: BUCKET,
            tool_version: "test",
        },
        &mut FsReleaseIo,
    )?;
    Ok(Unsealed {
        f,
        dir,
        out,
        grant,
        redemption,
    })
}

fn pinned(f: &Fixture) -> VerifyOptions<'static> {
    VerifyOptions {
        pinned: Some(f.kernel.device_verifying_key()),
        anchors_dir: None,
        tsa_cas: &[],
    }
}

fn lines(report: &SidecarReport) -> Vec<String> {
    report.checks.iter().map(Check::line).collect()
}

fn has_line(report: &SidecarReport, want: &str) -> bool {
    lines(report).iter().any(|l| l == want)
}

/// Re-canonicalize an edited sidecar (it is unsigned: anyone can), so only
/// the content checks decide.
fn edited(bytes: &[u8], edit: impl FnOnce(&mut HandoffSidecar)) -> Vec<u8> {
    let mut s: HandoffSidecar = serde_json::from_slice(bytes).unwrap();
    edit(&mut s);
    s.to_canonical_bytes().unwrap()
}

#[test]
fn recipient_commitment_matches_the_pinned_vector() {
    // The salt of a pinned vector (the same bytes the JS suite and the shell
    // recipe pin): a test vector, not a deployed secret.
    // codeql[rust/hard-coded-cryptographic-value]
    let salt = [0x11; 32];
    assert_eq!(
        hex::encode(recipient_commitment(&salt, &dana())),
        "5d02af2bfc1a29471141dbb6b118f2b2d1af259e7f4dcf8681d9dfc8e9e598ce"
    );
    assert_eq!(DOMAIN_HANDOFF_RECIPIENT.len(), 33);
}

#[test]
fn plaintext_commitment_matches_the_pinned_vector() -> Result<()> {
    // The salt of a pinned vector (the same bytes the JS suite and the shell
    // recipe pin): a test vector, not a deployed secret.
    // codeql[rust/hard-coded-cryptographic-value]
    let salt = [0x22; 32];
    let c = plaintext_commitment(&salt, PLAINTEXT);
    assert_eq!(
        hex::encode(c),
        "926a5596f3ec19a0f56ad186d1e7a28a444694a184d506e37b28685e625da1cb"
    );
    assert_eq!(DOMAIN_UNSEALED_PLAINTEXT.len(), 34);
    assert_eq!(
        salted_commitment_reader(DOMAIN_UNSEALED_PLAINTEXT, &salt, PLAINTEXT)?,
        c
    );
    let big = vec![7u8; 200_000];
    // Any salt shows the reader and the slice form agree: random, never a literal.
    let any_salt: [u8; 32] = rand::random();
    assert_eq!(
        salted_commitment_reader(DOMAIN_UNSEALED_PLAINTEXT, &any_salt, &big[..])?,
        salted_commitment(DOMAIN_UNSEALED_PLAINTEXT, &any_salt, &big)
    );
    Ok(())
}

#[test]
fn recipient_cjson_sorts_and_omits_absent_and_equals_jq_cjs() -> Result<()> {
    assert_eq!(
        dana().canonical_bytes(),
        br#"{"name":"Dana Reyes","organization":"Example County Records Office"}"#
    );
    let keyed = HandoffRecipient::new("Zoë \"Q\" O'Neil \\ Søren", None, Some(&"AB".repeat(32)))?;
    assert_eq!(
        String::from_utf8(keyed.canonical_bytes())?,
        format!(
            r#"{{"key":"{}","name":"Zoë \"Q\" O'Neil \\ Søren"}}"#,
            "ab".repeat(32)
        )
    );
    // The bytes equal `jq -cjS .recipient` over a sidecar naming them: jq
    // sorts keys, prints compactly, and escapes exactly `"` and `\` among
    // the characters check_text_field admits.
    for r in [dana(), keyed] {
        let value = serde_json::to_value(&r)?;
        let sorted: std::collections::BTreeMap<String, serde_json::Value> =
            serde_json::from_value(value)?;
        assert_eq!(serde_json::to_vec(&sorted)?, r.canonical_bytes());
    }
    Ok(())
}

#[test]
fn recipient_validation() -> Result<()> {
    assert_eq!(
        HandoffRecipient::new("   ", None, None)
            .unwrap_err()
            .to_string(),
        "recipient name cannot be empty"
    );
    let err = HandoffRecipient::new("Dana\u{202e}", None, None).unwrap_err();
    assert!(
        err.to_string()
            .starts_with("recipient name contains a control"),
        "{err}"
    );
    let err = HandoffRecipient::new("Dana", Some("Org\nX"), None).unwrap_err();
    assert!(
        err.to_string()
            .starts_with("recipient organization contains"),
        "{err}"
    );
    assert_eq!(
        HandoffRecipient::new("Dana", None, Some("abcd"))
            .unwrap_err()
            .to_string(),
        "recipient key must be 32 bytes (Ed25519 public key, 64 hex), got 2"
    );
    let r = HandoffRecipient::new(" Dana ", Some(" "), Some(&"CD".repeat(32)))?;
    assert_eq!(r.name, "Dana");
    assert_eq!(r.organization, None);
    assert_eq!(r.key.as_deref(), Some("cd".repeat(32).as_str()));

    let dir = tempfile::tempdir()?;
    let path = dir.path().join("recipient.json");
    std::fs::write(&path, r#"{"name":"Secret Name","nickname":"Hidden Value"}"#)?;
    let err = HandoffRecipient::from_file(&path).unwrap_err().to_string();
    assert!(
        err.starts_with(&format!("recipient file {}:", path.display())),
        "{err}"
    );
    assert!(
        !err.contains("Secret Name") && !err.contains("Hidden Value") && !err.contains("nickname"),
        "{err}"
    );
    std::fs::write(
        &path,
        r#"{"name":"Dana Reyes","organization":"Example County Records Office"}"#,
    )?;
    assert_eq!(HandoffRecipient::from_file(&path)?, dana());
    Ok(())
}

#[test]
fn sidecar_is_canonical_without_trailing_newline() -> Result<()> {
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    assert_ne!(bytes.last(), Some(&b'\n'));
    let parsed = HandoffSidecar::parse(&bytes)?;
    assert_eq!(parsed.to_canonical_bytes()?, bytes);
    let pretty = serde_json::to_vec_pretty(&parsed)?;
    assert_eq!(
        HandoffSidecar::parse(&pretty).unwrap_err().to_string(),
        NOT_CANONICAL
    );
    let mut newline = bytes.clone();
    newline.push(b'\n');
    assert!(HandoffSidecar::parse(&newline).is_err());
    Ok(())
}

fn walk_times(v: &serde_json::Value, path: &str, out: &mut Vec<String>) {
    match v {
        serde_json::Value::Object(map) => {
            for (k, child) in map {
                let p = format!("{path}.{k}");
                let lower = k.to_ascii_lowercase();
                assert!(
                    !(lower == "created_at"
                        || lower == "gen_time"
                        || lower == "id"
                        || lower == "seq"
                        || lower.ends_with("_at")
                        || lower.starts_with("token_der")),
                    "forbidden key {p}"
                );
                if ["created_bucket", "time_bucket", "request_bucket", "window"]
                    .contains(&k.as_str())
                    && !child.is_null()
                {
                    for (field, n) in child.as_object().expect("bucket object") {
                        let n = n.as_u64().expect("integer");
                        assert_eq!(n % 600, 0, "{p}.{field} = {n} is not on the 600 s grid");
                    }
                    out.push(p.clone());
                }
                // Embedded payloads are strings: walk them parsed too.
                if k == "payload_json" || k == "approvals_json" {
                    if let Some(s) = child.as_str() {
                        if let Ok(inner) = serde_json::from_str::<serde_json::Value>(s) {
                            walk_times(&inner, &p, out);
                        }
                    }
                }
                walk_times(child, &p, out);
            }
        }
        serde_json::Value::Array(items) => {
            for (i, child) in items.iter().enumerate() {
                walk_times(child, &format!("{path}[{i}]"), out);
            }
        }
        _ => {}
    }
}

#[test]
fn sidecar_carries_no_precise_time() -> Result<()> {
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let v: serde_json::Value = serde_json::from_slice(&bytes)?;
    let mut buckets = Vec::new();
    walk_times(&v, "", &mut buckets);
    assert!(buckets.contains(&".created_bucket".to_string()));
    assert!(v["anchors"].as_array().expect("anchors").is_empty());
    Ok(())
}

#[test]
fn unseal_sidecar_verifies_self_consistent_and_valid_when_pinned() -> Result<()> {
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let report = verify_sidecar(&bytes, &u.out.raw_path, &VerifyOptions::default())?;
    assert_eq!(report.failed(), 0, "{:#?}", lines(&report));
    assert_eq!(report.verdict(), "self-consistent; identity unverified");
    let report = verify_sidecar(&bytes, &u.out.raw_path, &pinned(&u.f))?;
    assert_eq!(report.verdict(), "valid", "{:#?}", lines(&report));
    let fp = &hex::encode(u.f.kernel.device_verifying_key().to_bytes())[..16];
    for want in [
        "OK         format: securacv-handoff:v1, canonical (securacv-cjson-v1)".to_string(),
        format!(
            "OK         file: sha256 {} and 16 bytes match the sidecar",
            hex::encode(Sha256::digest(PLAINTEXT))
        ),
        format!("OK         record grant: entry hash re-derives; device signature checks under {fp}…"),
        "OK         links: the unseal record names the redemption, and the redemption names the grant".to_string(),
        "OK         consent: the grant's recorded request context re-derives the request hash its 1 trustee(s) approved (alice)".to_string(),
        "UNVERIFIED consent: the quorum behind the grant is re-derived against its policy era only with --db".to_string(),
        "OK         pairing: this file opens the plaintext commitment in the device-signed unseal record".to_string(),
        "OK         recipient: opens the commitment in the signed unseal record — declared by the operator at disclosure, not verified by the device".to_string(),
    ] {
        assert!(has_line(&report, &want), "missing {want:?} in {:#?}", lines(&report));
    }
    assert_eq!(
        report.verdict_line(),
        "OK: handoff sidecar checked — VALID (identity anchored out-of-band). 0 item(s) are declared, not attested by the device."
    );
    let sidecar = HandoffSidecar::parse(&bytes)?;
    assert_eq!(sidecar.records[0].entry_hash, hex::encode(u.grant));
    assert_eq!(sidecar.records[1].entry_hash, hex::encode(u.redemption));
    assert_eq!(sidecar.created_bucket, BUCKET);
    Ok(())
}

#[test]
fn tamper_matrix_gives_exact_fail_lines() -> Result<()> {
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let opts = pinned(&u.f);
    let fp = &hex::encode(u.f.kernel.device_verifying_key().to_bytes())[..16];

    // A file byte flip.
    let flipped = u.dir.path().join("flipped.raw");
    let mut data = PLAINTEXT.to_vec();
    data[0] ^= 1;
    std::fs::write(&flipped, &data)?;
    let report = verify_sidecar(&bytes, &flipped, &opts)?;
    assert!(has_line(&report, &format!(
        "FAIL       file: sha256 {} does not match the sidecar's {} — this sidecar does not describe this file",
        hex::encode(Sha256::digest(&data)),
        hex::encode(Sha256::digest(PLAINTEXT))
    )), "{:#?}", lines(&report));
    assert!(has_line(&report, "FAIL       pairing: this file does not open the plaintext commitment in the signed unseal record — wrong file or wrong salt"));

    // Non-canonical whitespace.
    let mut spaced = bytes.clone();
    spaced.insert(1, b' ');
    let report = verify_sidecar(&spaced, &u.out.raw_path, &opts)?;
    assert!(has_line(&report, "FAIL       format: not in canonical securacv-cjson-v1 form — the sidecar was edited after it was written (re-emit it; never edit or pretty-print it)"));

    // An edited payload.
    let tampered = edited(&bytes, |s| s.records[0].payload_json.push(' '));
    let report = verify_sidecar(&tampered, &u.out.raw_path, &opts)?;
    assert!(has_line(
        &report,
        "FAIL       record grant: entry hash does not re-derive from prev_hash and payload_json"
    ));

    // A flipped signature.
    let tampered = edited(&bytes, |s| {
        let mut sig = hex::decode(&s.records[1].signature).unwrap();
        sig[0] ^= 1;
        s.records[1].signature = hex::encode(sig);
    });
    let report = verify_sidecar(&tampered, &u.out.raw_path, &opts)?;
    assert!(has_line(
        &report,
        &format!("FAIL       record redemption: device signature does not check under {fp}…")
    ));

    // A swapped recipient.
    let tampered = edited(&bytes, |s| {
        s.recipient = Some(HandoffRecipient::new("Someone Else", None, None).unwrap())
    });
    let report = verify_sidecar(&tampered, &u.out.raw_path, &opts)?;
    assert!(has_line(
        &report,
        "FAIL       recipient: does not open the commitment in the signed unseal record"
    ));

    // A wrong recipient_binding, a wrong pairing.
    let tampered = edited(&bytes, |s| s.recipient_binding = RecipientBinding::Declared);
    let report = verify_sidecar(&tampered, &u.out.raw_path, &opts)?;
    assert!(has_line(&report, "FAIL       binding: the sidecar says recipient_binding=declared; its contents support receipt_commitment"));
    let tampered = edited(&bytes, |s| s.pairing = Pairing::Declared);
    let report = verify_sidecar(&tampered, &u.out.raw_path, &opts)?;
    assert!(has_line(&report, "FAIL       binding: the sidecar says pairing=declared; its contents support plaintext_commitment"));

    // An unknown top-level key.
    let mut v: serde_json::Value = serde_json::from_slice(&bytes)?;
    v["emitted_by"] = serde_json::json!("someone");
    let extra = crate::canonical_json::to_canonical_bytes(&v)?;
    let report = verify_sidecar(&extra, &u.out.raw_path, &opts)?;
    let first = report.first_failure().expect("fail");
    assert_eq!(first.subject, "format");
    assert!(
        first
            .detail
            .starts_with("unknown key or malformed: unknown field `emitted_by`"),
        "{}",
        first.detail
    );

    // A different pinned key.
    let other = SigningKey::from_bytes(&[3; 32]).verifying_key();
    let report = verify_sidecar(
        &bytes,
        &u.out.raw_path,
        &VerifyOptions {
            pinned: Some(other),
            ..Default::default()
        },
    )?;
    assert!(has_line(&report, &format!("FAIL       device key: the sidecar's records name key {fp}…, not the key you supplied (after a key rotation, pass --db to check the lineage)")));
    assert_eq!(report.verdict(), "verification failed");
    assert!(report
        .verdict_line()
        .starts_with("FAIL: handoff sidecar verification failed — "));
    Ok(())
}

/// An `export:events` grant consented to an event export, never to an
/// unseal: `emit --grant` refuses to build an unseal sidecar over one, and a
/// hand-built unseal sidecar embedding one fails `links` even with no
/// `Unsealed` row (where the walk's own U3 never fires).
#[test]
fn an_unseal_sidecar_over_an_export_grant_is_refused_and_fails_links() -> Result<()> {
    let mut u = unseal(None)?;
    let (export_grant, export_redemption) = u.f.redeemed(EXPORT_EVENTS_ENVELOPE_ID)?;
    let file = HandoffFile {
        bytes: PLAINTEXT.len() as u64,
        name: "vault-1.raw".to_string(),
        sha256: hex::encode(Sha256::digest(PLAINTEXT)),
    };
    let key = u.f.kernel.device_verifying_key();
    for build in [
        build_unseal_sidecar(
            &u.f.kernel.conn,
            &key,
            UnsealSelector::Grant(export_grant),
            &file,
            None,
            None,
            BUCKET,
            "test",
        ),
        build_unseal_sidecar_from_db(
            &u.f.kernel.conn,
            UnsealSelector::Grant(export_grant),
            &file,
            None,
            BUCKET,
            "test",
        ),
    ] {
        assert_eq!(
            build.unwrap_err().to_string(),
            "an export:events redemption is not an unseal — exports are receipted in the export \
             chain (emit the export's handoff sidecar with --file <bundle>)"
        );
    }

    // Hand-build one: the real vault:1 unseal sidecar with its grant and
    // redemption swapped for the export grant's rows and no unseal record.
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let row = |h: &[u8; 32], role| {
        record_from_row(
            &crate::break_glass_row_by_entry_hash(&u.f.kernel.conn, h)
                .unwrap()
                .expect("row"),
            role,
        )
    };
    let records = vec![
        row(&export_grant, RecordRole::Grant),
        row(&export_redemption, RecordRole::Redemption),
    ];
    let forged = edited(&bytes, |s| {
        s.records = records;
        s.pairing = Pairing::Declared;
        s.salts.plaintext = None;
    });
    let opts = pinned(&u.f);
    let report = verify_sidecar(&forged, &u.out.raw_path, &opts)?;
    assert!(
        has_line(
            &report,
            "FAIL       links: an unseal sidecar embeds the redemption of an export:events token"
        ),
        "{:#?}",
        lines(&report)
    );
    assert!(!has_line(
        &report,
        "OK         links: the redemption names the grant"
    ));
    assert_eq!(report.verdict(), "verification failed");
    let report = verify_sidecar_with_db(&u.f.kernel.conn, &forged, &u.out.raw_path, &opts)?;
    assert_eq!(report.verdict(), "verification failed");
    Ok(())
}

#[test]
fn four_recipient_bindings_round_trip_through_build_and_verify() -> Result<()> {
    // receipt_commitment: the unseal named and committed one.
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let s = HandoffSidecar::parse(&bytes)?;
    assert_eq!(s.recipient_binding, RecipientBinding::ReceiptCommitment);
    // committed_not_disclosed: emit from the database after the fact.
    let file = s.file.clone();
    let emitted = build_unseal_sidecar_from_db(
        &u.f.kernel.conn,
        UnsealSelector::UnsealRecord(u.out.unsealed_entry_hash),
        &file,
        None,
        BUCKET,
        "test",
    )?;
    assert_eq!(
        emitted.recipient_binding,
        RecipientBinding::CommittedNotDisclosed
    );
    assert_eq!(emitted.pairing, Pairing::Declared);
    let report = verify_sidecar(
        &emitted.to_canonical_bytes()?,
        &u.out.raw_path,
        &pinned(&u.f),
    )?;
    assert_eq!(report.failed(), 0, "{:#?}", lines(&report));
    assert!(has_line(&report, "OK         recipient: the signed unseal record commits to a recipient this sidecar does not disclose"));
    assert!(has_line(&report, "DECLARED   pairing: no device-signed record this sidecar can open binds this file to the envelope; the tool that wrote the sidecar says it does"));
    // A recipient beside a committing row is refused.
    let err = build_unseal_sidecar_from_db(
        &u.f.kernel.conn,
        UnsealSelector::UnsealRecord(u.out.unsealed_entry_hash),
        &file,
        Some(&dana()),
        BUCKET,
        "test",
    )
    .unwrap_err();
    assert_eq!(err.to_string(), "the signed unseal record commits to a recipient; its salt existed only in the original handoff sidecar, so this tool cannot name a recipient beside it — omit the recipient flags (the sidecar records the recipient as committed but not disclosed)");

    // none, then declared: an unseal that named nobody.
    let u = unseal(None)?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let report = verify_sidecar(&bytes, &u.out.raw_path, &pinned(&u.f))?;
    assert!(has_line(&report, "OK         recipient: none named"));
    let s = HandoffSidecar::parse(&bytes)?;
    let declared = build_unseal_sidecar_from_db(
        &u.f.kernel.conn,
        UnsealSelector::Grant(u.grant),
        &s.file,
        Some(&dana()),
        BUCKET,
        "test",
    )?;
    assert_eq!(declared.recipient_binding, RecipientBinding::Declared);
    let report = verify_sidecar(
        &declared.to_canonical_bytes()?,
        &u.out.raw_path,
        &pinned(&u.f),
    )?;
    assert!(has_line(&report, "DECLARED   recipient: Dana Reyes (Example County Records Office) — named in this sidecar only; no signed record commits to it"));
    assert_eq!(report.declared(), 2);
    Ok(())
}

#[test]
fn pairing_declared_without_salt_and_fail_with_wrong_salt() -> Result<()> {
    let u = unseal(None)?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let wrong = edited(&bytes, |s| s.salts.plaintext = Some(hex::encode([9u8; 32])));
    let report = verify_sidecar(&wrong, &u.out.raw_path, &pinned(&u.f))?;
    assert!(has_line(&report, "FAIL       pairing: this file does not open the plaintext commitment in the signed unseal record — wrong file or wrong salt"));
    let no_salt = edited(&bytes, |s| {
        s.salts.plaintext = None;
        s.pairing = Pairing::Declared;
    });
    let report = verify_sidecar(&no_salt, &u.out.raw_path, &pinned(&u.f))?;
    assert_eq!(report.failed(), 0, "{:#?}", lines(&report));
    assert_eq!(report.declared(), 1);
    Ok(())
}

#[test]
fn export_sidecar_verifies_and_a_swapped_bundle_fails_pairing() -> Result<()> {
    let mut f = fixture()?;
    let (mut token, granted) = f.grant(EXPORT_EVENTS_ENVELOPE_ID)?;
    let salt: [u8; 32] = rand::random();
    let bundle = f.kernel.export_events_bundle_authorized_with(
        f.cfg.ruleset_hash,
        crate::ExportOptions::default(),
        &mut token,
        Some(recipient_commitment(&salt, &dana())),
    )?;
    let bundle_bytes = serde_json::to_vec(&bundle)?;
    let sidecar = build_export_sidecar(
        &f.kernel.conn,
        &bundle_bytes,
        "bundle.json",
        Some((&dana(), Some(salt))),
        BUCKET,
        "test",
    )?;
    assert_eq!(sidecar.records.len(), 3);
    assert_eq!(sidecar.records[0].entry_hash, hex::encode(granted));
    let bytes = sidecar.to_canonical_bytes()?;
    let dir = tempfile::tempdir()?;
    let path = dir.path().join("bundle.json");
    std::fs::write(&path, &bundle_bytes)?;
    let report = verify_sidecar(&bytes, &path, &pinned(&f))?;
    assert_eq!(report.verdict(), "valid", "{:#?}", lines(&report));
    assert!(has_line(&report, "OK         pairing: this file is the export bundle whose receipt is the embedded export record"));
    assert!(has_line(
        &report,
        "OK         links: the export receipt names the grant, and the grant was redeemed once"
    ));
    assert!(has_line(&report, "OK         recipient: opens the commitment in the signed export receipt — declared by the operator at disclosure, not verified by the device"));
    // Another bundle from the same device: the pairing fails.
    let other = f
        .kernel
        .export_events_bundle_self(f.cfg.ruleset_hash, crate::ExportOptions::default())?;
    let other_bytes = serde_json::to_vec(&other)?;
    let swapped = edited(&bytes, |s| {
        s.file.sha256 = hex::encode(Sha256::digest(&other_bytes));
        s.file.bytes = other_bytes.len() as u64;
    });
    std::fs::write(&path, &other_bytes)?;
    let report = verify_sidecar(&swapped, &path, &pinned(&f))?;
    assert!(has_line(&report, "FAIL       pairing: this file is not the export bundle whose receipt is the embedded export record (its receipt is a different export receipt)"), "{:#?}", lines(&report));
    Ok(())
}

/// The TSA fixture token (imprint = sha256("securacv-fixture")), and the
/// same DER with its imprint patched to `subject` — structurally a token
/// over `subject`, its countersignature no longer valid (selection and
/// imprint checks are structural; `--ca` is the countersignature check).
fn tokens(subject: &[u8; 32]) -> (crate::tsa::TimestampToken, crate::tsa::TimestampToken) {
    let reply = include_bytes!("../tests/fixtures/tsa/reply.tsr");
    let real = crate::tsa::parse_response(reply).unwrap();
    let imprint = Sha256::digest(PLAINTEXT);
    let mut der = real.token_der.clone();
    let at = der
        .windows(32)
        .position(|w| w == imprint.as_slice())
        .expect("imprint in token");
    der[at..at + 32].copy_from_slice(subject);
    let patched = crate::tsa::parse_token(&der).unwrap();
    (real, patched)
}

#[test]
fn anchors_selected_by_hash_only() -> Result<()> {
    let u = unseal(None)?;
    let conn = &u.f.kernel.conn;
    crate::tsa::ensure_anchor_table(conn)?;
    let file_sha: [u8; 32] = Sha256::digest(PLAINTEXT).into();
    let (real, head) = tokens(&u.out.unsealed_entry_hash);
    // A digest anchor over the file, a chain-head anchor over the unseal
    // record (NOT excluded for being a head), an unrelated digest, and a
    // row whose token does not cover its subject.
    crate::tsa::insert_anchor(conn, "digest", &file_sha, "https://tsa.invalid", &real)?;
    crate::tsa::insert_anchor(
        conn,
        "break_glass_receipt_head",
        &u.out.unsealed_entry_hash,
        "https://tsa.invalid",
        &head,
    )?;
    crate::tsa::insert_anchor(conn, "digest", &[7; 32], "https://tsa.invalid", &real)?;
    crate::tsa::insert_anchor(conn, "digest", &file_sha, "https://tsa.invalid", &head)?;
    let selected = select_anchors(conn, &file_sha, &u.out.unsealed_entry_hash)?;
    assert_eq!(selected.len(), 2, "{selected:?}");
    let real_sha = hex::encode(Sha256::digest(&real.token_der));
    let head_sha = hex::encode(Sha256::digest(&head.token_der));
    assert_eq!(
        selected[0].0,
        serde_json::json!({"covers": "file_bytes", "kind": "rfc3161", "subject_hash": hex::encode(file_sha), "token_sha256": real_sha})
    );
    assert_eq!(selected[1].0["covers"], "disclosure_record");
    assert_eq!(selected[1].0["token_sha256"], head_sha.as_str());

    // emit references both; verify resolves them from --anchors-dir.
    let s = HandoffSidecar::parse(&std::fs::read(&u.out.sidecar_path)?)?;
    let emitted = build_unseal_sidecar_from_db(
        conn,
        UnsealSelector::UnsealRecord(u.out.unsealed_entry_hash),
        &s.file,
        None,
        BUCKET,
        "test",
    )?;
    assert_eq!(emitted.anchors.len(), 2);
    let anchors = tempfile::tempdir()?;
    for (i, (_, der)) in selected.iter().enumerate() {
        std::fs::write(anchors.path().join(format!("token-{i}.der")), der)?;
    }
    std::fs::write(anchors.path().join("unrelated.der"), b"nope")?;
    let bytes = emitted.to_canonical_bytes()?;
    let report = verify_sidecar(
        &bytes,
        &u.out.raw_path,
        &VerifyOptions {
            pinned: Some(u.f.kernel.device_verifying_key()),
            anchors_dir: Some(anchors.path()),
            tsa_cas: &[],
        },
    )?;
    assert!(has_line(&report, &format!("UNVERIFIED anchor (rfc3161, covers the file bytes): token {}… found; imprint matches; countersignature not checked (pass --ca <tsa-ca.pem>)", &real_sha[..16])), "{:#?}", lines(&report));
    assert!(has_line(&report, &format!("UNVERIFIED anchor (rfc3161, covers the disclosure record): token {}… found; imprint matches; countersignature not checked (pass --ca <tsa-ca.pem>)", &head_sha[..16])));
    // Without the directory: not supplied.
    let report = verify_sidecar(&bytes, &u.out.raw_path, &pinned(&u.f))?;
    assert!(has_line(&report, &format!("UNVERIFIED anchor (rfc3161, covers the file bytes): token {}… not supplied (pass --db or --anchors-dir)", &real_sha[..16])));
    // An imprint mismatch FAILs at verify.
    let lying = edited(&bytes, |s| {
        s.anchors = vec![
            serde_json::json!({"covers": "file_bytes", "kind": "rfc3161", "subject_hash": hex::encode(file_sha), "token_sha256": head_sha}),
        ];
    });
    let report = verify_sidecar(
        &lying,
        &u.out.raw_path,
        &VerifyOptions {
            pinned: Some(u.f.kernel.device_verifying_key()),
            anchors_dir: Some(anchors.path()),
            tsa_cas: &[],
        },
    )?;
    assert!(has_line(&report, &format!("FAIL       anchor (rfc3161, covers the file bytes): token {}… covers {}…, not the file's sha256", &head_sha[..16], &hex::encode(u.out.unsealed_entry_hash)[..16])), "{:#?}", lines(&report));
    // An unknown kind never fails.
    let ots = edited(&bytes, |s| {
        s.anchors
            .push(serde_json::json!({"kind": "opentimestamps", "proof_sha256": "00"}))
    });
    let report = verify_sidecar(&ots, &u.out.raw_path, &pinned(&u.f))?;
    assert!(has_line(
        &report,
        "UNVERIFIED anchor (opentimestamps): this build does not check that anchor kind"
    ));
    assert_eq!(report.failed(), 0);
    // The countersignature, through the local openssl.
    if crate::anchor_policy::openssl_available() {
        let ca = vec!["tests/fixtures/tsa/tsa.crt".to_string()];
        let report = verify_sidecar(
            &bytes,
            &u.out.raw_path,
            &VerifyOptions {
                pinned: Some(u.f.kernel.device_verifying_key()),
                anchors_dir: Some(anchors.path()),
                tsa_cas: &ca,
            },
        )?;
        assert!(has_line(&report, &format!("OK         anchor (rfc3161, covers the file bytes): token {}… found; imprint matches; countersignature checks under tests/fixtures/tsa/tsa.crt", &real_sha[..16])), "{:#?}", lines(&report));
    } else {
        println!("skipped: openssl is not on PATH (the countersignature leg)");
    }
    Ok(())
}

#[test]
fn verify_with_db_ok_missing_row_and_tampered_ledger() -> Result<()> {
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let conn = &u.f.kernel.conn;
    let report = verify_sidecar_with_db(conn, &bytes, &u.out.raw_path, &pinned(&u.f))?;
    assert_eq!(report.verdict(), "valid", "{:#?}", lines(&report));
    assert!(report.db_checked);
    assert!(has_line(&report, "OK         database: every embedded record is a row of this database, both receipt chains verify end to end, the grant's quorum re-derives, and the device key is in its lineage"));
    assert!(!lines(&report).iter().any(|l| l.contains("only with --db")));
    // The unseal row dropped from the chain tail.
    conn.execute(
        "DELETE FROM break_glass_receipts WHERE entry_hash = ?1",
        rusqlite::params![u.out.unsealed_entry_hash.to_vec()],
    )?;
    let report = verify_sidecar_with_db(conn, &bytes, &u.out.raw_path, &pinned(&u.f))?;
    assert!(has_line(&report, "FAIL       database: record unsealed is not in this database's break_glass_receipts (the sidecar did not come from this database, or the row was removed)"), "{:#?}", lines(&report));
    // A tampered ledger row.
    conn.execute(
        "UPDATE break_glass_receipts SET payload_json = payload_json || ' ' WHERE entry_hash = ?1",
        rusqlite::params![u.grant.to_vec()],
    )?;
    let report = verify_sidecar_with_db(conn, &bytes, &u.out.raw_path, &pinned(&u.f))?;
    assert!(
        lines(&report).iter().any(|l| l.starts_with(
            "FAIL       database: this database's break_glass_receipts does not verify ("
        )),
        "{:#?}",
        lines(&report)
    );
    Ok(())
}

#[test]
fn redact_drops_recipient_and_keeps_pairing() -> Result<()> {
    let u = unseal(Some(&dana()))?;
    let bytes = std::fs::read(&u.out.sidecar_path)?;
    let s = HandoffSidecar::parse(&bytes)?;
    let r = redact(&s);
    assert_eq!(r.recipient, None);
    assert_eq!(r.salts.recipient, None);
    assert_eq!(r.salts.plaintext, s.salts.plaintext);
    assert_eq!(r.recipient_binding, RecipientBinding::CommittedNotDisclosed);
    assert_eq!(r.pairing, Pairing::PlaintextCommitment);
    let out = r.to_canonical_bytes()?;
    assert!(!String::from_utf8_lossy(&out).contains("Dana"));
    let report = verify_sidecar(&out, &u.out.raw_path, &pinned(&u.f))?;
    assert_eq!(report.verdict(), "valid", "{:#?}", lines(&report));
    let mut declared = s.clone();
    declared.recipient_binding = RecipientBinding::Declared;
    assert_eq!(redact(&declared).recipient_binding, RecipientBinding::None);
    Ok(())
}

/// Every 64-hex run in `text` is one of `known` (no salt leaks).
fn only_known_hex(text: &str, known: &[[u8; 32]]) {
    let known: Vec<String> = known.iter().map(hex::encode).collect();
    let chars: Vec<char> = text.chars().collect();
    let mut i = 0;
    while i < chars.len() {
        if chars[i].is_ascii_hexdigit() {
            let start = i;
            while i < chars.len() && chars[i].is_ascii_hexdigit() {
                i += 1;
            }
            let run: String = chars[start..i].iter().collect();
            if run.len() >= 32 {
                assert!(
                    known.contains(&run),
                    "unexpected hex in error: {run} ({text})"
                );
            }
        } else {
            i += 1;
        }
    }
}

#[test]
fn release_unsealed_is_sidecar_first_and_fail_closed() -> Result<()> {
    let run =
        |io: &mut FakeIo, grant_as_redemption: bool| -> Result<(String, [u8; 32], [u8; 32])> {
            let mut f = fixture()?;
            let (grant, redemption) = f.redeemed("vault:1")?;
            let dir = tempfile::tempdir()?;
            let raw = dir.path().join("vault-1.raw");
            let err = release_unsealed(
                &mut f.kernel,
                PLAINTEXT.to_vec(),
                &UnsealRelease {
                    raw_path: &raw,
                    redemption_entry_hash: if grant_as_redemption {
                        grant
                    } else {
                        redemption
                    },
                    granted_entry_hash: grant,
                    recipient: Some(&dana()),
                    created_bucket: BUCKET,
                    tool_version: "test",
                },
                io,
            )
            .unwrap_err()
            .to_string();
            assert!(
                !err.contains("Dana") && !err.contains("Example County"),
                "{err}"
            );
            Ok((err, grant, redemption))
        };
    // (a) the sidecar write fails: no .raw.
    let mut io = FakeIo {
        fail_sidecar: true,
        ..FakeIo::default()
    };
    let (err, grant, redemption) = run(&mut io, false)?;
    assert!(!io.raw_written);
    assert!(
        err.starts_with("the unseal is recorded (unseal record "),
        "{err}"
    );
    assert!(err.ends_with("(disk full (simulated)); the cleartext was discarded and nothing was written, because the salt that opens the signed record exists only in that sidecar. The token is spent — the trustees must approve a new request to unseal again."), "{err}");
    only_known_hex(&err, &[grant, redemption]);
    // (b) the .raw write fails: the sidecar is removed.
    let mut io = FakeIo {
        fail_raw: true,
        ..FakeIo::default()
    };
    let (err, grant, redemption) = run(&mut io, false)?;
    let (sidecar_path, _) = io.sidecar.clone().expect("sidecar written first");
    assert_eq!(io.removed, vec![sidecar_path]);
    assert!(
        err.starts_with("the cleartext could not be written to "),
        "{err}"
    );
    assert!(err.ends_with("(disk full (simulated)); nothing was released and its handoff sidecar was removed. The unseal record stays in the receipt chain (the device did decrypt the envelope). The token is spent — the trustees must approve a new request to unseal again."), "{err}");
    only_known_hex(&err, &[grant, redemption]);
    // (c) record_unsealed fails: nothing written at all.
    let mut io = FakeIo::default();
    let (err, grant, redemption) = run(&mut io, true)?;
    assert!(io.sidecar.is_none() && !io.raw_written);
    assert_eq!(
        err,
        format!(
            "the unseal could not be recorded in the receipt chain (unseal record refused: receipt {} is not a token redemption row); the cleartext was discarded and nothing was written. The token is spent — its redemption is recorded — so the trustees must approve a new request to unseal again.",
            hex::encode(grant)
        )
    );
    only_known_hex(&err, &[grant, redemption]);
    Ok(())
}

#[cfg(unix)]
#[test]
fn write_sidecar_new_is_0600_never_clobbers_never_follows_a_symlink() -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    let dir = tempfile::tempdir()?;
    let path = dir.path().join("a.securacv-handoff.json");
    write_sidecar_new(&path, b"{}")?;
    assert_eq!(
        std::fs::metadata(&path)?.permissions().mode() & 0o777,
        0o600
    );
    let err = write_sidecar_new(&path, b"{\"x\":1}").unwrap_err();
    assert_eq!(
        err.to_string(),
        format!(
            "a file already exists at {}; a handoff sidecar is never written over another file",
            path.display()
        )
    );
    assert_eq!(std::fs::read(&path)?, b"{}");
    let target = dir.path().join("target");
    std::fs::write(&target, b"original")?;
    let link = dir.path().join("b.securacv-handoff.json");
    std::os::unix::fs::symlink(&target, &link)?;
    assert!(write_sidecar_new(&link, b"{}").is_err());
    assert_eq!(std::fs::read(&target)?, b"original");
    // A dangling symlink is refused too (O_EXCL never follows it).
    let dangling = dir.path().join("d.securacv-handoff.json");
    std::os::unix::fs::symlink(dir.path().join("nowhere"), &dangling)?;
    assert!(write_sidecar_new(&dangling, b"{}").is_err());
    assert!(!dir.path().join("nowhere").exists());
    // The final path is written directly — no temp file and no hard link
    // (FAT/exFAT and SMB mounts refuse link(2)) — and nothing is left over.
    let leftovers: Vec<_> = std::fs::read_dir(dir.path())?
        .flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with('.'))
        .collect();
    assert!(leftovers.is_empty());
    // The pre-burn probe creates and removes the sidecar path.
    probe_sidecar_path(&dir.path().join("p"))?;
    assert!(!dir.path().join("p.securacv-handoff.json").exists());
    assert!(probe_sidecar_path(&dir.path().join("a")).is_err());
    // A stale sidecar is removed; a directory at the path is an error.
    remove_stale_sidecar(&dir.path().join("a"))?;
    assert!(!path.exists());
    remove_stale_sidecar(&dir.path().join("missing"))?;
    std::fs::create_dir_all(dir.path().join("c.securacv-handoff.json").join("x"))?;
    assert!(remove_stale_sidecar(&dir.path().join("c")).is_err());
    Ok(())
}

/// The emitters move an earlier sidecar aside before the burn and restore
/// it on every failure: its salts are the only opening of an earlier
/// disclosure's signed commitments. `discard` deletes it once new bytes
/// replaced the old ones; a restore never writes over a new file.
#[test]
fn a_stale_sidecar_set_aside_is_restored_on_failure_and_discarded_on_success() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let file = dir.path().join("vault-1.raw");
    let side = sidecar_path_for(&file);
    let hidden = |dir: &Path| -> Vec<String> {
        std::fs::read_dir(dir)
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .filter(|n| n.starts_with('.'))
            .collect()
    };
    // Nothing there: nothing moved.
    let guard = set_aside_stale_sidecar(&file)?;
    assert!(!guard.moved());
    drop(guard);
    // A failure (the guard dropped): restored byte-identical.
    std::fs::write(&side, b"earlier sidecar")?;
    let guard = set_aside_stale_sidecar(&file)?;
    assert!(guard.moved());
    assert!(!side.exists(), "the path is free before the burn");
    probe_sidecar_path(&file)?;
    drop(guard);
    assert_eq!(std::fs::read(&side)?, b"earlier sidecar");
    assert!(hidden(dir.path()).is_empty());
    // Success: discarded.
    let guard = set_aside_stale_sidecar(&file)?;
    write_sidecar_new(&side, b"new sidecar")?;
    guard.discard();
    assert_eq!(std::fs::read(&side)?, b"new sidecar");
    assert!(hidden(dir.path()).is_empty());
    // A restore never writes over what stands at the path by then.
    let guard = set_aside_stale_sidecar(&file)?;
    std::fs::write(&side, b"something new")?;
    drop(guard);
    assert_eq!(std::fs::read(&side)?, b"something new");
    assert_eq!(
        hidden(dir.path()).len(),
        1,
        "the earlier sidecar is kept aside"
    );
    // A directory at the path is refused.
    let other = dir.path().join("vault-2.raw");
    std::fs::create_dir_all(sidecar_path_for(&other).join("x"))?;
    assert_eq!(
        set_aside_stale_sidecar(&other).unwrap_err().to_string(),
        "a directory stands at that path"
    );
    Ok(())
}

#[test]
fn handoff_sources_open_no_sockets() {
    for (name, source) in [
        ("src/handoff.rs", include_str!("handoff.rs")),
        (
            "src/bin/handoff_sidecar.rs",
            include_str!("bin/handoff_sidecar.rs"),
        ),
    ] {
        for needle in ["std::net", "TcpStream", "UdpSocket", "ureq", "reqwest"] {
            assert!(!source.contains(needle), "{name} names {needle}");
        }
    }
}
