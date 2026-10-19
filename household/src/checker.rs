//! The checker: challenge → response → class/purpose policy → a new signed
//! household posture statement.
//!
//! The policy, in full:
//!
//! 1. **Nothing blocks raising sensing.** Five rejected responses on one
//!    lane of one door in ten minutes cool that lane for 60 seconds
//!    ([`crate::ratelimit`]), and a roster on disk that no longer verifies
//!    marks the checker's roster stale ([`Checker::mark_roster_stale`]).
//!    Either refuses `home`, the posture that lowers sensing, and nothing
//!    else on the signature lane: `away` and `night` still go through, so
//!    neither a cooldown nor a stale roster can hold a house in `home`. The
//!    card lane carries `away` and `night` only and stays under its
//!    cooldown. (Other denials of service against the LAN API are the
//!    server's to bound: [`crate::server`].)
//! 2. The challenge is taken (single use) and must be unexpired, from the
//!    same session and door, and from the current roster epoch.
//! 3. The handle must name a current member of the **current** roster.
//!    Every response that reaches the credential check with a DER signature
//!    runs exactly one P-256 verification, against a dummy key when the
//!    handle is unknown, expired, a card, or does not match the binding the
//!    request used, so the time taken does not say whether the handle exists.
//!    A signature that is not DER runs none, whatever the handle.
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
//!    `away`, on the wall clock or the monotonic clock, whichever comes
//!    first, and also ends a `home` once a revocation applies or the roster
//!    goes stale. An accepted `away` or `night` that changes nothing
//!    publishes nothing, so the seam does not reveal a tap that left the
//!    posture as it was, unless a failed publish left the seam unknown;
//!    `home` is always reissued, because that renews its lapse.
//!
//! The checker starts in `away`, the fail-safe, whatever it said before a
//! restart. It records nothing about who presented what: no counters, no
//! history, and the rejection limiter is keyed by door and lane only.

use crate::challenge::{ChallengeBook, ChallengeId, SessionId};
use crate::ntag424::{Ev2Auth, Ev2Step};
use crate::posture::{HouseholdPosture, Statement, HOME_MAX_SECS, STATEMENT_LEN};
use crate::ratelimit::{DoorLimiter, Lane};
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
    roster_stale: bool,
    /// A publish failed, so the seam may not hold `current`: the next
    /// command never takes the no-op shortcut, and [`Checker::tick`]
    /// republishes until a publish succeeds.
    seam_dirty: bool,
    /// When the current `home` lapses on the monotonic clock, so it lapses
    /// on time even if the wall clock is held back.
    home_deadline_ms: Option<u64>,
    /// End a current `home` at the next tick: a member was revoked, the
    /// roster on disk stopped verifying, or the `home` lapsed and the `away`
    /// that ends it has not published yet. Cleared only once no `home` is
    /// current (that `away` was published, or none was in force), or a new
    /// `home` is authorized under the roster now held.
    end_home: bool,
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
/// `minute(issued_at) << 10`, so a restored older `seq.json` cannot make the
/// hub reuse a sequence number from an earlier minute. It can reuse one from
/// the current minute: a restore and restart in the minute of earlier
/// statements, or a clock held or stepped back into it, issues the floor
/// again (`tests::next_seq_floor_collides_within_the_minute`); a witness
/// holding the higher `seq` refuses that statement, as `away`, until the
/// hub's `seq` passes it, which the next minute's floor guarantees. The
/// floor carries nothing beyond `issued_at`, which the statement already
/// states.
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
        // A dummy key nobody holds: drawn fresh, its private half dropped at
        // once. (A fixed seed would make its private key public.)
        let dummy = (0..8)
            .find_map(|_| {
                let seed = zeroize::Zeroizing::new(crate::rng::array::<32>().ok()?);
                p256::ecdsa::SigningKey::from_slice(&seed[..]).ok()
            })
            .ok_or(Error::Rng)?;
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
            roster_stale: false,
            seam_dirty: false,
            home_deadline_ms: None,
            end_home: false,
        };
        c.issue(HouseholdPosture::Away, now_unix, None)?;
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
    ///
    /// A roster that drops any member also ends a current `home` at the next
    /// [`Checker::tick`]: nothing records who set it, so a revoked member's
    /// `home` cannot be told from anyone else's, and ending it is the
    /// fail-safe. Members still enrolled set it again. A held handle whose
    /// entry differs counts as dropped: entries are never edited in place,
    /// so it was revoked and added again in epochs this checker skipped.
    pub fn set_roster(&mut self, roster: Roster) -> Result<()> {
        roster.validate()?;
        if roster.household != self.household {
            return Err(Error::Rejected("roster is for another household"));
        }
        if roster.epoch < self.roster.epoch {
            return Err(Error::Rejected("roster epoch went backward"));
        }
        let dropped = self
            .roster
            .members
            .iter()
            .any(|m| roster.member(&m.handle) != Some(m));
        self.book.set_epoch(roster.epoch);
        self.roster = roster;
        self.roster_stale = false;
        if dropped {
            self.end_home = true;
        }
        Ok(())
    }

    /// The roster on disk changed but did not load or verify. Until a roster
    /// that verifies is set, `home` is refused; `away` and `night` are still
    /// checked against the last roster that verified, because raising
    /// sensing is safe whoever asks (even a member the unreadable roster may
    /// have revoked). A current `home` ends at the next [`Checker::tick`],
    /// since the change that did not verify may have revoked whoever set it.
    pub fn mark_roster_stale(&mut self) {
        if !self.roster_stale {
            self.end_home = true;
        }
        self.roster_stale = true;
    }

    /// Whether `home` is refused because the roster on disk did not verify.
    pub fn roster_is_stale(&self) -> bool {
        self.roster_stale
    }

    /// Replace the card keys.
    pub fn set_cards(&mut self, cards: CardStore) {
        self.cards = cards;
    }

    /// Issue, persist and publish a statement. `keep_lapse` caps a `home`'s
    /// `not_after` (a republish never extends a `home`).
    fn issue(
        &mut self,
        posture: HouseholdPosture,
        now_unix: u64,
        keep_lapse: Option<u64>,
    ) -> Result<Statement> {
        let floored = now_unix - now_unix % 60;
        let seq = next_seq(self.last_seq, floored);
        let mut st = Statement::issue(posture, self.household, seq, now_unix, self.home_ttl_secs)?;
        if let Some(cap) = keep_lapse {
            st.not_after = st.not_after.min(cap);
        }
        self.persist.save_seq(seq)?;
        self.last_seq = seq;
        let bytes = st.sign(&self.posture_key);
        if let Err(e) = self.persist.publish(&bytes) {
            // The publish may have landed (a rename that succeeded before a
            // directory sync failed): the seam may now differ from `current`.
            self.seam_dirty = true;
            return Err(e);
        }
        self.seam_dirty = false;
        if posture != HouseholdPosture::Home {
            self.home_deadline_ms = None;
        }
        self.current = st;
        self.current_bytes = bytes;
        Ok(st)
    }

    /// Call periodically (the server does, every second). In order:
    ///
    /// - a `home` that is no longer current, or past its monotonic deadline,
    ///   or that a revocation or a stale roster ended, becomes `away` (if
    ///   that publish fails, the next tick tries the `away` again; the
    ///   ended `home` is never republished).
    ///   "Current" is [`Statement::is_current`] on the hub's own clock, so a
    ///   `home` also lapses at once if that clock steps back more than
    ///   [`crate::posture::MAX_SKEW_SECS`] behind its `issued_at`; the
    ///   monotonic deadline makes it lapse on time even if the wall clock is
    ///   held back;
    /// - otherwise the current statement is republished, without extending a
    ///   `home`, when a publish failed (the seam may differ from it) or when
    ///   its `issued_at` is ahead of the hub's corrected clock (every witness
    ///   with a correct clock refuses it, rule 7).
    pub fn tick(&mut self, now_unix: u64, now_ms: u64) -> Result<()> {
        let cur = self.current;
        if cur.posture == HouseholdPosture::Home
            && (self.end_home
                || !cur.is_current(now_unix)
                || self.home_deadline_ms.is_some_and(|d| now_ms >= d))
        {
            // Latch the end: if this publish fails, the next tick tries the
            // `away` again rather than republishing the `home` from the
            // dirty seam below, even if the wall clock steps back into the
            // `home`'s window.
            self.end_home = true;
            self.issue(HouseholdPosture::Away, now_unix, None)?;
            self.end_home = false;
            return Ok(());
        }
        self.end_home = false;
        let from_future = cur.issued_at > now_unix.saturating_add(crate::posture::MAX_SKEW_SECS);
        if self.seam_dirty || from_future {
            let keep = (cur.posture == HouseholdPosture::Home).then_some(cur.not_after);
            self.issue(cur.posture, now_unix, keep)?;
        }
        Ok(())
    }

    /// Whether `lane` at `door` is cooling down.
    pub fn is_cooling(&mut self, door: &[u8; 16], lane: Lane, now_ms: u64) -> bool {
        self.limiter.is_cooling(door, lane, now_ms)
    }

    /// Why `home` is refused right now at `door`, if it is (policy rule 1).
    fn home_refusal(&mut self, door: &[u8; 16], now_ms: u64) -> Option<Error> {
        if self.limiter.is_cooling(door, Lane::Signature, now_ms) {
            return Some(Error::Rejected("door is cooling down"));
        }
        if self.roster_stale {
            return Some(Error::Rejected("the roster on disk did not verify"));
        }
        None
    }

    /// Issue a challenge for `purpose` at `door`, bound to `session`.
    pub fn issue_challenge(
        &mut self,
        purpose: Purpose,
        door: [u8; 16],
        session: SessionId,
        now_ms: u64,
    ) -> Result<IssuedChallenge> {
        if purpose == Purpose::Home {
            if let Some(e) = self.home_refusal(&door, now_ms) {
                return Err(e);
            }
        }
        let c = self.book.issue(purpose, door, session, now_ms)?;
        Ok(IssuedChallenge {
            id: c.id,
            msg: c.msg().to_vec(),
        })
    }

    /// Check a response against the member its handle names, if any.
    ///
    /// Every path runs exactly one P-256 verification over input of the same
    /// shape (when the signature is DER; one that is not runs none, whatever
    /// the handle): against the member's key when the member is current, is
    /// not a card, and every check but the signature passes for the binding
    /// the request used; against the dummy key otherwise. The result is read
    /// only after that verification, so the time taken does not say whether
    /// the handle exists, is current, or is enrolled with another binding.
    fn check_response(
        &self,
        member: Option<&MemberEntry>,
        msg: &[u8],
        req: &CommandRequest,
        now_unix: u64,
    ) -> bool {
        let candidate = member.filter(|m| m.is_current(now_unix) && m.kind != MemberKind::Card);
        let (signed, shape_ok) = match &req.authenticator_data {
            None => (
                msg.to_vec(),
                candidate.is_some_and(|m| m.alg == MemberAlg::Es256),
            ),
            Some(ad) => {
                let cdh: [u8; 32] = Sha256::digest(msg).into();
                let fido = candidate.filter(|m| m.alg == MemberAlg::Fido2Es256);
                let rp = fido.and_then(|m| m.rp_id.as_deref()).unwrap_or("");
                let rp_hash: [u8; 32] = Sha256::digest(rp.as_bytes()).into();
                let auth_ok = verify::fido2_auth_data_ok(
                    &rp_hash,
                    ad,
                    fido.is_some_and(|m| m.class == CredentialClass::Intent),
                );
                (
                    verify::fido2_signed_bytes(ad, &cdh),
                    fido.is_some_and(|m| m.rp_id.is_some()) && auth_ok,
                )
            }
        };
        let member_key = candidate.and_then(|m| m.public_key.as_deref());
        let shape_ok = shape_ok && member_key.is_some();
        let key = match member_key {
            Some(pk) if shape_ok => pk,
            _ => &self.dummy_key[..],
        };
        let sig_ok = verify::es256(key, &signed, &req.signature);
        // The dummy key's verdict is never read: only a member's key can
        // authorize (and nobody holds the dummy's private half anyway).
        shape_ok && sig_ok
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
        let ch = match self.book.take(&req.challenge_id, session, door, now_ms) {
            Ok(ch) => ch,
            Err(e) => {
                self.limiter.record_rejection(door, Lane::Signature, now_ms);
                return Err(e);
            }
        };
        if ch.purpose == Purpose::Home {
            // Refusals for a cooldown or a stale roster are not counted: they
            // say nothing about the credential.
            if let Some(e) = self.home_refusal(door, now_ms) {
                return Err(e);
            }
        }
        let member = self.roster.member(&req.handle).cloned();
        let refusal = if self.check_response(member.as_ref(), &ch.msg(), req, now_unix) {
            match member {
                Some(m) if ch.purpose == Purpose::Home && m.class != CredentialClass::Intent => {
                    Some(Error::Rejected("a possession credential cannot set home"))
                }
                Some(_) => None,
                None => Some(Error::Rejected("credential refused")),
            }
        } else {
            Some(Error::Rejected("credential refused"))
        };
        if let Some(e) = refusal {
            self.limiter.record_rejection(door, Lane::Signature, now_ms);
            return Err(e);
        }
        // Accepted: a storage error from here on says nothing about the
        // credential, so it is not counted toward a cooldown.
        self.apply(ch.purpose, now_unix, now_ms)
    }

    /// Carry out an accepted purpose: reissue `home` (renewing its lapse);
    /// publish nothing for an `away` or `night` that changes nothing, unless
    /// a failed publish left the seam unknown (then it is rewritten).
    fn apply(&mut self, purpose: Purpose, now_unix: u64, now_ms: u64) -> Result<Statement> {
        let target = purpose.target();
        if target == self.current.posture && target != HouseholdPosture::Home && !self.seam_dirty {
            return Ok(self.current);
        }
        let st = self.issue(target, now_unix, None)?;
        if target == HouseholdPosture::Home {
            self.home_deadline_ms =
                Some(now_ms.saturating_add(self.home_ttl_secs.saturating_mul(1000)));
            // This `home` was authorized under the roster now held (a stale
            // roster refuses `home`), so a pending end was for the one it
            // replaced.
            self.end_home = false;
        }
        Ok(st)
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
        if self.limiter.is_cooling(door, Lane::Card, now_ms) {
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
            self.limiter.record_rejection(door, Lane::Card, now_ms);
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
        enum Next {
            Send(Vec<u8>),
            Apply(Purpose),
        }
        let door = s.challenge.door_id;
        let r = (|| match s.auth.on_response(resp) {
            Ev2Step::Send(apdu) => Ok(Next::Send(apdu)),
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
                Ok(Next::Apply(ch.purpose))
            }
        })();
        match r {
            Err(e) => {
                self.limiter.record_rejection(&door, Lane::Card, now_ms);
                Err(e)
            }
            Ok(Next::Send(apdu)) => Ok(CardProgress::Send(apdu)),
            // Accepted: a storage error from here on says nothing about the
            // card, so it is not counted toward a cooldown.
            Ok(Next::Apply(purpose)) => {
                Ok(CardProgress::Done(self.apply(purpose, now_unix, now_ms)?))
            }
        }
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
        c.tick(st.not_after - 1, 2).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
        c.tick(st.not_after, 2).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        assert!(c.current().seq > st.seq);
    }

    #[test]
    fn seq_strictly_increases_and_floor_survives_a_rollback() {
        let mut c = checker();
        let mut last = c.current().seq;
        for p in [Purpose::Night, Purpose::Away, Purpose::Night] {
            let req = es256_req(&mut c, p, INTENT, 0x31, 0);
            let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
            assert!(st.seq > last);
            last = st.seq;
        }
        // A restored seq.json of 0 still issues above every earlier minute.
        assert!(next_seq(0, T0 + 60) > last);
        assert_eq!(next_seq(u64::MAX, T0), u64::MAX);
    }

    #[test]
    fn next_seq_floor_collides_within_the_minute() {
        // The floor stops reuse across minutes, not within one: a seq.json
        // restored to before this minute's statements, or a clock held or
        // stepped back into their minute, issues the floor again. Pinned so
        // the spec (§3.1) and the README claim exactly that and no more.
        let floor = (T0 / 60) * 1024;
        let mut c = checker();
        let mut issued = vec![c.current().seq];
        for p in [Purpose::Night, Purpose::Away] {
            let req = es256_req(&mut c, p, INTENT, 0x31, 0);
            issued.push(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap().seq);
        }
        assert_eq!(issued, [floor, floor + 1, floor + 2]);
        // seq.json restored to anything below the floor, in the same minute:
        // the first statement's seq again. Restored to the first's: the
        // second's.
        assert_eq!(next_seq(0, T0), issued[0]);
        assert_eq!(next_seq(floor - 1, T0), issued[0]);
        assert_eq!(next_seq(issued[0], T0), issued[1]);
        // The checker restarted from that seq.json publishes the collision,
        // and a witness holding the newest seq refuses it (§3.2 rule 4): away
        // until the hub's seq passes what the witness holds.
        let restarted = checker();
        assert_eq!(restarted.current().seq, issued[0]);
        assert_eq!(
            posture::verify(
                &restarted.current_bytes(),
                &[0xaa; 16],
                &c.posture_public_key(),
                Some(issued[2]),
                Some(T0)
            ),
            Err(posture::Reject::Seq)
        );
        // The next minute's floor is above every one of them.
        assert!(next_seq(0, T0 + 60) > issued[2]);
        assert!(next_seq(floor - 1, T0 + 60) > issued[2]);
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
    fn five_rejections_cool_home_at_the_door_and_not_another() {
        let mut c = checker();
        for i in 0..5 {
            let req = es256_req(&mut c, Purpose::Night, INTENT, 0x99, i);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, i, T0).is_err());
        }
        assert!(
            c.issue_challenge(Purpose::Home, LAN_DOOR_ID, S, 10)
                .is_err(),
            "cooling door issued a home challenge"
        );
        assert!(
            c.issue_challenge(Purpose::Home, [5; 16], S, 10).is_ok(),
            "other door blocked"
        );
        assert!(c
            .issue_challenge(
                Purpose::Home,
                LAN_DOOR_ID,
                S,
                10 + crate::ratelimit::COOLDOWN_MS
            )
            .is_ok());
    }

    #[test]
    fn a_cooldown_never_blocks_away_or_night() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        for i in 0..5 {
            let req = es256_req(&mut c, Purpose::Night, INTENT, 0x99, i);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, i, T0).is_err());
        }
        assert!(c.is_cooling(&LAN_DOOR_ID, Lane::Signature, 10));
        // The household leaves: away and night still work while cooling...
        let req = es256_req(&mut c, Purpose::Night, POSSESSION, 0x32, 10);
        assert_eq!(
            c.command(&req, &S, &LAN_DOOR_ID, 11, T0).unwrap().posture,
            HouseholdPosture::Night
        );
        let req = es256_req(&mut c, Purpose::Away, INTENT, 0x31, 12);
        assert_eq!(
            c.command(&req, &S, &LAN_DOOR_ID, 13, T0).unwrap().posture,
            HouseholdPosture::Away
        );
        // ...and home does not, even with a good intent credential.
        assert!(c
            .issue_challenge(Purpose::Home, LAN_DOOR_ID, S, 14)
            .is_err());
        assert_eq!(c.current().posture, HouseholdPosture::Away);
    }

    #[test]
    fn a_stale_roster_refuses_home_and_nothing_else() {
        let mut c = checker();
        c.mark_roster_stale();
        assert!(c.roster_is_stale());
        assert!(c.issue_challenge(Purpose::Home, LAN_DOOR_ID, S, 0).is_err());
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_ok());
        // A home challenge issued before the roster went stale is refused
        // when answered, and the refusal is not counted.
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.mark_roster_stale();
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        // A roster that verifies clears it.
        let next = c.roster.successor().unwrap();
        c.set_roster(next).unwrap();
        assert!(!c.roster_is_stale());
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_ok());
    }

    #[test]
    fn an_away_that_changes_nothing_publishes_nothing() {
        let mut c = checker();
        let before = c.current_bytes();
        for p in [Purpose::Away, Purpose::Away] {
            let req = es256_req(&mut c, p, INTENT, 0x31, 0);
            let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0 + 600).unwrap();
            assert_eq!(st.posture, HouseholdPosture::Away);
        }
        assert_eq!(c.current_bytes(), before, "a no-op away was published");
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        let night = c.command(&req, &S, &LAN_DOOR_ID, 1, T0 + 600).unwrap();
        let night_bytes = c.current_bytes();
        let req = es256_req(&mut c, Purpose::Night, POSSESSION, 0x32, 0);
        assert_eq!(
            c.command(&req, &S, &LAN_DOOR_ID, 1, T0 + 1200).unwrap(),
            night
        );
        assert_eq!(c.current_bytes(), night_bytes);
        // Home is always reissued: that renews its lapse.
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        let h1 = c.command(&req, &S, &LAN_DOOR_ID, 1, T0 + 1200).unwrap();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        let h2 = c.command(&req, &S, &LAN_DOOR_ID, 1, T0 + 1800).unwrap();
        assert!(h2.seq > h1.seq && h2.not_after > h1.not_after);
    }

    #[test]
    fn every_refusal_runs_exactly_one_p256_verification() {
        use crate::verify::ES256_RUNS;
        let mut c = checker();
        // An expired member, and a card handle.
        let mut next = c.roster.successor().unwrap();
        next.members
            .push(phone(0x20, 0x34, CredentialClass::Intent));
        for m in &mut next.members {
            if m.handle == [0x20; 16] {
                m.not_after = Some(T0);
            }
        }
        next.sort();
        c.set_roster(next).unwrap();
        let fido_ad = |rp: &str, flags: u8| crate::verify::build_auth_data(rp, flags, 1);
        // (what, handle byte, signing-key seed, authenticator data, accepted)
        type Case = (&'static str, u8, u8, Option<Vec<u8>>, bool);
        let cases: Vec<Case> = vec![
            ("current es256", INTENT, 0x31, None, true),
            ("wrong key", INTENT, 0x44, None, false),
            ("unknown handle", 0x77, 0x31, None, false),
            ("expired member", 0x20, 0x34, None, false),
            ("card handle", CARD, 0x31, None, false),
            (
                "es256 member sent authenticator data",
                INTENT,
                0x31,
                Some(fido_ad(RP, FLAG_UP | FLAG_UV)),
                false,
            ),
            (
                "fido2 member without authenticator data",
                FIDO_UV,
                0x33,
                None,
                false,
            ),
            (
                "fido2 member, wrong rp",
                FIDO_UV,
                0x33,
                Some(fido_ad("elsewhere.example", FLAG_UP | FLAG_UV)),
                false,
            ),
            (
                "fido2 intent member without UV",
                FIDO_UV,
                0x33,
                Some(fido_ad(RP, FLAG_UP)),
                false,
            ),
            (
                "unparsable authenticator data",
                FIDO_UV,
                0x33,
                Some(vec![1, 2, 3]),
                false,
            ),
            (
                "unknown handle sent authenticator data",
                0x77,
                0x33,
                Some(fido_ad(RP, FLAG_UP | FLAG_UV)),
                false,
            ),
            (
                "expired member sent authenticator data",
                0x20,
                0x34,
                Some(fido_ad(RP, FLAG_UP | FLAG_UV)),
                false,
            ),
            (
                "card handle sent authenticator data",
                CARD,
                0x33,
                Some(fido_ad(RP, FLAG_UP | FLAG_UV)),
                false,
            ),
        ];
        for (name, h, seed, ad, want) in cases {
            let ch = c
                .issue_challenge(Purpose::Night, LAN_DOOR_ID, S, 0)
                .unwrap();
            let signed = match &ad {
                None => ch.msg.clone(),
                Some(ad) => {
                    let cdh: [u8; 32] = Sha256::digest(&ch.msg).into();
                    crate::verify::fido2_signed_bytes(ad, &cdh)
                }
            };
            let sig: p256::ecdsa::Signature = pkey(seed).sign(&signed);
            let req = CommandRequest {
                challenge_id: ch.id,
                handle: [h; 16],
                signature: sig.to_der().as_bytes().to_vec(),
                authenticator_data: ad,
            };
            let before = ES256_RUNS.with(|r| r.get());
            let ok = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_ok();
            let runs = ES256_RUNS.with(|r| r.get()) - before;
            assert_eq!(ok, want, "{name}");
            assert_eq!(runs, 1, "{name}: {runs} P-256 verifications");
            // Keep the door out of cooldown (it would not change the count,
            // but it keeps the cases independent).
            c.limiter = DoorLimiter::new();
        }
    }

    #[test]
    fn home_lapses_when_the_hub_clock_steps_back() {
        // Issued while the hub's clock ran a year fast; the clock is then
        // corrected. The next tick replaces it with away.
        let mut c = checker();
        let ahead = T0 + 365 * 86_400;
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        let st = c.command(&req, &S, &LAN_DOOR_ID, 1, ahead).unwrap();
        assert_eq!(st.posture, HouseholdPosture::Home);
        c.tick(T0, 2).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        // And a witness with a correct clock never accepted it.
        let b = st.sign(&c.posture_key);
        assert_eq!(
            posture::verify(&b, &[0xaa; 16], &c.posture_public_key(), None, Some(T0)),
            Err(posture::Reject::FromFuture { seq: st.seq })
        );
        // A small step back inside the skew keeps a home that is current.
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        let st = c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        c.tick(st.issued_at - 60, 2).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
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

    /// A Persist that counts writes, can fail the next publish after it has
    /// "landed" (the rename succeeded, the directory sync did not), and
    /// shares what it holds with the test.
    #[derive(Clone, Default)]
    struct Probe(std::sync::Arc<std::sync::Mutex<ProbeState>>);
    #[derive(Default)]
    struct ProbeState {
        seam: Option<[u8; STATEMENT_LEN]>,
        seq_writes: usize,
        publishes: usize,
        fail_next_publish: bool,
        fail_save_seq: bool,
    }
    impl Persist for Probe {
        fn save_seq(&mut self, _: u64) -> Result<()> {
            let mut st = self.0.lock().unwrap();
            if st.fail_save_seq {
                return Err(Error::Invalid("the disk is full"));
            }
            st.seq_writes += 1;
            Ok(())
        }
        fn publish(&mut self, b: &[u8; STATEMENT_LEN]) -> Result<()> {
            let mut st = self.0.lock().unwrap();
            st.seam = Some(*b);
            st.publishes += 1;
            if std::mem::take(&mut st.fail_next_publish) {
                return Err(Error::Invalid("the directory sync failed after the rename"));
            }
            Ok(())
        }
    }

    fn probed() -> (Checker, Probe) {
        let probe = Probe::default();
        let mut cards = CardStore::new();
        cards.insert([CARD; 16], [0xC0; 16]);
        let c = Checker::new(
            ed25519_dalek::SigningKey::from_bytes(&[9; 32]),
            roster(),
            cards,
            0,
            posture::DEFAULT_HOME_TTL_SECS,
            Box::new(probe.clone()),
            T0,
        )
        .unwrap();
        (c, probe)
    }

    #[test]
    fn a_no_op_writes_neither_the_seam_nor_seq_json() {
        let (mut c, p) = probed();
        let (seqs, pubs) = {
            let s = p.0.lock().unwrap();
            (s.seq_writes, s.publishes)
        };
        let req = es256_req(&mut c, Purpose::Away, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1, T0 + 600).unwrap();
        let s = p.0.lock().unwrap();
        assert_eq!((s.seq_writes, s.publishes), (seqs, pubs), "a no-op wrote");
    }

    #[test]
    fn a_failed_publish_never_lets_a_no_op_leave_home_on_the_seam() {
        // The verified regression: home's publish lands but reports failure;
        // memory still says away, so an away looked like a no-op and left the
        // seam saying home until it lapsed.
        let (mut c, p) = probed();
        p.0.lock().unwrap().fail_next_publish = true;
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        let seam = |p: &Probe| {
            posture::decode_structure(&p.0.lock().unwrap().seam.unwrap())
                .unwrap()
                .posture
        };
        assert_eq!(seam(&p), HouseholdPosture::Home, "the probe did not land");
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        let req = es256_req(&mut c, Purpose::Away, POSSESSION, 0x32, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        assert_eq!(
            seam(&p),
            HouseholdPosture::Away,
            "away left home on the seam"
        );
        // And with no command at all, the next tick heals it.
        let (mut c, p) = probed();
        p.0.lock().unwrap().fail_next_publish = true;
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        c.tick(T0, 2).unwrap();
        assert_eq!(seam(&p), HouseholdPosture::Away);
        // The card lane takes the same path.
        let (mut c, p) = probed();
        p.0.lock().unwrap().fail_next_publish = true;
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
        let mut good = CardSimulator::new([0xC0; 16]);
        run_card(&mut c, Purpose::Away, &mut good).unwrap();
        assert_eq!(seam(&p), HouseholdPosture::Away);
    }

    #[test]
    fn a_card_tap_that_changes_nothing_publishes_nothing() {
        let (mut c, p) = probed();
        let before = p.0.lock().unwrap().publishes;
        let mut good = CardSimulator::new([0xC0; 16]);
        run_card(&mut c, Purpose::Away, &mut good).unwrap();
        assert_eq!(p.0.lock().unwrap().publishes, before);
    }

    #[test]
    fn home_lapses_on_the_monotonic_clock_when_the_wall_clock_is_held() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1_000, T0).unwrap();
        let ttl_ms = posture::DEFAULT_HOME_TTL_SECS * 1000;
        // The wall clock never moves (held by a spoofed time source).
        c.tick(T0, 1_000 + ttl_ms - 1).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
        c.tick(T0, 1_000 + ttl_ms).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
    }

    #[test]
    fn a_revocation_or_a_stale_roster_ends_home() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        let mut next = c.roster.successor().unwrap();
        next.members.retain(|m| m.handle != [POSSESSION; 16]);
        c.set_roster(next).unwrap();
        c.tick(T0, 2).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
        // A change that adds only does not end it.
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 3, T0).unwrap();
        let mut next = c.roster.successor().unwrap();
        next.members
            .push(phone(0x40, 0x41, CredentialClass::Intent));
        next.sort();
        c.set_roster(next).unwrap();
        c.tick(T0, 4).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
        c.mark_roster_stale();
        c.tick(T0, 5).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
    }

    #[test]
    fn an_ended_home_stays_ended_when_the_away_fails_to_publish() {
        // The verified hole: the end was taken before the away published,
        // so after a failed publish the dirty-seam republish put the home
        // back on the seam until it lapsed.
        let seam = |p: &Probe| {
            posture::decode_structure(&p.0.lock().unwrap().seam.unwrap())
                .unwrap()
                .posture
        };
        for revoke in [true, false] {
            let (mut c, p) = probed();
            let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
            c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
            if revoke {
                let mut next = c.roster.successor().unwrap();
                next.members.retain(|m| m.handle != [POSSESSION; 16]);
                c.set_roster(next).unwrap();
            } else {
                c.mark_roster_stale();
            }
            p.0.lock().unwrap().fail_next_publish = true;
            assert!(c.tick(T0, 2).is_err());
            assert_eq!(c.current().posture, HouseholdPosture::Home);
            c.tick(T0, 3).unwrap();
            assert_eq!(c.current().posture, HouseholdPosture::Away);
            assert_eq!(seam(&p), HouseholdPosture::Away, "the ended home came back");
            c.tick(T0, 4).unwrap();
            assert_eq!(seam(&p), HouseholdPosture::Away);
        }
    }

    #[test]
    fn a_lapsed_home_stays_ended_when_the_away_fails_to_publish() {
        let seam = |p: &Probe| {
            posture::decode_structure(&p.0.lock().unwrap().seam.unwrap())
                .unwrap()
                .posture
        };
        let ttl = posture::DEFAULT_HOME_TTL_SECS;
        // The wall clock runs past the lapse, then is set back into the
        // home's window; or it steps back so far that the home is from the
        // future. Either way the failed away is tried again, and the home
        // is never republished.
        for lapse_at in [T0 + ttl + 600, T0 - 3600] {
            let (mut c, p) = probed();
            let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
            c.command(&req, &S, &LAN_DOOR_ID, 1_000, T0).unwrap();
            p.0.lock().unwrap().fail_next_publish = true;
            assert!(c.tick(lapse_at, 2_000).is_err());
            c.tick(T0 + 60, 3_000).unwrap();
            assert_eq!(c.current().posture, HouseholdPosture::Away);
            assert_eq!(
                seam(&p),
                HouseholdPosture::Away,
                "the lapsed home came back"
            );
        }
    }

    #[test]
    fn a_storage_error_after_an_accepted_credential_is_not_counted() {
        let (mut c, p) = probed();
        p.0.lock().unwrap().fail_save_seq = true;
        for i in 0..6 {
            let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, i);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, i, T0).is_err());
        }
        p.0.lock().unwrap().fail_save_seq = false;
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 9);
        c.command(&req, &S, &LAN_DOOR_ID, 9, T0).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
        // The card lane too.
        let (mut c, p) = probed();
        let mut good = CardSimulator::new([0xC0; 16]);
        for _ in 0..6 {
            p.0.lock().unwrap().fail_next_publish = true;
            assert!(run_card(&mut c, Purpose::Night, &mut good).is_err());
        }
        run_card(&mut c, Purpose::Night, &mut good).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Night);
    }

    #[test]
    fn a_handle_revoked_and_added_again_in_skipped_epochs_ends_home() {
        let mut c = checker();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        let mut revoked = c.roster.successor().unwrap();
        revoked.members.retain(|m| m.handle != [INTENT; 16]);
        let mut readded = revoked.successor().unwrap();
        readded
            .members
            .push(phone(INTENT, 0x44, CredentialClass::Intent));
        readded.sort();
        // The checker sees only the newest roster: the handle is held, its
        // entry is not.
        c.set_roster(readded).unwrap();
        c.tick(T0, 2).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Away);
    }

    #[test]
    fn a_home_set_after_a_revocation_is_not_ended_by_it() {
        let (mut c, p) = probed();
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 1, T0).unwrap();
        let mut next = c.roster.successor().unwrap();
        next.members.retain(|m| m.handle != [POSSESSION; 16]);
        c.set_roster(next).unwrap();
        // The away that ends it fails, so the end is still pending...
        p.0.lock().unwrap().fail_next_publish = true;
        assert!(c.tick(T0, 2).is_err());
        // ...when a member still enrolled sets home under the new roster.
        let req = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        c.command(&req, &S, &LAN_DOOR_ID, 3, T0).unwrap();
        c.tick(T0, 4).unwrap();
        assert_eq!(c.current().posture, HouseholdPosture::Home);
    }

    #[test]
    fn a_future_dated_away_is_reissued_once_the_clock_is_corrected() {
        let mut c = checker();
        let ahead = T0 + 86_400;
        let req = es256_req(&mut c, Purpose::Night, INTENT, 0x31, 0);
        let st = c.command(&req, &S, &LAN_DOOR_ID, 1, ahead).unwrap();
        c.tick(T0, 2).unwrap();
        let now = c.current();
        assert_eq!(now.posture, HouseholdPosture::Night);
        assert!(now.seq > st.seq && now.issued_at <= T0);
        assert!(posture::verify(
            &c.current_bytes(),
            &[0xaa; 16],
            &c.posture_public_key(),
            Some(st.seq),
            Some(T0)
        )
        .is_ok());
    }

    #[test]
    fn a_cooldown_refuses_home_at_the_command_too_and_does_not_count_it() {
        let mut c = checker();
        // A home challenge issued before the door cooled...
        let home = es256_req(&mut c, Purpose::Home, INTENT, 0x31, 0);
        for i in 0..5 {
            let req = es256_req(&mut c, Purpose::Night, INTENT, 0x99, i);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, i, T0).is_err());
        }
        // ...is refused when answered during the cooldown.
        assert!(c.command(&home, &S, &LAN_DOOR_ID, 10, T0).is_err());
        assert_eq!(c.current().posture, HouseholdPosture::Away);
    }

    #[test]
    fn a_member_without_a_key_never_authorizes() {
        // Validation forbids it; the checker does not rely on that.
        let mut c = checker();
        for m in &mut c.roster.members {
            if m.handle == [INTENT; 16] {
                m.public_key = None;
            }
        }
        for seed in [0x31, 0x01] {
            let req = es256_req(&mut c, Purpose::Night, INTENT, seed, 0);
            assert!(c.command(&req, &S, &LAN_DOOR_ID, 1, T0).is_err());
            c.limiter = DoorLimiter::new();
        }
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
        c.tick(T0 + 3600, 2).unwrap();
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
