//! The checker: challenge → response → class/purpose policy → a new signed
//! household posture statement.
//!
//! The policy, in full:
//!
//! 1. A door cooling down (five rejections in ten minutes) refuses
//!    everything for 60 seconds ([`crate::ratelimit`]).
//! 2. The challenge is taken (single use) and must be unexpired, from the
//!    same session and door, and from the current roster epoch.
//! 3. The handle must name a current member of the **current** roster.
//!    An unknown handle runs a dummy verification, so the time taken does
//!    not say whether the handle exists.
//! 4. The credential must verify over the challenge under its own binding:
//!    `es256` over `msg`; `fido2-es256` with `clientDataHash = SHA-256(msg)`
//!    and UV required when the credential is `intent`; cards through
//!    [`Checker::start_card`].
//! 5. **Intent lowers, possession only raises.** Only an `intent`
//!    credential may set `home`. Possession credentials (cards, fobs, FIDO2
//!    without UV) may set `away` and `night`.
//! 6. The result is a fresh statement: `seq` persisted first, then signed,
//!    then published through [`Persist`] (the file seam). A `home`
//!    statement lapses; [`Checker::tick`] replaces a lapsed `home` with
//!    `away`.
//!
//! The checker starts in `away`, the fail-safe, whatever it said before a
//! restart. It records nothing about who presented what: no counters, no
//! history, and the rejection limiter is keyed by door only.

use crate::challenge::{ChallengeBook, ChallengeId, SessionId};
use crate::ntag424::{Ev2Auth, Ev2Step};
use crate::posture::{HouseholdPosture, Statement, HOME_MAX_SECS, STATEMENT_LEN};
use crate::ratelimit::DoorLimiter;
use crate::roster::{CredentialClass, Handle, MemberAlg, MemberEntry, MemberKind, Roster};
use crate::secrets::CardStore;
use crate::wire::Purpose;
use crate::{verify, Error, Result};
use sha2::{Digest, Sha256};

/// Where statements go and how `seq` survives a restart.
pub trait Persist: Send {
    /// Persist the last issued `seq` (called before the statement is signed).
    fn save_seq(&mut self, seq: u64) -> Result<()>;
    /// Publish a new statement (the file seam).
    fn publish(&mut self, statement: &[u8; STATEMENT_LEN]) -> Result<()>;
}

/// Keeps everything in memory (tests, embedding).
#[derive(Debug, Default)]
pub struct MemoryPersist {
    /// The last saved seq.
    pub seq: u64,
    /// The last published statement.
    pub last: Option<[u8; STATEMENT_LEN]>,
}

impl Persist for MemoryPersist {
    fn save_seq(&mut self, seq: u64) -> Result<()> {
        self.seq = seq;
        Ok(())
    }
    fn publish(&mut self, s: &[u8; STATEMENT_LEN]) -> Result<()> {
        self.last = Some(*s);
        Ok(())
    }
}

/// Writes `seq.json` and the seam file in a data directory.
#[derive(Debug)]
pub struct DirPersist(pub crate::store::DataDir);

impl Persist for DirPersist {
    fn save_seq(&mut self, seq: u64) -> Result<()> {
        self.0.save_seq(seq)
    }
    fn publish(&mut self, s: &[u8; STATEMENT_LEN]) -> Result<()> {
        crate::store::write_atomic(&self.0.seam_path(), s)
    }
}

/// A member's response to a challenge over the LAN API.
#[derive(Clone, Debug)]
pub struct CommandRequest {
    /// The challenge being answered.
    pub challenge_id: ChallengeId,
    /// The member handle.
    pub handle: Handle,
    /// The signature: ES256 DER.
    pub signature: Vec<u8>,
    /// FIDO2 only: authenticator data.
    pub authenticator_data: Option<Vec<u8>>,
}

/// A freshly issued challenge.
#[derive(Clone, Debug)]
pub struct IssuedChallenge {
    /// Its id.
    pub id: ChallengeId,
    /// The message to sign.
    pub msg: Vec<u8>,
}

/// An NFC card authentication in progress.
#[derive(Debug)]
pub struct CardSession {
    challenge: crate::challenge::Challenge,
    auth: Ev2Auth,
}

/// What the next card step is.
#[derive(Debug)]
pub enum CardProgress {
    /// Send these APDU bytes to the card and feed back its response.
    Send(Vec<u8>),
    /// The card authenticated and the posture changed.
    Done(Statement),
}

/// The checker.
pub struct Checker {
    household: [u8; 16],
    posture_key: ed25519_dalek::SigningKey,
    roster: Roster,
    cards: CardStore,
    book: ChallengeBook,
    limiter: DoorLimiter,
    home_ttl_secs: u64,
    last_seq: u64,
    current: Statement,
    current_bytes: [u8; STATEMENT_LEN],
    persist: Box<dyn Persist>,
    dummy_key: Vec<u8>,
}

impl std::fmt::Debug for Checker {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Checker")
            .field("epoch", &self.roster.epoch)
            .field("posture", &self.current.posture)
            .finish_non_exhaustive()
    }
}

/// The next `seq`: strictly above the last one, and never below
/// `minute(issued_at) << 10`, so a restored older `seq.json` (with a sane
/// clock) still cannot make the hub reuse a sequence number. The floor
/// carries nothing beyond `issued_at`, which the statement already states.
pub fn next_seq(last: u64, issued_at: u64) -> u64 {
    let floor = (issued_at / 60).saturating_mul(1024);
    last.saturating_add(1).max(floor)
}

impl Checker {
    /// Start a checker. Publishes a fresh `away` statement immediately.
    pub fn new(
        posture_key: ed25519_dalek::SigningKey,
        roster: Roster,
        cards: CardStore,
        last_seq: u64,
        home_ttl_secs: u64,
        persist: Box<dyn Persist>,
        now_unix: u64,
    ) -> Result<Checker> {
        if home_ttl_secs == 0 || home_ttl_secs > HOME_MAX_SECS {
            return Err(Error::Invalid("home TTL must be in 1..=86400 seconds"));
        }
        roster.validate()?;
        let dummy = p256::ecdsa::SigningKey::from_slice(&[0x01; 32])
            .map_err(|_| Error::Invalid("dummy key"))?;
        let household = roster.household;
        let placeholder = Statement {
            posture: HouseholdPosture::Away,
            household,
            seq: last_seq,
            issued_at: 0,
            not_after: crate::posture::NEVER,
        };
        let mut c = Checker {
            household,
            posture_key,
            book: ChallengeBook::new(roster.epoch),
            roster,
            cards,
            limiter: DoorLimiter::new(),
            home_ttl_secs,
            last_seq,
            current: placeholder,
            current_bytes: [0u8; STATEMENT_LEN],
            persist,
            dummy_key: dummy
                .verifying_key()
                .to_sec1_point(false)
                .as_bytes()
                .to_vec(),
        };
        c.issue(HouseholdPosture::Away, now_unix)?;
        Ok(c)
    }

    /// The household id.
    pub fn household(&self) -> [u8; 16] {
        self.household
    }

    /// The current roster epoch.
    pub fn epoch(&self) -> u64 {
        self.roster.epoch
    }

    /// The current statement.
    pub fn current(&self) -> Statement {
        self.current
    }

    /// The current statement's 112 bytes.
    pub fn current_bytes(&self) -> [u8; STATEMENT_LEN] {
        self.current_bytes
    }

    /// The hub posture public key witnesses pin.
    pub fn posture_public_key(&self) -> ed25519_dalek::VerifyingKey {
        self.posture_key.verifying_key()
    }

    /// Replace the roster (the caller has run the chain checks on it). Any epoch change
    /// discards every outstanding challenge.
    pub fn set_roster(&mut self, roster: Roster) -> Result<()> {
        roster.validate()?;
        if roster.household != self.household {
            return Err(Error::Rejected("roster is for another household"));
        }
        if roster.epoch < self.roster.epoch {
            return Err(Error::Rejected("roster epoch went backward"));
        }
        self.book.set_epoch(roster.epoch);
        self.roster = roster;
        Ok(())
    }

    /// Replace the card keys.
    pub fn set_cards(&mut self, cards: CardStore) {
        self.cards = cards;
    }

    fn issue(&mut self, posture: HouseholdPosture, now_unix: u64) -> Result<Statement> {
        let floored = now_unix - now_unix % 60;
        let seq = next_seq(self.last_seq, floored);
        let st = Statement::issue(posture, self.household, seq, now_unix, self.home_ttl_secs)?;
        self.persist.save_seq(seq)?;
        self.last_seq = seq;
        let bytes = st.sign(&self.posture_key);
        self.persist.publish(&bytes)?;
        self.current = st;
        self.current_bytes = bytes;
        Ok(st)
    }

    /// Replace a lapsed `home` with `away`. Call periodically.
    pub fn tick(&mut self, now_unix: u64) -> Result<()> {
        if self.current.posture == HouseholdPosture::Home && !self.current.is_current(now_unix) {
            self.issue(HouseholdPosture::Away, now_unix)?;
        }
        Ok(())
    }

    /// Record a rejection at `door` that happened outside the checker (a bad
    /// bearer at the LAN API, a malformed request).
    pub fn record_rejection(&mut self, door: &[u8; 16], now_ms: u64) {
        self.limiter.record_rejection(door, now_ms);
    }

    /// Whether `door` is cooling down.
    pub fn is_cooling(&mut self, door: &[u8; 16], now_ms: u64) -> bool {
        self.limiter.is_cooling(door, now_ms)
    }

    /// Issue a challenge for `purpose` at `door`, bound to `session`.
    pub fn issue_challenge(
        &mut self,
        purpose: Purpose,
        door: [u8; 16],
        session: SessionId,
        now_ms: u64,
    ) -> Result<IssuedChallenge> {
        if self.limiter.is_cooling(&door, now_ms) {
            return Err(Error::Rejected("door is cooling down"));
        }
        let c = self.book.issue(purpose, door, session, now_ms)?;
        Ok(IssuedChallenge {
            id: c.id,
            msg: c.msg().to_vec(),
        })
    }

    fn verify_member(&self, m: &MemberEntry, msg: &[u8], req: &CommandRequest) -> bool {
        let Some(pk) = m.public_key.as_deref() else {
            return false;
        };
        match (m.alg, &req.authenticator_data) {
            (MemberAlg::Es256, None) => verify::es256(pk, msg, &req.signature),
            (MemberAlg::Fido2Es256, Some(ad)) => {
                let Some(rp) = m.rp_id.as_deref() else {
                    return false;
                };
                let cdh: [u8; 32] = Sha256::digest(msg).into();
                verify::fido2_es256(
                    pk,
                    rp,
                    &cdh,
                    ad,
                    &req.signature,
                    m.class == CredentialClass::Intent,
                )
            }
            _ => false,
        }
    }

    /// Answer a challenge with a member credential over the LAN API (or a
    /// satellite relaying a phone, watch, fob or FIDO2 assertion).
    pub fn command(
        &mut self,
        req: &CommandRequest,
        session: &SessionId,
        door: &[u8; 16],
        now_ms: u64,
        now_unix: u64,
    ) -> Result<Statement> {
        if self.limiter.is_cooling(door, now_ms) {
            return Err(Error::Rejected("door is cooling down"));
        }
        let r = self.command_inner(req, session, door, now_ms, now_unix);
        if r.is_err() {
            self.limiter.record_rejection(door, now_ms);
        }
        r
    }

    fn command_inner(
        &mut self,
        req: &CommandRequest,
        session: &SessionId,
        door: &[u8; 16],
        now_ms: u64,
        now_unix: u64,
    ) -> Result<Statement> {
        let ch = self.book.take(&req.challenge_id, session, door, now_ms)?;
        let msg = ch.msg();
        let member = self.roster.member(&req.handle).cloned();
        let ok = match &member {
            Some(m) if m.is_current(now_unix) && m.kind != MemberKind::Card => {
                self.verify_member(m, &msg, req)
            }
            _ => {
                // Same work as a real check, then refuse.
                let _ = verify::es256(&self.dummy_key, &msg, &req.signature);
                false
            }
        };
        let member = match (ok, member) {
            (true, Some(m)) => m,
            _ => return Err(Error::Rejected("credential refused")),
        };
        if ch.purpose == Purpose::Home && member.class != CredentialClass::Intent {
            return Err(Error::Rejected("a possession credential cannot set home"));
        }
        self.issue(ch.purpose.target(), now_unix)
    }

    /// Start an NTAG 424 DNA authentication for a challenge. Returns the
    /// session and the first APDU to send through the satellite.
    pub fn start_card(
        &mut self,
        challenge_id: &ChallengeId,
        session: &SessionId,
        door: &[u8; 16],
        now_ms: u64,
        now_unix: u64,
    ) -> Result<(CardSession, Vec<u8>)> {
        if self.limiter.is_cooling(door, now_ms) {
            return Err(Error::Rejected("door is cooling down"));
        }
        let r = (|| {
            let ch = self.book.take(challenge_id, session, door, now_ms)?;
            if ch.purpose == Purpose::Home {
                return Err(Error::Rejected("a card cannot set home"));
            }
            let handles: Vec<Handle> = self
                .roster
                .members
                .iter()
                .filter(|m| m.kind == MemberKind::Card && m.is_current(now_unix))
                .map(|m| m.handle)
                .collect();
            let keys = self.cards.keys_for(&handles);
            let (auth, first) = Ev2Auth::start(&ch.msg(), keys)?;
            Ok((
                CardSession {
                    challenge: ch,
                    auth,
                },
                first,
            ))
        })();
        if r.is_err() {
            self.limiter.record_rejection(door, now_ms);
        }
        r
    }

    /// Feed a card response.
    pub fn card_response(
        &mut self,
        s: &mut CardSession,
        resp: &[u8],
        now_ms: u64,
        now_unix: u64,
    ) -> Result<CardProgress> {
        let door = s.challenge.door_id;
        let r = (|| match s.auth.on_response(resp) {
            Ev2Step::Send(apdu) => Ok(CardProgress::Send(apdu)),
            Ev2Step::Failed => Err(Error::Rejected("card refused")),
            Ev2Step::Authenticated(ok) => {
                let ch = &s.challenge;
                if now_ms.saturating_sub(ch.issued_ms) >= crate::challenge::CHALLENGE_TTL_MS {
                    return Err(Error::Rejected("card exchange outlived its challenge"));
                }
                if ch.epoch != self.roster.epoch {
                    return Err(Error::Rejected("roster changed during the card exchange"));
                }
                match self.roster.member(&ok.handle) {
                    Some(m) if m.kind == MemberKind::Card && m.is_current(now_unix) => {}
                    _ => return Err(Error::Rejected("card is no longer enrolled")),
                }
                let purpose = ch.purpose;
                Ok(CardProgress::Done(self.issue(purpose.target(), now_unix)?))
            }
        })();
        if r.is_err() {
            self.limiter.record_rejection(&door, now_ms);
        }
        r
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use crate::ntag424::sim::CardSimulator;
    use crate::posture;
    use crate::roster::tests::{card, ed_admin, phone};
    use crate::roster::{MemberAlg, MemberKind};
    use crate::verify::{FLAG_UP, FLAG_UV};
    use crate::wire::LAN_DOOR_ID;
    use p256::ecdsa::signature::Signer;
    use p256::ecdsa::SigningKey as PKey;

    pub const T0: u64 = 1_800_000_000;
    pub const S: SessionId = [7; 32];
    const INTENT: u8 = 0x10;
    const POSSESSION: u8 = 0x11;
    const FIDO_UV: u8 = 0x12;
    const CARD: u8 = 0x13;
    const RP: &str = "hk-member.example";

    pub fn pkey(seed: u8) -> PKey {
        PKey::from_slice(&[seed; 32]).unwrap()
    }

    fn fido_member(h: u8, seed: u8, class: CredentialClass) -> MemberEntry {
        MemberEntry {
            handle: [h; 16],
            kind: MemberKind::Fido2,
            class,
            alg: MemberAlg::Fido2Es256,
            public_key: Some(crate::roster::tests::p256_pub(seed)),
            rp_id: Some(RP.into()),
            credential_id: Some(vec![h; 8]),
            not_after: None,
        }
    }

    pub fn roster() -> Roster {
        let mut r = Roster::genesis([0xaa; 16], vec![ed_admin(1, 1)], 1).unwrap();
        r = r.successor().unwrap();
        r.members.push(phone(INTENT, 0x31, CredentialClass::Intent));
        let mut fob = phone(POSSESSION, 0x32, CredentialClass::Possession);
        fob.kind = MemberKind::Fob;
        r.members.push(fob);
        r.members
            .push(fido_member(FIDO_UV, 0x33, CredentialClass::Intent));
        r.members.push(card(CARD));
        r.sort();
        r.validate().unwrap();
        r
    }

    fn checker() -> Checker {
        let mut cards = CardStore::new();
        cards.insert([CARD; 16], [0xC0; 16]);
        Checker::new(
            ed25519_dalek::SigningKey::from_bytes(&[9; 32]),
            roster(),
            cards,
            0,
            posture::DEFAULT_HOME_TTL_SECS,
            Box::new(MemoryPersist::default()),
            T0,
        )
        .unwrap()
    }

    fn es256_req(
        c: &mut Checker,
        purpose: Purpose,
        h: u8,
        seed: u8,
        now_ms: u64,
    ) -> CommandRequest {
        let ch = c.issue_challenge(purpose, LAN_DOOR_ID, S, now_ms).unwrap();
        let sig: p256::ecdsa::Signature = pkey(seed).sign(&ch.msg);
        CommandRequest {
            challenge_id: ch.id,
            handle: [h; 16],
            signature: sig.to_der().as_bytes().to_vec(),
            authenticator_data: None,
        }
    }

    #[test]
    fn starts_in_away() {
        let c = checker();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        let st = posture::verify(
            &c.current_bytes(),
            &[0xaa; 16],
            &c.posture_public_key(),
            None,
            Some(T0),
        )
        .unwrap();
        assert_eq!(st.posture, HouseholdPosture::Away);
    }

    #[test]
    fn intent_sets_home_away_night() {
        let mut c = checker();
        for (p, want) in [
            (Purpose::Home, HouseholdPosture::Home),
            (Purpose::Night, HouseholdPosture::Night),
            (Purpose::Away, HouseholdPosture::Away),
        ] {
            let req = es256_req(&mut c, p, INTENT, 0x31, 0);
            let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
            assert_eq!(st.posture, want);
        }
    }

    #[test]
    fn possession_cannot_set_home() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, POSSESSION, 0x32, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        for p in [Purpose::Night, Purpose::Away] {
            let req = es256_req(&mut c, p, POSSESSION, 0x32, 0);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_ok());
        }
    }

    #[test]
    fn home_statement_lapses_to_away() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        assert_eq!(st.not_after, st.issued_at + posture::DEFAULT_HOME_TTL_SECS);
        c.tick(st.not_after - 1).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
        c.tick(st.not_after).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        assert!(c.current().seq > st.seq);
    }

    #[test]
    fn seq_strictly_increases_and_floor_survives_a_rollback() {
        let mut c = checker();
        let mut last = c.current().seq;
        for _ in 0..3 {
            let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
            let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
            assert!(st.seq > last);
            last = st.seq;
        }
        // A restored seq.json of 0 still issues above every earlier minute.
        assert!(next_seq(0, T0 + 60) > last);
        assert_eq!(next_seq(u64::MAX, T0), u64::MAX);
    }

    #[test]
    fn fido2_intent_needs_uv() {
        let mut c = checker();
        for (flags, ok) in [(FLAG_UP | FLAG_UV, true), (FLAG_UP, false)] {
            let ch = c
                .issue_challenge(Purpose::Night, LAN_DOOR_ID, S, 0)
                .unwrap();
            let cdh: [u8; 32] = Sha256::digest(&ch.msg).into();
            let (ad, sig) = crate::admin::soft_fido2_assert(&pkey(0x33), RP, flags, &cdh);
            let req = CommandRequest {
                challenge_id: ch.id,
                handle: [FIDO_UV; 16],
                signature: sig,
                authenticator_data: Some(ad),
            };
            assert_eq!(
                c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_ok(),
                ok,
                "flags {flags:#x}"
            );
        }
    }

    #[test]
    fn wrong_key_unknown_handle_and_replay_refused() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x99, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        let req = es256_req(&mut c, Purpose::Night, 0x77, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_ok());
        assert!(
            c.command(&req, &S, &LAN_DOOR_ID, 2, T0).is_err(),
            "replay accepted"
        );
    }

    #[test]
    fn cross_session_expiry_and_epoch_bump() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        assert!(
            c.command(&req, &[8; 32], &LAN_DOOR_ID, 1, T0).is_err(),
            "cross-session"
        );
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        assert!(
            c.command(&req, &S, &LAN_DOOR_ID, 10_000, T0).is_err(),
            "expired"
        );
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        let mut next = c.roster.successor().unwrap();
        next.members.retain(|m| m.handle != [POSSESSION; 16]);
        c.set_roster(next).unwrap();
        assert!(
            c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err(),
            "epoch bump kept a challenge"
        );
    }

    #[test]
    fn revoked_member_is_refused_at_the_current_epoch() {
        let mut c = checker();
        let mut next = c.roster.successor().unwrap();
        next.members.retain(|m| m.handle != [INTENT; 16]);
        c.set_roster(next).unwrap();
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
    }

    #[test]
    fn expired_member_is_refused() {
        let mut c = checker();
        let mut next = c.roster.successor().unwrap();
        for m in &mut next.members {
            if m.handle == [INTENT; 16] {
                m.not_after = Some(T0);
            }
        }
        // (An in-place edit is not a valid roster CHANGE, but set_roster
        // takes whatever the caller checked; here we test the checker.)
        c.set_roster(next).unwrap();
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
    }

    #[test]
    fn five_rejections_cool_the_door_and_not_another() {
        let mut c = checker();
        for i in 0..5 {
            let req = es256_req(&mut c, Purpose::Night, INTENT, 0x99, i);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, i, T0).is_err());
        }
        assert!(
            c.issue_challenge(Purpose::Night, LAN_DOOR_ID, S, 10)
                .is_err(),
            "cooling door issued"
        );
        assert!(
            c.issue_challenge(Purpose::Night, [5; 16], S, 10).is_ok(),
            "other door blocked"
        );
        assert!(c
            .issue_challenge(
                Purpose::Night,
                LAN_DOOR_ID,
                S,
                10 + crate::ratelimit::COOLDOWN_MS
            )
            .is_ok());
    }

    fn run_card(c: &mut Checker, purpose: Purpose, card: &mut CardSimulator) -> Result<Statement> {
        let door = [3u8; 16];
        let ch = c.issue_challenge(purpose, door, S, 0)?;
        let (mut sess, mut apdu) = c.start_card(&ch.id, &S, &door, 1, T0)?;
        for _ in 0..100 {
            let resp = card.transceive(&apdu);
            match c.card_response(&mut sess, &resp, 2, T0)? {
                CardProgress::Send(a) => apdu = a,
                CardProgress::Done(st) => return Ok(st),
            }
        }
        panic!("runaway card exchange")
    }

    #[test]
    fn card_sets_away_and_night_never_home() {
        let mut c = checker();
        let mut good = CardSimulator::new([0xC0; 16]);
        assert_eq!(
            run_card(&mut c, Purpose::Night, &mut good).unwrap().posture,
            HouseholdPosture::Night
        );
        assert_eq!(
            run_card(&mut c, Purpose::Away, &mut good).unwrap().posture,
            HouseholdPosture::Away
        );
        assert!(run_card(&mut c, Purpose::Home, &mut good).is_err());
        let mut stranger = CardSimulator::new([0xEE; 16]);
        assert!(run_card(&mut c, Purpose::Night, &mut stranger).is_err());
        assert_eq!(c.current().posture, HouseholdPosture::Away);
    }

    #[test]
    fn revoked_card_is_refused() {
        let mut c = checker();
        let mut next = c.roster.successor().unwrap();
        next.members.retain(|m| m.handle != [CARD; 16]);
        c.set_roster(next).unwrap();
        let mut good = CardSimulator::new([0xC0; 16]);
        assert!(run_card(&mut c, Purpose::Night, &mut good).is_err());
    }

    /// SENTINEL: a planted marker handle never appears in a statement, a
    /// published seam write, or the checker's Debug output, through every
    /// outcome (accepted, refused, lapsed).
    #[test]
    fn sentinel_handle_never_reaches_outputs() {
        const MARK: u8 = 0x5e;
        let mut r = roster();
        r.members.push(phone(MARK, 0x55, CredentialClass::Intent));
        r.sort();
        let published = std::sync::Arc::new(std::sync::Mutex::new(Vec::<Vec<u8>>::new()));
        struct Spy(std::sync::Arc<std::sync::Mutex<Vec<Vec<u8>>>>);
        impl Persist for Spy {
            fn save_seq(&mut self, s: u64) -> Result<()> {
                self.0.lock().unwrap().push(s.to_be_bytes().to_vec());
                Ok(())
            }
            fn publish(&mut self, b: &[u8; STATEMENT_LEN]) -> Result<()> {
                self.0.lock().unwrap().push(b.to_vec());
                Ok(())
            }
        }
        let mut c = Checker::new(
            ed25519_dalek::SigningKey::from_bytes(&[9; 32]),
            r,
            CardStore::new(),
            0,
            60,
            Box::new(Spy(published.clone())),
            T0,
        )
        .unwrap();
        let mut outs: Vec<Vec<u8>> = Vec::new();
        for p in [Purpose::Home, Purpose::Night, Purpose::Away] {
            let req = es256_req(&mut c, p, MARK, 0x55, 0);
            let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
            outs.push(st.sign(&c.posture_key).to_vec());
            let bad = es256_req(&mut c, p, MARK, 0x56, 0);
            let e = c.command(&bad, &S, &LAN_DOOR_ID, 1, T0).unwrap_err();
            outs.push(format!("{e} {e:?}").into_bytes());
        }
        c.tick(T0 + 3600).unwrap();
        outs.push(format!("{c:?}").into_bytes());
        outs.extend(published.lock().unwrap().iter().cloned());
        let raw = [MARK; 16];
        let hexm = crate::hex::encode(&raw);
        for o in &outs {
            assert!(!o.windows(16).any(|w| w == raw), "raw handle leaked");
            let s = String::from_utf8_lossy(o).to_ascii_lowercase();
            assert!(!s.contains(&hexm), "hex handle leaked: {s}");
        }
    }
}
