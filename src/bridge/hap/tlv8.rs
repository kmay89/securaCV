//! TLV8 — the type-length-value encoding HAP uses for pairing.
//!
//! One byte of type, one byte of length, then that many bytes of value. A
//! value longer than 255 bytes is split across consecutive items **of the
//! same type**, which the reader concatenates back together. That
//! fragmentation rule is the whole reason this needs to be a parser rather
//! than a `match` on a slice: an accessory's Ed25519 public key fits in one
//! item, but the encrypted sub-TLVs in pair-setup M5/M6 do not.
//!
//! Everything here takes untrusted bytes off a socket, so nothing in this
//! module panics, indexes without a bound, or allocates in proportion to a
//! length field it has not yet validated (FR-2, FR-4).

/// TLV item types used by HAP pairing. Only the ones this implementation
/// reads or writes are named; anything else is skipped on decode rather than
/// rejected, because HAP explicitly reserves the right to add items.
pub mod ty {
    pub const METHOD: u8 = 0x00;
    pub const IDENTIFIER: u8 = 0x01;
    pub const SALT: u8 = 0x02;
    pub const PUBLIC_KEY: u8 = 0x03;
    pub const PROOF: u8 = 0x04;
    pub const ENCRYPTED_DATA: u8 = 0x05;
    pub const STATE: u8 = 0x06;
    pub const ERROR: u8 = 0x07;
    pub const SIGNATURE: u8 = 0x0A;
    pub const PERMISSIONS: u8 = 0x0B;
    pub const FLAGS: u8 = 0x13;
    pub const SEPARATOR: u8 = 0xFF;
}

/// The pairing methods a controller can ask for.
///
/// These are the HAP R2 values, where plain pair setup is `0` and `1` is the
/// MFi-authenticated variant. R1 numbered them one higher; getting this wrong
/// shows up only as a controller that refuses to pair, so the values are
/// written down here once rather than inlined at each comparison.
pub mod method {
    pub const PAIR_SETUP: u8 = 0;
    pub const PAIR_SETUP_WITH_AUTH: u8 = 1;
    pub const PAIR_VERIFY: u8 = 2;
    pub const ADD_PAIRING: u8 = 3;
    pub const REMOVE_PAIRING: u8 = 4;
    pub const LIST_PAIRINGS: u8 = 5;
}

/// HAP's `kTLVError_*` codes — what we hand back when a pairing step fails.
///
/// Deliberately coarse: a controller learns *that* a step failed, never which
/// byte gave it away. `AUTHENTICATION` covers a wrong setup code, a bad SRP
/// proof and a bad signature alike.
pub mod error {
    pub const UNKNOWN: u8 = 0x01;
    pub const AUTHENTICATION: u8 = 0x02;
    pub const BACKOFF: u8 = 0x03;
    pub const MAX_PEERS: u8 = 0x04;
    pub const MAX_TRIES: u8 = 0x05;
    pub const UNAVAILABLE: u8 = 0x06;
    pub const BUSY: u8 = 0x07;
}

/// The largest TLV payload we will assemble from a single decode.
///
/// FR-4: a fragmented item is the one place a peer controls how much we
/// allocate, so the total is capped. Real HAP items are well under this — the
/// biggest is an encrypted sub-TLV of a few hundred bytes — so this bounds a
/// hostile peer without bounding a legitimate one.
pub const MAX_VALUE_LEN: usize = 16 * 1024;

/// A decoded TLV8 item list.
///
/// Kept as a flat `Vec` of (type, value) pairs rather than a map because HAP
/// uses a repeated `SEPARATOR` item to delimit records in `ListPairings`
/// responses — order is meaningful, and a map would silently lose it.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Tlv {
    items: Vec<(u8, Vec<u8>)>,
}

/// Why a TLV8 buffer could not be decoded.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum TlvError {
    /// A length byte ran past the end of the buffer.
    Truncated,
    /// A fragmented value exceeded [`MAX_VALUE_LEN`].
    TooLong,
}

impl std::fmt::Display for TlvError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            TlvError::Truncated => write!(f, "TLV8 item ran past the end of the buffer"),
            TlvError::TooLong => write!(f, "TLV8 value exceeded the {MAX_VALUE_LEN}-byte cap"),
        }
    }
}

impl std::error::Error for TlvError {}

impl Tlv {
    /// An empty item list.
    pub fn new() -> Self {
        Tlv::default()
    }

    /// Append an item. Values longer than 255 bytes are fragmented on encode,
    /// not here, so callers never think about the wire format.
    pub fn push(&mut self, ty: u8, value: impl Into<Vec<u8>>) -> &mut Self {
        self.items.push((ty, value.into()));
        self
    }

    /// Append a single-byte item — the common case for state, method, error
    /// and permissions.
    pub fn push_u8(&mut self, ty: u8, value: u8) -> &mut Self {
        self.push(ty, vec![value])
    }

    /// The first value with this type, if present.
    pub fn get(&self, ty: u8) -> Option<&[u8]> {
        self.items
            .iter()
            .find(|(t, _)| *t == ty)
            .map(|(_, v)| v.as_slice())
    }

    /// The first value with this type, as a single byte.
    ///
    /// Returns `None` for a zero-length or multi-byte value rather than
    /// taking the first byte of something that was never a `u8` — a lenient
    /// read here would let a malformed state field steer the state machine.
    pub fn get_u8(&self, ty: u8) -> Option<u8> {
        match self.get(ty) {
            Some([b]) => Some(*b),
            _ => None,
        }
    }

    /// Every item, in wire order.
    pub fn items(&self) -> &[(u8, Vec<u8>)] {
        &self.items
    }

    /// Encode to bytes, fragmenting any value longer than 255 bytes into
    /// consecutive same-type items.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for (ty, value) in &self.items {
            if value.is_empty() {
                out.push(*ty);
                out.push(0);
                continue;
            }
            for chunk in value.chunks(255) {
                out.push(*ty);
                // A chunk of `chunks(255)` is 1..=255 bytes, so this cast is
                // exact — no truncation is possible.
                out.push(chunk.len() as u8);
                out.extend_from_slice(chunk);
            }
        }
        out
    }

    /// Decode from bytes, reassembling fragmented values.
    ///
    /// Consecutive items of the same type are concatenated, per the HAP spec.
    /// A non-consecutive repeat starts a new item, which is what makes
    /// `SEPARATOR`-delimited pairing lists decode correctly.
    pub fn decode(mut buf: &[u8]) -> Result<Self, TlvError> {
        let mut items: Vec<(u8, Vec<u8>)> = Vec::new();
        while !buf.is_empty() {
            let (&ty, rest) = buf.split_first().ok_or(TlvError::Truncated)?;
            let (&len, rest) = rest.split_first().ok_or(TlvError::Truncated)?;
            let len = usize::from(len);
            if rest.len() < len {
                return Err(TlvError::Truncated);
            }
            let (value, rest) = rest.split_at(len);
            buf = rest;

            // Fragmentation: a repeat of the immediately preceding type
            // continues that value. Only a full 255-byte item may be
            // continued — a shorter one is complete by definition, so a
            // same-type item after it is a new item, not a fragment.
            match items.last_mut() {
                Some((last_ty, last_val)) if *last_ty == ty && last_val.len() % 255 == 0 => {
                    if last_val.len() + len > MAX_VALUE_LEN {
                        return Err(TlvError::TooLong);
                    }
                    last_val.extend_from_slice(value);
                }
                _ => {
                    if len > MAX_VALUE_LEN {
                        return Err(TlvError::TooLong);
                    }
                    items.push((ty, value.to_vec()));
                }
            }
        }
        Ok(Tlv { items })
    }

    /// The canonical one-item error response for a failed pairing step.
    pub fn error_response(state: u8, code: u8) -> Vec<u8> {
        let mut t = Tlv::new();
        t.push_u8(ty::STATE, state).push_u8(ty::ERROR, code);
        t.encode()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_short_items() {
        let mut t = Tlv::new();
        t.push_u8(ty::STATE, 2)
            .push(ty::SALT, vec![7u8; 16])
            .push_u8(ty::ERROR, error::AUTHENTICATION);
        let decoded = Tlv::decode(&t.encode()).expect("decodes");
        assert_eq!(decoded.get_u8(ty::STATE), Some(2));
        assert_eq!(decoded.get(ty::SALT), Some(&[7u8; 16][..]));
        assert_eq!(decoded.get_u8(ty::ERROR), Some(error::AUTHENTICATION));
    }

    /// The SRP public key is 384 bytes, so it *must* fragment. This is the
    /// case a naive one-byte-length encoder gets wrong.
    #[test]
    fn fragments_and_reassembles_a_384_byte_key() {
        let key = (0..384u32).map(|i| i as u8).collect::<Vec<_>>();
        let mut t = Tlv::new();
        t.push_u8(ty::STATE, 2).push(ty::PUBLIC_KEY, key.clone());
        let bytes = t.encode();

        // Two items on the wire: 255 bytes then 129.
        let key_items = bytes
            .iter()
            .enumerate()
            .filter(|(i, _)| *i == 3 || *i == 3 + 2 + 255)
            .count();
        assert_eq!(key_items, 2, "expected the key to fragment into two items");

        let decoded = Tlv::decode(&bytes).expect("decodes");
        assert_eq!(decoded.get(ty::PUBLIC_KEY), Some(key.as_slice()));
    }

    /// A value that is an exact multiple of 255 is the fragmentation edge
    /// case: the encoder must not emit a trailing empty item, and the decoder
    /// must not glue the next item onto it.
    #[test]
    fn exact_multiple_of_255_does_not_swallow_the_next_item() {
        let value = vec![9u8; 255];
        let mut t = Tlv::new();
        t.push(ty::PUBLIC_KEY, value.clone()).push_u8(ty::STATE, 4);
        let decoded = Tlv::decode(&t.encode()).expect("decodes");
        assert_eq!(decoded.get(ty::PUBLIC_KEY), Some(value.as_slice()));
        assert_eq!(decoded.get_u8(ty::STATE), Some(4));
    }

    #[test]
    fn truncated_input_is_an_error_not_a_panic() {
        // Length byte claims 8 bytes; only 2 follow.
        assert_eq!(
            Tlv::decode(&[0x03, 0x08, 0x01, 0x02]),
            Err(TlvError::Truncated)
        );
        assert_eq!(Tlv::decode(&[0x03]), Err(TlvError::Truncated));
    }

    #[test]
    fn a_multi_byte_value_is_not_read_as_a_u8() {
        let mut t = Tlv::new();
        t.push(ty::STATE, vec![1, 2]);
        let decoded = Tlv::decode(&t.encode()).expect("decodes");
        assert_eq!(decoded.get_u8(ty::STATE), None);
    }

    #[test]
    fn empty_value_round_trips() {
        let mut t = Tlv::new();
        t.push(ty::SEPARATOR, Vec::new());
        let decoded = Tlv::decode(&t.encode()).expect("decodes");
        assert_eq!(decoded.get(ty::SEPARATOR), Some(&[][..]));
    }

    /// A hostile peer must not be able to make us allocate without bound by
    /// chaining full-length fragments.
    #[test]
    fn fragmentation_is_capped() {
        let mut bytes = Vec::new();
        for _ in 0..(MAX_VALUE_LEN / 255 + 2) {
            bytes.push(ty::PUBLIC_KEY);
            bytes.push(255);
            bytes.extend_from_slice(&[0u8; 255]);
        }
        assert_eq!(Tlv::decode(&bytes), Err(TlvError::TooLong));
    }

    #[test]
    fn unknown_types_survive_a_round_trip() {
        let mut t = Tlv::new();
        t.push_u8(0x7E, 1).push_u8(ty::STATE, 6);
        let decoded = Tlv::decode(&t.encode()).expect("decodes");
        assert_eq!(decoded.get_u8(ty::STATE), Some(6));
        assert_eq!(decoded.get_u8(0x7E), Some(1));
    }
}
