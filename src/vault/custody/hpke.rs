//! The RFC 9180 HPKE subset the custody boxes use (spec §2.3): single-shot,
//! sequence 0, base and PSK modes, and the exporter.
//!
//! Suite: KEM `0x0020` DHKEM(X25519, HKDF-SHA256), KDF `0x0001`
//! HKDF-SHA256, AEAD `0x0003` ChaCha20-Poly1305. X25519 is dalek's
//! `MontgomeryPoint::mul_clamped`, and both sides refuse the all-zero DH
//! output. Pinned by the HPKE draft's own Base and PSK vectors for this
//! suite (`tests::draft_base_vector_x25519_chacha`,
//! `tests::draft_psk_vector_x25519_chacha`).
//!
//! Two boxes are built on it:
//!
//! - **box 1** (`hpke-x25519`): `SealBase(pkR, info, aad, pt)`; `enc` is 32
//!   bytes.
//! - **box 2** (`hpke-x25519-psk-mlkem768`): ML-KEM-768 encapsulates to the
//!   recipient's ML-KEM key, and the shared secret is the HPKE PSK, with
//!   `psk_id = "securacv:custody:mlkem768-psk-id:v1" ‖ SHA-256(ct_M)`;
//!   `enc = ct_M (1088) ‖ enc_X (32)`. HPKE's PSK mode is expected to stay
//!   secret unless both the X25519 secret and the PSK are compromised; the
//!   HPKE draft states that this hybrid property is not proven.

use curve25519_dalek::montgomery::MontgomeryPoint;
use hkdf::Hkdf;
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use super::group::{random_array, CustodyRng};
use super::{CustodyError, MLKEM_PSK_ID_PREFIX};
use crate::vault::crypto::{decrypt_payload, encrypt_payload};

/// `"KEM" ‖ I2OSP(0x0020, 2)`.
const KEM_SUITE_ID: &[u8] = b"KEM\x00\x20";
/// `"HPKE" ‖ kem_id ‖ kdf_id ‖ aead_id`.
const HPKE_SUITE_ID: &[u8] = b"HPKE\x00\x20\x00\x01\x00\x03";
const MODE_BASE: u8 = 0x00;
const MODE_PSK: u8 = 0x01;
/// Length of an ML-KEM-768 ciphertext.
pub(crate) const MLKEM768_CT_LEN: usize = 1088;
/// Length of an ML-KEM-768 encapsulation key.
pub(crate) const MLKEM768_PK_LEN: usize = 1184;
/// Length of an ML-KEM-768 decapsulation key.
pub(crate) const MLKEM768_SK_LEN: usize = 2400;
/// Length of box 2's `enc`: `ct_M ‖ enc_X`.
pub(crate) const BOX2_ENC_LEN: usize = MLKEM768_CT_LEN + 32;

fn labeled_extract(suite: &[u8], salt: &[u8], label: &[u8], ikm: &[u8]) -> Zeroizing<[u8; 32]> {
    let mut labeled = Zeroizing::new(Vec::with_capacity(
        7 + suite.len() + label.len() + ikm.len(),
    ));
    labeled.extend_from_slice(b"HPKE-v1");
    labeled.extend_from_slice(suite);
    labeled.extend_from_slice(label);
    labeled.extend_from_slice(ikm);
    let (prk, _) = Hkdf::<Sha256>::extract(Some(salt), &labeled);
    let mut out = Zeroizing::new([0u8; 32]);
    out.copy_from_slice(&prk);
    out
}

fn labeled_expand(suite: &[u8], prk: &[u8; 32], label: &[u8], info: &[u8], out: &mut [u8]) {
    let mut labeled = Vec::with_capacity(9 + suite.len() + label.len() + info.len());
    labeled.extend_from_slice(&(out.len() as u16).to_be_bytes());
    labeled.extend_from_slice(b"HPKE-v1");
    labeled.extend_from_slice(suite);
    labeled.extend_from_slice(label);
    labeled.extend_from_slice(info);
    Hkdf::<Sha256>::from_prk(prk)
        .expect("a 32-byte PRK")
        .expand(&labeled, out)
        .expect("every HPKE length here is at most 32 bytes");
}

/// X25519, refusing the all-zero output (a low-order public key).
fn x25519(sk: &[u8; 32], pk: &[u8; 32]) -> Result<Zeroizing<[u8; 32]>, CustodyError> {
    let dh = Zeroizing::new(MontgomeryPoint(*pk).mul_clamped(*sk).to_bytes());
    if bool::from(dh.ct_eq(&[0u8; 32])) {
        return Err(CustodyError::HpkeZeroDh);
    }
    Ok(dh)
}

/// The X25519 public key of `sk`.
pub(crate) fn x25519_public(sk: &[u8; 32]) -> [u8; 32] {
    MontgomeryPoint::mul_base_clamped(*sk).to_bytes()
}

/// `DeriveKeyPair(ikm)` for DHKEM(X25519, HKDF-SHA256): the secret is
/// `LabeledExpand(LabeledExtract("", "dkp_prk", ikm), "sk", "", 32)`.
pub(crate) fn derive_keypair(ikm: &[u8]) -> (Zeroizing<[u8; 32]>, [u8; 32]) {
    let dkp_prk = labeled_extract(KEM_SUITE_ID, b"", b"dkp_prk", ikm);
    let mut sk = Zeroizing::new([0u8; 32]);
    labeled_expand(KEM_SUITE_ID, &dkp_prk, b"sk", b"", sk.as_mut());
    let pk = x25519_public(&sk);
    (sk, pk)
}

fn extract_and_expand(dh: &[u8; 32], enc: &[u8; 32], pk_r: &[u8; 32]) -> Zeroizing<[u8; 32]> {
    let eae_prk = labeled_extract(KEM_SUITE_ID, b"", b"eae_prk", dh);
    let mut kem_context = [0u8; 64];
    kem_context[..32].copy_from_slice(enc);
    kem_context[32..].copy_from_slice(pk_r);
    let mut ss = Zeroizing::new([0u8; 32]);
    labeled_expand(
        KEM_SUITE_ID,
        &eae_prk,
        b"shared_secret",
        &kem_context,
        ss.as_mut(),
    );
    ss
}

/// `Encap(pkR)` with the ephemeral key derived from `ikm_e`: the shared
/// secret and `enc`.
fn encap_derand(
    pk_r: &[u8; 32],
    ikm_e: &[u8; 32],
) -> Result<(Zeroizing<[u8; 32]>, [u8; 32]), CustodyError> {
    let (sk_e, pk_e) = derive_keypair(ikm_e);
    let dh = x25519(&sk_e, pk_r)?;
    Ok((extract_and_expand(&dh, &pk_e, pk_r), pk_e))
}

/// `Decap(enc, skR)`.
fn decap(enc: &[u8; 32], sk_r: &[u8; 32]) -> Result<Zeroizing<[u8; 32]>, CustodyError> {
    let dh = x25519(sk_r, enc)?;
    Ok(extract_and_expand(&dh, enc, &x25519_public(sk_r)))
}

/// One HPKE context, used for sequence number 0 only.
struct Context {
    key: Zeroizing<[u8; 32]>,
    base_nonce: [u8; 12],
    exporter_secret: Zeroizing<[u8; 32]>,
}

impl Context {
    /// `Seal(aad, pt)` at sequence 0: `ct ‖ tag`.
    fn seal(&self, aad: &[u8], pt: &[u8]) -> Result<Vec<u8>, CustodyError> {
        let mut out = pt.to_vec();
        let tag = encrypt_payload(&self.key, &self.base_nonce, aad, &mut out)
            .map_err(|_| CustodyError::HpkeOpen)?;
        out.extend_from_slice(&tag);
        Ok(out)
    }

    /// `Open(aad, ct)` at sequence 0.
    fn open(&self, aad: &[u8], ct: &[u8]) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
        if ct.len() < 16 {
            return Err(CustodyError::HpkeOpen);
        }
        let (body, tag) = ct.split_at(ct.len() - 16);
        let mut out = Zeroizing::new(body.to_vec());
        decrypt_payload(&self.key, &self.base_nonce, aad, &mut out, tag)
            .map_err(|_| CustodyError::HpkeOpen)?;
        Ok(out)
    }

    /// `Export(exporter_context, 32)`.
    fn export(&self, exporter_context: &[u8]) -> Zeroizing<[u8; 32]> {
        let mut out = Zeroizing::new([0u8; 32]);
        labeled_expand(
            HPKE_SUITE_ID,
            &self.exporter_secret,
            b"sec",
            exporter_context,
            out.as_mut(),
        );
        out
    }
}

/// `KeySchedule(mode, shared_secret, info, psk, psk_id)`, with
/// `VerifyPSKInputs`: PSK mode needs both a PSK and its id, base mode
/// neither.
fn key_schedule(
    mode: u8,
    shared_secret: &[u8; 32],
    info: &[u8],
    psk: &[u8],
    psk_id: &[u8],
) -> Result<Context, CustodyError> {
    let got_psk = !psk.is_empty();
    let got_psk_id = !psk_id.is_empty();
    if got_psk != got_psk_id || (mode == MODE_BASE && got_psk) || (mode == MODE_PSK && !got_psk) {
        return Err(CustodyError::HpkePskInputs);
    }
    let psk_id_hash = labeled_extract(HPKE_SUITE_ID, b"", b"psk_id_hash", psk_id);
    let info_hash = labeled_extract(HPKE_SUITE_ID, b"", b"info_hash", info);
    let mut ctx = [0u8; 65];
    ctx[0] = mode;
    ctx[1..33].copy_from_slice(psk_id_hash.as_ref());
    ctx[33..].copy_from_slice(info_hash.as_ref());
    let secret = labeled_extract(HPKE_SUITE_ID, shared_secret, b"secret", psk);
    let mut key = Zeroizing::new([0u8; 32]);
    labeled_expand(HPKE_SUITE_ID, &secret, b"key", &ctx, key.as_mut());
    let mut base_nonce = [0u8; 12];
    labeled_expand(HPKE_SUITE_ID, &secret, b"base_nonce", &ctx, &mut base_nonce);
    let mut exporter_secret = Zeroizing::new([0u8; 32]);
    labeled_expand(
        HPKE_SUITE_ID,
        &secret,
        b"exp",
        &ctx,
        exporter_secret.as_mut(),
    );
    Ok(Context {
        key,
        base_nonce,
        exporter_secret,
    })
}

/// `SealBase` with the ephemeral key derived from `ikm_e` (the KAT seam).
pub(crate) fn seal_base_derand(
    pk_r: &[u8; 32],
    info: &[u8],
    aad: &[u8],
    pt: &[u8],
    ikm_e: &[u8; 32],
) -> Result<([u8; 32], Vec<u8>), CustodyError> {
    let (ss, enc) = encap_derand(pk_r, ikm_e)?;
    let ct = key_schedule(MODE_BASE, &ss, info, b"", b"")?.seal(aad, pt)?;
    Ok((enc, ct))
}

/// `SealBase(pkR, info, aad, pt)`, single shot: `(enc, ct)`.
pub(crate) fn seal_base(
    pk_r: &[u8; 32],
    info: &[u8],
    aad: &[u8],
    pt: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<([u8; 32], Vec<u8>), CustodyError> {
    let ikm_e = Zeroizing::new(random_array::<32>(rng));
    seal_base_derand(pk_r, info, aad, pt, &ikm_e)
}

/// `OpenBase(enc, skR, info, aad, ct)`, single shot.
pub(crate) fn open_base(
    sk_r: &[u8; 32],
    enc: &[u8; 32],
    info: &[u8],
    aad: &[u8],
    ct: &[u8],
) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
    let ss = decap(enc, sk_r)?;
    key_schedule(MODE_BASE, &ss, info, b"", b"")?.open(aad, ct)
}

/// `SealPSK` with the ephemeral key derived from `ikm_e` (the KAT seam).
pub(crate) fn seal_psk_derand(
    pk_r: &[u8; 32],
    info: &[u8],
    aad: &[u8],
    pt: &[u8],
    psk: &[u8],
    psk_id: &[u8],
    ikm_e: &[u8; 32],
) -> Result<([u8; 32], Vec<u8>), CustodyError> {
    let (ss, enc) = encap_derand(pk_r, ikm_e)?;
    let ct = key_schedule(MODE_PSK, &ss, info, psk, psk_id)?.seal(aad, pt)?;
    Ok((enc, ct))
}

/// `SealPSK(pkR, info, aad, pt, psk, psk_id)`, single shot.
pub(crate) fn seal_psk(
    pk_r: &[u8; 32],
    info: &[u8],
    aad: &[u8],
    pt: &[u8],
    psk: &[u8],
    psk_id: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<([u8; 32], Vec<u8>), CustodyError> {
    let ikm_e = Zeroizing::new(random_array::<32>(rng));
    seal_psk_derand(pk_r, info, aad, pt, psk, psk_id, &ikm_e)
}

/// `OpenPSK(enc, skR, info, aad, ct, psk, psk_id)`, single shot.
pub(crate) fn open_psk(
    sk_r: &[u8; 32],
    enc: &[u8; 32],
    info: &[u8],
    aad: &[u8],
    ct: &[u8],
    psk: &[u8],
    psk_id: &[u8],
) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
    let ss = decap(enc, sk_r)?;
    key_schedule(MODE_PSK, &ss, info, psk, psk_id)?.open(aad, ct)
}

/// `SetupBaseS(pkR, info)` then `Export(exporter_context, 32)`: the seam a
/// later change seals vault envelopes through (the envelope DEK).
pub(crate) fn setup_base_s_export(
    pk_r: &[u8; 32],
    info: &[u8],
    exporter_context: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<([u8; 32], Zeroizing<[u8; 32]>), CustodyError> {
    let ikm_e = Zeroizing::new(random_array::<32>(rng));
    let (ss, enc) = encap_derand(pk_r, &ikm_e)?;
    Ok((
        enc,
        key_schedule(MODE_BASE, &ss, info, b"", b"")?.export(exporter_context),
    ))
}

/// `SetupPSKS(pkR, info, psk, psk_id)` then `Export(exporter_context, 32)`.
pub(crate) fn setup_psk_s_export(
    pk_r: &[u8; 32],
    info: &[u8],
    exporter_context: &[u8],
    psk: &[u8],
    psk_id: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<([u8; 32], Zeroizing<[u8; 32]>), CustodyError> {
    let ikm_e = Zeroizing::new(random_array::<32>(rng));
    let (ss, enc) = encap_derand(pk_r, &ikm_e)?;
    Ok((
        enc,
        key_schedule(MODE_PSK, &ss, info, psk, psk_id)?.export(exporter_context),
    ))
}

/// Box 2's `psk_id`: `"securacv:custody:mlkem768-psk-id:v1" ‖ SHA-256(ct_M)`.
pub(crate) fn box2_psk_id(ct_m: &[u8]) -> Vec<u8> {
    let mut id = MLKEM_PSK_ID_PREFIX.to_vec();
    id.extend_from_slice(&Sha256::digest(ct_m));
    id
}

/// Box 2's HPKE layer with a given ML-KEM shared secret and ciphertext:
/// `SealPSK(pk_x, info, aad, pt, psk = ss_m, psk_id = box2_psk_id(ct_m))`
/// with `enc = ct_m ‖ enc_X`. The KAT seam: no ML-KEM runs here.
pub(crate) fn seal_box2_psk_layer_derand(
    pk_x: &[u8; 32],
    ss_m: &[u8; 32],
    ct_m: &[u8],
    info: &[u8],
    aad: &[u8],
    pt: &[u8],
    ikm_e: &[u8; 32],
) -> Result<(Vec<u8>, Vec<u8>), CustodyError> {
    if ct_m.len() != MLKEM768_CT_LEN {
        return Err(CustodyError::MlKem);
    }
    let (enc_x, ct) = seal_psk_derand(pk_x, info, aad, pt, ss_m, &box2_psk_id(ct_m), ikm_e)?;
    let mut enc = ct_m.to_vec();
    enc.extend_from_slice(&enc_x);
    Ok((enc, ct))
}

/// Open box 2's HPKE layer with a given ML-KEM shared secret: `enc` is
/// `ct_M ‖ enc_X` (1120 bytes).
pub(crate) fn open_box2_psk_layer(
    sk_x: &[u8; 32],
    ss_m: &[u8; 32],
    enc: &[u8],
    info: &[u8],
    aad: &[u8],
    ct: &[u8],
) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
    if enc.len() != BOX2_ENC_LEN {
        return Err(CustodyError::HpkeOpen);
    }
    let (ct_m, enc_x) = enc.split_at(MLKEM768_CT_LEN);
    let enc_x: [u8; 32] = enc_x.try_into().expect("32 bytes");
    open_psk(sk_x, &enc_x, info, aad, ct, ss_m, &box2_psk_id(ct_m))
}

/// Box 2: ML-KEM-768 encapsulates to `pk_m`, and its shared secret keys
/// HPKE PSK mode to `pk_x`. Returns `(enc = ct_M ‖ enc_X, ct)`.
#[cfg(feature = "pqc-vault")]
pub(crate) fn seal_box2(
    pk_x: &[u8; 32],
    pk_m: &[u8],
    info: &[u8],
    aad: &[u8],
    pt: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<(Vec<u8>, Vec<u8>), CustodyError> {
    use ml_kem::{Encapsulate, EncapsulationKey768, TryKeyInit};
    use zeroize::Zeroize;
    // `ML-KEM.Encaps` (FIPS 203 section 7.2) through the same `ml-kem`
    // crate the vault uses; the encapsulation key check of that section
    // (length and modulus) is the import.
    let pk = EncapsulationKey768::new_from_slice(pk_m).map_err(|_| CustodyError::MlKem)?;
    let (ct_m, mut shared) =
        pk.encapsulate_with_rng(&mut rand::rand_core::UnwrapErr(rand::rngs::SysRng));
    let mut ss_m = Zeroizing::new([0u8; 32]);
    ss_m.copy_from_slice(&shared[..]);
    shared.zeroize();
    let ikm_e = Zeroizing::new(random_array::<32>(rng));
    seal_box2_psk_layer_derand(pk_x, &ss_m, &ct_m[..], info, aad, pt, &ikm_e)
}

/// Open box 2: decapsulate `ct_M` with `sk_m`, then open the HPKE PSK
/// layer with `sk_x`.
#[cfg(feature = "pqc-vault")]
pub(crate) fn open_box2(
    sk_x: &[u8; 32],
    sk_m: &[u8],
    enc: &[u8],
    info: &[u8],
    aad: &[u8],
    ct: &[u8],
) -> Result<Zeroizing<Vec<u8>>, CustodyError> {
    use ml_kem::{Decapsulate, DecapsulationKey768, ExpandedDecapsulationKey, MlKem768};
    use zeroize::Zeroize;
    if enc.len() != BOX2_ENC_LEN {
        return Err(CustodyError::HpkeOpen);
    }
    // The stored 2400-byte decapsulation key, imported the way the vault's
    // `KemKeypair::from_bytes` imports it (FIPS 203 section 7.3's hash
    // check is the import); `ml-kem` marks the expanded import deprecated
    // in favor of seeds, and a stored key has no seed to return to.
    let mut encoded =
        ExpandedDecapsulationKey::<MlKem768>::try_from(sk_m).map_err(|_| CustodyError::MlKem)?;
    #[allow(deprecated)]
    let decoded = DecapsulationKey768::from_expanded(&encoded);
    encoded.zeroize();
    let sk = decoded.map_err(|_| CustodyError::MlKem)?;
    let mut shared = sk
        .decapsulate_slice(&enc[..MLKEM768_CT_LEN])
        .map_err(|_| CustodyError::MlKem)?;
    let mut ss_m = Zeroizing::new([0u8; 32]);
    ss_m.copy_from_slice(&shared[..]);
    shared.zeroize();
    open_box2_psk_layer(sk_x, &ss_m, enc, info, aad, ct)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::vault::custody::group::OsCustodyRng;

    fn h(s: &str) -> Vec<u8> {
        hex::decode(s).unwrap()
    }
    fn h32(s: &str) -> [u8; 32] {
        h(s).try_into().unwrap()
    }

    const INFO: &str = "4f6465206f6e2061204772656369616e2055726e";
    const PT: &str = "4265617574792069732074727574682c20747275746820626561757479";
    const AAD0: &str = "436f756e742d30";

    /// draft-irtf-cfrg-hpke (the editor's source RFC 9180 was published
    /// from), Test Vectors, DHKEM(X25519, HKDF-SHA256), HKDF-SHA256,
    /// ChaCha20Poly1305, Base Setup Information, sequence 0, and the three
    /// exported values.
    #[test]
    fn draft_base_vector_x25519_chacha() {
        let (sk_e, pk_e) = derive_keypair(&h(
            "909a9b35d3dc4713a5e72a4da274b55d3d3821a37e5d099e74a647db583a904b",
        ));
        assert_eq!(
            hex::encode(sk_e.as_ref()),
            "f4ec9b33b792c372c1d2c2063507b684ef925b8c75a42dbcbf57d63ccd381600"
        );
        assert_eq!(
            hex::encode(pk_e),
            "1afa08d3dec047a643885163f1180476fa7ddb54c6a8029ea33f95796bf2ac4a"
        );
        let (sk_r, pk_r) = derive_keypair(&h(
            "1ac01f181fdf9f352797655161c58b75c656a6cc2716dcb66372da835542e1df",
        ));
        assert_eq!(
            hex::encode(sk_r.as_ref()),
            "8057991eef8f1f1af18f4a9491d16a1ce333f695d4db8e38da75975c4478e0fb"
        );
        assert_eq!(
            hex::encode(pk_r),
            "4310ee97d88cc1f088a5576c77ab0cf5c3ac797f3d95139c6c84b5429c59662a"
        );
        let ikm_e = h32("909a9b35d3dc4713a5e72a4da274b55d3d3821a37e5d099e74a647db583a904b");
        let (ss, enc) = encap_derand(&pk_r, &ikm_e).unwrap();
        assert_eq!(enc, pk_e);
        assert_eq!(
            hex::encode(ss.as_ref()),
            "0bbe78490412b4bbea4812666f7916932b828bba79942424abb65244930d69a7"
        );
        assert_eq!(*decap(&enc, &sk_r).unwrap(), *ss);
        let ctx = key_schedule(
            MODE_BASE,
            &ss,
            &h(INFO),
            &crate::vault::custody::tv_bytes(""),
            b"",
        )
        .unwrap();
        assert_eq!(
            hex::encode(ctx.key.as_ref()),
            "ad2744de8e17f4ebba575b3f5f5a8fa1f69c2a07f6e7500bc60ca6e3e3ec1c91"
        );
        assert_eq!(hex::encode(ctx.base_nonce), "5c4d98150661b848853b547f");
        assert_eq!(
            hex::encode(ctx.exporter_secret.as_ref()),
            "a3b010d4994890e2c6968a36f64470d3c824c8f5029942feb11e7a74b2921922"
        );
        let want_ct = "1c5250d8034ec2b784ba2cfd69dbdb8af406cfe3ff938e131f0def8c8b60b4db21993c62ce81883d2dd1b51a28";
        assert_eq!(hex::encode(ctx.seal(&h(AAD0), &h(PT)).unwrap()), want_ct);
        let (enc2, ct2) = seal_base_derand(&pk_r, &h(INFO), &h(AAD0), &h(PT), &ikm_e).unwrap();
        assert_eq!((enc2, hex::encode(&ct2)), (pk_e, want_ct.to_string()));
        assert_eq!(
            *open_base(&sk_r, &enc, &h(INFO), &h(AAD0), &h(want_ct)).unwrap(),
            h(PT)
        );
        assert_eq!(
            hex::encode(ctx.export(b"").as_ref()),
            "4bbd6243b8bb54cec311fac9df81841b6fd61f56538a775e7c80a9f40160606e"
        );
        assert_eq!(
            hex::encode(ctx.export(&[0]).as_ref()),
            "8c1df14732580e5501b00f82b10a1647b40713191b7c1240ac80e2b68808ba69"
        );
        assert_eq!(
            hex::encode(ctx.export(b"TestContext").as_ref()),
            "5acb09211139c43b3090489a9da433e8a30ee7188ba8b0a9a1ccf0c229283e53"
        );
    }

    /// The same suite's PSK Setup Information, sequence 0, and exports.
    #[test]
    fn draft_psk_vector_x25519_chacha() {
        let ikm_e = h32("35706a0b09fb26fb45c39c2f5079c709c7cf98e43afa973f14d88ece7e29c2e3");
        let (sk_e, pk_e) = derive_keypair(&ikm_e);
        assert_eq!(
            hex::encode(sk_e.as_ref()),
            "0c35fdf49df7aa01cd330049332c40411ebba36e0c718ebc3edf5845795f6321"
        );
        assert_eq!(
            hex::encode(pk_e),
            "2261299c3f40a9afc133b969a97f05e95be2c514e54f3de26cbe5644ac735b04"
        );
        let (sk_r, pk_r) = derive_keypair(&h(
            "26b923eade72941c8a85b09986cdfa3f1296852261adedc52d58d2930269812b",
        ));
        assert_eq!(
            hex::encode(sk_r.as_ref()),
            "77d114e0212be51cb1d76fa99dd41cfd4d0166b08caa09074430a6c59ef17879"
        );
        assert_eq!(
            hex::encode(pk_r),
            "13640af826b722fc04feaa4de2f28fbd5ecc03623b317834e7ff4120dbe73062"
        );
        let psk = h("0247fd33b913760fa1fa51e1892d9f307fbe65eb171e8132c2af18555a738b82");
        let psk_id = h("456e6e796e20447572696e206172616e204d6f726961");
        let (ss, enc) = encap_derand(&pk_r, &ikm_e).unwrap();
        assert_eq!(enc, pk_e);
        assert_eq!(
            hex::encode(ss.as_ref()),
            "4be079c5e77779d0215b3f689595d59e3e9b0455d55662d1f3666ec606e50ea7"
        );
        let ctx = key_schedule(MODE_PSK, &ss, &h(INFO), &psk, &psk_id).unwrap();
        assert_eq!(
            hex::encode(ctx.key.as_ref()),
            "600d2fdb0313a7e5c86a9ce9221cd95bed069862421744cfb4ab9d7203a9c019"
        );
        assert_eq!(hex::encode(ctx.base_nonce), "112e0465562045b7368653e7");
        assert_eq!(
            hex::encode(ctx.exporter_secret.as_ref()),
            "73b506dc8b6b4269027f80b0362def5cbb57ee50eed0c2873dac9181f453c5ac"
        );
        let want_ct = "4a177f9c0d6f15cfdf533fb65bf84aecdc6ab16b8b85b4cf65a370e07fc1d78d28fb073214525276f4a89608ff";
        let (enc2, ct2) =
            seal_psk_derand(&pk_r, &h(INFO), &h(AAD0), &h(PT), &psk, &psk_id, &ikm_e).unwrap();
        assert_eq!((enc2, hex::encode(&ct2)), (pk_e, want_ct.to_string()));
        assert_eq!(
            *open_psk(&sk_r, &enc, &h(INFO), &h(AAD0), &h(want_ct), &psk, &psk_id).unwrap(),
            h(PT)
        );
        assert_eq!(
            hex::encode(ctx.export(b"").as_ref()),
            "813c1bfc516c99076ae0f466671f0ba5ff244a41699f7b2417e4c59d46d39f40"
        );
        assert_eq!(
            hex::encode(ctx.export(&[0]).as_ref()),
            "2745cf3d5bb65c333658732954ee7af49eb895ce77f8022873a62a13c94cb4e1"
        );
        assert_eq!(
            hex::encode(ctx.export(b"TestContext").as_ref()),
            "ad40e3ae14f21c99bfdebc20ae14ab86f4ca2dc9a4799d200f43a25f99fa78ae"
        );
    }

    #[test]
    fn open_refuses_all_zero_dh() {
        let (sk_r, pk_r) = derive_keypair(&crate::vault::custody::tv_bytes(
            "open_refuses_all_zero_dh recipient",
        ));
        // Low-order u-coordinates: 0, 1, and a point of order 8. X25519
        // clamps its scalar to a multiple of 8, so each gives all zeros.
        let order8 = h32("e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800");
        for low in [
            [0u8; 32],
            {
                let mut one = [0u8; 32];
                one[0] = 1;
                one
            },
            order8,
        ] {
            let err = open_base(&sk_r, &low, b"info", b"aad", &[0u8; 32]).unwrap_err();
            assert_eq!(
                err.to_string(),
                "HPKE: X25519 produced an all-zero shared secret (invalid public key)"
            );
            assert_eq!(
                seal_base(&low, b"info", b"aad", b"pt", &mut OsCustodyRng).unwrap_err(),
                CustodyError::HpkeZeroDh
            );
        }
        // A real key still works.
        let (enc, ct) = seal_base(&pk_r, b"info", b"aad", b"pt", &mut OsCustodyRng).unwrap();
        assert_eq!(
            *open_base(&sk_r, &enc, b"info", b"aad", &ct).unwrap(),
            b"pt".to_vec()
        );
    }

    #[test]
    fn open_refuses_wrong_aad_and_wrong_info() {
        let (sk_r, pk_r) = derive_keypair(&crate::vault::custody::tv_bytes(
            "open_refuses_wrong_aad recipient",
        ));
        let (other_sk, _) = derive_keypair(&crate::vault::custody::tv_bytes("another recipient"));
        let (enc, ct) = seal_base(&pk_r, b"info", b"aad", b"secret", &mut OsCustodyRng).unwrap();
        assert!(open_base(&sk_r, &enc, b"info", b"aad", &ct).is_ok());
        for (sk, info, aad) in [
            (&sk_r, &b"info"[..], &b"aaD"[..]),
            (&sk_r, &b"infO"[..], &b"aad"[..]),
            (&other_sk, &b"info"[..], &b"aad"[..]),
        ] {
            assert_eq!(
                open_base(sk, &enc, info, aad, &ct).unwrap_err().to_string(),
                "HPKE: open failed"
            );
        }
        let mut bent = ct.clone();
        bent[0] ^= 1;
        assert_eq!(
            open_base(&sk_r, &enc, b"info", b"aad", &bent).unwrap_err(),
            CustodyError::HpkeOpen
        );
        assert_eq!(
            open_base(&sk_r, &enc, b"info", b"aad", &ct[..10]).unwrap_err(),
            CustodyError::HpkeOpen
        );
    }

    #[test]
    fn psk_requires_psk_and_id_together() {
        let (sk_r, pk_r) = derive_keypair(&crate::vault::custody::tv_bytes("psk recipient"));
        let psk = [7u8; 32];
        let refusal = "HPKE: a PSK and its id must be given together, and only in PSK mode";
        let mut rng = OsCustodyRng;
        assert_eq!(
            seal_psk(
                &pk_r,
                b"i",
                b"a",
                b"p",
                &psk,
                &crate::vault::custody::tv_bytes(""),
                &mut rng
            )
            .unwrap_err()
            .to_string(),
            refusal
        );
        assert_eq!(
            seal_psk(
                &pk_r,
                b"i",
                b"a",
                b"p",
                &crate::vault::custody::tv_bytes(""),
                &crate::vault::custody::tv_bytes("id"),
                &mut rng
            )
            .unwrap_err()
            .to_string(),
            refusal
        );
        assert_eq!(
            seal_psk(
                &pk_r,
                b"i",
                b"a",
                b"p",
                &crate::vault::custody::tv_bytes(""),
                b"",
                &mut rng
            )
            .unwrap_err()
            .to_string(),
            refusal
        );
        let ss = crate::vault::custody::tv::<32>("01");
        assert_eq!(
            key_schedule(
                MODE_BASE,
                &ss,
                b"i",
                &psk,
                &crate::vault::custody::tv_bytes("id")
            )
            .err()
            .unwrap(),
            CustodyError::HpkePskInputs
        );
        let (enc, ct) = seal_psk(
            &pk_r,
            b"i",
            b"a",
            b"p",
            &psk,
            &crate::vault::custody::tv_bytes("id"),
            &mut rng,
        )
        .unwrap();
        assert!(open_psk(
            &sk_r,
            &enc,
            b"i",
            b"a",
            &ct,
            &psk,
            &crate::vault::custody::tv_bytes("id")
        )
        .is_ok());
        // The wrong PSK, the wrong id, and base mode all fail to open it.
        assert_eq!(
            open_psk(
                &sk_r,
                &enc,
                b"i",
                b"a",
                &ct,
                &crate::vault::custody::tv::<32>("08"),
                &crate::vault::custody::tv_bytes("id")
            )
            .unwrap_err(),
            CustodyError::HpkeOpen
        );
        assert_eq!(
            open_psk(
                &sk_r,
                &enc,
                b"i",
                b"a",
                &ct,
                &psk,
                &crate::vault::custody::tv_bytes("iD")
            )
            .unwrap_err(),
            CustodyError::HpkeOpen
        );
        assert_eq!(
            open_base(&sk_r, &enc, b"i", b"a", &ct).unwrap_err(),
            CustodyError::HpkeOpen
        );
        // The export seams agree between sender and recipient.
        let (enc, exported) = setup_psk_s_export(
            &pk_r,
            b"i",
            b"ctx",
            &psk,
            &crate::vault::custody::tv_bytes("id"),
            &mut rng,
        )
        .unwrap();
        let ss = decap(&enc, &sk_r).unwrap();
        let ctx = key_schedule(
            MODE_PSK,
            &ss,
            b"i",
            &psk,
            &crate::vault::custody::tv_bytes("id"),
        )
        .unwrap();
        assert_eq!(*ctx.export(b"ctx"), *exported);
        let (enc, exported) = setup_base_s_export(&pk_r, b"i", b"ctx", &mut rng).unwrap();
        let ss = decap(&enc, &sk_r).unwrap();
        assert_eq!(
            *key_schedule(
                MODE_BASE,
                &ss,
                b"i",
                &crate::vault::custody::tv_bytes(""),
                b""
            )
            .unwrap()
            .export(b"ctx"),
            *exported
        );
    }

    #[cfg(feature = "pqc-vault")]
    #[test]
    fn box2_round_trip() {
        let kp = crate::vault::crypto::KemKeypair::generate();
        let (pk_m, sk_m) = (kp.public_bytes(), kp.secret_bytes());
        let (sk_x, pk_x) = derive_keypair(&crate::vault::custody::tv_bytes("box2 recipient"));
        let mut rng = OsCustodyRng;
        let (enc, ct) = seal_box2(&pk_x, &pk_m, b"info", b"aad", b"share bytes", &mut rng).unwrap();
        assert_eq!(enc.len(), BOX2_ENC_LEN);
        let pt = open_box2(&sk_x, &sk_m, &enc, b"info", b"aad", &ct).unwrap();
        assert_eq!(*pt, b"share bytes".to_vec());
        // Both halves are needed: the wrong X25519 key, the wrong ML-KEM
        // key, and a changed ML-KEM ciphertext all fail.
        let (other_x, _) = derive_keypair(&crate::vault::custody::tv_bytes("other"));
        assert!(open_box2(&other_x, &sk_m, &enc, b"info", b"aad", &ct).is_err());
        let other_m = crate::vault::crypto::KemKeypair::generate().secret_bytes();
        assert!(open_box2(&sk_x, &other_m, &enc, b"info", b"aad", &ct).is_err());
        let mut bent = enc.clone();
        bent[5] ^= 1;
        assert!(open_box2(&sk_x, &sk_m, &bent, b"info", b"aad", &ct).is_err());
        assert!(open_box2(&sk_x, &sk_m, &enc, b"info", b"aaD", &ct).is_err());
    }
}
