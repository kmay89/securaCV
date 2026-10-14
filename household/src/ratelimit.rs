//! The per-door rejection limiter.
//!
//! Five rejected responses on one lane of one door within ten minutes start a
//! 60-second cooldown for that lane. Rejections are counted in RAM only,
//! keyed by door id and lane, and **never per credential or per handle**: the
//! limiter holds no state that could become a per-member usage tally. The
//! LAN API is one "door" (`door_id` = sixteen zero bytes).
//!
//! There are two lanes per door ([`Lane`]), so junk taps on a card reader
//! never cool down the phones answering at the same door, and the other way
//! round. Only a response to a live challenge counts: a missing or wrong
//! bearer token, or a body that does not parse, never reaches the limiter, so
//! a client with no token cannot start a cooldown (spec §4.3).
//!
//! A cooldown never blocks raising sensing. The checker lets `away` and
//! `night` through a cooling signature lane and refuses only `home`
//! ([`crate::checker`]); the card lane carries `away` and `night` only, and
//! stays under the cooldown because every trial `AuthenticateEV2First`
//! touches a real card's failed-authentication counter.

use std::collections::{HashMap, VecDeque};

/// Rejections that trigger a cooldown.
pub const MAX_REJECTIONS: usize = 5;
/// The window those rejections are counted in.
pub const WINDOW_MS: u64 = 10 * 60 * 1000;
/// The cooldown.
pub const COOLDOWN_MS: u64 = 60 * 1000;
/// Most (door, lane) entries tracked at once (the oldest is dropped beyond
/// this).
pub const MAX_DOORS: usize = 64;

/// Which way a response arrived at a door.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Lane {
    /// A signature: `es256` or `fido2-es256` (a phone, watch, fob or FIDO2
    /// key, over the LAN API or relayed by a satellite).
    Signature,
    /// An NTAG 424 DNA card authentication.
    Card,
}

#[derive(Debug, Default)]
struct DoorState {
    rejections: VecDeque<u64>,
    cooldown_until: Option<u64>,
    last_touch: u64,
}

impl DoorState {
    /// Forget a cooldown that has ended, and the count that started it.
    fn expire(&mut self, now_ms: u64) {
        if self.cooldown_until.is_some_and(|t| now_ms >= t) {
            self.cooldown_until = None;
            self.rejections.clear();
        }
    }
}

/// The limiter.
#[derive(Debug, Default)]
pub struct DoorLimiter {
    doors: HashMap<([u8; 16], Lane), DoorState>,
}

impl DoorLimiter {
    /// An empty limiter.
    pub fn new() -> Self {
        Self::default()
    }

    /// Whether `lane` at `door` is cooling down at `now_ms`.
    pub fn is_cooling(&mut self, door: &[u8; 16], lane: Lane, now_ms: u64) -> bool {
        let Some(st) = self.doors.get_mut(&(*door, lane)) else {
            return false;
        };
        st.expire(now_ms);
        st.cooldown_until.is_some()
    }

    /// Record a rejection on `lane` at `door`.
    pub fn record_rejection(&mut self, door: &[u8; 16], lane: Lane, now_ms: u64) {
        let key = (*door, lane);
        if !self.doors.contains_key(&key) && self.doors.len() >= MAX_DOORS {
            if let Some(oldest) = self
                .doors
                .iter()
                .min_by_key(|(_, s)| s.last_touch)
                .map(|(k, _)| *k)
            {
                self.doors.remove(&oldest);
            }
        }
        let st = self.doors.entry(key).or_default();
        st.last_touch = now_ms;
        // A cooldown that has ended is forgotten here too, not only in
        // `is_cooling`: otherwise the rejections that started it would still
        // sit in the window, and one more would re-arm it at once.
        st.expire(now_ms);
        if st.cooldown_until.is_some() {
            return;
        }
        while st
            .rejections
            .front()
            .is_some_and(|t| now_ms.saturating_sub(*t) >= WINDOW_MS)
        {
            st.rejections.pop_front();
        }
        st.rejections.push_back(now_ms);
        if st.rejections.len() >= MAX_REJECTIONS {
            st.cooldown_until = Some(now_ms.saturating_add(COOLDOWN_MS));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const D: [u8; 16] = [0; 16];
    const E: [u8; 16] = [1; 16];
    const SIG: Lane = Lane::Signature;

    #[test]
    fn five_in_ten_minutes_cools_for_sixty_seconds() {
        let mut l = DoorLimiter::new();
        for i in 0..4 {
            l.record_rejection(&D, SIG, i * 1000);
            assert!(!l.is_cooling(&D, SIG, i * 1000));
        }
        l.record_rejection(&D, SIG, 5000);
        assert!(l.is_cooling(&D, SIG, 5000));
        assert!(l.is_cooling(&D, SIG, 5000 + COOLDOWN_MS - 1));
        assert!(!l.is_cooling(&D, SIG, 5000 + COOLDOWN_MS));
        // The count restarts after the cooldown.
        l.record_rejection(&D, SIG, 5000 + COOLDOWN_MS + 1);
        assert!(!l.is_cooling(&D, SIG, 5000 + COOLDOWN_MS + 1));
    }

    #[test]
    fn an_ended_cooldown_is_not_rearmed_by_one_rejection() {
        // The stream the review found: five rejections, then one a minute,
        // with nobody calling `is_cooling` in between. Before the fix the
        // five old ones still sat in the window and the first new one
        // re-armed the cooldown at once; now the count starts over, and only
        // a fifth rejection inside ten minutes cools the lane again.
        let mut l = DoorLimiter::new();
        for i in 0..5 {
            l.record_rejection(&D, SIG, i);
        }
        let mut t = 4 + COOLDOWN_MS;
        for n in 1..=4 {
            l.record_rejection(&D, SIG, t);
            assert!(!l.is_cooling(&D, SIG, t + 1), "re-armed after {n}");
            t += COOLDOWN_MS;
        }
        l.record_rejection(&D, SIG, t);
        assert!(l.is_cooling(&D, SIG, t + 1));
    }

    #[test]
    fn old_rejections_age_out() {
        let mut l = DoorLimiter::new();
        for i in 0..4 {
            l.record_rejection(&D, SIG, i);
        }
        l.record_rejection(&D, SIG, WINDOW_MS + 10);
        assert!(!l.is_cooling(&D, SIG, WINDOW_MS + 10));
    }

    #[test]
    fn doors_and_lanes_are_independent() {
        let mut l = DoorLimiter::new();
        for i in 0..5 {
            l.record_rejection(&D, Lane::Card, i);
        }
        assert!(l.is_cooling(&D, Lane::Card, 10));
        assert!(!l.is_cooling(&D, SIG, 10), "card junk cooled the phones");
        assert!(!l.is_cooling(&E, Lane::Card, 10));
    }

    #[test]
    fn memory_is_bounded() {
        let mut l = DoorLimiter::new();
        for i in 0..(MAX_DOORS as u64 + 10) {
            let mut d = [0u8; 16];
            d[..8].copy_from_slice(&i.to_be_bytes());
            l.record_rejection(&d, SIG, i);
            l.record_rejection(&d, Lane::Card, i);
        }
        assert!(l.doors.len() <= MAX_DOORS);
    }
}
