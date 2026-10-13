//! Lowercase hex, strict on input.
//!
//! The roster is canonical JSON, so a hex field has exactly one spelling:
//! lowercase, even length. Uppercase input is refused rather than folded, so
//! two rosters that differ only in hex case can never both exist.

use crate::{Error, Result};

/// Encode bytes as lowercase hex.
pub fn encode(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        out.push(DIGITS[(b >> 4) as usize] as char);
        out.push(DIGITS[(b & 0x0f) as usize] as char);
    }
    out
}

fn nibble(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    }
}

/// Decode lowercase hex of any even length.
pub fn decode(s: &str) -> Result<Vec<u8>> {
    let b = s.as_bytes();
    if !b.len().is_multiple_of(2) {
        return Err(Error::Invalid("hex has odd length"));
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    for pair in b.chunks_exact(2) {
        let hi = nibble(pair[0]).ok_or(Error::Invalid("hex is not lowercase hex"))?;
        let lo = nibble(pair[1]).ok_or(Error::Invalid("hex is not lowercase hex"))?;
        out.push((hi << 4) | lo);
    }
    Ok(out)
}

/// Decode lowercase hex of exactly `N` bytes.
pub fn decode_array<const N: usize>(s: &str) -> Result<[u8; N]> {
    let v = decode(s)?;
    v.try_into()
        .map_err(|_| Error::Invalid("hex has the wrong length"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let b = [0x00, 0x01, 0xab, 0xff];
        assert_eq!(encode(&b), "0001abff");
        assert_eq!(decode("0001abff").unwrap(), b);
    }

    #[test]
    fn refuses_uppercase_odd_and_garbage() {
        assert!(decode("AB").is_err());
        assert!(decode("abc").is_err());
        assert!(decode("zz").is_err());
        assert!(decode_array::<2>("aabbcc").is_err());
        assert_eq!(decode_array::<2>("aabb").unwrap(), [0xaa, 0xbb]);
    }
}
