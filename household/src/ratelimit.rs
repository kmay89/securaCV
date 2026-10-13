//! The per-door rejection limiter.
//!
//! Five rejected responses at one door within ten minutes start a 60-second
//! cooldown for that door's transport. Rejections are counted in RAM only,
//! keyed by door id, and **never per credential or per handle**: the limiter
//! holds no state that could become a per-member usage tally. The LAN API is
//! one "door" (`door_id` = sixteen zero bytes).

use std::collections::{HashMap, VecDeque};

/// Rejections that trigger a cooldown.
pub const MAX_REJECTIONS: usize = 5;
/// The window those rejections are counted in.
pub const WINDOW_MS: u64 = 10 * 60 * 1000;
/// The cooldown.
pub const COOLDOWN_MS: u64 = 60 * 1000;
/// Most doors tracked at once (the oldest entry is dropped beyond this).
pub const MAX_DOORS: usize = 64;

#[derive(Debug, Default)]
struct DoorState {
    rejections: VecDeque<u64>,
    cooldown_until: Option<u64>,
    last_touch: u64,
}

/// The limiter.
#[derive(Debug, Default)]
pub struct DoorLimiter {
    doors: HashMap<[u8; 16], DoorState>,
}

impl DoorLimiter {
    /// An empty limiter.
    pub fn new() -> Self {
        Self::default()
    }

    /// Whether `door` is cooling down at `now_ms`.
    pub fn is_cooling(&mut self, door: &[u8; 16], now_ms: u64) -> bool {
        let Some(st) = self.doors.get_mut(door) else {
            return false;
        };
        match st.cooldown_until {
            Some(t) if now_ms < t => true,
            Some(_) => {
                st.cooldown_until = None;
                st.rejections.clear();
                false
            }
            None => false,
        }
    }

    /// Record a rejection at `door`.
    pub fn record_rejection(&mut self, door: &[u8; 16], now_ms: u64) {
        if !self.doors.contains_key(door) && self.doors.len() >= MAX_DOORS {
            if let Some(oldest) = self
                .doors
                .iter()
                .min_by_key(|(_, s)| s.last_touch)
                .map(|(k, _)| *k)
            {
                self.doors.remove(&oldest);
            }
        }
        let st = self.doors.entry(*door).or_default();
        st.last_touch = now_ms;
        if st.cooldown_until.is_some_and(|t| now_ms < t) {
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
            st.cooldown_until = Some(now_ms + COOLDOWN_MS);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const D: [u8; 16] = [0; 16];
    const E: [u8; 16] = [1; 16];

    #[test]
    fn five_in_ten_minutes_cools_for_sixty_seconds() {
        let mut l = DoorLimiter::new();
        for i in 0..4 {
            l.record_rejection(&D, i * 1000);
            assert!(!l.is_cooling(&D, i * 1000));
        }
        l.record_rejection(&D, 5000);
        assert!(l.is_cooling(&D, 5000));
        assert!(l.is_cooling(&D, 5000 + COOLDOWN_MS - 1));
        assert!(!l.is_cooling(&D, 5000 + COOLDOWN_MS));
        // The count restarts after the cooldown.
        l.record_rejection(&D, 5000 + COOLDOWN_MS + 1);
        assert!(!l.is_cooling(&D, 5000 + COOLDOWN_MS + 1));
    }

    #[test]
    fn old_rejections_age_out() {
        let mut l = DoorLimiter::new();
        for i in 0..4 {
            l.record_rejection(&D, i);
        }
        l.record_rejection(&D, WINDOW_MS + 10);
        assert!(!l.is_cooling(&D, WINDOW_MS + 10));
    }

    #[test]
    fn doors_are_independent() {
        let mut l = DoorLimiter::new();
        for i in 0..5 {
            l.record_rejection(&D, i);
        }
        assert!(l.is_cooling(&D, 10));
        assert!(!l.is_cooling(&E, 10));
    }

    #[test]
    fn memory_is_bounded() {
        let mut l = DoorLimiter::new();
        for i in 0..(MAX_DOORS as u64 + 10) {
            let mut d = [0u8; 16];
            d[..8].copy_from_slice(&i.to_be_bytes());
            l.record_rejection(&d, i);
        }
        assert!(l.doors.len() <= MAX_DOORS);
    }
}
