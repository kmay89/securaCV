//! The household posture statement (hub → witnesses).
//!
//! A fixed 112-byte record, all integers big-endian:
//!
//! | Offset | Size | Field       | Notes |
//! |-------:|-----:|-------------|-------|
//! | 0      | 4    | `magic`     | ASCII `SCVP` |
//! | 4      | 1    | `version`   | `0x01` |
//! | 5      | 1    | `posture`   | `0x00` away, `0x01` home, `0x02` night |
//! | 6      | 2    | `reserved`  | `0x0000`; refused otherwise |
//! | 8      | 16   | `household` | random household id |
//! | 24     | 8    | `seq`       | strictly increasing per household |
//! | 32     | 8    | `issued_at` | Unix seconds, floored to the minute |
//! | 40     | 8    | `not_after` | Unix seconds; [`NEVER`] only for away and night |
//! | 48     | 64   | `sig`       | Ed25519 over `"securacv-posture-v1\0" ‖ bytes[0..48]` |
//!
//! A statement carries no credential, member, door or zone. A witness that
//! cannot verify a current statement behaves as `away` ([`effective_posture`]).

use crate::{Error, Result};
use ed25519_dalek::{Signature, Signer, SigningKey, VerifyingKey};

/// Statement magic.
pub const MAGIC: [u8; 4] = *b"SCVP";
/// Statement version.
pub const VERSION: u8 = 1;
/// Total statement length.
pub const STATEMENT_LEN: usize = 112;
/// Length of the signed prefix.
pub const SIGNED_LEN: usize = 48;
/// Signature domain prefix.
pub const DOMAIN: &[u8; 20] = b"securacv-posture-v1\0";
/// The longest a `home` statement may live: 24 hours.
pub const HOME_MAX_SECS: u64 = 86_400;
/// The default `home` lifetime: 12 hours.
pub const DEFAULT_HOME_TTL_SECS: u64 = 43_200;
/// `not_after` meaning "never lapses" (allowed for away and night only).
pub const NEVER: u64 = u64::MAX;

/// The household posture. Never call this bare "posture" in code: that word
/// already means body posture in Vision events.
///
/// "unset" is deliberately not a variant: it exists only as the attention
/// engine's input when no valid statement is available, and it is never
/// signed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum HouseholdPosture {
    /// Full sensing everywhere. The default and the fail-safe.
    Away,
    /// Indoor witnesses sense tamper only. Lapses.
    Home,
    /// Full sensing everywhere (behaves as away in v0).
    Night,
}

impl HouseholdPosture {
    /// The wire byte.
    pub fn as_byte(self) -> u8 {
        match self {
            HouseholdPosture::Away => 0x00,
            HouseholdPosture::Home => 0x01,
            HouseholdPosture::Night => 0x02,
        }
    }

    /// Parse the wire byte.
    pub fn from_byte(b: u8) -> Option<HouseholdPosture> {
        match b {
            0x00 => Some(HouseholdPosture::Away),
            0x01 => Some(HouseholdPosture::Home),
            0x02 => Some(HouseholdPosture::Night),
            _ => None,
        }
    }

    /// The dictionary word.
    pub fn word(self) -> &'static str {
        match self {
            HouseholdPosture::Away => "away",
            HouseholdPosture::Home => "home",
            HouseholdPosture::Night => "night",
        }
    }
}

/// The decoded fields of a statement (everything but the signature).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Statement {
    /// The household posture.
    pub posture: HouseholdPosture,
    /// The household id.
    pub household: [u8; 16],
    /// Sequence number, strictly increasing per household.
    pub seq: u64,
    /// Unix seconds, floored to the minute.
    pub issued_at: u64,
    /// Unix seconds after which the statement is void, or [`NEVER`].
    pub not_after: u64,
}

/// Why a witness refused a statement. Every reason means "behave as away".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Reject {
    /// Rule 1: not exactly 112 bytes.
    Length,
    /// Rule 1: wrong magic.
    Magic,
    /// Rule 1: wrong version.
    Version,
    /// Rule 1: reserved bytes are not zero.
    Reserved,
    /// Rule 1: unknown posture byte.
    UnknownPosture,
    /// `issued_at` is not floored to the minute.
    Precision,
    /// Rule 2: a different household.
    Household,
    /// Rule 3: the signature does not verify against the pinned key.
    Signature,
    /// Rule 4: `seq` is not greater than the last accepted one.
    Seq,
    /// Rule 5: the witness has no trusted wall clock.
    NoClock,
    /// Rule 5: `now >= not_after`.
    Lapsed,
    /// Rule 6: a `home` statement lives longer than [`HOME_MAX_SECS`] (this
    /// includes `home` with [`NEVER`]).
    HomeTooLong,
}

impl Statement {
    /// Build a statement issued at `now_unix` (floored to the minute).
    ///
    /// `home` gets `not_after = issued_at + home_ttl_secs` with
    /// `0 < home_ttl_secs <= HOME_MAX_SECS`; away and night never lapse.
    pub fn issue(
        posture: HouseholdPosture,
        household: [u8; 16],
        seq: u64,
        now_unix: u64,
        home_ttl_secs: u64,
    ) -> Result<Statement> {
        let issued_at = now_unix - (now_unix % 60);
        let not_after = match posture {
            HouseholdPosture::Home => {
                if home_ttl_secs == 0 || home_ttl_secs > HOME_MAX_SECS {
                    return Err(Error::Invalid("home TTL must be in 1..=HOME_MAX_SECS"));
                }
                issued_at
                    .checked_add(home_ttl_secs)
                    .ok_or(Error::Invalid("home TTL overflows"))?
            }
            HouseholdPosture::Away | HouseholdPosture::Night => NEVER,
        };
        Ok(Statement {
            posture,
            household,
            seq,
            issued_at,
            not_after,
        })
    }

    /// The 48 signed bytes.
    pub fn signed_part(&self) -> [u8; SIGNED_LEN] {
        let mut b = [0u8; SIGNED_LEN];
        b[0..4].copy_from_slice(&MAGIC);
        b[4] = VERSION;
        b[5] = self.posture.as_byte();
        // b[6..8] reserved, zero.
        b[8..24].copy_from_slice(&self.household);
        b[24..32].copy_from_slice(&self.seq.to_be_bytes());
        b[32..40].copy_from_slice(&self.issued_at.to_be_bytes());
        b[40..48].copy_from_slice(&self.not_after.to_be_bytes());
        b
    }

    /// Sign with the hub posture key and encode all 112 bytes.
    pub fn sign(&self, key: &SigningKey) -> [u8; STATEMENT_LEN] {
        let body = self.signed_part();
        let sig = key.sign(&signing_input(&body));
        let mut out = [0u8; STATEMENT_LEN];
        out[..SIGNED_LEN].copy_from_slice(&body);
        out[SIGNED_LEN..].copy_from_slice(&sig.to_bytes());
        out
    }

    /// Whether this statement is current at `now_unix`.
    pub fn is_current(&self, now_unix: u64) -> bool {
        now_unix < self.not_after
    }
}

fn signing_input(body: &[u8; SIGNED_LEN]) -> [u8; 20 + SIGNED_LEN] {
    let mut m = [0u8; 20 + SIGNED_LEN];
    m[..20].copy_from_slice(DOMAIN);
    m[20..].copy_from_slice(body);
    m
}

/// Decode the structure (rule 1 and the minute floor) without checking the
/// signature, household, sequence or clock. Never act on the result alone.
pub fn decode_structure(bytes: &[u8]) -> std::result::Result<Statement, Reject> {
    if bytes.len() != STATEMENT_LEN {
        return Err(Reject::Length);
    }
    if bytes[0..4] != MAGIC {
        return Err(Reject::Magic);
    }
    if bytes[4] != VERSION {
        return Err(Reject::Version);
    }
    let posture = HouseholdPosture::from_byte(bytes[5]).ok_or(Reject::UnknownPosture)?;
    if bytes[6] != 0 || bytes[7] != 0 {
        return Err(Reject::Reserved);
    }
    let mut household = [0u8; 16];
    household.copy_from_slice(&bytes[8..24]);
    let be = |r: std::ops::Range<usize>| {
        let mut a = [0u8; 8];
        a.copy_from_slice(&bytes[r]);
        u64::from_be_bytes(a)
    };
    let st = Statement {
        posture,
        household,
        seq: be(24..32),
        issued_at: be(32..40),
        not_after: be(40..48),
    };
    if !st.issued_at.is_multiple_of(60) {
        return Err(Reject::Precision);
    }
    Ok(st)
}

/// Every witness-side check of draft §3.2, in order.
///
/// - `expected_household`: the household the witness was paired into;
/// - `pinned`: the hub posture key pinned at pairing;
/// - `last_seq`: the last accepted `seq` (persisted by the witness), if any;
/// - `now_unix`: the witness's trusted wall clock, or `None` if it has none.
pub fn verify(
    bytes: &[u8],
    expected_household: &[u8; 16],
    pinned: &VerifyingKey,
    last_seq: Option<u64>,
    now_unix: Option<u64>,
) -> std::result::Result<Statement, Reject> {
    let st = decode_structure(bytes)?;
    if &st.household != expected_household {
        return Err(Reject::Household);
    }
    let mut body = [0u8; SIGNED_LEN];
    body.copy_from_slice(&bytes[..SIGNED_LEN]);
    let sig = Signature::from_slice(&bytes[SIGNED_LEN..]).map_err(|_| Reject::Signature)?;
    pinned
        .verify_strict(&signing_input(&body), &sig)
        .map_err(|_| Reject::Signature)?;
    if let Some(last) = last_seq {
        if st.seq <= last {
            return Err(Reject::Seq);
        }
    }
    let now = now_unix.ok_or(Reject::NoClock)?;
    if now >= st.not_after {
        return Err(Reject::Lapsed);
    }
    if st.posture == HouseholdPosture::Home {
        match st.not_after.checked_sub(st.issued_at) {
            Some(life) if life <= HOME_MAX_SECS => {}
            _ => return Err(Reject::HomeTooLong),
        }
    }
    Ok(st)
}

/// The posture a witness acts on: the statement's if every check passes,
/// otherwise `away`.
pub fn effective_posture(
    bytes: &[u8],
    expected_household: &[u8; 16],
    pinned: &VerifyingKey,
    last_seq: Option<u64>,
    now_unix: Option<u64>,
) -> HouseholdPosture {
    verify(bytes, expected_household, pinned, last_seq, now_unix)
        .map(|s| s.posture)
        .unwrap_or(HouseholdPosture::Away)
}

#[cfg(test)]
mod tests {
    use super::*;

    const HH: [u8; 16] = [0x42; 16];
    const T0: u64 = 1_790_000_000; // a minute boundary is computed below

    fn key() -> SigningKey {
        SigningKey::from_bytes(&[7u8; 32])
    }

    fn home(seq: u64) -> (Statement, [u8; STATEMENT_LEN]) {
        let st =
            Statement::issue(HouseholdPosture::Home, HH, seq, T0, DEFAULT_HOME_TTL_SECS).unwrap();
        let b = st.sign(&key());
        (st, b)
    }

    fn vk() -> VerifyingKey {
        key().verifying_key()
    }

    #[test]
    fn layout_is_exact() {
        let (st, b) = home(5);
        assert_eq!(b.len(), 112);
        assert_eq!(&b[0..4], b"SCVP");
        assert_eq!(b[4], 1);
        assert_eq!(b[5], 1);
        assert_eq!(&b[6..8], &[0, 0]);
        assert_eq!(&b[8..24], &HH);
        assert_eq!(&b[24..32], &5u64.to_be_bytes());
        assert_eq!(st.issued_at % 60, 0);
        assert!(st.issued_at <= T0 && T0 - st.issued_at < 60);
        assert_eq!(&b[32..40], &st.issued_at.to_be_bytes());
        assert_eq!(st.not_after, st.issued_at + DEFAULT_HOME_TTL_SECS);
        assert_eq!(&b[40..48], &st.not_after.to_be_bytes());
        // The signature covers the domain-prefixed 48 bytes.
        let mut m = DOMAIN.to_vec();
        m.extend_from_slice(&b[..48]);
        let sig = Signature::from_slice(&b[48..]).unwrap();
        vk().verify_strict(&m, &sig).unwrap();
        assert_eq!(DOMAIN.len(), 20);
    }

    #[test]
    fn valid_home_verifies_and_round_trips() {
        let (st, b) = home(5);
        let got = verify(&b, &HH, &vk(), Some(4), Some(T0 + 10)).unwrap();
        assert_eq!(got, st);
        assert_eq!(
            effective_posture(&b, &HH, &vk(), Some(4), Some(T0)),
            HouseholdPosture::Home
        );
    }

    #[test]
    fn rule1_structure() {
        let (_, b) = home(5);
        assert_eq!(
            verify(&b[..111], &HH, &vk(), None, Some(T0)),
            Err(Reject::Length)
        );
        let mut long = b.to_vec();
        long.push(0);
        assert_eq!(
            verify(&long, &HH, &vk(), None, Some(T0)),
            Err(Reject::Length)
        );
        let mut x = b;
        x[0] = b'X';
        assert_eq!(verify(&x, &HH, &vk(), None, Some(T0)), Err(Reject::Magic));
        let mut x = b;
        x[4] = 2;
        assert_eq!(verify(&x, &HH, &vk(), None, Some(T0)), Err(Reject::Version));
        let mut x = b;
        x[7] = 1;
        assert_eq!(
            verify(&x, &HH, &vk(), None, Some(T0)),
            Err(Reject::Reserved)
        );
        let mut x = b;
        x[5] = 3;
        assert_eq!(
            verify(&x, &HH, &vk(), None, Some(T0)),
            Err(Reject::UnknownPosture)
        );
    }

    #[test]
    fn rule2_household() {
        let (_, b) = home(5);
        assert_eq!(
            verify(&b, &[0x43; 16], &vk(), None, Some(T0)),
            Err(Reject::Household)
        );
    }

    #[test]
    fn rule3_tamper_and_wrong_key() {
        let (_, b) = home(5);
        // Flip every signed byte that keeps the structure valid: each must
        // break the signature.
        for i in [5usize, 24, 31, 39, 47] {
            let mut x = b;
            x[i] ^= 0x01;
            let r = verify(&x, &HH, &vk(), None, Some(T0));
            assert!(r.is_err(), "byte {i} tamper accepted");
            assert_eq!(
                effective_posture(&x, &HH, &vk(), None, Some(T0)),
                HouseholdPosture::Away
            );
        }
        let mut x = b;
        x[100] ^= 0x80;
        assert_eq!(
            verify(&x, &HH, &vk(), None, Some(T0)),
            Err(Reject::Signature)
        );
        let other = SigningKey::from_bytes(&[8u8; 32]).verifying_key();
        assert_eq!(
            verify(&b, &HH, &other, None, Some(T0)),
            Err(Reject::Signature)
        );
    }

    #[test]
    fn rule4_seq() {
        let (_, b) = home(5);
        assert_eq!(verify(&b, &HH, &vk(), Some(5), Some(T0)), Err(Reject::Seq));
        assert_eq!(verify(&b, &HH, &vk(), Some(6), Some(T0)), Err(Reject::Seq));
        assert!(verify(&b, &HH, &vk(), Some(4), Some(T0)).is_ok());
        assert!(verify(&b, &HH, &vk(), None, Some(T0)).is_ok());
    }

    #[test]
    fn rule5_clock_and_lapse() {
        let (st, b) = home(5);
        assert_eq!(verify(&b, &HH, &vk(), None, None), Err(Reject::NoClock));
        assert_eq!(
            verify(&b, &HH, &vk(), None, Some(st.not_after)),
            Err(Reject::Lapsed)
        );
        assert!(verify(&b, &HH, &vk(), None, Some(st.not_after - 1)).is_ok());
        assert_eq!(
            effective_posture(&b, &HH, &vk(), None, Some(st.not_after + 3600)),
            HouseholdPosture::Away
        );
    }

    #[test]
    fn rule6_home_max() {
        // Hand-build a home statement that lives too long and sign it: the
        // signature is fine, the lifetime is not.
        let mut st = Statement::issue(HouseholdPosture::Home, HH, 1, T0, 60).unwrap();
        st.not_after = st.issued_at + HOME_MAX_SECS + 1;
        let b = st.sign(&key());
        assert_eq!(
            verify(&b, &HH, &vk(), None, Some(T0)),
            Err(Reject::HomeTooLong)
        );
        st.not_after = st.issued_at + HOME_MAX_SECS;
        let b = st.sign(&key());
        assert!(verify(&b, &HH, &vk(), None, Some(T0)).is_ok());
        // Home that never lapses is refused.
        st.not_after = NEVER;
        let b = st.sign(&key());
        assert_eq!(
            verify(&b, &HH, &vk(), None, Some(T0)),
            Err(Reject::HomeTooLong)
        );
        // Home whose not_after precedes issued_at is refused too.
        st.not_after = st.issued_at.saturating_sub(1);
        let b = st.sign(&key());
        assert!(verify(&b, &HH, &vk(), None, Some(st.not_after.saturating_sub(10))).is_err());
    }

    #[test]
    fn away_and_night_never_lapse() {
        for p in [HouseholdPosture::Away, HouseholdPosture::Night] {
            let st = Statement::issue(p, HH, 9, T0, DEFAULT_HOME_TTL_SECS).unwrap();
            assert_eq!(st.not_after, NEVER);
            let b = st.sign(&key());
            assert!(verify(&b, &HH, &vk(), None, Some(T0 + 10 * HOME_MAX_SECS)).is_ok());
        }
    }

    #[test]
    fn issue_refuses_bad_ttl() {
        assert!(Statement::issue(HouseholdPosture::Home, HH, 1, T0, 0).is_err());
        assert!(Statement::issue(HouseholdPosture::Home, HH, 1, T0, HOME_MAX_SECS + 1).is_err());
        assert!(Statement::issue(HouseholdPosture::Home, HH, 1, T0, HOME_MAX_SECS).is_ok());
    }

    #[test]
    fn unfloored_issued_at_is_refused() {
        let mut st = Statement::issue(HouseholdPosture::Away, HH, 1, T0, 60).unwrap();
        st.issued_at += 7;
        let b = st.sign(&key());
        assert_eq!(
            verify(&b, &HH, &vk(), None, Some(T0)),
            Err(Reject::Precision)
        );
    }

    #[test]
    fn posture_bytes() {
        for (p, b, w) in [
            (HouseholdPosture::Away, 0u8, "away"),
            (HouseholdPosture::Home, 1, "home"),
            (HouseholdPosture::Night, 2, "night"),
        ] {
            assert_eq!(p.as_byte(), b);
            assert_eq!(HouseholdPosture::from_byte(b), Some(p));
            assert_eq!(p.word(), w);
        }
        assert_eq!(HouseholdPosture::from_byte(3), None);
    }
}
