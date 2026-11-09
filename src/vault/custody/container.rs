//! `MKC1`, the custody container (spec §2.5): the unchanged vault master
//! key, and the ML-KEM-768 key pair if the vault has one, wrapped under the
//! KEK. The format is defined here; nothing in this change writes the file
//! into a vault.
//!
//! ```text
//! 0      4   magic "MKC1"
//! 4      1   version 0x01
//! 5      1   suite 0x01
//! 6      1   crypto_mode: 0x01 classical · 0x02 pq · 0x03 hybrid
//! 7      16  kek_salt
//! 23     32  C_0, the KEK commitment
//! 55     32  vault_x25519_public (all zero iff pq)
//! 87     2   mlkem_pk_len u16le (0 iff classical, else 1184)
//! 89     P   mlkem768_public
//! 89+P   12  nonce
//! 101+P  4   ct_len u32le (= payload length + 16, at most 4096)
//! 105+P  …   ChaCha20-Poly1305(wrap_key, nonce, "securacv-vault-custody-v1" ‖ bytes[0..105+P], payload)
//! ```
//!
//! `wrap_key = HKDF-SHA256(salt = kek_salt, ikm = k, info =
//! "securacv:vault:kek-wrap:v1")`; `payload = 0x01 ‖ master_key[32] ‖
//! (0x00 | 0x01 ‖ u32le pk_len ‖ pk ‖ u32le sk_len ‖ sk)`, the KEM part
//! byte for byte the existing `kem-mlkem768.key` encoding. There is no path
//! binding: custody exists to survive the loss of the hub, so a restore to a
//! new path must open.

use std::fmt;

use hkdf::Hkdf;
use sha2::Sha256;
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use super::hpke::{derive_keypair, MLKEM768_PK_LEN, MLKEM768_SK_LEN};
use super::vss::Kek;
use super::{
    CustodyError, MKC1_AAD_DOMAIN, MKC1_MAGIC, MKC1_MAX_BYTES, SUITE_V1, VAULT_X25519_SALT,
};
use crate::vault::crypto::{decrypt_payload, encrypt_payload};

/// A classical vault: the master key alone.
pub(crate) const CRYPTO_MODE_CLASSICAL: u8 = 0x01;
/// A post-quantum vault: the ML-KEM-768 key pair, no vault X25519 key.
pub(crate) const CRYPTO_MODE_PQ: u8 = 0x02;
/// A hybrid vault: both.
pub(crate) const CRYPTO_MODE_HYBRID: u8 = 0x03;
/// The largest ciphertext a container carries.
const MKC1_MAX_CT: u32 = 4096;
/// The fixed part of the header before the ML-KEM public key.
const MKC1_FIXED: usize = 89;

const NOT_THIS_CONTAINER: &str = "the key rebuilt from the shares is not this container's (KEK commitment differs) — nothing was decrypted";
const AUTH_FAILED: &str = "authentication failed — the file was altered or damaged";
const KEYS_MISMATCH: &str = "its public keys do not match its contents";

/// The public header of an `MKC1` container.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Mkc1Header {
    /// `0x01` classical, `0x02` pq, `0x03` hybrid.
    pub(crate) crypto_mode: u8,
    /// The HKDF salt of the wrap key.
    pub(crate) kek_salt: [u8; 16],
    /// `C_0`, which every share of this KEK carries.
    pub(crate) kek_commitment: [u8; 32],
    /// The vault X25519 public key (zero in pq mode).
    pub(crate) vault_x25519_public: [u8; 32],
    /// The ML-KEM-768 encapsulation key (empty in classical mode).
    pub(crate) mlkem768_public: Vec<u8>,
}

/// What a container holds.
pub(crate) struct Mkc1Plain {
    /// The vault master key.
    pub(crate) master_key: Zeroizing<[u8; 32]>,
    /// The ML-KEM-768 key pair `(public, secret)`, if any.
    pub(crate) kem: Option<(Vec<u8>, Zeroizing<Vec<u8>>)>,
}

impl fmt::Debug for Mkc1Plain {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Mkc1Plain(<redacted>)")
    }
}

/// The vault X25519 key pair a later change derives from the master key:
/// `ikm = HKDF-SHA256(salt = "securacv:vault:x25519-ikm:v1", ikm =
/// master_key, info = "", L = 32)`, then HPKE `DeriveKeyPair(ikm)`.
pub(crate) fn vault_x25519_from_master(master: &[u8; 32]) -> (Zeroizing<[u8; 32]>, [u8; 32]) {
    let hk = Hkdf::<Sha256>::new(Some(VAULT_X25519_SALT), master);
    let mut ikm = Zeroizing::new([0u8; 32]);
    hk.expand(b"", ikm.as_mut())
        .expect("32 bytes is a valid HKDF-SHA256 length");
    derive_keypair(ikm.as_ref())
}

/// The plaintext a container seals.
fn encode_payload(master: &[u8; 32], kem: Option<(&[u8], &[u8])>) -> Zeroizing<Vec<u8>> {
    let mut out = Zeroizing::new(Vec::with_capacity(
        34 + 8 + MLKEM768_PK_LEN + MLKEM768_SK_LEN,
    ));
    out.push(0x01);
    out.extend_from_slice(master);
    match kem {
        None => out.push(0x00),
        Some((pk, sk)) => {
            out.push(0x01);
            out.extend_from_slice(&(pk.len() as u32).to_le_bytes());
            out.extend_from_slice(pk);
            out.extend_from_slice(&(sk.len() as u32).to_le_bytes());
            out.extend_from_slice(sk);
        }
    }
    out
}

/// Seal `payload` under `wrap_key` with the given header fields. No check
/// that the header describes the payload: callers do that (and the tests
/// use it to build a container whose header lies).
fn encode_raw(
    wrap_key: &[u8; 32],
    header: &Mkc1Header,
    nonce: [u8; 12],
    payload: &[u8],
) -> Result<Vec<u8>, CustodyError> {
    let mut out = Vec::with_capacity(105 + header.mlkem768_public.len() + payload.len() + 16);
    out.extend_from_slice(MKC1_MAGIC);
    out.push(1);
    out.push(SUITE_V1);
    out.push(header.crypto_mode);
    out.extend_from_slice(&header.kek_salt);
    out.extend_from_slice(&header.kek_commitment);
    out.extend_from_slice(&header.vault_x25519_public);
    out.extend_from_slice(&(header.mlkem768_public.len() as u16).to_le_bytes());
    out.extend_from_slice(&header.mlkem768_public);
    out.extend_from_slice(&nonce);
    out.extend_from_slice(&((payload.len() + 16) as u32).to_le_bytes());
    let mut aad = MKC1_AAD_DOMAIN.to_vec();
    aad.extend_from_slice(&out);
    let mut ct = Zeroizing::new(payload.to_vec());
    let tag = encrypt_payload(wrap_key, &nonce, &aad, &mut ct)
        .map_err(|_| CustodyError::Container(AUTH_FAILED))?;
    out.extend_from_slice(&ct);
    out.extend_from_slice(&tag);
    if out.len() > MKC1_MAX_BYTES {
        return Err(CustodyError::Container("longer than 8192 bytes"));
    }
    Ok(out)
}

/// Encode an `MKC1` container for `master` (and the vault's ML-KEM key
/// pair, for pq and hybrid) under `kek`, with the salt and nonce given.
/// Classical mode carries no KEM key; pq and hybrid need one.
pub(crate) fn encode_mkc1_with(
    kek: &Kek,
    kek_salt: [u8; 16],
    nonce: [u8; 12],
    crypto_mode: u8,
    master: &[u8; 32],
    kem: Option<(&[u8], &[u8])>,
) -> Result<Vec<u8>, CustodyError> {
    let kem_ok = match (crypto_mode, kem) {
        (CRYPTO_MODE_CLASSICAL, None) => true,
        (CRYPTO_MODE_PQ | CRYPTO_MODE_HYBRID, Some((pk, sk))) => {
            pk.len() == MLKEM768_PK_LEN && sk.len() == MLKEM768_SK_LEN
        }
        _ => false,
    };
    if !kem_ok {
        return Err(CustodyError::Container(KEYS_MISMATCH));
    }
    let vault_x25519_public = if crypto_mode == CRYPTO_MODE_PQ {
        [0u8; 32]
    } else {
        vault_x25519_from_master(master).1
    };
    let header = Mkc1Header {
        crypto_mode,
        kek_salt,
        kek_commitment: kek.commitment(),
        vault_x25519_public,
        mlkem768_public: kem.map(|(pk, _)| pk.to_vec()).unwrap_or_default(),
    };
    let wrap_key = kek.wrap_key(&kek_salt);
    encode_raw(&wrap_key, &header, nonce, &encode_payload(master, kem))
}

/// A bounded little cursor over container bytes.
struct Cursor<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Cursor<'a> {
    fn take(&mut self, n: usize) -> Result<&'a [u8], CustodyError> {
        let end = self.at.checked_add(n).filter(|&e| e <= self.bytes.len());
        let end = end.ok_or(CustodyError::Container("truncated"))?;
        let out = &self.bytes[self.at..end];
        self.at = end;
        Ok(out)
    }
    fn array<const N: usize>(&mut self) -> Result<[u8; N], CustodyError> {
        Ok(self.take(N)?.try_into().expect("N bytes"))
    }
}

/// The parsed parts of a container: header, the AAD-covered prefix
/// length, nonce and ciphertext.
struct Parsed<'a> {
    header: Mkc1Header,
    header_len: usize,
    nonce: [u8; 12],
    ct: &'a [u8],
}

fn parse(bytes: &[u8]) -> Result<Parsed<'_>, CustodyError> {
    if bytes.len() > MKC1_MAX_BYTES {
        return Err(CustodyError::Container("longer than 8192 bytes"));
    }
    let mut c = Cursor { bytes, at: 0 };
    if c.take(4)? != MKC1_MAGIC {
        return Err(CustodyError::Container("bad magic"));
    }
    if c.array::<1>()?[0] != 1 {
        return Err(CustodyError::Container("unsupported version"));
    }
    if c.array::<1>()?[0] != SUITE_V1 {
        return Err(CustodyError::Container("unsupported suite"));
    }
    let crypto_mode = c.array::<1>()?[0];
    if !matches!(
        crypto_mode,
        CRYPTO_MODE_CLASSICAL | CRYPTO_MODE_PQ | CRYPTO_MODE_HYBRID
    ) {
        return Err(CustodyError::Container("unknown crypto mode"));
    }
    let kek_salt = c.array::<16>()?;
    let kek_commitment = c.array::<32>()?;
    let vault_x25519_public = c.array::<32>()?;
    if (vault_x25519_public == [0u8; 32]) != (crypto_mode == CRYPTO_MODE_PQ) {
        return Err(CustodyError::Container(
            "the X25519 public key must be zero exactly when the crypto mode is pq",
        ));
    }
    let pk_len = usize::from(u16::from_le_bytes(c.array::<2>()?));
    let want_pk = if crypto_mode == CRYPTO_MODE_CLASSICAL {
        0
    } else {
        MLKEM768_PK_LEN
    };
    if pk_len != want_pk {
        return Err(CustodyError::Container(
            "the ML-KEM public key length does not match the crypto mode",
        ));
    }
    let mlkem768_public = c.take(pk_len)?.to_vec();
    let nonce = c.array::<12>()?;
    let ct_len = u32::from_le_bytes(c.array::<4>()?);
    if !(17..=MKC1_MAX_CT).contains(&ct_len) {
        return Err(CustodyError::ContainerCiphertextLength(ct_len));
    }
    let header_len = c.at;
    debug_assert_eq!(header_len, MKC1_FIXED + pk_len + 16);
    let ct = c.take(ct_len as usize)?;
    if c.at != bytes.len() {
        return Err(CustodyError::Container("trailing bytes"));
    }
    Ok(Parsed {
        header: Mkc1Header {
            crypto_mode,
            kek_salt,
            kek_commitment,
            vault_x25519_public,
            mlkem768_public,
        },
        header_len,
        nonce,
        ct,
    })
}

/// Decode and bound-check a container's public header. Needs no key.
pub(crate) fn decode_mkc1_header(bytes: &[u8]) -> Result<Mkc1Header, CustodyError> {
    parse(bytes).map(|p| p.header)
}

fn parse_payload(pt: &[u8]) -> Option<Mkc1Plain> {
    let (&version, rest) = pt.split_first()?;
    if version != 0x01 || rest.len() < 33 {
        return None;
    }
    let mut master_key = Zeroizing::new([0u8; 32]);
    master_key.copy_from_slice(&rest[..32]);
    let rest = &rest[32..];
    match rest {
        [0x00] => Some(Mkc1Plain {
            master_key,
            kem: None,
        }),
        [0x01, kem @ ..] => {
            let pk_len = u32::from_le_bytes(kem.get(..4)?.try_into().ok()?) as usize;
            let pk = kem.get(4..4usize.checked_add(pk_len)?)?;
            let at = 4 + pk_len;
            let sk_len = u32::from_le_bytes(kem.get(at..at + 4)?.try_into().ok()?) as usize;
            let sk = kem.get(at + 4..(at + 4).checked_add(sk_len)?)?;
            if at + 4 + sk_len != kem.len()
                || pk_len != MLKEM768_PK_LEN
                || sk_len != MLKEM768_SK_LEN
            {
                return None;
            }
            Some(Mkc1Plain {
                master_key,
                kem: Some((pk.to_vec(), Zeroizing::new(sk.to_vec()))),
            })
        }
        _ => None,
    }
}

/// Open a container with a rebuilt KEK, in the order of spec §2.5: header
/// bounds; the KEK commitment (a clean error before any AEAD runs); HKDF;
/// the AEAD; the payload; then the header's public keys against the
/// contents (the X25519 key re-derived from the master key unless pq, and
/// the ML-KEM public key byte for byte).
pub(crate) fn open_mkc1(bytes: &[u8], kek: &Kek) -> Result<Mkc1Plain, CustodyError> {
    let parsed = parse(bytes)?;
    let header = &parsed.header;
    if !bool::from(kek.commitment().ct_eq(&header.kek_commitment)) {
        return Err(CustodyError::Container(NOT_THIS_CONTAINER));
    }
    let wrap_key = kek.wrap_key(&header.kek_salt);
    let mut aad = MKC1_AAD_DOMAIN.to_vec();
    aad.extend_from_slice(&bytes[..parsed.header_len]);
    let (body, tag) = parsed.ct.split_at(parsed.ct.len() - 16);
    let mut pt = Zeroizing::new(body.to_vec());
    decrypt_payload(&wrap_key, &parsed.nonce, &aad, &mut pt, tag)
        .map_err(|_| CustodyError::Container(AUTH_FAILED))?;
    let plain = parse_payload(&pt).ok_or(CustodyError::Container("the payload is malformed"))?;
    let x_ok = header.crypto_mode == CRYPTO_MODE_PQ
        || vault_x25519_from_master(&plain.master_key).1 == header.vault_x25519_public;
    let kem_ok = match (&plain.kem, header.crypto_mode) {
        (None, CRYPTO_MODE_CLASSICAL) => true,
        (Some((pk, _)), CRYPTO_MODE_PQ | CRYPTO_MODE_HYBRID) => *pk == header.mlkem768_public,
        _ => false,
    };
    if !(x_ok && kem_ok) {
        return Err(CustodyError::Container(KEYS_MISMATCH));
    }
    Ok(plain)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::vault::custody::group::OsCustodyRng;

    fn kem_blob() -> (Vec<u8>, Vec<u8>) {
        (
            (0..MLKEM768_PK_LEN).map(|i| (i * 7) as u8).collect(),
            (0..MLKEM768_SK_LEN).map(|i| (i * 13 + 1) as u8).collect(),
        )
    }

    pub(crate) fn mkc1_plain_debug_is_redacted() {
        let plain = Mkc1Plain {
            master_key: Zeroizing::new([0x42; 32]),
            kem: Some((vec![1, 2], Zeroizing::new(vec![0x42; 8]))),
        };
        let shown = format!("{plain:?}");
        assert_eq!(shown, "Mkc1Plain(<redacted>)");
        assert!(!shown.contains("42"));
    }

    #[test]
    fn mkc1_round_trip_classical_and_with_kem_blob() {
        let kek = Kek::generate(&mut OsCustodyRng);
        let master = [0x5a; 32];
        let c =
            encode_mkc1_with(&kek, [1; 16], [2; 12], CRYPTO_MODE_CLASSICAL, &master, None).unwrap();
        assert_eq!(c.len(), 105 + 34 + 16);
        let header = decode_mkc1_header(&c).unwrap();
        assert_eq!(header.kek_commitment, kek.commitment());
        assert_eq!(
            header.vault_x25519_public,
            vault_x25519_from_master(&master).1
        );
        let plain = open_mkc1(&c, &kek).unwrap();
        assert_eq!(*plain.master_key, master);
        assert!(plain.kem.is_none());

        let (pk, sk) = kem_blob();
        for mode in [CRYPTO_MODE_HYBRID, CRYPTO_MODE_PQ] {
            let c =
                encode_mkc1_with(&kek, [3; 16], [4; 12], mode, &master, Some((&pk, &sk))).unwrap();
            assert_eq!(c.len(), 4931, "the worst case of spec §2.5");
            let header = decode_mkc1_header(&c).unwrap();
            assert_eq!(header.mlkem768_public, pk);
            assert_eq!(
                header.vault_x25519_public == [0u8; 32],
                mode == CRYPTO_MODE_PQ
            );
            let plain = open_mkc1(&c, &kek).unwrap();
            assert_eq!(*plain.master_key, master);
            let (got_pk, got_sk) = plain.kem.unwrap();
            assert_eq!((got_pk, got_sk.to_vec()), (pk.clone(), sk.clone()));
        }
        // The mode and the KEM key go together.
        assert!(encode_mkc1_with(
            &kek,
            [1; 16],
            [2; 12],
            CRYPTO_MODE_CLASSICAL,
            &master,
            Some((&pk, &sk))
        )
        .is_err());
        assert!(
            encode_mkc1_with(&kek, [1; 16], [2; 12], CRYPTO_MODE_HYBRID, &master, None).is_err()
        );
    }

    #[test]
    fn mkc1_wrong_kek_is_a_clean_error_before_aead() {
        let kek = Kek::generate(&mut OsCustodyRng);
        let other = Kek::generate(&mut OsCustodyRng);
        let mut c = encode_mkc1_with(
            &kek,
            [1; 16],
            [2; 12],
            CRYPTO_MODE_CLASSICAL,
            &[9; 32],
            None,
        )
        .unwrap();
        let want = "custody container: the key rebuilt from the shares is not this container's \
                    (KEK commitment differs) — nothing was decrypted";
        assert_eq!(open_mkc1(&c, &other).unwrap_err().to_string(), want);
        // With the ciphertext damaged too, the KEK check still answers
        // first: no AEAD ran.
        let last = c.len() - 1;
        c[last] ^= 1;
        assert_eq!(open_mkc1(&c, &other).unwrap_err().to_string(), want);
        assert_eq!(
            open_mkc1(&c, &kek).unwrap_err().to_string(),
            "custody container: authentication failed — the file was altered or damaged"
        );
    }

    #[test]
    fn mkc1_tamper_fails_authentication() {
        let kek = Kek::generate(&mut OsCustodyRng);
        let c = encode_mkc1_with(
            &kek,
            [1; 16],
            [2; 12],
            CRYPTO_MODE_CLASSICAL,
            &[9; 32],
            None,
        )
        .unwrap();
        // Every byte outside the KEK commitment is authenticated (the salt
        // changes the wrap key; the rest is AAD or ciphertext). A flip
        // either breaks the header bounds or fails the AEAD.
        for i in (0..c.len()).filter(|i| !(23..55).contains(i)) {
            let mut bent = c.clone();
            bent[i] ^= 0x01;
            let err = open_mkc1(&bent, &kek).unwrap_err().to_string();
            assert!(err.starts_with("custody container: "), "byte {i}: {err}");
            assert_ne!(
                err,
                format!("custody container: {KEYS_MISMATCH}"),
                "byte {i}"
            );
        }
    }

    #[test]
    fn mkc1_public_key_mismatch_refused() {
        let kek = Kek::generate(&mut OsCustodyRng);
        let master = [7u8; 32];
        let salt = [1u8; 16];
        let wrap_key = kek.wrap_key(&salt);
        let mut header = Mkc1Header {
            crypto_mode: CRYPTO_MODE_CLASSICAL,
            kek_salt: salt,
            kek_commitment: kek.commitment(),
            vault_x25519_public: vault_x25519_from_master(&master).1,
            mlkem768_public: Vec::new(),
        };
        header.vault_x25519_public[0] ^= 1;
        let c = encode_raw(&wrap_key, &header, [2; 12], &encode_payload(&master, None)).unwrap();
        assert_eq!(
            open_mkc1(&c, &kek).unwrap_err().to_string(),
            "custody container: its public keys do not match its contents"
        );
        // A hybrid header naming another ML-KEM key than the payload holds.
        let (pk, sk) = kem_blob();
        let mut other_pk = pk.clone();
        other_pk[100] ^= 1;
        let header = Mkc1Header {
            crypto_mode: CRYPTO_MODE_HYBRID,
            kek_salt: salt,
            kek_commitment: kek.commitment(),
            vault_x25519_public: vault_x25519_from_master(&master).1,
            mlkem768_public: other_pk,
        };
        let c = encode_raw(
            &wrap_key,
            &header,
            [2; 12],
            &encode_payload(&master, Some((&pk, &sk))),
        )
        .unwrap();
        assert_eq!(
            open_mkc1(&c, &kek).unwrap_err(),
            CustodyError::Container(KEYS_MISMATCH)
        );
    }

    #[test]
    fn mkc1_rejects_oversize_before_reading() {
        // 8193 bytes of nonsense: the size is refused before the magic is
        // read.
        assert_eq!(
            decode_mkc1_header(&vec![0u8; 8193])
                .unwrap_err()
                .to_string(),
            "custody container: longer than 8192 bytes"
        );
        let kek = Kek::generate(&mut OsCustodyRng);
        let c = encode_mkc1_with(
            &kek,
            [1; 16],
            [2; 12],
            CRYPTO_MODE_CLASSICAL,
            &[9; 32],
            None,
        )
        .unwrap();
        // ct_len 4097 is refused before any slice of that length is taken.
        let mut big = c.clone();
        big[101..105].copy_from_slice(&4097u32.to_le_bytes());
        assert_eq!(
            decode_mkc1_header(&big).unwrap_err().to_string(),
            "custody container: ciphertext length 4097 is out of bounds (17 to 4096)"
        );
        let mut huge = c.clone();
        huge[101..105].copy_from_slice(&u32::MAX.to_le_bytes());
        assert_eq!(
            decode_mkc1_header(&huge).unwrap_err(),
            CustodyError::ContainerCiphertextLength(u32::MAX)
        );
        assert_eq!(
            decode_mkc1_header(&c[..c.len() - 1]).unwrap_err(),
            CustodyError::Container("truncated")
        );
        let mut trailing = c.clone();
        trailing.push(0);
        assert_eq!(
            decode_mkc1_header(&trailing).unwrap_err(),
            CustodyError::Container("trailing bytes")
        );
        let mut magic = c.clone();
        magic[3] = b'2';
        assert_eq!(
            decode_mkc1_header(&magic).unwrap_err(),
            CustodyError::Container("bad magic")
        );
        let mut mode = c.clone();
        mode[6] = 4;
        assert_eq!(
            decode_mkc1_header(&mode).unwrap_err(),
            CustodyError::Container("unknown crypto mode")
        );
        let mut kem_len = c.clone();
        kem_len[87] = 1;
        assert!(decode_mkc1_header(&kem_len).is_err());
    }
}
