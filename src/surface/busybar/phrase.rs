//! The closed public vocabulary of the room-facing matrix.
//!
//! Constraint 2 of the surface design (`docs/design/busybar_surface.md` §3)
//! says no PII reaches the glass, and says it must be **structurally
//! impossible** rather than merely avoided. The front matrix is 72 pixels
//! wide; a designer under that pressure abbreviates, and abbreviation slides
//! toward identity — the worked example in the design doc is `K. AT DOOR`,
//! which is shorter, more useful, and exactly the thing this project exists
//! not to build.
//!
//! You cannot get that property by reviewing strings. You get it by making
//! the render path refuse to accept one. So:
//!
//! - The front renderer takes a [`PublicPhrase`], never a `String`. There is
//!   no `From<String>`, no `FromStr`, and no public constructor that takes
//!   free text. Everything it can say is a variant of this enum.
//! - The only variable part of any phrase is a [`ZoneWord`] — a **place**,
//!   not a person. It is read from the operator's own config table at load
//!   time, keyed by `zone_id`, and validated once by [`ZoneWord::try_new`].
//!   A `zone_id` that is not in the table renders with no label at all, so
//!   a payload cannot introduce a word by naming a zone nobody configured.
//! - No field of any MQTT payload, kernel event, or HTTP response is ever
//!   interpolated into a front phrase. The zone table is the whole of the
//!   variable vocabulary and it is authored, not received.
//!
//! [`ZoneWord`]'s grammar then closes the abbreviation route specifically: no
//! `.` is in the allowed character set, and every space-separated token must
//! be at least two characters, so `K. AT DOOR` and `K AT DOOR` are both
//! rejected at config load with a message that says why.
//!
//! What this does **not** claim: an operator who types a housemate's name
//! into their own zone table has typed it themselves, and no grammar can
//! tell `ROSE` the room from `ROSE` the person. The honest guarantee is
//! narrower and worth more — *nothing the network carries can put a word on
//! the front matrix*, and the shape identity-abbreviation actually takes is
//! refused. That is a `can't`; the rest is the operator's own glass.

use std::fmt;

/// Longest zone label the front matrix will accept.
///
/// The matrix is 72 px wide and the device's small font is ~5 px per glyph,
/// so roughly 12 characters fit without scrolling and longer labels scroll.
/// Sixteen is deliberately a little past the no-scroll budget — a label that
/// scrolls is legible, a label that is a sentence is not — and it is a
/// bound (FR-4), not a layout claim: the exact glyph width is the device's
/// business and this module never asserts a pixel count.
pub const MAX_ZONE_WORD_CHARS: usize = 16;

/// Shortest token allowed inside a zone label. Two, so a single initial
/// cannot be a token — see the module docs.
pub const MIN_ZONE_TOKEN_CHARS: usize = 2;

/// Why a proposed zone label was refused. Every variant names the rule, so
/// the config loader can print something an operator can act on.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ZoneWordError {
    /// The label was empty or only spaces.
    Empty,
    /// Longer than [`MAX_ZONE_WORD_CHARS`].
    TooLong { chars: usize },
    /// A character outside `A-Z`, `0-9` and the single interior space.
    BadChar(char),
    /// Leading, trailing, or doubled space.
    BadSpacing,
    /// A space-separated token shorter than [`MIN_ZONE_TOKEN_CHARS`] — the
    /// initial-abbreviation shape.
    TokenTooShort { token: String },
    /// No token contains a letter (a bare number is not a place name).
    NoLetters,
}

impl fmt::Display for ZoneWordError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ZoneWordError::Empty => write!(f, "zone label is empty"),
            ZoneWordError::TooLong { chars } => write!(
                f,
                "zone label is {chars} characters; the front matrix accepts at most \
                 {MAX_ZONE_WORD_CHARS}"
            ),
            ZoneWordError::BadChar(c) => write!(
                f,
                "zone label contains {c:?}; the front matrix accepts only A-Z, 0-9 and \
                 single spaces (no punctuation — an abbreviation like \"K. AT DOOR\" is \
                 refused on purpose)"
            ),
            ZoneWordError::BadSpacing => {
                write!(f, "zone label has a leading, trailing, or doubled space")
            }
            ZoneWordError::TokenTooShort { token } => write!(
                f,
                "zone label word {token:?} is shorter than {MIN_ZONE_TOKEN_CHARS} characters; \
                 single letters read as initials, and the front matrix is a place label, \
                 never a person"
            ),
            ZoneWordError::NoLetters => {
                write!(f, "zone label has no letters; a bare number is not a place")
            }
        }
    }
}

impl std::error::Error for ZoneWordError {}

/// An operator-authored place label, cleared for the room-facing matrix.
///
/// Constructed only by [`ZoneWord::try_new`], and only from the surface's own
/// config file. Nothing in the ingest path can build one.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ZoneWord(String);

impl ZoneWord {
    /// Validate an operator-authored label. See [`ZoneWordError`] for the
    /// rules; the input is upper-cased first, so an operator may write
    /// `front door` in their TOML and get `FRONT DOOR` on the glass.
    pub fn try_new(raw: &str) -> Result<Self, ZoneWordError> {
        let upper = raw.trim_matches('\u{feff}').to_ascii_uppercase();
        if upper.is_empty() {
            return Err(ZoneWordError::Empty);
        }
        let chars = upper.chars().count();
        if chars > MAX_ZONE_WORD_CHARS {
            return Err(ZoneWordError::TooLong { chars });
        }
        for c in upper.chars() {
            let ok = c.is_ascii_uppercase() || c.is_ascii_digit() || c == ' ';
            if !ok {
                return Err(ZoneWordError::BadChar(c));
            }
        }
        if upper.starts_with(' ') || upper.ends_with(' ') || upper.contains("  ") {
            return Err(ZoneWordError::BadSpacing);
        }
        let mut any_letter = false;
        for token in upper.split(' ') {
            if token.chars().count() < MIN_ZONE_TOKEN_CHARS {
                return Err(ZoneWordError::TokenTooShort {
                    token: token.to_string(),
                });
            }
            any_letter |= token.chars().any(|c| c.is_ascii_uppercase());
        }
        if !any_letter {
            return Err(ZoneWordError::NoLetters);
        }
        Ok(ZoneWord(upper))
    }

    /// The label as it goes to the glass.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for ZoneWord {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// The chain or a witness is in trouble. Front words, not diagnoses.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum DegradedWord {
    /// A witness reports interference with itself.
    Tamper,
    /// The sealed log failed verification on this surface's own check.
    ChainFail,
    /// A Canary is past its lost deadline.
    WitnessLost,
    /// A Canary is late but not yet lost.
    WitnessLate,
}

impl DegradedWord {
    /// The fixed word for the matrix.
    pub fn text(self) -> &'static str {
        match self {
            DegradedWord::Tamper => "TAMPER",
            DegradedWord::ChainFail => "CHAIN FAIL",
            DegradedWord::WitnessLost => "WITNESS LOST",
            DegradedWord::WitnessLate => "WITNESS LATE",
        }
    }
}

/// The surface cannot see witness state at all. Distinct from healthy and
/// from degraded: degraded means *the house knows something is wrong*,
/// unknown means *this glass does not know anything*.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum UnknownWord {
    /// The MQTT broker is unreachable.
    BrokerLost,
    /// The witness kernel's read-only API is unreachable.
    KernelLost,
}

impl UnknownWord {
    /// The fixed word for the matrix.
    ///
    /// Both variants render the same word on purpose. To a person walking
    /// past the door the public fact is identical — this glass does not
    /// currently know — and 72 pixels is not the place to explain which link
    /// died. The operator-facing rear display names the specific one, which
    /// is the whole two-audience idea working as intended.
    pub fn text(self) -> &'static str {
        match self {
            UnknownWord::BrokerLost | UnknownWord::KernelLost => "NO LINK",
        }
    }

    /// The operator-facing detail, for the rear display only.
    pub fn operator_text(self) -> &'static str {
        match self {
            UnknownWord::BrokerLost => "broker unreachable",
            UnknownWord::KernelLost => "kernel API unreachable",
        }
    }
}

/// A life-safety advisory the surface **displays**. It never originates one.
///
/// Beacon invariant 1 (`AGENTS.md`, Beacon Channel Invariants) is that
/// sensors prompt humans and never originate; a desk display is even further
/// from a human than a sensor is. So this enum has no constructor that takes
/// sensor state: an [`AdvisoryWord`] can only be built by the ingest path
/// from an advisory that already exists in the witness vocabulary, and the
/// surface has no publish path at all (see [`super::controls`]).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum AdvisoryWord {
    /// A smoke alarm's NFPA 72 T3 cadence was heard by a Canary.
    Smoke,
    /// A CO alarm's UL 2034 T4 cadence was heard by a Canary.
    CoAlarm,
}

impl AdvisoryWord {
    /// The fixed word for the matrix. Same words the shipped Home Assistant
    /// blueprint already puts on this hardware
    /// (`docs/blueprints/securacv_busybar_alert.yaml`), so a household that
    /// runs both lanes reads one vocabulary.
    pub fn text(self) -> &'static str {
        match self {
            AdvisoryWord::Smoke => "SMOKE",
            AdvisoryWord::CoAlarm => "CO ALARM",
        }
    }
}

/// Everything the room-facing matrix is able to say.
///
/// Exhaustive by construction. Adding a phrase is a deliberate edit here plus
/// a docs change, never a payload field finding its way to the glass.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PublicPhrase {
    /// Healthy and quiet. Renders as the breathing pulse with no text — the
    /// calm state is the absence of a word, not a word saying "calm".
    Calm,
    /// A presence card is dwelling. `zone` is `None` when the event's
    /// `zone_id` has no operator-authored label, which is the safe default:
    /// the state still shows, the place stays unnamed.
    Presence { zone: Option<ZoneWord> },
    /// Something is wrong and the house knows what.
    Degraded(DegradedWord),
    /// This glass cannot see witness state.
    Unknown(UnknownWord),
    /// A life-safety advisory, relayed.
    Advisory(AdvisoryWord),
    /// A microphone or camera in this room is live.
    ///
    /// **Not a witness claim, and deliberately not a card.** It says nothing
    /// about what SecuraCV is doing — SecuraCV's Canaries do not stream, so
    /// a capture indicator about *them* would read false in the one direction
    /// that matters. It is a fact about the room's other devices, which is
    /// precisely the fact worth putting where people can see it: a box that
    /// announces when a camera is running is on this project's side of the
    /// argument.
    ///
    /// The bar has a native on-call state, driven by the vendor's desktop app
    /// watching the host machine's microphone and camera. That state is **not
    /// exposed by the device's HTTP API** — nothing in the firmware's OpenAPI
    /// mentions either device, and the feature lives host-side. So this phrase
    /// is fed instead by a signal the household already owns: an
    /// operator-configured MQTT topic, off by default, carrying nothing but
    /// the boolean. The word borrows the vendor's own, so nobody reads it as
    /// SecuraCV having started recording.
    OnCall,
}

/// Fixed word for a presence card whose zone has no operator label.
pub const PRESENCE_UNLABELED: &str = "PRESENCE";

/// Fixed word for [`PublicPhrase::OnCall`].
pub const ON_CALL: &str = "ON CALL";

impl PublicPhrase {
    /// The exact text for the matrix. Empty means "draw no text" — the
    /// calm state is a pulse, not a caption.
    pub fn text(&self) -> &str {
        match self {
            PublicPhrase::Calm => "",
            PublicPhrase::Presence { zone: Some(z) } => z.as_str(),
            PublicPhrase::Presence { zone: None } => PRESENCE_UNLABELED,
            PublicPhrase::Degraded(w) => w.text(),
            PublicPhrase::Unknown(w) => w.text(),
            PublicPhrase::Advisory(w) => w.text(),
            PublicPhrase::OnCall => ON_CALL,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_place_label_is_accepted_and_upper_cased() {
        let z = ZoneWord::try_new("front door").expect("front door");
        assert_eq!(z.as_str(), "FRONT DOOR");
    }

    /// Constraint 2, the worked example. The front matrix is 72 pixels wide,
    /// and the shortest useful thing to write on it is a person. Both halves
    /// of that abbreviation are refused, so it cannot be typed into a config
    /// file, let alone arrive on the wire.
    #[test]
    fn the_identity_abbreviation_is_refused_twice_over() {
        // The period is not in the character set.
        assert_eq!(
            ZoneWord::try_new("K. AT DOOR"),
            Err(ZoneWordError::BadChar('.'))
        );
        // And with the period gone, a one-letter word is still an initial.
        assert_eq!(
            ZoneWord::try_new("K AT DOOR"),
            Err(ZoneWordError::TokenTooShort {
                token: "K".to_string()
            })
        );
    }

    #[test]
    fn punctuation_and_spacing_are_refused() {
        assert!(matches!(
            ZoneWord::try_new("FRONT-DOOR"),
            Err(ZoneWordError::BadChar('-'))
        ));
        assert_eq!(ZoneWord::try_new(" FRONT"), Err(ZoneWordError::BadSpacing));
        assert_eq!(
            ZoneWord::try_new("FRONT  DOOR"),
            Err(ZoneWordError::BadSpacing)
        );
        assert_eq!(ZoneWord::try_new(""), Err(ZoneWordError::Empty));
        assert_eq!(ZoneWord::try_new("12 34"), Err(ZoneWordError::NoLetters));
    }

    #[test]
    fn a_label_longer_than_the_matrix_budget_is_refused() {
        let long = "A".repeat(MAX_ZONE_WORD_CHARS + 1);
        assert!(matches!(
            ZoneWord::try_new(&long),
            Err(ZoneWordError::TooLong { .. })
        ));
        assert!(ZoneWord::try_new(&"A".repeat(MAX_ZONE_WORD_CHARS)).is_ok());
    }

    /// Everything the matrix can say is printable ASCII, which is what the
    /// device's own text element accepts. A phrase that needed encoding
    /// would be a phrase built from something other than this vocabulary.
    #[test]
    fn every_phrase_is_printable_ascii() {
        let zone = ZoneWord::try_new("FRONT DOOR").unwrap();
        let all = [
            PublicPhrase::Calm,
            PublicPhrase::Presence { zone: Some(zone) },
            PublicPhrase::Presence { zone: None },
            PublicPhrase::Degraded(DegradedWord::Tamper),
            PublicPhrase::Degraded(DegradedWord::ChainFail),
            PublicPhrase::Degraded(DegradedWord::WitnessLost),
            PublicPhrase::Degraded(DegradedWord::WitnessLate),
            PublicPhrase::Unknown(UnknownWord::BrokerLost),
            PublicPhrase::Unknown(UnknownWord::KernelLost),
            PublicPhrase::Advisory(AdvisoryWord::Smoke),
            PublicPhrase::Advisory(AdvisoryWord::CoAlarm),
            PublicPhrase::OnCall,
        ];
        for p in &all {
            let t = p.text();
            assert!(
                t.chars().all(|c| c.is_ascii_graphic() || c == ' '),
                "phrase {p:?} is not printable ASCII: {t:?}"
            );
            assert!(t.len() <= MAX_ZONE_WORD_CHARS, "phrase {p:?} is too long");
        }
    }

    /// The calm state is the absence of a word. If this ever starts
    /// returning text, the idle matrix has become a caption and the
    /// motion-carries-the-meaning design is gone.
    #[test]
    fn calm_says_nothing() {
        assert_eq!(PublicPhrase::Calm.text(), "");
    }

    /// Both unknown variants read the same in the room and differ only for
    /// the operator — the two-audience split, in one assertion.
    #[test]
    fn unknown_is_one_public_word_and_two_operator_words() {
        assert_eq!(
            UnknownWord::BrokerLost.text(),
            UnknownWord::KernelLost.text()
        );
        assert_ne!(
            UnknownWord::BrokerLost.operator_text(),
            UnknownWord::KernelLost.operator_text()
        );
    }
}
