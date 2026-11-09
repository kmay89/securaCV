//! Pedersen verifiable secret sharing of the KEK pair `(k, r)` over
//! ristretto255 (spec §2.2).
//!
//! Dealing draws `f(x) = k + a_1 x + … + a_{n−1} x^{n−1}` and
//! `g(x) = r + b_1 x + … + b_{n−1} x^{n−1}` and publishes
//! `C_j = a_j·G + b_j·H` (`a_0 = k`, `b_0 = r`). The holder at index x gets
//! `(f(x), g(x))` and checks `s·G + t·H == Σ_j C_j x^j`. Any n distinct
//! shares rebuild `(k, r)` by Lagrange interpolation at zero, and the result
//! must commit to `C_0` before k is used.

use std::fmt;

use curve25519_dalek::scalar::Scalar;
use hkdf::Hkdf;
use sha2::Sha256;
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

use super::group::{commit, encode_point, random_array, random_scalar, Commitments, CustodyRng};
use super::{CustodyError, KEK_WRAP_INFO, MAX_HOLDERS, MIN_THRESHOLD};

/// The KEK pair `(k, r)`. `k` is the key; `r` is the Pedersen blinding
/// that keeps `C_0` from revealing anything about k.
pub(crate) struct Kek {
    k: Zeroizing<Scalar>,
    r: Zeroizing<Scalar>,
}

impl fmt::Debug for Kek {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("Kek(<redacted>)")
    }
}

impl Kek {
    /// A fresh KEK from `rng`: `k`, then `r`, each 64 bytes reduced mod ℓ
    /// and redrawn on zero.
    pub(crate) fn generate(rng: &mut dyn CustodyRng) -> Self {
        let k = random_scalar(rng);
        let r = random_scalar(rng);
        Kek { k, r }
    }

    /// A KEK from known scalars (known-answer tests and reconstruction).
    pub(crate) fn from_scalars(k: Scalar, r: Scalar) -> Self {
        Kek {
            k: Zeroizing::new(k),
            r: Zeroizing::new(r),
        }
    }

    /// `C_0 = k·G + r·H`, the KEK commitment.
    pub(crate) fn commitment(&self) -> [u8; 32] {
        encode_point(&commit(&self.k, &self.r))
    }

    /// `HKDF-SHA256(salt = kek_salt, ikm = k as 32 LE bytes,
    /// info = "securacv:vault:kek-wrap:v1", L = 32)`: the key of the `MKC1`
    /// AEAD.
    pub(crate) fn wrap_key(&self, kek_salt: &[u8; 16]) -> Zeroizing<[u8; 32]> {
        let ikm = Zeroizing::new(self.k.to_bytes());
        let hk = Hkdf::<Sha256>::new(Some(kek_salt), ikm.as_ref());
        let mut out = Zeroizing::new([0u8; 32]);
        hk.expand(KEK_WRAP_INFO, out.as_mut())
            .expect("32 bytes is a valid HKDF-SHA256 length");
        out
    }

    /// Constant-time equality of both scalars.
    pub(crate) fn ct_eq(&self, other: &Kek) -> bool {
        (self.k.ct_eq(&other.k) & self.r.ct_eq(&other.r)).into()
    }

    #[cfg(test)]
    pub(crate) fn scalars(&self) -> (Scalar, Scalar) {
        (*self.k, *self.r)
    }
}

/// One holder's share `(s, t) = (f(x), g(x))` at index x.
pub(crate) struct SecretShare {
    /// The holder's index x, never 0.
    pub(crate) index: u8,
    s: Zeroizing<Scalar>,
    t: Zeroizing<Scalar>,
}

impl fmt::Debug for SecretShare {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SecretShare(index {}, <redacted>)", self.index)
    }
}

impl SecretShare {
    /// `s_le32 ‖ t_le32`, the plaintext a share file seals.
    pub(crate) fn to_bytes(&self) -> Zeroizing<[u8; 64]> {
        let mut out = Zeroizing::new([0u8; 64]);
        out[..32].copy_from_slice(&self.s.to_bytes());
        out[32..].copy_from_slice(&self.t.to_bytes());
        out
    }

    /// Decode `s_le32 ‖ t_le32` at `index`: both scalars canonical, the
    /// index nonzero (callers check its range against their split).
    pub(crate) fn from_bytes(index: u8, b: &[u8; 64]) -> Result<Self, CustodyError> {
        if index == 0 {
            return Err(CustodyError::BadIndices { m: MAX_HOLDERS });
        }
        let mut s_bytes = Zeroizing::new([0u8; 32]);
        s_bytes.copy_from_slice(&b[..32]);
        let mut t_bytes = Zeroizing::new([0u8; 32]);
        t_bytes.copy_from_slice(&b[32..]);
        let s = Zeroizing::new(super::group::decode_scalar(&s_bytes)?);
        let t = Zeroizing::new(super::group::decode_scalar(&t_bytes)?);
        Ok(SecretShare { index, s, t })
    }

    /// The same share under another index (tests that relabel a share).
    #[cfg(test)]
    pub(crate) fn relabeled(&self, index: u8) -> Self {
        SecretShare {
            index,
            s: self.s.clone(),
            t: self.t.clone(),
        }
    }

    /// The share with `s + ds` and `t + dt` (tests that forge a share).
    #[cfg(test)]
    pub(crate) fn shifted(&self, ds: Scalar, dt: Scalar) -> Self {
        SecretShare {
            index: self.index,
            s: Zeroizing::new(*self.s + ds),
            t: Zeroizing::new(*self.t + dt),
        }
    }

    #[cfg(test)]
    pub(crate) fn scalars(&self) -> (Scalar, Scalar) {
        (*self.s, *self.t)
    }
}

/// True when `indices` are distinct and each in `1..=MAX_HOLDERS`.
fn indices_ok(indices: &[u8]) -> bool {
    let mut seen = [false; 256];
    for &x in indices {
        if x == 0 || x > MAX_HOLDERS || seen[usize::from(x)] {
            return false;
        }
        seen[usize::from(x)] = true;
    }
    true
}

/// `p(x)` by Horner's rule, constant time in the coefficients.
fn eval(poly: &[Scalar], x: u8) -> Zeroizing<Scalar> {
    let x = Scalar::from(x);
    let mut acc = Zeroizing::new(Scalar::ZERO);
    for c in poly.iter().rev() {
        *acc = *acc * x + c;
    }
    acc
}

/// Deal from given coefficients: `f = [k, a_1, …]`, `g = [r, b_1, …]`, one
/// share per index. Refuses `n` outside `2..=32`, mismatched lengths, more
/// indices than 32 or fewer than n, and a zero or repeated index.
pub(crate) fn deal_with_coefficients(
    f: &[Scalar],
    g: &[Scalar],
    indices: &[u8],
) -> Result<(Commitments, Vec<SecretShare>), CustodyError> {
    let n = f.len();
    if n != g.len()
        || n < usize::from(MIN_THRESHOLD)
        || n > usize::from(MAX_HOLDERS)
        || indices.len() < n
        || indices.len() > usize::from(MAX_HOLDERS)
        || !indices_ok(indices)
    {
        return Err(CustodyError::DealParameters);
    }
    let points = f.iter().zip(g).map(|(a, b)| commit(a, b)).collect();
    let shares = indices
        .iter()
        .map(|&x| SecretShare {
            index: x,
            s: eval(f, x),
            t: eval(g, x),
        })
        .collect();
    Ok((Commitments::from_points(points), shares))
}

/// Deal `kek` n-of-`indices.len()`: draws `a_1..a_{n−1}`, then
/// `b_1..b_{n−1}`, from `rng`.
pub(crate) fn deal(
    kek: &Kek,
    n: u8,
    indices: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<(Commitments, Vec<SecretShare>), CustodyError> {
    if !(MIN_THRESHOLD..=MAX_HOLDERS).contains(&n) || indices.len() < usize::from(n) {
        return Err(CustodyError::DealParameters);
    }
    let n = usize::from(n);
    let mut f: Zeroizing<Vec<Scalar>> = Zeroizing::new(Vec::with_capacity(n));
    let mut g: Zeroizing<Vec<Scalar>> = Zeroizing::new(Vec::with_capacity(n));
    f.push(*kek.k);
    g.push(*kek.r);
    for _ in 1..n {
        f.push(*random_scalar(rng));
    }
    for _ in 1..n {
        g.push(*random_scalar(rng));
    }
    deal_with_coefficients(&f, &g, indices)
}

/// The Pedersen check `s·G + t·H == Σ_j C_j x^j`: the left side in
/// constant time, compared in constant time.
pub(crate) fn check_share(c: &Commitments, share: &SecretShare) -> bool {
    if share.index == 0 {
        return false;
    }
    let lhs = commit(&share.s, &share.t);
    lhs.ct_eq(&c.public_share(share.index)).into()
}

/// The Lagrange coefficients at zero for `indices`:
/// `λ_i = Π_{j≠i} x_j · (x_j − x_i)^{-1} mod ℓ`. Refuses an empty set, a
/// zero index and a repeated one. Public data.
pub(crate) fn lagrange_at_zero(indices: &[u8]) -> Result<Vec<Scalar>, CustodyError> {
    if indices.is_empty() || !indices_ok(indices) {
        return Err(CustodyError::BadIndices { m: MAX_HOLDERS });
    }
    Ok(indices
        .iter()
        .map(|&i| {
            let xi = Scalar::from(i);
            let (mut num, mut den) = (Scalar::ONE, Scalar::ONE);
            for &j in indices.iter().filter(|&&j| j != i) {
                let xj = Scalar::from(j);
                num *= xj;
                den *= xj - xi;
            }
            num * den.invert()
        })
        .collect())
}

/// Rebuild `(k, r)` from exactly n shares. The indices are checked before
/// any secret is read, every share passes its Pedersen check against `c`,
/// and the rebuilt pair must commit to `kek_commitment`, the split's pinned
/// KEK commitment, before it is returned.
pub(crate) fn reconstruct(
    kek_commitment: &[u8; 32],
    c: &Commitments,
    shares: &[&SecretShare],
) -> Result<Kek, CustodyError> {
    let n = c.threshold();
    if shares.len() != usize::from(n) {
        return Err(CustodyError::WrongShareCount {
            given: shares.len(),
            needed: n,
        });
    }
    let indices: Vec<u8> = shares.iter().map(|s| s.index).collect();
    let lambda = lagrange_at_zero(&indices)?;
    for share in shares {
        if !check_share(c, share) {
            return Err(CustodyError::ShareMismatch(share.index));
        }
    }
    let mut k = Zeroizing::new(Scalar::ZERO);
    let mut r = Zeroizing::new(Scalar::ZERO);
    for (share, l) in shares.iter().zip(&lambda) {
        *k += l * *share.s;
        *r += l * *share.t;
    }
    let rebuilt = Kek { k, r };
    let ok: bool = rebuilt.commitment().ct_eq(kek_commitment).into();
    if !ok {
        return Err(CustodyError::KekMismatch);
    }
    Ok(rebuilt)
}

/// One dealing that passed deal-and-prove.
pub(crate) struct Dealt {
    /// The split id, drawn before the coefficients.
    pub(crate) split_id: [u8; 16],
    /// `C_0..C_{n−1}`.
    pub(crate) commitments: Commitments,
    /// One share per index, in index order.
    pub(crate) shares: Vec<SecretShare>,
    /// How many n-subsets rebuilt the KEK.
    pub(crate) subsets_checked: usize,
}

/// Binomial coefficient, saturating at `u64::MAX`.
fn binomial(m: u64, n: u64) -> u64 {
    let mut acc: u64 = 1;
    for i in 0..n {
        acc = acc.saturating_mul(m - i) / (i + 1);
    }
    acc
}

/// Every n-subset of `indices` in lexicographic order.
fn all_subsets(indices: &[u8], n: usize) -> Vec<Vec<u8>> {
    let mut out = Vec::new();
    let mut pick: Vec<usize> = (0..n).collect();
    loop {
        out.push(pick.iter().map(|&p| indices[p]).collect());
        let mut i = n;
        while i > 0 && pick[i - 1] == indices.len() - n + i - 1 {
            i -= 1;
        }
        if i == 0 {
            return out;
        }
        pick[i - 1] += 1;
        for j in i..n {
            pick[j] = pick[j - 1] + 1;
        }
    }
}

/// The subsets deal-and-prove rebuilds from: every n-subset when there are
/// at most 64, otherwise the first n indices, the last n and 62 drawn at
/// random.
fn proof_subsets(indices: &[u8], n: usize, rng: &mut dyn CustodyRng) -> Vec<Vec<u8>> {
    if binomial(indices.len() as u64, n as u64) <= 64 {
        return all_subsets(indices, n);
    }
    let mut out = vec![indices[..n].to_vec(), indices[indices.len() - n..].to_vec()];
    for _ in 0..62 {
        let mut pool = indices.to_vec();
        for i in 0..n {
            let draw = u32::from_le_bytes(random_array::<4>(rng));
            let j = i + (draw as usize) % (pool.len() - i);
            pool.swap(i, j);
        }
        let mut subset = pool[..n].to_vec();
        subset.sort_unstable();
        out.push(subset);
    }
    out
}

/// Deal `kek` and prove the dealing before anything leaves memory: draw the
/// split id, deal, check every share against the commitments, and rebuild
/// `(k, r)` from every n-subset (or 64 of them) and compare each with
/// `kek` in constant time. Any fault returns `deal-and-prove failed` and no
/// shares.
pub(crate) fn deal_and_prove(
    kek: &Kek,
    n: u8,
    indices: &[u8],
    rng: &mut dyn CustodyRng,
) -> Result<Dealt, CustodyError> {
    deal_and_prove_inner(kek, kek, n, indices, rng, &mut |_| {})
}

/// [`deal_and_prove`] with seams: `expect` is the KEK the rebuilt pairs
/// are compared with, and `tamper` runs on the shares between dealing and
/// the checks. Production passes the dealt KEK and a no-op.
fn deal_and_prove_inner(
    kek: &Kek,
    expect: &Kek,
    n: u8,
    indices: &[u8],
    rng: &mut dyn CustodyRng,
    tamper: &mut dyn FnMut(&mut Vec<SecretShare>),
) -> Result<Dealt, CustodyError> {
    let split_id: [u8; 16] = random_array(rng);
    let (commitments, mut shares) = deal(kek, n, indices, rng)?;
    tamper(&mut shares);
    for share in &shares {
        if !check_share(&commitments, share) {
            return Err(CustodyError::DealAndProve(format!(
                "share {} does not match its commitments",
                share.index
            )));
        }
    }
    let kek_commitment = expect.commitment();
    let subsets = proof_subsets(indices, usize::from(n), rng);
    for subset in &subsets {
        let picked: Vec<&SecretShare> = subset
            .iter()
            .map(|x| {
                shares
                    .iter()
                    .find(|s| s.index == *x)
                    .expect("a subset names dealt indices only")
            })
            .collect();
        let named = subset
            .iter()
            .map(|x| x.to_string())
            .collect::<Vec<_>>()
            .join(",");
        let rebuilt = reconstruct(&kek_commitment, &commitments, &picked).map_err(|_| {
            CustodyError::DealAndProve(format!("subset {{{named}}} rebuilt a different key"))
        })?;
        if !rebuilt.ct_eq(expect) {
            return Err(CustodyError::DealAndProve(format!(
                "subset {{{named}}} rebuilt a different key"
            )));
        }
    }
    Ok(Dealt {
        split_id,
        commitments,
        shares,
        subsets_checked: subsets.len(),
    })
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::vault::custody::group::OsCustodyRng;

    /// A seam RNG: zeros for the first `zeros` draws, then a counter.
    pub(crate) struct ZerosThenCounter {
        pub(crate) zeros: usize,
        pub(crate) draws: usize,
    }

    impl CustodyRng for ZerosThenCounter {
        fn fill(&mut self, out: &mut [u8]) {
            self.draws += 1;
            if self.draws <= self.zeros {
                out.fill(0);
            } else {
                for (i, b) in out.iter_mut().enumerate() {
                    *b = (self.draws as u8).wrapping_mul(31).wrapping_add(i as u8);
                }
            }
        }
    }

    fn indices(m: u8) -> Vec<u8> {
        (1..=m).collect()
    }

    #[test]
    fn check_accepts_every_dealt_share() {
        let mut rng = OsCustodyRng;
        for (n, m) in [(2u8, 2u8), (2, 3), (3, 5), (4, 6), (5, 9), (32, 32)] {
            let kek = Kek::generate(&mut rng);
            let (c, shares) = deal(&kek, n, &indices(m), &mut rng).unwrap();
            assert_eq!(c.threshold(), n);
            assert_eq!(c.kek_commitment(), kek.commitment());
            assert_eq!(shares.len(), usize::from(m));
            for share in &shares {
                assert!(
                    check_share(&c, share),
                    "share {} of {n}-of-{m}",
                    share.index
                );
            }
        }
    }

    #[test]
    fn check_rejects_s_plus_one_t_plus_one_and_wrong_index() {
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let (c, shares) = deal(&kek, 3, &indices(5), &mut rng).unwrap();
        let share = &shares[1];
        assert!(check_share(&c, share));
        assert!(!check_share(&c, &share.shifted(Scalar::ONE, Scalar::ZERO)));
        assert!(!check_share(&c, &share.shifted(Scalar::ZERO, Scalar::ONE)));
        assert!(!check_share(&c, &share.relabeled(3)));
        assert!(!check_share(&c, &share.relabeled(0)));
        // s and t swapped is not the share either.
        let (s, t) = share.scalars();
        let swapped = SecretShare {
            index: share.index,
            s: Zeroizing::new(t),
            t: Zeroizing::new(s),
        };
        assert!(!check_share(&c, &swapped));
    }

    #[test]
    fn reconstruct_every_n_subset_m_up_to_7() {
        let mut rng = OsCustodyRng;
        let mut rebuilt_count = 0usize;
        for m in 2u8..=7 {
            for n in 2u8..=m {
                let kek = Kek::generate(&mut rng);
                let (c, shares) = deal(&kek, n, &indices(m), &mut rng).unwrap();
                for subset in all_subsets(&indices(m), usize::from(n)) {
                    let picked: Vec<&SecretShare> = subset
                        .iter()
                        .map(|x| &shares[usize::from(*x) - 1])
                        .collect();
                    let rebuilt = reconstruct(&kek.commitment(), &c, &picked).unwrap();
                    assert!(rebuilt.ct_eq(&kek), "{n}-of-{m} subset {subset:?}");
                    rebuilt_count += 1;
                }
            }
        }
        // Σ_{m=2..7} Σ_{n=2..m} C(m, n) = 1 + 4 + 11 + 26 + 57 + 120.
        assert_eq!(rebuilt_count, 219);
    }

    #[test]
    fn reconstruct_refuses_fewer_or_more_than_n() {
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let (c, shares) = deal(&kek, 3, &indices(5), &mut rng).unwrap();
        let two: Vec<&SecretShare> = shares.iter().take(2).collect();
        let four: Vec<&SecretShare> = shares.iter().take(4).collect();
        assert_eq!(
            reconstruct(&kek.commitment(), &c, &two)
                .unwrap_err()
                .to_string(),
            "custody: 2 shares given; this split needs exactly 3"
        );
        assert_eq!(
            reconstruct(&kek.commitment(), &c, &four)
                .unwrap_err()
                .to_string(),
            "custody: 4 shares given; this split needs exactly 3"
        );
        assert_eq!(
            reconstruct(&kek.commitment(), &c, &[]).unwrap_err(),
            CustodyError::WrongShareCount {
                given: 0,
                needed: 3
            }
        );
    }

    #[test]
    fn lagrange_refuses_zero_duplicate_and_empty() {
        let refusal = "custody: share indices must be distinct and between 1 and 32";
        assert_eq!(lagrange_at_zero(&[]).unwrap_err().to_string(), refusal);
        assert_eq!(
            lagrange_at_zero(&[0, 1, 2]).unwrap_err().to_string(),
            refusal
        );
        assert_eq!(
            lagrange_at_zero(&[1, 2, 2]).unwrap_err().to_string(),
            refusal
        );
        assert_eq!(lagrange_at_zero(&[1, 33]).unwrap_err().to_string(), refusal);
        // λ for {1, 2}: λ_1 = 2/(2−1) = 2, λ_2 = 1/(1−2) = −1.
        assert_eq!(
            lagrange_at_zero(&[1, 2]).unwrap(),
            vec![Scalar::from(2u8), -Scalar::ONE]
        );

        // The SLIP-0039 attack: a share relabeled to index 0 would hand
        // over f(0) = k itself. Reconstruction refuses it before reading a
        // secret, and SecretShare::from_bytes refuses index 0 outright.
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let (c, shares) = deal(&kek, 2, &indices(3), &mut rng).unwrap();
        let zero = shares[0].relabeled(0);
        assert_eq!(
            reconstruct(&kek.commitment(), &c, &[&zero, &shares[1]])
                .unwrap_err()
                .to_string(),
            refusal
        );
        let dup = shares[1].relabeled(1);
        assert_eq!(
            reconstruct(&kek.commitment(), &c, &[&shares[0], &dup])
                .unwrap_err()
                .to_string(),
            refusal
        );
        assert!(SecretShare::from_bytes(0, &shares[0].to_bytes()).is_err());
    }

    #[test]
    fn reconstruct_mismatch_is_detected() {
        // A dealer forges the polynomials so that shares 1 and 2 stay the
        // real ones and share 3 is new: f' = f + δ(x−1)(x−2), likewise g'.
        // All three shares pass their checks against the forged
        // commitments, so only the rebuilt key's commitment can tell: it
        // must not match the split's pinned KEK commitment.
        let mut rng = OsCustodyRng;
        let f: Vec<Scalar> = (0..3).map(|_| *random_scalar(&mut rng)).collect();
        let g: Vec<Scalar> = (0..3).map(|_| *random_scalar(&mut rng)).collect();
        let (c, shares) = deal_with_coefficients(&f, &g, &indices(5)).unwrap();
        let kek = Kek::from_scalars(f[0], g[0]);
        assert_eq!(c.kek_commitment(), kek.commitment());
        let delta = *random_scalar(&mut rng);
        let eps = *random_scalar(&mut rng);
        let two = Scalar::from(2u8);
        let three = Scalar::from(3u8);
        let f2 = vec![f[0] + two * delta, f[1] - three * delta, f[2] + delta];
        let g2 = vec![g[0] + two * eps, g[1] - three * eps, g[2] + eps];
        let (c_forged, forged) = deal_with_coefficients(&f2, &g2, &indices(5)).unwrap();
        assert_eq!(forged[0].scalars(), shares[0].scalars());
        assert_eq!(forged[1].scalars(), shares[1].scalars());
        assert_ne!(forged[2].scalars(), shares[2].scalars());
        let picked = [&shares[0], &shares[1], &forged[2]];
        for s in picked {
            assert!(check_share(&c_forged, s));
        }
        assert_eq!(
            reconstruct(&kek.commitment(), &c_forged, &picked)
                .unwrap_err()
                .to_string(),
            "custody: the key rebuilt from the shares does not match the split's KEK commitment"
        );
        // Against the real commitments the forged share is named.
        assert_eq!(
            reconstruct(&kek.commitment(), &c, &picked)
                .unwrap_err()
                .to_string(),
            "custody: share 3 does not match the split's commitments"
        );
        // The honest shares rebuild the honest key.
        let honest = [&shares[0], &shares[1], &shares[2]];
        assert!(reconstruct(&kek.commitment(), &c, &honest)
            .unwrap()
            .ct_eq(&kek));
    }

    #[test]
    fn deal_and_prove_detects_a_corrupted_share() {
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let ok = deal_and_prove(&kek, 3, &indices(5), &mut rng).unwrap();
        assert_eq!(ok.subsets_checked, 10);
        assert_eq!(ok.shares.len(), 5);

        let err = deal_and_prove_inner(&kek, &kek, 3, &indices(5), &mut rng, &mut |shares| {
            shares[3] = shares[3].shifted(Scalar::ONE, Scalar::ZERO);
        })
        .err()
        .expect("a corrupted share fails the call and returns nothing");
        assert_eq!(
            err.to_string(),
            "deal-and-prove failed: share 4 does not match its commitments — nothing was written"
        );

        // A rebuilt key that is not the one dealt names its subset.
        let other = Kek::generate(&mut rng);
        let err = deal_and_prove_inner(&kek, &other, 2, &indices(3), &mut rng, &mut |_| {})
            .err()
            .expect("a different key fails the call");
        assert_eq!(
            err.to_string(),
            "deal-and-prove failed: subset {1,2} rebuilt a different key — nothing was written"
        );

        // More than 64 subsets: 64 are checked (C(10, 5) = 252).
        let big = deal_and_prove(&kek, 5, &indices(10), &mut rng).unwrap();
        assert_eq!(big.subsets_checked, 64);
    }

    #[test]
    fn recovery_split_has_the_same_c0() {
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let operational = deal_and_prove(&kek, 3, &indices(5), &mut rng).unwrap();
        let recovery = deal_and_prove(&kek, 4, &indices(6), &mut rng).unwrap();
        assert_eq!(
            operational.commitments.kek_commitment(),
            recovery.commitments.kek_commitment()
        );
        assert_eq!(operational.commitments.kek_commitment(), kek.commitment());
        assert_ne!(operational.split_id, recovery.split_id);
        assert_ne!(
            operational.commitments.encode()[1],
            recovery.commitments.encode()[1]
        );
        let picked: Vec<&SecretShare> = recovery.shares.iter().skip(2).collect();
        let rebuilt = reconstruct(&kek.commitment(), &recovery.commitments, &picked).unwrap();
        assert!(rebuilt.ct_eq(&kek));
    }

    #[test]
    fn kek_is_never_zero() {
        // The seam returns zero bytes for the first two draws, which reduce
        // to the zero scalar: k is redrawn, and so is r.
        let mut rng = ZerosThenCounter { zeros: 2, draws: 0 };
        let kek = Kek::generate(&mut rng);
        let (k, r) = kek.scalars();
        assert_ne!(k, Scalar::ZERO);
        assert_ne!(r, Scalar::ZERO);
        assert_eq!(rng.draws, 4, "two zero draws were discarded");
        let mut rng = ZerosThenCounter { zeros: 0, draws: 0 };
        Kek::generate(&mut rng);
        assert_eq!(rng.draws, 2);
    }

    #[test]
    fn secret_types_debug_is_redacted() {
        let mut rng = OsCustodyRng;
        let kek = Kek::generate(&mut rng);
        let (_, shares) = deal(&kek, 2, &indices(2), &mut rng).unwrap();
        assert_eq!(format!("{kek:?}"), "Kek(<redacted>)");
        assert_eq!(
            format!("{:?}", shares[0]),
            "SecretShare(index 1, <redacted>)"
        );
        let (k, _) = kek.scalars();
        let k_hex = hex::encode(k.to_bytes());
        assert!(!format!("{kek:?} {:?}", shares).contains(&k_hex));
        crate::vault::custody::recipient::tests::recipient_secret_debug_is_redacted();
        crate::vault::custody::container::tests::mkc1_plain_debug_is_redacted();
    }
}
