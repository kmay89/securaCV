//! The encrypted session: everything after Pair Verify succeeds.
//!
//! HAP frames each direction independently. A frame is a two-byte
//! little-endian plaintext length, then the ciphertext, then a 16-byte
//! Poly1305 tag — and the **length prefix is the AAD**, so an attacker
//! cannot resize a frame without breaking its tag. Each direction counts its
//! own frames from zero and uses that counter as the nonce, which is what
//! makes reordering or replaying a frame fail to decrypt.
//!
//! The plaintext of one frame is capped at 1024 bytes by the specification,
//! so a longer HTTP body is simply split across frames. That cap is also a
//! useful bound for us (FR-4): a peer cannot make the accessory buffer an
//! arbitrary amount before the first authentication check happens.

use super::crypto::{self, counter_nonce, AeadError};
use super::pairing::SessionKeys;

/// The largest plaintext HAP puts in one frame.
pub const MAX_FRAME_PLAINTEXT: usize = 1024;

/// Length prefix (2) + plaintext (≤1024) + Poly1305 tag (16).
pub const MAX_FRAME_ON_WIRE: usize = 2 + MAX_FRAME_PLAINTEXT + 16;

/// An established, authenticated session with one controller.
pub struct Session {
    keys: SessionKeys,
    /// Frames the accessory has sent.
    out_counter: u64,
    /// Frames the accessory has accepted.
    in_counter: u64,
}

impl std::fmt::Debug for Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Session")
            .field("out_counter", &self.out_counter)
            .field("in_counter", &self.in_counter)
            .finish_non_exhaustive()
    }
}

impl Session {
    /// Start a session from the keys a completed Pair Verify produced.
    pub fn new(keys: SessionKeys) -> Self {
        Session {
            keys,
            out_counter: 0,
            in_counter: 0,
        }
    }

    /// Frame and encrypt a whole message for the controller, splitting it
    /// across as many frames as its length needs.
    pub fn seal(&mut self, plaintext: &[u8]) -> Vec<u8> {
        let mut out = Vec::with_capacity(plaintext.len() + 18);
        // An empty message still gets no frame — HAP has nothing to say
        // about zero-length bodies and a zero-length frame would burn a
        // nonce for nothing.
        for chunk in plaintext.chunks(MAX_FRAME_PLAINTEXT) {
            // `chunks` yields at most MAX_FRAME_PLAINTEXT, which fits a u16.
            let len = chunk.len() as u16;
            let aad = len.to_le_bytes();
            let sealed = crypto::seal(
                &self.keys.read,
                &counter_nonce(self.out_counter),
                &aad,
                chunk,
            );
            self.out_counter = self.out_counter.saturating_add(1);
            out.extend_from_slice(&aad);
            out.extend_from_slice(&sealed);
        }
        out
    }

    /// Try to take one complete frame off the front of `buf`.
    ///
    /// Returns `Ok(None)` when the buffer does not yet hold a whole frame —
    /// the normal case on a stream socket — and consumes the frame's bytes
    /// only once it has been authenticated, so a partial read is never
    /// destructive.
    pub fn open_frame(&mut self, buf: &mut Vec<u8>) -> Result<Option<Vec<u8>>, AeadError> {
        if buf.len() < 2 {
            return Ok(None);
        }
        let len = u16::from_le_bytes([buf[0], buf[1]]) as usize;
        if len > MAX_FRAME_PLAINTEXT {
            // A length past the spec cap is not a short read we should wait
            // on — it is a malformed peer, and waiting would let it pin
            // memory forever.
            return Err(AeadError);
        }
        let total = 2 + len + 16;
        if buf.len() < total {
            return Ok(None);
        }

        let aad = [buf[0], buf[1]];
        let plaintext = crypto::open(
            &self.keys.write,
            &counter_nonce(self.in_counter),
            &aad,
            &buf[2..total],
        )?;
        self.in_counter = self.in_counter.saturating_add(1);
        buf.drain(..total);
        Ok(Some(plaintext))
    }

    /// How many frames each direction has carried — an FR-10 observable, and
    /// the thing to look at when a session goes quiet.
    pub fn counters(&self) -> (u64, u64) {
        (self.out_counter, self.in_counter)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build the two ends of one session. The controller's view is the
    /// mirror image: what the accessory seals with, the controller opens
    /// with, and vice versa.
    fn pair() -> (Session, Session) {
        let keys = SessionKeys {
            read: [1u8; 32],
            write: [2u8; 32],
        };
        let mirrored = SessionKeys {
            read: keys.write,
            write: keys.read,
        };
        (Session::new(keys), Session::new(mirrored))
    }

    #[test]
    fn a_message_round_trips() {
        let (mut accessory, mut controller) = pair();
        let mut wire = accessory.seal(b"HTTP/1.1 200 OK");
        let got = controller
            .open_frame(&mut wire)
            .expect("opens")
            .expect("complete frame");
        assert_eq!(got, b"HTTP/1.1 200 OK");
        assert!(wire.is_empty(), "a consumed frame leaves nothing behind");
    }

    /// A body longer than the spec cap must split, and reassemble in order.
    #[test]
    fn a_long_message_splits_across_frames_and_reassembles() {
        let (mut accessory, mut controller) = pair();
        let body: Vec<u8> = (0..3000u32).map(|i| i as u8).collect();
        let mut wire = accessory.seal(&body);

        let mut got = Vec::new();
        while let Some(frame) = controller.open_frame(&mut wire).expect("opens") {
            assert!(frame.len() <= MAX_FRAME_PLAINTEXT);
            got.extend_from_slice(&frame);
        }
        assert_eq!(got, body);
        assert_eq!(accessory.counters().0, 3, "3000 bytes is three frames");
    }

    /// The realistic stream case: bytes arrive a few at a time. A partial
    /// frame must not be consumed or mis-parsed.
    #[test]
    fn a_frame_arriving_in_pieces_is_not_consumed_early() {
        let (mut accessory, mut controller) = pair();
        let full = accessory.seal(b"a body worth several reads");

        let mut buf = Vec::new();
        let mut delivered = None;
        for byte in &full {
            buf.push(*byte);
            if let Some(frame) = controller.open_frame(&mut buf).expect("opens") {
                delivered = Some(frame);
            }
        }
        assert_eq!(delivered.as_deref(), Some(&b"a body worth several reads"[..]));
        assert!(buf.is_empty());
    }

    /// Two frames back to back in one read must both come out.
    #[test]
    fn two_frames_in_one_buffer_both_open() {
        let (mut accessory, mut controller) = pair();
        let mut wire = accessory.seal(b"first");
        wire.extend_from_slice(&accessory.seal(b"second"));

        assert_eq!(
            controller.open_frame(&mut wire).expect("ok").as_deref(),
            Some(&b"first"[..])
        );
        assert_eq!(
            controller.open_frame(&mut wire).expect("ok").as_deref(),
            Some(&b"second"[..])
        );
        assert_eq!(controller.open_frame(&mut wire).expect("ok"), None);
    }

    /// Counters are the replay defense: a frame replayed at the wrong
    /// position must not decrypt.
    #[test]
    fn a_replayed_frame_does_not_open_twice() {
        let (mut accessory, mut controller) = pair();
        let frame = accessory.seal(b"once");
        let mut wire = frame.clone();
        controller.open_frame(&mut wire).expect("ok").expect("frame");

        // The same bytes again, now at counter 1.
        let mut replay = frame;
        assert_eq!(controller.open_frame(&mut replay), Err(AeadError));
    }

    /// Reordering must fail too — frame two cannot be accepted first.
    #[test]
    fn frames_out_of_order_are_refused() {
        let (mut accessory, mut controller) = pair();
        let _first = accessory.seal(b"first");
        let mut second = accessory.seal(b"second");
        assert_eq!(controller.open_frame(&mut second), Err(AeadError));
    }

    /// The length prefix is the AAD, so editing it breaks the tag rather
    /// than resizing the frame.
    ///
    /// Note which direction this test edits. Shrinking the length still
    /// leaves a complete frame, so the tag check runs and rejects it.
    /// *Inflating* it makes the frame look incomplete, and the reader
    /// correctly waits for bytes that will never come — that is a property
    /// of any length-prefixed stream, not a weakness here: no plaintext is
    /// ever released without the tag, and the spec cap plus the socket
    /// timeout bound the wait.
    #[test]
    fn the_length_prefix_is_authenticated() {
        let (mut accessory, mut controller) = pair();
        let mut wire = accessory.seal(b"body");
        wire[0] = wire[0].wrapping_sub(1);
        assert_eq!(controller.open_frame(&mut wire), Err(AeadError));
    }

    /// The companion case, asserted so the behavior is deliberate rather
    /// than incidental: an inflated length yields "not yet", never data.
    #[test]
    fn an_inflated_length_waits_rather_than_releasing_data() {
        let (mut accessory, mut controller) = pair();
        let mut wire = accessory.seal(b"body");
        wire[0] = wire[0].wrapping_add(1);
        assert_eq!(controller.open_frame(&mut wire), Ok(None));
    }

    #[test]
    fn a_tampered_body_does_not_open() {
        let (mut accessory, mut controller) = pair();
        let mut wire = accessory.seal(b"body");
        wire[3] ^= 0x40;
        assert_eq!(controller.open_frame(&mut wire), Err(AeadError));
    }

    /// A length past the spec cap is rejected outright rather than waited
    /// on, or a peer could pin a buffer forever by promising bytes it never
    /// sends.
    #[test]
    fn an_oversized_length_is_rejected_not_awaited() {
        let (_, mut controller) = pair();
        let mut buf = vec![0xFF, 0xFF];
        assert_eq!(controller.open_frame(&mut buf), Err(AeadError));
    }

    #[test]
    fn an_empty_buffer_is_not_an_error() {
        let (_, mut controller) = pair();
        let mut buf = Vec::new();
        assert_eq!(controller.open_frame(&mut buf), Ok(None));
        let mut one = vec![0x05];
        assert_eq!(controller.open_frame(&mut one), Ok(None));
        assert_eq!(one.len(), 1, "an incomplete header stays buffered");
    }

    /// The two directions must not interfere: the accessory's counter is
    /// its own.
    #[test]
    fn directions_count_independently() {
        let (mut accessory, mut controller) = pair();
        let mut a_to_c = accessory.seal(b"one");
        let mut c_to_a = controller.seal(b"two");

        assert_eq!(
            controller.open_frame(&mut a_to_c).expect("ok").as_deref(),
            Some(&b"one"[..])
        );
        assert_eq!(
            accessory.open_frame(&mut c_to_a).expect("ok").as_deref(),
            Some(&b"two"[..])
        );
        assert_eq!(accessory.counters(), (1, 1));
    }

    #[test]
    fn sealing_nothing_produces_nothing() {
        let (mut accessory, _) = pair();
        assert!(accessory.seal(b"").is_empty());
        assert_eq!(accessory.counters().0, 0, "no nonce is burned");
    }

    /// A frame of exactly the cap is legal and must not be split.
    #[test]
    fn an_exactly_full_frame_is_one_frame() {
        let (mut accessory, mut controller) = pair();
        let body = vec![7u8; MAX_FRAME_PLAINTEXT];
        let mut wire = accessory.seal(&body);
        assert_eq!(wire.len(), MAX_FRAME_ON_WIRE);
        assert_eq!(accessory.counters().0, 1);
        assert_eq!(
            controller.open_frame(&mut wire).expect("ok").map(|f| f.len()),
            Some(MAX_FRAME_PLAINTEXT)
        );
    }
}
