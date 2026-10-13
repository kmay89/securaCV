//! Challenges: OS-RNG nonces, 10-second TTL, single use, bound to the
//! session that asked, and all discarded on every roster epoch change.
//!
//! A challenge is taken out of the book on its first presentation, whatever
//! the outcome, so a nonce is never checked twice. Membership is checked by
//! the caller against the **current** roster when the response arrives; the
//! book refuses any challenge whose epoch is not the current one, and
//! [`ChallengeBook::set_epoch`] drops every outstanding challenge when the
//! epoch moves.

use crate::wire::{self, Purpose, MSG_LEN};
use crate::{rng, Error, Result};
use std::collections::HashMap;

/// How long a challenge is valid.
pub const CHALLENGE_TTL_MS: u64 = 10_000;
/// The `expires_in` the LAN API reports, in seconds.
pub const CHALLENGE_TTL_SECS: u64 = CHALLENGE_TTL_MS / 1000;
/// Most outstanding challenges overall.
pub const MAX_OUTSTANDING: usize = 64;
/// Most outstanding challenges per session.
pub const MAX_PER_SESSION: usize = 4;

/// An opaque session identifier: for the LAN API, the SHA-256 of the paired
/// bearer token; for a satellite, its authenticated session.
pub type SessionId = [u8; 32];
/// A challenge identifier (random, not the nonce).
pub type ChallengeId = [u8; 16];

/// An outstanding challenge.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Challenge {
    /// Random identifier handed to the client.
    pub id: ChallengeId,
    /// What it authorizes.
    pub purpose: Purpose,
    /// The door (zeros for the LAN API).
    pub door_id: [u8; 16],
    /// The roster epoch at issue.
    pub epoch: u64,
    /// The hub nonce.
    pub n_h: [u8; 32],
    /// The session that asked.
    pub session: SessionId,
    /// Monotonic milliseconds at issue.
    pub issued_ms: u64,
}

impl Challenge {
    /// The message a credential signs.
    pub fn msg(&self) -> [u8; MSG_LEN] {
        wire::build_msg(self.purpose, &self.door_id, self.epoch, &self.n_h)
    }

    fn expired(&self, now_ms: u64) -> bool {
        now_ms.saturating_sub(self.issued_ms) >= CHALLENGE_TTL_MS || now_ms < self.issued_ms
    }
}

/// The book of outstanding challenges.
#[derive(Debug, Default)]
pub struct ChallengeBook {
    pending: HashMap<ChallengeId, Challenge>,
    epoch: u64,
}

impl ChallengeBook {
    /// An empty book at `epoch`.
    pub fn new(epoch: u64) -> Self {
        ChallengeBook {
            pending: HashMap::new(),
            epoch,
        }
    }

    /// The epoch challenges are issued under.
    pub fn epoch(&self) -> u64 {
        self.epoch
    }

    /// Number of outstanding challenges.
    pub fn len(&self) -> usize {
        self.pending.len()
    }

    /// Whether no challenge is outstanding.
    pub fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }

    /// Move to a new epoch. Any change discards every outstanding challenge.
    pub fn set_epoch(&mut self, epoch: u64) {
        if epoch != self.epoch {
            self.pending.clear();
            self.epoch = epoch;
        }
    }

    fn prune(&mut self, now_ms: u64) {
        self.pending.retain(|_, c| !c.expired(now_ms));
    }

    /// Issue a challenge.
    pub fn issue(
        &mut self,
        purpose: Purpose,
        door_id: [u8; 16],
        session: SessionId,
        now_ms: u64,
    ) -> Result<Challenge> {
        self.prune(now_ms);
        if self.pending.len() >= MAX_OUTSTANDING {
            return Err(Error::Rejected("too many outstanding challenges"));
        }
        if self
            .pending
            .values()
            .filter(|c| c.session == session)
            .count()
            >= MAX_PER_SESSION
        {
            return Err(Error::Rejected(
                "too many outstanding challenges for this session",
            ));
        }
        let c = Challenge {
            id: rng::array()?,
            purpose,
            door_id,
            epoch: self.epoch,
            n_h: rng::array()?,
            session,
            issued_ms: now_ms,
        };
        self.pending.insert(c.id, c.clone());
        Ok(c)
    }

    /// Take a challenge for checking. It leaves the book whatever happens
    /// next (single use), then must be unexpired, from the same session and
    /// door, and issued under the current epoch.
    pub fn take(
        &mut self,
        id: &ChallengeId,
        session: &SessionId,
        door_id: &[u8; 16],
        now_ms: u64,
    ) -> Result<Challenge> {
        let c = self
            .pending
            .remove(id)
            .ok_or(Error::Rejected("unknown or used challenge"))?;
        if c.expired(now_ms) {
            return Err(Error::Rejected("challenge expired"));
        }
        if &c.session != session {
            return Err(Error::Rejected("challenge belongs to another session"));
        }
        if &c.door_id != door_id {
            return Err(Error::Rejected("challenge belongs to another door"));
        }
        if c.epoch != self.epoch {
            return Err(Error::Rejected("challenge is from an earlier epoch"));
        }
        Ok(c)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::LAN_DOOR_ID;

    const S1: SessionId = [1; 32];
    const S2: SessionId = [2; 32];

    #[test]
    fn single_use() {
        let mut b = ChallengeBook::new(5);
        let c = b.issue(Purpose::Home, LAN_DOOR_ID, S1, 0).unwrap();
        assert_eq!(c.msg()[15], 0x02);
        assert!(b.take(&c.id, &S1, &LAN_DOOR_ID, 1).is_ok());
        assert!(b.take(&c.id, &S1, &LAN_DOOR_ID, 2).is_err());
    }

    #[test]
    fn expiry() {
        let mut b = ChallengeBook::new(5);
        let c = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 1000).unwrap();
        assert!(b
            .take(&c.id, &S1, &LAN_DOOR_ID, 1000 + CHALLENGE_TTL_MS)
            .is_err());
        let c = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 1000).unwrap();
        assert!(b
            .take(&c.id, &S1, &LAN_DOOR_ID, 1000 + CHALLENGE_TTL_MS - 1)
            .is_ok());
        // A clock that runs backward does not resurrect anything.
        let c = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 5000).unwrap();
        assert!(b.take(&c.id, &S1, &LAN_DOOR_ID, 4000).is_err());
    }

    #[test]
    fn cross_session_is_refused_and_burns_the_challenge() {
        let mut b = ChallengeBook::new(5);
        let c = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 0).unwrap();
        assert!(b.take(&c.id, &S2, &LAN_DOOR_ID, 1).is_err());
        assert!(
            b.take(&c.id, &S1, &LAN_DOOR_ID, 2).is_err(),
            "challenge survived a wrong-session attempt"
        );
    }

    #[test]
    fn cross_door_is_refused() {
        let mut b = ChallengeBook::new(5);
        let c = b.issue(Purpose::Away, [9; 16], S1, 0).unwrap();
        assert!(b.take(&c.id, &S1, &LAN_DOOR_ID, 1).is_err());
    }

    #[test]
    fn epoch_bump_discards_everything() {
        let mut b = ChallengeBook::new(5);
        let c1 = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 0).unwrap();
        let c2 = b.issue(Purpose::Night, LAN_DOOR_ID, S2, 0).unwrap();
        b.set_epoch(5);
        assert_eq!(b.len(), 2, "same epoch must not discard");
        b.set_epoch(6);
        assert!(b.is_empty());
        assert!(b.take(&c1.id, &S1, &LAN_DOOR_ID, 1).is_err());
        assert!(b.take(&c2.id, &S2, &LAN_DOOR_ID, 1).is_err());
        let c3 = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 2).unwrap();
        assert_eq!(c3.epoch, 6);
    }

    #[test]
    fn nonces_and_ids_are_fresh() {
        let mut b = ChallengeBook::new(1);
        let a = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 0).unwrap();
        let c = b.issue(Purpose::Away, LAN_DOOR_ID, S1, 0).unwrap();
        assert_ne!(a.n_h, c.n_h);
        assert_ne!(a.id, c.id);
        assert_ne!(a.id[..], a.n_h[..16]);
    }

    #[test]
    fn outstanding_is_bounded() {
        let mut b = ChallengeBook::new(1);
        for _ in 0..MAX_PER_SESSION {
            b.issue(Purpose::Away, LAN_DOOR_ID, S1, 0).unwrap();
        }
        assert!(b.issue(Purpose::Away, LAN_DOOR_ID, S1, 0).is_err());
        assert!(b.issue(Purpose::Away, LAN_DOOR_ID, S2, 0).is_ok());
        // Expired ones are pruned and free their slots.
        assert!(b
            .issue(Purpose::Away, LAN_DOOR_ID, S1, CHALLENGE_TTL_MS)
            .is_ok());
        let mut b = ChallengeBook::new(1);
        for i in 0..MAX_OUTSTANDING {
            let mut s = [0u8; 32];
            s[..8].copy_from_slice(&(i as u64).to_be_bytes());
            b.issue(Purpose::Away, LAN_DOOR_ID, s, 0).unwrap();
        }
        assert!(b.issue(Purpose::Away, LAN_DOOR_ID, [0xff; 32], 0).is_err());
    }
}
