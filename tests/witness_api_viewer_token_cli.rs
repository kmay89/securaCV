//! End-to-end test of `witness_api mint-viewer-token` / `revoke-viewer-token`,
//! the operator's half of pairing a Witness Wall: the receipt is printed
//! once, on stdout and nowhere else; the viewer file beside the capability
//! token keeps only the token's sha256, at 0600; the receipt pins the key
//! `/api/sealed-log` actually serves; revocation takes an id and refuses a
//! typo loudly.

use anyhow::Result;
use std::path::Path;
use std::process::{Command, Output};
use witness_kernel::{api::ViewerTokenSet, Kernel, KernelConfig, ZonePolicy};

const SEED: &str = "devkey:test:viewer-token-cli-0123456789";
const RULESET: &str = "ruleset:test";

/// Run `witness_api <args>` against `config`, with the seed in the
/// environment and no stray API env override from the caller's shell.
fn witness_api(config: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_witness_api"))
        .args(args)
        .env("WITNESS_CONFIG", config)
        .env("DEVICE_KEY_SEED", SEED)
        .env_remove("WITNESS_API_TOKEN_PATH")
        .env_remove("WITNESS_API_VIEWER_TOKEN_PATH")
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

#[test]
fn mint_prints_the_receipt_once_and_revoke_removes_it() -> Result<()> {
    let dir = tempfile::tempdir()?;
    let db = dir.path().join("witness.db");
    let token_path = dir.path().join("api_token");
    let viewer_path = dir.path().join("viewer_tokens.json");
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

    // The key the route serves, read the way /api/sealed-log reads it.
    let served_key = {
        let cfg = KernelConfig {
            db_path: db.to_string_lossy().to_string(),
            ruleset_id: RULESET.to_string(),
            ruleset_hash: KernelConfig::ruleset_hash_from_id(RULESET),
            kernel_version: "0.0.0-test".to_string(),
            retention: std::time::Duration::from_secs(7 * 86_400),
            device_key_seed: SEED.to_string(),
            zone_policy: ZonePolicy::default(),
        };
        Kernel::open(&cfg)?.sealed_log_document()?.verifying_key
    };

    // A label is required, and nothing is written without one.
    let out = witness_api(&config, &["mint-viewer-token"]);
    assert!(!out.status.success(), "stderr: {}", stderr(&out));
    assert!(stderr(&out).contains("--label"), "stderr: {}", stderr(&out));
    assert!(!viewer_path.exists());
    // A base URL that the Wall could not connect to is refused up front.
    let out = witness_api(
        &config,
        &[
            "mint-viewer-token",
            "--label",
            "tv",
            "--base-url",
            "ftp://x",
        ],
    );
    assert!(!out.status.success(), "stderr: {}", stderr(&out));
    assert!(!viewer_path.exists());

    let out = witness_api(
        &config,
        &[
            "mint-viewer-token",
            "--label",
            "living room tv",
            "--base-url",
            "http://192.168.1.20:8799",
        ],
    );
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    // stdout is exactly the receipt: one line of JSON, so a redirect
    // captures a pasteable pairing receipt and nothing else.
    let printed = stdout(&out);
    assert_eq!(printed.lines().count(), 1, "stdout: {printed}");
    let receipt: serde_json::Value = serde_json::from_str(printed.trim())?;
    let token = receipt["sealed_log_token"].as_str().unwrap_or_default();
    let id = receipt["token_id"].as_str().unwrap_or_default();
    assert_eq!(receipt["kernel"], "witness-kernel");
    assert_eq!(receipt["base_url"], "http://192.168.1.20:8799");
    assert_eq!(receipt["verifying_key"], served_key.as_str());
    assert_eq!(token.len(), 64);
    assert!(token.bytes().all(|b| b.is_ascii_hexdigit()));
    assert_eq!(id.len(), 8);
    assert!(
        !stderr(&out).contains(token),
        "the token is printed once, on stdout only"
    );

    // Beside the capability token, 0600, holding the hash and never the token.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = std::fs::metadata(&viewer_path)?.permissions().mode() & 0o777;
        assert_eq!(mode, 0o600);
    }
    assert!(!std::fs::read_to_string(&viewer_path)?.contains(token));
    let set = ViewerTokenSet::load(&viewer_path)?;
    assert_eq!(set.matches(token).map(|e| e.id.as_str()), Some(id));
    assert_eq!(set.tokens[0].label, "living room tv");

    // Revoke: a typo is an error that changes nothing; the id removes it.
    let out = witness_api(&config, &["revoke-viewer-token", "00000000"]);
    assert!(!out.status.success(), "stderr: {}", stderr(&out));
    assert_eq!(ViewerTokenSet::load(&viewer_path)?.tokens.len(), 1);
    let out = witness_api(&config, &["revoke-viewer-token", id]);
    assert!(out.status.success(), "stderr: {}", stderr(&out));
    assert!(stdout(&out).is_empty(), "revoke prints no secret");
    let set = ViewerTokenSet::load(&viewer_path)?;
    assert!(set.tokens.is_empty());
    assert!(set.matches(token).is_none());

    // An unknown subcommand never falls through to serving.
    let out = witness_api(&config, &["serve-please"]);
    assert!(!out.status.success(), "stderr: {}", stderr(&out));
    assert!(
        stderr(&out).contains("unknown subcommand"),
        "stderr: {}",
        stderr(&out)
    );
    Ok(())
}
