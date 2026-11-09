//! ristretto255, the generators G and H, Pedersen commitments, strict
//! decoders and the randomness seam (spec §2.2).
//!
//! Points are the 32-byte canonical encoding; decoding goes through
//! `CompressedRistretto::decompress`, which refuses a non-canonical
//! encoding, and the identity is refused wherever a point is a commitment.
//! The group has prime order, so two commitments are equal exactly when
//! their encodings are.

use std::sync::OnceLock;

use curve25519_dalek::constants::RISTRETTO_BASEPOINT_POINT;
use curve25519_dalek::ristretto::{CompressedRistretto, RistrettoPoint};
use curve25519_dalek::scalar::Scalar;
use curve25519_dalek::traits::{IsIdentity, MultiscalarMul, VartimeMultiscalarMul};
use rand::TryRng;
use sha2::{Digest, Sha256, Sha512};
use zeroize::Zeroizing;

use super::{frame, CustodyError, COMMITMENTS_DOMAIN, GENERATOR_H_LABEL};

/// The second Pedersen generator:
/// `RistrettoPoint::from_uniform_bytes(SHA-512("securacv:custody:pedersen-generator-H:v1"))`.
/// The one-way map of a uniform input gives a point whose discrete log
/// with respect to G nobody knows.
pub(crate) fn generator_h() -> RistrettoPoint {
    static H: OnceLock<RistrettoPoint> = OnceLock::new();
    *H.get_or_init(|| {
        let digest: [u8; 64] = Sha512::digest(GENERATOR_H_LABEL).into();
        RistrettoPoint::from_uniform_bytes(&digest)
    })
}

/// `a·G + b·H`, constant time in both scalars.
pub(crate) fn commit(a: &Scalar, b: &Scalar) -> RistrettoPoint {
    RistrettoPoint::multiscalar_mul([a, b], [RISTRETTO_BASEPOINT_POINT, generator_h()])
}

/// The canonical 32-byte encoding of a point.
pub(crate) fn encode_point(p: &RistrettoPoint) -> [u8; 32] {
    p.compress().to_bytes()
}

/// Decode a commitment: canonical and not the identity. `j` names it in
/// the error.
pub(crate) fn decode_commitment(
    bytes: &[u8; 32],
    j: usize,
) -> Result<RistrettoPoint, CustodyError> {
    let point = CompressedRistretto(*bytes)
        .decompress()
        .ok_or(CustodyError::PointNotCanonical(j))?;
    if point.is_identity() {
        return Err(CustodyError::PointIdentity(j));
    }
    Ok(point)
}

/// Decode a 32-byte little-endian scalar, refusing any value at or above
/// the group order.
pub(crate) fn decode_scalar(bytes: &[u8; 32]) -> Result<Scalar, CustodyError> {
    Option::<Scalar>::from(Scalar::from_canonical_bytes(*bytes))
        .ok_or(CustodyError::ScalarNotCanonical)
}

/// The commitments `C_0..C_{n-1}` of one split; `C_0` is the KEK
/// commitment. Public data.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct Commitments(Vec<RistrettoPoint>);

impl Commitments {
    /// Wrap already-checked points (dealing).
    pub(crate) fn from_points(points: Vec<RistrettoPoint>) -> Self {
        Commitments(points)
    }

    /// Decode `C_0..C_{n-1}`: each canonical and not the identity, and
    /// between 2 and 32 of them.
    pub(crate) fn decode(points: &[[u8; 32]]) -> Result<Self, CustodyError> {
        if points.len() < usize::from(super::MIN_THRESHOLD)
            || points.len() > usize::from(super::MAX_HOLDERS)
        {
            return Err(CustodyError::DealParameters);
        }
        let mut out = Vec::with_capacity(points.len());
        for (j, p) in points.iter().enumerate() {
            out.push(decode_commitment(p, j)?);
        }
        Ok(Commitments(out))
    }

    /// The canonical encodings, `C_0` first.
    pub(crate) fn encode(&self) -> Vec<[u8; 32]> {
        self.0.iter().map(encode_point).collect()
    }

    /// The threshold n (the number of commitments).
    pub(crate) fn threshold(&self) -> u8 {
        self.0.len() as u8
    }

    /// `C_0`, the KEK commitment.
    pub(crate) fn kek_commitment(&self) -> [u8; 32] {
        encode_point(&self.0[0])
    }

    /// The public share commitment `P_x = Σ_j C_j · x^j`. Variable time:
    /// every input is public.
    pub(crate) fn public_share(&self, x: u8) -> RistrettoPoint {
        let x = Scalar::from(x);
        let mut power = Scalar::ONE;
        let mut powers = Vec::with_capacity(self.0.len());
        for _ in 0..self.0.len() {
            powers.push(power);
            power *= x;
        }
        RistrettoPoint::vartime_multiscalar_mul(&powers, &self.0)
    }

    /// The commitment-set hash:
    /// `SHA-256(frame(COMMITMENTS_DOMAIN) ‖ suite ‖ role ‖ split_id ‖ n ‖ m ‖ C_0 ‖ … ‖ C_{n-1})`.
    pub(crate) fn sha256(&self, suite: u8, role: u8, split_id: &[u8; 16], m: u8) -> [u8; 32] {
        let mut buf = Vec::with_capacity(64 + 32 * self.0.len());
        frame(&mut buf, COMMITMENTS_DOMAIN);
        buf.push(suite);
        buf.push(role);
        buf.extend_from_slice(split_id);
        buf.push(self.threshold());
        buf.push(m);
        for c in self.encode() {
            buf.extend_from_slice(&c);
        }
        Sha256::digest(&buf).into()
    }
}

/// The randomness every custody operation draws from. Production uses the
/// OS RNG ([`OsCustodyRng`]); the known-answer tests use a deterministic
/// stream whose draws are normative (`kat::KatRng`).
pub(crate) trait CustodyRng {
    /// Fill `out` with random bytes. One call is one draw.
    fn fill(&mut self, out: &mut [u8]);
}

/// The OS CSPRNG. It panics "OS RNG unavailable", as the vault's own
/// draws do: no custody value is ever drawn from anything weaker.
pub(crate) struct OsCustodyRng;

impl CustodyRng for OsCustodyRng {
    fn fill(&mut self, out: &mut [u8]) {
        rand::rngs::SysRng
            .try_fill_bytes(out)
            .expect("OS RNG unavailable");
    }
}

/// A uniform nonzero scalar: 64 random bytes reduced mod ℓ (bias at most
/// ℓ/2^512), redrawn on zero.
pub(crate) fn random_scalar(rng: &mut dyn CustodyRng) -> Zeroizing<Scalar> {
    loop {
        let mut wide = Zeroizing::new([0u8; 64]);
        rng.fill(wide.as_mut());
        let s = Zeroizing::new(Scalar::from_bytes_mod_order_wide(&wide));
        if *s != Scalar::ZERO {
            return s;
        }
    }
}

/// `count` random bytes as one draw.
pub(crate) fn random_array<const N: usize>(rng: &mut dyn CustodyRng) -> [u8; N] {
    let mut out = [0u8; N];
    rng.fill(&mut out);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// H as the stdlib-only Python generator computes it, independently of
    /// dalek (`spec/fixtures/threshold_custody_v1.json`, `generator_h`).
    /// The design brief's prototype printed a different value; that
    /// prototype used the even square root for `SQRT_AD_MINUS_ONE` where
    /// the ristretto255 constant is the odd one, and so failed the
    /// one-way-map vectors checked below (`docs/design/threshold_custody.md`).
    const H_HEX: &str = "b293a0f3f02ff22ef8bd4148b908d7e2629a2d16dfb690fe43e295a35574e562";

    #[test]
    fn generator_h_matches_fixture() {
        assert_eq!(hex::encode(encode_point(&generator_h())), H_HEX);
        // The one-way map H is built with, against the first vector of
        // draft-irtf-cfrg-ristretto255-decaf448-04 Appendix A.3 (as copied
        // into curve25519-dalek 5.0.0, src/ristretto/elligator.rs): the
        // input is SHA-512("Ristretto is traditionally a short shot of
        // espresso coffee").
        let input: [u8; 64] =
            Sha512::digest(b"Ristretto is traditionally a short shot of espresso coffee").into();
        assert_eq!(
            hex::encode(encode_point(&RistrettoPoint::from_uniform_bytes(&input))),
            "3066f82a1a747d45120d1740f14358531a8f04bbffe6a819f86dfe50f44a0a46"
        );
    }

    #[test]
    fn generator_h_is_not_g_nor_identity() {
        let h = generator_h();
        assert!(!h.is_identity());
        assert_ne!(encode_point(&h), encode_point(&RISTRETTO_BASEPOINT_POINT));
        assert_ne!(
            encode_point(&h),
            encode_point(&(RISTRETTO_BASEPOINT_POINT + RISTRETTO_BASEPOINT_POINT))
        );
        // H decodes from its own encoding (it is a canonical point).
        assert_eq!(decode_commitment(&encode_point(&h), 0).unwrap(), h);
    }

    #[test]
    fn commitments_reject_noncanonical_and_identity() {
        let g = encode_point(&RISTRETTO_BASEPOINT_POINT);
        let h = encode_point(&generator_h());
        assert!(Commitments::decode(&[g, h]).is_ok());
        // The identity encodes as 32 zero bytes.
        assert_eq!(
            Commitments::decode(&[g, [0u8; 32]]).unwrap_err(),
            CustodyError::PointIdentity(1)
        );
        // p - 1 is not a canonical field element encoding of s (it is
        // negative), and 2^255 - 1 is out of range.
        let mut p_minus_1 = [0xffu8; 32];
        p_minus_1[0] = 0xec;
        p_minus_1[31] = 0x7f;
        assert_eq!(
            Commitments::decode(&[p_minus_1, g]).unwrap_err(),
            CustodyError::PointNotCanonical(0)
        );
        assert_eq!(
            Commitments::decode(&[g, [0xff; 32]]).unwrap_err(),
            CustodyError::PointNotCanonical(1)
        );
        // Flipping the low bit of a valid encoding makes s negative.
        let mut neg = g;
        neg[0] ^= 1;
        assert!(Commitments::decode(&[neg, h]).is_err());
        // One commitment is not a split; 33 is more than any split has.
        assert_eq!(
            Commitments::decode(&[g]).unwrap_err(),
            CustodyError::DealParameters
        );
        assert_eq!(
            Commitments::decode(&[g; 33]).unwrap_err(),
            CustodyError::DealParameters
        );
    }

    #[test]
    fn scalar_decode_rejects_l_and_above() {
        // ℓ = 2^252 + 27742317777372353535851937790883648493, little-endian.
        let l: [u8; 32] = [
            0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9,
            0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10,
        ];
        assert_eq!(
            decode_scalar(&l).unwrap_err(),
            CustodyError::ScalarNotCanonical
        );
        let mut l_plus_1 = l;
        l_plus_1[0] += 1;
        assert_eq!(
            decode_scalar(&l_plus_1).unwrap_err(),
            CustodyError::ScalarNotCanonical
        );
        assert!(decode_scalar(&[0xff; 32]).is_err());
        let mut l_minus_1 = l;
        l_minus_1[0] -= 1;
        assert_eq!(decode_scalar(&l_minus_1).unwrap(), -Scalar::ONE);
        assert_eq!(decode_scalar(&[0u8; 32]).unwrap(), Scalar::ZERO);
    }
}
