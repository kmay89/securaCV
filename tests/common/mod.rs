//! Shared helpers for the anchoring integration tests: the committed OpenSSL
//! fixture token, byte-spliced tokens over arbitrary imprints (structurally
//! valid, cryptographically dead — enough for every offline structural
//! test), a throwaway local TSA for the tests that need a real
//! countersignature (skipped when openssl is absent), and the one gated
//! policy write that produces a `policy_head`.
#![allow(dead_code)]

use sha2::{Digest, Sha256};
use std::path::{Path, PathBuf};
use std::process::Command;
use witness_kernel::break_glass::{QuorumPolicy, TrusteeEntry, TrusteeId};
use witness_kernel::{tsa, Kernel, PolicyChangeOutcome, TimeBucket};

pub fn fixture_path(name: &str) -> String {
    format!("{}/tests/fixtures/tsa/{name}", env!("CARGO_MANIFEST_DIR"))
}

/// sha256("securacv-fixture"), the imprint every committed fixture covers.
pub fn fixture_digest() -> [u8; 32] {
    Sha256::digest(b"securacv-fixture").into()
}

/// The committed OpenSSL-generated token (`reply.tsr`).
pub fn fixture_token() -> tsa::TimestampToken {
    tsa::parse_response(&std::fs::read(fixture_path("reply.tsr")).expect("reading TSA fixture"))
        .expect("fixture token parses")
}

/// The fixture bare token with its message imprint replaced by `h`. The
/// fixture digest occurs exactly once in the token, so the splice is exact;
/// the result parses and carries `h` as its imprint but no longer verifies
/// under any CA (the signature covers the original TSTInfo).
pub fn token_with_imprint(h: &[u8; 32]) -> Vec<u8> {
    let token = fixture_token().token_der;
    let needle = fixture_digest();
    let hits: Vec<usize> = (0..=token.len() - 32)
        .filter(|&i| token[i..i + 32] == needle)
        .collect();
    assert_eq!(
        hits.len(),
        1,
        "the fixture digest must occur exactly once in the bare token"
    );
    let mut out = token;
    out[hits[0]..hits[0] + 32].copy_from_slice(h);
    out
}

/// `token_with_imprint(h)` wrapped as a granted `TimeStampResp` (status 0),
/// for `.tsr` files handed to `log_anchor import`.
pub fn response_with_imprint(h: &[u8; 32]) -> Vec<u8> {
    let token = token_with_imprint(h);
    let mut body = vec![0x30, 0x03, 0x02, 0x01, 0x00];
    body.extend_from_slice(&token);
    let mut out = vec![0x30];
    out.extend(der_len(body.len()));
    out.extend(body);
    out
}

fn der_len(len: usize) -> Vec<u8> {
    if len < 0x80 {
        return vec![len as u8];
    }
    let bytes = len.to_be_bytes();
    let first = bytes
        .iter()
        .position(|&b| b != 0)
        .unwrap_or(bytes.len() - 1);
    let mut out = vec![0x80 | (bytes.len() - first) as u8];
    out.extend_from_slice(&bytes[first..]);
    out
}

/// A parsed `token_with_imprint(h)` as a `TimestampToken`, for
/// `tsa::insert_anchor`.
pub fn spliced_token(h: &[u8; 32]) -> tsa::TimestampToken {
    tsa::parse_token(&token_with_imprint(h)).expect("spliced token parses")
}

pub fn openssl_available() -> bool {
    Command::new("openssl")
        .arg("version")
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false)
}

fn tsa_cnf(cn: &str) -> String {
    format!(
        "[ req ]\ndistinguished_name = dn\nprompt = no\n[ dn ]\nCN = {cn}\nO = Fixture Only\n\
         [ tsa_cert ]\nextendedKeyUsage = critical,timeStamping\nkeyUsage = critical,digitalSignature\nbasicConstraints = CA:false\n\
         [ ca_cert ]\nbasicConstraints = critical,CA:true\nkeyUsage = critical,keyCertSign,cRLSign\n\
         [ tsa ]\ndefault_tsa = tsa_config1\n[ tsa_config1 ]\nserial = ./serial\ncrypto_device = builtin\n\
         signer_cert = ./tsa.crt\nsigner_key = ./tsa.key\ndefault_policy = 1.3.6.1.4.1.13762.3\ndigests = sha256\n\
         accuracy = secs:1\nordering = no\ntsa_name = no\ness_cert_id_chain = no\nsigner_digest = sha256\n"
    )
}

fn run_ok(cmd: &mut Command, what: &str) {
    let out = cmd.output().unwrap_or_else(|e| panic!("{what}: {e}"));
    assert!(
        out.status.success(),
        "{what} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
}

/// A self-signed root that can sign several TSA leaves (for the CA-overlap
/// tests).
pub struct SharedRoot {
    pub dir: PathBuf,
}

impl SharedRoot {
    pub fn new(dir: &Path) -> Option<Self> {
        if !openssl_available() {
            return None;
        }
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join("root.cnf"), tsa_cnf("SecuraCV Test Root")).unwrap();
        run_ok(
            Command::new("openssl").current_dir(dir).args([
                "req",
                "-x509",
                "-newkey",
                "ec",
                "-pkeyopt",
                "ec_paramgen_curve:P-256",
                "-keyout",
                "root.key",
                "-out",
                "root.crt",
                "-days",
                "2",
                "-nodes",
                "-config",
                "root.cnf",
                "-extensions",
                "ca_cert",
            ]),
            "openssl req (root)",
        );
        Some(Self {
            dir: dir.to_path_buf(),
        })
    }

    pub fn ca_path(&self) -> String {
        self.dir.join("root.crt").to_string_lossy().into_owned()
    }
}

/// A throwaway local TSA (the fixtures README recipe): key + cert + serial
/// in a scratch dir; it signs this test's artifacts only. `None` when
/// openssl is not on PATH.
pub struct ThrowawayTsa {
    pub dir: PathBuf,
    counter: std::cell::Cell<u32>,
}

impl ThrowawayTsa {
    /// Self-signed TSA certificate (its own CA).
    pub fn new(dir: &Path, cn: &str) -> Option<Self> {
        if !openssl_available() {
            return None;
        }
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join("tsa.cnf"), tsa_cnf(cn)).unwrap();
        std::fs::write(dir.join("serial"), "01\n").unwrap();
        run_ok(
            Command::new("openssl").current_dir(dir).args([
                "req",
                "-x509",
                "-newkey",
                "ec",
                "-pkeyopt",
                "ec_paramgen_curve:P-256",
                "-keyout",
                "tsa.key",
                "-out",
                "tsa.crt",
                "-days",
                "2",
                "-nodes",
                "-config",
                "tsa.cnf",
                "-extensions",
                "tsa_cert",
            ]),
            "openssl req (tsa)",
        );
        Some(Self {
            dir: dir.to_path_buf(),
            counter: std::cell::Cell::new(0),
        })
    }

    /// A TSA leaf signed by `root`; `ca_path()` is then the ROOT bundle, so
    /// two such TSAs verify under the same CA file.
    pub fn new_under_shared_root(dir: &Path, cn: &str, root: &SharedRoot) -> Option<Self> {
        if !openssl_available() {
            return None;
        }
        std::fs::create_dir_all(dir).unwrap();
        std::fs::write(dir.join("tsa.cnf"), tsa_cnf(cn)).unwrap();
        std::fs::write(dir.join("serial"), "01\n").unwrap();
        run_ok(
            Command::new("openssl").current_dir(dir).args([
                "req",
                "-new",
                "-newkey",
                "ec",
                "-pkeyopt",
                "ec_paramgen_curve:P-256",
                "-keyout",
                "tsa.key",
                "-out",
                "tsa.csr",
                "-nodes",
                "-config",
                "tsa.cnf",
            ]),
            "openssl req (leaf csr)",
        );
        // Distinct serials per leaf so two leaves under one issuer never
        // share (issuer, serial).
        let serial = format!("{:02x}", 0x10 + (cn.len() as u32 % 0xef));
        run_ok(
            Command::new("openssl").current_dir(dir).args([
                "x509",
                "-req",
                "-in",
                "tsa.csr",
                "-CA",
                &root.dir.join("root.crt").to_string_lossy(),
                "-CAkey",
                &root.dir.join("root.key").to_string_lossy(),
                "-set_serial",
                &format!("0x{serial}{}", hex::encode(cn.as_bytes())),
                "-out",
                "tsa.crt",
                "-days",
                "2",
                "-extfile",
                "tsa.cnf",
                "-extensions",
                "tsa_cert",
            ]),
            "openssl x509 (leaf sign)",
        );
        // The chain bundle: leaf then root, so `openssl ts -verify -CAfile`
        // against the root alone can build the path (the token embeds the
        // leaf via -cert).
        std::fs::copy(root.dir.join("root.crt"), dir.join("root.crt")).unwrap();
        Some(Self {
            dir: dir.to_path_buf(),
            counter: std::cell::Cell::new(0),
        })
    }

    /// PEM path of the CA that validates this TSA's tokens: the TSA's own
    /// cert when self-signed, the shared root otherwise.
    pub fn ca_path(&self) -> String {
        let root = self.dir.join("root.crt");
        if root.exists() {
            root.to_string_lossy().into_owned()
        } else {
            self.dir.join("tsa.crt").to_string_lossy().into_owned()
        }
    }

    /// PEM path of this TSA's own (leaf) certificate.
    pub fn leaf_path(&self) -> String {
        self.dir.join("tsa.crt").to_string_lossy().into_owned()
    }

    /// SHA-256 of this TSA's signing certificate DER — what a
    /// `cert_sha256` pin holds.
    pub fn fingerprint_hex(&self) -> String {
        let out = Command::new("openssl")
            .args(["x509", "-in", &self.leaf_path(), "-outform", "DER"])
            .output()
            .expect("openssl x509");
        assert!(out.status.success());
        hex::encode(Sha256::digest(&out.stdout))
    }

    /// Mint a granted response over `digest` (no nonce, cert embedded) and
    /// return its bytes together with the parsed token.
    pub fn mint_response(&self, digest: &[u8; 32]) -> (Vec<u8>, tsa::TimestampToken) {
        let n = self.counter.get() + 1;
        self.counter.set(n);
        let q = format!("q{n}.tsq");
        let r = format!("r{n}.tsr");
        std::fs::write(self.dir.join(&q), tsa::build_request(digest, None, true)).unwrap();
        run_ok(
            Command::new("openssl")
                .current_dir(&self.dir)
                .env("OPENSSL_CONF", "tsa.cnf")
                .args([
                    "ts",
                    "-reply",
                    "-queryfile",
                    &q,
                    "-out",
                    &r,
                    "-section",
                    "tsa_config1",
                ]),
            "openssl ts -reply",
        );
        let bytes = std::fs::read(self.dir.join(&r)).unwrap();
        let token = tsa::parse_response(&bytes).expect("minted response parses");
        tsa::verify_match(&token, digest, None).expect("minted token covers the digest");
        (bytes, token)
    }

    pub fn mint(&self, digest: &[u8; 32]) -> tsa::TimestampToken {
        self.mint_response(digest).1
    }
}

/// Bootstrap the quorum policy through the GATED path — the only path that
/// appends a `policy_change_history` row and therefore produces a
/// `policy_head`. Returns that row's entry hash.
pub fn bootstrap_policy_history(kernel: &mut Kernel) -> [u8; 32] {
    let trustee = ed25519_dalek::SigningKey::from_bytes(&[31u8; 32]);
    let policy = QuorumPolicy::new(
        1,
        vec![TrusteeEntry {
            id: TrusteeId::new("alice"),
            public_key: trustee.verifying_key().to_bytes(),
        }],
    )
    .expect("valid policy");
    let outcome = kernel
        .set_break_glass_policy_gated(&policy, &[], TimeBucket::now_10min().unwrap())
        .expect("gated bootstrap");
    assert_eq!(outcome, PolicyChangeOutcome::Bootstrapped);
    let count: i64 = kernel
        .conn
        .query_row("SELECT count(*) FROM policy_change_history", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 1);
    let hash: Vec<u8> = kernel
        .conn
        .query_row(
            "SELECT entry_hash FROM policy_change_history ORDER BY id DESC LIMIT 1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    hash.try_into().expect("32-byte entry hash")
}
