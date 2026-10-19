//! Lowercase hex, strict on input.
//!
//! The roster is canonical JSON, so a hex field has exactly one spelling:
//! lowercase, even length. Uppercase input is refused rather than folded, so
//! two rosters that differ only in hex case can never both exist.
//!
//! A secret (a KEK, a card key, a signing key's scalar) goes through the
//! `*_secret*` forms: they decode or encode straight into a [`Zeroizing`]
//! buffer, so no plain heap buffer ever holds the key bytes or their hex on
//! the way. The same holds one step upstream: a file whose text is a secret
//! is read by [`crate::store::read_bounded_secret`], into one [`Zeroizing`]
//! buffer sized by its bound that never grows (a growing read frees each
//! outgrown copy unwiped). The plain forms are for public data (handles,
//! digests, public keys, signatures); [`decode_array`] makes no heap buffer
//! at all.

use crate::{Error, Result};
use zeroize::Zeroizing;

fn encode_into(bytes: &[u8], out: &mut String) {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    for b in bytes {
        out.push(DIGITS[(b >> 4) as usize] as char);
        out.push(DIGITS[(b & 0x0f) as usize] as char);
    }
}

/// Encode bytes as lowercase hex.
pub fn encode(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    encode_into(bytes, &mut out);
    out
}

/// Encode a secret as lowercase hex, straight into a buffer that is
/// zeroized on drop.
pub fn encode_secret(bytes: &[u8]) -> Zeroizing<String> {
    let mut out = Zeroizing::new(String::with_capacity(bytes.len() * 2));
    encode_into(bytes, &mut out);
    out
}

fn nibble(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    }
}

/// Decode `s` into `out`, which must be exactly `s.len() / 2` bytes long.
/// On an error `out` may be partly written; a secret caller's `out` is a
/// [`Zeroizing`] buffer, so that is wiped with the rest.
fn decode_into(s: &str, out: &mut [u8]) -> Result<()> {
    let b = s.as_bytes();
    if !b.len().is_multiple_of(2) {
        return Err(Error::Invalid("hex has odd length"));
    }
    if b.len() / 2 != out.len() {
        return Err(Error::Invalid("hex has the wrong length"));
    }
    for (pair, o) in b.as_chunks::<2>().0.iter().zip(out.iter_mut()) {
        let hi = nibble(pair[0]).ok_or(Error::Invalid("hex is not lowercase hex"))?;
        let lo = nibble(pair[1]).ok_or(Error::Invalid("hex is not lowercase hex"))?;
        *o = (hi << 4) | lo;
    }
    Ok(())
}

/// Decode lowercase hex of any even length (public data).
pub fn decode(s: &str) -> Result<Vec<u8>> {
    let mut out = vec![0u8; s.len() / 2];
    decode_into(s, &mut out)?;
    Ok(out)
}

/// Decode a secret of any even length, straight into a buffer that is
/// zeroized on drop.
pub fn decode_secret(s: &str) -> Result<Zeroizing<Vec<u8>>> {
    let mut out = Zeroizing::new(vec![0u8; s.len() / 2]);
    decode_into(s, &mut out)?;
    Ok(out)
}

/// Decode lowercase hex of exactly `N` bytes (public data), with no heap
/// buffer on the way.
pub fn decode_array<const N: usize>(s: &str) -> Result<[u8; N]> {
    let mut out = [0u8; N];
    decode_into(s, &mut out)?;
    Ok(out)
}

/// Decode a secret of exactly `N` bytes, straight into a buffer that is
/// zeroized on drop.
pub fn decode_secret_array<const N: usize>(s: &str) -> Result<Zeroizing<[u8; N]>> {
    let mut out = Zeroizing::new([0u8; N]);
    decode_into(s, &mut out[..])?;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let b = [0x00, 0x01, 0xab, 0xff];
        assert_eq!(encode(&b), "0001abff");
        assert_eq!(decode("0001abff").unwrap(), b);
        assert_eq!(encode_secret(&b).as_str(), "0001abff");
        assert_eq!(&decode_secret("0001abff").unwrap()[..], &b);
        assert_eq!(*decode_secret_array::<4>("0001abff").unwrap(), b);
        assert_eq!(decode("").unwrap(), Vec::<u8>::new());
    }

    #[test]
    fn refuses_uppercase_odd_and_garbage() {
        assert!(decode("AB").is_err());
        assert!(decode("abc").is_err());
        assert!(decode("zz").is_err());
        assert!(decode_array::<2>("aabbcc").is_err());
        assert!(decode_array::<2>("aab").is_err());
        assert!(decode_array::<2>("AABB").is_err());
        assert_eq!(decode_array::<2>("aabb").unwrap(), [0xaa, 0xbb]);
        assert!(decode_secret("AB").is_err());
        assert!(decode_secret("abc").is_err());
        assert!(decode_secret_array::<2>("aabbcc").is_err());
        assert!(decode_secret_array::<2>("aab").is_err());
        assert!(decode_secret_array::<2>("zzzz").is_err());
    }

    /// The secret forms hand back `Zeroizing` buffers and nothing else: the
    /// bindings below do not compile against a plain `Vec`, array or
    /// `String`, so a secret path that calls them cannot leave its key bytes
    /// in a buffer that is freed without being wiped.
    #[test]
    fn secret_forms_return_zeroizing_buffers() {
        fn zeroized_on_drop<T: zeroize::Zeroize>(_: &Zeroizing<T>) {}
        let key: Zeroizing<[u8; 16]> =
            decode_secret_array::<16>("00112233445566778899aabbccddeeff").unwrap();
        zeroized_on_drop(&key);
        let bytes: Zeroizing<Vec<u8>> = decode_secret("00112233").unwrap();
        zeroized_on_drop(&bytes);
        let text: Zeroizing<String> = encode_secret(&key[..]);
        zeroized_on_drop(&text);
        assert_eq!(text.as_str(), "00112233445566778899aabbccddeeff");
    }
}
