//! `break_glass trustee keygen / wrap / pubkey`, wrapped keys on `approve`,
//! and the `trustee enroll --generate` deprecation, through the real binary.
//! Every line these commands print is pinned here. Passphrases arrive on
//! `--passphrase-fd 0` (stdin); the no-terminal refusals run the binary in a
//! new session (`setsid`) so it has no controlling terminal to fall back to.
#![cfg(unix)]

use std::io::Write;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::process::CommandExt;
use std::path::Path;
use std::process::{Command, Output, Stdio};

use ed25519_dalek::SigningKey;

const PASS: &str = "correct horse battery staple";
const SEED: &str = "devkey:trustee-keys-cli:0011223344556677";

/// Run break_glass with `stdin` piped in. `detach` starts it in a new
/// session, so it has no controlling terminal.
fn run(args: &[&str], stdin: &str, detach: bool) -> Output {
    let mut cmd = Command::new(env!("CARGO_BIN_EXE_break_glass"));
    cmd.args(args)
        .env_remove("DEVICE_KEY_SEED")
        .env_remove("SECURACV_DB_KEY_SEED")
        .env_remove("SECURACV_DB_KEY")
        .env("RUST_LOG", "off")
        .env("RUST_BACKTRACE", "0")
        .env("RUST_LIB_BACKTRACE", "0")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    if detach {
        // SAFETY: setsid is async-signal-safe; it only detaches the child
        // from the parent's controlling terminal.
        unsafe {
            cmd.pre_exec(|| {
                libc::setsid();
                Ok(())
            });
        }
    }
    let mut child = cmd.spawn().expect("spawn break_glass");
    child
        .stdin
        .take()
        .unwrap()
        .write_all(stdin.as_bytes())
        .unwrap();
    child.wait_with_output().expect("wait for break_glass")
}

fn stdout(o: &Output) -> String {
    String::from_utf8(o.stdout.clone()).unwrap()
}

fn stderr(o: &Output) -> String {
    String::from_utf8(o.stderr.clone()).unwrap()
}

/// The stderr of a command that runs under a UI stage, with the stage's
/// opening line and its timed closing line checked and removed.
fn inside_stage(o: &Output, name: &str) -> String {
    let err = stderr(o);
    let rest = err
        .strip_prefix(&format!("==> {name}\n"))
        .unwrap_or_else(|| panic!("no opening stage line: {err}"));
    let (body, last) = rest
        .trim_end_matches('\n')
        .rsplit_once('\n')
        .map_or(("", rest.trim_end_matches('\n')), |(b, l)| (b, l));
    assert!(
        last.starts_with(&format!("\u{2714} {name} (")) && last.ends_with(')'),
        "no closing stage line: {err}"
    );
    if body.is_empty() {
        String::new()
    } else {
        format!("{body}\n")
    }
}

fn p(path: &Path) -> &str {
    path.to_str().unwrap()
}

fn pk_of(seed: [u8; 32]) -> [u8; 32] {
    SigningKey::from_bytes(&seed).verifying_key().to_bytes()
}

fn key_block(header: &str, pk: &[u8; 32]) -> String {
    format!(
        "{header}\n  public key  {}\n  fingerprint {}\n",
        hex::encode(pk),
        hex::encode(&pk[..8])
    )
}

const PLAINTEXT_WARNING_TAIL: &str = " is an unwrapped trustee key — anyone who copies it can sign as this trustee. Wrap it: break_glass trustee wrap --signing-key ";

fn plaintext_warning(path: &str) -> String {
    format!("WARNING: {path}{PLAINTEXT_WARNING_TAIL}{path} --id <id> --output <id>.tkey\n")
}

#[test]
fn keygen_writes_a_wrapped_key_and_prints_only_public_material() {
    let dir = tempfile::tempdir().unwrap();
    let out = dir.path().join("alice.tkey");
    let o = run(
        &[
            "trustee",
            "keygen",
            "--id",
            "alice",
            "--output",
            p(&out),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(stderr(&o), "");

    // The file: one line, the TKG1 prefix, mode 0600.
    let text = std::fs::read_to_string(&out).unwrap();
    assert!(text.starts_with("securacv-trustee-key:v1 544b4731"));
    assert_eq!(text.matches('\n').count(), 1);
    assert!(text.ends_with('\n'));
    let mode = std::fs::metadata(&out).unwrap().permissions().mode() & 0o777;
    assert_eq!(mode, 0o600);

    // The public key printed is the one the file opens to.
    let shown = stdout(&o);
    let pk_hex = shown
        .lines()
        .nth(1)
        .unwrap()
        .strip_prefix("  public key  ")
        .unwrap()
        .to_string();
    let pk: [u8; 32] = hex::decode(&pk_hex).unwrap().try_into().unwrap();
    assert_eq!(
        shown,
        format!(
            "{}Give the operator the public key above for: break_glass trustee enroll --id alice --public-key <it>\n\
             Read the fingerprint back when they enroll you. The key file and its passphrase stay with you.\n",
            key_block(
                "trustee key for 'alice': wrapped under your passphrase (Argon2id, mode 0600)",
                &pk
            )
        )
    );
    // Nothing but the 64-hex public key: no other 64-hex run, no path.
    assert!(!shown.contains(p(dir.path())));

    let o = run(
        &[
            "trustee",
            "pubkey",
            "--signing-key",
            p(&out),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(
        stdout(&o),
        key_block(
            "trustee key for 'alice': wrapped (TKG1); the passphrase opened it",
            &pk
        )
    );
    assert_eq!(stderr(&o), "");
}

#[test]
fn keygen_and_wrap_refusals() {
    let dir = tempfile::tempdir().unwrap();
    let existing = dir.path().join("taken.tkey");
    std::fs::write(&existing, "keep me\n").unwrap();
    let o = run(
        &[
            "trustee",
            "keygen",
            "--id",
            "alice",
            "--output",
            p(&existing),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert!(!o.status.success());
    assert_eq!(stdout(&o), "");
    assert_eq!(
        stderr(&o),
        "Error: the output file already exists; this tool never overwrites a key file\n"
    );
    assert_eq!(std::fs::read_to_string(&existing).unwrap(), "keep me\n");

    // No terminal and no --passphrase-fd: refused, nothing written.
    let fresh = dir.path().join("fresh.tkey");
    let o = run(
        &["trustee", "keygen", "--id", "alice", "--output", p(&fresh)],
        &format!("{PASS}\n"),
        true,
    );
    assert!(!o.status.success());
    assert_eq!(
        stderr(&o),
        "Error: a new trustee key needs a passphrase: run on a terminal or pass --passphrase-fd\n"
    );
    assert!(!fresh.exists());

    // An empty passphrase line is refused.
    let o = run(
        &[
            "trustee",
            "keygen",
            "--id",
            "alice",
            "--output",
            p(&fresh),
            "--passphrase-fd",
            "0",
        ],
        "\n",
        false,
    );
    assert_eq!(stderr(&o), "Error: the passphrase is empty\n");
    assert!(!fresh.exists());

    // An id the trailer cannot carry.
    let o = run(
        &[
            "trustee",
            "keygen",
            "--id",
            "",
            "--output",
            p(&fresh),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert_eq!(stderr(&o), "Error: trustee id must be 1 to 64 bytes\n");

    // A descriptor that is not open.
    let o = run(
        &[
            "trustee",
            "keygen",
            "--id",
            "alice",
            "--output",
            p(&fresh),
            "--passphrase-fd",
            "97",
        ],
        "",
        false,
    );
    assert_eq!(
        stderr(&o),
        "Error: --passphrase-fd 97 is not an open file descriptor\n"
    );
}

#[test]
fn wrap_migrates_a_plaintext_key_and_leaves_the_original() {
    let dir = tempfile::tempdir().unwrap();
    let plain = dir.path().join("bob.key");
    std::fs::write(&plain, format!("{}\n", hex::encode([0x22u8; 32]))).unwrap();
    let wrapped = dir.path().join("bob.tkey");
    let pk = pk_of([0x22; 32]);

    // pubkey on the plaintext file: the warning on stderr, the key on stdout.
    let o = run(&["trustee", "pubkey", "--signing-key", p(&plain)], "", true);
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(
        stdout(&o),
        key_block("trustee key: plaintext (unwrapped)", &pk)
    );
    assert_eq!(stderr(&o), plaintext_warning(p(&plain)));

    let o = run(
        &[
            "trustee",
            "wrap",
            "--signing-key",
            p(&plain),
            "--id",
            "bob",
            "--output",
            p(&wrapped),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(
        stdout(&o),
        format!(
            "{}now destroy the plaintext file; this tool does not delete it\n",
            key_block(
                "trustee key for 'bob': wrapped under your passphrase (Argon2id, mode 0600)",
                &pk
            )
        )
    );
    assert_eq!(stderr(&o), "");
    assert!(plain.exists(), "wrap never deletes the plaintext file");

    // The wrapped file opens to the same key; a wrong passphrase does not.
    let o = run(
        &[
            "trustee",
            "pubkey",
            "--signing-key",
            p(&wrapped),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert_eq!(
        stdout(&o),
        key_block(
            "trustee key for 'bob': wrapped (TKG1); the passphrase opened it",
            &pk
        )
    );
    let o = run(
        &[
            "trustee",
            "pubkey",
            "--signing-key",
            p(&wrapped),
            "--passphrase-fd",
            "0",
        ],
        "not it\n",
        false,
    );
    assert!(!o.status.success());
    assert_eq!(stdout(&o), "");
    assert_eq!(stderr(&o), "Error: trustee key: wrong passphrase\n");
    // No terminal, no fd: the exact refusal.
    let o = run(
        &["trustee", "pubkey", "--signing-key", p(&wrapped)],
        "",
        true,
    );
    assert_eq!(
        stderr(&o),
        "Error: wrapped trustee key needs a passphrase: run on a terminal or pass --passphrase-fd\n"
    );

    // Wrapping a wrapped file, or onto an existing file, is refused.
    let again = dir.path().join("again.tkey");
    let o = run(
        &[
            "trustee",
            "wrap",
            "--signing-key",
            p(&wrapped),
            "--id",
            "bob",
            "--output",
            p(&again),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert_eq!(
        stderr(&o),
        "Error: the signing key file is already wrapped (securacv-trustee-key:v1); nothing to do\n"
    );
    let o = run(
        &[
            "trustee",
            "wrap",
            "--signing-key",
            p(&plain),
            "--id",
            "bob",
            "--output",
            p(&wrapped),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert_eq!(
        stderr(&o),
        "Error: the output file already exists; this tool never overwrites a key file\n"
    );
}

#[test]
fn approve_signs_with_a_wrapped_key_and_warns_on_a_plaintext_one() {
    let dir = tempfile::tempdir().unwrap();
    let plain = dir.path().join("carol.key");
    std::fs::write(&plain, format!("{}\n", hex::encode([0x33u8; 32]))).unwrap();
    let wrapped = dir.path().join("carol.tkey");
    let o = run(
        &[
            "trustee",
            "wrap",
            "--signing-key",
            p(&plain),
            "--id",
            "carol",
            "--output",
            p(&wrapped),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    let pk = pk_of([0x33; 32]);
    let request_hash = [0x5au8; 32];
    let blind = "WARNING: signing a bare request hash is BLIND SIGNING — this tool cannot show \
                 you what it authorizes. Ask the operator for the request context file \
                 (`request --output-request`) and use --request instead.\n";

    for (key, stdin, extra_stderr) in [
        (p(&wrapped).to_string(), format!("{PASS}\n"), String::new()),
        (
            p(&plain).to_string(),
            String::new(),
            plaintext_warning(p(&plain)),
        ),
    ] {
        let approval = dir.path().join("carol.approval");
        let _ = std::fs::remove_file(&approval);
        let o = run(
            &[
                "approve",
                "--request-hash",
                &hex::encode(request_hash),
                "--trustee",
                "carol",
                "--signing-key",
                &key,
                "--passphrase-fd",
                "0",
                "--output",
                p(&approval),
            ],
            &stdin,
            false,
        );
        assert!(o.status.success(), "{}", stderr(&o));
        assert_eq!(
            stdout(&o),
            format!(
                "Approval written to: {}\nTrustee public key: {}\n",
                p(&approval),
                hex::encode(pk)
            )
        );
        assert_eq!(
            inside_stage(&o, "Sign approval"),
            format!("{blind}{extra_stderr}")
        );
        let approval: witness_kernel::break_glass::Approval =
            serde_json::from_str(&std::fs::read_to_string(&approval).unwrap()).unwrap();
        assert_eq!(approval.trustee.0, "carol");
        assert_eq!(approval.request_hash, request_hash);
        assert!(witness_kernel::break_glass::verify_approval(
            &pk,
            &request_hash,
            &approval.signature
        ));
    }
}

#[test]
fn enroll_generate_is_deprecated_and_setup_points_at_keygen() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("witness.db");
    let o = run(
        &[
            "--ui",
            "plain",
            "init",
            "--threshold",
            "1",
            "--trustees",
            "2",
            "--db",
            p(&db),
            "--device-key-seed",
            SEED,
        ],
        "",
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    let shown = stdout(&o);
    assert!(
        shown.ends_with(
            "  break_glass trustee enroll --id <name> --public-key <HEX>\n  \
             (each trustee makes their key on their own machine: break_glass trustee keygen \
             --id <name> --output <name>.tkey)\n"
        ),
        "{shown}"
    );

    let key = dir.path().join("dave.key");
    let o = run(
        &[
            "--ui",
            "plain",
            "trustee",
            "enroll",
            "--id",
            "dave",
            "--generate",
            "--output",
            p(&key),
            "--db",
            p(&db),
            "--device-key-seed",
            SEED,
        ],
        "",
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    assert!(
        stderr(&o).contains(
            "WARNING: trustee enroll --generate is deprecated: it leaves the operator holding \
             the trustee's secret. Have the trustee run break_glass trustee keygen on their \
             own machine and enroll the public key it prints with --public-key.\n"
        ),
        "{}",
        stderr(&o)
    );
    assert!(key.exists());
}
