//! End-to-end test of `witness_api issue-pairing-code` /
//! `list-paired-tokens` / `revoke-paired-token`, the operator's half of
//! pairing the Home Assistant integration: the code is printed once with
//! the key fingerprint, the file beside the capability token keeps only
//! its sha256 and a 5-guess budget, a running API redeems it, the list
//! never shows a token, and revocation by id is immediate.

use anyhow::Result;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::Path;
use std::process::{Command, Output};
use witness_kernel::api::pairing::{
    key_fingerprint, PairingCodeFile, PAIRED_TOKEN_FILE_NAME, PAIRING_CODE_ATTEMPTS,
    PAIRING_CODE_FILE_NAME,
};
use witness_kernel::api::{ApiConfig, ApiServer, PairingPaths, ViewerTokenSet};
use witness_kernel::{Kernel, KernelConfig, ZonePolicy};

const SEED: &str = "devkey:test:pairing-cli-0123456789abcdef";
const RULESET: &str = "ruleset:test";

fn witness_api(config: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_witness_api"))
        .args(args)
        .env("WITNESS_CONFIG", config)
        .env("DEVICE_KEY_SEED", SEED)
        .env_remove("WITNESS_API_TOKEN_PATH")
        .env_remove("WITNESS_API_VIEWER_TOKEN_PATH")
        .env_remove("WITNESS_API_PAIRED_TOKEN_PATH")
        .env_remove("WITNESS_API_PAIRING_CODE_PATH")
        .env_remove("WITNESS_API_PAIRING_TTL_SECS")
        .env_remove("WITNESS_API_FLEET_ROLLCALL")
        .env_remove("WITNESS_API_ADDR")
        .output()
        .expect("witness_api runs")
}

fn stdout(out: &Output) -> String {
    String::from_utf8_lossy(&out.stdout).into_owned()
}

fn stderr(out: &Output) -> String {
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn kernel_config(db: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db.to_string_lossy().to_string(),
        ruleset_id: RULESET.to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET),
        kernel_version: "0.0.0-test".to_string(),
        retention: std::time::Duration::from_secs(7 * 86_400),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

#[test]
fn issue_prints_the_code_once_a_running_api_redeems_it_and_revoke_removes_it() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let db = dir.path().join("witness.db");
    let token_path = dir.path().join("api_token");
    let code_path = dir.path().join(PAIRING_CODE_FILE_NAME);
    let tokens_path = dir.path().join(PAIRED_TOKEN_FILE_NAME);
    let config = dir.path().join("witness_config.json");
    std::fs::write(
        &config,
        serde_json::json!({
            "db_path": db,
            "ruleset_id": RULESET,
            "api": { "addr": "127.0.0.1:8799", "token_path": token_path },
        })
        .to_string(),
    )?;
    // The key the API serves, read the way /api/sealed-log reads it.
    let cfg = kernel_config(&db);
    let served_key = Kernel::open(&cfg)?.sealed_log_document()?.verifying_key;

    // Nothing paired yet.
    let out = witness_api(&config, &["list-paired-tokens"]);
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    assert_eq!(stdout(&out).trim(), "[]");

    // A zero TTL is refused and writes nothing.
    let out = witness_api(&config, &["issue-pairing-code", "--ttl-secs", "0"]);
    assert!(!out.status.success());
    assert!(!code_path.exists());

    let before = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)?
        .as_secs();
    let out = witness_api(
        &config,
        &[
            "issue-pairing-code",
            "--ttl-secs",
            "120",
            "--label",
            "docker exec securacv pair",
        ],
    );
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    // stdout is exactly the receipt: one JSON line.
    let printed = stdout(&out);
    assert_eq!(printed.lines().count(), 1, "stdout: {printed}");
    let receipt: Value = serde_json::from_str(printed.trim())?;
    let code = receipt["pairing_code"].as_str().expect("code").to_string();
    assert_eq!(code.len(), 6);
    assert!(code.bytes().all(|b| b.is_ascii_digit()));
    assert_eq!(receipt["kernel"], "witness-kernel");
    let expires = receipt["expires"].as_u64().expect("expires");
    assert!(
        (before + 120..=before + 125).contains(&expires),
        "expires {expires}"
    );
    assert_eq!(receipt["attempts"], PAIRING_CODE_ATTEMPTS);
    assert_eq!(receipt["device_id"], witness_kernel::ha_device_id(SEED));
    assert_eq!(receipt["verifying_key"], served_key.as_str());
    let fingerprint = key_fingerprint(&served_key)?;
    assert_eq!(receipt["fingerprint"], fingerprint.as_str());
    // The receipt's keys, in the order the sidecar's `pair` reads them (its
    // json_line_field runs over this line without jq): the shared vector
    // tests/fixtures/ha_contract/pair.json holds the same line, and
    // privacy_witness_kernel/tests/test_config_render.sh parses THAT with the
    // lifted shell function — so the two sides meet in one file.
    let fixture_path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/ha_contract/pair.json");
    let fixture: Value = serde_json::from_str(&std::fs::read_to_string(&fixture_path)?)?;
    let vector_line = fixture["issue_pairing_code_stdout"]
        .as_str()
        .expect("stdout vector");
    let keys_in = |line: &str| -> Vec<String> {
        let mut found: Vec<(usize, String)> = Vec::new();
        for key in [
            "kernel",
            "pairing_code",
            "expires",
            "attempts",
            "device_id",
            "verifying_key",
            "fingerprint",
        ] {
            let needle = format!("\"{key}\":");
            let at = line
                .find(&needle)
                .unwrap_or_else(|| panic!("{key} missing from {line}"));
            found.push((at, key.to_string()));
        }
        found.sort();
        found.into_iter().map(|(_, k)| k).collect()
    };
    assert_eq!(keys_in(printed.trim()), keys_in(vector_line));
    let vector: Value = serde_json::from_str(vector_line)?;
    assert_eq!(
        vector.as_object().map(|o| o.len()),
        receipt.as_object().map(|o| o.len()),
        "the receipt grew or shrank: regenerate the vector and update pair's reader"
    );
    // The human sentence names the fingerprint and the budget; the code is
    // on stderr as the operator's own echo (the daemon never logs it).
    let human = stderr(&out);
    assert!(human.contains(&fingerprint), "stderr: {human}");
    assert!(human.contains("5 attempts"), "stderr: {human}");

    // Beside the capability token, 0600, hash only, budget intact.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = std::fs::metadata(&code_path)?.permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
    }
    let on_disk = std::fs::read_to_string(&code_path)?;
    assert!(!on_disk.contains(&code), "the clear code is never on disk");
    let file = PairingCodeFile::load(&code_path)?.expect("issued");
    assert_eq!(file.attempts_left, PAIRING_CODE_ATTEMPTS);
    assert_eq!(file.expires, expires);
    assert_eq!(file.label, "docker exec securacv pair");

    // A running API over the same files redeems it.
    let api = ApiServer::new(
        ApiConfig {
            addr: "127.0.0.1:0".to_string(),
            token_path: Some(token_path.clone()),
            pairing: Some(PairingPaths {
                code_path: code_path.clone(),
                tokens_path: tokens_path.clone(),
            }),
            ..ApiConfig::default()
        },
        cfg,
    )
    .spawn()?;
    let body = format!(r#"{{"code":"{code}","label":"home-assistant"}}"#);
    let mut stream = TcpStream::connect(api.addr)?;
    stream.write_all(
        format!(
            "POST /api/pair HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n{body}",
            body.len()
        )
        .as_bytes(),
    )?;
    let mut response = String::new();
    stream.read_to_string(&mut response)?;
    assert!(
        response.starts_with("HTTP/1.1 200 OK"),
        "response: {response}"
    );
    let paired: Value = serde_json::from_str(response.split("\r\n\r\n").nth(1).unwrap_or(""))?;
    let token = paired["token"].as_str().expect("token").to_string();
    let token_id = paired["token_id"].as_str().expect("token_id").to_string();
    assert_eq!(paired["device_id"], receipt["device_id"]);
    assert_eq!(paired["verifying_key"], receipt["verifying_key"]);
    api.stop()?;
    assert!(!code_path.exists(), "a redeemed code is consumed");

    // The list shows id, label and minted — never the token or its hash.
    let out = witness_api(&config, &["list-paired-tokens"]);
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    let rows: Value = serde_json::from_str(stdout(&out).trim())?;
    assert_eq!(rows.as_array().map(Vec::len), Some(1), "{rows}");
    assert_eq!(rows[0]["id"], token_id.as_str());
    assert_eq!(rows[0]["label"], "home-assistant");
    assert!(rows[0].get("minted").is_some());
    assert!(rows[0].get("sha256").is_none());
    assert!(!stdout(&out).contains(&token));
    let set = ViewerTokenSet::load_kind(&tokens_path, "paired token")?;
    assert!(set.matches(&token).is_some());

    // Revoke: a typo changes nothing; the id removes it.
    let out = witness_api(&config, &["revoke-paired-token", "00000000"]);
    assert!(!out.status.success(), "stderr: {}", stderr(&out));
    assert_eq!(
        ViewerTokenSet::load_kind(&tokens_path, "paired token")?
            .tokens
            .len(),
        1
    );
    let out = witness_api(&config, &["revoke-paired-token", &token_id]);
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    assert!(stdout(&out).is_empty(), "revoke prints no secret");
    let set = ViewerTokenSet::load_kind(&tokens_path, "paired token")?;
    assert!(set.tokens.is_empty());
    assert!(set.matches(&token).is_none());
    let out = witness_api(&config, &["list-paired-tokens"]);
    assert_eq!(stdout(&out).trim(), "[]");

    // Issuing again overwrites any outstanding code (there is only one).
    let out = witness_api(&config, &["issue-pairing-code"]);
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    let first: Value = serde_json::from_str(stdout(&out).trim())?;
    let out = witness_api(&config, &["issue-pairing-code"]);
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    let second: Value = serde_json::from_str(stdout(&out).trim())?;
    let file = PairingCodeFile::load(&code_path)?.expect("issued");
    let digest = <sha2::Sha256 as sha2::Digest>::digest(second["pairing_code"].as_str().unwrap());
    assert_eq!(
        file.sha256,
        hex::encode(digest),
        "the file holds the newest code's hash"
    );
    assert_eq!(
        file.expires,
        second["expires"].as_u64().unwrap(),
        "and its expiry ({} was the first)",
        first["expires"]
    );
    Ok(())
}

#[test]
fn pairing_subcommands_refuse_without_pairing_paths_or_identity() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let db = dir.path().join("witness.db");
    // No token path: the pairing files have no default home, so pairing
    // is off and the subcommands say why.
    let config = dir.path().join("witness_config.json");
    std::fs::write(
        &config,
        serde_json::json!({ "db_path": db, "ruleset_id": RULESET }).to_string(),
    )?;
    for args in [
        &["issue-pairing-code"][..],
        &["list-paired-tokens"][..],
        &["revoke-paired-token", "abcdef01"][..],
    ] {
        let out = witness_api(&config, args);
        assert!(!out.status.success(), "{args:?} must refuse");
        assert!(
            stderr(&out).contains("pairing is not configured"),
            "{args:?}: {}",
            stderr(&out)
        );
    }
    assert!(!db.exists(), "a refused subcommand created the database");

    // With paths but no identity (no seed anywhere): a code must not mint
    // one — the file would pin a key no log serves.
    let token_path = dir.path().join("api_token");
    std::fs::write(
        &config,
        serde_json::json!({
            "db_path": db,
            "ruleset_id": RULESET,
            "api": { "addr": "127.0.0.1:8799", "token_path": token_path },
        })
        .to_string(),
    )?;
    let out = Command::new(env!("CARGO_BIN_EXE_witness_api"))
        .args(["issue-pairing-code"])
        .env("WITNESS_CONFIG", &config)
        .env_remove("DEVICE_KEY_SEED")
        .env_remove("WITNESS_API_TOKEN_PATH")
        .env_remove("WITNESS_API_ADDR")
        .output()
        .expect("witness_api runs");
    assert!(!out.status.success());
    assert!(
        stderr(&out).contains("no device identity"),
        "stderr: {}",
        stderr(&out)
    );
    assert!(stdout(&out).is_empty());
    assert!(!dir.path().join(PAIRING_CODE_FILE_NAME).exists());
    assert!(!db.exists());
    Ok(())
}
