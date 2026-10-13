//! End to end over the LAN API: a real server on an ephemeral loopback port,
//! driven by the reference software credential, for home, away and night
//! with intent and possession credentials (es256 and FIDO2).
//!
//! The sentinel half plants recognizable handles and asserts they never
//! appear in any HTTP response body, statement, seam write or log line.

use base64::Engine;
use household_keys::checker::{Checker, Persist};
use household_keys::client::{self, Binding, SoftCredential};
use household_keys::posture::{self, HouseholdPosture, STATEMENT_LEN};
use household_keys::roster::{
    AdminAlg, AdminEntry, AdminKind, CredentialClass, MemberAlg, MemberEntry, MemberKind, Roster,
};
use household_keys::secrets::CardStore;
use household_keys::server::{Clock, Server, ServerConfig, ServerHandle};
use household_keys::wire::Purpose;
use household_keys::{hex, Result};
use p256::ecdsa::SigningKey;
use sha2::Digest;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

const HH: [u8; 16] = [0xaa; 16];
const INTENT: [u8; 16] = [0x5e; 16]; // sentinel handles: recognizable bytes
const POSSESSION: [u8; 16] = [0x5f; 16];
const FIDO: [u8; 16] = [0x6a; 16];
const RP: &str = "hk-member.example";
const TOKEN: &str = "test-bearer-token-0123456789";
const T0: u64 = 1_800_000_000;

struct TestClock {
    mono: AtomicU64,
    unix: AtomicU64,
}

impl Clock for TestClock {
    fn mono_ms(&self) -> u64 {
        self.mono.load(Ordering::SeqCst)
    }
    fn unix(&self) -> u64 {
        self.unix.load(Ordering::SeqCst)
    }
}

#[derive(Clone, Default)]
struct Spy(Arc<Mutex<Vec<Vec<u8>>>>);

impl Persist for Spy {
    fn save_seq(&mut self, _seq: u64) -> Result<()> {
        Ok(())
    }
    fn publish(&mut self, s: &[u8; STATEMENT_LEN]) -> Result<()> {
        self.0.lock().unwrap().push(s.to_vec());
        Ok(())
    }
}

fn pk(seed: u8) -> Vec<u8> {
    SigningKey::from_slice(&[seed; 32])
        .unwrap()
        .verifying_key()
        .to_sec1_point(false)
        .as_bytes()
        .to_vec()
}

fn member(handle: [u8; 16], kind: MemberKind, class: CredentialClass, seed: u8) -> MemberEntry {
    let fido = kind == MemberKind::Fido2;
    MemberEntry {
        handle,
        kind,
        class,
        alg: if fido {
            MemberAlg::Fido2Es256
        } else {
            MemberAlg::Es256
        },
        public_key: Some(pk(seed)),
        rp_id: fido.then(|| RP.to_string()),
        credential_id: fido.then(|| vec![0x6a; 8]),
        not_after: None,
    }
}

fn roster() -> Roster {
    let admin = ed25519_dalek::SigningKey::from_bytes(&[1; 32]);
    let g = Roster::genesis(
        HH,
        vec![AdminEntry {
            handle: [1; 16],
            alg: AdminAlg::Ed25519,
            kind: AdminKind::Cli,
            public_key: admin.verifying_key().to_bytes().to_vec(),
            rp_id: None,
            credential_id: None,
        }],
        1,
    )
    .unwrap();
    let mut r = g.successor().unwrap();
    r.members.push(member(
        INTENT,
        MemberKind::Phone,
        CredentialClass::Intent,
        0x31,
    ));
    r.members.push(member(
        POSSESSION,
        MemberKind::Fob,
        CredentialClass::Possession,
        0x32,
    ));
    r.members.push(member(
        FIDO,
        MemberKind::Fido2,
        CredentialClass::Intent,
        0x33,
    ));
    r.sort();
    r.validate().unwrap();
    r
}

struct Harness {
    handle: ServerHandle,
    clock: Arc<TestClock>,
    spy: Spy,
    logs: Arc<Mutex<Vec<String>>>,
    bodies: Vec<String>,
    posture_key: ed25519_dalek::VerifyingKey,
}

fn start() -> Harness {
    let clock = Arc::new(TestClock {
        mono: AtomicU64::new(1_000),
        unix: AtomicU64::new(T0),
    });
    let spy = Spy::default();
    let sk = ed25519_dalek::SigningKey::from_bytes(&[9; 32]);
    let posture_key = sk.verifying_key();
    let checker = Checker::new(
        sk,
        roster(),
        CardStore::new(),
        0,
        3600,
        Box::new(spy.clone()),
        T0,
    )
    .unwrap();
    let logs = Arc::new(Mutex::new(Vec::new()));
    let l2 = logs.clone();
    let token_hash: [u8; 32] = sha2::Sha256::digest(TOKEN.as_bytes()).into();
    let srv = Server::bind(
        &ServerConfig {
            bind: "127.0.0.1:0".parse().unwrap(),
            allow_lan: false,
        },
        checker,
        vec![token_hash],
        None,
        clock.clone(),
        Arc::new(move |line: &str| l2.lock().unwrap().push(line.to_string())),
    )
    .unwrap();
    Harness {
        handle: srv.spawn().unwrap(),
        clock,
        spy,
        logs,
        bodies: Vec::new(),
        posture_key,
    }
}

impl Harness {
    fn set(&mut self, cred: &SoftCredential, p: Purpose) -> (u16, String) {
        let r = client::set_posture(self.handle.addr(), TOKEN, cred, p).unwrap();
        self.bodies.push(r.1.clone());
        r
    }

    fn get_posture(&mut self) -> (HouseholdPosture, Vec<u8>) {
        let (st, body) =
            client::http(self.handle.addr(), "GET", "/v1/posture", Some(TOKEN), None).unwrap();
        assert_eq!(st, 200);
        self.bodies.push(body.clone());
        let v: serde_json::Value = serde_json::from_str(&body).unwrap();
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(v["statement_b64"].as_str().unwrap())
            .unwrap();
        let s = posture::verify(
            &bytes,
            &HH,
            &self.posture_key,
            None,
            Some(self.clock.unix()),
        )
        .unwrap();
        assert_eq!(v["posture"], s.posture.word());
        (s.posture, bytes)
    }
}

fn cred(handle: [u8; 16], seed: u8, binding: Binding) -> SoftCredential {
    SoftCredential::new(
        SigningKey::from_slice(&[seed; 32]).unwrap(),
        handle,
        binding,
    )
}

#[test]
fn lan_api_end_to_end_with_sentinels() {
    let mut h = start();
    let intent = cred(INTENT, 0x31, Binding::Es256);
    let possession = cred(POSSESSION, 0x32, Binding::Es256);
    let fido_uv = cred(
        FIDO,
        0x33,
        Binding::Fido2 {
            rp_id: RP.into(),
            uv: true,
        },
    );
    let fido_no_uv = cred(
        FIDO,
        0x33,
        Binding::Fido2 {
            rp_id: RP.into(),
            uv: false,
        },
    );

    assert_eq!(h.get_posture().0, HouseholdPosture::Away);

    // Intent: home, night, away.
    let (st, body) = h.set(&intent, Purpose::Home);
    assert_eq!(st, 200, "{body}");
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["posture"], "home");
    assert!(v["not_after"].as_u64().unwrap() > T0);
    assert_eq!(h.get_posture().0, HouseholdPosture::Home);
    assert_eq!(h.set(&intent, Purpose::Night).0, 200);
    assert_eq!(h.get_posture().0, HouseholdPosture::Night);
    let (st, body) = h.set(&intent, Purpose::Away);
    assert_eq!(st, 200);
    assert!(body.contains(r#""not_after":null"#), "{body}");

    // Possession: night and away yes, home never.
    assert_eq!(h.set(&possession, Purpose::Night).0, 200);
    assert_eq!(h.set(&possession, Purpose::Away).0, 200);
    let (st, body) = h.set(&possession, Purpose::Home);
    assert_eq!(st, 401);
    assert_eq!(body, household_keys::server::REJECTED_BODY);
    assert_eq!(h.get_posture().0, HouseholdPosture::Away);

    // FIDO2 intent: UV sets home; the same key without UV is refused.
    assert_eq!(h.set(&fido_uv, Purpose::Home).0, 200);
    assert_eq!(h.get_posture().0, HouseholdPosture::Home);
    assert_eq!(h.set(&fido_no_uv, Purpose::Night).0, 401);

    // The rejection body is identical for an unknown handle and a wrong key,
    // so no response says whether a handle exists. (Two more rejections; the
    // door now has four in its window.)
    let ghost = cred([0x77; 16], 0x31, Binding::Es256);
    let wrong_key = cred(INTENT, 0x44, Binding::Es256);
    let (s1, b1) = h.set(&ghost, Purpose::Away);
    let (s2, b2) = h.set(&wrong_key, Purpose::Away);
    assert_eq!((s1, s2), (401, 401));
    assert_eq!(b1, b2);

    // A fifth rejection starts the cooldown: even a good credential is now
    // refused, with the same body.
    assert_eq!(h.set(&ghost, Purpose::Away).0, 401);
    let (st, body) = h.set(&intent, Purpose::Away);
    assert_eq!(
        (st, body.as_str()),
        (401, household_keys::server::REJECTED_BODY)
    );
    h.clock
        .mono
        .fetch_add(household_keys::ratelimit::COOLDOWN_MS, Ordering::SeqCst);
    assert_eq!(h.set(&intent, Purpose::Away).0, 200);

    // Home lapses on the hub's own tick.
    assert_eq!(h.set(&intent, Purpose::Home).0, 200);
    h.clock.unix.fetch_add(3600 + 120, Ordering::SeqCst);
    std::thread::sleep(std::time::Duration::from_millis(1300));
    assert_eq!(h.get_posture().0, HouseholdPosture::Away);

    // SENTINEL: no handle in any body, statement, seam write or log line.
    let addr = h.handle.addr();
    h.handle.stop();
    let logs = h.logs.lock().unwrap().clone();
    assert!(
        !logs.is_empty(),
        "the server logged nothing at all; the log capture is not wired"
    );
    let mut texts: Vec<String> = h.bodies.clone();
    texts.extend(logs.iter().cloned());
    let published = h.spy.0.lock().unwrap().clone();
    assert!(published.len() >= 10);
    for handle in [INTENT, POSSESSION, FIDO] {
        let hx = hex::encode(&handle);
        let b64 = base64::engine::general_purpose::STANDARD.encode(handle);
        for t in &texts {
            let lower = t.to_ascii_lowercase();
            assert!(!lower.contains(&hx), "hex handle in output: {t}");
            assert!(!t.contains(&b64), "base64 handle in output: {t}");
        }
        for s in &published {
            assert!(
                !s.windows(16).any(|w| w == handle),
                "raw handle in a statement"
            );
        }
    }
    // Nothing in the logs is per-request.
    assert!(
        logs.iter()
            .all(|l| l.contains("listening") || l.contains("stopped")),
        "{logs:?}"
    );
    let _ = addr;
}

#[test]
fn challenge_from_one_session_cannot_be_answered_on_another() {
    // Two paired bearers; a challenge fetched with the first is refused when
    // answered with the second.
    let clock = Arc::new(TestClock {
        mono: AtomicU64::new(1_000),
        unix: AtomicU64::new(T0),
    });
    let sk = ed25519_dalek::SigningKey::from_bytes(&[9; 32]);
    let checker = Checker::new(
        sk,
        roster(),
        CardStore::new(),
        0,
        3600,
        Box::new(Spy::default()),
        T0,
    )
    .unwrap();
    let t1 = "bearer-one";
    let t2 = "bearer-two";
    let hashes = vec![
        sha2::Sha256::digest(t1.as_bytes()).into(),
        sha2::Sha256::digest(t2.as_bytes()).into(),
    ];
    let srv = Server::bind(
        &ServerConfig {
            bind: "127.0.0.1:0".parse().unwrap(),
            allow_lan: false,
        },
        checker,
        hashes,
        None,
        clock,
        Arc::new(|_: &str| {}),
    )
    .unwrap();
    let h = srv.spawn().unwrap();
    let a = h.addr();
    let (st, body) = client::http(
        a,
        "POST",
        "/v1/challenge",
        Some(t1),
        Some(r#"{"purpose":"night"}"#),
    )
    .unwrap();
    assert_eq!(st, 200);
    let v: serde_json::Value = serde_json::from_str(&body).unwrap();
    assert_eq!(v["expires_in"], 10);
    let msg = base64::engine::general_purpose::STANDARD
        .decode(v["msg_b64"].as_str().unwrap())
        .unwrap();
    assert_eq!(msg.len(), household_keys::wire::MSG_LEN);
    let c = cred(INTENT, 0x31, Binding::Es256);
    let (sig, _) = c.answer(&msg, Purpose::Night).unwrap();
    let cmd = serde_json::json!({
        "challenge_id": v["challenge_id"],
        "handle": hex::encode(&INTENT),
        "signature_b64": base64::engine::general_purpose::STANDARD.encode(&sig),
    })
    .to_string();
    let (st, _) = client::http(a, "POST", "/v1/command", Some(t2), Some(&cmd)).unwrap();
    assert_eq!(st, 401, "cross-session answer accepted");
    // And the challenge is burned: the right session cannot use it either.
    let (st, _) = client::http(a, "POST", "/v1/command", Some(t1), Some(&cmd)).unwrap();
    assert_eq!(st, 401, "challenge survived a cross-session attempt");
    h.stop();
}
