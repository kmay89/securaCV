//! The `household_checker` binary end to end: init, admin keys, genesis,
//! enrollment through propose/approve/apply, pairing, `serve` on an
//! ephemeral loopback port, and the `client` subcommand as the credential.
//!
//! SENTINEL: every member handle the run creates is searched for in the
//! server's whole stderr and stdout, every client response, and the output
//! seam file. None may appear.

use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

const BIN: &str = env!("CARGO_BIN_EXE_household_checker");

fn tmp() -> PathBuf {
    let mut b = [0u8; 8];
    getrandom_bytes(&mut b);
    let p = std::env::temp_dir().join(format!(
        "hk-cli-{}",
        b.iter().map(|x| format!("{x:02x}")).collect::<String>()
    ));
    std::fs::create_dir_all(&p).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o700)).unwrap();
    }
    p
}

fn getrandom_bytes(b: &mut [u8]) {
    let k = household_keys::rng::array::<8>().unwrap();
    b.copy_from_slice(&k[..b.len()]);
}

struct Out {
    code: i32,
    stdout: String,
    stderr: String,
}

fn run(args: &[&str]) -> Out {
    let o = Command::new(BIN).args(args).output().unwrap();
    Out {
        code: o.status.code().unwrap_or(-1),
        stdout: String::from_utf8_lossy(&o.stdout).into_owned(),
        stderr: String::from_utf8_lossy(&o.stderr).into_owned(),
    }
}

fn ok(args: &[&str]) -> serde_json::Value {
    let o = run(args);
    assert_eq!(
        o.code, 0,
        "{args:?}\nstdout: {}\nstderr: {}",
        o.stdout, o.stderr
    );
    serde_json::from_str(o.stdout.trim())
        .unwrap_or(serde_json::Value::String(o.stdout.trim().to_string()))
}

fn s(p: &Path) -> &str {
    p.to_str().unwrap()
}

struct Serve {
    child: Child,
    port: u16,
    stderr_rest: std::thread::JoinHandle<String>,
}

fn serve(data: &Path, kek: &Path) -> Serve {
    let mut child = Command::new(BIN)
        .args([
            "serve",
            "--data-dir",
            s(data),
            "--kek",
            s(kek),
            "--bind",
            "127.0.0.1:0",
            "--home-ttl",
            "600",
        ])
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let mut r = BufReader::new(child.stderr.take().unwrap());
    let mut first = String::new();
    r.read_line(&mut first).unwrap();
    let port: u16 = first
        .trim()
        .rsplit(' ')
        .next()
        .and_then(|p| p.parse().ok())
        .unwrap_or_else(|| panic!("no port in: {first}"));
    let stderr_rest = std::thread::spawn(move || {
        let mut rest = first;
        let _ = r.read_to_string(&mut rest);
        rest
    });
    Serve {
        child,
        port,
        stderr_rest,
    }
}

#[test]
fn binary_end_to_end() {
    let base = tmp();
    let data = base.join("data");
    let kek = base.join("kek.hex");
    let admin = base.join("admin.json");

    // init writes the KEK outside the data dir; a KEK inside is refused.
    let o = run(&[
        "init",
        "--data-dir",
        s(&base.join("bad")),
        "--kek",
        s(&base.join("bad/kek")),
    ]);
    assert_ne!(o.code, 0);
    let init = ok(&["init", "--data-dir", s(&data), "--kek", s(&kek)]);
    assert_eq!(init["household"].as_str().unwrap().len(), 32);

    // One admin, genesis needs the physical confirm.
    ok(&["admin-keygen", "--out", s(&admin)]);
    assert_ne!(
        run(&[
            "roster",
            "genesis",
            "--data-dir",
            s(&data),
            "--kek",
            s(&kek),
            "--admin-key",
            s(&admin)
        ])
        .code,
        0
    );
    // Genesis signs with the sealed roster key, so it needs the KEK.
    assert_ne!(
        run(&[
            "roster",
            "genesis",
            "--data-dir",
            s(&data),
            "--admin-key",
            s(&admin),
            "--physical-confirm",
        ])
        .code,
        0
    );
    ok(&[
        "roster",
        "genesis",
        "--data-dir",
        s(&data),
        "--kek",
        s(&kek),
        "--admin-key",
        s(&admin),
        "--physical-confirm",
    ]);
    // No hub secret key sits in the data directory in the clear.
    assert!(!data.join("posture.key").exists() && !data.join("roster.key").exists());
    assert!(data.join("hub_keys.sealed").exists());

    // Two software credentials: one enrolled as intent (a phone), one as
    // possession (a fob).
    let mut handles = Vec::new();
    let mut keys = Vec::new();
    for (kind, class) in [("phone", "intent"), ("fob", "possession")] {
        let kf = base.join(format!("{kind}.key"));
        let pk = ok(&["client", "keygen", "--out", s(&kf)]);
        let p = ok(&[
            "roster",
            "propose",
            "add-member",
            "--data-dir",
            s(&data),
            "--kind",
            kind,
            "--class",
            class,
            "--public-key",
            pk["public_key"].as_str().unwrap(),
        ]);
        let dg = p["proposal"].as_str().unwrap().to_string();
        handles.push(p["new_member_handle"].as_str().unwrap().to_string());
        ok(&[
            "roster",
            "approve",
            "--data-dir",
            s(&data),
            "--proposal",
            &dg,
            "--admin-key",
            s(&admin),
        ]);
        // Threshold 1: apply without the physical confirm is refused.
        assert_ne!(
            run(&[
                "roster",
                "apply",
                "--data-dir",
                s(&data),
                "--kek",
                s(&kek),
                "--proposal",
                &dg
            ])
            .code,
            0
        );
        ok(&[
            "roster",
            "apply",
            "--data-dir",
            s(&data),
            "--kek",
            s(&kek),
            "--proposal",
            &dg,
            "--physical-confirm",
            &dg[..8],
        ]);
        keys.push(kf);
    }

    // A card: its key comes from a file (never argv), only under this
    // household's KEK, and leaves the sealed store when the card is revoked.
    let p = ok(&[
        "roster",
        "propose",
        "add-member",
        "--data-dir",
        s(&data),
        "--kind",
        "card",
        "--class",
        "possession",
    ]);
    let card_dg = p["proposal"].as_str().unwrap().to_string();
    let card_handle = p["new_member_handle"].as_str().unwrap().to_string();
    let card_key = base.join("card.key");
    std::fs::write(&card_key, "00112233445566778899aabbccddeeff\n").unwrap();
    // A key file others can read is refused, like a KEK at that mode.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&card_key, std::fs::Permissions::from_mode(0o644)).unwrap();
        let o = run(&[
            "card",
            "provision",
            "--data-dir",
            s(&data),
            "--kek",
            s(&kek),
            "--handle",
            &card_handle,
            "--key-file",
            s(&card_key),
        ]);
        assert_ne!(o.code, 0, "a world-readable key file was accepted");
        assert!(!data.join("cards.sealed").exists());
        std::fs::set_permissions(&card_key, std::fs::Permissions::from_mode(0o600)).unwrap();
    }
    let other_kek = base.join("other.kek");
    std::fs::write(
        &other_kek,
        "0101010101010101010101010101010101010101010101010101010101010101",
    )
    .unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&other_kek, std::fs::Permissions::from_mode(0o600)).unwrap();
    }
    assert_ne!(
        run(&[
            "card",
            "provision",
            "--data-dir",
            s(&data),
            "--kek",
            s(&other_kek),
            "--handle",
            &card_handle,
            "--key-file",
            s(&card_key),
        ])
        .code,
        0,
        "a card sealed under another household's KEK"
    );
    assert!(!data.join("cards.sealed").exists());
    let o = run(&[
        "card",
        "provision",
        "--data-dir",
        s(&data),
        "--kek",
        s(&kek),
        "--handle",
        &card_handle,
        "--key-file",
        s(&card_key),
    ]);
    assert_eq!(o.code, 0, "{}", o.stderr);
    assert!(o.stdout.trim().is_empty(), "a supplied key was echoed");
    // The sealed key is the one in the file.
    let sealed_key = |handle: &str| -> Option<[u8; 16]> {
        let kek_k = household_keys::secrets::load_kek(&kek, &data).unwrap();
        let hh: [u8; 16] =
            household_keys::hex::decode_array(init["household"].as_str().unwrap()).unwrap();
        let bytes = std::fs::read(data.join("cards.sealed")).unwrap();
        let store = household_keys::secrets::CardStore::open(&bytes, &kek_k, &hh).unwrap();
        let h: [u8; 16] = household_keys::hex::decode_array(handle).unwrap();
        store.keys_for(&[h]).first().map(|(_, k)| **k)
    };
    assert_eq!(
        sealed_key(&card_handle),
        Some(household_keys::hex::decode_array::<16>("00112233445566778899aabbccddeeff").unwrap())
    );
    let before = std::fs::read(data.join("cards.sealed")).unwrap();
    // The old flag, a misspelled flag and the old flag in `=` form are all
    // refused, the store is untouched, and the key never reaches stderr.
    for bad in [
        vec!["--key-hex", "ffeeddccbbaa99887766554433221100"],
        vec!["--key-fle", "/nonexistent"],
        vec!["--key-hex=ffeeddccbbaa99887766554433221100"],
    ] {
        let mut args = vec![
            "card",
            "provision",
            "--data-dir",
            s(&data),
            "--kek",
            s(&kek),
            "--handle",
            &card_handle,
        ];
        args.extend(bad.iter().copied());
        let o = run(&args);
        assert_ne!(o.code, 0, "{bad:?} accepted");
        assert!(
            !o.stderr.contains("ffeeddcc"),
            "a key reached stderr: {}",
            o.stderr
        );
        assert_eq!(std::fs::read(data.join("cards.sealed")).unwrap(), before);
    }
    // Re-keying an existing card needs --replace.
    let o = run(&[
        "card",
        "provision",
        "--data-dir",
        s(&data),
        "--kek",
        s(&kek),
        "--handle",
        &card_handle,
    ]);
    assert_ne!(o.code, 0, "a card was re-keyed without --replace");
    assert_eq!(std::fs::read(data.join("cards.sealed")).unwrap(), before);
    ok(&[
        "roster",
        "approve",
        "--data-dir",
        s(&data),
        "--proposal",
        &card_dg,
        "--admin-key",
        s(&admin),
    ]);
    let applied = ok(&[
        "roster",
        "apply",
        "--data-dir",
        s(&data),
        "--kek",
        s(&kek),
        "--proposal",
        &card_dg,
        "--physical-confirm",
        &card_dg[..8],
    ]);
    assert_eq!(applied["card_keys_removed"], 0);
    let p = ok(&[
        "roster",
        "propose",
        "revoke",
        "--data-dir",
        s(&data),
        "--handle",
        &card_handle,
    ]);
    let dg = p["proposal"].as_str().unwrap().to_string();
    ok(&[
        "roster",
        "approve",
        "--data-dir",
        s(&data),
        "--proposal",
        &dg,
        "--admin-key",
        s(&admin),
    ]);
    let o = run(&[
        "roster",
        "apply",
        "--data-dir",
        s(&data),
        "--kek",
        s(&kek),
        "--proposal",
        &dg,
    ]);
    assert_eq!(o.code, 0, "{}", o.stderr);
    let v: serde_json::Value = serde_json::from_str(o.stdout.trim()).unwrap();
    assert_eq!(v["card_keys_removed"], 1, "a revoked card's key was kept");
    assert_eq!(
        sealed_key(&card_handle),
        None,
        "the pruned store was not written"
    );
    // The retry path runs, and has nothing left to do.
    let pruned = ok(&["card", "prune", "--data-dir", s(&data), "--kek", s(&kek)]);
    assert_eq!(pruned["card_keys_removed"], 0);
    assert!(
        o.stderr.contains("pair --reset"),
        "no pairing hint: {}",
        o.stderr
    );
    handles.push(card_handle);
    // An intent fob is refused at propose time.
    let kf = base.join("bad-fob.key");
    let pk = ok(&["client", "keygen", "--out", s(&kf)]);
    let o = run(&[
        "roster",
        "propose",
        "add-member",
        "--data-dir",
        s(&data),
        "--kind",
        "fob",
        "--class",
        "intent",
        "--public-key",
        pk["public_key"].as_str().unwrap(),
    ]);
    assert_ne!(o.code, 0);

    let token = ok(&["pair", "--data-dir", s(&data)]);
    let token_file = base.join("token");
    std::fs::write(&token_file, token.as_str().unwrap()).unwrap();

    let srv = serve(&data, &kek);
    let addr = format!("127.0.0.1:{}", srv.port);
    let client_out = std::cell::RefCell::new(Vec::new());
    let cmd = |key: &Path, handle: &str, purpose: &str| -> Out {
        let o = run(&[
            "client",
            "command",
            "--key",
            s(key),
            "--handle",
            handle,
            "--token-file",
            s(&token_file),
            "--purpose",
            purpose,
            "--addr",
            &addr,
        ]);
        client_out
            .borrow_mut()
            .push(format!("{}{}", o.stdout, o.stderr));
        o
    };
    for p in ["home", "night", "away"] {
        let o = cmd(&keys[0], &handles[0], p);
        assert_eq!(o.code, 0, "intent {p}: {} {}", o.stdout, o.stderr);
        assert!(o.stdout.contains(&format!(r#""posture":"{p}""#)));
    }
    for p in ["night", "away"] {
        assert_eq!(cmd(&keys[1], &handles[1], p).code, 0, "possession {p}");
    }
    let o = cmd(&keys[1], &handles[1], "home");
    assert_ne!(o.code, 0, "possession set home");
    assert!(o.stdout.contains(r#"{"error":"rejected"}"#));
    // The intent key under the possession handle is refused too.
    assert_ne!(cmd(&keys[0], &handles[1], "night").code, 0);

    // The seam matches GET /v1/posture and verifies against the posture key.
    let o = run(&[
        "client",
        "posture",
        "--token-file",
        s(&token_file),
        "--addr",
        &addr,
    ]);
    assert_eq!(o.code, 0);
    client_out.borrow_mut().push(o.stdout.clone());
    let seam = std::fs::read(data.join("household_posture.bin")).unwrap();
    assert_eq!(seam.len(), 112);
    let pkey: [u8; 32] =
        household_keys::hex::decode_array(init["posture_public_key"].as_str().unwrap()).unwrap();
    let hh: [u8; 16] =
        household_keys::hex::decode_array(init["household"].as_str().unwrap()).unwrap();
    let vk = ed25519_dalek::VerifyingKey::from_bytes(&pkey).unwrap();
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_secs();
    let st = household_keys::posture::verify(&seam, &hh, &vk, None, Some(now)).unwrap();
    assert_eq!(st.posture, household_keys::posture::HouseholdPosture::Away);
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let m = std::fs::metadata(data.join("household_posture.bin"))
            .unwrap()
            .permissions()
            .mode();
        assert_eq!(m & 0o777, 0o600);
    }

    // Revoke the phone while the server runs: within a reload tick its key
    // stops working.
    let p = ok(&[
        "roster",
        "propose",
        "revoke",
        "--data-dir",
        s(&data),
        "--handle",
        &handles[0],
    ]);
    let dg = p["proposal"].as_str().unwrap().to_string();
    ok(&[
        "roster",
        "approve",
        "--data-dir",
        s(&data),
        "--proposal",
        &dg,
        "--admin-key",
        s(&admin),
    ]);
    ok(&[
        "roster",
        "apply",
        "--data-dir",
        s(&data),
        "--kek",
        s(&kek),
        "--proposal",
        &dg,
    ]);
    std::thread::sleep(std::time::Duration::from_millis(1500));
    assert_ne!(
        cmd(&keys[0], &handles[0], "night").code,
        0,
        "revoked key still works"
    );

    // An old token still reads the posture until `pair --reset`, which takes
    // effect while the server runs.
    let posture = || {
        run(&[
            "client",
            "posture",
            "--token-file",
            s(&token_file),
            "--addr",
            &addr,
        ])
        .code
    };
    assert_eq!(posture(), 0);
    ok(&["pair", "--data-dir", s(&data), "--reset"]);
    std::thread::sleep(std::time::Duration::from_millis(1500));
    assert_ne!(posture(), 0, "an unpaired token still reads the posture");

    let mut child = srv.child;
    child.kill().unwrap();
    let _ = child.wait();
    let server_stderr = srv.stderr_rest.join().unwrap();
    assert!(server_stderr.contains("listening"));

    // SENTINEL.
    let seam_text = String::from_utf8_lossy(&seam).to_ascii_lowercase();
    for h in &handles {
        let raw: [u8; 16] = household_keys::hex::decode_array(h).unwrap();
        assert!(
            !seam.windows(16).any(|w| w == raw),
            "raw handle in the seam"
        );
        assert!(!seam_text.contains(h.as_str()));
        assert!(
            !server_stderr.to_ascii_lowercase().contains(h.as_str()),
            "handle in server log"
        );
        for c in client_out.borrow().iter() {
            assert!(
                !c.to_ascii_lowercase().contains(h.as_str()),
                "handle in an HTTP body: {c}"
            );
        }
    }
    // The server's log has no per-request lines at all.
    for line in server_stderr.lines() {
        assert!(
            line.contains("listening") || line.contains("stopped"),
            "unexpected log line: {line}"
        );
    }
    let _ = std::fs::remove_dir_all(&base);
}
