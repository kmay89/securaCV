//! Canary Cards in Rust, and the classing gate that decides which display a
//! card may reach.
//!
//! The schema is `docs/standard/CANARY_CARDS.md` v1. This is its third
//! implementation — the reference renderer is
//! `canary-local/assets/canary-cards.js`, the glass carries a pure-logic
//! model in `firmware/projects/canary-display/include/canary/fleet/fleet_cards.h`,
//! and this is the hub's. All three are projections of the same MQTT entity
//! set, so the surface formats nothing the other two do not: a card that
//! renders here renders there, with the same `null`-as-unknown and `absent`
//! honesty rules.
//!
//! # The classing gate
//!
//! The BUSY Bar has two displays with two different audiences, and the whole
//! design rests on mapping them onto the card schema's privacy classes:
//!
//! | display | audience | admits |
//! |---|---|---|
//! | front RGB matrix | the room — anyone at the door | public phrases only |
//! | rear display | the operator, at arm's length | P0 detail, P1 by opt-in |
//!
//! Four rules, each enforced by [`front_phrase`] or [`rear_admits`] and each
//! covered by a test:
//!
//! 1. **A card that cannot be classed is not rendered. Ever.** A descriptor
//!    with no `privacy` field is [`Refusal::Unclassed`] on *both* displays.
//!    The schema makes `privacy` optional for forward compatibility; a
//!    surface that renders witness state does not get to inherit that
//!    latitude. Unknown class is refused, not defaulted — the fail-closed
//!    half of Invariant conformance (`spec/invariants.md` §10).
//! 2. **P2 never renders anywhere.** P2 means the value never leaves the
//!    device and is seen only as its coarse derivative (`range_band` is the
//!    worked example: centimeters stay on the radar, the band is a separate
//!    P0 card). A surface that rendered P2 would be the leak the class
//!    exists to name.
//! 3. **P1 is operator-only and opt-in.** Wellbeing numerics — breath rate,
//!    heart rate — are a person's vitals. They are refused on the front
//!    unconditionally and refused on the rear unless the operator turned
//!    them on in the surface config.
//! 4. **Only coarse kinds can become a public phrase.** `stat` and
//!    `sparkline` are numbers, and a number on a room-facing display is a
//!    reading about whoever is in the room. [`front_phrase`] has no arm that
//!    produces a phrase from either kind, so P0 numerics such as
//!    `illuminance` and `frame_errors` reach the rear and stop there.
//!
//! Rule 4 is why the front takes a [`PublicPhrase`] and not a formatted
//! string: the type has no constructor that accepts one.

use super::phrase::{AdvisoryWord, DegradedWord, PublicPhrase, ZoneWord};

/// Schema version this surface implements. A card carrying any other `v` is
/// refused rather than guessed at (`CANARY_CARDS.md` §6).
pub const CARD_SCHEMA_V: u8 = 1;

/// Privacy class chip (`CANARY_CARDS.md` §1, design doc §2).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PrivacyClass {
    /// Coarse witness claim.
    P0,
    /// Opt-in wellbeing numeric.
    P1,
    /// Never leaves the device; seen only as its coarse derivative.
    P2,
}

/// Card kind (`CANARY_CARDS.md` §2).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum CardKind {
    Binary,
    Stat,
    Band,
    Sparkline,
    Event,
    Trust,
}

/// Severity accent.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Severity {
    Ok,
    Notice,
    Warn,
    Alert,
}

/// Trust badge. The vocabulary discipline is AD-Core §2.5: `Verified` only
/// after a signature this process actually checked against a pinned key; a
/// merely well-formed signature is `Signed`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Badge {
    Verified,
    Signed,
    Unsigned,
    Failed,
    Unknown,
}

/// The per-kind payload. `None` is *unknown*, never zero — a stalled radar is
/// unknown presence, not absence of presence (AD-Core §2.1).
#[derive(Clone, Debug, PartialEq)]
pub enum CardValue {
    Binary(Option<bool>),
    Stat {
        value: Option<f64>,
        unit: String,
    },
    Band {
        options: Vec<String>,
        value: Option<String>,
    },
    Sparkline {
        value: Option<f64>,
        unit: String,
    },
    Event {
        value: Option<String>,
        signed: Option<bool>,
    },
    Trust {
        chain: u64,
        badge: Badge,
    },
}

impl CardValue {
    /// The kind this value belongs to.
    pub fn kind(&self) -> CardKind {
        match self {
            CardValue::Binary(_) => CardKind::Binary,
            CardValue::Stat { .. } => CardKind::Stat,
            CardValue::Band { .. } => CardKind::Band,
            CardValue::Sparkline { .. } => CardKind::Sparkline,
            CardValue::Event { .. } => CardKind::Event,
            CardValue::Trust { .. } => CardKind::Trust,
        }
    }
}

/// One card descriptor.
#[derive(Clone, Debug, PartialEq)]
pub struct Card {
    /// Schema version. Anything but [`CARD_SCHEMA_V`] is refused.
    pub v: u8,
    /// Lowercase slug — the HA `object_id` suffix, the join key.
    pub id: String,
    /// Human label, for the rear display.
    pub title: String,
    /// Privacy class. `None` means unclassed, which is refused everywhere.
    pub privacy: Option<PrivacyClass>,
    pub severity: Option<Severity>,
    /// The entity is compiled out of this build — "provably not here", which
    /// is a fact worth rendering on the rear and nothing at all on the front.
    pub absent: bool,
    pub value: CardValue,
}

impl Card {
    /// This card's kind.
    pub fn kind(&self) -> CardKind {
        self.value.kind()
    }
}

/// Why a card was kept off a display. Every refusal names its rule so the
/// rear's diagnostics page and the tests can both quote it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Refusal {
    /// `v` is not [`CARD_SCHEMA_V`].
    SchemaVersion,
    /// No `privacy` field. Rule 1 — refused on every display.
    Unclassed,
    /// P2. Rule 2 — refused on every display.
    NeverLeavesDevice,
    /// P1 and the operator has not opted in. Rule 3.
    WellbeingNotOptedIn,
    /// P1 on the room-facing matrix. Rule 3, unconditional.
    WellbeingIsNotPublic,
    /// The kind carries a number, and numbers are not public. Rule 4.
    NumericIsNotPublic,
    /// The kind is coarse but this particular card has no public meaning —
    /// nothing on the front says it, and the front vocabulary is closed.
    NoPublicPhrase,
    /// The value is `null`: unknown. Unknown is a rear fact ("—"), never a
    /// front phrase, because the front's own unknown state means something
    /// stricter (this surface cannot see the fleet at all).
    ValueUnknown,
    /// The entity is compiled out of this build.
    AbsentFromBuild,
}

impl Refusal {
    /// One short operator-facing line. Used by the rear's diagnostics and by
    /// the surface's log; never rendered on the front.
    pub fn reason(self) -> &'static str {
        match self {
            Refusal::SchemaVersion => "card schema version is not v1",
            Refusal::Unclassed => "card carries no privacy class",
            Refusal::NeverLeavesDevice => "P2: never leaves the device",
            Refusal::WellbeingNotOptedIn => "P1 wellbeing: not opted in",
            Refusal::WellbeingIsNotPublic => "P1 wellbeing: never room-facing",
            Refusal::NumericIsNotPublic => "numeric card: never room-facing",
            Refusal::NoPublicPhrase => "no public phrase for this card",
            Refusal::ValueUnknown => "value unknown",
            Refusal::AbsentFromBuild => "entity not in this build",
        }
    }
}

/// Operator switches that widen what the rear may show. Nothing here can
/// widen the front: the front's ceiling is not configurable, on purpose.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ClassOptions {
    /// Show P1 wellbeing numerics on the operator-facing rear display.
    /// Default `false`. There is deliberately no front equivalent.
    pub rear_shows_wellbeing: bool,
}

/// Shared refusals — the rules that hold on every display, checked first so
/// neither gate can accidentally admit what the other refuses.
fn common_refusal(card: &Card, opts: &ClassOptions) -> Option<Refusal> {
    if card.v != CARD_SCHEMA_V {
        return Some(Refusal::SchemaVersion);
    }
    match card.privacy {
        None => Some(Refusal::Unclassed),
        Some(PrivacyClass::P2) => Some(Refusal::NeverLeavesDevice),
        Some(PrivacyClass::P1) if !opts.rear_shows_wellbeing => {
            Some(Refusal::WellbeingNotOptedIn)
        }
        _ => None,
    }
}

/// May this card render on the operator-facing rear display?
pub fn rear_admits(card: &Card, opts: &ClassOptions) -> Result<(), Refusal> {
    if let Some(r) = common_refusal(card, opts) {
        return Err(r);
    }
    Ok(())
}

/// May this card put a phrase on the room-facing front matrix, and which?
///
/// `zone` is the operator-authored label for the Canary that published this
/// card, resolved from the surface config's zone table before the call. It is
/// the only variable text any phrase can carry, and it never comes from the
/// card (see [`super::phrase`]).
///
/// The match below is the complete list of cards with a public meaning. It is
/// short on purpose: a new entity does not become room-facing by existing.
pub fn front_phrase(
    card: &Card,
    zone: Option<&ZoneWord>,
    opts: &ClassOptions,
) -> Result<PublicPhrase, Refusal> {
    // The front is stricter than the rear on P1, and stricter than the
    // operator's own switch: opting into wellbeing on the rear must never
    // move a vitals card to the room-facing glass.
    if card.privacy == Some(PrivacyClass::P1) {
        return Err(Refusal::WellbeingIsNotPublic);
    }
    if let Some(r) = common_refusal(card, opts) {
        return Err(r);
    }
    if card.absent {
        return Err(Refusal::AbsentFromBuild);
    }
    // Rule 4, as a type-level fact: neither arm below can be reached with a
    // numeric kind, because neither `Stat` nor `Sparkline` appears in the
    // match on `card.value` that follows.
    match (card.id.as_str(), &card.value) {
        // Presence: the one card whose public form names a place.
        ("presence", CardValue::Binary(Some(true))) => Ok(PublicPhrase::Presence {
            zone: zone.cloned(),
        }),
        ("presence", CardValue::Binary(Some(false))) => Ok(PublicPhrase::Calm),
        ("presence", CardValue::Binary(None)) => Err(Refusal::ValueUnknown),

        // The witness chain, as the room sees it: proved, or not.
        ("chain", CardValue::Trust { badge, .. }) => match badge {
            Badge::Failed => Ok(PublicPhrase::Degraded(DegradedWord::ChainFail)),
            Badge::Verified | Badge::Signed => Ok(PublicPhrase::Calm),
            // `unsigned` and `unknown` are not a public claim in either
            // direction. Saying CHAIN FAIL would overclaim a failure the
            // surface has not seen; saying nothing (Calm) would overclaim
            // health. The rear names it; the front leaves it to the
            // surface's own unknown state.
            Badge::Unsigned | Badge::Unknown => Err(Refusal::NoPublicPhrase),
        },

        // A witness reporting interference with itself.
        ("tamper", CardValue::Binary(Some(true))) => {
            Ok(PublicPhrase::Degraded(DegradedWord::Tamper))
        }

        // Heard life-safety cadences, relayed — never originated (Beacon
        // invariant 1; see `super::phrase::AdvisoryWord`).
        ("smoke_alarm_heard", CardValue::Binary(Some(true))) => {
            Ok(PublicPhrase::Advisory(AdvisoryWord::Smoke))
        }
        ("co_alarm_heard", CardValue::Binary(Some(true))) => {
            Ok(PublicPhrase::Advisory(AdvisoryWord::CoAlarm))
        }

        // Everything else — every numeric card, every band, every event
        // string, every entity a future peripheral invents — has no public
        // phrase and therefore never reaches the room.
        _ => Err(Refusal::NoPublicPhrase),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surface::busybar::phrase::ZoneWord;

    fn zone() -> ZoneWord {
        ZoneWord::try_new("FRONT DOOR").expect("valid label")
    }

    fn card(id: &str, privacy: Option<PrivacyClass>, value: CardValue) -> Card {
        Card {
            v: CARD_SCHEMA_V,
            id: id.to_string(),
            title: id.to_string(),
            privacy,
            severity: None,
            absent: false,
            value,
        }
    }

    /// Classing rule 1. The schema makes `privacy` optional for forward
    /// compatibility; a surface rendering witness state does not inherit
    /// that latitude. Unknown class fails closed, on BOTH displays.
    #[test]
    fn a_card_that_cannot_be_classed_is_not_rendered_ever() {
        let c = card("presence", None, CardValue::Binary(Some(true)));
        let opts = ClassOptions {
            rear_shows_wellbeing: true,
        };
        assert_eq!(front_phrase(&c, Some(&zone()), &opts), Err(Refusal::Unclassed));
        assert_eq!(rear_admits(&c, &opts), Err(Refusal::Unclassed));
    }

    /// Classing rule 2. P2 is the class whose whole meaning is "this value
    /// never leaves the device"; a surface that rendered it anywhere would
    /// be the leak the class exists to name.
    #[test]
    fn p2_never_renders_on_either_display() {
        let c = card(
            "range_band",
            Some(PrivacyClass::P2),
            CardValue::Band {
                options: vec!["near".into(), "mid".into(), "far".into()],
                value: Some("near".into()),
            },
        );
        for opts in [
            ClassOptions {
                rear_shows_wellbeing: false,
            },
            ClassOptions {
                rear_shows_wellbeing: true,
            },
        ] {
            assert_eq!(
                front_phrase(&c, Some(&zone()), &opts),
                Err(Refusal::NeverLeavesDevice)
            );
            assert_eq!(rear_admits(&c, &opts), Err(Refusal::NeverLeavesDevice));
        }
    }

    /// Classing rule 3, and the asymmetry that matters: opting into
    /// wellbeing on the OPERATOR display must never move a person's vitals
    /// onto the room-facing matrix.
    #[test]
    fn p1_is_operator_only_and_opting_in_does_not_widen_the_front() {
        let c = card(
            "heart_rate",
            Some(PrivacyClass::P1),
            CardValue::Sparkline {
                value: Some(62.0),
                unit: "bpm".into(),
            },
        );
        let off = ClassOptions {
            rear_shows_wellbeing: false,
        };
        let on = ClassOptions {
            rear_shows_wellbeing: true,
        };
        assert_eq!(rear_admits(&c, &off), Err(Refusal::WellbeingNotOptedIn));
        assert_eq!(rear_admits(&c, &on), Ok(()));
        assert_eq!(
            front_phrase(&c, Some(&zone()), &on),
            Err(Refusal::WellbeingIsNotPublic)
        );
    }

    /// Classing rule 4. A number on a room-facing display is a reading about
    /// whoever is in the room. These are ordinary P0 cards and they still
    /// stop at the rear.
    #[test]
    fn p0_numerics_reach_the_rear_and_stop_there() {
        let opts = ClassOptions::default();
        for c in [
            card(
                "illuminance",
                Some(PrivacyClass::P0),
                CardValue::Stat {
                    value: Some(12.0),
                    unit: "lx".into(),
                },
            ),
            card(
                "frame_errors",
                Some(PrivacyClass::P0),
                CardValue::Stat {
                    value: Some(3.0),
                    unit: String::new(),
                },
            ),
        ] {
            assert_eq!(rear_admits(&c, &opts), Ok(()));
            assert_eq!(
                front_phrase(&c, Some(&zone()), &opts),
                Err(Refusal::NoPublicPhrase),
                "{} reached the room-facing matrix",
                c.id
            );
        }
    }

    #[test]
    fn presence_is_the_one_card_that_names_a_place() {
        let opts = ClassOptions::default();
        let c = card("presence", Some(PrivacyClass::P0), CardValue::Binary(Some(true)));
        assert_eq!(
            front_phrase(&c, Some(&zone()), &opts),
            Ok(PublicPhrase::Presence { zone: Some(zone()) })
        );
        // No operator label for the zone: the state shows, the place does not.
        assert_eq!(
            front_phrase(&c, None, &opts),
            Ok(PublicPhrase::Presence { zone: None })
        );
    }

    /// Unknown presence is not an all-clear. A radar that stopped answering
    /// must not render as "clear" (AD-Core section 2.1).
    #[test]
    fn unknown_presence_is_not_an_all_clear() {
        let opts = ClassOptions::default();
        let c = card("presence", Some(PrivacyClass::P0), CardValue::Binary(None));
        assert_eq!(
            front_phrase(&c, Some(&zone()), &opts),
            Err(Refusal::ValueUnknown)
        );
    }

    /// The trust card never overclaims in either direction: an unsigned or
    /// unknown chain is neither a public failure nor a public all-well.
    #[test]
    fn an_unproven_chain_makes_no_public_claim() {
        let opts = ClassOptions::default();
        let mk = |badge| {
            card(
                "chain",
                Some(PrivacyClass::P0),
                CardValue::Trust { chain: 12, badge },
            )
        };
        assert_eq!(
            front_phrase(&mk(Badge::Failed), None, &opts),
            Ok(PublicPhrase::Degraded(
                crate::surface::busybar::phrase::DegradedWord::ChainFail
            ))
        );
        assert_eq!(front_phrase(&mk(Badge::Verified), None, &opts), Ok(PublicPhrase::Calm));
        assert_eq!(
            front_phrase(&mk(Badge::Unsigned), None, &opts),
            Err(Refusal::NoPublicPhrase)
        );
        assert_eq!(
            front_phrase(&mk(Badge::Unknown), None, &opts),
            Err(Refusal::NoPublicPhrase)
        );
    }

    /// The front vocabulary is closed. A peripheral that invents an entity
    /// does not get a room-facing word by existing — it gets the rear.
    #[test]
    fn a_new_entity_does_not_become_room_facing_by_existing() {
        let opts = ClassOptions::default();
        let c = card(
            "soil_moisture",
            Some(PrivacyClass::P0),
            CardValue::Binary(Some(true)),
        );
        assert_eq!(rear_admits(&c, &opts), Ok(()));
        assert_eq!(
            front_phrase(&c, Some(&zone()), &opts),
            Err(Refusal::NoPublicPhrase)
        );
    }

    /// An `event` card carries a wire-supplied string. It must never become
    /// the front's text, or the closed vocabulary is closed in name only.
    #[test]
    fn a_wire_supplied_event_string_never_becomes_a_front_phrase() {
        let opts = ClassOptions::default();
        let c = card(
            "last_event",
            Some(PrivacyClass::P0),
            CardValue::Event {
                value: Some("K. AT DOOR".to_string()),
                signed: Some(true),
            },
        );
        assert_eq!(
            front_phrase(&c, Some(&zone()), &opts),
            Err(Refusal::NoPublicPhrase)
        );
    }

    #[test]
    fn a_card_from_a_future_schema_is_refused_rather_than_guessed_at() {
        let mut c = card("presence", Some(PrivacyClass::P0), CardValue::Binary(Some(true)));
        c.v = CARD_SCHEMA_V + 1;
        let opts = ClassOptions::default();
        assert_eq!(rear_admits(&c, &opts), Err(Refusal::SchemaVersion));
        assert_eq!(front_phrase(&c, None, &opts), Err(Refusal::SchemaVersion));
    }

    #[test]
    fn an_entity_compiled_out_of_the_build_draws_nothing_on_the_front() {
        let mut c = card("presence", Some(PrivacyClass::P0), CardValue::Binary(Some(true)));
        c.absent = true;
        let opts = ClassOptions::default();
        assert_eq!(
            front_phrase(&c, None, &opts),
            Err(Refusal::AbsentFromBuild)
        );
    }
}
