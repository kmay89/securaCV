//! The challenge message every member credential signs.
//!
//! ```text
//! msg = "securacv-hk-v1\0" ‖ purpose ‖ door_id ‖ be64(epoch) ‖ n_h
//!        15 bytes            1         16        8             32
//! ```
//!
//! `purpose` is `0x01` (set away), `0x02` (set home) or `0x03` (set night).
//! `door_id` is a key satellite's random door id, or sixteen zero bytes for
//! the LAN API. `epoch` is the roster epoch the challenge was issued under and
//! `n_h` is 32 bytes from the hub's OS RNG, used once.
//!
//! Note on length: the field list above sums to **72** bytes, which is what
//! the spec (`spec/household_keys_v0.md` §4.1) states. An earlier draft said
//! 88, which did not match its own fields. [`MSG_LEN`] is pinned by a test,
//! so a change to the layout is deliberate.

use crate::posture::HouseholdPosture;
use crate::{Error, Result};

/// Domain separation prefix for member challenge messages.
pub const HK_DOMAIN: &[u8; 15] = b"securacv-hk-v1\0";

/// Length of a challenge message in bytes (see the module note).
pub const MSG_LEN: usize = 15 + 1 + 16 + 8 + 32;

/// The door id the LAN API uses: sixteen zero bytes.
pub const LAN_DOOR_ID: [u8; 16] = [0u8; 16];

/// What a challenge asks the credential to authorize.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Purpose {
    /// Set the household posture to `away`.
    Away,
    /// Set the household posture to `home`. Intent credentials only.
    Home,
    /// Set the household posture to `night`.
    Night,
}

impl Purpose {
    /// The wire byte.
    pub fn as_byte(self) -> u8 {
        match self {
            Purpose::Away => 0x01,
            Purpose::Home => 0x02,
            Purpose::Night => 0x03,
        }
    }

    /// Parse the wire byte.
    pub fn from_byte(b: u8) -> Result<Purpose> {
        match b {
            0x01 => Ok(Purpose::Away),
            0x02 => Ok(Purpose::Home),
            0x03 => Ok(Purpose::Night),
            _ => Err(Error::Invalid("unknown purpose byte")),
        }
    }

    /// Parse the LAN API word (`away`, `home`, `night`).
    pub fn from_word(w: &str) -> Result<Purpose> {
        match w {
            "away" => Ok(Purpose::Away),
            "home" => Ok(Purpose::Home),
            "night" => Ok(Purpose::Night),
            _ => Err(Error::Invalid("unknown purpose word")),
        }
    }

    /// The LAN API word.
    pub fn word(self) -> &'static str {
        self.target().word()
    }

    /// The household posture this purpose sets.
    pub fn target(self) -> HouseholdPosture {
        match self {
            Purpose::Away => HouseholdPosture::Away,
            Purpose::Home => HouseholdPosture::Home,
            Purpose::Night => HouseholdPosture::Night,
        }
    }
}

/// Build a challenge message.
pub fn build_msg(
    purpose: Purpose,
    door_id: &[u8; 16],
    epoch: u64,
    n_h: &[u8; 32],
) -> [u8; MSG_LEN] {
    let mut m = [0u8; MSG_LEN];
    m[0..15].copy_from_slice(HK_DOMAIN);
    m[15] = purpose.as_byte();
    m[16..32].copy_from_slice(door_id);
    m[32..40].copy_from_slice(&epoch.to_be_bytes());
    m[40..72].copy_from_slice(n_h);
    m
}

/// The fields of a parsed challenge message.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ParsedMsg {
    /// What the challenge authorizes.
    pub purpose: Purpose,
    /// The door the challenge was issued for.
    pub door_id: [u8; 16],
    /// The roster epoch it was issued under.
    pub epoch: u64,
    /// The hub nonce.
    pub n_h: [u8; 32],
}

/// Parse a challenge message. A credential holder (the reference client, a
/// phone app) parses before signing and refuses a message whose purpose is
/// not the one its holder chose: on a plaintext LAN, a machine in the middle
/// could otherwise swap an `away` challenge for a `home` one.
pub fn parse_msg(m: &[u8]) -> Result<ParsedMsg> {
    if m.len() != MSG_LEN {
        return Err(Error::Invalid("challenge message has the wrong length"));
    }
    if &m[0..15] != HK_DOMAIN {
        return Err(Error::Invalid("challenge message has the wrong domain"));
    }
    let purpose = Purpose::from_byte(m[15])?;
    let mut door_id = [0u8; 16];
    door_id.copy_from_slice(&m[16..32]);
    let mut e = [0u8; 8];
    e.copy_from_slice(&m[32..40]);
    let mut n_h = [0u8; 32];
    n_h.copy_from_slice(&m[40..72]);
    Ok(ParsedMsg {
        purpose,
        door_id,
        epoch: u64::from_be_bytes(e),
        n_h,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn msg_length_is_pinned_to_the_field_list() {
        // 15 + 1 + 16 + 8 + 32. If this changes, the wire contract changed.
        assert_eq!(MSG_LEN, 72);
        assert_eq!(HK_DOMAIN.len(), 15);
    }

    #[test]
    fn layout_is_exact() {
        let door = [0x11u8; 16];
        let n = [0x22u8; 32];
        let m = build_msg(Purpose::Home, &door, 0x0102_0304_0506_0708, &n);
        assert_eq!(&m[..15], b"securacv-hk-v1\0");
        assert_eq!(m[15], 0x02);
        assert_eq!(&m[16..32], &door);
        assert_eq!(&m[32..40], &[1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(&m[40..], &n);
        let p = parse_msg(&m).unwrap();
        assert_eq!(p.purpose, Purpose::Home);
        assert_eq!(p.epoch, 0x0102_0304_0506_0708);
        assert_eq!(p.door_id, door);
        assert_eq!(p.n_h, n);
    }

    #[test]
    fn purpose_bytes_and_words() {
        for (p, b, w) in [
            (Purpose::Away, 1u8, "away"),
            (Purpose::Home, 2, "home"),
            (Purpose::Night, 3, "night"),
        ] {
            assert_eq!(p.as_byte(), b);
            assert_eq!(Purpose::from_byte(b).unwrap(), p);
            assert_eq!(Purpose::from_word(w).unwrap(), p);
            assert_eq!(p.word(), w);
        }
        assert!(Purpose::from_byte(0).is_err());
        assert!(Purpose::from_byte(4).is_err());
        assert!(Purpose::from_word("unset").is_err());
    }

    #[test]
    fn parse_refuses_bad_input() {
        let m = build_msg(Purpose::Away, &LAN_DOOR_ID, 1, &[0; 32]);
        assert!(parse_msg(&m[..71]).is_err());
        let mut bad = m;
        bad[0] ^= 1;
        assert!(parse_msg(&bad).is_err());
        let mut bad = m;
        bad[15] = 9;
        assert!(parse_msg(&bad).is_err());
    }
}
