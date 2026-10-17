//! NTAG 424 DNA `AuthenticateEV2First`, reader (hub) side.
//!
//! The hub runs the card's mutual authentication through a key satellite's
//! APDU tunnel (the satellite is not built in v0). This module produces the
//! command APDUs to send and consumes the card's responses; it holds no
//! socket and no radio. A card counts as presented when it proves knowledge
//! of an enrolled card key over a reader random bound to the challenge:
//!
//! ```text
//! RndA = SHA-256(msg)[0..16]
//! → 00 A4 04 00 07 D2760000850101 00          ISOSelectFile (NTAG 424 DNA application)
//! ← 90 00
//! → 90 71 00 00 02 KeyNo 00 00               AuthenticateEV2First, part 1
//! ← E(K, RndB) ‖ 91 AF                        AES-128-CBC, IV = 0
//! → 90 AF 00 00 20 E(K, RndA ‖ RndB<<<8) 00   part 2
//! ← E(K, TI ‖ RndA<<<8 ‖ PDcap2 ‖ PCDcap2) ‖ 91 00
//! ```
//!
//! The hub accepts only when the decrypted `RndA'` equals `RndA` rotated left
//! one byte (constant-time compare). Session keys follow NXP AN12196: `SV1 =
//! A5 5A 00 01 00 80 ‖ RndA[15..14] ‖ (RndA[13..8] ⊕ RndB[15..10]) ‖
//! RndB[9..0] ‖ RndA[7..0]` (`SV2` with `5A A5`), `SesAuthENCKey =
//! CMAC(K, SV1)`, `SesAuthMACKey = CMAC(K, SV2)`. They are derived and
//! returned for completeness; v0 sends nothing under them.
//!
//! **Trial authentication.** The card's response to part 1 does not say
//! which key it holds, and keys are never diversified from the UID (the UID
//! is a tracking identifier and Random ID SHOULD be on). So the hub tries
//! each enrolled card key in turn, bounded by the roster
//! ([`CARD_TRIAL_SLOTS`]). Real keys go first in a fresh random order, then
//! random dummy keys fill the remaining slots: anything that is not a
//! household card (an emulator, a skimmer) always sees exactly
//! [`CARD_TRIAL_SLOTS`] attempts, so the attempt count does not reveal how
//! many cards the household enrolled, while a household card never sees a
//! dummy attempt (its failures per tap stay below the number of real cards).
//! Someone relaying a genuine household card over many taps can still learn
//! the number of enrolled cards from the attempt counts.
//!
//! SUN/SDM messages are never used for authority.
//!
//! **Test vectors.** `tests::an12196_*` replay the two `AuthenticateEV2First`
//! worked examples of NXP AN12196 ("NTAG 424 DNA and NTAG 424 DNA TagTamper
//! features and hints"): key 0x00 (Table 14, full APDU transcript) and key
//! 0x03 (part 2 APDU, response, TI and session keys), kept as data in
//! `tests/vectors/an12196.json`. The bytes were taken
//! from the transcription in the `ntag424` crate 0.1.1 (MIT OR Apache-2.0,
//! <https://codeberg.org/jannschu/ntag424>, files `src/session/tests.rs`,
//! `src/commands/authenticate/aes.rs` and `src/crypto/suite.rs`), which
//! cites AN12196 for each; the application note itself was not fetched.

use crate::roster::Handle;
use crate::{rng, Error, Result};
use aes::cipher::{BlockCipherDecrypt, BlockCipherEncrypt};
use aes::Aes128;
use cmac::{Cmac, KeyInit, Mac};
use sha2::{Digest, Sha256};
use subtle::ConstantTimeEq;
use zeroize::Zeroizing;

/// How many keys the hub tries per tap, padded with dummies.
pub const CARD_TRIAL_SLOTS: usize = 16;

/// ISOSelectFile by DF name for the NTAG 424 DNA application
/// (`D2 76 00 00 85 01 01`).
pub const SELECT_APP: [u8; 13] = [
    0x00, 0xA4, 0x04, 0x00, 0x07, 0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01, 0x00,
];

/// The card key number the household uses for authentication.
pub const DEFAULT_KEY_NO: u8 = 0x00;

/// An AES-128 card key, zeroized on drop.
pub type CardKeyBytes = Zeroizing<[u8; 16]>;

/// AES-128-CBC encryption as `AuthenticateEV2First` uses it: the card's
/// protocol starts the chain from an all-zero IV (NXP AN12196), so the first
/// block is enciphered as it is and every later block is first XORed with the
/// ciphertext before it. The protocol fixes that IV, so the hub cannot choose
/// another and still talk to a real card; it is no secret either way. Secrecy
/// comes from the card's own AES key, and freshness from `RndA` and `RndB`,
/// which both sides draw anew for every exchange (each message is one or two
/// blocks, the first of them fresh random bytes). Written as the chaining
/// itself rather than through a CBC type, so no IV value exists in the code.
fn cbc_encrypt(key: &[u8; 16], buf: &mut [u8]) {
    debug_assert!(buf.len().is_multiple_of(16));
    let aes = Aes128::new(key.into());
    let mut prev: Option<[u8; 16]> = None;
    for block in buf.as_chunks_mut::<16>().0 {
        if let Some(p) = prev {
            block.iter_mut().zip(p).for_each(|(b, c)| *b ^= c);
        }
        aes.encrypt_block(block.into());
        prev = Some(*block);
    }
}

/// The inverse of [`cbc_encrypt`].
fn cbc_decrypt(key: &[u8; 16], buf: &mut [u8]) {
    debug_assert!(buf.len().is_multiple_of(16));
    let aes = Aes128::new(key.into());
    let mut prev: Option<[u8; 16]> = None;
    for block in buf.as_chunks_mut::<16>().0 {
        let ciphertext = *block;
        aes.decrypt_block(block.into());
        if let Some(p) = prev {
            block.iter_mut().zip(p).for_each(|(b, c)| *b ^= c);
        }
        prev = Some(ciphertext);
    }
}

fn rotl1(x: &[u8; 16]) -> [u8; 16] {
    let mut o = [0u8; 16];
    o[..15].copy_from_slice(&x[1..]);
    o[15] = x[0];
    o
}

/// `SV1`/`SV2` per AN12196 / the NT4H2421Gx datasheet.
pub fn session_vector(label: [u8; 2], rnd_a: &[u8; 16], rnd_b: &[u8; 16]) -> [u8; 32] {
    let mut sv = [0u8; 32];
    sv[0..2].copy_from_slice(&label);
    sv[2..6].copy_from_slice(&[0x00, 0x01, 0x00, 0x80]);
    sv[6..8].copy_from_slice(&rnd_a[0..2]);
    for i in 0..6 {
        sv[8 + i] = rnd_a[2 + i] ^ rnd_b[i];
    }
    sv[14..24].copy_from_slice(&rnd_b[6..16]);
    sv[24..32].copy_from_slice(&rnd_a[8..16]);
    sv
}

fn cmac16(key: &[u8; 16], data: &[u8]) -> [u8; 16] {
    let mut mac = <Cmac<Aes128> as KeyInit>::new_from_slice(key).expect("16-byte key");
    mac.update(data);
    let out = mac.finalize().into_bytes();
    let mut o = [0u8; 16];
    o.copy_from_slice(&out);
    o
}

/// `(SesAuthENCKey, SesAuthMACKey)`.
pub fn session_keys(
    key: &[u8; 16],
    rnd_a: &[u8; 16],
    rnd_b: &[u8; 16],
) -> (CardKeyBytes, CardKeyBytes) {
    let enc = cmac16(key, &session_vector([0xA5, 0x5A], rnd_a, rnd_b));
    let mac = cmac16(key, &session_vector([0x5A, 0xA5], rnd_a, rnd_b));
    (Zeroizing::new(enc), Zeroizing::new(mac))
}

/// The reader random for a challenge message: `SHA-256(msg)[0..16]`.
pub fn rnd_a_for(msg: &[u8]) -> [u8; 16] {
    let d = Sha256::digest(msg);
    let mut a = [0u8; 16];
    a.copy_from_slice(&d[..16]);
    a
}

/// Part 1 APDU.
pub fn part1_apdu(key_no: u8) -> [u8; 8] {
    [0x90, 0x71, 0x00, 0x00, 0x02, key_no, 0x00, 0x00]
}

/// Part 2 APDU for a candidate key, given the card's encrypted RndB.
/// Returns the APDU and the decrypted RndB.
pub fn part2_apdu(
    key: &[u8; 16],
    rnd_a: &[u8; 16],
    enc_rnd_b: &[u8; 16],
) -> ([u8; 38], CardKeyBytes) {
    let mut rnd_b = Zeroizing::new(*enc_rnd_b);
    cbc_decrypt(key, &mut rnd_b[..]);
    let mut body = Zeroizing::new([0u8; 32]);
    body[..16].copy_from_slice(rnd_a);
    body[16..].copy_from_slice(&rotl1(&rnd_b));
    cbc_encrypt(key, &mut body[..]);
    let mut apdu = [0u8; 38];
    apdu[..5].copy_from_slice(&[0x90, 0xAF, 0x00, 0x00, 0x20]);
    apdu[5..37].copy_from_slice(&body[..]);
    apdu[37] = 0x00;
    (apdu, rnd_b)
}

/// What a successful authentication yields.
pub struct Ev2Success {
    /// The member handle whose key authenticated.
    pub handle: Handle,
    /// The transaction identifier the card chose.
    pub ti: [u8; 4],
    /// Session encryption key.
    pub ses_enc: CardKeyBytes,
    /// Session MAC key.
    pub ses_mac: CardKeyBytes,
}

impl std::fmt::Debug for Ev2Success {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Ev2Success { .. }")
    }
}

/// Verify the card's part 2 response under a key. Returns the TI on success.
pub fn finish(key: &[u8; 16], rnd_a: &[u8; 16], enc_resp: &[u8; 32]) -> Option<[u8; 4]> {
    let mut pt = Zeroizing::new(*enc_resp);
    cbc_decrypt(key, &mut pt[..]);
    let want = rotl1(rnd_a);
    if !bool::from(pt[4..20].ct_eq(&want)) {
        return None;
    }
    let mut ti = [0u8; 4];
    ti.copy_from_slice(&pt[0..4]);
    Some(ti)
}

/// One step of the state machine.
#[derive(Debug)]
pub enum Ev2Step {
    /// Send these APDU bytes to the card and feed back its response.
    Send(Vec<u8>),
    /// An enrolled card authenticated.
    Authenticated(Ev2Success),
    /// No enrolled key authenticated, or the card misbehaved.
    Failed,
}

struct Candidate {
    handle: Option<Handle>,
    key: CardKeyBytes,
}

enum State {
    Select,
    Part1,
    Part2 { rnd_b: CardKeyBytes },
    Done,
}

/// The reader-side `AuthenticateEV2First` state machine with trial
/// authentication over the enrolled card keys.
pub struct Ev2Auth {
    rnd_a: [u8; 16],
    key_no: u8,
    candidates: Vec<Candidate>,
    idx: usize,
    state: State,
}

impl std::fmt::Debug for Ev2Auth {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Ev2Auth { .. }")
    }
}

impl Ev2Auth {
    /// Start an authentication for challenge message `msg` against the
    /// enrolled card keys. Returns the machine and the first APDU (select).
    pub fn start(msg: &[u8], keys: Vec<(Handle, CardKeyBytes)>) -> Result<(Ev2Auth, Vec<u8>)> {
        if keys.len() > CARD_TRIAL_SLOTS {
            return Err(Error::Invalid("more card keys than trial slots"));
        }
        let mut real: Vec<Candidate> = keys
            .into_iter()
            .map(|(h, k)| Candidate {
                handle: Some(h),
                key: k,
            })
            .collect();
        rng::shuffle(&mut real)?;
        while real.len() < CARD_TRIAL_SLOTS {
            real.push(Candidate {
                handle: None,
                key: Zeroizing::new(rng::array::<16>()?),
            });
        }
        Ok(Self::with_candidates(rnd_a_for(msg), DEFAULT_KEY_NO, real))
    }

    fn with_candidates(
        rnd_a: [u8; 16],
        key_no: u8,
        candidates: Vec<Candidate>,
    ) -> (Ev2Auth, Vec<u8>) {
        (
            Ev2Auth {
                rnd_a,
                key_no,
                candidates,
                idx: 0,
                state: State::Select,
            },
            SELECT_APP.to_vec(),
        )
    }

    fn fail(&mut self) -> Ev2Step {
        self.state = State::Done;
        Ev2Step::Failed
    }

    /// Feed the card's full response (data ‖ SW1 ‖ SW2).
    pub fn on_response(&mut self, resp: &[u8]) -> Ev2Step {
        let state = std::mem::replace(&mut self.state, State::Done);
        match state {
            State::Done => Ev2Step::Failed,
            State::Select => {
                if resp != [0x90, 0x00] {
                    return self.fail();
                }
                self.state = State::Part1;
                Ev2Step::Send(part1_apdu(self.key_no).to_vec())
            }
            State::Part1 => {
                if resp.len() != 18 || resp[16..] != [0x91, 0xAF] {
                    return self.fail();
                }
                let mut enc_b = [0u8; 16];
                enc_b.copy_from_slice(&resp[..16]);
                let (apdu, rnd_b) = part2_apdu(&self.candidates[self.idx].key, &self.rnd_a, &enc_b);
                self.state = State::Part2 { rnd_b };
                Ev2Step::Send(apdu.to_vec())
            }
            State::Part2 { rnd_b } => {
                if resp == [0x91, 0xAE] {
                    // Authentication error: this candidate is not the card's
                    // key. Try the next one with a fresh part 1.
                    self.idx += 1;
                    if self.idx >= self.candidates.len() {
                        return self.fail();
                    }
                    self.state = State::Part1;
                    return Ev2Step::Send(part1_apdu(self.key_no).to_vec());
                }
                if resp.len() != 34 || resp[32..] != [0x91, 0x00] {
                    return self.fail();
                }
                let mut enc = [0u8; 32];
                enc.copy_from_slice(&resp[..32]);
                let cand = &self.candidates[self.idx];
                match (finish(&cand.key, &self.rnd_a, &enc), cand.handle) {
                    (Some(ti), Some(handle)) => {
                        let (ses_enc, ses_mac) = session_keys(&cand.key, &self.rnd_a, &rnd_b);
                        Ev2Step::Authenticated(Ev2Success {
                            handle,
                            ti,
                            ses_enc,
                            ses_mac,
                        })
                    }
                    _ => self.fail(),
                }
            }
        }
    }
}

/// A software NTAG 424 DNA (card side of `AuthenticateEV2First` only), for
/// tests and for exercising the checker without a satellite.
pub mod sim {
    use super::*;

    /// A simulated card holding one AES key at key number 0.
    pub struct CardSimulator {
        key: CardKeyBytes,
        selected: bool,
        rnd_b: Option<[u8; 16]>,
        fixed_rnd_b: Option<[u8; 16]>,
        fixed_ti: Option<[u8; 4]>,
        /// Number of part 2 attempts that failed (the card's view).
        pub failed_auths: u32,
    }

    impl CardSimulator {
        /// A card with key `key`, drawing RndB and TI from the OS RNG.
        pub fn new(key: [u8; 16]) -> Self {
            CardSimulator {
                key: Zeroizing::new(key),
                selected: false,
                rnd_b: None,
                fixed_rnd_b: None,
                fixed_ti: None,
                failed_auths: 0,
            }
        }

        /// A card that always answers with this RndB and TI (vector replay).
        pub fn deterministic(key: [u8; 16], rnd_b: [u8; 16], ti: [u8; 4]) -> Self {
            let mut c = Self::new(key);
            c.fixed_rnd_b = Some(rnd_b);
            c.fixed_ti = Some(ti);
            c
        }

        /// Process one command APDU, returning data ‖ SW1 ‖ SW2.
        pub fn transceive(&mut self, apdu: &[u8]) -> Vec<u8> {
            if apdu == SELECT_APP {
                self.selected = true;
                self.rnd_b = None;
                return vec![0x90, 0x00];
            }
            if !self.selected {
                return vec![0x6A, 0x82];
            }
            if apdu.len() == 8 && apdu[..5] == [0x90, 0x71, 0x00, 0x00, 0x02] && apdu[7] == 0x00 {
                if apdu[5] != 0x00 {
                    return vec![0x91, 0x40]; // no such key
                }
                let rb = match self.fixed_rnd_b {
                    Some(r) => r,
                    None => rng::array::<16>().expect("os rng"),
                };
                self.rnd_b = Some(rb);
                let mut e = rb;
                cbc_encrypt(&self.key, &mut e);
                let mut out = e.to_vec();
                out.extend_from_slice(&[0x91, 0xAF]);
                return out;
            }
            if apdu.len() == 38 && apdu[..5] == [0x90, 0xAF, 0x00, 0x00, 0x20] && apdu[37] == 0x00 {
                let Some(rb) = self.rnd_b.take() else {
                    return vec![0x91, 0xCA]; // command aborted
                };
                let mut pt = [0u8; 32];
                pt.copy_from_slice(&apdu[5..37]);
                cbc_decrypt(&self.key, &mut pt);
                if pt[16..] != rotl1(&rb) {
                    self.failed_auths += 1;
                    return vec![0x91, 0xAE];
                }
                let mut rnd_a = [0u8; 16];
                rnd_a.copy_from_slice(&pt[..16]);
                let ti = match self.fixed_ti {
                    Some(t) => t,
                    None => rng::array::<4>().expect("os rng"),
                };
                let mut resp = [0u8; 32];
                resp[..4].copy_from_slice(&ti);
                resp[4..20].copy_from_slice(&rotl1(&rnd_a));
                // PDcap2 and PCDcap2 are zero.
                cbc_encrypt(&self.key, &mut resp);
                let mut out = resp.to_vec();
                out.extend_from_slice(&[0x91, 0x00]);
                return out;
            }
            vec![0x91, 0x1C] // illegal command
        }
    }
}

#[cfg(test)]
mod tests {
    use super::sim::CardSimulator;
    use super::*;
    use crate::hex;

    fn v(s: &str) -> Vec<u8> {
        hex::decode(&s.to_ascii_lowercase()).unwrap()
    }

    /// One field of an AN12196 worked example, read from
    /// `tests/vectors/an12196.json`. The published vectors live there as data
    /// (as `spec/fixtures/` keeps the attention vectors), so the examples'
    /// all-zero factory key is never a key literal in the code.
    fn an12196(example: &str, field: &str) -> Vec<u8> {
        let path = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/vectors/an12196.json");
        let text = std::fs::read_to_string(path).expect("read the AN12196 vectors");
        let all: serde_json::Value = serde_json::from_str(&text).expect("AN12196 vectors JSON");
        let hx = all[example][field]
            .as_str()
            .unwrap_or_else(|| panic!("AN12196 vectors: no {example}.{field}"));
        v(hx)
    }

    fn an12196_n<const N: usize>(example: &str, field: &str) -> [u8; N] {
        an12196(example, field)
            .try_into()
            .unwrap_or_else(|_| panic!("AN12196 vectors: {example}.{field} is not {N} bytes"))
    }

    /// A card response: the data, then the status word.
    fn with_sw(mut data: Vec<u8>, sw: [u8; 2]) -> Vec<u8> {
        data.extend_from_slice(&sw);
        data
    }

    /// AN12196, AuthenticateEV2First with key 0x00 (Table 14): the full APDU
    /// transcript, replayed through the state machine byte for byte.
    #[test]
    fn an12196_key0_full_transcript() {
        let x = |f| an12196("key0", f);
        let cands = vec![Candidate {
            handle: Some([0x01; 16]),
            key: Zeroizing::new(an12196_n::<16>("key0", "key")),
        }];
        let key_no = x("key_no")[0];
        let (mut m, first) = Ev2Auth::with_candidates(an12196_n("key0", "rnd_a"), key_no, cands);
        assert_eq!(first, x("select_apdu"));
        let Ev2Step::Send(p1) = m.on_response(&[0x90, 0x00]) else {
            panic!("expected part 1")
        };
        assert_eq!(p1, x("part1_apdu"));
        let Ev2Step::Send(p2) = m.on_response(&with_sw(x("card_part1"), [0x91, 0xAF])) else {
            panic!("expected part 2")
        };
        assert_eq!(p2, x("part2_apdu"));
        let Ev2Step::Authenticated(ok) = m.on_response(&with_sw(x("card_part2"), [0x91, 0x00]))
        else {
            panic!("expected success")
        };
        assert_eq!(ok.handle, [0x01; 16]);
        assert_eq!(ok.ti.to_vec(), x("ti"));
        assert_eq!(ok.ses_enc.to_vec(), x("ses_enc"));
        assert_eq!(ok.ses_mac.to_vec(), x("ses_mac"));
    }

    /// AN12196 key-0x00 example: the decrypted RndB and both session vectors.
    #[test]
    fn an12196_key0_rnd_b_and_session_vectors() {
        let key = Zeroizing::new(an12196_n::<16>("key0", "key"));
        let rnd_a = an12196_n::<16>("key0", "rnd_a");
        let (_, rnd_b) = part2_apdu(&key, &rnd_a, &an12196_n("key0", "card_part1"));
        assert_eq!(rnd_b.to_vec(), an12196("key0", "rnd_b"));
        assert_eq!(
            session_vector([0xA5, 0x5A], &rnd_a, &rnd_b).to_vec(),
            an12196("key0", "sv1")
        );
        assert_eq!(
            session_vector([0x5A, 0xA5], &rnd_a, &rnd_b).to_vec(),
            an12196("key0", "sv2")
        );
    }

    /// AN12196, AuthenticateEV2First with key 0x03: part 2 APDU, card
    /// response, TI, session vectors and session keys.
    #[test]
    fn an12196_key3_part2_ti_and_session_keys() {
        let x = |f| an12196("key3", f);
        let key = Zeroizing::new(an12196_n::<16>("key3", "key"));
        let rnd_a = an12196_n::<16>("key3", "rnd_a");
        let (apdu, rnd_b) = part2_apdu(&key, &rnd_a, &an12196_n("key3", "card_part1"));
        assert_eq!(rnd_b.to_vec(), x("rnd_b"));
        assert_eq!(apdu.to_vec(), x("part2_apdu"));
        let ti = finish(&key, &rnd_a, &an12196_n("key3", "card_part2"));
        assert_eq!(ti.map(|t| t.to_vec()), Some(x("ti")));
        assert_eq!(
            session_vector([0xA5, 0x5A], &rnd_a, &rnd_b).to_vec(),
            x("sv1")
        );
        let (enc, mac) = session_keys(&key, &rnd_a, &rnd_b);
        assert_eq!(enc.to_vec(), x("ses_enc"));
        assert_eq!(mac.to_vec(), x("ses_mac"));
        assert_eq!(part1_apdu(x("key_no")[0]).to_vec(), x("part1_apdu"));
    }

    /// The simulator reproduces the AN12196 key-0x00 card responses, so the
    /// simulator and the vectors agree before either is used alone.
    #[test]
    fn simulator_matches_an12196_key0() {
        let x = |f| an12196("key0", f);
        let mut card = CardSimulator::deterministic(
            an12196_n("key0", "key"),
            an12196_n("key0", "rnd_b"),
            an12196_n("key0", "ti"),
        );
        assert_eq!(card.transceive(&SELECT_APP), vec![0x90, 0x00]);
        let r1 = card.transceive(&x("part1_apdu"));
        assert_eq!(r1[..16], x("card_part1")[..]);
        let r2 = card.transceive(&x("part2_apdu"));
        assert_eq!(r2[..32], x("card_part2")[..]);
        assert_eq!(r2[32..], [0x91, 0x00]);
    }

    fn drive(m: &mut Ev2Auth, first: Vec<u8>, card: &mut CardSimulator) -> (Ev2Step, usize) {
        let mut apdu = first;
        let mut n = 0;
        loop {
            n += 1;
            assert!(n < 100, "runaway exchange");
            let resp = card.transceive(&apdu);
            match m.on_response(&resp) {
                Ev2Step::Send(next) => apdu = next,
                done => return (done, n),
            }
        }
    }

    fn keyset(n: u8) -> Vec<(Handle, CardKeyBytes)> {
        (0..n)
            .map(|i| ([0x30 + i; 16], Zeroizing::new([0x50 + i; 16])))
            .collect()
    }

    #[test]
    fn round_trip_finds_the_right_card_among_many() {
        let msg = crate::wire::build_msg(crate::wire::Purpose::Away, &[7; 16], 3, &[9; 32]);
        for target in 0..5u8 {
            let (mut m, first) = Ev2Auth::start(&msg, keyset(5)).unwrap();
            let mut card = CardSimulator::new([0x50 + target; 16]);
            let (step, _) = drive(&mut m, first, &mut card);
            match step {
                Ev2Step::Authenticated(ok) => assert_eq!(ok.handle, [0x30 + target; 16]),
                other => panic!("expected success, got {other:?}"),
            }
            // Real keys come first, so a household card never sees a dummy.
            assert!(card.failed_auths < 5);
        }
    }

    #[test]
    fn unknown_card_always_sees_every_slot() {
        let msg = crate::wire::build_msg(crate::wire::Purpose::Away, &[7; 16], 3, &[9; 32]);
        for n_real in [0u8, 1, 5, 16] {
            let (mut m, first) = Ev2Auth::start(&msg, keyset(n_real)).unwrap();
            let mut card = CardSimulator::new([0xEE; 16]);
            let (step, _) = drive(&mut m, first, &mut card);
            assert!(matches!(step, Ev2Step::Failed));
            assert_eq!(
                card.failed_auths as usize, CARD_TRIAL_SLOTS,
                "n_real={n_real}"
            );
        }
        assert!(Ev2Auth::start(&msg, keyset(17)).is_err());
    }

    #[test]
    fn rnd_a_is_bound_to_the_challenge() {
        let m1 = crate::wire::build_msg(crate::wire::Purpose::Away, &[7; 16], 3, &[9; 32]);
        let m2 = crate::wire::build_msg(crate::wire::Purpose::Night, &[7; 16], 3, &[9; 32]);
        assert_ne!(rnd_a_for(&m1), rnd_a_for(&m2));
        // A card response recorded under one challenge does not finish another.
        let key: [u8; 16] = crate::rng::array().unwrap();
        let mut resp = [0u8; 32];
        resp[4..20].copy_from_slice(&rotl1(&rnd_a_for(&m1)));
        cbc_encrypt(&key, &mut resp);
        assert!(finish(&key, &rnd_a_for(&m1), &resp).is_some());
        assert!(finish(&key, &rnd_a_for(&m2), &resp).is_none());
    }

    #[test]
    fn malformed_responses_fail_closed() {
        let msg = [0u8; 72];
        let (mut m, _) = Ev2Auth::start(&msg, keyset(1)).unwrap();
        assert!(matches!(m.on_response(&[0x6A, 0x82]), Ev2Step::Failed));
        assert!(
            matches!(m.on_response(&[0x90, 0x00]), Ev2Step::Failed),
            "machine resumed after failure"
        );
        let (mut m, _) = Ev2Auth::start(&msg, keyset(1)).unwrap();
        assert!(matches!(m.on_response(&[0x90, 0x00]), Ev2Step::Send(_)));
        assert!(
            matches!(m.on_response(&[0x91, 0xAF]), Ev2Step::Failed),
            "short part 1"
        );
        let (mut m, _) = Ev2Auth::start(&msg, keyset(1)).unwrap();
        m.on_response(&[0x90, 0x00]);
        let mut r = vec![0u8; 16];
        r.extend_from_slice(&[0x91, 0xAF]);
        assert!(matches!(m.on_response(&r), Ev2Step::Send(_)));
        // A card that claims success with a wrong RndA' is refused, and the
        // machine does not move on to the next key.
        let mut bad = vec![0u8; 32];
        bad.extend_from_slice(&[0x91, 0x00]);
        assert!(matches!(m.on_response(&bad), Ev2Step::Failed));
    }
}
