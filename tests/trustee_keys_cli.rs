//! `break_glass trustee keygen / wrap / pubkey`, wrapped keys on `approve`
//! and `policy approve`, the next-step lines `request` and `policy propose`
//! print for trustees, and the `trustee enroll --generate` deprecation,
//! through the real binary. Every line these commands print is pinned here.
//! Passphrases arrive on `--passphrase-fd 0`: a pipe, or (for the typed-twice
//! path) a pseudo-terminal; the no-terminal refusals run the binary in a new
//! session (`setsid`) so it has no controlling terminal to fall back to.
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
    // A command that refuses before reading stdin may exit, closing the
    // pipe, before this write; the asserts on its output pin the refusal.
    match child.stdin.take().unwrap().write_all(stdin.as_bytes()) {
        Ok(()) => {}
        Err(e) if e.kind() == std::io::ErrorKind::BrokenPipe => {}
        Err(e) => panic!("write break_glass stdin: {e}"),
    }
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
    let enroll_next = |left: usize| {
        format!(
            "\nNext: enroll {left} more trustee(s) — the policy goes live when all 2 are enrolled:\n  \
             break_glass trustee enroll --id <name> --public-key <HEX>\n  \
             (each trustee makes their key on their own machine: break_glass trustee keygen \
             --id <name> --output <name>.tkey)\n"
        )
    };
    assert_eq!(
        stdout(&o),
        format!(
            "=== Break-glass setup — init ===\n  \
             ✓ device identity pinned: ed710fe4b26dffaa\n  \
             ✓ started a 1-of-2 setup draft ({}.setup-draft.json)\n    \
             the quorum policy commits automatically once all 2 trustees are enrolled\n{}",
            p(&db),
            enroll_next(2)
        )
    );
    assert_eq!(inside_stage(&o, "Break-glass init"), "");

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
    assert_eq!(
        inside_stage(&o, "Enroll trustee"),
        "WARNING: trustee enroll --generate is deprecated: it leaves the operator holding \
         the trustee's secret. Have the trustee run break_glass trustee keygen on their \
         own machine and enroll the public key it prints with --public-key.\n"
    );
    assert_eq!(
        stdout(&o),
        format!(
            "  ✓ minted a signing key for 'dave' → {} (secret, mode 0600 — hand it to the trustee)\n  \
             ✓ enrolled trustee 'dave' (1/2)\n{}",
            p(&key),
            enroll_next(1)
        )
    );
    assert!(key.exists());
}

/// The line `request` and `policy propose` print after each "each trustee
/// runs" command.
const PASSPHRASE_NOTE: &str =
    "  (a wrapped key's passphrase is read from the terminal, or from --passphrase-fd N)\n";

/// The one "Valid window: …" line of `shown`, checked to name the bucket
/// that starts at `start` (its UTC rendering is the clock's, so it is bound
/// by the bucket start rather than retyped).
fn window_line(shown: &str, start: u64) -> String {
    let line = shown
        .lines()
        .find(|l| l.starts_with("Valid window: "))
        .unwrap_or_else(|| panic!("no window line: {shown}"));
    assert!(
        line.ends_with(&format!("(bucket start {start}, size 600s)")),
        "{line}"
    );
    format!("{line}\n")
}

fn json_of(path: &Path) -> serde_json::Value {
    serde_json::from_str(&std::fs::read_to_string(path).unwrap()).unwrap()
}

#[test]
fn request_tells_trustees_to_sign_with_a_wrapped_key() {
    let dir = tempfile::tempdir().unwrap();
    let file = dir.path().join("unlock.request");
    let fields = [
        "--ui",
        "plain",
        "request",
        "--envelope",
        "env-1",
        "--purpose",
        "review the door",
        "--requested-by",
        "ops",
        "--reason",
        "incident-review",
    ];
    let head = "=== Unlock Request ===\nEnvelope:     env-1\nPurpose:      review the door\n\
                Requested by: ops\nReason code:  incident-review\nRuleset:      ruleset:v0.3.0\n";
    let tail = "Trustee roster is loaded from the canonical policy in the kernel database.\n";

    let mut args = fields.to_vec();
    args.extend(["--output-request", p(&file)]);
    let o = run(&args, "", false);
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(inside_stage(&o, "Create unlock request"), "");
    let written = json_of(&file);
    let hash = written["request_hash"].as_str().unwrap();
    let start = written["time_bucket"]["start_epoch_s"].as_u64().unwrap();
    let shown = stdout(&o);
    assert_eq!(
        shown,
        format!(
            "{head}{}Request hash: {hash}\n\nRequest context written to: {f}\n\
             Send the FILE to each trustee (full context, never a bare hash). Each trustee runs:\n  \
             break_glass approve --request {f} --trustee <name> --signing-key <name>.tkey \
             --output <name>.approval\n{PASSPHRASE_NOTE}{tail}",
            window_line(&shown, start),
            f = p(&file)
        )
    );

    // Without a context file: the bare hash, the same signing-key hint.
    let o = run(&fields, "", false);
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(inside_stage(&o, "Create unlock request"), "");
    let shown = stdout(&o);
    let hash = shown
        .lines()
        .find_map(|l| l.strip_prefix("Request hash: "))
        .unwrap()
        .to_string();
    let start: u64 = shown
        .split("(bucket start ")
        .nth(1)
        .and_then(|r| r.split(',').next())
        .unwrap()
        .parse()
        .unwrap();
    assert_eq!(
        shown,
        format!(
            "{head}{}Request hash: {hash}\n\nShare the request hash with trustees. Each trustee runs:\n  \
             break_glass approve --request-hash {hash} --trustee <name> --signing-key <name>.tkey \
             --output <name>.approval\n{PASSPHRASE_NOTE}\
             Better: re-run with --output-request <file> and send trustees the full context file \
             instead, so their tool can show them what they are signing.\n{tail}",
            window_line(&shown, start)
        )
    );
}

#[test]
fn policy_approve_signs_with_a_wrapped_key_and_warns_on_a_plaintext_one() {
    let dir = tempfile::tempdir().unwrap();
    let db = dir.path().join("witness.db");
    let plain = dir.path().join("alice.key");
    std::fs::write(&plain, format!("{}\n", hex::encode([0x44u8; 32]))).unwrap();
    let wrapped = dir.path().join("alice.tkey");
    let o = run(
        &[
            "trustee",
            "wrap",
            "--signing-key",
            p(&plain),
            "--id",
            "alice",
            "--output",
            p(&wrapped),
            "--passphrase-fd",
            "0",
        ],
        &format!("{PASS}\n"),
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    let apk = hex::encode(pk_of([0x44; 32]));
    let bpk = hex::encode(pk_of([0x55; 32]));
    let alice = format!("alice:{apk}");
    let bob = format!("bob:{bpk}");

    // A 1-of-1 policy (bootstrap), then a proposal that adds bob.
    let o = run(
        &[
            "--ui",
            "plain",
            "policy",
            "set",
            "--threshold",
            "1",
            "--trustee",
            &alice,
            "--db",
            p(&db),
            "--device-key-seed",
            SEED,
        ],
        "",
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    let proposal = dir.path().join("change.proposal");
    let o = run(
        &[
            "--ui",
            "plain",
            "policy",
            "propose",
            "--threshold",
            "1",
            "--trustee",
            &alice,
            "--trustee",
            &bob,
            "--output",
            p(&proposal),
            "--db",
            p(&db),
            "--device-key-seed",
            SEED,
        ],
        "",
        false,
    );
    assert!(o.status.success(), "{}", stderr(&o));
    assert_eq!(inside_stage(&o, "Propose policy change"), "");
    let file = json_of(&proposal);
    let change_hash = file["change_hash"].as_str().unwrap().to_string();
    let start = file["time_bucket"]["start_epoch_s"].as_u64().unwrap();
    let prev: witness_kernel::break_glass::QuorumPolicy =
        serde_json::from_value(file["prev_policy"].clone()).unwrap();
    let rosters = format!(
        "Current: 1-of-1 (crypto mode: classical)\n  trustee alice {apk}\n\
         Proposed: 1-of-2 (crypto mode: classical)\n  trustee alice {apk}\n  trustee bob {bpk}\n"
    );
    let proposed = stdout(&o);
    let window = window_line(&proposed, start);
    assert_eq!(
        proposed,
        format!(
            "=== Policy change proposal ===\n{rosters}{window}Change hash:  {change_hash}\n\
             Proposal written to: {f}\n\nSend the FILE to 1 current trustee(s). Each runs:\n  \
             break_glass policy approve --proposal {f} --trustee <name> --signing-key <name>.tkey \
             --output <name>.policy-approval\n{PASSPHRASE_NOTE}\
             Then apply within the window: break_glass policy set … --approvals <files>\n",
            f = p(&proposal)
        )
    );

    let review = format!(
        "=== Policy change — review before signing ===\n{rosters}Adds trustee key(s):    bob\n\
         {window}Change hash:  {change_hash} (recomputed locally)\n\
         Verify the CURRENT-policy fingerprint out of band with the operator: {}\n\n",
        hex::encode(prev.full_commitment())
    );
    let approval = dir.path().join("alice.policy-approval");
    for (key, stdin, extra_stderr) in [
        (p(&wrapped).to_string(), format!("{PASS}\n"), String::new()),
        (
            p(&plain).to_string(),
            String::new(),
            plaintext_warning(p(&plain)),
        ),
    ] {
        let _ = std::fs::remove_file(&approval);
        let o = run(
            &[
                "policy",
                "approve",
                "--proposal",
                p(&proposal),
                "--trustee",
                "alice",
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
                "{review}Policy-change approval written to: {}\nTrustee public key: {apk}\n",
                p(&approval)
            )
        );
        assert_eq!(inside_stage(&o, "Approve policy change"), extra_stderr);
        let signed: witness_kernel::break_glass::Approval =
            serde_json::from_str(&std::fs::read_to_string(&approval).unwrap()).unwrap();
        let hash: [u8; 32] = hex::decode(&change_hash).unwrap().try_into().unwrap();
        assert_eq!(signed.trustee.0, "alice");
        assert_eq!(signed.request_hash, hash);
        assert!(witness_kernel::break_glass::verify_policy_change_approval(
            &pk_of([0x44; 32]),
            &hash,
            &signed.signature
        ));
    }

    // A wrapped key with no terminal and no --passphrase-fd: refused after
    // the review, nothing written.
    let _ = std::fs::remove_file(&approval);
    let o = run(
        &[
            "policy",
            "approve",
            "--proposal",
            p(&proposal),
            "--trustee",
            "alice",
            "--signing-key",
            p(&wrapped),
            "--output",
            p(&approval),
        ],
        "",
        true,
    );
    assert!(!o.status.success());
    assert_eq!(stdout(&o), review);
    let err = stderr(&o);
    let (staged, refusal) = err
        .trim_end_matches('\n')
        .rsplit_once('\n')
        .unwrap_or_else(|| panic!("{err}"));
    assert_eq!(
        refusal,
        "Error: wrapped trustee key needs a passphrase: run on a terminal or pass --passphrase-fd"
    );
    let closing = staged
        .strip_prefix("==> Approve policy change\n")
        .unwrap_or_else(|| panic!("{err}"));
    assert!(
        closing.starts_with("\u{2714} Approve policy change (") && closing.ends_with(')'),
        "{err}"
    );
    assert!(!approval.exists());
}

/// Read from a pty controller until exactly `expected` has been shown (or
/// five seconds pass), and check it.
#[cfg(target_os = "linux")]
fn expect_shown(controller: i32, expected: &str) {
    let mut out = Vec::new();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
    while out.len() < expected.len() && std::time::Instant::now() < deadline {
        let mut pfd = libc::pollfd {
            fd: controller,
            events: libc::POLLIN,
            revents: 0,
        };
        if unsafe { libc::poll(&mut pfd, 1, 200) } <= 0 {
            continue;
        }
        let mut buf = [0u8; 256];
        let n = unsafe { libc::read(controller, buf.as_mut_ptr().cast(), buf.len()) };
        if n <= 0 {
            break;
        }
        out.extend_from_slice(&buf[..n as usize]);
    }
    assert_eq!(String::from_utf8_lossy(&out), expected);
}

#[cfg(target_os = "linux")]
fn type_at(controller: i32, text: &str) {
    let n = unsafe { libc::write(controller, text.as_ptr().cast(), text.len()) };
    assert_eq!(n, text.len() as isize);
}

/// `trustee keygen --passphrase-fd 0` with stdin a terminal: the passphrase
/// is asked for twice, with echo off, and two different entries are
/// refused with nothing written.
#[cfg(target_os = "linux")]
#[test]
fn keygen_on_a_terminal_asks_twice_with_echo_off() {
    use std::os::fd::{FromRawFd, OwnedFd};
    let dir = tempfile::tempdir().unwrap();
    for (first, second, ok) in [
        ("aaa-one\n".to_string(), "bbb-two\n".to_string(), false),
        (format!("{PASS}\n"), format!("{PASS}\n"), true),
    ] {
        let out = dir.path().join("erin.tkey");
        let (controller, terminal) = unsafe {
            let controller = libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY);
            assert!(controller >= 0, "posix_openpt");
            assert_eq!(libc::grantpt(controller), 0);
            assert_eq!(libc::unlockpt(controller), 0);
            let mut name = [0 as libc::c_char; 128];
            assert_eq!(
                libc::ptsname_r(controller, name.as_mut_ptr(), name.len()),
                0
            );
            let terminal = libc::open(name.as_ptr(), libc::O_RDWR | libc::O_NOCTTY);
            assert!(terminal >= 0);
            (controller, terminal)
        };
        // SAFETY: a fresh duplicate the child's stdin takes ownership of.
        let stdin = unsafe { OwnedFd::from_raw_fd(libc::dup(terminal)) };
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_break_glass"));
        cmd.args([
            "trustee",
            "keygen",
            "--id",
            "erin",
            "--output",
            p(&out),
            "--passphrase-fd",
            "0",
        ])
        .env("RUST_LOG", "off")
        .env("RUST_BACKTRACE", "0")
        .env("RUST_LIB_BACKTRACE", "0")
        .stdin(Stdio::from(stdin))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
        // SAFETY: setsid only detaches the child from any controlling tty.
        unsafe {
            cmd.pre_exec(|| {
                libc::setsid();
                Ok(())
            });
        }
        let child = cmd.spawn().expect("spawn break_glass");
        expect_shown(controller, "new passphrase for trustee key 'erin': ");
        type_at(controller, &first);
        // The newline echoes (ECHONL, \r\n on a terminal); the line does not.
        expect_shown(controller, "\r\nrepeat it: ");
        type_at(controller, &second);
        let o = child.wait_with_output().expect("wait for break_glass");
        expect_shown(controller, "\r\n");
        if ok {
            assert!(o.status.success(), "{}", stderr(&o));
            assert_eq!(stderr(&o), "");
            let pk_hex = stdout(&o)
                .lines()
                .nth(1)
                .and_then(|l| l.strip_prefix("  public key  "))
                .unwrap()
                .to_string();
            let pk: [u8; 32] = hex::decode(&pk_hex).unwrap().try_into().unwrap();
            assert_eq!(
                stdout(&o),
                format!(
                    "{}Give the operator the public key above for: break_glass trustee enroll --id erin --public-key <it>\n\
                     Read the fingerprint back when they enroll you. The key file and its passphrase stay with you.\n",
                    key_block(
                        "trustee key for 'erin': wrapped under your passphrase (Argon2id, mode 0600)",
                        &pk
                    )
                )
            );
            // The file opens under the passphrase typed (twice).
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
            assert_eq!(
                stdout(&o),
                key_block(
                    "trustee key for 'erin': wrapped (TKG1); the passphrase opened it",
                    &pk
                )
            );
        } else {
            assert!(!o.status.success());
            assert_eq!(stdout(&o), "");
            assert_eq!(stderr(&o), "Error: the two passphrases differ\n");
            assert!(!out.exists());
        }
        unsafe {
            libc::close(terminal);
            libc::close(controller);
        }
    }
}
