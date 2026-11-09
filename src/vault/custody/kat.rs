//! Cross-implementation known-answer tests (spec §2.8): every value in
//! `spec/fixtures/threshold_custody_v1.json` was written by a Python 3
//! standard-library implementation of its own
//! (`spec/fixtures/gen_threshold_custody_v1.py`); these tests rebuild each
//! one with the Rust code and compare bytes. A disagreement is a stop-ship,
//! never a fixture edit.
//!
//! [`KatRng`] is the fixture's deterministic stream, and it is normative:
//! `block(label, i) = SHA-512("securacv-threshold-custody-kat:v1" ‖
//! u32le(len label) ‖ label ‖ u64le(i))`, each draw taking the next blocks.

use std::sync::OnceLock;

use curve25519_dalek::constants::RISTRETTO_BASEPOINT_POINT;
use curve25519_dalek::ristretto::RistrettoPoint;
use curve25519_dalek::scalar::Scalar;
use ed25519_dalek::{Signer, SigningKey};
use serde_json::Value;
use sha2::{Digest, Sha256, Sha512};

use super::container::{
    decode_mkc1_header, encode_mkc1_with, open_mkc1, vault_x25519_from_master,
    CRYPTO_MODE_CLASSICAL, CRYPTO_MODE_HYBRID,
};
use super::group::{
    decode_scalar, encode_point, generator_h, random_array, Commitments, CustodyRng,
};
use super::hpke::{
    box2_psk_id, derive_keypair, open_base, open_psk, seal_base_derand, seal_psk_derand,
    setup_base_s_export, setup_psk_s_export,
};
use super::recipient::{
    RecipientPublic, RecipientSecret, HOLDER_ROLE_RECOVERY, HOLDER_ROLE_TRUSTEE,
};
use super::share::{RosterEntry, ShareFile, ShareHeader};
use super::split_record::SplitRecord;
use super::vss::{
    check_share, deal_and_prove, deal_with_coefficients, lagrange_at_zero, reconstruct, Kek,
    SecretShare,
};
use super::*;
use crate::crypto::signatures::{sign_ed25519_only, DOMAIN_CUSTODY_RECIPIENT_BINDING};
use crate::vault::crypto::keyguard_kek;

const FIXTURE: &str = include_str!("../../../spec/fixtures/threshold_custody_v1.json");

/// The fixture's deterministic stream.
pub(crate) struct KatRng {
    label: Vec<u8>,
    next: u64,
}

impl KatRng {
    pub(crate) fn new(label: &str) -> Self {
        KatRng {
            label: label.as_bytes().to_vec(),
            next: 0,
        }
    }

    fn block(&mut self) -> [u8; 64] {
        let mut h = Sha512::new();
        h.update(b"securacv-threshold-custody-kat:v1");
        h.update((self.label.len() as u32).to_le_bytes());
        h.update(&self.label);
        h.update(self.next.to_le_bytes());
        self.next += 1;
        h.finalize().into()
    }
}

impl CustodyRng for KatRng {
    fn fill(&mut self, out: &mut [u8]) {
        let mut at = 0;
        while at < out.len() {
            let block = self.block();
            let n = (out.len() - at).min(64);
            out[at..at + n].copy_from_slice(&block[..n]);
            at += n;
        }
    }
}

/// A seam RNG that hands out given bytes (the HPKE vectors' ikmE).
struct GivenRng(Vec<u8>);

impl CustodyRng for GivenRng {
    fn fill(&mut self, out: &mut [u8]) {
        out.copy_from_slice(&self.0[..out.len()]);
        self.0.drain(..out.len());
    }
}

pub(crate) fn fx() -> &'static Value {
    static V: OnceLock<Value> = OnceLock::new();
    V.get_or_init(|| serde_json::from_str(FIXTURE).expect("the fixture is JSON"))
}

pub(crate) fn s(v: &Value) -> &str {
    v.as_str().expect("a string")
}

pub(crate) fn h(v: &Value) -> Vec<u8> {
    hex::decode(s(v)).expect("lowercase hex")
}

pub(crate) fn h32(v: &Value) -> [u8; 32] {
    h(v).try_into().expect("32 bytes")
}

fn scalar(v: &Value) -> Scalar {
    decode_scalar(&h32(v)).expect("a canonical scalar")
}

fn u8_of(v: &Value) -> u8 {
    v.as_u64().expect("a number") as u8
}

fn split_names() -> [&'static str; 2] {
    ["operational", "recovery"]
}

fn split_commitments(split: &Value) -> Commitments {
    let points: Vec<[u8; 32]> = split["commitments"]
        .as_array()
        .unwrap()
        .iter()
        .map(h32)
        .collect();
    Commitments::decode(&points).unwrap()
}

fn holder_role(id: &str) -> u8 {
    match s(&fx()["recipients"][id]["role"]) {
        "trustee" => HOLDER_ROLE_TRUSTEE,
        _ => HOLDER_ROLE_RECOVERY,
    }
}

pub(crate) fn recipient_secret(id: &str) -> RecipientSecret {
    RecipientSecret::from_parts(
        id,
        holder_role(id),
        h32(&fx()["recipients"][id]["signing_public_key"]),
        h32(&fx()["recipients"][id]["x25519_secret"]),
        None,
    )
}

pub(crate) fn recipient_public(id: &str) -> RecipientPublic {
    RecipientPublic::from_json(s(&fx()["recipients"][id]["recipient_json"]).as_bytes()).unwrap()
}

/// The split a holder belongs to, and their index in it.
fn holder_split(id: &str) -> (&'static str, u8) {
    for name in split_names() {
        let ids = fx()["splits"][name]["holders"].as_array().unwrap();
        if let Some(i) = ids.iter().position(|x| s(x) == id) {
            return (name, i as u8 + 1);
        }
    }
    panic!("{id} is in no split");
}

pub(crate) fn fixture_share(id: &str) -> SecretShare {
    let (name, index) = holder_split(id);
    let row = &fx()["splits"][name]["shares"][usize::from(index) - 1];
    let mut b = [0u8; 64];
    b[..32].copy_from_slice(&h32(&row["s"]));
    b[32..].copy_from_slice(&h32(&row["t"]));
    SecretShare::from_bytes(index, &b).unwrap()
}

/// The header of `id`'s share as the fixture's split describes it.
fn fixture_header(id: &str, protection: u8, release_box: u8, container: &[u8]) -> ShareHeader {
    let (name, index) = holder_split(id);
    let split = &fx()["splits"][name];
    let roster: Vec<RosterEntry> = split["holders"]
        .as_array()
        .unwrap()
        .iter()
        .enumerate()
        .map(|(i, hid)| RosterEntry {
            index: i as u8 + 1,
            id: s(hid).to_string(),
            signing_public_key: h32(&fx()["recipients"][s(hid)]["signing_public_key"]),
            recipient_key_hash: h32(&fx()["recipients"][s(hid)]["recipient_key_hash"]),
        })
        .collect();
    let own = roster[usize::from(index) - 1].clone();
    ShareHeader {
        suite: SUITE_V1,
        protection,
        role: u8_of(&split["role"]),
        release_box,
        split_id: h(&split["split_id"]).try_into().unwrap(),
        container_sha256: Sha256::digest(container).into(),
        device_public_key: h32(&fx()["device"]["public_key"]),
        issued_bucket_start: 1_794_312_000,
        n: u8_of(&split["n"]),
        m: u8_of(&split["m"]),
        index,
        holder: own.id.clone(),
        recipient_key_hash: own.recipient_key_hash,
        commitments: split_commitments(split),
        roster,
        container: container.to_vec(),
    }
}

fn classical_container() -> Vec<u8> {
    h(&fx()["mkc1"]["classical"]["container_hex"])
}

fn fixture_kek() -> Kek {
    Kek::from_scalars(scalar(&fx()["kek"]["k"]), scalar(&fx()["kek"]["r"]))
}

/// Run one negative case and return the error it produces, or `None` if
/// the Rust code accepted it.
fn run_negative(case: &Value) -> Option<String> {
    let err = |r: Result<(), CustodyError>| r.err().map(|e| e.to_string());
    match s(&case["kind"]) {
        "reconstruct" => {
            let split = &fx()["splits"][s(&case["split"])];
            let c = split_commitments(split);
            let mut shares = Vec::new();
            for row in case["shares"].as_array().unwrap() {
                let mut b = [0u8; 64];
                b[..32].copy_from_slice(&h32(&row["s"]));
                b[32..].copy_from_slice(&h32(&row["t"]));
                match SecretShare::from_bytes(u8_of(&row["index"]), &b) {
                    Ok(share) => shares.push(share),
                    Err(e) => return Some(e.to_string()),
                }
            }
            let picked: Vec<&SecretShare> = shares.iter().collect();
            err(reconstruct(&h32(&fx()["kek"]["kek_commitment"]), &c, &picked).map(|_| ()))
        }
        "lagrange" => {
            let indices: Vec<u8> = case["indices"]
                .as_array()
                .unwrap()
                .iter()
                .map(u8_of)
                .collect();
            err(lagrange_at_zero(&indices).map(|_| ()))
        }
        "scalar_decode" => err(decode_scalar(&h32(&case["input"])).map(|_| ())),
        "commitments_decode" => {
            let points: Vec<[u8; 32]> = case["input"].as_array().unwrap().iter().map(h32).collect();
            err(Commitments::decode(&points).map(|_| ()))
        }
        "share_file" => match ShareFile::decode(&h(&case["file_hex"])) {
            Err(e) => Some(e.to_string()),
            Ok(file) => err(file
                .open_transport(&recipient_secret(s(&case["holder"])))
                .map(|_| ())),
        },
        "mkc1_header" => err(decode_mkc1_header(&h(&case["container_hex"])).map(|_| ())),
        "mkc1_open" => {
            let kek = Kek::from_scalars(scalar(&case["k"]), scalar(&case["r"]));
            err(open_mkc1(&h(&case["container_hex"]), &kek).map(|_| ()))
        }
        "tek1_parse" => err(RecipientSecret::unwrap(
            s(&case["file_text"]).as_bytes(),
            "kat passphrase",
        )
        .map(|_| ())),
        other => panic!("unknown negative kind {other}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn kat_standards() {
        let std = &fx()["standards"];
        // ristretto255: the encodings of i*G, decoded back.
        let table = std["ristretto255_small_multiples"]["encodings"]
            .as_array()
            .unwrap();
        assert_eq!(table.len(), 16);
        for (i, want) in table.iter().enumerate() {
            let p = RISTRETTO_BASEPOINT_POINT * Scalar::from(i as u8);
            assert_eq!(hex::encode(encode_point(&p)), s(want), "{i}*G");
            if i > 0 {
                let back = super::super::group::decode_commitment(&h32(want), i).unwrap();
                assert_eq!(back, p);
            }
        }
        // The Elligator map and the one-way map.
        let ell = std["ristretto255_elligator"]["vectors"].as_array().unwrap();
        assert_eq!(ell.len(), 16);
        for v in ell {
            let p = RistrettoPoint::map_to_curve(h32(&v["input"]));
            assert_eq!(hex::encode(encode_point(&p)), s(&v["output"]));
        }
        let owm = std["ristretto255_one_way_map"]["vectors"]
            .as_array()
            .unwrap();
        assert_eq!(owm.len(), 11);
        for v in owm {
            let input: [u8; 64] = h(&v["input"]).try_into().unwrap();
            let p = RistrettoPoint::from_uniform_bytes(&input);
            assert_eq!(hex::encode(encode_point(&p)), s(&v["output"]));
        }
        // HPKE: the draft's Base and PSK vectors, through the custody
        // module's seal, open and export.
        for (name, psk_mode) in [("hpke_base", false), ("hpke_psk", true)] {
            let v = &std[name];
            let (sk_e, pk_e) = derive_keypair(&h(&v["ikmE"]));
            assert_eq!(sk_e.as_ref(), &h(&v["skEm"])[..], "{name}");
            assert_eq!(pk_e.to_vec(), h(&v["pkEm"]), "{name}");
            let (sk_r, pk_r) = derive_keypair(&h(&v["ikmR"]));
            assert_eq!(sk_r.as_ref(), &h(&v["skRm"])[..], "{name}");
            assert_eq!(pk_r.to_vec(), h(&v["pkRm"]), "{name}");
            let ikm_e = h32(&v["ikmE"]);
            let (info, aad, pt) = (h(&v["info"]), h(&v["aad"]), h(&v["pt"]));
            let (psk, psk_id) = if psk_mode {
                (h(&v["psk"]), h(&v["psk_id"]))
            } else {
                (Vec::new(), Vec::new())
            };
            let (enc, ct) = if psk_mode {
                seal_psk_derand(&pk_r, &info, &aad, &pt, &psk, &psk_id, &ikm_e).unwrap()
            } else {
                seal_base_derand(&pk_r, &info, &aad, &pt, &ikm_e).unwrap()
            };
            assert_eq!(enc.to_vec(), h(&v["enc"]), "{name}");
            assert_eq!(ct, h(&v["ct"]), "{name}");
            let opened = if psk_mode {
                open_psk(&sk_r, &enc, &info, &aad, &ct, &psk, &psk_id).unwrap()
            } else {
                open_base(&sk_r, &enc, &info, &aad, &ct).unwrap()
            };
            assert_eq!(opened.to_vec(), pt, "{name}");
            let contexts = v["exporter_contexts"].as_array().unwrap();
            for (ctx, want) in contexts.iter().zip(v["exports"].as_array().unwrap()) {
                let mut rng = GivenRng(ikm_e.to_vec());
                let (enc2, exported) = if psk_mode {
                    setup_psk_s_export(&pk_r, &info, &h(ctx), &psk, &psk_id, &mut rng).unwrap()
                } else {
                    setup_base_s_export(&pk_r, &info, &h(ctx), &mut rng).unwrap()
                };
                assert_eq!(enc2, enc);
                assert_eq!(exported.to_vec(), h(want), "{name} export");
            }
        }
        // RFC 8032 section 7.1 test 1.
        let v = &std["ed25519_rfc8032_test1"];
        let key = SigningKey::from_bytes(&h32(&v["secret_key"]));
        assert_eq!(key.verifying_key().to_bytes().to_vec(), h(&v["public_key"]));
        assert_eq!(
            key.sign(&h(&v["message"])).to_bytes().to_vec(),
            h(&v["signature"])
        );
    }

    #[test]
    fn kat_generator_h() {
        assert_eq!(
            hex::encode(encode_point(&generator_h())),
            s(&fx()["generator_h"])
        );
        let digest: [u8; 64] = Sha512::digest(GENERATOR_H_LABEL).into();
        assert_eq!(
            hex::encode(encode_point(&RistrettoPoint::from_uniform_bytes(&digest))),
            s(&fx()["generator_h"])
        );
    }

    #[test]
    fn kat_dealing_from_fixture_coefficients() {
        // The KEK and its salt from the "kek" stream.
        let mut rng = KatRng::new("kek");
        let kek = Kek::generate(&mut rng);
        let kek_salt: [u8; 16] = random_array(&mut rng);
        let (k, r) = kek.scalars();
        let fk = &fx()["kek"];
        assert_eq!(k, scalar(&fk["k"]));
        assert_eq!(r, scalar(&fk["r"]));
        assert_eq!(kek_salt.to_vec(), h(&fk["kek_salt"]));
        assert_eq!(kek.commitment(), h32(&fk["kek_commitment"]));
        assert_eq!(*kek.wrap_key(&kek_salt), h32(&fk["wrap_key"]));
        for name in split_names() {
            let split = &fx()["splits"][name];
            let (n, m, role) = (
                u8_of(&split["n"]),
                u8_of(&split["m"]),
                u8_of(&split["role"]),
            );
            let f: Vec<Scalar> = split["f"].as_array().unwrap().iter().map(scalar).collect();
            let g: Vec<Scalar> = split["g"].as_array().unwrap().iter().map(scalar).collect();
            assert_eq!((f[0], g[0]), (k, r), "{name}: a_0 = k, b_0 = r");
            let indices: Vec<u8> = (1..=m).collect();
            let (c, shares) = deal_with_coefficients(&f, &g, &indices).unwrap();
            let want: Vec<[u8; 32]> = split["commitments"]
                .as_array()
                .unwrap()
                .iter()
                .map(h32)
                .collect();
            assert_eq!(c.encode(), want, "{name} commitments");
            assert_eq!(c.kek_commitment(), h32(&fk["kek_commitment"]));
            let split_id: [u8; 16] = h(&split["split_id"]).try_into().unwrap();
            assert_eq!(
                c.sha256(SUITE_V1, role, &split_id, m).to_vec(),
                h(&split["commitments_sha256"])
            );
            for (share, row) in shares.iter().zip(split["shares"].as_array().unwrap()) {
                assert_eq!(share.index, u8_of(&row["index"]));
                assert_eq!(share.scalars(), (scalar(&row["s"]), scalar(&row["t"])));
                assert_eq!(
                    hex::encode(encode_point(&c.public_share(share.index))),
                    s(&row["public_share"])
                );
                assert!(check_share(&c, share));
            }
            // deal_and_prove over the split's stream draws the same split
            // id and coefficients, and checks every n-subset.
            let mut rng = KatRng::new(&format!("split/{name}"));
            let dealt = deal_and_prove(&kek, n, &indices, &mut rng).unwrap();
            assert_eq!(dealt.split_id, split_id);
            assert_eq!(dealt.commitments.encode(), want);
            for (a, b) in dealt.shares.iter().zip(&shares) {
                assert_eq!(a.scalars(), b.scalars());
            }
            let expect_subsets = if name == "operational" { 10 } else { 15 };
            assert_eq!(dealt.subsets_checked, expect_subsets);
        }
        assert_eq!(
            fx()["splits"]["operational"]["commitments"][0],
            fx()["splits"]["recovery"]["commitments"][0]
        );
    }

    #[test]
    fn kat_lagrange_subsets() {
        let kek_commitment = h32(&fx()["kek"]["kek_commitment"]);
        let mut checked = 0;
        for name in split_names() {
            let split = &fx()["splits"][name];
            let c = split_commitments(split);
            let ids = split["holders"].as_array().unwrap();
            for row in split["subsets"].as_array().unwrap() {
                let indices: Vec<u8> = row["indices"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(u8_of)
                    .collect();
                let lambda = lagrange_at_zero(&indices).unwrap();
                let want: Vec<Scalar> = row["lagrange"]
                    .as_array()
                    .unwrap()
                    .iter()
                    .map(scalar)
                    .collect();
                assert_eq!(lambda, want, "{name} {indices:?}");
                let shares: Vec<SecretShare> = indices
                    .iter()
                    .map(|&x| fixture_share(s(&ids[usize::from(x) - 1])))
                    .collect();
                let picked: Vec<&SecretShare> = shares.iter().collect();
                let rebuilt = reconstruct(&kek_commitment, &c, &picked).unwrap();
                assert_eq!(rebuilt.scalars(), (scalar(&row["k"]), scalar(&row["r"])));
                checked += 1;
            }
        }
        assert_eq!(checked, 10 + 4);
    }

    #[test]
    fn kat_share_files_byte_identical() {
        let container = classical_container();
        let files = fx()["share_files"].as_object().unwrap();
        assert_eq!(files.len(), 11);
        for (id, want) in files {
            let (name, _) = holder_split(id);
            let release_box = if name == "operational" {
                BOX_HPKE_X25519
            } else {
                0
            };
            let header = fixture_header(id, PROTECTION_TRANSPORT_BOX1, release_box, &container);
            assert_eq!(header.to_bytes(), h(&want["header_hex"]), "{id} header");
            let ikm_e = h32(&want["ikm_e"]);
            assert_eq!(
                ikm_e,
                random_array::<32>(&mut KatRng::new(&format!("share/{id}")))
            );
            let file = ShareFile::seal_transport_box1_derand(
                header,
                &fixture_share(id),
                &recipient_public(id),
                &ikm_e,
            )
            .unwrap();
            assert_eq!(
                hex::encode(file.encode()),
                s(&want["file_hex"]),
                "{id} file"
            );
            assert_eq!(file.to_text(), s(&want["text"]), "{id} text");
            assert_eq!(file.header_bytes(), h(&want["header_hex"]));
        }
    }

    #[test]
    fn kat_share_files_open_with_fixture_recipients() {
        for (id, want) in fx()["share_files"].as_object().unwrap() {
            let file = ShareFile::from_text(s(&want["text"]).as_bytes()).unwrap();
            let raw = h(&want["file_hex"]);
            assert!(file.file_check_ok(&raw));
            assert_eq!(file.header.holder, *id);
            let opened = file.open_transport(&recipient_secret(id)).unwrap();
            assert_eq!(opened.scalars(), fixture_share(id).scalars(), "{id}");
            assert!(check_share(&file.header.commitments, &opened));
            // The split record every holder compares against.
            let (name, _) = holder_split(id);
            let record_text = s(&fx()["split_records"][name]);
            let record = SplitRecord::from_json(record_text.as_bytes()).unwrap();
            assert!(record.matches(&file.header), "{id}");
            assert_eq!(
                SplitRecord::from_header(&file.header).to_json_text(),
                record_text
            );
            // The container copy is the fixture's classical container.
            assert_eq!(file.header.container, classical_container());
        }
    }

    #[test]
    fn kat_box2_psk_layer() {
        let b = &fx()["share_file_box2"];
        let hybrid = h(&fx()["mkc1"]["hybrid"]["container_hex"]);
        let header = fixture_header(
            "alice",
            PROTECTION_TRANSPORT_BOX2,
            BOX_HPKE_X25519_PSK_MLKEM768,
            &hybrid,
        );
        assert_eq!(header.to_bytes(), h(&b["header_hex"]));
        let mut rng = KatRng::new("box2");
        let ss_m: [u8; 32] = random_array(&mut rng);
        let mut ct_m = vec![0u8; super::super::hpke::MLKEM768_CT_LEN];
        rng.fill(&mut ct_m);
        let ikm_e: [u8; 32] = random_array(&mut rng);
        assert_eq!(ss_m, h32(&b["ss_m"]));
        assert_eq!(ct_m, h(&b["ct_m"]));
        assert_eq!(ikm_e, h32(&b["ikm_e"]));
        assert_eq!(box2_psk_id(&ct_m), h(&b["psk_id"]));
        let file = ShareFile::seal_transport_box2_layer_derand(
            header,
            &fixture_share("alice"),
            &recipient_public("alice"),
            &ss_m,
            &ct_m,
            &ikm_e,
        )
        .unwrap();
        assert_eq!(hex::encode(file.encode()), s(&b["file_hex"]));
        let decoded = ShareFile::decode(&h(&b["file_hex"])).unwrap();
        let opened = decoded
            .open_transport_box2_layer(&recipient_secret("alice"), &ss_m)
            .unwrap();
        assert_eq!(opened.scalars(), fixture_share("alice").scalars());
        assert!(decoded
            .open_transport_box2_layer(&recipient_secret("alice"), &[0; 32])
            .is_err());
    }

    #[test]
    fn kat_at_rest_and_tek1_aead_layers() {
        // The at-rest share: alice's transport share rewrapped.
        let a = &fx()["share_file_at_rest"];
        let mut rng = KatRng::new("at-rest");
        let argon_out: [u8; 32] = random_array(&mut rng);
        let salt: [u8; 16] = random_array(&mut rng);
        let nonce: [u8; 12] = random_array(&mut rng);
        assert_eq!(argon_out, h32(&a["argon_out"]));
        assert_eq!(salt.to_vec(), h(&a["salt"]));
        assert_eq!(nonce.to_vec(), h(&a["nonce"]));
        let transport = ShareFile::decode(&h(&fx()["share_files"]["alice"]["file_hex"])).unwrap();
        let share = transport
            .open_transport(&recipient_secret("alice"))
            .unwrap();
        let protected = transport
            .protect_with_kek(&share, &argon_out, salt, nonce)
            .unwrap();
        assert_eq!(hex::encode(protected.encode()), s(&a["file_hex"]));
        let decoded = ShareFile::decode(&h(&a["file_hex"])).unwrap();
        assert_eq!(decoded.header.protection, PROTECTION_AT_REST);
        assert_eq!(
            decoded.open_at_rest_with_kek(&argon_out).unwrap().scalars(),
            share.scalars()
        );

        // TEK1: alice's recipient key under a given Argon2id output.
        let t = &fx()["tek1"];
        let mut rng = KatRng::new("tek1");
        let argon_out: [u8; 32] = random_array(&mut rng);
        let salt: [u8; 16] = random_array(&mut rng);
        let nonce: [u8; 12] = random_array(&mut rng);
        assert_eq!(argon_out, h32(&t["argon_out"]));
        let tek1 = recipient_secret("alice")
            .wrap_with_kek(&argon_out, &salt, &nonce)
            .unwrap();
        assert_eq!(hex::encode(&tek1), s(&t["tek1_hex"]));
        assert_eq!(
            format!("{RECIPIENT_KEY_TEXT_PREFIX} {}\n", hex::encode(&tek1)),
            s(&t["file_text"])
        );
        let (opened, header) = RecipientSecret::open_with_kek(&tek1, &argon_out).unwrap();
        assert_eq!(
            opened.x25519_secret(),
            recipient_secret("alice").x25519_secret()
        );
        assert_eq!(header.holder, "alice");
        let spk = h32(&fx()["recipients"]["alice"]["signing_public_key"]);
        assert_eq!(header.signing_public_key, spk);
        assert_eq!(opened.signing_public_key(), &spk);
        assert_eq!(
            header.recipient_key_hash,
            h32(&fx()["recipients"]["alice"]["recipient_key_hash"])
        );
        assert_eq!(
            super::super::recipient::tek1_from_text(s(&t["file_text"]).as_bytes()).unwrap(),
            tek1
        );
    }

    #[test]
    fn kat_argon2id_rust_pins() {
        for section in ["share_file_at_rest", "tek1"] {
            let pin = &fx()[section]["argon2id_rust_pin"];
            assert_eq!(s(&pin["source"]), "rust argon2 0.6, not cross-implemented");
            assert_eq!(
                pin["params"]["m_kib"].as_u64(),
                Some(u64::from(ARGON2_M_KIB))
            );
            assert_eq!(pin["params"]["t"].as_u64(), Some(u64::from(ARGON2_T)));
            assert_eq!(pin["params"]["p"].as_u64(), Some(u64::from(ARGON2_P)));
            assert_eq!(pin["salt"], fx()[section]["salt"]);
            let out = keyguard_kek(
                s(&pin["passphrase"]),
                &h(&pin["salt"]),
                ARGON2_M_KIB,
                ARGON2_T,
                ARGON2_P,
            )
            .unwrap();
            assert_eq!(hex::encode(*out), s(&pin["output"]), "{section}");
        }
    }

    #[test]
    fn kat_mkc1_byte_identical() {
        let kek = fixture_kek();
        let kek_salt: [u8; 16] = h(&fx()["kek"]["kek_salt"]).try_into().unwrap();
        let c = &fx()["mkc1"]["classical"];
        let mut rng = KatRng::new("mkc1");
        let master: [u8; 32] = random_array(&mut rng);
        let nonce: [u8; 12] = random_array(&mut rng);
        assert_eq!(master, h32(&c["master_key"]));
        assert_eq!(nonce.to_vec(), h(&c["nonce"]));
        let (vx_sk, vx_pk) = vault_x25519_from_master(&master);
        assert_eq!(*vx_sk, h32(&c["vault_x25519_secret"]));
        assert_eq!(vx_pk, h32(&c["vault_x25519_public"]));
        let classical =
            encode_mkc1_with(&kek, kek_salt, nonce, CRYPTO_MODE_CLASSICAL, &master, None).unwrap();
        assert_eq!(hex::encode(&classical), s(&c["container_hex"]));
        assert_eq!(Sha256::digest(&classical).to_vec(), h(&c["sha256"]));
        let opened = open_mkc1(&classical, &kek).unwrap();
        assert_eq!(*opened.master_key, master);
        assert!(opened.kem.is_none());

        let y = &fx()["mkc1"]["hybrid"];
        let mut rng = KatRng::new("mkc1-kem");
        let mut pk = vec![0u8; super::super::hpke::MLKEM768_PK_LEN];
        let mut sk = vec![0u8; super::super::hpke::MLKEM768_SK_LEN];
        rng.fill(&mut pk);
        rng.fill(&mut sk);
        let nonce: [u8; 12] = random_array(&mut rng);
        assert_eq!(Sha256::digest(&pk).to_vec(), h(&y["kem_public_sha256"]));
        assert_eq!(Sha256::digest(&sk).to_vec(), h(&y["kem_secret_sha256"]));
        let hybrid = encode_mkc1_with(
            &kek,
            kek_salt,
            nonce,
            CRYPTO_MODE_HYBRID,
            &master,
            Some((&pk, &sk)),
        )
        .unwrap();
        assert_eq!(hex::encode(&hybrid), s(&y["container_hex"]));
        let header = decode_mkc1_header(&hybrid).unwrap();
        assert_eq!(header.vault_x25519_public, h32(&y["vault_x25519_public"]));
        let opened = open_mkc1(&hybrid, &kek).unwrap();
        let (got_pk, got_sk) = opened.kem.unwrap();
        assert_eq!((got_pk, got_sk.to_vec()), (pk, sk));
    }

    #[test]
    fn kat_binding_signatures_verify() {
        let recipients = fx()["recipients"].as_object().unwrap();
        assert_eq!(recipients.len(), 11);
        for (id, want) in recipients {
            let mut rng = KatRng::new(&format!("recipient/{id}"));
            let seed: [u8; 32] = random_array(&mut rng);
            assert_eq!(seed, h32(&want["signing_seed"]));
            let key = SigningKey::from_bytes(&seed);
            let spk = key.verifying_key().to_bytes();
            assert_eq!(spk, h32(&want["signing_public_key"]));
            let secret =
                RecipientSecret::generate(id, holder_role(id), &spk, false, &mut rng).unwrap();
            assert_eq!(secret.x25519_secret(), &h32(&want["x25519_secret"]));
            assert_eq!(secret.x25519_public(), h32(&want["x25519_public"]));
            let unsigned = secret.public_unsigned();
            let hash = unsigned.key_hash().unwrap();
            assert_eq!(hash, h32(&want["recipient_key_hash"]));
            assert_eq!(secret.key_hash(&spk), hash);
            let sig = sign_ed25519_only(DOMAIN_CUSTODY_RECIPIENT_BINDING, &key, &hash);
            assert_eq!(sig.to_vec(), h(&want["binding_signature"]), "{id}");
            let public = unsigned.with_binding(&sig);
            assert_eq!(public.to_json_text(), s(&want["recipient_json"]), "{id}");
            let parsed = recipient_public(id);
            parsed.check_binding_ed25519().unwrap();
            assert_eq!(parsed.fingerprint().unwrap(), s(&want["fingerprint"]));
        }
        let seed: [u8; 32] = random_array(&mut KatRng::new("device"));
        assert_eq!(seed, h32(&fx()["device"]["seed"]));
        assert_eq!(
            SigningKey::from_bytes(&seed).verifying_key().to_bytes(),
            h32(&fx()["device"]["public_key"])
        );
    }

    #[test]
    fn kat_negative_cases() {
        let cases = fx()["negative"].as_array().unwrap();
        assert!(cases.len() >= 30, "{}", cases.len());
        let mut kinds = std::collections::BTreeSet::new();
        for case in cases {
            let name = s(&case["name"]);
            let got = run_negative(case).unwrap_or_else(|| panic!("{name}: accepted"));
            let want = case
                .get("error")
                .or_else(|| case.get("open_error"))
                .map(s)
                .unwrap();
            assert_eq!(got, want, "{name}");
            if case.get("open_error").is_some() {
                // Decoded fine: the refusal is the AEAD's.
                assert!(ShareFile::decode(&h(&case["file_hex"])).is_ok(), "{name}");
            }
            kinds.insert(s(&case["kind"]).to_string());
        }
        assert_eq!(
            kinds.into_iter().collect::<Vec<_>>(),
            [
                "commitments_decode",
                "lagrange",
                "mkc1_header",
                "mkc1_open",
                "reconstruct",
                "scalar_decode",
                "share_file",
                "tek1_parse"
            ]
        );
    }

    /// Every hex secret the fixture holds.
    fn fixture_secrets() -> Vec<String> {
        let f = fx();
        let mut out = vec![
            s(&f["kek"]["k"]).to_string(),
            s(&f["kek"]["r"]).to_string(),
            s(&f["kek"]["wrap_key"]).to_string(),
            s(&f["device"]["seed"]).to_string(),
            s(&f["mkc1"]["classical"]["master_key"]).to_string(),
            s(&f["mkc1"]["classical"]["vault_x25519_secret"]).to_string(),
            s(&f["share_file_at_rest"]["argon_out"]).to_string(),
            s(&f["tek1"]["argon_out"]).to_string(),
            s(&f["share_file_box2"]["ss_m"]).to_string(),
        ];
        for name in split_names() {
            let split = &f["splits"][name];
            for key in ["f", "g"] {
                out.extend(
                    split[key]
                        .as_array()
                        .unwrap()
                        .iter()
                        .map(|v| s(v).to_string()),
                );
            }
            for row in split["shares"].as_array().unwrap() {
                out.push(s(&row["s"]).to_string());
                out.push(s(&row["t"]).to_string());
            }
        }
        for (_, r) in f["recipients"].as_object().unwrap() {
            for key in ["signing_seed", "x25519_secret", "x25519_ikm"] {
                out.push(s(&r[key]).to_string());
            }
        }
        out
    }

    #[test]
    fn no_error_string_contains_a_fixture_secret() {
        let mut errors: Vec<String> = fx()["negative"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(run_negative)
            .collect();
        // Refusals that hold a secret in hand when they fire.
        let alice = ShareFile::decode(&h(&fx()["share_files"]["alice"]["file_hex"])).unwrap();
        errors.push(
            alice
                .open_transport(&recipient_secret("bob"))
                .unwrap_err()
                .to_string(),
        );
        let at_rest = ShareFile::decode(&h(&fx()["share_file_at_rest"]["file_hex"])).unwrap();
        errors.push(
            at_rest
                .open_at_rest_with_kek(&[1; 32])
                .unwrap_err()
                .to_string(),
        );
        let tek1 = h(&fx()["tek1"]["tek1_hex"]);
        errors.push(
            RecipientSecret::open_with_kek(&tek1, &[1; 32])
                .unwrap_err()
                .to_string(),
        );
        let wrong = Kek::from_scalars(scalar(&fx()["kek"]["r"]), scalar(&fx()["kek"]["k"]));
        errors.push(
            open_mkc1(&classical_container(), &wrong)
                .unwrap_err()
                .to_string(),
        );
        let c = split_commitments(&fx()["splits"]["operational"]);
        let two = [fixture_share("alice"), fixture_share("bob")];
        errors.push(
            reconstruct(&c.kek_commitment(), &c, &[&two[0], &two[1]])
                .unwrap_err()
                .to_string(),
        );
        // Debug forms of every secret type, built from fixture secrets.
        let mut shown = vec![
            format!("{:?}", fixture_kek()),
            format!("{:?}", fixture_share("alice")),
            format!("{:?}", recipient_secret("alice")),
            format!(
                "{:?}",
                open_mkc1(&classical_container(), &fixture_kek()).unwrap()
            ),
        ];
        shown.append(&mut errors);
        assert!(shown.len() > 40);
        let secrets = fixture_secrets();
        assert!(secrets.len() > 60);
        for text in &shown {
            // No 64-hex run at all (no full hash, key or secret).
            let longest_hex_run = text
                .split(|c: char| !c.is_ascii_hexdigit())
                .map(str::len)
                .max()
                .unwrap_or(0);
            assert!(longest_hex_run < 64, "{text}");
            for secret in &secrets {
                for at in (0..=secret.len() - 16).step_by(2) {
                    assert!(
                        !text.contains(&secret[at..at + 16]),
                        "a fixture secret appears in: {text}"
                    );
                }
            }
        }
    }
}
