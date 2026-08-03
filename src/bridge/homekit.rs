//! The Apple Home projection — a paced, closed-vocabulary view of coarse
//! witness state, shaped so that publishing it cannot leak precise event
//! timing.
//!
//! # Why this is a *pacer* and not a publisher
//!
//! [Invariant III](../../../spec/invariants.md) (Metadata Minimization) says
//! the system "cannot emit precise timestamps externally" and "cannot vary
//! network behavior in proportion to event occurrence **unless explicitly
//! configured for cover traffic**." A naive bridge violates both: it writes a
//! characteristic the instant a witness fires, so the *arrival time of the
//! packet* is the precise timestamp we refused to put in the payload.
//!
//! So this module never publishes on an event. Events only ever mark state
//! *pending*; publication happens on a **metronome** ([`Projection::tick`]),
//! which emits on every tick whether or not anything happened. Two properties
//! follow, and both are tested below:
//!
//! - **Quantization.** The finest external time resolution anything
//!   downstream can learn is one tick — never the true event instant. The
//!   tick is therefore a privacy/latency dial, not a performance constant:
//!   coarsen it and external timing blurs; shorten it and automations feel
//!   snappier. It is the same instinct as the log's 10-minute
//!   [`TimeBucket`](crate::TimeBucket), applied to the wire.
//! - **Cover traffic.** Our publication rate is constant by construction, so
//!   *that we published* carries no information.
//!
//! ## What this does not claim
//!
//! Cover traffic is only ours for the hop we own (kernel → controller). A
//! downstream controller that relays changes onward (an Apple home hub
//! syncing beyond the LAN) has its own traffic pattern we do not control.
//! What this design guarantees is the **bound**: nothing downstream can ever
//! resolve an event more precisely than the configured tick, because we never
//! knew a finer time on the wire in the first place. That is a real
//! reduction, and it is not the same as anonymity — see the design doc's
//! honesty note rather than reading more into it.
//!
//! # The dumb-PIR bar
//!
//! [`HomeSignal`] is a closed vocabulary of coarse booleans. By default it
//! projects no more than a hardware PIR sensor would: motion, occupancy,
//! contact, tamper, liveness, battery. The four class-scoped signals
//! ([`HomeSignal::is_class_scoped`]) carry one extra word — the sanctioned
//! [`ObjectClass`] vocabulary, never identity — and are **off unless a human
//! turns them on**.
//!
//! Vocabulary changes start in `spec/witness_dictionary.json` (FR-13); this
//! enum and its two mapping functions are a mirror the dictionary linter
//! gates.

use crate::detect::ObjectClass;
use crate::EventType;
use serde::{Deserialize, Serialize};

/// Number of distinct signals. Every per-signal table in this module is a
/// fixed array of this length — FR-4 ("everything is bounded"): the
/// projection allocates nothing per event and cannot grow in steady state.
pub const SIGNAL_COUNT: usize = 10;

/// A coarse boolean this kernel is willing to project into a home-automation
/// ecosystem.
///
/// Deliberately closed and deliberately dull. There is no zone field, no
/// timestamp, no count, no confidence, and no identity variant — an
/// ecosystem receiving this learns *what kind of thing is true now*, never
/// who, where precisely, or exactly when.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
#[non_exhaustive]
pub enum HomeSignal {
    /// Something moved in the witnessed area.
    Motion,
    /// A presence is being sensed (radar/occupancy-class witnesses).
    Occupancy,
    /// A binary open/closed contact changed and is currently open.
    Contact,
    /// The witnessing device itself reports tampering. Latching.
    Tamper,
    /// The witness is answering — the dead-man's-switch, projected.
    Active,
    /// The witness reports a low battery.
    LowBattery,
    /// Motion whose coarse class was `Person`. Opt-in.
    MotionPerson,
    /// Motion whose coarse class was `Vehicle`. Opt-in.
    MotionVehicle,
    /// Motion whose coarse class was `Animal`. Opt-in.
    MotionAnimal,
    /// Motion whose coarse class was `Package`. Opt-in.
    MotionPackage,
}

/// How a signal behaves over time — which is what decides whether the pacer
/// has to hold it, latch it, or simply mirror it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Shape {
    /// Momentary: asserted by an event, auto-clears after the hold window.
    Pulse,
    /// Continuous: mirrors a state the witness reports.
    Level,
    /// Sticky: stays asserted until explicitly cleared by an operator.
    Latched,
}

impl HomeSignal {
    /// Every signal, in declaration order. The index of a signal here is its
    /// bit position in a [`SignalSet`] and its slot in the hold table.
    pub const ALL: [HomeSignal; SIGNAL_COUNT] = [
        HomeSignal::Motion,
        HomeSignal::Occupancy,
        HomeSignal::Contact,
        HomeSignal::Tamper,
        HomeSignal::Active,
        HomeSignal::LowBattery,
        HomeSignal::MotionPerson,
        HomeSignal::MotionVehicle,
        HomeSignal::MotionAnimal,
        HomeSignal::MotionPackage,
    ];

    /// The dictionary id for this signal (`spec/witness_dictionary.json`).
    pub fn as_str(self) -> &'static str {
        match self {
            HomeSignal::Motion => "motion",
            HomeSignal::Occupancy => "occupancy",
            HomeSignal::Contact => "contact",
            HomeSignal::Tamper => "tamper",
            HomeSignal::Active => "active",
            HomeSignal::LowBattery => "low_battery",
            HomeSignal::MotionPerson => "motion_person",
            HomeSignal::MotionVehicle => "motion_vehicle",
            HomeSignal::MotionAnimal => "motion_animal",
            HomeSignal::MotionPackage => "motion_package",
        }
    }

    /// The HomeKit Accessory Protocol characteristic this signal projects as.
    ///
    /// Kept as data rather than woven into a HAP server so the mapping is
    /// reviewable, testable, and gated by the dictionary linter without
    /// anything having to open a socket.
    pub fn hap_characteristic(self) -> &'static str {
        match self {
            HomeSignal::Motion => "motion-detected",
            HomeSignal::Occupancy => "occupancy-detected",
            HomeSignal::Contact => "contact-sensor-state",
            HomeSignal::Tamper => "status-tampered",
            HomeSignal::Active => "status-active",
            HomeSignal::LowBattery => "status-lo-batt",
            HomeSignal::MotionPerson => "motion-detected",
            HomeSignal::MotionVehicle => "motion-detected",
            HomeSignal::MotionAnimal => "motion-detected",
            HomeSignal::MotionPackage => "motion-detected",
        }
    }

    /// Behavior over time.
    pub fn shape(self) -> Shape {
        match self {
            HomeSignal::Motion
            | HomeSignal::MotionPerson
            | HomeSignal::MotionVehicle
            | HomeSignal::MotionAnimal
            | HomeSignal::MotionPackage => Shape::Pulse,
            HomeSignal::Occupancy | HomeSignal::Contact | HomeSignal::Active
            | HomeSignal::LowBattery => Shape::Level,
            HomeSignal::Tamper => Shape::Latched,
        }
    }

    /// Whether this signal carries a coarse [`ObjectClass`] word — the one
    /// sanctioned step past the dumb-PIR bar, and therefore off by default.
    pub fn is_class_scoped(self) -> bool {
        matches!(
            self,
            HomeSignal::MotionPerson
                | HomeSignal::MotionVehicle
                | HomeSignal::MotionAnimal
                | HomeSignal::MotionPackage
        )
    }

    /// Bit position in a [`SignalSet`]. Always `< SIGNAL_COUNT`, so every
    /// table index derived from it is in bounds without a bounds check that
    /// could panic (FR-2: Class B paths cannot panic).
    fn index(self) -> usize {
        match self {
            HomeSignal::Motion => 0,
            HomeSignal::Occupancy => 1,
            HomeSignal::Contact => 2,
            HomeSignal::Tamper => 3,
            HomeSignal::Active => 4,
            HomeSignal::LowBattery => 5,
            HomeSignal::MotionPerson => 6,
            HomeSignal::MotionVehicle => 7,
            HomeSignal::MotionAnimal => 8,
            HomeSignal::MotionPackage => 9,
        }
    }

    /// The class-scoped signal for a coarse object class, if one exists.
    /// [`ObjectClass::Unknown`] deliberately maps to nothing.
    pub fn for_object_class(class: ObjectClass) -> Option<HomeSignal> {
        match class {
            ObjectClass::Person => Some(HomeSignal::MotionPerson),
            ObjectClass::Vehicle => Some(HomeSignal::MotionVehicle),
            ObjectClass::Animal => Some(HomeSignal::MotionAnimal),
            ObjectClass::Package => Some(HomeSignal::MotionPackage),
            ObjectClass::Unknown => None,
        }
    }
}

/// A set of signals, as a bitset. Copy, allocation-free, bounded by
/// construction.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SignalSet(u16);

impl SignalSet {
    /// The empty set.
    pub const fn new() -> Self {
        SignalSet(0)
    }

    /// The set every projection starts with: everything that clears the
    /// dumb-PIR bar, and nothing that doesn't.
    pub fn default_enabled() -> Self {
        let mut s = SignalSet::new();
        for sig in HomeSignal::ALL {
            if !sig.is_class_scoped() {
                s.insert(sig);
            }
        }
        s
    }

    /// Add a signal.
    pub fn insert(&mut self, sig: HomeSignal) {
        self.0 |= 1 << sig.index();
    }

    /// Remove a signal.
    pub fn remove(&mut self, sig: HomeSignal) {
        self.0 &= !(1 << sig.index());
    }

    /// Whether the signal is in the set.
    pub fn contains(self, sig: HomeSignal) -> bool {
        self.0 & (1 << sig.index()) != 0
    }

    /// Whether the set is empty.
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// How many signals are in the set.
    pub fn len(self) -> u32 {
        self.0.count_ones()
    }

    /// Set intersection.
    pub fn intersect(self, other: SignalSet) -> SignalSet {
        SignalSet(self.0 & other.0)
    }

    /// Iterate the signals in the set, in declaration order.
    pub fn iter(self) -> impl Iterator<Item = HomeSignal> {
        HomeSignal::ALL.into_iter().filter(move |s| self.contains(*s))
    }
}

/// The metronome's configuration — the privacy/latency dial.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PacingConfig {
    /// Milliseconds between publications. This is the finest external time
    /// resolution the projection can ever leak; coarsen it to blur timing
    /// further, at the cost of automation latency.
    pub tick_ms: u32,
    /// How many ticks a [`Shape::Pulse`] signal stays asserted once fired.
    /// Bounds how long a momentary event remains visible, and guarantees a
    /// pulse shorter than one tick is still seen.
    pub motion_hold_ticks: u16,
}

impl PacingConfig {
    /// A tick coarser than this is refused at construction: past it, a home
    /// automation stops feeling like a response and the feature is a lie.
    pub const MAX_TICK_MS: u32 = 600_000;
    /// A tick finer than this is refused: it would approach publishing on
    /// the event itself, which is the leak this whole module exists to stop.
    pub const MIN_TICK_MS: u32 = 200;
    /// Hold windows are bounded so a pulse cannot latch by accident.
    pub const MAX_HOLD_TICKS: u16 = 3_600;
}

impl Default for PacingConfig {
    fn default() -> Self {
        // One second: automation-grade responsiveness, while still meaning
        // that nothing downstream can place an event more precisely than the
        // second it fell in. Ten ticks of hold matches the ~10 s dwell
        // commodity HomeKit motion sensors use, so automations written
        // against a Canary behave like the ones users already own.
        PacingConfig {
            tick_ms: 1_000,
            motion_hold_ticks: 10,
        }
    }
}

/// What the projection refused to accept.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum PacingError {
    /// `tick_ms` outside [`PacingConfig::MIN_TICK_MS`]..=[`PacingConfig::MAX_TICK_MS`].
    TickOutOfRange,
    /// `motion_hold_ticks` above [`PacingConfig::MAX_HOLD_TICKS`].
    HoldOutOfRange,
}

impl std::fmt::Display for PacingError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PacingError::TickOutOfRange => write!(
                f,
                "tick_ms must be {}..={}",
                PacingConfig::MIN_TICK_MS,
                PacingConfig::MAX_TICK_MS
            ),
            PacingError::HoldOutOfRange => write!(
                f,
                "motion_hold_ticks must be <= {}",
                PacingConfig::MAX_HOLD_TICKS
            ),
        }
    }
}

impl std::error::Error for PacingError {}

/// One publication. Produced by every [`Projection::tick`] call, including
/// the overwhelming majority where nothing happened — that is the cover
/// traffic, and it is why this type has no "changed" flag for callers to
/// optimize against.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Publication {
    /// Monotonic publication counter. Increments on every tick, so a gap is
    /// a missed beat and therefore observable (FR-10).
    pub seq: u64,
    /// The signals currently asserted.
    pub asserted: SignalSet,
}

/// The paced projection: coarse witness state in, a constant-rate stream of
/// [`Publication`]s out.
///
/// Drive it by calling [`observe_event`](Self::observe_event) /
/// [`set_level`](Self::set_level) whenever the kernel learns something, and
/// [`tick`](Self::tick) on the metronome. Nothing this type does depends on
/// the wall clock, which is what makes the timing properties testable.
#[derive(Clone, Debug)]
pub struct Projection {
    cfg: PacingConfig,
    enabled: SignalSet,
    /// Pulses observed since the last tick. Set, never counted: ten events
    /// in one window publish exactly like one, which is coalescing by
    /// construction rather than by a rate limiter.
    pending: SignalSet,
    /// Current value of every [`Shape::Level`] / [`Shape::Latched`] signal.
    levels: SignalSet,
    /// Remaining hold ticks per signal.
    hold: [u16; SIGNAL_COUNT],
    seq: u64,
}

impl Projection {
    /// Build a projection with the default (dumb-PIR bar) signal set.
    ///
    /// Fails closed on an out-of-range config rather than silently clamping:
    /// a pacing constant is a privacy parameter, so a caller that asked for
    /// something impossible must be told, not quietly overruled.
    pub fn new(cfg: PacingConfig) -> Result<Self, PacingError> {
        if !(PacingConfig::MIN_TICK_MS..=PacingConfig::MAX_TICK_MS).contains(&cfg.tick_ms) {
            return Err(PacingError::TickOutOfRange);
        }
        if cfg.motion_hold_ticks > PacingConfig::MAX_HOLD_TICKS {
            return Err(PacingError::HoldOutOfRange);
        }
        Ok(Projection {
            cfg,
            enabled: SignalSet::default_enabled(),
            pending: SignalSet::new(),
            levels: SignalSet::new(),
            hold: [0; SIGNAL_COUNT],
            seq: 0,
        })
    }

    /// The pacing in force.
    pub fn config(&self) -> PacingConfig {
        self.cfg
    }

    /// Which signals this projection will publish.
    pub fn enabled(&self) -> SignalSet {
        self.enabled
    }

    /// Turn a signal on or off.
    ///
    /// [`HomeSignal::Tamper`] cannot be turned off: a witness that reports
    /// its own tampering must not be able to do so invisibly to a home the
    /// operator already chose to publish into. Everything else — including
    /// every class-scoped signal — is the operator's call.
    pub fn set_enabled(&mut self, sig: HomeSignal, on: bool) -> bool {
        if !on && sig == HomeSignal::Tamper {
            return false;
        }
        if on {
            self.enabled.insert(sig);
        } else {
            self.enabled.remove(sig);
            // Drop anything already asserted for a signal being switched off,
            // so disabling takes effect at the next tick rather than after a
            // stale hold window drains.
            self.hold[sig.index()] = 0;
            self.levels.remove(sig);
        }
        true
    }

    /// Record that an event happened. Marks state pending; publishes nothing.
    ///
    /// `class` is the coarse [`ObjectClass`] where the pipeline knew one. It
    /// only ever reaches the wire if the matching class-scoped signal has
    /// been explicitly enabled.
    pub fn observe_event(&mut self, event: EventType, class: Option<ObjectClass>) {
        for sig in signals_for_event(&event) {
            self.assert_pending(*sig);
        }
        if let Some(sig) = class.and_then(HomeSignal::for_object_class) {
            // A class word only rides along with an event that is motion-shaped
            // in the first place — it can never introduce a projection an
            // unclassed event would not have produced.
            if signals_for_event(&event).contains(&HomeSignal::Motion) {
                self.assert_pending(sig);
            }
        }
    }

    /// Set a continuous signal's current value (liveness, occupancy, contact,
    /// battery). Ignored for pulse-shaped signals, which are event-driven.
    pub fn set_level(&mut self, sig: HomeSignal, on: bool) {
        if sig.shape() == Shape::Pulse {
            return;
        }
        if on {
            self.levels.insert(sig);
        } else if sig != HomeSignal::Tamper {
            self.levels.remove(sig);
        }
    }

    /// Clear a latched [`HomeSignal::Tamper`] — the deliberate operator act
    /// that `set_level(Tamper, false)` refuses to perform by accident.
    pub fn clear_tamper(&mut self) {
        self.levels.remove(HomeSignal::Tamper);
    }

    /// Advance the metronome and publish.
    ///
    /// **Always** returns a [`Publication`], on every tick, whether or not
    /// anything was observed. That is the cover traffic: a caller must send
    /// what this returns unconditionally, because suppressing "unchanged"
    /// publications is exactly the optimization that would turn our traffic
    /// back into an event-timing oracle.
    pub fn tick(&mut self) -> Publication {
        self.seq = self.seq.saturating_add(1);

        // Pending evidence (re)arms its hold window, then the window drains by
        // one tick. Draining after arming is what guarantees an event observed
        // between two ticks is visible in at least one publication.
        for sig in self.pending.intersect(self.enabled).iter() {
            self.hold[sig.index()] = self.cfg.motion_hold_ticks.max(1);
        }
        self.pending = SignalSet::new();

        // Two independent sources, unioned: `levels` is *reported state*, which
        // persists until the witness says otherwise, and the hold table is
        // *evidence with a deadline*, which expires on its own. An event can
        // therefore assert a signal without ever being able to strand it on —
        // the bug an event-writes-a-level shortcut would introduce.
        let mut asserted = self.levels.intersect(self.enabled);
        for sig in HomeSignal::ALL {
            let slot = &mut self.hold[sig.index()];
            if *slot > 0 {
                if self.enabled.contains(sig) {
                    asserted.insert(sig);
                }
                *slot = slot.saturating_sub(1);
            }
        }

        Publication {
            seq: self.seq,
            asserted,
        }
    }

    fn assert_pending(&mut self, sig: HomeSignal) {
        match sig.shape() {
            // A latching signal is the one thing an event may assert durably:
            // tamper is meant to survive until a human clears it. Everything
            // else an event says is evidence, and evidence expires — a
            // momentary observation must never be able to strand a signal on
            // forever, which is what writing it straight into `levels` did.
            Shape::Latched => self.levels.insert(sig),
            Shape::Pulse | Shape::Level => self.pending.insert(sig),
        }
    }
}

/// The event → signal mapping.
///
/// Deliberately partial: an event with no honest HAP counterpart projects
/// **nothing** rather than being forced into a sensor that would misdescribe
/// it. Two events are mapped to nothing, for two different reasons:
///
/// - `AcousticImpulseInZone` — HAP has no acoustic sensor, and publishing a
///   sound as "motion" would be a false statement about someone's home.
/// - `ContactStateChange` — it reports that a contact *changed*, not what it
///   changed **to**. HAP's contact characteristic is a state (open/closed),
///   so deriving it from a change event would show a door as open every time
///   it was closed. [`HomeSignal::Contact`] is therefore driven only by
///   [`Projection::set_level`], from a witness that reports the actual state.
pub fn signals_for_event(event: &EventType) -> &'static [HomeSignal] {
    match event {
        EventType::BoundaryCrossingObjectLarge
        | EventType::BoundaryCrossingObjectSmall
        | EventType::ObjectRemovedFromZone
        | EventType::VehiclePresenceAfterHours
        | EventType::VehicleArrivalDeparture => &[HomeSignal::Motion],
        EventType::PresenceInRestrictedZone => &[HomeSignal::Occupancy],
        EventType::TamperDetected => &[HomeSignal::Tamper],
        EventType::AcousticImpulseInZone | EventType::ContactStateChange => &[],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn proj() -> Projection {
        Projection::new(PacingConfig::default()).expect("default config is valid")
    }

    // ---- the cover-traffic property (Invariant III's named carve-out) ----

    #[test]
    fn publishes_every_tick_even_when_nothing_ever_happens() {
        let mut p = proj();
        for expected_seq in 1..=100 {
            let pubn = p.tick();
            assert_eq!(pubn.seq, expected_seq, "a tick must always publish");
        }
    }

    #[test]
    fn publication_rate_does_not_vary_with_event_rate() {
        // The same number of ticks yields the same number of publications
        // whether the window was silent or saturated with events.
        let mut quiet = proj();
        let mut busy = proj();
        let mut quiet_pubs = 0;
        let mut busy_pubs = 0;
        for i in 0..50 {
            if i % 2 == 0 {
                busy.observe_event(EventType::BoundaryCrossingObjectLarge, None);
                busy.observe_event(EventType::ContactStateChange, None);
            }
            quiet.tick();
            busy.tick();
            quiet_pubs += 1;
            busy_pubs += 1;
        }
        assert_eq!(quiet_pubs, busy_pubs);
        assert_eq!(quiet.seq, busy.seq);
    }

    // ---- the quantization property ----

    #[test]
    fn an_event_is_not_observable_before_the_next_tick() {
        let mut p = proj();
        p.observe_event(EventType::BoundaryCrossingObjectLarge, None);
        // Nothing has been published yet: the event exists only as pending
        // state, so no observer can time it more finely than the next tick.
        assert_eq!(p.seq, 0, "observing must not publish");
        let first = p.tick();
        assert!(first.asserted.contains(HomeSignal::Motion));
    }

    #[test]
    fn many_events_in_one_window_publish_exactly_like_one() {
        let mut a = proj();
        let mut b = proj();
        a.observe_event(EventType::BoundaryCrossingObjectLarge, None);
        for _ in 0..1_000 {
            b.observe_event(EventType::BoundaryCrossingObjectLarge, None);
        }
        assert_eq!(a.tick(), b.tick(), "count must not leak through the pacer");
    }

    #[test]
    fn a_pulse_shorter_than_a_tick_still_reaches_a_publication() {
        let mut p = proj();
        p.observe_event(EventType::BoundaryCrossingObjectSmall, None);
        assert!(p.tick().asserted.contains(HomeSignal::Motion));
    }

    #[test]
    fn motion_clears_after_the_hold_window() {
        let cfg = PacingConfig {
            tick_ms: 1_000,
            motion_hold_ticks: 3,
        };
        let mut p = Projection::new(cfg).expect("valid");
        p.observe_event(EventType::BoundaryCrossingObjectLarge, None);
        for tick in 1..=3 {
            assert!(
                p.tick().asserted.contains(HomeSignal::Motion),
                "motion should still be held at tick {tick}"
            );
        }
        assert!(
            !p.tick().asserted.contains(HomeSignal::Motion),
            "motion must clear once the hold window drains"
        );
    }

    #[test]
    fn a_zero_hold_config_still_shows_a_pulse_once() {
        // Guards the `.max(1)`: a caller asking for no hold must not get a
        // projection that silently swallows every event.
        let cfg = PacingConfig {
            tick_ms: 1_000,
            motion_hold_ticks: 0,
        };
        let mut p = Projection::new(cfg).expect("valid");
        p.observe_event(EventType::BoundaryCrossingObjectLarge, None);
        assert!(p.tick().asserted.contains(HomeSignal::Motion));
        assert!(!p.tick().asserted.contains(HomeSignal::Motion));
    }

    // ---- the dumb-PIR bar ----

    #[test]
    fn class_scoped_signals_are_off_by_default() {
        let p = proj();
        for sig in HomeSignal::ALL {
            assert_eq!(
                p.enabled().contains(sig),
                !sig.is_class_scoped(),
                "{} default-enabled state is wrong",
                sig.as_str()
            );
        }
    }

    #[test]
    fn a_class_word_cannot_reach_the_wire_without_opt_in() {
        let mut p = proj();
        p.observe_event(
            EventType::BoundaryCrossingObjectLarge,
            Some(ObjectClass::Person),
        );
        let pubn = p.tick();
        assert!(pubn.asserted.contains(HomeSignal::Motion));
        assert!(
            !pubn.asserted.contains(HomeSignal::MotionPerson),
            "the class word must stay home until a human opts in"
        );
    }

    #[test]
    fn an_opted_in_class_word_projects() {
        let mut p = proj();
        assert!(p.set_enabled(HomeSignal::MotionPerson, true));
        p.observe_event(
            EventType::BoundaryCrossingObjectLarge,
            Some(ObjectClass::Person),
        );
        let pubn = p.tick();
        assert!(pubn.asserted.contains(HomeSignal::MotionPerson));
        assert!(
            !pubn.asserted.contains(HomeSignal::MotionVehicle),
            "only the observed class projects"
        );
    }

    #[test]
    fn unknown_object_class_projects_no_class_word() {
        let mut p = proj();
        for sig in HomeSignal::ALL {
            p.set_enabled(sig, true);
        }
        p.observe_event(
            EventType::BoundaryCrossingObjectLarge,
            Some(ObjectClass::Unknown),
        );
        let pubn = p.tick();
        assert!(pubn.asserted.contains(HomeSignal::Motion));
        for sig in HomeSignal::ALL.iter().filter(|s| s.is_class_scoped()) {
            assert!(!pubn.asserted.contains(*sig));
        }
    }

    #[test]
    fn a_class_word_cannot_conjure_a_projection_of_its_own() {
        // A non-motion event carrying a class must not produce motion_person.
        let mut p = proj();
        assert!(p.set_enabled(HomeSignal::MotionPerson, true));
        p.observe_event(
            EventType::PresenceInRestrictedZone,
            Some(ObjectClass::Person),
        );
        let pubn = p.tick();
        assert!(!pubn.asserted.contains(HomeSignal::MotionPerson));
        assert!(pubn.asserted.contains(HomeSignal::Occupancy));
    }

    // ---- an event is evidence, and evidence expires ----

    #[test]
    fn a_presence_event_does_not_strand_occupancy_on_forever() {
        // A one-shot event carries no current-state value, so asserting a
        // level from it permanently would mean one presence observation left
        // the house "occupied" until something unrelated cleared it.
        let cfg = PacingConfig {
            tick_ms: 1_000,
            motion_hold_ticks: 3,
        };
        let mut p = Projection::new(cfg).expect("valid");
        p.observe_event(EventType::PresenceInRestrictedZone, None);
        for tick in 1..=3 {
            assert!(
                p.tick().asserted.contains(HomeSignal::Occupancy),
                "occupancy should still be held at tick {tick}"
            );
        }
        for _ in 0..20 {
            assert!(
                !p.tick().asserted.contains(HomeSignal::Occupancy),
                "occupancy must expire — an event is evidence, not a state"
            );
        }
    }

    #[test]
    fn a_contact_change_event_does_not_assert_a_contact_state() {
        // The event says a contact *changed*, not what it changed to. Deriving
        // "open" from it would show a door as open every time it was closed.
        let mut p = proj();
        p.observe_event(EventType::ContactStateChange, None);
        assert!(
            !p.tick().asserted.contains(HomeSignal::Contact),
            "a change event must not be published as a contact state"
        );
        assert!(signals_for_event(&EventType::ContactStateChange).is_empty());
    }

    #[test]
    fn contact_is_driven_by_reported_state_and_persists() {
        // The honest source for a state characteristic: a witness reporting it.
        let mut p = proj();
        p.set_level(HomeSignal::Contact, true);
        for _ in 0..20 {
            assert!(p.tick().asserted.contains(HomeSignal::Contact));
        }
        p.set_level(HomeSignal::Contact, false);
        assert!(!p.tick().asserted.contains(HomeSignal::Contact));
    }

    #[test]
    fn reported_state_outlives_an_expiring_event_for_the_same_signal() {
        // Radar says "occupied" durably while an event also asserts evidence;
        // the evidence expiring must not withdraw the reported state.
        let cfg = PacingConfig {
            tick_ms: 1_000,
            motion_hold_ticks: 2,
        };
        let mut p = Projection::new(cfg).expect("valid");
        p.set_level(HomeSignal::Occupancy, true);
        p.observe_event(EventType::PresenceInRestrictedZone, None);
        for _ in 0..10 {
            assert!(
                p.tick().asserted.contains(HomeSignal::Occupancy),
                "reported state must survive the event's hold expiring"
            );
        }
    }

    // ---- tamper ----

    #[test]
    fn tamper_cannot_be_disabled() {
        let mut p = proj();
        assert!(
            !p.set_enabled(HomeSignal::Tamper, false),
            "disabling tamper must be refused"
        );
        assert!(p.enabled().contains(HomeSignal::Tamper));
    }

    #[test]
    fn tamper_latches_until_explicitly_cleared() {
        let mut p = proj();
        p.observe_event(EventType::TamperDetected, None);
        for _ in 0..50 {
            assert!(p.tick().asserted.contains(HomeSignal::Tamper));
        }
        // The soft path must not clear it...
        p.set_level(HomeSignal::Tamper, false);
        assert!(p.tick().asserted.contains(HomeSignal::Tamper));
        // ...only the deliberate operator act does.
        p.clear_tamper();
        assert!(!p.tick().asserted.contains(HomeSignal::Tamper));
    }

    // ---- levels ----

    #[test]
    fn liveness_projects_as_active_and_darkness_withdraws_it() {
        let mut p = proj();
        p.set_level(HomeSignal::Active, true);
        assert!(p.tick().asserted.contains(HomeSignal::Active));
        p.set_level(HomeSignal::Active, false);
        assert!(
            !p.tick().asserted.contains(HomeSignal::Active),
            "a dark witness must stop asserting liveness"
        );
    }

    #[test]
    fn levels_persist_across_ticks_without_being_re_observed() {
        let mut p = proj();
        p.set_level(HomeSignal::Occupancy, true);
        for _ in 0..10 {
            assert!(p.tick().asserted.contains(HomeSignal::Occupancy));
        }
    }

    #[test]
    fn set_level_ignores_pulse_signals() {
        let mut p = proj();
        p.set_level(HomeSignal::Motion, true);
        assert!(
            !p.tick().asserted.contains(HomeSignal::Motion),
            "motion is event-driven; a level write must not fake it"
        );
    }

    #[test]
    fn disabling_a_signal_withdraws_it_at_the_next_tick() {
        let mut p = proj();
        p.set_level(HomeSignal::Occupancy, true);
        assert!(p.tick().asserted.contains(HomeSignal::Occupancy));
        assert!(p.set_enabled(HomeSignal::Occupancy, false));
        assert!(!p.tick().asserted.contains(HomeSignal::Occupancy));
    }

    // ---- the mapping ----

    #[test]
    fn an_event_with_no_honest_counterpart_projects_nothing() {
        let mut p = proj();
        p.observe_event(EventType::AcousticImpulseInZone, None);
        assert!(
            p.tick().asserted.is_empty(),
            "a sound must not be published as motion"
        );
        assert!(signals_for_event(&EventType::AcousticImpulseInZone).is_empty());
    }

    #[test]
    fn every_event_type_maps_only_into_the_closed_vocabulary() {
        let every_event = [
            EventType::BoundaryCrossingObjectLarge,
            EventType::BoundaryCrossingObjectSmall,
            EventType::AcousticImpulseInZone,
            EventType::PresenceInRestrictedZone,
            EventType::VehiclePresenceAfterHours,
            EventType::ContactStateChange,
            EventType::ObjectRemovedFromZone,
            EventType::TamperDetected,
            EventType::VehicleArrivalDeparture,
        ];
        for event in every_event {
            for sig in signals_for_event(&event) {
                assert!(
                    HomeSignal::ALL.contains(sig),
                    "{sig:?} is outside the projectable vocabulary"
                );
                assert!(
                    !sig.is_class_scoped(),
                    "an event alone must never imply a class word"
                );
            }
        }
    }

    // ---- config, bounds, and the bitset ----

    #[test]
    fn out_of_range_pacing_is_refused_not_clamped() {
        assert!(matches!(
            Projection::new(PacingConfig {
                tick_ms: 1,
                motion_hold_ticks: 10
            }),
            Err(PacingError::TickOutOfRange)
        ));
        assert!(matches!(
            Projection::new(PacingConfig {
                tick_ms: u32::MAX,
                motion_hold_ticks: 10
            }),
            Err(PacingError::TickOutOfRange)
        ));
        assert!(matches!(
            Projection::new(PacingConfig {
                tick_ms: 1_000,
                motion_hold_ticks: PacingConfig::MAX_HOLD_TICKS + 1
            }),
            Err(PacingError::HoldOutOfRange)
        ));
    }

    #[test]
    fn the_hold_table_cannot_run_away() {
        // FR-4: hammering the projection must not grow or overflow anything.
        let mut p = proj();
        for _ in 0..100_000 {
            p.observe_event(EventType::BoundaryCrossingObjectLarge, None);
        }
        for _ in 0..1_000 {
            p.tick();
        }
        assert!(p.hold.iter().all(|h| *h <= PacingConfig::MAX_HOLD_TICKS));
    }

    #[test]
    fn signal_indices_are_unique_and_in_bounds() {
        let mut seen = [false; SIGNAL_COUNT];
        for sig in HomeSignal::ALL {
            let i = sig.index();
            assert!(i < SIGNAL_COUNT);
            assert!(!seen[i], "duplicate index for {}", sig.as_str());
            seen[i] = true;
        }
        assert!(seen.into_iter().all(|s| s));
    }

    #[test]
    fn signal_ids_are_unique_snake_case() {
        let mut ids: Vec<&str> = HomeSignal::ALL.iter().map(|s| s.as_str()).collect();
        ids.sort_unstable();
        let before = ids.len();
        ids.dedup();
        assert_eq!(before, ids.len(), "duplicate signal id");
        for id in ids {
            assert!(
                id.chars().all(|c| c.is_ascii_lowercase() || c == '_'),
                "{id} is not snake_case"
            );
        }
    }

    #[test]
    fn signal_set_round_trips() {
        let mut s = SignalSet::new();
        assert!(s.is_empty());
        s.insert(HomeSignal::Motion);
        s.insert(HomeSignal::Tamper);
        assert_eq!(s.len(), 2);
        assert_eq!(
            s.iter().collect::<Vec<_>>(),
            vec![HomeSignal::Motion, HomeSignal::Tamper]
        );
        s.remove(HomeSignal::Motion);
        assert!(!s.contains(HomeSignal::Motion));
        assert!(s.contains(HomeSignal::Tamper));
    }

    #[test]
    fn the_vocabulary_carries_no_identity_shaped_word() {
        // A cheap, blunt guard on Invariant II: if someone ever adds a
        // face/plate/identity signal, this fails before it reaches a home.
        const FORBIDDEN: [&str; 6] = ["face", "plate", "identity", "person_id", "who", "recognize"];
        for sig in HomeSignal::ALL {
            let id = sig.as_str();
            for bad in FORBIDDEN {
                assert!(
                    !id.contains(bad),
                    "{id} looks like an identity signal ({bad})"
                );
            }
        }
    }
}
