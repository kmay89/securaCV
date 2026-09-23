//! The break-glass console's one-time setup, end to end: a real kernel
//! database behind `KernelVaultOps`, the real `BreakGlassServer` transport
//! (capability token, loopback), and the real `break_glass` binary reading
//! back what the console stored.
//!
//! `POST /breakglass/policy` is accepted only while no quorum policy exists;
//! afterwards every change goes through the quorum-consented CLI flow
//! (Invariant V).

use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::path::Path;
use std::process::{Command, Output, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::JoinHandle;
use std::time::Duration;

use ed25519_dalek::SigningKey;
use witness_kernel::break_glass::{BreakGlassServer, BreakGlassServerConfig, KernelVaultOps};
use witness_kernel::{KernelConfig, ZonePolicy};

const SEED: &str = "devkey:console-bootstrap:0011223344556677";

fn cfg(db: &Path) -> KernelConfig {
    KernelConfig {
        db_path: db.to_string_lossy().to_string(),
        ruleset_id: "ruleset:test".to_string(),
        ruleset_hash: KernelConfig::ruleset_hash_from_id("ruleset:test"),
        kernel_version: "0.0.0-test".to_string(),
        retention: Duration::from_secs(3600),
        device_key_seed: SEED.to_string(),
        zone_policy: ZonePolicy::default(),
    }
}

fn public_hex(byte: u8) -> String {
    hex::encode(
        SigningKey::from_bytes(&[byte; 32])
            .verifying_key()
            .to_bytes(),
    )
}

struct Running {
    addr: SocketAddr,
    token: String,
    shutdown: Arc<AtomicBool>,
    join: JoinHandle<anyhow::Result<()>>,
}

impl Running {
    fn start(dir: &Path) -> Self {
        // `Kernel` is not `Send`: open it on the thread that serves it, and
        // hand the bound address and token back.
        let (tx, rx) = std::sync::mpsc::channel();
        let shutdown = Arc::new(AtomicBool::new(false));
        let flag = shutdown.clone();
        let dir = dir.to_path_buf();
        let join = std::thread::spawn(move || {
            let ops = KernelVaultOps::open(
                cfg(&dir.join("witness.db")),
                dir.join("vault").to_string_lossy().to_string(),
            )?;
            let server = BreakGlassServer::bind(BreakGlassServerConfig {
                addr: "127.0.0.1:0".to_string(),
                output_dir: dir.join("unsealed").to_string_lossy().to_string(),
                token_path: None,
                tls: None,
            })?;
            tx.send((server.local_addr(), server.capability_token()))
                .expect("hand back address and token");
            server.serve(ops, flag)
        });
        let (addr, token) = match rx.recv() {
            Ok(started) => started,
            Err(_) => panic!("server did not start: {:?}", join.join()),
        };
        Self {
            addr,
            token,
            shutdown,
            join,
        }
    }

    fn request(&self, method: &str, path: &str, auth: bool, body: &str) -> (u16, String) {
        let mut stream = TcpStream::connect(self.addr).expect("connect");
        stream
            .set_read_timeout(Some(Duration::from_secs(10)))
            .expect("timeout");
        let auth_line = if auth {
            format!("Authorization: Bearer {}\r\n", self.token)
        } else {
            String::new()
        };
        write!(
            stream,
            "{method} {path} HTTP/1.1\r\nHost: localhost\r\n{auth_line}\
             Content-Type: application/json\r\nContent-Length: {}\r\n\
             Connection: close\r\n\r\n{body}",
            body.len()
        )
        .expect("write request");
        let mut raw = String::new();
        stream.read_to_string(&mut raw).expect("read response");
        let status: u16 = raw
            .split_whitespace()
            .nth(1)
            .and_then(|s| s.parse().ok())
            .unwrap_or_else(|| panic!("no status line in {raw:?}"));
        let body = raw
            .split_once("\r\n\r\n")
            .map(|(_, b)| b.to_string())
            .unwrap_or_default();
        (status, body)
    }

    fn stop(self) {
        self.shutdown.store(true, Ordering::SeqCst);
        self.join
            .join()
            .expect("serve thread")
            .expect("serve loop exits cleanly");
    }
}

fn break_glass(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_break_glass"))
        .args(args)
        .env_remove("DEVICE_KEY_SEED")
        .env_remove("SECURACV_DB_KEY_SEED")
        .env_remove("SECURACV_DB_KEY")
        .stdin(Stdio::null())
        .output()
        .expect("spawn break_glass")
}

fn text(out: &Output) -> String {
    format!(
        "--- stdout ---\n{}\n--- stderr ---\n{}",
        String::from_utf8_lossy(&out.stdout),
        String::from_utf8_lossy(&out.stderr)
    )
}

fn two_of_two_body() -> String {
    serde_json::json!({
        "n": 2,
        "trustees": [
            { "id": "alice", "public_key": public_hex(1) },
            { "id": "bob", "public_key": public_hex(2) },
        ],
    })
    .to_string()
}

#[test]
fn console_bootstraps_the_policy_once_and_the_cli_reads_it_back() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    let db_str = db.to_str().expect("utf8 path");
    let server = Running::start(dir.path());

    // No capability token: refused at the transport, nothing stored.
    let (status, body) = server.request("POST", "/breakglass/policy", false, &two_of_two_body());
    assert_eq!(status, 401, "{body}");
    assert!(body.contains("missing_token"), "{body}");

    // Before setup every other route still reports the missing policy.
    let (status, body) = server.request("GET", "/breakglass/policy", true, "");
    assert_eq!(status, 409, "{body}");
    assert!(body.contains("policy_not_configured"), "{body}");

    // The bootstrap: accepted once.
    let (status, body) = server.request("POST", "/breakglass/policy", true, &two_of_two_body());
    assert_eq!(status, 201, "{body}");
    let stored: serde_json::Value = serde_json::from_str(&body).expect("json");
    assert_eq!(stored["n"], 2);
    assert_eq!(stored["m"], 2);

    // ... and never again, even for a different valid policy.
    let one_of_one = serde_json::json!({
        "n": 1,
        "trustees": [{ "id": "mallory", "public_key": public_hex(3) }],
    })
    .to_string();
    let (status, body) = server.request("POST", "/breakglass/policy", true, &one_of_one);
    assert_eq!(status, 409, "{body}");
    assert!(body.contains("policy_already_configured"), "{body}");

    // The console reads back what it stored.
    let (status, body) = server.request("GET", "/breakglass/policy", true, "");
    assert_eq!(status, 200, "{body}");
    assert!(body.contains("alice") && body.contains("bob"), "{body}");
    assert!(!body.contains("mallory"), "{body}");
    server.stop();

    // The CLI reads the same policy from the database ...
    let out = break_glass(&["policy", "show", "--db", db_str, "--device-key-seed", SEED]);
    assert!(out.status.success(), "{}", text(&out));
    let shown: serde_json::Value =
        serde_json::from_slice(&out.stdout).unwrap_or_else(|_| panic!("{}", text(&out)));
    assert_eq!(shown["n"], 2);
    assert_eq!(shown["m"], 2);
    assert_eq!(shown["trustees"][0]["id"], "alice");
    assert_eq!(shown["trustees"][1]["id"], "bob");

    // ... and the policy history holds exactly one row: the bootstrap, signed
    // and chained like the CLI's own.
    let out = break_glass(&[
        "policy",
        "history",
        "--db",
        db_str,
        "--device-key-seed",
        SEED,
    ]);
    assert!(out.status.success(), "{}", text(&out));
    let all = text(&out);
    assert!(all.contains("[1] VALID bootstrap — 2-of-2"), "{all}");
    assert!(!all.contains("[2]"), "{all}");
    assert!(all.contains("History chain VALID (1 entries)"), "{all}");
}

/// The CLI bootstraps while the console is already running (its in-memory
/// view still says "no policy"): the console's bootstrap re-reads the
/// database, answers 409, and the CLI's policy stands.
#[test]
fn console_bootstrap_loses_to_a_policy_the_cli_stored_meanwhile() {
    let dir = tempfile::tempdir().expect("tempdir");
    let db = dir.path().join("witness.db");
    let db_str = db.to_str().expect("utf8 path");
    let server = Running::start(dir.path());

    let carol = format!("carol:{}", public_hex(4));
    let out = break_glass(&[
        "policy",
        "set",
        "--threshold",
        "1",
        "--trustee",
        &carol,
        "--db",
        db_str,
        "--ruleset-id",
        "ruleset:test",
        "--device-key-seed",
        SEED,
    ]);
    assert!(out.status.success(), "{}", text(&out));

    let (status, body) = server.request("POST", "/breakglass/policy", true, &two_of_two_body());
    assert_eq!(status, 409, "{body}");
    assert!(body.contains("policy_already_configured"), "{body}");
    server.stop();

    let out = break_glass(&["policy", "show", "--db", db_str, "--device-key-seed", SEED]);
    assert!(out.status.success(), "{}", text(&out));
    let shown: serde_json::Value = serde_json::from_slice(&out.stdout).expect("json");
    assert_eq!(shown["trustees"][0]["id"], "carol");
    assert_eq!(shown["m"], 1);
}
